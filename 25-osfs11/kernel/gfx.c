/*************************************************************************//**
 *****************************************************************************
 * @file   gfx.c
 * @brief  TASK_GFX -- a framebuffer graphics demo (VGA mode 13h).
 *
 * This is the first step towards a graphical environment: it proves the
 * pixel pipeline (switch VGA to a graphics mode without BIOS, drive the
 * linear framebuffer, draw primitives, run a stable animation loop).
 *
 * The task lives in an infinite message loop. A user process (the `demo'
 * command) sends a GFX_RUN message; TASK_GFX switches to mode 13h, renders
 * test patterns (~2 s) then a bouncing-ball animation, polls TTY for ESC,
 * and finally switches back to text mode 3 before replying GFX_DONE.
 *
 * VGA is programmed with register tables (no BIOS, since we are in
 * protected mode). The text-mode registers are saved on entry and restored
 * on exit so the console keeps working after the demo.
 *
 * @author noxisOS
 * @date   2026-08-27
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
#include "keyboard.h"
#include "proto.h"


/* Number of registers we save/restore for the VGA state.
 * misc(1) + sequencer(5) + CRTC(25) + graphics controller(9) + attr(21) */
#define GFX_N_MISC	1
#define GFX_N_SEQ	5
#define GFX_N_CRTC	25
#define GFX_N_GC	9
#define GFX_N_AC	21
#define GFX_STATE_SZ	(GFX_N_MISC + GFX_N_SEQ + GFX_N_CRTC + GFX_N_GC + GFX_N_AC)


/* Frame pacing: ~18 FPS, i.e. advance one frame every few ticks. */
#define GFX_FPS		18
#define GFX_TICKS_PER_FRAME	6	/* ~16.7 fps at HZ=100 */

/* Demo phases, in ticks (HZ=100). */
#define GFX_PATTERN_TICKS	200	/* ~2 s of static test patterns   */
#define GFX_BALL_MAX_TICKS	1000	/* ~10 s fallback cap for the ball */


/* Double buffer: one byte per pixel, 320x200 = 64000 bytes (BSS of this task). */
static u8 gfx_buf[GFX_FB_BYTES];


/*****************************************************************************
 *                                mode 13h register table
 *****************************************************************************
 * Standard VGA mode 13h (320x200, 256 colors, linear framebuffer @0xA0000).
 * Order: MISC, SEQ(5), CRTC(25), GC(9), AC(21).
 *****************************************************************************/
static const u8 gfx_mode13h[GFX_STATE_SZ] = {
	/* MISC output */
	0x63,
	/* Sequencer */
	0x03, 0x01, 0x0F, 0x00, 0x0E,
	/* CRTC */
	0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28,
	0x40, 0x96, 0xB9, 0xA3, 0xFF,
	/* Graphics Controller */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x00, 0xFF,
	/* Attribute Controller */
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
	0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x41, 0x00, 0x0F, 0x00, 0x00
};


/* ---- VGA state save / restore ---------------------------------------------- */

PRIVATE void vga_save_state(u8 * s);
PRIVATE void vga_restore_state(const u8 * s);
PRIVATE void vga_write_mode13(void);

/* ---- drawing primitives (all operate on the double buffer) ----------------- */
PRIVATE void gfx_clear(u8 color);
PRIVATE void gfx_putpixel(int x, int y, u8 color);
PRIVATE void gfx_hline(int x1, int x2, int y, u8 color);
PRIVATE void gfx_vline(int x, int y1, int y2, u8 color);
PRIVATE void gfx_fill_rect(int x1, int y1, int x2, int y2, u8 color);
PRIVATE void gfx_circle(int cx, int cy, int r, u8 color);
PRIVATE void gfx_fill_circle(int cx, int cy, int r, u8 color);

/* ---- demo phases ----------------------------------------------------------- */
PRIVATE void gfx_run_demo(u8 * saved);
PRIVATE void gfx_draw_pattern(void);
PRIVATE void gfx_draw_ball(int bx, int by, int r, int frame);
PRIVATE void gfx_present(void);
PRIVATE void gfx_sync_frame(int * last);
PRIVATE int  gfx_poll_esc(void);


/*****************************************************************************
 *                                task_gfx
 *****************************************************************************
 * <Ring 1> Main loop of TASK_GFX.
 *****************************************************************************/
PUBLIC void task_gfx()
{
	MESSAGE msg;
	u8 saved[GFX_STATE_SZ];

	while (1) {
		send_recv(RECEIVE, ANY, &msg);

		int src = msg.source;
		assert(src != TASK_GFX);

		switch (msg.type) {
		case GFX_RUN:
			gfx_run_demo(saved);

			reset_msg(&msg);
			msg.type = GFX_DONE;
			send_recv(SEND, src, &msg);
			break;
		default:
			dump_msg("GFX::unknown msg", &msg);
			break;
		}
	}
}


/*****************************************************************************
 *                                gfx_run_demo
 *****************************************************************************
 * Save text-mode VGA state, switch to mode 13h, run the demo, then restore
 * text mode 3 and return. Called with the saved-state buffer.
 *****************************************************************************/
PRIVATE void gfx_run_demo(u8 * saved)
{
	int start = get_ticks();

	/* Switch to graphics mode. */
	vga_save_state(saved);
	disable_int();
	vga_write_mode13();
	enable_int();

	/* ---- Phase 1: static test patterns (~2 s) ---- */
	int last = start;
	int esc = 0;
	while ((get_ticks() - start) < GFX_PATTERN_TICKS) {
		gfx_draw_pattern();
		gfx_present();

		if (gfx_poll_esc()) {
			esc = 1;
			break;
		}
		gfx_sync_frame(&last);
	}

	/* ---- Phase 2: bouncing-ball animation (until ESC or ~10 s) ---- */
	if (!esc) {
		int bx = 40, by = 40, dx = 3, dy = 2;
		int r = 12, frame = 0;

		while ((get_ticks() - start) <
		       (GFX_PATTERN_TICKS + GFX_BALL_MAX_TICKS)) {
			frame++;

			bx += dx;
			by += dy;
			if (bx < r)       { bx = r;       dx = -dx; }
			else if (bx > GFX_FB_W - r) { bx = GFX_FB_W - r;  dx = -dx; }
			if (by < r)       { by = r;       dy = -dy; }
			else if (by > GFX_FB_H - r) { by = GFX_FB_H - r;  dy = -dy; }

			gfx_draw_ball(bx, by, r, frame);
			gfx_present();

			if (gfx_poll_esc())
				break;
			gfx_sync_frame(&last);
		}
	}

	/* Restore the text console. */
	disable_int();
	vga_restore_state(saved);
	enable_int();
}


/*****************************************************************************
 *                                gfx_poll_esc
 *****************************************************************************
 * Ask TTY (which owns the keyboard) whether ESC was pressed since the last
 * poll. Returns non-zero if ESC is pending.
 *****************************************************************************/
PRIVATE int gfx_poll_esc(void)
{
	MESSAGE msg;
	reset_msg(&msg);
	msg.type = TTY_POLL_KEY;
	send_recv(BOTH, TASK_TTY, &msg);
	return msg.RETVAL;
}


/*****************************************************************************
 *                                gfx_sync_frame
 *****************************************************************************
 * Busy-wait until roughly one frame's worth of ticks has elapsed since the
 * last synchronisation point. Keeps the animation at a stable ~18 FPS.
 *****************************************************************************/
PRIVATE void gfx_sync_frame(int * last)
{
	int target = *last + GFX_TICKS_PER_FRAME;
	while (get_ticks() < target) { /* spin; clock IRQs keep firing (IF=1) */ }
	*last = target;
}


/*****************************************************************************
 *                                gfx_present
 *****************************************************************************
 * Copy the double buffer into the linear framebuffer @0xA0000.
 *****************************************************************************/
PRIVATE void gfx_present(void)
{
	memcpy((void *)GFX_FB_BASE, gfx_buf, GFX_FB_BYTES);
}


/*============================================================================
 *  VGA programming (no BIOS)
 *============================================================================*/

/*****************************************************************************
 *                                vga_save_state
 *****************************************************************************/
PRIVATE void vga_save_state(u8 * s)
{
	int i;

	/* Misc Output Register (read port). */
	s[0] = in_byte(VGA_MISC_R);

	/* Sequencer. */
	for (i = 0; i < GFX_N_SEQ; i++) {
		out_byte(VGA_SEQ_ADDR, i);
		s[GFX_N_MISC + i] = in_byte(VGA_SEQ_DATA);
	}

	/* CRTC. */
	for (i = 0; i < GFX_N_CRTC; i++) {
		out_byte(VGA_CRTC_ADDR, i);
		s[GFX_N_MISC + GFX_N_SEQ + i] = in_byte(VGA_CRTC_DATA);
	}

	/* Graphics Controller. */
	for (i = 0; i < GFX_N_GC; i++) {
		out_byte(VGA_GC_ADDR, i);
		s[GFX_N_MISC + GFX_N_SEQ + GFX_N_CRTC + i] = in_byte(VGA_GC_DATA);
	}

	/* Attribute Controller: reset the address/data flip-flop, then read. */
	in_byte(VGA_AC_RDY);
	for (i = 0; i < GFX_N_AC; i++) {
		out_byte(VGA_AC_ADDR, i);
		s[GFX_N_MISC + GFX_N_SEQ + GFX_N_CRTC + GFX_N_GC + i] =
			in_byte(0x3C1);
	}
}

/*****************************************************************************
 *                                vga_restore_state
 *****************************************************************************/
PRIVATE void vga_restore_state(const u8 * s)
{
	int i;

	out_byte(VGA_MISC_W, s[0]);

	for (i = 0; i < GFX_N_SEQ; i++) {
		out_byte(VGA_SEQ_ADDR, i);
		out_byte(VGA_SEQ_DATA, s[GFX_N_MISC + i]);
	}

	for (i = 0; i < GFX_N_CRTC; i++) {
		out_byte(VGA_CRTC_ADDR, i);
		out_byte(VGA_CRTC_DATA, s[GFX_N_MISC + GFX_N_SEQ + i]);
	}

	for (i = 0; i < GFX_N_GC; i++) {
		out_byte(VGA_GC_ADDR, i);
		out_byte(VGA_GC_DATA, s[GFX_N_MISC + GFX_N_SEQ + GFX_N_CRTC + i]);
	}

	/* Attribute Controller: reset flip-flop, then write (addr, data) pairs. */
	in_byte(VGA_AC_RDY);
	for (i = 0; i < GFX_N_AC; i++) {
		out_byte(VGA_AC_ADDR, i);
		out_byte(VGA_AC_ADDR,
			 s[GFX_N_MISC + GFX_N_SEQ + GFX_N_CRTC + GFX_N_GC + i]);
	}
}

/*****************************************************************************
 *                                vga_write_mode13
 *****************************************************************************
 * Program the VGA into mode 13h from a register table. The caller should
 * disable interrupts around this routine.
 *****************************************************************************/
PRIVATE void vga_write_mode13(void)
{
	const u8 * r = gfx_mode13h;
	int i;

	/* Misc Output Register. */
	out_byte(VGA_MISC_W, *r++);

	/* Sequencer. */
	for (i = 0; i < GFX_N_SEQ; i++) {
		out_byte(VGA_SEQ_ADDR, i);
		out_byte(VGA_SEQ_DATA, *r++);
	}

	/* CRTC. */
	for (i = 0; i < GFX_N_CRTC; i++) {
		out_byte(VGA_CRTC_ADDR, i);
		out_byte(VGA_CRTC_DATA, *r++);
	}

	/* Graphics Controller. */
	for (i = 0; i < GFX_N_GC; i++) {
		out_byte(VGA_GC_ADDR, i);
		out_byte(VGA_GC_DATA, *r++);
	}

	/* Attribute Controller: reset flip-flop, then write (addr, data) pairs. */
	in_byte(VGA_AC_RDY);
	for (i = 0; i < GFX_N_AC; i++) {
		out_byte(VGA_AC_ADDR, i);
		out_byte(VGA_AC_ADDR, *r++);
	}
}


/*============================================================================
 *  Drawing primitives (double buffer)
 *============================================================================*/

PRIVATE void gfx_clear(u8 color)
{
	memset(gfx_buf, color, GFX_FB_BYTES);
}

PRIVATE void gfx_putpixel(int x, int y, u8 color)
{
	if (x < 0 || x >= GFX_FB_W || y < 0 || y >= GFX_FB_H)
		return;
	gfx_buf[y * GFX_FB_W + x] = color;
}

PRIVATE void gfx_hline(int x1, int x2, int y, u8 color)
{
	int x;
	if (y < 0 || y >= GFX_FB_H)
		return;
	if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
	if (x1 < 0) x1 = 0;
	if (x2 >= GFX_FB_W) x2 = GFX_FB_W - 1;
	for (x = x1; x <= x2; x++)
		gfx_buf[y * GFX_FB_W + x] = color;
}

PRIVATE void gfx_vline(int x, int y1, int y2, u8 color)
{
	int y;
	if (x < 0 || x >= GFX_FB_W)
		return;
	if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
	if (y1 < 0) y1 = 0;
	if (y2 >= GFX_FB_H) y2 = GFX_FB_H - 1;
	for (y = y1; y <= y2; y++)
		gfx_buf[y * GFX_FB_W + x] = color;
}

PRIVATE void gfx_fill_rect(int x1, int y1, int x2, int y2, u8 color)
{
	int x, y;
	if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
	if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
	if (x1 < 0) x1 = 0;
	if (y1 < 0) y1 = 0;
	if (x2 >= GFX_FB_W) x2 = GFX_FB_W - 1;
	if (y2 >= GFX_FB_H) y2 = GFX_FB_H - 1;
	for (y = y1; y <= y2; y++)
		for (x = x1; x <= x2; x++)
			gfx_buf[y * GFX_FB_W + x] = color;
}

PRIVATE void gfx_circle(int cx, int cy, int r, u8 color)
{
	/* Midpoint circle algorithm. */
	int x = r, y = 0;
	int err = 1 - r;

	while (x >= y) {
		gfx_putpixel(cx + x, cy + y, color);
		gfx_putpixel(cx + y, cy + x, color);
		gfx_putpixel(cx - y, cy + x, color);
		gfx_putpixel(cx - x, cy + y, color);
		gfx_putpixel(cx - x, cy - y, color);
		gfx_putpixel(cx - y, cy - x, color);
		gfx_putpixel(cx + y, cy - x, color);
		gfx_putpixel(cx + x, cy - y, color);
		y++;
		if (err < 0)
			err += 2 * y + 1;
		else {
			x--;
			err += 2 * (y - x) + 1;
		}
	}
}

PRIVATE void gfx_fill_circle(int cx, int cy, int r, u8 color)
{
	int x, y;
	for (y = -r; y <= r; y++)
		for (x = -r; x <= r; x++)
			if (x * x + y * y <= r * r)
				gfx_putpixel(cx + x, cy + y, color);
}


/*============================================================================
 *  Demo rendering
 *============================================================================*/

/*****************************************************************************
 *                                gfx_draw_pattern
 *****************************************************************************
 * Test pattern: vertical colour bars across the whole screen, with a few
 * shapes drawn only in the upper band (y < 150) so the bottom band stays a
 * clean, verifiable bar pattern.
 *****************************************************************************/
PRIVATE void gfx_draw_pattern(void)
{
	int x, y;

	/* Vertical colour bars (16 bars, palette indices 0..15). */
	for (y = 0; y < GFX_FB_H; y++)
		for (x = 0; x < GFX_FB_W; x++)
			gfx_buf[y * GFX_FB_W + x] = (x / 20) % 16;

	/* Shapes in the upper band only (y < 150). */
	gfx_fill_circle(80,  65, 30, 15);	/* white disc   */
	gfx_fill_circle(160, 65, 30, 1);	/* blue disc    */
	gfx_fill_circle(240, 65, 30, 4);	/* red disc     */
	gfx_fill_rect(110, 100, 210, 140, 14);	/* yellow rect  */
}

/*****************************************************************************
 *                                gfx_draw_ball
 *****************************************************************************
 * Clear to a dark background, draw a border and a bouncing ball whose colour
 * cycles with the frame counter.
 *****************************************************************************/
PRIVATE void gfx_draw_ball(int bx, int by, int r, int frame)
{
	gfx_clear(0);			/* black background */

	/* Border rectangle in dark grey. */
	gfx_hline(0, GFX_FB_W - 1, 0, 8);
	gfx_hline(0, GFX_FB_W - 1, GFX_FB_H - 1, 8);
	gfx_vline(0, 0, GFX_FB_H - 1, 8);
	gfx_vline(GFX_FB_W - 1, 0, GFX_FB_H - 1, 8);

	/* Bouncing ball: colour cycles through 1..15. */
	u8 color = (u8)((frame % 15) + 1);
	gfx_fill_circle(bx, by, r, color);
	gfx_circle(bx, by, r, 15);	/* white outline */
}
