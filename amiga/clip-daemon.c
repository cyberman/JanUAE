/************************************************************************ 
 *
 * Clip-Daemon
 *
 * Copyright 2009 Oliver Brunner - aros<at>oliver-brunner.de
 *
 * This file is part of Janus-UAE
 *
 * Janus-UAE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Janus-Daemon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Janus-Daemon. If not, see <http://www.gnu.org/licenses/>.
 *
 * $Id$
 *
 ************************************************************************/

/*
 * share the clipboard between aos3 and the host
 */

#include <stdio.h>
#include <stdlib.h>

#include <devices/clipboard.h>
#include <exec/devices.h>
#include <exec/interrupts.h>
#include <exec/nodes.h>
#include <exec/io.h>
#include <exec/memory.h>


#include <clib/alib_protos.h>
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>

#include "janus-daemon.h"
#include "clip-daemon.h"

char verstag[] = "\0$VER: clip-daemon 0.4";
LONG          *cmdbuffer=NULL;

/* signal sent by the clipboard hook (clipboard content changed) */
BYTE         clip_signal_bit;
ULONG        clipsignal;

/* signal sent by copy_clipboard_to_aros (copy amigaos content to aros) */
BYTE         to_aros_signal_bit;
ULONG        to_aros_signal;

/* signal sent by copy_clipboard_to_amiga (copy amiga content to amigaos) */
BYTE         to_amigaos_signal_bit;
ULONG        to_amigaos_signal;

struct Task *mytask = NULL;

struct Hook ClipHook;
struct ClipHookMsg ClipHookMsg = {0, CMD_UPDATE, 0};


struct IOClipReq *ior;

/****i* januae/open_libs ***************************************************
*
*   NAME
*       open_libs() -- Report clipboard daemon library initialization.
*
*   SYNOPSIS
*       BOOL open_libs();
*
*   FUNCTION
*       Return TRUE. The previous intuition.library opening code is disabled
*       by a preprocessor condition.
*
*   INPUTS
*       None.
*
*   RESULT
*       TRUE.
*
****************************************************************************
*
*/

BOOL open_libs() {
   return TRUE;
}

/****************************************************
 * open clipboard unit 
 ****************************************************/
/****i* januae/open_device *************************************************
*
*   NAME
*       open_device() -- Open a clipboard device unit.
*
*   SYNOPSIS
*       static struct IOClipReq *open_device(ULONG unit);
*
*   FUNCTION
*       Create a message port and extended I/O request, then open
*       clipboard.device at the specified unit. Release partially created
*       resources if opening fails.
*
*   INPUTS
*       unit - Clipboard device unit number.
*
*   RESULT
*       An open IOClipReq pointer on success; NULL on failure.
*
****************************************************************************
*
*/

static struct IOClipReq *open_device(ULONG unit) {
  struct MsgPort *mp;
  struct IORequest *ior;

  C_ENTER

  if (( mp = CreatePort(0L, 0L) )) {
    if (( ior=(struct IORequest *) CreateExtIO(mp,sizeof(struct IOClipReq)) )) {
        if (!(OpenDevice((CONST_STRPTR) "clipboard.device", unit, ior, 0L))) {
	    C_LEAVE
            return((struct IOClipReq *) ior);
	}
    DeleteExtIO(ior);
    }
  DeletePort(mp);
  }
  C_LEAVE
  return(NULL);
}

/****************************************************
 * cb_read_done
 *
 * tell clipboard.device, we are done
 ****************************************************/
/****i* januae/cb_read_done ************************************************
*
*   NAME
*       cb_read_done() -- Complete a clipboard read transaction.
*
*   SYNOPSIS
*       void cb_read_done(struct IOClipReq *ior);
*
*   FUNCTION
*       Issue further CMD_READ requests with a local buffer until no data
*       remains or DoIO reports an error.
*
*   INPUTS
*       ior - Open clipboard I/O request with a pending read transaction.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       This function belongs to the guest clipboard daemon and is
*       independent of the host-side function of the same name.
*
****************************************************************************
*
*/

void cb_read_done(struct IOClipReq *ior) {
  char buffer[256];

  C_ENTER

  ior->io_Command = CMD_READ;
  ior->io_Data    = (STRPTR)buffer;
  ior->io_Length  = 254;


  /* falls through immediately if io_Actual == 0 */
  while (ior->io_Actual) {
    if (DoIO( (struct IORequest *) ior)) {
      break;
    }
  }

  C_LEAVE
}

/****************************************************
 * register us!
 * as long as setup is not called, janus-uae behaves 
 * just like a normal uae
 *
 * if called with stop=0, we want to sleep
 *
 * if result is TRUE, we need to run (share clipboard)
 * if result is FALSE, we need to sleep 
 ****************************************************/
/****i* januae/setup *******************************************************
*
*   NAME
*       setup() -- Register clipboard synchronization with the host.
*
*   SYNOPSIS
*       int setup(struct Task *task, ULONG to_aros_signal, ULONG
*       to_amigaos_signal, ULONG stop);
*
*   FUNCTION
*       Write the task and two signal masks into the guest setup buffer,
*       request AD_CLIP_SETUP, and return the status written by the host.
*
*   INPUTS
*       task - Clipboard daemon task. to_aros_signal - Host-bound change
*       notification. to_amigaos_signal - Guest-bound change notification.
*       stop - 0 to query, 1 to disable, or 2 to enable synchronization.
*
*   RESULT
*       The host-written status from command_mem[12].
*
*   BUGS
*       The AllocVec result is dereferenced without a NULL check.
*
****************************************************************************
*
*/

int setup(struct Task *task, ULONG to_aros_signal, ULONG to_amigaos_signal, ULONG stop) {
  ULONG *command_mem;
  ULONG  state;

  C_ENTER

  DebOut("clipd: clipboard setup(%lx,%lx,%lx,%d)\n",(ULONG) task, 
                                         to_aros_signal, to_amigaos_signal, stop);

  command_mem=AllocVec(AD__MAXMEM,MEMF_CLEAR);

  command_mem[ 0]=(ULONG) task;
  command_mem[ 4]=(ULONG) to_aros_signal;
  command_mem[ 8]=(ULONG) to_amigaos_signal;
  command_mem[12]=(ULONG) stop;

  state = calltrap (AD_CLIP_SETUP, AD__MAXMEM, command_mem);

  state=command_mem[12];

  FreeVec(command_mem);

  DebOut("clipd: clipboard setup done(): result %d\n",(int) state);

  C_LEAVE

  return state;
}

/**************************************************************
 * get_aros_len()
 *
 * get the length of the AROS clipboard
 **************************************************************/
/****i* januae/get_aros_len ************************************************
*
*   NAME
*       get_aros_len() -- Query the AROS clipboard length.
*
*   SYNOPSIS
*       static ULONG get_aros_len();
*
*   FUNCTION
*       Request JD_CLIP_GET_AROS_LEN and read the first field of the
*       returned guest parameter buffer.
*
*   INPUTS
*       None.
*
*   RESULT
*       The length supplied by the host dispatcher.
*
*   BUGS
*       Allocation of the command buffer is not checked before accessing the
*       returned length.
*
****************************************************************************
*
*/

static ULONG get_aros_len() {
  ULONG  *command_mem;
  LONG   len;

  C_ENTER

  command_mem=AllocVec(AD__MAXMEM, MEMF_CLEAR);

  calltrap (AD_CLIP_JOB, JD_CLIP_GET_AROS_LEN, command_mem);

  len=command_mem[0];

  FreeVec(command_mem);

  C_LEAVE

  return len;
}

/**************************************************************
 * copy_clip_from_aros(target buffer, len)
 *
 * copy amigaOS clipboard data to target buffer, which has
 * a maximum size of len
 **************************************************************/
/****i* januae/copy_clip_from_aros *****************************************
*
*   NAME
*       copy_clip_from_aros() -- Copy host clipboard data into guest memory.
*
*   SYNOPSIS
*       static void copy_clip_from_aros(UBYTE *data, ULONG size);
*
*   FUNCTION
*       Send the destination pointer and maximum byte count through
*       JD_CLIP_COPY_FROM_AROS.
*
*   INPUTS
*       data - Writable guest destination buffer. size - Maximum number of
*       bytes requested.
*
*   RESULT
*       No value is returned; no copy-success status is exposed.
*
*   BUGS
*       The command-buffer allocation and trap result are not checked.
*
****************************************************************************
*
*/

static void copy_clip_from_aros(UBYTE *data, ULONG size) {
  ULONG *command_mem;

  C_ENTER

  DebOut("(%lx,%d)\n", data, size);

  command_mem=AllocVec(AD__MAXMEM, MEMF_CLEAR);

  command_mem[0]=(ULONG) data;
  command_mem[4]=(ULONG) size;

  calltrap (AD_CLIP_JOB, JD_CLIP_COPY_FROM_AROS, command_mem);

  FreeVec(command_mem);

  DebOut("copy_clip_from_aros(%lx,%d) done\n", data, size);

  C_LEAVE

  return;
}

/**************************************************************
 * copy_clip_to_aros
 *
 * send amigaOS clipboard data pointer and clipboard size 
 * to UAE and invoke copy_clipboard_to_aros_real
 **************************************************************/
/****i* januae/copy_clip_to_aros *******************************************
*
*   NAME
*       copy_clip_to_aros() -- Submit guest clipboard content to the host.
*
*   SYNOPSIS
*       static void copy_clip_to_aros(UBYTE *data, ULONG size);
*
*   FUNCTION
*       Send a guest-data address and byte count through
*       JD_CLIP_COPY_TO_AROS.
*
*   INPUTS
*       data - Guest source buffer. size - Data length in bytes.
*
*   RESULT
*       No value is returned; the host copy result is not reported.
*
*   BUGS
*       The command-buffer allocation and trap result are not checked.
*
****************************************************************************
*
*/

static void copy_clip_to_aros(UBYTE *data, ULONG size) {
  ULONG *command_mem;

  C_ENTER

  DebOut("clipd: copy_clip_to_aros(%lx,%d)\n", data, size);

  command_mem=AllocVec(AD__MAXMEM, MEMF_CLEAR);

  command_mem[0]=(ULONG) TRUE;
  command_mem[4]=(ULONG) data;
  command_mem[8]=(ULONG) size;

  calltrap (AD_CLIP_JOB, JD_CLIP_COPY_TO_AROS, command_mem);

  DebOut("clipd: JD_CLIP_COPY_TO_AROS returned)\n");

  FreeVec(command_mem);

  DebOut("clipd: copy_clip_to_aros done\n");

  C_LEAVE

  return;
}

/***************************************************
 * handle_to_aros_signal()
 ***************************************************/
/****i* januae/handle_to_aros_signal ***************************************
*
*   NAME
*       handle_to_aros_signal() -- Transfer clipboard content to AROS.
*
*   SYNOPSIS
*       static void handle_to_aros_signal();
*
*   FUNCTION
*       Read the guest clipboard length and content through
*       clipboard.device, then invoke copy_clip_to_aros with the captured
*       buffer.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   BUGS
*       Read errors and allocation failure are not checked before the buffer
*       is used.
*
****************************************************************************
*
*/

static void handle_to_aros_signal() {
  UBYTE       *data=NULL;
  ULONG        len;

  C_ENTER

  /* we are active */
  DebOut("clipd: handle_to_aros_signal entered\n");

  /* get length of data */

  DebOut("clipd: DoIO (%lx) ..\n",  ior);
  ior->io_ClipID  = 0;
  ior->io_Offset  = 0;
  ior->io_Command = CMD_READ;
  ior->io_Data    = NULL;
  ior->io_Length  = 0xFFFFFFFF;
  DoIO( (struct IORequest *) ior);
  len=ior->io_Actual;
  DebOut("clipd: clipboard size: %d\n", len);
  cb_read_done(ior);

  data=(UBYTE *) AllocVec(len+10, MEMF_CLEAR); 
  DebOut("clipd: to_aros_signal new data: %lx\n",data);

  ior->io_ClipID  = 0;
  ior->io_Offset  = 0;
  ior->io_Command = CMD_READ;
  ior->io_Data    = data;
  ior->io_Length  = len+1;
  DoIO( (struct IORequest *) ior);
  cb_read_done(ior);

  copy_clip_to_aros(data, len);

  FreeVec(data);

  C_LEAVE
}

/***************************************************
 * handle_to_amigaos_signal()
 ***************************************************/
/****i* januae/handle_to_amigaos_signal ************************************
*
*   NAME
*       handle_to_amigaos_signal() -- Transfer clipboard content from AROS.
*
*   SYNOPSIS
*       static void handle_to_amigaos_signal();
*
*   FUNCTION
*       Retrieve the AROS clipboard length, request the payload, and write
*       it to the guest clipboard with CMD_WRITE followed by CMD_UPDATE.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       A length of -1 is treated as an incompatible clipboard.device
*       response.
*
*   BUGS
*       An allocated destination buffer is used without a NULL check;
*       individual DoIO failures do not prevent the update request.
*
****************************************************************************
*
*/

static void handle_to_amigaos_signal() {
  BOOL         success;
  UBYTE       *data;
  LONG         len;

  C_ENTER

  /* get length of data */
  len=get_aros_len();

  if(len == -1) {
    /* Bug in old aros clipboard.device */
    printf("Please update your AROS clipboard.device!\n");
    DebOut("clipd: ERROR: old AROS clipboard.device detected\n");
    return;
  }

  if(len) {
    DebOut("clipd: clipboard size: len=%d\n", len);

    data=(UBYTE *) AllocVec(len+10, MEMF_CLEAR); /* freed on next call */

    copy_clip_from_aros(data, len);

    /* initial set-up for Offset, Error, and ClipID */
    ior->io_Offset = 0;
    ior->io_Error  = 0;
    ior->io_ClipID = 0;

    DebOut("clipd: write data to amigaos clipboard ..\n");
    /* Write data */
    ior->io_Data    = data;
    ior->io_Length  = len;
    ior->io_Command = CMD_WRITE;
    DoIO( (struct IORequest *) ior);

    DebOut("clipd: update amigaos clipboard ..\n");
    /* Tell the clipboard we are done writing */
    ior->io_Command=CMD_UPDATE;
    DoIO( (struct IORequest *) ior);

    /* Check if io_Error was set by any of the preceeding IO requests */
    success = ior->io_Error ? FALSE : TRUE;

    if(!success) {
      DebOut("clipd: ERROR: %d\n",ior->io_Error);
    }
    if(data) {
      FreeVec(data);
    }
  }

  C_LEAVE
}

/**************************************************************
 * amiga_changed
 *
 * tell UAE, that the content of the amigaOS clipboard changed.
 * So if the UAE windows loose the focus, we can copy the
 * amigaOS content to the AROS clipboard
 *
 **************************************************************/
/****i* januae/amiga_changed ***********************************************
*
*   NAME
*       amiga_changed() -- Notify the host of guest clipboard changes.
*
*   SYNOPSIS
*       static void amiga_changed();
*
*   FUNCTION
*       Send JD_AMIGA_CHANGED with a TRUE change indicator through the guest
*       trap.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   BUGS
*       The command-buffer allocation and trap result are not checked.
*
****************************************************************************
*
*/

static void amiga_changed() {
  ULONG *command_mem;

  C_ENTER

  DebOut("clipd: clipboard_changed signal received\n");

  command_mem=AllocVec(AD__MAXMEM,MEMF_CLEAR);

  command_mem[0]=(ULONG) TRUE;
  calltrap (AD_CLIP_JOB, JD_AMIGA_CHANGED, command_mem);

  FreeVec(command_mem);

  C_LEAVE
}


/****i* januae/runme *******************************************************
*
*   NAME
*       runme() -- Handle clipboard change signals.
*
*   SYNOPSIS
*       static void runme();
*
*   FUNCTION
*       Wait for transfer and clipboard-hook signals, query the host
*       synchronization state, process enabled transfers, and forward Ctrl-C
*       and Ctrl-D state requests.
*
*   INPUTS
*       None.
*
*   RESULT
*       Does not return during normal daemon operation.
*
****************************************************************************
*
*/

static void runme() {
  ULONG        newsignals;
  BOOL         done;
  BOOL         init;
  BOOL         set;

  C_ENTER
  DebOut("clipd: clipd running (CTRL-C to go to normal mode, CTRL-D to shared mode)..\n");

  done=FALSE;
  init=FALSE;
  while(!done) {
    newsignals=Wait(to_aros_signal | to_amigaos_signal |
                    clipsignal | 
		    SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D);

    /* test if we are still active */
    set=setup(mytask, to_aros_signal, to_amigaos_signal, 0);
    DebOut("clipd: set %d, signal %d\n", set, newsignals);

    if(set) {

      if(newsignals & to_aros_signal) {
	DebOut("clipd: to_aros_signal from UAE received\n");
	handle_to_aros_signal();
      }

      if(newsignals & to_amigaos_signal) {
	DebOut("clipd: to_amigaos_signal from UAE received\n");
	handle_to_amigaos_signal();
      }

      if(newsignals & clipsignal) {
	amiga_changed();
      }
    }

    if((newsignals & SIGBREAKF_CTRL_C) ||
      (!set && (newsignals & to_aros_signal))) {
      DebOut("clipd: !set || got SIGBREAKF_CTRL_C..\n");
      /* we are tired */
      set=setup(mytask, to_aros_signal, to_amigaos_signal, 1);
    }

    if(newsignals & SIGBREAKF_CTRL_D) {
      DebOut("clipd: got SIGBREAKF_CTRL_D..\n");
      /* tell uae, that we received a SIGBREAKF_CTRL_D */
      set=setup(mytask, to_aros_signal, to_amigaos_signal, 2);
    }
  }

  /* never arrive here */
  DebOut("clipd: try to sleep ..\n");
  C_LEAVE
}

/***************************************************
 * ClipChange Hook signals the main loop, that the
 * clipboard content changed
 ***************************************************/
/****i* januae/ClipChange **************************************************
*
*   NAME
*       ClipChange() -- Signal a guest clipboard change.
*
*   SYNOPSIS
*       ULONG ClipChange(struct Hook *c_hook, VOID *o, struct ClipHookMsg
*       *msg);
*
*   FUNCTION
*       Signal the clipboard daemon task to process a clipboard change
*       notification.
*
*   INPUTS
*       c_hook, o, msg - Hook callback arguments; not examined by this
*       implementation.
*
*   RESULT
*       Zero.
*
****************************************************************************
*
*/

ULONG ClipChange(struct Hook *c_hook, VOID *o, struct ClipHookMsg *msg) {

  C_ENTER

  DebOut("clipd: Signal(%lx,%d\n",mytask,clipsignal);

  Signal(mytask, clipsignal);

  C_LEAVE

  return 0;
}


/****i* januae/main ********************************************************
*
*   NAME
*       main() -- Initialize and run the clipboard daemon.
*
*   SYNOPSIS
*       int main(int argc, char **argv);
*
*   FUNCTION
*       Open clipboard.device unit zero, allocate change and transfer
*       signals, install the clipboard change hook, register with the host,
*       and enter the notification loop.
*
*   INPUTS
*       argc - Argument count. argv - Argument vector; not used for
*       configuration.
*
*   RESULT
*       Terminates on selected startup errors; otherwise runs indefinitely.
*
*   BUGS
*       Failed startup does not consistently release resources already
*       acquired.
*
****************************************************************************
*
*/

int main (int argc, char **argv) {

  C_ENTER

  DebOut("clipd: started\n");

  if(!open_libs()) {
    C_LEAVE
    exit(1);
  }

  ior=open_device(0);
  if(!ior) {
    C_LEAVE
    printf("ERROR: unable to init %s\n", CLIPDEV_NAME);
    exit(1);
  }

  /* alloc clip change signal */
  clip_signal_bit=AllocSignal(-1);
  if(clip_signal_bit == -1) {
    printf("no signal..!\n");
    DebOut("clipd: no signal..!\n");
    C_LEAVE
    exit(1);
  }
  clipsignal=1L << clip_signal_bit;

  /* init clipboard hook */
  ClipHook.h_Entry = &ClipChange;
  ClipHook.h_SubEntry = NULL;
  ClipHook.h_Data = &ClipHookMsg;

  /* install clipboard changed hook */
  ior->io_Command=CBD_CHANGEHOOK;
  ior->io_Length =1;
  ior->io_Data   =(APTR) &ClipHook;

  if (DoIO ((struct IORequest *) ior)) {
    printf("ERROR: Can't install clipboard hook\n");
    DebOut("clipd: ERROR: Can't install clipboard hook\n");
    /* TODO! */
    C_LEAVE
    exit(1);
  }

  /* try to get a signal */
  to_aros_signal_bit=AllocSignal(-1);
  if(to_aros_signal_bit == -1) {
    printf("no signal..!\n");
    DebOut("clipd: no signal..!\n");
    C_LEAVE
    exit(1);
  }
  to_aros_signal=1L << to_aros_signal_bit;

  /* try to get a signal */
  to_amigaos_signal_bit=AllocSignal(-1);
  if(to_amigaos_signal_bit == -1) {
    printf("no signal..!\n");
    DebOut("clipd: no signal..!\n");
    C_LEAVE
    exit(1);
  }
  to_amigaos_signal=1L << to_amigaos_signal_bit;

  mytask=FindTask(NULL);
  DebOut("clipd: task: %lx\n",mytask);

  /* init everything for the patches */
  setup(mytask, to_aros_signal, to_amigaos_signal, 0); 

  while(1) {
    runme(); /* as we patched the system, we will run (sleep) forever */
  }
  C_LEAVE
}
