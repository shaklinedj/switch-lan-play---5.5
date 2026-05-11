/**
 * nx_common.h — central header for the Switch sysmodule.
 *
 * Replaces src/lan-play.h + src/helper.h + src/config.h for the NX build.
 * All adapted C files include only this header instead of lan-play.h.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Standard C / POSIX */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

/* Switch SDK */
#include <switch.h>

/* BSD socket API (initialised via socketInitializeDefault()) */
#include <sys/socket.h>
#include <netinet/in.h>
/* #include <netinet/ip.h> */
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <netdb.h>

/* -------------------------------------------------------------------------
 * Compatibility types missing from Horizon/libnx
 * ---------------------------------------------------------------------- */
typedef uint8_t u_char;

/* Minimal pcap_pkthdr so the adapted C files keep the same function
 * signatures as the originals without pulling in libpcap. */
struct pcap_pkthdr {
    struct timeval ts;
    uint32_t caplen;
    uint32_t len;
};

/* -------------------------------------------------------------------------
 * Configuration constants  (mirrors src/config.h)
 * ---------------------------------------------------------------------- */
#define SERVER_IP            "10.13.37.1"
#define SUBNET_NET           "10.13.0.0"
#define SUBNET_MASK          "255.255.0.0"
#define BUFFER_SIZE          2048
#define IP_STR_LEN           16
#define SERVER_PORT          11451
#define ETHER_MTU            1500
#define PCAP_ERRBUF_SIZE     256
#define MIN_FRAG_PAYLOAD_LEN 500
#define LC_FRAG_COUNT        100
#define LP_KEY_LEN           20

/* -------------------------------------------------------------------------
 * Logging  (mirrors base/include/base/llog.h)
 * ---------------------------------------------------------------------- */
#define LLOG_ERROR   1
#define LLOG_WARNING 2
#define LLOG_NOTICE  3
#define LLOG_INFO    4
#define LLOG_DEBUG   5

void nx_log(int level, const char *fmt, ...);
#define LLOG(level, ...) nx_log(level, __VA_ARGS__)
#define eprintf(...)     fprintf(stderr, __VA_ARGS__)

/* -------------------------------------------------------------------------
 * Helper macros  (mirrors src/helper.h)
 * ---------------------------------------------------------------------- */
#define LMIN(a,b)     ((a)<(b)?(a):(b))
#define LABS(x)       ((x)<0?(-(x)):(x))

#define READ_NET8(p,o)     (*(uint8_t*)((uint8_t*)(p)+(o)))
#define READ_NET16(p,o)    htons(*(uint16_t*)((uint8_t*)(p)+(o)))
#define READ_NET32(p,o)    ntohl(*(uint32_t*)((uint8_t*)(p)+(o)))
#define WRITE_NET8(p,o,v)  (*(uint8_t*)((uint8_t*)(p)+(o)) = (v))
#define WRITE_NET16(p,o,v) (*(uint16_t*)((uint8_t*)(p)+(o)) = htons(v))
#define WRITE_NET32(p,o,v) (*(uint32_t*)((uint8_t*)(p)+(o)) = htonl(v))

#define CPY_IPV4(a,b)   memcpy((a),(b),4)
#define CPY_MAC(a,b)    memcpy((a),(b),6)
#define CMP_IPV4(a,b)   (memcmp((a),(b),4)==0)
#define CMP_MAC(a,b)    (memcmp((a),(b),6)==0)

#define IS_SUBNET(ip,net,mask) \
    (((*(uint32_t*)(ip)) & (*(uint32_t*)(mask))) == *(uint32_t*)(net))

#define IS_BROADCAST(ip,net,mask) \
    (((*(uint32_t*)(net)) | (~*(uint32_t*)(mask))) == *(uint32_t*)(ip))

#define RT_ASSERT(x) do { \
    if (!(x)) { \
        nx_log(LLOG_ERROR, "RT_ASSERT failed: %s  (%s:%d)", #x, __FILE__, __LINE__); \
        svcExitProcess(); \
    } \
} while(0)

/* -------------------------------------------------------------------------
 * ARP cache  (mirrors src/arp.h)
 * ---------------------------------------------------------------------- */
#define ARP_CACHE_LEN 100

struct arp_item {
    uint8_t ip[4];
    uint8_t mac[6];
    time_t  expire_at;
};

typedef int (*arp_for_each_cb)(void *userdata, const struct arp_item *item);

/* -------------------------------------------------------------------------
 * Packet-layer constants  (mirrors src/packet.h)
 * ---------------------------------------------------------------------- */
#define ETHER_OFF_DST      0
#define ETHER_OFF_SRC      6
#define ETHER_OFF_TYPE     12
#define ETHER_OFF_END      14
#define ETHER_OFF_ARP      14
#define ETHER_OFF_IPV4     14
#define ETHER_TYPE_ARP     0x0806
#define ETHER_TYPE_IPV4    0x0800
#define ETHER_HEADER_LEN   14

#define IPV4_PROTOCOL_ICMP 1
#define IPV4_PROTOCOL_TCP  6
#define IPV4_PROTOCOL_UDP  17
#define IPV4_HEADER_LEN    20

#define IPV4_OFF_VER_LEN         0
#define IPV4_OFF_DSCP_ECN        1
#define IPV4_OFF_TOTAL_LEN       2
#define IPV4_OFF_ID              4
#define IPV4_OFF_FLAGS_FRAG_OFFSET 6
#define IPV4_OFF_TTL             8
#define IPV4_OFF_PROTOCOL        9
#define IPV4_OFF_CHECKSUM        10
#define IPV4_OFF_SRC             12
#define IPV4_OFF_DST             16
#define IPV4_OFF_END             20

#define IPV4P_OFF_SRC      0
#define IPV4P_OFF_DST      4
#define IPV4P_OFF_ZERO     8
#define IPV4P_OFF_PROTOCOL 9
#define IPV4P_OFF_LENGTH   10
#define IPV4P_OFF_END      12

#define UDP_OFF_SRCPORT    0
#define UDP_OFF_DSTPORT    2
#define UDP_OFF_LENGTH     4
#define UDP_OFF_CHECKSUM   6
#define UDP_OFF_END        8

#define ARP_OFF_HARDWARE       0
#define ARP_OFF_PROTOCOL       2
#define ARP_OFF_HARDWARE_SIZE  4
#define ARP_OFF_PROTOCOL_SIZE  5
#define ARP_OFF_OPCODE         6
#define ARP_OFF_SENDER_MAC     8
#define ARP_OFF_SENDER_IP      14
#define ARP_OFF_TARGET_MAC     18
#define ARP_OFF_TARGET_IP      24
#define ARP_OFF_END            28
#define ARP_LEN                28
#define ARP_HARDTYPE_ETHER     1
#define ARP_OPCODE_REQUEST     1
#define ARP_OPCODE_REPLY       2

/* -------------------------------------------------------------------------
 * Relay-client fragment buffer  (mirrors lan-play.h)
 * ---------------------------------------------------------------------- */
struct lan_client_fragment {
    uint16_t local_id;
    uint8_t  src[4];
    uint16_t id;
    uint8_t  part;
    uint8_t  used;
    uint16_t total_len;
    uint8_t  buffer[ETHER_MTU];
};

/* -------------------------------------------------------------------------
 * Packet parsing structures  (mirrors src/packet.h)
 * ---------------------------------------------------------------------- */
struct ether_frame {
    const u_char *raw;
    uint16_t      raw_len;
    uint8_t       dst[6];
    uint8_t       src[6];
    uint16_t      type;
    const u_char *payload;
};

struct ipv4 {
    const struct ether_frame *ether;
    uint8_t  version;
    uint8_t  header_len;
    uint8_t  dscp;
    uint8_t  ecn;
    uint16_t total_len;
    uint16_t identification;
    uint8_t  flags;
    uint16_t fragment_offset;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
    const u_char *payload;
};

struct udp {
    const struct ipv4 *ipv4;
    uint16_t srcport;
    uint16_t dstport;
    uint16_t length;
    uint16_t checksum;
    const u_char *payload;
};

struct arp {
    const struct ether_frame *ether;
    uint16_t hardware_type;
    uint16_t protocol_type;
    uint8_t  hardware_size;
    uint8_t  protocol_size;
    uint16_t opcode;
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
    const u_char *payload;
};

struct icmp {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
    uint64_t timestamp;
    const u_char *payload;
};

struct payload {
    const u_char      *ptr;
    uint16_t           len;
    const struct payload *next;
};

/* -------------------------------------------------------------------------
 * Forward-declare the main context, then packet_ctx, then the full struct.
 * packet_ctx.arg points back to the parent lan_play; the original code
 * relies on this exact field layout.
 * ---------------------------------------------------------------------- */
struct lan_play;

struct packet_ctx {
    struct lan_play *arg;       /* back-pointer */
    void   *buffer;
    size_t  buffer_len;

    uint8_t  ip[4];
    uint8_t  subnet_net[4];
    uint8_t  subnet_mask[4];
    const uint8_t *mac;
    uint16_t identification;
    struct arp_item arp_list[ARP_CACHE_LEN];
    time_t   arp_ttl;

    uint64_t upload_byte;
    uint64_t download_byte;
    uint64_t upload_packet;
    uint64_t download_packet;
};

/**
 * Main context — the Switch equivalent of src/lan-play.h::struct lan_play.
 * packet_ctx MUST remain the first field; C code accesses arg->packet_ctx
 * by casting the lan_play pointer.
 */
struct lan_play {
    struct packet_ctx packet_ctx;   /* MUST be first */

    /* Relay UDP socket */
    int relay_fd;
    struct sockaddr_in server_addr;
    uint8_t relay_buf[4096];
    uint8_t send_buf[BUFFER_SIZE];

    /* Settings */
    int  pmtu;
    bool broadcast;
    bool next_real_broadcast;
    int  frag_id;
    int  local_id;
    struct lan_client_fragment frags[LC_FRAG_COUNT];

    /* Auth */
    char         *username;
    unsigned char key[LP_KEY_LEN];

    /* TAP (Universal BPF capture) */
    int      bpf_fd;
    uint32_t wifi_ip;    /* actual Switch WiFi IP (network byte order) — used to filter self-echo */
    uint8_t  my_ip[4];   /* assigned IP in 10.13.0.0/16 */
    uint8_t  my_mac[6];  /* fake Ethernet MAC */

    /* Stats */
    uint64_t upload_byte;
    uint64_t download_byte;
    uint64_t upload_packet;
    uint64_t download_packet;

    /* Threading */
    Thread         relay_thread;
    Thread         tap_thread;
    Thread         keepalive_thread;
    Thread         ldn_udp_thread;
    Thread         ldn_tcp_thread;
    Mutex          mutex;
    volatile bool  running;
};

/* -------------------------------------------------------------------------
 * Cross-module function declarations
 * ---------------------------------------------------------------------- */

/* tap_iface.c  — raw-socket TAP layer */
int  tap_init(struct lan_play *lp);
void tap_close(struct lan_play *lp);
int  tap_send_packet(struct lan_play *lp, const void *data, int len);

/* lan_client_nx.c  — relay UDP client (replaces lan-client.c libuv logic) */
int  lan_client_init(struct lan_play *lp);
void lan_client_close(struct lan_play *lp);
int  lan_client_send_ipv4(struct lan_play *lp, void *dst_ip,
                          const void *packet, uint16_t len);

/* packet.c — called by relay client to deliver a received frame to the game */
int  lan_play_send_packet(struct lan_play *lp, void *data, int size);

#ifdef __cplusplus
}
#endif
