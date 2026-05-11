#pragma once

#include "arp.h"

#ifdef __cplusplus
extern "C" {
#endif

uint16_t payload_total_len(const struct payload *payload);

int packet_init(
    struct packet_ctx *self,
    struct lan_play   *arg,
    void              *buffer,
    size_t             buffer_len,
    void              *ip,
    void              *subnet_net,
    void              *subnet_mask,
    time_t             arp_ttl
);
int  packet_close(struct packet_ctx *self);
void packet_set_mac(struct packet_ctx *arg, const uint8_t *mac);
void get_packet(struct packet_ctx *arg,
                const struct pcap_pkthdr *pkthdr,
                const u_char *packet);

int  process_arp(struct packet_ctx *arg,
                 const struct ether_frame *ether);
int  process_ipv4(struct packet_ctx *arg,
                  const struct ether_frame *ether);

int  send_ether_ex(struct packet_ctx *arg,
                   const void *dst,
                   const void *src,
                   uint16_t type,
                   const struct payload *payload);
int  send_ether(struct packet_ctx *arg,
                const void *dst,
                uint16_t type,
                const struct payload *payload);

void payload_print_hex(const struct payload *payload);

#ifdef __cplusplus
}
#endif
