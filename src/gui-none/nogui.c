 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Interface to the Tcl/Tk GUI
  *
  * Copyright 1996 Bernd Schmidt
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "gui.h"


/****i* januae-gui-none/gui_init *******************************************
*
*   NAME
*       gui_init()
*
*   SYNOPSIS
*       void gui_init (int argc, char **argv);
*
*   FUNCTION
*       Perform no GUI initialization.
*
*   INPUTS
*       argc - Unused argument count.
*       argv - Unused argument vector.
*
****************************************************************************
*
*/
void gui_init (int argc, char **argv)
{
}

/****i* januae-gui-none/gui_open *******************************************
*
*   NAME
*       gui_open()
*
*   SYNOPSIS
*       int gui_open (void);
*
*   FUNCTION
*       Report that this backend has no GUI window to open.
*
*   RESULT
*       Always -1.
*
****************************************************************************
*
*/
int gui_open (void)
{
    return -1;
}

/****i* januae-gui-none/gui_notify_state ***********************************
*
*   NAME
*       gui_notify_state()
*
*   SYNOPSIS
*       void gui_notify_state (int state);
*
*   FUNCTION
*       Ignore notifications of emulator state changes.
*
*   INPUTS
*       state - Unused state value.
*
****************************************************************************
*
*/
void gui_notify_state (int state)
{
}

/****i* januae-gui-none/gui_update *****************************************
*
*   NAME
*       gui_update()
*
*   SYNOPSIS
*       int gui_update (void);
*
*   FUNCTION
*       Perform no GUI refresh.
*
*   RESULT
*       Always 0.
*
****************************************************************************
*
*/
int gui_update (void)
{
    return 0;
}

/****i* januae-gui-none/gui_exit *******************************************
*
*   NAME
*       gui_exit()
*
*   SYNOPSIS
*       void gui_exit (void);
*
*   FUNCTION
*       Perform no GUI shutdown.
*
****************************************************************************
*
*/
void gui_exit (void)
{
}

/****i* januae-gui-none/gui_fps ********************************************
*
*   NAME
*       gui_fps()
*
*   SYNOPSIS
*       void gui_fps (int fps, int idle);
*
*   FUNCTION
*       Record frame rate and idle measurements in the shared GUI status.
*
*   INPUTS
*       fps - Current frame rate.
*       idle - Current idle measurement.
*
****************************************************************************
*
*/
void gui_fps (int fps, int idle)
{
    gui_data.fps  = fps;
    gui_data.idle = idle;
}

/****i* januae-gui-none/gui_led ********************************************
*
*   NAME
*       gui_led()
*
*   SYNOPSIS
*       void gui_led (int led, int on);
*
*   FUNCTION
*       Perform no visible LED update.
*
*   INPUTS
*       led - Unused LED index.
*       on - Unused LED state.
*
****************************************************************************
*
*/
void gui_led (int led, int on)
{
}

/****i* januae-gui-none/gui_hd_led *****************************************
*
*   NAME
*       gui_hd_led()
*
*   SYNOPSIS
*       void gui_hd_led (int led);
*
*   FUNCTION
*       Record hard-disk activity in gui_data.hd and keep the indication
*       active for six subsequent calls with a zero value.
*
*   INPUTS
*       led - Nonzero for activity; zero to count down the indication.
*
*   NOTES
*       An actual LED display is not provided by this backend.
*
****************************************************************************
*
*/
void gui_hd_led (int led)
{
    static int resetcounter;

    int old = gui_data.hd;

    if (led == 0) {
	resetcounter--;
	if (resetcounter > 0)
	    return;
    }

    gui_data.hd = led;
    resetcounter = 6;
    if (old != gui_data.hd)
	gui_led (5, gui_data.hd);
}

/****i* januae-gui-none/gui_cd_led *****************************************
*
*   NAME
*       gui_cd_led()
*
*   SYNOPSIS
*       void gui_cd_led (int led);
*
*   FUNCTION
*       Record CD activity in gui_data.cd and retain the indication for up
*       to six subsequent calls with a zero value.
*
*   INPUTS
*       led - Nonzero for activity; zero to count down the indication.
*
*   NOTES
*       An actual LED display is not provided by this backend.
*
****************************************************************************
*
*/
void gui_cd_led (int led)
{
    static int resetcounter;

    int old = gui_data.cd;
    if (led == 0) {
	resetcounter--;
	if (resetcounter > 0)
	    return;
    }

    gui_data.cd = led;
    resetcounter = 6;
    if (old != gui_data.cd)
	gui_led (6, gui_data.cd);
}

/****i* januae-gui-none/gui_filename ***************************************
*
*   NAME
*       gui_filename()
*
*   SYNOPSIS
*       void gui_filename (int num, const char *name);
*
*   FUNCTION
*       Ignore disk filename notifications.
*
*   INPUTS
*       num - Unused disk index.
*       name - Unused filename.
*
****************************************************************************
*
*/
void gui_filename (int num, const char *name)
{
}

/****i* januae-gui-none/gui_handle_events **********************************
*
*   NAME
*       gui_handle_events()
*
*   SYNOPSIS
*       void gui_handle_events (void);
*
*   FUNCTION
*       Perform no GUI event dispatch.
*
****************************************************************************
*
*/
void gui_handle_events (void)
{
}

/****i* januae-gui-none/gui_display ****************************************
*
*   NAME
*       gui_display()
*
*   SYNOPSIS
*       void gui_display(int shortcut);
*
*   FUNCTION
*       Perform no GUI display action.
*
*   INPUTS
*       shortcut - Unused display shortcut.
*
****************************************************************************
*
*/
void gui_display(int shortcut)
{
}

/****i* januae-gui-none/gui_message ****************************************
*
*   NAME
*       gui_message()
*
*   SYNOPSIS
*       void gui_message (const char *format,...);
*
*   FUNCTION
*       Format a message and forward it to the emulator logging function.
*
*   INPUTS
*       format - printf-style format string.
*       ... - Arguments selected by the format string.
*
*   NOTES
*       Formatting uses vsprintf() into a fixed 2048-byte stack buffer
*       without a length check. The formatted message is passed to
*       write_log() as its format string.
*
****************************************************************************
*
*/
void gui_message (const char *format,...)
{   
       char msg[2048];
       va_list parms;

       va_start (parms,format);
       vsprintf ( msg, format, parms);
       va_end (parms);

       write_log (msg);
}
