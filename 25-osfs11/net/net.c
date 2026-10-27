/*************************************************************************//**
 * @file   net/net.c
 * @brief  Ethernet glue: interface config, frame build/dispatch, helpers.
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

PRIVATE u8 host_mac[6];
PRIVATE int nic_present;

PUBLIC void net_start(void)
{
	nic_present = (ne2000_init() == 0);
	if (nic_present)
		ne2000_get_mac(host_mac);
}

PUBLIC int net_ready(void)
{
	return nic_present;
}

PUBLIC u8 *host_mac_of(void)
{
	return host_mac;
}

PUBLIC u16 htons16(u16 v)
{
	return (u16)((v >> 8) | (v << 8));
}

PUBLIC u32 htonl32(u32 v)
{
	return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
	       ((v >> 8) & 0xFF00) | (v >> 24);
}

PUBLIC u32 ntohl32(u32 v)
{
	return htonl32(v);
}

PUBLIC int eth_send(const u8 dst[6], u16 ethertype,
		    const u8 *payload, u16 plen)
{
	static u8 frame[NE_FRAME_MAX];
	int i;

	if (!nic_present)
		return -1;
	if (plen > 1500)
		return -1;

	for (i = 0; i < 6; i++) {
		frame[i] = dst[i];
		frame[6 + i] = host_mac[i];
	}
	frame[12] = (u8)(ethertype >> 8);
	frame[13] = (u8)(ethertype & 0xFF);
	memcpy(frame + 14, (void *)payload, plen);

	return ne2000_send(frame, 14 + plen);
}

/*======================================================================*
			   net_poll
 *======================================================================*/
/**
 * Pop every queued frame off the NIC and dispatch it by ethertype.
 * Called from the NET task's wait loops; safe to call often.
 */
PUBLIC void net_poll(void)
{
	static u8 buf[NE_FRAME_MAX];
	int len;

	ne2000_poll();
	while ((len = ne2000_recv(buf)) >= 14) {
		u16 ethertype = (buf[12] << 8) | buf[13];
		u16 plen = len - 14;

		if (ethertype == ET_ARP)
			arp_input(buf + 6, buf + 14, plen);
		else if (ethertype == ET_IPV4)
			ip_input(buf + 14, plen);
		/* anything else (IPv6, VLAN, ...) is dropped */
	}
	dns_poll();
}

/*======================================================================*
			   ip_next_hop
 *======================================================================*/
/** Off-subnet destinations go through the gateway. */
PUBLIC u32 ip_next_hop(u32 dst)
{
	return ((dst ^ NET_IP) & NET_MASK) ? NET_GW : dst;
}

/*======================================================================*
			   ip_to_str
 *======================================================================*/
PUBLIC void ip_to_str(u32 ip, char out[16])
{
	static const char digits[] = "0123456789";
	int i, p = 0;

	for (i = 0; i < 4; i++) {
		int v = (ip >> (24 - 8 * i)) & 0xFF;

		if (i)
			out[p++] = '.';
		if (v >= 100)
			out[p++] = digits[v / 100];
		if (v >= 10)
			out[p++] = digits[(v / 10) % 10];
		out[p++] = digits[v % 10];
	}
	out[p] = 0;
}
