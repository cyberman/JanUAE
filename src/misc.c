/*
 * A collection of ugly and random junk brought in from Win32
 * which desparately needs to be tidied up
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "events.h"
#include "uae.h"
#include "autoconf.h"
#include "traps.h"
#include "enforcer.h"
#include "p96.h"

static uae_u32 REGPARAM2 misc_demux (TrapContext *context)
{
//use the extern int (6 #13)
// d0 0=opensound      d1=unit d2=samplerate d3=blksize ret: sound frequency
// d0 1=closesound     d1=unit
// d0 2=writesamples   d1=unit a0=addr      write blksize samples to card
// d0 3=readsamples    d1=unit a0=addr      read samples from card ret: d0=samples read
      // make sure you have from amigaside blksize*4 mem alloced
      // d0=-1 no data available d0=-2 no recording open
          // d0 > 0 there are more blksize Data in the que
          // do the loop until d0 get 0
          // if d0 is greater than 200 bring a message
          // that show the user that data is lost
          // maximum blocksbuffered are 250 (8,5 sec)
// d0 4=writeinterrupt d1=unit  d0=0 no interrupt happen for this unit
          // d0=-2 no playing open

        //note units for now not support use only unit 0

// d0=10 get clipboard size      d0=size in bytes
// d0=11 get clipboard data      a0=clipboarddata
                                  //Note: a get clipboard size must do before
// d0=12 write clipboard data    a0=clipboarddata
// d0=13 setp96mouserate         d1=hz value
// d0=100 open dll               d1=dll name in windows name conventions
// d0=101 get dll function addr  d1=dllhandle a0 function/var name
// d0=102 exec dllcode           a0=addr of function (see 101)
// d0=103 close dll
// d0=104 screenlost
// d0=105 mem offset
// d0=106 16Bit byteswap
// d0=107 32Bit byteswap
// d0=108 free swap array
// d0=200 ahitweak               d1=offset for dsound position pointer

    int opcode = m68k_dreg (&context->regs, 0);

    switch (opcode) {
        int i, slen, t, todo, byte1, byte2;
        uae_u32 src, num_vars;
        static int cap_pos, clipsize;
        int cur_pos;

/*
 * AHI emulation support
 */
#ifdef AHI
	case 0:
	    cap_pos = 0;
	    sound_freq_ahi = m68k_dreg (&context->regs, 2);
	    amigablksize = m68k_dreg (&context->regs, 3);
	    sound_freq_ahi = ahi_open_sound();
	    uaevar.changenum--;
	    return sound_freq_ahi;
	case 1:
	    ahi_close_sound();
	    sound_freq_ahi = 0;
	    return 0;
	case 2:
	    addr=(char *)m68k_areg (&context->regs, 0);
	    for (i = 0; i < (amigablksize*4); i += 4) {
		ahisndbufpt[0] = get_long((unsigned int)addr + i);
		ahisndbufpt+=1;
		/*ahisndbufpt[0]=chipmem_bget((unsigned int)addr+i+2);
		  ahisndbufpt+=1;
		  ahisndbufpt[0]=chipmem_bget((unsigned int)addr+i+1);
		  ahisndbufpt+=1;
		  ahisndbufpt[0]=chipmem_bget((unsigned int)addr+i);
		  ahisndbufpt+=1;*/
	    }
	    ahi_finish_sound_buffer();
	    return amigablksize;
	case 3:
	    if (norec)
		return -1;
	    if (!ahi_on)
		return -2;
	    i = IDirectSoundCaptureBuffer_GetCurrentPosition (lpDSB2r, &t, &cur_pos);
	    t = amigablksize*4;

	    if (cap_pos <= cur_pos)
		todo = cur_pos - cap_pos;
	    else
		todo = cur_pos + (RECORDBUFFER * t) - cap_pos;
	    if (todo < t) {
	        //if no complete buffer ready exit
		return -1;
	    }
	    i = IDirectSoundCaptureBuffer_Lock (lpDSB2r, cap_pos, t, &pos1, &byte1, &pos2, &byte2, 0);

	    if ((cap_pos + t) < (t * RECORDBUFFER)) {
		cap_pos=cap_pos+t;
	    } else {
		cap_pos = 0;
	    }
	    addr= (char *) m68k_areg (&context->regs, 0);
	    sndbufrecpt= (unsigned int*) pos1;
	    t = t / 4;
	    for (i=0; i < t; i++) {
		put_long ((uae_u32) addr, sndbufrecpt[0]);
		addr += 4;
		sndbufrecpt += 1;
	    }
	    t = t * 4;
	    i = IDirectSoundCaptureBuffer_Unlock (lpDSB2r, pos1, byte1, pos2, byte2);
	    return (todo - t) / t;
	case 4:
	    if (!ahi_on)
		return -2;
	    i = intcount;
	    intcount = 0;
	    return i;
	case 5:
	    if (!ahi_on)
		return 0;
	    ahi_updatesound (1);
	    return 1;
#endif


/*
 * Hack for higher P96 mouse draw rate
 */
#ifdef PICASSO96
	case 13: {
	    extern int p96hack_vpos2;
	    extern int hack_vpos;
	    extern int p96refresh_active;
	    extern uae_u16 vtotal;
	    extern unsigned int new_beamcon0;
	    p96hack_vpos2 = 15625 / m68k_dreg (&context->regs, 1);
	    p96refresh_active = 1;
	    if (!picasso_on)
		return 0;
	    vtotal = p96hack_vpos2; // only do below if P96 screen is visible
	    new_beamcon0 |= 0x80;
	    hack_vpos = vtotal;
	    return 0;
	}
#endif

/*
 * Support for enforcer emulation
 */
#ifdef ENFORCER
	case 20:
	    return enforcer_enable ();

	case 21:
	    return enforcer_disable ();
#endif



	default:
	    return 0x12345678;	// Code for not supported function
    }
}


void misc_hsync_stuff (void)
{
    static int misc_demux_installed;

#ifdef AHI
    {
	static int count;
	if (ahi_on) {
	    count++;
	    //15625/count freebuffer check
	    if (count > 20) {
		ahi_updatesound (1);
		count = 0;
	    }
	}
    }
#endif

    if (!misc_demux_installed) {
	uaecptr a = here ();
	org (RTAREA_BASE + 0xFFC0);
	calltrap (deftrap (misc_demux));
	dw (0x4e75);// rts
	org (a);
	misc_demux_installed = 1;
    }
}
