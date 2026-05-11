#pragma once

#include "nx_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LANP_GATE_SERVICE_NAME "lanp:gt"
#define LANP_GATE_MAGIC 0x47504E4CUL /* 'LNPG' little-endian */
#define LANP_GATE_VERSION 1
#define LANP_GATE_TIMEOUT_SECONDS 30
#define LANP_GATE_TICKS_PER_SECOND 19200000ULL

typedef enum {
    LANP_GATE_EVENT_NONE = 0,
    LANP_GATE_EVENT_SCAN = 1,
    LANP_GATE_EVENT_HOST = 2,
    LANP_GATE_EVENT_CONNECT = 3,
    LANP_GATE_EVENT_IDLE = 4,
    LANP_GATE_EVENT_FINALIZE = 5,
} lanp_gate_event_type_t;

typedef struct {
    u32 magic;
    u32 version;
    u32 event_type;
    u32 active;
    u64 process_id;
    u64 title_id;
    u64 local_communication_id;
    u64 last_event_tick;
    u16 scene_id;
    u16 reserved0;
    u32 reserved1;
} lanp_gate_state_t;

typedef struct {
    u32 magic;
    u32 version;
    u32 event_type;
    u32 reserved0;
    u64 process_id;
    u64 title_id;
    u64 local_communication_id;
    u16 scene_id;
    u16 reserved1;
    u32 reserved2;
} lanp_gate_event_t;

Result ldn_gate_service_init(void);
void   ldn_gate_service_exit(void);
bool   ldn_gate_service_available(void);
void   ldn_gate_get_state(lanp_gate_state_t *out);
bool   ldn_gate_is_active(void);
const char *ldn_gate_event_name(u32 event_type);

#ifdef __cplusplus
}
#endif
