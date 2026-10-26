/* Ethernet glue: interface configuration, frame build/dispatch, statistics. */
#include "net.h"
#include "ne2000.h"
#include "arp.h"
#include "ip.h"
#include "../libc/mem.h"
#include "../libc/string.h"

uint32_t net_ip   = IP4(10, 0, 2, 15);
uint32_t net_mask = IP4(255, 255, 255, 0);
uint32_t net_gw   = IP4(10, 0, 2, 2);

uint32_t net_rx_packets;
uint32_t net_tx_packets;
uint32_t net_rx_dropped;

void net_stat_rx(void)     { net_rx_packets++; }
void net_stat_tx(void)     { net_tx_packets++; }
void net_stat_dropped(void){ net_rx_dropped++; }

static uint8_t host_mac[6];
static int nic_present;

int net_init(void) {
    nic_present = (ne2000_init() == 0);
    if (nic_present) ne2000_get_mac(host_mac);
    return nic_present ? 0 : -1;
}

int net_up(void) {
    return nic_present;
}

void net_get_mac(uint8_t mac[6]) {
    int i;
    for (i = 0; i < 6; i++) mac[i] = host_mac[i];
}

int eth_send(const uint8_t dst[6], uint16_t ethertype,
             const uint8_t *payload, uint16_t plen) {
    static uint8_t frame[NE_FRAME_MAX];
    int i;

    if (!nic_present) return -1;
    if (plen > 1500) return -1;

    for (i = 0; i < 6; i++) frame[i] = dst[i];
    for (i = 0; i < 6; i++) frame[6 + i] = host_mac[i];
    frame[12] = (uint8_t)(ethertype >> 8);
    frame[13] = (uint8_t)(ethertype & 0xFF);
    memory_copy((uint8_t *)payload, frame + 14, plen);

    return ne2000_send(frame, (uint16_t)(14 + plen));
}

void net_poll(void) {
    static uint8_t buf[NE_FRAME_MAX];
    int len;

    while ((len = ne2000_recv(buf)) >= 14) {
        uint16_t ethertype = (uint16_t)((buf[12] << 8) | buf[13]);
        uint16_t plen = (uint16_t)(len - 14);
        const uint8_t *payload = buf + 14;
        const uint8_t *src = buf + 6;

        if (ethertype == ETHERTYPE_ARP)
            arp_input(src, payload, plen);
        else if (ethertype == ETHERTYPE_IPV4)
            ip_input(payload, plen);
        /* Anything else (VLAN, IPv6, ...) is dropped silently. */
    }
}

void ip_to_str(uint32_t ip, char out[16]) {
    /* int_to_ascii appends the digits reversed-then-reversed; build one
     * octet at a time. out[16] always fits "255.255.255.255". */
    char *p = out;
    int i;
    for (i = 0; i < 4; i++) {
        char oct[4];
        int v = (int)((ip >> (24 - 8 * i)) & 0xFF);
        if (i) *p++ = '.';
        int_to_ascii(v, oct);
        int n = strlen(oct);
        memory_copy((uint8_t *)oct, (uint8_t *)p, n);
        p += n;
    }
    *p = '\0';
}

void mac_to_str(const uint8_t mac[6], char out[18]) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 6; i++) {
        if (i) *out++ = ':';
        *out++ = hex[mac[i] >> 4];
        *out++ = hex[mac[i] & 0xF];
    }
    *out = '\0';
}
