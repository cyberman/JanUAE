/************************************************************************ 
 *
 * Copyright 2009 Oliver Brunner - aros<at>oliver-brunner.de
 *
 * This file is part of Janus-UAE.
 *
 * Janus-UAE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Janus-UAE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Janus-UAE. If not, see <http://www.gnu.org/licenses/>.
 *
 * This file is based on a parts of cbio.c, (c) 1992 Commodore-Amiga.
 *
 * $Id$
 *
 ************************************************************************/

#include "sysconfig.h"
#include "sysdeps.h"

#include <stdlib.h>
#include <stdarg.h>
#include <devices/clipboard.h>
#include <exec/devices.h>
#include <exec/io.h>

#include <clib/alib_protos.h>

#include "j-clipboard.h"

#include "threaddep/thread.h"
#include "memory.h"
#if 0
#include "native2amiga.h"
#else
void uae_Signal(uaecptr task, uae_u32 mask);
#endif
#include "j.h"

//#define DEBOUT_ENABLED 1
#if DEBOUT_ENABLED
#define DebOut(...) do { kprintf("%s: %s():%d: ",__FILE__,__func__,__LINE__);kprintf(__VA_ARGS__); } while(0)
#else
#define DebOut(...) do { ; } while(0)
#endif

static struct IOClipReq *ior=NULL;
static struct Hook ClipHook;

/****i* januae/CBOpen ******************************************************
*
*   NAME
*       CBOpen() -- open a clipboard device unit.
*
*   SYNOPSIS
*       struct IOClipReq *CBOpen(ULONG unit);
*
*   FUNCTION
*       Allocate a reply port and I/O request, then open the specified
*       clipboard.device unit. Release both resources if opening fails.
*
*   INPUTS
*       unit - Clipboard device unit to open.
*
*   RESULT
*       Returns an allocated, open I/O request, or NULL if a port, request,
*       or device could not be obtained. The caller owns the request and its
*       associated resources.
*
*   NOTES
*       On success, release the request with CBClose(). The implementation
*       does not validate a numerical range for unit.
*
****************************************************************************
*
*/
struct IOClipReq *CBOpen(ULONG unit) {
  struct MsgPort *mp;
  struct IORequest *ior;

  if ((mp = CreatePort(0L,0L))) {
    if ((ior=(struct IORequest *)CreateExtIO(mp,sizeof(struct IOClipReq)))) {
      if (!(OpenDevice("clipboard.device", unit, ior, 0L))) {
	return((struct IOClipReq *)ior);
      }
      DeleteExtIO(ior);
    }
  DeletePort(mp);
  }

  DebOut("ERROR opening clipboard.device\n");
  return(NULL);
}

/****i* januae/CBClose *****************************************************
*
*   NAME
*       CBClose() -- release a clipboard device request.
*
*   SYNOPSIS
*       static void CBClose(struct IOClipReq *ior);
*
*   FUNCTION
*       Close an opened clipboard request, free the I/O request, and delete
*       its reply port.
*
*   INPUTS
*       ior - Request returned by CBOpen(), or NULL.
*
*   RESULT
*       No return value. A NULL argument has no effect.
*
*   NOTES
*       A non-NULL argument must identify an open request owned by the
*       caller. The function closes the device, frees the request, and
*       deletes its reply port. It must not be called twice on the same
*       request.
*
****************************************************************************
*
*/
static void CBClose(struct IOClipReq *ior) {
  struct MsgPort *mp;

  if(!ior) {
    return;
  }

  mp = ior->io_Message.mn_ReplyPort;

  CloseDevice((struct IORequest *)ior);
  DeleteExtIO((struct IORequest *)ior);
  DeletePort(mp);
}

/****i* januae/ClipChange **************************************************
*
*   NAME
*       ClipChange() -- mark the AROS clipboard as changed.
*
*   SYNOPSIS
*       static ULONG ClipChange(struct Hook *c_hook, VOID *o, struct
*       ClipHookMsg *msg);
*
*   FUNCTION
*       Set clipboard_aros_changed to TRUE when clipboard.device invokes the
*       change notification hook.
*
*   INPUTS
*       c_hook - Hook supplied to clipboard.device.
*       o - Hook object passed by the device.
*       msg - Clipboard change notification.
*
*   RESULT
*       Sets clipboard_aros_changed to TRUE. No defined return value can be
*       stated for the current implementation.
*
*   NOTES
*       The callback does not inspect its arguments.
*
*   BUGS
*       The function is declared to return ULONG but reaches its end without
*       returning a value.
*
****************************************************************************
*
*/
static ULONG ClipChange(struct Hook *c_hook, VOID *o, struct ClipHookMsg *msg) {

  DebOut("clipboard changed\n");

  clipboard_aros_changed=TRUE;
}

static BOOL clip_hook_installed=FALSE;

/****i* januae/clipboard_hook_install **************************************
*
*   NAME
*       clipboard_hook_install() -- register the clipboard change hook.
*
*   SYNOPSIS
*       void clipboard_hook_install(void);
*
*   FUNCTION
*       Initialize the module-owned Hook and submit CBD_CHANGEHOOK to
*       clipboard.device unit 0 with io_Length set to 1.
*
*   INPUTS
*       None.
*
*   RESULT
*       No return value. Sets clip_hook_installed to TRUE when the
*       CBD_CHANGEHOOK request completes without an I/O error.
*
*   NOTES
*       Sets the module-owned ClipHook fields and submits a synchronous
*       registration request. The caller cannot determine registration
*       success from the return value.
*
*   BUGS
*       The opened request is dereferenced without checking whether CBOpen()
*       returned NULL.
*
****************************************************************************
*
*/

void clipboard_hook_install(void) {

  ior=CBOpen(0);

  /* init clipboard hook */
  ClipHook.h_Entry =   &ClipChange;
  ClipHook.h_SubEntry = NULL;
  ClipHook.h_Data =     NULL;

  /* install clipboard changed hook */
  ior->io_Command= CBD_CHANGEHOOK;
  ior->io_Length = 1; /* install */
  ior->io_Data   = (APTR) &ClipHook;

  if (DoIO ((struct IORequest *) ior)) {
    DebOut("ERROR: Can't install clipboard hook\n");
  }
  else {
    clip_hook_installed=TRUE;
  }

  CBClose(ior);
  ior=NULL;

}

/****i* januae/clipboard_hook_deinstall ************************************
*
*   NAME
*       clipboard_hook_deinstall() -- remove the registered clipboard change
*       hook.
*
*   SYNOPSIS
*       void clipboard_hook_deinstall(void);
*
*   FUNCTION
*       If the module records the hook as installed, submit CBD_CHANGEHOOK
*       with io_Length set to 0.
*
*   INPUTS
*       None.
*
*   RESULT
*       No return value. If the remove request succeeds, sets
*       clip_hook_installed to FALSE.
*
*   NOTES
*       Returns without device I/O when clip_hook_installed is FALSE. A
*       failed removal leaves the flag set.
*
*   BUGS
*       When removal is attempted, the opened request is dereferenced
*       without checking whether CBOpen() returned NULL.
*
****************************************************************************
*
*/

void clipboard_hook_deinstall(void) {

  DebOut("clipboard_hook_deinstall entered\n");

  if(clip_hook_installed) {
    ior=CBOpen(0);
  
    /* init clipboard hook */
    ClipHook.h_Entry =   &ClipChange;
    ClipHook.h_SubEntry = NULL;
    ClipHook.h_Data =     NULL;
  
    /* deinstall clipboard changed hook */
    ior->io_Command= CBD_CHANGEHOOK;
    ior->io_Length = 0; /* remove */
    ior->io_Data   = (APTR) &ClipHook;
  
    DebOut("doio\n");

    if (DoIO ((struct IORequest *) ior)) {
      DebOut("ERROR: Can't deinstall clipboard hook\n");
    }
    else {
      clip_hook_installed=FALSE;
    }

    DebOut("close..\n");
    CBClose(ior);
    ior=NULL;
  }
}


/****i* januae/clipboard_write_raw *****************************************
*
*   NAME
*       clipboard_write_raw() -- write an IFF clipboard block.
*
*   SYNOPSIS
*       static BOOL clipboard_write_raw(struct IOClipReq *ior, UBYTE *data,
*       ULONG len);
*
*   FUNCTION
*       Submit the supplied IFF data with CMD_WRITE. Submit CMD_UPDATE only
*       if the write reports no I/O error.
*
*   INPUTS
*       ior - Open clipboard I/O request.
*       data - Buffer containing the IFF data to write.
*       len - Number of bytes in data.
*
*   RESULT
*       Returns FALSE if CMD_WRITE reports an error. Otherwise issues
*       CMD_UPDATE and returns FALSE if that request reports an error, or
*       TRUE if no error is reported.
*
*   NOTES
*       The caller is responsible for the validity and lifetime of the
*       request and buffer. The function does not validate the IFF data.
*
****************************************************************************
*
*/
static BOOL clipboard_write_raw(struct IOClipReq *ior, UBYTE *data, ULONG len) {
  BOOL success;

  DebOut("clipboard_write_raw(%lx, %lx, %d)\n", ior, data, len);

  /* initial set-up for Offset, Error, and ClipID */
  ior->io_Offset = 0;
  ior->io_Error  = 0;
  ior->io_ClipID = 0;

  DebOut("write data ..\n");
  /* Write data */
  ior->io_Data    = data;
  ior->io_Length  = len;
  ior->io_Command = CMD_WRITE;
  DoIO( (struct IORequest *) ior);

  if(ior->io_Error) {
    DebOut("ERROR: %d\n", ior->io_Error);
    return FALSE;
  }

  DebOut("update clipboard ..\n");
  /* Tell the clipboard we are done writing */
  ior->io_Command=CMD_UPDATE;
  DoIO( (struct IORequest *) ior);

  /* Check if io_Error was set by any of the preceding IO requests */
  success = ior->io_Error ? FALSE : TRUE;

  if(!success) {
    DebOut("ERROR: %d\n",ior->io_Error);
  }

  return(success);
}

/****i* januae/cb_read_done ************************************************
*
*   NAME
*       cb_read_done() -- finish a clipboard read.
*
*   SYNOPSIS
*       void cb_read_done(struct IOClipReq *ior);
*
*   FUNCTION
*       Discard subsequent clipboard data into a local buffer until
*       io_Actual becomes zero or an I/O operation fails.
*
*   INPUTS
*       ior - Open clipboard I/O request whose io_Actual reflects the
*       current read state.
*
*   RESULT
*       No return value. Continues issuing CMD_READ requests while io_Actual
*       is nonzero; stops after a reported I/O failure.
*
*   NOTES
*       Uses a 256-byte local array and requests up to 254 bytes per read.
*       Does not report a read error to its caller.
*
****************************************************************************
*
*/
void cb_read_done(struct IOClipReq *ior) {
  char buffer[256];

  DebOut("(%lx)\n",ior);

  ior->io_Command = CMD_READ;
  ior->io_Data    = (STRPTR)buffer;
  ior->io_Length  = 254;

  /* falls through immediately if io_Actual == 0 */
  while (ior->io_Actual) {
    if (DoIO( (struct IORequest *) ior)) {
      break;
    }
  }
}

/****i* januae/copy_clipboard_to_aros **************************************
*
*   NAME
*       copy_clipboard_to_aros() -- request guest-to-AROS clipboard
*       synchronization.
*
*   SYNOPSIS
*       void copy_clipboard_to_aros(void);
*
*   FUNCTION
*       Signal the guest clipboard daemon when the guest clipboard is marked
*       as changed.
*
*   INPUTS
*       None.
*
*   RESULT
*       No return value. Does nothing if clipboard_amiga_changed is FALSE;
*       otherwise calls uae_Signal() to request transfer.
*
*   NOTES
*       The transfer is delegated to the guest daemon; this function does
*       not wait for confirmation.
*
*   BUGS
*       A missing daemon task, missing signal, or disabled clipboard
*       preference is logged but does not prevent uae_Signal() from being
*       called.
*
****************************************************************************
*
*/
void copy_clipboard_to_aros(void) {

  DebOut("entered\n");

  if(!clipboard_amiga_changed) {
    /* nothing to do for us */
    DebOut("nothing to do\n");
    return; 
  }

  if(!aos3_clip_task || !aos3_clip_signal || !changed_prefs.jclipboard) {
    DebOut("clipd not running / active\n");
  }

  /* 
   * just signal clipd and let him handle all the trouble 
   * in the end, he will call copy_clipboard_to_aros_real
   */ 

  DebOut("send signal %lx to clipd task %lx\n", aos3_clip_signal, aos3_clip_task);
  uae_Signal (aos3_clip_task, aos3_clip_signal);

}

/****i* januae/copy_clipboard_to_aros_real *********************************
*
*   NAME
*       copy_clipboard_to_aros_real() -- transfer a guest clipboard block to
*       AROS.
*
*   SYNOPSIS
*       void copy_clipboard_to_aros_real(uaecptr data, uae_u32 len);
*
*   FUNCTION
*       Check the guest buffer length and address range before attempting to
*       write the data to clipboard.device unit 0.
*
*   INPUTS
*       data - Guest address of the clipboard data.
*       len - Number of bytes available at data.
*
*   RESULT
*       No return value. Rejects input shorter than 18 bytes or a guest
*       address range rejected by valid_address(). After attempting the
*       clipboard write, clears both clipboard change flags.
*
*   NOTES
*       Converts the guest address to host-accessible memory and passes the
*       data to clipboard_write_raw().
*
*   BUGS
*       CBOpen() failure is not checked. The write result is ignored and
*       both change flags are cleared even if writing or updating the
*       clipboard fails.
*
****************************************************************************
*
*/

void copy_clipboard_to_aros_real(uaecptr data, uae_u32 len) {
  char *content;

  DebOut("entered (data %lx, len %d)\n", data, len);

  if (len < 18) {
    DebOut("len too small! (<18)\n");
    return;
  }

  if (!valid_address (data, len)) {
    DebOut("invalid_address!!\n");
    return;
  }

  ior=CBOpen(0);

  clipboard_write_raw(ior, get_real_address(data), len);

  CBClose(ior);
  ior=NULL;

  DebOut("clipboard_aros_changed =FALSE\n");
  DebOut("clipboard_amiga_changed=FALSE\n");
  /* we are in sync now */
  clipboard_aros_changed =FALSE;
  clipboard_amiga_changed=FALSE;
}

/****i* januae/copy_clipboard_to_amigaos ***********************************
*
*   NAME
*       copy_clipboard_to_amigaos() -- request AROS-to-guest clipboard
*       synchronization.
*
*   SYNOPSIS
*       void copy_clipboard_to_amigaos(void);
*
*   FUNCTION
*       Signal the guest clipboard daemon when the native clipboard is
*       marked as changed and the daemon readiness check passes.
*
*   INPUTS
*       None.
*
*   RESULT
*       No return value. Does nothing if clipboard_aros_changed is FALSE or
*       the required daemon state or clipboard preference is unavailable;
*       otherwise calls uae_Signal().
*
*   NOTES
*       This function requests the transfer but does not wait for
*       completion.
*
****************************************************************************
*
*/
void copy_clipboard_to_amigaos(void) {

  DebOut("entered\n");

  if(!clipboard_aros_changed) {
    /* nothing to do for us */
    DebOut("nothing to do\n");
    return; 
  }

  if(!aos3_clip_task || !aos3_clip_signal || !changed_prefs.jclipboard) {
    DebOut("clipd not running / enabled\n");
    return;
  }

  /* 
   * just signal clipd and let him handle all the trouble 
   * in the end, he will call copy_clipboard_to_amigaos_real
   */ 

  DebOut("send signal %lx to clipd task %lx\n", aos3_clip_to_amigaos_signal, aos3_clip_task);
  uae_Signal (aos3_clip_task, aos3_clip_to_amigaos_signal);

}

/****i* januae/aros_clipboard_len ******************************************
*
*   NAME
*       aros_clipboard_len() -- query the AROS clipboard data length.
*
*   SYNOPSIS
*       ULONG aros_clipboard_len(void);
*
*   FUNCTION
*       Issue a CMD_READ size query on clipboard.device unit 0 and consume
*       the remainder of the read sequence.
*
*   INPUTS
*       None.
*
*   RESULT
*       Returns the io_Actual value from a clipboard CMD_READ query when no
*       I/O error is reported. Returns zero when the query reports an I/O
*       error.
*
*   NOTES
*       Queries unit 0 with a NULL data pointer and io_Length set to
*       0xFFFFFFFF, then completes the read sequence. A zero return value is
*       not an unambiguous success indicator.
*
*   BUGS
*       The opened request is dereferenced without checking whether CBOpen()
*       returned NULL.
*
****************************************************************************
*
*/
ULONG aros_clipboard_len() {
  ULONG len;
  struct IOClipReq *ior;

  ior=CBOpen(0);
  DebOut("ior: %lx\n", ior);

  ior->io_ClipID  = 0;
  ior->io_Offset  = 0;
  ior->io_Command = CMD_READ;
  ior->io_Data    = NULL;
  ior->io_Length  = 0xFFFFFFFF;
  DoIO( (struct IORequest *) ior);
  len=ior->io_Actual;
  DebOut("ior->io_Offset: %d\n", ior->io_Offset);
  DebOut("ior->io_Actual: %d\n", ior->io_Actual);
  DebOut("clipboard size: %d\n", len);

  if(ior->io_Error) {
    DebOut("ERROR: %d\n", ior->io_Error);
    len=0;
  }
  cb_read_done(ior);

  CBClose(ior);
  ior=NULL;

  return len;
}

/****i* januae/copy_clipboard_to_amigaos_real ******************************
*
*   NAME
*       copy_clipboard_to_amigaos_real() -- transfer AROS clipboard data to
*       guest memory.
*
*   SYNOPSIS
*       void copy_clipboard_to_amigaos_real(uaecptr m68k_data, uae_u32 len);
*
*   FUNCTION
*       Read at most len bytes into the guest-addressed destination and
*       finish the clipboard read sequence.
*
*   INPUTS
*       m68k_data - Guest address of the destination.
*       len - Maximum number of bytes requested for the initial read.
*
*   RESULT
*       No return value. After the read sequence, clears both clipboard
*       change flags.
*
*   NOTES
*       Uses get_real_address() to obtain a host pointer for the guest
*       destination and completes the device read sequence with
*       cb_read_done(). The function does not itself establish the validity
*       or capacity of m68k_data.
*
*   BUGS
*       CBOpen() failure and clipboard read errors are not handled before
*       clearing both change flags.
*
****************************************************************************
*
*/
void copy_clipboard_to_amigaos_real(uaecptr m68k_data, uae_u32 len) {
  UBYTE *aros_data;
  struct IOClipReq *ior;
  
  DebOut("(%lx, %d)\n", m68k_data, len);
  ior=CBOpen(0);

  aros_data=get_real_address(m68k_data);
  DebOut("aros_data: %lx\n", aros_data);

  ior->io_ClipID  = 0;
  ior->io_Offset  = 0;
  ior->io_Command = CMD_READ;
  ior->io_Data    = aros_data;
  /* maximum length. avoid overwrites, in case clipboard has changed: */
  ior->io_Length  = len; 
  DoIO( (struct IORequest *) ior);
  cb_read_done(ior);

  CBClose(ior);
  ior=NULL;

  /* we are in sync now (or will be soon at least) */
  clipboard_aros_changed =FALSE;
  clipboard_amiga_changed=FALSE;
}

