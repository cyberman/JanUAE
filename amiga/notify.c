/************************************************************************ 
 *
 * Janus-Daemon
 *
 * Copyright 2009-2014 Oliver Brunner - aros<at>oliver-brunner.de
 *
 * This file is part of Janus-Daemon.
 *
 * AmigaOS specific hooks/patches for syncing and triggering
 *
 * Janus-Daemon is free software: you can redistribute it and/or modify
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

#if __AROS__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <intuition/intuitionbase.h>
#include <aros/libcall.h>

#include <proto/exec.h>
#include <proto/utility.h>
#include <proto/intuition.h>

//#define DEBUG 1
#include "janus-daemon.h"

/* global */
ULONG           notify_signal;
struct MsgPort *notify_port;

/* original 68k functions */
APTR old_SetWindowTitles;
APTR old_WindowLimits;

/* should be in libcall.h, shouldn't it !? */
#define __AROS_CALL3(t,n,a1,a2,a3,bt,bn) \
  AROS_UFC4(t,n,AROS_UFCA(a1),AROS_UFCA(a2),AROS_UFCA(a3),AROS_UFCA(bt,bn,A6))


/****************************************************************************
 * new SetWindowTitle function
 ****************************************************************************/
/****i* januae/my_SetWindowTitles_SetFunc **********************************
*
*   NAME
*       my_SetWindowTitles_SetFunc() -- Forward guest title changes.
*
*   SYNOPSIS
*       void my_SetWindowTitles_SetFunc(struct Window *window, CONST_STRPTR
*       windowtitle, CONST_STRPTR screentitle);
*
*   FUNCTION
*       Call the original Intuition SetWindowTitles entry and notify the
*       host when Janus synchronization is active.
*
*   INPUTS
*       window - Window being updated. windowtitle - New window title.
*       screentitle - New screen title.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       This AROS library-function wrapper uses AROS_LH3 and calls the saved
*       original function before the host trap.
*
****************************************************************************
*
*/

AROS_LH3(void, my_SetWindowTitles_SetFunc,
         AROS_LHA(struct Window *, window,      A0),
         AROS_LHA(CONST_STRPTR,    windowtitle, A1),
         AROS_LHA(CONST_STRPTR,    screentitle, A2),
         struct IntuitionBase *, IntuitionBase, 46, Intuition) {

    AROS_LIBFUNC_INIT

  DebOut("(%lx, %s, %s) state is %d\n", window, windowtitle, screentitle, state);

    /* call original library function. Oh man, AROS macros are scary.. */
    AROS_CALL3(void, old_SetWindowTitles,
               AROS_LCA(struct Window *, window, A0),
               AROS_LCA(CONST_STRPTR,    windowtitle, A1),
               AROS_LCA(CONST_STRPTR,    screentitle, A2),
               struct IntuitionBase *, IntuitionBase);

    if(state) {
      calltrap(AD_GET_JOB, AD_GET_JOB_SET_WINDOW_TITLES, (ULONG *) window);
    }
    AROS_LIBFUNC_EXIT
}

/****************************************************************************
 * new WindowLimits function
 ****************************************************************************/
/****i* januae/my_WindowLimits_SetFunc *************************************
*
*   NAME
*       my_WindowLimits_SetFunc() -- Forward guest window limits.
*
*   SYNOPSIS
*       BOOL my_WindowLimits_SetFunc(struct Window *window, WORD MinWidth,
*       WORD MinHeight, UWORD MaxWidth, UWORD MaxHeight);
*
*   FUNCTION
*       Call the original Intuition WindowLimits entry and forward the
*       requested limits to the host while synchronization is active.
*
*   INPUTS
*       window - Window being updated. MinWidth, MinHeight - Minimum
*       dimensions. MaxWidth, MaxHeight - Maximum dimensions.
*
*   RESULT
*       The BOOL returned by the original Intuition call.
*
*   NOTES
*       This AROS library-function wrapper uses AROS_LH5; the host trap
*       result does not replace the original result.
*
****************************************************************************
*
*/

AROS_LH5(BOOL, my_WindowLimits_SetFunc,
         AROS_LHA(struct Window *, window, A0),
         AROS_LHA(WORD,            MinWidth, D0),
         AROS_LHA(WORD,            MinHeight, D1),
         AROS_LHA(UWORD,           MaxWidth, D2),
         AROS_LHA(UWORD,           MaxHeight, D3),
         struct IntuitionBase *, IntuitionBase, 53, Intuition) {

  AROS_LIBFUNC_INIT

  BOOL result;

  DebOut("(%lx, %d, %d, %d, %d) state is %d\n", window, 
               MinWidth, MinHeight, MaxWidth, MaxHeight, state);

  /* call original function */
  result=AROS_CALL5(BOOL, old_WindowLimits,
               AROS_LCA(struct Window *, window, A0),
               AROS_LCA(WORD,            MinWidth, D0),
               AROS_LCA(WORD,            MinHeight, D1),
               AROS_LCA(UWORD,           MaxWidth, D2),
               AROS_LCA(UWORD,           MaxHeight, D3),
               struct IntuitionBase *, IntuitionBase);

  if(state) {
    DebOut("calling AD_GET_JOB_WINDOW_LIMITS..\n");
    calltrap_d01_a0_d2345(AD_GET_JOB, AD_GET_JOB_WINDOW_LIMITS, 
              (ULONG) window, 
              (ULONG) MinWidth, (ULONG) MinHeight,
              (ULONG) MaxWidth, (ULONG) MaxHeight);
    DebOut("called AD_GET_JOB_WINDOW_LIMITS\n");
  }


  return result;
  AROS_LIBFUNC_EXIT
}



/*********************************************************************************
 * setup_notify
 *
 * alloc notify signal
 *********************************************************************************/
/****i* januae/setup_notify ************************************************
*
*   NAME
*       setup_notify() -- Allocate the screen-notification signal.
*
*   SYNOPSIS
*       static ULONG setup_notify(void);
*
*   FUNCTION
*       Create the Janus notification message port and record its signal
*       mask.
*
*   INPUTS
*       None.
*
*   RESULT
*       The signal mask derived from the allocated message port.
*
*   NOTES
*       The function calls exit(1) if CreateMsgPort fails.
*
****************************************************************************
*
*/

static ULONG setup_notify(void) {

  DebOut("entered\n");

  notify_port=CreateMsgPort();
  if(!notify_port) {
    DebOut("ERROR: no notify_port!?\n");
    printf("ERROR: no notify_port!?\n");
    exit(1);
  }
  notify_signal=1L << notify_port->mp_SigBit;

  return notify_signal;
}

/****i* januae/do_notify ***************************************************
*
*   NAME
*       do_notify() -- Register a screen notification.
*
*   SYNOPSIS
*       static APTR do_notify(struct Screen *screen, struct MsgPort *port,
*       ULONG class, ULONG thread);
*
*   FUNCTION
*       Register a screen-notification class for delivery to a message port
*       and task.
*
*   INPUTS
*       screen - Ignored; the current Intuition front screen is used
*       instead. port - Destination message port. class - Notification mask.
*       thread - Notification task address.
*
*   RESULT
*       A notification handle, or NULL if registration fails.
*
*   NOTES
*       When no front screen is available, SNA_UserData is omitted. A
*       registration failure is reported but not fatal.
*
****************************************************************************
*
*/

static APTR do_notify(struct Screen *screen, struct MsgPort *port, ULONG class, ULONG thread) {
  APTR notify;

  ENTER

  screen=IntuitionBase->FirstScreen;
  DebOut("do_notify(class %lx, screen %lx, port %lx, thread %lx)\n", class, screen, port, thread);

  if(screen) {
    notify=StartScreenNotifyTags(SNA_MsgPort,  port,
            SNA_UserData, screen,
            SNA_SigTask,  thread,
            SNA_Notify,   class,
            TAG_END); 
  }
  else {
    notify=StartScreenNotifyTags(SNA_MsgPort,  port,
            SNA_SigTask,  thread,
            SNA_Notify,   class,
            TAG_END); 

  }

  if(!notify) {
    DebOut("ERROR: StartScreenNotifyTags(%lx) failed\n", class);
    printf("ERROR: StartScreenNotifyTags(%lx) failed\n", class);
  }

  LEAVE

  return notify;
}


/****************************************************
 * patch_functions
 *
 * Use notifications, wherever possible. On AmigaOS
 * systems (<4), SetPatch is required to force
 * notifications.
 *
 * - OpenWindow
 *   It feels too slow, to wait for the window 
 *   sync cycle, as intuition
 *   also needs some time to open the window.
 *   Also it is necessary, to update the public
 *   screen list, in case the window should be
 *   opened on a public screen, which not yet
 *   exists as an AROS public screen.
 *
 * - CloseWindow
 *   If a window is closed, the clonewindow
 *   function has no chance, to detect that.
 *   So we need to detect a CloseWindow, *before*
 *   it is closed.
 *
 ****************************************************/
/****i* januae/patch_functions *********************************************
*
*   NAME
*       patch_functions() -- Install AROS synchronization notifications.
*
*   SYNOPSIS
*       void patch_functions(void);
*
*   FUNCTION
*       Register screen and window notifications and replace the Intuition
*       title and window-limit entry points with Janus wrappers.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       This is the AROS implementation under #if __AROS__. Registration
*       handles are not retained.
*
*   BUGS
*       Failed notification registrations do not stop subsequent setup, and
*       the routine provides no cleanup or rollback on partial failure.
*
****************************************************************************
*
*/

void patch_functions(void ) {
  ULONG thread= (ULONG) FindTask(NULL);

  ENTER

  setup_notify();

  DebOut("Installing notifications..\n");

  do_notify(NULL, notify_port, SNOTIFY_BEFORE_OPENWINDOW |SNOTIFY_WAIT_REPLY, thread);
  do_notify(NULL, notify_port, SNOTIFY_AFTER_OPENWINDOW  |SNOTIFY_WAIT_REPLY, thread);

  do_notify(NULL, notify_port, SNOTIFY_BEFORE_CLOSEWINDOW|SNOTIFY_WAIT_REPLY, thread);
  do_notify(NULL, notify_port, SNOTIFY_AFTER_CLOSEWINDOW, thread);

  do_notify(NULL, notify_port, SNOTIFY_AFTER_OPENSCREEN  |SNOTIFY_WAIT_REPLY, thread);
  do_notify(NULL, notify_port, SNOTIFY_BEFORE_CLOSESCREEN|SNOTIFY_WAIT_REPLY, thread);

  //do_notify(NULL, notify_port, SNOTIFY_SCREENDEPTH       |SNOTIFY_WAIT_REPLY, thread);

/*
 * so far, everything could be archieved with nice
 * notifications. But there are some things, we need
 * to patch:
 *
 * - setwindowtitle
 */

 DebOut("patch functions..\n");

  old_SetWindowTitles=SetFunction((struct Library *) IntuitionBase, -46 * LIB_VECTSIZE, 
			      Intuition_46_my_SetWindowTitles_SetFunc);
 
  old_WindowLimits=SetFunction((struct Library *) IntuitionBase, -53 * LIB_VECTSIZE, 
			      Intuition_53_my_WindowLimits_SetFunc);
 

  LEAVE
}

extern struct IntuitionBase* IntuitionBase;

/****************************************************
 * TODO(?)
 *
 * - OpenScreen
 *   If it is a custom screen, we want
 *   to open up an according screen
 *   on AROS. If it is a public
 *   screen, we can care for that,
 *   if a window opens on the
 *   screen.
 *
 * - CloseScreen
 *   We need it for custom screens
 *
 * - ModifyIDCMP
 *   MUI windows seem to use this quite
 *   often. Without this patch, menus
 *   might not work.
 *
 * - AddGadget/AddGList/RemoveGadget/RemoveGList
 *   If someone adds/removes gadgets to his 
 *   window, we might need to do that,
 *   too.
 *
 * - SetWindowTitles
 *   if they update, we follow
 *
 * - WindowLimits
 *   if it updates, we follow
 */

struct TagItem       mytags_linked[2]={ { SA_Draggable, FALSE}, { TAG_MORE, 0}};
struct TagItem       mytags[2]={ { SA_Draggable, FALSE}, { TAG_DONE, TAG_DONE}};
struct ExtNewScreen  nodrag_newscreen;
struct TagItem       taglist_mytags_linked[2]={ { SA_Draggable, FALSE}, { TAG_MORE, 0}};
struct TagItem       taglist_mytags[2]={ { SA_Draggable, FALSE}, { TAG_DONE, TAG_DONE}};


/*********************************************************************************
 * remove_dragging
 *
 * set SA_Draggable to FALSE, AROS has no screen dragging ;)
 *********************************************************************************/
/****i* januae/remove_dragging *********************************************
*
*   NAME
*       remove_dragging() -- Prepare a nondraggable screen descriptor.
*
*   SYNOPSIS
*       ULONG remove_dragging(struct NewScreen *newscreen);
*
*   FUNCTION
*       Copy a NewScreen descriptor into shared extended storage and attach
*       an SA_Draggable=FALSE tag.
*
*   INPUTS
*       newscreen - Screen description to copy.
*
*   RESULT
*       Address of the shared extended screen description, encoded as ULONG.
*
*   NOTES
*       The input description is not modified; the returned descriptor and
*       linked tag storage are shared and overwritten on later calls.
*
****************************************************************************
*
*/

#ifndef __AROS__
ULONG remove_dragging (struct NewScreen *newscreen __asm("a0")) {
#else
ULONG remove_dragging (struct NewScreen *newscreen) {
#endif
  struct TagItem      *tags;
  struct TagItem      *tstate;
  struct TagItem      *tag;
  ULONG result;

  DebOut("entered remove_dragging\n");
  DebOut("newscreen: %lx\n", newscreen);
  DebOut(" name: %s\n",newscreen->DefaultTitle);
  DebOut(" type: %x\n",newscreen->Type);

  if((newscreen->Type & SCREENTYPE) != CUSTOMSCREEN) {
    DebOut(" no custom screen\n");
    /* return; */
  }

  if(! (newscreen->Type & NS_EXTENDED)) {
    /* allocvec a new ExtNewScreen structure! */
    DebOut(" not extended\n");
    CopyMem( newscreen, &nodrag_newscreen, sizeof(struct NewScreen));
    nodrag_newscreen.Extension=NULL;
    nodrag_newscreen.Type = nodrag_newscreen.Type | NS_EXTENDED;
  }
  else {
    DebOut(" extended\n");
    CopyMem( newscreen, &nodrag_newscreen, sizeof(struct ExtNewScreen));
  }

  /* now we have a NS_EXTENDED nodrag_newscreen */

  DebOut(" new screen with Extension: %lx\n");

  tags=nodrag_newscreen.Extension; 
  DebOut(" Tags: %lx\n", tags);

  tstate=tags;
  while((tag = NextTagItem(&tstate))) {
    DebOut("  TAG    : %08lx = %08lx\n", tag->ti_Tag - 0x80000020, tag->ti_Data); /* - SA_Dummy */
  }

  if(tags) {
    DebOut(" we have tags\n"); /* TODO: care, if Draggable was set to TRUE ? who would do that..? */
    mytags_linked[1].ti_Data=(ULONG) nodrag_newscreen.Extension;
    nodrag_newscreen.Extension=mytags_linked;
  }
  else {
    nodrag_newscreen.Extension=mytags;
  }

  DebOut("after the patch:\n");
  tags=nodrag_newscreen.Extension;
  DebOut(" Tags at: %lx\n", tags);

  tstate=tags;
  while((tag = NextTagItem(&tstate))) {
    DebOut("  TAG    : %08lx = %08lx\n", tag->ti_Tag - 0x80000020, tag->ti_Data); /* - SA_Dummy */
  }

  result=(ULONG) &nodrag_newscreen;

  return result;
}

/* 
 * use taglist_mytags_linked here and not mytags_linked, as OpenScreen might call
 * OpenScreenTagList.. and so we would cause wrong links 
 */
/****i* januae/remove_dragging_TagList *************************************
*
*   NAME
*       remove_dragging_TagList() -- Prepare nondraggable screen tags.
*
*   SYNOPSIS
*       ULONG remove_dragging_TagList(struct TagItem *original_tags);
*
*   FUNCTION
*       Prepend SA_Draggable=FALSE to an existing tag list, or return a
*       standalone default tag list.
*
*   INPUTS
*       original_tags - Original tags or NULL.
*
*   RESULT
*       Pointer to shared tag storage, encoded as ULONG.
*
*   NOTES
*       Returned shared storage is overwritten by later calls.
*
****************************************************************************
*
*/

#ifndef __AROS__
ULONG remove_dragging_TagList (struct TagItem *original_tags __asm("a1")) {
#else
ULONG remove_dragging_TagList (struct TagItem *original_tags) {
#endif
  struct TagItem      *tstate;
  struct TagItem      *tag;

  DebOut("entered remove_dragging_TagList\n");

  tstate=original_tags;
  while((tag = NextTagItem(&tstate))) {
    DebOut("  TAG    : %08lx = %08lx\n", tag->ti_Tag - 0x80000020, tag->ti_Data); /* - SA_Dummy */
  }

  if(!original_tags) {
    return (ULONG) taglist_mytags;
  }

  taglist_mytags_linked[1].ti_Data=(ULONG) original_tags;
  return (ULONG) taglist_mytags_linked;
}

/****i* januae/do_update_screens *******************************************
*
*   NAME
*       do_update_screens() -- Refresh guest screen metadata.
*
*   SYNOPSIS
*       void do_update_screens(void);
*
*   FUNCTION
*       Invoke update_screens to report guest screen information to the
*       host.
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

void do_update_screens (void) {
  update_screens();
}

/****i* januae/handle_notify_msg *******************************************
*
*   NAME
*       handle_notify_msg() -- Dispatch AROS screen notifications.
*
*   SYNOPSIS
*       void handle_notify_msg(ULONG notify_class, ULONG notify_object);
*
*   FUNCTION
*       Forward supported Intuition window and screen notifications to their
*       corresponding Janus host services.
*
*   INPUTS
*       notify_class - Intuition notification class. notify_object -
*       Affected screen or window address.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Notifications are processed only when the daemon state is active.
*       Window open/close, screen open/close and screen-depth services are
*       recognized.
*
*   BUGS
*       The SNOTIFY_SCREENDEPTH handler is present but its notification
*       registration is commented out.
*
****************************************************************************
*
*/

void handle_notify_msg(ULONG notify_class, ULONG notify_object) {

  //notify_code      = notify_msg->snm_Code;
  //notify_userdata  = notify_msg->snm_UserData;

  DebOut("notify_class %lx, notify_object %lx, state %d\n", notify_class, notify_object, state);

  if(state) {

    switch(notify_class) {
      /*********** Window handling ************/
      case SNOTIFY_BEFORE_OPENWINDOW: DebOut("SNOTIFY_BEFORE_OPENWINDOW\n");
        calltrap(AD_GET_JOB, AD_GET_JOB_WINDOW_GFX_UPDATE, NULL);
        update_screens();
        DebOut("SNOTIFY_BEFORE_OPENWINDOW done\n");
        break;
      case SNOTIFY_AFTER_OPENWINDOW: DebOut("SNOTIFY_AFTER_OPENWINDOW\n");
        calltrap(AD_GET_JOB, AD_GET_JOB_NEW_WINDOW, (ULONG *) notify_object);
        calltrap(AD_GET_JOB, AD_GET_JOB_WINDOW_GFX_UPDATE, (ULONG *) 1);
        DebOut("SNOTIFY_AFTER_OPENWINDOW done\n");
        break;

      case SNOTIFY_BEFORE_CLOSEWINDOW: DebOut("SNOTIFY_BEFORE_CLOSEWINDOW\n");
        calltrap(AD_GET_JOB, AD_GET_JOB_MARK_WINDOW_DEAD, (ULONG *) notify_object);
        break;
      case SNOTIFY_AFTER_CLOSEWINDOW: DebOut("SNOTIFY_AFTER_CLOSEWINDOW\n");
        calltrap(AD_GET_JOB, AD_GET_JOB_WINDOW_CLOSED, (ULONG *) notify_object);
        break;


      /*********** Screen handling ************/
      /*case SNOTIFY_BEFORE_OPENSCREEN: DebOut("SNOTIFY_BEFORE_OPENSCREEN\n");
        remove tagging ..!? 
        break;*/
      case SNOTIFY_AFTER_OPENSCREEN: DebOut("SNOTIFY_AFTER_OPENSCREEN\n");
        update_screens();
        break;

      case SNOTIFY_BEFORE_CLOSESCREEN: DebOut("SNOTIFY_BEFORE_CLOSESCREEN\n");
        calltrap(AD_GET_JOB, AD_GET_JOB_CLOSE_SCREEN, (ULONG *) notify_object);
        break;
      /*case SNOTIFY_AFTER_CLOSESCREEN: DebOut("SNOTIFY_AFTER_CLOSESCREEN\n");
        break;*/

      case SNOTIFY_SCREENDEPTH: DebOut("SNOTIFY_SCREENDEPTH\n");
        /* WARNING: THIS IS MAYBE WRONG, have a look at the 666 hack.. */
        calltrap(AD_GET_JOB, AD_GET_JOB_SCREEN_DEPTH, (ULONG *) notify_object);
        break;

      default: printf("unknown(%lx)\n", notify_class);
        break;
    }
  }



}
  
#endif
