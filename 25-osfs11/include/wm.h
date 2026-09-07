/*************************************************************************//**
 *****************************************************************************
 * @file   wm.h
 * @brief  Window Manager - minimal desktop environment for noxisOS
 *
 * Provides a basic window manager with mouse support, window rendering,
 * and desktop shell functionality for VGA mode 13h (320x200).
 *
 * @author noxisOS
 * @date   2026-08-28
 *****************************************************************************
 *****************************************************************************/

#ifndef _WM_H_
#define _WM_H_

#include "type.h"

/* Window manager constants */
#define WM_MAX_WINDOWS      8
#define WM_TITLE_HEIGHT     18   /* fits one 8x16 glyph row (see kernel/wm_font.h) */
#define WM_BORDER_WIDTH     2
#define WM_MIN_WIDTH        60
#define WM_MIN_HEIGHT       48   /* >= TITLE_HEIGHT + 2*BORDER + a usable client area */

/* Window states */
#define WM_WINDOW_CLOSED    0
#define WM_WINDOW_NORMAL    1
#define WM_WINDOW_MINIMIZED 2
#define WM_WINDOW_MAXIMIZED 3

/* Window colors (VGA palette indices).
 *
 * The desktop is painted with a single flat colour: WM_COLOR_DESKTOP for
 * every pixel. There is no gradient and no second/third hue -- the old
 * "WM_COLOR_DESKTOP + y/50" ramp walked into palette entries 2 (green),
 * 3 (cyan) and 4 (red), which is exactly what we do not want here. */
#define WM_COLOR_DESKTOP    1   /* blue background (the whole desktop) */
#define WM_COLOR_BORDER     8   /* dark grey */
#define WM_COLOR_TITLEBAR   9   /* light blue */
#define WM_COLOR_TITLE_TEXT 15  /* white */
#define WM_COLOR_WINDOW_BG  7   /* light grey */
#define WM_COLOR_SHADOW     0   /* black */
#define WM_COLOR_TERM_BG    0   /* black -- terminal client area */
#define WM_COLOR_TERM_TEXT  15  /* white -- terminal text and cursor */

/* Mouse cursor */
#define WM_CURSOR_WIDTH     8
#define WM_CURSOR_HEIGHT    11

/* Terminal (TTY) window ---------------------------------------------------
 *
 * One window may host a text terminal. Its character cell is the size of a
 * glyph of the shared 8x16 font (WM_FONT_W/WM_FONT_H in kernel/wm_font.h);
 * the values are mirrored here so wm.h stays free of font internals. */
#define WM_TERM_CELL_W      8
#define WM_TERM_CELL_H      16
#define WM_TERM_COLS        36
#define WM_TERM_ROWS        9
#define WM_TERM_PAD         2   /* pixels between the client edge and text */

/* Outer window size that makes the client area fit COLS x ROWS cells. */
#define WM_TERM_WIN_W  (WM_TERM_COLS * WM_TERM_CELL_W + \
                        2 * WM_BORDER_WIDTH + 2 * WM_TERM_PAD)
#define WM_TERM_WIN_H  (WM_TERM_ROWS * WM_TERM_CELL_H + WM_TITLE_HEIGHT + \
                        WM_BORDER_WIDTH + 2 * WM_TERM_PAD)

/* Terminal text buffer: a fixed grid of characters plus a cursor. */
typedef struct s_terminal {
	char cells[WM_TERM_ROWS][WM_TERM_COLS];
	int  cur_col, cur_row;   /* cursor cell */
	int  win_id;             /* hosting window, -1 when there is none */
} TERMINAL;

/* Window structure */
typedef struct s_window {
	int x, y;              /* position on screen */
	int width, height;     /* dimensions */
	int state;             /* WM_WINDOW_* */
	int z_order;           /* stacking order (higher = on top) */
	char title[32];        /* window title */
	u8 *content;           /* window content buffer (optional) */
} WINDOW;

/* Desktop manager structure */
typedef struct s_desktop {
	WINDOW windows[WM_MAX_WINDOWS];
	int active_window;     /* index of focused window, -1 if none */
	int mouse_x, mouse_y;  /* mouse cursor position */
	int mouse_buttons;     /* button state: bit 0=left, 1=right, 2=middle */
	int drag_window;       /* window being dragged, or -1 */
	int drag_offset_x;     /* cursor offset from window origin */
	int drag_offset_y;
	u8 *framebuffer;       /* pointer to graphics buffer */
	int running;           /* 1 if desktop is active */
	TERMINAL term;         /* the TTY window's text buffer */
} DESKTOP;

/* Window manager functions */
PUBLIC void wm_init(DESKTOP *desk, u8 *fb);
PUBLIC int wm_create_window(DESKTOP *desk, int x, int y, int w, int h, const char *title);
PUBLIC void wm_close_window(DESKTOP *desk, int win_id);
PUBLIC void wm_draw_desktop(DESKTOP *desk);
PUBLIC void wm_draw_welcome(DESKTOP *desk, const char *title, const char *subtitle);
PUBLIC void wm_draw_window(DESKTOP *desk, int win_id);
PUBLIC void wm_paint_all(DESKTOP *desk);
PUBLIC void wm_draw_cursor(DESKTOP *desk);
PUBLIC void wm_update_mouse(DESKTOP *desk, int dx, int dy, int buttons);
PUBLIC void wm_handle_click(DESKTOP *desk, int x, int y);
PUBLIC void wm_focus_window(DESKTOP *desk, int win_id);
PUBLIC int wm_hit_test(DESKTOP *desk, int x, int y);

/* Terminal (TTY window) functions */
PUBLIC int  wm_term_open(DESKTOP *desk, const char *title);
PUBLIC void wm_term_clear(DESKTOP *desk);
PUBLIC void wm_term_putc(DESKTOP *desk, char c);
PUBLIC void wm_term_puts(DESKTOP *desk, const char *s);
PUBLIC void wm_term_backspace(DESKTOP *desk);

/* PS/2 Mouse driver functions */
PUBLIC void mouse_init(void);
PUBLIC void mouse_handler(int irq);
PUBLIC void mouse_get_state(int *x, int *y, int *buttons);

#endif /* _WM_H_ */
