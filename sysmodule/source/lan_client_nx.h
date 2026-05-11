#pragma once

#include "nx_common.h"
#include "sha1.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * lan_client_init — create a UDP socket, resolve the relay server address,
 * and prepare the fragment reassembly buffers.
 * Returns 0 on success.
 */
int  lan_client_init(struct lan_play *lp);

/** lan_client_close — close the relay UDP socket. */
void lan_client_close(struct lan_play *lp);

/**
 * lan_client_recv_thread_fn — background thread.
 * Reads UDP datagrams from the relay server, decodes the SLP protocol and
 * calls lan_client_process() (or lan_client_process_frag()) to deliver the
 * inner IPv4 packet to the TAP/game side.
 */
void lan_client_recv_thread_fn(void *arg);

/**
 * lan_client_keepalive_thread_fn — sends a keepalive datagram every 10 s.
 */
void lan_client_keepalive_thread_fn(void *arg);

/** Forward an IPv4 packet from the game to the relay server. */
int  lan_client_send_ipv4(struct lan_play *lp, void *dst_ip,
                          const void *packet, uint16_t len);

#ifdef __cplusplus
}
#endif
