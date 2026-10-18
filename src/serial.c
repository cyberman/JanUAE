/************************************************************************ 
 *
 * UAE - The Un*x Amiga Emulator
 *
 *  Serial Line Emulation
 *
 * (c) 1996, 1997 Stefan Reinauer <stepan@linux.de>
 * (c) 1997 Christian Schmitt <schmitt@freiburg.linux.de>
 * (c) 2011 Krzysztof Smiechowicz
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

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "cia.h"
#include "serial.h"

#ifdef SERIAL_PORT
# ifndef WIN32
#  include "osdep/serial.h"
# else
#  include "od-win32/parser.h"
# endif
#endif

#define SERIALDEBUG	0 /* 0, 1, 2 3 */
#define SERIALHSDEBUG	0
#define MODEMTEST	0 /* 0 or 1 */

static int data_in_serdat;	/* new data written to SERDAT */
static int data_in_serdatr;	/* new data received */
static int data_in_sershift;	/* data transferred from SERDAT to shift register */
static uae_u16 serdatshift;	/* serial shift register */
static int ovrun;
static int dtr;
static int serial_period_hsyncs, serial_period_hsync_counter;
static int ninebit;
int serdev;

void serial_open(void);
void serial_close(void);

uae_u16 serper, serdat, serdatr;

static const int allowed_baudrates[] = {
    0, 110, 300, 600, 1200, 2400, 4800, 9600, 14400,
    19200, 31400, 38400, 57600, 115200, 128000, 256000, -1
};

/****i* januae-serial/SERPER ***********************************************
*
*   NAME
*       SERPER() -- Configure the emulated serial transmission period.
*
*   SYNOPSIS
*       void SERPER (uae_u16 w);
*
*   FUNCTION
*       Store the guest SERPER value, select eight- or nine-bit transmission
*       framing and choose the nearest supported host baud rate. Update the
*       horizontal-sync transmission interval and configure the host device
*       when serial forwarding is compiled in.
*
*   INPUTS
*       w - Guest serial-period register value, including the nine-bit mode
*       flag.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       An unchanged register value is ignored. Direct-serial mode uses its
*       own host-rate and timing adjustments.
*
****************************************************************************
*
*/

void SERPER (uae_u16 w)
{
    int baud = 0, i, per;
    static int warned;

    if (serper == w)  /* don't set baudrate if it's already ok */
	return;
    ninebit = 0;
    serper = w;

    if (w & 0x8000) {
	if (!warned) {
	    write_log( "SERIAL: program uses 9bit mode PC=%x\n", m68k_getpc (&regs) );
	    warned++;
	}
	ninebit = 1;
    }
    w &= 0x7fff;

    if (w < 13)
	w = 13;

    per = w;
    if (per == 0) per = 1;
    per = 3546895 / (per + 1);
    if (per <= 0) per = 1;
    i = 0;
    while (allowed_baudrates[i] >= 0 && per > allowed_baudrates[i] * 100 / 97)
	i++;
    baud = allowed_baudrates[i];

    serial_period_hsyncs = (((serper & 0x7fff) + 1) * 10) / maxhpos;
    if (serial_period_hsyncs <= 0)
	serial_period_hsyncs = 1;
    serial_period_hsync_counter = 0;

    write_log ("SERIAL: period=%d, baud=%d, hsyncs=%d PC=%x\n", w, baud, serial_period_hsyncs, m68k_getpc (&regs));

    if (ninebit)
	baud *= 2;
    if (currprefs.serial_direct) {
	if (baud != 31400 && baud < 115200)
	    baud = 115200;
	serial_period_hsyncs = 1;
    }
#ifdef SERIAL_PORT
    setbaud (baud);
#endif
}

/****i* januae-serial/dochar ***********************************************
*
*   NAME
*       dochar() -- Format a serial byte for diagnostic output.
*
*   SYNOPSIS
*       static char dochar (int v);
*
*   FUNCTION
*       Return printable ASCII characters unchanged and substitute a period
*       for bytes outside the printable range.
*
*   INPUTS
*       v - Serial value; only its low eight bits are used.
*
*   RESULT
*       The printable character, or a period for a control or non-ASCII
*       byte.
*
****************************************************************************
*
*/

static char dochar (int v)
{
    v &= 0xff;
    if (v >= 32 && v < 127) return (char)v;
    return '.';
}

/****i* januae-serial/checkreceive *****************************************
*
*   NAME
*       checkreceive() -- Transfer available host serial input to the guest
*       receiver.
*
*   SYNOPSIS
*       static void checkreceive (int mode);
*
*   FUNCTION
*       When serial forwarding is enabled, read host input into SERDATR,
*       assemble the two-byte framing used for nine-bit mode and request a
*       receive interrupt. Track receive overruns while a previous word
*       remains pending.
*
*   INPUTS
*       mode - Reserved argument; not examined by this implementation.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Without SERIAL_PORT support, the routine performs no receive
*       operation.
*
****************************************************************************
*
*/

static void checkreceive (int mode)
{
#ifdef SERIAL_PORT
    static uae_u32 lastchartime;
    static int ninebitdata;
    struct timeval tv;
    int recdata;

    if (!readseravail ())
	return;

    if (data_in_serdatr) {
	/* probably not needed but there may be programs that expect OVRUNs.. */
	gettimeofday (&tv, NULL);
	if (tv.tv_sec > lastchartime) {
	    ovrun = 1;
	    INTREQ (0x8000 | 0x0800);
	    while (readser (&recdata));
	    write_log ("SERIAL: overrun\n");
	}
	return;
    }

    if (ninebit) {
	for (;;) {
	    if (!readser (&recdata))
		return;
	    if (ninebitdata) {
		serdatr = (ninebitdata & 1) << 8;
		serdatr |= recdata;
		serdatr |= 0x200;
		ninebitdata = 0;
		break;
	    } else {
		ninebitdata = recdata;
		if ((ninebitdata & ~1) != 0xa8) {
		    write_log ("SERIAL: 9-bit serial emulation sync lost, %02.2X != %02.2X\n", ninebitdata & ~1, 0xa8);
		    ninebitdata = 0;
		    return;
		}
		continue;
	    }
	}
    } else {
	if (!readser (&recdata))
	    return;
	serdatr = recdata;
	serdatr |= 0x100;
    }
    gettimeofday (&tv, NULL);
    lastchartime = tv.tv_sec + 5;
    data_in_serdatr = 1;
    serial_check_irq ();
#if SERIALDEBUG > 2
    write_log ("SERIAL: received %02.2X (%c)\n", serdatr & 0xff, dochar (serdatr));
#endif
#endif
}

/****i* januae-serial/checksend ********************************************
*
*   NAME
*       checksend() -- Move pending guest serial output to the transmit
*       shift register.
*
*   SYNOPSIS
*       static void checksend (int mode);
*
*   FUNCTION
*       Test the host writer, retire completed shift-register data when
*       permitted and move a waiting SERDAT word into the shift register.
*       Forward the word to the host when available and signal
*       transmit-buffer availability.
*
*   INPUTS
*       mode - Zero permits retiring a completed shift-register transfer; a
*       nonzero value retains it.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Nine-bit host forwarding emits a framing byte before the data word.
*
****************************************************************************
*
*/

static void checksend (int mode)
{
    int bufstate;

#ifdef SERIAL_PORT
    bufstate = checkserwrite ();
#else
    bufstate = 1;
#endif

    if (!data_in_serdat && !data_in_sershift)
	return;

    if (data_in_sershift && mode == 0 && bufstate)
	data_in_sershift = 0;

    if (data_in_serdat && !data_in_sershift) {
	data_in_sershift = 1;
	serdatshift = serdat;
#ifdef SERIAL_PORT
	if (ninebit)
	    writeser (((serdatshift >> 8) & 1) | 0xa8);
	writeser (serdatshift);
#endif
	data_in_serdat = 0;
        INTREQ (0x8000 | 0x0001);
#if SERIALDEBUG > 2
	write_log ("SERIAL: send %04.4X (%c)\n", serdatshift, dochar (serdatshift));
#endif
    }
}

/****i* januae-serial/serial_hsynchandler **********************************
*
*   NAME
*       serial_hsynchandler() -- Service serial transfers at horizontal
*       synchronization.
*
*   SYNOPSIS
*       void serial_hsynchandler (void);
*
*   FUNCTION
*       Advance the serial interval counter and schedule receiver and
*       transmitter checks according to the current SERPER-derived
*       horizontal-sync periods.
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

void serial_hsynchandler (void)
{
    if (serial_period_hsyncs == 0)
	return;
    serial_period_hsync_counter++;
    if (serial_period_hsyncs == 1 || (serial_period_hsync_counter % (serial_period_hsyncs - 1)) == 0)
	checkreceive (0);
    if ((serial_period_hsync_counter % serial_period_hsyncs) == 0)
	checksend (0);
}

/* Not (fully) implemented yet: (on windows this work)
 *
 *  -  Something's wrong with the Interrupts.
 *     (NComm works, TERM does not. TERM switches to a
 *     blind mode after a connect and wait's for the end
 *     of an asynchronous read before switching blind
 *     mode off again. It never gets there on UAE :-< )
 *
 */

/****i* januae-serial/SERDAT ***********************************************
*
*   NAME
*       SERDAT() -- Write the emulated serial data register.
*
*   SYNOPSIS
*       void SERDAT (uae_u16 w);
*
*   FUNCTION
*       Store the guest transmit word and, when it contains a nonzero serial
*       frame, mark it pending for immediate transfer if the shift register
*       is available.
*
*   INPUTS
*       w - Guest serial data word.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       A word with no set bits in the low ten positions does not start
*       transmission.
*
****************************************************************************
*
*/

void SERDAT (uae_u16 w)
{
    serdat = w;

    if (!(w & 0x3ff)) {
#if SERIALDEBUG > 1
	write_log ("SERIAL: zero serial word written?! PC=%x\n", m68k_getpc (&regs));
#endif
	return;
    }

#if SERIALDEBUG > 1
    if (data_in_serdat) {
	write_log ("SERIAL: program wrote to SERDAT but old byte wasn't fetched yet\n");
    }
#endif

    data_in_serdat = 1;
    if (!data_in_sershift)
	checksend (1);

#if SERIALDEBUG > 2
    write_log ("SERIAL: wrote 0x%04x (%c) PC=%x\n", w, dochar (w), m68k_getpc (&regs));
#endif

    return;
}

/****i* januae-serial/SERDATR **********************************************
*
*   NAME
*       SERDATR() -- Read the emulated serial receive and status register.
*
*   SYNOPSIS
*       uae_u16 SERDATR (void);
*
*   FUNCTION
*       Combine the buffered receive value with transmit-ready, receive-full
*       and overrun status flags, then acknowledge the current receive data
*       and overrun state.
*
*   INPUTS
*       None.
*
*   RESULT
*       The assembled 16-bit receive-data and serial-status value.
*
****************************************************************************
*
*/

uae_u16 SERDATR (void)
{
    serdatr &= 0x03ff;
    if (!data_in_serdat)
	serdatr |= 0x2000;
    if (!data_in_sershift)
	serdatr |= 0x1000;
    if (data_in_serdatr)
	serdatr |= 0x4000;
    if (ovrun)
	serdatr |= 0x8000;
#if SERIALDEBUG > 2
    write_log( "SERIAL: read 0x%04.4x (%c) %x\n", serdatr, dochar (serdatr), m68k_getpc (&regs));
#endif
    ovrun = 0;
    data_in_serdatr = 0;
    return serdatr;
}

/****i* januae-serial/serial_check_irq *************************************
*
*   NAME
*       serial_check_irq() -- Request a serial receive interrupt when data
*       is pending.
*
*   SYNOPSIS
*       void serial_check_irq (void);
*
*   FUNCTION
*       Raise the serial receive interrupt source when the guest receive
*       register contains unconsumed input.
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

void serial_check_irq (void)
{
    if (data_in_serdatr)
	INTREQ_0 (0x8000 | 0x0800);
}

/****i* januae-serial/serial_dtr_on ****************************************
*
*   NAME
*       serial_dtr_on() -- Assert serial data-terminal-ready.
*
*   SYNOPSIS
*       void serial_dtr_on(void);
*
*   FUNCTION
*       Record DTR as asserted, open a demand-driven serial device when
*       configured and forward the DTR level to the host serial
*       implementation if compiled in.
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

void serial_dtr_on(void)
{
#if SERIALHSDEBUG > 0
    write_log ("SERIAL: DTR on\n");
#endif
    dtr = 1;
    if (currprefs.serial_demand)
	serial_open ();
#ifdef SERIAL_PORT
    setserstat (TIOCM_DTR, dtr);
#endif
}

/****i* januae-serial/serial_dtr_off ***************************************
*
*   NAME
*       serial_dtr_off() -- Deassert serial data-terminal-ready.
*
*   SYNOPSIS
*       void serial_dtr_off(void);
*
*   FUNCTION
*       Record DTR as deasserted, close a demand-driven host serial
*       connection when configured and forward the DTR level to the host
*       serial implementation if compiled in.
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

void serial_dtr_off(void)
{
#if SERIALHSDEBUG > 0
    write_log ("SERIAL: DTR off\n");
#endif
    dtr = 0;
#ifdef SERIAL_PORT
    if (currprefs.serial_demand)
	serial_close ();
    setserstat(TIOCM_DTR, dtr);
#endif
}

/****i* januae-serial/serial_flush_buffer **********************************
*
*   NAME
*       serial_flush_buffer() -- Provide the serial buffer-flush entry
*       point.
*
*   SYNOPSIS
*       void serial_flush_buffer (void);
*
*   FUNCTION
*       Return without modifying receive or transmit buffers.
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

void serial_flush_buffer (void)
{
}

static uae_u8 oldserbits;

/****i* januae-serial/serial_status_debug **********************************
*
*   NAME
*       serial_status_debug() -- Emit an optional serial modem-status trace.
*
*   SYNOPSIS
*       static void serial_status_debug (const char *s);
*
*   FUNCTION
*       When high-detail serial tracing is enabled at compile time, log the
*       current DTR, RTS, carrier-detect, clear-to-send and data-set-ready
*       levels.
*
*   INPUTS
*       s - Text prefix for a diagnostic status line.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Normal builds compile out the trace body.
*
****************************************************************************
*
*/

static void serial_status_debug (const char *s)
{
#if SERIALHSDEBUG > 1
    write_log ("%s: DTR=%d RTS=%d CD=%d CTS=%d DSR=%d\n", s,
	(oldserbits & 0x80) ? 0 : 1, (oldserbits & 0x40) ? 0 : 1,
	(oldserbits & 0x20) ? 0 : 1, (oldserbits & 0x10) ? 0 : 1, (oldserbits & 0x08) ? 0 : 1);
#endif
}

/****i* januae-serial/serial_readstatus ************************************
*
*   NAME
*       serial_readstatus() -- Read modem input status for the guest CIA
*       port.
*
*   SYNOPSIS
*       uae_u8 serial_readstatus (uae_u8 dir);
*
*   FUNCTION
*       Sample the host carrier-detect, clear-to-send and data-set-ready
*       signals when available, map their levels into the stored active-low
*       status bits and return the resulting status byte.
*
*   INPUTS
*       dir - CIA direction mask; not used by the current implementation.
*
*   RESULT
*       The stored modem-status byte.
*
*   NOTES
*       Without SERIAL_PORT support, the previously stored status is
*       returned.
*
****************************************************************************
*
*/

uae_u8 serial_readstatus (uae_u8 dir)
{
    int status = 0;
    uae_u8 serbits = oldserbits;

#ifdef SERIAL_PORT
    getserstat (&status);

    if (!(status & TIOCM_CAR)) {
	if (!(serbits & 0x20)) {
	    serbits |= 0x20;
#if SERIALHSDEBUG > 0
	    write_log ("SERIAL: CD off\n");
#endif
	}
    } else {
	if (serbits & 0x20) {
	    serbits &= ~0x20;
#if SERIALHSDEBUG > 0
	    write_log ("SERIAL: CD on\n");
#endif
	}
    }

    if (!(status & TIOCM_DSR)) {
	if (!(serbits & 0x08)) {
	    serbits |= 0x08;
#if SERIALHSDEBUG > 0
	    write_log ("SERIAL: DSR off\n");
#endif
	}
    } else {
	if (serbits & 0x08) {
	    serbits &= ~0x08;
#if SERIALHSDEBUG > 0
	    write_log ("SERIAL: DSR on\n");
#endif
	}
    }

    if (!(status & TIOCM_CTS)) {
	if (!(serbits & 0x10)) {
	    serbits |= 0x10;
#if SERIALHSDEBUG > 0
	    write_log ("SERIAL: CTS off\n");
#endif
	}
    } else {
	if (serbits & 0x10) {
	    serbits &= ~0x10;
#if SERIALHSDEBUG > 0
	    write_log ("SERIAL: CTS on\n");
#endif
	}
    }

    serbits &= 0x08 | 0x10 | 0x20;
    oldserbits &= ~(0x08 | 0x10 | 0x20);
    oldserbits |= serbits;
#endif

    serial_status_debug ("read");

    return oldserbits;
}

/****i* januae-serial/serial_writestatus ***********************************
*
*   NAME
*       serial_writestatus() -- Apply guest CIA modem-control outputs.
*
*   SYNOPSIS
*       uae_u8 serial_writestatus (uae_u8 newstate, uae_u8 dir);
*
*   FUNCTION
*       Compare guest port levels with the saved state and update DTR and,
*       when hardware flow control is disabled, RTS as directed by the
*       output mask. Retain the new output bits in the status byte.
*
*   INPUTS
*       newstate - Guest CIA port output levels; dir - Direction mask
*       identifying output-controlled modem signals.
*
*   RESULT
*       Updated stored modem-status byte.
*
*   NOTES
*       Physical modem-line writes are conditional on SERIAL_PORT support.
*
****************************************************************************
*
*/

uae_u8 serial_writestatus (uae_u8 newstate, uae_u8 dir)
{
#ifdef SERIAL_PORT
    if (((oldserbits ^ newstate) & 0x80) && (dir & 0x80)) {
	if (newstate & 0x80)
	    serial_dtr_off();
	else
	    serial_dtr_on();
    }

    if (!currprefs.serial_hwctsrts && (dir & 0x40)) {
	if ((oldserbits ^ newstate) & 0x40) {
	    if (newstate & 0x40) {
		setserstat (TIOCM_RTS, 0);
#if SERIALHSDEBUG > 0
		write_log ("SERIAL: RTS cleared\n");
#endif
	    } else {
	        setserstat (TIOCM_RTS, 1);
#if SERIALHSDEBUG > 0
		write_log ("SERIAL: RTS set\n");
#endif
	    }
	}
    }

#if 0 /* CIA io-pins can be read even when set to output.. */
    if ((newstate & 0x20) != (oldserbits & 0x20) && (dir & 0x20))
        write_log ("SERIAL: warning, program tries to use CD as an output!\n");
    if ((newstate & 0x10) != (oldserbits & 0x10) && (dir & 0x10))
        write_log ("SERIAL: warning, program tries to use CTS as an output!\n");
    if ((newstate & 0x08) != (oldserbits & 0x08) && (dir & 0x08))
        write_log ("SERIAL: warning, program tries to use DSR as an output!\n");
#endif

    if (((newstate ^ oldserbits) & 0x40) && !(dir & 0x40))
	write_log ("SERIAL: warning, program tries to use RTS as an input!\n");
    if (((newstate ^ oldserbits) & 0x80) && !(dir & 0x80))
	write_log ("SERIAL: warning, program tries to use DTR as an input!\n");

#endif

    oldserbits &= ~(0x80 | 0x40);
    newstate &= 0x80 | 0x40;
    oldserbits |= newstate;
    serial_status_debug ("write");

    return oldserbits;
}

/****i* januae-serial/serial_open ******************************************
*
*   NAME
*       serial_open() -- Open the configured host serial device.
*
*   SYNOPSIS
*       void serial_open(void);
*
*   FUNCTION
*       If serial forwarding is built in and the device is not already open,
*       attempt to open the configured device name and record a successful
*       connection.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Opening failure is logged and leaves the device marked closed.
*
****************************************************************************
*
*/

void serial_open(void)
{
#ifdef SERIAL_PORT
    if (serdev)
	return;

    if (!openser (currprefs.sername)) {
	write_log ("SERIAL: Could not open device %s\n", currprefs.sername);
	return;
    }
    serdev = 1;
#endif
}

/****i* januae-serial/serial_close *****************************************
*
*   NAME
*       serial_close() -- Close the host serial device.
*
*   SYNOPSIS
*       void serial_close (void);
*
*   FUNCTION
*       If serial forwarding is built in, close the host serial channel and
*       clear the connection flag.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The routine has no device operation without SERIAL_PORT support.
*
****************************************************************************
*
*/

void serial_close (void)
{
#ifdef SERIAL_PORT
    closeser ();
    serdev = 0;
#endif
}

/****i* januae-serial/serial_init ******************************************
*
*   NAME
*       serial_init() -- Initialize the serial connection policy.
*
*   SYNOPSIS
*       void serial_init (void);
*
*   FUNCTION
*       Open a configured host serial device immediately when serial
*       forwarding is enabled without demand-open mode. In the alternate
*       AROS serial compatibility configuration, enable the guest serial
*       preference and mark the device present.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The AROS_SERIAL_HACK path is compiled instead of the normal
*       device-opening path.
*
****************************************************************************
*
*/

void serial_init (void)
{
#if !defined(AROS_SERIAL_HACK)
#ifdef SERIAL_PORT
    if (!currprefs.use_serial)
	return;

    if (!currprefs.serial_demand)
	serial_open ();

#endif
#else
	/* patch to make AROS ROMs useable */
	currprefs.use_serial = 1;
	serdev = 1;
#endif
}

/****i* januae-serial/serial_exit ******************************************
*
*   NAME
*       serial_exit() -- Release the serial connection and modem state.
*
*   SYNOPSIS
*       void serial_exit (void);
*
*   FUNCTION
*       Close the serial backend when the compiled configuration requires
*       it, then clear the stored DTR state and modem-status bits.
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

void serial_exit (void)
{
#if defined SERIAL_PORT || defined AROS_SERIAL_HACK
    serial_close ();	/* serial_close can always be called because it	*/
#endif
    dtr = 0;		/* just closes *opened* filehandles which is ok	*/
    oldserbits = 0;	/* when exiting.				*/
}

/****i* januae-serial/serial_uartbreak *************************************
*
*   NAME
*       serial_uartbreak() -- Forward a guest UART break request.
*
*   SYNOPSIS
*       void serial_uartbreak (int v);
*
*   FUNCTION
*       Pass the requested break value to the host serial backend when
*       serial forwarding is compiled in.
*
*   INPUTS
*       v - Break-control argument passed unchanged to the host backend.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Without SERIAL_PORT support, no host break is generated.
*
****************************************************************************
*
*/

void serial_uartbreak (int v)
{
#ifdef SERIAL_PORT
    serialuartbreak (v);
#endif
}
