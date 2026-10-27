/*************************************************************************//**
 * @file   net/ne2000.c
 * @brief  NE2000 (DP8390) ISA NIC driver for TASK_NET.
 *
 * Ported from the 23-fixes noxis kernel (word-wide remote DMA, correct CR
 * bit layout: STRT=0x02/STP=0x01, TXP=0x04). Differences for this kernel:
 * port I/O goes through in_byte/out_byte/port_read/port_write, the IRQ is
 * hooked with put_irq_handler/enable_irq, and there is no console output --
 * errors are return codes for the NET task.
 *
 * Card SRAM layout (256-byte pages):
 *   0x40..0x45  transmit staging buffer
 *   0x46..0x7F  receive ring written by the card
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

/* Register offsets from the base. */
#define REG_CR		0x00
#define REG_PSTART	0x01
#define REG_PSTOP	0x02
#define REG_BNRY	0x03
#define REG_TPSR	0x04
#define REG_TBCR0	0x05
#define REG_TBCR1	0x06
#define REG_ISR		0x07
#define REG_RSAR0	0x08
#define REG_RSAR1	0x09
#define REG_RBCR0	0x0A
#define REG_RBCR1	0x0B
#define REG_RCR		0x0C
#define REG_TCR		0x0D
#define REG_DCR		0x0E
#define REG_IMR		0x0F
#define REG_CURR	0x07	/* page 1 */
#define REG_PAR		0x01	/* page 1: PAR0..PAR5 */
#define REG_DATA	0x10
#define REG_RESET	0x1F

/* CR command bits: [7:6]=page, [5:3]=RD2..RD0, [2]=TXP, [1]=STRT, [0]=STP. */
#define CR_PAGE0	0x00
#define CR_PAGE1	0x40
#define CR_NODMA	0x20	/* RD=100: no/abort remote DMA */
#define CR_RWRITE	0x10	/* RD=010 */
#define CR_RREAD	0x08	/* RD=001 */
#define CR_TXP		0x04
#define CR_START	0x02

/* ISR bits (write 1 to clear). */
#define ISR_PRX		0x01
#define ISR_PTX		0x02
#define ISR_TXE		0x08
#define ISR_RDC		0x40

#define TX_START	0x40
#define RX_START	0x46
#define RX_STOP		0x80

#define NE_FRAME_MAX	1536

/* Software receive ring: the IRQ handler (single producer) pushes frames,
 * the NET task (single consumer) drains them through ne2000_recv(). */
#define SW_RING		8
PRIVATE u8		sw_ring[SW_RING][NE_FRAME_MAX];
PRIVATE u16		sw_len[SW_RING];
PRIVATE volatile u8	sw_head;
PRIVATE volatile u8	sw_tail;

PRIVATE int		card_present;
PRIVATE u8		card_mac[6];

PUBLIC int ne2000_present(void)
{
	return card_present;
}

PUBLIC void ne2000_get_mac(u8 mac[6])
{
	int i;

	for (i = 0; i < 6; i++)
		mac[i] = card_mac[i];
}

/*======================================================================*
			   remote_read
 *======================================================================*/
/**
 * Move `len` bytes (padded to even) from card SRAM into buf. The NE2000
 * data port moves one WORD per access, so every transfer goes through
 * port_read/port_write.
 */
PRIVATE void remote_read(u16 addr, u16 len, u8 *buf)
{
	u16 words = (len + 1) / 2;

	out_byte(NE_BASE + REG_CR, CR_START | CR_NODMA | CR_PAGE0);
	out_byte(NE_BASE + REG_RSAR0, addr & 0xFF);
	out_byte(NE_BASE + REG_RSAR1, addr >> 8);
	out_byte(NE_BASE + REG_RBCR0, (words * 2) & 0xFF);
	out_byte(NE_BASE + REG_RBCR1, (words * 2) >> 8);
	out_byte(NE_BASE + REG_CR, CR_START | CR_RREAD | CR_PAGE0);
	port_read(NE_BASE + REG_DATA, buf, words);

	/* Ack the remote-DMA-complete bit (bounded wait, IRQ context safe). */
	{
		int spin;

		for (spin = 0; spin < 100000; spin++)
			if (in_byte(NE_BASE + REG_ISR) & ISR_RDC)
				break;
		out_byte(NE_BASE + REG_ISR, ISR_RDC);
	}
}

/*======================================================================*
			   remote_write
 *======================================================================*/
PRIVATE void remote_write(u16 addr, u16 len, const u8 *buf)
{
	u16 words = (len + 1) / 2;
	u8 tmp[NE_FRAME_MAX];
	int spin;

	if (words * 2 > NE_FRAME_MAX)
		return;
	if (len & 1) {			/* pad odd tail with a zero byte */
		memcpy(tmp, (void *)buf, len);
		tmp[len] = 0;
		buf = tmp;
	}

	out_byte(NE_BASE + REG_CR, CR_START | CR_NODMA | CR_PAGE0);
	out_byte(NE_BASE + REG_RSAR0, addr & 0xFF);
	out_byte(NE_BASE + REG_RSAR1, addr >> 8);
	out_byte(NE_BASE + REG_RBCR0, (words * 2) & 0xFF);
	out_byte(NE_BASE + REG_RBCR1, (words * 2) >> 8);
	out_byte(NE_BASE + REG_CR, CR_START | CR_RWRITE | CR_PAGE0);
	port_write(NE_BASE + REG_DATA, (void *)buf, words);

	for (spin = 0; spin < 100000; spin++)
		if (in_byte(NE_BASE + REG_ISR) & ISR_RDC)
			break;
	out_byte(NE_BASE + REG_ISR, ISR_RDC);
}

/*======================================================================*
			   ne2000_poll
 *======================================================================*/
/**
 * Drain every packet currently sitting in the card's receive ring into the
 * software ring. Called both from the IRQ handler and from the NET task's
 * poll loops (both advances are idempotent).
 */
PUBLIC void ne2000_poll(void)
{
	u8 bnry;
	int n;

	if (!card_present)
		return;

	bnry = in_byte(NE_BASE + REG_BNRY);
	for (n = 0; n < 32; n++) {
		u8 pkt, hdr[4], next, curr;
		u16 count, len;

		pkt = bnry + 1;
		if (pkt >= RX_STOP)
			pkt = RX_START;

		out_byte(NE_BASE + REG_CR, CR_START | CR_NODMA | CR_PAGE1);
		curr = in_byte(NE_BASE + REG_CURR);
		out_byte(NE_BASE + REG_CR, CR_START | CR_NODMA | CR_PAGE0);

		if (pkt == curr)
			break;			/* ring empty */

		remote_read(pkt << 8, 4, hdr);
		next = hdr[1];
		count = hdr[2] | (hdr[3] << 8);
		if (count < 4)
			count = 4;
		len = count - 4;		/* strip the card's 4-byte header */
		if (len > NE_FRAME_MAX)
			len = NE_FRAME_MAX;

		if (next < RX_START || next >= RX_STOP)
			break;			/* corrupted ring */

		{
			u8 push = (sw_head + 1) % SW_RING;

			if (push != sw_tail) {	/* drop when full */
				remote_read((pkt << 8) + 4, len,
					    sw_ring[sw_head]);
				sw_len[sw_head] = len;
				sw_head = push;
			}
		}

		bnry = next - 1;
		if (bnry < RX_START)
			bnry = RX_STOP - 1;
		out_byte(NE_BASE + REG_BNRY, bnry);
	}
}

/*======================================================================*
			   ne2000_irq
 *======================================================================*/
PRIVATE void ne2000_irq(int irq)
{
	u8 isr;

	irq = irq;
	isr = in_byte(NE_BASE + REG_ISR);
	if (isr & ISR_PRX)
		ne2000_poll();
	out_byte(NE_BASE + REG_ISR, isr & 0x7F);
}

/*======================================================================*
			   ne2000_init
 *======================================================================*/
PUBLIC int ne2000_init(void)
{
	u8 prom[16];
	int i;

	/* Reset pulse. */
	out_byte(NE_BASE + REG_RESET, 0x00);
	for (i = 0; i < 1000; i++)
		(void)in_byte(NE_BASE + REG_RESET);

	out_byte(NE_BASE + REG_CR, CR_PAGE0);	/* stopped, page 0 */

	out_byte(NE_BASE + REG_DCR, 0x49);	/* word-wide DMA */
	out_byte(NE_BASE + REG_RBCR0, 0x00);
	out_byte(NE_BASE + REG_RBCR1, 0x00);
	out_byte(NE_BASE + REG_RCR, 0x04);	/* accept broadcast */
	out_byte(NE_BASE + REG_TCR, 0x02);	/* loopback while configuring */
	out_byte(NE_BASE + REG_PSTART, RX_START);
	out_byte(NE_BASE + REG_PSTOP, RX_STOP);
	out_byte(NE_BASE + REG_BNRY, RX_START - 1);
	out_byte(NE_BASE + REG_TPSR, TX_START);
	out_byte(NE_BASE + REG_ISR, 0xFF);

	/* The station address sits in the PROM, mirrored into SRAM page 0.
	 * Reads of an empty port return 0xFF, which doubles as detection. */
	remote_read(0x0000, 16, prom);
	{
		int all_ff = 1, all_00 = 1;

		for (i = 0; i < 6; i++) {
			if (prom[i] != 0xFF) all_ff = 0;
			if (prom[i] != 0x00) all_00 = 0;
		}
		if (all_ff || all_00)
			return -1;			/* no card at NE_BASE */
	}

	/* The PROM stores each MAC byte mirrored in both halves of its
	 * 16-bit word: prom[2i] == prom[2i+1] == mac[i]. */
	for (i = 0; i < 6; i++)
		card_mac[i] = prom[i * 2];

	out_byte(NE_BASE + REG_CR, CR_PAGE1);	/* stopped, page 1 */
	for (i = 0; i < 6; i++)
		out_byte(NE_BASE + REG_PAR + i, card_mac[i]);
	out_byte(NE_BASE + REG_CURR, RX_START);
	for (i = 0; i < 8; i++)
		out_byte(NE_BASE + 0x08 + i, 0x00);	/* MAR: no multicast */

	out_byte(NE_BASE + REG_CR, CR_START | CR_NODMA | CR_PAGE0);
	out_byte(NE_BASE + REG_TCR, 0x00);	/* out of loopback */
	out_byte(NE_BASE + REG_IMR, 0x01);	/* enable the RX interrupt */

	put_irq_handler(NET_IRQ, ne2000_irq);
	enable_irq(NET_IRQ);

	card_present = 1;
	return 0;
}

/*======================================================================*
			   ne2000_send
 *======================================================================*/
PUBLIC int ne2000_send(const u8 *frame, u16 len)
{
	u16 n;
	int spin;

	if (!card_present)
		return -1;
	if (len > NE_FRAME_MAX - 16)
		return -1;

	n = len < 60 ? 60 : len;		/* minimum ethernet frame */
	if (n & 1)
		n++;

	disable_irq(NET_IRQ);			/* SRAM is shared with the IRQ */
	remote_write(TX_START << 8, n, frame);
	out_byte(NE_BASE + REG_TBCR0, n & 0xFF);
	out_byte(NE_BASE + REG_TBCR1, n >> 8);
	out_byte(NE_BASE + REG_TPSR, TX_START);
	out_byte(NE_BASE + REG_CR, CR_START | CR_TXP | CR_PAGE0);

	for (spin = 0; spin < 2000000; spin++) {
		u8 isr = in_byte(NE_BASE + REG_ISR);

		if (isr & (ISR_PTX | ISR_TXE)) {
			out_byte(NE_BASE + REG_ISR, isr & (ISR_PTX | ISR_TXE));
			break;
		}
	}
	enable_irq(NET_IRQ);
	return 0;
}

/*======================================================================*
			   ne2000_recv
 *======================================================================*/
PUBLIC int ne2000_recv(u8 *buf)
{
	u16 len;

	if (sw_tail == sw_head)
		return -1;
	len = sw_len[sw_tail];
	memcpy(buf, sw_ring[sw_tail], len);
	sw_tail = (sw_tail + 1) % SW_RING;
	return len;
}
