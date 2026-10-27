/*************************************************************************//**
 * @file   net/arp.c
 * @brief  Minimal IPv4-over-Ethernet ARP: cache, answer requests for our
 *         own address, keep one outstanding request for lookups.
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

#define ARP_TABLE_MAX	8
#define ARP_RETRY_TICKS	(HZ / 2)	/* re-send every 0.5 s */

typedef struct {
	u32	ip;			/* host order; 0 = free slot */
	u8	mac[6];
} ARP_ENTRY;

PRIVATE ARP_ENTRY arp_table[ARP_TABLE_MAX];

PRIVATE int	 pending;
PRIVATE u32	 pending_ip;
PRIVATE u32	 pending_since;		/* tick of the last request */

PRIVATE const u8 broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/*======================================================================*
			   learn
 *======================================================================*/
PRIVATE void arp_learn(u32 ip, const u8 *mac)
{
	int i, victim = 0;

	if (ip == 0)
		return;
	for (i = 0; i < ARP_TABLE_MAX; i++) {
		if (arp_table[i].ip == ip) {
			memcpy(arp_table[i].mac, (void *)mac, 6);
			return;
		}
	}
	for (i = 0; i < ARP_TABLE_MAX; i++)
		if (arp_table[i].ip == 0) { victim = i; break; }
	arp_table[victim].ip = ip;
	memcpy(arp_table[victim].mac, (void *)mac, 6);
}

/*======================================================================*
			   arp_send
 *======================================================================*/
PRIVATE void arp_send(u16 op, u32 tip, const u8 *tha)
{
	u8 pkt[28];
	u32 v;

	pkt[0] = 0x00; pkt[1] = 0x01;		/* htype: ethernet */
	pkt[2] = 0x08; pkt[3] = 0x00;		/* ptype: IPv4 */
	pkt[4] = 6;				/* hlen */
	pkt[5] = 4;				/* plen */
	pkt[6] = op >> 8; pkt[7] = op & 0xFF;
	memcpy(pkt + 8, host_mac_of(), 6);	/* sha */
	v = htonl32(NET_IP);
	memcpy(pkt + 14, (void *)&v, 4);	/* spa */
	memcpy(pkt + 18, (void *)tha, 6);	/* tha */
	v = htonl32(tip);
	memcpy(pkt + 24, (void *)&v, 4);	/* tpa */

	eth_send(broadcast, ET_ARP, pkt, sizeof(pkt));
}

/*======================================================================*
			   arp_input
 *======================================================================*/
PUBLIC void arp_input(const u8 *eth_src, const u8 *pkt, u16 len)
{
	u32 sip, tip;

	if (len < 28)
		return;
	if (pkt[0] != 0x00 || pkt[1] != 0x01)	/* ethernet */
		return;
	if (pkt[2] != 0x08 || pkt[3] != 0x00)	/* IPv4 */
		return;
	if (pkt[4] != 6 || pkt[5] != 4)
		return;

	memcpy((void *)&sip, (void *)(pkt + 14), 4);
	memcpy((void *)&tip, (void *)(pkt + 24), 4);
	sip = ntohl32(sip);
	tip = ntohl32(tip);

	arp_learn(sip, pkt + 8);		/* every frame teaches us */

	if ((pkt[6] << 8 | pkt[7]) == 1 && tip == NET_IP)
		arp_send(2, sip, pkt + 8);	/* who-has us: reply */
	else if ((pkt[6] << 8 | pkt[7]) == 2 && sip == pending_ip)
		pending = 0;			/* our request answered */

	eth_src = eth_src;
}

/*======================================================================*
			   arp_lookup
 *======================================================================*/
PUBLIC int arp_lookup(u32 ip, u8 mac[6])
{
	int i;

	for (i = 0; i < ARP_TABLE_MAX; i++) {
		if (arp_table[i].ip == ip && arp_table[i].ip != 0) {
			memcpy(mac, arp_table[i].mac, 6);
			return 0;
		}
	}

	/* Miss: ask, re-sending periodically so a lost frame cannot wedge
	 * the lookup. The caller keeps polling net_poll() and retrying. */
	{
		static const u8 zero[6] = {0, 0, 0, 0, 0, 0};
		int now = get_ticks();

		if (!pending || now - pending_since > ARP_RETRY_TICKS) {
			arp_send(1, ip, zero);
			pending = 1;
			pending_ip = ip;
			pending_since = now;
		}
	}
	return -1;
}
