 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Target-independent support for parsing hotkey sequences
  *
  * Copyright 2004 Richard Drummond
  *
  * Based on the hotkey handling previously found
  * in x11win.c and sdlgfx.c
  */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "inputdevice.h"
#include "hotkeys.h"


static struct uae_hotkeyseq *hotkey_table;


/****i* januae-hotkeys/set_default_hotkeys *********************************
*
*   NAME
*       set_default_hotkeys() -- Install a table of emulator hotkey
*       sequences.
*
*   SYNOPSIS
*       void set_default_hotkeys (struct uae_hotkeyseq *keys);
*
*   FUNCTION
*       Retain the supplied sequence table as the active hotkey
*       configuration and clear its recorded key states.
*
*   INPUTS
*       keys - Sequence table terminated by an entry with a zero event
*       identifier.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The table remains owned by the caller and must remain valid while
*       installed.
*
****************************************************************************
*
*/

void set_default_hotkeys (struct uae_hotkeyseq *keys)
{
    hotkey_table = keys;
    reset_hotkeys ();
}

/****i* januae-hotkeys/reset_hotkeys ***************************************
*
*   NAME
*       reset_hotkeys() -- Clear the progress of every registered hotkey
*       sequence.
*
*   SYNOPSIS
*       void reset_hotkeys (void);
*
*   FUNCTION
*       Visit each sequence until the zero-event terminator, clear all
*       recorded pressed-key entries, and reset its completion flag.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       When no table is installed, the operation has no effect.
*
****************************************************************************
*
*/

void reset_hotkeys (void) {
    struct uae_hotkeyseq *seq = hotkey_table;
    int i;

    if (!seq)
	return;

    while (seq->event) {
	for (i=0; i < HOTKEYSEQ_MAXKEYS; i++)
	    seq->key_pressed[i] = 0;
	seq->complete = 0;
	seq++;
    }
}

/****i* januae-hotkeys/match_hotkey_sequence *******************************
*
*   NAME
*       match_hotkey_sequence() -- Match a keyboard transition against
*       installed hotkey sequences.
*
*   SYNOPSIS
*       int match_hotkey_sequence (int key, int state);
*
*   FUNCTION
*       Track ordered key presses in the active sequence table. A matching
*       final key marks a sequence complete; releasing a participating key
*       clears that key and subsequent entries and reports a completed
*       sequence as released.
*
*   INPUTS
*       key - Host key code to match; state - Nonzero for key down or zero
*       for key up.
*
*   RESULT
*       The matching emulator hotkey event identifier, or zero if no event
*       is generated.
*
*   NOTES
*       Matching state is retained between calls until a release or explicit
*       reset.
*
****************************************************************************
*
*/

int match_hotkey_sequence (int key, int state)
{
    struct uae_hotkeyseq *seq = hotkey_table;
    int event = 0;

    if (!seq)
    	return 0;

    if (state) {
	/* Key down */
	while (seq->event != 0) {
	    int i;
	    /* Check whether key is the next key in this sequence */
	    if (key == seq->keyseq[0])
		seq->key_pressed[0] = 1;
	    for (i=1; i < HOTKEYSEQ_MAXKEYS; i++) {
	        if ((seq->key_pressed[i-1] == 1) && (key == seq->keyseq[i])) {
		    seq->key_pressed[i] = 1;
		    /* Was it the final key in this sequence ? */
		    if ((i+1 == HOTKEYSEQ_MAXKEYS) || (seq->keyseq[i+1] == (uae_u16)-1)) {
		        /* Yes. Return the event */
		        event = seq->event;
		        seq->complete = 1;
			break;
		    }
		}
	    }
	    seq++;
	}
    } else {
	/* key up */
	while (seq->event != 0) {
	    int i, j;
	    for (i = 0; i < HOTKEYSEQ_MAXKEYS; i++) {
	        if ((key == seq->keyseq[i]) && (seq->key_pressed[i] == 1)) {
		    /* letting up a key - cancels any keys following it
		     * in the sequence that are pressed */
		    for (j=i; j < HOTKEYSEQ_MAXKEYS; j++)
		        seq->key_pressed[j] = 0;
		    /* if it was previously complete generate return the event
		     * (so that it can be used as a key-up event */
		    if (seq->complete) {
		        seq->complete = 0;
		        event = seq->event;
		    }
		}
	    }
	    seq++;
	}
    }
    return event;
}
