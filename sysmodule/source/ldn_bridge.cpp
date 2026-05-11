/**
 * ldn_bridge.cpp — LDN↔Relay bridge for the unified sysmodule.
 *
 * This module bridges ldn_mitm's LAN traffic (UDP/TCP on port 11452)
 * to the switch-lan-play relay server.  It listens on a companion
 * bridge port (11453) where a patched ldn_mitm mirrors all outgoing
 * packets.  Incoming relay traffic is re-broadcast on port 11452 so
 * that ldn_mitm picks it up as if it came from the local network.
 *
 * IP addresses embedded in NetworkInfo.nodes[].ipv4Address are
 * rewritten: outgoing packets get the virtual 10.13.x.x IP, and
 * incoming packets from the relay are left as-is (they already carry
 * the remote player's virtual IP which ldn_mitm will use to connect
 * through our TCP proxy).
 */
#include "ldn_bridge.h"
#include "packet.h"
#include <poll.h>

/* ------------------------------------------------------------------ */
/*  Offsets into NetworkInfo for IP rewriting (ALG)                     */
/* ------------------------------------------------------------------ */

/*
 * NetworkInfo layout (from ldn_types.hpp):
 *   NetworkId       networkId;       // 32 bytes
 *   CommonNetworkInfo common;        // MacAddress(6) + Ssid(34) + int16(2) + int8(1) + uint8(1) + uint32(4) = 48 bytes
 *   LdnNetworkInfo  ldn;            // starts at offset 80
 *
 * LdnNetworkInfo layout:
 *   uint8_t  unkRandom[16];          // +0
 *   uint16_t securityMode;           // +16
 *   uint8_t  stationAcceptPolicy;    // +18
 *   uint8_t  _unk1[3];              // +19
 *   uint8_t  nodeCountMax;           // +22
 *   uint8_t  nodeCount;              // +23
 *   NodeInfo nodes[8];               // +24
 *
 * NodeInfo layout (64 bytes each):
 *   uint32_t ipv4Address;            // +0 (first field!)
 *   MacAddress macAddress;           // +4
 *   ...
 *
 * So nodes[0].ipv4Address is at: 80 + 24 + 0 = offset 104 in NetworkInfo
 * Each NodeInfo is 64 bytes, and there are 8 of them.
 */
#define NI_LDN_OFFSET      80
#define NI_NODES_OFFSET     (NI_LDN_OFFSET + 24)
#define NI_NODE_SIZE        64
#define NI_NODE_COUNT       8
#define NI_NODECOUNT_OFFSET (NI_LDN_OFFSET + 23) /* nodeCount field */

/* Size of the full NetworkInfo struct */
#define NETWORK_INFO_SIZE   0x480  /* ~1152 bytes, matches ldn_mitm */

/* LDN packet header size */
#define LDN_HEADER_SIZE     12

/* ------------------------------------------------------------------ */
/*  Static state                                                        */
/* ------------------------------------------------------------------ */

/* UDP socket for capturing from ldn_mitm (bound to bridge port 11453) */
static int g_bridge_udp_fd = -1;

/* UDP socket for injecting relay traffic back to ldn_mitm (port 11452) */
static int g_bridge_inject_fd = -1;

/* TCP listening socket for relay proxy (port 11453) */
static int g_bridge_tcp_fd = -1;

/* Track our WiFi IP so we can detect our own packets for rewriting */
static uint32_t g_wifi_ip = 0; /* host byte order */

/* Optional anti-ghosting lobby cache. Relays and WiFi jitter can make ScanResp
 * packets arrive between game scan windows; caching recent ScanResp and
 * re-injecting them briefly improves discoverability without changing the
 * legacy relay protocol. Disable by creating:
 *   sdmc:/config/lan-play/disable_lobby_cache
 */
#define LOBBY_CACHE_SIZE 8
#define LOBBY_REINJECT_INTERVAL_NS 3000000000ULL
#define LOBBY_EXPIRY_NS            30000000000ULL

struct cached_lobby {
    bool valid;
    uint8_t data[2048];
    int data_len;
    uint64_t timestamp;
};

static cached_lobby g_lobby_cache[LOBBY_CACHE_SIZE];
static Mutex g_lobby_cache_mutex;
static bool g_lobby_cache_ready = false;
static uint64_t g_last_lobby_reinject = 0;

static bool lobby_cache_enabled(void)
{
    struct stat st;
    return stat("sdmc:/config/lan-play/disable_lobby_cache", &st) != 0;
}

static void lobby_cache_init(void)
{
    mutexInit(&g_lobby_cache_mutex);
    memset(g_lobby_cache, 0, sizeof(g_lobby_cache));
    g_lobby_cache_ready = true;
    g_last_lobby_reinject = 0;
}

static const char *ldn_type_name(uint8_t type) {
    switch (type) {
        case LDN_TYPE_SCAN: return "Scan";
        case LDN_TYPE_SCAN_RESP: return "ScanResp";
        case LDN_TYPE_CONNECT: return "Connect";
        case LDN_TYPE_SYNC_NETWORK: return "SyncNetwork";
        default: return "Unknown";
    }
}


static void lobby_cache_store_if_scanresp(const void *payload, int payload_len)
{
    if (!g_lobby_cache_ready || !lobby_cache_enabled()) return;
    if (payload_len < LDN_HEADER_SIZE) return;

    const struct ldn_packet_header *hdr = (const struct ldn_packet_header *)payload;
    if (hdr->magic != LDN_MAGIC || hdr->type != LDN_TYPE_SCAN_RESP) return;
    if (payload_len > (int)sizeof(g_lobby_cache[0].data)) return;

    mutexLock(&g_lobby_cache_mutex);

    int slot = -1;
    int oldest_slot = 0;
    uint64_t oldest_tick = 0;

    for (int i = 0; i < LOBBY_CACHE_SIZE; i++) {
        if (!g_lobby_cache[i].valid) {
            if (slot < 0) slot = i;
            continue;
        }

        /* NetworkInfo starts after the 12-byte LDN header. The first 32 bytes
         * are NetworkId; use them as a stable lobby identity when available. */
        if (payload_len >= LDN_HEADER_SIZE + 32 &&
            g_lobby_cache[i].data_len >= LDN_HEADER_SIZE + 32 &&
            memcmp(g_lobby_cache[i].data + LDN_HEADER_SIZE,
                   (const uint8_t *)payload + LDN_HEADER_SIZE, 32) == 0) {
            slot = i;
            break;
        }

        if (oldest_tick == 0 || g_lobby_cache[i].timestamp < oldest_tick) {
            oldest_tick = g_lobby_cache[i].timestamp;
            oldest_slot = i;
        }
    }

    if (slot < 0) slot = oldest_slot;

    memcpy(g_lobby_cache[slot].data, payload, payload_len);
    g_lobby_cache[slot].data_len = payload_len;
    g_lobby_cache[slot].timestamp = armGetSystemTick();
    g_lobby_cache[slot].valid = true;

    mutexUnlock(&g_lobby_cache_mutex);
}

static void lobby_cache_reinject_if_due(void)
{
    if (!g_lobby_cache_ready || !lobby_cache_enabled()) return;
    if (g_bridge_inject_fd < 0) return;

    uint64_t now = armGetSystemTick();
    if (g_last_lobby_reinject != 0 &&
        armTicksToNs(now - g_last_lobby_reinject) < LOBBY_REINJECT_INTERVAL_NS) {
        return;
    }
    g_last_lobby_reinject = now;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port        = htons(LDN_GAME_PORT);

    mutexLock(&g_lobby_cache_mutex);
    for (int i = 0; i < LOBBY_CACHE_SIZE; i++) {
        if (!g_lobby_cache[i].valid) continue;

        if (armTicksToNs(now - g_lobby_cache[i].timestamp) > LOBBY_EXPIRY_NS) {
            g_lobby_cache[i].valid = false;
            continue;
        }

        sendto(g_bridge_inject_fd, g_lobby_cache[i].data,
               (size_t)g_lobby_cache[i].data_len, 0,
               (struct sockaddr *)&dst, sizeof(dst));
    }
    mutexUnlock(&g_lobby_cache_mutex);
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static void refresh_wifi_ip(void) {
    u32 ip = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip))) {
        g_wifi_ip = ntohl(ip);
    }
}

/**
 * Build a synthetic IP+UDP packet wrapping the given LDN payload.
 * src_ip/dst_ip are in network byte order in the output.
 */
static int build_ip_udp(uint8_t *out, int max_out,
                        uint32_t src_ip_ho, uint32_t dst_ip_ho,
                        uint16_t sport, uint16_t dport,
                        const void *payload, int payload_len)
{
    int total = 20 + 8 + payload_len;
    if (total > max_out) return -1;

    /* IP header */
    out[0] = 0x45;  /* version=4, ihl=5 */
    out[1] = 0x00;
    out[2] = (total >> 8) & 0xFF;
    out[3] = total & 0xFF;
    out[4] = out[5] = 0; /* identification */
    out[6] = 0x40; /* DF */
    out[7] = 0;
    out[8] = 64;   /* TTL */
    out[9] = 17;   /* UDP */
    out[10] = out[11] = 0; /* checksum (relay doesn't validate) */
    /* src IP */
    uint32_t sip = htonl(src_ip_ho);
    uint32_t dip = htonl(dst_ip_ho);
    memcpy(out + 12, &sip, 4);
    memcpy(out + 16, &dip, 4);

    /* UDP header */
    out[20] = (sport >> 8) & 0xFF;
    out[21] = sport & 0xFF;
    out[22] = (dport >> 8) & 0xFF;
    out[23] = dport & 0xFF;
    uint16_t udp_len = 8 + payload_len;
    out[24] = (udp_len >> 8) & 0xFF;
    out[25] = udp_len & 0xFF;
    out[26] = out[27] = 0; /* checksum optional */

    memcpy(out + 28, payload, payload_len);
    return total;
}

/* ------------------------------------------------------------------ */
/*  IP rewriting (ALG)                                                  */
/* ------------------------------------------------------------------ */

void ldn_bridge_rewrite_ips(struct lan_play *lp, void *ldn_data, int len,
                            bool outgoing)
{
    /* We need at least an LDN header to check type */
    if (len < LDN_HEADER_SIZE) return;

    struct ldn_packet_header *hdr = (struct ldn_packet_header *)ldn_data;
    if (hdr->magic != LDN_MAGIC) return;

    /* We only rewrite ScanResp, Connect, and SyncNetwork which carry
     * NetworkInfo (containing node IP addresses) */
    if (hdr->type != LDN_TYPE_SCAN_RESP &&
        hdr->type != LDN_TYPE_CONNECT   &&
        hdr->type != LDN_TYPE_SYNC_NETWORK)
        return;

    /* The body starts after the header.  For ScanResp and SyncNetwork,
     * the body IS a NetworkInfo struct.  For Connect, it's a NodeInfo. */
    uint8_t *body = (uint8_t *)ldn_data + LDN_HEADER_SIZE;
    int body_len = len - LDN_HEADER_SIZE;

    /* Note: body might be compressed.  If compressed, we can't easily
     * rewrite.  Skip rewriting for compressed packets for now. */
    if (hdr->compressed) return;

    if (hdr->type == LDN_TYPE_SCAN_RESP || hdr->type == LDN_TYPE_SYNC_NETWORK) {
        /* Body is NetworkInfo — rewrite all nodes */
        if (body_len < NI_NODES_OFFSET + NI_NODE_SIZE) return;

        /* Get node count */
        uint8_t node_count = (body_len > NI_NODECOUNT_OFFSET)
                                ? body[NI_NODECOUNT_OFFSET] : 0;
        if (node_count > NI_NODE_COUNT) node_count = NI_NODE_COUNT;

        for (int i = 0; i < node_count; i++) {
            int off = NI_NODES_OFFSET + (i * NI_NODE_SIZE);
            if (off + 4 > body_len) break;

            uint32_t *ip_ptr = (uint32_t *)(body + off);
            uint32_t ip_ho = *ip_ptr; /* host byte order in struct */

            if (outgoing) {
                /* Outgoing: replace real WiFi IP → virtual 10.13.x.x */
                if (ip_ho == g_wifi_ip && g_wifi_ip != 0) {
                    uint32_t virtual_ip;
                    memcpy(&virtual_ip, lp->my_ip, 4);
                    virtual_ip = ntohl(virtual_ip);
                    *ip_ptr = virtual_ip;
                    static bool logged = false;
                    if (!logged) {
                        LLOG(LLOG_INFO, "ldn_bridge: ALG rewrite WiFi IP %08x → virtual %08x",
                             g_wifi_ip, virtual_ip);
                        logged = true;
                    }
                }
            }
            /* Incoming: IPs from relay already have virtual 10.13.x.x
             * addresses, which is what we want. ldn_mitm will use them
             * for TCP connect (routed through our proxy). */
        }
    } else if (hdr->type == LDN_TYPE_CONNECT) {
        /* Body is NodeInfo — rewrite single node's IP */
        if (body_len < 4) return;
        uint32_t *ip_ptr = (uint32_t *)body;
        if (outgoing && *ip_ptr == g_wifi_ip && g_wifi_ip != 0) {
            uint32_t virtual_ip;
            memcpy(&virtual_ip, lp->my_ip, 4);
            *ip_ptr = ntohl(virtual_ip);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Init / close                                                        */
/* ------------------------------------------------------------------ */

int ldn_bridge_init(struct lan_play *lp)
{
    (void)lp;
    lobby_cache_init();
    refresh_wifi_ip();

    /* --- UDP capture socket (bridge port 11453) --- */
    g_bridge_udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_bridge_udp_fd < 0) {
        LLOG(LLOG_ERROR, "ldn_bridge: UDP socket failed: %s", strerror(errno));
        return -1;
    }

    int on = 1;
    setsockopt(g_bridge_udp_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 only */
    bind_addr.sin_port        = htons(LDN_BRIDGE_PORT);

    if (bind(g_bridge_udp_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LLOG(LLOG_ERROR, "ldn_bridge: bind UDP %d failed: %s", LDN_BRIDGE_PORT, strerror(errno));
        close(g_bridge_udp_fd);
        g_bridge_udp_fd = -1;
        return -1;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(g_bridge_udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    LLOG(LLOG_INFO, "ldn_bridge: UDP capture fd=%d on port %d", g_bridge_udp_fd, LDN_BRIDGE_PORT);

    /* --- Injection socket for re-broadcasting to ldn_mitm --- */
    g_bridge_inject_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_bridge_inject_fd >= 0) {
        int bcast = 1;
        setsockopt(g_bridge_inject_fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
        LLOG(LLOG_INFO, "ldn_bridge: inject fd=%d", g_bridge_inject_fd);
    }

    /* --- TCP proxy socket (bridge port 11453) --- */
    g_bridge_tcp_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_bridge_tcp_fd >= 0) {
        int reuse = 1;
        setsockopt(g_bridge_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in tcp_addr;
        memset(&tcp_addr, 0, sizeof(tcp_addr));
        tcp_addr.sin_family      = AF_INET;
        tcp_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        tcp_addr.sin_port        = htons(LDN_BRIDGE_PORT);

        if (bind(g_bridge_tcp_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
            LLOG(LLOG_ERROR, "ldn_bridge: bind TCP %d failed: %s", LDN_BRIDGE_PORT, strerror(errno));
            close(g_bridge_tcp_fd);
            g_bridge_tcp_fd = -1;
        } else if (listen(g_bridge_tcp_fd, 4) < 0) {
            LLOG(LLOG_ERROR, "ldn_bridge: listen failed: %s", strerror(errno));
            close(g_bridge_tcp_fd);
            g_bridge_tcp_fd = -1;
        } else {
            LLOG(LLOG_INFO, "ldn_bridge: TCP proxy fd=%d on port %d",
                 g_bridge_tcp_fd, LDN_BRIDGE_PORT);
        }
    }

    return 0;
}

void ldn_bridge_close(struct lan_play *lp)
{
    (void)lp;
    if (g_bridge_udp_fd >= 0) { close(g_bridge_udp_fd);    g_bridge_udp_fd = -1; }
    if (g_bridge_inject_fd >= 0) { close(g_bridge_inject_fd); g_bridge_inject_fd = -1; }
    if (g_bridge_tcp_fd >= 0) { close(g_bridge_tcp_fd);    g_bridge_tcp_fd = -1; }
}

/* ------------------------------------------------------------------ */
/*  Injection: relay → local ldn_mitm (UDP broadcast on :11452)         */
/* ------------------------------------------------------------------ */

int ldn_bridge_inject(struct lan_play *lp, const void *udp_payload,
                      int payload_len, uint16_t src_port)
{
    (void)lp;
    if (g_bridge_inject_fd < 0 || payload_len <= 0) return -1;

    /* Verify it's an LDN packet */
    if (payload_len < LDN_HEADER_SIZE) {
        static int short_count = 0;
        if (++short_count <= 5) {
            LLOG(LLOG_WARNING, "ldn_bridge: inject skip short payload from relay src=%u len=%d",
                 src_port, payload_len);
        }
        return 0;
    }
    const struct ldn_packet_header *hdr = (const struct ldn_packet_header *)udp_payload;
    if (hdr->magic != LDN_MAGIC) {
        static int bad_magic_count = 0;
        if (++bad_magic_count <= 8) {
            LLOG(LLOG_WARNING,
                 "ldn_bridge: inject non-LDN payload from relay src=%u len=%d magic=%08x",
                 src_port, payload_len, hdr->magic);
        }
        return 0;
    }

    lobby_cache_store_if_scanresp(udp_payload, payload_len);

    /* Send via loopback to ldn_mitm's UDP socket on port 11452.
     * Using 127.0.0.1 instead of 255.255.255.255 because on Horizon
     * broadcasts go out on the WiFi interface and are NOT delivered
     * back to local sockets on the same device. */
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 */
    dst.sin_port        = htons(LDN_GAME_PORT);

    ssize_t n = sendto(g_bridge_inject_fd, udp_payload, (size_t)payload_len, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
    if (n < 0) {
        static int err = 0;
        if (++err <= 3) {
            LLOG(LLOG_ERROR, "ldn_bridge: inject sendto(:11452) failed: %s", strerror(errno));
        }
        return -1;
    }

    static int inject_count = 0;
    inject_count++;
    if (inject_count <= 8) {
        LLOG(LLOG_INFO,
             "ldn_bridge: inject OK from relay src=%u type=%s(%u) len=%d count=%d",
             src_port, ldn_type_name(hdr->type), hdr->type, payload_len, inject_count);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  UDP capture thread: ldn_mitm → bridge → relay                       */
/* ------------------------------------------------------------------ */

void ldn_bridge_udp_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;
    uint8_t recv_buf[2048];
    uint8_t ip_pkt[2048 + 28]; /* room for IP+UDP header */

    LLOG(LLOG_INFO, "ldn_bridge: UDP capture thread started (fd=%d)", g_bridge_udp_fd);

    refresh_wifi_ip();

    int poll_timeout = 200;
    uint64_t last_activity_tick = armGetSystemTick();

    while (lp->running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        struct pollfd pfd;
        pfd.fd = g_bridge_udp_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pret = poll(&pfd, 1, poll_timeout);
        if (pret < 0) {
            if (errno == EBADF) {
                LLOG(LLOG_WARNING, "ldn_bridge: UDP poll EBADF — exiting thread");
                break;
            }
            if (errno != EINTR) svcSleepThread(10000000LL);
            continue;
        }

        if (pret == 0) {
            lobby_cache_reinject_if_due();

            uint64_t now = armGetSystemTick();
            uint64_t idle_ms = armTicksToNs(now - last_activity_tick) / 1000000ULL;
            poll_timeout = idle_ms > 2000 ? 500 : 200;
            continue;
        }

        last_activity_tick = armGetSystemTick();
        poll_timeout = 10;

        ssize_t n = recvfrom(g_bridge_udp_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != ETIMEDOUT && errno != EINTR) {
                LLOG(LLOG_ERROR, "ldn_bridge: recvfrom error: %s", strerror(errno));
            }
            continue;
        }

        /* Verify LDN magic */
        if (n < LDN_HEADER_SIZE) continue;
        struct ldn_packet_header *hdr = (struct ldn_packet_header *)recv_buf;
        if (hdr->magic != LDN_MAGIC) continue;

        /* Make sure our WiFi IP is current */
        if (g_wifi_ip == 0) refresh_wifi_ip();

        /* Rewrite IPs in outgoing ScanResp/SyncNetwork/Connect packets:
         * replace real WiFi IP with our virtual 10.13.x.x */
        ldn_bridge_rewrite_ips(lp, recv_buf, (int)n, true);

        /* Build IP+UDP wrapper and send to relay */
        uint32_t my_virtual_ip;
        memcpy(&my_virtual_ip, lp->my_ip, 4);
        my_virtual_ip = ntohl(my_virtual_ip);

        /* Use broadcast in virtual subnet (10.13.255.255) as dest */
        uint32_t dst_ip = 0x0A0DFFFF; /* 10.13.255.255 */

        int pkt_len = build_ip_udp(ip_pkt, sizeof(ip_pkt),
                                   my_virtual_ip, dst_ip,
                                   LDN_GAME_PORT, LDN_GAME_PORT,
                                   recv_buf, (int)n);
        if (pkt_len <= 0) continue;

        /* Send to relay as IPv4 packet */
        lan_client_send_ipv4(lp, ip_pkt + 16, ip_pkt, (uint16_t)pkt_len);

        static int count = 0;
        count++;
        if (count <= 3) {
            LLOG(LLOG_INFO, "ldn_bridge: captured LDN type=%d, %d bytes → relay (%d total)",
                 hdr->type, (int)n, count);
        }

        lobby_cache_reinject_if_due();
    }

    LLOG(LLOG_INFO, "ldn_bridge: UDP capture thread exiting");
}

/* ------------------------------------------------------------------ */
/*  TCP proxy thread: ldn_mitm <→ relay <→ remote ldn_mitm              */
/*                                                                      */
/*  When ldn_mitm connects to 127.0.0.1:11453 (via_relay path), it     */
/*  sends a 4-byte target IP prefix.  We then tunnel the TCP data      */
/*  as relay packets to/from that target.                               */
/*                                                                      */
/*  TCP data is encapsulated as:                                        */
/*    relay TYPE_IPV4 → IP(proto=TCP) + TCP-like header + payload      */
/*  This is a simplified tunnel: we use UDP relay packets that carry    */
/*  the TCP stream data reliably enough for LDN (which is itself a     */
/*  protocol with its own framing).                                     */
/* ------------------------------------------------------------------ */

/* Simple TCP relay: forward data between local ldn_mitm and the relay
 * server.  Each TCP connection gets its own goroutine-like handler. */

static void tcp_proxy_handle_client(struct lan_play *lp, int client_fd)
{
    /* Read 4-byte target IP (big-endian, host byte order value) */
    uint32_t target_ip_be;
    ssize_t r = recv(client_fd, &target_ip_be, 4, MSG_WAITALL);
    if (r != 4) {
        LLOG(LLOG_ERROR, "ldn_bridge: TCP proxy - failed to read target IP");
        close(client_fd);
        return;
    }

    uint32_t target_ip_ho = ntohl(target_ip_be);
    LLOG(LLOG_INFO, "ldn_bridge: TCP proxy client, target=%08x", target_ip_ho);

    /* For now, we encapsulate each TCP chunk as a UDP relay packet.
     * The relay server will forward it to the target virtual IP.
     * The remote sysmodule will extract and deliver to remote ldn_mitm. */

    /* Set receive timeout on client socket */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[2048];
    uint8_t ip_pkt[2048 + 28];

    uint32_t my_virtual_ip;
    memcpy(&my_virtual_ip, lp->my_ip, 4);
    my_virtual_ip = ntohl(my_virtual_ip);

    while (lp->running) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n == 0) break; /* Connection closed */
            if (errno == EAGAIN || errno == ETIMEDOUT || errno == EINTR) continue;
            break;
        }

        /* Rewrite IPs in the LDN data being sent */
        ldn_bridge_rewrite_ips(lp, buf, (int)n, true);

        /* Wrap as IP/UDP and send to relay.
         * Use TCP port 11452 as dst_port to indicate this is TCP-tunneled data */
        int pkt_len = build_ip_udp(ip_pkt, sizeof(ip_pkt),
                                   my_virtual_ip, target_ip_ho,
                                   LDN_GAME_PORT + 1, /* src=11453 as marker for TCP */
                                   LDN_GAME_PORT,
                                   buf, (int)n);
        if (pkt_len > 0) {
            lan_client_send_ipv4(lp, ip_pkt + 16, ip_pkt, (uint16_t)pkt_len);
        }
    }

    LLOG(LLOG_INFO, "ldn_bridge: TCP proxy client disconnected");
    close(client_fd);
}

void ldn_bridge_tcp_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;

    if (g_bridge_tcp_fd < 0) {
        LLOG(LLOG_WARNING, "ldn_bridge: TCP proxy not available (no socket)");
        return;
    }

    LLOG(LLOG_INFO, "ldn_bridge: TCP proxy thread started (fd=%d)", g_bridge_tcp_fd);

    /* Set accept timeout so we can check lp->running */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(g_bridge_tcp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (lp->running) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        int client_fd = accept(g_bridge_tcp_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == ETIMEDOUT || errno == EINTR) continue;
            if (errno == ECONNABORTED) {
                /* On Horizon, accept() may return ECONNABORTED instead of
                 * EAGAIN when the SO_RCVTIMEO expires.  Sleep to avoid
                 * flooding the log when no real client is connecting. */
                svcSleepThread(500000000LL); /* 500 ms */
                continue;
            }
            LLOG(LLOG_ERROR, "ldn_bridge: accept failed: %s", strerror(errno));
            svcSleepThread(1000000000LL);
            continue;
        }

        LLOG(LLOG_INFO, "ldn_bridge: TCP proxy accepted fd=%d", client_fd);

        /* Handle synchronously (ldn_mitm only makes 1 TCP connection at a time) */
        tcp_proxy_handle_client(lp, client_fd);
    }

    LLOG(LLOG_INFO, "ldn_bridge: TCP proxy thread exiting");
}
