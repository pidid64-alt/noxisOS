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
#define WM_TITLE_HEIGHT     12
#define WM_BORDER_WIDTH     2
#define WM_MIN_WIDTH        60
#define WM_MIN_HEIGHT       40

/* Window states */
#define WM_WINDOW_CLOSED    0
#define WM_WINDOW_NORMAL    1
#define WM_WINDOW_MINIMIZED 2
#define WM_WINDOW_MAXIMIZED 3

/* Window colors (VGA palette indices) */
#define WM_COLOR_DESKTOP    1   /* blue background */
#define WM_COLOR_BORDER     8   /* dark grey */
#define WM_COLOR_TITLEBAR   9   /* light blue */
#define WM_COLOR_TITLE_TEXT 15  /* white */
#define WM_COLOR_WINDOW_BG  7   /* light grey */
#define WM_COLOR_SHADOW     0   /* black */

/* Mouse cursor */
#define WM_CURSOR_WIDTH     8
#define WM_CURSOR_HEIGHT    11

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
	u8 *framebuffer;       /* pointer to graphics buffer */
	int running;           /* 1 if desktop is active */
} DESKTOP;

/* Window manager functions */
PUBLIC void wm_init(DESKTOP *desk, u8 *fb);
PUBLIC int wm_create_window(DESKTOP *desk, int x, int y, int w, int h, const char *title);
PUBLIC void wm_close_window(DESKTOP *desk, int win_id);
PUBLIC void wm_draw_desktop(DESKTOP *desk);
PUBLIC void wm_draw_window(DESKTOP *desk, int win_id);
PUBLIC void wm_draw_cursor(DESKTOP *desk);
PUBLIC void wm_update_mouse(DESKTOP *desk, int dx, int dy, int buttons);
PUBLIC void wm_handle_click(DESKTOP *desk, int x, int y);
PUBLIC void wm_focus_window(DESKTOP *desk, int win_id);
PUBLIC int wm_hit_test(DESKTOP *desk, int x, int y);

/* PS/2 Mouse driver functions */
PUBLIC void mouse_init(void);
PUBLIC void mouse_handler(int irq);
PUBLIC void mouse_get_state(int *x, int *y, int *buttons);

#endif /* _WM_H_ */
