#ifndef NET_H
#define NET_H

#include <stdint.h>

/* Interface configuration.
 *
 * QEMU's default user-mode networking (`-netdev user`) is a virtual LAN with
 * fixed addresses, so a static configuration is enough for now:
 *
 *   guest address .15   gateway (the host) .2   name server .3   net /24
 *
 * DHCP could replace this later; nothing else in the stack assumes the
 * addresses are fixed. */

#define IP4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | \
     (uint32_t)(d))

extern uint32_t net_ip;        /* our address, host byte order   */
extern uint32_t net_mask;      /* netmask, host byte order       */
extern uint32_t net_gw;        /* default gateway                */

extern uint32_t net_rx_packets;
extern uint32_t net_tx_packets;
extern uint32_t net_rx_dropped;

/* Statistics hooks used by the driver (declared here so ne2000.c does not
 * need to know about the protocol stack). */
void net_stat_rx(void);
void net_stat_tx(void);
void net_stat_dropped(void);

int  net_init(void);           /* probe the NIC; 0 if one was found */
int  net_up(void);             /* 1 if a card was detected at boot */
void net_get_mac(uint8_t mac[6]);

/* Send an ethernet payload (adds the 14-byte header). */
int eth_send(const uint8_t dst[6], uint16_t ethertype,
             const uint8_t *payload, uint16_t plen);

/* Process every queued received frame. Cheap to call often. */
void net_poll(void);

/* Formatting helpers shared by the shell commands. */
void ip_to_str(uint32_t ip, char out[16]);   /* host order -> "a.b.c.d" */
void mac_to_str(const uint8_t mac[6], char out[18]);

static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint16_t ntohs(uint16_t v) {
    return htons(v);
}
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) |
           (v >> 24);
}
static inline uint32_t ntohl(uint32_t v) {
    return htonl(v);
}

#endif
