/**
 * lan_client_nx.cpp — relay UDP client for the Switch sysmodule.
 *
 * Replaces src/lan-client.c.  Uses nn::socket (BSD socket API via libnx)
 * instead of libuv.  Multithreading is handled with Horizon Threads
 * (threadCreate / threadStart) rather than a libuv event loop.
 *
 * The SLP relay protocol is identical to the PC client so this sysmodule
 * is 100% compatible with the existing switch-lan-play relay server.
 */
#include "lan_client_nx.h"
#include "packet.h"

/* SLP relay protocol type bytes (mirrors src/lan-client.c) */
#define LAN_CLIENT_TYPE_KEEPALIVE   0x00
#define LAN_CLIENT_TYPE_IPV4        0x01
#define LAN_CLIENT_TYPE_PING        0x02
#define LAN_CLIENT_TYPE_IPV4_FRAG   0x03
#define LAN_CLIENT_TYPE_AUTH_ME     0x04
#define LAN_CLIENT_TYPE_INFO        0x10

/* Fragment header layout (mirrors src/lan-client.c) */
#define LC_FRAG_SRC          0
#define LC_FRAG_DST          4
#define LC_FRAG_ID           8
#define LC_FRAG_PART         10
#define LC_FRAG_TOTAL_PART   11
#define LC_FRAG_LEN          12
#define LC_FRAG_PMTU         14
#define LC_FRAG_HEADER_LEN   16

static uint8_t BROADCAST_MAC[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

/* ---------------------------------------------------------------------- */
/*  Internal helpers                                                        */
/* ---------------------------------------------------------------------- */

/** Send raw bytes to the relay server via the UDP socket. */
static int relay_send_raw(struct lan_play *lp, const void *data, size_t len)
{
    mutexLock(&lp->mutex);
    int fd = lp->relay_fd;
    if (fd < 0) {
        mutexUnlock(&lp->mutex);
        errno = EBADF;
        return -1;
    }

    ssize_t sent = sendto(fd, data, len, 0,
                          (struct sockaddr *)&lp->server_addr,
                          sizeof(lp->server_addr));
    mutexUnlock(&lp->mutex);

    if (sent < 0) {
        /* Rate-limit error logs: only once every 30 failures.
         * Use lp->send_err_count (not a static local) so the counter
         * resets properly when the runtime restarts. */
        lp->send_err_count++;
        if (lp->send_err_count <= 3 || (lp->send_err_count % 30 == 0)) {
            LLOG(LLOG_ERROR, "relay_send_raw: sendto failed (%u): %s",
                 lp->send_err_count, strerror(errno));
        }
        return -1;
    }
    lp->send_err_count = 0; /* reset on success */
    lp->upload_packet++;
    lp->upload_byte += (uint64_t)len;
    return 0;
}

/** Send a type-prefixed relay packet, fragmenting if pmtu is set. */
static int lan_client_send(struct lan_play *lp, uint8_t type,
                           const uint8_t *packet, uint16_t len)
{
    int pmtu = lp->pmtu;
    if (type == LAN_CLIENT_TYPE_IPV4 && pmtu > 0 && len > (uint16_t)pmtu) {
        /* Fragment the packet */
        int total_part = len / pmtu + (len % pmtu ? 1 : 0);
        int id         = lp->frag_id++;

        uint8_t header[LC_FRAG_HEADER_LEN];
        CPY_IPV4(header + LC_FRAG_SRC, packet + IPV4_OFF_SRC);
        CPY_IPV4(header + LC_FRAG_DST, packet + IPV4_OFF_DST);
        WRITE_NET8(header, LC_FRAG_TOTAL_PART, total_part);
        WRITE_NET16(header, LC_FRAG_PMTU, pmtu);

        for (int i = 0, pos = 0; pos < len; i++, pos += pmtu) {
            int part_len = LMIN(pmtu, (int)len - pos);
            uint8_t frag_type = LAN_CLIENT_TYPE_IPV4_FRAG;
            WRITE_NET16(header, LC_FRAG_ID,   id);
            WRITE_NET8(header,  LC_FRAG_PART, i);
            WRITE_NET16(header, LC_FRAG_LEN,  part_len);

            /* Assemble: [type(1)] [header(16)] [data(part_len)] */
            size_t total = 1 + LC_FRAG_HEADER_LEN + part_len;
            uint8_t *buf = (uint8_t *)malloc(total);
            if (!buf) return -1;
            buf[0] = frag_type;
            memcpy(buf + 1,                        header, LC_FRAG_HEADER_LEN);
            memcpy(buf + 1 + LC_FRAG_HEADER_LEN,   packet + pos, part_len);
            int ret = relay_send_raw(lp, buf, total);
            free(buf);
            if (ret) return ret;
        }
        return 0;
    }

    /* Non-fragmented: [type(1)] [data(len)] */
    size_t total = 1 + len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        LLOG(LLOG_ERROR, "lan_client_send: out of memory (%zu bytes)", total);
        return -1;
    }
    buf[0] = type;
    if (len > 0) memcpy(buf + 1, packet, len);
    int ret = relay_send_raw(lp, buf, total);
    free(buf);
    return ret;
}

int lan_client_send_ipv4(struct lan_play *lp, void *dst_ip,
                         const void *packet, uint16_t len)
{
    (void)dst_ip;
    return lan_client_send(lp, LAN_CLIENT_TYPE_IPV4, (const uint8_t *)packet, len);
}

/* ---------------------------------------------------------------------- */
/*  Broadcast / unicast delivery                                            */
/* ---------------------------------------------------------------------- */

typedef struct {
    struct lan_play *lp;
    const uint8_t   *packet;
    uint16_t         len;
} for_each_ctx_t;

static int lan_client_arp_cb(void *p, const struct arp_item *item)
{
    for_each_ctx_t *ctx = (for_each_ctx_t *)p;
    struct payload part;
    part.ptr  = ctx->packet;
    part.len  = ctx->len;
    part.next = NULL;
    send_ether(&ctx->lp->packet_ctx, item->mac, ETHER_TYPE_IPV4, &part);
    return 0;
}

static int lan_client_on_broadcast(struct lan_play *lp,
                                   const uint8_t *packet, uint16_t len)
{
    if (lp->next_real_broadcast) {
        lp->next_real_broadcast = false;
        struct payload part;
        part.ptr  = packet;
        part.len  = len;
        part.next = NULL;
        return send_ether(&lp->packet_ctx, BROADCAST_MAC, ETHER_TYPE_IPV4, &part);
    }

    for_each_ctx_t ctx = { lp, packet, len };
    arp_for_each(&lp->packet_ctx, &ctx, lan_client_arp_cb);
    return 0;
}

static int lan_client_process(struct lan_play *lp,
                              const uint8_t *packet, uint16_t len)
{
    if (len == 0) return 0;

    uint8_t dst_mac[6];
    const uint8_t *dst = packet + IPV4_OFF_DST;
    struct payload part;

    if (IS_BROADCAST(dst, lp->packet_ctx.subnet_net, lp->packet_ctx.subnet_mask)) {
        return lan_client_on_broadcast(lp, packet, len);
    } else if (!arp_get_mac_by_ip(&lp->packet_ctx, dst_mac, dst)) {
        return 0;
    }

    part.ptr  = packet;
    part.len  = len;
    part.next = NULL;
    return send_ether(&lp->packet_ctx, dst_mac, ETHER_TYPE_IPV4, &part);
}

static int lan_client_process_frag(struct lan_play *lp,
                                   const uint8_t *packet, uint16_t len)
{
    if (len < LC_FRAG_HEADER_LEN) {
        LLOG(LLOG_WARNING, "relay: invalid fragment length %u < header", len);
        return 0;
    }

    struct lan_client_fragment *frags = lp->frags;

    uint8_t  src[4], dst[4];
    CPY_IPV4(src, packet + LC_FRAG_SRC);
    CPY_IPV4(dst, packet + LC_FRAG_DST);
    uint16_t id         = READ_NET16(packet, LC_FRAG_ID);
    uint8_t  part_num   = READ_NET8(packet,  LC_FRAG_PART);
    uint8_t  total_part = READ_NET8(packet,  LC_FRAG_TOTAL_PART);
    uint16_t frag_len   = READ_NET16(packet, LC_FRAG_LEN);
    uint16_t pmtu       = READ_NET16(packet, LC_FRAG_PMTU);

    if (total_part == 0 || total_part > 8) {
        LLOG(LLOG_WARNING, "relay: invalid fragment total_part=%u", total_part);
        return 0;
    }
    if (part_num >= total_part) {
        LLOG(LLOG_WARNING, "relay: invalid fragment part=%u total=%u", part_num, total_part);
        return 0;
    }
    if (pmtu == 0 || frag_len > pmtu) {
        LLOG(LLOG_WARNING, "relay: invalid fragment sizes pmtu=%u len=%u", pmtu, frag_len);
        return 0;
    }
    if ((uint32_t)LC_FRAG_HEADER_LEN + (uint32_t)frag_len > len) {
        LLOG(LLOG_WARNING, "relay: invalid fragment payload recv=%u need=%u", len, (uint32_t)LC_FRAG_HEADER_LEN + (uint32_t)frag_len);
        return 0;
    }
    if ((uint32_t)pmtu * (uint32_t)part_num + (uint32_t)frag_len > sizeof(frags[0].buffer)) {
        LLOG(LLOG_WARNING, "relay: fragment out of bounds part=%u pmtu=%u len=%u", part_num, pmtu, frag_len);
        return 0;
    }

    struct lan_client_fragment *frag = NULL;
    int i;
    for (i = 0; i < LC_FRAG_COUNT; i++) {
        if (frags[i].used && frags[i].id == id && CMP_IPV4(frags[i].src, src)) {
            frag = &frags[i];
            break;
        }
    }
    if (!frag) {
        for (i = 0; i < LC_FRAG_COUNT; i++) {
            if (!frags[i].used) {
                frag = &frags[i];
                frag->used     = 1;
                frag->id       = id;
                frag->local_id = (uint16_t)lp->local_id++;
                CPY_IPV4(frag->src, src);
                frag->part     = 0;
                break;
            }
        }
    }
    if (!frag) {
        LLOG(LLOG_WARNING, "relay: fragment buffer full, dropping");
        return 0;
    }

    if (pmtu > 0 && (size_t)(pmtu * part_num + frag_len) <= ETHER_MTU) {
        frag->part |= (uint8_t)(1u << part_num);
        memcpy(&frag->buffer[pmtu * part_num],
               packet + LC_FRAG_HEADER_LEN, frag_len);
        if (part_num == total_part - 1)
            frag->total_len = (uint16_t)((total_part - 1) * pmtu + frag_len);
        if (((uint8_t)((1u << total_part) - 1u)) == frag->part) {
            frag->used = 0;
            return lan_client_process(lp, frag->buffer, frag->total_len);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/*  Auth handling                                                           */
/* ---------------------------------------------------------------------- */
static void lan_client_process_auth_me(struct lan_play *lp,
                                       const uint8_t *packet, uint16_t len)
{
    if (!lp->username) {
        LLOG(LLOG_WARNING, "relay: server requests auth but no username set");
        return;
    }
    if (len < 1) return;
    uint8_t auth_type = packet[0];
    if (auth_type != 0) {
        LLOG(LLOG_WARNING, "relay: unknown auth type %d", auth_type);
        return;
    }
    const uint8_t *challenge     = packet + 1;
    uint16_t       challenge_len = (uint16_t)(len - 1);
    uint16_t       uname_len     = (uint16_t)strlen(lp->username);
    uint16_t       resp_len      = 20 + uname_len;
    uint8_t       *resp          = (uint8_t *)malloc(resp_len);
    if (!resp) {
        LLOG(LLOG_ERROR, "lan_client_process_auth_me: out of memory");
        return;
    }

    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, lp->key, LP_KEY_LEN);
    SHA1Update(&ctx, challenge, challenge_len);
    SHA1Final(resp, &ctx);
    /* Scrub the SHA1 context — it contains key material */
    memset(&ctx, 0, sizeof(ctx));
    memcpy(resp + 20, lp->username, uname_len);
    lan_client_send(lp, LAN_CLIENT_TYPE_AUTH_ME, resp, resp_len);
    /* Scrub the response buffer before freeing */
    memset(resp, 0, resp_len);
    free(resp);
}

/* ---------------------------------------------------------------------- */
/*  Receive thread                                                          */
/* ---------------------------------------------------------------------- */
void lan_client_recv_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;
    LLOG(LLOG_INFO, "relay: receive thread started");

    while (lp->running) {
        mutexLock(&lp->mutex);
        int fd = lp->relay_fd;
        mutexUnlock(&lp->mutex);

        if (fd < 0) {
            svcSleepThread(1000000000LL); /* 1 second */
            continue;
        }

        ssize_t n = recvfrom(fd, lp->relay_buf, sizeof(lp->relay_buf), 0, NULL, NULL);
        if (n <= 0) {
            if (n < 0) {
                int err = errno;
                if (err == EBADF) {
                    LLOG(LLOG_WARNING, "relay: recvfrom EBADF on fd %d, socket closed, retrying", fd);
                    continue;
                }
                /* EAGAIN/EWOULDBLOCK/ETIMEDOUT → normal timeout, just loop */
                if (err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT || err == EINTR) {
                    continue;
                }
                /* Real error — rate-limit logs using lp field so counter resets
                 * on runtime restart (avoids silent error suppression after crash). */
                lp->recv_err_count++;
                if (lp->recv_err_count <= 3 || (lp->recv_err_count % 30 == 0)) {
                    LLOG(LLOG_ERROR, "relay: recvfrom error (%u): %s (fd=%d)",
                         lp->recv_err_count, strerror(err), fd);
                }
                /* Suspend or broken network (sleep transition) */
                svcSleepThread(500000000LL); /* 500ms delay */
                if (err == ENETDOWN || err == EPIPE || err == ENXIO) {
                    lp->running = false;
                }
            }
            continue;
        }
        lp->recv_err_count = 0; /* reset on successful recv */

        lp->download_packet++;
        lp->download_byte += (uint64_t)n;

        uint8_t type = lp->relay_buf[0] & 0x7f;

        switch (type) {
        case LAN_CLIENT_TYPE_KEEPALIVE:
            break;
        case LAN_CLIENT_TYPE_IPV4:
            {
                static int ipv4_count = 0;
                if (++ipv4_count <= 8) {
                    LLOG(LLOG_INFO, "relay: recv IPV4 packet len=%d count=%d", (int)n, ipv4_count);
                }
            }
            lan_client_process(lp, lp->relay_buf + 1, (uint16_t)(n - 1));
            break;
        case LAN_CLIENT_TYPE_IPV4_FRAG:
            {
                static int frag_count = 0;
                if (++frag_count <= 8) {
                    LLOG(LLOG_INFO, "relay: recv IPV4_FRAG packet len=%d count=%d", (int)n, frag_count);
                }
            }
            lan_client_process_frag(lp, lp->relay_buf + 1, (uint16_t)(n - 1));
            break;
        case LAN_CLIENT_TYPE_AUTH_ME:
            lan_client_process_auth_me(lp, lp->relay_buf + 1, (uint16_t)(n - 1));
            break;
        case LAN_CLIENT_TYPE_INFO:
            LLOG(LLOG_INFO, "[Server]: %.*s", (int)(n - 1), lp->relay_buf + 1);
            break;
        }
    }

    LLOG(LLOG_INFO, "relay: receive thread exiting");
}

/* ---------------------------------------------------------------------- */
/*  Keepalive thread                                                        */
/* ---------------------------------------------------------------------- */
void lan_client_keepalive_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;
    LLOG(LLOG_INFO, "relay: keepalive thread started");

    int consecutive_fails = 0;

    while (lp->running) {
        uint8_t ka = LAN_CLIENT_TYPE_KEEPALIVE;
        int ret = relay_send_raw(lp, &ka, 1);

        if (ret < 0) {
            int err = errno;
            /* EHOSTUNREACH / ENETUNREACH = route temporarily missing (WiFi
             * just reconnected).  The socket is fine; don't destroy it —
             * just wait for the routing table to recover. */
            if (err == EHOSTUNREACH || err == ENETUNREACH) {
                LLOG(LLOG_WARNING, "relay: route not ready (%s), waiting...", strerror(err));
                /* Sleep in 1-second chunks so we can terminate quickly */
                for (int i = 0; i < 10 && lp->running; i++)
                    svcSleepThread(1000000000LL);
                continue;
            } else if (err == ENETDOWN || err == EPIPE || err == ENXIO) {
                /* Network suspended completely (sleep or interface down) */
                LLOG(LLOG_ERROR, "relay: keepalive hard error %d, tearing down", err);
                lp->running = false;
                break;
            }

            consecutive_fails++;
            /* Do not close/recreate relay_fd from this thread. The receive
             * thread may be blocked in recvfrom() on the same descriptor.
             * Closing it here creates an intermittent EBADF/descriptor-reuse
             * race that is especially dangerous while Horizon changes WiFi
             * state. A future runtime manager should restart the whole LAN
             * runtime instead of hot-swapping this shared socket. */
            if (consecutive_fails >= 6) {
                LLOG(LLOG_ERROR,
                     "relay: %d keepalive failures; tearing down LAN runtime for clean self-healing restart",
                     consecutive_fails);
                lp->running = false;
                break;
            }
        } else {
            if (consecutive_fails > 0) {
                LLOG(LLOG_INFO, "relay: keepalive OK after %d failures", consecutive_fails);
            }
            consecutive_fails = 0;
        }

        /* Also toggle next_real_broadcast so every 1 s we do a real broadcast */
        lp->next_real_broadcast = true;

        /* Sleep 15 seconds in 1-second chunks.
         * 15s is safe for all common NAT/firewall UDP mapping timeouts (>= 30s)
         * while being significantly less noisy than 10s. */
        for (int i = 0; i < 15 && lp->running; i++) {
            svcSleepThread(1000000000LL); /* 1 second */
        }
    }
    LLOG(LLOG_INFO, "relay: keepalive thread exiting");
}

/* ---------------------------------------------------------------------- */
/*  Init / close                                                            */
/* ---------------------------------------------------------------------- */
int lan_client_init(struct lan_play *lp)
{
    lp->frag_id    = 0;
    lp->local_id   = 0;
    lp->next_real_broadcast = true;
    memset(lp->frags, 0, sizeof(lp->frags));

    mutexLock(&lp->mutex);
    lp->relay_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int fd = lp->relay_fd;
    mutexUnlock(&lp->mutex);

    if (fd < 0) {
        LLOG(LLOG_ERROR, "relay: socket failed: %s", strerror(errno));
        return -1;
    }

    /* Allow broadcast sends (for servers on LAN) */
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    /* Set receive timeout so thread can check lp->running */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char srv_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &lp->server_addr.sin_addr, srv_ip, sizeof(srv_ip));
    LLOG(LLOG_INFO, "relay: server %s:%d  fd=%d",
         srv_ip, ntohs(lp->server_addr.sin_port), fd);

    lp->upload_packet   = 0;
    lp->upload_byte     = 0;
    lp->download_packet = 0;
    lp->download_byte   = 0;
    return 0;
}

void lan_client_close(struct lan_play *lp)
{
    mutexLock(&lp->mutex);
    int fd = lp->relay_fd;
    lp->relay_fd = -1;
    mutexUnlock(&lp->mutex);

    if (fd >= 0) {
        close(fd);
    }
    /* Wipe fragment reassembly buffers — they may contain packet data */
    memset(lp->frags, 0, sizeof(lp->frags));
}
