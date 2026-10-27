/*************************************************************************//**
 *****************************************************************************
 * @file   desktop.c
 * @brief  TASK_DESKTOP - Desktop shell with window manager
 *
 * Provides a graphical desktop environment with window management.
 * Runs in 800x600x8 graphics mode and handles mouse/keyboard input for the
 * GUI.
 *
 * Startup sequence: the flat blue desktop shows a single welcome message,
 * then two windows open side by side -- the "Files" explorer (root
 * directory listing with a read-only text viewer) and the TTY terminal.
 * Everything typed while the desktop owns the screen goes to the focused
 * window: text is line-edited in the TTY window (see term_exec() for the
 * commands it understands), while arrows/Enter steer the file explorer.
 * ESC closes the desktop (or steps out of a file view first).
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

/* How long the welcome screen stays up before the windows open
 * (ticks, HZ per second). Any key skips the rest of the wait. */
#define WELCOME_TICKS		(HZ * 2)

/* Echo requests per `ping' (TASK_NET accepts up to 4). */
#define PING_REPLY_MAX		4

/* Raw directory-read scratch space: one sector more than the biggest root
 * directory we can ever show (WM_EXPL_MAX_ENTRIES x 16 bytes). */
#define EXPL_RAW_MAX		(WM_EXPL_MAX_ENTRIES * 16 + 512)
PRIVATE char expl_raw[EXPL_RAW_MAX];

PRIVATE void desktop_present(void);
PRIVATE int desktop_run(void);
PRIVATE int desktop_poll_esc(void);
PRIVATE void desktop_welcome(void);
PRIVATE void term_start(void);
PRIVATE void term_prompt(void);
PRIVATE void term_key(char ch);
PRIVATE void term_exec(char *line);
PRIVATE void term_put_int(int value);
PRIVATE void term_ping(const char *arg);
PRIVATE void term_rm(const char *name);
PRIVATE void term_write(const char *file, const char *text);
PRIVATE int  term_parse_ip(const char *s, u32 *out);
PRIVATE void term_puts_row(const EXPL_ENTRY *e);

PRIVATE int  expl_refresh(void);
PRIVATE void expl_open_view(int idx);
PRIVATE void expl_key(char ch);
PRIVATE int  expl_window_rows(void);
PRIVATE int  expl_view_lines(void);

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
 * Enter graphics mode, show the welcome screen, open the Files + TTY
 * windows, run the desktop, then restore text mode.
 *****************************************************************************/
PRIVATE int desktop_run(void)
{
	int mx, my, buttons, ch;
	int last_tick;
	int term_id, expl_id;

	if (vga_enter_graphics() != 0)
		return -1;
	/* An ESC pressed in the shell must not immediately close a new GUI,
	 * and neither must the command line that launched it end up in the
	 * terminal window. */
	desktop_poll_esc();
	tty_gui_flush();

	/* Initialize desktop */
	wm_init(&desktop, vga_framebuffer);

	/* Startup: only the welcome message, then the two windows. */
	desktop_welcome();
	if (!desktop.running) {		/* ESC during the welcome screen */
		vga_leave_graphics();
		return 0;
	}

	/* Files explorer on the left, TTY terminal next to it. */
	expl_id = wm_expl_open(&desktop, WM_EXPL_WIN_X, WM_EXPL_WIN_Y,
	                       WM_EXPL_WIN_W, WM_EXPL_WIN_H, "Files: /");
	expl_refresh();
	term_id = wm_term_open(&desktop, "TTY");
	if (expl_id >= 0)
		wm_focus_window(&desktop, expl_id);
	term_start();
	if (term_id >= 0)
		wm_focus_window(&desktop, term_id);

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

		/* Every key goes to the focused window: navigation keys and
		 * Enter steer the Files window, everything else is text for
		 * the terminal. */
		while ((ch = tty_gui_getchar()) >= 0) {
			if (desktop.expl.win_id >= 0 &&
			    desktop.active_window == desktop.expl.win_id)
				expl_key((char)ch);
			else
				term_key((char)ch);
		}

		/* Redraw at ~50 FPS (two ticks at HZ=100). */
		if (get_ticks() - last_tick >= 2) {
			wm_paint_all(&desktop);
			desktop_present();
			last_tick = get_ticks();
		}

		/* ESC: step out of a file view first, then close the desktop. */
		if (desktop_poll_esc()) {
			if (desktop.expl.state == WM_EXPL_VIEW &&
			    desktop.expl.win_id == desktop.active_window) {
				desktop.expl.state = WM_EXPL_LIST;
			} else {
				desktop.running = 0;
			}
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
 * Print the banner and the first prompt into the terminal window.
 *****************************************************************************/
PRIVATE void term_start(void)
{
	wm_term_puts(&desktop, "noxisOS terminal\n");
	wm_term_puts(&desktop, "help: commands  ls: files  esc: close\n");
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
 *                                term_puts_row
 *****************************************************************************
 * Print one explorer row: name left, size right, kind column in between.
 *****************************************************************************/
PRIVATE void term_puts_row(const EXPL_ENTRY *e)
{
	int pad, i;

	for (i = 0; e->name[i] && i < 12; i++)
		wm_term_putc(&desktop, e->name[i]);
	for (pad = i; pad < 13; pad++)
		wm_term_putc(&desktop, ' ');

	wm_term_putc(&desktop, e->kind);
	for (pad = 0; pad < 4; pad++)
		wm_term_putc(&desktop, ' ');

	term_put_int(e->size);
	wm_term_putc(&desktop, '\n');
}

/*****************************************************************************
 *                                term_cat
 *****************************************************************************
 * `cat <file>' implementation: dump a regular file, printable bytes only,
 * capped so the small terminal grid cannot be flooded.
 *****************************************************************************/
#define TERM_CAT_MAX	(WM_TERM_COLS * (WM_TERM_ROWS - 2))

PRIVATE void term_cat(const char *name)
{
	char path[WM_EXPL_NAME_LEN + 1];
	char buf[512];
	struct stat st;
	int fd, got, i, shown = 0, k;

	if (name[0] == 0) {
		wm_term_puts(&desktop, "usage: cat <file>\n");
		return;
	}

	path[0] = '/';
	for (k = 0; name[k] && k < WM_EXPL_NAME_LEN - 1; k++)
		path[k + 1] = name[k];
	path[k + 1] = 0;

	if (stat(path, &st) != 0 ||
	    (st.st_mode & I_TYPE_MASK) != I_REGULAR) {
		wm_term_puts(&desktop, "cat: not a file: ");
		wm_term_puts(&desktop, name);
		wm_term_putc(&desktop, '\n');
		return;
	}

	fd = open(path, O_RDWR);
	if (fd < 0) {
		wm_term_puts(&desktop, "cat: cannot open: ");
		wm_term_puts(&desktop, name);
		wm_term_putc(&desktop, '\n');
		return;
	}

	while ((got = read(fd, buf, sizeof(buf))) > 0 && shown < TERM_CAT_MAX) {
		for (i = 0; i < got; i++) {
			char c = buf[i];

			if (c == '\n') {
				wm_term_putc(&desktop, '\n');
				shown++;
			} else if (c == '\t') {
				wm_term_putc(&desktop, ' ');
				shown++;
			} else if (c >= 32 && c <= 126) {
				wm_term_putc(&desktop, c);
				shown++;
			} else if (c == '\r') {
				/* skip carriage returns */
			} else {
				wm_term_putc(&desktop, '.');
				shown++;
			}
		}
	}
	close(fd);

	if (shown >= TERM_CAT_MAX)
		wm_term_puts(&desktop, "\n[...truncated]\n");
}

/*****************************************************************************
 *                                term_parse_ip
 *****************************************************************************
 * Parse "a.b.c.d" into a host-order address. Returns 0 on success.
 *****************************************************************************/
PRIVATE int term_parse_ip(const char *s, u32 *out)
{
	u32 ip = 0;
	int part;

	if (s == 0)
		return -1;
	for (part = 0; part < 4; part++) {
		int v = 0, digits = 0;

		while (*s >= '0' && *s <= '9') {
			v = v * 10 + (*s - '0');
			s++;
			digits++;
			if (digits > 3 || v > 255)
				return -1;
		}
		if (digits == 0)
			return -1;
		ip = (ip << 8) | (u32)v;
		if (part < 3) {
			if (*s != '.')
				return -1;
			s++;
		}
	}
	if (*s != 0)
		return -1;
	*out = ip;
	return 0;
}

/*****************************************************************************
 *                                term_ping
 *****************************************************************************
 * `ping <ip|name>': resolve through TASK_NET (DNS via slirp), then send
 * four ICMP echoes and print the round-trip times. The desktop task
 * blocks in IPC while TASK_NET drives the NIC.
 *****************************************************************************/
PRIVATE void term_ping(const char *arg)
{
	static u32 rtts[PING_REPLY_MAX];
	MESSAGE msg;
	u32 ip = 0;
	char ipbuf[16];
	int i, received;

	if (arg[0] == 0) {
		wm_term_puts(&desktop, "usage: ping <a.b.c.d | name>\n");
		return;
	}

	if (term_parse_ip(arg, &ip) != 0) {
		reset_msg(&msg);
		msg.type = NET_RESOLVE;
		msg.PATHNAME = (void *)arg;
		send_recv(BOTH, TASK_NET, &msg);
		if (msg.RETVAL != 0) {
			wm_term_puts(&desktop, "cannot resolve: ");
			wm_term_puts(&desktop, (char *)arg);
			wm_term_putc(&desktop, '\n');
			return;
		}
		ip = (u32)msg.DEVICE;
	}

	ip_to_str(ip, ipbuf);
	wm_term_puts(&desktop, "PING ");
	wm_term_puts(&desktop, (char *)arg);
	wm_term_puts(&desktop, " (");
	wm_term_puts(&desktop, ipbuf);
	wm_term_puts(&desktop, "): 4 requests\n");

	reset_msg(&msg);
	msg.type = NET_PING;
	msg.u.m3.m3i1 = (int)ip;
	msg.u.m3.m3i2 = PING_REPLY_MAX;
	msg.BUF = rtts;
	send_recv(BOTH, TASK_NET, &msg);

	if (msg.u.m3.m3i1 < 0) {
		wm_term_puts(&desktop, "no ARP response from ");
		wm_term_puts(&desktop, ipbuf);
		wm_term_putc(&desktop, '\n');
		return;
	}

	received = msg.u.m3.m3i1;
	for (i = 0; i < PING_REPLY_MAX; i++) {
		if (rtts[i] == 0xFFFFFFFF) {
			wm_term_puts(&desktop, "timeout for icmp_seq ");
			term_put_int(i + 1);
			wm_term_putc(&desktop, '\n');
		} else {
			wm_term_puts(&desktop, "reply from ");
			wm_term_puts(&desktop, ipbuf);
			wm_term_puts(&desktop, ": icmp_seq=");
			term_put_int(i + 1);
			wm_term_puts(&desktop, " time=");
			term_put_int((int)rtts[i]);
			wm_term_puts(&desktop, " ms\n");
		}
	}
	term_put_int(received);
	wm_term_puts(&desktop, " of 4 received\n");
}

/*****************************************************************************
 *                                term_rm
 *****************************************************************************
 * `rm <file>': unlink and refresh the explorer.
 *****************************************************************************/
PRIVATE void term_rm(const char *name)
{
	char path[WM_EXPL_NAME_LEN + 2];
	int k;

	if (name[0] == 0) {
		wm_term_puts(&desktop, "usage: rm <file>\n");
		return;
	}

	path[0] = '/';
	for (k = 0; name[k] && k < WM_EXPL_NAME_LEN - 1; k++)
		path[k + 1] = name[k];
	path[k + 1] = 0;

	if (unlink(path) != 0) {
		wm_term_puts(&desktop, "rm: cannot remove: ");
		wm_term_puts(&desktop, (char *)name);
		wm_term_putc(&desktop, '\n');
		return;
	}
	wm_term_puts(&desktop, "removed ");
	wm_term_puts(&desktop, (char *)name);
	wm_term_putc(&desktop, '\n');
	expl_refresh();
}

/*****************************************************************************
 *                                term_write
 *****************************************************************************
 * `write <file> <text>': create (or truncate) a file and store the text.
 *****************************************************************************/
PRIVATE void term_write(const char *file, const char *text)
{
	char path[WM_EXPL_NAME_LEN + 2];
	int fd, k, len = 0;

	if (file[0] == 0) {
		wm_term_puts(&desktop, "usage: write <file> <text>\n");
		return;
	}

	path[0] = '/';
	for (k = 0; file[k] && k < WM_EXPL_NAME_LEN - 1; k++)
		path[k + 1] = file[k];
	path[k + 1] = 0;

	while (text[len])
		len++;

	fd = open(path, O_CREAT | O_RDWR);
	if (fd < 0) {
		wm_term_puts(&desktop, "write: cannot open: ");
		wm_term_puts(&desktop, (char *)file);
		wm_term_putc(&desktop, '\n');
		return;
	}
	if (write(fd, text, len) != len)
		wm_term_puts(&desktop, "write: short write\n");
	close(fd);

	wm_term_puts(&desktop, "wrote ");
	term_put_int(len);
	wm_term_puts(&desktop, " bytes to ");
	wm_term_puts(&desktop, (char *)file);
	wm_term_putc(&desktop, '\n');
	expl_refresh();
}

/*****************************************************************************
 *                                term_exec
 *****************************************************************************
 * Run one terminal command line. Commands are matched case-insensitively.
 *****************************************************************************/
PRIVATE void term_exec(char *line)
{
	char cmd[TERM_INPUT_MAX + 1];
	char arg[TERM_INPUT_MAX + 1];
	char rest[TERM_INPUT_MAX + 1];
	int i = 0, n = 0, a = 0, r = 0;

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

	while (line[i] && line[i] != ' ' && a < TERM_INPUT_MAX)
		arg[a++] = line[i++];
	arg[a] = 0;

	while (line[i] == ' ')			/* rest of the line */
		i++;
	while (line[i] && r < TERM_INPUT_MAX)
		rest[r++] = line[i++];
	rest[r] = 0;

	if (cmd[0] == 0)
		return;

	if (strcmp(cmd, "help") == 0) {
		wm_term_puts(&desktop,
		             "help clear echo ver uptime exit\n");
		wm_term_puts(&desktop,
		             "ls | dir      list files\n");
		wm_term_puts(&desktop,
		             "cat <file>    show a file\n");
		wm_term_puts(&desktop,
		             "write <f> <t> create/write a file
");
		wm_term_puts(&desktop,
		             "rm <file>     delete a file
");
		wm_term_puts(&desktop,
		             "ping <ip|name> 4 icmp echoes
");
		wm_term_puts(&desktop,
		             "end | poweroff  shut down\n");
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
	else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0) {
		if (expl_refresh() < 0) {
			wm_term_puts(&desktop, "ls: cannot read /\n");
		} else {
			int k;
			wm_term_puts(&desktop, "/\n");
			for (k = 0; k < desktop.expl.n_entries; k++)
				term_puts_row(&desktop.expl.entries[k]);
		}
	}
	else if (strcmp(cmd, "cat") == 0 || strcmp(cmd, "type") == 0) {
		term_cat(arg);
	}
	else if (strcmp(cmd, "write") == 0) {
		term_write(arg, rest);
	}
	else if (strcmp(cmd, "rm") == 0 || strcmp(cmd, "del") == 0) {
		term_rm(arg);
	}
	else if (strcmp(cmd, "ping") == 0) {
		term_ping(arg);
	}
	else if (strcmp(cmd, "end") == 0 ||
	         strcmp(cmd, "poweroff") == 0) {
		wm_term_puts(&desktop, "powering off...\n");
		desktop_present();
		power_off();		/* does not return */
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
 *                                expl_window_rows
 *****************************************************************************
 * How many whole glyph rows the Files window can show (mirrors wm.c).
 *****************************************************************************/
PRIVATE int expl_window_rows(void)
{
	WINDOW *win;

	if (desktop.expl.win_id < 0)
		return 0;
	win = &desktop.windows[desktop.expl.win_id];

	return (win->height - WM_TITLE_HEIGHT - WM_BORDER_WIDTH
	        - 2 * WM_EXPL_PAD) / WM_TERM_CELL_H;
}

/*****************************************************************************
 *                                expl_view_lines
 *****************************************************************************
 * Number of logical (wrapped) lines in the current file view.
 *****************************************************************************/
PRIVATE int expl_view_lines(void)
{
	WINDOW *win;
	int cols, lines = 1, col = 0, i;

	if (desktop.expl.win_id < 0)
		return 1;
	win = &desktop.windows[desktop.expl.win_id];
	cols = (win->width - 2 * WM_BORDER_WIDTH - 2 * WM_EXPL_PAD)
	       / WM_TERM_CELL_W;
	if (cols > 60)
		cols = 60;
	if (cols < 1)
		cols = 1;

	for (i = 0; i < desktop.expl.view_len; i++) {
		if (desktop.expl.view[i] == '\n') {
			lines++;
			col = 0;
		} else {
			col++;
			if (col >= cols) {
				lines++;
				col = 0;
			}
		}
	}
	return lines;
}

/*****************************************************************************
 *                                expl_refresh
 *****************************************************************************
 * Re-read the root directory and fill desktop.expl.entries[] (sorted by
 * name). Returns the number of entries, or -1 when "/" cannot be read.
 *****************************************************************************/
PRIVATE int expl_refresh(void)
{
	int fd, got, i, n = 0;

	fd = open("/", O_RDWR);
	if (fd < 0)
		return -1;

	got = read(fd, expl_raw, sizeof(expl_raw));
	close(fd);

	desktop.expl.n_entries = 0;
	if (got <= 0)
		return 0;

	for (i = 0; i + 16 <= got && n < WM_EXPL_MAX_ENTRIES; i += 16) {
		struct dir_entry *de = (struct dir_entry *)(expl_raw + i);
		struct stat st;
		EXPL_ENTRY *e;
		char path[WM_EXPL_NAME_LEN + 1];
		int k, ok;

		if (de->inode_nr <= 0)
			continue;		/* empty slot */
		if (de->name[0] == '.')		/* skip "." (root) */
			continue;

		e = &desktop.expl.entries[n];
		e->inode = de->inode_nr;
		e->size = 0;
		e->kind = '-';

		for (k = 0; k < WM_EXPL_NAME_LEN - 1 && de->name[k]; k++)
			e->name[k] = de->name[k];
		e->name[k] = 0;

		/* Ask the filesystem for the size and the type. */
		path[0] = '/';
		strcpy(path + 1, e->name);
		ok = stat(path, &st);
		if (ok == 0) {
			e->size = st.st_size;
			switch (st.st_mode & I_TYPE_MASK) {
			case I_DIRECTORY:
				e->kind = 'd';
				break;
			case I_CHAR_SPECIAL:
				e->kind = 'c';
				break;
			case I_BLOCK_SPECIAL:
				e->kind = 'b';
				break;
			default:
				e->kind = 'f';
				break;
			}
		}
		n++;
	}

	desktop.expl.n_entries = n;

	/* Stable insertion sort: directories first, then by name. */
	for (i = 1; i < n; i++) {
		EXPL_ENTRY key = desktop.expl.entries[i];
		int j = i - 1;
		int kr = (key.kind == 'd') ? 0 : 1;
		while (j >= 0) {
			EXPL_ENTRY *t = &desktop.expl.entries[j];
			int tr = (t->kind == 'd') ? 0 : 1;
			int c = tr - kr;

			if (c == 0)
				c = strcmp(t->name, key.name);
			if (c <= 0)
				break;
			desktop.expl.entries[j + 1] = *t;
			j--;
		}
		desktop.expl.entries[j + 1] = key;
	}

	if (desktop.expl.cursor >= n)
		desktop.expl.cursor = n > 0 ? n - 1 : 0;
	wm_expl_scroll_to_cursor(&desktop);

	return n;
}

/*****************************************************************************
 *                                expl_open_view
 *****************************************************************************
 * Read the selected regular file into the viewer (printable text only).
 *****************************************************************************/
PRIVATE void expl_open_view(int idx)
{
	EXPLORER *e = &desktop.expl;
	EXPL_ENTRY *ent;
	char path[WM_EXPL_NAME_LEN + 1];
	int fd, got, i, w = 0;

	if (idx < 0 || idx >= e->n_entries)
		return;
	ent = &e->entries[idx];

	if (ent->kind != 'f') {
		e->state = WM_EXPL_LIST;
		return;
	}

	path[0] = '/';
	strcpy(path + 1, ent->name);

	fd = open(path, O_RDWR);
	if (fd < 0) {
		e->state = WM_EXPL_LIST;
		return;
	}

	got = read(fd, e->view, WM_EXPL_VIEW_MAX);
	close(fd);
	if (got < 0)
		got = 0;

	/* Sanitize in place: keep text, skip CR, map other bytes to '.'. */
	for (i = 0; i < got; i++) {
		char c = e->view[i];
		if (c == '\n') {
			e->view[w++] = '\n';
		} else if (c == '\t') {
			e->view[w++] = ' ';
		} else if (c == '\r') {
			/* skip */
		} else if (c >= 32 && c <= 126) {
			e->view[w++] = c;
		} else {
			e->view[w++] = '.';
		}
	}
	e->view_len = w;

	if (ent->size > e->view_len && w < WM_EXPL_VIEW_MAX) {
		e->view[w++] = '\n';
		if (w < WM_EXPL_VIEW_MAX) {
			e->view[w++] = '[';
			e->view[w++] = 't';
			e->view[w++] = 'r';
			e->view[w++] = 'u';
			e->view[w++] = 'n';
			e->view[w++] = 'c';
			e->view[w++] = 'a';
			e->view[w++] = 't';
			e->view[w++] = 'e';
			e->view[w++] = 'd';
			e->view[w++] = ']';
			e->view_len = w;
		}
	}

	for (i = 0; ent->name[i] && i < WM_EXPL_NAME_LEN - 1; i++)
		e->view_name[i] = ent->name[i];
	e->view_name[i] = 0;
	e->view_row = 0;
	e->state = WM_EXPL_VIEW;
}

/*****************************************************************************
 *                                expl_key
 *****************************************************************************
 * One navigation key for the focused Files window.
 *****************************************************************************/
PRIVATE void expl_key(char ch)
{
	EXPLORER *e = &desktop.expl;
	int rows = expl_window_rows();

	if (rows < 1)
		rows = 1;

	if (e->state == WM_EXPL_VIEW) {
		int lines = expl_view_lines();
		int max_row = lines - rows;
		if (max_row < 0)
			max_row = 0;

		switch (ch) {
		case GUI_KEY_UP:
			if (e->view_row > 0)
				e->view_row--;
			break;
		case GUI_KEY_DOWN:
			if (e->view_row < max_row)
				e->view_row++;
			break;
		case GUI_KEY_PGUP:
			e->view_row -= rows - 1;
			if (e->view_row < 0)
				e->view_row = 0;
			break;
		case GUI_KEY_PGDN:
			e->view_row += rows - 1;
			if (e->view_row > max_row)
				e->view_row = max_row;
			break;
		case GUI_KEY_HOME:
			e->view_row = 0;
			break;
		case GUI_KEY_END:
			e->view_row = max_row;
			break;
		case '\n':
		case '\b':
		case 'q':
			e->state = WM_EXPL_LIST;	/* back to the list */
			break;
		default:
			break;
		}
		return;
	}

	/* List mode */
	switch (ch) {
	case GUI_KEY_UP:
		if (e->cursor > 0)
			e->cursor--;
		break;
	case GUI_KEY_DOWN:
		if (e->cursor < e->n_entries - 1)
			e->cursor++;
		break;
	case GUI_KEY_PGUP:
		e->cursor -= rows - 1;
		if (e->cursor < 0)
			e->cursor = 0;
		break;
	case GUI_KEY_PGDN:
		e->cursor += rows - 1;
		if (e->cursor >= e->n_entries)
			e->cursor = e->n_entries - 1;
		break;
	case GUI_KEY_HOME:
		e->cursor = 0;
		break;
	case GUI_KEY_END:
		e->cursor = e->n_entries - 1;
		break;
	case GUI_KEY_DEL: {
		EXPL_ENTRY *ent;
		char path[WM_EXPL_NAME_LEN + 2];
		int k;

		if (e->cursor < 0 || e->cursor >= e->n_entries)
			break;
		ent = &e->entries[e->cursor];
		path[0] = '/';
		for (k = 0; ent->name[k] && k < WM_EXPL_NAME_LEN - 1; k++)
			path[k + 1] = ent->name[k];
		path[k + 1] = 0;
		if (unlink(path) == 0)
			expl_refresh();
		break;
	}
		expl_open_view(e->cursor);
		break;
	default:
		break;
	}
	wm_expl_scroll_to_cursor(&desktop);
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
 * Copy the framebuffer through the banked VBE aperture.
 *****************************************************************************/
PRIVATE void desktop_present(void)
{
	vga_blit();
}
