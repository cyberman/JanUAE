/************************************************************************ 
 *
 * Copyright 2009-2010 Oliver Brunner - aros<at>oliver-brunner.de
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

/****i* januae/j_shutdown_all **********************************************
*
*   NAME
*       j_shutdown_all() -- Clear guest integration state.
*
*   SYNOPSIS
*       static void j_shutdown_all(void);
*
*   FUNCTION
*       Close cloned guest windows and screens, uninstall the clipboard
*       hook, clear registered guest daemon tasks and signals, and request
*       CLI proxy termination.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The Workbench launch proxy is left running.
*
****************************************************************************
*
*/

static void j_shutdown_all(void) {

  JWLOG("unregister all daemons\n");

  /* reset janusd */
  JWLOG("close_all_janus_windows ..\n");
  close_all_janus_windows();
  JWLOG("close_all_janus_screens ..\n");
  close_all_janus_screens();
  aos3_task=0;
  aos3_task_signal=0;

  /* reset clipd */
  JWLOG("clipboard_hook_deinstall ..\n");
  clipboard_hook_deinstall();
  aos3_clip_task=0;
  aos3_clip_signal=0;
  aos3_clip_to_amigaos_signal=0;

  /* reset launchd */
  aos3_launch_task=0;
  aos3_launch_signal=0;
  JWLOG("aros_cli_kill_thread ..\n");
  aros_cli_kill_thread();

  JWLOG("exit\n");

}

/*********************************************************
 * j_reset
 *
 * AmigaOS was resetted, either throuh a crash or with the
 * Reset/Stop buttons.
 *
 * So we need to shutdown everything.
 *
 * The launch thread may continue to run, it is not
 * dependend on any AmigaOS daemon.
 *
 * The cli thread must die, it is only running, if
 * launchd is running.
 ********************************************************/
/****i* januae/j_reset *****************************************************
*
*   NAME
*       j_reset() -- Handle a guest reset.
*
*   SYNOPSIS
*       void j_reset(void);
*
*   FUNCTION
*       Shut down guest integration state and unlock the JanUAE graphical
*       interface after a guest reset.
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

void j_reset(void) {

  JWLOG("j_reset\n");

  j_shutdown_all();

  /* update gui */
  unlock_jgui();

  JWLOG("j_reset => done\n");
}

/****i* januae/j_quit ******************************************************
*
*   NAME
*       j_quit() -- End guest integration.
*
*   SYNOPSIS
*       void j_quit(void);
*
*   FUNCTION
*       Release the registered guest integration state through
*       j_shutdown_all.
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

void j_quit(void) {

  JWLOG("j_quit\n");

  j_shutdown_all();

  JWLOG("j_quit => done\n");

}
