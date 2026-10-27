/*************************************************************************//**
 * @file   net/net.h
 * @brief  Internal header shared by the net/ stack (TASK_NET).
 *
 * QEMU user networking (slirp) has fixed addresses, so the stack is
 * configured statically exactly like the 23-fixes kernel:
 *   guest 10.0.2.15/24, gateway 10.0.2.2, name server 10.0.2.3.
 *****************************************************************************/
#ifndef _NET_H_
#define _NET_H_

#include "type.h"

#define NET_IRQ		9	/* QEMU ne2k_isa irq= -> IRQ9 (vectors 41) */
#define NE_BASE		0x300

#define NET_IP		0x0A00020F	/* 10.0.2.15, host order */
#define NET_MASK	0xFFFFFF00	/* /24 */
#define NET_GW		0x0A000202	/* 10.0.2.2 */
#define NET_DNS		0x0A000203	/* 10.0.2.3 */

#define NE_FRAME_MAX	1536
#define ET_IPV4		0x0800
#define ET_ARP		0x0806

/* driver (ne2000.c) */
PUBLIC int  ne2000_init(void);
PUBLIC int  ne2000_present(void);
PUBLIC void ne2000_get_mac(u8 mac[6]);
PUBLIC void ne2000_poll(void);
PUBLIC int  ne2000_send(const u8 *frame, u16 len);
PUBLIC int  ne2000_recv(u8 *buf);

/* ethernet + dispatch (net.c) */
PUBLIC int  eth_send(const u8 dst[6], u16 ethertype,
		     const u8 *payload, u16 plen);
PUBLIC void net_poll(void);		/* drain the NIC, dispatch frames */
PUBLIC void net_start(void);		/* probe the NIC */
PUBLIC int  net_ready(void);
PUBLIC u8 * host_mac_of(void);
PUBLIC void ip_to_str(u32 ip, char out[16]);
PUBLIC u32  ip_next_hop(u32 dst);
PUBLIC u32  htonl32(u32 v);
PUBLIC u32  ntohl32(u32 v);
PUBLIC u16  htons16(u16 v);

/* arp.c */
PUBLIC void arp_input(const u8 *eth_src, const u8 *pkt, u16 len);
PUBLIC int  arp_lookup(u32 ip, u8 mac[6]);	/* -1 = try again later */

/* ip.c: IPv4 + ICMP + UDP dispatch */
PUBLIC int  ip_send(u32 dst, u8 proto, const u8 *payload, u16 plen);
PUBLIC void ip_input(const u8 *pkt, u16 len);
PUBLIC void icmp_ping(u32 dst, u16 seq);
PUBLIC void ping_reset(void);
PUBLIC int  ping_pending(void);
PUBLIC int  ping_got_reply(u16 seq);
PUBLIC u32  ping_reply_from(void);

/* dns.c: UDP + a minimal resolver against NET_DNS */
PUBLIC int  udp_send(u32 dst, u16 sport, u16 dport,
		     const u8 *payload, u16 plen);
PUBLIC void udp_input(const u8 *pkt, u16 len);
PUBLIC int  dns_resolve(const char *name, u32 *ip_out);
PUBLIC void dns_poll(void);

#endif /* _NET_H_ */
