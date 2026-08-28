/*************************************************************************//**
 *****************************************************************************
 * @file   desktop.c
 * @brief  desktop_start() -- user-space wrapper for TASK_DESKTOP.
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


/*****************************************************************************
 *                                desktop_start
 *****************************************************************************
 * Ask TASK_DESKTOP to start the desktop environment. The calling process
 * sleeps until TASK_DESKTOP replies (after the desktop exits or ESC is pressed).
 *
 * @return 0 on success.
 *****************************************************************************/
PUBLIC int desktop_start(void)
{
	MESSAGE msg;
	msg.type = DESKTOP_START;

	send_recv(BOTH, TASK_DESKTOP, &msg);

	return 0;
}
