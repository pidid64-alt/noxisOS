/*
 * Unit test for the REAL kernel/gfx.c drawing primitives.
 *
 * gfx.c is #included so every PRIVATE/static function lives in this
 * translation unit and is directly callable. Kernel dependencies are
 * stubbed two ways:
 *
 *   1. This file mirrors, above the #include, every constant/type the
 *      real headers define (values cross-checked against include/sys/const.h
 *      and include/type.h).
 *   2. gfx.c's own #include "type.h" ... "proto.h" resolve to the empty
 *      guards in tests/stubs/ (pass -iquote tests/stubs; do NOT pass
 *      -I include, or the real headers collide with these mirrors).
 *
 * Unlike on real hardware, both the double buffer and the blit target are
 * normal BSS buffers, so the primitives and the framebuffer copy can run.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- mirrored kernel types/macros (see include/type.h) ------------------ */
typedef unsigned char	u8;
typedef unsigned short	u16;
typedef unsigned int	u32;

#define PUBLIC
#define PRIVATE	static

/* ---- mirrored constants (see include/sys/const.h) ----------------------- */
#define GFX_FB_W	320
#define GFX_FB_H	200
#define GFX_FB_BYTES	(GFX_FB_W * GFX_FB_H)

#define VGA_MISC_W	0x3C2
#define VGA_MISC_R	0x3CC
#define VGA_SEQ_ADDR	0x3C4
#define VGA_SEQ_DATA	0x3C5
#define VGA_CRTC_ADDR	0x3D4
#define VGA_CRTC_DATA	0x3D5
#define VGA_GC_ADDR	0x3CE
#define VGA_GC_DATA	0x3CF
#define VGA_AC_ADDR	0x3C0
#define VGA_AC_RDY	0x3DA

#define TASK_TTY	0
#define TASK_GFX	5
#define TASK_DESKTOP	6
#define ANY		1000		/* real: NR_TASKS+NR_PROCS+10 */
#define SEND		1
#define RECEIVE		2
#define BOTH		3

enum {
	GFX_RUN,	/* keep values self-consistent; nothing external depends on them */
	GFX_DONE,
	TTY_POLL_KEY,
	DESKTOP_START,
	DESKTOP_DONE,
};

/* MESSAGE: layout mirrors include/type.h (fields gfx.c touches). */
typedef struct {
	int source;
	int type;
	int RETVAL;
} MESSAGE;

/* ---- kernel function stubs (signatures match include/sys/proto.h) ------- */
static u8	fake_ports[0x10000];	/* in_byte/out_byte land here, unused below */
static int	g_ticks = 0;
static int	g_sr_result, g_sr_bad_reply;
static int	g_sr_func, g_sr_dest, g_sr_type;	/* send_recv capture */

int	get_ticks(void)			{ return g_ticks; }
void	send_recv(int func, int src_dest, MESSAGE *m)
{
	g_sr_func = func; g_sr_dest = src_dest; g_sr_type = m->type;
	m->type = g_sr_bad_reply ? -1 : (src_dest == TASK_DESKTOP ? DESKTOP_DONE : GFX_DONE);
	m->RETVAL = g_sr_result;
}
void	reset_msg(MESSAGE *m)		{ memset(m, 0, sizeof(*m)); }
void	out_byte(u32 port, u8 val)	{ fake_ports[port & 0xFFFF] = val; }
u8	in_byte(u32 port)		{ return fake_ports[port & 0xFFFF]; }
void	disable_int(void)		{}
void	enable_int(void)		{}
void	dump_msg(const char *title, MESSAGE *m) { (void)title; (void)m; }

static int failures = 0;

/* Real assert lives in include/stdio.h; gfx.c uses it once in task_gfx(). */
#define	assert(exp) \
	do { if (!(exp)) { printf("ASSERT FAILED: %s\n", #exp); failures++; } } while (0)

/* Real hardware keeps the double buffer in free RAM and blits it to the
 * VBE linear framebuffer. Here both are plain BSS buffers so the drawing
 * primitives and vga_blit() can be exercised. */
static u8	fb_store[GFX_FB_BYTES];	/* the double buffer */
u8 *vga_framebuffer = fb_store;

static u8	test_fb[GFX_FB_BYTES];	/* fake "hardware" target */
void vga_blit(void) { memcpy(test_fb, vga_framebuffer, GFX_FB_BYTES); }
int vga_enter_graphics(void) { return 0; }
void vga_leave_graphics(void) {}

/* Pull in the real implementations (all functions now in this TU). */
#include "../kernel/gfx.c"
#include "../lib/gfx.c"
#include "../lib/desktop.c"

/* ---------------- test scaffolding -------------------------------------- */
static int checks = 0;
#define CHECK(c, m) do { checks++; if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

int main(void)
{
	/* === Phase 1: static test pattern === */
	gfx_draw_pattern();

	int bad = 0;
	for (int y = 141; y < GFX_FB_H; y++)	/* shapes end at y=140 */
		for (int x = 0; x < GFX_FB_W; x++)
			if (vga_framebuffer[y * GFX_FB_W + x] != (u8)((x / 20) % 16))
				bad++;
	CHECK(bad == 0, "pattern: rows below the shapes == (x/20)%16");

	CHECK(vga_framebuffer[65 * GFX_FB_W + 80]  == 15, "white disc centre (80,65)");
	CHECK(vga_framebuffer[65 * GFX_FB_W + 160] == 1,  "blue disc centre (160,65)");
	CHECK(vga_framebuffer[65 * GFX_FB_W + 240] == 4,  "red disc centre (240,65)");
	CHECK(vga_framebuffer[120 * GFX_FB_W + 160] == 14, "yellow rect interior (160,120)");

	int bottom_bad = 0;
	for (int y = 150; y < GFX_FB_H; y++)
		for (int x = 0; x < GFX_FB_W; x++)
			if (vga_framebuffer[y * GFX_FB_W + x] != (u8)((x / 20) % 16))
				bottom_bad++;
	CHECK(bottom_bad == 0, "bottom band (y>=150) stays clean bars");

	/* === gfx_present copies the full double buffer to the framebuffer === */
	gfx_present();
	CHECK(memcmp(test_fb, vga_framebuffer, GFX_FB_BYTES) == 0, "present: fb == double buffer");

	/* === Phase 2: bouncing ball === */
	gfx_draw_ball(160, 100, 20, 0);

	CHECK(vga_framebuffer[50 * GFX_FB_W + 50] == 0, "ball: black bg (50,50)");
	CHECK(vga_framebuffer[0 * GFX_FB_W + 0] == 8, "ball: top-left border grey");
	CHECK(vga_framebuffer[(GFX_FB_H-1) * GFX_FB_W + 0] == 8, "ball: bottom-left border grey");
	CHECK(vga_framebuffer[0 * GFX_FB_W + (GFX_FB_W-1)] == 8, "ball: top-right border grey");
	CHECK(vga_framebuffer[(GFX_FB_H-1) * GFX_FB_W + (GFX_FB_W-1)] == 8, "ball: bottom-right border grey");
	CHECK(vga_framebuffer[100 * GFX_FB_W + 160] == 1, "ball: filled colour=1 at centre (frame 0)");
	{
		/* Outline: white pixels exist, all within ~2 px of the ideal
		 * radius (midpoint circle picks the nearest pixel to the arc). */
		int seen = 0, min_d2 = 1 << 30, max_d2 = -1;
		for (int y = 0; y < GFX_FB_H; y++)
			for (int x = 0; x < GFX_FB_W; x++)
				if (vga_framebuffer[y * GFX_FB_W + x] == 15) {
					int dx = x - 160, dy = y - 100, d2 = dx * dx + dy * dy;
					seen++;
					if (d2 < min_d2) min_d2 = d2;
					if (d2 > max_d2) max_d2 = d2;
				}
		CHECK(seen > 0, "ball: white outline present");
		CHECK(min_d2 >= 18 * 18, "ball: outline no more than 2 px inside radius");
		CHECK(max_d2 <= 22 * 22, "ball: outline no more than 2 px outside radius");
	}

	gfx_draw_ball(160, 100, 20, 5);
	CHECK(vga_framebuffer[100 * GFX_FB_W + 160] == 6, "ball: colour cycles to 6 at frame 5");

	/* === clipping: primitives must not write outside the buffer === */
	memset(vga_framebuffer, 0xAA, GFX_FB_BYTES);	/* sentinel fill */
	gfx_putpixel(-1, 0, 1);  gfx_putpixel(0, -1, 1);
	gfx_putpixel(GFX_FB_W, 0, 1);  gfx_putpixel(0, GFX_FB_H, 1);
	gfx_hline(-10, GFX_FB_W + 10, -1, 1);		/* row clipped away */
	gfx_hline(-10, GFX_FB_W + 10, 5, 1);		/* row clamped to full width */
	gfx_vline(-1, 0, GFX_FB_H - 1, 1);		/* col clipped away */
	gfx_vline(GFX_FB_W - 1, -5, GFX_FB_H + 5, 1);	/* col clamped */

	int clip_bad = 0;
	for (int y = 0; y < GFX_FB_H; y++)
		for (int x = 0; x < GFX_FB_W; x++) {
			u8 want = 0xAA;				/* untouched */
			if (y == 5)              want = 1;	/* hline row */
			if (x == GFX_FB_W - 1)   want = 1;	/* vline col */
			if (vga_framebuffer[y * GFX_FB_W + x] != want)
				clip_bad++;
		}
	CHECK(clip_bad == 0, "clipping: hline/vline clamp, never wrap or corrupt");

	/* fill_rect clamps to the whole screen */
	memset(vga_framebuffer, 0xAA, GFX_FB_BYTES);
	gfx_fill_rect(-10, -10, GFX_FB_W + 10, GFX_FB_H + 10, 2);
	clip_bad = 0;
	for (int y = 0; y < GFX_FB_H; y++)
		for (int x = 0; x < GFX_FB_W; x++)
			if (vga_framebuffer[y * GFX_FB_W + x] != 2)
				clip_bad++;
	CHECK(clip_bad == 0, "clipping: fill_rect clamps to screen bounds");

	/* hline/vline swap unsorted endpoints */
	memset(vga_framebuffer, 0, GFX_FB_BYTES);
	gfx_hline(50, 10, 7, 3);			/* x1 > x2 */
	CHECK(vga_framebuffer[7 * GFX_FB_W + 10] == 3 && vga_framebuffer[7 * GFX_FB_W + 50] == 3 &&
	      vga_framebuffer[7 * GFX_FB_W + 9] == 0 && vga_framebuffer[7 * GFX_FB_W + 51] == 0,
	      "hline: swaps reversed endpoints");
	gfx_vline(9, 50, 10, 4);			/* y1 > y2 */
	CHECK(vga_framebuffer[10 * GFX_FB_W + 9] == 4 && vga_framebuffer[50 * GFX_FB_W + 9] == 4 &&
	      vga_framebuffer[9 * GFX_FB_W + 9] == 0 && vga_framebuffer[51 * GFX_FB_W + 9] == 0,
	      "vline: swaps reversed endpoints");

	/* === user-side wrapper: gfx_run() sends GFX_RUN to TASK_GFX, BOTH === */
	g_sr_func = g_sr_dest = g_sr_type = -1;		/* poison the capture */
	CHECK(gfx_run() == 0, "gfx_run returns 0");
	CHECK(g_sr_func == BOTH, "gfx_run uses BOTH (send+receive)");
	CHECK(g_sr_dest == TASK_GFX, "gfx_run addresses TASK_GFX");
	CHECK(g_sr_type == GFX_RUN, "gfx_run sends GFX_RUN");
	CHECK(desktop_start() == 0, "desktop_start returns success");
	CHECK(g_sr_func == BOTH && g_sr_dest == TASK_DESKTOP && g_sr_type == DESKTOP_START,
	      "desktop_start sends DESKTOP_START to TASK_DESKTOP");
	g_sr_result = -1;
	CHECK(gfx_run() == -1 && desktop_start() == -1, "wrappers propagate a busy VGA result");
	g_sr_result = 0;
	g_sr_bad_reply = 1;
	CHECK(gfx_run() == -1 && desktop_start() == -1, "wrappers reject unexpected replies");
	if (failures == 0)
		printf("ALL %d GFX UNIT CHECKS PASSED\n", checks);
	else
		printf("%d/%d GFX UNIT CHECK(S) FAILED\n", failures, checks);
	return failures ? 1 : 0;
}
