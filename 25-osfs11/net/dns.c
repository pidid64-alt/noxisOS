/*************************************************************************//**
 * @file   net/dns.c
 * @brief  UDP + a minimal A-record resolver against the slirp name server
 *         (10.0.2.3). One query in flight at a time, polled by the NET
 *         task through dns_poll().
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

#define DNS_PORT	53
#define DNS_LOCAL_PORT	0x1234
#define DNS_QUERY_ID	0x1234
#define DNS_TIMEOUT	(3 * HZ)

/* resolution state, filled in by udp_input */
PRIVATE void	dns_input(const u8 *pkt, u16 len);

PRIVATE volatile int	dns_done;
PRIVATE volatile int	dns_ok;
PRIVATE volatile u32	dns_result;

/*======================================================================*
			   udp_send
 *======================================================================*/
PUBLIC int udp_send(u32 dst, u16 sport, u16 dport,
		    const u8 *payload, u16 plen)
{
	static u8 pkt[1500];
	u16 csum_hdr;
	u16 total;

	if (plen > 1472)
		return -1;
	total = 8 + plen;

	pkt[0] = sport >> 8;  pkt[1] = sport & 0xFF;
	pkt[2] = dport >> 8;  pkt[3] = dport & 0xFF;
	pkt[4] = total >> 8;  pkt[5] = total & 0xFF;
	pkt[6] = 0x00; pkt[7] = 0x00;		/* checksum: 0 = none (IPv4) */
	memcpy(pkt + 8, (void *)payload, plen);

	/* slirp verifies nothing extra, but many stacks drop UDP with a
	 * wrong checksum field; leave 0 (legal for IPv4). */
	csum_hdr = 0; csum_hdr = csum_hdr;

	return ip_send(dst, 17, pkt, total);
}

/*======================================================================*
			   udp_input
 *======================================================================*/
PUBLIC void udp_input(const u8 *pkt, u16 len)
{
	u16 dport, clen;

	if (len < 8)
		return;
	dport = (pkt[2] << 8) | pkt[3];
	clen = ((pkt[4] << 8) | pkt[5]) - 8;
	if (clen > len - 8)
		clen = len - 8;

	if (dport == DNS_LOCAL_PORT)
		dns_input(pkt + 8, clen);
}

/*======================================================================*
			   dns_query_build
 *======================================================================*/
/**
 * Build a standard A-record query: header + QNAME labels + QTYPE=A +
 * QCLASS=IN. Returns the length, or -1 if the name is empty/too long.
 */
PRIVATE int dns_query_build(const char *name, u8 *q, int cap)
{
	int nlen, i, p = 0, label = 0;

	nlen = strlen(name);
	if (nlen == 0 || nlen > 200)
		return -1;

	/* header */
	q[p++] = DNS_QUERY_ID >> 8; q[p++] = DNS_QUERY_ID & 0xFF;
	q[p++] = 0x01; q[p++] = 0x00;		/* flags: RD=1 */
	q[p++] = 0x00; q[p++] = 0x01;		/* QDCOUNT = 1 */
	q[p++] = 0x00; q[p++] = 0x00;		/* ANCOUNT = 0 */
	q[p++] = 0x00; q[p++] = 0x00;		/* NSCOUNT = 0 */
	q[p++] = 0x00; q[p++] = 0x00;		/* ARCOUNT = 0 */

	/* QNAME: "google.com" -> 6'g''o'... 3'c''o''m' 0 */
	for (i = 0; i <= nlen; i++) {
		if (name[i] == '.' || name[i] == 0) {
			if (i == label || i - label > 63)
				return -1;	/* empty/oversized label */
			q[p++] = i - label;
			while (label < i)
				q[p++] = name[label++];
			label = i + 1;
		}
		if (p + nlen + 6 > cap)
			return -1;
	}
	q[p++] = 0;				/* root label */

	q[p++] = 0x00; q[p++] = 0x01;		/* QTYPE  = A */
	q[p++] = 0x00; q[p++] = 0x01;		/* QCLASS = IN */
	return p;
}

/*======================================================================*
			   dns_skip_name
 *======================================================================*/
/** Skip a (possibly compressed) name; returns the offset after it. */
PRIVATE int dns_skip_name(const u8 *p, int len, int off)
{
	while (off < len) {
		u8 c = p[off];

		if (c == 0)
			return off + 1;
		if ((c & 0xC0) == 0xC0)
			return (off + 2 < len) ? off + 2 : len;
		off += 1 + c;
	}
	return len;
}

/*======================================================================*
			   dns_input
 *======================================================================*/
PUBLIC void dns_input(const u8 *pkt, u16 len)
{
	u16 id, flags, qd, an;
	int off, i;

	if (len < 12)
		return;
	id = (pkt[0] << 8) | pkt[1];
	if (id != DNS_QUERY_ID)
		return;
	flags = (pkt[2] << 8) | pkt[3];
	if (!(flags & 0x8000))			/* not a response */
		return;
	if ((flags & 0x000F) != 0) {		/* non-zero RCODE: NXDomain */
		dns_done = 1;
		dns_ok = 0;
		return;
	}
	an = (pkt[6] << 8) | pkt[7];
	if (an == 0) {
		dns_done = 1;
		dns_ok = 0;
		return;
	}

	/* skip header + the single question */
	off = 12;
	qd = (pkt[4] << 8) | pkt[5];
	for (i = 0; i < qd && off < len; i++) {
		off = dns_skip_name(pkt, len, off);
		off += 4;			/* QTYPE + QCLASS */
	}

	/* first A answer wins */
	for (i = 0; i < an && off + 12 <= len; i++) {
		u16 type, rdlen;
		int saved;

		off = dns_skip_name(pkt, len, off);
		if (off + 10 > len)
			return;
		type = (pkt[off] << 8) | pkt[off + 1];
		rdlen = (pkt[off + 8] << 8) | pkt[off + 9];
		off += 10;
		saved = off;

		if (type == 1 && rdlen == 4 && off + 4 <= len) {
			/* A record: network order in the wire, host order
			 * for the rest of the stack */
			dns_result = ntohl32(*(const u32 *)(pkt + off));
			dns_done = 1;
			dns_ok = 1;
			return;
		}
		off = saved + rdlen;
	}

	dns_done = 1;
	dns_ok = 0;				/* answers, but no A record */
}

/*======================================================================*
			   dns_poll
 *======================================================================*/
/** Hook called from net_poll(); nothing to do, the flags are set inline. */
PUBLIC void dns_poll(void)
{
}

/*======================================================================*
			   dns_resolve
 *======================================================================*/
/**
 * Resolve `name` to an IPv4 address. Blocking: polls net_poll() until the
 * answer arrives or DNS_TIMEOUT ticks pass. Returns 0 and fills *ip_out,
 * or -1 (nic down, bad name, timeout, NXDomain, no A record).
 */
PUBLIC int dns_resolve(const char *name, u32 *ip_out)
{
	static u8 query[300];
	int qlen, resent = 0;
	u32 deadline;

	if (!net_ready())
		return -1;

	qlen = dns_query_build(name, query, sizeof(query));
	if (qlen < 0)
		return -1;

	dns_done = 0;
	dns_ok = 0;
	dns_result = 0;

	if (udp_send(NET_DNS, DNS_LOCAL_PORT, DNS_PORT, query, qlen) != 0)
		return -1;

	deadline = get_ticks() + DNS_TIMEOUT;
	while (!dns_done) {
		net_poll();
		if (get_ticks() > deadline)
			return -1;		/* timeout */
		if (!resent && get_ticks() >= deadline - DNS_TIMEOUT / 2) {
			/* one re-send midway, in case the query was lost */
			udp_send(NET_DNS, DNS_LOCAL_PORT, DNS_PORT,
				 query, qlen);
			resent = 1;
		}
		milli_delay(2);
	}

	if (!dns_ok)
		return -1;
	*ip_out = dns_result;
	return 0;
}
