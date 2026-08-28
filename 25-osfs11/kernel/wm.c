/*************************************************************************//**
 *****************************************************************************
 * @file   wm.c
 * @brief  Window Manager implementation for noxisOS
 *
 * Implements a minimal desktop environment with window management,
 * rendering, and mouse interaction for VGA mode 13h.
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
#include "proc.h"
#include "global.h"
#include "proto.h"
#include "wm.h"

/* Simple 8x11 cursor bitmap (arrow pointer) */
PRIVATE const u8 cursor_bitmap[WM_CURSOR_HEIGHT] = {
	0x80, /* X....... */
	0xC0, /* XX...... */
	0xE0, /* XXX..... */
	0xF0, /* XXXX.... */
	0xF8, /* XXXXX... */
	0xFC, /* XXXXXX.. */
	0xFE, /* XXXXXXX. */
	0xF8, /* XXXXX... */
	0xD8, /* XX.XX... */
	0x8C, /* X...XX.. */
	0x0C  /* ....XX.. */
};

/*****************************************************************************
 *                                Drawing Primitives
 *****************************************************************************/

PRIVATE void wm_putpixel(u8 *fb, int x, int y, u8 color)
{
	if (x < 0 || x >= GFX_FB_W || y < 0 || y >= GFX_FB_H)
		return;
	fb[y * GFX_FB_W + x] = color;
}

PRIVATE void wm_hline(u8 *fb, int x1, int x2, int y, u8 color)
{
	int x;
	if (y < 0 || y >= GFX_FB_H)
		return;
	if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
	if (x1 < 0) x1 = 0;
	if (x2 >= GFX_FB_W) x2 = GFX_FB_W - 1;
	for (x = x1; x <= x2; x++)
		fb[y * GFX_FB_W + x] = color;
}

PRIVATE void wm_vline(u8 *fb, int x, int y1, int y2, u8 color)
{
	int y;
	if (x < 0 || x >= GFX_FB_W)
		return;
	if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
	if (y1 < 0) y1 = 0;
	if (y2 >= GFX_FB_H) y2 = GFX_FB_H - 1;
	for (y = y1; y <= y2; y++)
		fb[y * GFX_FB_W + x] = color;
}

PRIVATE void wm_fill_rect(u8 *fb, int x1, int y1, int x2, int y2, u8 color)
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
			fb[y * GFX_FB_W + x] = color;
}

PRIVATE void wm_draw_char(u8 *fb, int x, int y, char ch, u8 color)
{
	/* Simple 8x8 character rendering - just draw a placeholder box for now */
	int i, j;
	for (i = 0; i < 8; i++)
		for (j = 0; j < 6; j++)
			if ((i == 0 || i == 7 || j == 0 || j == 5) && ch != ' ')
				wm_putpixel(fb, x + j, y + i, color);
}

PRIVATE void wm_draw_text(u8 *fb, int x, int y, const char *text, u8 color)
{
	int i = 0;
	while (text[i] && i < 40) {
		wm_draw_char(fb, x + i * 6, y, text[i], color);
		i++;
	}
}

/*****************************************************************************
 *                                wm_init
 *****************************************************************************
 * Initialize the desktop manager structure.
 *****************************************************************************/
PUBLIC void wm_init(DESKTOP *desk, u8 *fb)
{
	int i;

	desk->framebuffer = fb;
	desk->mouse_x = GFX_FB_W / 2;
	desk->mouse_y = GFX_FB_H / 2;
	desk->mouse_buttons = 0;
	desk->active_window = -1;
	desk->running = 1;

	for (i = 0; i < WM_MAX_WINDOWS; i++) {
		desk->windows[i].state = WM_WINDOW_CLOSED;
		desk->windows[i].z_order = 0;
		desk->windows[i].content = 0;
	}
}

/*****************************************************************************
 *                                wm_create_window
 *****************************************************************************
 * Create a new window. Returns window ID, or -1 on failure.
 *****************************************************************************/
PUBLIC int wm_create_window(DESKTOP *desk, int x, int y, int w, int h,
                            const char *title)
{
	int i;

	/* Find free slot */
	for (i = 0; i < WM_MAX_WINDOWS; i++) {
		if (desk->windows[i].state == WM_WINDOW_CLOSED)
			break;
	}

	if (i >= WM_MAX_WINDOWS)
		return -1;

	/* Enforce minimum size */
	if (w < WM_MIN_WIDTH) w = WM_MIN_WIDTH;
	if (h < WM_MIN_HEIGHT) h = WM_MIN_HEIGHT;

	/* Keep on screen */
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x + w > GFX_FB_W) x = GFX_FB_W - w;
	if (y + h > GFX_FB_H) y = GFX_FB_H - h;

	/* Initialize window */
	desk->windows[i].x = x;
	desk->windows[i].y = y;
	desk->windows[i].width = w;
	desk->windows[i].height = h;
	desk->windows[i].state = WM_WINDOW_NORMAL;
	desk->windows[i].z_order = i + 1;

	/* Copy title */
	int j = 0;
	while (title[j] && j < 31) {
		desk->windows[i].title[j] = title[j];
		j++;
	}
	desk->windows[i].title[j] = '\0';

	return i;
}

/*****************************************************************************
 *                                wm_close_window
 *****************************************************************************
 * Close a window by ID.
 *****************************************************************************/
PUBLIC void wm_close_window(DESKTOP *desk, int win_id)
{
	if (win_id < 0 || win_id >= WM_MAX_WINDOWS)
		return;

	desk->windows[win_id].state = WM_WINDOW_CLOSED;

	if (desk->active_window == win_id)
		desk->active_window = -1;
}

/*****************************************************************************
 *                                wm_draw_desktop
 *****************************************************************************
 * Draw the desktop background.
 *****************************************************************************/
PUBLIC void wm_draw_desktop(DESKTOP *desk)
{
	/* Fill with desktop color */
	memset(desk->framebuffer, WM_COLOR_DESKTOP, GFX_FB_BYTES);

	/* Draw a simple pattern - horizontal gradient */
	int y;
	for (y = 0; y < GFX_FB_H; y++) {
		u8 color = WM_COLOR_DESKTOP + (y / 50);
		if (color > WM_COLOR_DESKTOP + 3)
			color = WM_COLOR_DESKTOP + 3;
		wm_hline(desk->framebuffer, 0, GFX_FB_W - 1, y, color);
	}
}

/*****************************************************************************
 *                                wm_draw_window
 *****************************************************************************
 * Draw a single window with border, titlebar, and content area.
 *****************************************************************************/
PUBLIC void wm_draw_window(DESKTOP *desk, int win_id)
{
	if (win_id < 0 || win_id >= WM_MAX_WINDOWS)
		return;

	WINDOW *win = &desk->windows[win_id];

	if (win->state == WM_WINDOW_CLOSED)
		return;

	u8 *fb = desk->framebuffer;
	int x = win->x;
	int y = win->y;
	int w = win->width;
	int h = win->height;

	/* Draw shadow */
	wm_fill_rect(fb, x + 2, y + 2, x + w + 1, y + h + 1, WM_COLOR_SHADOW);

	/* Draw border */
	wm_fill_rect(fb, x, y, x + w - 1, y + h - 1, WM_COLOR_BORDER);

	/* Draw titlebar */
	u8 title_color = (win_id == desk->active_window) ?
	                 WM_COLOR_TITLEBAR : (WM_COLOR_TITLEBAR - 2);
	wm_fill_rect(fb, x + WM_BORDER_WIDTH, y + WM_BORDER_WIDTH,
	             x + w - WM_BORDER_WIDTH - 1,
	             y + WM_TITLE_HEIGHT - 1, title_color);

	/* Draw title text */
	wm_draw_text(fb, x + WM_BORDER_WIDTH + 4, y + WM_BORDER_WIDTH + 2,
	             win->title, WM_COLOR_TITLE_TEXT);

	/* Draw content area */
	wm_fill_rect(fb, x + WM_BORDER_WIDTH, y + WM_TITLE_HEIGHT,
	             x + w - WM_BORDER_WIDTH - 1,
	             y + h - WM_BORDER_WIDTH - 1, WM_COLOR_WINDOW_BG);
}

/*****************************************************************************
 *                                wm_draw_cursor
 *****************************************************************************
 * Draw mouse cursor at current position.
 *****************************************************************************/
PUBLIC void wm_draw_cursor(DESKTOP *desk)
{
	int x, y, bit;
	u8 *fb = desk->framebuffer;

	for (y = 0; y < WM_CURSOR_HEIGHT; y++) {
		for (bit = 7; bit >= 0; bit--) {
			if (cursor_bitmap[y] & (1 << bit)) {
				x = desk->mouse_x + (7 - bit);
				wm_putpixel(fb, x, desk->mouse_y + y, WM_COLOR_TITLE_TEXT);
			}
		}
	}
}

/*****************************************************************************
 *                                wm_update_mouse
 *****************************************************************************
 * Update mouse position and buttons from driver.
 *****************************************************************************/
PUBLIC void wm_update_mouse(DESKTOP *desk, int dx, int dy, int buttons)
{
	desk->mouse_x += dx;
	desk->mouse_y += dy;

	/* Clamp to screen */
	if (desk->mouse_x < 0) desk->mouse_x = 0;
	if (desk->mouse_x >= GFX_FB_W) desk->mouse_x = GFX_FB_W - 1;
	if (desk->mouse_y < 0) desk->mouse_y = 0;
	if (desk->mouse_y >= GFX_FB_H) desk->mouse_y = GFX_FB_H - 1;

	/* Detect clicks (button press) */
	if ((buttons & 1) && !(desk->mouse_buttons & 1)) {
		wm_handle_click(desk, desk->mouse_x, desk->mouse_y);
	}

	desk->mouse_buttons = buttons;
}

/*****************************************************************************
 *                                wm_hit_test
 *****************************************************************************
 * Find which window is under the given coordinates.
 * Returns window ID, or -1 if none.
 *****************************************************************************/
PUBLIC int wm_hit_test(DESKTOP *desk, int x, int y)
{
	int i, best = -1, best_z = -1;

	/* Find topmost window at this position */
	for (i = 0; i < WM_MAX_WINDOWS; i++) {
		WINDOW *win = &desk->windows[i];

		if (win->state != WM_WINDOW_NORMAL)
			continue;

		if (x >= win->x && x < win->x + win->width &&
		    y >= win->y && y < win->y + win->height) {
			if (win->z_order > best_z) {
				best = i;
				best_z = win->z_order;
			}
		}
	}

	return best;
}

/*****************************************************************************
 *                                wm_focus_window
 *****************************************************************************
 * Bring a window to front and give it focus.
 *****************************************************************************/
PUBLIC void wm_focus_window(DESKTOP *desk, int win_id)
{
	int i, max_z = 0;

	if (win_id < 0 || win_id >= WM_MAX_WINDOWS)
		return;

	if (desk->windows[win_id].state != WM_WINDOW_NORMAL)
		return;

	/* Find max z-order */
	for (i = 0; i < WM_MAX_WINDOWS; i++) {
		if (desk->windows[i].state == WM_WINDOW_NORMAL) {
			if (desk->windows[i].z_order > max_z)
				max_z = desk->windows[i].z_order;
		}
	}

	/* Bring to front */
	desk->windows[win_id].z_order = max_z + 1;
	desk->active_window = win_id;
}

/*****************************************************************************
 *                                wm_handle_click
 *****************************************************************************
 * Handle mouse click at given coordinates.
 *****************************************************************************/
PUBLIC void wm_handle_click(DESKTOP *desk, int x, int y)
{
	int win_id = wm_hit_test(desk, x, y);

	if (win_id >= 0) {
		wm_focus_window(desk, win_id);
	}
}
