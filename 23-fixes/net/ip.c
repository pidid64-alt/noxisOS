/* IPv4 and ICMP (echo request/reply only, which is all noxis needs today). */
#include "ip.h"
#include "net.h"
#include "arp.h"
#include "../cpu/timer.h"
#include "../libc/mem.h"

volatile int      ping_got_reply;
volatile uint16_t ping_reply_seq;
volatile uint32_t ping_reply_from;

#define IP_PROTO_ICMP 1
#define ICMP_ID       0xBEAF

static uint16_t checksum(const uint8_t *data, int len) {
    /* Ones' complement sum over 16-bit words, with the odd tail byte
     * padded; the final inversion is left to the caller. */
    uint32_t sum = 0;

    while (len > 1) {
        sum += (uint16_t)((data[0] << 8) | data[1]);
        data += 2;
        len -= 2;
    }
    if (len == 1)
        sum += (uint16_t)(data[0] << 8);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}

static uint16_t checksum_final(const uint8_t *data, int len) {
    return (uint16_t)(~checksum(data, len));
}

int ip_send(uint32_t dst, uint8_t proto, const uint8_t *payload,
            uint16_t plen) {
    static uint8_t pkt[1500];
    uint8_t dmac[6];
    uint32_t next_hop;

    if (plen > 1480) return -1;   /* 1500-byte ethernet payload - IP header */

    /* Off-subnet destinations go through the gateway. */
    next_hop = ((dst ^ net_ip) & net_mask) ? net_gw : dst;

    if (arp_lookup(next_hop, dmac) != 0)
        return -1;   /* ARP request in flight; caller retries */

    pkt[0] = 0x45;                       /* IPv4, 20-byte header */
    pkt[1] = 0x00;                       /* TOS */
    {
        uint16_t total = (uint16_t)(20 + plen);
        pkt[2] = total >> 8; pkt[3] = total & 0xFF;
    }
    pkt[4] = 0x00; pkt[5] = 0x00;        /* id */
    pkt[6] = 0x00; pkt[7] = 0x00;        /* flags: may fragment (we never do) */
    pkt[8] = 64;                         /* TTL */
    pkt[9] = proto;
    pkt[10] = 0x00; pkt[11] = 0x00;      /* checksum, filled last */
    {
        uint32_t src = htonl(net_ip), d = htonl(dst);
        memory_copy((uint8_t *)&src, pkt + 12, 4);
        memory_copy((uint8_t *)&d, pkt + 16, 4);
    }

    memory_copy((uint8_t *)payload, pkt + 20, plen);

    {
        uint16_t csum = checksum_final(pkt, 20);
        pkt[10] = csum >> 8; pkt[11] = csum & 0xFF;
    }

    eth_send(dmac, ETHERTYPE_IPV4, pkt, (uint16_t)(20 + plen));
    return 0;
}

static void icmp_input(const uint8_t *pkt, uint16_t len, uint32_t src) {
    if (len < 8) return;
    /* A wrong checksum is not worth answering, but echo requests that
     * made it this far are replied to below. */

    if (pkt[0] == 8 && pkt[1] == 0) {           /* echo request */
        uint8_t reply[1480];      /* max IP payload that fits one frame */
        uint16_t n = len;

        if (n > sizeof reply) n = (uint16_t)sizeof reply;
        memory_copy((uint8_t *)pkt, reply, n);
        reply[0] = 0;                            /* echo reply */
        reply[2] = 0; reply[3] = 0;              /* clear checksum */
        {
            uint16_t csum = checksum_final(reply, n);
            reply[2] = csum >> 8; reply[3] = csum & 0xFF;
        }
        ip_send(src, IP_PROTO_ICMP, reply, n);
    } else if (pkt[0] == 0 && pkt[1] == 0) {     /* echo reply: our ping */
        uint16_t id = (uint16_t)((pkt[4] << 8) | pkt[5]);
        if (id == ICMP_ID) {
            ping_reply_seq = (uint16_t)((pkt[6] << 8) | pkt[7]);
            ping_reply_from = src;
            ping_got_reply = 1;
        }
    }
}

void ip_input(const uint8_t *pkt, uint16_t len) {
    uint32_t src, dst;
    uint16_t total;
    int ihl;

    if (len < 20) return;
    if ((pkt[0] >> 4) != 4) return;

    ihl = (pkt[0] & 0x0F) * 4;
    if (ihl < 20) return;

    total = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if (total < 20 || total > len) return;

    /* Verify the header checksum (zero-filled field included in the sum). */
    if (checksum(pkt, ihl) != 0xFFFF) return;

    memory_copy((uint8_t *)(pkt + 12), (uint8_t *)&src, 4);
    memory_copy((uint8_t *)(pkt + 16), (uint8_t *)&dst, 4);
    src = ntohl(src);
    dst = ntohl(dst);

    /* Accept packets addressed to us; hosts also get away with answering
     * broadcasts, but noxis listens for its own address only. */
    if (dst != net_ip) return;

    if (pkt[9] == IP_PROTO_ICMP)
        icmp_input(pkt + ihl, (uint16_t)(total - ihl), src);
}

void icmp_ping(uint32_t dst, uint16_t seq) {
    uint8_t req[16];

    req[0] = 8;                                  /* echo request */
    req[1] = 0;
    req[2] = 0; req[3] = 0;                      /* checksum, filled below */
    req[4] = ICMP_ID >> 8; req[5] = ICMP_ID & 0xFF;
    req[6] = seq >> 8; req[7] = seq & 0xFF;
    memory_copy((uint8_t *)"noxis-ping", req + 8, 8);   /* payload */

    {
        uint16_t csum = checksum_final(req, sizeof req);
        req[2] = csum >> 8; req[3] = csum & 0xFF;
    }

    ip_send(dst, IP_PROTO_ICMP, req, sizeof req);
}
