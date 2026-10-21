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
 * $Id$
 *
 ************************************************************************/

#include <utility/utility.h>
#include <workbench/workbench.h>
#include <proto/utility.h>

//#define JWTRACING_ENABLED 1
#include "j.h"
#include "memory.h"
#include "include/filesys.h"
#include "include/filesys_internal.h"

/* we get a JUAE_Sync_Launch_Message for a synchronous command  */
struct JUAE_Sync_Launch_Message {
  struct Message  ExecMessage;
  ULONG           type;
  STRPTR          ln_Name;
  struct TagItem *tags;
  void           *mempool;
  ULONG           error;
};



/***********************************************************
 * convert_tags
 *
 * browse through the taglist and convert all paths to
 * amigaos paths, if possible.
 *
 * store resulting tags in an array.
 *
 * array and strings need to be FreeVec'ed somewhere!
 ***********************************************************/

/***********************************************************
 * This is the process, which watches the CLI Execute
 * Port of J-UAE, so wanderer can send us messages.
 ***********************************************************/  
/****i* januae/aros_cli_thread *********************************************
*
*   NAME
*       aros_cli_thread() -- Receive asynchronous CLI launch requests.
*
*   SYNOPSIS
*       static void aros_cli_thread(void);
*
*   FUNCTION
*       Receive requests on J-UAE Run, translate accessible executable
*       paths, enqueue CLI launch jobs, and signal the guest launch daemon.
*
*   INPUTS
*       None; requests arrive on the named Exec message port.
*
*   RESULT
*       Returns on CLI_TYPE_DIE or initialization failure.
*
*   NOTES
*       Each received message is replied to. An inaccessible path sets
*       ERROR_OBJECT_NOT_FOUND in the request.
*
*   BUGS
*       The message-port allocation is not fully released when the thread
*       exits.
*
****************************************************************************
*
*/

static void aros_cli_thread (void) {

  struct Process *thread = (struct Process *) FindTask (NULL);
  BOOL            done   = FALSE;
  ULONG           s;
  struct MsgPort *port   = NULL;
  char           *amiga_exe;
  JanusLaunch    *jlaunch;
  ULONG           error;
  struct JUAE_Sync_Launch_Message *msg;

  /* There's a time to live .. */

  JWLOG("aros_cli_thread[%lx]: thread running \n",thread);

  JWLOG("aros_cli_thread[%lx]: open port %s\n",thread, CLI_PORT_NAME);

  port=CreateMsgPort();
  if(!port) {
    JWLOG("aros_cli_thread[%lx]: ERROR: failed to open port \n",thread);
    goto EXIT;
  }

  port->mp_Node.ln_Name = AllocVec(strlen(CLI_PORT_NAME)+1, MEMF_PUBLIC);
  strcpy(port->mp_Node.ln_Name, CLI_PORT_NAME);
  port->mp_Node.ln_Type = NT_MSGPORT;
  port->mp_Node.ln_Pri  = 1; /* not too quick search */

  Forbid();
  if( !FindPort(port->mp_Node.ln_Name) ) {
    AddPort(port);
    Permit();
    JWLOG("aros_cli_thread[%lx]: new port %lx\n",thread, port);
  }
  else {
    Permit();
    JWLOG("aros_cli_thread[%lx]: port was already open (other j-uae running?)\n",thread);
    goto EXIT;
  }


  while(!done) {

    WaitPort(port);
    JWLOG("aros_cli_thread[%lx]: WaitPort done\n",thread);
    while( (msg = (struct JUAE_Sync_Launch_Message *) GetMsg(port)) ) {
      JWLOG("aros_cli_thread[%lx]: msg %lx received!\n", thread, msg);
      JWLOG("aros_cli_thread[%lx]: msg->ln_Name: >%s< \n", thread, msg->ln_Name);
      error=0;

      switch (msg->type) {
	case CLI_TYPE_DIE:
	  done=TRUE;
	  break;
	case CLI_TYPE_CLI_ASYNC:
	  amiga_exe=aros_path_to_amigaos(msg->ln_Name);
	  if(amiga_exe) {
	    /* store it for the launchd to fetch and execute */
	    ObtainSemaphore(&sem_janus_launch_list);
	    jlaunch=(JanusLaunch *) AllocVec(sizeof(JanusLaunch),MEMF_CLEAR);
	    if(jlaunch) {
	      jlaunch->type      =CLI_TYPE_CLI_ASYNC;
	      jlaunch->amiga_path=amiga_exe;
	      jlaunch->args      =NULL;
	      janus_launch       =g_slist_append(janus_launch,jlaunch);
	    }
    
	    ReleaseSemaphore(&sem_janus_launch_list);
	    JWLOG("aros_cli_thread[%lx]: uae_Signal(%lx, %lx)\n", thread, aos3_launch_task, aos3_launch_signal);
	    uae_Signal(aos3_launch_task, aos3_launch_signal);
	  }
	  else {
	    msg->error=ERROR_OBJECT_NOT_FOUND;
	    JWLOG(    "ERROR: not reachable from AmigaOS: %s\n", msg->ln_Name);
	    write_log("ERROR: not reachable from AmigaOS: %s\n", msg->ln_Name);
	  }
	  break;
	default:
	  JWLOG("aros_cli_thread[%lx]: unknown message type %d\n", msg->type);
      }

      ReplyMsg ((struct Message *) msg);

      if(error) {
	    gui_message_with_title("ERROR",
				   "Failed to start \"%s\".\n\nThis path is not available inside of amigaOS.\n\nYou need to add the AROS directory\n\n(or one of its parents)\n\nas an amigaOS device.\n\nBest way to do this is the \"Harddisk\" tab in the J-UAE GUI.", msg->ln_Name);
      }
    }
  }


  JWLOG("aros_cli_thread[%lx]: EXIT\n",thread);
  /* remove port */
  Forbid();
  /* We hope, there are no messages waiting anymore.
   * As this port is not really busy, this is not unlikely (hopefully).
   * Worst case is, that we loose the memory of the waiting messages?
   */
  RemPort(port);
  Permit();

  if(port->mp_Node.ln_Name) {
    FreeVec(port->mp_Node.ln_Name);
  }
  port=NULL;

  /* end thread */
EXIT:
  aros_cli_task=NULL;
  JWLOG("aros_cli_thread[%lx]: dies..\n", thread);
}

/****i* januae/aros_cli_start_thread ***************************************
*
*   NAME
*       aros_cli_start_thread() -- Create the CLI proxy when the launcher is
*       available.
*
*   SYNOPSIS
*       int aros_cli_start_thread(void);
*
*   FUNCTION
*       Attempt to create the CLI proxy process if the guest launch task has
*       registered.
*
*   INPUTS
*       None.
*
*   RESULT
*       Nonzero when aros_launch_task is set; zero otherwise.
*
*   NOTES
*       The function stores the newly created task in aros_cli_task.
*
*   BUGS
*       The return value tests aros_launch_task instead of aros_cli_task, so
*       it does not report CLI process creation success.
*
****************************************************************************
*
*/

int aros_cli_start_thread (void) {

  JWLOG("aros_cli_start_thread()\n");

  if(aos3_launch_task) {
    aros_cli_task = (struct Task *)
	    myCreateNewProcTags ( //NP_Output,      Output (),
				  //NP_Input,       Input (),
				  NP_Name,        (ULONG) "j-uae cli proxy",
				  //NP_CloseOutput, FALSE,
				  //NP_CloseInput,  FALSE,
				  NP_StackSize,   4096,
				  NP_Priority,    0,
				  NP_Entry,       (ULONG) aros_cli_thread,
				  TAG_DONE);

    JWLOG("cli thread %lx created\n", aros_launch_task);
  }
  else {
    JWLOG("amigaos launchd is not running\n");
  }

  return aros_launch_task != 0;
}

/****i* januae/aros_cli_kill_thread ****************************************
*
*   NAME
*       aros_cli_kill_thread() -- Send the CLI-proxy shutdown request.
*
*   SYNOPSIS
*       void aros_cli_kill_thread(void);
*
*   FUNCTION
*       Send a CLI_TYPE_DIE message to J-UAE Run and wait for a reply when
*       the proxy and message port are available.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The shutdown request uses a temporary reply port; the function does
*       not itself delete the CLI task.
*
*   BUGS
*       WaitPort is called while Forbid is in effect; scheduling and
*       shutdown behavior require separate correction review.
*
****************************************************************************
*
*/

void aros_cli_kill_thread(void) {
  struct JUAE_Sync_Launch_Message  *die_msg;
  struct Message                   *back_msg;
  struct MsgPort                   *port;
  struct MsgPort                   *back_port;

  JWLOG("aros_cli_kill_thread()\n");

  if(!aros_cli_task) {
    JWLOG("no aos3 cli task\n");
    return;
  }

  if(!FindPort((STRPTR) CLI_PORT_NAME)) {
    JWLOG("port %s not open\n", CLI_PORT_NAME);
    return;
  }

  if(! ((back_port=CreateMsgPort() )) ) {
    JWLOG("ERROR: could not create message port!\n");
    return;
  }

  die_msg=AllocVec(sizeof(struct JUAE_Sync_Launch_Message), MEMF_CLEAR);
  if(die_msg) {
    Forbid();
    if(( port=FindPort((STRPTR) CLI_PORT_NAME))) {

      ((struct Message *) die_msg)->mn_ReplyPort=back_port;
      die_msg->type=CLI_TYPE_DIE;

      JWLOG("send DIE message to %s..\n", CLI_PORT_NAME);
      PutMsg(port, die_msg); /* two way */

      JWLOG("wait for answer from %s ..\n", CLI_PORT_NAME);
      back_msg=WaitPort(back_port);
    }
    Permit();
    FreeVec(die_msg);
  }

  if(back_port) {
    DeleteMsgPort(back_port);
    back_port=NULL;
  }

  JWLOG("send DIE to %s done.\n", CLI_PORT_NAME);
}
