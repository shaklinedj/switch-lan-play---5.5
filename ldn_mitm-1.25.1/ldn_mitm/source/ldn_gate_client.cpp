#include "ldn_gate_client.hpp"
#include "debug.hpp"

namespace ams::mitm::ldn {

    namespace {
        constexpr const char *GateServiceName = "lanp:gt";
        constexpr u32 GateMagic = 0x47504E4C; /* 'LNPG' little-endian */
        constexpr u32 GateVersion = 1;
        constexpr u32 GateCmdNotify = 0;

        struct GateEvent {
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
        };

        const char *GateEventName(LanPlayGateEvent event_type) {
            switch (event_type) {
                case LanPlayGateEvent::Scan: return "scan";
                case LanPlayGateEvent::Host: return "host";
                case LanPlayGateEvent::Connect: return "connect";
                case LanPlayGateEvent::Idle: return "idle";
                case LanPlayGateEvent::Finalize: return "finalize";
                default: return "none";
            }
        }
    }

    Result NotifyLanPlayGate(LanPlayGateEvent event_type,
                             u64 process_id,
                             u64 title_id,
                             u64 local_communication_id,
                             u16 scene_id) {
        Service srv = {};
        Result rc = smGetService(&srv, GateServiceName);
        if (R_FAILED(rc)) {
            /* lan-play may not be installed or may still be booting. This is
             * intentionally non-fatal: LDN must continue to work locally. */
            static int miss_count = 0;
            if (++miss_count <= 3 || (miss_count % 60) == 0) {
                LogFormat("lanp_gate: service unavailable rc=0x%x event=%s", rc, GateEventName(event_type));
            }
            return rc;
        }

        GateEvent ev = {};
        ev.magic = GateMagic;
        ev.version = GateVersion;
        ev.event_type = static_cast<u32>(event_type);
        ev.process_id = process_id;
        ev.title_id = title_id;
        ev.local_communication_id = local_communication_id;
        ev.scene_id = scene_id;

        rc = serviceDispatchIn(&srv, GateCmdNotify, ev);
        serviceClose(&srv);

        if (R_SUCCEEDED(rc)) {
            LogFormat("lanp_gate: notified event=%s pid=%" PRIu64 " intent=%" PRIu64 " scene=%u",
                      GateEventName(event_type), process_id, local_communication_id, scene_id);
        } else {
            LogFormat("lanp_gate: notify failed rc=0x%x event=%s", rc, GateEventName(event_type));
        }
        return rc;
    }

    Result NotifyLanPlayGateFromIntent(LanPlayGateEvent event_type,
                                       u64 process_id,
                                       u64 title_id,
                                       const IntentId &intent_id) {
        return NotifyLanPlayGate(event_type,
                                 process_id,
                                 title_id,
                                 intent_id.localCommunicationId,
                                 intent_id.sceneId);
    }
}
