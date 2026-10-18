/*
 *  uaeexe.c - UAE remote cli
 *
 *  (c) 1997 by Samuel Devulder
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "autoconf.h"
#include "traps.h"
#include "uaeexe.h"

static struct uae_xcmd *first = NULL;
static struct uae_xcmd *last  = NULL;
static char running = 0;
static uae_u32 uaeexe_server (TrapContext *context) REGPARAM;

/*
 * Install the server
 */
/****i* januae-uaeexe/uaeexe_install ***************************************
*
*   NAME
*       uaeexe_install() -- Install the guest-facing remote-command trap.
*
*   SYNOPSIS
*       void uaeexe_install (void);
*
*   FUNCTION
*       Write a call to the remote-command handler and an RTS instruction at
*       UAEEXE_ORG, then restore the previous runtime-area assembly
*       position.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void uaeexe_install (void)
{
    uaecptr loop;

    loop = here ();
    org (UAEEXE_ORG);
    calltrap (deftrap (uaeexe_server));
    dw (RTS);
    org (loop);
}

/*
 * Send command to the remote cli.
 *
 * To use this, just call uaeexe("command") and the command will be
 * executed by the remote cli (provided you've started it in the
 * s:user-startup for example). Be sure to add "run" if you want
 * to launch the command asynchronously. Please note also that the
 * remote cli works better if you've got the fifo-handler installed.
 */
/****i* januae-uaeexe/uaeexe ***********************************************
*
*   NAME
*       uaeexe() -- Queue a command for execution by the guest remote CLI.
*
*   SYNOPSIS
*       int uaeexe (const char *cmd);
*
*   FUNCTION
*       Copy the command into a newly allocated linked-list entry and append
*       it to the pending queue if the remote CLI has registered itself.
*
*   INPUTS
*       cmd - Zero-terminated command text to enqueue.
*
*   RESULT
*       UAEEXE_OK when queued, UAEEXE_NOMEM on allocation failure, or
*       UAEEXE_NOTRUNNING when the server is inactive.
*
*   NOTES
*       The command remains queued until the guest-side server retrieves it.
*
****************************************************************************
*
*/

int uaeexe (const char *cmd)
{
    struct uae_xcmd *nw;

    if (!running)
	goto NORUN;

    nw = (struct uae_xcmd *) malloc (sizeof *nw);
    if (!nw)
	goto NOMEM;
    nw->cmd = (char *) malloc (strlen (cmd) + 1);
    if (!nw->cmd) {
	free (nw);
	goto NOMEM;
    }

    strcpy (nw->cmd, cmd);
    nw->prev = last;
    nw->next = NULL;

    if (!first)
	first      = nw;
    if (last) {
	last->next = nw;
	last       = nw;
    } else
	last       = nw;

    return UAEEXE_OK;
  NOMEM:
    return UAEEXE_NOMEM;
  NORUN:
    write_log ("Remote cli is not running.\n");
    return UAEEXE_NOTRUNNING;
}

/*
 * returns next command to be executed
 */
/****i* januae-uaeexe/get_cmd **********************************************
*
*   NAME
*       get_cmd() -- Remove the oldest pending remote command.
*
*   SYNOPSIS
*       static char *get_cmd (void);
*
*   FUNCTION
*       Unlink the head of the remote-command queue, retain ownership of its
*       command text, and release the queue node.
*
*   INPUTS
*       None.
*
*   RESULT
*       Owned command text, or NULL when the queue is empty.
*
*   NOTES
*       The caller is responsible for freeing the returned text.
*
****************************************************************************
*
*/

static char *get_cmd (void)
{
    struct uae_xcmd *cmd;
    char *s;

    if (!first)
	return NULL;
    s = first->cmd;
    cmd = first;
    first = first->next;
    if (!first)
	last = NULL;
    free (cmd);
    return s;
}

/*
 * helper function
 */
#define ARG(x) (get_long (m68k_areg (&context->regs, 7) + 4*(x+1)))
/****i* januae-uaeexe/uaeexe_server ****************************************
*
*   NAME
*       uaeexe_server() -- Deliver a queued command to the guest CLI.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 uaeexe_server (TrapContext *context);
*
*   FUNCTION
*       Recognize an active guest command buffer, dequeue the next command,
*       and copy it into the guest buffer up to the supplied length.
*
*   INPUTS
*       context - 68k trap context; the guest stack supplies the destination
*       address and maximum length.
*
*   RESULT
*       Guest destination address when a command is delivered; otherwise
*       zero.
*
*   NOTES
*       The first nonzero command-buffer address marks the remote CLI as
*       running.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 uaeexe_server (TrapContext *context)
{
    int len;
    char *cmd;
    char *dst;

    if (ARG(0) && !running) {
	running = 1;
	write_log ("Remote CLI started.\n");
    }

    cmd = get_cmd ();
    if (!cmd)
	return 0;
    if (!ARG(0)) {
	running = 0;
	return 0;
    }

    dst = (char *) get_real_address (ARG (0));
    len = ARG (1);
    strncpy (dst, cmd, len);
    write_log ("Sending '%s' to remote cli\n", cmd);
    free (cmd);
    return ARG (0);
}
