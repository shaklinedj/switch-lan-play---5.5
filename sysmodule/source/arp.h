#pragma once

#include "nx_common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct packet_ctx;

void arp_list_init(struct arp_item *list);
bool arp_get_mac_by_ip(struct packet_ctx *arg, void *mac, const void *ip);
bool arp_has_ip(struct packet_ctx *arg, const void *ip);
bool arp_set(struct packet_ctx *arg, const void *mac, const void *ip);
void arp_for_each(struct packet_ctx *self, void *userdata, arp_for_each_cb cb);

#ifdef __cplusplus
}
#endif
