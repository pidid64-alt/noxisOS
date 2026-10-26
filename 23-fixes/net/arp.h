#ifndef ARP_H
#define ARP_H

#include <stdint.h>

#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV4 0x0800

#define ARP_TABLE_MAX 8

/* Process one ARP packet (the ethernet payload, after the 14-byte header).
 * `eth_src` is the source hardware address of the carrying frame. */
void arp_input(const uint8_t *eth_src, const uint8_t *pkt, uint16_t len);

/* Look up `ip` (host order) in the cache. On a miss a request is (re)sent
 * if no request is currently outstanding; the caller is expected to keep
 * calling net_poll() and retry. Returns 0 on a hit (mac filled in), -1 on
 * a miss. */
int arp_lookup(uint32_t ip, uint8_t mac[6]);

/* Number of valid entries (for the ARP shell command). */
int arp_count(void);

/* Print the table: one "ip mac" line per entry via kprint. */
void arp_dump(void);

#endif
