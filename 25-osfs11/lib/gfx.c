/*************************************************************************//**
 *****************************************************************************
 * @file   gfx.c
 * @brief  gfx_run() -- user-space wrapper for the TASK_GFX demo.
 * @author noxisOS
 * @date   2026-08-27
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
 *                                gfx_run
 *****************************************************************************
 * Ask TASK_GFX to run the graphics demo. The calling process sleeps until
 * TASK_GFX replies (after the demo ends or ESC is pressed).
 *
 * @return 0 on success.
 *****************************************************************************/
PUBLIC int gfx_run(void)
{
	MESSAGE msg;
	msg.type = GFX_RUN;

	send_recv(BOTH, TASK_GFX, &msg);

	return 0;
}
