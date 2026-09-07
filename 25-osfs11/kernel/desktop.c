/*************************************************************************//**
 *****************************************************************************
 * @file   desktop.c
 * @brief  TASK_DESKTOP - Desktop shell with window manager
 *
 * Provides a graphical desktop environment with window management.
 * Runs in graphics mode and handles mouse/keyboard input for the GUI.
 *
 * Startup sequence: the flat blue desktop shows a single welcome message,
 * then one window opens -- the TTY terminal. Everything typed while the
 * desktop owns the screen is line-edited in that window (see term_exec()
 * for the commands it understands); ESC closes the desktop.
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

/* Terminal line editing: the characters typed after the prompt. */
#define TERM_PROMPT		"$ "
#define TERM_PROMPT_LEN		2
#define TERM_INPUT_MAX		(WM_TERM_COLS - TERM_PROMPT_LEN - 1)

PRIVATE char term_input[TERM_INPUT_MAX + 1];
PRIVATE int  term_input_len;

/* How long the welcome screen stays up before the TTY window opens
 * (ticks, HZ per second). Any key skips the rest of the wait. */
#define WELCOME_TICKS		(HZ * 2)

PRIVATE void desktop_present(void);
PRIVATE int desktop_run(void);
PRIVATE int desktop_poll_esc(void);
PRIVATE void desktop_welcome(void);
PRIVATE void term_start(void);
PRIVATE void term_prompt(void);
PRIVATE void term_key(char ch);
PRIVATE void term_exec(char *line);
PRIVATE void term_put_int(int value);

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
 * Enter graphics mode, show the welcome screen, open the TTY window, run the
 * desktop, then restore text mode.
 *****************************************************************************/
PRIVATE int desktop_run(void)
{
	int mx, my, buttons, ch;
	int last_tick;

	if (vga_enter_graphics() != 0)
		return -1;
	/* An ESC pressed in the shell must not immediately close a new GUI,
	 * and neither must the command line that launched it end up in the
	 * terminal window. */
	desktop_poll_esc();
	tty_gui_flush();

	/* Initialize desktop */
	wm_init(&desktop, vga_framebuffer);

	/* Startup: only the welcome message, then the TTY window. */
	desktop_welcome();
	if (!desktop.running) {		/* ESC during the welcome screen */
		vga_leave_graphics();
		return 0;
	}

	term_start();

	/* Force initial draw */
	wm_paint_all(&desktop);
	desktop_present();
	last_tick = get_ticks();

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

		/* Everything typed while the desktop owns the screen goes to
		 * the terminal window. */
		while ((ch = tty_gui_getchar()) >= 0)
			term_key((char)ch);

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
 *                                desktop_welcome
 *****************************************************************************
 * The only thing on screen right after startup: a welcome message on the
 * plain blue desktop. It stays for WELCOME_TICKS or until a key is pressed;
 * ESC closes the desktop straight away.
 *****************************************************************************/
PRIVATE void desktop_welcome(void)
{
	int start = get_ticks();

	wm_draw_welcome(&desktop, "Welcome to noxisOS", 0);
	desktop_present();

	while (get_ticks() - start < WELCOME_TICKS) {
		if (desktop_poll_esc()) {
			desktop.running = 0;
			return;
		}
		if (tty_gui_getchar() >= 0)	/* any key skips the wait */
			break;
	}

	tty_gui_flush();
}

/*****************************************************************************
 *                                term_start
 *****************************************************************************
 * Open the TTY window and print its banner and first prompt.
 *****************************************************************************/
PRIVATE void term_start(void)
{
	if (wm_term_open(&desktop, "TTY") < 0)
		return;

	wm_term_puts(&desktop, "noxisOS terminal\n");
	wm_term_puts(&desktop, "type HELP, ESC closes the desktop\n");
	term_prompt();
}

/*****************************************************************************
 *                                term_prompt
 *****************************************************************************/
PRIVATE void term_prompt(void)
{
	term_input_len = 0;
	term_input[0] = 0;
	wm_term_puts(&desktop, TERM_PROMPT);
}

/*****************************************************************************
 *                                term_key
 *****************************************************************************
 * One character typed into the terminal window.
 *****************************************************************************/
PRIVATE void term_key(char ch)
{
	if (desktop.term.win_id < 0)
		return;

	if (ch == '\n') {
		wm_term_putc(&desktop, '\n');
		term_input[term_input_len] = 0;
		term_exec(term_input);
		if (desktop.running)
			term_prompt();
		return;
	}

	if (ch == '\b') {
		if (term_input_len > 0) {
			term_input_len--;
			term_input[term_input_len] = 0;
			wm_term_backspace(&desktop);
		}
		return;
	}

	if (ch < 32 || ch > 126)
		return;

	if (term_input_len >= TERM_INPUT_MAX)	/* line full: ignore */
		return;

	term_input[term_input_len++] = ch;
	wm_term_putc(&desktop, ch);
}

/*****************************************************************************
 *                                term_put_int
 *****************************************************************************
 * Print a non-negative decimal number in the terminal (itoa() is hex only).
 *****************************************************************************/
PRIVATE void term_put_int(int value)
{
	char digits[12];
	int n = 0;

	if (value < 0) {
		wm_term_putc(&desktop, '-');
		value = -value;
	}

	do {
		digits[n++] = '0' + (value % 10);
		value /= 10;
	} while (value && n < (int)sizeof(digits));

	while (n--)
		wm_term_putc(&desktop, digits[n]);
}

/*****************************************************************************
 *                                term_exec
 *****************************************************************************
 * Run one terminal command line. Commands are matched case-insensitively.
 *****************************************************************************/
PRIVATE void term_exec(char *line)
{
	char cmd[TERM_INPUT_MAX + 1];
	int i = 0, n = 0;

	while (line[i] == ' ')			/* leading blanks */
		i++;

	while (line[i] && line[i] != ' ' && n < TERM_INPUT_MAX) {
		char c = line[i++];
		if (c >= 'A' && c <= 'Z')	/* fold to lower case */
			c += 'a' - 'A';
		cmd[n++] = c;
	}
	cmd[n] = 0;

	while (line[i] == ' ')			/* start of the arguments */
		i++;

	if (cmd[0] == 0)
		return;

	if (strcmp(cmd, "help") == 0) {
		wm_term_puts(&desktop, "help clear echo ver uptime exit\n");
	}
	else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
		wm_term_clear(&desktop);
	}
	else if (strcmp(cmd, "echo") == 0) {
		wm_term_puts(&desktop, line + i);
		wm_term_putc(&desktop, '\n');
	}
	else if (strcmp(cmd, "ver") == 0 || strcmp(cmd, "version") == 0) {
		wm_term_puts(&desktop, "noxisOS desktop, TTY window\n");
	}
	else if (strcmp(cmd, "uptime") == 0) {
		wm_term_puts(&desktop, "up ");
		term_put_int(get_ticks() / HZ);
		wm_term_puts(&desktop, " s\n");
	}
	else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
		desktop.running = 0;
	}
	else {
		wm_term_puts(&desktop, "unknown: ");
		wm_term_puts(&desktop, cmd);
		wm_term_putc(&desktop, '\n');
	}
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
