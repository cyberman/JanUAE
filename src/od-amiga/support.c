 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Misc support code for AmigaOS target
  *
  * Copyright 2003-2005 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include <proto/timer.h>

#include "sleep.h"
#include "osdep/hrtimer.h"

/****i* januae-support/truncate ********************************************
*
*   NAME
*       truncate() -- Handle a host file-truncation request.
*
*   SYNOPSIS
*       int truncate (const char *path, off_t length);
*
*   FUNCTION
*       Return a fixed success indication without changing the named file.
*
*   INPUTS
*       path - Target filename; unused. length - Requested file length;
*       unused.
*
*   RESULT
*       0 for every call.
*
****************************************************************************
*
*/

int truncate (const char *path, off_t length)
{
   /* FIXME */
   return 0;
}


frame_time_t timebase;

#ifndef __AROS__
/****i* januae-support/osdep_inithrtimer ***********************************
*
*   NAME
*       osdep_inithrtimer() -- Initialize the native high-resolution
*       timebase.
*
*   SYNOPSIS
*       int osdep_inithrtimer (void);
*
*   FUNCTION
*       Read the Exec EClock frequency into the shared timebase once and
*       retain it for later queries.
*
*   INPUTS
*       None.
*
*   RESULT
*       1 on every call.
*
*   NOTES
*       Compiled only when __AROS__ is not defined.
*
****************************************************************************
*
*/

int osdep_inithrtimer (void)
{
    static int done = 0;

    if (!done) {
	struct EClockVal etime;

	timebase  = ReadEClock (&etime);
	write_log ("EClock frequency:%.6f MHz\n", timebase/1e6);

	done = 1;
    }

    return 1;
}

/****i* januae-support/osdep_gethrtimebase *********************************
*
*   NAME
*       osdep_gethrtimebase() -- Retrieve the native EClock timebase.
*
*   SYNOPSIS
*       frame_time_t osdep_gethrtimebase (void);
*
*   FUNCTION
*       Return the shared frequency recorded by osdep_inithrtimer().
*
*   INPUTS
*       None.
*
*   RESULT
*       Current value of timebase.
*
*   NOTES
*       Compiled only when __AROS__ is not defined.
*
****************************************************************************
*
*/

frame_time_t osdep_gethrtimebase (void)
{
    return timebase;
}
#endif
