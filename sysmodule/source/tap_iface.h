#pragma once

#include "nx_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * tap_init — open the raw IP socket used to capture and inject LAN-play
 * packets on the Switch.
 *
 * The raw socket listens on INADDR_ANY with IPPROTO_IP so it receives all
 * inbound IP datagrams.  IP_HDRINCL lets us send full IP packets including
 * the header (needed for injection).
 *
 * Requirements: the Switch must have an IP address in the 10.13.0.0/16
 * subnet configured in System Settings → Internet → Manual, OR the relay
 * server must assign that address via DHCP before this sysmodule starts.
 *
 * Returns 0 on success, -1 on error.
 */
int  tap_init(struct lan_play *lp);

/** tap_close — close the raw socket. */
void tap_close(struct lan_play *lp);

/**
 * tap_send_packet — inject a raw Ethernet frame into the Switch network
 * stack so the game receives it as if it arrived from the physical LAN.
 *
 * Because the Switch has no "real" Ethernet on WiFi, we strip the 14-byte
 * Ethernet header and inject the inner IP datagram via the raw socket.
 *
 * Returns 0 on success.
 */
int  tap_send_packet(struct lan_play *lp, const void *eth_frame, int len);

/**
 * tap_recv_thread_fn — background thread.
 * Reads inbound IP packets from the raw socket, wraps them in a synthetic
 * Ethernet header using the virtual MAC and then calls get_packet() so the
 * rest of the pipeline (ARP, IPv4, relay forwarding) can process them.
 */
void tap_recv_thread_fn(void *arg);

#ifdef __cplusplus
}
#endif
