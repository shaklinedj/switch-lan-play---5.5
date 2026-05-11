#include "tap_iface.h"
#include "packet.h"
#include "ldn_bridge.h"
#include <fcntl.h>

/**
 * TAP layer for sysmodules — dual-socket approach.
 *
 * CAPTURE (game → relay):
 *   Raw socket (SOCK_RAW/IPPROTO_IP) receives all inbound IP datagrams.
 *   We prepend a synthetic Ethernet header for the pipeline.
 *
 * INJECTION (relay → game):
 *   Raw socket sendto() does NOT work on Horizon for virtual IPs (EINVAL).
 *   Instead we parse the IP/UDP header from the pipeline's Ethernet frame
 *   and forward the UDP payload via a regular SOCK_DGRAM socket.
 *   This covers all LAN games (which use UDP for discovery + gameplay).
 */

#define TAP_BUF_SIZE 2048

/* Broadcast MAC used as destination in synthetic Ethernet headers */
static const uint8_t BCAST_MAC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* Dedicated UDP socket for packet injection (relay → game) */
static int g_inject_fd = -1;

static void build_virtual_mac(uint8_t mac[6])
{
    mac[0] = 0x02;
    SetSysSerialNumber serial;
    if (R_SUCCEEDED(setsysGetSerialNumber(&serial))) {
        uint32_t h = 0x811c9dc5u;
        size_t i = 0;
        for (; i < sizeof(serial.number) && serial.number[i]; i++) {
            h ^= (uint8_t)serial.number[i];
            h *= 0x01000193u;
        }
        mac[1] = (h >> 24) & 0xff;
        mac[2] = (h >> 16) & 0xff;
        mac[3] = (h >>  8) & 0xff;
        mac[4] =  h        & 0xff;
        mac[5] = (uint8_t)(i % 256);
    } else {
        mac[1] = 0x4E; mac[2] = 0x58; mac[3] = 0x4C; mac[4] = 0x50; mac[5] = 0x01;
    }
}

int tap_init(struct lan_play *lp)
{
    build_virtual_mac(lp->my_mac);
    LLOG(LLOG_INFO, "tap: virtual MAC %02x:%02x:%02x:%02x:%02x:%02x",
         lp->my_mac[0], lp->my_mac[1], lp->my_mac[2],
         lp->my_mac[3], lp->my_mac[4], lp->my_mac[5]);

    /* ---- Capture socket (raw) ---- */
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_IP);
    if (fd < 0) {
        LLOG(LLOG_ERROR, "tap: socket(SOCK_RAW) failed: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    int on = 1;
    setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on));

    /* Bind to INADDR_ANY for capture */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    lp->bpf_fd = fd;
    LLOG(LLOG_INFO, "tap: capture raw socket fd=%d opened", fd);

    /* ---- Injection socket (UDP) ---- */
    g_inject_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_inject_fd < 0) {
        LLOG(LLOG_ERROR, "tap: inject socket(SOCK_DGRAM) failed: %s", strerror(errno));
        /* Non-fatal — capture still works, injection won't */
    } else {
        int bcast = 1;
        setsockopt(g_inject_fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

        /* Bind to any port — we'll use sendto with specific dest */
        struct sockaddr_in inj_bind;
        memset(&inj_bind, 0, sizeof(inj_bind));
        inj_bind.sin_family = AF_INET;
        inj_bind.sin_addr.s_addr = INADDR_ANY;
        inj_bind.sin_port = 0;
        bind(g_inject_fd, (struct sockaddr *)&inj_bind, sizeof(inj_bind));

        LLOG(LLOG_INFO, "tap: inject UDP socket fd=%d opened", g_inject_fd);
    }

    return 0;
}

void tap_close(struct lan_play *lp)
{
    if (lp->bpf_fd >= 0) {
        close(lp->bpf_fd);
        lp->bpf_fd = -1;
    }
    if (g_inject_fd >= 0) {
        close(g_inject_fd);
        g_inject_fd = -1;
    }
}

int tap_send_packet(struct lan_play *lp, const void *eth_frame, int len)
{
    (void)lp;

    if (len <= ETHER_HEADER_LEN) return -1;

    /* Check EtherType */
    const uint8_t *frame = (const uint8_t *)eth_frame;
    uint16_t ether_type = (frame[ETHER_OFF_TYPE] << 8) | frame[ETHER_OFF_TYPE + 1];

    if (ether_type != 0x0800) {
        /* ARP or non-IPv4 — can't inject via UDP, just skip */
        return 0;
    }

    /* Strip Ethernet header → IP packet */
    const uint8_t *ip_pkt = frame + ETHER_HEADER_LEN;
    int ip_len = len - ETHER_HEADER_LEN;

    if (ip_len < 20 || (ip_pkt[0] >> 4) != 4) return 0;

    uint8_t ip_hdr_len = (ip_pkt[0] & 0x0F) * 4;
    uint8_t protocol = ip_pkt[9];

    /* Extract dest IP from IP header */
    struct in_addr dst_ip;
    memcpy(&dst_ip, ip_pkt + 16, 4);

    if (protocol == 17 /* UDP */ && ip_len >= ip_hdr_len + 8) {
        /* Parse UDP header */
        const uint8_t *udp_hdr = ip_pkt + ip_hdr_len;
        uint16_t src_port = (udp_hdr[0] << 8) | udp_hdr[1];
        uint16_t dst_port = (udp_hdr[2] << 8) | udp_hdr[3];
        uint16_t udp_len  = (udp_hdr[4] << 8) | udp_hdr[5];

        const uint8_t *udp_payload = udp_hdr + 8;
        int payload_len = udp_len - 8;

        if (payload_len <= 0 || ip_len < ip_hdr_len + 8 + payload_len) {
            return 0; /* Malformed */
        }

        /* === LDN Bridge: packets targeting port 11452 are LDN traffic ===
         * Route them through the bridge injection which broadcasts to
         * ldn_mitm with proper IP rewriting. */
        if (dst_port == LDN_GAME_PORT && payload_len >= 12) {
            static int ldn_in_count = 0;
            const struct ldn_packet_header *ldn_hdr = (const struct ldn_packet_header *)udp_payload;
            if (++ldn_in_count <= 8) {
                char src_ip_str[16] = {0};
                struct in_addr src_ip;
                memcpy(&src_ip, ip_pkt + 12, 4);
                inet_ntop(AF_INET, &src_ip, src_ip_str, sizeof(src_ip_str));
                LLOG(LLOG_INFO,
                     "tap: relay->ldn candidate src=%s:%u dst=%u len=%d magic=%08x type=%u",
                     src_ip_str, src_port, dst_port, payload_len, ldn_hdr->magic, ldn_hdr->type);
            }
            /* Rewrite IPs in incoming relay packets before injection */
            /* (incoming = false: don't modify remote IPs, they're virtual) */
            int ret = ldn_bridge_inject(lp, udp_payload, payload_len, src_port);
            if (ret == 0) return 0; /* Handled by bridge */
            /* Fall through to generic injection if bridge failed */
        }

        if (g_inject_fd < 0) return -1;

        /* Determine where to send:
         * - Broadcast/subnet-broadcast → send to 255.255.255.255
         * - Unicast → send to 127.0.0.1 (local game process) */
        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port = htons(dst_port);

        uint32_t dst_raw = ntohl(dst_ip.s_addr);
        if (dst_raw == 0xFFFFFFFF ||                   /* global broadcast */
            (dst_raw & 0xFFFF0000) == 0x0A0D0000) {    /* 10.13.x.x = our virtual subnet */
            dst.sin_addr.s_addr = htonl(0xFFFFFFFF);   /* broadcast */
        } else {
            dst.sin_addr.s_addr = htonl(0x7F000001);   /* 127.0.0.1 */
        }

        ssize_t n = sendto(g_inject_fd, udp_payload, (size_t)payload_len, 0,
                           (struct sockaddr *)&dst, sizeof(dst));
        if (n < 0) {
            static int err_count = 0;
            err_count++;
            if (err_count <= 5) {
                char ip_str[16];
                inet_ntop(AF_INET, &dst.sin_addr, ip_str, sizeof(ip_str));
                LLOG(LLOG_ERROR, "tap: UDP inject sendto(%s:%d, %d bytes) failed: %s (errno=%d)",
                     ip_str, dst_port, payload_len, strerror(errno), errno);
            } else if (err_count == 6) {
                LLOG(LLOG_WARNING, "tap: inject errors — suppressing further logs");
            }
            return -1;
        }

        static bool first_inject = true;
        if (first_inject) {
            LLOG(LLOG_INFO, "tap: first UDP inject OK (port=%d, %d bytes)", dst_port, payload_len);
            first_inject = false;
        }
        return 0;

    } else if (protocol == 1 /* ICMP */) {
        /* ICMP — can't inject via UDP socket, skip */
        return 0;
    }

    /* Other protocols — skip silently */
    return 0;
}

void tap_recv_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;

    /* Buffer: 14 bytes Ethernet header + up to TAP_BUF_SIZE IP payload */
    uint8_t frame_buf[ETHER_HEADER_LEN + TAP_BUF_SIZE];

    LLOG(LLOG_INFO, "tap: receive thread started (fd=%d)", lp->bpf_fd);

    while (lp->running) {
        struct sockaddr_in src_addr;
        socklen_t addr_len = sizeof(src_addr);

        ssize_t n = recvfrom(lp->bpf_fd, frame_buf + ETHER_HEADER_LEN,
                             TAP_BUF_SIZE, 0,
                             (struct sockaddr *)&src_addr, &addr_len);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != ETIMEDOUT && errno != EINTR) {
                LLOG(LLOG_ERROR, "tap: recvfrom error: %s", strerror(errno));
            }
            continue;
        }

        /* Anti-echo: skip packets that originate from ourselves.
         * SOCK_RAW on Horizon may deliver outgoing broadcasts back to the
         * capture socket, which would create a relay loop.
         * Filter: own WiFi IP (inject socket source) and full loopback range. */
        if (lp->wifi_ip != 0 && src_addr.sin_addr.s_addr == lp->wifi_ip) {
            continue; /* self-sent packet — drop */
        }
        if ((src_addr.sin_addr.s_addr & htonl(0xFF000000u)) == htonl(0x7F000000u)) {
            continue; /* loopback (127.x.x.x) — not a real remote host */
        }

        /* Build synthetic Ethernet header */
        memcpy(frame_buf + ETHER_OFF_DST, BCAST_MAC, 6);
        frame_buf[ETHER_OFF_SRC + 0] = 0x02;
        frame_buf[ETHER_OFF_SRC + 1] = ((uint8_t *)&src_addr.sin_addr)[0];
        frame_buf[ETHER_OFF_SRC + 2] = ((uint8_t *)&src_addr.sin_addr)[1];
        frame_buf[ETHER_OFF_SRC + 3] = ((uint8_t *)&src_addr.sin_addr)[2];
        frame_buf[ETHER_OFF_SRC + 4] = ((uint8_t *)&src_addr.sin_addr)[3];
        frame_buf[ETHER_OFF_SRC + 5] = 0x00;
        frame_buf[ETHER_OFF_TYPE]     = 0x08;
        frame_buf[ETHER_OFF_TYPE + 1] = 0x00;

        uint16_t total_len = (uint16_t)(ETHER_HEADER_LEN + n);

        struct pcap_pkthdr phdr;
        memset(&phdr, 0, sizeof(phdr));
        phdr.caplen = total_len;
        phdr.len    = total_len;

        packet_set_mac(&lp->packet_ctx, lp->my_mac);
        get_packet(&lp->packet_ctx, &phdr, frame_buf);
    }

    LLOG(LLOG_INFO, "tap: receive thread exiting");
}
