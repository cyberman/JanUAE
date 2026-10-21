/************************************************************************ 
 *
 * Janus-Daemon
 *
 * Copyright 2009-2014 Oliver Brunner - aros<at>oliver-brunner.de
 *
 * This file is part of Janus-Daemon.
 *
 * AmigaOS specific hooks/patches for syncing and triggering.
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


#include <string.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include "janus-daemon.h"

#ifndef __AROS__
extern struct IntuitionBase* IntuitionBase;

/****************************************************
 * patch_functions
 *
 * Although I tried to avoid patching the OS, I
 * have to patch the following:
 *
 * - CloseWindow
 *   If a window is closed, the clonewindow
 *   function has no chance, to detect that.
 *   So we need to detect a CloseWindow, *before*
 *   it is closed.
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

APTR           old_CloseWindow;
APTR           old_OpenWindow;
APTR           old_OpenWindowTagList;
APTR           old_CloseScreen;
APTR           old_OpenScreen;
APTR           old_OpenScreenTagList;
APTR           old_ScreenDepth;
APTR           old_ModifyIDCMP;
APTR           old_AddGadget;
APTR           old_AddGList;
APTR           old_RemoveGadget;
APTR           old_RemoveGList;
APTR           old_RefreshGList;
APTR           old_RefreshGadgets;
APTR           old_SetWindowTitles;
APTR           old_WindowLimits;

#define PUSHFULLSTACK "movem.l d0-d7/a0-a6,-(SP)\n"
#define POPFULLSTACK  "movem.l (SP)+,d0-d7/a0-a6\n"
#define PUSHSTACK     "movem.l d2-d7/a2-a6,-(SP)\n"
#define POPSTACK      "movem.l (SP)+,d2-d7/a2-a6\n"
#define PUSHA3        "move.l a3,-(SP)\n"
#define POPA3         "move.l (SP)+,a3\n"
#define PUSHD3        "move.l d3,-(SP)\n"
#define POPD3         "move.l (SP)+,d3\n"
#define PUSHA0        "move.l a0,-(SP)\n"
#define POPA0         "move.l (SP)+,a0\n"

#ifndef __AROS__
/*********************************************************************************
 * _my_CloseWindow_SetFunc
 *
 * done in assembler, as C-Source always at least destroys the
 * a5 register for the local variable stack frame. But library
 * functions may only trash d0, d1, a0 and a1.
 *
 * for calltrap:
 * AD_GET_JOB  11 (d0)
 * AD_GET_JOB_MARK_WINDOW_DEAD 7 (d1)
 * AD_GET_JOB_WINDOW_CLOSED 24 (d1)
 * window is already in a0
 *********************************************************************************/
/****i* januae/my_CloseWindow_SetFunc **************************************
*
*   NAME
*       my_CloseWindow_SetFunc() -- Track window closure.
*
*   SYNOPSIS
*       Assembler entry: _my_CloseWindow_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Intercept CloseWindow before and after the original call.
*
*   INPUTS
*       The A0 register carries the guest window address.
*
*   RESULT
*       The original CloseWindow operation is invoked.
*
*   NOTES
*       With the daemon active, sends jobs 7 and 24 around the original
*       call. This symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_CloseWindow_SetFunc:\n"
	"cmp.l #1,_state\n"
	"blt close_patch_disabled\n"
	PUSHFULLSTACK
	"moveq #11,d0\n"
	"moveq #7,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPFULLSTACK
	PUSHA0
	"close_patch_disabled:\n"
	PUSHA3
	"move.l _old_CloseWindow, a3\n"
	"jsr (a3)\n"
	POPA3
	"cmp.l #1,_state\n"
	"blt close_patch_disabled2\n"
	POPA0
	PUSHFULLSTACK
	"moveq #11,d0\n"
	"moveq #24,d1\n"
	"move.l _calltrap, a1\n"
	"jsr (a1)\n"
	POPFULLSTACK
	"close_patch_disabled2:\n"
  "rts\n");

/*********************************************************************************
 * _my_OpenWindow_SetFunc
 *
 * done in assembler, as C-Source always at least destroys the
 * a5 register for the local variable stack frame. But library
 * functions may only trash d0, d1, a0 and a1.
 *
 * for calltrap:
 * AD_GET_JOB  11 (d0)
 * AD_GET_JOB_NEW_WINDOW 11 (d1)
 * new window in a0
 *********************************************************************************/
/****i* januae/my_OpenWindow_SetFunc ***************************************
*
*   NAME
*       my_OpenWindow_SetFunc() -- Track window creation.
*
*   SYNOPSIS
*       Assembler entry: _my_OpenWindow_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Notify the host around an OpenWindow call.
*
*   INPUTS
*       The original OpenWindow vector registers, including A0.
*
*   RESULT
*       The original OpenWindow result is returned.
*
*   NOTES
*       With the daemon active, refreshes screens before opening and sends
*       job 11 after the call. This symbol is defined only for the AmigaOS
*       guest.
*
****************************************************************************
*
*/

__asm__("_my_OpenWindow_SetFunc:\n"
/*
	"movem.l d4/a4,-(SP)\n"
	"lea 24(a0),a4\n"
	"move.l (a4),d4\n"
	"clr.l (a4)\n"
	*/
  "cmp.l #1,_state\n"
	"blt openwin_patch_disabled1\n"
    PUSHFULLSTACK
    "jsr _update_screens\n"
    POPFULLSTACK
	"openwin_patch_disabled1:\n"
	PUSHA3
	"move.l _old_OpenWindow,a3\n"
	"jsr (a3)\n"
	POPA3
	/*
	"move.l d4,(a4)\n"
	"movem.l (SP)+,d4/a4\n"
	*/
	"cmp.l #1,_state\n"
	"blt openwin_patch_disabled2\n"
    PUSHFULLSTACK
    "move.l d0,a0\n"
    "moveq #11,d0\n"
    "moveq #11,d1\n"
    "move.l _calltrap,a1\n"
    "jsr (a1)\n"
    POPFULLSTACK
	"openwin_patch_disabled2:\n"
  "rts\n");

/*********************************************************************************
 * _my_OpenWindowTagList_SetFunc
 *********************************************************************************/
/****i* januae/my_OpenWindowTagList_SetFunc ********************************
*
*   NAME
*       my_OpenWindowTagList_SetFunc() -- Track tagged window creation.
*
*   SYNOPSIS
*       Assembler entry: _my_OpenWindowTagList_SetFunc (AmigaOS Intuition
*       vector ABI).
*
*   FUNCTION
*       Notify the host around an OpenWindowTagList call.
*
*   INPUTS
*       The original OpenWindowTagList vector registers.
*
*   RESULT
*       The original OpenWindowTagList result is returned.
*
*   NOTES
*       With the daemon active, refreshes screens before opening and sends
*       job 11 afterward. This symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_OpenWindowTagList_SetFunc:\n"
	"cmp.l #1,_state\n"
	"blt openwintag_patch_disabled1\n"
	PUSHFULLSTACK
	"jsr _update_screens\n"
	POPFULLSTACK
	"openwintag_patch_disabled1:\n"
	PUSHA3
	"move.l _old_OpenWindowTagList, a3\n"
	"jsr (a3)\n"
	POPA3
	"cmp.l #1,_state\n"
	"blt openwintag_patch_disabled2\n"
	PUSHFULLSTACK
	"move.l d0,a0\n"
	"moveq #11,d0\n"
	"moveq #11,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPFULLSTACK
	"openwintag_patch_disabled2:\n"
        "rts\n");
#endif /* AROS */


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
*       remove_dragging() -- Prepare a non-draggable screen.
*
*   SYNOPSIS
*       ULONG remove_dragging(struct NewScreen *newscreen);
*
*   FUNCTION
*       Prepare a copy of a screen descriptor with SA_Draggable disabled.
*
*   INPUTS
*       newscreen - Existing screen descriptor.
*
*   RESULT
*       Address of the shared modified ExtNewScreen descriptor.
*
*   NOTES
*       AmigaOS guest build only. The result points to static storage; it
*       must not be freed or retained across another call.
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
*       remove_dragging_TagList() -- Prepend screen attributes.
*
*   SYNOPSIS
*       ULONG remove_dragging_TagList(struct TagItem *original_tags);
*
*   FUNCTION
*       Prepend a non-draggable screen attribute to a tag list.
*
*   INPUTS
*       original_tags - Original attribute list or NULL.
*
*   RESULT
*       Address of the shared replacement tag list.
*
*   NOTES
*       AmigaOS guest build only. The returned tag arrays have static
*       storage and are reused.
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
*       do_update_screens() -- Refresh the guest screen inventory.
*
*   SYNOPSIS
*       void do_update_screens(void);
*
*   FUNCTION
*       Forward an Intuition screen-change event to update_screens.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       AmigaOS guest build only; called by the OpenWindow assembler hooks.
*
****************************************************************************
*
*/

void do_update_screens (void) {
  update_screens();
}



/*********************************************************************************
 * _my_OpenScreen_SetFunc
 *
 * We need to care for new custom screens. Public screens are handled, as soon
 * as the first window is opened on them.
 * AD_GET_JOB_OPEN_CUSTOM_SCREEN has to check, if it is really a custom
 * screen. We could do that here, but there are not many OpenScreens
 * at all, so performance is no issue.
 *
 * We don't want draggable screens, as we run into trouble with updates of
 * the background, if for example behind the amigaOS screen a native
 * aros screen should be visible. So we create a new NewScreen struct
 * and patch it so that it has SA_Draggable=FALSE. The original
 * struct stays intact (including the Taglist).
 *
 * We might need to define exceptions from that, as stuff like
 * Brilliance, which uses two screens at the same time, will
 * stop to work otherwise. This is a TODO!
 *
 * for calltrap:
 * AD_GET_JOB  11                   (d0)
 * AD_GET_JOB_OPEN_CUSTOM_SCREEN 13 (d1)
 * new screen                       (a0)
 *
 * ad_job_open_custom_screen is a dummy at the moment. Screens are
 * opened with sync-screen.
 *
 *********************************************************************************/
#ifndef __AROS__
/****i* januae/my_OpenScreen_SetFunc ***************************************
*
*   NAME
*       my_OpenScreen_SetFunc() -- Process screen creation.
*
*   SYNOPSIS
*       Assembler entry: _my_OpenScreen_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Apply screen drag policy and report new screens.
*
*   INPUTS
*       The original OpenScreen descriptor in A0 and vector register
*       arguments.
*
*   RESULT
*       Returns from the original OpenScreen call.
*
*   NOTES
*       When patch_draggable is enabled, substitutes a copy of the
*       descriptor. With the daemon active, refreshes the screen list. This
*       symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_OpenScreen_SetFunc:\n"
        PUSHA0
	"cmp.l #1,_patch_draggable\n"
	"bne no_patch_draggable\n"
        "movem.l d1-d7/a1-a6,-(SP)\n"
	"jsr _remove_dragging\n"      /* we get back a new struct Screen here in D0*/
        "movem.l (SP)+,d1-d7/a1-a6\n"
	/* call original function */
	"move.l d0,a0\n"
	"no_patch_draggable:\n"
	PUSHA3
	"move.l _old_OpenScreen,a3\n"
	"jsr (a3)\n"
	POPA3
	POPA0                        /* restore original struct Screen */
	/* check, if we are disabled */
	"cmp.l #1,_state\n"
	"blt openscreen_patch_disabled\n"
	PUSHFULLSTACK
	"jsr _update_screens\n"
	POPFULLSTACK
	"openscreen_patch_disabled:\n"
        "rts\n");


/*********************************************************************************
 * _my_OpenScreenTagList_SetFunc
 *
 * We need to care for new custom screens. Public screens are handled, as soon
 * as the first window is opened on them.
 * AD_GET_JOB_OPEN_CUSTOM_SCREEN has to check, if it is really a custom
 * screen. We could do that here, but there are not many OpenScreens
 * at all, so performance is no issue.
 *
 * for calltrap:
 * AD_GET_JOB  11                   (d0)
 * AD_GET_JOB_OPEN_CUSTOM_SCREEN 13 (d1)
 * new screen                       (a0)
 *
 * AD_GET_JOB_OPEN_CUSTOM_SCREEN is not used anymore
 *********************************************************************************/
/****i* januae/my_OpenScreenTagList_SetFunc ********************************
*
*   NAME
*       my_OpenScreenTagList_SetFunc() -- Process tagged screen creation.
*
*   SYNOPSIS
*       Assembler entry: _my_OpenScreenTagList_SetFunc (AmigaOS Intuition
*       vector ABI).
*
*   FUNCTION
*       Apply screen tag policy and report new screens.
*
*   INPUTS
*       The original OpenScreenTagList arguments, including A1.
*
*   RESULT
*       Returns from the original OpenScreenTagList call.
*
*   NOTES
*       Prepends SA_Draggable=FALSE to the tag list and refreshes screens
*       when the daemon is active. This symbol is defined only for the
*       AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_OpenScreenTagList_SetFunc:\n"
        "move.l  a0,-(SP)\n"             /* backup struct NewScreen   */
        "movem.l d0-d7/a2-a6,-(SP)\n"    /* backup everything but A1  */
	"jsr _remove_dragging_TagList\n"
	"move.l d0, a1\n"                /* new TagList               */
        "movem.l (SP)+,d0-d7/a2-a6\n"    /* restore everything but A1 */
        "move.l  (SP)+,a0\n"             /* restore struct NewScreen  */
	/* call original function */
	PUSHA3
	"move.l _old_OpenScreenTagList,a3\n"
	"jsr (a3)\n"
	POPA3
	/* check, if we are disabled */
	"cmp.l #1,_state\n"
	"blt openscreentags_patch_disabled\n"
	PUSHFULLSTACK
	"jsr _update_screens\n"
	POPFULLSTACK
	"openscreentags_patch_disabled:\n"
	
        "rts\n");


/*********************************************************************************
 * _my_CloseScreen_SetFunc
 *
 * done in assembler, as C-Source always at least destroys the
 * a5 register for the local variable stack frame. But library
 * functions may only trash d0, d1, a0 and a1.
 *
 * for calltrap:
 * AD_GET_JOB  11 (d0)
 * AD_GET_JOB_CLOSE_SCREEN 14 (d1)
 * screen is already in a0
 *********************************************************************************/
/****i* januae/my_CloseScreen_SetFunc **************************************
*
*   NAME
*       my_CloseScreen_SetFunc() -- Track screen closure.
*
*   SYNOPSIS
*       Assembler entry: _my_CloseScreen_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Notify the host before closing a guest screen.
*
*   INPUTS
*       A0 contains the screen address.
*
*   RESULT
*       Invokes the saved original CloseScreen vector.
*
*   NOTES
*       With the daemon active, sends AD_GET_JOB_CLOSE_SCREEN (14) before
*       closing. This symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_CloseScreen_SetFunc:\n"
	"cmp.l #1,_state\n"
	"blt close_screen_patch_disabled\n"
	PUSHSTACK
	"moveq #11,d0\n"
	"moveq #14,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPSTACK
	"close_screen_patch_disabled:\n"
	PUSHA3
	"move.l _old_CloseScreen, a3\n"
	"jsr (a3)\n"
	POPA3

        "rts\n");

/*********************************************************************************
 * _my_ScreenDepth_SetFunc
 *
 * done in assembler, as C-Source always at least destroys the
 * a5 register for the local variable stack frame. But library
 * functions may only trash d0, d1, a0 and a1.
 *
 * We won't let the screen depth change here. We call uae, let
 * it change the AROS screen for us and then wait, until
 * janusd changes the screen itself (with A1 == 666).
 *
 * For the moment, we only patch SDEPTH_TOBACK, but I am
 * unsure, if this is the right thing to do.
 *
 * for calltrap:
 * AD_GET_JOB  11 (d0)
 * AD_GET_JOB_SCREEN_DEPTH 16 (d1)
 * screen is already in a0
 * send flags in d3, as d0 is used for AD_GET_JOB
 *********************************************************************************/
/****i* januae/my_ScreenDepth_SetFunc **************************************
*
*   NAME
*       my_ScreenDepth_SetFunc() -- Dispatch eligible depth changes.
*
*   SYNOPSIS
*       Assembler entry: _my_ScreenDepth_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Dispatch eligible guest ScreenDepth requests to the host.
*
*   INPUTS
*       Original ScreenDepth arguments in Intuition vector registers.
*
*   RESULT
*       Uses the original path except when a qualifying request takes the
*       host path.
*
*   NOTES
*       With state active, A1 other than 666, and D0 equal to 1, issues job
*       16; A1=666 bypasses the host path. This symbol is defined only for
*       the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_ScreenDepth_SetFunc:\n"
	"cmp.l #1,_state\n"
	"blt call_old_ScreenDepth\n"
	"cmp.l #666, a1\n"
	"beq call_old_ScreenDepth\n"
	"cmp.l #1, d0\n"
	"bne call_old_ScreenDepth\n"
	PUSHSTACK
	"move.l d0,d3\n"
	"moveq #11,d0\n"
	"moveq #16,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPSTACK
        "rts\n"
	"call_old_ScreenDepth:\n"
	PUSHA3
	"sub.l a1, a1\n"
	"move.l _old_ScreenDepth, a3\n"
	"jsr (a3)\n"
	POPA3
	"rts\n"
	);

/*********************************************************************************
 * _my_ModifyIDCMP_SetFunc
 *
 * done in assembler, as C-Source always at least destroys the
 * a5 register for the local variable stack frame. But library
 * functions may only trash d0, d1, a0 and a1.
 *
 * for calltrap:
 * AD_GET_JOB  11 (d0)
 * AD_GET_JOB_MODIFY_IDCMP 17 (d1)
 * new IDCMP flags (d3)
 * window (a0)
 *********************************************************************************/
/****i* januae/my_ModifyIDCMP_SetFunc **************************************
*
*   NAME
*       my_ModifyIDCMP_SetFunc() -- Observe IDCMP changes.
*
*   SYNOPSIS
*       Assembler entry: _my_ModifyIDCMP_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Forward IDCMP changes before the original call.
*
*   INPUTS
*       Window pointer and IDCMP arguments in their original vector
*       registers.
*
*   RESULT
*       Invokes the saved original ModifyIDCMP vector.
*
*   NOTES
*       Issues AD_GET_JOB_MODIFY_IDCMP (17). Its SetFunction registration is
*       disabled in patch_functions. This symbol is defined only for the
*       AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_ModifyIDCMP_SetFunc:\n"
	PUSHFULLSTACK
	"move.l d0,d3\n"
	"moveq #11,d0\n"
	"moveq #17,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPFULLSTACK
	PUSHA3
	"move.l _old_ModifyIDCMP, a3\n"
	"jsr (a3)\n"
	POPA3
	"rts\n"
	);

/*********************************************************************************
 * _my_AddGadget_SetFunc
 *
 * done in assembler, as C-Source always at least destroys the
 * a5 register for the local variable stack frame. But library
 * functions may only trash d0, d1, a0 and a1.
 *
 * We call the original function, then we need to react on the
 * changes.
 *
 * for calltrap:
 * AD_GET_JOB  11 (d0)
 * AD_GET_JOB_UPDATE_GADGETS 18 (d1)
 * window is already in a0
 *********************************************************************************/
/****i* januae/my_AddGadget_SetFunc ****************************************
*
*   NAME
*       my_AddGadget_SetFunc() -- Observe gadget additions.
*
*   SYNOPSIS
*       Assembler entry: _my_AddGadget_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Report AddGadget updates after the original call.
*
*   INPUTS
*       The original AddGadget vector arguments.
*
*   RESULT
*       Invokes the saved original AddGadget vector.
*
*   NOTES
*       Issues job 18 when active. Registration is disabled in
*       patch_functions. This symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_AddGadget_SetFunc:\n"
	PUSHA0
	PUSHA3
	"move.l _old_AddGadget, a3\n"
	"jsr (a3)\n"
	POPA3
	POPA0

	"cmp.l #1,_state\n"
	"blt addgadget_patch_disabled\n"
	PUSHSTACK
	"moveq #11,d0\n"
	"moveq #18,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPSTACK
	"addgadget_patch_disabled:\n"

        "rts\n");

/****i* januae/my_AddGList_SetFunc *****************************************
*
*   NAME
*       my_AddGList_SetFunc() -- Observe gadget-list additions.
*
*   SYNOPSIS
*       Assembler entry: _my_AddGList_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Report AddGList updates after the original call.
*
*   INPUTS
*       The original AddGList vector arguments.
*
*   RESULT
*       Invokes the saved original AddGList vector.
*
*   NOTES
*       Issues job 18 when active. Registration is disabled in
*       patch_functions. This symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_AddGList_SetFunc:\n"
	PUSHA0
	PUSHA3
	"move.l _old_AddGList, a3\n"
	"jsr (a3)\n"
	POPA3
	POPA0

	"cmp.l #1,_state\n"
	"blt addglist_patch_disabled\n"
	PUSHSTACK
	"moveq #11,d0\n"
	"moveq #18,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPSTACK
	"addglist_patch_disabled:\n"

        "rts\n");

/****i* januae/my_RemoveGadget_SetFunc *************************************
*
*   NAME
*       my_RemoveGadget_SetFunc() -- Observe gadget removals.
*
*   SYNOPSIS
*       Assembler entry: _my_RemoveGadget_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Report RemoveGadget updates after the original call.
*
*   INPUTS
*       The original RemoveGadget vector arguments.
*
*   RESULT
*       Invokes the saved original RemoveGadget vector.
*
*   NOTES
*       Issues job 18 when active. Registration is disabled in
*       patch_functions. This symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_RemoveGadget_SetFunc:\n"
	PUSHA0
	PUSHA3
	"move.l _old_RemoveGadget, a3\n"
	"jsr (a3)\n"
	POPA3
	POPA0

	"cmp.l #1,_state\n"
	"blt removegadget_patch_disabled\n"
	PUSHSTACK
	"moveq #11,d0\n"
	"moveq #18,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPSTACK
	"removegadget_patch_disabled:\n"

        "rts\n");

/****i* januae/my_RemoveGList_SetFunc **************************************
*
*   NAME
*       my_RemoveGList_SetFunc() -- Observe gadget-list removals.
*
*   SYNOPSIS
*       Assembler entry: _my_RemoveGList_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Report RemoveGList updates after the original call.
*
*   INPUTS
*       The original RemoveGList vector arguments.
*
*   RESULT
*       Invokes the saved original RemoveGList vector.
*
*   NOTES
*       Issues job 18 when active. Registration is disabled in
*       patch_functions. This symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_RemoveGList_SetFunc:\n"
	PUSHA0
	PUSHA3
	"move.l _old_RemoveGList, a3\n"
	"jsr (a3)\n"
	POPA3
	POPA0

	"cmp.l #1,_state\n"
	"blt removeglist_patch_disabled\n"
	PUSHSTACK
	"moveq #11,d0\n"
	"moveq #18,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPSTACK
	"removeglist_patch_disabled:\n"

        "rts\n");

/****i* januae/my_RefreshGList_SetFunc *************************************
*
*   NAME
*       my_RefreshGList_SetFunc() -- Observe gadget-list refreshes.
*
*   SYNOPSIS
*       Assembler entry: _my_RefreshGList_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Report RefreshGList updates after the original call.
*
*   INPUTS
*       The original RefreshGList vector arguments.
*
*   RESULT
*       Invokes the saved original RefreshGList vector.
*
*   NOTES
*       Issues job 18 when active; the refresh hooks are registered by
*       patch_functions. This symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_RefreshGList_SetFunc:\n"
	"movem.l a0/a1/a3,-(SP)\n"  
	"move.l _old_RefreshGList, a3\n"
	"jsr (a3)\n"
	"movem.l (SP)+,a0/a1/a3\n"  

	"cmp.l #1,_state\n"
	"blt refreshglist_patch_disabled\n"
	PUSHSTACK
	"move.l a1,a3\n" /* supply window parameter in a3 */
	"moveq #11,d0\n"
	"move.l #18,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPSTACK
	"refreshglist_patch_disabled:\n"

        "rts\n");

/****i* januae/my_RefreshGadgets_SetFunc ***********************************
*
*   NAME
*       my_RefreshGadgets_SetFunc() -- Observe gadget refreshes.
*
*   SYNOPSIS
*       Assembler entry: _my_RefreshGadgets_SetFunc (AmigaOS Intuition
*       vector ABI).
*
*   FUNCTION
*       Report RefreshGadgets updates after the original call.
*
*   INPUTS
*       The original RefreshGadgets vector arguments.
*
*   RESULT
*       Invokes the saved original RefreshGadgets vector.
*
*   NOTES
*       Issues job 18 when active; the refresh hooks are registered by
*       patch_functions. This symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_RefreshGadgets_SetFunc:\n"
	"movem.l a0/a1/a3,-(SP)\n"  
	"move.l _old_RefreshGadgets, a3\n"
	"jsr (a3)\n"
	"movem.l (SP)+,a0/a1/a3\n"  

	"cmp.l #1,_state\n"
	"blt refreshgadgets_patch_disabled\n"
	PUSHSTACK
	"move.l a1,a3\n" /* supply window parameter in a3 */
	"moveq #11,d0\n"
	"move.l #18,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	POPSTACK
	"refreshgadgets_patch_disabled:\n"

        "rts\n");

/*********************************************************************************
 * _my_SetWindowTitles_SetFunc
 *
 * done in assembler, as C-Source always at least destroys the
 * a5 register for the local variable stack frame. But library
 * functions may only trash d0, d1, a0 and a1.
 *
 * We call the original function, then we need to react on the
 * changes.
 *
 * for calltrap:
 * AD_GET_JOB  11 (d0)
 * AD_GET_JOB_SET_WINDOW_TITLES 19 (d1)
 * window is already in a0, we don't need the rest
 *********************************************************************************/

/****i* januae/my_SetWindowTitles_SetFunc **********************************
*
*   NAME
*       my_SetWindowTitles_SetFunc() -- Observe window title changes.
*
*   SYNOPSIS
*       Assembler entry: _my_SetWindowTitles_SetFunc (AmigaOS Intuition
*       vector ABI).
*
*   FUNCTION
*       Report relevant guest title changes after setting titles.
*
*   INPUTS
*       The original SetWindowTitles vector arguments.
*
*   RESULT
*       Calls the original SetWindowTitles vector.
*
*   NOTES
*       When active, compares the requested title pointers before the
*       original call and dispatches job 19 if the helper indicates a
*       change. This symbol is defined only for the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_SetWindowTitles_SetFunc:\n"

	"cmp.l #1,_state\n"
	"blt setwindowtitles_patch_disabled_test\n"

    "movem.l d1-d7/a0-a6,-(SP)\n"    /* backup everything but D0  */
	  "jsr _set_aros_titles\n"         /* return in D0, if there is something to change */
	  "movem.l (SP)+,d1-d7/a0-a6\n"    /* restore everything but D0 */
	  "move.l d0,-(SP)\n"              /* backup D0  */

	"setwindowtitles_patch_disabled_test:\n"

	/* call original function */
	PUSHA0
	PUSHA3
	"move.l _old_SetWindowTitles, a3\n"
	"jsr (a3)\n"
	POPA3
	POPA0

	"cmp.l #1,_state\n"
	"blt setwindowtitles_patch_disabled\n"

	  "move.l (SP)+, d0\n"    /* restore D0  */
	  "cmp.l #0,d0\n"
	  "beq setwindowtitles_patch_disabled\n" /* nothing to do */
	    PUSHSTACK
	    "moveq #11,d0\n"
	    "moveq #19,d1\n"
	    "move.l _calltrap,a1\n"
	    "jsr (a1)\n"
	    POPSTACK
	"setwindowtitles_patch_disabled:\n"

        "rts\n");


/*********************************************************************************
 * _my_WindowLimits_SetFunc
 *
 * done in assembler, as C-Source always at least destroys the
 * a5 register for the local variable stack frame. But library
 * functions may only trash d0, d1, a0 and a1.
 *
 * We call the original function, then we need to react on the
 * changes.
 *
 * for calltrap:
 * AD_GET_JOB  11 (d0)
 * AD_GET_JOB_WINDOW_LIMITS 20 (d1)
 * window is already in a0, so we move d0,d1,d2,d3 to d2,d3,d4,d5, 
 * as d0/d1 are trashed by our service numbers
 * we store the return code of the original WindowLimits function in d6, to
 * return it later on
 *********************************************************************************/

/****i* januae/my_WindowLimits_SetFunc *************************************
*
*   NAME
*       my_WindowLimits_SetFunc() -- Observe window limit changes.
*
*   SYNOPSIS
*       Assembler entry: _my_WindowLimits_SetFunc (AmigaOS Intuition vector
*       ABI).
*
*   FUNCTION
*       Report window limit changes while preserving the original result.
*
*   INPUTS
*       Window in A0; limits in D0 through D3.
*
*   RESULT
*       Restores the original WindowLimits result in D0.
*
*   NOTES
*       When active, passes the limits to job 20 in D2 through D5 without
*       replacing the original return value. This symbol is defined only for
*       the AmigaOS guest.
*
****************************************************************************
*
*/

__asm__("_my_WindowLimits_SetFunc:\n"
	/* we trash d6 to store the amigaOS result, so back it up */
	"move.l d6,-(SP)\n"
	/* amigaOS WindowLimits may trash d0-d3/a0, but we need it later */
	"movem.l d0-d3/a0,-(SP)\n"
	PUSHA3
	"move.l _old_WindowLimits, a3\n"
	"jsr (a3)\n"
	POPA3
	/* store amigaOS WindowLimits result in d6 */
	"move.l d0,d6\n"
	/* restore original WindowLimits parameter */
	"movem.l (SP)+,d0-d3/a0\n"
	
	"cmp.l #1,_state\n"
	"blt windowlimits_patch_disabled\n"
	/* backup everything */
	PUSHFULLSTACK
	/* shift registers, as we use d0/d1 for job identifier */
	"move.l d3,d5\n"
	"move.l d2,d4\n"
	"move.l d1,d3\n"
	"move.l d0,d2\n"
	"moveq #11,d0\n"
	"moveq #20,d1\n"
	"move.l _calltrap,a1\n"
	"jsr (a1)\n"
	/* restore  everything, don't care for calltrap result */
	POPFULLSTACK
	"windowlimits_patch_disabled:\n"
	/* restore amigaOS WindowLimits result from d6 */
	"move.l d6,d0\n"
	"move.l (SP)+,d6\n"
        "rts\n");
#endif /* AROS */

/*
 * assembler functions need to be declared or used, before
 * you can reference them (?).
 */
void my_CloseWindow_SetFunc();
void my_OpenWindow_SetFunc();
void my_OpenWindowTagList_SetFunc();
void my_OpenScreen_SetFunc();
void my_OpenScreenTagList_SetFunc();
void my_CloseScreen_SetFunc();
void my_ScreenDepth_SetFunc();
void my_ModifyIDCMP_SetFunc();
void my_AddGadget_SetFunc();
void my_AddGList_SetFunc();
void my_RemoveGadget_SetFunc();
void my_RemoveGList_SetFunc();
void my_SetWindowTitles_SetFunc();
void my_WindowLimits_SetFunc();
void my_RefreshGadgets_SetFunc();
void my_RefreshGList_SetFunc();

/* According to Ralph Babel: ".. as
 * of 2.0, SetFunction() calls Forbid()/Permit() 
 * automatically  and also takes care of the code 
 * cache itself."
 */
/****i* januae/patch_functions *********************************************
*
*   NAME
*       patch_functions() -- Install legacy Intuition hooks.
*
*   SYNOPSIS
*       void patch_functions(void);
*
*   FUNCTION
*       Install the AmigaOS Intuition vector hooks used by guest
*       integration.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       AmigaOS guest build only. The implementation installs window/screen,
*       title, limit and refresh hooks. ModifyIDCMP and gadget
*       insertion/removal registrations are disabled.
*
*   BUGS
*       Hook installation return values are not validated and partial
*       installation has no recovery path.
*
****************************************************************************
*
*/

void patch_functions() {

  ENTER

#ifndef __AROS__
  old_CloseWindow=SetFunction((struct Library *)IntuitionBase, 
                              -72, 
			      (APTR) my_CloseWindow_SetFunc);

  /* AROS version uses SNOTIFY_BEFORE_OPENWINDOW */
  old_OpenWindow=SetFunction((struct Library *)IntuitionBase, 
                              -204, 
			      (APTR) my_OpenWindow_SetFunc);

  old_OpenWindowTagList=SetFunction((struct Library *)IntuitionBase, 
                              -606, 
			      (APTR) my_OpenWindowTagList_SetFunc);

  old_CloseScreen=SetFunction((struct Library *)IntuitionBase, 
                              -66, 
			      (APTR) my_CloseScreen_SetFunc);

  old_OpenScreen=SetFunction((struct Library *)IntuitionBase, 
                              -198, 
			      (APTR) my_OpenScreen_SetFunc);

  old_OpenScreenTagList=SetFunction((struct Library *)IntuitionBase, 
                              -612, 
			      (APTR) my_OpenScreenTagList_SetFunc);

  old_ScreenDepth=SetFunction((struct Library *)IntuitionBase, 
                              -786, 
			      (APTR) my_ScreenDepth_SetFunc);
#endif



#ifndef __AROS__
  old_SetWindowTitles=SetFunction((struct Library *)IntuitionBase, 
                              -276, 
			      (APTR) my_SetWindowTitles_SetFunc);
#endif

#ifndef __AROS__
  old_WindowLimits=SetFunction((struct Library *)IntuitionBase, 
                              -318, 
			      (APTR) my_WindowLimits_SetFunc);

  old_RefreshGadgets=SetFunction((struct Library *)IntuitionBase, 
                              -222, 
			      (APTR) my_RefreshGadgets_SetFunc);

  old_RefreshGList=SetFunction((struct Library *)IntuitionBase, 
                              -432, 
			      (APTR) my_RefreshGList_SetFunc);
#else
#warning TODO!!!!!!!!!!!!!!!!!!!!!!1
#endif



  LEAVE
}

/* TODO: unsafe! */
/****i* januae/unpatch_functions *******************************************
*
*   NAME
*       unpatch_functions() -- Restore the CloseWindow hook.
*
*   SYNOPSIS
*       void unpatch_functions(void);
*
*   FUNCTION
*       Restore the saved CloseWindow vector.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       AmigaOS guest build only. The routine does not restore the other
*       vectors installed by patch_functions.
*
*   BUGS
*       Teardown is explicitly marked unsafe in the source; this is not a
*       complete or safe hook removal operation.
*
****************************************************************************
*
*/

void unpatch_functions() {

  ENTER

  SetFunction((struct Library *)IntuitionBase,
                              -72,
			      old_CloseWindow);

  LEAVE
}

#endif
