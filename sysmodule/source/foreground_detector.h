/**
 * foreground_detector.h — lightweight application foreground gate.
 *
 * This intentionally avoids touching BSD/NIFM/network services. It only asks
 * Process Manager whether a user Application process exists and resolves its
 * program/title id. The runtime manager can use this to keep LAN Play invisible
 * until a real game/application is present.
 */
#pragma once

#include "nx_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct foreground_state {
    bool available;        /* pm:shell + pm:info were initialized */
    bool has_application;  /* PM reports an application process */
    bool is_game;          /* application title id passed our system-title filters */
    u64  process_id;
    u64  title_id;
    Result last_rc;
} foreground_state_t;

/**
 * Initialize PM services used by the detector.
 * Must be called while SM is available, ideally from __appInit().
 * Failure is non-fatal; the runtime can fall back to legacy behavior.
 */
Result foreground_detector_init(void);

/** Release PM services. */
void foreground_detector_exit(void);

/** True if PM services are available. */
bool foreground_detector_available(void);

/**
 * Poll current application process/title state.
 * Returns true only when a probable user game/application is present.
 */
bool foreground_has_game(foreground_state_t *out_state);

/** Return true for known system/applets that must not activate LAN Play. */
bool foreground_is_system_title(u64 title_id);

/** Helper for status/log formatting. */
const char *foreground_state_reason(const foreground_state_t *state);

#ifdef __cplusplus
}
#endif
