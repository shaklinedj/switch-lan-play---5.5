/**
 * main.cpp — entry point for the switch-lan-play Atmosphere sysmodule.
 *
 * Boot sequence
 * =============
 * 1. Initialise libnx services (sockets, filesystem, set:sys)
 * 2. Read /config/lan-play/config.ini from the SD card
 * 3. Resolve the relay server address via DNS
 * 4. Initialise the packet_ctx (virtual LAN interface state)
 * 5. Initialise the raw-socket TAP layer
 * 6. Initialise the relay UDP client
 * 7. Launch three background threads:
 *      - tap_recv_thread      (inbound packets from WiFi/LAN → game)
 *      - relay_recv_thread    (inbound packets from relay server → game)
 *      - keepalive_thread     (periodic UDP keepalive → relay server)
 * 8. Sleep forever (sysmodule stays resident until console shuts down)
 *
 * Memory / resource ownership
 * ============================
 * - lp           : heap-allocated, freed in every error path and cleanup.
 * - lp->username : strdup(), freed before freeing lp.
 * - Thread stacks: statically allocated (no dynamic lifetime concern).
 * - Sockets      : closed by tap_close() / lan_client_close().
 */

#include "nx_common.h"
#include "config.h"
#include "tap_iface.h"
#include "lan_client_nx.h"
#include "ldn_bridge.h"
#include "packet.h"
#include "sha1.h"
#include "foreground_detector.h"
#include "ldn_gate.h"

/* bsd.h needed for bsdInitialize() — same init pattern as ldn_mitm */
extern "C" {
#include <switch/services/bsd.h>
#include <switch/services/psc.h>
}

/* -------------------------------------------------------------------------
 * Logging implementation
 * ---------------------------------------------------------------------- */
static const char *level_names[] = {
    "", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG"
};

static Mutex g_log_mutex;
static bool  g_log_mutex_ready = false;
static time_t g_last_log_commit = 0;

struct known_relay_entry {
    const char *host;
    const char *ip;
};

static const known_relay_entry g_known_relays[] = {
    /* tekn0.net removed from builtin to test DNS resolution */
    { "lan.nonny.horse", "65.21.20.230" },
    { "switch.servegame.com", "89.163.151.130" },
    { "switch-lanyplay-de.ddns.net", "37.201.39.187" },
    { "switch.jayseateam.nl", "45.83.241.140" },
    { "switch.r3ps4j.nl", "141.144.207.91" },
    { "muitxobem-lanplay.ddns.net", "129.148.17.98" },
};

static const char *lookup_known_relay_ip(const char *host)
{
    for (size_t i = 0; i < sizeof(g_known_relays) / sizeof(g_known_relays[0]); i++) {
        if (strcmp(g_known_relays[i].host, host) == 0) {
            return g_known_relays[i].ip;
        }
    }
    return NULL;
}

static void cache_host_mapping(const char *host, const char *ip)
{
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/lan-play", 0777);

    char lines[64][256];
    int line_count = 0;
    bool found = false;

    FILE *in = fopen("sdmc:/config/lan-play/hosts.txt", "r");
    if (in) {
        while (line_count < 64 && fgets(lines[line_count], sizeof(lines[line_count]), in)) {
            char existing_ip[64] = {0};
            char existing_host[128] = {0};
            if (sscanf(lines[line_count], "%63s %127s", existing_ip, existing_host) == 2 &&
                strcmp(existing_host, host) == 0) {
                snprintf(lines[line_count], sizeof(lines[line_count]), "%s %s\n", ip, host);
                found = true;
            }
            line_count++;
        }
        fclose(in);
    }

    if (!found && line_count < 64) {
        snprintf(lines[line_count++], sizeof(lines[0]), "%s %s\n", ip, host);
    }

    FILE *out = fopen("sdmc:/config/lan-play/hosts.txt", "w");
    if (!out) return;
    for (int i = 0; i < line_count; i++) {
        fputs(lines[i], out);
    }
    fclose(out);
    fsdevCommitDevice("sdmc");
}

static int resolve_from_hosts_file(const char *host, struct in_addr *out_addr)
{
    FILE *f = fopen("sdmc:/config/lan-play/hosts.txt", "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char ip[64] = {0};
        char name[128] = {0};

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        if (sscanf(line, "%63s %127s", ip, name) != 2) continue;
        if (strcmp(name, host) != 0) continue;

        if (inet_pton(AF_INET, ip, out_addr) == 1) {
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return -1;
}

static int resolve_from_builtin_fallback(const char *host, struct in_addr *out_addr)
{
    const char *ip = lookup_known_relay_ip(host);
    if (!ip) return -1;
    return inet_pton(AF_INET, ip, out_addr) == 1 ? 0 : -1;
}

void nx_log(int level, const char *fmt, ...)
{
    if (level > LLOG_DEBUG) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = snprintf(buf, sizeof(buf), "[LanPlay][%s] ", level_names[level]);
    vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
    va_end(ap);

    /* Atmosphere Debug Stream */
    svcOutputDebugString(buf, strlen(buf));

    if (g_log_mutex_ready) mutexLock(&g_log_mutex);

    /* Persistent File Log - ROOT visibility. Do not fsdevCommitDevice() on
     * every single line: several runtime threads can log at once, and forced
     * SD commits inside hot paths can amplify Horizon network/settings races. */
    const char *log_path = "sdmc:/lan-play.log";
    struct stat st;
    if (stat(log_path, &st) == 0 && st.st_size > 102400) {
        remove("sdmc:/lan-play.old.log");
        rename(log_path, "sdmc:/lan-play.old.log");
    }

    FILE *f = fopen(log_path, "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);

        time_t now = time(NULL);
        if (level <= LLOG_ERROR || g_last_log_commit == 0 || now - g_last_log_commit >= 5) {
            fsdevCommitDevice("sdmc");
            g_last_log_commit = now;
        }
    }

    if (g_log_mutex_ready) mutexUnlock(&g_log_mutex);
}

/* -------------------------------------------------------------------------
 * DNS resolution helper
 * ---------------------------------------------------------------------- */
static int resolve_server(const char *addr_str, struct sockaddr_in *out)
{
    char host[256];
    strncpy(host, addr_str, sizeof(host)-1);
    host[sizeof(host)-1] = '\0';

    char *colon = strchr(host, ':');
    int port = 11451;
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);

    /* 1. Try parsing as a direct IP address first to avoid DNS issues/timeouts */
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) {
        LLOG(LLOG_INFO, "main: relay parsed as direct IP: %s:%d", host, port);
        return 0;
    }

    /* 2. hosts.txt fallback — instant, no network needed */
    if (resolve_from_hosts_file(host, &out->sin_addr) == 0) {
        char ip_str[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &out->sin_addr, ip_str, sizeof(ip_str));
        LLOG(LLOG_WARNING, "main: hosts.txt fallback for '%s' -> %s:%d", host, ip_str, port);
        return 0;
    }

    /* 3. Built-in table fallback — instant, no network needed */
    if (resolve_from_builtin_fallback(host, &out->sin_addr) == 0) {
        char ip_str[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &out->sin_addr, ip_str, sizeof(ip_str));
        LLOG(LLOG_WARNING, "main: builtin fallback for '%s' -> %s:%d", host, ip_str, port);
        return 0;
    }

    /* 4. DNS — last resort, 3 retries only (avoids 10s wait on 90DNS) */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    for (int retry = 0; retry < 3; retry++) {
        int gai_err = getaddrinfo(host, NULL, &hints, &res);
        if (gai_err == 0 && res) break;
        LLOG(LLOG_WARNING, "main: DNS failed for '%s' (try %d/3): %s", host, retry + 1, gai_strerror(gai_err));
        svcSleepThread(1000000000ULL);
    }

    if (!res) {
        LLOG(LLOG_ERROR, "main: cannot resolve '%s' — use IP:port in config", host);
        return -1;
    }

    memcpy(&out->sin_addr, &((struct sockaddr_in*)res->ai_addr)->sin_addr, sizeof(struct in_addr));
    freeaddrinfo(res);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &out->sin_addr, ip_str, sizeof(ip_str));
    cache_host_mapping(host, ip_str);
    LLOG(LLOG_INFO, "main: resolved '%s' -> %s:%d (DNS)", host, ip_str, port);
    return 0;
}

/* -------------------------------------------------------------------------
 * SHA1 of password → stored in lp->key (for auth)
 * ---------------------------------------------------------------------- */
static void hash_password(struct lan_play *lp, const char *password)
{
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, (const unsigned char *)password, (uint32_t)strlen(password));
    SHA1Final(lp->key, &ctx);
    /* Scrub the temporary context so the password doesn't linger in memory */
    memset(&ctx, 0, sizeof(ctx));
}

/* -------------------------------------------------------------------------
 * Free all heap resources owned by a lan_play context.
 * Sockets are expected to have been closed before this is called
 * (by tap_close / lan_client_close).
 * ---------------------------------------------------------------------- */
static void lan_play_free(struct lan_play *lp)
{
    if (!lp) return;
    if (lp->username) {
        free(lp->username);
        lp->username = NULL;
    }
    free(lp);
}

/* -------------------------------------------------------------------------
 * Build the send buffer for packet_ctx
 * ---------------------------------------------------------------------- */
static uint8_t g_pkt_buffer[BUFFER_SIZE];

/* -------------------------------------------------------------------------
 * Thread stacks (statically allocated — sysmodule has no heap growth)
 * ---------------------------------------------------------------------- */
#define STACK_SIZE 0x8000 /* 32 KB per thread */
static uint8_t s_tap_stack[STACK_SIZE]       __attribute__((aligned(0x1000)));
static uint8_t s_relay_stack[STACK_SIZE]     __attribute__((aligned(0x1000)));
static uint8_t s_keepalive_stack[STACK_SIZE] __attribute__((aligned(0x1000)));
static uint8_t s_ldn_udp_stack[STACK_SIZE]   __attribute__((aligned(0x1000)));
static uint8_t s_ldn_tcp_stack[STACK_SIZE]   __attribute__((aligned(0x1000)));
static uint8_t s_psc_stack[STACK_SIZE]       __attribute__((aligned(0x1000)));
static uint8_t s_pgl_stack[STACK_SIZE]       __attribute__((aligned(0x1000)));

/* PSC sleep/wake state. This lets the sysmodule close runtime sockets before
 * Horizon enters sleep, preventing blocked recvfrom()/accept() from surviving
 * suspend/resume. */
static PscPmModule g_psc_module;
static Handle      g_psc_event = INVALID_HANDLE;
static Thread      g_psc_thread;
static bool        g_psc_thread_started = false;
static volatile bool g_main_alive = true;
static struct lan_play *volatile g_active_lp = NULL;
static volatile bool g_sleep_teardown_requested = false;

static Result g_rc_pgl = 0;
static Mutex  g_pgl_mutex;
static bool   g_pgl_mutex_ready = false;
static Thread g_pgl_thread;
static bool   g_pgl_thread_started = false;
static volatile bool g_pgl_running = false;
static bool   g_pgl_ready = false;
static bool   g_pgl_observer_open = false;
static bool   g_pgl_event_open = false;
static PglEventObserver g_pgl_observer;
static Event  g_pgl_process_event;
static u64    g_pgl_app_pid = 0;
static PmProcessEvent g_pgl_last_event = PmProcessEvent_None;
static Result g_pgl_last_rc = 0;

/* =========================================================================
 * CRITICAL: libnx sysmodule boilerplate.
 * Without these, libnx treats us as a normal application and tries to
 * connect to applet services that don't exist → immediate fatal crash.
 * ====================================================================== */
extern "C" {
    /* Tell libnx this is a sysmodule, not a regular app */
    u32 __nx_applet_type = AppletType_None;

    /* Don't try to use the applet exit mechanism */
    u32 __nx_applet_exit_mode = 0;

    /* Custom heap for the sysmodule (2 MB for safety) */
    #define INNER_HEAP_SIZE 0x200000
    static char g_inner_heap[INNER_HEAP_SIZE];

    void __libnx_initheap(void) {
        extern char *fake_heap_start;
        extern char *fake_heap_end;
        fake_heap_start = g_inner_heap;
        fake_heap_end   = g_inner_heap + INNER_HEAP_SIZE;
    }
}

/* Socket configuration — sysmodules can't use the default large buffers.
 * Pattern matches ldn_mitm: bsdInitialize() first, then socketInitialize(). */
static const SocketInitConfig g_socket_config = {
    .tcp_tx_buf_size     = 0x800,
    .tcp_rx_buf_size     = 0x800,
    .tcp_tx_buf_max_size = 0x2000,
    .tcp_rx_buf_max_size = 0x2000,
    .udp_tx_buf_size     = 0x2000,  /* 8 KB — game LAN packets are small (<2 KB) */
    .udp_rx_buf_size     = 0x2000,  /* 8 KB */
    .sb_efficiency       = 2,
    .num_bsd_sessions    = 8,   /* tap(2) + relay(1) + ldn_bridge(3) + spare(2) */
    .bsd_service_type    = BsdServiceType_System, /* bsd:s needed for SOCK_RAW (EPERM with bsd:u) */
};

/* Transfer Memory for BSD sockets.
 * Per UDP socket: sb_efficiency × (udp_tx + udp_rx) = 2×(8+8) KB = 32 KB.
 * 8 sessions × 32 KB = 256 KB → 0x40000 with comfortable margin. */
#define SOCKET_TMEM_SIZE 0x40000
static uint8_t g_socket_tmem_buffer[SOCKET_TMEM_SIZE] alignas(0x1000);

static const BsdInitConfig g_bsd_config = {
    .version             = 1,
    .tmem_buffer         = g_socket_tmem_buffer,
    .tmem_buffer_size    = sizeof(g_socket_tmem_buffer),
    .tcp_tx_buf_size     = g_socket_config.tcp_tx_buf_size,
    .tcp_rx_buf_size     = g_socket_config.tcp_rx_buf_size,
    .tcp_tx_buf_max_size = g_socket_config.tcp_tx_buf_max_size,
    .tcp_rx_buf_max_size = g_socket_config.tcp_rx_buf_max_size,
    .udp_tx_buf_size     = g_socket_config.udp_tx_buf_size,
    .udp_rx_buf_size     = g_socket_config.udp_rx_buf_size,
    .sb_efficiency       = g_socket_config.sb_efficiency,
};

/* -------------------------------------------------------------------------
 * Init Error Tracking
 * ---------------------------------------------------------------------- */
static Result g_rc_fs     = 0;
static Result g_rc_setsys = 0;
static Result g_rc_bsd    = 0;
static Result g_rc_socket = 0;
static Result g_rc_nifm   = 0;
static Result g_rc_fg     = 0;
static Result g_rc_gate   = 0;
static Result g_rc_pscm   = 0;

static bool pm_monitors_enabled(void);

static bool g_network_ready = false;
static bool g_nifm_ready    = false;
static bool g_bsd_ready     = false;
static bool g_socket_ready  = false;


/* -------------------------------------------------------------------------
 * Lazy network runtime services
 * ---------------------------------------------------------------------- */
static int runtime_network_init(void)
{
    if (g_network_ready) return 0;

    Result sm_rc = smInitialize();
    bool sm_ready = R_SUCCEEDED(sm_rc);
    if (!sm_ready) {
        LLOG(LLOG_ERROR, "network: smInitialize failed: 0x%x", sm_rc);
        return -1;
    }

    g_rc_nifm = nifmInitialize(NifmServiceType_Admin);
    if (R_FAILED(g_rc_nifm)) {
        g_rc_nifm = nifmInitialize(NifmServiceType_User);
    }
    if (R_FAILED(g_rc_nifm)) {
        LLOG(LLOG_ERROR, "network: nifmInitialize failed: 0x%x", g_rc_nifm);
        smExit();
        return -1;
    }
    g_nifm_ready = true;

    g_rc_bsd = bsdInitialize(&g_bsd_config,
                             g_socket_config.num_bsd_sessions,
                             g_socket_config.bsd_service_type);
    if (R_FAILED(g_rc_bsd)) {
        LLOG(LLOG_ERROR, "network: bsdInitialize failed: 0x%x", g_rc_bsd);
        nifmExit();
        g_nifm_ready = false;
        smExit();
        return -1;
    }
    g_bsd_ready = true;

    g_rc_socket = socketInitialize(&g_socket_config);
    if (R_FAILED(g_rc_socket)) {
        LLOG(LLOG_ERROR, "network: socketInitialize failed: 0x%x", g_rc_socket);
        bsdExit();
        nifmExit();
        g_bsd_ready = false;
        g_nifm_ready = false;
        smExit();
        return -1;
    }
    g_socket_ready = true;
    g_network_ready = true;

    LLOG(LLOG_INFO, "network: runtime services initialized (nifm=0x%x bsd=0x%x socket=0x%x)",
         g_rc_nifm, g_rc_bsd, g_rc_socket);

    smExit();
    return 0;
}

static void runtime_network_exit(void)
{
    LLOG(LLOG_INFO, "runtime_network_exit: Closing network resources...");
    socketExit();
    bsdExit();
    nifmExit();
    LLOG(LLOG_INFO, "runtime_network_exit: Network resources closed.");
}

/* -------------------------------------------------------------------------
 * PSC sleep/wake monitoring
 * ---------------------------------------------------------------------- */
static void psc_emergency_teardown(struct lan_play *lp)
{
    if (!lp) return;

    LLOG(LLOG_WARNING, "psc: emergency teardown before sleep");
    g_sleep_teardown_requested = true;
    lp->running = false;

    /* Close sockets first. This intentionally does not wait for threads because
     * PSC acknowledgements must be fast; the normal cleanup path will join and
     * close thread handles afterwards. */
    ldn_bridge_close(lp);
    lan_client_close(lp);
    tap_close(lp);
}

static void psc_thread_fn(void *arg)
{
    (void)arg;
    LLOG(LLOG_INFO, "psc: monitor thread started");

    while (g_main_alive) {
        if (g_psc_event == INVALID_HANDLE) {
            svcSleepThread(1000000000LL);
            continue;
        }

        Result rc = waitSingleHandle(g_psc_event, 1000000000LL);
        if (R_FAILED(rc)) {
            /* Timeout is expected. Other errors are logged sparingly. */
            continue;
        }

        PscPmState state;
        u32 flags = 0;
        rc = pscPmModuleGetRequest(&g_psc_module, &state, &flags);
        if (R_FAILED(rc)) {
            LLOG(LLOG_WARNING, "psc: get request failed: 0x%x", rc);
            continue;
        }

        LLOG(LLOG_INFO, "psc: state=%u flags=%u", (u32)state, flags);

        if (state == PscPmState_ReadySleep) {
            struct lan_play *lp = (struct lan_play *)g_active_lp;
            if (lp) psc_emergency_teardown(lp);
        }

        pscPmModuleAcknowledge(&g_psc_module, state);
    }

    LLOG(LLOG_INFO, "psc: monitor thread exiting");
}

static void psc_monitor_start(void)
{
    if (g_psc_thread_started) return;
    if (R_FAILED(g_rc_pscm)) return;

    Result rc = pscmGetPmModule(&g_psc_module, (PscPmModuleId)101, NULL, 0, true);
    if (R_FAILED(rc)) {
        LLOG(LLOG_WARNING, "psc: pscmGetPmModule failed: 0x%x", rc);
        return;
    }

    g_psc_event = g_psc_module.event.revent;
    rc = threadCreate(&g_psc_thread, psc_thread_fn, NULL,
                      s_psc_stack, sizeof(s_psc_stack), 31, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_WARNING, "psc: threadCreate failed: 0x%x", rc);
        g_psc_event = INVALID_HANDLE;
        return;
    }

    threadStart(&g_psc_thread);
    g_psc_thread_started = true;
}

static void psc_monitor_stop(void)
{
    g_main_alive = false;
    if (g_psc_thread_started) {
        threadWaitForExit(&g_psc_thread);
        threadClose(&g_psc_thread);
        g_psc_thread_started = false;
    }
}

static const char *pgl_process_event_name(PmProcessEvent event)
{
    switch (event) {
        case PmProcessEvent_Exit:       return "exit";
        case PmProcessEvent_Start:      return "start";
        case PmProcessEvent_Crash:      return "crash";
        case PmProcessEvent_DebugStart: return "debug_start";
        case PmProcessEvent_DebugBreak: return "debug_break";
        default:                        return "none";
    }
}

static void pgl_set_state(u64 pid, PmProcessEvent event, Result rc)
{
    if (!g_pgl_mutex_ready) return;

    mutexLock(&g_pgl_mutex);
    g_pgl_app_pid = pid;
    g_pgl_last_event = event;
    g_pgl_last_rc = rc;
    mutexUnlock(&g_pgl_mutex);
}

static void pgl_clear_pid(Result rc)
{
    if (!g_pgl_mutex_ready) return;

    mutexLock(&g_pgl_mutex);
    g_pgl_app_pid = 0;
    g_pgl_last_rc = rc;
    mutexUnlock(&g_pgl_mutex);
}

static void pgl_get_state(u64 *pid_out, PmProcessEvent *event_out, Result *rc_out)
{
    if (pid_out) *pid_out = 0;
    if (event_out) *event_out = PmProcessEvent_None;
    if (rc_out) *rc_out = 0;
    if (!g_pgl_mutex_ready) return;

    mutexLock(&g_pgl_mutex);
    if (pid_out) *pid_out = g_pgl_app_pid;
    if (event_out) *event_out = g_pgl_last_event;
    if (rc_out) *rc_out = g_pgl_last_rc;
    mutexUnlock(&g_pgl_mutex);
}

static bool pgl_monitor_available(void)
{
    return g_pgl_ready;
}

static bool pgl_refresh_application_state(u64 *pid_out)
{
    if (pid_out) *pid_out = 0;
    if (!g_pgl_ready) return false;

    u64 pid = 0;
    Result rc = pglGetApplicationProcessId(&pid);
    if (R_SUCCEEDED(rc) && pid != 0) {
        pgl_set_state(pid, PmProcessEvent_Start, rc);
        if (pid_out) *pid_out = pid;
        return true;
    }

    pgl_clear_pid(rc);
    return false;
}

static bool pgl_has_application(u64 *pid_out, PmProcessEvent *event_out, Result *rc_out)
{
    if (pid_out) *pid_out = 0;
    if (event_out) *event_out = PmProcessEvent_None;
    if (rc_out) *rc_out = 0;
    if (!g_pgl_ready) return false;

    if (!g_pgl_thread_started) {
        pgl_refresh_application_state(NULL);
    }

    pgl_get_state(pid_out, event_out, rc_out);
    return pid_out ? (*pid_out != 0) : (g_pgl_app_pid != 0);
}

static void pgl_thread_fn(void *arg)
{
    (void)arg;
    LLOG(LLOG_INFO, "pgl: monitor thread started");

    while (g_pgl_running) {
        if (!g_pgl_event_open || g_pgl_process_event.revent == INVALID_HANDLE) {
            svcSleepThread(1000000000LL);
            continue;
        }

        Result rc = waitSingleHandle(g_pgl_process_event.revent, 1000000000LL);
        if (R_FAILED(rc)) continue;

        PmProcessEventInfo info;
        rc = pglEventObserverGetProcessEventInfo(&g_pgl_observer, &info);
        if (R_FAILED(rc)) {
            pgl_refresh_application_state(NULL);
            continue;
        }

        u64 current_pid = 0;
        pgl_get_state(&current_pid, NULL, NULL);

        switch (info.event) {
            case PmProcessEvent_Start:
            case PmProcessEvent_DebugStart:
                pgl_set_state(info.process_id, info.event, rc);
                break;

            case PmProcessEvent_Exit:
            case PmProcessEvent_Crash:
                if (current_pid == 0 || current_pid == info.process_id) {
                    pgl_set_state(0, info.event, rc);
                } else {
                    pgl_set_state(current_pid, info.event, rc);
                }
                break;

            default:
                pgl_set_state(current_pid, info.event, rc);
                break;
        }

        LLOG(LLOG_INFO, "pgl: app event=%s pid=%llu",
             pgl_process_event_name(info.event),
             (unsigned long long)info.process_id);
    }

    LLOG(LLOG_INFO, "pgl: monitor thread exiting");
}

static void pgl_monitor_start(void)
{
    if (!g_pgl_ready || g_pgl_thread_started) return;

    Result rc = pglGetEventObserver(&g_pgl_observer);
    if (R_FAILED(rc)) {
        LLOG(LLOG_WARNING, "pgl: pglGetEventObserver failed: 0x%x", rc);
        return;
    }
    g_pgl_observer_open = true;

    rc = pglEventObserverGetProcessEvent(&g_pgl_observer, &g_pgl_process_event);
    if (R_FAILED(rc)) {
        LLOG(LLOG_WARNING, "pgl: pglEventObserverGetProcessEvent failed: 0x%x", rc);
        pglEventObserverClose(&g_pgl_observer);
        g_pgl_observer_open = false;
        return;
    }
    g_pgl_event_open = true;
    g_pgl_running = true;

    rc = threadCreate(&g_pgl_thread, pgl_thread_fn, NULL,
                      s_pgl_stack, sizeof(s_pgl_stack), 31, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_WARNING, "pgl: threadCreate failed: 0x%x", rc);
        g_pgl_running = false;
        eventClose(&g_pgl_process_event);
        g_pgl_event_open = false;
        pglEventObserverClose(&g_pgl_observer);
        g_pgl_observer_open = false;
        return;
    }

    threadStart(&g_pgl_thread);
    g_pgl_thread_started = true;
}

static void pgl_monitor_stop(void)
{
    g_pgl_running = false;

    if (g_pgl_thread_started) {
        threadWaitForExit(&g_pgl_thread);
        threadClose(&g_pgl_thread);
        g_pgl_thread_started = false;
    }
    if (g_pgl_event_open) {
        eventClose(&g_pgl_process_event);
        g_pgl_event_open = false;
    }
    if (g_pgl_observer_open) {
        pglEventObserverClose(&g_pgl_observer);
        g_pgl_observer_open = false;
    }
    if (g_pgl_ready) {
        pglExit();
        g_pgl_ready = false;
    }
}

static void pgl_monitor_init_late(void)
{
    if (g_pgl_ready) return;

    Result sm_rc = smInitialize();
    if (R_FAILED(sm_rc)) {
        g_rc_pgl = sm_rc;
        LLOG(LLOG_WARNING, "pgl: smInitialize failed: 0x%x", sm_rc);
        return;
    }

    g_rc_pgl = pglInitialize();
    smExit();
    if (R_FAILED(g_rc_pgl)) {
        LLOG(LLOG_WARNING, "pgl: initialize failed: 0x%x", g_rc_pgl);
        return;
    }

    mutexInit(&g_pgl_mutex);
    g_pgl_mutex_ready = true;
    g_pgl_ready = true;

    if (pgl_refresh_application_state(NULL)) {
        u64 pid = 0;
        pgl_get_state(&pid, NULL, NULL);
        LLOG(LLOG_INFO, "pgl: current application pid=%llu", (unsigned long long)pid);
    } else {
        u64 pid = 0;
        PmProcessEvent event = PmProcessEvent_None;
        Result rc = 0;
        pgl_get_state(&pid, &event, &rc);
        LLOG(LLOG_INFO, "pgl: no running application yet (last=%s rc=0x%x)",
             pgl_process_event_name(event), rc);
    }

    pgl_monitor_start();
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */
extern "C" void __appInit(void)
{
    /* Open the service manager session FIRST! */
    Result sm_rc = smInitialize();
    if (R_FAILED(sm_rc)) {
        return; /* If SM fails, everything else will too */
    }

    g_rc_fs = fsInitialize();
    if (R_SUCCEEDED(g_rc_fs)) {
        Result mount_rc = fsdevMountSdmc();
        if (R_FAILED(mount_rc)) {
            g_rc_fs = mount_rc;
        } else {
            mkdir("sdmc:/config", 0777);
            mkdir("sdmc:/config/lan-play", 0777);
            mkdir("sdmc:/tmp", 0777);
        }
        /* ULTRA EARLY LOG for debugging Atmosphere 1.1.0 boots */
        LLOG(LLOG_INFO, "=== switch-lan-play sysmodule v1.14 EARLY BOOT ===");
    }

    mutexInit(&g_log_mutex);
    g_log_mutex_ready = true;

    g_rc_setsys = setsysInitialize();
    if (pm_monitors_enabled()) {
        g_rc_pscm = pscmInitialize();
    } else {
        g_rc_pscm = 0;
    }

    /* Network services are intentionally NOT initialized at boot.
     * They are opened lazily inside run_service(), after the foreground gate
     * sees a probable game/application. This keeps the sysmodule invisible to
     * Horizon network settings while idle. */
    g_rc_nifm = 0;
    g_rc_bsd = 0;
    g_rc_socket = 0;

    /* Foreground detector uses pm:shell + pm:info. It is intentionally
     * initialized later from main() after boot has settled; touching PM too
     * early can race system services during boot. */
    g_rc_fg = 0;

    /* Lightweight IPC gate used by ldn_mitm. This is not network I/O; it only
     * carries LDN activity state so the heavy LAN runtime can stay asleep
     * until a game actually enters local wireless mode. */
    g_rc_gate = ldn_gate_service_init();

    /* PGL is intentionally initialized later from main() after boot has
     * settled. This keeps one more system service out of the earliest boot
     * path while we are still chasing suspend/resume and HBL regressions. */
    g_rc_pgl = 0;

    /* Close the service manager session now that lookups are done */
    smExit();
}

extern "C" void __appExit(void)
{
    LLOG(LLOG_INFO, "__appExit: Cleaning up sysmodule...");
    psc_monitor_stop();
    pgl_monitor_stop();
    ldn_gate_service_exit();
    foreground_detector_exit();
    runtime_network_exit();
    LLOG(LLOG_INFO, "__appExit: Cleanup complete.");
    if (R_SUCCEEDED(g_rc_pscm)) pscmExit();
    fsdevUnmountAll();
    setsysExit();
    fsExit();
}

static void ensure_tmp_dir(void) {
    if (R_SUCCEEDED(g_rc_fs)) {
        mkdir("sdmc:/tmp", 0777);
    }
}

static void write_status_error(const char* error_msg) {
    if (R_FAILED(g_rc_fs)) return;
    ensure_tmp_dir();
    FILE *sf = fopen("sdmc:/tmp/lanplay.status", "w");
    if (sf) {
        fprintf(sf, "active=0\n");
        fprintf(sf, "error=%s\n", error_msg);
        fclose(sf);
    }
}

static bool foreground_gate_disabled_by_file(void)
{
    struct stat st;
    return stat("sdmc:/config/lan-play/disable_foreground_gate", &st) == 0;
}

/* PM-facing monitors are disabled by default to avoid service regressions on
 * some firmware/CFW combinations. Create this file to opt-in explicitly:
 *   sdmc:/config/lan-play/enable_pm_monitors */
static bool pm_monitors_enabled(void)
{
    struct stat st;
    return stat("sdmc:/config/lan-play/enable_pm_monitors", &st) == 0;
}

static bool foreground_gate_enabled(void)
{
    if (!pm_monitors_enabled()) return false;
    return !foreground_gate_disabled_by_file() && foreground_detector_available();
}

static void write_status_waiting_game(const foreground_state_t *fg);

static bool ldn_activity_gate_disabled_by_file(void)
{
    struct stat st;
    return stat("sdmc:/config/lan-play/disable_ldn_gate", &st) == 0;
}

static bool ldn_activity_gate_enabled(void)
{
    return ldn_gate_service_available() &&
           !ldn_activity_gate_disabled_by_file();
}

static void write_status_waiting_ldn(const foreground_state_t *fg, const lanp_gate_state_t *gate)
{
    if (R_FAILED(g_rc_fs)) return;
    ensure_tmp_dir();

    FILE *sf = fopen("sdmc:/tmp/lanplay.status", "w");
    if (!sf) return;

    fprintf(sf, "active=0\n");
    fprintf(sf, "state=waiting_ldn\n");
    fprintf(sf, "error=Esperando modo local/LDN\n");
    if (fg) {
        fprintf(sf, "foreground_reason=%s\n", foreground_state_reason(fg));
        fprintf(sf, "foreground_pid=%llu\n", (unsigned long long)fg->process_id);
        fprintf(sf, "foreground_title_id=%016llX\n", (unsigned long long)fg->title_id);
    }
    if (gate) {
        fprintf(sf, "ldn_active=%u\n", gate->active);
        fprintf(sf, "ldn_event=%s\n", ldn_gate_event_name(gate->event_type));
        fprintf(sf, "ldn_pid=%llu\n", (unsigned long long)gate->process_id);
        fprintf(sf, "ldn_title_id=%016llX\n", (unsigned long long)gate->title_id);
        fprintf(sf, "ldn_intent=%llu\n", (unsigned long long)gate->local_communication_id);
        fprintf(sf, "ldn_scene=%u\n", gate->scene_id);
    }
    fclose(sf);
}

static bool wait_for_ldn_activity(void)
{
    if (!ldn_activity_gate_enabled()) {
        if (!ldn_gate_service_available()) {
            LLOG(LLOG_WARNING, "ldn_gate: unavailable (0x%x), falling back to foreground-only startup", g_rc_gate);
        } else {
            LLOG(LLOG_WARNING, "ldn_gate: disabled by sdmc:/config/lan-play/disable_ldn_gate");
        }
        return true;
    }

    int quiet_ticks = 0;
    while (true) {
        if (pgl_monitor_available()) {
            u64 app_pid = 0;
            PmProcessEvent app_event = PmProcessEvent_None;
            Result app_rc = 0;
            if (!pgl_has_application(&app_pid, &app_event, &app_rc)) {
                write_status_waiting_game(NULL);
                if (quiet_ticks < 5 || (quiet_ticks % 30) == 0) {
                    LLOG(LLOG_INFO, "pgl: waiting for application before LDN (last=%s rc=0x%x)",
                         pgl_process_event_name(app_event), app_rc);
                }
                quiet_ticks++;

                struct stat reload_st;
                if (stat("sdmc:/tmp/lanplay.reload", &reload_st) == 0) {
                    unlink("sdmc:/tmp/lanplay.reload");
                    LLOG(LLOG_INFO, "pgl: reload trigger consumed while waiting for application");
                }

                svcSleepThread(1000000000LL);
                continue;
            }
        }

        lanp_gate_state_t gate;
        ldn_gate_get_state(&gate);
        if (ldn_gate_is_active()) {
            LLOG(LLOG_INFO, "ldn_gate: active event=%s pid=%llu intent=%llu scene=%u; starting LAN runtime",
                 ldn_gate_event_name(gate.event_type),
                 (unsigned long long)gate.process_id,
                 (unsigned long long)gate.local_communication_id,
                 gate.scene_id);
            return true;
        }

        write_status_waiting_ldn(NULL, &gate);
        if (quiet_ticks < 5 || (quiet_ticks % 30) == 0) {
            LLOG(LLOG_INFO, "ldn_gate: waiting for LDN activity (last=%s active=%u)",
                 ldn_gate_event_name(gate.event_type), gate.active);
        }
        quiet_ticks++;

        struct stat reload_st;
        if (stat("sdmc:/tmp/lanplay.reload", &reload_st) == 0) {
            unlink("sdmc:/tmp/lanplay.reload");
            LLOG(LLOG_INFO, "ldn_gate: reload trigger consumed while waiting for LDN");
        }

        svcSleepThread(1000000000LL);
    }
}

static bool ldn_runtime_still_allowed(const char *phase)
{
    if (ldn_activity_gate_enabled() && pgl_monitor_available()) {
        u64 app_pid = 0;
        PmProcessEvent app_event = PmProcessEvent_None;
        Result app_rc = 0;
        if (!pgl_has_application(&app_pid, &app_event, &app_rc)) {
            LLOG(LLOG_INFO, "pgl: application missing during %s (last=%s rc=0x%x) — stopping LAN runtime",
                 phase ? phase : "runtime", pgl_process_event_name(app_event), app_rc);
            write_status_waiting_game(NULL);
            return false;
        }
    }

    if (!ldn_activity_gate_enabled()) return true;

    if (ldn_gate_is_active()) return true;

    lanp_gate_state_t gate;
    ldn_gate_get_state(&gate);
    LLOG(LLOG_INFO, "ldn_gate: inactive during %s (last=%s active=%u) — stopping LAN runtime",
         phase ? phase : "runtime", ldn_gate_event_name(gate.event_type), gate.active);
    write_status_waiting_ldn(NULL, &gate);
    return false;
}

static void write_status_waiting_game(const foreground_state_t *fg)
{
    if (R_FAILED(g_rc_fs)) return;
    ensure_tmp_dir();

    FILE *sf = fopen("sdmc:/tmp/lanplay.status", "w");
    if (!sf) return;

    fprintf(sf, "active=0\n");
    fprintf(sf, "state=waiting_game\n");
    fprintf(sf, "error=Esperando juego en primer plano\n");
    if (fg) {
        fprintf(sf, "foreground_reason=%s\n", foreground_state_reason(fg));
        fprintf(sf, "foreground_pid=%llu\n", (unsigned long long)fg->process_id);
        fprintf(sf, "foreground_title_id=%016llX\n", (unsigned long long)fg->title_id);
        fprintf(sf, "foreground_rc=0x%08X\n", fg->last_rc);
    }
    fclose(sf);
}

static bool wait_for_foreground_game(void)
{
    if (!foreground_gate_enabled()) {
        if (!foreground_detector_available()) {
            LLOG(LLOG_WARNING, "foreground: detector unavailable (0x%x), using legacy always-on startup", g_rc_fg);
        } else {
            LLOG(LLOG_WARNING, "foreground: gate disabled by sdmc:/config/lan-play/disable_foreground_gate");
        }
        return true;
    }

    int quiet_ticks = 0;
    while (true) {
        foreground_state_t fg;
        if (foreground_has_game(&fg)) {
            LLOG(LLOG_INFO, "foreground: game candidate detected title_id=%016llX pid=%llu",
                 (unsigned long long)fg.title_id, (unsigned long long)fg.process_id);
            return true;
        }

        write_status_waiting_game(&fg);
        if (quiet_ticks < 5 || (quiet_ticks % 30) == 0) {
            LLOG(LLOG_INFO, "foreground: waiting for game (%s rc=0x%x)",
                 foreground_state_reason(&fg), fg.last_rc);
        }
        quiet_ticks++;

        struct stat reload_st;
        if (stat("sdmc:/tmp/lanplay.reload", &reload_st) == 0) {
            unlink("sdmc:/tmp/lanplay.reload");
            LLOG(LLOG_INFO, "foreground: reload trigger consumed while waiting for game");
        }

        svcSleepThread(1000000000LL);
    }
}

static bool foreground_runtime_still_allowed(const char *phase)
{
    if (ldn_activity_gate_enabled()) return true;
    if (!foreground_gate_enabled()) return true;

    foreground_state_t fg;
    if (foreground_has_game(&fg)) return true;

    LLOG(LLOG_INFO, "foreground: game disappeared during %s (%s) — stopping runtime",
         phase ? phase : "runtime", foreground_state_reason(&fg));
    write_status_waiting_game(&fg);
    return false;
}

static int run_service(void)
{
    g_sleep_teardown_requested = false;

    if (R_SUCCEEDED(g_rc_fs)) ensure_tmp_dir();

    LLOG(LLOG_INFO, "=== switch-lan-play sysmodule v1.14 starting ===");

    if (runtime_network_init() != 0) {
        write_status_error("Runtime network init failed");
        svcSleepThread(3000000000LL);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* 0. Wait for Network to be fully established by Horizon (Boot time) */
    /* ------------------------------------------------------------------ */
    LLOG(LLOG_INFO, "Waiting for active Internet Connection...");
    u32 g_local_wifi_ip = 0; /* captured here, stored in lp->wifi_ip later */
    int wait_seconds = 0;
    while (true) {
        u32 out_ip = 0;
        if (R_SUCCEEDED(g_rc_nifm)) {
            nifmGetCurrentIpAddress(&out_ip);
            if (out_ip != 0) {
                g_local_wifi_ip = out_ip;
                LLOG(LLOG_INFO, "Network is UP and stabilized!");
                break;
            }
        }

        /* Update status so HBAPP knows we are alive but waiting */
        write_status_error("Esperando WiFi...");

        if (!foreground_runtime_still_allowed("wifi wait") || !ldn_runtime_still_allowed("wifi wait")) {
            runtime_network_exit();
            return 1;
        }

        /* Only log every 30 seconds after the first minute to save SD wear */
        if (wait_seconds < 60 || (wait_seconds % 30 == 0)) {
            LLOG(LLOG_DEBUG, "Still waiting for WiFi (T+%ds)...", wait_seconds);
        }

        svcSleepThread(2000000000LL); /* 2 seconds */
        wait_seconds += 2;
    }

    /* Give Horizon extra time to set up default gateway & routing table.
     * nifmGetCurrentIpAddress can return an IP before the route is ready,
     * causing "Host is unreachable" on the first sendto().  A short
     * stabilization sleep avoids this race condition.                      */
    LLOG(LLOG_INFO, "Waiting 5s for routing table to stabilize...");
    svcSleepThread(5000000000LL); /* 5 seconds */

    /* ------------------------------------------------------------------ */
    /* 1. Read config                                                       */
    /* ------------------------------------------------------------------ */
    nx_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    if (nx_config_load(&cfg) != 0) {
        LLOG(LLOG_WARNING, "Config missing. Waiting for app to configure.");
        
        /* Loop until a reload is triggered */
        while (true) {
            FILE *sf = fopen("sdmc:/tmp/lanplay.status", "w");
            if (sf) { fprintf(sf, "active=0\n"); fclose(sf); }

            struct stat st;
            if (stat("sdmc:/tmp/lanplay.reload", &st) == 0) {
                unlink("sdmc:/tmp/lanplay.reload");
                runtime_network_exit();
                return 1; /* Trigger reload loop */
            }
            if (!foreground_runtime_still_allowed("config wait") || !ldn_runtime_still_allowed("config wait")) {
                runtime_network_exit();
                return 1;
            }
            svcSleepThread(2000000000LL);
        }
    }

    /* Auto-assign a unique IP from the device serial if not set in config */
    nx_config_auto_ip(&cfg);

    /* ------------------------------------------------------------------ */
    /* 2. Allocate & zero the main context                                  */
    /* ------------------------------------------------------------------ */
    struct lan_play *lp = (struct lan_play *)malloc(sizeof(*lp));
    if (!lp) {
        LLOG(LLOG_ERROR, "Out of memory allocating lan_play context");
        write_status_error("Out of memory");
        svcSleepThread(3000000000LL);
        runtime_network_exit();
        return 1;
    }
    memset(lp, 0, sizeof(*lp));
    lp->bpf_fd   = -1;
    lp->relay_fd = -1;
    lp->running  = true;
    lp->pmtu     = 0; /* no fragmentation by default */
    lp->wifi_ip  = g_local_wifi_ip; /* local WiFi IP for self-echo filtering in tap_recv */

    mutexInit(&lp->mutex);
    g_active_lp = lp;

    /* ------------------------------------------------------------------ */
    /* 3. Resolve relay server address                                      */
    /* ------------------------------------------------------------------ */
    while (resolve_server(cfg.relay_addr, &lp->server_addr) != 0) {
        LLOG(LLOG_WARNING, "Relay DNS not ready for '%s' — retrying...", cfg.relay_addr);
        write_status_error("Esperando DNS del relay...");

        struct stat rst;
        if (stat("sdmc:/tmp/lanplay.reload", &rst) == 0) {
            unlink("sdmc:/tmp/lanplay.reload");
            lan_play_free(lp);
            runtime_network_exit();
            return 1;
        }

        if (!foreground_runtime_still_allowed("relay DNS wait") || !ldn_runtime_still_allowed("relay DNS wait")) {
            lan_play_free(lp);
            runtime_network_exit();
            return 1;
        }

        svcSleepThread(2000000000LL);
    }

    /* ------------------------------------------------------------------ */
    /* 3b. Route probe — verify we can actually reach the relay IP          */
    /* ------------------------------------------------------------------ */
    {
        struct sockaddr_in probe_dst = lp->server_addr;
        int probe_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (probe_fd >= 0) {
            int probe_ok = 0;
            for (int attempt = 0; attempt < 15; attempt++) {
                uint8_t dummy = 0xFF;
                ssize_t r = sendto(probe_fd, &dummy, 1, 0,
                                   (struct sockaddr *)&probe_dst,
                                   sizeof(probe_dst));
                if (r >= 0) {
                    probe_ok = 1;
                    LLOG(LLOG_INFO, "Route probe OK (attempt %d)", attempt + 1);
                    break;
                }
                LLOG(LLOG_DEBUG, "Route probe attempt %d: %s", attempt + 1, strerror(errno));
                svcSleepThread(2000000000LL); /* 2 s */
            }
            close(probe_fd);
            if (!probe_ok) {
                LLOG(LLOG_WARNING, "Route probe never succeeded — proceeding anyway");
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* 4. Parse network config                                              */
    /* ------------------------------------------------------------------ */
    struct in_addr ip_addr, net_addr, mask_addr;
    if (!inet_aton(cfg.my_ip,       &ip_addr)  ||
        !inet_aton(cfg.subnet_net,  &net_addr)  ||
        !inet_aton(cfg.subnet_mask, &mask_addr)) {
        LLOG(LLOG_ERROR, "Invalid IP/subnet in config (ip=%s net=%s mask=%s)",
             cfg.my_ip, cfg.subnet_net, cfg.subnet_mask);
        write_status_error("Invalid IP/subnet config");
        g_active_lp = NULL;
        lan_play_free(lp);
        svcSleepThread(3000000000LL);
        runtime_network_exit();
        return 1;
    }
    memcpy(lp->my_ip, &ip_addr.s_addr, 4);

    /* ------------------------------------------------------------------ */
    /* 5. Initialise packet_ctx (virtual LAN interface state)               */
    /* ------------------------------------------------------------------ */
    packet_init(&lp->packet_ctx, lp,
                g_pkt_buffer, sizeof(g_pkt_buffer),
                &ip_addr.s_addr,
                &net_addr.s_addr,
                &mask_addr.s_addr,
                300 /* ARP TTL seconds */);

    /* ------------------------------------------------------------------ */
    /* 6. Auth — keep username in lp->username (freed by lan_play_free)    */
    /* ------------------------------------------------------------------ */
    if (cfg.username[0] != '\0') {
        lp->username = strdup(cfg.username);
        if (!lp->username) {
            LLOG(LLOG_ERROR, "Out of memory for username");
            write_status_error("Out of memory for username");
            lan_play_free(lp);
            svcSleepThread(3000000000LL);
            runtime_network_exit();
            return 1;
        }
        hash_password(lp, cfg.password);
        /* Scrub password string from config struct */
        memset(cfg.password, 0, sizeof(cfg.password));
        LLOG(LLOG_INFO, "Auth enabled for user '%s'", lp->username);
    }

    /* ------------------------------------------------------------------ */
    /* 7. TAP init                                                          */
    /* ------------------------------------------------------------------ */
    if (tap_init(lp) != 0) {
        LLOG(LLOG_ERROR, "TAP init failed — aborting");
        write_status_error("TAP interface init failed");
        g_active_lp = NULL;
        lan_play_free(lp);
        svcSleepThread(3000000000LL);
        runtime_network_exit();
        return 1;
    }
    /* Set packet_ctx MAC to our virtual MAC now that TAP is ready */
    packet_set_mac(&lp->packet_ctx, lp->my_mac);

    /* ------------------------------------------------------------------ */
    /* 8. Relay client init                                                 */
    /* ------------------------------------------------------------------ */
    if (lan_client_init(lp) != 0) {
        LLOG(LLOG_ERROR, "Relay client init failed — aborting");
        write_status_error("Relay client init failed");
        tap_close(lp);
        g_active_lp = NULL;
        lan_play_free(lp);
        svcSleepThread(3000000000LL);
        runtime_network_exit();
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* 8b. LDN Bridge init (captures ldn_mitm traffic for relay)           */
    /* ------------------------------------------------------------------ */
    if (ldn_bridge_init(lp) != 0) {
        LLOG(LLOG_WARNING, "LDN bridge init failed — LDN games won't work via relay");
        /* Non-fatal: native LAN Play games still work via TAP */
    }

    /* ------------------------------------------------------------------ */
    /* 9. Start background threads                                          */
    /* ------------------------------------------------------------------ */
    Result rc;
    bool tap_started       = false;
    bool relay_started     = false;
    bool keepalive_started = false;
    bool ldn_udp_started   = false;
    bool ldn_tcp_started   = false;
    s32 base_prio = 31; /* Safe static priority for sysmodule */

    rc = threadCreate(&lp->tap_thread, tap_recv_thread_fn, lp,
                      s_tap_stack, sizeof(s_tap_stack), base_prio, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_ERROR, "threadCreate tap failed: 0x%x (prio %d)", rc, base_prio);
        write_status_error("Failed to create tap thread");
        goto cleanup;
    }

    rc = threadCreate(&lp->relay_thread, lan_client_recv_thread_fn, lp,
                      s_relay_stack, sizeof(s_relay_stack), base_prio, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_ERROR, "threadCreate relay failed: 0x%x (prio %d)", rc, base_prio);
        write_status_error("Failed to create relay thread");
        threadClose(&lp->tap_thread);
        goto cleanup;
    }

    rc = threadCreate(&lp->keepalive_thread, lan_client_keepalive_thread_fn, lp,
                      s_keepalive_stack, sizeof(s_keepalive_stack), base_prio, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_ERROR, "threadCreate keepalive failed: 0x%x", rc);
        write_status_error("Failed to create keepalive thread");
        threadClose(&lp->tap_thread);
        threadClose(&lp->relay_thread);
        goto cleanup;
    }

    threadStart(&lp->tap_thread);      tap_started      = true;
    threadStart(&lp->relay_thread);    relay_started    = true;
    threadStart(&lp->keepalive_thread); keepalive_started = true;

    /* LDN bridge threads (optional — may fail if bridge init failed) */
    rc = threadCreate(&lp->ldn_udp_thread, ldn_bridge_udp_thread_fn, lp,
                      s_ldn_udp_stack, sizeof(s_ldn_udp_stack), base_prio, -2);
    if (R_SUCCEEDED(rc)) {
        threadStart(&lp->ldn_udp_thread);
        ldn_udp_started = true;
    } else {
        LLOG(LLOG_WARNING, "ldn_bridge UDP thread create failed: 0x%x", rc);
    }

    rc = threadCreate(&lp->ldn_tcp_thread, ldn_bridge_tcp_thread_fn, lp,
                      s_ldn_tcp_stack, sizeof(s_ldn_tcp_stack), base_prio, -2);
    if (R_SUCCEEDED(rc)) {
        threadStart(&lp->ldn_tcp_thread);
        ldn_tcp_started = true;
    } else {
        LLOG(LLOG_WARNING, "ldn_bridge TCP thread create failed: 0x%x", rc);
    }

    /* Write everything in a single LLOG block to prevent FatFS concurrent fopen locking drops */
    LLOG(LLOG_INFO, "ALL threads started. LAN relay is ACTIVE. My IP: %s | Relay: %s", cfg.my_ip, cfg.relay_addr);

    /* ------------------------------------------------------------------ */
    /* 10. Service Loop (Restartable without reboot)                       */
    /* ------------------------------------------------------------------ */
    while (true) {
        if (g_sleep_teardown_requested || !lp->running) {
            LLOG(LLOG_INFO, "runtime: sleep teardown requested, stopping service loop");
            write_status_error("Suspendiendo: cerrando red para modo reposo");
            break;
        }

        /* Write status for the Homebrew App to read */
        FILE *sf = fopen("sdmc:/tmp/lanplay.status", "w");
        if (sf) {
            fprintf(sf, "active=1\n");
            fprintf(sf, "error=\n");
            fprintf(sf, "relay=%s\n", cfg.relay_addr);
            fprintf(sf, "up_pkt=%llu\n", (unsigned long long)lp->upload_packet);
            fprintf(sf, "up_bytes=%llu\n", (unsigned long long)lp->upload_byte);
            fprintf(sf, "dn_pkt=%llu\n", (unsigned long long)lp->download_packet);
            fprintf(sf, "dn_bytes=%llu\n", (unsigned long long)lp->download_byte);
            fclose(sf);
        }

        /* Check for reload trigger from the Homebrew App */
        struct stat st;
        if (stat("sdmc:/tmp/lanplay.reload", &st) == 0) {
            LLOG(LLOG_INFO, "Reload trigger detected — restarting service...");
            unlink("sdmc:/tmp/lanplay.reload");
            
            /* Give FS time to commit the config.ini changes from hbapp */
            svcSleepThread(1000000000LL);
            break; /* Exit inner loop to trigger reload */
        }

        if (foreground_gate_enabled()) {
            foreground_state_t fg;
            if (!foreground_has_game(&fg)) {
                LLOG(LLOG_INFO, "foreground: game disappeared (%s) — stopping LAN runtime",
                     foreground_state_reason(&fg));
                write_status_waiting_game(&fg);
                break;
            }
        }

        if (!ldn_runtime_still_allowed("service loop")) {
            break;
        }

        svcSleepThread(2000000000LL); /* 2 s check */
    }

    /* ------------------------------------------------------------------ */
    /* 11. Cleanup (before reload or exit)                                 */
    /* ------------------------------------------------------------------ */
cleanup:
    LLOG(LLOG_INFO, "Shutting down threads for reload...");
    lp->running = false;

    /* Close sockets first to unblock recvfrom()/accept() users quickly. */
    ldn_bridge_close(lp);
    lan_client_close(lp);
    tap_close(lp);

    if (ldn_tcp_started) {
        threadWaitForExit(&lp->ldn_tcp_thread);
        threadClose(&lp->ldn_tcp_thread);
    }
    if (ldn_udp_started) {
        threadWaitForExit(&lp->ldn_udp_thread);
        threadClose(&lp->ldn_udp_thread);
    }
    if (keepalive_started) {
        threadWaitForExit(&lp->keepalive_thread);
        threadClose(&lp->keepalive_thread);
    }
    if (relay_started) {
        threadWaitForExit(&lp->relay_thread);
        threadClose(&lp->relay_thread);
    }
    if (tap_started) {
        threadWaitForExit(&lp->tap_thread);
        threadClose(&lp->tap_thread);
    }

    g_active_lp = NULL;
    lan_play_free(lp);
    runtime_network_exit();

    /* Small delay before loop restart */
    svcSleepThread(500000000LL);
    return 1; /* returning 1 here would exit main, we need the loop outside */
}

/* Updated main with the retry/reload loop */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* RECOVERY: Ensure directories exist as early as possible for logging */
    /* TRUNCATE LOG: root visibility */
    if (R_SUCCEEDED(g_rc_fs)) {
        mkdir("sdmc:/config", 0777);
        mkdir("sdmc:/config/lan-play", 0777);
        FILE *f_init = fopen("sdmc:/lan-play.log", "w");
        if (f_init) {
            fprintf(f_init, "=== switch-lan-play sysmodule v1.14 (CLEAN START) ===\n");
            fclose(f_init);
        }
    }

    LLOG(LLOG_INFO, "=== switch-lan-play sysmodule v1.14 starting ===");
    LLOG(LLOG_INFO, "Init Results (0x0 = Success):");
    LLOG(LLOG_INFO, "  FS:     0x%08X", g_rc_fs);
    LLOG(LLOG_INFO, "  SetSys: 0x%08X", g_rc_setsys);
    LLOG(LLOG_INFO, "  Network: lazy init enabled");
    if (pm_monitors_enabled()) {
        LLOG(LLOG_INFO, "  FG:     lazy init pending");
        LLOG(LLOG_INFO, "  PGL:    late init pending");
    } else {
        LLOG(LLOG_INFO, "  FG:     disabled (pm monitors off)");
        LLOG(LLOG_INFO, "  PGL:    disabled (pm monitors off)");
    }
    LLOG(LLOG_INFO, "  Gate:   0x%08X", g_rc_gate);
    LLOG(LLOG_INFO, "  PSC:    %s", pm_monitors_enabled() ? "enabled" : "disabled");

    if (R_FAILED(g_rc_fs)) {
        LLOG(LLOG_ERROR, "CRITICAL: FS failed to initialize. Check SD card/mount state.");
        svcSleepThread(5000000000LL);
        return 0;
    }

    if (pm_monitors_enabled()) {
        /* Give Horizon boot services time to settle before touching PM/PGL. */
        for (int i = 0; i < 15; i++) svcSleepThread(1000000000LL);

        pgl_monitor_init_late();
        LLOG(LLOG_INFO, "  PGL:    0x%08X", g_rc_pgl);

        psc_monitor_start();
    } else {
        LLOG(LLOG_INFO, "pm monitors disabled: skipping pgl/psc init");
    }

    int restart_count = 0;
    while (true) {
        if (ldn_activity_gate_enabled()) {
            LLOG(LLOG_INFO, "  FG:     skipped (ldn_gate primary)");
            if (!wait_for_ldn_activity()) {
                svcSleepThread(1000000000LL);
                continue;
            }
        } else {
            if (pm_monitors_enabled()) {
                g_rc_fg = foreground_detector_init();
                LLOG(LLOG_INFO, "  FG:     0x%08X", g_rc_fg);
                wait_for_foreground_game();
            } else {
                LLOG(LLOG_WARNING, "foreground gate unavailable: pm monitors disabled");
            }
        }
        restart_count = 0;
        if (run_service() == 0) break;
        restart_count++;
        /* Exponential backoff: 3s, 6s, 12s, ... capped at 60s */
        int delay = 3;
        for (int i = 1; i < restart_count && delay < 60; i++) delay *= 2;
        if (delay > 60) delay = 60;
        LLOG(LLOG_INFO, "Restarting service loop (attempt #%d, backoff %ds)...", restart_count, delay);
        svcSleepThread((s64)delay * 1000000000LL);
    }

    psc_monitor_stop();
    return 0;
}
