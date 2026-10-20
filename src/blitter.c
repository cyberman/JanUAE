 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Custom chip emulation
  *
  * (c) 1995 Bernd Schmidt, Alessandro Bissacco
  * (c) 2002 - 2005 Toni Wilen
  */

//#define BLITTER_DEBUG
//#define BLITTER_SLOWDOWNDEBUG 4

#define SPEEDUP

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "custom_private.h"
#include "events.h"
#include "newcpu.h"
#include "blitter.h"
#include "blit.h"
#include "savestate.h"

/* we must not change ce-mode while blitter is running.. */
static int blitter_cycle_exact;

uae_u16 bltcon0, bltcon1;
uae_u32 bltapt, bltbpt, bltcpt, bltdpt;

unsigned int blinea_shift;
static uae_u16 blinea, blineb;
static unsigned int blitline, blitfc, blitfill, blitife, blitsing, blitdesc;
static unsigned int blitonedot, blitsign;
static int blit_add;
static int blit_modadda, blit_modaddb, blit_modaddc, blit_modaddd;
static unsigned int blit_ch;

#ifdef BLITTER_DEBUG
static int blitter_dontdo;
static int blitter_delayed_debug;
#endif
#ifdef BLITTER_SLOWDOWNDEBUG
static int blitter_slowdowndebug;
#endif

struct bltinfo blt_info;

static uae_u8 blit_filltable[256][4][2];
uae_u32 blit_masktable[BLITTER_MAX_WORDS];

enum blitter_states bltstate;

static unsigned int blit_cyclecounter;
static unsigned int blit_maxcyclecounter;
static int blit_slowdown;
#ifdef BLITTER_DEBUG
static int blit_misscyclecounter;
#endif

#ifdef CPUEMU_6
extern uae_u8 cycle_line[];
#endif

static unsigned long blit_firstline_cycles;
static unsigned long blit_first_cycle;
static unsigned int blit_last_cycle, blit_dmacount, blit_dmacount2;

static unsigned int blit_nod;

static const uae_s8 *blit_diag;

static uae_u16 ddat1, ddat2;
static unsigned int ddat1use, ddat2use;

/*

Confirmed blitter �formation by Toni Wilen
(order of channels or position of idle cycles are not confirmed)

1=BLTCON0 channel mask
2=total cycles per blitted word
[3=steals all cycles if BLTNASTY=1 (always if A-channel is enabled. this is illogical..)]
4=total cycles per blitted word in fillmode
5=cycle diagram (first cycle)
6=main cycle diagram (ABCD=channels,-=idle cycle,x=idle cycle but bus allocated)

1 234 5    6

F 4*4*ABC- ABCD
E 4*4*ABC- ABC-
D 3*4 AB-  ABD
C 3*3 AB-  AB-
B 3*3*AC-  ACD
A 2*2*AC   AC
9 2*3 A-   AD
8 2*2 A-   A-
7 4 4 -BC- -BCD
6 4 4 -BC- -BC-
5 3 4 -B-  -BD
4 3 3 -B-  -B-
3 3 3 -C-  -CD
2 3 3 -C-  -C-
1 2 3 -D   -D
0 2 3 --   --

NOTES: (BLTNASTY=1)

- Blitter ALWAYS needs free bus cycle, even if it is running an "idle" cycle.
  Exception: possible extra fill mode idle cycle is "real" idle cycle.
  Can someone explain this? Why does idle cycles need bus cycles?
- Fill mode may add one extra real idle cycle.(depends on channel mask)
- All blits with channel A enabled use all available bus cycles
  (stops CPU accesses to Agnus bus if BLTNASTY=1) WTF? I did another test
  and this can't be true... Maybe I am becoming crazy..
- idle cycles (no A-channel enabled) are not "used" by blitter, they are freely
  available for CPU.

BLTNASTY=0 makes things even more interesting..

- even zero channel blits get slower if BLTNASTY=0 depending on the number of
  active bitplanes. ALSO "2 cycle" blits with one real cycle and one idle cycle
  have the exact same speed as zero channel blit in all situations -> only the
  total number of cycles count, number of active channels does not matter.

*/


/* -1 = idle cycle and allocate bus */

static const uae_s8 blit_cycle_diagram[][10] =
{
    { 0, 2, 0,0 },		/* 0 */
    { 0, 2, 0,4 },		/* 1 */
    { 0, 3, 0,3,0 },		/* 2 */
    { 2, 3, 0,3,4, 3,0 },	/* 3 */
    { 0, 3, 0,2,0 },		/* 4 */
    { 2, 3, 0,2,4, 2,0 },	/* 5 */
    { 0, 4, 0,2,3,0 },		/* 6 */
    { 3, 4, 0,2,3,4, 2,3,0 },	/* 7 */
    { 0, 2, 1,0 },		/* 8 */
    { 2, 2, 1,4, 1,0 },		/* 9 */
    { 0, 2, 1,3 },		/* A */
    { 3, 3, 1,3,4, 1,3,0 },	/* B */
    { 2, 3, 1,2,0, 1,2 },	/* C */
    { 3, 3, 1,2,4, 1,2,0 },	/* D */
    { 0, 4, 1,2,3,0 },		/* E */
    { 4, 4, 1,2,3,4, 1,2,3,0 }	/* F */
};

/* 5 = fill mode idle cycle ("real" idle cycle) */

static const uae_s8 blit_cycle_diagram_fill[][10] =
{
    { 0, 3, 0,5,0 },		/* 0 */
    { 0, 3, 3,5,4 },		/* 1 */
    { 0, 3, 0,3,0 },		/* 2 */
    { 2, 3, 3,5,4, 3,0 },	/* 3 */
    { 0, 3, 0,2,5 },		/* 4 */
    { 3, 4, 0,2,5,4, 2,0,0 },	/* 5 */
    { 0, 4, 2,3,5,0 },		/* 6 */
    { 3, 4, 2,3,5,4, 2,3,0 },	/* 7 */
    { 0, 2, 1,5 },		/* 8 */
    { 2, 3, 1,5,4, 1,0},	/* 9 */
    { 0, 2, 1,3 },		/* A */
    { 3, 3, 1,3,4, 1,3,0 },	/* B */
    { 2, 3, 1,2,5, 1,2 },	/* C */
    { 3, 4, 1,2,5,4, 1,2,0 },	/* D */
    { 0, 4, 1,2,3,0 },		/* E */
    { 4, 4, 1,2,3,4, 1,2,3,0 }	/* F */
};

/*

    line draw takes 4 cycles (-X-X)
    it also have real idle cycles and only 2 dma fetches
    (read from C, write to D, but see below)

    Oddities:

    - first word is written to address pointed by BLTDPT
      but all following writes go to address pointed by BLTCPT!
    - BLTDMOD is ignored by blitter (BLTCMOD is used)
    - state of D-channel enable bit does not matter!
    - disabling A-channel freezes the content of BPLAPT

*/

static const uae_s8 blit_cycle_diagram_line[] =
{
    0, 4, 0,3,0,4, 0,0,0,0,0,0,0,0,0,0  /* guessed */
};

static const uae_s8 blit_cycle_diagram_finald[] =
    { 0, 2, 0,4 };

/****i* januae-blitter/build_blitfilltable
 *
 *   NAME
 *       build_blitfilltable()
 *
 *   SYNOPSIS
 *       void build_blitfilltable (void)
 *
 *   FUNCTION
 *       Initialize the word masks and the 256-entry lookup table for inclusive and
 *       exclusive fill operations.
 *
 *   NOTES
 *       The table holds the filled byte and the carry state for each of four fill
 *       configurations.
 *
****************************************************************************/

void build_blitfilltable (void)
{
    unsigned int d, fillmask;
    unsigned int i;

    for (i = 0; i < BLITTER_MAX_WORDS; i++)
	blit_masktable[i] = 0xFFFF;

    for (d = 0; d < 256; d++) {
	for (i = 0; i < 4; i++) {
	    unsigned int fc = i & 1;
	    uae_u8 data = d;
	    for (fillmask = 1; fillmask != 0x100; fillmask <<= 1) {
		uae_u16 tmp = data;
		if (fc) {
		    if (i & 2)
			data |= fillmask;
		    else
			data ^= fillmask;
		}
		if (tmp & fillmask) fc = !fc;
	    }
	    blit_filltable[d][i][0] = data;
	    blit_filltable[d][i][1] = fc;
	}
    }
}

/****i* januae-blitter/blitter_dump
 *
 *   NAME
 *       blitter_dump()
 *
 *   SYNOPSIS
 *       static void blitter_dump (void)
 *
 *   FUNCTION
 *       Log the current blitter addresses, control words, input data and modulos.
 *
 *   NOTES
 *       Emits diagnostic output through write_log().
 *
****************************************************************************/

static void blitter_dump (void)
{
    write_log ("APT=%08.8X BPT=%08.8X CPT=%08.8X DPT=%08.8X\n", bltapt, bltbpt, bltcpt, bltdpt);
    write_log ("CON0=%04.4X CON1=%04.4X ADAT=%04.4X BDAT=%04.4X CDAT=%04.4X\n",
	bltcon0, bltcon1, blt_info.bltadat, blt_info.bltbdat, blt_info.bltcdat);
    write_log ("AFWM=%04.4X ALWM=%04.4X AMOD=%04.4X BMOD=%04.4X CMOD=%04.4X DMOD=%04.4X\n",
	blt_info.bltafwm, blt_info.bltalwm,
	blt_info.bltamod & 0xffff, blt_info.bltbmod & 0xffff, blt_info.bltcmod & 0xffff, blt_info.bltdmod & 0xffff);
}

/****i* januae-blitter/channel_state
 *
 *   NAME
 *       channel_state()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int channel_state (unsigned int cycles)
 *
 *   FUNCTION
 *       Select the DMA-channel code for a blitter cycle from the initial and
 *       repeating timing diagrams.
 *
 *   RESULT
 *       The channel or idle-cycle code at the given cycle offset.
 *
 *   NOTES
 *       The caller must establish a valid blit_diag and a nonzero repeating cycle
 *       count.
 *
****************************************************************************/

STATIC_INLINE int channel_state (unsigned int cycles)
{
    if ((int)cycles < blit_diag[0])
	return blit_diag[blit_diag[1] + 2 + cycles];
    return blit_diag[((cycles - blit_diag[0]) % blit_diag[1]) + 2];
}

#ifdef CPUEMU_6
/****i* januae-blitter/canblit
 *
 *   NAME
 *       canblit()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int canblit (unsigned int hpos)
 *
 *   FUNCTION
 *       Check whether the bus is available to the blitter at a horizontal position.
 *
 *   RESULT
 *       1 for an available bus slot, 0 for a busy or bitplane-DMA slot, or -1 for a
 *       refresh slot.
 *
 *   NOTES
 *       Available only when CPUEMU_6 is defined; indexes cycle_line by hpos.
 *
****************************************************************************/

STATIC_INLINE int canblit (unsigned int hpos)
{
    if (is_bitplane_dma (hpos))
	return 0;
    if (cycle_line[hpos] == 0)
	return 1;
    if (cycle_line[hpos] & CYCLE_REFRESH)
	return -1;
    return 0;
}
#endif

/****i* januae-blitter/blitter_done
 *
 *   NAME
 *       blitter_done()
 *
 *   SYNOPSIS
 *       static void blitter_done (void)
 *
 *   FUNCTION
 *       Finish the active blit, clear queued destination writes, raise the blitter
 *       interrupt and deactivate the scheduled event.
 *
 *   NOTES
 *       Also clears the CPU blitter-nasty special flag.
 *
****************************************************************************/

static void blitter_done (void)
{
    ddat1use = ddat2use = 0;
    bltstate = BLT_done;
    blitter_done_notify ();
    INTREQ(0x8040);
    eventtab[ev_blitter].active = 0;
    unset_special (&regs, SPCFLAG_BLTNASTY);
#ifdef BLITTER_DEBUG
    write_log ("vpos=%d, cycles %d, missed %d, total %d\n",
	vpos, blit_cyclecounter, blit_misscyclecounter, blit_cyclecounter + blit_misscyclecounter);
#endif
}

/****i* januae-blitter/blitter_dofast
 *
 *   NAME
 *       blitter_dofast()
 *
 *   SYNOPSIS
 *       static void blitter_dofast (void)
 *
 *   FUNCTION
 *       Execute a forward block blit using enabled A/B/C/D memory channels, shifts,
 *       masks and optional fill logic.
 *
 *   NOTES
 *       Advances enabled channel pointers and updates blt_info; uses a generated
 *       optimized minterm routine when available without fill.
 *
****************************************************************************/

static void blitter_dofast (void)
{
    int i,j;
    uaecptr bltadatptr = 0, bltbdatptr = 0, bltcdatptr = 0, bltddatptr = 0;
    uae_u8 mt = bltcon0 & 0xFF;

    blit_masktable[0] = blt_info.bltafwm;
    blit_masktable[blt_info.hblitsize - 1] &= blt_info.bltalwm;

    if (bltcon0 & 0x800) {
	bltadatptr = bltapt;
	bltapt += (blt_info.hblitsize*2 + blt_info.bltamod)*blt_info.vblitsize;
    }
    if (bltcon0 & 0x400) {
	bltbdatptr = bltbpt;
	bltbpt += (blt_info.hblitsize*2 + blt_info.bltbmod)*blt_info.vblitsize;
    }
    if (bltcon0 & 0x200) {
	bltcdatptr = bltcpt;
	bltcpt += (blt_info.hblitsize*2 + blt_info.bltcmod)*blt_info.vblitsize;
    }
    if (bltcon0 & 0x100) {
	bltddatptr = bltdpt;
	bltdpt += (blt_info.hblitsize*2 + blt_info.bltdmod)*blt_info.vblitsize;
    }

#ifdef SPEEDUP
    if (blitfunc_dofast[mt] && !blitfill) {
	(*blitfunc_dofast[mt])(bltadatptr, bltbdatptr, bltcdatptr, bltddatptr, &blt_info);
    } else
#endif
    {
	uae_u32 blitbhold = blt_info.bltbhold;
	uae_u32 preva = 0, prevb = 0;
	uaecptr dstp = 0;
	int dodst = 0;

	/*if (!blitfill) write_log ("minterm %x not present\n",mt); */
	for (j = 0; j < blt_info.vblitsize; j++) {
	    blitfc = !!(bltcon1 & 0x4);
	    for (i = 0; i < blt_info.hblitsize; i++) {
		uae_u32 bltadat, blitahold;
		uae_u16 bltbdat;
		if (bltadatptr) {
		    blt_info.bltadat = bltadat = chipmem_wget (bltadatptr);
		    bltadatptr += 2;
		} else
		    bltadat = blt_info.bltadat;
		bltadat &= blit_masktable[i];
		blitahold = (((uae_u32)preva << 16) | bltadat) >> blt_info.blitashift;
		preva = bltadat;

		if (bltbdatptr) {
		    blt_info.bltbdat = bltbdat = chipmem_wget (bltbdatptr);
		    bltbdatptr += 2;
		    blitbhold = (((uae_u32)prevb << 16) | bltbdat) >> blt_info.blitbshift;
		    prevb = bltbdat;
		}

		if (bltcdatptr) {
		    blt_info.bltcdat = chipmem_wget (bltcdatptr);
		    bltcdatptr += 2;
		}
		if (dodst) chipmem_wput (dstp, blt_info.bltddat);
		blt_info.bltddat = blit_func (blitahold, blitbhold, blt_info.bltcdat, mt) & 0xFFFF;
		if (blitfill) {
		    uae_u16 d = blt_info.bltddat;
		    int ifemode = blitife ? 2 : 0;
		    int fc1 = blit_filltable[d & 255][ifemode + blitfc][1];
		    blt_info.bltddat = (blit_filltable[d & 255][ifemode + blitfc][0]
			+ (blit_filltable[d >> 8][ifemode + fc1][0] << 8));
		    blitfc = blit_filltable[d >> 8][ifemode + fc1][1];
		}
		if (blt_info.bltddat)
		    blt_info.blitzero = 0;
		if (bltddatptr) {
		    dodst = 1;
		    dstp = bltddatptr;
		    bltddatptr += 2;
		}
	    }
	    if (bltadatptr) bltadatptr += blt_info.bltamod;
	    if (bltbdatptr) bltbdatptr += blt_info.bltbmod;
	    if (bltcdatptr) bltcdatptr += blt_info.bltcmod;
	    if (bltddatptr) bltddatptr += blt_info.bltdmod;
	}
	if (dodst) chipmem_wput (dstp, blt_info.bltddat);
	blt_info.bltbhold = blitbhold;
    }
    blit_masktable[0] = 0xFFFF;
    blit_masktable[blt_info.hblitsize - 1] = 0xFFFF;

    bltstate = BLT_done;
}

/****i* januae-blitter/blitter_dofast_desc
 *
 *   NAME
 *       blitter_dofast_desc()
 *
 *   SYNOPSIS
 *       static void blitter_dofast_desc (void)
 *
 *   FUNCTION
 *       Execute a descending block blit using reversed channel addresses and
 *       shifts.
 *
 *   NOTES
 *       Updates the blitter registers and destination memory; uses the descending
 *       optimized minterm routine when available without fill.
 *
****************************************************************************/

static void blitter_dofast_desc (void)
{
    int i,j;
    uaecptr bltadatptr = 0, bltbdatptr = 0, bltcdatptr = 0, bltddatptr = 0;
    uae_u8 mt = bltcon0 & 0xFF;

    blit_masktable[0] = blt_info.bltafwm;
    blit_masktable[blt_info.hblitsize - 1] &= blt_info.bltalwm;

    if (bltcon0 & 0x800) {
	bltadatptr = bltapt;
	bltapt -= (blt_info.hblitsize*2 + blt_info.bltamod)*blt_info.vblitsize;
    }
    if (bltcon0 & 0x400) {
	bltbdatptr = bltbpt;
	bltbpt -= (blt_info.hblitsize*2 + blt_info.bltbmod)*blt_info.vblitsize;
    }
    if (bltcon0 & 0x200) {
	bltcdatptr = bltcpt;
	bltcpt -= (blt_info.hblitsize*2 + blt_info.bltcmod)*blt_info.vblitsize;
    }
    if (bltcon0 & 0x100) {
	bltddatptr = bltdpt;
	bltdpt -= (blt_info.hblitsize*2 + blt_info.bltdmod)*blt_info.vblitsize;
    }
#ifdef SPEEDUP
    if (blitfunc_dofast_desc[mt] && !blitfill) {
	(*blitfunc_dofast_desc[mt])(bltadatptr, bltbdatptr, bltcdatptr, bltddatptr, &blt_info);
    } else
#endif
    {
	uae_u32 blitbhold = blt_info.bltbhold;
	uae_u32 preva = 0, prevb = 0;
	uaecptr dstp = 0;
	int dodst = 0;

	for (j = 0; j < blt_info.vblitsize; j++) {
	    blitfc = !!(bltcon1 & 0x4);
	    for (i = 0; i < blt_info.hblitsize; i++) {
		uae_u32 bltadat, blitahold;
		uae_u16 bltbdat;
		if (bltadatptr) {
		    bltadat = blt_info.bltadat = chipmem_wget (bltadatptr);
		    bltadatptr -= 2;
		} else
		    bltadat = blt_info.bltadat;
		bltadat &= blit_masktable[i];
		blitahold = (((uae_u32)bltadat << 16) | preva) >> blt_info.blitdownashift;
		preva = bltadat;

		if (bltbdatptr) {
		    blt_info.bltbdat = bltbdat = chipmem_wget (bltbdatptr);
		    bltbdatptr -= 2;
		    blitbhold = (((uae_u32)bltbdat << 16) | prevb) >> blt_info.blitdownbshift;
		    prevb = bltbdat;
		}

		if (bltcdatptr) {
		    blt_info.bltcdat = blt_info.bltbdat = chipmem_wget (bltcdatptr);
		    bltcdatptr -= 2;
		}
		if (dodst) chipmem_wput (dstp, blt_info.bltddat);
		blt_info.bltddat = blit_func (blitahold, blitbhold, blt_info.bltcdat, mt) & 0xFFFF;
		if (blitfill) {
		    uae_u16 d = blt_info.bltddat;
		    int ifemode = blitife ? 2 : 0;
		    int fc1 = blit_filltable[d & 255][ifemode + blitfc][1];
		    blt_info.bltddat = (blit_filltable[d & 255][ifemode + blitfc][0]
			+ (blit_filltable[d >> 8][ifemode + fc1][0] << 8));
		    blitfc = blit_filltable[d >> 8][ifemode + fc1][1];
		}
		if (blt_info.bltddat)
		    blt_info.blitzero = 0;
		if (bltddatptr) {
		    dstp = bltddatptr;
		    dodst = 1;
		    bltddatptr -= 2;
		}
	    }
	    if (bltadatptr) bltadatptr -= blt_info.bltamod;
	    if (bltbdatptr) bltbdatptr -= blt_info.bltbmod;
	    if (bltcdatptr) bltcdatptr -= blt_info.bltcmod;
	    if (bltddatptr) bltddatptr -= blt_info.bltdmod;
	}
	if (dodst) chipmem_wput (dstp, blt_info.bltddat);
	blt_info.bltbhold = blitbhold;
    }
    blit_masktable[0] = 0xFFFF;
    blit_masktable[blt_info.hblitsize - 1] = 0xFFFF;

    bltstate = BLT_done;
}

/****i* januae-blitter/blitter_read
 *
 *   NAME
 *       blitter_read()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void blitter_read (void)
 *
 *   FUNCTION
 *       Read the line-mode C-channel source word when enabled and advance the line
 *       state to work.
 *
 *   NOTES
 *       If blitter DMA is disabled while C is enabled, preserves the current state
 *       without reading.
 *
****************************************************************************/

STATIC_INLINE void blitter_read (void)
{
    if (bltcon0 & 0x200) {
	if (!dmaen(DMA_BLITTER))
	    return;
	blt_info.bltcdat = chipmem_bank.wget(bltcpt);
    }
    bltstate = BLT_work;
}

/****i* januae-blitter/blitter_write
 *
 *   NAME
 *       blitter_write()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void blitter_write (void)
 *
 *   FUNCTION
 *       Write the current line-mode destination word through chip memory when
 *       C-channel DMA is enabled.
 *
 *   NOTES
 *       Updates the zero flag and advances the line state only if the DMA gate
 *       permits progress.
 *
****************************************************************************/

STATIC_INLINE void blitter_write (void)
{
    if (blt_info.bltddat)
	blt_info.blitzero = 0;
    /* D-channel state has no effect on linedraw, but C must be enabled or nothing is drawn! */
    if (bltcon0 & 0x200) {
	if (!dmaen(DMA_BLITTER))
	    return;
	chipmem_bank.wput(bltdpt, blt_info.bltddat);
    }
    bltstate = BLT_next;
}

/****i* januae-blitter/blitter_line_incx
 *
 *   NAME
 *       blitter_line_incx()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void blitter_line_incx (void)
 *
 *   FUNCTION
 *       Advance the line-mode horizontal bit position and move the C pointer when
 *       it wraps.
 *
 *   NOTES
 *       Wraps the shift position at 16 bits.
 *
****************************************************************************/

STATIC_INLINE void blitter_line_incx (void)
{
    if (++blinea_shift == 16) {
	blinea_shift = 0;
	bltcpt += 2;
    }
}

/****i* januae-blitter/blitter_line_decx
 *
 *   NAME
 *       blitter_line_decx()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void blitter_line_decx (void)
 *
 *   FUNCTION
 *       Decrease the line-mode horizontal bit position and move the C pointer when
 *       it wraps.
 *
 *   NOTES
 *       Wraps the shift position to bit 15 after crossing zero.
 *
****************************************************************************/

STATIC_INLINE void blitter_line_decx (void)
{
    if (blinea_shift-- == 0) {
	blinea_shift = 15;
	bltcpt -= 2;
    }
}

/****i* januae-blitter/blitter_line_decy
 *
 *   NAME
 *       blitter_line_decy()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void blitter_line_decy (void)
 *
 *   FUNCTION
 *       Move the line-mode C pointer upward by the C-channel modulo.
 *
 *   NOTES
 *       Clears the one-dot flag.
 *
****************************************************************************/

STATIC_INLINE void blitter_line_decy (void)
{
    bltcpt -= blt_info.bltcmod;
    blitonedot = 0;
}

/****i* januae-blitter/blitter_line_incy
 *
 *   NAME
 *       blitter_line_incy()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void blitter_line_incy (void)
 *
 *   FUNCTION
 *       Move the line-mode C pointer downward by the C-channel modulo.
 *
 *   NOTES
 *       Clears the one-dot flag.
 *
****************************************************************************/

STATIC_INLINE void blitter_line_incy (void)
{
    bltcpt += blt_info.bltcmod;
    blitonedot = 0;
}

/****i* januae-blitter/blitter_line
 *
 *   NAME
 *       blitter_line()
 *
 *   SYNOPSIS
 *       static void blitter_line (void)
 *
 *   FUNCTION
 *       Evaluate the active line-mode minterm from the shifted A pattern, B sign
 *       pattern and C data.
 *
 *   NOTES
 *       Single-dot mode suppresses the A pattern after a dot has already been
 *       emitted.
 *
****************************************************************************/

static void blitter_line (void)
{
    uae_u16 blitahold = blinea >> blinea_shift;
    uae_u16 blitbhold = blineb & 1 ? 0xFFFF : 0;
    uae_u16 blitchold = blt_info.bltcdat;

    if (blitsing && blitonedot)
	blitahold = 0;
    blitonedot = 1;
    blt_info.bltddat = blit_func(blitahold, blitbhold, blitchold, bltcon0 & 0xFF);
}

/****i* januae-blitter/blitter_line_proc
 *
 *   NAME
 *       blitter_line_proc()
 *
 *   SYNOPSIS
 *       static void blitter_line_proc (void)
 *
 *   FUNCTION
 *       Advance the line drawing error accumulator and destination coordinates
 *       according to BLTCON1 direction bits.
 *
 *   NOTES
 *       Recomputes the error sign and enters the write state.
 *
****************************************************************************/

static void blitter_line_proc (void)
{
    if (!blitsign) {
	if (bltcon0 & 0x800)
	    bltapt += (uae_s16)blt_info.bltamod;
	if (bltcon1 & 0x10){
	    if (bltcon1 & 0x8)
		blitter_line_decy();
	    else
		blitter_line_incy();
	} else {
	    if (bltcon1 & 0x8)
		blitter_line_decx();
	    else
		blitter_line_incx();
	}
    } else {
	if (bltcon0 & 0x800)
	    bltapt += (uae_s16)blt_info.bltbmod;
    }
    if (bltcon1 & 0x10){
	if (bltcon1 & 0x4)
	    blitter_line_decx();
	else
	    blitter_line_incx();
    } else {
	if (bltcon1 & 0x4)
	    blitter_line_decy();
	else
	    blitter_line_incy();
    }
    blitsign = 0 > (uae_s16)bltapt;
    bltstate = BLT_write;
}

/****i* januae-blitter/blitter_nxline
 *
 *   NAME
 *       blitter_nxline()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void blitter_nxline (void)
 *
 *   FUNCTION
 *       Advance to the next line-mode step, rotating the B pattern and decrementing
 *       the remaining vertical count.
 *
 *   NOTES
 *       Returns the line state to the read phase.
 *
****************************************************************************/

STATIC_INLINE void blitter_nxline (void)
{
    blineb = (blineb << 1) | (blineb >> 15);
    blt_info.vblitsize--;
    bltstate = BLT_read;
}

#ifdef CPUEMU_6

static unsigned int blit_last_hpos;

static unsigned int blitter_cyclecounter;
static int blitter_hcounter1, blitter_hcounter2;
static int blitter_vcounter1, blitter_vcounter2;

/****i* januae-blitter/decide_blitter_line
 *
 *   NAME
 *       decide_blitter_line()
 *
 *   SYNOPSIS
 *       static void decide_blitter_line (unsigned int hpos)
 *
 *   FUNCTION
 *       Run cycle-exact line-mode DMA through the supplied horizontal position.
 *
 *   NOTES
 *       Processes only available DMA slots and finishes when the vertical line
 *       count reaches zero; requires CPUEMU_6.
 *
****************************************************************************/

static void decide_blitter_line (unsigned int hpos)
{
    hpos++;
    if (dmaen (DMA_BLITTER)) {
	while (blit_last_hpos < hpos) {
	    int c = channel_state (blit_cyclecounter);
	    for (;;) {
		if (c) {
		    if (!canblit (blit_last_hpos))
			break;
		}
		blit_cyclecounter++;
		if (c == 3) {
		    blitter_read();
		    cycle_line[blit_last_hpos] |= CYCLE_BLITTER;
		} else if (c == 4) {
		    if (ddat1use) {
			bltdpt = bltcpt;
		    }
		    ddat1use = 1;
		    blitter_line();
		    blitter_line_proc();
		    blitter_nxline();
		    blitter_write();
		    cycle_line[blit_last_hpos] |= CYCLE_BLITTER;
		    if (blt_info.vblitsize == 0) {
			bltdpt = bltcpt;
		        blitter_done();
		        return;
		    }
		}
	        break;
	    }
	    blit_last_hpos++;
	}
    } else {
	blit_last_hpos = hpos;
    }
    if (blit_last_hpos > maxhpos)
	blit_last_hpos = 0;
}

#endif

/****i* januae-blitter/actually_do_blit
 *
 *   NAME
 *       actually_do_blit()
 *
 *   SYNOPSIS
 *       static void actually_do_blit (void)
 *
 *   FUNCTION
 *       Execute the pending blit immediately, using line drawing or the
 *       forward/descending block transfer path.
 *
 *   NOTES
 *       Updates the blitter state but does not itself issue the completion
 *       interrupt.
 *
****************************************************************************/

static void actually_do_blit (void)
{
    if (blitline) {
	do {
	    blitter_read ();
	    blitter_line ();
	    blitter_line_proc ();
	    blitter_write ();
	    bltdpt = bltcpt;
	    blitter_nxline ();
	    if (blt_info.vblitsize == 0)
		bltstate = BLT_done;
	} while (bltstate != BLT_done);
    } else {
	if (blitdesc)
	    blitter_dofast_desc ();
	else
	    blitter_dofast ();
	bltstate = BLT_done;
    }
}

/****i* januae-blitter/blitter_handler
 *
 *   NAME
 *       blitter_handler()
 *
 *   SYNOPSIS
 *       void blitter_handler (void)
 *
 *   FUNCTION
 *       Complete or reschedule the event-driven blitter operation according to DMA
 *       availability and slowdown.
 *
 *   NOTES
 *       If DMA is disabled, reschedules the event; an immediate-blit fallback can
 *       force progress after repeated stalls.
 *
****************************************************************************/

void blitter_handler (void)
{
    static int blitter_stuck;
    if (!dmaen (DMA_BLITTER)) {
	eventtab[ev_blitter].active = 1;
	eventtab[ev_blitter].oldcycles = get_cycles ();
	eventtab[ev_blitter].evtime = 10 * CYCLE_UNIT + get_cycles (); /* wait a little */
	blitter_stuck++;
	if (blitter_stuck < 20000 || !currprefs.immediate_blits)
	    return; /* gotta come back later. */
	/* "free" blitter in immediate mode if it has been "stuck" ~3 frames
	 * fixes some JIT game incompatibilities
	 */
	write_log ("Blitter force-unstuck!\n");
    }
    blitter_stuck = 0;
    if (blit_slowdown > 0 && !currprefs.immediate_blits) {
	eventtab[ev_blitter].active = 1;
	eventtab[ev_blitter].oldcycles = get_cycles ();
	eventtab[ev_blitter].evtime = blit_slowdown * CYCLE_UNIT + get_cycles ();
	blit_slowdown = -1;
	return;
    }
#ifdef BLITTER_DEBUG
    if (!blitter_dontdo)
	actually_do_blit ();
    else
	bltstate = BLT_done;
#else
    actually_do_blit ();
#endif
    blitter_done ();
}

#ifdef CPUEMU_6

static uae_u32 preva, prevb;
/****i* januae-blitter/blitter_doblit
 *
 *   NAME
 *       blitter_doblit()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u16 blitter_doblit (void)
 *
 *   FUNCTION
 *       Compute one cycle-exact block-blitter destination word from current source
 *       words and the minterm.
 *
 *   RESULT
 *       The computed 16-bit destination word.
 *
 *   NOTES
 *       Applies first/last-word masks and optional fill; updates the zero flag and
 *       prior A word. Available only with CPUEMU_6.
 *
****************************************************************************/

STATIC_INLINE uae_u16 blitter_doblit (void)
{
    uae_u32 blitahold;
    uae_u16 bltadat, ddat;
    uae_u8 mt = bltcon0 & 0xFF;

    bltadat = blt_info.bltadat;
    if (blitter_hcounter1 == 0)
	bltadat &= blt_info.bltafwm;
    if (blitter_hcounter1 == blt_info.hblitsize - 1)
	bltadat &= blt_info.bltalwm;
    if (blitdesc)
	blitahold = (((uae_u32)bltadat << 16) | preva) >> blt_info.blitdownashift;
    else
	blitahold = (((uae_u32)preva << 16) | bltadat) >> blt_info.blitashift;
    preva = bltadat;

    ddat = blit_func (blitahold, blt_info.bltbhold, blt_info.bltcdat, mt) & 0xFFFF;

    if (bltcon1 & 0x18) {
	uae_u16 d = ddat;
	int ifemode = blitife ? 2 : 0;
	int fc1 = blit_filltable[d & 255][ifemode + blitfc][1];
	ddat = (blit_filltable[d & 255][ifemode + blitfc][0]
	    + (blit_filltable[d >> 8][ifemode + fc1][0] << 8));
	blitfc = blit_filltable[d >> 8][ifemode + fc1][1];
    }

    if (ddat)
	blt_info.blitzero = 0;

    return ddat;
}


/****i* januae-blitter/blitter_doddma
 *
 *   NAME
 *       blitter_doddma()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int blitter_doddma (void)
 *
 *   FUNCTION
 *       Write a pending destination word in the cycle-exact block-blitter path,
 *       when a write is due.
 *
 *   RESULT
 *       -1 for an immediate generated word, 1 or 2 for a queued word, or 0 when no
 *       write occurs.
 *
 *   NOTES
 *       Advances D-channel pointers and the destination line counters when a write
 *       occurs; requires CPUEMU_6.
 *
****************************************************************************/

STATIC_INLINE int blitter_doddma (void)
{
    int wd;
    uae_u16 d;

    wd = 0;
    if (blit_dmacount2 == 0) {
	d =  blitter_doblit ();
	wd = -1;
    } else if (ddat2use) {
	d = ddat2;
	ddat2use = 0;
	wd = 2;
    } else if (ddat1use) {
	d = ddat1;
	ddat1use = 0;
	wd = 1;
    }
    if (wd) {
	chipmem_wput (bltdpt, d);
	bltdpt += blit_add;
	blitter_hcounter2++;
	if (blitter_hcounter2 == blt_info.hblitsize) {
	    blitter_hcounter2 = 0;
	    bltdpt += blit_modaddd;
	    blitter_vcounter2++;
	    if (blitter_vcounter2 > blitter_vcounter1)
		blitter_vcounter1 = blitter_vcounter2;
	}
	if (blit_ch == 1)
	    blitter_hcounter1 = blitter_hcounter2;
    }
    return wd;
}

/****i* januae-blitter/blitter_dodma
 *
 *   NAME
 *       blitter_dodma()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void blitter_dodma (unsigned int ch)
 *
 *   FUNCTION
 *       Fetch the selected A, B or C DMA word and advance cycle-exact block-blitter
 *       counters.
 *
 *   NOTES
 *       Channel values 1, 2 and 3 select A, B and C; other values perform no source
 *       fetch. Available with CPUEMU_6.
 *
****************************************************************************/

STATIC_INLINE void blitter_dodma (unsigned int ch)
{

    switch (ch)
    {
	case 1:
	blt_info.bltadat = chipmem_wget (bltapt);
	bltapt += blit_add;
	break;
	case 2:
	blt_info.bltbdat = chipmem_wget (bltbpt);
	bltbpt += blit_add;
	if (blitdesc)
	    blt_info.bltbhold = (((uae_u32)blt_info.bltbdat << 16) | prevb) >> blt_info.blitdownbshift;
	else
	    blt_info.bltbhold = (((uae_u32)prevb << 16) | blt_info.bltbdat) >> blt_info.blitbshift;
	prevb = blt_info.bltbdat;
	break;
	case 3:
	blt_info.bltcdat = chipmem_wget (bltcpt);
	bltcpt += blit_add;
	break;
    }

    blitter_cyclecounter++;
    if (blitter_cyclecounter >= blit_dmacount2) {
	blitter_cyclecounter = 0;
	ddat2 = ddat1;
	ddat2use = ddat1use;
	ddat1use = 0;
	ddat1 = blitter_doblit ();
	if (bltcon0 & 0x100)
	    ddat1use = 1;
	blitter_hcounter1++;
	if (blitter_hcounter1 == blt_info.hblitsize) {
	    blitter_hcounter1 = 0;
	    if (bltcon0 & 0x800)
		bltapt += blit_modadda;
	    if (bltcon0 & 0x400)
		bltbpt += blit_modaddb;
	    if (bltcon0 & 0x200)
		bltcpt += blit_modaddc;
	    blitter_vcounter1++;
	    blitfc = !!(bltcon1 & 0x4);
	}
    }
}

/****i* januae-blitter/decide_blitter
 *
 *   NAME
 *       decide_blitter()
 *
 *   SYNOPSIS
 *       void decide_blitter (unsigned int hpos)
 *
 *   FUNCTION
 *       Advance the active blitter to the requested horizontal position in
 *       cycle-exact mode.
 *
 *   NOTES
 *       When CPUEMU_6 is not defined, this entry point is a no-op. Otherwise it
 *       allocates bus cycles and completes work when the DMA counters finish.
 *
****************************************************************************/

void decide_blitter (unsigned int hpos)
{
    if (bltstate == BLT_done)
	return;
#ifdef BLITTER_DEBUG
    if (blitter_delayed_debug) {
	blitter_delayed_debug = 0;
	blitter_dump ();
    }
#endif
    if (!blitter_cycle_exact)
	return;
    if (blitline) {
	decide_blitter_line (hpos);
	return;
    }
    hpos++;
    if (dmaen (DMA_BLITTER)) {
	while (blit_last_hpos < hpos) {
	    int c = channel_state (blit_cyclecounter);
#ifdef BLITTER_SLOWDOWNDEBUG
	    blitter_slowdowndebug--;
	    if (blitter_slowdowndebug < 0) {
		cycle_line[blit_last_hpos] |= CYCLE_BLITTER;
		blitter_slowdowndebug = BLITTER_SLOWDOWNDEBUG;
	    }
#endif
	    for (;;) {
		int v;

		if (c == 5) { /* real idle cycle */
		    blit_cyclecounter++;
		    break;
		}

		/* all cycles need free bus, even idle cycles (except fillmode idle) */
		v = canblit (blit_last_hpos);
		if (v < 0 && c == 0) {
		    blit_cyclecounter++;
		    break;
		}
		if (v <= 0) {
#ifdef BLITTER_DEBUG
		    blit_misscyclecounter++;
#endif
		    break;
		}

		if (c < 0) { /* no channel but bus still needs to be allocated.. */
		    cycle_line[blit_last_hpos] |= CYCLE_BLITTER;
		    blit_cyclecounter++;
		} else if (c == 4) {
		    if (blitter_doddma ()) {
			cycle_line[blit_last_hpos] |= CYCLE_BLITTER;
			blit_cyclecounter++;
		    }
		} else if (c) {
		    if (blitter_vcounter1 < blt_info.vblitsize) {
			cycle_line[blit_last_hpos] |= CYCLE_BLITTER;
			blitter_dodma (c);
		    }
		    blit_cyclecounter++;
		} else {
		    blit_cyclecounter++;
		    /* check if blit with zero channels has ended  */
		    if (blit_cyclecounter >= blit_maxcyclecounter) {
			blitter_done ();
			return;
		    }
		}
		if (blitter_vcounter1 >= blt_info.vblitsize && blitter_vcounter2 >= blt_info.vblitsize) {
		    if (!ddat1use && !ddat2use) {
			blitter_done ();
			return;
		    }
		    if (blit_diag != blit_cycle_diagram_finald) {
			blit_cyclecounter = 0;
			blit_diag = blit_cycle_diagram_finald;
		    }
		}
		break;
	    }
	    blit_last_hpos++;
	}
    } else {
	blit_last_hpos = hpos;
    }
    if (blit_last_hpos > maxhpos)
	blit_last_hpos = 0;
}
#else
void decide_blitter (unsigned int hpos) { }
#endif

/****i* januae-blitter/blitter_force_finish
 *
 *   NAME
 *       blitter_force_finish()
 *
 *   SYNOPSIS
 *       static void blitter_force_finish (void)
 *
 *   FUNCTION
 *       Finish an active blit before operations that cannot preserve its running
 *       state.
 *
 *   NOTES
 *       Temporarily enables blitter DMA, executes or advances the pending
 *       operation, then restores DMACON.
 *
****************************************************************************/

static void blitter_force_finish (void)
{
    uae_u16 odmacon;
    if (bltstate == BLT_done)
	return;
    if (bltstate != BLT_done) {
	 /* blitter is currently running
	  * force finish (no blitter state support yet)
	  */
	odmacon = dmacon;
	dmacon |= DMA_MASTER | DMA_BLITTER;
	write_log ("forcing blitter finish\n");
#ifdef CPUEMU_6
	if (currprefs.blitter_cycle_exact) {
	    int rounds = 10000;
	    while (bltstate != BLT_done && rounds > 0) {
		memset (cycle_line, 0, maxhpos);
		decide_blitter (maxhpos);
		rounds--;
	    }
	    if (rounds == 0)
		write_log ("blitter froze!?\n");
	} else
#endif
	    actually_do_blit ();

	blitter_done ();
	dmacon = odmacon;
    }
}

/****i* januae-blitter/blit_bltset
 *
 *   NAME
 *       blit_bltset()
 *
 *   SYNOPSIS
 *       static void blit_bltset (unsigned int con)
 *
 *   FUNCTION
 *       Decode the blitter control registers into line, fill, descending and
 *       channel timing state.
 *
 *   NOTES
 *       Updates shifts, DMA cycle counts and the active timing diagram. The con
 *       bitmask controls whether the fill carry is refreshed.
 *
****************************************************************************/

static void blit_bltset (unsigned int con)
{
    int i;

    blitline = bltcon1 & 1;
    blitfill = bltcon1 & 0x18;
    blitdesc = bltcon1 & 2;
    blit_ch = (bltcon0 & 0x0f00) >> 8;

    if (blitline) {
	if (blt_info.hblitsize != 2)
	    write_log ("weird hblitsize in linemode: %d vsize=%d PC%=%x\n",
		blt_info.hblitsize, blt_info.vblitsize, m68k_getpc (&regs));
	blit_diag = blit_cycle_diagram_line;
    } else {
	if (con & 2) {
	    blitfc = !!(bltcon1 & 0x4);
	    blitife = bltcon1 & 0x8;
	    if ((bltcon1 & 0x18) == 0x18) {
		/* Digital "Trash" demo does this; others too. Apparently, no
		* negative effects. */
		static int warn = 1;
		if (warn)
		    write_log ("warning: weird fill mode (further messages suppressed) PC=%x\n",
			m68k_getpc (&regs));
		warn = 0;
		blitife = 0;
	    }
	}
	if (blitfill && !blitdesc) {
	    static int warn = 1;
	    if (warn)
		write_log ("warning: blitter fill without desc (further messages suppressed) PC=%x\n",
		    m68k_getpc (&regs));
	    warn = 0;
	}
	blit_diag = blitfill ? blit_cycle_diagram_fill[blit_ch] : blit_cycle_diagram[blit_ch];
    }
    if ((bltcon1 & 0x80) && (currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	write_log("warning: BLTCON1 DOFF-bit set\n");

    ddat1use = ddat2use = 0;
    blit_dmacount = blit_dmacount2 = 0;
    blit_nod = 1;
    for (i = 0; i < blit_diag[1]; i++) {
	int v = blit_diag[2 + i];
	if (v)
	    blit_dmacount++;
	if (v > 0 && v < 4)
	    blit_dmacount2++;
	if (v == 4)
	    blit_nod = 0;
    }

    blt_info.blitashift = bltcon0 >> 12;
    blt_info.blitdownashift = 16 - blt_info.blitashift;
    blt_info.blitbshift = bltcon1 >> 12;
    blt_info.blitdownbshift = 16 - blt_info.blitbshift;
}

/****i* januae-blitter/blit_modset
 *
 *   NAME
 *       blit_modset()
 *
 *   SYNOPSIS
 *       static void blit_modset (void)
 *
 *   FUNCTION
 *       Set byte-address steps and channel modulos for forward or descending blits.
 *
 *   NOTES
 *       Uses a two-byte step and reverses its sign for descending mode.
 *
****************************************************************************/

static void blit_modset (void)
{
    int mult;

    blit_add = blitdesc ? -2 : 2;
    mult = blitdesc ? -1 : 1;
    blit_modadda = mult * blt_info.bltamod;
    blit_modaddb = mult * blt_info.bltbmod;
    blit_modaddc = mult * blt_info.bltcmod;
    blit_modaddd = mult * blt_info.bltdmod;
}

/****i* januae-blitter/reset_blit
 *
 *   NAME
 *       reset_blit()
 *
 *   SYNOPSIS
 *       void reset_blit (unsigned int bltcon)
 *
 *   FUNCTION
 *       Refresh control decoding and channel modulos while a blit is active.
 *
 *   NOTES
 *       A nonzero bltcon argument requests control redecoding; performs no work
 *       when the blitter is done.
 *
****************************************************************************/

void reset_blit (unsigned int bltcon)
{
    if (bltstate == BLT_done)
	return;
    if (bltcon)
	blit_bltset (bltcon);
    blit_modset ();
}

/****i* januae-blitter/do_blitter
 *
 *   NAME
 *       do_blitter()
 *
 *   SYNOPSIS
 *       void do_blitter (unsigned int hpos)
 *
 *   FUNCTION
 *       Start a new blitter operation using the current control registers, sizes
 *       and DMA configuration.
 *
 *   NOTES
 *       Initializes line or block state, sets the CPU nasty flag when requested and
 *       schedules an event unless cycle-exact execution is selected.
 *
****************************************************************************/

void do_blitter (unsigned int hpos)
{
    unsigned int cycles;
#ifdef BLITTER_DEBUG
    enum blitter_states oldstate = bltstate;
#endif

    blitter_cycle_exact = currprefs.blitter_cycle_exact;
    blt_info.blitzero = 1;
    bltstate = BLT_init;

#ifdef CPUEMU_6
    preva = 0;
    prevb = 0;
#endif

    blit_firstline_cycles = blit_first_cycle = get_cycles ();
#ifdef BLITTER_DEBUG
    blit_misscyclecounter = 0;
#endif
    blit_last_cycle = 0;
    blit_maxcyclecounter = 0;
    blit_cyclecounter = 0;
#ifdef CPUEMU_6
    blit_last_hpos = hpos;
#endif

    reset_blit (1|2);

    if (blitline) {
	blitsing = bltcon1 & 0x2;
	blinea = blt_info.bltadat;
	blineb = (blt_info.bltbdat >> blt_info.blitbshift) | (blt_info.bltbdat << (16 - blt_info.blitbshift));
	blitsign = bltcon1 & 0x40;
	blitonedot = 0;
	cycles = blt_info.vblitsize;
    } else {
	blit_firstline_cycles = blit_first_cycle + blit_diag[1] * blt_info.hblitsize * CYCLE_UNIT;
	cycles = blt_info.vblitsize * blt_info.hblitsize;
    }

#ifdef BLITTER_DEBUG
    blitter_dontdo = 0;
    if (1) {
	if (oldstate != BLT_done)
	    write_log ("blitter was already active!\n");
	write_log("blitstart: v=%03.3d h=%03.3d %dx%d %d (%d) d=%d f=%02.2X n=%d pc=%p l=%d dma=%d\n",
	    vpos, hpos, blt_info.hblitsize, blt_info.vblitsize, cycles, blit_ch,
	    blitdesc ? 1 : 0, blitfill,
	    dmaen(DMA_BLITPRI) ? 1 : 0, m68k_getpc (&regs), blitline, dmaen(DMA_BLITTER));
	blitter_dump ();
    }
#endif
    blit_slowdown = 0;

    unset_special (&regs, SPCFLAG_BLTNASTY);
    if (dmaen (DMA_BLITPRI))
	set_special (&regs, SPCFLAG_BLTNASTY);

    if (blt_info.vblitsize == 0 || (blitline && blt_info.hblitsize != 2)) {
	blitter_done ();
	return;
    }

    if (dmaen (DMA_BLITTER))
	bltstate = BLT_work;

    blit_maxcyclecounter = 0x7fffffff;

#ifdef CPUEMU_6
    if (currprefs.blitter_cycle_exact) {
	blitter_hcounter1 = blitter_hcounter2 = 0;
	blitter_vcounter1 = blitter_vcounter2 = 0;
	if (blit_nod)
	    blitter_vcounter2 = blt_info.vblitsize;
	if (blit_ch == 0)
	    blit_maxcyclecounter = blt_info.hblitsize * blt_info.vblitsize;
	return;
    }
#endif

    if (currprefs.immediate_blits)
	cycles = 1;

    eventtab[ev_blitter].active = 1;
    eventtab[ev_blitter].oldcycles = get_cycles ();
    eventtab[ev_blitter].evtime = cycles * blit_diag[1] * CYCLE_UNIT + get_cycles ();
    events_schedule();
}


/****i* januae-blitter/maybe_blit
 *
 *   NAME
 *       maybe_blit()
 *
 *   SYNOPSIS
 *       void maybe_blit (unsigned int hpos, int hack)
 *
 *   FUNCTION
 *       Synchronize a pending blit when the emulated CPU accesses blitter-dependent
 *       state.
 *
 *   NOTES
 *       In cycle-exact mode advances to hpos; otherwise invokes the event handler
 *       unless the optional first-line guard suppresses it.
 *
****************************************************************************/

void maybe_blit (unsigned int hpos, int hack)
{
    static int warned;

    if (bltstate == BLT_done)
	return;

    if (!warned && dmaen (DMA_BLITTER)) {
#ifndef BLITTER_DEBUG
	warned = 1;
#endif
	if (m68k_getpc (&regs) < 0xe0000 || m68k_getpc (&regs) >= 0x10000000)
	    write_log ("warning: Program does not wait for blitter %p vpos=%d tc=%d\n",
		m68k_getpc (&regs), vpos, blit_cyclecounter);
    }

    if (blitter_cycle_exact) {
	decide_blitter (hpos);
	goto end;
    }

    if (!eventtab[ev_blitter].active)
	write_log ("FOO!!?\n");
    if (hack == 1 && get_cycles() < blit_firstline_cycles)
	goto end;

    blitter_handler ();
end:;
#ifdef BLITTER_DEBUG
	blitter_delayed_debug = 1;
#endif
}

/****i* januae-blitter/blitnasty
 *
 *   NAME
 *       blitnasty()
 *
 *   SYNOPSIS
 *       unsigned int blitnasty (void)
 *
 *   FUNCTION
 *       Count blitter cycles that do not allocate a DMA channel since the last
 *       check.
 *
 *   RESULT
 *       The accumulated idle-cycle count; zero if the blitter is finished, disabled
 *       or has no such cycles.
 *
 *   NOTES
 *       Advances the internal cycle cursor to the current emulator cycle.
 *
****************************************************************************/

unsigned int blitnasty (void)
{
    unsigned int cycles, ccnt;
    if (bltstate == BLT_done)
	return 0;
    if (!dmaen(DMA_BLITTER))
	return 0;
    if ((int)blit_last_cycle >= blit_diag[0] && (int)blit_dmacount == blit_diag[1])
	return 0;
    cycles = (get_cycles () - blit_first_cycle) / CYCLE_UNIT;
    ccnt = 0;
    while (blit_last_cycle < cycles) {
	int c = channel_state (blit_last_cycle++);
	if (!c)
	    ccnt++;
    }
    return ccnt;
}

/****i* januae-blitter/blitter_slowdown
 *
 *   NAME
 *       blitter_slowdown()
 *
 *   SYNOPSIS
 *       void blitter_slowdown (int ddfstrt, int ddfstop, unsigned int totalcycles,
 *       unsigned int freecycles)
 *
 *   FUNCTION
 *       Estimate and accumulate non-cycle-exact blitter delay caused by competing
 *       bitplane DMA.
 *
 *   NOTES
 *       Updates the slowdown only with valid cycle geometry and when not drawing a
 *       line; uses cached geometry to recalculate delay.
 *
****************************************************************************/

void blitter_slowdown (int ddfstrt, int ddfstop, unsigned int totalcycles, unsigned int freecycles)
{
    static int oddfstrt, oddfstop;
    static unsigned ototal, ofree;
    static int slow;

    if (!totalcycles || ddfstrt < 0 || ddfstop < 0)
	return;
    if (ddfstrt != oddfstrt || ddfstop != oddfstop || totalcycles != ototal || ofree != freecycles) {
	int linecycles = ((ddfstop - ddfstrt + totalcycles - 1) / totalcycles) * totalcycles;
	int freelinecycles = ((ddfstop - ddfstrt + totalcycles - 1) / totalcycles) * freecycles;
	int dmacycles = (linecycles * blit_dmacount) / blit_diag[1];
	oddfstrt = ddfstrt;
	oddfstop = ddfstop;
	ototal = totalcycles;
	ofree = freecycles;
	slow = 0;
	if (dmacycles > freelinecycles)
	    slow = dmacycles - freelinecycles;
    }
    if (blit_slowdown < 0 || blitline)
	return;
    blit_slowdown += slow;
#ifdef BLITTER_DEBUG
    blit_misscyclecounter += slow;
#endif
}

#ifdef SAVESTATE

/****i* januae-blitter/restore_blitter
 *
 *   NAME
 *       restore_blitter()
 *
 *   SYNOPSIS
 *       const uae_u8 *restore_blitter (const uae_u8 *src)
 *
 *   FUNCTION
 *       Restore the limited blitter-active flag from the save-state stream.
 *
 *   RESULT
 *       The source pointer supplied by the caller.
 *
 *   NOTES
 *       Available only with SAVESTATE. Reads state via restore_u32(); it does not
 *       restore in-flight blitter transfers.
 *
****************************************************************************/

const uae_u8 *restore_blitter (const uae_u8 *src)
{
    uae_u32 flags = restore_u32 ();

    bltstate = (flags & 1) ? BLT_init : BLT_done;
    if (flags & 2) {
	write_log ("blitter was force-finished when this statefile was saved\n");
	write_log ("contact the author if restored program freezes\n");
    }
    return src;
}

/****i* januae-blitter/restore_blitter_finish
 *
 *   NAME
 *       restore_blitter_finish()
 *
 *   SYNOPSIS
 *       void restore_blitter_finish (void)
 *
 *   FUNCTION
 *       Restart a saved blit that was marked initialized but not executing.
 *
 *   NOTES
 *       Available only with SAVESTATE; restarts the operation with horizontal
 *       position zero.
 *
****************************************************************************/

void restore_blitter_finish (void)
{
    if (bltstate == BLT_init) {
	write_log ("blitter was started but DMA was inactive during save\n");
	do_blitter (0);
    }
}

/****i* januae-blitter/save_blitter
 *
 *   NAME
 *       save_blitter()
 *
 *   SYNOPSIS
 *       uae_u8 *save_blitter (uae_u32 *len, uae_u8 *dstptr)
 *
 *   FUNCTION
 *       Record the limited blitter completion state and forcibly finish running
 *       transfers before saving.
 *
 *   RESULT
 *       The caller-provided destination or an allocated fallback buffer.
 *
 *   NOTES
 *       Available only with SAVESTATE. Writes state through save_u32(); *len
 *       records the difference of local pointers, not the bytes written by
 *       save_u32().
 *
****************************************************************************/

uae_u8 *save_blitter (uae_u32 *len, uae_u8 *dstptr)
{
    uae_u8 *dstbak, *dst;
    int forced;

    forced = 0;
    if (bltstate != BLT_done && bltstate != BLT_init) {
	write_log ("blitter is active, forcing immediate finish\n");
	 /* blitter is active just now but we don't have blitter state support yet */
	blitter_force_finish ();
	forced = 2;
    }
    if (dstptr)
	dstbak = dst = dstptr;
    else
	dstbak = dst = malloc (16);
    save_u32 (((bltstate != BLT_done) ? 0 : 1) | forced);
    *len = dst - dstbak;
    return dstbak;
}

#endif /* SAVESTATE */
