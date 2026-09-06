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
#include "vga.h"

/* Desktop state */
PRIVATE DESKTOP desktop;

PRIVATE void desktop_present(void);
PRIVATE int desktop_run(void);
PRIVATE int desktop_poll_esc(void);

/*****************************************************************************
 *                                task_desktop
 *****************************************************************************
 * <Ring 1> Main loop of TASK_DESKTOP.
 *****************************************************************************/
PUBLIC void task_desktop()
{
	MESSAGE msg;

	while (1) {
		send_recv(RECEIVE, ANY, &msg);

		int src = msg.source;

		switch (msg.type) {
		case DESKTOP_START: {
			int result = desktop_run();

			reset_msg(&msg);
			msg.type = DESKTOP_DONE;
			msg.RETVAL = result;
			send_recv(SEND, src, &msg);
			break;
		}
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
PRIVATE int desktop_run(void)
{
	int mx, my, buttons;
	int last_tick = get_ticks();

	if (vga_enter_graphics() != 0)
		return -1;
	/* An ESC pressed in the shell must not immediately close a new GUI. */
	desktop_poll_esc();

	/* Initialize desktop */
	wm_init(&desktop, vga_framebuffer);

	/* Create some demo windows */
	wm_create_window(&desktop, 20, 20, 120, 90, "Welcome");
	wm_create_window(&desktop, 80, 50, 140, 110, "Window 1");
	wm_create_window(&desktop, 160, 80, 130, 100, "Demo");

	wm_focus_window(&desktop, 0);

	/* Force initial draw */
	wm_paint_all(&desktop);
	desktop_present();

	/* Main desktop loop */
	while (desktop.running) {
		/* Feed the driver's absolute position into the WM as a delta.
		 * wm_update_mouse() also owns click-edge detection, so the
		 * button state no longer lives in a function-static here --
		 * it used to survive across desktop sessions, which made the
		 * first click after a restart get swallowed. */
		mouse_get_state(&mx, &my, &buttons);
		wm_update_mouse(&desktop,
		                mx - desktop.mouse_x,
		                my - desktop.mouse_y,
		                buttons);

		/* Redraw at ~50 FPS (two ticks at HZ=100). */
		if (get_ticks() - last_tick >= 2) {
			wm_paint_all(&desktop);
			desktop_present();
			last_tick = get_ticks();
		}

		/* Check for ESC to exit */
		if (desktop_poll_esc()) {
			desktop.running = 0;
		}
	}

	vga_leave_graphics();
	return 0;
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
	memcpy((void *)GFX_FB_BASE, vga_framebuffer, GFX_FB_BYTES);
}
