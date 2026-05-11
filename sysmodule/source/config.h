#pragma once

#include "nx_common.h"

#define NX_CONFIG_PATH      "sdmc:/config/lan-play/config.ini"
#define NX_CONFIG_DIR       "sdmc:/config/lan-play"
#define NX_CONFIG_RELAY_DEF ""          /* empty = require explicit config */
/* Default IP is empty — auto-generated from device UID if not set. */
#define NX_CONFIG_IP_DEF    ""

#define NX_IP_MAX  16
#define NX_STR_MAX 256

typedef struct {
    char relay_addr[NX_STR_MAX]; /* e.g.  relay.example.com:11451 */
    char my_ip[NX_IP_MAX];       /* e.g.  10.13.4.22  (auto if empty) */
    char subnet_net[NX_IP_MAX];  /* always 10.13.0.0                  */
    char subnet_mask[NX_IP_MAX]; /* always 255.255.0.0                */
    char username[NX_STR_MAX];   /* optional auth username            */
    char password[NX_STR_MAX];   /* optional auth password            */
} nx_config_t;

/**
 * Load configuration from NX_CONFIG_PATH on the SD card.
 * Returns 0 on success, -1 if relay_addr is still empty after loading.
 * Missing optional keys (ip, subnet_*) get sensible defaults.
 */
int nx_config_load(nx_config_t *cfg);

/** Write a minimal config file — only relay_addr is required. */
int nx_config_write_default(void);

/**
 * nx_config_auto_ip — derive a deterministic IP in 10.13.1.1–10.13.254.254
 * from the Switch device serial number so every console gets a unique address
 * without any manual network configuration.
 *
 * The result is written into cfg->my_ip (16-byte dotted-decimal string).
 */
void nx_config_auto_ip(nx_config_t *cfg);

/**
 * nx_config_save_relay — overwrite only the relay_addr line in the config
 * file (used by the homebrew configurator NRO).
 */
int nx_config_save_relay(const char *relay_addr);
