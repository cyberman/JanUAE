/************************************************************************ 
 *
 * Janus-Daemon
 *
 * Copyright 2009 Oliver Brunner - aros<at>oliver-brunner.de
 *
 * This file is part of Janus-Daemon.
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

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>

#include <proto/exec.h>

#include "janus-daemon.h"

extern struct IntuitionBase* IntuitionBase;

/****************************************************
 * BOOL assert_window_on_screen(screen, window)
 *
 * check, if a window still exists on a screen.
 *
 * ATT: you have to call LockIBase yourself!!
 ****************************************************/
/****i* januae/assert_window_on_screen *************************************
*
*   NAME
*       assert_window_on_screen() -- Find a window on one screen.
*
*   SYNOPSIS
*       static BOOL assert_window_on_screen(struct Screen *screen, struct
*       Window *window);
*
*   FUNCTION
*       Determine whether a window is present in one screen's window list.
*
*   INPUTS
*       screen - Screen whose FirstWindow chain is inspected. window -
*       Window pointer to match.
*
*   RESULT
*       TRUE if the pointer appears in the list; otherwise FALSE.
*
*   NOTES
*       The caller must already hold the appropriate Intuition base lock.
*       The routine does not acquire a lock.
*
****************************************************************************
*
*/

static BOOL assert_window_on_screen(struct Screen *screen, struct Window *window) {

  struct Window *i;

  ENTER

  i=screen->FirstWindow;

  while(i) {
    if(i==window) {
      LEAVE
      return TRUE;
    }
    i=i->NextWindow;
  }

  LEAVE

  return FALSE;
}

/****************************************************
 * BOOL assert_window(window)
 *
 * check, if the window still exists on any screen.
 *
 * ATT: you have to call LockIBase yourself!!
 ****************************************************/
/****i* januae/assert_window ***********************************************
*
*   NAME
*       assert_window() -- Find a window on any screen.
*
*   SYNOPSIS
*       BOOL assert_window(struct Window *window);
*
*   FUNCTION
*       Check whether a window pointer occurs on any currently linked guest
*       screen.
*
*   INPUTS
*       window - Window pointer to search for.
*
*   RESULT
*       TRUE if a matching window is found; otherwise FALSE.
*
*   NOTES
*       The caller must hold the Intuition base lock; this function does not
*       acquire it.
*
*   BUGS
*       The result is not a safe lifetime guarantee once the caller releases
*       its lock.
*
****************************************************************************
*
*/

BOOL assert_window(struct Window *window) {

  struct Screen *i;

  ENTER

  i=IntuitionBase->FirstScreen;

  while(i) {
    if(assert_window_on_screen(i, window)) {
      LEAVE
      return TRUE;
    }
    i=i->NextScreen;
  }

  LEAVE
  return FALSE;
}

/****************************************************
 * BOOL window_exists(window)
 *
 * - tests, if window exists
 * - can't work 100%, sorry
 * - we are not allowed to call any high level system
 *   functions during LockIBase!
 *
 * OBSOLETE: use assert_window instead!
 ****************************************************/
#if 0
BOOL window_exists(struct Window *window) {

  ULONG lock;
  BOOL  result;
  ULONG pri;

  ENTER
#if 0

  DebOut("test window %lx .. \n", window);

  pri=SetTaskPri(FindTask(0), 0);
  DebOut("try to LockIBase ..\n");
  lock=LockIBase(0);
  DebOut("LockIBase(0) returned: %lx\n", lock);

  result=assert_window(window);

  UnlockIBase(lock);
  SetTaskPri(FindTask(0), pri);

  DebOut("window %lx exists: %d\n", window, result);

#endif
  result=TRUE;
  LEAVE

  return result;
}
#endif

