 /*
  * UAE - The Un*x Amiga Emulator
  *
  * A "replacement" for a missing Kickstart
  * Warning! Q&D
  *
  * (c) 1995 Bernd Schmidt
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "cpu_prefetch.h"
#include "cia.h"
#include "disk.h"
#include "ersatz.h"
#include "gui.h"

#define EOP_INIT     0
#define EOP_NIMP     1
#define EOP_SERVEINT 2
#define EOP_DOIO     3
#define EOP_OPENLIB  4
#define EOP_AVAILMEM 5
#define EOP_ALLOCMEM 6
#define EOP_ALLOCABS 7
#define EOP_LOOP 8

static int already_failed = 0;

/****f* januae-ersatz/init_ersatz_rom ************************************
*
*   NAME
*       init_ersatz_rom() -- Install the replacement Kickstart entry sequence.
*
*   SYNOPSIS
*       void init_ersatz_rom (uae_u8 *data);
*
*   FUNCTION
*       Write the initial stack pointer, reset vector, and replacement
*       opcode sequence into the supplied Kickstart memory.
*
*   INPUTS
*       data - Writable start of a buffer with at least 50 bytes available.
*
*   RESULT
*       None.
*
*   NOTES
*       Exactly 50 bytes are written consecutively. No bounds check is made.
*
****************************************************************************
*
*/

void init_ersatz_rom (uae_u8 *data)
{
    *data++ = 0x00; *data++ = 0x08; /* initial SP */
    *data++ = 0x00; *data++ = 0x00;
    *data++ = 0x00; *data++ = 0xF8; /* initial PC */
    *data++ = 0x00; *data++ = 0x08;

    *data++ = 0xFF; *data++ = 0x0D;
    *data++ = 0x00; *data++ = EOP_INIT;
    *data++ = 0xFF; *data++ = 0x0D;
    *data++ = 0x00; *data++ = EOP_NIMP;

    *data++ = 0xFF; *data++ = 0x0D;
    *data++ = 0x00; *data++ = EOP_LOOP;
    *data++ = 0xFF; *data++ = 0x0D;
    *data++ = 0x00; *data++ = EOP_DOIO;

    *data++ = 0x4E; *data++ = 0x75;
    *data++ = 0xFF; *data++ = 0x0D;
    *data++ = 0x00; *data++ = EOP_SERVEINT;
    *data++ = 0x4E; *data++ = 0x73;

    *data++ = 0xFF; *data++ = 0x0D;
    *data++ = 0x00; *data++ = EOP_AVAILMEM;
    *data++ = 0x4E; *data++ = 0x75;
    *data++ = 0xFF; *data++ = 0x0D;

    *data++ = 0x00; *data++ = EOP_ALLOCMEM;
    *data++ = 0x4E; *data++ = 0x75;
    *data++ = 0xFF; *data++ = 0x0D;
    *data++ = 0x00; *data++ = EOP_ALLOCABS;

    *data++ = 0x4E; *data++ = 0x75;
}

/****i* januae-ersatz/ersatz_failed **************************************
*
*   NAME
*       ersatz_failed() -- Signal an unsupported Kickstart replacement operation.
*
*   SYNOPSIS
*       static void ersatz_failed (void);
*
*   FUNCTION
*       Report the unsupported replacement operation and request an emulator
*       restart. Subsequent calls do not repeat the message or restart request.
*
*   INPUTS
*       None.
*
*   RESULT
*       None.
*
****************************************************************************
*
*/

static void ersatz_failed (void)
{
    if (already_failed)
	return;
    already_failed = 1;
//    notify_user (NUMSG_KICKREPNO);
    gui_message ("Diskfile in DF0: is not compatible with Kickstart replacement.\n");
    uae_restart (-1, NULL);
}

/****i* januae-ersatz/ersatz_doio ****************************************
*
*   NAME
*       ersatz_doio() -- Transfer sectors for a replacement disk I/O request.
*
*   SYNOPSIS
*       static void ersatz_doio (void);
*
*   FUNCTION
*       Read the request address from emulated register A1. Recognize
*       commands 2, 0x8002, and 9; all other commands request a restart.
*       Convert the request offset and length to 512-byte sectors and copy
*       those sectors from the emulated floppy through DISK_ersatz_read().
*
*   INPUTS
*       None; A1 holds a guest IORequest address.
*
*   RESULT
*       None.
*
*   NOTES
*       Request offsets and lengths are divided by 512 without validating
*       alignment. Command 9 follows the sector-transfer path even though
*       it is recognized as TD_MOTOR.
*
****************************************************************************
*
*/

static void ersatz_doio (void)
{
    uaecptr request = m68k_areg (&regs, 1);
    switch (get_word (request + 0x1C)) {
     case 9: /* TD_MOTOR is harmless */
     case 2: case 0x8002: /* READ commands */
	break;

     default:
	write_log ("Only CMD_READ supported in DoIO()\n");
	ersatz_failed ();
    }
    {
	uaecptr dest = get_long (request + 0x28);
	int start = get_long (request + 0x2C) / 512;
	int nsecs = get_long (request + 0x24) / 512;
	int tr = start / 11;
	int sec = start % 11;
	while (nsecs--) {
	    DISK_ersatz_read (tr, sec, dest);
	    dest += 512;
	    if (++sec == 11)
		sec = 0, tr++;
	}
    }
}

/****i* januae-ersatz/ersatz_init ****************************************
*
*   NAME
*       ersatz_init() -- Initialize the replacement Kickstart environment.
*
*   SYNOPSIS
*       static void ersatz_init (void);
*
*   FUNCTION
*       Initialize the minimal replacement Exec state and read the boot
*       blocks from DF0. A Kickstart disk triggers ROM loading, disk
*       ejection, and an emulator reset; otherwise execution continues from
*       the disk boot code with initialized custom-chip registers.
*
*   INPUTS
*       None.
*
*   RESULT
*       None.
*
*   NOTES
*       An empty DF0 requests a restart without initializing the
*       replacement system.
*
****************************************************************************
*
*/

static void ersatz_init (void)
{
    int f;
    uaecptr request;
    uaecptr a;

    already_failed = 0;
    write_log ("initializing kickstart replacement\n");
    if (disk_empty (0)) {
	already_failed = 1;
//	notify_user (NUMSG_KICKREP);
	gui_message ("You need to have a diskfile in DF0 to use the Kickstart replacement!\n");
	uae_restart (-1, NULL);
	return;
    }

    regs.s = 0;
    /* Set some interrupt vectors */
    for (a = 8; a < 0xC0; a += 4) {
	put_long (a, 0xF8001A);
    }
    regs.isp = regs.msp = regs.usp = 0x800;
    m68k_areg (&regs, 7) = 0x80000;
    regs.intmask = 0;

    /* Build a dummy execbase */
    put_long (4, m68k_areg (&regs, 6) = 0x676);
    put_byte (0x676 + 0x129, 0);
    for (f = 1; f < 105; f++) {
	put_word (0x676 - 6*f, 0x4EF9);
	put_long (0x676 - 6*f + 2, 0xF8000C);
    }
    /* Some "supported" functions */
    put_long (0x676 - 456 + 2, 0xF80014);
    put_long (0x676 - 216 + 2, 0xF80020);
    put_long (0x676 - 198 + 2, 0xF80026);
    put_long (0x676 - 204 + 2, 0xF8002c);
    put_long (0x676 - 210 + 2, 0xF8002a);

    /* Build an IORequest */
    request = 0x800;
    put_word (request + 0x1C, 2);
    put_long (request + 0x28, 0x4000);
    put_long (request + 0x2C, 0);
    put_long (request + 0x24, 0x200 * 4);
    m68k_areg (&regs, 1) = request;
    ersatz_doio ();
    /* kickstart disk loader */
    if (get_long(0x4000) == 0x4b49434b) {
	/* a kickstart disk was found in drive 0! */
	write_log ("Loading Kickstart rom image from Kickstart disk\n");
	/* print some notes... */
	write_log ("NOTE: if UAE crashes set CPU to 68000 and/or chipmem size to 512KB!\n");

	/* read rom image from kickstart disk */
	put_word (request + 0x1C, 2);
	put_long (request + 0x28, 0xF80000);
	put_long (request + 0x2C, 0x200);
	put_long (request + 0x24, 0x200 * 512);
	m68k_areg (&regs, 1) = request;
	ersatz_doio ();

	/* read rom image once again to mirror address space.
	   not elegant, but it works... */
	put_word (request + 0x1C, 2);
	put_long (request + 0x28, 0xFC0000);
	put_long (request + 0x2C, 0x200);
	put_long (request + 0x24, 0x200 * 512);
	m68k_areg (&regs, 1) = request;
	ersatz_doio ();

	disk_eject (0);

	m68k_setpc (&regs, 0xFC0002);
	fill_prefetch_slow (&regs);
	uae_reset (0);
	ersatzkickfile = 0;
	return;
    }

    m68k_setpc (&regs, 0x400C);
    fill_prefetch_slow (&regs);

    /* Init the hardware */
    put_long (0x3000, 0xFFFFFFFEul);
    put_long (0xDFF080, 0x3000);
    put_word (0xDFF088, 0);
    put_word (0xDFF096, 0xE390);
    put_word (0xDFF09A, 0xE02C);
    put_word (0xDFF09E, 0x0000);
    put_word (0xDFF092, 0x0038);
    put_word (0xDFF094, 0x00D0);
    put_word (0xDFF08E, 0x2C81);
    put_word (0xDFF090, 0xF4C1);
    put_word (0xDFF02A, 0x8000);

    put_byte (0xBFD100, 0xF7);
    put_byte (0xBFEE01, 0);
    put_byte (0xBFEF01, 0x08);
    put_byte (0xBFDE00, 0x04);
    put_byte (0xBFDF00, 0x84);
    put_byte (0xBFDD00, 0x9F);
    put_byte (0xBFED01, 0x9F);
}

/****f* januae-ersatz/ersatz_perform *************************************
*
*   NAME
*       ersatz_perform() -- Dispatch a replacement Kickstart opcode.
*
*   SYNOPSIS
*       void ersatz_perform (uae_u16 what);
*
*   FUNCTION
*       Supported cases initialize the replacement environment, acknowledge
*       pending interrupts, run disk I/O, or update emulated CPU registers.
*       Unimplemented and unknown operations request an emulator restart.
*
*   INPUTS
*       what - Replacement opcode encoded by the Kickstart entry sequence.
*
*   RESULT
*       None; selected operations update emulator registers or guest memory.
*
*   NOTES
*       Memory availability and allocation cases return fixed values in D0;
*       they do not allocate host or guest memory. The unsupported library
*       open and unknown cases request a restart.
*
****************************************************************************
*
*/

void ersatz_perform (uae_u16 what)
{
    switch (what) {
     case EOP_INIT:
	ersatz_init ();
	break;

     case EOP_SERVEINT:
	/* Just reset all the interrupt request bits */
	put_word (0xDFF09C, get_word (0xDFF01E) & 0x3FFF);
	break;

     case EOP_DOIO:
	ersatz_doio ();
	break;

     case EOP_AVAILMEM:
	m68k_dreg (&regs, 0) = m68k_dreg (&regs, 1) & 4 ? 0 : 0x70000;
	break;

     case EOP_ALLOCMEM:
	m68k_dreg (&regs, 0) = m68k_dreg (&regs, 1) & 4 ? 0 : 0x0F000;
	break;

     case EOP_ALLOCABS:
	m68k_dreg (&regs, 0) = m68k_areg (&regs, 1);
	break;

     case EOP_NIMP:
	write_log ("Unimplemented Kickstart function called\n");
	ersatz_failed ();

	/* fall through */
     case EOP_LOOP:
	m68k_setpc (&regs, 0xF80010);
	break;

     case EOP_OPENLIB:
     default:
	write_log ("Internal error. Giving up.\n");
	ersatz_failed ();
    }
}
