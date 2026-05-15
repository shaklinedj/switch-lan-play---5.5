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

        /* Pending event slot - written by handler threads, drained by worker thread.
         * Handlers NEVER call serviceDispatchIn (svcSendSyncRequest from a Stratosphere
         * handler thread = potential deadlock). Only the worker thread sends IPC.
         * generation increments on every publish so the worker can tell whether a
         * newer state arrived while an older one was being delivered. */
        struct PendingSlot {
            bool valid;
            u64 generation;
            GateEvent ev;
        };
        constinit os::SdkMutex g_pending_mutex;
        PendingSlot g_pending_slot = {};

        /* Worker thread - managed by Stratosphere like the rest of ldn_mitm.
         * Runs two phases in a loop:
         *   1. Connect: retry smGetService until success.
         *   2. Deliver: poll g_pending_slot every 100 ms and call serviceDispatchIn.
         *      On dispatch failure → close session → back to phase 1. */
        constexpr size_t GateWorkerStackSize = 0x4000; /* 16 KB */
        alignas(os::MemoryPageSize) u8 g_worker_stack[GateWorkerStackSize];
        os::ThreadType g_worker_thread;

        const char *GateEventName(LanPlayGateEvent event_type) {
            switch (event_type) {
                case LanPlayGateEvent::Scan:     return "scan";
                case LanPlayGateEvent::Host:     return "host";
                case LanPlayGateEvent::Connect:  return "connect";
                case LanPlayGateEvent::Idle:     return "idle";
                case LanPlayGateEvent::Finalize: return "finalize";
                case LanPlayGateEvent::Prepare:  return "prepare";
                default:                         return "none";
            }
        }

        void GateWorkerThreadFunc(void *) {
            while (true) {
                /* --- Phase 1: Connect --- */
                Service srv = {};
                for (int i = 0; ; i++) {
                    Result rc = smGetService(&srv, GateServiceName);
                    if (R_SUCCEEDED(rc)) {
                        LogFormat("lanp_gate: connected attempt=%d", i + 1);
                        break;
                    }
                    if (i < 3 || i >= 10) {
                        LogFormat("lanp_gate: connect attempt=%d rc=0x%x", i + 1, rc);
                    }
                    svcSleepThread(i < 10 ? 100'000'000LL : 5'000'000'000LL);
                }

                /* --- Phase 2: Deliver pending events --- */
                bool session_ok = true;
                while (session_ok) {
                    svcSleepThread(100'000'000LL); /* poll every 100 ms */

                    PendingSlot slot;
                    {
                        std::scoped_lock lk(g_pending_mutex);
                        slot = g_pending_slot;
                    }

                    if (!slot.valid) continue;

                    Result rc = serviceDispatchIn(&srv, GateCmdNotify, slot.ev);
                    if (R_FAILED(rc)) {
                        LogFormat("lanp_gate: dispatch failed rc=0x%x, reconnecting", rc);
                        serviceClose(&srv);
                        session_ok = false; /* back to Phase 1 */
                    } else {
                        {
                            std::scoped_lock lk(g_pending_mutex);
                            if (g_pending_slot.valid && g_pending_slot.generation == slot.generation) {
                                g_pending_slot.valid = false;
                            }
                        }
                        LogFormat("lanp_gate: delivered event=%s pid=%" PRIu64 " intent=%" PRIu64,
                                  GateEventName(static_cast<LanPlayGateEvent>(slot.ev.event_type)),
                                  slot.ev.process_id, slot.ev.local_communication_id);
                    }
                }
            }
        }
    }

    void InitGateClient() {
        Result rc = os::CreateThread(&g_worker_thread,
                                     GateWorkerThreadFunc,
                                     nullptr,
                                     g_worker_stack,
                                     GateWorkerStackSize,
                                     21);
        if (R_SUCCEEDED(rc)) {
            os::SetThreadNamePointer(&g_worker_thread, "ldn_mitm::GateWorker");
            os::StartThread(&g_worker_thread);
        } else {
            LogFormat("lanp_gate: CreateThread failed rc=0x%x", rc);
        }
    }

    void FinalizeGateClient() {
        /* Worker thread owns the Service handle; nothing to do here.
         * The sysmodule process is immortal so the thread keeps running. */
    }

    Result NotifyLanPlayGate(LanPlayGateEvent event_type,
                             u64 process_id,
                             u64 title_id,
                             u64 local_communication_id,
                             u16 scene_id) {
        GateEvent ev = {};
        ev.magic                  = GateMagic;
        ev.version                = GateVersion;
        ev.event_type             = static_cast<u32>(event_type);
        ev.process_id             = process_id;
        ev.title_id               = title_id;
        ev.local_communication_id = local_communication_id;
        ev.scene_id               = scene_id;

        /* Store latest event for async delivery by worker thread.
         * If a previous event hasn't been sent yet it is overwritten — the
         * most-recent state is what the gate server needs. */
        std::scoped_lock lk(g_pending_mutex);
        g_pending_slot.generation++;
        g_pending_slot.valid = true;
        g_pending_slot.ev    = ev;
        return 0;
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
