#ifndef IP_H
#define IP_H

#include <stdint.h>

/* IPv4 + ICMP.
 *
 * Everything is driven from task context: ip_input is called by net_poll(),
 * and the PING shell command feeds echo requests in and watches the flags
 * set when the matching reply arrives. */

/* Feed one IPv4 packet (the ethernet payload). */
void ip_input(const uint8_t *pkt, uint16_t len);

/* Send `plen` bytes of `payload` as IPv4 to `dst` (host byte order).
 * Returns 0, or -1 when the next hop's MAC is not resolved yet (an ARP
 * request went out; retry after polling) or the card is down. */
int ip_send(uint32_t dst, uint8_t proto, const uint8_t *payload,
            uint16_t plen);

/* One echo request. `seq` is reported back by the ping session flags. */
void icmp_ping(uint32_t dst, uint16_t seq);

/* Ping session flags, filled in by ip_input when an echo reply arrives. */
extern volatile int      ping_got_reply;
extern volatile uint16_t ping_reply_seq;
extern volatile uint32_t ping_reply_from;

#endif
