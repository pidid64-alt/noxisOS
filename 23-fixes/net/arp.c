/* Minimal IPv4-over-Ethernet ARP: cache learned entries, answer requests
 * for our own address, and keep one outstanding request for lookups. */
#include "arp.h"
#include "net.h"
#include "../cpu/timer.h"
#include "../libc/mem.h"
#include "../libc/string.h"
#include "../libc/function.h"
#include "../drivers/screen.h"

typedef struct {
    uint32_t ip;          /* host byte order; 0 = free slot */
    uint8_t  mac[6];
} arp_entry_t;

static arp_entry_t table[ARP_TABLE_MAX];

/* One request in flight is plenty for a shell-driven stack. */
static int      pending;
static uint32_t pending_ip;
static uint32_t pending_since;   /* tick when the request went out */

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void learn(uint32_t ip, const uint8_t *mac) {
    int i, victim = 0;

    if (ip == 0) return;
    for (i = 0; i < ARP_TABLE_MAX; i++) {
        if (table[i].ip == ip) {
            memory_copy((uint8_t *)mac, table[i].mac, 6);
            return;
        }
    }
    /* Free slot or the oldest one (slot 0 doubles as the victim). */
    for (i = 0; i < ARP_TABLE_MAX; i++)
        if (table[i].ip == 0) { victim = i; break; }
    table[victim].ip = ip;
    memory_copy((uint8_t *)mac, table[victim].mac, 6);
}

static void send_packet(uint16_t op, uint32_t tip, const uint8_t *tha) {
    /* htype, ptype, hlen, plen, op, sha, spa, tha, tpa */
    uint8_t pkt[28];
    uint8_t our_mac[6];

    net_get_mac(our_mac);

    pkt[0] = 0x00; pkt[1] = 0x01;                    /* ethernet */
    pkt[2] = 0x08; pkt[3] = 0x00;                    /* IPv4 */
    pkt[4] = 6;                                      /* hlen */
    pkt[5] = 4;                                      /* plen */
    pkt[6] = op >> 8; pkt[7] = op & 0xFF;
    memory_copy(our_mac, pkt + 8, 6);            /* sha */
    {
        uint32_t spa = htonl(net_ip);
        memory_copy((uint8_t *)&spa, pkt + 14, 4);   /* spa */
    }
    memory_copy((uint8_t *)tha, pkt + 18, 6);    /* tha */
    {
        uint32_t tpa = htonl(tip);
        memory_copy((uint8_t *)&tpa, pkt + 24, 4);   /* tpa */
    }

    eth_send(BROADCAST, ETHERTYPE_ARP, pkt, sizeof pkt);
}

void arp_input(const uint8_t *eth_src, const uint8_t *pkt, uint16_t len) {
    uint32_t sip, tip;

    if (len < 28) return;
    if (pkt[0] != 0x00 || pkt[1] != 0x01) return;    /* not ethernet */
    if (pkt[2] != 0x08 || pkt[3] != 0x00) return;    /* not IPv4 */
    if (pkt[4] != 6 || pkt[5] != 4) return;

    memory_copy((uint8_t *)(pkt + 14), (uint8_t *)&sip, 4);
    memory_copy((uint8_t *)(pkt + 24), (uint8_t *)&tip, 4);
    sip = ntohl(sip);
    tip = ntohl(tip);

    /* Every valid ARP frame tells us one binding — cache it. */
    learn(sip, pkt + 8);

    if ((pkt[6] << 8 | pkt[7]) == 1 && tip == net_ip) {
        /* Who-has us: answer with our MAC, unicast to the asker. */
        send_packet(2, sip, pkt + 8);
    } else if ((pkt[6] << 8 | pkt[7]) == 2 && sip == pending_ip) {
        pending = 0;                                 /* reply arrived */
    }
    UNUSED(eth_src);
}

int arp_lookup(uint32_t ip, uint8_t mac[6]) {
    int i;

    for (i = 0; i < ARP_TABLE_MAX; i++) {
        if (table[i].ip == ip && table[i].ip != 0) {
            memory_copy(table[i].mac, mac, 6);
            return 0;
        }
    }

    /* Miss: ask, unless a request is already in flight (re-send after
     * half a second so a lost frame cannot wedge the stack). */
    if (!pending || tick - pending_since > 25) {
        static const uint8_t zero[6] = {0};
        send_packet(1, ip, zero);
        pending = 1;
        pending_ip = ip;
        pending_since = tick;
    }
    return -1;
}

int arp_count(void) {
    int i, n = 0;
    for (i = 0; i < ARP_TABLE_MAX; i++)
        if (table[i].ip != 0) n++;
    return n;
}

void arp_dump(void) {
    int i;
    char ipbuf[16], macbuf[18];

    kprint("Address          HWaddress\n");
    for (i = 0; i < ARP_TABLE_MAX; i++) {
        int pad;
        if (table[i].ip == 0) continue;
        ip_to_str(table[i].ip, ipbuf);
        mac_to_str(table[i].mac, macbuf);
        kprint(ipbuf);
        for (pad = strlen(ipbuf); pad < 17; pad++) kprint(" ");
        kprint(macbuf);
        kprint("\n");
    }
}
