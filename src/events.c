 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Events
  * These are best for low-frequency events. Having too many of them,
  * or using them for events that occur too frequently, can cause massive
  * slowdown.
  *
  * Copyright 1995-1998 Bernd Schmidt
  * Copyright 2004-2005 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "events.h"
#include "custom.h"
#include "cia.h"
#include "blitter.h"
#include "disk.h"
#include "audio.h"
#include "hrtimer.h"

/* Current time in cycles */
unsigned long currcycle;

/* Cycles to next event pending */
unsigned long nextevent;

#ifdef JIT
/* For faster cycles handling */
signed long pissoff = 0;
#endif

struct ev eventtab[ev_max];


/****i* januae-events/init_eventtab ****************************************
*
*   NAME
*       init_eventtab() -- Initialize the emulator cycle-event scheduler.
*
*   SYNOPSIS
*       void init_eventtab (void);
*
*   FUNCTION
*       Reset event timing, register the CIA, horizontal sync, copper,
*       blitter, disk and audio handlers, enable the first horizontal-sync
*       event and schedule the next deadline.
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

void init_eventtab (void)
{
    int i;

    nextevent = 0;
    set_cycles (0);

    for (i = 0; i < ev_max; i++) {
	eventtab[i].active = 0;
	eventtab[i].oldcycles = 0;
    }

    eventtab[ev_cia].handler     = CIA_handler;
    eventtab[ev_hsync].handler   = hsync_handler;
    eventtab[ev_hsync].evtime    = get_cycles () + HSYNCTIME;
    eventtab[ev_hsync].active    = 1;
    eventtab[ev_copper].handler  = copper_handler;
    eventtab[ev_copper].active   = 0;
    eventtab[ev_blitter].handler = blitter_handler;
    eventtab[ev_blitter].active  = 0;
    eventtab[ev_disk].handler    = DISK_handler;
    eventtab[ev_disk].active     = 0;
    eventtab[ev_audio].handler   = audio_evhandler;
    eventtab[ev_audio].active    = 0;

    events_schedule ();
}

/*
 * Determine next event pending
 */
/****i* januae-events/events_schedule **************************************
*
*   NAME
*       events_schedule() -- Select the next scheduled emulator event.
*
*   SYNOPSIS
*       void events_schedule (void);
*
*   FUNCTION
*       Examine active event entries and set the global next-event cycle to
 *       the earliest pending deadline. If no event is active, the minimum
*       remains the unsigned all-ones sentinel and nextevent wraps to the
*       preceding cycle in the unsigned cycle counter.
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

void events_schedule (void)
{
    int i;

    unsigned long int mintime = ~0L;
    for (i = 0; i < ev_max; i++) {
	if (eventtab[i].active) {
	    unsigned long int eventtime = eventtab[i].evtime - currcycle;
	    if (eventtime < mintime)
		mintime = eventtime;
	}
    }
    nextevent = currcycle + mintime;
}

/*
 * Handle all events due at current time
 */
/****i* januae-events/handle_active_events *********************************
*
*   NAME
*       handle_active_events() -- Dispatch events due at the current
*       emulator cycle.
*
*   SYNOPSIS
*       void handle_active_events (void);
*
*   FUNCTION
*       Invoke the handler of every active event whose scheduled cycle
*       equals the current cycle.
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

void handle_active_events (void)
{
    int i;
    for (i = 0; i < ev_max; i++) {
	if (eventtab[i].active && eventtab[i].evtime == currcycle) {
	    (*eventtab[i].handler)();
	}
    }
}
