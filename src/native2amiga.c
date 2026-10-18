 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Call Amiga Exec functions outside the main UAE thread.
  *
  * Copyright 1999 Patrick Ohly
  *
  * Uses the EXTER interrupt that is setup in filesys.c
  * and needs thread support.
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "threaddep/thread.h"
#include "options.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "autoconf.h"
#include "traps.h"
#include "native2amiga.h"

smp_comm_pipe native2amiga_pending;
/*static*/ uae_sem_t n2asem;

/*
 * to be called when setting up the hardware
 */

/****i* januae-native2amiga/native2amiga_install ***************************
*
*   NAME
*       native2amiga_install() -- Initialize the host-to-guest notification
*       channel.
*
*   SYNOPSIS
*       void native2amiga_install (void);
*
*   FUNCTION
*       Create the pending communication pipe and initialize its access
*       semaphore to one.
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

void native2amiga_install (void)
{
    init_comm_pipe (&native2amiga_pending, 10, 2);
    uae_sem_init (&n2asem, 0, 1);
}

/*
 * to be called when the Amiga boots, i.e. by filesys_diagentry()
 */
/****i* januae-native2amiga/native2amiga_startup ***************************
*
*   NAME
*       native2amiga_startup() -- Provide the guest-boot notification hook.
*
*   SYNOPSIS
*       void native2amiga_startup (void);
*
*   FUNCTION
*       Return without altering the pending communication channel or guest
*       state.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The active implementation is empty.
*
****************************************************************************
*
*/

void native2amiga_startup (void)
{
}

#ifdef SUPPORT_THREADS

/****i* januae-native2amiga/uae_Cause **************************************
*
*   NAME
*       uae_Cause() -- Queue a guest interrupt request from a host thread.
*
*   SYNOPSIS
*       void uae_Cause (uaecptr interrupt);
*
*   FUNCTION
*       Serialize the interrupt-cause command and interrupt address into the
*       pending pipe, request emulator interrupt handling, and release the
*       pipe semaphore.
*
*   INPUTS
*       interrupt - Guest interrupt address.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Available only with SUPPORT_THREADS.
*
****************************************************************************
*
*/

void uae_Cause (uaecptr interrupt)
{
    uae_sem_wait (&n2asem);
    write_comm_pipe_int (&native2amiga_pending, 3, 0);
    write_comm_pipe_u32 (&native2amiga_pending, interrupt, 1);
    do_uae_int_requested ();
    uae_sem_post (&n2asem);
}

/****i* januae-native2amiga/uae_ReplyMsg ***********************************
*
*   NAME
*       uae_ReplyMsg() -- Queue a guest message reply from a host thread.
*
*   SYNOPSIS
*       void uae_ReplyMsg (uaecptr msg);
*
*   FUNCTION
*       Place the reply-message command and guest message address in the
*       pending pipe under the shared semaphore, then request interrupt
*       processing.
*
*   INPUTS
*       msg - Guest address of the message to reply.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Available only with SUPPORT_THREADS.
*
****************************************************************************
*
*/

void uae_ReplyMsg (uaecptr msg)
{
    uae_sem_wait (&n2asem);
    write_comm_pipe_int (&native2amiga_pending, 2, 0);
    write_comm_pipe_u32 (&native2amiga_pending, msg, 1);
    do_uae_int_requested ();
    uae_sem_post (&n2asem);
}

/****i* januae-native2amiga/uae_PutMsg *************************************
*
*   NAME
*       uae_PutMsg() -- Queue delivery of a message to a guest message port.
*
*   SYNOPSIS
*       void uae_PutMsg (uaecptr port, uaecptr msg);
*
*   FUNCTION
*       Write the message-port operation, guest port address, and guest
*       message address to the pending pipe while holding its semaphore,
*       then signal pending interrupt work.
*
*   INPUTS
*       port - Guest message-port address; msg - Guest message address.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Available only with SUPPORT_THREADS.
*
****************************************************************************
*
*/

void uae_PutMsg (uaecptr port, uaecptr msg)
{
    uae_sem_wait (&n2asem);
    write_comm_pipe_int (&native2amiga_pending, 1, 0);
    write_comm_pipe_u32 (&native2amiga_pending, port, 0);
    write_comm_pipe_u32 (&native2amiga_pending, msg, 1);
    do_uae_int_requested ();
    uae_sem_post (&n2asem);
}

/****i* januae-native2amiga/uae_Signal *************************************
*
*   NAME
*       uae_Signal() -- Queue an Exec signal for delivery in guest context.
*
*   SYNOPSIS
*       void uae_Signal (uaecptr task, uae_u32 mask);
*
*   FUNCTION
*       Enqueue the guest task address and signal mask under the
*       pending-pipe semaphore and request emulator interrupt handling.
*
*   INPUTS
*       task - Guest task address; mask - Signal bits to deliver.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Available only with SUPPORT_THREADS.
*
****************************************************************************
*
*/

void uae_Signal (uaecptr task, uae_u32 mask)
{
    uae_sem_wait (&n2asem);
    write_comm_pipe_int (&native2amiga_pending, 0, 0);
    write_comm_pipe_u32 (&native2amiga_pending, task, 0);
    write_comm_pipe_int (&native2amiga_pending, mask, 1);
    do_uae_int_requested ();
    uae_sem_post (&n2asem);
}

/****i* januae-native2amiga/uae_NotificationHack ***************************
*
*   NAME
*       uae_NotificationHack() -- Queue a guest filesystem notification
*       request.
*
*   SYNOPSIS
*       void uae_NotificationHack (uaecptr port, uaecptr nr);
*
*   FUNCTION
*       Write the notification command, port address, and notification
*       identifier to the shared pipe and request emulator interrupt
*       processing.
*
*   INPUTS
*       port - Guest notification-port address; nr - Guest notification
*       identifier.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Available only with SUPPORT_THREADS.
*
****************************************************************************
*
*/

void uae_NotificationHack (uaecptr port, uaecptr nr)
{
    uae_sem_wait (&n2asem);
    write_comm_pipe_int (&native2amiga_pending, 4, 0);
    write_comm_pipe_int (&native2amiga_pending, port, 0);
    write_comm_pipe_int (&native2amiga_pending, nr, 1);
    do_uae_int_requested ();
    uae_sem_post (&n2asem);
}

#endif

/****i* januae-native2amiga/uae_NewList ************************************
*
*   NAME
*       uae_NewList() -- Initialize the linkage fields of a guest Exec list.
*
*   SYNOPSIS
*       void uae_NewList (uaecptr list);
*
*   FUNCTION
*       Write the empty-list head, tail, and tail-predecessor pointers into
*       the guest list structure.
*
*   INPUTS
*       list - Writable guest address of the Exec list header.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void uae_NewList (uaecptr list)
{
    put_long (list, list + 4);
    put_long (list + 4, 0);
    put_long (list + 8, list);
}

/****i* januae-native2amiga/uae_AllocMem ***********************************
*
*   NAME
*       uae_AllocMem() -- Allocate memory through guest Exec.
*
*   SYNOPSIS
*       uaecptr uae_AllocMem (TrapContext *context, uae_u32 size, uae_u32
*       flags);
*
*   FUNCTION
*       Place the requested byte count and allocation flags in guest D0 and
*       D1, then call Exec AllocMem through the extended trap interface.
*
*   INPUTS
*       context - Extended trap context; size - Requested byte count; flags
*       - Exec allocation flags.
*
*   RESULT
*       Guest memory address returned by AllocMem.
*
****************************************************************************
*
*/

uaecptr uae_AllocMem (TrapContext *context, uae_u32 size, uae_u32 flags)
{
    m68k_dreg (&context->regs, 0) = size;
    m68k_dreg (&context->regs, 1) = flags;
    return CallLib (context, get_long (4), -198); /* AllocMem */
}

/****i* januae-native2amiga/uae_FreeMem ************************************
*
*   NAME
*       uae_FreeMem() -- Release memory through guest Exec.
*
*   SYNOPSIS
*       void uae_FreeMem (TrapContext *context, uaecptr memory, uae_u32
*       size);
*
*   FUNCTION
*       Place the allocation size in guest D0 and the guest memory address
*       in A1, then call Exec FreeMem through the extended trap interface.
*
*   INPUTS
*       context - Extended trap context; memory - Guest memory address; size
*       - Allocation size.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void uae_FreeMem (TrapContext *context, uaecptr memory, uae_u32 size)
{
    m68k_dreg (&context->regs, 0) = size;
    m68k_areg (&context->regs, 1) = memory;
    CallLib (context, get_long (4), -0xD2); /* FreeMem */
}
