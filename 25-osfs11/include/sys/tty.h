
/*++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
				tty.h
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
						    Forrest Yu, 2005
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/

#ifndef _ORANGES_TTY_H_
#define _ORANGES_TTY_H_


#define TTY_IN_BYTES		256	/* tty input queue size */
#define TTY_OUT_BUF_LEN		2	/* tty output buffer size */

/* Navigation keys translated for the GUI owner (the desktop). They are
 * delivered as ASCII control characters that normal typing never produces
 * (keyboard.c maps letters, digits, space, '\n' and '\b' only). */
#define GUI_KEY_UP	0x01	/* Up arrow      */
#define GUI_KEY_DOWN	0x02	/* Down arrow    */
#define GUI_KEY_PGUP	0x03	/* Page Up       */
#define GUI_KEY_PGDN	0x04	/* Page Down     */
#define GUI_KEY_HOME	0x05	/* Home          */
#define GUI_KEY_END	0x06	/* End           */

struct s_tty;
struct s_console;

/* TTY */
typedef struct s_tty
{
	u32	ibuf[TTY_IN_BYTES];	/* TTY input buffer */
	u32*	ibuf_head;		/* the next free slot */
	u32*	ibuf_tail;		/* the val to be processed by TTY */
	int	ibuf_cnt;		/* how many */

	int	tty_caller;
	int	tty_procnr;
	void*	tty_req_buf;
	int	tty_left_cnt;
	int	tty_trans_cnt;

	struct s_console *	console;
}TTY;

#endif /* _ORANGES_TTY_H_ */
