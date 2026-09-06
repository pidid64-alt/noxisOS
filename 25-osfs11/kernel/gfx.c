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
#include "vga.h"


/* Frame pacing: ~18 FPS, i.e. advance one frame every few ticks. */
#define GFX_FPS		18
#define GFX_TICKS_PER_FRAME	6	/* ~16.7 fps at HZ=100 */

/* Demo phases, in ticks (HZ=100). */
#define GFX_PATTERN_TICKS	200	/* ~2 s of static test patterns   */
#define GFX_BALL_MAX_TICKS	1000	/* ~10 s fallback cap for the ball */


/* ---- drawing primitives (all operate on the double buffer) ----------------- */
PRIVATE void gfx_clear(u8 color);
PRIVATE void gfx_putpixel(int x, int y, u8 color);
PRIVATE void gfx_hline(int x1, int x2, int y, u8 color);
PRIVATE void gfx_vline(int x, int y1, int y2, u8 color);
PRIVATE void gfx_fill_rect(int x1, int y1, int x2, int y2, u8 color);
PRIVATE void gfx_circle(int cx, int cy, int r, u8 color);
PRIVATE void gfx_fill_circle(int cx, int cy, int r, u8 color);

/* ---- demo phases ----------------------------------------------------------- */
PRIVATE int gfx_run_demo(void);
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

	while (1) {
		send_recv(RECEIVE, ANY, &msg);

		int src = msg.source;
		assert(src != TASK_GFX);

		switch (msg.type) {
		case GFX_RUN: {
			int result = gfx_run_demo();

			reset_msg(&msg);
			msg.type = GFX_DONE;
			msg.RETVAL = result;
			send_recv(SEND, src, &msg);
			break;
		}
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
 * text mode 3 and return. VGA state is owned by the shared driver.
 *****************************************************************************/
PRIVATE int gfx_run_demo(void)
{
	int start = get_ticks();

	if (vga_enter_graphics() != 0)
		return -1;
	gfx_poll_esc(); /* discard ESC left over from the text shell */

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

	vga_leave_graphics();
	return 0;
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
	memcpy((void *)GFX_FB_BASE, vga_framebuffer, GFX_FB_BYTES);
}


/*============================================================================
 *  Drawing primitives (double buffer)
 *============================================================================*/

PRIVATE void gfx_clear(u8 color)
{
	memset(vga_framebuffer, color, GFX_FB_BYTES);
}

PRIVATE void gfx_putpixel(int x, int y, u8 color)
{
	if (x < 0 || x >= GFX_FB_W || y < 0 || y >= GFX_FB_H)
		return;
	vga_framebuffer[y * GFX_FB_W + x] = color;
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
		vga_framebuffer[y * GFX_FB_W + x] = color;
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
		vga_framebuffer[y * GFX_FB_W + x] = color;
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
			vga_framebuffer[y * GFX_FB_W + x] = color;
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
			vga_framebuffer[y * GFX_FB_W + x] = (x / 20) % 16;

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
