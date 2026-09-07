/*************************************************************************//**
 *****************************************************************************
 * @file   wm.c
 * @brief  Window Manager implementation for noxisOS
 *
 * Implements a minimal desktop environment with window management,
 * rendering, and mouse interaction for the 800x600 VBE framebuffer.
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
#include "wm.h"
#include "wm_font.h"

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
	/* 8x16 glyph from the shared public-domain font (kernel/wm_font.h).
	 * Row bit 7 is the leftmost pixel. Anything outside 32..126 (or the
	 * space glyph, which is all-zero) simply paints the background, which
	 * is what the previous "draw a hollow box" placeholder could not do. */
	const u8 *g;
	int ry, rx;
	unsigned char c = (unsigned char)ch;

	if (c < WM_FONT_FIRST || c > WM_FONT_LAST)
		return;

	g = wm_font8x16[c - WM_FONT_FIRST];

	for (ry = 0; ry < WM_FONT_H; ry++)
		for (rx = 0; rx < WM_FONT_W; rx++)
			if (g[ry] & (0x80 >> rx))
				wm_putpixel(fb, x + rx, y + ry, color);
}

/*
 * Render `text' at (x, y) in `color'.
 *
 * `max_px' is a hard clip width in pixels: glyphs that would not fit inside
 * it are not drawn at all. Titles are drawn inside the title bar, so without
 * this the text ran off the right edge of the window and over its neighbours.
 */
PRIVATE void wm_draw_text(u8 *fb, int x, int y, const char *text, u8 color,
                          int max_px)
{
	int i = 0;

	while (text[i]) {
		if ((i + 1) * WM_FONT_W > max_px)
			break;
		wm_draw_char(fb, x + i * WM_FONT_W, y, text[i], color);
		i++;
	}
}

/* wm_restack is defined further down, next to wm_focus_window. */
PRIVATE void wm_restack(DESKTOP *desk, int front);

/*****************************************************************************
 *                                Terminal (TTY window)
 *****************************************************************************
 * A single window may host a text terminal: a WM_TERM_ROWS x WM_TERM_COLS
 * grid of characters that scrolls up when the cursor leaves the bottom row.
 * The window manager only owns the buffer and its rendering; line editing
 * and command handling live in kernel/desktop.c.
 *****************************************************************************/

/*****************************************************************************
 *                                wm_term_clear
 *****************************************************************************/
PUBLIC void wm_term_clear(DESKTOP *desk)
{
	int r, c;

	for (r = 0; r < WM_TERM_ROWS; r++)
		for (c = 0; c < WM_TERM_COLS; c++)
			desk->term.cells[r][c] = ' ';

	desk->term.cur_row = 0;
	desk->term.cur_col = 0;
}

/*****************************************************************************
 *                                wm_term_scroll
 *****************************************************************************/
PRIVATE void wm_term_scroll(DESKTOP *desk)
{
	int r, c;

	for (r = 0; r < WM_TERM_ROWS - 1; r++)
		for (c = 0; c < WM_TERM_COLS; c++)
			desk->term.cells[r][c] = desk->term.cells[r + 1][c];

	for (c = 0; c < WM_TERM_COLS; c++)
		desk->term.cells[WM_TERM_ROWS - 1][c] = ' ';

	desk->term.cur_row = WM_TERM_ROWS - 1;
}

/*****************************************************************************
 *                                wm_term_open
 *****************************************************************************
 * Create the terminal window (centred, sized to fit the character grid) and
 * make it the terminal host. Returns the window id, or -1 on failure.
 *****************************************************************************/
PUBLIC int wm_term_open(DESKTOP *desk, const char *title)
{
	int id = wm_create_window(desk,
	                          WM_TERM_WIN_X, WM_TERM_WIN_Y,
	                          WM_TERM_WIN_W, WM_TERM_WIN_H, title);

	if (id < 0)
		return -1;

	desk->term.win_id = id;
	wm_term_clear(desk);
	wm_focus_window(desk, id);

	return id;
}

/*****************************************************************************
 *                                wm_term_putc
 *****************************************************************************
 * Print one character at the cursor. '\n' starts a new line, the grid
 * scrolls when the cursor runs past the last row.
 *****************************************************************************/
PUBLIC void wm_term_putc(DESKTOP *desk, char c)
{
	if (c == '\n') {
		desk->term.cur_col = 0;
		desk->term.cur_row++;
	}
	else if (c == '\b') {
		wm_term_backspace(desk);
		return;
	}
	else {
		if (c < 32 || c > 126)		/* not printable: ignore */
			return;
		desk->term.cells[desk->term.cur_row][desk->term.cur_col] = c;
		desk->term.cur_col++;
		if (desk->term.cur_col >= WM_TERM_COLS) {
			desk->term.cur_col = 0;
			desk->term.cur_row++;
		}
	}

	if (desk->term.cur_row >= WM_TERM_ROWS)
		wm_term_scroll(desk);
}

/*****************************************************************************
 *                                wm_term_puts
 *****************************************************************************/
PUBLIC void wm_term_puts(DESKTOP *desk, const char *s)
{
	if (!s)
		return;

	while (*s)
		wm_term_putc(desk, *s++);
}

/*****************************************************************************
 *                                wm_term_backspace
 *****************************************************************************
 * Erase the character left of the cursor. Stops at the start of the grid.
 *****************************************************************************/
PUBLIC void wm_term_backspace(DESKTOP *desk)
{
	if (desk->term.cur_col > 0) {
		desk->term.cur_col--;
	}
	else if (desk->term.cur_row > 0) {
		desk->term.cur_row--;
		desk->term.cur_col = WM_TERM_COLS - 1;
	}
	else {
		return;
	}

	desk->term.cells[desk->term.cur_row][desk->term.cur_col] = ' ';
}

/*****************************************************************************
 *                                wm_draw_terminal
 *****************************************************************************
 * Paint the terminal grid (and its cursor) inside a window's client area.
 *****************************************************************************/
PRIVATE void wm_draw_terminal(DESKTOP *desk, int win_id)
{
	WINDOW *win = &desk->windows[win_id];
	u8 *fb = desk->framebuffer;
	int x0 = win->x + WM_BORDER_WIDTH + WM_TERM_PAD;
	int y0 = win->y + WM_TITLE_HEIGHT + WM_TERM_PAD;
	int r, c;

	/* Dark client area: text terminals are not light grey. */
	wm_fill_rect(fb, win->x + WM_BORDER_WIDTH, win->y + WM_TITLE_HEIGHT,
	             win->x + win->width - WM_BORDER_WIDTH - 1,
	             win->y + win->height - WM_BORDER_WIDTH - 1,
	             WM_COLOR_TERM_BG);

	for (r = 0; r < WM_TERM_ROWS; r++) {
		int y = y0 + r * WM_TERM_CELL_H;

		/* Never draw a row that would leave the client area. */
		if (y + WM_TERM_CELL_H >
		    win->y + win->height - WM_BORDER_WIDTH)
			break;

		for (c = 0; c < WM_TERM_COLS; c++) {
			int x = x0 + c * WM_TERM_CELL_W;

			if (x + WM_TERM_CELL_W >
			    win->x + win->width - WM_BORDER_WIDTH)
				break;

			wm_draw_char(fb, x, y, desk->term.cells[r][c],
			             WM_COLOR_TERM_TEXT);
		}
	}

	/* Block cursor, only while the terminal window has the focus. */
	if (win_id == desk->active_window) {
		int cx = x0 + desk->term.cur_col * WM_TERM_CELL_W;
		int cy = y0 + desk->term.cur_row * WM_TERM_CELL_H;

		if (cx + WM_TERM_CELL_W <=
		    win->x + win->width - WM_BORDER_WIDTH &&
		    cy + WM_TERM_CELL_H <=
		    win->y + win->height - WM_BORDER_WIDTH)
			wm_fill_rect(fb, cx, cy + WM_TERM_CELL_H - 2,
			             cx + WM_TERM_CELL_W - 1,
			             cy + WM_TERM_CELL_H - 1,
			             WM_COLOR_TERM_TEXT);
	}
}

/*****************************************************************************
 *                                File explorer (Files window)
 *****************************************************************************
 * A second kind of window: a list of the files in the root directory (the
 * Orange'S filesystem is flat, so "explorer" means one scrollable list).
 * The DESKTOP task owns the filesystem access and fills expl.entries[];
 * this file only renders the list / the text view and turns mouse clicks
 * into row selections. Rows are one 8x16 glyph high; the selected row is
 * highlighted like the title bar.
 *****************************************************************************/

/* How many whole rows fit into the window's client area (list or view). */
PRIVATE int wm_expl_visible_rows(DESKTOP *desk)
{
	WINDOW *win = &desk->windows[desk->expl.win_id];
	int client_h = win->height - WM_TITLE_HEIGHT - WM_BORDER_WIDTH
	               - 2 * WM_EXPL_PAD;

	if (client_h <= 0)
		return 0;
	return client_h / WM_TERM_CELL_H;
}

/*****************************************************************************
 *                                wm_expl_scroll_to_cursor
 *****************************************************************************
 * Make sure the selected row is visible after a cursor move or a click.
 *****************************************************************************/
PUBLIC void wm_expl_scroll_to_cursor(DESKTOP *desk)
{
	int rows = wm_expl_visible_rows(desk);

	if (rows <= 0)
		return;
	if (desk->expl.cursor < desk->expl.scroll)
		desk->expl.scroll = desk->expl.cursor;
	if (desk->expl.cursor >= desk->expl.scroll + rows)
		desk->expl.scroll = desk->expl.cursor - rows + 1;
}

/*****************************************************************************
 *                                wm_expl_open
 *****************************************************************************
 * Create the Files window, attach it to the explorer state and focus it.
 *****************************************************************************/
PUBLIC int wm_expl_open(DESKTOP *desk, int x, int y, int w, int h,
                        const char *title)
{
	int id = wm_create_window(desk, x, y, w, h, title);

	if (id < 0)
		return -1;

	desk->expl.win_id = id;
	desk->expl.state = WM_EXPL_LIST;
	desk->expl.n_entries = 0;
	desk->expl.cursor = 0;
	desk->expl.scroll = 0;
	desk->expl.view_row = 0;
	desk->expl.view_len = 0;
	desk->expl.view_name[0] = 0;
	wm_focus_window(desk, id);

	return id;
}

/*****************************************************************************
 *                                wm_draw_expl_line
 *****************************************************************************
 * Paint one text row of the explorer in `color' at client text position
 * (tx, ty). The caller paints the selection background first.
 *****************************************************************************/
PRIVATE void wm_draw_expl_line(u8 *fb, int tx, int ty, const char *text,
                               u8 color, int max_chars)
{
	int i = 0;

	while (text[i] && i < max_chars) {
		wm_draw_char(fb, tx + i * WM_TERM_CELL_W, ty, text[i], color);
		i++;
	}
}

/* Format one list row: the name left-aligned, the size flush right. */
PRIVATE void wm_expl_format_row(EXPL_ENTRY *e, char *out, int out_sz,
                                int cols)
{
	char sizebuf[12];
	int sz = e->size, sl = 0, i, pos;

	if (sz <= 0) {
		sizebuf[sl++] = '-';
	} else {
		char tmp[12];
		int tl = 0;
		while (sz && tl < 11) {
			tmp[tl++] = '0' + (sz % 10);
			sz /= 10;
		}
		while (tl)
			sizebuf[sl++] = tmp[--tl];
	}
	sizebuf[sl] = 0;

	int name_len = 0;
	while (e->name[name_len])
		name_len++;

	/* The size keeps its own column; the name gets the rest of the row. */
	int name_cols = cols - sl - 1;      /* one space before the size */
	if (name_cols < 1)
		name_cols = 1;
	if (name_cols + sl + 1 >= out_sz)
		name_cols = out_sz - sl - 2;

	pos = 0;
	for (i = 0; i < name_cols && i < name_len && pos < out_sz - 1; i++)
		out[pos++] = e->name[i];
	while (pos < out_sz - 1 && pos < name_cols)
		out[pos++] = ' ';
	if (pos < out_sz - 1)
		out[pos++] = ' ';
	for (i = 0; i < sl && pos < out_sz - 1; i++)
		out[pos++] = sizebuf[i];
	out[pos] = 0;
}

/*****************************************************************************
 *                                wm_draw_expl_list
 *****************************************************************************
 * List mode: every visible entry, the selected one highlighted.
 *****************************************************************************/
PRIVATE void wm_draw_expl_list(DESKTOP *desk, WINDOW *win)
{
	u8 *fb = desk->framebuffer;
	int tx = win->x + WM_BORDER_WIDTH + WM_EXPL_PAD;
	int ty = win->y + WM_TITLE_HEIGHT + WM_EXPL_PAD;
	int cols = (win->width - 2 * WM_BORDER_WIDTH - 2 * WM_EXPL_PAD)
	           / WM_TERM_CELL_W;
	int max_rows = wm_expl_visible_rows(desk);
	int r;

	if (cols > 40)
		cols = 40;
	if (desk->expl.n_entries == 0) {
		wm_draw_expl_line(fb, tx, ty, "(root is empty)", WM_COLOR_BORDER,
		                  cols);
		return;
	}

	for (r = 0; r < max_rows; r++) {
		int idx = desk->expl.scroll + r;
		char rowbuf[64];
		int selected;

		if (idx >= desk->expl.n_entries)
			break;

		selected = (idx == desk->expl.cursor);
		if (selected) {
			/* Highlight the whole row, then paint white text. */
			wm_fill_rect(fb, tx - WM_EXPL_PAD, ty + r * WM_TERM_CELL_H,
			             win->x + win->width - WM_BORDER_WIDTH
			             - WM_EXPL_PAD,
			             ty + r * WM_TERM_CELL_H + WM_TERM_CELL_H - 1,
			             WM_COLOR_EXPL_SEL);
		}

		wm_expl_format_row(&desk->expl.entries[idx], rowbuf,
		                   sizeof(rowbuf), cols);
		wm_draw_expl_line(fb, tx, ty + r * WM_TERM_CELL_H, rowbuf,
		                  selected ? WM_COLOR_TITLE_TEXT : WM_COLOR_BORDER,
		                  cols);
	}
}

/*****************************************************************************
 *                                wm_draw_expl_view
 *****************************************************************************
 * View mode: show expl.view[] as wrapped text lines starting at view_row.
 *****************************************************************************/
PRIVATE void wm_draw_expl_view(DESKTOP *desk, WINDOW *win)
{
	u8 *fb = desk->framebuffer;
	int tx = win->x + WM_BORDER_WIDTH + WM_EXPL_PAD;
	int ty = win->y + WM_TITLE_HEIGHT + WM_EXPL_PAD;
	int cols = (win->width - 2 * WM_BORDER_WIDTH - 2 * WM_EXPL_PAD)
	           / WM_TERM_CELL_W;
	int max_rows = wm_expl_visible_rows(desk);
	int first = desk->expl.view_row;
	int line = 0;           /* current logical line */
	int col = 0;            /* characters on the current line */
	int i;

	if (cols > 60)
		cols = 60;

	if (desk->expl.view_len == 0) {
		wm_draw_expl_line(fb, tx, ty, "(no text)", WM_COLOR_BORDER, cols);
		return;
	}

	for (i = 0; i < desk->expl.view_len; i++) {
		char ch = desk->expl.view[i];

		if (ch == '\n') {
			line++;
			col = 0;
			continue;
		}

		/* Paint only the lines inside the window. */
		if (line >= first && line < first + max_rows)
			wm_draw_char(fb, tx + col * WM_TERM_CELL_W,
			             ty + (line - first) * WM_TERM_CELL_H,
			             ch, WM_COLOR_BORDER);

		col++;
		if (col >= cols) {      /* wrap long lines */
			line++;
			col = 0;
		}
	}
}

/*****************************************************************************
 *                                wm_draw_explorer
 *****************************************************************************
 * Paint the Files window content (list or text view).
 *****************************************************************************/
PRIVATE void wm_draw_explorer(DESKTOP *desk, int win_id)
{
	WINDOW *win = &desk->windows[win_id];

	if (desk->expl.state == WM_EXPL_VIEW)
		wm_draw_expl_view(desk, win);
	else
		wm_draw_expl_list(desk, win);
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
	desk->drag_window = -1;
	desk->drag_offset_x = 0;
	desk->drag_offset_y = 0;
	desk->active_window = -1;
	desk->running = 1;

	for (i = 0; i < WM_MAX_WINDOWS; i++) {
		desk->windows[i].state = WM_WINDOW_CLOSED;
		desk->windows[i].z_order = 0;
		desk->windows[i].content = 0;
	}

	desk->term.win_id = -1;
	wm_term_clear(desk);

	desk->expl.win_id = -1;
	desk->expl.state = WM_EXPL_LIST;
	desk->expl.n_entries = 0;
	desk->expl.cursor = 0;
	desk->expl.scroll = 0;
	desk->expl.view_row = 0;
	desk->expl.view_len = 0;
	desk->expl.view_name[0] = 0;
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
	/* New window lands on top; wm_restack keeps every z_order inside
	 * 1..WM_MAX_WINDOWS (slot indices would collide once a slot is reused
	 * after wm_close_window). */
	wm_restack(desk, i);

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
	desk->windows[win_id].z_order = 0;

	if (desk->active_window == win_id)
		desk->active_window = -1;

	/* A closed window can no longer host the terminal or the explorer. */
	if (desk->term.win_id == win_id)
		desk->term.win_id = -1;
	if (desk->expl.win_id == win_id)
		desk->expl.win_id = -1;

	/* Close the gap so the remaining z_orders stay 1..N. */
	wm_restack(desk, -1);
}

/*****************************************************************************
 *                                wm_draw_desktop
 *****************************************************************************
 * Draw the desktop background.
 *****************************************************************************/
PUBLIC void wm_draw_desktop(DESKTOP *desk)
{
	/* One flat colour for the whole background. The previous version
	 * added a "gradient" of WM_COLOR_DESKTOP + y/50, which in the VGA
	 * palette is blue, green, cyan and red -- four different colours
	 * instead of the single blue the desktop is supposed to have. */
	memset(desk->framebuffer, WM_COLOR_DESKTOP, GFX_FB_BYTES);
}

/*****************************************************************************
 *                                wm_draw_welcome
 *****************************************************************************
 * Boot splash: the plain blue desktop with one or two centred lines of text.
 * Shown right after the desktop starts, before the TTY window opens.
 * `subtitle' may be 0.
 *****************************************************************************/
PUBLIC void wm_draw_welcome(DESKTOP *desk, const char *title,
                            const char *subtitle)
{
	int len, x, y;

	wm_draw_desktop(desk);

	if (title) {
		for (len = 0; title[len]; len++)
			;
		x = (GFX_FB_W - len * WM_FONT_W) / 2;
		if (x < 0) x = 0;
		y = GFX_FB_H / 2 - WM_FONT_H;
		wm_draw_text(desk->framebuffer, x, y, title,
		             WM_COLOR_TITLE_TEXT, GFX_FB_W - x);
	}

	if (subtitle) {
		for (len = 0; subtitle[len]; len++)
			;
		x = (GFX_FB_W - len * WM_FONT_W) / 2;
		if (x < 0) x = 0;
		y = GFX_FB_H / 2 + WM_FONT_H / 2;
		wm_draw_text(desk->framebuffer, x, y, subtitle,
		             WM_COLOR_TITLE_TEXT, GFX_FB_W - x);
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

	/* Draw title text, clipped so it can never leave the title bar */
	wm_draw_text(fb, x + WM_BORDER_WIDTH + 2, y + WM_BORDER_WIDTH,
	             win->title, WM_COLOR_TITLE_TEXT,
	             w - 2 * WM_BORDER_WIDTH - 4);

	/* Draw content area */
	wm_fill_rect(fb, x + WM_BORDER_WIDTH, y + WM_TITLE_HEIGHT,
	             x + w - WM_BORDER_WIDTH - 1,
	             y + h - WM_BORDER_WIDTH - 1, WM_COLOR_WINDOW_BG);

	/* The terminal window paints its own (dark) client area and text. */
	if (win_id == desk->term.win_id)
		wm_draw_terminal(desk, win_id);

	/* The Files window paints its file list / text view. */
	if (win_id == desk->expl.win_id)
		wm_draw_explorer(desk, win_id);

	/* Bevel: a light inner edge on the top and left of the border, so
	 * overlapping windows are still distinguishable where they touch. */
	wm_hline(fb, x + 1, x + w - 2, y + 1, WM_COLOR_WINDOW_BG);
	wm_vline(fb, x + 1, y + 1, y + h - 2, WM_COLOR_WINDOW_BG);
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

	/* Start dragging when the left button is pressed on a title bar. */
	if ((buttons & 1) && !(desk->mouse_buttons & 1)) {
		wm_handle_click(desk, desk->mouse_x, desk->mouse_y);
		desk->drag_window = desk->active_window;
		if (desk->drag_window >= 0) {
			WINDOW *win = &desk->windows[desk->drag_window];
			if (desk->mouse_y < win->y + WM_TITLE_HEIGHT) {
				desk->drag_offset_x = desk->mouse_x - win->x;
				desk->drag_offset_y = desk->mouse_y - win->y;
			} else {
				desk->drag_window = -1;
			}
		}
	}

	/* Move the focused window while its title bar is held. */
	if ((buttons & 1) && desk->drag_window >= 0) {
		WINDOW *win = &desk->windows[desk->drag_window];
		win->x = desk->mouse_x - desk->drag_offset_x;
		win->y = desk->mouse_y - desk->drag_offset_y;
		if (win->x < 0) win->x = 0;
		if (win->y < 0) win->y = 0;
		if (win->x + win->width > GFX_FB_W) win->x = GFX_FB_W - win->width;
		if (win->y + win->height > GFX_FB_H) win->y = GFX_FB_H - win->height;
	}
	if (!(buttons & 1))
		desk->drag_window = -1;

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
 *                                wm_restack
 *****************************************************************************
 * Re-number the z_order of every NORMAL window so the values are exactly
 * 1..N, preserving their current relative stacking, and put `front' (>= 0)
 * on top.
 *
 * This matters: the renderer walks z = 1..WM_MAX_WINDOWS, so the old
 * "z_order = max_z + 1" grew without bound and after a handful of clicks the
 * focused window's z_order exceeded WM_MAX_WINDOWS and it simply stopped
 * being drawn -- the desktop looked frozen with a window missing.
 *****************************************************************************/
PRIVATE void wm_restack(DESKTOP *desk, int front)
{
	int order[WM_MAX_WINDOWS];
	int n = 0, i, j;

	if (front < 0 || front >= WM_MAX_WINDOWS ||
	    desk->windows[front].state != WM_WINDOW_NORMAL)
		front = -1;

	for (i = 0; i < WM_MAX_WINDOWS; i++)
		if (i != front && desk->windows[i].state == WM_WINDOW_NORMAL)
			order[n++] = i;

	/* Insertion sort by the current z_order (N <= WM_MAX_WINDOWS == 8). */
	for (i = 1; i < n; i++) {
		int key = order[i];
		int kz = desk->windows[key].z_order;
		for (j = i - 1; j >= 0 && desk->windows[order[j]].z_order > kz; j--)
			order[j + 1] = order[j];
		order[j + 1] = key;
	}

	for (i = 0; i < n; i++)
		desk->windows[order[i]].z_order = i + 1;

	if (front >= 0)
		desk->windows[front].z_order = n + 1;
}

/*****************************************************************************
 *                                wm_focus_window
 *****************************************************************************
 * Bring a window to front and give it focus.
 *****************************************************************************/
PUBLIC void wm_focus_window(DESKTOP *desk, int win_id)
{
	if (win_id < 0 || win_id >= WM_MAX_WINDOWS)
		return;

	if (desk->windows[win_id].state != WM_WINDOW_NORMAL)
		return;

	wm_restack(desk, win_id);
	desk->active_window = win_id;
}

/*****************************************************************************
 *                                wm_paint_all
 *****************************************************************************
 * Repaint everything: desktop background, then every NORMAL window back to
 * front, then the cursor. Windows are sorted here rather than by walking
 * z = 1..WM_MAX_WINDOWS, so the paint order stays correct even if a z_order
 * ever ends up out of that range.
 *****************************************************************************/
PUBLIC void wm_paint_all(DESKTOP *desk)
{
	int order[WM_MAX_WINDOWS];
	int n = 0, i, j;

	wm_draw_desktop(desk);

	for (i = 0; i < WM_MAX_WINDOWS; i++)
		if (desk->windows[i].state == WM_WINDOW_NORMAL)
			order[n++] = i;

	for (i = 1; i < n; i++) {
		int key = order[i];
		int kz = desk->windows[key].z_order;
		for (j = i - 1; j >= 0 && desk->windows[order[j]].z_order > kz; j--)
			order[j + 1] = order[j];
		order[j + 1] = key;
	}

	for (i = 0; i < n; i++)
		wm_draw_window(desk, order[i]);

	wm_draw_cursor(desk);
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

	/* A click inside the Files window selects the row under the cursor. */
	if (win_id >= 0 && win_id == desk->expl.win_id &&
	    desk->expl.state == WM_EXPL_LIST) {
		WINDOW *win = &desk->windows[win_id];
		int ty = win->y + WM_TITLE_HEIGHT + WM_EXPL_PAD;
		int rows = wm_expl_visible_rows(desk);
		int row;

		if (y >= ty && rows > 0) {
			row = (y - ty) / WM_TERM_CELL_H + desk->expl.scroll;
			if (row >= 0 && row < desk->expl.n_entries) {
				desk->expl.cursor = row;
				wm_expl_scroll_to_cursor(desk);
			}
		}
	}
}
