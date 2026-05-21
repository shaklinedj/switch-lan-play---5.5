/*
 * ldn_gate_client.cpp — ldn_mitm hosts the "lanp:gt" IPC service.
 *
 * Role inversion vs. original design:
 *   BEFORE: sysmodule was the server; ldn_mitm called smGetService("lanp:gt")
 *           → smGetService blocked indefinitely inside the Stratosphere process.
 *   NOW:    ldn_mitm IS the server; sysmodule polls smGetService("lanp:gt")
 *           from its own plain C context, which works without deadlock.
 *
 * Protocol (cmd IDs must match sysmodule/source/ldn_gate.cpp):
 *   cmd 0  GetState → returns lanp_gate_state_t
 */
#include "ldn_gate_client.hpp"
#include "debug.hpp"

namespace ams::mitm::ldn {

    namespace {
        constexpr const char *GateServiceName = "lanp:gt";
        constexpr u32 GateMagic   = 0x47504E4C; /* 'LNPG' little-endian */
        constexpr u32 GateVersion = 1;
        constexpr u32 GateCmdGetState    = 0;
        constexpr u32 GateCmdGetEvent    = 1;  /* returns readable event handle via CopyHandle */

        /* State struct — must match lanp_gate_state_t in sysmodule/source/ldn_gate.h */
        struct GateState {
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
        };

        GateState      g_gate_state    = {};

        /* Kernel event: ldn_mitm signals the writable end on every state change.
         * The sysmodule waits on the readable end instead of polling. */
        Handle         g_event_w       = INVALID_HANDLE; /* writable (signal) */
        Handle         g_event_r       = INVALID_HANDLE; /* readable (wait)   */

        Handle         g_srv_port      = INVALID_HANDLE;
        bool           g_srv_registered = false;
        volatile bool  g_srv_running   = false;

        constexpr size_t SrvStackSize = 0x8000;  /* 32 KB — 8 KB caused stack overflow */
        alignas(os::ThreadStackAlignment) u8 g_srv_stack[SrvStackSize];
        os::ThreadType g_srv_thread       = {};
        bool           g_srv_thread_valid = false;

        static void MakeResponse(u32 result, const void *out_data, size_t out_size,
                                  Handle copy_handle = INVALID_HANDLE) {
            void *base = armGetTls();
            memset(base, 0, 0x100);
            const u32 payload_size = (u32)(sizeof(CmifOutHeader) + out_size);
            const u32 num_words    = (u32)((16 + payload_size + 3) / 4);
            const bool has_copy    = (copy_handle != INVALID_HANDLE);
            HipcRequest hipc = hipcMakeRequestInline(base,
                .type             = 0,
                .num_data_words   = num_words,
                .num_copy_handles = has_copy ? 1u : 0u
            );
            if (has_copy) {
                hipc.copy_handles[0] = copy_handle;
            }
            CmifOutHeader *hdr = (CmifOutHeader *)cmifGetAlignedDataStart(hipc.data_words, base);
            hdr->magic   = CMIF_OUT_HEADER_MAGIC;
            hdr->version = 0;
            hdr->result  = result;
            hdr->token   = 0;
            if (out_data && out_size > 0) {
                memcpy(hdr + 1, out_data, out_size);
            }
        }

        static void ProcessRequest(bool *close_session) {
            *close_session = false;
            HipcParsedRequest hipc = hipcParseRequest(armGetTls());
            if (hipc.meta.type == CmifCommandType_Close) {
                MakeResponse(0, nullptr, 0);
                *close_session = true;
                return;
            }
            if (hipc.meta.type != CmifCommandType_Request &&
                hipc.meta.type != CmifCommandType_RequestWithContext) {
                MakeResponse(MAKERESULT(Module_Libnx, LibnxError_BadInput), nullptr, 0);
                return;
            }
            CmifInHeader *hdr = (CmifInHeader *)cmifGetAlignedDataStart(hipc.data.data_words, armGetTls());
            if (!hdr || hdr->magic != CMIF_IN_HEADER_MAGIC) {
                MakeResponse(MAKERESULT(Module_Libnx, LibnxError_BadInput), nullptr, 0);
                return;
            }
            switch (hdr->command_id) {
            case GateCmdGetState: {
                GateState st = g_gate_state;
                MakeResponse(0, &st, sizeof(st));
                break;
            }
            case GateCmdGetEvent: {
                /* Return the readable event handle via CopyHandle.
                 * The kernel copies it into the client's handle table. */
                if (g_event_r != INVALID_HANDLE) {
                    MakeResponse(0, nullptr, 0, g_event_r);
                } else {
                    MakeResponse(MAKERESULT(Module_Libnx, LibnxError_NotInitialized), nullptr, 0);
                }
                break;
            }
            default:
                MakeResponse(MAKERESULT(Module_Libnx, LibnxError_BadInput), nullptr, 0);
                break;
            }
        }

        void SrvThreadFn(void *) {
            LogFormat("lanp_gate: [server] thread started, hosting %s", GateServiceName);
            Handle session        = INVALID_HANDLE;
            Handle reply_target   = INVALID_HANDLE;
            bool close_after_reply = false;

            while (g_srv_running) {
                Handle handles[2];
                s32 handle_count = 0;
                handles[handle_count++] = g_srv_port;
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

                if (R_FAILED(rc)) continue;

                if (index == 0) {
                    Handle new_session = INVALID_HANDLE;
                    if (R_SUCCEEDED(svcAcceptSession(&new_session, g_srv_port))) {
                        if (session == INVALID_HANDLE) {
                            session = new_session;
                        } else {
                            svcCloseHandle(new_session); /* single-client */
                        }
                    }
                } else if (index == 1 && session != INVALID_HANDLE) {
                    bool close_session = false;
                    ProcessRequest(&close_session);
                    reply_target      = session;
                    close_after_reply = close_session;
                }
            }

            if (session != INVALID_HANDLE) svcCloseHandle(session);
            LogFormat("lanp_gate: [server] thread exited");
        }

        const char *GateEventName(LanPlayGateEvent ev) {
            switch (ev) {
                case LanPlayGateEvent::Scan:     return "scan";
                case LanPlayGateEvent::Host:     return "host";
                case LanPlayGateEvent::Connect:  return "connect";
                case LanPlayGateEvent::Idle:     return "idle";
                case LanPlayGateEvent::Finalize: return "finalize";
                case LanPlayGateEvent::Prepare:  return "prepare";
                default:                         return "none";
            }
        }
    } /* anonymous namespace */

    void InitGateClient() {
        /* Initialise state */
        g_gate_state        = {};
        g_gate_state.magic   = GateMagic;
        g_gate_state.version = GateVersion;

        /* Create kernel event pair for push notifications */
        Result ev_rc = svcCreateEvent(&g_event_w, &g_event_r);
        if (R_FAILED(ev_rc)) {
            LogFormat("lanp_gate: svcCreateEvent failed rc=0x%x — falling back to poll-only", ev_rc);
            g_event_w = INVALID_HANDLE;
            g_event_r = INVALID_HANDLE;
        } else {
            LogFormat("lanp_gate: event created w=0x%x r=0x%x", g_event_w, g_event_r);
        }

        /* Register "lanp:gt" — ldn_mitm is now the server.
         * smRegisterService is called here, from ams::Main() before
         * os::WaitThread, so the global sm session is not contended. */
        Result rc = smRegisterService(&g_srv_port, smEncodeName(GateServiceName), false, 4);
        if (R_FAILED(rc)) {
            LogFormat("lanp_gate: smRegisterService failed rc=0x%x — gate disabled", rc);
            return;
        }
        LogFormat("lanp_gate: smRegisterService OK, hosting %s port=0x%x", GateServiceName, g_srv_port);
        g_srv_registered = true;

        g_srv_running = true;
        rc = os::CreateThread(&g_srv_thread, SrvThreadFn, nullptr, g_srv_stack, SrvStackSize, 30);
        if (R_FAILED(rc)) {
            LogFormat("lanp_gate: CreateThread failed rc=0x%x", rc);
            g_srv_running = false;
            smUnregisterService(smEncodeName(GateServiceName));
            svcCloseHandle(g_srv_port);
            g_srv_port       = INVALID_HANDLE;
            g_srv_registered = false;
            return;
        }
        os::SetThreadNamePointer(&g_srv_thread, "lanp:gate_srv");
        os::StartThread(&g_srv_thread);
        g_srv_thread_valid = true;
        LogFormat("lanp_gate: server ready");
    }

    void FinalizeGateClient() {
        g_srv_running = false;
        if (g_srv_thread_valid) {
            os::WaitThread(&g_srv_thread);
            os::DestroyThread(&g_srv_thread);
            g_srv_thread_valid = false;
        }
        if (g_srv_registered) {
            smUnregisterService(smEncodeName(GateServiceName));
            svcCloseHandle(g_srv_port);
            g_srv_port       = INVALID_HANDLE;
            g_srv_registered = false;
        }
        if (g_event_w != INVALID_HANDLE) { svcCloseHandle(g_event_w); g_event_w = INVALID_HANDLE; }
        if (g_event_r != INVALID_HANDLE) { svcCloseHandle(g_event_r); g_event_r = INVALID_HANDLE; }
    }

    Result NotifyLanPlayGate(LanPlayGateEvent event_type,
                             u64 process_id,
                             u64 title_id,
                             u64 local_communication_id,
                             u16 scene_id) {
        LogFormat("lanp_gate: NotifyLanPlayGate enter event=%u", static_cast<u32>(event_type));
        bool active = false;
        switch (event_type) {
            case LanPlayGateEvent::Prepare:
            case LanPlayGateEvent::Scan:
            case LanPlayGateEvent::Host:
            case LanPlayGateEvent::Connect:
                active = true;
                break;
            default:
                break;
        }

        LogFormat("lanp_gate: DBG1 before field writes");
        g_gate_state.magic                  = GateMagic;
        g_gate_state.version                = GateVersion;
        g_gate_state.event_type             = static_cast<u32>(event_type);
        g_gate_state.active                 = active ? 1u : 0u;
        g_gate_state.process_id             = process_id;
        g_gate_state.title_id               = title_id;
        g_gate_state.local_communication_id = local_communication_id;
        g_gate_state.scene_id               = scene_id;
        LogFormat("lanp_gate: DBG2 before GetCurrentTick");
        g_gate_state.last_event_tick        = armGetSystemTick();
        LogFormat("lanp_gate: state updated event=%s active=%u pid=%" PRIu64 " intent=%" PRIu64,
                  GateEventName(event_type), active ? 1u : 0u, process_id, local_communication_id);

        /* Signal the event so the sysmodule wakes up immediately */
        if (g_event_w != INVALID_HANDLE) {
            svcSignalEvent(g_event_w);
        }

        return ResultSuccess();
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
