/*
 * UAE - The U*nix Amiga Emulator
 *
 * UAE Library v0.1
 *
 * (c) 1996 Tauno Taipaleenmaki <tataipal@raita.oulu.fi>
 *
 * Change UAE parameters and other stuff from inside the emulation.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include <assert.h>
#include <string.h>

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "xwin.h"
#include "autoconf.h"
#include "traps.h"
#include "disk.h"
#include "debug.h"
#include "audio.h"
#include "p96.h"
#include "version.h"

/*
 * Returns UAE Version
 */
/****i* januae-uaelib/emulib_GetVersion ************************************
*
*   NAME
*       emulib_GetVersion() -- Return the emulator library version.
*
*   SYNOPSIS
*       static uae_u32 emulib_GetVersion (void);
*
*   FUNCTION
*       Read the compile-time UAE version constant exposed through the guest
*       library trap.
*
*   INPUTS
*       None.
*
*   RESULT
*       UAEVERSION.
*
****************************************************************************
*
*/

static uae_u32 emulib_GetVersion (void)
{
    return UAEVERSION;
}

/*
 * Resets your amiga
 */
/****i* januae-uaelib/emulib_HardReset *************************************
*
*   NAME
*       emulib_HardReset() -- Request a guest reset from the library
*       interface.
*
*   SYNOPSIS
*       static uae_u32 emulib_HardReset (void);
*
*   FUNCTION
*       Invoke the emulator reset routine with a zero reset argument.
*
*   INPUTS
*       None.
*
*   RESULT
*       Zero.
*
****************************************************************************
*
*/

static uae_u32 emulib_HardReset (void)
{
    uae_reset(0);
    return 0;
}

/****i* januae-uaelib/emulib_Reset *****************************************
*
*   NAME
*       emulib_Reset() -- Request the standard guest reset operation.
*
*   SYNOPSIS
*       static uae_u32 emulib_Reset (void);
*
*   FUNCTION
*       Pass a zero argument to the emulator reset routine.
*
*   INPUTS
*       None.
*
*   RESULT
*       Zero.
*
****************************************************************************
*
*/

static uae_u32 emulib_Reset (void)
{
    uae_reset(0);
    return 0;
}

/*
 * Enables SOUND
 */
/****i* januae-uaelib/emulib_EnableSound ***********************************
*
*   NAME
*       emulib_EnableSound() -- Change the requested sound-production
*       setting.
*
*   SYNOPSIS
*       static uae_u32 emulib_EnableSound (uae_u32 val);
*
*   FUNCTION
*       Assign the requested production mode only when sound is available
*       and the current production setting is not two.
*
*   INPUTS
*       val - Requested sound-production mode.
*
*   RESULT
*       One when the setting is accepted; zero otherwise.
*
****************************************************************************
*
*/

static uae_u32 emulib_EnableSound (uae_u32 val)
{
    if (!sound_available || currprefs.produce_sound == 2)
	return 0;

    currprefs.produce_sound = val;
    return 1;
}

/*
 * Enables FAKE JOYSTICK
 */
/****i* januae-uaelib/emulib_EnableJoystick ********************************
*
*   NAME
*       emulib_EnableJoystick() -- Select the two emulator joystick-port
*       assignments.
*
*   SYNOPSIS
*       static uae_u32 emulib_EnableJoystick (uae_u32 val);
*
*   FUNCTION
*       Store the low and next higher bytes of the supplied value in the
*       port-zero and port-one preferences.
*
*   INPUTS
*       val - Packed joystick-port selections.
*
*   RESULT
*       One.
*
****************************************************************************
*
*/

static uae_u32 emulib_EnableJoystick (uae_u32 val)
{
    currprefs.jport0 = val & 255;
    currprefs.jport1 = (val >> 8) & 255;
    return 1;
}

/*
 * Sets the framerate
 */
/****i* januae-uaelib/emulib_SetFrameRate **********************************
*
*   NAME
*       emulib_SetFrameRate() -- Set the frame-skipping rate used by the
*       emulator.
*
*   SYNOPSIS
*       static uae_u32 emulib_SetFrameRate (uae_u32 val);
*
*   FUNCTION
*       Accept a nonzero value no greater than twenty and assign it to the
*       current graphics frame-rate preference.
*
*   INPUTS
*       val - Requested frame-rate setting, from 1 through 20.
*
*   RESULT
*       One for an accepted setting; zero for an out-of-range value.
*
****************************************************************************
*
*/

static uae_u32 emulib_SetFrameRate (uae_u32 val)
{
    if (val == 0)
	return 0;
    else if (val > 20)
	return 0;
    else {
	currprefs.gfx_framerate = val;
	return 1;
    }
}

/*
 * Changes keyboard language settings
 */
/****i* januae-uaelib/emulib_ChangeLanguage ********************************
*
*   NAME
*       emulib_ChangeLanguage() -- Choose one of the supported keyboard
*       layouts.
*
*   SYNOPSIS
*       static uae_u32 emulib_ChangeLanguage (uae_u32 which);
*
*   FUNCTION
*       Map selection values zero through six to the US, Danish, German,
*       Swedish, French, Italian, and Spanish keyboard-language constants.
*
*   INPUTS
*       which - Keyboard-language selection.
*
*   RESULT
*       One for a supported selection; zero otherwise.
*
****************************************************************************
*
*/

static uae_u32 emulib_ChangeLanguage (uae_u32 which)
{
    if (which > 6)
	return 0;
    else {
	switch (which) {
	 case 0:
	    currprefs.keyboard_lang = KBD_LANG_US;
	    break;
	 case 1:
	    currprefs.keyboard_lang = KBD_LANG_DK;
	    break;
	 case 2:
	    currprefs.keyboard_lang = KBD_LANG_DE;
	    break;
	 case 3:
	    currprefs.keyboard_lang = KBD_LANG_SE;
	    break;
	 case 4:
	    currprefs.keyboard_lang = KBD_LANG_FR;
	    break;
	 case 5:
	    currprefs.keyboard_lang = KBD_LANG_IT;
	    break;
	 case 6:
	    currprefs.keyboard_lang = KBD_LANG_ES;
	    break;
	 default:
	    break;
	}
	return 1;
    }
}

/* The following ones don't work as we never realloc the arrays... */
/*
 * Changes chip memory size
 *  (reboots)
 */
/****i* januae-uaelib/emulib_ChgCMemSize ***********************************
*
*   NAME
*       emulib_ChgCMemSize() -- Request a reset with an updated chip-memory
*       preference.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 emulib_ChgCMemSize (struct regstruct *regs,
*       uae_u32 memsize);
*
*   FUNCTION
*       Normalize unsupported chip-memory sizes to 2 MiB, clear guest D0,
*       set the chip-memory preference, and request an emulator reset.
*
*   INPUTS
*       regs - 68k register state; memsize - Requested chip-memory size in
*       bytes.
*
*   RESULT
*       One.
*
*   NOTES
*       This operation updates a preference and initiates a reset; it does
*       not itself resize the allocated memory.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 emulib_ChgCMemSize (struct regstruct *regs, uae_u32 memsize)
{
    if (memsize != 0x80000 && memsize != 0x100000 &&
	memsize != 0x200000) {
	memsize = 0x200000;
	write_log ("Unsupported chipmem size!\n");
    }
    m68k_dreg (regs, 0) = 0;

    currprefs.chipmem_size = memsize;
    uae_reset(0);
    return 1;
}

/*
 * Changes slow memory size
 *  (reboots)
 */
/****i* januae-uaelib/emulib_ChgSMemSize ***********************************
*
*   NAME
*       emulib_ChgSMemSize() -- Request a reset with an updated slow-memory
*       preference.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 emulib_ChgSMemSize (struct regstruct *regs,
*       uae_u32 memsize);
*
*   FUNCTION
*       Replace unsupported bogomem sizes with zero, clear guest D0, update
*       the slow-memory preference, and request a reset.
*
*   INPUTS
*       regs - 68k register state; memsize - Requested slow-memory size in
*       bytes.
*
*   RESULT
*       One.
*
*   NOTES
*       Memory allocation is not performed by this routine.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 emulib_ChgSMemSize (struct regstruct *regs, uae_u32 memsize)
{
    if (memsize != 0x80000 && memsize != 0x100000 &&
	memsize != 0x180000 && memsize != 0x1C0000) {
	memsize = 0;
	write_log ("Unsupported bogomem size!\n");
    }

    m68k_dreg (regs, 0) = 0;
    currprefs.bogomem_size = memsize;
    uae_reset (0);
    return 1;
}

/*
 * Changes fast memory size
 *  (reboots)
 */
/****i* januae-uaelib/emulib_ChgFMemSize ***********************************
*
*   NAME
*       emulib_ChgFMemSize() -- Request a reset with an updated fast-memory
*       preference.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 emulib_ChgFMemSize (struct regstruct *regs,
*       uae_u32 memsize);
*
*   FUNCTION
*       Replace unsupported fast-memory sizes with zero, clear guest D0,
*       update the fast-memory preference, and request a reset.
*
*   INPUTS
*       regs - 68k register state; memsize - Requested fast-memory size in
*       bytes.
*
*   RESULT
*       Zero.
*
*   NOTES
*       The return value is zero even after the preference and reset request
*       have been applied.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 emulib_ChgFMemSize (struct regstruct *regs, uae_u32 memsize)
{
    if (memsize != 0x100000 && memsize != 0x200000 &&
	memsize != 0x400000 && memsize != 0x800000) {
	memsize = 0;
	write_log ("Unsupported fastmem size!\n");
    }
    m68k_dreg (regs, 0) = 0;
    currprefs.fastmem_size = memsize;
    uae_reset (0);
    return 0;
}

/*
 * Inserts a disk
 */
/****i* januae-uaelib/emulib_InsertDisk ************************************
*
*   NAME
*       emulib_InsertDisk() -- Set the pending image name for a floppy
*       drive.
*
*   SYNOPSIS
*       static uae_u32 emulib_InsertDisk (uaecptr name, uae_u32 drive);
*
*   FUNCTION
*       Read a zero-terminated filename from guest memory and copy it into
*       the selected drive entry in the changed preferences.
*
*   INPUTS
*       name - Guest address of the image filename; drive - Floppy index
*       from 0 through 3.
*
*   RESULT
*       One when the drive and filename length are accepted; zero otherwise.
*
****************************************************************************
*
*/

static uae_u32 emulib_InsertDisk (uaecptr name, uae_u32 drive)
{
    int i = 0;
    char real_name[256];

    if (drive > 3)
	return 0;

    while ((real_name[i] = get_byte (name + i)) != 0 && i++ != 254)
	;

    if (i == 255)
	return 0; /* ENAMETOOLONG */

    strcpy (changed_prefs.df[drive], real_name);

    return 1;
}

/*
 * Exits the emulator
 */
/****i* januae-uaelib/emulib_ExitEmu ***************************************
*
*   NAME
*       emulib_ExitEmu() -- Request emulator termination.
*
*   SYNOPSIS
*       static uae_u32 emulib_ExitEmu (void);
*
*   FUNCTION
*       Call the emulator quit routine.
*
*   INPUTS
*       None.
*
*   RESULT
*       One.
*
****************************************************************************
*
*/

static uae_u32 emulib_ExitEmu (void)
{
    uae_quit ();
    return 1;
}

/*
 * Gets UAE Configuration
 */
/****i* januae-uaelib/emulib_GetUaeConfig **********************************
*
*   NAME
*       emulib_GetUaeConfig() -- Write the current emulator configuration
*       into guest memory.
*
*   SYNOPSIS
*       static uae_u32 emulib_GetUaeConfig (uaecptr place);
*
*   FUNCTION
*       Serialize version, allocated memory sizes, frame rate, audio state,
*       joystick selections, keyboard layout, drive occupancy, and four
*       fixed-length floppy filename fields.
*
*   INPUTS
*       place - Writable guest address for the configuration data.
*
*   RESULT
*       One.
*
*   NOTES
*       The caller must provide space for the complete 1,060-byte
*       configuration record.
*
****************************************************************************
*
*/

static uae_u32 emulib_GetUaeConfig (uaecptr place)
{
    int i;

    put_long (place, UAEVERSION);
    put_long (place + 4, allocated_chipmem);
    put_long (place + 8, allocated_bogomem);
    put_long (place + 12, allocated_fastmem);
    put_long (place + 16, currprefs.gfx_framerate);
    put_long (place + 20, currprefs.produce_sound);
    put_long (place + 24, currprefs.jport0 | (currprefs.jport1 << 8));
    put_long (place + 28, currprefs.keyboard_lang);
    if (disk_empty (0))
	put_byte (place + 32, 0);
    else
	put_byte (place + 32, 1);
    if (disk_empty (1))
	put_byte (place + 33, 0);
    else
	put_byte (place + 33, 1);
    if (disk_empty(2))
	put_byte (place + 34, 0);
    else
	put_byte (place + 34, 1);
    if (disk_empty(3))
	put_byte (place + 35, 0);
    else
	put_byte (place + 35, 1);

    for (i = 0; i < 256; i++) {
	put_byte ((place + 36 + i), currprefs.df[0][i]);
	put_byte ((place + 36 + i + 256), currprefs.df[1][i]);
	put_byte ((place + 36 + i + 512), currprefs.df[2][i]);
	put_byte ((place + 36 + i + 768), currprefs.df[3][i]);
    }
    return 1;
}

/*
 * Sets UAE Configuration
 *
 * NOT IMPLEMENTED YET
 */
/****i* januae-uaelib/emulib_SetUaeConfig **********************************
*
*   NAME
*       emulib_SetUaeConfig() -- Accept the reserved configuration-set trap.
*
*   SYNOPSIS
*       static uae_u32 emulib_SetUaeConfig (uaecptr place);
*
*   FUNCTION
*       Return the current success constant without reading the supplied
*       configuration address or changing emulator settings.
*
*   INPUTS
*       place - Guest address reserved for configuration input; not
*       accessed.
*
*   RESULT
*       One.
*
*   NOTES
*       Configuration writing is not implemented in this routine.
*
****************************************************************************
*
*/

static uae_u32 emulib_SetUaeConfig (uaecptr place)
{
    return 1;
}

/*
 * Gets the name of the disk in the given drive
 */
/****i* januae-uaelib/emulib_GetDisk ***************************************
*
*   NAME
*       emulib_GetDisk() -- Copy the selected floppy filename to guest
*       memory.
*
*   SYNOPSIS
*       static uae_u32 emulib_GetDisk (uae_u32 drive, uaecptr name);
*
*   FUNCTION
*       Write the 256-byte filename field from the requested drive entry to
*       the supplied guest address.
*
*   INPUTS
*       drive - Floppy index from 0 through 3; name - Writable guest address
*       for 256 bytes.
*
*   RESULT
*       One for a valid drive index; zero otherwise.
*
****************************************************************************
*
*/

static uae_u32 emulib_GetDisk (uae_u32 drive, uaecptr name)
{
    int i;
    if (drive > 3)
	return 0;

    for (i = 0;i < 256; i++) {
	put_byte (name + i, currprefs.df[drive][i]);
    }
    return 1;
}

/*
 * Enter debugging state
 */
/****i* januae-uaelib/emulib_Debug *****************************************
*
*   NAME
*       emulib_Debug() -- Enter the debugger when debugger support is
*       compiled.
*
*   SYNOPSIS
*       static uae_u32 emulib_Debug (void);
*
*   FUNCTION
*       Invoke the debugger activation entry point only in builds that
*       enable DEBUGGER.
*
*   INPUTS
*       None.
*
*   RESULT
*       One when DEBUGGER is enabled; zero otherwise.
*
****************************************************************************
*
*/

static uae_u32 emulib_Debug (void)
{
#ifdef DEBUGGER
    activate_debugger ();
    return 1;
#else
    return 0;
#endif
}

/* We simply find the first "text" hunk, get the offset of its actual code segment (20 bytes away)
 * and add that offset to the base address of the object.  Now we've got code to execute.
 *
 * @@@ Brian: does anything actually use this yet?
 * @@@ Bernd: Not yet.  It needs to get much better.  Should spawn off a seperate task to handle the
 *            function, and then somehow "signal" the Amiga caller that completion or error has
 *            occurred.  I don't know how to do that, so right now it is a synchronous call.  Yuck!
 *            Would be nice to implement jpg decompression functionality for the Amiga which used
 *            the UAE Host to do all the work, for example.
 * @@@ Brian: I disabled it to prevent people from starting to use it - if that happens, we're
 *            stuck with this.
 */
#if 0
static uae_u32 FindFunctionInObject (uae_u8 *objectptr)
{
    uae_u8 *text_hdr;
    uae_u8 offset;
    text_hdr = (uae_u8 *)strstr ("text", (char *)objectptr);
    if (text_hdr != 0) {
	offset = *(text_hdr + 19);
	return (uae_u32)(objectptr + offset);
    }
    return 0;
}
#endif

#define CREATE_NATIVE_FUNC_PTR uae_u32 (* native_func)( uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, \
						 uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32)
#define SET_NATIVE_FUNC(x) native_func = (uae_u32 (*)(uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32, uae_u32))(x)
#define CALL_NATIVE_FUNC( d1,d2,d3,d4,d5,d6,d7,a1,a2,a3,a4,a5,a6 ) if(native_func) native_func( d1,d2,d3,d4,d5,d6,d7,a1,a2,a3,a4,a5,a6 )
/* A0 - Contains a ptr to the native .obj data.  This ptr is Amiga-based. */
/*      We simply find the first function in this .obj data, and execute it. */
/****i* januae-uaelib/emulib_ExecuteNativeCode *****************************
*
*   NAME
*       emulib_ExecuteNativeCode() -- Handle the reserved native-object
*       execution operation.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 emulib_ExecuteNativeCode (struct regstruct
*       *regs);
*
*   FUNCTION
*       Return without loading or invoking a native object. The former
*       object-execution path is excluded from compilation.
*
*   INPUTS
*       regs - 68k register state; unused by the active implementation.
*
*   RESULT
*       Zero.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 emulib_ExecuteNativeCode (struct regstruct *regs)
{
#if 0
    uaecptr object_AAM = m68k_areg (regs, 0);
    uae_u32 d1 = m68k_dreg (regs, 1);
    uae_u32 d2 = m68k_dreg (regs, 2);
    uae_u32 d3 = m68k_dreg (regs, 3);
    uae_u32 d4 = m68k_dreg (regs, 4);
    uae_u32 d5 = m68k_dreg (regs, 5);
    uae_u32 d6 = m68k_dreg (regs, 6);
    uae_u32 d7 = m68k_dreg (regs, 7);
    uae_u32 a1 = m68k_areg (regs, 1);
    uae_u32 a2 = m68k_areg (regs, 2);
    uae_u32 a3 = m68k_areg (regs, 3);
    uae_u32 a4 = m68k_areg (regs, 4);
    uae_u32 a5 = m68k_areg (regs, 5);
    uae_u32 a6 = m68k_areg (regs, 6);

    uae_u8* object_UAM = NULL;
    CREATE_NATIVE_FUNC_PTR;

    if( get_mem_bank( object_AAM ).check( object_AAM, 1 ) )
	object_UAM = get_mem_bank( object_AAM).xlateaddr( object_AAM );

    if( object_UAM )
    {
	SET_NATIVE_FUNC( FindFunctionInObject( object_UAM ) );
	CALL_NATIVE_FUNC( d1, d2, d3, d4, d5, d6, d7, a1, a2, a3, a4, a5, a6);
    }
    return 1;
#endif
    return 0;
}

/****i* januae-uaelib/emulib_Minimize **************************************
*
*   NAME
*       emulib_Minimize() -- Handle the reserved host minimize operation.
*
*   SYNOPSIS
*       static uae_u32 emulib_Minimize (void);
*
*   FUNCTION
*       Return without requesting a host window or application minimize
*       action.
*
*   INPUTS
*       None.
*
*   RESULT
*       Zero.
*
****************************************************************************
*
*/

static uae_u32 emulib_Minimize (void)
{
    return 0; // OSDEP_minimize_uae();
}

/****i* januae-uaelib/uaelib_demux *****************************************
*
*   NAME
*       uaelib_demux() -- Dispatch a guest UAE-library request to its
*       implementation.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 uaelib_demux (TrapContext *context);
*
*   FUNCTION
*       Read the operation selector and arguments from the guest stack,
*       invoke the corresponding emulator service, and return its value to
*       the trap caller.
*
*   INPUTS
*       context - Trap context containing the 68k registers and stack
*       arguments.
*
*   RESULT
*       Result returned by the selected operation; zero for an unrecognized
*       selector.
*
*   NOTES
*       Picasso96 and hardware-sprite selectors are present only when their
*       respective build options are enabled.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 uaelib_demux (TrapContext *context)
{
#define ARG0 (get_long (m68k_areg (&context->regs, 7) + 4))
#define ARG1 (get_long (m68k_areg (&context->regs, 7) + 8))
#define ARG2 (get_long (m68k_areg (&context->regs, 7) + 12))
#define ARG3 (get_long (m68k_areg (&context->regs, 7) + 16))
#define ARG4 (get_long (m68k_areg (&context->regs, 7) + 20))
#define ARG5 (get_long (m68k_areg (&context->regs, 7) + 24))

    switch (ARG0) {
     case 0: return emulib_GetVersion ();
     case 1: return emulib_GetUaeConfig (ARG1);
     case 2: return emulib_SetUaeConfig (ARG1);
     case 3: return emulib_HardReset ();
     case 4: return emulib_Reset ();
     case 5: return emulib_InsertDisk (ARG1, ARG2);
     case 6: return emulib_EnableSound (ARG1);
     case 7: return emulib_EnableJoystick (ARG1);
     case 8: return emulib_SetFrameRate (ARG1);
     case 9: return emulib_ChgCMemSize (&context->regs, ARG1);
     case 10: return emulib_ChgSMemSize (&context->regs, ARG1);
     case 11: return emulib_ChgFMemSize (&context->regs, ARG1);
     case 12: return emulib_ChangeLanguage (ARG1);
	/* The next call brings bad luck */
     case 13: return emulib_ExitEmu ();
     case 14: return emulib_GetDisk (ARG1, ARG2);
     case 15: return emulib_Debug ();

#ifdef PICASSO96
     case 16: return picasso_FindCard (&context->regs);
     case 17: return picasso_FillRect (&context->regs);
     case 18: return picasso_SetSwitch (&context->regs);
     case 19: return picasso_SetColorArray (&context->regs);
     case 20: return picasso_SetDAC (&context->regs);
     case 21: return picasso_SetGC (&context->regs);
     case 22: return picasso_SetPanning (&context->regs);
     case 23: return picasso_CalculateBytesPerRow (&context->regs);
     case 24: return picasso_BlitPlanar2Chunky (&context->regs);
     case 25: return picasso_BlitRect (&context->regs);
     case 26: return picasso_SetDisplay (&context->regs);
     case 27: return picasso_BlitTemplate (&context->regs);
     case 28: return picasso_BlitRectNoMaskComplete (&context->regs);
     case 29: return picasso_InitCard (&context->regs);
     case 30: return picasso_BlitPattern (&context->regs);
     case 31: return picasso_InvertRect (&context->regs);
     case 32: return picasso_BlitPlanar2Direct (&context->regs);
     /* case 34: return picasso_WaitVerticalSync (); handled in asm-code */
     case 35: return allocated_gfxmem ? 1 : 0;
#ifdef HARDWARE_SPRITE_EMULATION
     case 36: return picasso_SetSprite (&context->regs);
     case 37: return picasso_SetSpritePosition (&context->regs);
     case 38: return picasso_SetSpriteImage (&context->regs);
     case 39: return picasso_SetSpriteColor (&context->regs);
#endif
//     case 40: return picasso_DrawLine ();
#endif
     case 68: return emulib_Minimize ();
     case 69: return emulib_ExecuteNativeCode (&context->regs);

     case 80: return currprefs.maprom ? currprefs.maprom : 0xffffffff;
     case 81: return cfgfile_uaelib (ARG1, ARG2, ARG3, ARG4);
     case 82: return cfgfile_uaelib_modify (ARG1, ARG2, ARG3, ARG4, ARG5);
    }
    return 0;
}

/*
 * Installs the UAE LIBRARY
 */
/****i* januae-uaelib/emulib_install ***************************************
*
*   NAME
*       emulib_install() -- Install the guest library entry trap in runtime
*       memory.
*
*   SYNOPSIS
*       void emulib_install (void);
*
*   FUNCTION
*       Emit a call to the library dispatcher followed by an RTS at
*       RTAREA_BASE plus 0xFF60, then restore the previous emission address.
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

void emulib_install (void)
{
    uaecptr a = here ();
    org (RTAREA_BASE + 0xFF60);
//    dw (0x4eb9);
//    dw ((RTAREA_BASE >> 16) | get_word (RTAREA_BASE + 36));
//    dw (get_word (RTAREA_BASE + 38) + 12);
    calltrap (define_trap (uaelib_demux, 0, ""));
    dw (RTS);
    org (a);
}
