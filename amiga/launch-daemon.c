/************************************************************************ 
 *
 * Launch-Daemon
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
 * start ao3 workbench executables from aros wanderer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <devices/clipboard.h>
#include <exec/devices.h>
#include <exec/interrupts.h>
#include <exec/nodes.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <workbench/startup.h>
#include <dos/dostags.h>


#include <clib/alib_protos.h>
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>

#include "include/wbstart.h"
#ifndef __AROS__
#include "include/inline/wbstart.h"
#endif

#include "janus-daemon.h"
#include "launch-daemon.h"

#ifndef __AROS__
struct Library *WBStartBase=NULL;
#endif
struct Library *DOSBase    =NULL;

char verstag[] = "\0$VER: launch-daemon 0.3";
LONG          *cmdbuffer=NULL;

/* signal sent by j-uae to tell us, we have to do something! */
BYTE         launch_signal_bit;
ULONG        launchsignal;

struct Task *mytask = NULL;

/****i* januae/open_libs ***************************************************
*
*   NAME
*       open_libs() -- Open launch daemon dependencies.
*
*   SYNOPSIS
*       BOOL open_libs();
*
*   FUNCTION
*       Open dos.library; on AmigaOS also open wbstart.library. The AROS
*       branch does not request wbstart.library.
*
*   INPUTS
*       None.
*
*   RESULT
*       TRUE when required libraries open; FALSE if a required open fails.
*
****************************************************************************
*
*/

BOOL open_libs() {

   if(!(DOSBase=OpenLibrary((CONST_STRPTR) "dos.library",36L))) {
     printf("unable to open dos.library!?\n");
     return FALSE;
   }

#ifndef __AROS__
   if (!(WBStartBase=OpenLibrary((CONST_STRPTR) "wbstart.library", 2L))) {
     printf("unable to open wbstart.library! Get it from aminet.\n");
     return FALSE;
   }
#endif

   return TRUE;
}

/****************************************************
 * register us!
 *
 * as long as setup is not called, janus-uae behaves 
 * just like a normal uae
 *
 * if called with stop=0, we want to sleep
 *
 * if result is TRUE, we need to run 
 * if result is FALSE, we need to sleep 
 ****************************************************/
/****i* januae/setup *******************************************************
*
*   NAME
*       setup() -- Register the launch daemon with the host.
*
*   SYNOPSIS
*       int setup(struct Task *task, ULONG launch_signal, ULONG stop);
*
*   FUNCTION
*       Populate the setup parameter buffer, call AD_LAUNCH_SETUP, and read
*       the host-written status field.
*
*   INPUTS
*       task - Launch daemon task. launch_signal - Signal mask for queued
*       launch requests. stop - Requested daemon state.
*
*   RESULT
*       The value written to command_mem[12] by the host.
*
*   NOTES
*       The current host implementation derives this value from the
*       clipboard preference, not an independent launch state.
*
*   BUGS
*       The command buffer is dereferenced without checking AllocVec for
*       failure.
*
****************************************************************************
*
*/

int setup(struct Task *task, ULONG launch_signal, ULONG stop) {
  ULONG *command_mem;
  ULONG  state;

  C_ENTER

  DebOut("launchd: launcher setup(%lx,%lx,%lx,%d)\n",(ULONG) task, launch_signal, stop);

  command_mem=AllocVec(AD__MAXMEM,MEMF_CLEAR);

  command_mem[ 0]=(ULONG) task;
  command_mem[ 4]=(ULONG) launch_signal;
  command_mem[ 8]=(ULONG) 0;
  command_mem[12]=(ULONG) stop;

  state = calltrap (AD_LAUNCH_SETUP, AD__MAXMEM, command_mem);

  state=command_mem[12];

  FreeVec(command_mem);

  DebOut("launchd: launcher setup done(): result %d\n",(int) state);

  C_LEAVE

  return state;
}

/****************************************************************
 * start_it
 *
 * start filename in path with args
 *
 * args can be NULL, if there are no parameters
 ****************************************************************/
/****i* januae/start_it ****************************************************
*
*   NAME
*       start_it() -- Start a guest Workbench program.
*
*   SYNOPSIS
*       static LONG start_it(char *path, char *filename, struct WBArg
*       *args);
*
*   FUNCTION
*       On AmigaOS, pass the directory, file name, and optional Workbench
*       arguments to WBStartTags. The AROS branch currently returns zero
*       without invoking a launcher.
*
*   INPUTS
*       path - Program directory. filename - Program name. args - Optional
*       terminated Workbench argument array.
*
*   RESULT
*       WBStartTags result on AmigaOS; zero on AROS.
*
*   BUGS
*       The AROS implementation reports zero without starting the program.
*
****************************************************************************
*
*/

static LONG start_it(char *path, char *filename, struct WBArg *args) {
  LONG rc;
  ULONG nr_args;

  DebOut("launchd: start_it(%s, %s, %lx)\n", path, filename, args);

  /* count arguments */
  nr_args=0;
  if(args!=NULL) {
    while((args[nr_args].wa_Name) || (args[nr_args].wa_Lock)) {
      nr_args++;
    }
  }
  DebOut("launchd: #args: %d\n", nr_args);

#ifndef __AROS__
  if(nr_args) {
    rc = WBStartTags(WBStart_DirectoryName,  (ULONG) path,
                     WBStart_Name,          (ULONG) filename,
                     WBStart_ArgumentCount,  nr_args,
                     WBStart_ArgumentList,   (ULONG) args,
                     TAG_DONE);
  }
  else {
    rc = WBStartTags(WBStart_DirectoryName, (ULONG) path,
                     WBStart_Name,          (ULONG) filename,
                     TAG_DONE);
  }
#else
#warning TODO -------------------
  rc=0;
#endif

  if(rc != RETURN_OK) {
    DebOut("launchd: unable to start it: rc=%d\n", rc);
  }

  return rc;
}


/*
struct WBArg {
  BPTR  wa_Lock; => a lock descriptor 
  BYTE *wa_Name; => a string relative to that lock 
};
*/


/****i* januae/str_dup_pool ************************************************
*
*   NAME
*       str_dup_pool() -- Duplicate a string in a memory pool.
*
*   SYNOPSIS
*       static BYTE *str_dup_pool(void *pool, char *in);
*
*   FUNCTION
*       Allocate strlen(in)+1 bytes from the supplied pool and copy the
*       terminated source string.
*
*   INPUTS
*       pool - AllocPooled memory pool. in - NUL-terminated source string.
*
*   RESULT
*       Pointer to the copied string on successful allocation.
*
*   BUGS
*       AllocPooled is not checked before strcpy.
*
****************************************************************************
*
*/

static BYTE *str_dup_pool(void *pool, char *in) {
  char *result;

  DebOut("launchd: str_dup_pool(.., %s)\n", in);

  result=(char *) AllocPooled(pool, strlen(in)+1);
  strcpy(result, in);

  return (BYTE *) result;
}

/****i* januae/get_filename ************************************************
*
*   NAME
*       get_filename() -- Return the file name component of a path.
*
*   SYNOPSIS
*       static char *get_filename(char *in);
*
*   FUNCTION
*       Use PathPart to locate the path separator and return a pointer to
*       the following character.
*
*   INPUTS
*       in - NUL-terminated path.
*
*   RESULT
*       Pointer into the supplied path buffer; no allocation is performed.
*
****************************************************************************
*
*/

static char *get_filename(char *in) {
  STRPTR sep;

  DebOut("get_filename(%s)\n", in);

  sep=PathPart((CONST_STRPTR) in);

  DebOut("launchd: filename: %s\n", sep+1);

  return (char *) sep+1;
}

/* WARNING: this destroys in !!*/
/****i* januae/get_path ****************************************************
*
*   NAME
*       get_path() -- Copy the directory component of a path.
*
*   SYNOPSIS
*       static char *get_path(void *pool, char *in);
*
*   FUNCTION
*       Temporarily terminate the input string at PathPart, duplicate the
*       directory portion in the supplied pool, and restore the input byte.
*
*   INPUTS
*       pool - AllocPooled memory pool. in - Mutable NUL-terminated path.
*
*   RESULT
*       Pointer to a pooled copy of the directory component on successful
*       allocation.
*
*   NOTES
*       The input buffer is modified temporarily and must be writable.
*
*   BUGS
*       The pool allocation result is not checked before strcpy.
*
****************************************************************************
*
*/

static char *get_path(void *pool, char *in) {
  char *sep;
  char *res;
  char restore;

  DebOut("get_path(%s)\n", in);

  sep=(char *) PathPart((CONST_STRPTR) in);
  restore=sep[0];
  sep[0]=(char) 0;

  res=AllocPooled(pool, strlen(in)+1);
  strcpy(res, in);

  sep[0]=restore;

  DebOut("launchd: path: %s\n", res);

  return res;
}

/*
 * create an array of WBArg for the wbstart call.
 * we get all strings referenced in ULONG *in
 *
 * we use a pool here, to avoid complicate FreeVecs
 *
 * in looks like that:
 *
 * 0 : ignored here
 * 4 : ignored here
 * 8 : ignored here
 * 12: (ULONG) offset 1
 * 16: (ULONG) offset 2
 * ...
 * 20: NULL
 * offset 1: string 1
 * offset 2: string 2
 */
/****i* januae/create_wbargs ***********************************************
*
*   NAME
*       create_wbargs() -- Construct Workbench launch arguments.
*
*   SYNOPSIS
*       static struct WBArg *create_wbargs(void *pool, ULONG *in);
*
*   FUNCTION
*       Interpret the offset table beginning at in[3], resolve each path,
*       lock its directory, and collect successful entries into a terminated
*       pooled WBArg array.
*
*   INPUTS
*       pool - Memory pool for the array and copied names. in - Launch
*       message containing a zero-terminated string-offset table.
*
*   RESULT
*       A WBArg array, or NULL when the offset list is empty.
*
*   NOTES
*       Entries whose directory cannot be locked are omitted.
*
*   BUGS
*       Acquired directory locks have no corresponding release in the launch
*       handler.
*
****************************************************************************
*
*/

static struct WBArg *create_wbargs(void *pool, ULONG *in) {
  ULONG nr;
  ULONG i;
  ULONG t;
  char *strings;
  struct WBArg *args;
  BPTR  lock;
  char *path;
  ULONG *ref;

  DebOut("launchd: create_wbargs\n");

  /* here is the beginning of the offsets */
  //ref=in+(3*sizeof (ULONG *));
  ref=&in[3];

  nr=0;
  while(ref[nr]) {
    DebOut("launchd: ref[%d]: %d (%s)\n", nr, ref[nr], ref[nr]);
    nr++;
  }

  if(!nr) {
    DebOut("launchd: no args\n");
    return NULL;
  }

  DebOut("launchd: nr: %d\n", nr);

  args=AllocPooled(pool, sizeof(struct WBArg) * (nr+1));
  strings=(char *) in;

  t=0;
  for(i=0; i<nr; i++) {
    path=get_path(pool, strings + ref[i]);
    lock=Lock((CONST_STRPTR) path, ACCESS_READ);
    if(lock) {
      /* attention: get_path destroys input strings, so use get_filename first! */
      args[t].wa_Name=str_dup_pool(pool, get_filename(strings + ref[i]) );
      args[t].wa_Lock=lock;
      DebOut("launchd: args[%d]: Lock %lx, Name %s\n", t, args[t].wa_Lock, args[t].wa_Name);
      t++;
    }
    else {
      DebOut("launchd: WARNING: could not lock path #%d: %s)\n", i, path);
    }
  }

  /* terminate it, not neccessary, but feels better */
  args[t].wa_Name=NULL;
  args[t].wa_Lock=0;

  return args;
}

/****i* januae/handle_launch_signal ****************************************
*
*   NAME
*       handle_launch_signal() -- Consume queued host launch requests.
*
*   SYNOPSIS
*       static void handle_launch_signal(void);
*
*   FUNCTION
*       Fetch queued jobs through LD_GET_JOB. For Workbench jobs, decode the
*       path and argument array and call start_it. For CLI jobs, invoke
*       SystemTags asynchronously.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The handler stops when the queue response type is zero.
*
*   BUGS
*       Launch completion and errors are not reported through the request
*       protocol; Workbench directory locks are not released.
*
****************************************************************************
*
*/

static void handle_launch_signal(void) {
  ULONG *command_mem;
  char  *command_string;
  char  *path;
  char  *fullpath;
  char  *filename;
  struct WBArg *wbargs=NULL;
  void  *pool;
  BOOL   done;
  LONG  result;

  C_ENTER

  DebOut("launchd: handle_launch_signal()\n");

  command_mem=AllocVec(8192, MEMF_CLEAR);
  if(!command_mem) {
    C_LEAVE
    return;
  }

  pool=CreatePool(MEMF_CLEAR, 256, 256);

  done=FALSE;
  DebOut("launchd: handle_launch_signal->while..\n");
  while(!done) {
    DebOut("launchd: handle_launch_signal->while(!done)\n");

    calltrap (AD_LAUNCH_JOB, LD_GET_JOB, command_mem);

    if(command_mem[0]==0) {
      DebOut("launchd: no commands waiting for us any more ..\n");
      done=TRUE;
    }
    else {
      if(command_mem[0]==1) {

  /* WB == 1 */

  command_string=(char *) command_mem;
  path          =command_string + command_mem[1];
  filename      =command_string + command_mem[2];

  DebOut("launchd: Start WB program \n", filename);

  if(command_mem[3]) {
    wbargs      =create_wbargs(pool, command_mem);
  }
  else {
    DebOut("launchd: we have no arguments\n");
  }

  start_it(path, filename, wbargs);
  /* TODO: unlock all :( */
      }
      else {
  /* CLI == 2 */
  /* we get everything in path: path,filename and arguments */

  command_string=(char *) command_mem;
  fullpath      =         command_string + command_mem[1];

  DebOut("launchd: fullpath: >%s<\n", fullpath);

  result=SystemTags((STRPTR) fullpath,
            SYS_Asynch, TRUE,
            SYS_Input,  Open((CONST_STRPTR) "CON://200/100/RunAmigaOs/CLOSE/AUTO/WAIT", MODE_OLDFILE),
            SYS_Output, NULL,
            TAG_DONE);

  DebOut("result: %d\n", result);

      }
    }
  }

  DebOut("launchd: handle_launch_signal->FreeVec\n");
  FreeVec(command_mem);

  if(pool) {
    DeletePool(pool);
  }

  C_LEAVE
}

/****i* januae/runme *******************************************************
*
*   NAME
*       runme() -- Wait for guest launch requests.
*
*   SYNOPSIS
*       static void runme(void);
*
*   FUNCTION
*       Register the launch daemon and process the host launch signal. Break
*       signals are received but the intended enable/disable operations are
*       commented out.
*
*   INPUTS
*       None.
*
*   RESULT
*       Does not return during normal daemon operation.
*
*   BUGS
*       Ctrl-C and Ctrl-D do not currently alter the launch-processing
*       state.
*
****************************************************************************
*
*/

static void runme(void) {
  ULONG        newsignals;
  BOOL         done;
  BOOL         set=TRUE;

  C_ENTER
  DebOut("launchd: launchd running (CTRL-C to go to normal mode, CTRL-D to disabled mode)..\n");

  setup(mytask, launchsignal, 0);

  done=FALSE;
  while(!done) {
    newsignals=Wait(launchsignal | SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D);

    /* test if we are still active */
    //TODO set=setup(mytask, to_aros_signal, to_amigaos_signal, 0);
    DebOut("launchd: set %d, signal %d\n", set, newsignals);

    if(set) {

      if(newsignals & launchsignal) {
  DebOut("launchd: launchsignal from UAE received\n");
  handle_launch_signal();
      }
    }

    if((newsignals & SIGBREAKF_CTRL_C) ||
      (!set && (newsignals & launchsignal))) {
      DebOut("launchd: !set || got SIGBREAKF_CTRL_C..\n");
      /* we are tired */
      //TODO set=setup(mytask, to_aros_signal, to_amigaos_signal, 1);
    }

    if(newsignals & SIGBREAKF_CTRL_D) {
      DebOut("launchd: got SIGBREAKF_CTRL_D..\n");
      /* tell uae, that we received a SIGBREAKF_CTRL_D */
      //TODO set=setup(mytask, to_aros_signal, to_amigaos_signal, 2);
    }
  }

  /* never arrive here */
  DebOut("launchd: try to sleep ..\n");
  C_LEAVE
}

/****i* januae/main ********************************************************
*
*   NAME
*       main() -- Initialize and run the launch daemon.
*
*   SYNOPSIS
*       int main(int argc, char **argv);
*
*   FUNCTION
*       Open required libraries, allocate a launch signal, determine the
*       current task, and enter the launch-service loop.
*
*   INPUTS
*       argc - Argument count. argv - Argument vector; not used for
*       configuration.
*
*   RESULT
*       Terminates on library or signal initialization failure; otherwise
*       runs indefinitely.
*
****************************************************************************
*
*/

int main (int argc, char **argv) {

  C_ENTER

  DebOut("launchd: started\n");

  if(!open_libs()) {
    C_LEAVE
    exit(1);
  }

  launch_signal_bit=AllocSignal(-1);
  if(launch_signal_bit == -1) {
    printf("no signal..!\n");
    DebOut("launchd: no signal..!\n");
    C_LEAVE
    exit(1);
  }
  launchsignal=1L << launch_signal_bit;

  mytask=FindTask(NULL);
  DebOut("launchd: task: %lx\n",mytask);

  while(1) {
    runme(); /* as we patched the system, we will run (sleep) forever */
  }

  C_LEAVE
}
