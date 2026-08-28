/*************************************************************************//**
 *****************************************************************************
 * @file   desktop.c
 * @brief  TASK_DESKTOP - Desktop shell with window manager
 *
 * Provides a graphical desktop environment with window management.
 * Runs in graphics mode and handles mouse/keyboard input for the GUI.
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
#include "keyboard.h"
#include "proto.h"
#include "wm.h"

/* Desktop state */
PRIVATE DESKTOP desktop;
PRIVATE u8 desktop_framebuffer[GFX_FB_BYTES];

/* VGA state save/restore (from gfx.c pattern) */
#define GFX_N_MISC	1
#define GFX_N_SEQ	5
#define GFX_N_CRTC	25
#define GFX_N_GC	9
#define GFX_N_AC	21
#define GFX_STATE_SZ	(GFX_N_MISC + GFX_N_SEQ + GFX_N_CRTC + GFX_N_GC + GFX_N_AC)

PRIVATE const u8 gfx_mode13h[GFX_STATE_SZ] = {
	0x63,
	0x03, 0x01, 0x0F, 0x00, 0x0E,
	0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28,
	0x40, 0x96, 0xB9, 0xA3, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x00, 0xFF,
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
	0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x41, 0x00, 0x0F, 0x00, 0x00
};

PRIVATE void vga_save_state(u8 *s);
PRIVATE void vga_restore_state(const u8 *s);
PRIVATE void vga_write_mode13(void);
PRIVATE void desktop_present(void);
PRIVATE void desktop_run(u8 *saved);
PRIVATE int desktop_poll_esc(void);

/*****************************************************************************
 *                                task_desktop
 *****************************************************************************
 * <Ring 1> Main loop of TASK_DESKTOP.
 *****************************************************************************/
PUBLIC void task_desktop()
{
	MESSAGE msg;
	u8 saved[GFX_STATE_SZ];

	while (1) {
		send_recv(RECEIVE, ANY, &msg);

		int src = msg.source;

		switch (msg.type) {
		case DESKTOP_START:
			desktop_run(saved);

			reset_msg(&msg);
			msg.type = DESKTOP_DONE;
			send_recv(SEND, src, &msg);
			break;
		default:
			dump_msg("DESKTOP::unknown msg", &msg);
			break;
		}
	}
}

/*****************************************************************************
 *                                desktop_run
 *****************************************************************************
 * Enter graphics mode, run the desktop environment, then restore text mode.
 *****************************************************************************/
PRIVATE void desktop_run(u8 *saved)
{
	int mx, my, buttons;
	int last_tick = get_ticks();
	int frame_count = 0;

	/* Switch to graphics mode */
	vga_save_state(saved);
	disable_int();
	vga_write_mode13();
	enable_int();

	/* Initialize desktop */
	wm_init(&desktop, desktop_framebuffer);

	/* Create some demo windows */
	wm_create_window(&desktop, 20, 20, 120, 80, "Welcome");
	wm_create_window(&desktop, 80, 50, 140, 100, "Window 1");
	wm_create_window(&desktop, 160, 80, 130, 90, "Demo");

	wm_focus_window(&desktop, 0);

	/* Force initial draw */
	wm_draw_desktop(&desktop);

	/* Draw windows in z-order */
	int z, i;
	for (z = 1; z <= WM_MAX_WINDOWS; z++) {
		for (i = 0; i < WM_MAX_WINDOWS; i++) {
			if (desktop.windows[i].state == WM_WINDOW_NORMAL &&
			    desktop.windows[i].z_order == z) {
				wm_draw_window(&desktop, i);
			}
		}
	}
	wm_draw_cursor(&desktop);
	desktop_present();

	/* Main desktop loop */
	while (desktop.running) {
		/* Get mouse state */
		mouse_get_state(&mx, &my, &buttons);
		desktop.mouse_x = mx;
		desktop.mouse_y = my;
		desktop.mouse_buttons = buttons;

		/* Check for left click */
		static int last_buttons = 0;
		if ((buttons & 1) && !(last_buttons & 1)) {
			wm_handle_click(&desktop, mx, my);
		}
		last_buttons = buttons;

		/* Redraw at ~20 FPS */
		if (get_ticks() - last_tick >= 2) {
			wm_draw_desktop(&desktop);

			/* Draw windows in z-order */
			for (z = 1; z <= WM_MAX_WINDOWS; z++) {
				for (i = 0; i < WM_MAX_WINDOWS; i++) {
					if (desktop.windows[i].state == WM_WINDOW_NORMAL &&
					    desktop.windows[i].z_order == z) {
						wm_draw_window(&desktop, i);
					}
				}
			}

			wm_draw_cursor(&desktop);
			desktop_present();

			last_tick = get_ticks();
			frame_count++;
		}

		/* Check for ESC to exit */
		if (desktop_poll_esc()) {
			desktop.running = 0;
		}
	}

	/* Restore text mode */
	disable_int();
	vga_restore_state(saved);
	enable_int();
}

/*****************************************************************************
 *                                desktop_poll_esc
 *****************************************************************************
 * Check if ESC was pressed.
 *****************************************************************************/
PRIVATE int desktop_poll_esc(void)
{
	MESSAGE msg;
	reset_msg(&msg);
	msg.type = TTY_POLL_KEY;
	send_recv(BOTH, TASK_TTY, &msg);
	return msg.RETVAL;
}

/*****************************************************************************
 *                                desktop_present
 *****************************************************************************
 * Copy framebuffer to VGA memory.
 *****************************************************************************/
PRIVATE void desktop_present(void)
{
	memcpy((void *)GFX_FB_BASE, desktop_framebuffer, GFX_FB_BYTES);
}

/*****************************************************************************
 *                                VGA state management
 *****************************************************************************/

PRIVATE void vga_save_state(u8 *s)
{
	int i;

	s[0] = in_byte(VGA_MISC_R);

	for (i = 0; i < GFX_N_SEQ; i++) {
		out_byte(VGA_SEQ_ADDR, i);
		s[GFX_N_MISC + i] = in_byte(VGA_SEQ_DATA);
	}

	for (i = 0; i < GFX_N_CRTC; i++) {
		out_byte(VGA_CRTC_ADDR, i);
		s[GFX_N_MISC + GFX_N_SEQ + i] = in_byte(VGA_CRTC_DATA);
	}

	for (i = 0; i < GFX_N_GC; i++) {
		out_byte(VGA_GC_ADDR, i);
		s[GFX_N_MISC + GFX_N_SEQ + GFX_N_CRTC + i] = in_byte(VGA_GC_DATA);
	}

	in_byte(VGA_AC_RDY);
	for (i = 0; i < GFX_N_AC; i++) {
		out_byte(VGA_AC_ADDR, i);
		s[GFX_N_MISC + GFX_N_SEQ + GFX_N_CRTC + GFX_N_GC + i] =
			in_byte(0x3C1);
	}
}

PRIVATE void vga_restore_state(const u8 *s)
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

	in_byte(VGA_AC_RDY);
	for (i = 0; i < GFX_N_AC; i++) {
		out_byte(VGA_AC_ADDR, i);
		out_byte(VGA_AC_ADDR,
			 s[GFX_N_MISC + GFX_N_SEQ + GFX_N_CRTC + GFX_N_GC + i]);
	}
}

PRIVATE void vga_write_mode13(void)
{
	const u8 *r = gfx_mode13h;
	int i;

	out_byte(VGA_MISC_W, *r++);

	for (i = 0; i < GFX_N_SEQ; i++) {
		out_byte(VGA_SEQ_ADDR, i);
		out_byte(VGA_SEQ_DATA, *r++);
	}

	for (i = 0; i < GFX_N_CRTC; i++) {
		out_byte(VGA_CRTC_ADDR, i);
		out_byte(VGA_CRTC_DATA, *r++);
	}

	for (i = 0; i < GFX_N_GC; i++) {
		out_byte(VGA_GC_ADDR, i);
		out_byte(VGA_GC_DATA, *r++);
	}

	in_byte(VGA_AC_RDY);
	for (i = 0; i < GFX_N_AC; i++) {
		out_byte(VGA_AC_ADDR, i);
		out_byte(VGA_AC_ADDR, *r++);
	}
}
