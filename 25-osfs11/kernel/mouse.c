/*************************************************************************//**
 *****************************************************************************
 * @file   mouse.c
 * @brief  PS/2 Mouse driver for noxisOS
 *
 * Implements PS/2 mouse protocol support with IRQ12 handling.
 * Tracks mouse movement and button state for the window manager.
 *
 * @author noxisOS
 * @date   2026-08-28
 *****************************************************************************
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

/* PS/2 Mouse constants */
#define MOUSE_IRQ       12
#define MOUSE_PORT      0x60
#define MOUSE_STATUS    0x64
#define MOUSE_ABIT      0x02
#define MOUSE_BBIT      0x01
#define MOUSE_WRITE     0xD4
#define MOUSE_V_BIT     0x08

/* Mouse commands */
#define MOUSE_CMD_RESET         0xFF
#define MOUSE_CMD_ENABLE        0xF4
#define MOUSE_CMD_SET_SAMPLE    0xF3
#define MOUSE_CMD_SET_REMOTE    0xF0
#define MOUSE_CMD_GET_ID        0xF2
#define MOUSE_CMD_SET_STREAM    0xEA
#define MOUSE_CMD_SET_DEFAULTS  0xF6

/* Mouse state */
PRIVATE int mouse_x = GFX_FB_W / 2;
PRIVATE int mouse_y = GFX_FB_H / 2;
PRIVATE int mouse_buttons = 0;
PRIVATE u8 mouse_cycle = 0;
PRIVATE u8 mouse_packet[3];

/* Forward declarations */
PRIVATE void mouse_wait(u8 type);
PRIVATE void mouse_write(u8 data);
PRIVATE u8 mouse_read(void);

/*****************************************************************************
 *                                mouse_wait
 *****************************************************************************
 * Wait for mouse controller to be ready for read/write.
 * type: 0 = output buffer, 1 = input buffer
 *****************************************************************************/
PRIVATE void mouse_wait(u8 type)
{
	u32 timeout = 100000;
	if (type == 0) {
		while (timeout--) {
			if ((in_byte(MOUSE_STATUS) & MOUSE_BBIT) == 1)
				return;
		}
	} else {
		while (timeout--) {
			if ((in_byte(MOUSE_STATUS) & MOUSE_ABIT) == 0)
				return;
		}
	}
}

/*****************************************************************************
 *                                mouse_write
 *****************************************************************************
 * Write a byte to the mouse controller.
 *****************************************************************************/
PRIVATE void mouse_write(u8 data)
{
	mouse_wait(1);
	out_byte(MOUSE_STATUS, MOUSE_WRITE);
	mouse_wait(1);
	out_byte(MOUSE_PORT, data);
}

/*****************************************************************************
 *                                mouse_read
 *****************************************************************************
 * Read a byte from the mouse controller.
 *****************************************************************************/
PRIVATE u8 mouse_read(void)
{
	mouse_wait(0);
	return in_byte(MOUSE_PORT);
}

/*****************************************************************************
 *                                mouse_init
 *****************************************************************************
 * Initialize the PS/2 mouse hardware.
 * Called during kernel initialization.
 *****************************************************************************/
PUBLIC void mouse_init(void)
{
	u8 status;

	/* Enable the auxiliary mouse device */
	mouse_wait(1);
	out_byte(MOUSE_STATUS, 0xA8);

	/* Enable interrupts */
	mouse_wait(1);
	out_byte(MOUSE_STATUS, 0x20);
	mouse_wait(0);
	status = (in_byte(MOUSE_PORT) | 2);
	mouse_wait(1);
	out_byte(MOUSE_STATUS, 0x60);
	mouse_wait(1);
	out_byte(MOUSE_PORT, status);

	/* Set defaults */
	mouse_write(MOUSE_CMD_SET_DEFAULTS);
	mouse_read(); /* ACK */

	/* Enable data reporting */
	mouse_write(MOUSE_CMD_ENABLE);
	mouse_read(); /* ACK */

	/* Reset packet state */
	mouse_cycle = 0;

	/* Register IRQ handler */
	put_irq_handler(MOUSE_IRQ, mouse_handler);
	enable_irq(MOUSE_IRQ);

	disp_str("PS/2 Mouse initialized\n");
}

/*****************************************************************************
 *                                mouse_handler
 *****************************************************************************
 * IRQ12 handler for PS/2 mouse.
 * Collects 3-byte packets and updates mouse state.
 *****************************************************************************/
PUBLIC void mouse_handler(int irq)
{
	u8 status = in_byte(MOUSE_STATUS);

	/* Check if data is from mouse */
	if (!(status & 0x20))
		return;

	u8 data = in_byte(MOUSE_PORT);

	/* Build 3-byte packet */
	switch (mouse_cycle) {
	case 0:
		/* First byte: must have bit 3 set (always 1 flag) */
		if (data & MOUSE_V_BIT) {
			mouse_packet[0] = data;
			mouse_cycle = 1;
		}
		break;
	case 1:
		mouse_packet[1] = data;
		mouse_cycle = 2;
		break;
	case 2:
		mouse_packet[2] = data;
		mouse_cycle = 0;

		/* Process complete packet */
		mouse_buttons = mouse_packet[0] & 0x07;

		/* Extract movement (with sign extension) */
		int dx = mouse_packet[1];
		int dy = mouse_packet[2];

		/* Sign extend if needed */
		if (mouse_packet[0] & 0x10)
			dx |= 0xFFFFFF00;
		if (mouse_packet[0] & 0x20)
			dy |= 0xFFFFFF00;

		/* Update position (Y is inverted) */
		mouse_x += dx;
		mouse_y -= dy;

		/* Clamp to screen bounds */
		if (mouse_x < 0) mouse_x = 0;
		if (mouse_x >= GFX_FB_W) mouse_x = GFX_FB_W - 1;
		if (mouse_y < 0) mouse_y = 0;
		if (mouse_y >= GFX_FB_H) mouse_y = GFX_FB_H - 1;
		break;
	}
}

/*****************************************************************************
 *                                mouse_get_state
 *****************************************************************************
 * Get current mouse position and button state.
 *****************************************************************************/
PUBLIC void mouse_get_state(int *x, int *y, int *buttons)
{
	*x = mouse_x;
	*y = mouse_y;
	*buttons = mouse_buttons;
}
