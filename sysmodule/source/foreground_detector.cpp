/**
 * foreground_detector.cpp — Process Manager based application gate.
 *
 * Important limitation:
 * pm:shell tells us whether an Application process exists. It does not prove
 * that the game is visually in focus when HOME is open. This still prevents the
 * LAN runtime from starting at boot/Home Menu/no-game, and it stops the runtime
 * when the application exits. A later LDN/activity gate should be layered on top
 * for stricter “only while actively playing local wireless/LAN” behavior.
 */
#include "foreground_detector.h"

static bool   g_pm_ready = false;
static Result g_pm_init_rc = 0;

Result foreground_detector_init(void)
{
    if (g_pm_ready) return 0;

    /* This detector may be initialized from main(), long after __appInit()
     * closed SM. Open a short-lived SM session around PM service lookup so we
     * do not require global SM lifetime. */
    Result rc_sm = smInitialize();
    if (R_FAILED(rc_sm)) {
        g_pm_init_rc = rc_sm;
        LLOG(LLOG_WARNING, "foreground: smInitialize failed: 0x%x", rc_sm);
        return rc_sm;
    }

    Result rc_shell = pmshellInitialize();
    if (R_FAILED(rc_shell)) {
        smExit();
        g_pm_init_rc = rc_shell;
        LLOG(LLOG_WARNING, "foreground: pmshellInitialize failed: 0x%x", rc_shell);
        return rc_shell;
    }

    Result rc_info = pminfoInitialize();
    if (R_FAILED(rc_info)) {
        pmshellExit();
        smExit();
        g_pm_init_rc = rc_info;
        LLOG(LLOG_WARNING, "foreground: pminfoInitialize failed: 0x%x", rc_info);
        return rc_info;
    }

    smExit();

    g_pm_ready = true;
    g_pm_init_rc = 0;
    LLOG(LLOG_INFO, "foreground: PM detector initialized");
    return 0;
}

void foreground_detector_exit(void)
{
    if (!g_pm_ready) return;
    pminfoExit();
    pmshellExit();
    g_pm_ready = false;
}

bool foreground_detector_available(void)
{
    return g_pm_ready;
}

bool foreground_is_system_title(u64 title_id)
{
    if (title_id == 0) return true;

    /* System modules/applets live in the low 010000000000xxxx area. */
    if (title_id >= 0x0100000000000000ULL &&
        title_id <  0x0100000000010000ULL) {
        return true;
    }

    /* Explicitly block common shell/applets just in case title override or
     * firmware-specific behavior makes them appear as Application. */
    switch (title_id) {
        case 0x0100000000001000ULL: /* qlaunch / HOME menu */
        case 0x0100000000001001ULL:
        case 0x0100000000001002ULL:
        case 0x0100000000001003ULL:
        case 0x0100000000001004ULL:
        case 0x0100000000001005ULL:
        case 0x0100000000001006ULL:
        case 0x0100000000001007ULL:
        case 0x0100000000001008ULL:
        case 0x0100000000001009ULL:
        case 0x010000000000100AULL:
        case 0x010000000000100BULL:
        case 0x010000000000100CULL:
        case 0x010000000000100DULL: /* album applet related */
        case 0x010000000000100EULL:
        case 0x010000000000100FULL:
        case 0x0100000000001010ULL:
        case 0x0100000000001011ULL:
        case 0x0100000000001012ULL:
        case 0x0100000000001013ULL:
            return true;
        default:
            break;
    }

    return false;
}

static void foreground_clear_state(foreground_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->available = g_pm_ready;
    state->last_rc = g_pm_init_rc;
}

bool foreground_has_game(foreground_state_t *out_state)
{
    foreground_state_t tmp;
    foreground_clear_state(&tmp);

    if (!g_pm_ready) {
        if (out_state) *out_state = tmp;
        return false;
    }

    u64 pid = 0;
    Result rc = pmshellGetApplicationProcessIdForShell(&pid);
    tmp.last_rc = rc;

    if (R_FAILED(rc) || pid == 0) {
        if (out_state) *out_state = tmp;
        return false;
    }

    tmp.has_application = true;
    tmp.process_id = pid;

    u64 title_id = 0;
    rc = pminfoGetProgramId(&title_id, pid);
    tmp.last_rc = rc;

    if (R_FAILED(rc) || title_id == 0) {
        if (out_state) *out_state = tmp;
        return false;
    }

    tmp.title_id = title_id;
    tmp.is_game = !foreground_is_system_title(title_id);

    if (out_state) *out_state = tmp;
    return tmp.is_game;
}

const char *foreground_state_reason(const foreground_state_t *state)
{
    if (!state) return "unknown";
    if (!state->available) return "pm unavailable";
    if (!state->has_application) return "no application process";
    if (state->title_id == 0) return "no program id";
    if (!state->is_game) return "system/applet title ignored";
    return "game candidate";
}
