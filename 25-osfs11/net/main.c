/*************************************************************************//**
 * @file   net/main.c
 * @brief  TASK_NET -- network server task.
 *
 * Brings the NE2000 up at boot (silently; IFCONFIG-style commands report
 * what was found) and serves two requests:
 *
 *   NET_RESOLVE  PATHNAME -> hostname      reply DEVICE = ip, RETVAL
 *   NET_PING     m3i1 = ip, m3i2 = count,  reply m3i1 = replies received,
 *                BUF = u32[count] rtts in ms (0xFFFFFFFF = timeout)
 *
 * The requesters are ring-1 kernel tasks (the desktop TTY), so message
 * pointers live in the shared kernel address space. While serving, the
 * task busy-polls the NIC with milli_delay() backoffs so the rest of the
 * system keeps its timeslices.
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

#define PING_MAX	4
#define PING_TIMEOUT	(HZ)		/* 1 s per echo */
#define ARP_TIMEOUT	(2 * HZ)

/*======================================================================*
			   net_wait_reply
 *======================================================================*/
/**
 * Poll the NIC until cond() turns true or `ticks` pass. Returns 0 on
 * success, -1 on timeout.
 */
PRIVATE int net_wait_reply(int (*cond)(void *), void *arg, u32 ticks)
{
	u32 deadline = get_ticks() + ticks;

	while (!cond(arg)) {
		net_poll();
		if (get_ticks() > deadline)
			return -1;
		milli_delay(2);
	}
	return 0;
}

PRIVATE int cond_arp(void *arg)
{
	u8 mac[6];

	arg = arg;
	return arp_lookup(*(u32 *)arg, mac) == 0;
}

PRIVATE int cond_ping(void *arg)
{
	arg = arg;
	return !ping_pending();
}

/*======================================================================*
			   net_do_resolve
 *======================================================================*/
PRIVATE void net_do_resolve(MESSAGE *msg)
{
	const char *name = (const char *)msg->PATHNAME;
	u32 ip = 0;

	msg->RETVAL = -1;
	msg->DEVICE = 0;

	/* A dotted quad is accepted as-is; anything else goes to DNS. */
	{
		const char *p = name;
		u32 acc = 0;
		int part, digits, ok = 1;

		for (part = 0; part < 4 && ok; part++) {
			int v = 0;

			digits = 0;
			while (*p >= '0' && *p <= '9') {
				v = v * 10 + (*p - '0');
				p++;
				digits++;
				if (digits > 3 || v > 255) { ok = 0; break; }
			}
			if (digits == 0)
				ok = 0;
			acc = (acc << 8) | (u32)v;
			if (part < 3) {
				if (*p != '.')
					ok = 0;
				else
					p++;
			}
		}
		if (ok && *p == 0) {
			msg->RETVAL = 0;
			msg->DEVICE = (int)acc;
			return;
		}
	}

	if (net_ready() && dns_resolve(name, &ip) == 0) {
		msg->RETVAL = 0;
		msg->DEVICE = (int)ip;
	}
}

/*======================================================================*
			   net_do_ping
 *======================================================================*/
PRIVATE void net_do_ping(MESSAGE *msg)
{
	u32 ip = (u32)msg->u.m3.m3i1;
	int count = msg->u.m3.m3i2;
	u32 *rtts;
	int seq, received = 0;
	u8 mac[6];

	if (count < 1)
		count = 1;
	if (count > PING_MAX)
		count = PING_MAX;
	rtts = (u32 *)msg->BUF;
	for (seq = 0; seq < count; seq++)
		rtts[seq] = 0xFFFFFFFF;

	if (!net_ready()) {
		msg->u.m3.m3i1 = -1;
		return;
	}

	/* Resolve the next hop (gateway for off-subnet targets). */
	{
		u32 hop = ip_next_hop(ip);

		if (arp_lookup(hop, mac) != 0 &&
		    net_wait_reply(cond_arp, &hop, ARP_TIMEOUT) != 0) {
			msg->u.m3.m3i1 = -1;	/* no ARP response */
			return;
		}
	}

	for (seq = 1; seq <= count; seq++) {
		u32 t0 = get_ticks();

		ping_reset();
		icmp_ping(ip, (u16)seq);

		if (net_wait_reply(cond_ping, 0, PING_TIMEOUT) == 0 &&
		    ping_got_reply((u16)seq)) {
			rtts[seq - 1] =
				(get_ticks() - t0) * 1000 / HZ;
			received++;
		}
	}

	msg->u.m3.m3i1 = received;
}

/*======================================================================*
			   task_net
 *======================================================================*/
PUBLIC void task_net()
{
	MESSAGE msg;

	net_start();

	while (1) {
		reset_msg(&msg);
		send_recv(RECEIVE, ANY, &msg);

		switch (msg.type) {
		case NET_RESOLVE:
			net_do_resolve(&msg);
			reset_msg(&msg);
			msg.type = SYSCALL_RET;
			send_recv(SEND, msg.source, &msg);
			break;
		case NET_PING:
			net_do_ping(&msg);
			msg.type = SYSCALL_RET;
			send_recv(SEND, msg.source, &msg);
			break;
		default:
			dump_msg("NET::unknown msg", &msg);
			break;
		}
	}
}
