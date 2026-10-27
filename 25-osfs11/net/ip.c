/*************************************************************************//**
 * @file   net/ip.c
 * @brief  IPv4 + ICMP (echo) + UDP dispatch, ported from the 23-fixes
 *         noxis kernel. All timing uses the kernel's get_ticks() at HZ.
 *****************************************************************************/
#include "type.h"
#include "stdio.h"
#include "const.h"
#include "protect.h"
#include "string.h"
#include "fs.h"
#include "proc.h"
#include "tty.h"
#include "console.h"
#include "global.h"
#include "proto.h"
#include "net.h"#include "net.h"

#define IP_PROTO_ICMP	1
#define IP_PROTO_UDP	17
#define ICMP_ID		0xBEAF

/* ping session: the NET task's poll loop watches these flags */
PRIVATE volatile int	ping_replied;
PRIVATE volatile u16	ping_reply_seq;
PRIVATE volatile u32	ping_reply_ip;

/*======================================================================*
			   checksum
 *======================================================================*/
PRIVATE u16 checksum(const u8 *data, int len)
{
	u32 sum = 0;

	while (len > 1) {
		sum += (data[0] << 8) | data[1];
		data += 2;
		len -= 2;
	}
	if (len == 1)
		sum += data[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (u16)(~sum);
}

/*======================================================================*
			   ip_send
 *======================================================================*/
PUBLIC int ip_send(u32 dst, u8 proto, const u8 *payload, u16 plen)
{
	static u8 pkt[1500];
	u8 dmac[6];
	u32 v;

	if (plen > 1480)			/* 1500-byte ethernet payload */
		return -1;

	if (arp_lookup(ip_next_hop(dst), dmac) != 0)
		return -1;			/* ARP request in flight */

	pkt[0] = 0x45;				/* IPv4, 20-byte header */
	pkt[1] = 0x00;
	pkt[2] = (20 + plen) >> 8;
	pkt[3] = (20 + plen) & 0xFF;
	pkt[4] = 0x00; pkt[5] = 0x00;		/* id */
	pkt[6] = 0x00; pkt[7] = 0x00;		/* flags */
	pkt[8] = 64;				/* TTL */
	pkt[9] = proto;
	pkt[10] = 0x00; pkt[11] = 0x00;		/* checksum, filled last */
	v = htonl32(NET_IP);
	memcpy(pkt + 12, (void *)&v, 4);
	v = htonl32(dst);
	memcpy(pkt + 16, (void *)&v, 4);
	memcpy(pkt + 20, (void *)payload, plen);

	v = checksum(pkt, 20);
	pkt[10] = v >> 8; pkt[11] = v & 0xFF;

	return eth_send(dmac, ET_IPV4, pkt, 20 + plen);
}

/*======================================================================*
			   icmp_input
 *======================================================================*/
PRIVATE void icmp_input(const u8 *pkt, u16 len, u32 src)
{
	if (len < 8)
		return;

	if (pkt[0] == 8 && pkt[1] == 0) {	/* echo request: reply */
		static u8 reply[1480];
		u16 n = len, c;

		memcpy(reply, (void *)pkt, n);
		reply[0] = 0;
		reply[2] = 0; reply[3] = 0;
		c = checksum(reply, n);
		reply[2] = c >> 8; reply[3] = c & 0xFF;
		ip_send(src, IP_PROTO_ICMP, reply, n);
	} else if (pkt[0] == 0 && pkt[1] == 0) {	/* echo reply */
		u16 id = (pkt[4] << 8) | pkt[5];

		if (id == ICMP_ID) {
			ping_reply_seq = (pkt[6] << 8) | pkt[7];
			ping_reply_ip = src;
			ping_replied = 1;
		}
	}
}

/*======================================================================*
			   ip_input
 *======================================================================*/
PUBLIC void ip_input(const u8 *pkt, u16 len)
{
	u32 src, dst, v;
	u16 total;
	int ihl;

	if (len < 20)
		return;
	if ((pkt[0] >> 4) != 4)
		return;

	ihl = (pkt[0] & 0x0F) * 4;
	if (ihl < 20 || ihl > len)
		return;

	total = (pkt[2] << 8) | pkt[3];
	if (total < 20 || total > len)
		return;

	if (checksum(pkt, ihl) != 0xFFFF)
		return;				/* bad header checksum */

	memcpy((void *)&v, (void *)(pkt + 12), 4);
	src = ntohl32(v);
	memcpy((void *)&v, (void *)(pkt + 16), 4);
	dst = ntohl32(v);

	if (dst != NET_IP)
		return;

	if (pkt[9] == IP_PROTO_ICMP)
		icmp_input(pkt + ihl, total - ihl, src);
	else if (pkt[9] == IP_PROTO_UDP)
		udp_input(pkt + ihl, total - ihl);
}

/*======================================================================*
			   icmp_ping / ping accessors
 *======================================================================*/
PUBLIC void icmp_ping(u32 dst, u16 seq)
{
	static u8 req[16];
	u16 c;

	req[0] = 8; req[1] = 0;			/* echo request */
	req[2] = 0; req[3] = 0;
	req[4] = ICMP_ID >> 8; req[5] = ICMP_ID & 0xFF;
	req[6] = seq >> 8; req[7] = seq & 0xFF;
	memcpy(req + 8, "noxisOS", 8);		/* 8-byte payload, NUL ends it */

	c = checksum(req, 16);
	req[2] = c >> 8; req[3] = c & 0xFF;

	ping_replied = 0;
	ping_reply_seq = 0;
	ip_send(dst, IP_PROTO_ICMP, req, 16);
}

PUBLIC void ping_reset(void)
{
	ping_replied = 0;
	ping_reply_seq = 0;
}

PUBLIC int ping_pending(void)
{
	return !ping_replied;
}

PUBLIC int ping_got_reply(u16 seq)
{
	return ping_replied && ping_reply_seq == seq;
}

PUBLIC u32 ping_reply_from(void)
{
	return ping_reply_ip;
}
