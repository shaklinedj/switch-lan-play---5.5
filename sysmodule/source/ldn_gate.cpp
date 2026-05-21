/*
 * ldn_gate.cpp — sysmodule polls ldn_mitm's "lanp:gt" service for LDN state.
 *
 * Role inversion: ldn_mitm now HOSTS "lanp:gt". This sysmodule calls
 * smGetService("lanp:gt") from plain C context (no Stratosphere reentrancy),
 * which works reliably. Polling happens once per second in the waiting loop.
 *
 * IPC protocol (must match ldn_mitm/source/ldn_gate_client.cpp):
 *   cmd 0  GetState → returns lanp_gate_state_t
 */
#include "ldn_gate.h"

/* IPC command IDs — must match GateCmdGetState/GateCmdGetEvent in ldn_gate_client.cpp */
#define LANP_GATE_CMD_GET_STATE 0
#define LANP_GATE_CMD_GET_EVENT 1

static Mutex g_gate_mutex;
static bool  g_gate_mutex_ready = false;
static lanp_gate_state_t g_gate_state;

/* IPC session to ldn_mitm's lanp:gt service */
static Service g_mitm_srv;
static bool    g_mitm_connected = false;

/* Event handle received from ldn_mitm for push notifications.
 * INVALID_HANDLE = not acquired yet or unsupported → fall back to polling. */
static Handle  g_gate_event_handle = INVALID_HANDLE;

/* Local sysmodule timestamp of the last time we received a non-finalize,
 * non-none active event from ldn_mitm. Uses the sysmodule's own tick counter
 * so we never compare ticks across process boundaries (armGetSystemTick in
 * ldn_mitm reads cntvct_el0; svcGetSystemTick in the sysmodule reads
 * cntpct_el0 — they can differ). */
static u64 g_last_active_tick = 0;

/* Timestamp when finalize was first observed — for the 10s grace period.
 * Allows the relay to stay alive if the user briefly exits to the home menu
 * and returns to local play quickly. */
static u64 g_finalize_tick = 0;

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
    case LANP_GATE_EVENT_PREPARE: return "prepare";
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

    /* Host or Connect = game is actively using LDN (hosting lobby or joined).
     * Keep running indefinitely — no timeout applies. ldn_mitm won't send
     * periodic scan/idle events in these states. */
    if (st.event_type == LANP_GATE_EVENT_HOST ||
        st.event_type == LANP_GATE_EVENT_CONNECT) {
        return true;
    }

    /* Finalize = game exited local play.
     * Grace period: keep relay alive 10s so a quick home-menu detour doesn't
     * require a full restart cycle. */
    if (st.event_type == LANP_GATE_EVENT_FINALIZE) {
        if (g_finalize_tick != 0) {
            const u64 grace = LANP_GATE_TICKS_PER_SECOND * 10ULL;
            if (gate_now_tick() - g_finalize_tick <= grace) return true;
        }
        return false;
    }

    /* Never seen any activity yet */
    if (g_last_active_tick == 0) return false;

    /* Keep running while recent active events arrive. */
    const u64 now = gate_now_tick();
    const u64 timeout_ticks = LANP_GATE_TICKS_PER_SECOND * (u64)LANP_GATE_TIMEOUT_SECONDS;
    if (now - g_last_active_tick > timeout_ticks) return false;

    return true;
}

/* Strict check used by wait_for_ldn_activity() — NO grace period.
 * Finalize always blocks a start; only genuine new activity (PREPARE/SCAN/etc)
 * triggers a fresh run_service(). Prevents spurious restarts during the grace
 * window after a game closes. */
bool ldn_gate_is_active_for_start(void)
{
    lanp_gate_state_t st;
    ldn_gate_get_state(&st);

    if (st.magic != LANP_GATE_MAGIC || st.version != LANP_GATE_VERSION) return false;
    if (st.event_type == LANP_GATE_EVENT_FINALIZE) return false;
    if (g_last_active_tick == 0) return false;

    const u64 now = gate_now_tick();
    const u64 timeout_ticks = LANP_GATE_TICKS_PER_SECOND * (u64)LANP_GATE_TIMEOUT_SECONDS;
    if (now - g_last_active_tick > timeout_ticks) return false;

    return true;
}

void ldn_gate_poll(void)
{
    /* Connect to ldn_mitm's lanp:gt service on first call (or after disconnect).
     * smGetService() requires an open SM session; open/close it here because
     * __appInit calls smExit() before the polling loop runs. */
    if (!g_mitm_connected) {
        Result sm_rc = smInitialize();
        if (R_FAILED(sm_rc)) {
            LLOG(LLOG_INFO, "ldn_gate: smInitialize failed rc=0x%x", sm_rc);
            return;
        }
        Result rc = smGetService(&g_mitm_srv, LANP_GATE_SERVICE_NAME);
        smExit();
        if (R_FAILED(rc)) {
            LLOG(LLOG_INFO, "ldn_gate: smGetService(%s) failed rc=0x%x", LANP_GATE_SERVICE_NAME, rc);
            return;
        }
        g_mitm_connected = true;
        LLOG(LLOG_INFO, "ldn_gate: connected to ldn_mitm %s service", LANP_GATE_SERVICE_NAME);

        /* Try to acquire the event handle (cmd 1) for push notifications.
         * If ldn_mitm doesn't support it, we stay in poll-only mode. */
        if (g_gate_event_handle == INVALID_HANDLE) {
            Handle tmp_handle = INVALID_HANDLE;
            Result ev_rc = serviceDispatch(&g_mitm_srv, LANP_GATE_CMD_GET_EVENT,
                .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
                .out_handles = &tmp_handle,
            );
            if (R_SUCCEEDED(ev_rc) && tmp_handle != INVALID_HANDLE) {
                g_gate_event_handle = tmp_handle;
                LLOG(LLOG_INFO, "ldn_gate: event handle acquired (0x%x) — event-driven mode", g_gate_event_handle);
            } else {
                LLOG(LLOG_INFO, "ldn_gate: event handle not available (rc=0x%x) — poll-only mode", ev_rc);
            }
        }
    }

    /* Call GetState (cmd 0) and update local g_gate_state */
    lanp_gate_state_t st;
    memset(&st, 0, sizeof(st));
    Result rc = serviceDispatchOut(&g_mitm_srv, LANP_GATE_CMD_GET_STATE, st);
    if (R_FAILED(rc)) {
        LLOG(LLOG_WARNING, "ldn_gate: GetState failed rc=0x%x — disconnecting", rc);
        serviceClose(&g_mitm_srv);
        memset(&g_mitm_srv, 0, sizeof(g_mitm_srv));
        g_mitm_connected = false;
        return;
    }

    if (st.magic == LANP_GATE_MAGIC && st.version == LANP_GATE_VERSION) {
        /* Update local active timer whenever we see a non-finalize, non-none
         * event.  IDLE (between scan cycles) also counts — it indicates the
         * game is still actively using LDN.  The hosting timeout is handled
         * separately: HOST/CONNECT return true unconditionally in
         * ldn_gate_is_active(), so the timeout only applies to scan phases. */
        if (st.event_type != LANP_GATE_EVENT_NONE &&
            st.event_type != LANP_GATE_EVENT_FINALIZE) {
            g_last_active_tick = gate_now_tick();
            g_finalize_tick    = 0;  /* new activity clears the grace window */
        }
        /* Track when finalize is first seen (transition into finalize). */
        if (st.event_type == LANP_GATE_EVENT_FINALIZE &&
            g_gate_state.event_type != LANP_GATE_EVENT_FINALIZE) {
            g_finalize_tick = gate_now_tick();
        }
        if (g_gate_mutex_ready) mutexLock(&g_gate_mutex);
        g_gate_state = st;
        if (g_gate_mutex_ready) mutexUnlock(&g_gate_mutex);
    }
}

Result ldn_gate_service_init(void)
{
    mutexInit(&g_gate_mutex);
    g_gate_mutex_ready = true;
    mutexLock(&g_gate_mutex);
    gate_reset_state_locked();
    mutexUnlock(&g_gate_mutex);

    memset(&g_mitm_srv, 0, sizeof(g_mitm_srv));
    g_mitm_connected   = false;
    g_last_active_tick = 0;
    g_finalize_tick    = 0;

    /* No service registration here — ldn_mitm hosts lanp:gt now.
     * Return success unconditionally; polling will connect lazily. */
    return 0;
}

void ldn_gate_service_exit(void)
{
    if (g_gate_event_handle != INVALID_HANDLE) {
        svcCloseHandle(g_gate_event_handle);
        g_gate_event_handle = INVALID_HANDLE;
    }
    if (g_mitm_connected) {
        serviceClose(&g_mitm_srv);
        memset(&g_mitm_srv, 0, sizeof(g_mitm_srv));
        g_mitm_connected = false;
    }
}

bool ldn_gate_wait_event(u64 timeout_ns)
{
    if (g_gate_event_handle == INVALID_HANDLE) {
        /* No event handle — fall back to a simple sleep so callers
         * can use the same code path for both modes. */
        svcSleepThread((s64)timeout_ns);
        return false;
    }

    /* Clear any pending signal BEFORE waiting. Horizon events are "sticky":
     * if ldn_mitm signaled multiple times while we were busy (e.g., scan→
     * idle→finalize), the event stays signaled.  Without this reset,
     * svcWaitSynchronization would return immediately in a busy-loop. */
    svcResetSignal(g_gate_event_handle);

    Result rc = svcWaitSynchronizationSingle(g_gate_event_handle, (s64)timeout_ns);
    return R_SUCCEEDED(rc);
}

bool ldn_gate_service_available(void)
{
    /* Always available: we poll lazily, no registration needed */
    return true;
}
