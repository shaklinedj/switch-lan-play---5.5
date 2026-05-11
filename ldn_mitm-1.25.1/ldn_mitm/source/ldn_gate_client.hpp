#pragma once

#include <switch.h>
#include "ldn_types.hpp"

namespace ams::mitm::ldn {

    enum class LanPlayGateEvent : u32 {
        None = 0,
        Scan = 1,
        Host = 2,
        Connect = 3,
        Idle = 4,
        Finalize = 5,
    };

    Result NotifyLanPlayGate(LanPlayGateEvent event_type,
                             u64 process_id,
                             u64 title_id,
                             u64 local_communication_id,
                             u16 scene_id);

    Result NotifyLanPlayGateFromIntent(LanPlayGateEvent event_type,
                                       u64 process_id,
                                       u64 title_id,
                                       const IntentId &intent_id);
}
