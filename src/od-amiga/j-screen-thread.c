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

//#define JWTRACING_ENABLED 1
#include "j.h"
#include "memory.h"




/************************************************************************
 * new_aros_pub_screen for an aos3screen
 *
 * check if there is already a pubscreen with this name
 * - if yes, return that one
 * - if not, open new one
 ************************************************************************/
/****i* januae-j-screen-thread/new_aros_pub_screen *************************
*
*   NAME
*       new_aros_pub_screen() -- Open the mapped public screen.
*
*   SYNOPSIS
*       static struct Screen *new_aros_pub_screen(JanusScreen *jscreen,
*       uaecptr aos3screen, struct Process *thread, BYTE signal);
*
*   FUNCTION
*       Choose a compatible RTG display mode and open an AROS public screen
*       with the guest title and mapped public name.
*
*   INPUTS
*       jscreen - Screen mapping. aos3screen - Guest screen address. thread
*       - Owning process. signal - Public-screen notification signal bit.
*
*   RESULT
*       Opened screen pointer, or NULL when no compatible mode or screen is
*       available.
*
*   NOTES
*       The opened screen is assigned public-screen ownership to the worker
*       process.
*
****************************************************************************
*
*/

static struct Screen *new_aros_pub_screen(JanusScreen *jscreen, 
                                          uaecptr aos3screen,
					  struct Process *thread,
					  BYTE signal) {
  ULONG width;
  ULONG height;
  ULONG depth;
  LONG mode  = INVALID_ID;
  ULONG i;
  ULONG err;
  struct Screen *arosscr;
  const UBYTE preferred_depth[] = {24, 32, 16, 15, 8};
  struct screen *pub;
  struct Task *task;

  JWLOG("aros_scr_thread[%lx]: screen public name: >%s<\n",thread,
                                                           jscreen->pubname);

  task=(struct Task *)&((struct Process *)FindTask(NULL))->pr_Task;
  JWLOG("aros_scr_thread[%lx]: thread %lx, task %lx\n", thread, task);

  width =get_word(aos3screen+12);
  height=get_word(aos3screen+14);
  depth =jscreen->depth;

  //JWLOG("we want %dx%d in %d bit\n",width,height,depth);

  mode=find_rtg_mode(&width, &height, depth);
  //JWLOG("screen mode with depth %d: %lx\n",depth,mode);

  if(mode == INVALID_ID) {
    //JWLOG("try different depth:\n");
    i=0;
    while( (i<sizeof(preferred_depth)) && (mode==INVALID_ID)) {
      depth = preferred_depth[i];
      mode = find_rtg_mode (&width, &height, depth);
      i++;
    }
    //JWLOG("screen mode with depth %d: %lx\n",depth,mode);
  }

  if(mode==INVALID_ID) {
    JWLOG("aros_scr_thread[%lx]: ERROR: unable to find a screen mode !?\n",
          thread);
    return NULL;
  }

  arosscr=OpenScreenTags(NULL,
		     SA_Type, PUBLICSCREEN,
		     SA_DisplayID, mode,
		     SA_Width, width, /* necessary ? */
		     SA_Height, height,
		     SA_Title, get_real_address(get_long(aos3screen+26)),
		     SA_PubName, jscreen->pubname,
		     SA_PubTask, task,
		     SA_PubSig, signal,
		     SA_ErrorCode, &err,
		     TAG_DONE);

  JWLOG("OpenScreenTags returned :%lx\n", arosscr);
  jscreen->ownscreen=TRUE; /* TODO we have to close this one, how..? */

  /* Not sure, if this is necessary. But under AROS, you cannot open windows, if you don't do it. */
  PubScreenStatus(arosscr, 0);

  return arosscr;
}

/***********************************************************
 * every AOS3 screen gets its own process
 ***********************************************************/  

/****i* januae-j-screen-thread/aros_screen_thread **************************
*
*   NAME
*       aros_screen_thread() -- Manage a public screen process.
*
*   SYNOPSIS
*       static void aros_screen_thread(void);
*
*   FUNCTION
*       Resolve the screen mapping for the current process, open its host
*       public screen, wait for close notifications and release the
*       process-owned screen and mapping.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       SIGBREAKF_CTRL_C requests that the screen become private; the worker
*       exits when its public-screen signal arrives.
*
****************************************************************************
*
*/

static void aros_screen_thread (void) {

  struct Process *thread = (struct Process *) FindTask (NULL);
  GSList         *list_screen=NULL;
  JanusScreen    *jscr=NULL;
  BOOL            done;
  BYTE            signal;
  ULONG           s;
  uaecptr aos3screen;

  /* There's a time to live .. */

  JWLOG("aros_scr_thread[%lx]: thread running \n",thread);

  JWLOG("aros_scr_thread[%lx]: ObtainSemaphore(&sem_janus_screen_list) \n",thread);
  ObtainSemaphore(&sem_janus_screen_list);
  JWLOG("aros_scr_thread[%lx]: ObtainSemaphore(&sem_janus_screen_list) successfull\n",
        thread);
  list_screen=g_slist_find_custom(janus_screens, 
	 		     	  (gconstpointer) thread,
			     	  &aos3_screen_process_compare);


  if(!list_screen) {
    JWLOG("aros_scr_thread[%lx]: ERROR: screen of this thread not found in screen list !?\n",thread);

    JWLOG("aros_scr_thread[%lx]: ReleasedSemaphore(&sem_janus_screen_list)\n",thread);
    ReleaseSemaphore(&sem_janus_screen_list);

    goto EXIT; 
  }

  jscr=(JanusScreen *) list_screen->data;
  JWLOG("aros_scr_thread[%lx]: jscr: %lx \n",thread,jscr);

  signal=AllocSignal(-1);
  if(signal == -1) {
    JWLOG("aros_scr_thread[%lx]: unable to alloc signal\n");

    JWLOG("aros_scr_thread[%lx]: ReleasedSemaphore(&sem_janus_screen_list)\n",thread);
    ReleaseSemaphore(&sem_janus_screen_list);

    goto EXIT;
  }
  JWLOG("aros_scr_thread[%lx]: signal %d\n", thread, signal);

  aos3screen=(uaecptr) jscr->aos3screen;

  jscr->arosscreen=new_aros_pub_screen(jscr, aos3screen, thread, signal);
  if(!jscr->arosscreen) {
    JWLOG("aros_scr_thread[%lx]: ERROR: could not open screen !!\n",thread);

    JWLOG("aros_scr_thread[%lx]: ReleasedSemaphore(&sem_janus_screen_list)\n",thread);
    ReleaseSemaphore(&sem_janus_screen_list);

    goto EXIT; 
  }

  JWLOG("aros_scr_thread[%lx]: opened new aros public screen: %lx (%s)\n",
        thread,
	jscr->arosscreen,
	jscr->arosscreen->Title);
  JWLOG("aros_scr_thread[%lx]: jscr: %lx\n", thread, jscr);
  JWLOG("aros_scr_thread[%lx]: jscr->aos3screen: %lx\n", thread, aos3screen);
  JWLOG("aros_scr_thread[%lx]: jscr->arosscreen: %lx\n", thread, jscr->arosscreen);

  ReleaseSemaphore(&sem_janus_screen_list);

  done=FALSE;

  while(!done) {
    s=Wait(1L << signal | SIGBREAKF_CTRL_C);

    JWLOG("aros_scr_thread[%lx]: signal received (%d)\n", thread, signal);
    /* Ctrl-C */
    if(s & SIGBREAKF_CTRL_C) {
      JWLOG("aros_scr_thread[%lx]: SIGBREAKF_CTRL_C received\n", thread);
      /* WARNING: only die, if there is no other window left,
       *          we wait until there are no more windows left! (TODO!!)*/
      /* WARNING: someone has most likely a window on our screen! If j-uae exits, the only signal we get is
       *          this SIGBREAKF_CTRL_C. But our thread will end soon. Otherwise we would need to keep all
       *          of the uae memory etc alive.. But now we might crash.. anyway, at least make us private!
       */
      PubScreenStatus(jscr->arosscreen, PSNF_PRIVATE);
    }
    if(s & (1L << signal)) {
      JWLOG("aros_scr_thread[%lx]: signal %d received\n", thread, signal);
      done=TRUE;
    }
  }

  /* ... and a time to die. */

EXIT:
  JWLOG("aros_scr_thread[%lx]: EXIT\n",thread);

  ObtainSemaphore(&sem_janus_screen_list);

  if(jscr->arosscreen) {
    JWLOG("aros_scr_thread[%lx]: close aros screen %lx\n",thread, jscr->arosscreen);
    PubScreenStatus(jscr->arosscreen, PSNF_PRIVATE);

    CloseScreen(jscr->arosscreen);
    jscr->arosscreen=NULL;
  }

  if(jscr->name) {
    FreeVec(jscr->name);
    jscr->name=NULL;
  }

  if(signal != -1) {
    FreeSignal(signal);
  }

  if(list_screen) {
    JWLOG("aros_scr_thread[%lx]: remove %lx from janus_screens\n",thread, list_screen);
    janus_screens=g_slist_delete_link(janus_screens, list_screen);
    list_screen=NULL;
  }

  if(jscr) {
    JWLOG("aros_scr_thread[%lx]: FreeVec(%lx)\n",thread, jscr);
    FreeVec(jscr);
  }

  ReleaseSemaphore(&sem_janus_screen_list);

  JWLOG("aros_scr_thread[%lx]: dies..\n", thread);
}

/****i* januae-j-screen-thread/aros_screen_start_thread ********************
*
*   NAME
*       aros_screen_start_thread() -- Create a public screen process.
*
*   SYNOPSIS
*       int aros_screen_start_thread(JanusScreen *screen);
*
*   FUNCTION
*       Name the screen worker from its guest address and start the
*       public-screen process under the thread-start semaphore.
*
*   INPUTS
*       screen - Mapping receiving the new process identity and name.
*
*   RESULT
*       Nonzero when a process was created; zero otherwise.
*
****************************************************************************
*
*/

int aros_screen_start_thread (JanusScreen *screen) {

    JWLOG("aros_screen_start_thread(%lx)\n",screen);

    screen->name=AllocVec(8+strlen(SCREEN_TASK_PREFIX_NAME)+1,MEMF_CLEAR);

    sprintf(screen->name,"%s%lx",SCREEN_TASK_PREFIX_NAME,screen->aos3screen);

    ObtainSemaphore(&aos3_thread_start);
    screen->task = (struct Task *)
	    myCreateNewProcTags ( //NP_Output, Output (),
				  //NP_Input, Input (),
				  NP_Name, (ULONG) screen->name,
				  //NP_CloseOutput, FALSE,
				  //NP_CloseInput, FALSE,
				  NP_StackSize, 4096,
				  NP_Priority, 0,
				  NP_Entry, (ULONG) aros_screen_thread,
				  TAG_DONE);

    ReleaseSemaphore(&aos3_thread_start);

    JWLOG("screen thread %lx created\n",screen->task);

    return screen->task != 0;
}
