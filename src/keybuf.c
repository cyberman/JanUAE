 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Keyboard buffer. Not really needed for X, but for SVGAlib and possibly
  * Mac and DOS ports.
  *
  * Note: it's possible to have two threads in UAE, one reading keystrokes
  * and the other one writing them. Despite this, no synchronization effort
  * is needed. This code should be perfectly thread safe. At least if you
  * assume that integer store instructions are atomic.
  *
  * Copyright 1995, 1997 Bernd Schmidt
  */

#include "sysconfig.h"
#include "sysdeps.h"
#include <assert.h>

#include "options.h"
#include "keybuf.h"
#include "keyboard.h"
#include "inputdevice.h"
#include "custom.h"
#include "savestate.h"

static int fakestate[2][7] = { {0},{0} };

static int *fs_np;
static int *fs_np;
static int *fs_ck;
static int *fs_se;
#ifdef XARCADE
static int *fs_xa1;
static int *fs_xa2;
#endif

/* Not static so the DOS code can mess with them */
int kpb_first, kpb_last;

static int keybuf[256];

/****i* januae-keybuf/keys_available ***************************************
*
*   NAME
*       keys_available() -- Check whether the emulator keyboard queue
*       contains data.
*
*   SYNOPSIS
*       int keys_available (void);
*
*   FUNCTION
*       Compare the producer and consumer indices of the fixed-size keyboard
*       ring buffer.
*
*   INPUTS
*       None.
*
*   RESULT
*       Nonzero when a queued key code is available; zero otherwise.
*
****************************************************************************
*
*/

int keys_available (void)
{
    int val;
    val = kpb_first != kpb_last;
    return val;
}

/****i* januae-keybuf/get_next_key *****************************************
*
*   NAME
*       get_next_key() -- Remove the next key code from the keyboard ring
*       buffer.
*
*   SYNOPSIS
*       int get_next_key (void);
*
*   FUNCTION
*       Read the entry at the consumer index and advance that index,
*       wrapping after the last of 256 positions.
*
*   INPUTS
*       None.
*
*   RESULT
*       The oldest queued key code.
*
*   NOTES
*       The caller must ensure the queue is nonempty; an assertion checks
*       this precondition.
*
****************************************************************************
*
*/

int get_next_key (void)
{
    int key;
    assert (kpb_first != kpb_last);

    key = keybuf[kpb_last];
    if (++kpb_last == 256)
	kpb_last = 0;
    return key;
}

/****i* januae-keybuf/do_fake **********************************************
*
*   NAME
*       do_fake() -- Publish keyboard-derived joystick state for one
*       emulated port.
*
*   SYNOPSIS
*       static void do_fake (int nr);
*
*   FUNCTION
*       Resolve the configured compatibility device and map the stored
*       directional and three button states to joystick axis and button
*       events.
*
*   INPUTS
*       nr - Keyboard joystick state slot, zero or one.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The axis values use a magnitude of 100; opposing direction keys are
*       resolved by the ordering in the source expression.
*
****************************************************************************
*
*/

static void do_fake (int nr)
{
    int *fake = fakestate[nr];

    nr = compatibility_device[nr];
    setjoystickstate (nr, 0, fake[1] ? -100 : (fake[2] ? 100 : 0), 100);
    setjoystickstate (nr, 1, fake[0] ? -100 : (fake[3] ? 100 : 0), 100);
    setjoybuttonstate (nr, 0, fake[4]);
    setjoybuttonstate (nr, 1, fake[5]);
    setjoybuttonstate (nr, 2, fake[6]);
}

/****i* januae-keybuf/record_key *******************************************
*
*   NAME
*       record_key() -- Record an emulated keyboard transition or route it
*       to a keyboard joystick.
*
*   SYNOPSIS
*       void record_key (int kc);
*
*   FUNCTION
*       Reject an event when the ring buffer is full, update the selected
*       keyboard-joystick key states, and emit joystick events in
*       compatibility mode when applicable. Otherwise normalize the
*       right-Control key and optionally apply X-Arcade remapping before
*       enqueuing the key code.
*
*   INPUTS
*       kc - Encoded Amiga key code, with the low bit representing the
*       release state.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The keyboard queue uses 256 slots with one entry reserved to
*       distinguish full from empty. X-Arcade handling is conditional on
*       XARCADE.
*
****************************************************************************
*
*/

void record_key (int kc)
{
    int fs = 0;
    int kpb_next = kpb_first + 1;
    int k = kc >> 1;
    int b = !(kc & 1);

    //write_log ("got kc %02.2X\n", ((kc << 7) | (kc >> 1)) & 0xff);
    if (kpb_next == 256)
	kpb_next = 0;
    if (kpb_next == kpb_last) {
	write_log ("Keyboard buffer overrun. Congratulations.\n");
	return;
    }

    if (fs_np != 0) {
	switch (kc >> 1) {
	case AK_NP8: fs = 1; fs_np[0] = !(kc & 1); break;
	case AK_NP4: fs = 1; fs_np[1] = !(kc & 1); break;
	case AK_NP6: fs = 1; fs_np[2] = !(kc & 1); break;
	case AK_NP2: fs = 1; fs_np[3] = !(kc & 1); break;
	case AK_NP0: case AK_NP5: fs = 1; fs_np[4] = !(kc & 1); break;
	case AK_NPDEL: case AK_NPDIV: case AK_ENT: fs = 1; fs_np[5] = !(kc & 1); break;
	}
    }
    if (fs_ck != 0) {
	switch (kc >> 1) {
	case AK_UP: fs = 1; fs_ck[0] = !(kc & 1); break;
	case AK_LF: fs = 1; fs_ck[1] = !(kc & 1); break;
	case AK_RT: fs = 1; fs_ck[2] = !(kc & 1); break;
	case AK_DN: fs = 1; fs_ck[3] = !(kc & 1); break;
	case AK_RCTRL: case AK_RALT: fs = 1; fs_ck[4] = !(kc & 1); break;
	case AK_RSH: fs = 1; fs_ck[5] = !(kc & 1); break;
	}
    }
    if (fs_se != 0) {
	switch (k) {
	case AK_T: fs = 1; fs_se[0] = b; break;
	case AK_F: fs = 1; fs_se[1] = b; break;
	case AK_H: fs = 1; fs_se[2] = b; break;
	case AK_B: fs = 1; fs_se[3] = b; break;
	case AK_LALT: fs = 1; fs_se[4] = b; break;
	case AK_LSH: fs = 1; fs_se[5] = b; break;
	}
    }
#ifdef XARCADE
    if (fs_xa1 != 0) {
	switch (k) {
	case AK_NP8: fs = 1; fs_xa1[0] = b; break;
	case AK_NP4: fs = 1; fs_xa1[1] = b; break;
	case AK_NP6: fs = 1; fs_xa1[2] = b; break;
	case AK_NP2: case AK_NP5: fs = 1; fs_xa1[3] = b; break;
	case AK_CTRL: fs = 1; fs_xa1[4] = b; break;
	case AK_LALT: fs = 1; fs_xa1[5] = b; break;
	case AK_SPC: fs = 1; fs_xa1[6] = b; break;
	}
    }
    if (fs_xa2 != 0) {
	switch (k) {
	case AK_R: fs = 1; fs_xa2[0] = b; break;
	case AK_D: fs = 1; fs_xa2[1] = b; break;
	case AK_G: fs = 1; fs_xa2[2] = b; break;
	case AK_F: fs = 1; fs_xa2[3] = b; break;
	case AK_A: fs = 1; fs_xa2[4] = b; break;
	case AK_S: fs = 1; fs_xa2[5] = b; break;
	case AK_Q: fs = 1; fs_xa2[6] = b; break;
	}
    }
#endif
    if (fs && currprefs.input_selected_setting == 0) {
	if (JSEM_ISANYKBD (0, &currprefs))
	    do_fake (0);
	if (JSEM_ISANYKBD (1, &currprefs))
	    do_fake (1);
	return;
    } else {
	if ((kc >> 1) == AK_RCTRL) {
	    kc ^= AK_RCTRL << 1;
	    kc ^= AK_CTRL << 1;
	}
#ifdef XARCADE
	if (fs_xa1 || fs_xa2) {
	    int k2 = k;
	    if (k == AK_1)
		k2 = AK_F1;
	    if (k == AK_2)
		k2 = AK_F2;
	    if (k == AK_3)
		k2 = AK_LALT;
	    if (k == AK_4)
		k2 = AK_RALT;
	    if (k == AK_6)
		k2 = AK_DN;
	    if (k == AK_LBRACKET || k == AK_LSH)
		k2 = AK_SPC;
	    if (k == AK_RBRACKET)
		k2 = AK_RET;
	    if (k == AK_C)
		k2 = AK_1;
	    if (k == AK_5)
		k2 = AK_2;
	    if (k == AK_Z)
		k2 = AK_3;
	    if (k == AK_X)
		k2 = AK_4;
	    if (k != k2)
		kc = (k2 << 1) | (b ? 0 : 1);
	}
#endif
    }

    keybuf[kpb_first] = kc;
    kpb_first = kpb_next;
}

/****i* januae-keybuf/joystick_setting_changed *****************************
*
*   NAME
*       joystick_setting_changed() -- Rebind keyboard emulation to the
*       selected joystick configurations.
*
*   SYNOPSIS
*       void joystick_setting_changed (void);
*
*   FUNCTION
*       Clear the key-state pointers, then associate numpad, cursor-key, and
*       alternate-key layouts with either configured compatibility port.
*       Include the optional X-Arcade layouts when compiled in.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The function changes the active key-state destinations but does not
*       erase their previously stored values.
*
****************************************************************************
*
*/

void joystick_setting_changed (void)
{
    fs_np = fs_ck = fs_se = 0;
#ifdef XARCADE
    fs_xa1 = fs_xa2 = 0;
#endif

    if (JSEM_ISNUMPAD (0, &currprefs))
	fs_np = fakestate[0];
    else if (JSEM_ISNUMPAD (1, &currprefs))
	fs_np = fakestate[1];

    if (JSEM_ISCURSOR (0, &currprefs))
	fs_ck = fakestate[0];
    else if (JSEM_ISCURSOR (1, &currprefs))
	fs_ck = fakestate[1];

    if (JSEM_ISSOMEWHEREELSE (0, &currprefs))
	fs_se = fakestate[0];
    else if (JSEM_ISSOMEWHEREELSE (1, &currprefs))
	fs_se = fakestate[1];

#ifdef XARCADE
    if (JSEM_ISXARCADE1 (0, &currprefs))
	fs_xa1 = fakestate[0];
    else if (JSEM_ISXARCADE1 (1, &currprefs))
	fs_xa1 = fakestate[1];

    if (JSEM_ISXARCADE2 (0, &currprefs))
	fs_xa2 = fakestate[0];
    else if (JSEM_ISXARCADE2 (1, &currprefs))
	fs_xa2 = fakestate[1];
#endif
}

/****i* januae-keybuf/keybuf_init ******************************************
*
*   NAME
*       keybuf_init() -- Reset the keyboard queue and activate the current
*       input configuration.
*
*   SYNOPSIS
*       void keybuf_init (void);
*
*   FUNCTION
*       Set both queue indices to zero and ask the input-device subsystem to
*       update its configuration from the current preferences.
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

void keybuf_init (void)
{
    kpb_first = kpb_last = 0;
    inputdevice_updateconfig (&currprefs);
}


#ifdef SAVESTATE

/****i* januae-keybuf/save_keyboard ****************************************
*
*   NAME
*       save_keyboard() -- Serialize the keyboard state for an emulator save
*       state.
*
*   SYNOPSIS
*       uae_u8 *save_keyboard (uae_u32 *len);
*
*   FUNCTION
*       Allocate an eight-byte record containing the caps-lock state as a
*       32-bit value followed by a reserved zero word.
*
*   INPUTS
*       len - Output location for the serialized record length.
*
*   RESULT
*       Newly allocated record data; the function sets *len to eight.
*
*   NOTES
*       Available only when SAVESTATE is enabled. The caller owns the
*       allocated record.
*
****************************************************************************
*
*/

uae_u8 *save_keyboard (uae_u32 *len)
{
    uae_u8 *dst, *t;
    dst = t = malloc (8);
    save_u32 (getcapslockstate() ? 1 : 0);
    save_u32 (0);
    *len = 8;
    return t;
}

/****i* januae-keybuf/restore_keyboard *************************************
*
*   NAME
*       restore_keyboard() -- Restore the recorded keyboard caps-lock state.
*
*   SYNOPSIS
*       const uae_u8 *restore_keyboard (const uae_u8 *src);
*
*   FUNCTION
*       Read the serialized caps-lock word, update the host keyboard state,
*       and consume the following reserved word.
*
*   INPUTS
*       src - Serialized keyboard-state record.
*
*   RESULT
*       Pointer following the consumed record.
*
*   NOTES
*       Available only when SAVESTATE is enabled.
*
****************************************************************************
*
*/

const uae_u8 *restore_keyboard (const uae_u8 *src)
{
    setcapslockstate (restore_u32 ());
    restore_u32 ();
    return src;
}

#endif /* SAVESTATE */
