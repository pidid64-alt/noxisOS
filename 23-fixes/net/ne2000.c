/* NE2000 (DP8390) ISA NIC driver.
 *
 * Layout of the card's 16 KiB SRAM, in 256-byte pages:
 *
 *   page 0x40..0x45   transmit staging buffer (6 pages = 1536 bytes)
 *   page 0x46..0x7F   receive ring written by the card (58 pages)
 *
 * The card appends each received packet with a 4-byte header
 * {status, next page, count lo, count hi} and bumps CURR (page1). The host
 * reads packets from BNRY+1 onwards and moves BNRY up behind them.
 */
#include "ne2000.h"
#include "net.h"
#include "../cpu/isr.h"
#include "../cpu/ports.h"
#include "../cpu/timer.h"
#include "../libc/mem.h"
#include "../libc/function.h"

/* Register file offsets from the card base (page 0 unless noted). */
#define REG_CR     0x00
#define REG_PSTART 0x01   /* w: page0   r: CLDA0 */
#define REG_PSTOP  0x02   /* w: page0   r: CLDA1 */
#define REG_BNRY   0x03
#define REG_TPSR   0x04   /* w: page0   r: TSR   */
#define REG_TBCR0  0x05
#define REG_TBCR1  0x06
#define REG_ISR    0x07
#define REG_RSAR0  0x08
#define REG_RSAR1  0x09
#define REG_RBCR0  0x0A
#define REG_RBCR1  0x0B
#define REG_RCR    0x0C   /* w: page0   r: RSR   */
#define REG_TCR    0x0D   /* w: page0   r: CNTR0 */
#define REG_DCR    0x0E   /* w: page0   r: CNTR1 */
#define REG_IMR    0x0F   /* w: page0   r: CNTR2 */
#define REG_CURR   0x07   /* page1 */
#define REG_PAR    0x01   /* page1: PAR0..PAR5 at 0x01..0x06 */
#define REG_DATA   0x10
#define REG_RESET  0x1F

/* CR command bits: [7:6]=page, [5:3]=RD2..RD0, [2]=TXP, [1]=STRT, [0]=STP. */
#define CR_PAGE0    0x00
#define CR_PAGE1    0x40
#define CR_NODMA    0x20   /* RD=100: no/abort remote DMA */
#define CR_RWRITE   0x10   /* RD=010 */
#define CR_RREAD    0x08   /* RD=001 */
#define CR_TXP      0x04   /* transmit packet */
#define CR_STOP     0x01
#define CR_START    0x02

/* ISR bits (write 1 to clear, except RST). */
#define ISR_PRX   0x01   /* packet received */
#define ISR_PTX   0x02   /* packet transmitted */
#define ISR_TXE   0x08   /* transmit error */
#define ISR_RDC   0x40   /* remote DMA complete */
#define ISR_RST   0x80

#define TX_START  0x40
#define RX_START  0x46
#define RX_STOP   0x80

/* Software receive ring filled by the IRQ handler (single producer) and
 * drained by the protocol stack (single consumer), so no locking is needed
 * beyond volatile indices. */
#define SW_RING 8
static uint8_t  sw_ring[SW_RING][NE_FRAME_MAX];
static uint16_t sw_len[SW_RING];
static volatile uint8_t sw_head;   /* writer position (IRQ) */
static volatile uint8_t sw_tail;   /* reader position (task) */

static int card_present;
static uint8_t card_mac[6];

static void reg_out(uint8_t reg, uint8_t val) {
    port_byte_out(NE2000_BASE + reg, val);
}

static uint8_t reg_in(uint8_t reg) {
    return port_byte_in(NE2000_BASE + reg);
}

static void cr_out(uint8_t val) {
    reg_out(REG_CR, val);
}

/* Rough delay: a few port reads are plenty for the reset pulse on QEMU
 * and on real hardware alike. */
static void nic_delay(void) {
    volatile int i;
    for (i = 0; i < 100; i++) (void)reg_in(REG_RESET);
}

/* The NE2000 data port is 16-bit: the card moves one word per access and
 * byte reads come out mangled (each byte duplicated), so every transfer
 * between host and card SRAM goes through port_word_in/out and lengths are
 * padded to an even byte count. */

/* Move `len` bytes (padded to even) from the card SRAM at `addr` into buf.
 * Leaves the card started, page0, no-DMA. Bounded waits only: this runs
 * from IRQ context too, and QEMU completes remote DMA instantaneously. */
static void remote_read(uint16_t addr, uint16_t len, uint8_t *buf) {
    uint16_t words = (len + 1) / 2;
    uint16_t i;

    cr_out(CR_START | CR_NODMA | CR_PAGE0);
    reg_out(REG_RSAR0, addr & 0xFF);
    reg_out(REG_RSAR1, addr >> 8);
    reg_out(REG_RBCR0, (words * 2) & 0xFF);
    reg_out(REG_RBCR1, (words * 2) >> 8);
    cr_out(CR_START | CR_RREAD);
    for (i = 0; i < words; i++) {
        uint16_t w = port_word_in(NE2000_BASE + REG_DATA);
        buf[i * 2] = (uint8_t)(w & 0xFF);
        if (i * 2 + 1 < len) buf[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    /* Ack the remote-DMA-complete bit if it shows up shortly. */
    {
        int spin;
        for (spin = 0; spin < 100000; spin++)
            if (reg_in(REG_ISR) & ISR_RDC) break;
        reg_out(REG_ISR, ISR_RDC);
    }
}

/* Write buf (len bytes, padded to even with zeros) to card SRAM at addr. */
static void remote_write(uint16_t addr, uint16_t len, const uint8_t *buf) {
    uint16_t words = (len + 1) / 2;
    uint16_t i;

    cr_out(CR_START | CR_NODMA | CR_PAGE0);
    reg_out(REG_RSAR0, addr & 0xFF);
    reg_out(REG_RSAR1, addr >> 8);
    reg_out(REG_RBCR0, (words * 2) & 0xFF);
    reg_out(REG_RBCR1, (words * 2) >> 8);
    cr_out(CR_START | CR_RWRITE);
    for (i = 0; i < words; i++) {
        uint16_t lo = i * 2 < len ? buf[i * 2] : 0;
        uint16_t hi = i * 2 + 1 < len ? buf[i * 2 + 1] : 0;
        port_word_out(NE2000_BASE + REG_DATA, (uint16_t)(lo | (hi << 8)));
    }
    {
        int spin;
        for (spin = 0; spin < 100000; spin++)
            if (reg_in(REG_ISR) & ISR_RDC) break;
        reg_out(REG_ISR, ISR_RDC);
    }
}

/* Copy every packet currently in the card ring into the software ring. */
static void drain_card_ring(void) {
    uint8_t bnry = reg_in(REG_BNRY);
    int n;

    for (n = 0; n < 32; n++) {
        uint8_t pkt = bnry + 1;
        uint8_t hdr[4];
        uint8_t next, curr;
        uint16_t count, len;

        if (pkt >= RX_STOP) pkt = RX_START;

        /* CURR lives on page 1. */
        cr_out(CR_START | CR_NODMA | CR_PAGE1);
        curr = reg_in(REG_CURR);
        cr_out(CR_START | CR_NODMA | CR_PAGE0);

        if (pkt == curr) break;             /* ring empty */

        remote_read(pkt << 8, 4, hdr);
        next  = hdr[1];
        count = (uint16_t)(hdr[2] | (hdr[3] << 8));
        if (count < 4) count = 4;
        len = count - 4;                    /* strip the card's 4-byte header */
        if (len > NE_FRAME_MAX) len = NE_FRAME_MAX;

        if (next < RX_START || next >= RX_STOP) break;   /* corrupted ring */

        {
            uint8_t push = (uint8_t)((sw_head + 1) % SW_RING);
            if (push == sw_tail) {
                /* Ring full: drop the frame, but still advance BNRY so the
                 * card does not stall. */
                net_stat_dropped();
            } else {
                remote_read((uint16_t)((pkt << 8) + 4), len, sw_ring[sw_head]);
                sw_len[sw_head] = len;
                sw_head = push;
                net_stat_rx();
            }
        }

        bnry = (uint8_t)(next - 1);
        if (bnry < RX_START) bnry = RX_STOP - 1;
        reg_out(REG_BNRY, bnry);
    }
}

static void ne2000_irq(registers_t *r) {
    uint8_t isr;
    UNUSED(r);
    isr = reg_in(REG_ISR);
    if (isr & ISR_PRX) drain_card_ring();
    /* Acknowledge everything that fired except RST (bit 7). */
    reg_out(REG_ISR, isr & 0x7F);
}

int ne2000_init(void) {
    uint8_t prom[16];
    int i;

    /* Reset pulse, then let the card settle. */
    reg_out(REG_RESET, 0x00);
    nic_delay();

    /* Stop the card and select page 0 for programming. */
    cr_out(CR_NODMA | CR_PAGE0);

    reg_out(REG_DCR, 0x49);    /* word-wide remote DMA, normal operation */
    reg_out(REG_RBCR0, 0x00);
    reg_out(REG_RBCR1, 0x00);
    reg_out(REG_RCR, 0x04);    /* accept broadcast, reject nothing else */
    reg_out(REG_TCR, 0x02);    /* internal loopback while configuring */
    reg_out(REG_PSTART, RX_START);
    reg_out(REG_PSTOP, RX_STOP);
    reg_out(REG_BNRY, RX_START - 1);
    reg_out(REG_TPSR, TX_START);
    reg_out(REG_ISR, 0xFF);    /* clear all pending interrupts */

    /* The station address sits in the first 16 bytes of the address PROM,
     * readable through remote DMA from SRAM address 0. On an empty port the
     * reads come back as 0xFF, which doubles as card detection. */
    remote_read(0x0000, 16, prom);
    {
        int all_ff = 1, all_00 = 1;
        for (i = 0; i < 6; i++) {
            if (prom[i] != 0xFF) all_ff = 0;
            if (prom[i] != 0x00) all_00 = 0;
        }
        if (all_ff || all_00) return -1;    /* no card at NE2000_BASE */
    }
    /* The station address lives in the PROM's even bytes: the odd bytes
     * mirror their neighbours (QEMU builds the PROM word-wise), so taking
     * prom[2i] yields the real 52:54:00:12:34:56 instead of the doubled
     * 52:52:54:54:00:00. */
    for (i = 0; i < 6; i++) card_mac[i] = prom[i * 2];

    /* Page 1: the MAC filter and the ring write cursor. */
    cr_out(CR_NODMA | CR_PAGE1);
    for (i = 0; i < 6; i++) reg_out(REG_PAR + i, card_mac[i]);
    reg_out(REG_CURR, RX_START);
    for (i = 0; i < 8; i++) reg_out(0x08 + i, 0x00);   /* MAR: no multicast */

    /* Start the card, leave loopback, enable the receive interrupt. */
    cr_out(CR_START | CR_NODMA | CR_PAGE0);
    reg_out(REG_TCR, 0x00);
    reg_out(REG_IMR, 0x01);

    register_interrupt_handler(32 + NE2000_IRQ, ne2000_irq);
    card_present = 1;
    return 0;
}

void ne2000_get_mac(uint8_t mac[6]) {
    int i;
    for (i = 0; i < 6; i++) mac[i] = card_mac[i];
}

int ne2000_send(const uint8_t *frame, uint16_t len) {
    uint16_t n;
    int spin;

    if (!card_present) return -1;
    if (len > NE_FRAME_MAX - 22) return -1;

    n = len < 60 ? 60 : len;    /* minimum ethernet frame size */
    if (n & 1) n++;             /* word transfers move whole words */

    /* The card's SRAM is a shared resource with the IRQ handler (which does
     * its own remote DMA reads); keep it exclusive for the whole transfer.
     * All callers run with interrupts enabled, so a plain cli/sti pair is a
     * sufficient critical section. */
    asm volatile("cli");
    remote_write(TX_START << 8, n, frame);

    reg_out(REG_TBCR0, n & 0xFF);
    reg_out(REG_TBCR1, n >> 8);
    reg_out(REG_TPSR, TX_START);
    cr_out(CR_START | CR_TXP | CR_PAGE0);       /* fire */

    /* Wait for transmit done / error so the staging buffer can be reused. */
    for (spin = 0; spin < 2000000; spin++) {
        uint8_t isr = reg_in(REG_ISR);
        if (isr & (ISR_PTX | ISR_TXE)) {
            reg_out(REG_ISR, isr & (ISR_PTX | ISR_TXE));
            break;
        }
    }
    asm volatile("sti");

    net_stat_tx();
    return 0;
}

int ne2000_recv(uint8_t *buf) {
    uint16_t len;
    if (sw_tail == sw_head) return -1;
    len = sw_len[sw_tail];
    memory_copy(sw_ring[sw_tail], buf, len);
    sw_tail = (uint8_t)((sw_tail + 1) % SW_RING);
    return len;
}
