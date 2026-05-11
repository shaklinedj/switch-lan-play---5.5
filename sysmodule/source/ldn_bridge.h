/**
 * ldn_bridge.h — LDN↔Relay bridge for the unified sysmodule.
 *
 * Captures LDN protocol packets from ldn_mitm (via bridge port 11453)
 * and relays them to the switch-lan-play server.  Also injects relay
 * traffic back to ldn_mitm via UDP broadcast on port 11452.
 */
#pragma once
#include "nx_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LDN_GAME_PORT   11452  /* ldn_mitm's native port */
#define LDN_BRIDGE_PORT 11453  /* our bridge capture port */
#define LDN_MAGIC       0x11451400

/* LDN packet types (matches ldn_mitm's LANPacketType enum) */
#define LDN_TYPE_SCAN          0
#define LDN_TYPE_SCAN_RESP     1
#define LDN_TYPE_CONNECT       2
#define LDN_TYPE_SYNC_NETWORK  3

#pragma pack(push, 1)
struct ldn_packet_header {
    uint32_t magic;
    uint8_t  type;
    uint8_t  compressed;
    uint16_t length;
    uint16_t decompress_length;
    uint8_t  reserved[2];
};
#pragma pack(pop)

/**
 * Initialize the LDN bridge.
 * Creates the UDP listener on bridge port and the injection socket.
 */
int  ldn_bridge_init(struct lan_play *lp);

/** Close all bridge sockets. */
void ldn_bridge_close(struct lan_play *lp);

/**
 * Thread function: captures UDP packets from ldn_mitm on bridge port,
 * wraps them in IP/UDP headers, and sends them to the relay as IPV4.
 */
void ldn_bridge_udp_thread_fn(void *arg);

/**
 * Thread function: TCP proxy that accepts connections from ldn_mitm
 * and tunnels them through the relay to the remote host.
 */
void ldn_bridge_tcp_thread_fn(void *arg);

/**
 * Called by tap_send_packet when a relay packet targets port 11452.
 * Injects the UDP payload as a broadcast on the local LAN so that
 * ldn_mitm receives it.
 * Returns 0 on success, -1 on error.
 */
int  ldn_bridge_inject(struct lan_play *lp, const void *udp_payload,
                       int payload_len, uint16_t src_port);

/**
 * Rewrite IP addresses in LDN ScanResp packets.
 * Replaces the real WiFi IP in NetworkInfo.nodes[].ipv4Address
 * with the virtual 10.13.x.x IP so remote players can route
 * through the relay.
 */
void ldn_bridge_rewrite_ips(struct lan_play *lp, void *ldn_data, int len,
                            bool outgoing);

#ifdef __cplusplus
}
#endif
