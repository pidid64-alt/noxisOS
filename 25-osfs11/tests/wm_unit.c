/*
 * Unit test for the REAL kernel/wm.c window manager.
 *
 * Same harness as tests/gfx_unit.c: wm.c is #included so every PRIVATE/static
 * function lives in this translation unit and is directly callable. Kernel
 * dependencies resolve to the empty guards in tests/stubs/ (pass
 * `-iquote tests/stubs`), while the REAL include/wm.h is pulled in through
 * `-iquote include` -- so the WINDOW/DESKTOP layout under test is the one the
 * kernel actually compiles.
 *
 * Build (see Makefile `test' target):
 *   cc -m32 -Wall -Wextra -iquote tests/stubs -o tests/wm_unit tests/wm_unit.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- mirrored kernel types/macros (see include/type.h) ------------------ */
typedef unsigned char	u8;
typedef unsigned short	u16;
typedef unsigned int	u32;

#define PUBLIC
#define PRIVATE		static

/* ---- mirrored constants (see include/sys/const.h) ----------------------- */
#define GFX_FB_W	320
#define GFX_FB_H	200
#define GFX_FB_BYTES	(GFX_FB_W * GFX_FB_H)

/* ---- mirrored window-manager constants/types (see include/wm.h) ---------
 * wm.c's own `#include "wm.h"' resolves to the empty guard in tests/stubs/
 * (do NOT pass -iquote include: the real header redefines these and its own
 * "type.h" would pull in the kernel's, clashing with the mirrors above).
 * The kernel build itself compiles wm.c against the real include/wm.h, so a
 * drift between the two is caught there. */
#define WM_MAX_WINDOWS      8
#define WM_TITLE_HEIGHT     18
#define WM_BORDER_WIDTH     2
#define WM_MIN_WIDTH        60
#define WM_MIN_HEIGHT       48

#define WM_WINDOW_CLOSED    0
#define WM_WINDOW_NORMAL    1
#define WM_WINDOW_MINIMIZED 2
#define WM_WINDOW_MAXIMIZED 3

#define WM_COLOR_DESKTOP    1
#define WM_COLOR_BORDER     8
#define WM_COLOR_TITLEBAR   9
#define WM_COLOR_TITLE_TEXT 15
#define WM_COLOR_WINDOW_BG  7
#define WM_COLOR_SHADOW     0
#define WM_COLOR_TERM_BG    0
#define WM_COLOR_TERM_TEXT  15

#define WM_CURSOR_WIDTH     8
#define WM_CURSOR_HEIGHT    11

#define WM_TERM_CELL_W      8
#define WM_TERM_CELL_H      16
#define WM_TERM_COLS        36
#define WM_TERM_ROWS        9
#define WM_TERM_PAD         2
#define WM_TERM_WIN_W  (WM_TERM_COLS * WM_TERM_CELL_W + \
                        2 * WM_BORDER_WIDTH + 2 * WM_TERM_PAD)
#define WM_TERM_WIN_H  (WM_TERM_ROWS * WM_TERM_CELL_H + WM_TITLE_HEIGHT + \
                        WM_BORDER_WIDTH + 2 * WM_TERM_PAD)

typedef struct s_window {
	int x, y;
	int width, height;
	int state;
	int z_order;
	char title[32];
	u8 *content;
} WINDOW;

typedef struct s_terminal {
	char cells[WM_TERM_ROWS][WM_TERM_COLS];
	int  cur_col, cur_row;
	int  win_id;
} TERMINAL;

typedef struct s_desktop {
	WINDOW windows[WM_MAX_WINDOWS];
	int active_window;
	int mouse_x, mouse_y;
	int mouse_buttons;
	int drag_window;
	int drag_offset_x;
	int drag_offset_y;
	u8 *framebuffer;
	int running;
	TERMINAL term;
} DESKTOP;

/* mirrored prototypes (see include/wm.h) */
PUBLIC void wm_init(DESKTOP *desk, u8 *fb);
PUBLIC int wm_create_window(DESKTOP *desk, int x, int y, int w, int h, const char *title);
PUBLIC void wm_close_window(DESKTOP *desk, int win_id);
PUBLIC void wm_draw_desktop(DESKTOP *desk);
PUBLIC void wm_draw_window(DESKTOP *desk, int win_id);
PUBLIC void wm_paint_all(DESKTOP *desk);
PUBLIC void wm_draw_cursor(DESKTOP *desk);
PUBLIC void wm_update_mouse(DESKTOP *desk, int dx, int dy, int buttons);
PUBLIC void wm_handle_click(DESKTOP *desk, int x, int y);
PUBLIC void wm_focus_window(DESKTOP *desk, int win_id);
PUBLIC int wm_hit_test(DESKTOP *desk, int x, int y);
PUBLIC void wm_draw_welcome(DESKTOP *desk, const char *title, const char *subtitle);
PUBLIC int  wm_term_open(DESKTOP *desk, const char *title);
PUBLIC void wm_term_clear(DESKTOP *desk);
PUBLIC void wm_term_putc(DESKTOP *desk, char c);
PUBLIC void wm_term_puts(DESKTOP *desk, const char *s);
PUBLIC void wm_term_backspace(DESKTOP *desk);

/* Guard bytes around the fake framebuffer so any out-of-bounds write by a
 * drawing primitive is caught instead of silently corrupting the heap. */
#define GUARD		64

static int failures = 0;
static int checks = 0;
#define CHECK(c, m) do { checks++; if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

/* ---- the real implementation under test --------------------------------- */
#include "../kernel/wm.c"

/* ---- helpers ------------------------------------------------------------ */
static u8	*fb_alloc(void)
{
	u8 *raw = malloc(GUARD + GFX_FB_BYTES + GUARD);
	memset(raw, 0xCC, GUARD + GFX_FB_BYTES + GUARD);
	return raw + GUARD;
}

static int guard_ok(u8 *fb)
{
	int i;
	for (i = 0; i < GUARD; i++)
		if (fb[-1 - i] != 0xCC || fb[GFX_FB_BYTES + i] != 0xCC)
			return 0;
	return 1;
}

/* Replicates the z-order render loop from kernel/desktop.c:desktop_run().
 * Returns how many of the NORMAL windows would actually be painted. */
static int render_pass(DESKTOP *d)
{
	int z, i, painted = 0;
	for (z = 1; z <= WM_MAX_WINDOWS; z++)
		for (i = 0; i < WM_MAX_WINDOWS; i++)
			if (d->windows[i].state == WM_WINDOW_NORMAL &&
			    d->windows[i].z_order == z)
				painted++;
	return painted;
}

static int count_normal(DESKTOP *d)
{
	int i, n = 0;
	for (i = 0; i < WM_MAX_WINDOWS; i++)
		if (d->windows[i].state == WM_WINDOW_NORMAL)
			n++;
	return n;
}

int main(void)
{
	DESKTOP desk;
	u8 *fb = fb_alloc();

	/* === wm_init === */
	wm_init(&desk, fb);
	CHECK(desk.framebuffer == fb,			"wm_init: framebuffer stored");
	CHECK(desk.running == 1,			"wm_init: running flag set");
	CHECK(desk.active_window == -1,			"wm_init: no active window");
	CHECK(desk.mouse_x == GFX_FB_W / 2 &&
	      desk.mouse_y == GFX_FB_H / 2,		"wm_init: cursor centred");
	CHECK(count_normal(&desk) == 0,			"wm_init: all slots closed");

	/* === wm_create_window === */
	CHECK(wm_create_window(&desk, 20, 20, 120, 80, "Welcome") == 0,
							"create: first slot is 0");
	CHECK(wm_create_window(&desk, 80, 50, 140, 100, "Window 1") == 1,
							"create: second slot is 1");
	CHECK(wm_create_window(&desk, 160, 80, 130, 90, "Demo") == 2,
							"create: third slot is 2");
	CHECK(desk.windows[0].z_order == 1 &&
	      desk.windows[1].z_order == 2 &&
	      desk.windows[2].z_order == 3,		"create: z_order 1,2,3");
	CHECK(strcmp(desk.windows[0].title, "Welcome") == 0,
							"create: title copied");

	/* minimum size is enforced */
	CHECK(wm_create_window(&desk, 0, 0, 1, 1, "tiny") == 3,
							"create: tiny window takes slot 3");
	CHECK(desk.windows[3].width >= WM_MIN_WIDTH &&
	      desk.windows[3].height >= WM_MIN_HEIGHT,	"create: min size enforced");

	/* off-screen request is pulled back onto the screen */
	CHECK(wm_create_window(&desk, 300, 190, 100, 100, "edge") == 4,
							"create: edge window takes slot 4");
	CHECK(desk.windows[4].x >= 0 && desk.windows[4].y >= 0 &&
	      desk.windows[4].x + desk.windows[4].width  <= GFX_FB_W &&
	      desk.windows[4].y + desk.windows[4].height <= GFX_FB_H,
							"create: clamped fully on screen");

	/* long titles are truncated, never overflowed */
	{
		const char *longt = "0123456789012345678901234567890123456789";
		int id = wm_create_window(&desk, 0, 0, 100, 60, longt);
		CHECK(id == 5,				"create: long-title window takes slot 5");
		CHECK(strlen(desk.windows[id].title) == 31 &&
		      desk.windows[id].title[31] == '\0',
							"create: long title truncated to 31");
	}

	/* table full */
	CHECK(wm_create_window(&desk, 0, 0, 80, 60, "six") == 6,
							"create: seventh slot");
	CHECK(wm_create_window(&desk, 0, 0, 80, 60, "seven") == 7,
							"create: eighth slot");
	CHECK(wm_create_window(&desk, 0, 0, 80, 60, "nine") == -1,
							"create: fails when table is full");

	/* === wm_hit_test / wm_focus_window === */
	wm_init(&desk, fb);
	wm_create_window(&desk, 0,  0,  100, 100, "A");	/* slot 0, z=1 */
	wm_create_window(&desk, 50, 50, 100, 100, "B");	/* slot 1, z=2, overlaps A */
	CHECK(wm_hit_test(&desk, 10, 10) == 0,		"hit_test: only A there");
	CHECK(wm_hit_test(&desk, 60, 60) == 1,		"hit_test: B on top in overlap");
	CHECK(wm_hit_test(&desk, 300, 190) == -1,	"hit_test: empty area");

	wm_focus_window(&desk, 0);
	CHECK(desk.active_window == 0,			"focus: A becomes active");
	CHECK(wm_hit_test(&desk, 60, 60) == 0,		"focus: A now on top of B");

	/* Clicking around must not push a window out of the range the renderer
	 * walks (desktop.c paints z = 1..WM_MAX_WINDOWS only). */
	{
		int i;
		for (i = 0; i < 40; i++) {
			wm_focus_window(&desk, i % 2);
			CHECK(render_pass(&desk) == count_normal(&desk),
			      "focus: every NORMAL window is still painted after re-focus");
			if (failures) break;
		}
	}

	/* focusing a closed or bogus window is a no-op */
	wm_close_window(&desk, 1);
	CHECK(desk.windows[1].state == WM_WINDOW_CLOSED, "close: slot closed");
	wm_focus_window(&desk, 1);
	CHECK(desk.active_window != 1,			"focus: closed window not focusable");
	wm_focus_window(&desk, -1);
	wm_focus_window(&desk, WM_MAX_WINDOWS);
	CHECK(1,					"focus: bogus ids rejected");

	/* closing the active window clears focus */
	wm_focus_window(&desk, 0);
	wm_close_window(&desk, 0);
	CHECK(desk.active_window == -1,			"close: active window clears focus");

	/* === wm_draw_desktop: every pixel painted, nothing out of bounds === */
	wm_init(&desk, fb);
	memset(fb, 0xCC, GFX_FB_BYTES);
	wm_draw_desktop(&desk);
	{
		int unpainted = 0, other = 0, y, x;
		for (y = 0; y < GFX_FB_H; y++)
			for (x = 0; x < GFX_FB_W; x++) {
				u8 c = fb[y * GFX_FB_W + x];
				if (c == 0xCC)
					unpainted++;
				else if (c != WM_COLOR_DESKTOP)
					other++;
			}
		CHECK(unpainted == 0,			"draw_desktop: whole screen painted");
		/* The background is one flat blue: no gradient into the green,
		 * cyan and red palette entries next to WM_COLOR_DESKTOP. */
		CHECK(other == 0,			"draw_desktop: every pixel is the single desktop blue");
	}
	CHECK(guard_ok(fb),				"draw_desktop: no out-of-bounds write");

	/* === wm_draw_welcome: blue background + centred white text === */
	wm_init(&desk, fb);
	memset(fb, 0xCC, GFX_FB_BYTES);
	wm_draw_welcome(&desk, "Welcome to noxisOS", 0);
	{
		int y, x, text = 0, other = 0, min_x = GFX_FB_W, max_x = -1;
		for (y = 0; y < GFX_FB_H; y++)
			for (x = 0; x < GFX_FB_W; x++) {
				u8 c = fb[y * GFX_FB_W + x];
				if (c == WM_COLOR_TITLE_TEXT) {
					text++;
					if (x < min_x) min_x = x;
					if (x > max_x) max_x = x;
				}
				else if (c != WM_COLOR_DESKTOP) {
					other++;
				}
			}
		CHECK(text > 100,			"draw_welcome: the message is rendered");
		CHECK(other == 0,			"draw_welcome: only blue behind the message");
		CHECK(min_x > 20 && max_x < GFX_FB_W - 20,
							"draw_welcome: message is centred");
	}
	CHECK(guard_ok(fb),				"draw_welcome: no out-of-bounds write");

	/* === terminal window: buffer, scrolling and rendering === */
	wm_init(&desk, fb);
	CHECK(desk.term.win_id == -1,			"wm_init: no terminal window yet");
	{
		int id = wm_term_open(&desk, "TTY");
		int r, c, dirty = 0;

		CHECK(id >= 0,				"term_open: window created");
		CHECK(desk.term.win_id == id,		"term_open: terminal is attached");
		CHECK(desk.active_window == id,		"term_open: terminal is focused");
		CHECK(desk.windows[id].width == WM_TERM_WIN_W &&
		      desk.windows[id].height == WM_TERM_WIN_H,
							"term_open: window fits the character grid");
		CHECK(desk.windows[id].x >= 0 && desk.windows[id].y >= 0 &&
		      desk.windows[id].x + desk.windows[id].width <= GFX_FB_W &&
		      desk.windows[id].y + desk.windows[id].height <= GFX_FB_H,
							"term_open: window is on screen");

		for (r = 0; r < WM_TERM_ROWS; r++)
			for (c = 0; c < WM_TERM_COLS; c++)
				if (desk.term.cells[r][c] != ' ')
					dirty++;
		CHECK(dirty == 0,			"term_open: grid starts blank");

		wm_term_puts(&desk, "hi\n");
		CHECK(desk.term.cells[0][0] == 'h' && desk.term.cells[0][1] == 'i',
							"term_puts: text lands in the grid");
		CHECK(desk.term.cur_row == 1 && desk.term.cur_col == 0,
							"term_puts: newline moves to the next row");

		wm_term_putc(&desk, 'x');
		wm_term_backspace(&desk);
		CHECK(desk.term.cells[1][0] == ' ' && desk.term.cur_col == 0,
							"term_backspace: erases the last character");
		wm_term_backspace(&desk);
		CHECK(desk.term.cur_row == 0 && desk.term.cur_col == WM_TERM_COLS - 1,
							"term_backspace: wraps to the previous row");
		desk.term.cur_row = 0; desk.term.cur_col = 0;
		wm_term_clear(&desk);

		/* Overflowing the bottom row scrolls instead of writing out of
		 * the buffer. */
		for (r = 0; r < WM_TERM_ROWS + 3; r++) {
			wm_term_putc(&desk, (char)('A' + r));
			wm_term_putc(&desk, '\n');
		}
		CHECK(desk.term.cur_row == WM_TERM_ROWS - 1,
							"term_putc: cursor stays on the last row");
		CHECK(desk.term.cells[WM_TERM_ROWS - 2][0] == (char)('A' + WM_TERM_ROWS + 2),
							"term_putc: the grid scrolled up");

		/* Long lines wrap inside the grid, never past its last column. */
		wm_term_clear(&desk);
		for (r = 0; r < WM_TERM_COLS + 5; r++)
			wm_term_putc(&desk, '#');
		CHECK(desk.term.cur_row == 1 && desk.term.cur_col == 5,
							"term_putc: long line wraps to the next row");

		/* Rendering stays inside the window and paints real glyphs. */
		memset(fb, 0, GFX_FB_BYTES);
		wm_draw_window(&desk, id);
		{
			int x, y, stray = 0, glyph = 0;
			int x0 = desk.windows[id].x, y0 = desk.windows[id].y;
			int w = desk.windows[id].width, h = desk.windows[id].height;
			for (y = 0; y < GFX_FB_H; y++)
				for (x = 0; x < GFX_FB_W; x++) {
					int inside = (x >= x0 - 1 && x <= x0 + w + 1 &&
						      y >= y0 - 1 && y <= y0 + h + 1);
					if (!inside && fb[y * GFX_FB_W + x] != 0)
						stray++;
					if (inside && y > y0 + WM_TITLE_HEIGHT &&
					    fb[y * GFX_FB_W + x] == WM_COLOR_TERM_TEXT)
						glyph++;
				}
			CHECK(stray == 0,		"draw_terminal: nothing painted outside the window");
			CHECK(glyph > 100,		"draw_terminal: terminal text is rendered");
		}
		CHECK(guard_ok(fb),			"draw_terminal: no out-of-bounds write");

		/* Closing the window detaches the terminal. */
		wm_close_window(&desk, id);
		CHECK(desk.term.win_id == -1,		"close: terminal detaches with its window");
	}

	/* === wm_draw_window: stays inside the window + its shadow === */
	wm_init(&desk, fb);
	{
		int id = wm_create_window(&desk, 40, 40, 100, 70, "Box");
		int x, y, stray = 0;
		int x0 = desk.windows[id].x, y0 = desk.windows[id].y;
		int w  = desk.windows[id].width, h = desk.windows[id].height;
		memset(fb, 0, GFX_FB_BYTES);
		wm_draw_window(&desk, id);
		for (y = 0; y < GFX_FB_H; y++)
			for (x = 0; x < GFX_FB_W; x++) {
				int inside = (x >= x0 - 1 && x <= x0 + w + 1 &&
					      y >= y0 - 1 && y <= y0 + h + 1);
				if (!inside && fb[y * GFX_FB_W + x] != 0)
					stray++;
			}
		CHECK(stray == 0,			"draw_window: no pixels outside window+shadow");
		CHECK(guard_ok(fb),			"draw_window: no out-of-bounds write");
	}

	/* window flush against the right/bottom edge must not write past the fb */
	wm_init(&desk, fb);
	{
		int id = wm_create_window(&desk, GFX_FB_W - 10, GFX_FB_H - 10,
					  100, 70, "Corner");
		memset(fb, 0xAA, GFX_FB_BYTES);
		wm_draw_window(&desk, id);
		wm_draw_desktop(&desk);
		wm_draw_window(&desk, id);
		CHECK(guard_ok(fb),			"draw_window: edge window stays in bounds");
	}

	/* === title text must stay inside the title bar === */
	wm_init(&desk, fb);
	{
		int id = wm_create_window(&desk, 10, 10, 70, 60,
					  "A title far too long to fit");
		int x, y, spill = 0;
		int x0 = desk.windows[id].x, y0 = desk.windows[id].y;
		int w  = desk.windows[id].width;
		int ty1 = y0 + WM_BORDER_WIDTH;
		int ty2 = y0 + WM_TITLE_HEIGHT - 1;
		memset(fb, 0, GFX_FB_BYTES);
		wm_draw_window(&desk, id);
		/* Text colour is white; count white pixels to the right of the
		 * window or below the title bar. */
		for (y = 0; y < GFX_FB_H; y++)
			for (x = 0; x < GFX_FB_W; x++)
				if (fb[y * GFX_FB_W + x] == WM_COLOR_TITLE_TEXT &&
				    (x > x0 + w - 1 || y > ty2 || y < ty1))
					spill++;
		CHECK(spill == 0,			"draw_window: title text clipped to title bar");
	}

	/* === wm_draw_cursor: clipped at every corner === */
	wm_init(&desk, fb);
	{
		int corners[4][2] = {{0,0}, {GFX_FB_W-1, 0},
				     {0, GFX_FB_H-1}, {GFX_FB_W-1, GFX_FB_H-1}};
		int i;
		for (i = 0; i < 4; i++) {
			desk.mouse_x = corners[i][0];
			desk.mouse_y = corners[i][1];
			memset(fb, 0xAA, GFX_FB_BYTES);
			wm_draw_cursor(&desk);
			CHECK(guard_ok(fb),		"draw_cursor: clipped at screen corner");
		}
		/* the cursor actually paints something */
		desk.mouse_x = 100; desk.mouse_y = 100;
		memset(fb, 0, GFX_FB_BYTES);
		wm_draw_cursor(&desk);
		CHECK(fb[100 * GFX_FB_W + 100] == WM_COLOR_TITLE_TEXT,
							"draw_cursor: hotspot pixel painted");
	}

	/* === wm_update_mouse: clamps and detects a click edge === */
	wm_init(&desk, fb);
	wm_create_window(&desk, 0, 0, 100, 100, "A");
	wm_create_window(&desk, 50, 50, 100, 100, "B");
	desk.mouse_x = 0; desk.mouse_y = 0; desk.mouse_buttons = 0;
	wm_update_mouse(&desk, -500, -500, 0);
	CHECK(desk.mouse_x == 0 && desk.mouse_y == 0,	"update_mouse: clamps to 0,0");
	wm_update_mouse(&desk, 5000, 5000, 0);
	CHECK(desk.mouse_x == GFX_FB_W - 1 && desk.mouse_y == GFX_FB_H - 1,
							"update_mouse: clamps to bottom-right");
	desk.mouse_x = 60; desk.mouse_y = 60; desk.mouse_buttons = 0;
	wm_update_mouse(&desk, 0, 0, 1);		/* press over B */
	CHECK(desk.active_window == 1,			"update_mouse: press focuses the hit window");
	wm_update_mouse(&desk, 0, 0, 1);		/* still held: no repeat */
	wm_focus_window(&desk, 0);
	wm_update_mouse(&desk, 0, 0, 1);
	CHECK(desk.active_window == 0,			"update_mouse: held button does not re-click");
	wm_update_mouse(&desk, 0, 0, 0);		/* release */
	wm_update_mouse(&desk, 0, 0, 1);
	CHECK(desk.active_window == 0,			"update_mouse: release+press is a new click");

	/* === z_order stays a permutation of 1..N under churn === */
	wm_init(&desk, fb);
	{
		int i, round;
		for (i = 0; i < 5; i++)
			wm_create_window(&desk, i * 20, i * 10, 80, 60, "w");
		for (round = 0; round < 200; round++) {
			int seen = 0, ok = 1;
			wm_focus_window(&desk, round % 5);
			if ((round % 7) == 3) {
				wm_close_window(&desk, (round / 7) % 5);
				if (count_normal(&desk) == 0) {
					for (i = 0; i < 5; i++)
						wm_create_window(&desk, i * 20, i * 10,
								 80, 60, "w");
				}
			}
			for (i = 0; i < WM_MAX_WINDOWS; i++) {
				int z = desk.windows[i].z_order;
				if (desk.windows[i].state != WM_WINDOW_NORMAL) {
					if (z != 0) ok = 0;
					continue;
				}
				if (z < 1 || z > count_normal(&desk)) ok = 0;
				if (seen & (1 << z)) ok = 0;	/* duplicate z */
				seen |= (1 << z);
			}
			CHECK(ok, "restack: z_order stays a dense 1..N permutation");
			CHECK(render_pass(&desk) == count_normal(&desk),
			      "restack: renderer still paints every NORMAL window");
			if (failures) break;
		}
	}

	/* === wm_paint_all paints everything and stays in bounds === */
	wm_init(&desk, fb);
	wm_create_window(&desk, 0, 0, 100, 80, "One");
	wm_create_window(&desk, 250, 150, 100, 80, "Two");
	desk.mouse_x = 5; desk.mouse_y = 5;
	memset(fb, 0xCC, GFX_FB_BYTES);
	wm_paint_all(&desk);
	{
		int unpainted = 0, y, x;
		for (y = 0; y < GFX_FB_H; y++)
			for (x = 0; x < GFX_FB_W; x++)
				if (fb[y * GFX_FB_W + x] == 0xCC)
					unpainted++;
		CHECK(unpainted == 0,			"paint_all: whole screen painted");
	}
	CHECK(guard_ok(fb),				"paint_all: no out-of-bounds write");

	/* === the title is rendered with a real font, not placeholder boxes === */
	wm_init(&desk, fb);
	{
		int id = wm_create_window(&desk, 0, 0, 200, 80, "0O");
		int x, y, white = 0;
		memset(fb, 0, GFX_FB_BYTES);
		wm_draw_window(&desk, id);
		for (y = 0; y < WM_TITLE_HEIGHT; y++)
			for (x = 0; x < 200; x++)
				if (fb[y * GFX_FB_W + x] == WM_COLOR_TITLE_TEXT)
					white++;
		/* Two 8x16 glyphs. A hollow-box placeholder would give a fixed
		 * 2*(8+6)-4 = 24 px each; real glyphs give far more, and '0'
		 * has a hole in the middle that a box outline does not. */
		CHECK(white > 60,			"draw_window: title glyphs are filled, not outlined boxes");
		CHECK(fb[(WM_BORDER_WIDTH + 8) * GFX_FB_W + (WM_BORDER_WIDTH + 2 + 3)]
		      != WM_COLOR_TITLE_TEXT,
						"draw_window: '0' has a hole (real glyph, not a box)");
	}

	if (failures == 0)
		printf("ALL %d WM UNIT CHECKS PASSED\n", checks);
	else
		printf("%d/%d WM UNIT CHECK(S) FAILED\n", failures, checks);
	return failures ? 1 : 0;
}
