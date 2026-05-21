#include "ldn_icommunication.hpp"
#include <arpa/inet.h>

namespace ams::mitm::ldn {
    static_assert(sizeof(NetworkInfo) == 0x480, "sizeof(NetworkInfo) should be 0x480");
    static_assert(sizeof(ConnectNetworkData) == 0x7C, "sizeof(ConnectNetworkData) should be 0x7C");
    static_assert(sizeof(ScanFilter) == 0x60, "sizeof(ScanFilter) should be 0x60");

    namespace {
        const char *comm_state_name(CommState state) {
            switch (state) {
                case CommState::None:               return "none";
                case CommState::Initialized:        return "initialized";
                case CommState::AccessPoint:        return "access-point";
                case CommState::AccessPointCreated: return "access-point-created";
                case CommState::Station:            return "station";
                case CommState::StationConnected:   return "station-connected";
                case CommState::Error:              return "error";
                default:                            return "unknown";
            }
        }

        LanPlayGateEvent gate_event_for_state_transition(CommState previous_state, CommState new_state) {
            switch (new_state) {
                case CommState::AccessPoint:
                case CommState::Station:
                    return LanPlayGateEvent::Prepare;
                case CommState::AccessPointCreated:
                    return LanPlayGateEvent::Host;
                case CommState::StationConnected:
                    return LanPlayGateEvent::Connect;
                case CommState::Initialized:
                    switch (previous_state) {
                        case CommState::AccessPoint:
                        case CommState::AccessPointCreated:
                        case CommState::Station:
                        case CommState::StationConnected:
                        case CommState::Error:
                            return LanPlayGateEvent::Idle;
                        default:
                            return LanPlayGateEvent::None;
                    }
                case CommState::Error:
                    return LanPlayGateEvent::Idle;
                case CommState::None:
                    return LanPlayGateEvent::Finalize;
                default:
                    return LanPlayGateEvent::None;
            }
        }

        Result notify_gate_verbose(const char *origin,
                                   LanPlayGateEvent event_type,
                                   u64 process_id,
                                   u64 title_id,
                                   u64 local_communication_id,
                                   u16 scene_id) {
            LogFormat("[GATEDBG] %s: notify begin event=%u pid=%" PRIu64 " tid=%" PRIX64 " intent=%" PRIu64 " scene=%u",
                      origin,
                      static_cast<u32>(event_type),
                      process_id,
                      title_id,
                      local_communication_id,
                      scene_id);

            Result rc = NotifyLanPlayGate(event_type,
                                          process_id,
                                          title_id,
                                          local_communication_id,
                                          scene_id);

            LogFormat("[GATEDBG] %s: notify end event=%u rc=0x%x",
                      origin,
                      static_cast<u32>(event_type),
                      rc);
            return rc;
        }

    }

    // https://reswitched.github.io/SwIPC/ifaces.html#nn::ldn::detail::IUserLocalCommunicationService

    Result ICommunicationService::Initialize(const sf::ClientProcessId &client_process_id) {
        this->client_process_id = client_process_id.GetValue().value;
        this->client_title_id = 0;
        LogFormat("[GATEDBG] ICommunicationService::Initialize enter pid=%" PRIu64 " tid=%" PRIX64,
                  this->client_process_id,
                  this->client_title_id);

        if (this->state_event == nullptr) {
            // ClearMode, inter_process
            LogFormat("[GATEDBG] Initialize: creating state_event");
            this->state_event = new os::SystemEvent(::ams::os::EventClearMode_AutoClear, true);
        } else {
            LogFormat("[GATEDBG] Initialize: reusing existing state_event");
        }

        LogFormat("[GATEDBG] Initialize: calling lanDiscovery.initialize");
        Result init_rc = lanDiscovery.initialize([&](){
                                                    this->onEventFired();
                                                },
                                                [&](CommState previous_state, CommState new_state){
                                                    this->onLanStateChanged(previous_state, new_state);
                                                });
        if (R_FAILED(init_rc)) {
            LogFormat("[GATEDBG] Initialize: lanDiscovery.initialize FAILED rc=0x%x", init_rc);
            return init_rc;
        }

        LogFormat("[GATEDBG] Initialize: lanDiscovery.initialize OK");
        Result gate_prepare_rc = notify_gate_verbose("Initialize",
                                 LanPlayGateEvent::Prepare,
                                 this->client_process_id,
                                 this->client_title_id,
                                 0,
                                 0);
        AMS_UNUSED(gate_prepare_rc);
        LogFormat("[GATEDBG] ICommunicationService::Initialize exit success");

        return ResultSuccess();
    }

    Result ICommunicationService::InitializeSystem2(u64 unk, const sf::ClientProcessId &client_process_id) {
        LogFormat("[GATEDBG] ICommunicationService::InitializeSystem2 unk=%" PRIu64, unk);
        this->error_state = unk;
        Result rc = this->Initialize(client_process_id);
        // Initialize already notifies the gate, so no need to do it again
        LogFormat("[GATEDBG] ICommunicationService::InitializeSystem2 rc=0x%x", rc);
        return rc;
    }

    Result ICommunicationService::Finalize() {
        Result rc = lanDiscovery.finalize();
        LogFormat("[GATEDBG] Finalize: lanDiscovery.finalize rc=0x%x", rc);
        if (this->state_event) {
            delete this->state_event;
            this->state_event = nullptr;
            LogFormat("[GATEDBG] Finalize: state_event released");
        }
        return rc;
    }

    Result ICommunicationService::OpenAccessPoint() {
        LogFormat("[GATEDBG] OpenAccessPoint: enter");
        Result rc = this->lanDiscovery.openAccessPoint();
        LogFormat("[GATEDBG] OpenAccessPoint: lanDiscovery rc=0x%x", rc);
        return rc;
    }

    Result ICommunicationService::CloseAccessPoint() {
        return this->lanDiscovery.closeAccessPoint();
    }

    Result ICommunicationService::DestroyNetwork() {
        return this->lanDiscovery.destroyNetwork();
    }

    Result ICommunicationService::OpenStation() {
        LogFormat("[GATEDBG] OpenStation: enter");
        Result rc = this->lanDiscovery.openStation();
        LogFormat("[GATEDBG] OpenStation: lanDiscovery rc=0x%x", rc);
        return rc;
    }

    Result ICommunicationService::CloseStation() {
        return this->lanDiscovery.closeStation();
    }

    Result ICommunicationService::Disconnect() {
        return this->lanDiscovery.disconnect();
    }

    Result ICommunicationService::CreateNetwork(CreateNetworkConfig data) {
        this->cacheGateIntent(data.networkConfig.intentId);
        return this->lanDiscovery.createNetwork(&data.securityConfig, &data.userConfig, &data.networkConfig);;
    }

    Result ICommunicationService::SetAdvertiseData(sf::InAutoSelectBuffer data) {
        return lanDiscovery.setAdvertiseData(data.GetPointer(), data.GetSize());
    }

    Result ICommunicationService::GetState(sf::Out<u32> state) {
        state.SetValue(static_cast<u32>(this->lanDiscovery.getState()));

        if (this->error_state) {
            if (this->lanDiscovery.disconnect_reason != DisconnectReason::None) {
                return MAKERESULT(0x10, static_cast<u32>(this->lanDiscovery.disconnect_reason));
            }
        }

        return 0;
    }

    Result ICommunicationService::GetIpv4Address(sf::Out<u32> address, sf::Out<u32> netmask) {
        u32 gateway, primary_dns, secondary_dns;
        Result rc = nifmGetCurrentIpConfigInfo(address.GetPointer(), netmask.GetPointer(), &gateway, &primary_dns, &secondary_dns);

        address.SetValue(ntohl(address.GetValue()));
        netmask.SetValue(ntohl(netmask.GetValue()));

        LogFormat("get_ipv4_address %x %x", address.GetValue(), netmask.GetValue());

        return rc;
    }

    Result ICommunicationService::GetNetworkInfo(sf::Out<NetworkInfo> buffer) {
        LogFormat("get_network_info %p state: %d", buffer.GetPointer(), static_cast<u32>(this->lanDiscovery.getState()));

        return lanDiscovery.getNetworkInfo(buffer.GetPointer());
    }

    Result ICommunicationService::GetDisconnectReason(sf::Out<u32> reason) {
        auto dr = static_cast<u32>(this->lanDiscovery.disconnect_reason);
        LogFormat("GetDisconnectReason %p state: %d reason: %u", reason.GetPointer(), static_cast<u32>(this->lanDiscovery.getState()), dr);
        reason.SetValue(dr);

        return 0;
    }

    Result ICommunicationService::GetNetworkInfoLatestUpdate(sf::Out<NetworkInfo> buffer, sf::OutArray<NodeLatestUpdate> pUpdates) {
        LogFormat("get_network_info_latest buffer %p", buffer.GetPointer());
        LogFormat("get_network_info_latest pUpdates %p %" PRIu64, pUpdates.GetPointer(), pUpdates.GetSize());

        return lanDiscovery.getNetworkInfo(buffer.GetPointer(), pUpdates.GetPointer(), pUpdates.GetSize());
    }

    Result ICommunicationService::GetSecurityParameter(sf::Out<SecurityParameter> out) {
        Result rc = 0;

        SecurityParameter data;
        NetworkInfo info;
        rc = lanDiscovery.getNetworkInfo(&info);
        if (R_SUCCEEDED(rc)) {
            NetworkInfo2SecurityParameter(&info, &data);
            out.SetValue(data);
        }

        return rc;
    }

    Result ICommunicationService::GetNetworkConfig(sf::Out<NetworkConfig> out) {
        Result rc = 0;

        NetworkConfig data;
        NetworkInfo info;
        rc = lanDiscovery.getNetworkInfo(&info);
        if (R_SUCCEEDED(rc)) {
            NetworkInfo2NetworkConfig(&info, &data);
            out.SetValue(data);
        }

        return rc;
    }

    Result ICommunicationService::AttachStateChangeEvent(sf::Out<sf::CopyHandle> handle) {
        handle.SetValue(this->state_event->GetReadableHandle(), false);
        return ResultSuccess();
    }

    Result ICommunicationService::Scan(sf::Out<u32> outCount, sf::OutAutoSelectArray<NetworkInfo> buffer, u16 channel, ScanFilter filter) {
		AMS_UNUSED(channel);
        Result rc = 0;
        u16 count = buffer.GetSize();

        rc = lanDiscovery.scan(buffer.GetPointer(), &count, filter);
        outCount.SetValue(count);

        if (R_SUCCEEDED(rc)) {
            u64 local_communication_id = 0;
            u16 scene_id = 0;
            if (this->gate_intent_valid) {
                local_communication_id = this->gate_intent_id.localCommunicationId;
                scene_id = this->gate_intent_id.sceneId;
            }
            Result gate_scan_rc = notify_gate_verbose("Scan",
                                                      LanPlayGateEvent::Scan,
                                                      this->client_process_id,
                                                      this->client_title_id,
                                                      local_communication_id,
                                                      scene_id);
            AMS_UNUSED(gate_scan_rc);
        }

        LogFormat("scan %d %d", count, rc);

        return rc;
    }

    Result ICommunicationService::Connect(ConnectNetworkData param, const NetworkInfo &data) {
        LogFormat("ICommunicationService::connect");
        LogHex(&data, sizeof(NetworkInfo));
        LogHex(&param, sizeof(param));

        this->cacheGateIntent(data.networkId.intentId);

        return lanDiscovery.connect(&data, &param.userConfig, param.localCommunicationVersion);
    }

    void ICommunicationService::onEventFired() {
        if (this->state_event) {
            LogFormat("onEventFired signal_event");
            this->state_event->Signal();
        }
    }

    void ICommunicationService::cacheGateIntent(const IntentId &intent_id) {
        this->gate_intent_id = intent_id;
        this->gate_intent_valid = true;
        LogFormat("[GATEDBG] cache intent localCommunicationId=%" PRIu64 " sceneId=%u",
                  intent_id.localCommunicationId,
                  intent_id.sceneId);
    }

    void ICommunicationService::onLanStateChanged(CommState previous_state, CommState new_state) {
        const LanPlayGateEvent event_type = gate_event_for_state_transition(previous_state, new_state);
        LogFormat("[GATEDBG] state change %s -> %s gate_event=%u",
                  comm_state_name(previous_state),
                  comm_state_name(new_state),
                  static_cast<u32>(event_type));

        if (event_type == LanPlayGateEvent::None) {
            return;
        }

        u64 local_communication_id = 0;
        u16 scene_id = 0;

        if (event_type == LanPlayGateEvent::Host || event_type == LanPlayGateEvent::Connect) {
            NetworkInfo info;
            if (R_SUCCEEDED(this->lanDiscovery.getNetworkInfo(&info))) {
                this->cacheGateIntent(info.networkId.intentId);
            }
        }

        if (this->gate_intent_valid) {
            local_communication_id = this->gate_intent_id.localCommunicationId;
            scene_id = this->gate_intent_id.sceneId;
        }

        Result gate_rc = notify_gate_verbose("StateChange",
                                             event_type,
                                             this->client_process_id,
                                             this->client_title_id,
                                             local_communication_id,
                                             scene_id);
        AMS_UNUSED(gate_rc);

        if (event_type == LanPlayGateEvent::Finalize) {
            this->gate_intent_valid = false;
        }
    }

    /*nyi*/
    Result ICommunicationService::SetStationAcceptPolicy(u8 policy) {
		AMS_UNUSED(policy);
        return 0;
    }

    Result ICommunicationService::SetWirelessControllerRestriction() {
        return 0;
    }

    Result ICommunicationService::ScanPrivate() {
        return 0;
    }

    Result ICommunicationService::CreateNetworkPrivate() {
        return 0;
    }

    Result ICommunicationService::Reject() {
        return 0;
    }

    Result ICommunicationService::AddAcceptFilterEntry() {
        return 0;
    }

    Result ICommunicationService::ClearAcceptFilter() {
        return 0;
    }

    Result ICommunicationService::ConnectPrivate() {
        return 0;
    }
}
