/*************************************************************************//**
 *****************************************************************************
 * @file   wm.h
 * @brief  Window Manager - minimal desktop environment for noxisOS
 *
 * Provides a basic window manager with mouse support, window rendering,
 * and desktop shell functionality for the 800x600 VBE framebuffer.
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
#define WM_COLOR_EXPL_SEL   9   /* light blue -- selected explorer row */

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
#define WM_TERM_COLS        56
#define WM_TERM_ROWS        24
#define WM_TERM_PAD         2   /* pixels between the client edge and text */

/* Outer window size that makes the client area fit COLS x ROWS cells. */
#define WM_TERM_WIN_W  (WM_TERM_COLS * WM_TERM_CELL_W + \
                        2 * WM_BORDER_WIDTH + 2 * WM_TERM_PAD)
#define WM_TERM_WIN_H  (WM_TERM_ROWS * WM_TERM_CELL_H + WM_TITLE_HEIGHT + \
                        WM_BORDER_WIDTH + 2 * WM_TERM_PAD)

/* Default layout on the 800x600 desktop: the Files explorer is docked on
 * the left, the TTY window next to it (see desktop.c). */
#define WM_TERM_WIN_X   (GFX_FB_W - WM_TERM_WIN_W - 16)
#define WM_TERM_WIN_Y   8
#define WM_EXPL_WIN_X   8
#define WM_EXPL_WIN_Y   8
#define WM_EXPL_WIN_W   288
#define WM_EXPL_WIN_H   (GFX_FB_H - 16)

/* Terminal text buffer: a fixed grid of characters plus a cursor. */
typedef struct s_terminal {
	char cells[WM_TERM_ROWS][WM_TERM_COLS];
	int  cur_col, cur_row;   /* cursor cell */
	int  win_id;             /* hosting window, -1 when there is none */
} TERMINAL;

/* File explorer -----------------------------------------------------------
 *
 * A second kind of desktop window: a list of the files in the root
 * directory (the Orange'S filesystem is flat -- there are no subfolders).
 * The DESKTOP task fills the entries (it owns the FS access); the window
 * manager only renders them and translates mouse clicks into a selection.
 * Pressing Enter in list mode switches to a read-only text view of the
 * selected regular file (expl.state == WM_EXPL_VIEW). */

#define WM_EXPL_LIST        0   /* explorer shows the file list */
#define WM_EXPL_VIEW        1   /* explorer shows a file's contents */
#define WM_EXPL_MAX_ENTRIES 64
#define WM_EXPL_NAME_LEN    13  /* 12 chars + NUL (FS filenames) */
#define WM_EXPL_VIEW_MAX    4096
#define WM_EXPL_PAD         2   /* pixels around the client text */

typedef struct s_expl_entry {
	int  inode;
	int  size;
	char kind;              /* 'f' regular, 'd' dir, 'c' char dev, '-' */
	char name[WM_EXPL_NAME_LEN];
} EXPL_ENTRY;

typedef struct s_explorer {
	int  win_id;             /* hosting window, -1 when there is none */
	int  state;              /* WM_EXPL_LIST or WM_EXPL_VIEW */
	int  n_entries;          /* valid entries[] */
	int  cursor;             /* selected row in list mode */
	int  scroll;             /* first visible row in list mode */
	int  view_row;           /* first visible line in view mode */
	int  view_len;           /* bytes stored in view[] */
	char view_name[WM_EXPL_NAME_LEN]; /* file being viewed */
	char view[WM_EXPL_VIEW_MAX];       /* sanitised file contents */
	EXPL_ENTRY entries[WM_EXPL_MAX_ENTRIES];
} EXPLORER;

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
	EXPLORER expl;         /* the Files window */
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

/* File-explorer window functions */
PUBLIC int  wm_expl_open(DESKTOP *desk, int x, int y, int w, int h,
                         const char *title);
PUBLIC void wm_expl_scroll_to_cursor(DESKTOP *desk); /* keep the row visible */

/* PS/2 Mouse driver functions */
PUBLIC void mouse_init(void);
PUBLIC void mouse_handler(int irq);
PUBLIC void mouse_get_state(int *x, int *y, int *buttons);

#endif /* _WM_H_ */
