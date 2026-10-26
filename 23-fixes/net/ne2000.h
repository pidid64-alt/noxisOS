#ifndef NE2000_H
#define NE2000_H

#include <stdint.h>

/* Driver for the NE2000 (DP8390) ISA network card.
 *
 * QEMU wires it up like this (see the run targets in the Makefile):
 *
 *   -netdev user,id=net0 -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
 *
 * The card is programmed I/O: packets move between the host and the card's
 * 16 KiB internal SRAM through the "remote DMA" data port, so no host-side
 * busmaster DMA and no physical address restrictions are involved.
 *
 * Received frames are copied out of the card ring by the IRQ handler into a
 * small software ring; the protocol stack drains that ring from task context
 * via ne2000_recv(). */

#define NE2000_BASE   0x300
#define NE2000_IRQ    9          /* PIC line -> CPU vector 32+9 = 41 */

#define NE_FRAME_MAX  1536       /* 14 eth + 1500 payload + slack */

/* Detect the card, reset it, program the receive ring and hook the IRQ.
 * Returns 0 on success, -1 if no card answered at NE2000_BASE. */
int ne2000_init(void);

/* Copy the station address out of the card PROM (call after ne2000_init). */
void ne2000_get_mac(uint8_t mac[6]);

/* Transmit one ethernet frame. Pads to the 60-byte minimum. Returns 0,
 * or -1 if there is no card / the frame is too large. */
int ne2000_send(const uint8_t *frame, uint16_t len);

/* Pop the oldest queued received frame into buf (NE_FRAME_MAX bytes).
 * Returns its length, or -1 if the ring is empty. */
int ne2000_recv(uint8_t *buf);

#endif
