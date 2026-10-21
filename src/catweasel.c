/************************************************************************ 
 *
 * catweasel.c
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

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef CATWEASEL

#include "options.h"
#include "memory.h"
#include "catweasel.h"
#include "disk.h"
#include "uae.h"


#if defined(__linux__)
#define outb(v,port) ioport_write(port,v)
#define inb(port) ioport_read(port)
#else
#define PCICARD_OUTBYTE(card,address,value) outb(value, address)
#define PCICARD_INBYTE(card,address) inb(address)

int gEnableCWLogging = 0;

/****i* januae-catweasel/outb
 *
 *   NAME
 *       outb()
 *
 *   SYNOPSIS
 *       static void outb(unsigned char v, unsigned int address )
 *
 *   FUNCTION
 *       Write an 8-bit value to the memory-mapped Catweasel register address.
 *
 *   NOTES
 *       Available in the non-Linux build only. The address is dereferenced as a byte
 *       pointer; register accesses at offsets >= 0xe0 may be logged when
 *       gEnableCWLogging equals 1.
 *
 ****************************************************************************/

static void outb(unsigned char v, unsigned int address )
{
	*((unsigned char *)address)=v;
	if ( gEnableCWLogging == 1)
	{
		if ( (address & 0xFF) >= 0xe0 )
			write_log( "Written %x to address %x.\n" ,v, address );
	}
}

/****i* januae-catweasel/inb
 *
 *   NAME
 *       inb()
 *
 *   SYNOPSIS
 *       static unsigned char inb(unsigned int address)
 *
 *   FUNCTION
 *       Read one byte from a memory-mapped Catweasel register address.
 *
 *   RESULT
 *       Returns the register byte.
 *
 *   NOTES
 *       Available in the non-Linux build only. Logging is conditional on
 *       gEnableCWLogging and the low register offset.
 *
 ****************************************************************************/

static unsigned char inb(unsigned int address)
{
	unsigned char tValue = *((unsigned char *)address);
	if ( gEnableCWLogging == 1)
	{
		if ( (address & 0xFF) >= 0xe0 )
			write_log( "Read value %x from address %p.\n", tValue, address);
	}
	return tValue;
}
#include <clib/alib_protos.h>
#include <exec/exec.h>
#include <exec/devices.h>
#include <exec/types.h>
#include <clib/exec_protos.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>

/****i* januae-catweasel/sleep_millis
 *
 *   NAME
 *       sleep_millis()
 *
 *   SYNOPSIS
 *       void sleep_millis( int time )
 *
 *   FUNCTION
 *       Wait through timer.device for the specified duration in milliseconds.
 *
 *   NOTES
 *       Non-Linux implementation. Its static timer request and message port are
 *       lazily initialized but are not freed here; initialization and OpenDevice
 *       errors are not checked before DoIO.
 *
 ****************************************************************************/

void sleep_millis( int time )
{
	static struct timerequest *tRequest = NULL;
	static struct MsgPort *tMsgPort = NULL;
	int tErrorCode = 0;

	if ( tRequest  == NULL )
	{
		tMsgPort = CreateMsgPort();
		tRequest = (struct timerequest *)CreateIORequest( tMsgPort, sizeof( struct timerequest) );
		tErrorCode = OpenDevice("timer.device", 0, (struct IORequest *)tRequest, 0L );
	}

	tRequest -> tr_node.io_Command = TR_ADDREQUEST;
	tRequest -> tr_time.tv_secs = 0;
	tRequest -> tr_time.tv_micro = time * 1000;

	DoIO((struct IORequest *)tRequest );
}

#include "mk4_firmware.h"

/****i* januae-catweasel/cwfloppy_probe_mk3
 *
 *   NAME
 *       cwfloppy_probe_mk3()
 *
 *   SYNOPSIS
 *       static int cwfloppy_probe_mk3(unsigned int iobase)
 *
 *   FUNCTION
 *       Program the Mk III PCI bridge registers for direct Catweasel access.
 *
 *   RESULT
 *       Returns 0.
 *
 *   NOTES
 *       Non-Linux implementation. Sets global controller type to Mk III, mouse
 *       capability to 2 and direct_access to 1; it does not verify hardware
 *       responses.
 *
 ****************************************************************************/

static int cwfloppy_probe_mk3(unsigned int iobase)
{

	write_log( "Catweasel Mk III initialising.\n ");

    cwc.type = CATWEASEL_TYPE_MK3;

#if 1
    PCICARD_OUTBYTE( pcicard,  iobase + 0x0,0xf1 );
    PCICARD_OUTBYTE( pcicard,  iobase + 0x1,0x00 );
    PCICARD_OUTBYTE( pcicard,  iobase + 0x2,0x00 );
    PCICARD_OUTBYTE( pcicard,  iobase + 0x4,0x00 );
    PCICARD_OUTBYTE( pcicard,  iobase + 0x5,0x00 );
    PCICARD_OUTBYTE( pcicard,  iobase + 0x29,0x00 );
    PCICARD_OUTBYTE( pcicard,  iobase + 0x2b,0x00 );
#endif

	cwc.can_mouse = 2;
	cwc.direct_access = 1;

    return 0;
}

/****i* januae-catweasel/cw4_memory_check
 *
 *   NAME
 *       cw4_memory_check()
 *
 *   SYNOPSIS
 *       static int cw4_memory_check(void *address)
 *
 *   FUNCTION
 *       Write and read a test pattern through the Catweasel Mk IV memory window.
 *
 *   RESULT
 *       Returns 0 for matching bytes, or -EBUSY on the first mismatch.
 *
 *   NOTES
 *       Non-Linux helper. The address is converted to an integer register base;
 *       failure messages are issued through write_log.
 *
 ****************************************************************************/

static int cw4_memory_check(void *address)
{
    int             err;
    int             timeout;
    int             iobase = (int)address;
    unsigned char       mem[] = { 1,2,4,8,16,32,64,128,255,44,65,3 };

    // check if memory access is working 
    PCICARD_OUTBYTE( pcicard,  iobase + CW_FLOPPY_RESETPOINTER, 0 );

    for(err=0; err!=sizeof(mem); ++err)
        PCICARD_OUTBYTE( pcicard,  iobase + CW_FLOPPY_MEMORY, mem[err] );

    PCICARD_OUTBYTE( pcicard,  iobase + CW_FLOPPY_RESETPOINTER, 0);

    for(err=0; err!=sizeof(mem); ++err)
    {
        if((timeout=PCICARD_INBYTE( pcicard, iobase + CW_FLOPPY_MEMORY)) != mem[err]) 
        {
            write_log(  (char *)"memory test failed. Byte ",err);
            write_log(  (char *)"should be ",mem[err]);
            write_log(  (char *)"but is ",timeout);
            return -EBUSY;
        }
    }
    return 0;
}

/****i* januae-catweasel/cwfloppy_probe_mk4
 *
 *   NAME
 *       cwfloppy_probe_mk4()
 *
 *   SYNOPSIS
 *       static int cwfloppy_probe_mk4(unsigned int iobase)
 *
 *   FUNCTION
 *       Reset and load the Mk IV FPGA firmware over its PCI register interface.
 *
 *   RESULT
 *       Returns 0 on completion, or -EBUSY on a firmware-upload timeout.
 *
 *   NOTES
 *       Non-Linux implementation. The code disables interrupts during firmware
 *       upload, re-enables them on success or timeout, and finally reports the
 *       emulated controller as Mk III.
 *
 ****************************************************************************/

static int cwfloppy_probe_mk4(unsigned int iobase)
{
    int             err;
    unsigned int        timeout;
    unsigned char       mem[] = { 1,2,4,8,16,32,64,128,255,44,65,3 };

    cwc.type = CATWEASEL_TYPE_MK4;

    write_log((char *)"Controller PCI Catweasel MK4\n",iobase);

    // Initialize PCI bridge 
    PCICARD_OUTBYTE( pcicard,  iobase + 0x0, 0xf1 );
    PCICARD_OUTBYTE( pcicard,  iobase + 0x1,0x00 );
    PCICARD_OUTBYTE( pcicard,  iobase + CW_DATA_DIRECTION, 0xe3 );
    PCICARD_OUTBYTE( pcicard,  iobase + CW_SELECT_BANK, CW_BANK_FLOPPY);
    PCICARD_OUTBYTE( pcicard,  iobase + 0x4, 0x00);
    PCICARD_OUTBYTE( pcicard,  iobase + 0x5, 0x00);
    PCICARD_OUTBYTE( pcicard,  iobase + 0x29, 0x00);
    PCICARD_OUTBYTE( pcicard,  iobase + 0x2b, 0x00);

    // reset FPGA 
    //PCICARD_OUTBYTE( pcicard,  iobase + 0x2, 0xe3 );

    Disable();
    PCICARD_OUTBYTE( pcicard, iobase + CW_SELECT_BANK, CW_BANK_RESETFPGA);

    Delay(1);

//  cw_msdelay(IO_Timer,1);
    PCICARD_OUTBYTE( pcicard, iobase + CW_SELECT_BANK,CW_BANK_FLOPPY);

    // upload firmware 
    for(err=0; err!=sizeof(cw_firmware); ++err)
    {
        // set some direction 
        PCICARD_OUTBYTE( pcicard,  iobase + CW_SELECT_BANK, (cw_firmware[err] & 1)?(CW_BANK_FLOPPY+2):CW_BANK_FLOPPY );

        // wait for FPGA 
        timeout=0;
        while(!(PCICARD_INBYTE( pcicard, iobase+7)&8))
        {
//          cw_udelay(IO_Timer,1);
            if(++timeout >= 1000000)
            {
                Enable();
                write_log((char *)"timeout while writing firmware\n");
                return -EBUSY;
            }
        }
        // write byte 
        PCICARD_OUTBYTE( pcicard,  iobase + CW_FLOPPY_JOYDAT, cw_firmware[err] );
    }

    Enable();

    write_log((char *)"CW MEMORY is %p.\n",iobase + CW_FLOPPY_MEMORY);

    // now wait until FPGA really comes alive 
    timeout=0;
    while(PCICARD_INBYTE( pcicard, iobase + CW_FLOPPY_CONTROL) == 10) 
    {
        // cw_msdelay(IO_Timer,1);
        Delay(1);
        timeout++; 
    }

    PCICARD_OUTBYTE( pcicard,  iobase + CW_SELECT_BANK, CW_BANK_FLOPPY);

//    write_log( "FPGA is alive. Enabling byte logging.\n" );
  //  gEnableCWLogging = 1;

    write_log( "Catweasel type is %d.\n ",cwc.type );
    if ( cwc.type == CATWEASEL_TYPE_MK4 )
    {	
    	write_log( "Changing type to Mk3 as this is the emulated Catweasel.\n" );
		cwc.type = CATWEASEL_TYPE_MK3;
    }

	cwc.can_mouse = 2;
	cwc.direct_access = 1;

    return 0;
}

#endif


struct catweasel_contr cwc;

static int cwhsync;
static int handshake;

static int cwmk3port, cwmk3port1, cwmk3port2, cwmk3buttonsync;
static int handshake;
static int mouse_x[2], mouse_y[2], mouse_px[2], mouse_py[2];

/****i* januae-catweasel/catweasel_isjoystick
 *
 *   NAME
 *       catweasel_isjoystick()
 *
 *   SYNOPSIS
 *       static int catweasel_isjoystick (void)
 *
 *   FUNCTION
 *       Derive the joystick capability bits exposed by the active Catweasel
 *       controller.
 *
 *   RESULT
 *       Returns zero without direct access; otherwise returns can_joy with an
 *       optional high capability bit.
 *
 *   NOTES
 *       The high bit is added for Mk III with SID slot zero or for Mk IV and later.
 *
 ****************************************************************************/

static int catweasel_isjoystick (void)
{
	uae_u8 b = cwc.can_joy;
	if (!cwc.direct_access)
		return 0;
	if (b) {
		if (cwc.type == CATWEASEL_TYPE_MK3 && cwc.sid[0])
			b |= 0x80;
		if (cwc.type >= CATWEASEL_TYPE_MK4)
			b |= 0x80;
	}
	return b;
}

/****i* januae-catweasel/catweasel_ismouse
 *
 *   NAME
 *       catweasel_ismouse()
 *
 *   SYNOPSIS
 *       static int catweasel_ismouse (void)
 *
 *   FUNCTION
 *       Check whether direct Catweasel mouse input is available.
 *
 *   RESULT
 *       Returns zero without direct access, otherwise cwc.can_mouse.
 *
 ****************************************************************************/

static int catweasel_ismouse (void)
{
	if (!cwc.direct_access)
		return 0;
	return cwc.can_mouse;
}

static int hsync_requested;
/****i* januae-catweasel/hsync_request
 *
 *   NAME
 *       hsync_request()
 *
 *   SYNOPSIS
 *       static void hsync_request (void)
 *
 *   FUNCTION
 *       Extend the window during which horizontal-sync polling stays enabled.
 *
 *   NOTES
 *       Sets hsync_requested to 10; subsequent catweasel_hsync calls decrement it.
 *
 ****************************************************************************/

static void hsync_request (void)
{
	hsync_requested = 10;
};

/****i* januae-catweasel/sid_write
 *
 *   NAME
 *       sid_write()
 *
 *   SYNOPSIS
 *       static void sid_write (uae_u8 reg, uae_u8 val, int sidnum)
 *
 *   FUNCTION
 *       Write a register on one detected Catweasel SID interface.
 *
 *   NOTES
 *       The register write is ignored when sidnum >= cwc.can_sid; negative indices
 *       are not rejected. Performs two dummy data-register reads after selecting the
 *       SID register.
 *
 ****************************************************************************/

static void sid_write (uae_u8 reg, uae_u8 val, int sidnum)
{
	if (sidnum >= cwc.can_sid)
		return;
	catweasel_do_bput(0xd8, val);
	catweasel_do_bput(0xdc, reg | (sidnum << 7));
	catweasel_do_bget(0xd8); // dummy read
	catweasel_do_bget(0xd8); // dummy read
}

/****i* januae-catweasel/sid_read
 *
 *   NAME
 *       sid_read()
 *
 *   SYNOPSIS
 *       static uae_u8 sid_read (uae_u8 reg, int sidnum)
 *
 *   FUNCTION
 *       Read a register on one Catweasel SID interface.
 *
 *   RESULT
 *       Returns zero when sidnum is outside the upper capability bound; otherwise
 *       returns the data-port byte.
 *
 *   NOTES
 *       Selects read mode, performs two dummy reads and then returns a third read.
 *       Negative sidnum values are not rejected.
 *
 ****************************************************************************/

static uae_u8 sid_read (uae_u8 reg, int sidnum)
{
	if (sidnum >= cwc.can_sid)
		return 0;
	catweasel_do_bput(0xdc, 0x20 | reg | (sidnum << 7));
	catweasel_do_bget(0xd8); // dummy read
	catweasel_do_bget(0xd8); // dummy read
	return catweasel_do_bget(0xd8);
}

/****i* januae-catweasel/get_buttons
 *
 *   NAME
 *       get_buttons()
 *
 *   SYNOPSIS
 *       static uae_u8 get_buttons (void)
 *
 *   FUNCTION
 *       Combine joystick, mouse and optional SID-derived button signals into one bit
 *       mask.
 *
 *   RESULT
 *       Returns zero when the controller is earlier than Mk III or lacks direct
 *       access; otherwise returns a composite button mask.
 *
 *   NOTES
 *       Schedules hsync polling and reads hardware port and SID paddle registers.
 *       The returned bits are consumed in four-bit groups by catweasel_read_mouse.
 *
 ****************************************************************************/

static uae_u8 get_buttons (void)
{
	uae_u8 b, b2;
/*static int old[512];

int i;
*/
	b = 0;
	if (cwc.type < CATWEASEL_TYPE_MK3 || !cwc.direct_access)
		return b;
	hsync_request();
	b2 = catweasel_do_bget(0xc8) & (0x80 | 0x40);
	if (!(b2 & 0x80))
		b |= 0x80;
	if (!(b2 & 0x40))
		b |= 0x08;
/*
for ( i = 0 ; i < 0x1FF ; i++ )
{
	if ( old[ i ] != catweasel_do_bget( i ) )
		write_log( "port %x: %x   ", i, catweasel_do_bget( i ) );
	old[ i ]  = catweasel_do_bget( i );
}*/
//	if (cwc.type >= CATWEASEL_TYPE_MK4)
    {
		b &= ~0x80;
		catweasel_do_bput(3, 0x81);
		if (!(catweasel_do_bget(0x07) & 0x10))
			b |= 0x80;
		b2 = catweasel_do_bget(0xd0) ^ 15;
		catweasel_do_bput(3, 0x41);
		if (cwc.sid[0]) {
			b2 &= ~(1 | 2);
			if (sid_read(0x19, 0) > 0x7f)
				b2 |= 2;
			if (sid_read(0x1a, 0) > 0x7f)
				b2 |= 1;
		}
		if (cwc.sid[1]) {
			b2 &= ~(4 | 8);
			if (sid_read(0x19, 1) > 0x7f)
				b2 |= 8;
			if (sid_read(0x1a, 1) > 0x7f)
				b2 |= 4;
		}
	}
/* else {
		b2 = cwmk3port1 | (cwmk3port2 << 2);
	}*/

	b |= (b2 & (8 | 4)) << 3;
	b |= (b2 & (1 | 2)) << 1;
	return b;
}

/****i* januae-catweasel/catweasel_read_mouse
 *
 *   NAME
 *       catweasel_read_mouse()
 *
 *   SYNOPSIS
 *       int catweasel_read_mouse (int port, int *dx, int *dy, int *buttons)
 *
 *   FUNCTION
 *       Consume accumulated mouse motion and read the button state for one
 *       controller port.
 *
 *   RESULT
 *       Returns 1 on a direct-access mouse controller, or 0 when unavailable.
 *
 *   NOTES
 *       On success writes dx, dy and buttons; it clears the accumulated relative
 *       displacement. port indexes fixed two-element mouse arrays without bounds
 *       checking.
 *
 ****************************************************************************/

int catweasel_read_mouse (int port, int *dx, int *dy, int *buttons)
{
	if (!cwc.can_mouse || !cwc.direct_access)
		return 0;
	hsync_request();
	*dx = mouse_x[port];
	mouse_x[port] = 0;
	*dy = mouse_y[port];
	mouse_y[port] = 0;
	*buttons = (get_buttons() >> (port * 4)) & 0x0F;
	return 1;
}

/****i* januae-catweasel/sid_reset
 *
 *   NAME
 *       sid_reset()
 *
 *   SYNOPSIS
 *       static void sid_reset (void)
 *
 *   FUNCTION
 *       Clear SID registers 0 through 0x18 for both supported slots.
 *
 *   NOTES
 *       Delegates to sid_write; inaccessible slots are skipped by that helper.
 *
 ****************************************************************************/

static void sid_reset (void)
{
	int i;
	for (i = 0; i < 0x19; i++) {
		sid_write(i, 0, 0);
		sid_write(i, 0, 1);
	}
}

/****i* januae-catweasel/catweasel_detect_sid
 *
 *   NAME
 *       catweasel_detect_sid()
 *
 *   SYNOPSIS
 *       static void catweasel_detect_sid (void)
 *
 *   FUNCTION
 *       Probe supported SID slots and classify responsive chips by register
 *       behavior.
 *
 *   NOTES
 *       Resets both detected SID IDs; performs Mk IV paddle probing followed by
 *       oscillator tests. Records 6581 or 8580 in cwc.sid, and resets SID registers
 *       after detection.
 *
 ****************************************************************************/

static void catweasel_detect_sid (void)
{
	int i, j;
	uae_u8 b1, b2;

	cwc.sid[0] = cwc.sid[1] = 0;
	if (!cwc.can_sid || !cwc.direct_access)
		return;
	sid_reset();
	if (cwc.type >= CATWEASEL_TYPE_MK4) {
		catweasel_do_bput(3, 0x81);
		b1 = catweasel_do_bget(0xd0);
		for (i = 0; i < 100; i++) {
			sid_read(0x19, 0); // delay
			b2 = catweasel_do_bget(0xd0);
			if ((b1 & 3) != (b2 & 3))
				cwc.sid[0] = 6581;
			if ((b1 & 12) != (b2 & 12))
				cwc.sid[1] = 6581;
		}
	}
	catweasel_do_bput(3, 0x41);
	for (i = 0; i < 2 ;i++) {
		sid_reset();
		sid_write(0x0f, 0xff, i);
		sid_write(0x12, 0x10, i);
		for(j = 0; j != 1000; j++) {
			sid_write(0, 0, i);
			if((sid_read(0x1b, i) & 0x80) != 0) {
				cwc.sid[i] = 6581;
				break;
			}
		}
		sid_reset();
		sid_write(0x0f, 0xff, i);
		sid_write(0x12, 0x30, i);
		for(j = 0; j != 1000; j++) {
			sid_write(0, 0, i);
			if((sid_read(0x1b, i) & 0x80) != 0) {
				cwc.sid[i] = 8580;
				break;
			}
		}
	}
	sid_reset();
}

/****i* januae-catweasel/catweasel_hsync
 *
 *   NAME
 *       catweasel_hsync()
 *
 *   SYNOPSIS
 *       void catweasel_hsync (void)
 *
 *   FUNCTION
 *       Poll Catweasel keyboard-handshake, SID-button and mouse-counter state at
 *       periodic hsync intervals.
 *
 *   NOTES
 *       Returns early when direct access is unavailable or no poll is due. It
 *       accumulates relative mouse movement, acknowledges a pending keyboard
 *       handshake and throttles polling through cwhsync/hsync_requested.
 *
 ****************************************************************************/

void catweasel_hsync (void)
{
	int i;

	if (cwc.type < CATWEASEL_TYPE_MK3 || !cwc.direct_access)
		return;
	cwhsync--;
	if (cwhsync > 0)
		return;
	cwhsync = 10;
	if (handshake) {
		/* keyboard handshake */
		catweasel_do_bput (0xd0, 0);
		handshake = 0;
	}
	if (hsync_requested < 0)
		return;
	hsync_requested--;
	if (cwc.type == CATWEASEL_TYPE_MK3 && cwc.sid[0]) {
		uae_u8 b;
		cwmk3buttonsync--;
		if (cwmk3buttonsync <= 0) {
			cwmk3buttonsync = 30;
			b = 0;
			if (sid_read (0x19, 0) > 0x7f)
				b |= 2;
			if (sid_read (0x1a, 0) > 0x7f)
				b |= 1;
			if (cwmk3port == 0) {
				cwmk3port1 = b;
				catweasel_do_bput (0xd4, 0); // select port2
				cwmk3port = 1;
			} else {
				cwmk3port2 = b;
				catweasel_do_bget (0xd4); // select port1
				cwmk3port = 0;
			}
		}
	}
	if (!cwc.can_mouse)
		return;
	/* read MK4 mouse counters */
	catweasel_do_bput (3, 0x81);
	for (i = 0; i < 2; i++) {
		int x, y, dx, dy;
		x = (uae_s8)catweasel_do_bget (0xc4 + i * 8);
		y = (uae_s8)catweasel_do_bget (0xc0 + i * 8);
		dx = mouse_px[i] - x;
		if (dx > 127)
			dx = 255 - dx;
		if (dx < -128)
			dx = 255 + dx;
		dy = mouse_py[i] - y;
		if (dy > 127)
			dy = 255 - dy;
		if (dy < -128)
			dy = 255 + dy;
		mouse_x[i] -= dx;
		mouse_y[i] -= dy;
		mouse_px[i] = x;
		mouse_py[i] = y;
	}
	catweasel_do_bput (3, 0x41);
}

/*
void catweasel_hsync (void)
{
    if (cwhsync <= 0)
	return;
    cwhsync--;
    if (cwhsync == 0) {
        if (handshake)
	    outb (0, currprefs.catweasel_io + 0xd0);
	handshake = 0;
    }
}
*/

/****i* januae-catweasel/catweasel_read_joystick
 *
 *   NAME
 *       catweasel_read_joystick()
 *
 *   SYNOPSIS
 *       int catweasel_read_joystick (int stick, uae_u8 *dir, uae_u8 *buttons)
 *
 *   FUNCTION
 *       Read one Mk III-or-later joystick direction and primary button from raw
 *       registers.
 *
 *   RESULT
 *       Returns 1 and updates both output pointers on supported hardware, otherwise
 *       0.
 *
 *   NOTES
 *       stick == 1 selects the upper direction nibble; the pressed button is
 *       active-low. This routine does not test cwc.direct_access.
 *
 ****************************************************************************/

int catweasel_read_joystick (int stick, uae_u8 *dir, uae_u8 *buttons) {
  uae_u8 tButtonState;

  if (cwc.type < CATWEASEL_TYPE_MK3) {
    return 0;
  }

  *dir = inb (currprefs.catweasel_io + 0xc0);
  tButtonState = inb (currprefs.catweasel_io + 0xc8);

  if ( stick == 1 ) {
    *dir >>= 4;
  }
  if ( (tButtonState & (64 << stick)) == 0 ) {
    *buttons = 1;
  }
  else {
    *buttons = 0;
  }
  //write_log( "dir: %x  buttons: %x\n", *dir, *buttons );
  return 1;
}

/****i* januae-catweasel/catweasel_read_keyboard
 *
 *   NAME
 *       catweasel_read_keyboard()
 *
 *   SYNOPSIS
 *       int catweasel_read_keyboard (uae_u8 *keycode)
 *
 *   FUNCTION
 *       Fetch a pending Catweasel keyboard byte and start the acknowledgement
 *       handshake.
 *
 *   RESULT
 *       Returns 1 when a byte was accepted, 0 if no byte is ready or a handshake is
 *       pending.
 *
 *   NOTES
 *       Uses the controller base from currprefs.catweasel_io and marks a 10-tick
 *       handshake window.
 *
 ****************************************************************************/

int catweasel_read_keyboard (uae_u8 *keycode)
{
    uae_u8 v;
    if (cwc.type < CATWEASEL_TYPE_MK3)
	return 0;
    v = inb (currprefs.catweasel_io + 0xd4);
    if (!(v & 0x80))
	return 0;
    if (handshake)
	return 0;
    *keycode = inb (currprefs.catweasel_io + 0xd0);
    outb (0, currprefs.catweasel_io + 0xd0);
    handshake = 1;
    cwhsync = 10;
    return 1;
}

/****i* januae-catweasel/catweasel_do_bget
 *
 *   NAME
 *       catweasel_do_bget()
 *
 *   SYNOPSIS
 *       uae_u32 catweasel_do_bget (uaecptr addr)
 *
 *   FUNCTION
 *       Read an 8-bit Catweasel register through the target-dependent address
 *       mapping.
 *
 *   RESULT
 *       Returns the hardware byte when the access is accepted; non-AROS unsupported
 *       mappings return zero.
 *
 *   NOTES
 *       On AROS the address is always added to currprefs.catweasel_io. Non-AROS
 *       paths restrict Mk III and other controller address ranges.
 *
 ****************************************************************************/

uae_u32 catweasel_do_bget (uaecptr addr)
{
#if defined(__AROS__)
    return inb( currprefs.catweasel_io + addr );
#else

    if (cwc.type == CATWEASEL_TYPE_MK3) {
	if ((currprefs.catweasel_io & 3) == 0 && addr >= 0xc0 && addr <= 0xfc)
	    return inb (currprefs.catweasel_io + addr);
    } else {
	if (addr >= currprefs.catweasel_io && addr <= currprefs.catweasel_io + 8) {
	    return inb (addr & 0x3ff);
	} else if(addr >= 0x10000 + currprefs.catweasel_io && addr <= 0x10000 + currprefs.catweasel_io) {
	    return inb (addr & 0x3ff);
	} else if ((addr & 0x3ff) < 0x200 || (addr & 0x3ff) >= 0x400) {
	    write_log("catweasel_bget @%08.8X!\n",addr);
	}
    }
    return 0;
#endif
}

/****i* januae-catweasel/catweasel_do_bput
 *
 *   NAME
 *       catweasel_do_bput()
 *
 *   SYNOPSIS
 *       void catweasel_do_bput (uaecptr addr, uae_u32 b)
 *
 *   FUNCTION
 *       Write an 8-bit Catweasel register through the target-dependent address
 *       mapping.
 *
 *   NOTES
 *       AROS writes to currprefs.catweasel_io plus the requested offset; non-AROS
 *       paths use controller-dependent access restrictions.
 *
 ****************************************************************************/

void catweasel_do_bput (uaecptr addr, uae_u32 b)
{
#if defined(__AROS__)
	outb( b, currprefs.catweasel_io + addr );
	return;
#else
    if (cwc.type == CATWEASEL_TYPE_MK3) {
	if ((currprefs.catweasel_io & 3) == 0 && addr >= 0xc0 && addr <= 0xfc)
	    outb (b, currprefs.catweasel_io + addr);
    } else {
	if (addr >= currprefs.catweasel_io && addr <= currprefs.catweasel_io + 8) {
	    outb (b, addr & 0x3ff);
	} else if(addr >= 0x10000 + currprefs.catweasel_io && addr <= 0x10000 + currprefs.catweasel_io) {
	    outb (b, addr & 0x3ff);
	} else if ((addr & 0x3ff) < 0x200 || (addr & 0x3ff) >= 0x400) {
	    write_log("catweasel_bput @%08.8X=%02.2X!\n",addr,b);
	}
    }
#endif
}

/****i* januae-catweasel/catweasel_init
 *
 *   NAME
 *       catweasel_init()
 *
 *   SYNOPSIS
 *       int catweasel_init (void)
 *
 *   FUNCTION
 *       Initialize the configured Catweasel I/O backend and controller.
 *
 *   RESULT
 *       Returns zero when Catweasel I/O is not configured or when Linux I/O-port
 *       initialization fails; otherwise returns 1 after controller setup.
 *
 *   NOTES
 *       Conditional on CATWEASEL. Uses the configured I/O base and delegates
 *       controller configuration to catweasel_init_controller.
 *
 ****************************************************************************/

int catweasel_init (void)
{
	cwc.direct_access = cwc.can_joy = cwc.can_mouse = 0;
	cwc.sid[ 0 ] = cwc.sid[ 1 ] = 0;
    if (!currprefs.catweasel_io)
	return 0;
#if defined(__linux__)
    if (!ioport_init ())
	return 0;
#endif
#if defined(__AROS__)
    cwc.type = CATWEASEL_TYPE_MK4;
#else
    cwc.type = currprefs.catweasel_io >= 0x400 ? CATWEASEL_TYPE_MK3 : CATWEASEL_TYPE_MK1;
#endif
    cwc.iobase = currprefs.catweasel_io;
    catweasel_init_controller (&cwc);
    return 1;
}

/****i* januae-catweasel/catweasel_free
 *
 *   NAME
 *       catweasel_free()
 *
 *   SYNOPSIS
 *       void catweasel_free (void)
 *
 *   FUNCTION
 *       Release the Catweasel I/O backend state.
 *
 *   NOTES
 *       Does nothing when Catweasel I/O is not configured. Linux releases I/O-port
 *       access; the remaining routine does not free the controller structure.
 *
 ****************************************************************************/

void catweasel_free (void)
{
    if (!currprefs.catweasel_io)
	return;
#if defined(__linux__)
    ioport_free ();
#endif
}

#define LONGEST_TRACK 16000

static uae_u8 mfmbuf[LONGEST_TRACK * 4];
static uae_u8 tmpmfmbuffer[LONGEST_TRACK * 2];

/****i* januae-catweasel/bitshiftcompare
 *
 *   NAME
 *       bitshiftcompare()
 *
 *   SYNOPSIS
 *       static int bitshiftcompare(uae_u8 *src,int bit,int len,uae_u8 *comp)
 *
 *   FUNCTION
 *       Compare a bit-shifted source segment against a reference byte sequence.
 *
 *   RESULT
 *       Returns zero when all requested bytes match at the chosen bit shift; nonzero
 *       otherwise.
 *
 *   NOTES
 *       A match consisting entirely of zero or one bits is rejected with nonzero
 *       status. The caller supplies readable source and comparison spans; no bounds
 *       check is performed.
 *
 ****************************************************************************/

static int bitshiftcompare(uae_u8 *src,int bit,int len,uae_u8 *comp)
{
	uae_u8 b;
	int ones,zeros,len2;

	ones=zeros=0;
	len2=len;
	while(len--) {
		b = (comp[0] << bit) | (comp[1] >> (8 - bit));
		if(b != *src) return 1;
		if(b==0x00) zeros++;
		if(b==0xff) ones++;
		src++;
		comp++;
	}
	if(ones==len2||zeros==len2) return 1;
	return 0;
}

/****i* januae-catweasel/mergepieces
 *
 *   NAME
 *       mergepieces()
 *
 *   SYNOPSIS
 *       static uae_u8 *mergepieces(uae_u8 *start,int len,int bits,uae_u8 *sync)
 *
 *   FUNCTION
 *       Join aligned track segments using a discovered bit shift.
 *
 *   RESULT
 *       Returns a pointer to the merged working buffer.
 *
 *   NOTES
 *       Reuses static/internal track storage and writes the merged bytes there;
 *       callers must not free the result.
 *
 ****************************************************************************/

static uae_u8 *mergepieces(uae_u8 *start,int len,int bits,uae_u8 *sync)
{
	uae_u8 *dst=tmpmfmbuffer;
	uae_u8 b;
	int size;
	int shift;
	
	size=len-(sync-start);
	memcpy(dst,sync,size);
	dst+=size;
	b=start[len];
	b&=~(255>>bits);
	b|=start[0]>>bits;
	*dst++=b;
	shift=8-bits;
	while(start<=sync+2000) {
		*dst++=(start[0]<<shift)|(start[1]>>(8-shift));
		start++;
	}
	return tmpmfmbuffer;
}	
		
#define SCANOFFSET 1 /* scanning range in bytes, -SCANOFFSET to SCANOFFSET */
#define SCANOFFSET2 20
#define SCANLENGHT 200 /* scanning length in bytes */

/****i* januae-catweasel/scantrack
 *
 *   NAME
 *       scantrack()
 *
 *   SYNOPSIS
 *       static uae_u8* scantrack(uae_u8 *sync1,uae_u8 *sync2,int *trackbytes,int *trackbits)
 *
 *   FUNCTION
 *       Search two captured track windows for an unambiguous bit-level alignment.
 *
 *   RESULT
 *       Returns merged track data on success or NULL when no unique alignment is
 *       found.
 *
 *   NOTES
 *       Writes the detected track byte and residual-bit counts through the output
 *       pointers. Uses bitshiftcompare and mergepieces.
 *
 ****************************************************************************/

static uae_u8* scantrack(uae_u8 *sync1,uae_u8 *sync2,int *trackbytes,int *trackbits)
{
	int i,bits,bytes,matched;
	uae_u8 *sync2bak=sync2;
	
	sync1+=SCANOFFSET2;
	sync2+=SCANOFFSET2;
	while(sync1 < sync2bak - 2*SCANOFFSET - SCANOFFSET2 - SCANLENGHT) {
		matched=0x7fff;
		for(i=0;i<2*SCANOFFSET*8;i++) {
			bits=i&7;
			bytes=-SCANOFFSET+(i>>3);
			if(!bitshiftcompare(sync1,bits,SCANLENGHT,sync2+bytes)) {
				if(matched==0x7fff) {
					matched=i;
				} else {
					break;
				}
			}
		}
		if(matched!=0x7fff && i>=2*SCANOFFSET*8) {
			bits=matched&7;
			bytes=-SCANOFFSET+(matched>>3);
			*trackbytes=sync2+bytes-sync1;
			*trackbits=bits;
			return mergepieces(sync1,*trackbytes,*trackbits,sync2bak);
		}
		sync1++;
		sync2++;
	}
	return 0;
}	

static unsigned char threshtab[128];

/****i* januae-catweasel/codec_makethresh
 *
 *   NAME
 *       codec_makethresh()
 *
 *   SYNOPSIS
 *       static void codec_makethresh(unsigned int trycnt, const unsigned char *origt, unsigned char *t, int numthresh)
 *
 *   FUNCTION
 *       Perturb supplied pulse classification thresholds for a retry attempt.
 *
 *   NOTES
 *       Clamps the retry index to a fixed ten-entry offset table, then modifies
 *       numthresh output bytes. The input/output buffers must contain that many
 *       bytes.
 *
 ****************************************************************************/

static void codec_makethresh(unsigned int trycnt, const unsigned char *origt, unsigned char *t, int numthresh)
{
    static unsigned char tab[10] = { 0, 0, 0, 0, -1, -2, 1, 2, -1, 1 };

    if (trycnt >= sizeof (tab))
	trycnt = sizeof (tab) - 1;
    while(numthresh--)
	t[numthresh] = origt[numthresh] + tab[trycnt];
}

/****i* januae-catweasel/codec_init_threshtab
 *
 *   NAME
 *       codec_init_threshtab()
 *
 *   SYNOPSIS
 *       static void codec_init_threshtab(int trycnt, const unsigned char *origt)
 *
 *   FUNCTION
 *       Rebuild the pulse-duration classification lookup for the selected retry
 *       threshold.
 *
 *   NOTES
 *       Skips work when the two adjusted thresholds match the cached pair. Updates
 *       the global 128-entry threshold table and cached thresholds.
 *
 ****************************************************************************/

static void codec_init_threshtab(int trycnt, const unsigned char *origt)
{
    static unsigned char old_thresholds[2] = { 0, 0 };
    unsigned char t[2];
    int a, i;

    codec_makethresh(trycnt, origt, t, 2);

    if(*(unsigned short*)t == *(unsigned short*)old_thresholds)
	return;

    for(i=0,a=2; i<128; i++) {
	if(i == t[0] || i == t[1])
	    a++;
	threshtab[i] = a;
    }

    *(unsigned short*)&old_thresholds = *(unsigned short*)t;
}

/****i* januae-catweasel/CWSetCReg
 *
 *   NAME
 *       CWSetCReg()
 *
 *   SYNOPSIS
 *       static __inline__ void CWSetCReg(catweasel_contr *c, unsigned char clear, unsigned char set)
 *
 *   FUNCTION
 *       Update the cached control byte and write it to the Catweasel status/control
 *       register.
 *
 *   NOTES
 *       Bits selected by clear are removed; bits selected by set are added before
 *       the hardware write.
 *
 ****************************************************************************/

static __inline__ void CWSetCReg(catweasel_contr *c, unsigned char clear, unsigned char set)
{
    c->control_register = (c->control_register & ~clear) | set;
    outb(c->control_register, c->io_sr);
}

/****i* januae-catweasel/CWTriggerStep
 *
 *   NAME
 *       CWTriggerStep()
 *
 *   SYNOPSIS
 *       static void CWTriggerStep(catweasel_contr *c)
 *
 *   FUNCTION
 *       Pulse the configured floppy step-control line.
 *
 *   NOTES
 *       Clears and then sets crm_step through CWSetCReg.
 *
 ****************************************************************************/

static void CWTriggerStep(catweasel_contr *c)
{
    CWSetCReg(c, c->crm_step, 0);
    CWSetCReg(c, 0, c->crm_step);
}

/****i* januae-catweasel/catweasel_init_controller
 *
 *   NAME
 *       catweasel_init_controller()
 *
 *   SYNOPSIS
 *       void catweasel_init_controller(catweasel_contr *c)
 *
 *   FUNCTION
 *       Configure controller-specific register masks and home its available floppy
 *       drives.
 *
 *   NOTES
 *       Returns immediately with no I/O base or unsupported controller type. AROS Mk
 *       III/Mk IV probing uses the configured Catweasel base; drives are selected
 *       and stepped toward track zero up to 86 times.
 *
 ****************************************************************************/

void catweasel_init_controller(catweasel_contr *c)
{
    int i, j;

    if(!c->iobase)
	return;

#if defined(__AROS__)
    if ( c->type == CATWEASEL_TYPE_MK3 )
	cwfloppy_probe_mk3( currprefs.catweasel_io );
    if ( c->type == CATWEASEL_TYPE_MK4 )
	    cwfloppy_probe_mk4(currprefs.catweasel_io);
#endif

    switch(c->type) {
    case CATWEASEL_TYPE_MK1:
	c->crm_sel0 = 1 << 5;
	c->crm_sel1 = 1 << 4;
	c->crm_mot0 = 1 << 3;
	c->crm_mot1 = 1 << 7;
	c->crm_dir  = 1 << 1;
	c->crm_step = 1 << 0;
	c->srm_trk0 = 1 << 4;
	c->srm_dchg = 1 << 5;
	c->srm_writ = 1 << 1;
	c->io_sr    = c->iobase + 2;
	c->io_mem   = c->iobase;
	break;
    case CATWEASEL_TYPE_MK3:
    case CATWEASEL_TYPE_MK4:
	c->crm_sel0 = 1 << 2;
	c->crm_sel1 = 1 << 3;
	c->crm_mot0 = 1 << 1;
	c->crm_mot1 = 1 << 5;
	c->crm_dir  = 1 << 4;
	c->crm_step = 1 << 7;
	c->srm_trk0 = 1 << 2;
	c->srm_dchg = 1 << 5;
	c->srm_writ = 1 << 6;
	c->srm_dskready = 1 << 4;
	c->io_sr    = c->iobase + 0xe8;
	c->io_mem   = c->iobase + 0xe0;
	break;
    default:
	return;	
    }

    c->control_register = 255;

    /* select all drives, step inside */
    CWSetCReg(c, c->crm_dir | c->crm_sel0 | c->crm_sel1, 0);
    for(i=0;i<2;i++) {
	c->drives[i].number = i;
	c->drives[i].contr = c;
	c->drives[i].diskindrive = 0;
	
	/* select only the respective drive, step to track 0 */
	if(i == 0) {
	    CWSetCReg(c, c->crm_sel0, c->crm_dir | c->crm_sel1);
	} else {
	    CWSetCReg(c, c->crm_sel1, c->crm_dir | c->crm_sel0);
	}

	for(j = 0; j < 86 && (inb(c->io_sr) & c->srm_trk0); j++) {
	    CWTriggerStep(c);
	    sleep_millis(6);
	}
	
	if(j < 86) {
	    c->drives[i].type = 1;
	    c->drives[i].track = 0;
	} else {
	    c->drives[i].type = 0;
	}
    }
    c->drives[0].sel = c->crm_sel0;
    c->drives[0].mot = c->crm_mot0;
    c->drives[1].sel = c->crm_sel1;
    c->drives[1].mot = c->crm_mot1;
    CWSetCReg(c, 0, c->crm_sel0 | c->crm_sel1); /* deselect all drives */
}

/****i* januae-catweasel/catweasel_free_controller
 *
 *   NAME
 *       catweasel_free_controller()
 *
 *   SYNOPSIS
 *       void catweasel_free_controller(catweasel_contr *c)
 *
 *   FUNCTION
 *       Stop both floppy motors and deselect all Catweasel drives.
 *
 *   NOTES
 *       Returns without hardware I/O when the controller I/O base is zero.
 *
 ****************************************************************************/

void catweasel_free_controller(catweasel_contr *c)
{
    if(!c->iobase)
	return;

    /* all motors off, deselect all drives */
    CWSetCReg(c, 0, c->crm_mot0 | c->crm_mot1 | c->crm_sel0 | c->crm_sel1);
}

/****i* januae-catweasel/catweasel_set_motor
 *
 *   NAME
 *       catweasel_set_motor()
 *
 *   SYNOPSIS
 *       void catweasel_set_motor(catweasel_drive *d, int on)
 *
 *   FUNCTION
 *       Select a Catweasel drive, change its motor-control bit, then deselect it.
 *
 *   NOTES
 *       on requests the motor; controller and drive selectors must refer to
 *       initialized hardware.
 *
 ****************************************************************************/

void catweasel_set_motor(catweasel_drive *d, int on)
{
    CWSetCReg(d->contr, d->sel, 0);
    if (on)
	CWSetCReg(d->contr, d->mot, 0);
    else
	CWSetCReg(d->contr, 0, d->mot);
    CWSetCReg(d->contr, 0, d->sel);
}

/****i* januae-catweasel/catweasel_step
 *
 *   NAME
 *       catweasel_step()
 *
 *   SYNOPSIS
 *       int catweasel_step(catweasel_drive *d, int dir)
 *
 *   FUNCTION
 *       Issue one physical floppy-head step and update the cached track count.
 *
 *   RESULT
 *       Always returns 1.
 *
 *   NOTES
 *       Positive dir selects inward movement; zero or negative selects outward
 *       movement. The cached track increases or decreases by one without a hardware
 *       track-boundary check.
 *
 ****************************************************************************/

int catweasel_step(catweasel_drive *d, int dir)
{
    catweasel_contr *c = d->contr;
    CWSetCReg(c, d->sel, 0);
    if (dir > 0)
	CWSetCReg(c, c->crm_dir, 0);
    else
	CWSetCReg(c, 0, c->crm_dir);
    CWTriggerStep (c);
    CWSetCReg(c, 0, d->sel);
    d->track += dir > 0 ? 1 : -1;
    return 1;
}

/****i* januae-catweasel/catweasel_disk_changed
 *
 *   NAME
 *       catweasel_disk_changed()
 *
 *   SYNOPSIS
 *       int catweasel_disk_changed(catweasel_drive *d)
 *
 *   FUNCTION
 *       Read the active-low disk-change signal for a selected floppy drive.
 *
 *   RESULT
 *       Returns 1 when the change signal is asserted; 0 otherwise.
 *
 *   NOTES
 *       Selects the drive for the register read and deselects it afterward.
 *
 ****************************************************************************/

int catweasel_disk_changed(catweasel_drive *d)
{
    int ret;
    CWSetCReg(d->contr, d->sel, 0);
    ret = (inb(d->contr->io_sr) & d->contr->srm_dchg) ? 0 : 1;
    CWSetCReg(d->contr, 0, d->sel);
    return ret;
}

/****i* januae-catweasel/catweasel_diskready
 *
 *   NAME
 *       catweasel_diskready()
 *
 *   SYNOPSIS
 *       int catweasel_diskready(catweasel_drive *d)
 *
 *   FUNCTION
 *       Read the active-low disk-ready signal for a selected floppy drive.
 *
 *   RESULT
 *       Returns 1 when ready is asserted; 0 otherwise.
 *
 *   NOTES
 *       Selects and then deselects the drive.
 *
 ****************************************************************************/

int catweasel_diskready(catweasel_drive *d)
{
    int ret;
    CWSetCReg(d->contr, d->sel, 0);
    ret = (inb(d->contr->io_sr) & d->contr->srm_dskready) ? 0 : 1;
    CWSetCReg(d->contr, 0, d->sel);
    return ret;
}

/****i* januae-catweasel/catweasel_track0
 *
 *   NAME
 *       catweasel_track0()
 *
 *   SYNOPSIS
 *       int catweasel_track0(catweasel_drive *d)
 *
 *   FUNCTION
 *       Read the active-low track-zero signal and synchronize the cached track.
 *
 *   RESULT
 *       Returns 1 when track zero is asserted; 0 otherwise.
 *
 *   NOTES
 *       Sets d->track to zero when the sensor reports track zero.
 *
 ****************************************************************************/

int catweasel_track0(catweasel_drive *d)
{
    int ret;
    CWSetCReg(d->contr, d->sel, 0);
    ret = (inb(d->contr->io_sr) & d->contr->srm_trk0) ? 0 : 1;
    CWSetCReg(d->contr, 0, d->sel);
    if (ret)
	d->track = 0;
    return ret;
}

/****i* januae-catweasel/catweasel_write_protected
 *
 *   NAME
 *       catweasel_write_protected()
 *
 *   SYNOPSIS
 *       int catweasel_write_protected(catweasel_drive *d)
 *
 *   FUNCTION
 *       Read the active-low write-protect status for a selected drive.
 *
 *   RESULT
 *       Returns 1 when write-protect is asserted; 0 otherwise.
 *
 *   NOTES
 *       Selects and then deselects the drive.
 *
 ****************************************************************************/

int catweasel_write_protected(catweasel_drive *d)
{
    int ret;
    CWSetCReg(d->contr, d->sel, 0);
    ret = !(inb(d->contr->io_sr) & 8);
    CWSetCReg(d->contr, 0, d->sel);
    return ret;
}

/****i* januae-catweasel/catweasel_read_byte
 *
 *   NAME
 *       catweasel_read_byte()
 *
 *   SYNOPSIS
 *       static uae_u8 catweasel_read_byte(catweasel_drive *d)
 *
 *   FUNCTION
 *       Read one Catweasel track-memory byte and emit a printable debug character.
 *
 *   RESULT
 *       Returns the raw byte read from the track memory register.
 *
 *   NOTES
 *       Writes either the character or a dot to write_log for every byte read.
 *
 ****************************************************************************/

static uae_u8 catweasel_read_byte(catweasel_drive *d)
{
    uae_u8 tByte = inb(d->contr->io_mem);
    if ( tByte == 32 || (tByte >= 'A' && tByte <= 'z') )
	    write_log( "%c", tByte );
	else
		write_log( "." );
    return tByte;
}

static const unsigned char amiga_thresholds[] = { 0x22, 0x30 }; // 27, 38 for 5.25"

#define FLOPPY_WRITE_LEN 6250

#define MFMMASK 0x55555555

/****i* januae-catweasel/cw_getmfmlong
 *
 *   NAME
 *       cw_getmfmlong()
 *
 *   SYNOPSIS
 *       static uae_u32 cw_getmfmlong (uae_u16 * mbuf)
 *
 *   FUNCTION
 *       Extract MFM data bits from a pair of consecutive 16-bit words.
 *
 *   RESULT
 *       Returns the combined 32-bit value masked with 0x55555555.
 *
 *   NOTES
 *       The caller must provide at least two accessible words.
 *
 ****************************************************************************/

static uae_u32 cw_getmfmlong (uae_u16 * mbuf)
{
	return (uae_u32)(((*mbuf << 16) | *(mbuf + 1)) & MFMMASK);
}

/****i* januae-catweasel/drive_write_adf_amigados
 *
 *   NAME
 *       drive_write_adf_amigados()
 *
 *   SYNOPSIS
 *       static int drive_write_adf_amigados (uae_u16 *mbuf, uae_u16 *mend, uae_u8 *writebuffer, int track)
 *
 *   FUNCTION
 *       Decode one AmigaDOS track of MFM sectors into an ADF-style sector data
 *       buffer.
 *
 *   RESULT
 *       Returns zero after decoding 11 sectors, or a nonzero decoder error code.
 *
 *   NOTES
 *       Validates sync words, track/sector identity, header and data checksums. On
 *       error emits the code and sector map; input pointers and writebuffer must
 *       span the required track data.
 *
 ****************************************************************************/

static int drive_write_adf_amigados (uae_u16 *mbuf, uae_u16 *mend, uae_u8 *writebuffer, int track)
{
	int i, secwritten = 0;
	uae_u32 even, chksum, id, dlong;
	int odd;
	uae_u8 *secdata;
	uae_u8 secbuf[544];
	char sectable[22];
	int num_sectors = 11;
	int ec = 0;

	memset (sectable, 0, sizeof (sectable));
	mend -= (4 + 16 + 8 + 512);
	while (secwritten < num_sectors) {
		int trackoffs;

		do {
			while (*mbuf++ != 0x4489) {
			    if (mbuf >= mend) {
				ec = 1;
				goto err;
			    }
			}
		} while (*mbuf++ != 0x4489);

		odd = cw_getmfmlong (mbuf);
		even = cw_getmfmlong (mbuf + 2);
		mbuf += 4;
		id = (odd << 1) | even;

		trackoffs = (id & 0xff00) >> 8;
		if (trackoffs > 10) {
		    ec = 2;
		    goto err;
		}
		chksum = odd ^ even;
		for (i = 0; i < 4; i++) {
			odd = cw_getmfmlong (mbuf);
			even = cw_getmfmlong (mbuf + 8);
			mbuf += 2;

			dlong = (odd << 1) | even;
			if (dlong) {
			    ec = 6;
			    goto err;
			}
			chksum ^= odd ^ even;
		} /* could check here if the label is nonstandard */
		mbuf += 8;
		odd = cw_getmfmlong (mbuf);
		even = cw_getmfmlong (mbuf + 2);
		mbuf += 4;
		if (((odd << 1) | even) != chksum) {
		    ec = 3;
		    goto err;
		}
		odd = (id & 0x00ff0000) >> 16;
		if (odd != track) {
		    ec = 7;
		    goto err;
		}
		odd = cw_getmfmlong (mbuf);
		even = cw_getmfmlong (mbuf + 2);
		mbuf += 4;
		chksum = (odd << 1) | even;
		secdata = secbuf + 32;
		for (i = 0; i < 128; i++) {
			odd = cw_getmfmlong (mbuf);
			even = cw_getmfmlong (mbuf + 256);
			mbuf += 2;
			dlong = (odd << 1) | even;
			*secdata++ = dlong >> 24;
			*secdata++ = dlong >> 16;
			*secdata++ = dlong >> 8;
			*secdata++ = dlong;
			chksum ^= odd ^ even;
		}
		mbuf += 256;
		if (chksum) {
		    ec = 4;
		    goto err;
		}
		sectable[trackoffs] = 1;
		secwritten++;
		memcpy (writebuffer + trackoffs * 512, secbuf + 32, 512);
	}
	if (secwritten == 0 || secwritten < 0) {
	    ec = 5;
	    goto err;
	}
	return 0;
err:
	write_log ("mfm decode error %d. secwritten=%d\n", ec, secwritten);
	for (i = 0; i < num_sectors; i++)
	    write_log ("%d:%d ", i, sectable[i]);
	write_log ("\n");
	return ec;
}

#define FLOPPY_GAP_LEN 360

/****i* januae-catweasel/amigados_mfmcode
 *
 *   NAME
 *       amigados_mfmcode()
 *
 *   SYNOPSIS
 *       static int amigados_mfmcode (uae_u8 *src, uae_u16 *dst, int num_secs, int track)
 *
 *   FUNCTION
 *       Encode raw AmigaDOS sectors as MFM track words with sync and checksums.
 *
 *   RESULT
 *       Returns the generated track length in bits.
 *
 *   NOTES
 *       Each sector is 512 bytes; the destination includes the fixed gap, headers
 *       and encoded payload. Requires storage for num_secs sectors and the output
 *       track.
 *
 ****************************************************************************/

static int amigados_mfmcode (uae_u8 *src, uae_u16 *dst, int num_secs, int track)
{
	int sec;
	memset (dst, 0xaa, FLOPPY_GAP_LEN * 2);

	for (sec = 0; sec < num_secs; sec++) {
	    uae_u8 secbuf[544];
	    int i;
	    uae_u16 *mfmbuf = dst + 544 * sec + FLOPPY_GAP_LEN;
	    uae_u32 deven, dodd;
	    uae_u32 hck = 0, dck = 0;

	    secbuf[0] = secbuf[1] = 0x00;
	    secbuf[2] = secbuf[3] = 0xa1;
	    secbuf[4] = 0xff;
	    secbuf[5] = track;
	    secbuf[6] = sec;
	    secbuf[7] = num_secs - sec;

	    for (i = 8; i < 24; i++)
		secbuf[i] = 0;

	    mfmbuf[0] = mfmbuf[1] = 0xaaaa;
	    mfmbuf[2] = mfmbuf[3] = 0x4489;

	    memcpy (secbuf + 32, src + sec * 512, 512);
	    deven = ((secbuf[4] << 24) | (secbuf[5] << 16)
		     | (secbuf[6] << 8) | (secbuf[7]));
	    dodd = deven >> 1;
	    deven &= 0x55555555;
	    dodd &= 0x55555555;

	    mfmbuf[4] = dodd >> 16;
	    mfmbuf[5] = dodd;
	    mfmbuf[6] = deven >> 16;
	    mfmbuf[7] = deven;

	    for (i = 8; i < 48; i++)
		mfmbuf[i] = 0xaaaa;
	    for (i = 0; i < 512; i += 4) {
		deven = ((secbuf[i + 32] << 24) | (secbuf[i + 33] << 16)
			 | (secbuf[i + 34] << 8) | (secbuf[i + 35]));
		dodd = deven >> 1;
		deven &= 0x55555555;
		dodd &= 0x55555555;
		mfmbuf[(i >> 1) + 32] = dodd >> 16;
		mfmbuf[(i >> 1) + 33] = dodd;
		mfmbuf[(i >> 1) + 256 + 32] = deven >> 16;
		mfmbuf[(i >> 1) + 256 + 33] = deven;
	    }

	    for (i = 4; i < 24; i += 2)
		hck ^= (mfmbuf[i] << 16) | mfmbuf[i + 1];

	    deven = dodd = hck;
	    dodd >>= 1;
	    mfmbuf[24] = dodd >> 16;
	    mfmbuf[25] = dodd;
	    mfmbuf[26] = deven >> 16;
	    mfmbuf[27] = deven;

	    for (i = 32; i < 544; i += 2)
		dck ^= (mfmbuf[i] << 16) | mfmbuf[i + 1];

	    deven = dodd = dck;
	    dodd >>= 1;
	    mfmbuf[28] = dodd >> 16;
	    mfmbuf[29] = dodd;
	    mfmbuf[30] = deven >> 16;
	    mfmbuf[31] = deven;
	    mfmcode (mfmbuf + 4, 544 - 4);

	}
    return (num_secs * 544 + FLOPPY_GAP_LEN) * 2 * 8;
}

static uae_u16 amigamfmbuffer[LONGEST_TRACK];
static uae_u8 amigabuffer[512*22];

/* search and align to 0x4489 WORDSYNC markers */
/****i* januae-catweasel/isamigatrack
 *
 *   NAME
 *       isamigatrack()
 *
 *   SYNOPSIS
 *       static int isamigatrack(uae_u8 *mfmdata, uae_u8 *mfmdatae, uae_u16 *mfmdst, int track)
 *
 *   FUNCTION
 *       Align captured MFM bytes to 0x4489 sync words and regenerate a canonical
 *       AmigaDOS track.
 *
 *   RESULT
 *       Returns regenerated track length in bits, or zero when synchronization or
 *       sector decoding fails.
 *
 *   NOTES
 *       The destination is populated only after 11 valid sectors have been extracted
 *       and re-encoded.
 *
 ****************************************************************************/

static int isamigatrack(uae_u8 *mfmdata, uae_u8 *mfmdatae, uae_u16 *mfmdst, int track)
{
	uae_u16 *dst = amigamfmbuffer;
	int len;
	int shift, syncshift, sync,ret;
	uae_u32 l;
	uae_u16 w;

	sync = syncshift = shift = 0;
	len = (mfmdatae - mfmdata) * 8;
	if (len > LONGEST_TRACK * 8)
	    len = LONGEST_TRACK * 8;
	while (len--) {
		l = (mfmdata[0] << 16) | (mfmdata[1] << 8) | (mfmdata[2] << 0);
		w = l >> (8 - shift);
		if (w == 0x4489) {
			sync = 1;
			syncshift = 0;
		}
		if (sync) {
			if (syncshift == 0) *dst++ = w;
			syncshift ++;
			if (syncshift == 16) syncshift = 0;
		}
		shift++;
		if (shift == 8) {
			mfmdata++;
			shift = 0;
		}
	}
	if (sync) {
		ret=drive_write_adf_amigados (amigamfmbuffer, dst, amigabuffer, track);
		if(!ret)
		    return amigados_mfmcode (amigabuffer, mfmdst, 11, track);
		write_log ("decode error %d\n", ret);
	} else {
	    write_log ("decode error: no sync found\n");
	}
	return 0;
}



/****i* januae-catweasel/catweasel_fillmfm
 *
 *   NAME
 *       catweasel_fillmfm()
 *
 *   SYNOPSIS
 *       int catweasel_fillmfm (catweasel_drive *d, uae_u16 *mfm, int side, int clock, int rawmode)
 *
 *   FUNCTION
 *       Capture a floppy track and convert pulse durations into a UAE MFM track
 *       buffer.
 *
 *   RESULT
 *       Returns the number of resulting MFM bits, or zero when capture or track
 *       synchronization fails.
 *
 *   NOTES
 *       Requires a running drive motor; rawmode chooses stitched raw track windows,
 *       while the ordinary path decodes and re-encodes 11 AmigaDOS sectors. The
 *       clock parameter is ignored: capture always receives clock value 1.
 *
 ****************************************************************************/

int catweasel_fillmfm (catweasel_drive *d, uae_u16 *mfm, int side, int clock, int rawmode)
{
    int i, j, oldsync, syncs[10], synccnt, endcnt;
    uae_u32 tt1 = 0, tt2 = 0;
    uae_u8 *p1;
    int bytes = 0, bits = 0;
    static int lasttrack, trycnt;

	write_log( "Catweasel fillmfm.\n" );

    if (cwc.type == 0)
	return 0;
    if (d->contr->control_register & d->mot)
	return 0;
write_log( "Starting catweasel read....\n" );
    if (!catweasel_read (d, side, 1, rawmode))
	return 0;
write_log( "Done read.\n" );
    if(d->contr->type == CATWEASEL_TYPE_MK1) {
	inb(d->contr->iobase + 1);
	inb(d->contr->io_mem); /* ignore first byte */
    } else {
	outb(0, d->contr->iobase + 0xe4);
    }
    catweasel_read_byte (d);
    if (lasttrack == d->track)
	trycnt++;
    else
	trycnt = 0;
    lasttrack = d->track;
    codec_init_threshtab(trycnt, amiga_thresholds);
    i = 0; j = 0;
    synccnt = 0;
    oldsync = -1;
    endcnt = 0;
    while (j < LONGEST_TRACK * 4) {
	uae_u8 b = catweasel_read_byte (d);
	if (b >= 250) {
	    if (b == 255 - endcnt) {
		endcnt++;
		if (endcnt == 5)
		    break;
	    } else
		endcnt = 0;
	}
	if (rawmode) {
	    if (b & 0x80) {
		if (oldsync < j) {
		    syncs[synccnt++] = j;
		    oldsync = j + 300;
		}
	    }
	    if (synccnt >= 3 && j > oldsync)
		break;
	}
	b = threshtab[b & 0x7f];
	tt1 = (tt1 << b) + 1;
	tt2 += b;

	if (tt2 >= 16) {
	    tt2 -= 16;
	    mfmbuf[j++] = tt1 >> (tt2 + 8);
	    mfmbuf[j++] = tt1 >> tt2;
	}
	i++;
    }
    write_log ("cyl=%d, side=%d, length %d, syncs %d\n", d->track, side, j, synccnt);
    if (rawmode) {
	if (synccnt >= 3) {
	    p1 = scantrack (mfmbuf + syncs[1], mfmbuf + syncs[2], &bytes, &bits);
	    if (p1) {
		j = 0;
		for (i = 0; i < bytes + 2; i+=2) {
		    mfm[j++] = (p1[i] << 8) | p1[i + 1];
		}
	        return bytes * 8 + bits;
	    }
	}
    } else {
	return isamigatrack (mfmbuf, mfmbuf + j, mfm, d->track * 2 + side);
    }
    return 0;
}
	
/****i* januae-catweasel/catweasel_read
 *
 *   NAME
 *       catweasel_read()
 *
 *   SYNOPSIS
 *       int catweasel_read(catweasel_drive *d, int side, int clock, int rawmode)
 *
 *   FUNCTION
 *       Capture the selected side of a physical Catweasel floppy track into
 *       controller memory.
 *
 *   RESULT
 *       Returns 1 after the programmed acquisition sequence.
 *
 *   NOTES
 *       Selects the drive, sets side and clock for Mk I or newer controllers, waits
 *       550 ms in raw mode or 225 ms otherwise, writes an end marker and deselects
 *       the drive. It has no explicit read-failure return.
 *
 ****************************************************************************/

int catweasel_read(catweasel_drive *d, int side, int clock, int rawmode)
{
    int iobase = d->contr->iobase;

    CWSetCReg(d->contr, d->sel, 0);
    if(d->contr->type == CATWEASEL_TYPE_MK1) {
	CWSetCReg(d->contr, 1<<2, (!side)<<2); /* set disk side */

	inb(iobase+1); /* ra reset */
	outb(clock*128, iobase+3);

	inb(iobase+1);
	inb(iobase+0);
//	inb(iobase+0);
//	outb(0, iobase+3); /* don't store index pulse */

	inb(iobase+1);

	inb(iobase+7); /* start reading */
	sleep_millis(rawmode ? 550 : 225);
	outb(0, iobase+1); /* stop reading, don't reset RAM pointer */

	outb(128, iobase+0); /* add data end mark */
	outb(128, iobase+0);

	inb(iobase+1); /* Reset RAM pointer */
    } else {
	CWSetCReg(d->contr, 1<<6, (!side)<<6); /* set disk side */

	outb(0, iobase + 0xe4); /* Reset memory pointer */
	switch(clock) {
	case 0: /* 28MHz */
	    outb(128, iobase + 0xec);
	    break;
	case 1: /* 14MHz */
	    outb(0, iobase + 0xec);
	    break;
	}
	inb(iobase + 0xe0);
	inb(iobase + 0xe0);
	outb(0, iobase + 0xec); /* no IRQs, no MFM predecode */
	inb(iobase + 0xe0);
	outb(0, iobase + 0xec); /* don't store index pulse */

	outb(0, iobase + 0xe4); /* Reset memory pointer */
	inb(iobase + 0xf0); /* start reading */
	sleep_millis(rawmode ? 550 : 225);
	inb(iobase + 0xe4); /* stop reading, don't reset RAM pointer */

	outb(255, iobase + 0xe0); /* add data end mark */
	outb(254, iobase + 0xe0); /* add data end mark */
	outb(253, iobase + 0xe0); /* add data end mark */
	outb(252, iobase + 0xe0); /* add data end mark */
	outb(251, iobase + 0xe0); /* add data end mark */
	outb(0, iobase + 0xe4); /* Reset memory pointer */
    }
    CWSetCReg(d->contr, 0, d->sel);
    return 1;
}

#endif
