#include "ldn_gate.h"

#define LANP_GATE_CMD_NOTIFY 0
#define LANP_GATE_CMD_GET_STATE 1
#define LANP_GATE_CMD_CLEAR 2

static Handle g_gate_port = INVALID_HANDLE;
static Thread g_gate_thread;
static uint8_t g_gate_stack[0x4000] __attribute__((aligned(0x1000)));
static Mutex g_gate_mutex;
static volatile bool g_gate_running = false;
static bool g_gate_mutex_ready = false;
static bool g_gate_thread_started = false;
static bool g_gate_registered = false;
static lanp_gate_state_t g_gate_state;

static u64 gate_now_tick(void)
{
    return svcGetSystemTick();
}

const char *ldn_gate_event_name(u32 event_type)
{
    switch (event_type) {
    case LANP_GATE_EVENT_SCAN: return "scan";
    case LANP_GATE_EVENT_HOST: return "host";
    case LANP_GATE_EVENT_CONNECT: return "connect";
    case LANP_GATE_EVENT_IDLE: return "idle";
    case LANP_GATE_EVENT_FINALIZE: return "finalize";
    default: return "none";
    }
}

static void gate_reset_state_locked(void)
{
    memset(&g_gate_state, 0, sizeof(g_gate_state));
    g_gate_state.magic = LANP_GATE_MAGIC;
    g_gate_state.version = LANP_GATE_VERSION;
    g_gate_state.event_type = LANP_GATE_EVENT_NONE;
    g_gate_state.active = 0;
    g_gate_state.last_event_tick = gate_now_tick();
}

static void gate_apply_event_locked(const lanp_gate_event_t *ev)
{
    if (!ev || ev->magic != LANP_GATE_MAGIC || ev->version != LANP_GATE_VERSION) {
        return;
    }

    g_gate_state.magic = LANP_GATE_MAGIC;
    g_gate_state.version = LANP_GATE_VERSION;
    g_gate_state.event_type = ev->event_type;
    g_gate_state.process_id = ev->process_id;
    g_gate_state.title_id = ev->title_id;
    g_gate_state.local_communication_id = ev->local_communication_id;
    g_gate_state.scene_id = ev->scene_id;
    g_gate_state.last_event_tick = gate_now_tick();

    switch (ev->event_type) {
    case LANP_GATE_EVENT_SCAN:
    case LANP_GATE_EVENT_HOST:
    case LANP_GATE_EVENT_CONNECT:
        g_gate_state.active = 1;
        break;
    case LANP_GATE_EVENT_IDLE:
    case LANP_GATE_EVENT_FINALIZE:
    default:
        g_gate_state.active = 0;
        break;
    }
}

void ldn_gate_get_state(lanp_gate_state_t *out)
{
    if (!out) return;
    if (g_gate_mutex_ready) mutexLock(&g_gate_mutex);
    *out = g_gate_state;
    if (g_gate_mutex_ready) mutexUnlock(&g_gate_mutex);
}

bool ldn_gate_is_active(void)
{
    lanp_gate_state_t st;
    ldn_gate_get_state(&st);

    if (st.magic != LANP_GATE_MAGIC || st.version != LANP_GATE_VERSION) return false;
    if (!st.active) return false;

    const u64 now = gate_now_tick();
    const u64 timeout_ticks = LANP_GATE_TICKS_PER_SECOND * (u64)LANP_GATE_TIMEOUT_SECONDS;
    if (now >= st.last_event_tick && (now - st.last_event_tick) > timeout_ticks) {
        return false;
    }
    return true;
}

static void gate_make_response(Result result, const void *out_data, size_t out_size)
{
    void *base = armGetTls();
    memset(base, 0, 0x100);

    const u32 payload_size = (u32)(sizeof(CmifOutHeader) + out_size);
    const u32 num_words = (u32)((16 + payload_size + 3) / 4);
    HipcRequest hipc = hipcMakeRequestInline(base,
        .type = 0,
        .num_data_words = num_words
    );

    CmifOutHeader *hdr = (CmifOutHeader *)cmifGetAlignedDataStart(hipc.data_words, base);
    hdr->magic = CMIF_OUT_HEADER_MAGIC;
    hdr->version = 0;
    hdr->result = result;
    hdr->token = 0;

    if (out_data && out_size > 0) {
        memcpy(hdr + 1, out_data, out_size);
    }
}

static void gate_process_request(bool *close_session)
{
    *close_session = false;

    HipcParsedRequest hipc = hipcParseRequest(armGetTls());

    if (hipc.meta.type == CmifCommandType_Close) {
        gate_make_response(0, NULL, 0);
        *close_session = true;
        return;
    }

    if (hipc.meta.type != CmifCommandType_Request &&
        hipc.meta.type != CmifCommandType_RequestWithContext) {
        gate_make_response(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
        return;
    }

    CmifInHeader *hdr = (CmifInHeader *)cmifGetAlignedDataStart(hipc.data.data_words, armGetTls());
    if (!hdr || hdr->magic != CMIF_IN_HEADER_MAGIC) {
        gate_make_response(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
        return;
    }

    void *in_data = hdr + 1;

    switch (hdr->command_id) {
    case LANP_GATE_CMD_NOTIFY: {
        lanp_gate_event_t ev;
        memset(&ev, 0, sizeof(ev));
        memcpy(&ev, in_data, sizeof(ev));

        if (g_gate_mutex_ready) mutexLock(&g_gate_mutex);
        gate_apply_event_locked(&ev);
        lanp_gate_state_t snapshot = g_gate_state;
        if (g_gate_mutex_ready) mutexUnlock(&g_gate_mutex);

        LLOG(LLOG_INFO, "ldn_gate: event=%s active=%u pid=%llu title=%016llX intent=%llu scene=%u",
             ldn_gate_event_name(snapshot.event_type), snapshot.active,
             (unsigned long long)snapshot.process_id,
             (unsigned long long)snapshot.title_id,
             (unsigned long long)snapshot.local_communication_id,
             snapshot.scene_id);

        gate_make_response(0, NULL, 0);
        break;
    }
    case LANP_GATE_CMD_GET_STATE: {
        lanp_gate_state_t st;
        ldn_gate_get_state(&st);
        gate_make_response(0, &st, sizeof(st));
        break;
    }
    case LANP_GATE_CMD_CLEAR: {
        if (g_gate_mutex_ready) mutexLock(&g_gate_mutex);
        gate_reset_state_locked();
        if (g_gate_mutex_ready) mutexUnlock(&g_gate_mutex);
        gate_make_response(0, NULL, 0);
        break;
    }
    default:
        gate_make_response(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
        break;
    }
}

static void gate_server_thread_fn(void *arg)
{
    (void)arg;
    Handle session = INVALID_HANDLE;
    Handle reply_target = INVALID_HANDLE;
    bool close_after_reply = false;

    while (g_gate_running) {
        Handle handles[2];
        s32 handle_count = 0;
        handles[handle_count++] = g_gate_port;
        if (session != INVALID_HANDLE) {
            handles[handle_count++] = session;
        }

        s32 index = -1;
        Result rc = svcReplyAndReceive(&index, handles, handle_count, reply_target, 1000000000ULL);

        if (close_after_reply && reply_target != INVALID_HANDLE) {
            svcCloseHandle(reply_target);
            if (session == reply_target) session = INVALID_HANDLE;
            close_after_reply = false;
        }
        reply_target = INVALID_HANDLE;

        if (R_FAILED(rc)) {
            continue;
        }

        if (index == 0) {
            Handle new_session = INVALID_HANDLE;
            rc = svcAcceptSession(&new_session, g_gate_port);
            if (R_SUCCEEDED(rc)) {
                if (session == INVALID_HANDLE) {
                    session = new_session;
                } else {
                    /* Single-client service: reject extra concurrent sessions. */
                    svcCloseHandle(new_session);
                }
            }
        } else if (index == 1 && session != INVALID_HANDLE) {
            bool close_session = false;
            gate_process_request(&close_session);
            reply_target = session;
            close_after_reply = close_session;
        }
    }

    if (session != INVALID_HANDLE) svcCloseHandle(session);
}

Result ldn_gate_service_init(void)
{
    if (g_gate_registered) return 0;

    mutexInit(&g_gate_mutex);
    g_gate_mutex_ready = true;
    mutexLock(&g_gate_mutex);
    gate_reset_state_locked();
    mutexUnlock(&g_gate_mutex);

    Result rc = smRegisterService(&g_gate_port, smEncodeName(LANP_GATE_SERVICE_NAME), false, 4);
    if (R_FAILED(rc)) {
        LLOG(LLOG_WARNING, "ldn_gate: smRegisterService(%s) failed: 0x%x", LANP_GATE_SERVICE_NAME, rc);
        g_gate_port = INVALID_HANDLE;
        return rc;
    }

    g_gate_running = true;
    rc = threadCreate(&g_gate_thread, gate_server_thread_fn, NULL,
                      g_gate_stack, sizeof(g_gate_stack), 30, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_WARNING, "ldn_gate: threadCreate failed: 0x%x", rc);
        g_gate_running = false;
        smUnregisterService(smEncodeName(LANP_GATE_SERVICE_NAME));
        svcCloseHandle(g_gate_port);
        g_gate_port = INVALID_HANDLE;
        return rc;
    }

    threadStart(&g_gate_thread);
    g_gate_thread_started = true;
    g_gate_registered = true;
    LLOG(LLOG_INFO, "ldn_gate: service %s registered", LANP_GATE_SERVICE_NAME);
    return 0;
}

void ldn_gate_service_exit(void)
{
    if (!g_gate_registered) return;

    g_gate_running = false;
    if (g_gate_thread_started) {
        threadWaitForExit(&g_gate_thread);
        threadClose(&g_gate_thread);
        g_gate_thread_started = false;
    }

    bool sm_ready = R_SUCCEEDED(smInitialize());
    if (sm_ready) {
        smUnregisterService(smEncodeName(LANP_GATE_SERVICE_NAME));
        smExit();
    }
    if (g_gate_port != INVALID_HANDLE) {
        svcCloseHandle(g_gate_port);
        g_gate_port = INVALID_HANDLE;
    }
    g_gate_registered = false;
}

bool ldn_gate_service_available(void)
{
    return g_gate_registered;
}
