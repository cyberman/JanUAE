/************************************************************************ 
 *
 * Custom chip emulation
 *
 * Copyright 1995-2002 Bernd Schmidt
 * Copyright 1995 Alessandro Bissacco
 * Copyright 2000-2004 Toni Wilen
 * Copyright 2009-2010 Oliver Brunner - aros<at>oliver-brunner.de
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

//#define CUSTOM_DEBUG
#define SPRITE_DEBUG 0
#define SPRITE_DEBUG_MINY 0x6a
#define SPRITE_DEBUG_MAXY 0x70
#define SPR0_HPOS 0x15
#define MAX_SPRITES 8
#define SPRITE_COLLISIONS
#define SPEEDUP

#include "sysconfig.h"
#include "sysdeps.h"

#include <ctype.h>
#include <assert.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <gtk/gtk.h>

#include "options.h"
#include "uae.h"
#include "events.h"
#include "memory.h"
#include "custom.h"
#include "custom_private.h"
#include "newcpu.h"
#include "cia.h"
#include "disk.h"
#include "blitter.h"
#include "xwin.h"
#include "audio.h"
#include "keybuf.h"
#include "serial.h"
#include "osemu.h"
#include "autoconf.h"
#include "traps.h"
#include "gui.h"
#include "p96.h"
#include "drawing.h"
#include "savestate.h"
#include "ar.h"
#ifdef AVIOUTPUT
#include "avioutput.h"
#endif
#include "debug.h"
#include "akiko.h"
#if defined(ENFORCER)
#include "enforcer.h"
#endif
#include "hrtimer.h"
#include "sleep.h"
#include "od-amiga/j.h"

/****i* januae-custom-fetch/uae_abort
 *
 *   NAME
 *       uae_abort()
 *
 *   SYNOPSIS
 *       static void uae_abort (const char *format,...)
 *
 *   FUNCTION
 *       Format a custom-chip diagnostic and display it through gui_message().
 *
 *   INPUTS
 *       format -- printf-style message format; additional arguments follow it
 *
 *   NOTES
 *       Uses a 1000-byte local buffer; it does not abort execution.
 *
 ****************************************************************************/

static void uae_abort (const char *format,...)
{
    va_list parms;
    char buffer[1000];

    va_start (parms, format);
#ifdef _WIN32
    _vsnprintf( buffer, sizeof (buffer) -1, format, parms );
#else
    vsnprintf( buffer, sizeof (buffer) -1, format, parms );
#endif
    va_end (parms);
    gui_message (buffer);
}


static uae_u16 last_custom_value;

static unsigned int total_skipped = 0;

STATIC_INLINE void sync_copper (unsigned int hpos);

/* Events */

unsigned int is_lastline;

static int rpt_did_reset;

volatile frame_time_t vsynctime, vsyncmintime;

#ifdef JIT
extern uae_u8* compiled_code;
#endif

unsigned int vpos;
int hack_vpos;
static uae_u16 lof;
static int next_lineno;
static enum nln_how nextline_how;
static int lof_changed = 0;
/* Stupid genlock-detection prevention hack.
 * We should stop calling vsync_handler() and
 * hstop_handler() completely but it is not
 * worth the trouble..
 */
static int vpos_previous, hpos_previous;
static int vpos_lpen, hpos_lpen;

static uae_u32 sprtaba[256],sprtabb[256];
static uae_u32 sprite_ab_merge[256];
/* Tables for collision detection.  */
static uae_u32 sprclx[16], clxmask[16];

/*
 * Hardware registers of all sorts.
 */

static int custom_wput_1 (unsigned int, uaecptr, uae_u32, int) REGPARAM;

uae_u16 intena,intreq;
uae_u16 dmacon;
uae_u16 adkcon; /* used by audio code */

static uae_u32 cop1lc,cop2lc,copcon;

unsigned int maxhpos = MAXHPOS_PAL;
unsigned int maxvpos = MAXVPOS_PAL;
unsigned int minfirstline = VBLANK_ENDLINE_PAL;
int vblank_hz = VBLANK_HZ_PAL, fake_vblank_hz, vblank_skip;
frame_time_t syncbase;
static int fmode;
unsigned int beamcon0, new_beamcon0;
uae_u16 vtotal = MAXVPOS_PAL, htotal = MAXHPOS_PAL;
static uae_u16 hsstop, hbstrt, hbstop, vsstop, vbstrt, vbstop, hsstrt, vsstrt, hcenter;
static int interlace_started;


/* This is but an educated guess. It seems to be correct, but this stuff
 * isn't documented well. */
struct sprite {
    uaecptr pt;
    unsigned int xpos;
    unsigned int vstart;
    unsigned int vstop;
    int armed;
    int dmastate;
    int dmacycle;
};

static struct sprite spr[MAX_SPRITES];

static unsigned int sprite_vblank_endline = VBLANK_SPRITE_PAL;

static unsigned int sprctl[MAX_SPRITES], sprpos[MAX_SPRITES];
#ifdef AGA
static uae_u16 sprdata[MAX_SPRITES][4], sprdatb[MAX_SPRITES][4];
#else
static uae_u16 sprdata[MAX_SPRITES][1], sprdatb[MAX_SPRITES][1];
#endif
static unsigned int last_sprite_point;
static int nr_armed;
static unsigned int sprite_width;
static unsigned int sprres, sprite_buffer_res;

#ifdef CPUEMU_6
uae_u8 cycle_line[256];
#endif

static uae_u32 bpl1dat;
static uae_s16 bpl1mod, bpl2mod;

static uaecptr bplpt[8];
uae_u8 *real_bplpt[8];
/* Used as a debugging aid, to offset any bitplane temporarily.  */
int bpl_off[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

/*static int blitcount[256];  blitter debug */

static struct color_entry current_colors;
static unsigned int bplcon0, bplcon1, bplcon2, bplcon3, bplcon4;
/*static*/ unsigned int diwstrt, diwstop, diwhigh;
static int diwhigh_written;
static unsigned int ddfstrt, ddfstop, ddfstrt_old_hpos, ddfstrt_old_vpos;
static unsigned int ddf_change;

/* The display and data fetch windows */

enum diw_states
{
    DIW_waiting_start, DIW_waiting_stop
};

unsigned int plffirstline, plflastline;
unsigned int plfstrt;
unsigned int plfstop;
static int last_diw_pix_hpos, last_ddf_pix_hpos;
static int last_decide_line_hpos, last_sprite_decide_line_hpos;
static int last_fetch_hpos;
static unsigned int last_sprite_hpos;
int diwfirstword, diwlastword;
static enum diw_states diwstate, hdiwstate, ddfstate;

/* Sprite collisions */
static unsigned int clxdat, clxcon, clxcon2, clxcon_bpl_enable, clxcon_bpl_match;

enum copper_states {
    COP_stop,
    COP_read1_in2,
    COP_read1_wr_in4,
    COP_read1_wr_in2,
    COP_read1,
    COP_read2_wr_in2,
    COP_read2,
    COP_bltwait,
    COP_wait_in4,
    COP_wait_in2,
    COP_skip_in4,
    COP_skip_in2,
    COP_wait1,
    COP_wait,
    COP_skip1,
    COP_strobe_delay
};

struct copper {
    /* The current instruction words.  */
    unsigned int i1, i2;
    unsigned int saved_i1, saved_i2;
    enum copper_states state, state_prev;
    /* Instruction pointer.  */
    uaecptr ip, saved_ip;
    unsigned int hpos;
    unsigned int vpos;
    unsigned int ignore_next;
    unsigned int vcmp;
    unsigned int hcmp;

    int strobe; /* COPJMP1 / COPJMP2 accessed */
    int last_write, last_write_hpos;
};

static struct copper cop_state;
static int copper_enabled_thisline;
static int cop_min_waittime;

/*
 * Statistics
 */
unsigned int frametime;
frame_time_t lastframetime;
unsigned int timeframes;
frame_time_t idletime;
unsigned long int hsync_counter;

int bogusframe;

/* Recording of custom chip register changes.  */
static int current_change_set;

#ifdef OS_WITHOUT_MEMORY_MANAGEMENT
/* sam: Those arrays uses around 7Mb of BSS... That seems  */
/* too much for AmigaDOS (uae crashes as soon as one loads */
/* it. So I use a different strategy here (realloc the     */
/* arrays when needed. That strategy might be usefull for  */
/* computer with low memory.                               */
struct sprite_entry *sprite_entries[2];
struct color_change *color_changes[2];
#define DEFAULT_MAX_SPRITE_ENTRY	400
#define DEFAULT_MAX_COLOR_CHANGE	400
static int max_sprite_entry;
static int delta_sprite_entry;
static int max_color_change;
static int delta_color_change;
#else
struct sprite_entry sprite_entries[2][MAX_SPR_PIXELS / 16];
struct color_change color_changes[2][MAX_REG_CHANGE];
#endif

struct decision line_decisions[2 * (MAXVPOS + 1) + 1];
struct draw_info line_drawinfo[2][2 * (MAXVPOS + 1) + 1];
struct color_entry color_tables[2][(MAXVPOS + 1) * 2];

static int next_sprite_entry = 0;
static int prev_next_sprite_entry;
static int next_sprite_forced = 1;

struct sprite_entry *curr_sprite_entries, *prev_sprite_entries;
struct color_change *curr_color_changes, *prev_color_changes;
struct draw_info *curr_drawinfo, *prev_drawinfo;
struct color_entry *curr_color_tables, *prev_color_tables;

static int next_color_change;
static int next_color_entry, remembered_color_entry;
static int color_src_match, color_dest_match, color_compare_result;

static uae_u32 thisline_changed;

#ifdef SMART_UPDATE
#define MARK_LINE_CHANGED do { thisline_changed = 1; } while (0)
#else
#define MARK_LINE_CHANGED do { ; } while (0)
#endif

static struct decision thisline_decision;
static unsigned int passed_plfstop;
static unsigned int fetch_cycle;
static int fetch_modulo_cycle;

enum fetchstate {
    fetch_not_started,
    fetch_started,
    fetch_was_plane0
} fetch_state;

/*
 * helper functions
 */

/****i* januae-custom-fetch/nodraw
 *
 *   NAME
 *       nodraw()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int nodraw (void)
 *
 *   FUNCTION
 *       Determine whether this frame may skip ordinary drawing in non-cycle-exact
 *       CPU mode.
 *
 *   RESULT
 *       Returns nonzero when an ordinary frame can be skipped; otherwise zero.
 *
 *   NOTES
 *       Returns true only when CPU cycle-exact mode is off and framecnt is
 *       nonzero.
 *
 ****************************************************************************/

STATIC_INLINE int nodraw (void)
{
    return !currprefs.cpu_cycle_exact && framecnt != 0;
}

/****i* januae-custom-fetch/get_copper_address
 *
 *   NAME
 *       get_copper_address()
 *
 *   SYNOPSIS
 *       uae_u32 get_copper_address (int copno)
 *
 *   FUNCTION
 *       Select the Copper 1 pointer, Copper 2 pointer, or the current Copper
 *       instruction pointer.
 *
 *   INPUTS
 *       copno -- Copper pointer selector (1, 2, or -1)
 *
 *   RESULT
 *       Returns the selected Copper pointer or zero for an unknown selector.
 *
 *   NOTES
 *       copno=1 selects COP1LC; 2 selects COP2LC; -1 selects cop_state.ip; other
 *       values return zero.
 *
 ****************************************************************************/

uae_u32 get_copper_address (int copno)
{
    switch (copno) {
    case 1: return cop1lc;
    case 2: return cop2lc;
    case -1: return cop_state.ip;
    default: return 0;
    }
}


/****i* januae-custom-fetch/reset_frame_rate_hack
 *
 *   NAME
 *       reset_frame_rate_hack()
 *
 *   SYNOPSIS
 *       void reset_frame_rate_hack (void)
 *
 *   FUNCTION
 *       Reinitialize the fast-CPU frame pacing deadline and per-frame reset flags.
 *
 *   NOTES
 *       Does nothing unless currprefs.m68k_speed is -1; logs the reset.
 *
 ****************************************************************************/

void reset_frame_rate_hack (void)
{
    if (currprefs.m68k_speed != -1)
	return;

    rpt_did_reset = 1;
    is_lastline = 0;
    vsyncmintime = uae_gethrtime () + vsynctime;
    write_log ("Resetting frame rate hack\n");
}

/****i* januae-custom-fetch/setclr
 *
 *   NAME
 *       setclr()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void setclr (uae_u16 *p, uae_u16 val)
 *
 *   FUNCTION
 *       Apply the Amiga set-or-clear register convention to a 16-bit state word.
 *
 *   INPUTS
 *       p -- caller-owned 16-bit register state to modify
 *       val -- set/clear control word, with bit 15 selecting set mode
 *
 *   NOTES
 *       Bit 15 selects set versus clear; the low 15 bits specify which bits to
 *       modify.
 *
 ****************************************************************************/

STATIC_INLINE void setclr (uae_u16 *p, uae_u16 val)
{
    if (val & 0x8000)
	*p |= val & 0x7FFF;
    else
	*p &= ~val;
}

/****i* januae-custom-fetch/hsyncdelay
 *
 *   NAME
 *       hsyncdelay()
 *
 *   SYNOPSIS
 *       static void hsyncdelay(void)
 *
 *   FUNCTION
 *       Retain the legacy horizontal-sync delay hook.
 *
 *   NOTES
 *       The function body is empty; no sync delay is performed.
 *
 ****************************************************************************/

static void hsyncdelay(void)
{
}

/****i* januae-custom-fetch/current_hpos
 *
 *   NAME
 *       current_hpos()
 *
 *   SYNOPSIS
 *       STATIC_INLINE unsigned int current_hpos (void)
 *
 *   FUNCTION
 *       Calculate the current horizontal beam position from the event-cycle
 *       counter.
 *
 *   RESULT
 *       Returns elapsed horizontal cycles since the last horizontal-sync event.
 *
 *   NOTES
 *       Returns (get_cycles() - ev_hsync.oldcycles) divided by CYCLE_UNIT.
 *
 ****************************************************************************/

STATIC_INLINE unsigned int current_hpos (void)
{
    return (get_cycles () - eventtab[ev_hsync].oldcycles) / CYCLE_UNIT;
}

/****i* januae-custom-fetch/pfield_xlateptr
 *
 *   NAME
 *       pfield_xlateptr()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u8 *pfield_xlateptr (uaecptr plpt, int bytecount)
 *
 *   FUNCTION
 *       Resolve a bitplane DMA address into the allocated host chip-memory buffer.
 *
 *   INPUTS
 *       plpt -- emulated bitplane source address
 *       bytecount -- required contiguous chip-memory span in bytes
 *
 *   RESULT
 *       Returns a host chip-memory pointer when the complete requested span fits,
 *       otherwise NULL.
 *
 *   NOTES
 *       Applies chipmem_start/chipmem_mask before checking the requested byte
 *       span; returns NULL and logs once if the end exceeds allocated_chipmem.
 *
 ****************************************************************************/

STATIC_INLINE uae_u8 *pfield_xlateptr (uaecptr plpt, int bytecount)
{
    plpt -= chipmem_start & chipmem_mask;
    plpt &= chipmem_mask;
    if ((plpt + bytecount) <= allocated_chipmem)
	return chipmemory + plpt;
    else {
	static int count = 0;
	if (!count) {
	    count++;
	    write_log ("Warning: Bad playfield pointer\n");
	}
	return 0;
    }
}

/****i* januae-custom-fetch/docols
 *
 *   NAME
 *       docols()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void docols (struct color_entry *colentry)
 *
 *   FUNCTION
 *       Convert one emulated color-table entry into host-native palette values.
 *
 *   INPUTS
 *       colentry -- palette table updated in place
 *
 *   NOTES
 *       In AGA mode converts up to 256 24-bit entries; otherwise converts 32
 *       12-bit entries. Invalid palette values are skipped.
 *
 ****************************************************************************/

STATIC_INLINE void docols (struct color_entry *colentry)
{
    int i;

#ifdef AGA
    if (currprefs.chipset_mask & CSMASK_AGA) {
	for (i = 0; i < 256; i++) {
	    int v = color_reg_get (colentry, i);
	    if (v < 0 || v > 16777215)
		continue;
	    colentry->acolors[i] = CONVERT_RGB (v);
	}
    } else {
#endif
	for (i = 0; i < 32; i++) {
	    int v = color_reg_get (colentry, i);
	    if (v < 0 || v > 4095)
		continue;
	    colentry->acolors[i] = xcolors[v];
	}
#ifdef AGA
    }
#endif
}

/****i* januae-custom-fetch/notice_new_xcolors
 *
 *   NAME
 *       notice_new_xcolors()
 *
 *   SYNOPSIS
 *       void notice_new_xcolors (void)
 *
 *   FUNCTION
 *       Recalculate host-native colors for the live palette and both retained
 *       frame color tables.
 *
 *   NOTES
 *       Updates all (MAXVPOS+1)*2 entries in each frame buffer; the commented
 *       colors_for_drawing update is inactive.
 *
 ****************************************************************************/

void notice_new_xcolors (void)
{
    int i;

    docols(&current_colors);
/*    docols(&colors_for_drawing);*/
    for (i = 0; i < (MAXVPOS + 1)*2; i++) {
	docols(color_tables[0]+i);
	docols(color_tables[1]+i);
    }
}

static void do_sprites (unsigned int currhp);

/****i* januae-custom-fetch/remember_ctable
 *
 *   NAME
 *       remember_ctable()
 *
 *   SYNOPSIS
 *       static void remember_ctable (void)
 *
 *   FUNCTION
 *       Record or reuse the current line palette snapshot and compare it against
 *       the previous frame.
 *
 *   NOTES
 *       Maintains remembered_color_entry, color comparison cache and
 *       thisline_changed for smart updates.
 *
 ****************************************************************************/

static void remember_ctable (void)
{
    if (remembered_color_entry == -1) {
	/* The colors changed since we last recorded a color map. Record a
	 * new one. */
	color_reg_cpy (curr_color_tables + next_color_entry, &current_colors);
	remembered_color_entry = next_color_entry++;
    }
    thisline_decision.ctable = remembered_color_entry;
    if (color_src_match == -1 || color_dest_match != remembered_color_entry
	|| line_decisions[next_lineno].ctable != color_src_match)
    {
	/* The remembered comparison didn't help us - need to compare again. */
	int oldctable = line_decisions[next_lineno].ctable;
	int changed = 0;

	if (oldctable == -1) {
	    changed = 1;
	    color_src_match = color_dest_match = -1;
	} else {
	    color_compare_result = color_reg_cmp (&prev_color_tables[oldctable], &current_colors) != 0;
	    if (color_compare_result)
		changed = 1;
	    color_src_match = oldctable;
	    color_dest_match = remembered_color_entry;
	}
	thisline_changed |= changed;
    } else {
	/* We know the result of the comparison */
	if (color_compare_result)
	    thisline_changed = 1;
    }
}

/****i* januae-custom-fetch/remember_ctable_for_border
 *
 *   NAME
 *       remember_ctable_for_border()
 *
 *   SYNOPSIS
 *       static void remember_ctable_for_border (void)
 *
 *   FUNCTION
 *       Record the current palette for border rendering.
 *
 *   NOTES
 *       Delegates without additional state changes to remember_ctable().
 *
 ****************************************************************************/

static void remember_ctable_for_border (void)
{
    remember_ctable ();
}

/* Called to determine the state of the horizontal display window state
 * machine at the current position. It might have changed since we last
 * checked.  */
/****i* januae-custom-fetch/decide_diw
 *
 *   NAME
 *       decide_diw()
 *
 *   SYNOPSIS
 *       static void decide_diw (unsigned int hpos)
 *
 *   FUNCTION
 *       Advance the horizontal display-window start/stop state machine up to the
 *       specified beam cycle.
 *
 *   INPUTS
 *       hpos -- horizontal beam position in chip cycles, unless otherwise noted
 *
 *   NOTES
 *       Converts hpos into window coordinates and records the first crossing of
 *       DIW start or stop.
 *
 ****************************************************************************/

static void decide_diw (unsigned int hpos)
{
    int pix_hpos = coord_diw_to_window_x (hpos == 227 ? 455 : hpos * 2); /* (227.5*2 = 455) */
    if (hdiwstate == DIW_waiting_start && thisline_decision.diwfirstword == -1
	&& pix_hpos >= diwfirstword && last_diw_pix_hpos < diwfirstword)
    {
	thisline_decision.diwfirstword = diwfirstword < 0 ? 0 : diwfirstword;
	hdiwstate = DIW_waiting_stop;
    }
    if (hdiwstate == DIW_waiting_stop && thisline_decision.diwlastword == -1
	&& pix_hpos >= diwlastword && last_diw_pix_hpos < diwlastword)
    {
	thisline_decision.diwlastword = diwlastword < 0 ? 0 : diwlastword;
	hdiwstate = DIW_waiting_start;
    }
    last_diw_pix_hpos = pix_hpos;
}

static int fetchmode;
static int real_bitplane_number[3][3][9];

/* Disable bitplane DMA if planes > available DMA slots. This is needed
   e.g. by the Sanity WOC demo (at the "Party Effect").  */
/****i* januae-custom-fetch/GET_PLANES_LIMIT
 *
 *   NAME
 *       GET_PLANES_LIMIT()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int GET_PLANES_LIMIT (uae_u16 bc0)
 *
 *   FUNCTION
 *       Resolve the effective number of fetchable bitplanes for BPLCON0 under the
 *       current fetch mode.
 *
 *   INPUTS
 *       bc0 -- BPLCON0 register word
 *
 *   RESULT
 *       Returns the effective active DMA plane count from real_bitplane_number.
 *
 *   NOTES
 *       Returns the precomputed real_bitplane_number entry; excess planes are
 *       disabled by the cycle-table mapping.
 *
 ****************************************************************************/

STATIC_INLINE int GET_PLANES_LIMIT (uae_u16 bc0)
{
    int res = GET_RES(bc0);
    int planes = GET_PLANES(bc0);
    return real_bitplane_number[fetchmode][res][planes];
}

/* The HRM says 0xD8, but that can't work... */
#define HARD_DDF_STOP 0xd4
#define HARD_DDF_START 0x18

/****i* januae-custom-fetch/add_modulos
 *
 *   NAME
 *       add_modulos()
 *
 *   SYNOPSIS
 *       static void add_modulos (void)
 *
 *   FUNCTION
 *       Advance DMA bitplane pointers by odd/even or scanline-dependent modulos
 *       after a fetch row.
 *
 *   NOTES
 *       FMODE bit 0x4000 selects a line-dependent shared modulo; otherwise odd and
 *       even planes use BPL1MOD/BPL2MOD.
 *
 ****************************************************************************/

static void add_modulos (void)
{
    int m1, m2;

    if (fmode & 0x4000) {
	if (((diwstrt >> 8) ^ vpos) & 1)
	    m1 = m2 = bpl2mod;
	else
	    m1 = m2 = bpl1mod;
    } else {
	m1 = bpl1mod;
	m2 = bpl2mod;
    }

    switch (GET_PLANES_LIMIT (bplcon0)) {
#ifdef AGA
	case 8: bplpt[7] += m2;
	case 7: bplpt[6] += m1;
#endif
	case 6: bplpt[5] += m2;
	case 5: bplpt[4] += m1;
	case 4: bplpt[3] += m2;
	case 3: bplpt[2] += m1;
	case 2: bplpt[1] += m2;
	case 1: bplpt[0] += m1;
    }
}

/****i* januae-custom-fetch/finish_playfield_line
 *
 *   NAME
 *       finish_playfield_line()
 *
 *   SYNOPSIS
 *       static void finish_playfield_line (void)
 *
 *   FUNCTION
 *       Mark the final drawn scanline span and detect changed playfield geometry
 *       for incremental rendering.
 *
 *   NOTES
 *       Updates first/last drawn vpos. With SMART_UPDATE, compares key display
 *       decisions against the previous frame.
 *
 ****************************************************************************/

static void finish_playfield_line (void)
{
    /* The latter condition might be able to happen in interlaced frames. */
    if (vpos >= minfirstline && (thisframe_first_drawn_line == -1 || (int)vpos < thisframe_first_drawn_line))
	thisframe_first_drawn_line = vpos;
    thisframe_last_drawn_line = vpos;

#ifdef SMART_UPDATE
    if (line_decisions[next_lineno].plflinelen != thisline_decision.plflinelen
	|| line_decisions[next_lineno].plfleft != thisline_decision.plfleft
	|| line_decisions[next_lineno].bplcon0 != thisline_decision.bplcon0
	|| line_decisions[next_lineno].bplcon2 != thisline_decision.bplcon2
#ifdef AGA
	|| line_decisions[next_lineno].bplcon3 != thisline_decision.bplcon3
	|| line_decisions[next_lineno].bplcon4 != thisline_decision.bplcon4
#endif
	)
#endif /* SMART_UPDATE */
	thisline_changed = 1;
}

/* The fetch unit mainly controls ddf stop.  It's the number of cycles that
   are contained in an indivisible block during which ddf is active.  E.g.
   if DDF starts at 0x30, and fetchunit is 8, then possible DDF stops are
   0x30 + n * 8.  */
static unsigned int fetchunit, fetchunit_mask;
/* The delay before fetching the same bitplane again.  Can be larger than
   the number of bitplanes; in that case there are additional empty cycles
   with no data fetch (this happens for high fetchmodes and low
   resolutions).  */
static unsigned int fetchstart, fetchstart_shift, fetchstart_mask;
/* fm_maxplane holds the maximum number of planes possible with the current
   fetch mode.  This selects the cycle diagram:
   8 planes: 73516240
   4 planes: 3120
   2 planes: 10.  */
static int fm_maxplane, fm_maxplane_shift;

/* The corresponding values, by fetchmode and display resolution.  */
static unsigned int fetchunits[] = { 8,8,8,0, 16,8,8,0, 32,16,8,0 };
static unsigned int fetchstarts[] = { 3,2,1,0, 4,3,2,0, 5,4,3,0 };
static unsigned int fm_maxplanes[] = { 3,2,1,0, 3,3,2,0, 3,3,3,0 };

static uae_u8 cycle_diagram_table[3][3][9][32];
static uae_u8 cycle_diagram_free_cycles[3][3][9];
static uae_u8 cycle_diagram_total_cycles[3][3][9];
static uae_u8 *curr_diagram;
static uae_u8 cycle_sequences[3 * 8] = { 2,1,2,1,2,1,2,1, 4,2,3,1,4,2,3,1, 8,4,6,2,7,3,5,1 };


/****i* januae-custom-fetch/create_cycle_diagram_table
 *
 *   NAME
 *       create_cycle_diagram_table()
 *
 *   SYNOPSIS
 *       static void create_cycle_diagram_table(void)
 *
 *   FUNCTION
 *       Precompute bitplane bus-slot order, free cycles, and maximum plane counts
 *       for each fetch mode and resolution.
 *
 *   NOTES
 *       Initializes cycle_diagram_table and real_bitplane_number; a non-AGA
 *       seven-plane low-resolution case maps to four planes.
 *
 ****************************************************************************/

static void create_cycle_diagram_table(void)
{
    unsigned int fm, res, cycle, planes, rplanes, v;
    unsigned int fetch_start, max_planes, freecycles;
    uae_u8 *cycle_sequence;

    for (fm = 0; fm <= 2; fm++) {
	for (res = 0; res <= 2; res++) {
	    max_planes = fm_maxplanes[fm * 4 + res];
	    fetch_start = 1 << fetchstarts[fm * 4 + res];
	    cycle_sequence = &cycle_sequences[(max_planes - 1) * 8];
	    max_planes = 1 << max_planes;
	    for (planes = 0; planes <= 8; planes++) {
		freecycles = 0;
		for (cycle = 0; cycle < 32; cycle++)
		    cycle_diagram_table[fm][res][planes][cycle] = -1;
		if (planes <= max_planes) {
		    for (cycle = 0; cycle < fetch_start; cycle++) {
			if (cycle < max_planes && planes >= cycle_sequence[cycle & 7]) {
			    v = cycle_sequence[cycle & 7];
			} else {
			    v = 0;
			    freecycles++;
			}
			cycle_diagram_table[fm][res][planes][cycle] = v;
		    }
		}
		cycle_diagram_free_cycles[fm][res][planes] = freecycles;
		cycle_diagram_total_cycles[fm][res][planes] = fetch_start;
		rplanes = planes;
		if (rplanes > max_planes)
		    rplanes = 0;
		if (rplanes == 7 && fm == 0 && res == 0 && !(currprefs.chipset_mask & CSMASK_AGA))
		    rplanes = 4;
		real_bitplane_number[fm][res][planes] = rplanes;
	    }
	}
    }
}


/* Used by the copper.  */
static unsigned int estimated_last_fetch_cycle;
static unsigned int cycle_diagram_shift;

/****i* januae-custom-fetch/estimate_last_fetch_cycle
 *
 *   NAME
 *       estimate_last_fetch_cycle()
 *
 *   SYNOPSIS
 *       static void estimate_last_fetch_cycle (unsigned int hpos)
 *
 *   FUNCTION
 *       Estimate the end of bitplane fetches after the DDF stop boundary for
 *       Copper synchronization.
 *
 *   INPUTS
 *       hpos -- horizontal beam position in chip cycles, unless otherwise noted
 *
 *   NOTES
 *       Uses current fetch cycle and fetchunit; distinguishes pending stop, passed
 *       stop, and last-fetch-modulo state.
 *
 ****************************************************************************/

static void estimate_last_fetch_cycle (unsigned int hpos)
{
    unsigned int fetchunit = fetchunits[fetchmode * 4 + GET_RES (bplcon0)];

    if (! passed_plfstop) {
	unsigned int stop = plfstop < hpos || plfstop > HARD_DDF_STOP ? HARD_DDF_STOP : plfstop;
	/* We know that fetching is up-to-date up until hpos, so we can use fetch_cycle.  */
	unsigned int fetch_cycle_at_stop = fetch_cycle + (stop - hpos);
	unsigned int starting_last_block_at = (fetch_cycle_at_stop + fetchunit - 1) & ~(fetchunit - 1);

	estimated_last_fetch_cycle = hpos + (starting_last_block_at - fetch_cycle) + fetchunit;
    } else {
	unsigned int starting_last_block_at = (fetch_cycle + fetchunit - 1) & ~(fetchunit - 1);
	if (passed_plfstop == 2)
	    starting_last_block_at -= fetchunit;

	estimated_last_fetch_cycle = hpos + (starting_last_block_at - fetch_cycle) + fetchunit;
    }
}

static uae_u32 outword[MAX_PLANES];
static int out_nbits, out_offs;
static uae_u32 todisplay[MAX_PLANES][4];
static uae_u32 fetched[MAX_PLANES];
#ifdef AGA
static uae_u32 fetched_aga0[MAX_PLANES];
static uae_u32 fetched_aga1[MAX_PLANES];
#endif

/* Expansions from bplcon0/bplcon1.  */
static int toscr_res, toscr_nr_planes, fetchwidth;
static int toscr_delay1x, toscr_delay2x, toscr_delay1, toscr_delay2;

/* The number of bits left from the last fetched words.
   This is an optimization - conceptually, we have to make sure the result is
   the same as if toscr is called in each clock cycle.  However, to speed this
   up, we accumulate display data; this variable keeps track of how much.
   Thus, once we do call toscr_nbits (which happens at least every 16 bits),
   we can do more work at once.  */
static int toscr_nbits;

/* undocumented bitplane delay hardware feature */
static int delayoffset;

/****i* januae-custom-fetch/compute_delay_offset
 *
 *   NAME
 *       compute_delay_offset()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void compute_delay_offset (void)
 *
 *   FUNCTION
 *       Derive the bitplane display delay offset from fetch mode and DDF start
 *       phase.
 *
 *   NOTES
 *       Uses the fetchmode/offset expression directly; no alternative table is
 *       present.
 *
 ****************************************************************************/

STATIC_INLINE void compute_delay_offset (void)
{
    delayoffset = (16 << fetchmode) - (((plfstrt - HARD_DDF_START) & fetchstart_mask) << 1);
}

/****i* januae-custom-fetch/expand_fmodes
 *
 *   NAME
 *       expand_fmodes()
 *
 *   SYNOPSIS
 *       static void expand_fmodes (void)
 *
 *   FUNCTION
 *       Expand the active fetch mode and BPLCON0 resolution into bus-fetch
 *       geometry and cycle diagram state.
 *
 *   NOTES
 *       Sets fetchunit, fetchstart, masks, plane limits and modulo cycle.
 *
 ****************************************************************************/

static void expand_fmodes (void)
{
    int res = GET_RES(bplcon0);
    int fm = fetchmode;
    fetchunit = fetchunits[fm * 4 + res];
    fetchunit_mask = fetchunit - 1;
    fetchstart_shift = fetchstarts[fm * 4 + res];
    fetchstart = 1 << fetchstart_shift;
    fetchstart_mask = fetchstart - 1;
    fm_maxplane_shift = fm_maxplanes[fm * 4 + res];
    fm_maxplane = 1 << fm_maxplane_shift;
    curr_diagram = cycle_diagram_table[fm][res][GET_PLANES_LIMIT (bplcon0)];
    fetch_modulo_cycle = fetchunit - fetchstart;
}

/* Expand bplcon0/bplcon1 into the toscr_xxx variables.  */
/****i* januae-custom-fetch/compute_toscr_delay_1
 *
 *   NAME
 *       compute_toscr_delay_1()
 *
 *   SYNOPSIS
 *       static void compute_toscr_delay_1 (void)
 *
 *   FUNCTION
 *       Convert BPLCON1 scroll delay fields into odd/even bitplane shift delays.
 *
 *   NOTES
 *       Includes delayoffset and masks delays to the active fetch width and output
 *       resolution.
 *
 ****************************************************************************/

static void compute_toscr_delay_1 (void)
{
    int delay1 = (bplcon1 & 0x0f) | ((bplcon1 & 0x0c00) >> 6);
    int delay2 = ((bplcon1 >> 4) & 0x0f) | (((bplcon1 >> 4) & 0x0c00) >> 6);
    int delaymask;
    int fetchwidth = 16 << fetchmode;

    delay1 += delayoffset;
    delay2 += delayoffset;
    delaymask = (fetchwidth - 1) >> toscr_res;
    toscr_delay1x = (delay1 & delaymask) << toscr_res;
    toscr_delay2x = (delay2 & delaymask) << toscr_res;
}

/****i* januae-custom-fetch/compute_toscr_delay
 *
 *   NAME
 *       compute_toscr_delay()
 *
 *   SYNOPSIS
 *       static void compute_toscr_delay (int hpos)
 *
 *   FUNCTION
 *       Refresh display resolution, active plane count and scroll delays from
 *       current bitplane registers.
 *
 *   INPUTS
 *       hpos -- unused horizontal-position argument
 *
 *   NOTES
 *       The hpos argument is not read by this implementation.
 *
 ****************************************************************************/

static void compute_toscr_delay (int hpos)
{
    toscr_res = GET_RES (bplcon0);
    toscr_nr_planes = GET_PLANES_LIMIT (bplcon0);
    compute_toscr_delay_1 ();
}

/****i* januae-custom-fetch/maybe_first_bpl1dat
 *
 *   NAME
 *       maybe_first_bpl1dat()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void maybe_first_bpl1dat (unsigned int hpos)
 *
 *   FUNCTION
 *       Mark the first produced playfield word and initialize the corresponding
 *       display delay state.
 *
 *   INPUTS
 *       hpos -- horizontal beam position in chip cycles, unless otherwise noted
 *
 *   NOTES
 *       Only acts before thisline_decision.plfleft has been set.
 *
 ****************************************************************************/

STATIC_INLINE void maybe_first_bpl1dat (unsigned int hpos)
{
    if (thisline_decision.plfleft == -1) {
	thisline_decision.plfleft = hpos;
	compute_delay_offset ();
	compute_toscr_delay_1 ();
    }
}

/****i* januae-custom-fetch/fetch
 *
 *   NAME
 *       fetch()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void fetch (int nr, int fm)
 *
 *   FUNCTION
 *       Fetch one plane word or AGA longwords from chip memory and advance the
 *       corresponding DMA pointer.
 *
 *   INPUTS
 *       nr -- zero-based bitplane index to fetch
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       Skips planes above toscr_nr_planes; applies end-of-fetch modulo and
 *       updates the last custom-bus value.
 *
 ****************************************************************************/

STATIC_INLINE void fetch (int nr, int fm)
{
    uaecptr p;
    if (nr >= toscr_nr_planes)
	return;
    p = bplpt[nr];
    switch (fm) {
    case 0:
	fetched[nr] = last_custom_value = chipmem_wget (p);
	bplpt[nr] += 2;
	break;
#ifdef AGA
    case 1:
	fetched_aga0[nr] = chipmem_lget (p);
	last_custom_value = (uae_u16)fetched_aga0[nr];
	bplpt[nr] += 4;
	break;
    case 2:
	fetched_aga1[nr] = chipmem_lget (p);
	fetched_aga0[nr] = chipmem_lget (p + 4);
	last_custom_value = (uae_u16)fetched_aga0[nr];
	bplpt[nr] += 8;
	break;
#endif
    }
    if (passed_plfstop == 2 && fetch_cycle >= (fetch_cycle & ~fetchunit_mask) + fetch_modulo_cycle) {
	int mod;
	if (fmode & 0x4000) {
	    if (((diwstrt >> 8) ^ vpos) & 1)
		mod = bpl2mod;
	    else
		mod = bpl1mod;
	} else if (nr & 1)
	    mod = bpl2mod;
	else
	    mod = bpl1mod;
	bplpt[nr] += mod;
    }
    if (nr == 0)
	fetch_state = fetch_was_plane0;
}

/****i* januae-custom-fetch/clear_fetchbuffer
 *
 *   NAME
 *       clear_fetchbuffer()
 *
 *   SYNOPSIS
 *       static void clear_fetchbuffer (uae_u32 *ptr, int nwords)
 *
 *   FUNCTION
 *       Zero a run of retained bitplane output words while tracking whether
 *       nonzero prior data changed.
 *
 *   INPUTS
 *       ptr -- caller-owned start of the output buffer
 *       nwords -- number of source words or output words processed, as appropriate
 *           to the operation
 *
 *   NOTES
 *       The caller supplies a valid buffer of nwords 32-bit entries; memset writes
 *       nwords*4 bytes.
 *
 ****************************************************************************/

static void clear_fetchbuffer (uae_u32 *ptr, int nwords)
{
    int i;

    if (! thisline_changed)
	for (i = 0; i < nwords; i++)
	    if (ptr[i]) {
		thisline_changed = 1;
		break;
	    }

    memset (ptr, 0, nwords * 4);
}

/****i* januae-custom-fetch/update_toscr_planes
 *
 *   NAME
 *       update_toscr_planes()
 *
 *   SYNOPSIS
 *       static void update_toscr_planes (void)
 *
 *   FUNCTION
 *       Expand the line output to cover newly enabled bitplanes and clear
 *       preceding output for those planes.
 *
 *   NOTES
 *       Does not shrink thisline_decision.nr_planes when the active plane count
 *       drops.
 *
 ****************************************************************************/

static void update_toscr_planes (void)
{
    if (toscr_nr_planes > thisline_decision.nr_planes) {
	int j;
	for (j = thisline_decision.nr_planes; j < toscr_nr_planes; j++)
	    clear_fetchbuffer ((uae_u32 *)(line_data[next_lineno] + 2 * MAX_WORDS_PER_LINE * j), out_offs);
	thisline_decision.nr_planes = toscr_nr_planes;
    }
}

/****i* januae-custom-fetch/toscr_3_ecs
 *
 *   NAME
 *       toscr_3_ecs()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void toscr_3_ecs (int nbits)
 *
 *   FUNCTION
 *       Accumulate delayed ECS/OCS plane bits into the 32-bit output words.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *
 *   NOTES
 *       Uses separate scroll delays for alternating odd and even plane indices.
 *
 ****************************************************************************/

STATIC_INLINE void toscr_3_ecs (int nbits)
{
    int delay1 = toscr_delay1;
    int delay2 = toscr_delay2;
    int i;
    uae_u32 mask = 0xFFFF >> (16 - nbits);

    for (i = 0; i < toscr_nr_planes; i += 2) {
	outword[i] <<= nbits;
	outword[i] |= (todisplay[i][0] >> (16 - nbits + delay1)) & mask;
	todisplay[i][0] <<= nbits;
    }
    for (i = 1; i < toscr_nr_planes; i += 2) {
	outword[i] <<= nbits;
	outword[i] |= (todisplay[i][0] >> (16 - nbits + delay2)) & mask;
	todisplay[i][0] <<= nbits;
    }
}

/****i* januae-custom-fetch/shift32plus
 *
 *   NAME
 *       shift32plus()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void shift32plus (uae_u32 *p, int n)
 *
 *   FUNCTION
 *       Shift a 64-bit logical bitstream represented by two adjacent 32-bit words.
 *
 *   INPUTS
 *       p -- caller-owned 16-bit register state to modify
 *       n -- number of bits to shift
 *
 *   NOTES
 *       Updates p[1] with its shifted prior content and the bits carried from
 *       p[0]; leaves p[0] unchanged.
 *
 ****************************************************************************/

STATIC_INLINE void shift32plus (uae_u32 *p, int n)
{
    uae_u32 t = p[1];
    t <<= n;
    t |= p[0] >> (32 - n);
    p[1] = t;
}

#ifdef AGA
/****i* januae-custom-fetch/aga_shift
 *
 *   NAME
 *       aga_shift()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void aga_shift (uae_u32 *p, int n, int fm)
 *
 *   FUNCTION
 *       Shift one AGA multiword bitplane staging buffer by the requested number of
 *       bits.
 *
 *   INPUTS
 *       p -- caller-owned 16-bit register state to modify
 *       n -- number of bits to shift
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       Compiled only with AGA; shifts additional words for fetch mode 2.
 *
 ****************************************************************************/

STATIC_INLINE void aga_shift (uae_u32 *p, int n, int fm)
{
    if (fm == 2) {
	shift32plus (p + 2, n);
	shift32plus (p + 1, n);
    }
    shift32plus (p + 0, n);
    p[0] <<= n;
}

/****i* januae-custom-fetch/toscr_3_aga
 *
 *   NAME
 *       toscr_3_aga()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void toscr_3_aga (int nbits, int fm)
 *
 *   FUNCTION
 *       Accumulate delayed AGA bitplane data into the 32-bit display words.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       Compiled with AGA; selects the relevant 64-bit window and odd/even delay
 *       for each plane.
 *
 ****************************************************************************/

STATIC_INLINE void toscr_3_aga (int nbits, int fm)
{
    int delay1 = toscr_delay1;
    int delay2 = toscr_delay2;
    int i;
    uae_u32 mask = 0xFFFF >> (16 - nbits);

    {
	int offs = (16 << fm) - nbits + delay1;
	int off1 = offs >> 5;
	if (off1 == 3)
	    off1 = 2;
	offs -= off1 * 32;
	for (i = 0; i < toscr_nr_planes; i += 2) {
	    uae_u32 t0 = todisplay[i][off1];
	    uae_u32 t1 = todisplay[i][off1 + 1];
	    uae_u64 t = (((uae_u64)t1) << 32) | t0;
	    outword[i] <<= nbits;
	    outword[i] |= (t >> offs) & mask;
	    aga_shift (todisplay[i], nbits, fm);
	}
    }
    {
	int offs = (16 << fm) - nbits + delay2;
	int off1 = offs >> 5;
	if (off1 == 3)
	    off1 = 2;
	offs -= off1 * 32;
	for (i = 1; i < toscr_nr_planes; i += 2) {
	    uae_u32 t0 = todisplay[i][off1];
	    uae_u32 t1 = todisplay[i][off1 + 1];
	    uae_u64 t = (((uae_u64)t1) << 32) | t0;
	    outword[i] <<= nbits;
	    outword[i] |= (t >> offs) & mask;
	    aga_shift (todisplay[i], nbits, fm);
	}
    }
}

#endif

/****i* januae-custom-fetch/toscr_2_0
 *
 *   NAME
 *       toscr_2_0()
 *
 *   SYNOPSIS
 *       static void toscr_2_0 (int nbits)
 *
 *   FUNCTION
 *       Dispatch one ordinary 16-bit ECS/OCS display-bit conversion chunk.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *
 *   NOTES
 *       Inline wrapper around toscr_3_ecs().
 *
 ****************************************************************************/

static void toscr_2_0 (int nbits) { toscr_3_ecs (nbits); }
#ifdef AGA
/****i* januae-custom-fetch/toscr_2_1
 *
 *   NAME
 *       toscr_2_1()
 *
 *   SYNOPSIS
 *       static void toscr_2_1 (int nbits)
 *
 *   FUNCTION
 *       Dispatch one AGA fetch-mode-1 display-bit conversion chunk.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *
 *   NOTES
 *       Compiled with AGA; passes fetch mode 1 to toscr_3_aga().
 *
 ****************************************************************************/

static void toscr_2_1 (int nbits) { toscr_3_aga (nbits, 1); }
/****i* januae-custom-fetch/toscr_2_2
 *
 *   NAME
 *       toscr_2_2()
 *
 *   SYNOPSIS
 *       static void toscr_2_2 (int nbits)
 *
 *   FUNCTION
 *       Dispatch one AGA fetch-mode-2 display-bit conversion chunk.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *
 *   NOTES
 *       Compiled with AGA; passes fetch mode 2 to toscr_3_aga().
 *
 ****************************************************************************/

static void toscr_2_2 (int nbits) { toscr_3_aga (nbits, 2); }
#endif

/****i* januae-custom-fetch/toscr_1
 *
 *   NAME
 *       toscr_1()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void toscr_1 (int nbits, int fm)
 *
 *   FUNCTION
 *       Advance bitplane output staging and write completed 32-bit plane words to
 *       the current line buffer.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       Detects changed output words for smart redraw; clears high planes when the
 *       active plane count is reduced.
 *
 ****************************************************************************/

STATIC_INLINE void toscr_1 (int nbits, int fm)
{
    switch (fm) {
    case 0:
	toscr_2_0 (nbits);
	break;
#ifdef AGA
    case 1:
	toscr_2_1 (nbits);
	break;
    case 2:
	toscr_2_2 (nbits);
	break;
#endif
    }
    out_nbits += nbits;
    if (out_nbits == 32) {
	int i;
	uae_u8 *dataptr = line_data[next_lineno] + out_offs * 4;
	for (i = 0; i < thisline_decision.nr_planes; i++) {
	    uae_u32 *dataptr32 = (uae_u32 *)dataptr;
	    if (i >= toscr_nr_planes)
		outword[i] = 0;
	    if (*dataptr32 != outword[i])
		thisline_changed = 1;
	    *dataptr32 = outword[i];
	    dataptr += MAX_WORDS_PER_LINE * 2;
	}
	out_offs++;
	out_nbits = 0;
    }
}

static void toscr_fm0 (int);
static void toscr_fm1 (int);
static void toscr_fm2 (int);

/****i* januae-custom-fetch/toscr
 *
 *   NAME
 *       toscr()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void toscr (int nbits, int fm)
 *
 *   FUNCTION
 *       Dispatch a display-bit chunk to the selected OCS/ECS or AGA fetch-mode
 *       converter.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       Modes 1 and 2 have active switch arms only when AGA is compiled.
 *
 ****************************************************************************/

STATIC_INLINE void toscr (int nbits, int fm)
{
    switch (fm) {
    case 0: toscr_fm0 (nbits); break;
#ifdef AGA
    case 1: toscr_fm1 (nbits); break;
    case 2: toscr_fm2 (nbits); break;
#endif
    }
}

/****i* januae-custom-fetch/toscr_0
 *
 *   NAME
 *       toscr_0()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void toscr_0 (int nbits, int fm)
 *
 *   FUNCTION
 *       Divide a pending bit count into chunks that fit the 32-bit output staging
 *       accumulator.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       Flushes a 16-bit segment first when nbits exceeds 16, then completes
 *       partial 32-bit output as needed.
 *
 ****************************************************************************/

STATIC_INLINE void toscr_0 (int nbits, int fm)
{
    int t;

    if (nbits > 16) {
	toscr (16, fm);
	nbits -= 16;
    }

    t = 32 - out_nbits;
    if (t < nbits) {
	toscr_1 (t, fm);
	nbits -= t;
    }
    toscr_1 (nbits, fm);
}

/****i* januae-custom-fetch/toscr_fm0
 *
 *   NAME
 *       toscr_fm0()
 *
 *   SYNOPSIS
 *       static void toscr_fm0 (int nbits)
 *
 *   FUNCTION
 *       Convert a display-bit chunk using the OCS/ECS fetch-mode wrapper.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *
 *   NOTES
 *       Delegates to toscr_0() with mode 0.
 *
 ****************************************************************************/

static void toscr_fm0 (int nbits) { toscr_0 (nbits, 0); }
/****i* januae-custom-fetch/toscr_fm1
 *
 *   NAME
 *       toscr_fm1()
 *
 *   SYNOPSIS
 *       static void toscr_fm1 (int nbits)
 *
 *   FUNCTION
 *       Convert a display-bit chunk using the AGA mode-1 wrapper.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *
 *   NOTES
 *       Delegates to toscr_0() with mode 1; selected only by the AGA dispatch arm.
 *
 ****************************************************************************/

static void toscr_fm1 (int nbits) { toscr_0 (nbits, 1); }
/****i* januae-custom-fetch/toscr_fm2
 *
 *   NAME
 *       toscr_fm2()
 *
 *   SYNOPSIS
 *       static void toscr_fm2 (int nbits)
 *
 *   FUNCTION
 *       Convert a display-bit chunk using the AGA mode-2 wrapper.
 *
 *   INPUTS
 *       nbits -- number of display bits to shift or emit
 *
 *   NOTES
 *       Delegates to toscr_0() with mode 2; selected only by the AGA dispatch arm.
 *
 ****************************************************************************/

static void toscr_fm2 (int nbits) { toscr_0 (nbits, 2); }

/****i* januae-custom-fetch/flush_plane_data
 *
 *   NAME
 *       flush_plane_data()
 *
 *   SYNOPSIS
 *       static int flush_plane_data (int fm)
 *
 *   FUNCTION
 *       Finish pending bitplane output words at the end of a fetched playfield
 *       segment.
 *
 *   INPUTS
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   RESULT
 *       Returns the number of display positions advanced, scaled to toscr_res.
 *
 *   NOTES
 *       Returns the number of output pixel positions advanced after resolution
 *       scaling; also emits two final 16-bit chunks.
 *
 ****************************************************************************/

static int flush_plane_data (int fm)
{
    int i = 0;
    int fetchwidth = 16 << fm;

    if (out_nbits <= 16) {
	i += 16;
	toscr_1 (16, fm);
    }
    if (out_nbits != 0) {
	i += 32 - out_nbits;
	toscr_1 (32 - out_nbits, fm);
    }
    i += 32;

    toscr_1 (16, fm);
    toscr_1 (16, fm);
    return i >> (1 + toscr_res);
}

/****i* januae-custom-fetch/flush_display
 *
 *   NAME
 *       flush_display()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void flush_display (int fm)
 *
 *   FUNCTION
 *       Emit any accumulated display bits for the current line and clear the
 *       pending-bit count.
 *
 *   INPUTS
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       Only emits when the playfield has a valid left edge.
 *
 ****************************************************************************/

STATIC_INLINE void flush_display (int fm)
{
    if (toscr_nbits > 0 && thisline_decision.plfleft != -1)
	toscr (toscr_nbits, fm);
    toscr_nbits = 0;
}

/****i* januae-custom-fetch/fetch_start
 *
 *   NAME
 *       fetch_start()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void fetch_start (int hpoa)
 *
 *   FUNCTION
 *       Change the current bitplane fetch state to fetch_started.
 *
 *   INPUTS
 *       hpoa -- unused horizontal-position argument
 *
 *   NOTES
 *       The hpoa argument is unused.
 *
 ****************************************************************************/

STATIC_INLINE void fetch_start (int hpoa)
{
    fetch_state = fetch_started;
}

/* Called when all planes have been fetched, i.e. when a new block
   of data is available to be displayed.  The data in fetched[] is
   moved into todisplay[].  */
/****i* januae-custom-fetch/beginning_of_plane_block
 *
 *   NAME
 *       beginning_of_plane_block()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void beginning_of_plane_block (int pos, int fm)
 *
 *   FUNCTION
 *       Publish newly fetched plane data to display shift registers at the
 *       beginning of a fetch block.
 *
 *   INPUTS
 *       pos -- horizontal position at which the completed plane block begins
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       Flushes prior data, applies ECS/AGA fetch words and latches the first
 *       playfield position and delay state.
 *
 ****************************************************************************/

STATIC_INLINE void beginning_of_plane_block (int pos, int fm)
{
    int i;

    flush_display (fm);
    if (fm == 0)
	for (i = 0; i < MAX_PLANES; i++)
	    todisplay[i][0] |= fetched[i];
#ifdef AGA
    else
	for (i = 0; i < MAX_PLANES; i++) {
	    if (fm == 2)
		todisplay[i][1] = fetched_aga1[i];
	    todisplay[i][0] = fetched_aga0[i];
	}
#endif
    maybe_first_bpl1dat (pos);
    toscr_delay1 = toscr_delay1x;
    toscr_delay2 = toscr_delay2x;
}

#ifdef SPEEDUP

/* The usual inlining tricks - don't touch unless you know what you are doing. */
/****i* januae-custom-fetch/long_fetch_ecs
 *
 *   NAME
 *       long_fetch_ecs()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void long_fetch_ecs (int plane, int nwords, int
 *       weird_number_of_bits, int dma)
 *
 *   FUNCTION
 *       Run the optimized ECS/OCS word-fetch and display conversion loop for one
 *       plane.
 *
 *   INPUTS
 *       plane -- zero-based bitplane index
 *       nwords -- number of source words or output words processed, as appropriate
 *           to the operation
 *       weird_number_of_bits -- nonzero when the initial output word has partial
 *           alignment
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   NOTES
 *       Available under SPEEDUP. Uses a direct chip-memory translation; returns
 *       immediately on invalid translation instead of falling back to
 *       chipmem_wget().
 *
 ****************************************************************************/

STATIC_INLINE void long_fetch_ecs (int plane, int nwords, int weird_number_of_bits, int dma)
{
    uae_u16 *real_pt = (uae_u16 *)pfield_xlateptr (bplpt[plane] + bpl_off[plane], nwords * 2);
    int delay = (plane & 1) ? toscr_delay2 : toscr_delay1;
    int tmp_nbits = out_nbits;
    uae_u32 shiftbuffer = todisplay[plane][0];
    uae_u32 outval = outword[plane];
    uae_u32 fetchval = fetched[plane];
    uae_u32 *dataptr = (uae_u32 *)(line_data[next_lineno] + 2 * plane * MAX_WORDS_PER_LINE + 4 * out_offs);

    if (dma)
	bplpt[plane] += nwords * 2;

    if (real_pt == 0)
	/* @@@ Don't do this, fall back on chipmem_wget instead.  */
	return;

    while (nwords > 0) {
	int bits_left = 32 - tmp_nbits;
	uae_u32 t;

	shiftbuffer |= fetchval;

	t = (shiftbuffer >> delay) & 0xFFFF;

	if (weird_number_of_bits && bits_left < 16) {
	    outval <<= bits_left;
	    outval |= t >> (16 - bits_left);
	    thisline_changed |= *dataptr ^ outval;
	    *dataptr++ = outval;

	    outval = t;
	    tmp_nbits = 16 - bits_left;
	    shiftbuffer <<= 16;
	} else {
	    outval = (outval << 16) | t;
	    shiftbuffer <<= 16;
	    tmp_nbits += 16;
	    if (tmp_nbits == 32) {
		thisline_changed |= *dataptr ^ outval;
		*dataptr++ = outval;
		tmp_nbits = 0;
	    }
	}
	nwords--;
	if (dma) {
	    fetchval = do_get_mem_word (real_pt);
	    real_pt++;
	}
    }
    fetched[plane] = fetchval;
    todisplay[plane][0] = shiftbuffer;
    outword[plane] = outval;
}

#ifdef AGA
/****i* januae-custom-fetch/long_fetch_aga
 *
 *   NAME
 *       long_fetch_aga()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void long_fetch_aga (int plane, int nwords, int
 *       weird_number_of_bits, int fm, int dma)
 *
 *   FUNCTION
 *       Run the optimized AGA longword-fetch and display conversion loop for one
 *       plane.
 *
 *   INPUTS
 *       plane -- zero-based bitplane index
 *       nwords -- number of source words or output words processed, as appropriate
 *           to the operation
 *       weird_number_of_bits -- nonzero when the initial output word has partial
 *           alignment
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   NOTES
 *       Available only under SPEEDUP and AGA; uses direct translated memory and
 *       returns without fallback if translation fails.
 *
 ****************************************************************************/

STATIC_INLINE void long_fetch_aga (int plane, int nwords, int weird_number_of_bits, int fm, int dma)
{
    uae_u32 *real_pt = (uae_u32 *)pfield_xlateptr (bplpt[plane] + bpl_off[plane], nwords * 2);
    int delay = (plane & 1) ? toscr_delay2 : toscr_delay1;
    int tmp_nbits = out_nbits;
    uae_u32 *shiftbuffer = todisplay[plane];
    uae_u32 outval = outword[plane];
    uae_u32 fetchval0 = fetched_aga0[plane];
    uae_u32 fetchval1 = fetched_aga1[plane];
    uae_u32 *dataptr = (uae_u32 *)(line_data[next_lineno] + 2 * plane * MAX_WORDS_PER_LINE + 4 * out_offs);
    int offs = (16 << fm) - 16 + delay;
    int off1 = offs >> 5;
    if (off1 == 3)
	off1 = 2;
    offs -= off1 * 32;

    if (dma)
	bplpt[plane] += nwords * 2;

    if (real_pt == 0)
	/* @@@ Don't do this, fall back on chipmem_wget instead.  */
	return;

    while (nwords > 0) {
	int i;

	shiftbuffer[0] = fetchval0;
	if (fm == 2)
	    shiftbuffer[1] = fetchval1;

	for (i = 0; i < (1 << fm); i++) {
	    int bits_left = 32 - tmp_nbits;

	    uae_u32 t0 = shiftbuffer[off1];
	    uae_u32 t1 = shiftbuffer[off1 + 1];
	    uae_u64 t = (((uae_u64)t1) << 32) | t0;

	    t0 = (uae_u32)((t >> offs) & 0xFFFF);

	    if (weird_number_of_bits && bits_left < 16) {
		outval <<= bits_left;
		outval |= t0 >> (16 - bits_left);

		thisline_changed |= *dataptr ^ outval;
		*dataptr++ = outval;

		outval = t0;
		tmp_nbits = 16 - bits_left;
		aga_shift (shiftbuffer, 16, fm);
	    } else {
		outval = (outval << 16) | t0;
		aga_shift (shiftbuffer, 16, fm);
		tmp_nbits += 16;
		if (tmp_nbits == 32) {
		    thisline_changed |= *dataptr ^ outval;
		    *dataptr++ = outval;
		    tmp_nbits = 0;
		}
	    }
	}

	nwords -= 1 << fm;

	if (dma) {
	    if (fm == 1)
		fetchval0 = do_get_mem_long (real_pt);
	    else {
		fetchval1 = do_get_mem_long (real_pt);
		fetchval0 = do_get_mem_long (real_pt + 1);
	    }
	    real_pt += fm;
	}
    }
    fetched_aga0[plane] = fetchval0;
    fetched_aga1[plane] = fetchval1;
    outword[plane] = outval;
}
#endif

/****i* januae-custom-fetch/long_fetch_ecs_0
 *
 *   NAME
 *       long_fetch_ecs_0()
 *
 *   SYNOPSIS
 *       static void NOINLINE long_fetch_ecs_0 (int hpos, int nwords, int dma)
 *
 *   FUNCTION
 *       Use the optimized ECS word-fetch path with normal output-bit alignment.
 *
 *   INPUTS
 *       hpos -- plane index, despite the historical parameter name
 *       nwords -- number of source words or output words processed, as appropriate
 *           to the operation
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   NOTES
 *       SPEEDUP wrapper; the hpos parameter is passed as a plane number.
 *
 ****************************************************************************/

static void NOINLINE long_fetch_ecs_0 (int hpos, int nwords, int dma) { long_fetch_ecs (hpos, nwords, 0, dma); }
/****i* januae-custom-fetch/long_fetch_ecs_1
 *
 *   NAME
 *       long_fetch_ecs_1()
 *
 *   SYNOPSIS
 *       static void NOINLINE long_fetch_ecs_1 (int hpos, int nwords, int dma)
 *
 *   FUNCTION
 *       Use the optimized ECS word-fetch path with partial-word output alignment.
 *
 *   INPUTS
 *       hpos -- plane index, despite the historical parameter name
 *       nwords -- number of source words or output words processed, as appropriate
 *           to the operation
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   NOTES
 *       SPEEDUP wrapper; passes the alignment flag 1 to long_fetch_ecs().
 *
 ****************************************************************************/

static void NOINLINE long_fetch_ecs_1 (int hpos, int nwords, int dma) { long_fetch_ecs (hpos, nwords, 1, dma); }
#ifdef AGA
/****i* januae-custom-fetch/long_fetch_aga_1_0
 *
 *   NAME
 *       long_fetch_aga_1_0()
 *
 *   SYNOPSIS
 *       static void NOINLINE long_fetch_aga_1_0 (int hpos, int nwords, int dma)
 *
 *   FUNCTION
 *       Use optimized AGA fetch-mode-1 conversion without the partial-word
 *       alignment flag.
 *
 *   INPUTS
 *       hpos -- plane index, despite the historical parameter name
 *       nwords -- number of source words or output words processed, as appropriate
 *           to the operation
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   NOTES
 *       SPEEDUP and AGA wrapper; the hpos parameter is used as the plane index.
 *
 ****************************************************************************/

static void NOINLINE long_fetch_aga_1_0 (int hpos, int nwords, int dma) { long_fetch_aga (hpos, nwords, 0, 1, dma); }
/****i* januae-custom-fetch/long_fetch_aga_1_1
 *
 *   NAME
 *       long_fetch_aga_1_1()
 *
 *   SYNOPSIS
 *       static void NOINLINE long_fetch_aga_1_1 (int hpos, int nwords, int dma)
 *
 *   FUNCTION
 *       Use optimized AGA fetch-mode-1 conversion with the partial-word alignment
 *       flag.
 *
 *   INPUTS
 *       hpos -- plane index, despite the historical parameter name
 *       nwords -- number of source words or output words processed, as appropriate
 *           to the operation
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   NOTES
 *       SPEEDUP and AGA wrapper; delegates to long_fetch_aga() with fm=1, flag=1.
 *
 ****************************************************************************/

static void NOINLINE long_fetch_aga_1_1 (int hpos, int nwords, int dma) { long_fetch_aga (hpos, nwords, 1, 1, dma); }
/****i* januae-custom-fetch/long_fetch_aga_2_0
 *
 *   NAME
 *       long_fetch_aga_2_0()
 *
 *   SYNOPSIS
 *       static void NOINLINE long_fetch_aga_2_0 (int hpos, int nwords, int dma)
 *
 *   FUNCTION
 *       Use optimized AGA fetch-mode-2 conversion without the partial-word
 *       alignment flag.
 *
 *   INPUTS
 *       hpos -- plane index, despite the historical parameter name
 *       nwords -- number of source words or output words processed, as appropriate
 *           to the operation
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   NOTES
 *       SPEEDUP and AGA wrapper; delegates to long_fetch_aga() with fm=2, flag=0.
 *
 ****************************************************************************/

static void NOINLINE long_fetch_aga_2_0 (int hpos, int nwords, int dma) { long_fetch_aga (hpos, nwords, 0, 2, dma); }
/****i* januae-custom-fetch/long_fetch_aga_2_1
 *
 *   NAME
 *       long_fetch_aga_2_1()
 *
 *   SYNOPSIS
 *       static void NOINLINE long_fetch_aga_2_1 (int hpos, int nwords, int dma)
 *
 *   FUNCTION
 *       Use optimized AGA fetch-mode-2 conversion with the partial-word alignment
 *       flag.
 *
 *   INPUTS
 *       hpos -- plane index, despite the historical parameter name
 *       nwords -- number of source words or output words processed, as appropriate
 *           to the operation
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   NOTES
 *       SPEEDUP and AGA wrapper; delegates to long_fetch_aga() with fm=2, flag=1.
 *
 ****************************************************************************/

static void NOINLINE long_fetch_aga_2_1 (int hpos, int nwords, int dma) { long_fetch_aga (hpos, nwords, 1, 2, dma); }
#endif

/****i* januae-custom-fetch/do_long_fetch
 *
 *   NAME
 *       do_long_fetch()
 *
 *   SYNOPSIS
 *       static void do_long_fetch (int hpos, int nwords, int dma, int fm)
 *
 *   FUNCTION
 *       Dispatch accelerated fetch conversion across active bitplanes and update
 *       fetch/output cursors.
 *
 *   INPUTS
 *       hpos -- currently processed horizontal position; not used directly in body
 *       nwords -- number of source words or output words processed, as appropriate
 *           to the operation
 *       dma -- nonzero when DMA memory reads should be performed
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       Compiled only with SPEEDUP; selects the alignment specialization and marks
 *       the plane-zero fetch state when DMA is active.
 *
 ****************************************************************************/

static void do_long_fetch (int hpos, int nwords, int dma, int fm)
{
    int i;

    flush_display (fm);
    switch (fm) {
    case 0:
	if (out_nbits & 15) {
	    for (i = 0; i < toscr_nr_planes; i++)
		long_fetch_ecs_1 (i, nwords, dma);
	} else {
	    for (i = 0; i < toscr_nr_planes; i++)
		long_fetch_ecs_0 (i, nwords, dma);
	}
	break;
#ifdef AGA
    case 1:
	if (out_nbits & 15) {
	    for (i = 0; i < toscr_nr_planes; i++)
		long_fetch_aga_1_1 (i, nwords, dma);
	} else {
	    for (i = 0; i < toscr_nr_planes; i++)
		long_fetch_aga_1_0 (i, nwords, dma);
	}
	break;
    case 2:
	if (out_nbits & 15) {
	    for (i = 0; i < toscr_nr_planes; i++)
		long_fetch_aga_2_1 (i, nwords, dma);
	} else {
	    for (i = 0; i < toscr_nr_planes; i++)
		long_fetch_aga_2_0 (i, nwords, dma);
	}
	break;
#endif
    }

    out_nbits += nwords * 16;
    out_offs += out_nbits >> 5;
    out_nbits &= 31;

    if (dma && toscr_nr_planes > 0)
	fetch_state = fetch_was_plane0;
}

#endif

/* make sure fetch that goes beyond maxhpos is finished */
/****i* januae-custom-fetch/finish_final_fetch
 *
 *   NAME
 *       finish_final_fetch()
 *
 *   SYNOPSIS
 *       static void finish_final_fetch (int i, int fm)
 *
 *   FUNCTION
 *       Close the current playfield fetch sequence at the end of the permitted DDF
 *       interval.
 *
 *   INPUTS
 *       i -- current horizontal fetch-cycle position
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       If a playfield started and has not finished, flushes plane data, records
 *       right boundary/length and finishes line decisions.
 *
 ****************************************************************************/

static void finish_final_fetch (int i, int fm)
{
    if (thisline_decision.plfleft == -1)
	return;
    if (passed_plfstop == 3)
	return;
    passed_plfstop = 3;
    ddfstate = DIW_waiting_start;
    i += flush_plane_data (fm);
    thisline_decision.plfright = i;
    thisline_decision.plflinelen = out_offs;
    finish_playfield_line ();
}

/****i* januae-custom-fetch/one_fetch_cycle_0
 *
 *   NAME
 *       one_fetch_cycle_0()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int one_fetch_cycle_0 (int i, int ddfstop_to_test, int dma,
 *       int fm)
 *
 *   FUNCTION
 *       Execute one bitplane DMA fetch slot and advance the video data-to-screen
 *       accumulator.
 *
 *   INPUTS
 *       i -- current horizontal fetch-cycle position
 *       ddfstop_to_test -- effective stop position for the data-fetch window
 *       dma -- nonzero when DMA memory reads should be performed
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   RESULT
 *       Returns 1 when the final fetch completes; returns 0 while fetching
 *       continues.
 *
 *   NOTES
 *       Returns 1 when the final fetch completes, otherwise 0. Advances
 *       fetch_cycle and raises a GUI message if toscr_nbits exceeds 16.
 *
 ****************************************************************************/

STATIC_INLINE int one_fetch_cycle_0 (int i, int ddfstop_to_test, int dma, int fm)
{
    if (! passed_plfstop && i == ddfstop_to_test)
	passed_plfstop = 1;

    if ((fetch_cycle & fetchunit_mask) == 0) {
	if (passed_plfstop == 2) {
	    finish_final_fetch (i, fm);
	    return 1;
	}
	if (passed_plfstop)
	    passed_plfstop++;
    }

    if (dma) {
	/* fetchstart_mask can be larger than fm_maxplane if FMODE > 0.  This means
	   that the remaining cycles are idle; we'll fall through the whole switch
	   without doing anything.  */
	int cycle_start = fetch_cycle & fetchstart_mask;
	switch (fm_maxplane) {
	case 8:
	    switch (cycle_start) {
	    case 0: fetch (7, fm); break;
	    case 1: fetch (3, fm); break;
	    case 2: fetch (5, fm); break;
	    case 3: fetch (1, fm); break;
	    case 4: fetch (6, fm); break;
	    case 5: fetch (2, fm); break;
	    case 6: fetch (4, fm); break;
	    case 7: fetch (0, fm); break;
	    }
	    break;
	case 4:
	    switch (cycle_start) {
	    case 0: fetch (3, fm); break;
	    case 1: fetch (1, fm); break;
	    case 2: fetch (2, fm); break;
	    case 3: fetch (0, fm); break;
	    }
	    break;
	case 2:
	    switch (cycle_start) {
	    case 0: fetch (1, fm); break;
	    case 1: fetch (0, fm); break;
	    }
	    break;
	}
    }
    fetch_cycle++;
    toscr_nbits += 2 << toscr_res;

    if (toscr_nbits > 16) {
	uae_abort ("toscr_nbits > 16 (%d)", toscr_nbits);
	toscr_nbits = 0;
    }
    if (toscr_nbits == 16)
	flush_display (fm);

    return 0;
}

/****i* januae-custom-fetch/one_fetch_cycle_fm0
 *
 *   NAME
 *       one_fetch_cycle_fm0()
 *
 *   SYNOPSIS
 *       static int one_fetch_cycle_fm0 (int i, int ddfstop_to_test, int dma)
 *
 *   FUNCTION
 *       Run one fetch cycle with OCS/ECS fetch width.
 *
 *   INPUTS
 *       i -- current horizontal fetch-cycle position
 *       ddfstop_to_test -- effective stop position for the data-fetch window
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   RESULT
 *       Returns the final-fetch flag from one_fetch_cycle_0(..., 0).
 *
 *   NOTES
 *       Returns the result of one_fetch_cycle_0() using mode 0.
 *
 ****************************************************************************/

static int one_fetch_cycle_fm0 (int i, int ddfstop_to_test, int dma) { return one_fetch_cycle_0 (i, ddfstop_to_test, dma, 0); }
/****i* januae-custom-fetch/one_fetch_cycle_fm1
 *
 *   NAME
 *       one_fetch_cycle_fm1()
 *
 *   SYNOPSIS
 *       static int one_fetch_cycle_fm1 (int i, int ddfstop_to_test, int dma)
 *
 *   FUNCTION
 *       Run one fetch cycle with AGA fetch mode 1.
 *
 *   INPUTS
 *       i -- current horizontal fetch-cycle position
 *       ddfstop_to_test -- effective stop position for the data-fetch window
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   RESULT
 *       Returns the final-fetch flag from one_fetch_cycle_0(..., 1).
 *
 *   NOTES
 *       Returns the result of one_fetch_cycle_0() using mode 1.
 *
 ****************************************************************************/

static int one_fetch_cycle_fm1 (int i, int ddfstop_to_test, int dma) { return one_fetch_cycle_0 (i, ddfstop_to_test, dma, 1); }
/****i* januae-custom-fetch/one_fetch_cycle_fm2
 *
 *   NAME
 *       one_fetch_cycle_fm2()
 *
 *   SYNOPSIS
 *       static int one_fetch_cycle_fm2 (int i, int ddfstop_to_test, int dma)
 *
 *   FUNCTION
 *       Run one fetch cycle with AGA fetch mode 2.
 *
 *   INPUTS
 *       i -- current horizontal fetch-cycle position
 *       ddfstop_to_test -- effective stop position for the data-fetch window
 *       dma -- nonzero when DMA memory reads should be performed
 *
 *   RESULT
 *       Returns the final-fetch flag from one_fetch_cycle_0(..., 2).
 *
 *   NOTES
 *       Returns the result of one_fetch_cycle_0() using mode 2.
 *
 ****************************************************************************/

static int one_fetch_cycle_fm2 (int i, int ddfstop_to_test, int dma) { return one_fetch_cycle_0 (i, ddfstop_to_test, dma, 2); }

/****i* januae-custom-fetch/one_fetch_cycle
 *
 *   NAME
 *       one_fetch_cycle()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int one_fetch_cycle (int i, int ddfstop_to_test, int dma,
 *       int fm)
 *
 *   FUNCTION
 *       Dispatch one fetch cycle to the implementation for the selected fetch
 *       mode.
 *
 *   INPUTS
 *       i -- current horizontal fetch-cycle position
 *       ddfstop_to_test -- effective stop position for the data-fetch window
 *       dma -- nonzero when DMA memory reads should be performed
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   RESULT
 *       Returns 1 when the mode-specific final fetch completes; otherwise 0.
 *
 *   NOTES
 *       Returns the mode-specific stop indication; an unsupported mode emits a GUI
 *       diagnostic and returns 0.
 *
 ****************************************************************************/

STATIC_INLINE int one_fetch_cycle (int i, int ddfstop_to_test, int dma, int fm)
{
    switch (fm) {
    case 0: return one_fetch_cycle_fm0 (i, ddfstop_to_test, dma);
#ifdef AGA
    case 1: return one_fetch_cycle_fm1 (i, ddfstop_to_test, dma);
    case 2: return one_fetch_cycle_fm2 (i, ddfstop_to_test, dma);
#endif
    default: uae_abort ("fm corrupt"); return 0;
    }
}

/****i* januae-custom-fetch/update_fetch
 *
 *   NAME
 *       update_fetch()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void update_fetch (unsigned int until, int fm)
 *
 *   FUNCTION
 *       Advance bitplane DMA and the display shift registers up to the requested
 *       horizontal position.
 *
 *   INPUTS
 *       until -- horizontal beam position through which fetches should be
 *           simulated
 *       fm -- bitplane fetch mode index (0, 1, or 2)
 *
 *   NOTES
 *       Skips if nodraw() or fetches have ended; under SPEEDUP can process whole
 *       aligned blocks, then simulates remaining cycles.
 *
 ****************************************************************************/

STATIC_INLINE void update_fetch (unsigned int until, int fm)
{
    unsigned int pos;
    unsigned int dma = dmaen (DMA_BITPLANE);

    unsigned int ddfstop_to_test;

    if (nodraw() || passed_plfstop == 3)
	return;

    /* We need an explicit test against HARD_DDF_STOP here to guard against
       programs that move the DDFSTOP before our current position before we
       reach it.  */
    ddfstop_to_test = HARD_DDF_STOP;
    if ((int)ddfstop >= last_fetch_hpos && plfstop < ddfstop_to_test)
	ddfstop_to_test = plfstop;

    compute_toscr_delay (last_fetch_hpos);
    update_toscr_planes ();

    pos = last_fetch_hpos;
    cycle_diagram_shift = last_fetch_hpos - fetch_cycle;

    /* First, a loop that prepares us for the speedup code.  We want to enter
       the SPEEDUP case with fetch_state == fetch_was_plane0, and then unroll
       whole blocks, so that we end on the same fetch_state again.  */
    for (; ; pos++) {
	if (pos == until) {
	    if (until >= maxhpos) {
		finish_final_fetch (pos, fm);
		return;
	    }
	    flush_display (fm);
	    return;
	}

	if (fetch_state == fetch_was_plane0)
	    break;

	fetch_start (pos);
	if (one_fetch_cycle (pos, ddfstop_to_test, dma, fm))
	    return;
    }

#ifdef SPEEDUP
    /* Unrolled version of the for loop below.  */
    if (! passed_plfstop && ddf_change != vpos && ddf_change + 1 != vpos
	&& dma
	&& (fetch_cycle & fetchstart_mask) == (fm_maxplane & fetchstart_mask)
	&& toscr_delay1 == toscr_delay1x && toscr_delay2 == toscr_delay2x
	&& toscr_nr_planes == thisline_decision.nr_planes)
    {
	unsigned int offs = (pos - fetch_cycle) & fetchunit_mask;
	unsigned int ddf2 = ((ddfstop_to_test - offs + fetchunit - 1) & ~fetchunit_mask) + offs;
	unsigned int ddf3 = ddf2 + fetchunit;
	unsigned int stop = until < ddf2 ? until : until < ddf3 ? ddf2 : ddf3;
	unsigned int count;

	count = stop - pos;

	if (count >= fetchstart) {
	    count &= ~fetchstart_mask;

	    if (thisline_decision.plfleft == -1) {
		compute_delay_offset ();
		compute_toscr_delay_1 ();
	    }

	    do_long_fetch (pos, count >> (3 - toscr_res), dma, fm);

	    /* This must come _after_ do_long_fetch so as not to confuse flush_display
	       into thinking the first fetch has produced any output worth emitting to
	       the screen.  But the calculation of delay_offset must happen _before_.  */
	    maybe_first_bpl1dat (pos);

	    if (pos <= ddfstop_to_test && pos + count > ddfstop_to_test)
		passed_plfstop = 1;
	    if (pos <= ddfstop_to_test && pos + count > ddf2)
		passed_plfstop = 2;
	    if (pos <= ddf2 && pos + count >= ddf2 + fm_maxplane)
		add_modulos ();
	    pos += count;
	    fetch_cycle += count;
	}
    } else {
#endif
	//maybe_first_bpl1dat (pos);
#ifdef SPEEDUP
    }
#endif
    for (; pos < until; pos++) {
	if (fetch_state == fetch_was_plane0)
	    beginning_of_plane_block (pos, fm);
	fetch_start (pos);

	if (one_fetch_cycle (pos, ddfstop_to_test, dma, fm))
	    return;
    }
    if (until >= maxhpos) {
	finish_final_fetch (pos, fm);
	return;
    }
    flush_display (fm);
}

/****i* januae-custom-fetch/update_fetch_0
 *
 *   NAME
 *       update_fetch_0()
 *
 *   SYNOPSIS
 *       static void update_fetch_0 (int hpos)
 *
 *   FUNCTION
 *       Advance fetch state using OCS/ECS mode 0.
 *
 *   INPUTS
 *       hpos -- horizontal beam position in chip cycles, unless otherwise noted
 *
 *   NOTES
 *       Delegates to update_fetch() with mode 0.
 *
 ****************************************************************************/

static void update_fetch_0 (int hpos) { update_fetch (hpos, 0); }
/****i* januae-custom-fetch/update_fetch_1
 *
 *   NAME
 *       update_fetch_1()
 *
 *   SYNOPSIS
 *       static void update_fetch_1 (int hpos)
 *
 *   FUNCTION
 *       Advance fetch state using AGA mode 1.
 *
 *   INPUTS
 *       hpos -- horizontal beam position in chip cycles, unless otherwise noted
 *
 *   NOTES
 *       Delegates to update_fetch() with mode 1.
 *
 ****************************************************************************/

static void update_fetch_1 (int hpos) { update_fetch (hpos, 1); }
/****i* januae-custom-fetch/update_fetch_2
 *
 *   NAME
 *       update_fetch_2()
 *
 *   SYNOPSIS
 *       static void update_fetch_2 (int hpos)
 *
 *   FUNCTION
 *       Advance fetch state using AGA mode 2.
 *
 *   INPUTS
 *       hpos -- horizontal beam position in chip cycles, unless otherwise noted
 *
 *   NOTES
 *       Delegates to update_fetch() with mode 2.
 *
 ****************************************************************************/

static void update_fetch_2 (int hpos) { update_fetch (hpos, 2); }

/****i* januae-custom-fetch/decide_fetch
 *
 *   NAME
 *       decide_fetch()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void decide_fetch (int hpos)
 *
 *   FUNCTION
 *       Synchronize bitplane fetch processing through the requested horizontal
 *       position.
 *
 *   INPUTS
 *       hpos -- horizontal beam position in chip cycles, unless otherwise noted
 *
 *   NOTES
 *       Updates last_fetch_hpos even when no fetch is running; dispatches modes
 *       1/2 only when AGA is compiled.
 *
 ****************************************************************************/

STATIC_INLINE void decide_fetch (int hpos)
{
    if (fetch_state != fetch_not_started && hpos > last_fetch_hpos) {
	switch (fetchmode) {
	case 0: update_fetch_0 (hpos); break;
#ifdef AGA
	case 1: update_fetch_1 (hpos); break;
	case 2: update_fetch_2 (hpos); break;
#endif
	default: uae_abort ("fetchmode corrupt");
	}
    }
    last_fetch_hpos = hpos;
}

/****i* januae-custom-fetch/start_bpl_dma
 *
 *   NAME
 *       start_bpl_dma()
 *
 *   SYNOPSIS
 *       static void start_bpl_dma (unsigned int hpos, int hstart)
 *
 *   FUNCTION
 *       Begin a new bitplane DMA fetch sequence and initialize line output offsets
 *       and display delays.
 *
 *   INPUTS
 *       hpos -- horizontal beam position in chip cycles, unless otherwise noted
 *       hstart -- effective horizontal start used to initialize the fetch cursor
 *
 *   NOTES
 *       Preserves an earlier CPU BPL1DAT write by accounting for preexisting
 *       left-of-playfield output.
 *
 ****************************************************************************/

static void start_bpl_dma (unsigned int hpos, int hstart)
{
    fetch_start (hpos);
    fetch_cycle = 0;
    last_fetch_hpos = hstart;
    out_nbits = 0;
    out_offs = 0;
    toscr_nbits = 0;
    thisline_decision.bplres = GET_RES (bplcon0);

    ddfstate = DIW_waiting_stop;
    compute_toscr_delay (last_fetch_hpos);

    /* If someone already wrote BPL1DAT, clear the area between that point and
       the real fetch start.  */
    if (!nodraw ()) {
	if (thisline_decision.plfleft != -1) {
	    out_nbits = (plfstrt - thisline_decision.plfleft) << (1 + toscr_res);
	    out_offs = out_nbits >> 5;
	    out_nbits &= 31;
	}
	update_toscr_planes ();
    }
}

/* this may turn on datafetch if program turns dma on during the ddf */
/****i* januae-custom-fetch/maybe_start_bpl_dma
 *
 *   NAME
 *       maybe_start_bpl_dma()
 *
 *   SYNOPSIS
 *       static void maybe_start_bpl_dma (unsigned int hpos)
 *
 *   FUNCTION
 *       Conditionally restart bitplane DMA during the display-fetch window after
 *       DMA becomes enabled.
 *
 *   INPUTS
 *       hpos -- horizontal beam position in chip cycles, unless otherwise noted
 *
 *   NOTES
 *       Only applies to an ECS Agnus capable chipset, when no fetch has started
 *       and the horizontal position lies inside the allowed window.
 *
 ****************************************************************************/

static void maybe_start_bpl_dma (unsigned int hpos)
{
    /* OCS: BPL DMA never restarts if DMA is turned on during DDF
     * ECS/AGA: BPL DMA restarts but only if DMA was turned off
       outside of DDF or during current line, otherwise display
       processing jumps immediately to "DDFSTOP passed"-condition */
    if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	return;
    if (fetch_state != fetch_not_started)
	return;
    if (diwstate != DIW_waiting_stop)
	return;
    if (hpos <= plfstrt)
	return;
    if (hpos > plfstop - fetchunit)
	return;
    if (ddfstate != DIW_waiting_start)
	passed_plfstop = 1;
    start_bpl_dma (hpos, hpos);
}

/* This function is responsible for turning on datafetch if necessary. */
/****i* januae-custom-fetch/decide_line
 *
 *   NAME
 *       decide_line()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void decide_line (unsigned int hpos)
 *
 *   FUNCTION
 *       Evaluate vertical display-window state and trigger bitplane DMA at the DDF
 *       start threshold.
 *
 *   INPUTS
 *       hpos -- horizontal beam position in chip cycles, unless otherwise noted
 *
 *   NOTES
 *       Ignores already-processed positions and running fetches; includes a narrow
 *       DDFSTRT write-delay workaround before starting DMA.
 *
 ****************************************************************************/

STATIC_INLINE void decide_line (unsigned int hpos)
{
    /* Take care of the vertical DIW.  */
    if (vpos == plffirstline)
	diwstate = DIW_waiting_stop;
    if (vpos == plflastline)
	diwstate = DIW_waiting_start;

    if ((int)hpos <= last_decide_line_hpos)
	return;
    if (fetch_state != fetch_not_started)
	return;

    if (dmaen (DMA_BITPLANE) && diwstate == DIW_waiting_stop) {
	int ok = 0;
	/* Test if we passed the start of the DDF window.  */
	if (last_decide_line_hpos < (int)plfstrt && hpos >= plfstrt) {
	    ok = 1;
	    /* hack warning.. Writing to DDFSTRT when DMA should start must be ignored
	     * (correct fix would be emulate this delay for every custom register, but why bother..) */
	    if (hpos - 2 == ddfstrt_old_hpos && ddfstrt_old_vpos == vpos)
		ok = 0;
	    if (ok) {
		start_bpl_dma (hpos, plfstrt);
		estimate_last_fetch_cycle (plfstrt);
		last_decide_line_hpos = hpos;
#ifndef	CUSTOM_SIMPLE
		do_sprites (plfstrt);
#endif
		return;
	    }
	}
    }

#ifndef	CUSTOM_SIMPLE
    if (last_sprite_decide_line_hpos < SPR0_HPOS + 4 * MAX_SPRITES)
	do_sprites (hpos);
#endif
    last_sprite_decide_line_hpos = hpos;

    last_decide_line_hpos = hpos;
}

/* Called when a color is about to be changed (write to a color register),
 * but the new color has not been entered into the table yet. */
/****i* januae-custom-raster/record_color_change
 *
 *   NAME
 *       record_color_change()
 *
 *   SYNOPSIS
 *       static void record_color_change (unsigned int hpos, unsigned int regno,
 *       unsigned long value)
 *
 *   FUNCTION
 *       Record a timed palette or synthetic register change in the current
 *       scanline.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       regno -- custom register index being recorded
 *       value -- value to record for this register change
 *
 *   NOTES
 *       Synchronizes DIW and line decisions before appending to
 *       curr_color_changes. Early HBLANK positions are duplicated at
 *       HBLANK_OFFSET; drawing-suppressed and pre-display lines are skipped.
 *
 ****************************************************************************/

static void record_color_change (unsigned int hpos, unsigned int regno, unsigned long value)
{
    if (regno < 0x1000 && nodraw ())
	return;
    /* Early positions don't appear on-screen. */
    if (vpos < minfirstline)
	return;

    decide_diw (hpos);
    decide_line (hpos);

    if (thisline_decision.ctable == -1)
	remember_ctable ();

#ifdef OS_WITHOUT_MEMORY_MANAGEMENT
    if (next_color_change + 1 >= max_color_change) {
	++delta_color_change;
	return;
    }
#endif

    curr_color_changes[next_color_change].linepos = hpos;
    curr_color_changes[next_color_change].regno = regno;
    curr_color_changes[next_color_change++].value = value;
    if (hpos < HBLANK_OFFSET) {
	curr_color_changes[next_color_change].linepos = HBLANK_OFFSET;
	curr_color_changes[next_color_change].regno = regno;
	curr_color_changes[next_color_change++].value = value;
    }
    curr_color_changes[next_color_change].regno = -1;
}

/****i* januae-custom-raster/record_register_change
 *
 *   NAME
 *       record_register_change()
 *
 *   SYNOPSIS
 *       static void record_register_change (unsigned int hpos, unsigned int
 *       regno, unsigned long value)
 *
 *   FUNCTION
 *       Record a bitplane register write as a synthetic color-change event.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       regno -- custom register index being recorded
 *       value -- value to record for this register change
 *
 *   NOTES
 *       For BPLCON0, preserves HAM-seen state and updates early-line resolution;
 *       the recorded register number is offset by 0x1000.
 *
 ****************************************************************************/

static void record_register_change (unsigned int hpos, unsigned int regno, unsigned long value)
{
    if (regno == 0x100) {
	if (value & 0x800)
	    thisline_decision.ham_seen = 1;
	if (hpos < HARD_DDF_START || hpos < plfstrt + 0x20) {
	    thisline_decision.bplcon0 = value;
	    thisline_decision.bplres = GET_RES (value);
	}
    }
    record_color_change (hpos, regno + 0x1000, value);
}

typedef int sprbuf_res_t, cclockres_t, hwres_t,	bplres_t;

/* handle very rarely needed playfield collision (CLXDAT bit 0) */
/****i* januae-custom-raster/do_playfield_collisions
 *
 *   NAME
 *       do_playfield_collisions()
 *
 *   SYNOPSIS
 *       static void do_playfield_collisions (void)
 *
 *   FUNCTION
 *       Detect bitplane-to-bitplane collisions in the visible display span.
 *
 *   NOTES
 *       Sets CLXDAT bit zero on a match; an empty enabled-plane mask sets that
 *       bit immediately. Already latched collisions are not recomputed.
 *
 ****************************************************************************/

static void do_playfield_collisions (void)
{
    int bplres = GET_RES (bplcon0);
    hwres_t ddf_left = thisline_decision.plfleft * 2 << bplres;
    hwres_t hw_diwlast = coord_window_to_diw_x (thisline_decision.diwlastword);
    hwres_t hw_diwfirst = coord_window_to_diw_x (thisline_decision.diwfirstword);
    int i, collided, minpos, maxpos;
#ifdef AGA
    int planes = (currprefs.chipset_mask & CSMASK_AGA) ? 8 : 6;
#else
    int planes = 6;
#endif

    if (clxcon_bpl_enable == 0) {
	clxdat |= 1;
	return;
    }
    if (clxdat & 1)
	return;

    collided = 0;
    minpos = thisline_decision.plfleft * 2;
    if (minpos < hw_diwfirst)
	minpos = hw_diwfirst;
    maxpos = thisline_decision.plfright * 2;
    if (maxpos > hw_diwlast)
	maxpos = hw_diwlast;
    for (i = minpos; i < maxpos && !collided; i+= 32) {
	int offs = ((i << bplres) - ddf_left) >> 3;
	int j;
	uae_u32 total = 0xffffffff;
	for (j = 0; j < planes; j++) {
	    int ena = (clxcon_bpl_enable >> j) & 1;
	    int match = (clxcon_bpl_match >> j) & 1;
	    uae_u32 t = 0xffffffff;
	    if (ena) {
		if (j < thisline_decision.nr_planes) {
		    t = *(uae_u32 *)(line_data[next_lineno] + offs + 2 * j * MAX_WORDS_PER_LINE);
		    t ^= (match & 1) - 1;
		} else {
		    t = (match & 1) - 1;
		}
	    }
	    total &= t;
	}
	if (total) {
	    collided = 1;

	}
    }
    if (collided)
	clxdat |= 1;
}

/* Sprite-to-sprite collisions are taken care of in record_sprite.  This one does
   playfield/sprite collisions. */
/****i* januae-custom-raster/do_sprite_collisions
 *
 *   NAME
 *       do_sprite_collisions()
 *
 *   SYNOPSIS
 *       static void do_sprite_collisions (void)
 *
 *   FUNCTION
 *       Test recorded sprite pixels against the two interleaved playfields.
 *
 *   NOTES
 *       Combines sprite pair mask and bitplane comparisons, setting CLXDAT
 *       collision bits; an empty enabled-plane mask sets bits 0x1FE without
 *       scanning.
 *
 ****************************************************************************/

static void do_sprite_collisions (void)
{
    int nr_sprites = curr_drawinfo[next_lineno].nr_sprites;
    int first = curr_drawinfo[next_lineno].first_sprite_entry;
    int i;
    unsigned int collision_mask = clxmask[clxcon >> 12];
    int bplres = GET_RES (bplcon0);
    hwres_t ddf_left = thisline_decision.plfleft * 2 << bplres;
    hwres_t hw_diwlast = coord_window_to_diw_x (thisline_decision.diwlastword);
    hwres_t hw_diwfirst = coord_window_to_diw_x (thisline_decision.diwfirstword);

    if (clxcon_bpl_enable == 0) {
	clxdat |= 0x1FE;
	return;
    }

    for (i = 0; i < nr_sprites; i++) {
	struct sprite_entry *e = curr_sprite_entries + first + i;
	sprbuf_res_t j;
	sprbuf_res_t minpos = e->pos;
	sprbuf_res_t maxpos = e->max;
	hwres_t minp1 = minpos >> sprite_buffer_res;
	hwres_t maxp1 = maxpos >> sprite_buffer_res;

	if (maxp1 > hw_diwlast)
	    maxpos = hw_diwlast << sprite_buffer_res;
	if (maxp1 > thisline_decision.plfright * 2)
	    maxpos = thisline_decision.plfright * 2 << sprite_buffer_res;
	if (minp1 < hw_diwfirst)
	    minpos = hw_diwfirst << sprite_buffer_res;
	if (minp1 < thisline_decision.plfleft * 2)
	    minpos = thisline_decision.plfleft * 2 << sprite_buffer_res;

	for (j = minpos; j < maxpos; j++) {
	    int sprpix = spixels[e->first_pixel + j - e->pos] & collision_mask;
	    int k, offs, match = 1;

	    if (sprpix == 0)
		continue;

	    offs = ((j << bplres) >> sprite_buffer_res) - ddf_left;
	    sprpix = sprite_ab_merge[sprpix & 255] | (sprite_ab_merge[sprpix >> 8] << 2);
	    sprpix <<= 1;

	    /* Loop over number of playfields.  */
	    for (k = 1; k >= 0; k--) {
		unsigned int l;
#ifdef AGA
		unsigned int planes = (currprefs.chipset_mask & CSMASK_AGA) ? 8 : 6;
#else
		unsigned int planes = 6;
#endif
		if (bplcon0 & 0x400)
		    match = 1;
		for (l = k; match && l < planes; l += 2) {
		    unsigned int t = 0;
		    if (l < thisline_decision.nr_planes) {
			uae_u32 *ldata = (uae_u32 *)(line_data[next_lineno] + 2 * l * MAX_WORDS_PER_LINE);
			uae_u32 word = ldata[offs >> 5];
			t = (word >> (31 - (offs & 31))) & 1;
		    }
		    if (clxcon_bpl_enable & (1 << l)) {
			if (t != ((clxcon_bpl_match >> l) & 1))
			    match = 0;
		    }
		}
		if (match) {
		    clxdat |= sprpix << (k * 4);
		}
	    }
	}
    }
}

/****i* januae-custom-raster/expand_sprres
 *
 *   NAME
 *       expand_sprres()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void expand_sprres (void)
 *
 *   FUNCTION
 *       Derive sprite rendering resolution from BPLCON3 and the active chipset.
 *
 *   NOTES
 *       BPLCON3 sprite-resolution selector zero uses the ECS default, with a
 *       superhires/ECS Denise special case.
 *
 ****************************************************************************/

STATIC_INLINE void expand_sprres (void)
{
    switch ((bplcon3 >> 6) & 3) {
    case 0: /* ECS defaults (LORES,HIRES=140ns,SHRES=70ns) */
	if ((currprefs.chipset_mask & CSMASK_ECS_DENISE) && GET_RES (bplcon0) == RES_SUPERHIRES)
	    sprres = RES_HIRES;
	else
	    sprres = RES_LORES;
	break;
    case 1:
	sprres = RES_LORES;
	break;
    case 2:
	sprres = RES_HIRES;
	break;
    case 3:
	sprres = RES_SUPERHIRES;
	break;
    }
}

/****i* januae-custom-raster/record_sprite_1
 *
 *   NAME
 *       record_sprite_1()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void record_sprite_1 (uae_u16 *buf, uae_u32 datab, int
 *       num, int dbl, unsigned int mask, int do_collisions, uae_u32
 *       collision_mask)
 *
 *   FUNCTION
 *       Merge unpacked two-bit sprite pixels into the output sprite buffer.
 *
 *   INPUTS
 *       buf -- destination sprite pixel buffer; caller owns capacity
 *       datab -- packed two-bit sprite pixel words
 *       num -- zero-based bitplane index (for BPLxPT*), or Copper jump selector
 *           (for COPJMP)
 *       dbl -- nonzero to duplicate emitted sprite pixels
 *       mask -- pixel selection mask for sprite-resolution conversion
 *       do_collisions -- whether to update sprite collision flags
 *       collision_mask -- mask of enabled sprite pairs for collision detection
 *
 *   NOTES
 *       Advances through datab in two-bit packets; optionally duplicates pixels
 *       and accumulates pair-collision bits in CLXDAT.
 *
 ****************************************************************************/

STATIC_INLINE void record_sprite_1 (uae_u16 *buf, uae_u32 datab, int num, int dbl,
				    unsigned int mask, int do_collisions, uae_u32 collision_mask)
{
    int j = 0;
    while (datab) {
	unsigned int tmp = *buf;
	unsigned int col = (datab & 3) << (2 * num);
	tmp |= col;
	if ((j & mask) == 0)
	    *buf++ = tmp;
	if (dbl)
	    *buf++ = tmp;
	j++;
	datab >>= 2;
	if (do_collisions) {
	    tmp &= collision_mask;
	    if (tmp) {
		unsigned int shrunk_tmp = sprite_ab_merge[tmp & 255] | (sprite_ab_merge[tmp >> 8] << 2);
		clxdat |= sprclx[shrunk_tmp];
	    }
	}
    }
}

/* DATAB contains the sprite data; 16 pixels in two-bit packets.  Bits 0/1
   determine the color of the leftmost pixel, bits 2/3 the color of the next
   etc.
   This function assumes that for all sprites in a given line, SPRXP either
   stays equal or increases between successive calls.

   The data is recorded either in lores pixels (if ECS), or in hires pixels
   (if AGA).  No support for SHRES sprites.  */

/****i* januae-custom-raster/record_sprite
 *
 *   NAME
 *       record_sprite()
 *
 *   SYNOPSIS
 *       static void record_sprite (int line, int num, int sprxp, uae_u16 *data,
 *       uae_u16 *datb, unsigned int ctl, unsigned int ctlx)
 *
 *   FUNCTION
 *       Record a decoded sprite run and its attach and collision state for a
 *       scanline.
 *
 *   INPUTS
 *       line -- unused scanline argument
 *       num -- zero-based bitplane index (for BPLxPT*), or Copper jump selector
 *           (for COPJMP)
 *       sprxp -- sprite left coordinate in sprite-buffer units
 *       data -- sprite low data words
 *       datb -- sprite high data words
 *       ctl -- unused sprite control argument
 *       ctlx -- unused paired sprite control argument
 *
 *   NOTES
 *       Assumes sprites arrive in nondecreasing X position; coalesces nearby
 *       entries. ctl and ctlx arguments are not read; the routine uses sprctl[].
 *
 ****************************************************************************/

static void record_sprite (int line, int num, int sprxp, uae_u16 *data, uae_u16 *datb, unsigned int ctl, unsigned int ctlx)
{
    struct sprite_entry *e = curr_sprite_entries + next_sprite_entry;
    unsigned int i;
    int word_offs;
    uae_u16 *buf;
    uae_u32 collision_mask;
    unsigned int width = sprite_width;
    int dbl = 0, half = 0;
    unsigned int mask = 0;

#ifdef OS_WITHOUT_MEMORY_MANAGEMENT
    if (e >= &curr_sprite_entries[max_sprite_entry-1]) {
	/* We need to re-allocate the sprite_entries table */
	delta_sprite_entry++;
	return;
    }
#endif

    if (sprres != RES_LORES)
	thisline_decision.any_hires_sprites = 1;

#ifdef AGA
    if (currprefs.chipset_mask & CSMASK_AGA) {
	width = (width << 1) >> sprres;
	dbl = sprite_buffer_res - sprres;
	if (dbl < 0) {
	    half = -dbl;
	    dbl = 0;
	}
	mask = sprres == RES_SUPERHIRES ? 1 : 0;
    }
#endif

    /* Try to coalesce entries if they aren't too far apart.  */
    if (! next_sprite_forced && e[-1].max + 16 >= sprxp) {
	e--;
    } else {
	next_sprite_entry++;
	e->pos = sprxp;
	e->has_attached = 0;
    }

    if (sprxp < e->pos)
	uae_abort ("sprxp < e->pos");

    e->max = sprxp + width;
    e[1].first_pixel = e->first_pixel + ((e->max - e->pos + 3) & ~3);
    next_sprite_forced = 0;

    collision_mask = clxmask[clxcon >> 12];
    word_offs = e->first_pixel + sprxp - e->pos;

    for (i = 0; i < sprite_width; i += 16) {
	unsigned int da = *data;
	unsigned int db = *datb;
	uae_u32 datab = ((sprtaba[da & 0xFF] << 16) | sprtaba[da >> 8]
			 | (sprtabb[db & 0xFF] << 16) | sprtabb[db >> 8]);

	buf = spixels + word_offs + ((i << dbl) >> half);
	if (currprefs.collision_level > 0 && collision_mask)
	    record_sprite_1 (buf, datab, num, dbl, mask, 1, collision_mask);
	else
	    record_sprite_1 (buf, datab, num, dbl, mask, 0, collision_mask);
	data++;
	datb++;
    }

    /* We have 8 bits per pixel in spixstate, two for every sprite pair.  The
       low order bit records whether the attach bit was set for this pair.  */
    if ((sprctl[num] & 0x80) || (!(currprefs.chipset_mask & CSMASK_AGA) && (sprctl[num ^ 1] & 0x80))) {
	uae_u32 state = 0x01010101 << (num & ~1);
	uae_u32 *stbuf = spixstate.words + (word_offs >> 2);
	uae_u8 *stb1 = spixstate.bytes + word_offs;
	for (i = 0; i < width; i += 8) {
	    stb1[0] |= state;
	    stb1[1] |= state;
	    stb1[2] |= state;
	    stb1[3] |= state;
	    stb1[4] |= state;
	    stb1[5] |= state;
	    stb1[6] |= state;
	    stb1[7] |= state;
	    stb1 += 8;
	}
	e->has_attached = 1;
    }
}

/****i* januae-custom-raster/decide_sprites
 *
 *   NAME
 *       decide_sprites()
 *
 *   SYNOPSIS
 *       static void decide_sprites (unsigned int hpos)
 *
 *   FUNCTION
 *       Collect armed sprites crossing the beam position and record them in X
 *       order.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *
 *   NOTES
 *       Skips inactive, out-of-window, and already processed sprites; equal X
 *       positions are ordered by descending sprite number.
 *
 ****************************************************************************/

static void decide_sprites (unsigned int hpos)
{
    unsigned int nrs[MAX_SPRITES], posns[MAX_SPRITES];
    unsigned int count, i;
     /* apparently writes to custom registers happen in the 3/4th of cycle
      * and sprite xpos comparator sees it immediately */
    unsigned int point = hpos * 2 - 3;
    unsigned int width = sprite_width;
    unsigned int window_width = (width << lores_shift) >> sprres;

    if (nodraw () || hpos < 0x14 || nr_armed == 0 || point == last_sprite_point)
	return;

    decide_diw (hpos);
    decide_line (hpos);

    count = 0;
    for (i = 0; i < MAX_SPRITES; i++) {
	unsigned int sprxp = spr[i].xpos;
	unsigned int hw_xp = (sprxp >> sprite_buffer_res);
	int window_xp = coord_hw_to_window_x (hw_xp) + (DIW_DDF_OFFSET << lores_shift);
	unsigned int j;
	unsigned int bestp;

#if DEBUGGER
	if (!((debug_sprite_mask) & (1 << i)))
	    continue;
#endif

	if (! spr[i].armed || hw_xp <= last_sprite_point || hw_xp > point)
	    continue;
	if ( !(bplcon3 & 2) && /* sprites outside playfields enabled? */
	    ((thisline_decision.diwfirstword >= 0 && window_xp + (int)window_width < thisline_decision.diwfirstword)
	    || (thisline_decision.diwlastword >= 0 && window_xp > thisline_decision.diwlastword)))
	    continue;

	/* Sort the sprites in order of ascending X position before recording them.  */
	for (bestp = 0; bestp < count; bestp++) {
	    if (posns[bestp] > sprxp)
		break;
	    if (posns[bestp] == sprxp && nrs[bestp] < i)
		break;
	}
	for (j = count; j > bestp; j--) {
	    posns[j] = posns[j-1];
	    nrs[j] = nrs[j-1];
	}
	posns[j] = sprxp;
	nrs[j] = i;
	count++;
    }
    for (i = 0; i < count; i++) {
	unsigned int nr = nrs[i];
	record_sprite (next_lineno, nr, spr[nr].xpos, sprdata[nr], sprdatb[nr], sprctl[nr], sprctl[nr ^ 1]);
    }
    last_sprite_point = point;
}

/****i* januae-custom-raster/sprites_differ
 *
 *   NAME
 *       sprites_differ()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int sprites_differ (struct draw_info *dip, struct
 *       draw_info *dip_old)
 *
 *   FUNCTION
 *       Compare current and previous scanline sprite entries and pixel buffers.
 *
 *   INPUTS
 *       dip -- current line drawing information
 *       dip_old -- previous-frame line drawing information
 *
 *   RESULT
 *       Returns 1 when sprite count, geometry, attach data or pixels differ;
 *       otherwise 0.
 *
 *   NOTES
 *       Checks count, entry geometry, attach flags, pixel words and
 *       attachment-state bytes.
 *
 ****************************************************************************/

STATIC_INLINE int sprites_differ (struct draw_info *dip, struct draw_info *dip_old)
{
    struct sprite_entry *this_first = curr_sprite_entries + dip->first_sprite_entry;
    struct sprite_entry *this_last = curr_sprite_entries + dip->last_sprite_entry;
    struct sprite_entry *prev_first = prev_sprite_entries + dip_old->first_sprite_entry;
    int npixels;
    int i;

    if (dip->nr_sprites != dip_old->nr_sprites)
	return 1;

    if (dip->nr_sprites == 0)
	return 0;

    for (i = 0; i < dip->nr_sprites; i++)
	if (this_first[i].pos != prev_first[i].pos
	    || this_first[i].max != prev_first[i].max
	    || this_first[i].has_attached != prev_first[i].has_attached)
	    return 1;

    npixels = this_last->first_pixel + (this_last->max - this_last->pos) - this_first->first_pixel;
    if (memcmp (spixels + this_first->first_pixel, spixels + prev_first->first_pixel,
		npixels * sizeof (uae_u16)) != 0)
	return 1;
    if (memcmp (spixstate.bytes + this_first->first_pixel, spixstate.bytes + prev_first->first_pixel, npixels) != 0)
	return 1;
    return 0;
}

/****i* januae-custom-raster/color_changes_differ
 *
 *   NAME
 *       color_changes_differ()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int color_changes_differ (struct draw_info *dip, struct
 *       draw_info *dip_old)
 *
 *   FUNCTION
 *       Compare the number and contents of current versus prior line color
 *       changes.
 *
 *   INPUTS
 *       dip -- current line drawing information
 *       dip_old -- previous-frame line drawing information
 *
 *   RESULT
 *       Returns 1 when the line color-change lists differ; otherwise 0.
 *
 *   NOTES
 *       Performs a raw memcmp of the recorded color-change entries when their
 *       counts are nonzero.
 *
 ****************************************************************************/

STATIC_INLINE int color_changes_differ (struct draw_info *dip, struct draw_info *dip_old)
{
    if (dip->nr_color_changes != dip_old->nr_color_changes)
	return 1;

    if (dip->nr_color_changes == 0)
	return 0;
    if (memcmp (curr_color_changes + dip->first_color_change,
		prev_color_changes + dip_old->first_color_change,
		dip->nr_color_changes * sizeof *curr_color_changes) != 0)
	return 1;
    return 0;
}

/* End of a horizontal scan line. Finish off all decisions that were not
 * made yet. */
/****i* januae-custom-raster/finish_decisions
 *
 *   NAME
 *       finish_decisions()
 *
 *   SYNOPSIS
 *       static void finish_decisions (void)
 *
 *   FUNCTION
 *       Finalize DIW, bitplane fetch and sprite decisions for the current
 *       scanline.
 *
 *   NOTES
 *       Marks a line changed when its geometry, color changes or sprite state
 *       differs from the preceding frame; incomplete fetches are diagnosed
 *       rather than silently repaired.
 *
 ****************************************************************************/

static void finish_decisions (void)
{
    struct draw_info *dip;
    struct draw_info *dip_old;
    struct decision *dp;
    int changed;
    int hpos = current_hpos ();

    if (nodraw ())
	return;

    decide_diw (hpos);
    decide_line (hpos);
    decide_fetch (hpos);

    if (thisline_decision.plfleft != -1 && thisline_decision.plflinelen == -1) {
	if (fetch_state != fetch_not_started) {
	    write_log("fetch_state=%d plfleft=%d,len=%d,vpos=%d,hpos=%d\n",
		fetch_state, thisline_decision.plfleft, thisline_decision.plflinelen,
		vpos, hpos);
	    uae_abort ("fetch_state != fetch_not_started");
	}
	thisline_decision.plfright = thisline_decision.plfleft;
	thisline_decision.plflinelen = 0;
	thisline_decision.bplres = RES_LORES;
    }

    /* Large DIWSTOP values can cause the stop position never to be
     * reached, so the state machine always stays in the same state and
     * there's a more-or-less full-screen DIW. */
    if (hdiwstate == DIW_waiting_stop || thisline_decision.diwlastword > max_diwlastword)
	thisline_decision.diwlastword = max_diwlastword;

    if (thisline_decision.diwfirstword != line_decisions[next_lineno].diwfirstword)
	MARK_LINE_CHANGED;
    if (thisline_decision.diwlastword != line_decisions[next_lineno].diwlastword)
	MARK_LINE_CHANGED;

    dip = curr_drawinfo + next_lineno;
    dip_old = prev_drawinfo + next_lineno;
    dp = line_decisions + next_lineno;
    dp->valid = 0;
    changed = thisline_changed + interlace_started;

    if (thisline_decision.plfleft != -1)
	record_diw_line (diwfirstword, diwlastword);

    if (thisline_decision.plfleft != -1 || (bplcon3 & 2))
	decide_sprites (hpos);

    dip->last_sprite_entry = next_sprite_entry;
    dip->last_color_change = next_color_change;

    if (thisline_decision.ctable == -1) {
	if (thisline_decision.plfleft == -1)
	    remember_ctable_for_border ();
	else
	    remember_ctable ();
    }

    dip->nr_color_changes = next_color_change - dip->first_color_change;
    dip->nr_sprites = next_sprite_entry - dip->first_sprite_entry;

    if (thisline_decision.plfleft != line_decisions[next_lineno].plfleft)
	changed = 1;
    if (! changed && color_changes_differ (dip, dip_old))
	changed = 1;
    if (!changed && thisline_decision.plfleft != -1 && sprites_differ (dip, dip_old))
	changed = 1;

    if (changed) {
	thisline_changed = 1;
	*dp = thisline_decision;
	dp->valid = 1;
    } else
	/* The only one that may differ: */
	dp->ctable = thisline_decision.ctable;
}

/* Set the state of all decisions to "undecided" for a new scanline. */
/****i* januae-custom-raster/reset_decisions
 *
 *   NAME
 *       reset_decisions()
 *
 *   SYNOPSIS
 *       static void reset_decisions (void)
 *
 *   FUNCTION
 *       Reset per-line drawing and fetch decisions before processing a new
 *       scanline.
 *
 *   NOTES
 *       Clears bitplane buffers, line comparison cursors and fetch state;
 *       preserves current BPLCON values for subsequent comparisons.
 *
 ****************************************************************************/

static void reset_decisions (void)
{
    if (nodraw ())
	return;

    thisline_decision.bplres = GET_RES (bplcon0);
    thisline_decision.any_hires_sprites = 0;
    thisline_decision.nr_planes = 0;

    thisline_decision.plfleft = -1;
    thisline_decision.plflinelen = -1;
    thisline_decision.ham_seen = !! (bplcon0 & 0x800);
    thisline_decision.ham_at_start = !! (bplcon0 & 0x800);

    /* decided_res shouldn't be touched before it's initialized by decide_line(). */
    thisline_decision.diwfirstword = -1;
    thisline_decision.diwlastword = -1;
    if (hdiwstate == DIW_waiting_stop) {
	thisline_decision.diwfirstword = 0;
	if (thisline_decision.diwfirstword != line_decisions[next_lineno].diwfirstword)
	    MARK_LINE_CHANGED;
    }
    thisline_decision.ctable = -1;

    thisline_changed = 0;
    curr_drawinfo[next_lineno].first_color_change = next_color_change;
    curr_drawinfo[next_lineno].first_sprite_entry = next_sprite_entry;
    next_sprite_forced = 1;

    last_sprite_point = 0;
    fetch_state = fetch_not_started;
    passed_plfstop = 0;

    memset (todisplay, 0, sizeof todisplay);
    memset (fetched, 0, sizeof fetched);
#ifdef AGA
    memset (fetched_aga0, 0, sizeof fetched_aga0);
    memset (fetched_aga1, 0, sizeof fetched_aga1);
#endif
    memset (outword, 0, sizeof outword);

    last_decide_line_hpos = -1;
    last_sprite_decide_line_hpos = -1;
    last_diw_pix_hpos = -1;
    last_ddf_pix_hpos = -1;
    last_sprite_hpos = 0;
    last_fetch_hpos = -1;

    /* These are for comparison. */
    thisline_decision.bplcon0 = bplcon0;
    thisline_decision.bplcon2 = bplcon2;
#ifdef AGA
    thisline_decision.bplcon3 = bplcon3;
    thisline_decision.bplcon4 = bplcon4;
#endif
}

int vsynctime_orig;
int turbo_emulation;

/****i* januae-custom-raster/compute_vsynctime
 *
 *   NAME
 *       compute_vsynctime()
 *
 *   SYNOPSIS
 *       void compute_vsynctime (void)
 *
 *   FUNCTION
 *       Calculate frame pacing from configured refresh rate and emulation
 *       timing.
 *
 *   NOTES
 *       In turbo mode sets both timing values to one. Otherwise divides syncbase
 *       by fake_vblank_hz and optionally refreshes sound and rendering backends.
 *
 ****************************************************************************/

void compute_vsynctime (void)
{
    fake_vblank_hz = 0;
    if (currprefs.chipset_refreshrate) {
	vblank_hz = currprefs.chipset_refreshrate;
	if (is_vsync ()) {
	    vblank_skip = 1;
	    if (!fake_vblank_hz && vblank_hz > 85) {
		vblank_hz /= 2;
		vblank_skip = -1;
	    }
	}
    }
    if (!fake_vblank_hz)
	fake_vblank_hz = vblank_hz;
    if (turbo_emulation)
	vsynctime = vsynctime_orig = 1;
    else
	vsynctime = vsynctime_orig = syncbase / fake_vblank_hz;
#ifdef OPENGL
    OGL_refresh ();
#endif
#ifdef D3D
    D3D_refresh ();
#endif
    if (currprefs.produce_sound > 1)
	update_sound (fake_vblank_hz);
}


/****i* januae-custom-raster/dumpsync
 *
 *   NAME
 *       dumpsync()
 *
 *   SYNOPSIS
 *       static void dumpsync (void)
 *
 *   FUNCTION
 *       Log the current programmable synchronization register state.
 *
 *   NOTES
 *       Uses a static counter and stops logging after the initial limited
 *       diagnostic emissions.
 *
 ****************************************************************************/

static void dumpsync (void)
{
    static int cnt = 10;
    if (cnt < 0)
	return;
    cnt--;
    write_log ("BEAMCON0 = %04.4X VTOTAL=%04.4X HTOTAL=%04.4X\n", new_beamcon0, vtotal, htotal);
    write_log ("HSSTOP=%04.4X HBSTRT=%04.4X HBSTOP=%04.4X\n", hsstop, hbstrt, hbstop);
    write_log ("VSSTOP=%04.4X VBSTRT=%04.4X VBSTOP=%04.4X\n", vsstop, vbstrt, vbstop);
    write_log ("HSSTRT=%04.4X VSSTRT=%04.4X HCENTER=%04.4X\n", hsstrt, vsstrt, hcenter);
}

/* set PAL or NTSC timing variables */
/****i* januae-custom-raster/init_hz
 *
 *   NAME
 *       init_hz()
 *
 *   SYNOPSIS
 *       void init_hz (void)
 *
 *   FUNCTION
 *       Initialize PAL, NTSC or programmable beam timing and reschedule
 *       horizontal sync.
 *
 *   NOTES
 *       Clamps refresh rate to 10 through 300 Hz, sets event timing and calls
 *       compute_vsynctime(); configured and active preferences can be changed.
 *
 ****************************************************************************/

void init_hz (void)
{
    int isntsc;

    if ((currprefs.chipset_refreshrate == 50 && !currprefs.ntscmode) ||
	(currprefs.chipset_refreshrate == 60 && currprefs.ntscmode)) {
	currprefs.chipset_refreshrate = 0;
	changed_prefs.chipset_refreshrate = 0;
    }
    if (is_vsync ()) {
	currprefs.chipset_refreshrate = abs (currprefs.gfx_refreshrate);
	changed_prefs.chipset_refreshrate = abs (currprefs.gfx_refreshrate);
    }

    beamcon0 = new_beamcon0;
    isntsc = beamcon0 & 0x20 ? 0 : 1;
    if (hack_vpos > 0) {
	if ((int)maxvpos == hack_vpos)
	    return;
	maxvpos = hack_vpos;
	vblank_hz = 15600 / hack_vpos;
	hack_vpos = -1;
    } else if (hack_vpos < 0) {
	hack_vpos = 0;
    }
    if (hack_vpos == 0) {
	if (!isntsc) {
	    maxvpos = MAXVPOS_PAL;
	    maxhpos = MAXHPOS_PAL;
	    minfirstline = VBLANK_ENDLINE_PAL;
	    vblank_hz = VBLANK_HZ_PAL;
	    sprite_vblank_endline = VBLANK_SPRITE_PAL;
	} else {
	    maxvpos = MAXVPOS_NTSC;
	    maxhpos = MAXHPOS_NTSC;
	    minfirstline = VBLANK_ENDLINE_NTSC;
	    vblank_hz = VBLANK_HZ_NTSC;
	    sprite_vblank_endline = VBLANK_SPRITE_NTSC;
	}
    }
    if (beamcon0 & 0x80) {
	if (vtotal >= MAXVPOS)
	    vtotal = MAXVPOS - 1;
	maxvpos = vtotal + 1;
	if (htotal >= MAXHPOS)
	    htotal = MAXHPOS - 1;
	maxhpos = htotal + 1;
	vblank_hz = 227 * 312 * 50 / (maxvpos * maxhpos);
	minfirstline = vsstop;
	if (minfirstline < 2)
	    minfirstline = 2;
	sprite_vblank_endline = minfirstline - 2;
	dumpsync ();
    }
    /* limit to sane values */
    if (vblank_hz < 10)
	vblank_hz = 10;
    if (vblank_hz > 300)
	vblank_hz = 300;
    eventtab[ev_hsync].oldcycles = get_cycles ();
    eventtab[ev_hsync].evtime = get_cycles() + HSYNCTIME;
    events_schedule ();
    compute_vsynctime ();
#ifdef OPENGL
    OGL_refresh ();
#endif
#ifdef PICASSO96
    init_hz_p96 ();
#endif
    write_log ("%s mode, %dHz (h=%d v=%d)\n",
	isntsc ? "NTSC" : "PAL", vblank_hz, maxhpos, maxvpos);
}

/****i* januae-custom-raster/calcdiw
 *
 *   NAME
 *       calcdiw()
 *
 *   SYNOPSIS
 *       static void calcdiw (void)
 *
 *   FUNCTION
 *       Derive display-window and fetch bounds from DIW/DDF register values.
 *
 *   NOTES
 *       Uses DIWHIGH extension bits only after they were written; clamps left
 *       window edge and adjusts ECS Agnus fetch start.
 *
 ****************************************************************************/

static void calcdiw (void)
{
    unsigned int hstrt = diwstrt & 0xFF;
    unsigned int hstop = diwstop & 0xFF;
    unsigned int vstrt = diwstrt >> 8;
    unsigned int vstop = diwstop >> 8;

    if (diwhigh_written) {
	hstrt |= ((diwhigh >> 5) & 1) << 8;
	hstop |= ((diwhigh >> 13) & 1) << 8;
	vstrt |= (diwhigh & 7) << 8;
	vstop |= ((diwhigh >> 8) & 7) << 8;
    } else {
	hstop += 0x100;
	if ((vstop & 0x80) == 0)
	    vstop |= 0x100;
    }

    diwfirstword = coord_diw_to_window_x (hstrt);
    diwlastword = coord_diw_to_window_x (hstop);
    if (diwfirstword >= diwlastword) {
	diwfirstword = 0;
	diwlastword = max_diwlastword;
    }
    if (diwfirstword < 0)
	diwfirstword = 0;

    plffirstline = vstrt;
    plflastline = vstop;



    plfstrt = ddfstrt;
    plfstop = ddfstop;
    /* probably not the correct place.. */
    if (currprefs.chipset_mask & CSMASK_ECS_AGNUS) {
	if (ddfstop > maxhpos)
	    plfstrt = 0;
	if (plfstrt < HARD_DDF_START)
	    plfstrt = HARD_DDF_START;
    }
}

/* display mode changed (lores, doubling etc..), recalculate everything */
/****i* januae-custom-raster/init_custom
 *
 *   NAME
 *       init_custom()
 *
 *   SYNOPSIS
 *       void init_custom (void)
 *
 *   FUNCTION
 *       Reinitialize drawing tables and raster timing after a display-mode
 *       change.
 *
 *   NOTES
 *       Runs cycle-diagram creation, drawing reset, PAL/NTSC timing setup, and
 *       display-window recalculation in that order.
 *
 ****************************************************************************/

void init_custom (void)
{
    create_cycle_diagram_table ();
    reset_drawing ();
    init_hz ();
    calcdiw ();
}

static int timehack_alive = 0;

/****i* januae-custom-raster/timehack_helper
 *
 *   NAME
 *       timehack_helper()
 *
 *   SYNOPSIS
 *       static uae_u32 REGPARAM2 timehack_helper (TrapContext *context)
 *
 *   FUNCTION
 *       Expose a small guest trap for host wall-clock time and the timehack
 *       liveness flag.
 *
 *   INPUTS
 *       context -- guest CPU trap context
 *
 *   RESULT
 *       Returns timehack_alive for a D0==0 query or 0 after a successful clock
 *       write; returns 2 without HAVE_GETTIMEOFDAY.
 *
 *   NOTES
 *       When gettimeofday is available, D0==0 returns timehack_alive; otherwise
 *       writes time relative to the Amiga epoch into the guest buffer addressed
 *       by A0. Without gettimeofday returns 2.
 *
 ****************************************************************************/

static uae_u32 REGPARAM2 timehack_helper (TrapContext *context)
{
#ifdef HAVE_GETTIMEOFDAY
    struct timeval tv;
    if (m68k_dreg (&context->regs, 0) == 0)
	return timehack_alive;

    timehack_alive = 10;

    gettimeofday (&tv, NULL);
    put_long (m68k_areg (&context->regs, 0), tv.tv_sec - (((365 * 8 + 2) * 24) * 60 * 60));
    put_long (m68k_areg (&context->regs, 0) + 4, tv.tv_usec);
    return 0;
#else
    return 2;
#endif
}

/****************************************
 * we want a lot of commands with one
 * helper. So we create a dispatcher
 * here.
 ****************************************/

struct Window *native_window;

/****i* januae-custom-raster/copyonepixel
 *
 *   NAME
 *       copyonepixel()
 *
 *   SYNOPSIS
 *       static void copyonepixel (PLANEPTR src, ULONG xsrc, PLANEPTR dest, ULONG
 *       xdest, ULONG minterm)
 *
 *   FUNCTION
 *       Apply the selected minterm to a single source and destination bitplane
 *       pixel.
 *
 *   INPUTS
 *       src -- source plane pointer; NULL supplies zero and (PLANEPTR)-1
 *           supplies one
 *       xsrc -- horizontal bit coordinate in source bitplane
 *       dest -- valid writable destination bitplane
 *       xdest -- horizontal bit coordinate in destination bitplane
 *       minterm -- boolean pixel-combine minterm bits
 *
 *   NOTES
 *       A null source means zero and source (PLANEPTR)-1 means one; destination
 *       must be a valid writable bitplane. Only minterm bits 0x10 to 0x80 are
 *       used.
 *
 ****************************************************************************/

static void copyonepixel (PLANEPTR src, ULONG xsrc, PLANEPTR dest, ULONG xdest,
	ULONG minterm)
{
    ULONG sByte, sSet;
    ULONG dByte, dSet;
    UBYTE sBit;
    UBYTE dBit;
    BOOL set;

    if (src == NULL)
    {
        sSet = FALSE;
    } else if (src == (PLANEPTR)-1)
    {
        sSet = TRUE;
    } else {
	sByte = xsrc >> 3;
	sBit = 1L << (7 - (xsrc & 0x07));
	sSet = (src[sByte] & sBit) != 0;
    }
    
    /* dest PLANEPTR here will never be NULL or -1 */
    dByte = xdest >> 3;
    dBit = 1L << (7 - (xdest & 0x07));
    dSet = (dest[dByte] & dBit) != 0;

    set = 0;

    if (minterm & 0x0010)
    {
	if (!sSet && !dSet)
	    set = 1;
    }
    if (minterm & 0x0020)
    {
	if (!sSet && dSet)
	    set = 1;
    }
    if (minterm & 0x0040)
    {
	if (sSet && !dSet)
	    set = 1;
    }
    if (minterm & 0x0080)
    {
	if (sSet && dSet)
	    set = 1;
    }

    if (set)
	dest[dByte] |= dBit;
    else
	dest[dByte] &= ~dBit;
}

 /*
  * register functions
  */
/****i* januae-custom-raster/DENISEID
 *
 *   NAME
 *       DENISEID()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u16 DENISEID (void)
 *
 *   FUNCTION
 *       Report the emulated Denise identity value for the active chipset.
 *
 *   RESULT
 *       Returns 0xF8 for AGA, 0xFC for ECS Denise or 0xFFFF otherwise.
 *
 *   NOTES
 *       Returns 0xF8 for AGA, 0xFC for ECS Denise, or 0xFFFF for the remaining
 *       case.
 *
 ****************************************************************************/

STATIC_INLINE uae_u16 DENISEID (void)
{
#ifdef AGA
    if (currprefs.chipset_mask & CSMASK_AGA)
	return 0xF8;
#endif
    if (currprefs.chipset_mask & CSMASK_ECS_DENISE)
	return 0xFC;
    return 0xffff;
}
/****i* januae-custom-raster/DMACONR
 *
 *   NAME
 *       DMACONR()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u16 DMACONR (void)
 *
 *   FUNCTION
 *       Read the DMA control register augmented with current blitter status.
 *
 *   RESULT
 *       Returns DMA control with the current blitter busy and zero status bits.
 *
 *   NOTES
 *       Synchronizes the blitter first; sets busy bit 0x4000 unless done, and
 *       zero flag bit 0x2000 when blt_info.blitzero is set.
 *
 ****************************************************************************/

STATIC_INLINE uae_u16 DMACONR (void)
{
    uae_u16 v;
    decide_blitter (current_hpos ());
    v = dmacon | (bltstate == BLT_done ? 0 : 0x4000)
	    | (blt_info.blitzero ? 0x2000 : 0);
    return v;
}
/****i* januae-custom-raster/INTENAR
 *
 *   NAME
 *       INTENAR()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u16 INTENAR (void)
 *
 *   FUNCTION
 *       Read the current interrupt enable mask.
 *
 *   RESULT
 *       Returns the current interrupt enable mask.
 *
 *   NOTES
 *       Returns intena without side effects.
 *
 ****************************************************************************/

STATIC_INLINE uae_u16 INTENAR (void)
{
    return intena;
}
/****i* januae-custom-raster/INTREQR
 *
 *   NAME
 *       INTREQR()
 *
 *   SYNOPSIS
 *       uae_u16 INTREQR (void)
 *
 *   FUNCTION
 *       Read the current interrupt request mask.
 *
 *   RESULT
 *       Returns the current interrupt request mask.
 *
 *   NOTES
 *       Returns intreq without side effects.
 *
 ****************************************************************************/

uae_u16 INTREQR (void)
{
    return intreq;
}
/****i* januae-custom-raster/ADKCONR
 *
 *   NAME
 *       ADKCONR()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u16 ADKCONR (void)
 *
 *   FUNCTION
 *       Read the audio/disk control register image.
 *
 *   RESULT
 *       Returns the current ADKCON state.
 *
 *   NOTES
 *       Returns adkcon without side effects.
 *
 ****************************************************************************/

STATIC_INLINE uae_u16 ADKCONR (void)
{
    return adkcon;
}

/****i* januae-custom-raster/GETVPOS
 *
 *   NAME
 *       GETVPOS()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int GETVPOS(void)
 *
 *   FUNCTION
 *       Select the effective vertical beam position for custom-register reads.
 *
 *   RESULT
 *       Returns the selected vertical beam position.
 *
 *   NOTES
 *       Prefers latched lightpen position when positive; otherwise can use the
 *       previous position in genlock-related mode.
 *
 ****************************************************************************/

STATIC_INLINE int GETVPOS(void)
{
    return vpos_lpen > 0 ? vpos_lpen : (((bplcon0 & 2) && !currprefs.genlock) ? vpos_previous : (int)vpos);
}
/****i* januae-custom-raster/GETHPOS
 *
 *   NAME
 *       GETHPOS()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int GETHPOS(void)
 *
 *   FUNCTION
 *       Select the effective horizontal beam position for custom-register reads.
 *
 *   RESULT
 *       Returns the selected horizontal beam position.
 *
 *   NOTES
 *       Prefers latched lightpen position when positive; otherwise uses prior
 *       beam position or current_hpos().
 *
 ****************************************************************************/

STATIC_INLINE int GETHPOS(void)
{
    return vpos_lpen > 0 ? hpos_lpen : (((bplcon0 & 2) && !currprefs.genlock) ? hpos_previous : (int)current_hpos ());
}

/****i* januae-custom-raster/VPOSR
 *
 *   NAME
 *       VPOSR()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u16 VPOSR (void)
 *
 *   FUNCTION
 *       Synthesize the VPOSR register from vertical beam, interlace and chipset
 *       bits.
 *
 *   RESULT
 *       Returns the constructed 16-bit vertical position and
 *       chipset-identification register.
 *
 *   NOTES
 *       Applies OCS/ECS vertical high-bit limits and optional AGA chip ID bits;
 *       hsyncdelay() currently has no effect.
 *
 ****************************************************************************/

STATIC_INLINE uae_u16 VPOSR (void)
{
    unsigned int csbit = currprefs.ntscmode ? 0x1000 : 0;
    unsigned int vp = (GETVPOS() >> 8) & 7;
#ifdef AGA
    csbit |= (currprefs.chipset_mask & CSMASK_AGA) ? 0x2300 : 0;
#endif
    csbit |= (currprefs.chipset_mask & CSMASK_ECS_AGNUS) ? 0x2000 : 0;
    if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	vp &= 1;
    vp = vp | lof | csbit;
    if (currprefs.cpu_level >= 2)
	hsyncdelay ();
    return vp;
}
/****i* januae-custom-raster/VPOSW
 *
 *   NAME
 *       VPOSW()
 *
 *   SYNOPSIS
 *       static void VPOSW (uae_u16 v)
 *
 *   FUNCTION
 *       Apply a guest write to the vertical-position and long-frame flag
 *       register.
 *
 *   INPUTS
 *       v -- new register value
 *
 *   NOTES
 *       Updates LOF and marks LOF changes; bit zero can activate the hack_vpos
 *       override when the beam is beyond line zero.
 *
 ****************************************************************************/

static void VPOSW (uae_u16 v)
{
    if (lof != (v & 0x8000))
	lof_changed = 1;
    lof = v & 0x8000;
    if ( (v & 1) && vpos > 0)
	hack_vpos = vpos;
}

/****i* januae-custom-raster/VHPOSR
 *
 *   NAME
 *       VHPOSR()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u16 VHPOSR (void)
 *
 *   FUNCTION
 *       Read the combined low-byte vertical and horizontal beam positions.
 *
 *   RESULT
 *       Returns the low eight bits of the vertical position as the high byte and
 *       the low eight bits of horizontal position as the low byte.
 *
 *   NOTES
 *       Uses GETVPOS() and GETHPOS(); the optional CPU-level horizontal delay
 *       hook is a no-op.
 *
 ****************************************************************************/

STATIC_INLINE uae_u16 VHPOSR (void)
{
    uae_u16 vp = GETVPOS();
    uae_u16 hp = GETHPOS();
    vp <<= 8;
    vp |= hp;
    if (currprefs.cpu_level >= 2)
	hsyncdelay ();
    return vp;
}

/****i* januae-custom-raster/COP1LCH
 *
 *   NAME
 *       COP1LCH()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void COP1LCH (uae_u16 v)
 *
 *   FUNCTION
 *       Update the high 16 bits of the Copper 1 list pointer.
 *
 *   INPUTS
 *       v -- new register value
 *
 *   NOTES
 *       Retains its existing low halfword.
 *
 ****************************************************************************/

STATIC_INLINE void COP1LCH (uae_u16 v) { cop1lc = (cop1lc & 0xffff) | ((uae_u32)v << 16); }
/****i* januae-custom-raster/COP1LCL
 *
 *   NAME
 *       COP1LCL()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void COP1LCL (uae_u16 v)
 *
 *   FUNCTION
 *       Update the low 16 bits of the Copper 1 list pointer.
 *
 *   INPUTS
 *       v -- new register value
 *
 *   NOTES
 *       Forces bit zero to zero, keeping the Copper list pointer even.
 *
 ****************************************************************************/

STATIC_INLINE void COP1LCL (uae_u16 v) { cop1lc = (cop1lc & ~0xffff) | (v & 0xfffe); }
/****i* januae-custom-raster/COP2LCH
 *
 *   NAME
 *       COP2LCH()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void COP2LCH (uae_u16 v)
 *
 *   FUNCTION
 *       Update the high 16 bits of the Copper 2 list pointer.
 *
 *   INPUTS
 *       v -- new register value
 *
 *   NOTES
 *       Retains its existing low halfword.
 *
 ****************************************************************************/

STATIC_INLINE void COP2LCH (uae_u16 v) { cop2lc = (cop2lc & 0xffff) | ((uae_u32)v << 16); }
/****i* januae-custom-raster/COP2LCL
 *
 *   NAME
 *       COP2LCL()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void COP2LCL (uae_u16 v)
 *
 *   FUNCTION
 *       Update the low 16 bits of the Copper 2 list pointer.
 *
 *   INPUTS
 *       v -- new register value
 *
 *   NOTES
 *       Forces bit zero to zero, keeping the Copper list pointer even.
 *
 ****************************************************************************/

STATIC_INLINE void COP2LCL (uae_u16 v) { cop2lc = (cop2lc & ~0xffff) | (v & 0xfffe); }

/****i* januae-custom-raster/COPJMP
 *
 *   NAME
 *       COPJMP()
 *
 *   SYNOPSIS
 *       static void COPJMP (int num)
 *
 *   FUNCTION
 *       Strobe a Copper jump and restart the instruction-fetch state.
 *
 *   INPUTS
 *       num -- Copper strobe selector 1 or 2
 *
 *   NOTES
 *       Cancels the pending Copper event, records the strobe number, and
 *       schedules CPU Copper handling only when Copper DMA is enabled.
 *
 ****************************************************************************/

static void COPJMP (int num)
{
    int was_active = eventtab[ev_copper].active;
    int oldstrobe = cop_state.strobe;

    eventtab[ev_copper].active = 0;
    if (was_active)
	events_schedule ();

    unset_special (&regs, SPCFLAG_COPPER);
    cop_state.ignore_next = 0;
    if (!oldstrobe)
	cop_state.state_prev = cop_state.state;
    cop_state.state = COP_read1;
    cop_state.vpos = vpos;
    cop_state.hpos = current_hpos () & ~1;
    copper_enabled_thisline = 0;
    cop_state.strobe = num;

    if (dmaen (DMA_COPPER)) {
	copper_enabled_thisline = 1;
	set_special (&regs, SPCFLAG_COPPER);
    } else if (oldstrobe > 0 && oldstrobe != num && cop_state.state_prev == COP_wait) {
	/* dma disabled, copper idle and accessing both COPxJMPs -> copper stops! */
	cop_state.state = COP_stop;
    }
}

/****i* januae-custom-raster/COPCON
 *
 *   NAME
 *       COPCON()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void COPCON (uae_u16 a)
 *
 *   FUNCTION
 *       Store the writable Copper control register state.
 *
 *   INPUTS
 *       a -- new Copper control value
 *
 *   NOTES
 *       Assigns the 16-bit value directly to copcon.
 *
 ****************************************************************************/

STATIC_INLINE void COPCON (uae_u16 a)
{
    copcon = a;
}

static void compute_spcflag_copper (void);
/****i* januae-custom-raster/DMACON
 *
 *   NAME
 *       DMACON()
 *
 *   SYNOPSIS
 *       static void DMACON (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Apply DMA set/clear bits and resynchronize dependent hardware state.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Masks writable state to 0x1FFF and updates Copper, blitter, audio,
 *       bitplane-fetch, and event scheduling as relevant.
 *
 ****************************************************************************/

static void DMACON (unsigned int hpos, uae_u16 v)
{
    int oldcop, newcop;
    uae_u16 changed;

    uae_u16 oldcon = dmacon;

    decide_line (hpos);
    decide_fetch (hpos);
    decide_blitter (hpos);

    setclr (&dmacon, v);
    dmacon &= 0x1FFF;

    changed = dmacon ^ oldcon;

    oldcop = (oldcon & DMA_COPPER) && (oldcon & DMA_MASTER);
    newcop = (dmacon & DMA_COPPER) && (dmacon & DMA_MASTER);

    if (oldcop != newcop) {
	eventtab[ev_copper].active = 0;
	if (newcop && !oldcop) {
	    compute_spcflag_copper ();
	} else if (!newcop) {
	    copper_enabled_thisline = 0;
	    unset_special (&regs, SPCFLAG_COPPER);
	}
    }
    if ((dmacon & DMA_BLITPRI) > (oldcon & DMA_BLITPRI) && bltstate != BLT_done) {
	static int count = 0;
	if (!count) {
	    count = 1;
	    write_log ("warning: program is doing blitpri hacks.\n");
	}
	set_special (&regs, SPCFLAG_BLTNASTY);
	decide_blitter (hpos);
    }
    if (dmaen (DMA_BLITTER) && bltstate == BLT_init)
	bltstate = BLT_work;
    if ((dmacon & (DMA_BLITPRI | DMA_BLITTER | DMA_MASTER)) != (DMA_BLITPRI | DMA_BLITTER | DMA_MASTER)) {
	unset_special (&regs, SPCFLAG_BLTNASTY);
	decide_blitter (hpos);
    }
    if (changed & (DMA_MASTER | 0x0f))
	audio_hsync (0);
    if (changed & (DMA_MASTER | DMA_BITPLANE)) {
	ddf_change = vpos;
	if (dmaen (DMA_BITPLANE))
	    maybe_start_bpl_dma (hpos);
    }

    events_schedule();
}

#ifdef CPUEMU_6

static int irq_pending[15];       /* If true, an IRQ is pending arrival at the CPU. */
static unsigned long irq_due[15]; /* Cycle time that IRQ will arrive at CPU */

/*
 * Handle interrupt delay in cycle-exact mode.
 */
/****i* januae-custom-raster/intlev_2
 *
 *   NAME
 *       intlev_2()
 *
 *   SYNOPSIS
 *       static int intlev_2 (void)
 *
 *   FUNCTION
 *       Resolve a pending interrupt with cycle-exact delivery delay.
 *
 *   RESULT
 *       Returns the available interrupt level (1 through 5), or -1 if none is
 *       due.
 *
 *   NOTES
 *       Compiled with CPUEMU_6. Returns -1 if none is ready, otherwise a
 *       hardware IRQ level; pending sources are retired when due.
 *
 ****************************************************************************/

static int intlev_2 (void)
{
    uae_u16 imask = intreq & intena;
    int il = -1;

    if (imask && (intena & 0x4000)) {
	unsigned long cycles = get_cycles ();
	int i;

	for (i = 14; i >= 0; i--) {
	    if (imask & (1 << i)) {
		if (irq_pending[i] && (cycles >= irq_due[i])) {
		    irq_pending[i] = 0;

		    if (i == 13 || i == 14)
			il = -1;
		    else if (i == 11 || i == 12)
			return 5;
		    else if (i >= 7 && i <= 10)
			return 4;
		    else if (i >= 4 && i <= 6)
			return 3;
		    else if (i == 3)
			return 2;
		    else
			return 1;
	        }
	    }
	}
    } else
	unset_special (&regs, SPCFLAG_INT);

    return il;
}
#endif

/*
 * Get interrupt level of IRQ.
 */
/****i* januae-custom-raster/intlev
 *
 *   NAME
 *       intlev()
 *
 *   SYNOPSIS
 *       int intlev (void)
 *
 *   FUNCTION
 *       Resolve the highest enabled and requested Amiga interrupt level.
 *
 *   RESULT
 *       Returns the highest eligible interrupt level (1 through 6), or -1 when
 *       none is enabled.
 *
 *   NOTES
 *       Uses the delayed cycle-exact resolver when configured; otherwise maps
 *       matching INTREQ/INTENA groups into levels 1 through 6.
 *
 ****************************************************************************/

int intlev (void)
{
    int il = -1;

#ifdef CPUEMU_6
    if (currprefs.cpu_cycle_exact) {
	il = intlev_2 ();
	if (il >= 0 && il <= (int) regs.intmask)
	    unset_special (&regs, SPCFLAG_INT);
    } else
#endif
    {
	uae_u16 imask = intreq & intena;
	if (imask && (intena & 0x4000)) {
	    if (imask & 0x6000)
		il = 6;
	    if (imask & 0x1800)
		il = 5;
	    if (imask & 0x0780)
		il = 4;
	    if (imask & 0x0070)
		il = 3;
	    if (imask & 0x0008)
		il = 2;
	    if (imask & 0x0007)
		il = 1;
	}
    }

    return il;
}

/****i* januae-custom-raster/doint
 *
 *   NAME
 *       doint()
 *
 *   SYNOPSIS
 *       static void doint (void)
 *
 *   FUNCTION
 *       Signal the CPU that interrupt processing needs reevaluation.
 *
 *   NOTES
 *       With cycle-exact CPU emulation, also schedules newly pending enabled IRQ
 *       sources four CYCLE_UNIT units later.
 *
 ****************************************************************************/

static void doint (void)
{
    set_special (&regs, SPCFLAG_INT);

#ifdef CPUEMU_6
    if (currprefs.cpu_cycle_exact) {
	int i;
	uae_u16 imask;

	imask = intreq & intena;

	if (imask && (intena & 0x4000)) {
	    /* Set up delay for IRQ to arrive at the CPU. */
	    unsigned long cycle_irq_due = get_cycles () + 4 * CYCLE_UNIT;
	    for (i = 0; i < 15; i++) {
		if ((imask & (1 << i)) && irq_pending[i] == 0) {
		    irq_pending[i] = 1;
		    irq_due[i]     = cycle_irq_due;
		}
	    }
	}
    }
#endif
}

/****i* januae-custom-raster/INTENA
 *
 *   NAME
 *       INTENA()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void INTENA (uae_u16 v)
 *
 *   FUNCTION
 *       Apply interrupt-enable register set/clear semantics.
 *
 *   INPUTS
 *       v -- new register value
 *
 *   NOTES
 *       Calls doint() when the write has the set bit 0x8000; clearing does not
 *       call doint() here.
 *
 ****************************************************************************/

STATIC_INLINE void INTENA (uae_u16 v)
{
    setclr (&intena,v);
    if (v & 0x8000)
	doint ();
}

/****i* januae-custom-raster/INTREQ_0
 *
 *   NAME
 *       INTREQ_0()
 *
 *   SYNOPSIS
 *       void INTREQ_0 (uae_u16 v)
 *
 *   FUNCTION
 *       Apply interrupt request set/clear semantics to the custom-chip IRQ
 *       latch.
 *
 *   INPUTS
 *       v -- new register value
 *
 *   NOTES
 *       Updates audio IRQ state for relevant bits and clears cycle-exact pending
 *       flags for cleared sources before calling doint().
 *
 ****************************************************************************/

void INTREQ_0 (uae_u16 v)
{
    if (v & (0x80|0x100|0x200|0x400))
	audio_update_irq (v);

    setclr (&intreq, v);

#ifdef CPUEMU_6
    if (currprefs.cpu_cycle_exact) {
	if (!(v & 0x8000)) {
	    /* Interrupt request is being cleared - reset
	     * pending status. */
	    int i;
	    for (i = 0; i < 15; i++) {
		if (v & (1 << i))
		    irq_pending[i] = 0;
	    }
	}
    }
#endif

    doint ();
}


/****i* januae-custom-raster/INTREQ_f
 *
 *   NAME
 *       INTREQ_f()
 *
 *   SYNOPSIS
 *       void INTREQ_f (uae_u32 data)
 *
 *   FUNCTION
 *       Write a full interrupt request and notify device-specific interrupt
 *       producers.
 *
 *   INPUTS
 *       data -- sprite low data words
 *
 *   NOTES
 *       Calls INTREQ_0(), serial and CIA reevaluation plus optional
 *       A2091/CDTV/CD32 hooks.
 *
 ****************************************************************************/

void INTREQ_f (uae_u32 data)
{
    INTREQ_0 (data);
    serial_check_irq ();
    rethink_cias ();
#ifdef A2091
    rethink_a2091 ();
#endif
#ifdef CDTV
    rethink_cdtv ();
#endif
#ifdef CD32
    rethink_akiko ();
#endif
}

/****i* januae-custom-raster/INTREQ
 *
 *   NAME
 *       INTREQ()
 *
 *   SYNOPSIS
 *       void INTREQ (uae_u16 v)
 *
 *   FUNCTION
 *       Write interrupt requests and notify serial and CIA interrupt producers.
 *
 *   INPUTS
 *       v -- new register value
 *
 *   NOTES
 *       Delegates to INTREQ_0() then reevaluates serial and CIA state; does not
 *       call the optional device hooks present in INTREQ_f().
 *
 ****************************************************************************/

void INTREQ (uae_u16 v)
{
    INTREQ_0 (v);
    serial_check_irq ();
    rethink_cias ();
}

/****i* januae-custom-raster/ADKCON
 *
 *   NAME
 *       ADKCON()
 *
 *   SYNOPSIS
 *       static void ADKCON (int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Apply disk/audio control set/clear bits and propagate their effects.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Refreshes audio masks and disk timing; bit 11 may trigger a serial break
 *       according to the updated register.
 *
 ****************************************************************************/

static void ADKCON (int hpos, uae_u16 v)
{
    if (currprefs.produce_sound > 0)
	update_audio ();

    setclr (&adkcon,v);
    audio_update_adkmasks ();
    DISK_update (hpos);

    if ((v >> 11) & 1)
	serial_uartbreak ((adkcon >> 11) & 1);
}

/****i* januae-custom-raster/BEAMCON0
 *
 *   NAME
 *       BEAMCON0()
 *
 *   SYNOPSIS
 *       static void BEAMCON0 (uae_u16 v)
 *
 *   FUNCTION
 *       Stage programmable beam-control changes for ECS Agnus timing.
 *
 *   INPUTS
 *       v -- new register value
 *
 *   NOTES
 *       Ignored without ECS Agnus; if ECS Denise is absent masks all bits except
 *       0x20. Changes are staged in new_beamcon0 until timing setup.
 *
 ****************************************************************************/

static void BEAMCON0 (uae_u16 v)
{
    if (currprefs.chipset_mask & CSMASK_ECS_AGNUS) {
	if (!(currprefs.chipset_mask & CSMASK_ECS_DENISE))
	    v &= 0x20;
	if (v != new_beamcon0) {
	    new_beamcon0 = v;
	    if (v & ~0x20)
		write_log ("warning: %04.4X written to BEAMCON0\n", v);
	}
    }
}

#ifndef CUSTOM_SIMPLE

/****i* januae-custom-raster/varsync
 *
 *   NAME
 *       varsync()
 *
 *   SYNOPSIS
 *       static void varsync (void)
 *
 *   FUNCTION
 *       Adjust synchronization state for programmable beam timing.
 *
 *   NOTES
 *       Excluded with CUSTOM_SIMPLE. Picasso96 refresh can set vtotal; otherwise
 *       requires ECS Denise and beamcon0 programmable-sync bit.
 *
 ****************************************************************************/

static void varsync (void)
{
#ifdef PICASSO96
    if (p96refresh_active)
    {
	extern int p96hack_vpos2;
	static int p96hack_vpos_old;
	if (p96hack_vpos_old == p96hack_vpos2) return;
	vtotal = p96hack_vpos2;
	p96hack_vpos_old = p96hack_vpos2;
	hack_vpos = -1;
	return;
    }
#endif
    if (!(currprefs.chipset_mask & CSMASK_ECS_DENISE))
	return;
    if (!(beamcon0 & 0x80))
	return;
    hack_vpos = -1;
    dumpsync ();
}
#endif

/****i* januae-custom-raster/is_bitplane_dma
 *
 *   NAME
 *       is_bitplane_dma()
 *
 *   SYNOPSIS
 *       int is_bitplane_dma (unsigned int hpos)
 *
 *   FUNCTION
 *       Identify the active bitplane fetch slot at a horizontal beam position.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *
 *   RESULT
 *       Returns zero when no fetch is active, or the current cycle-diagram slot
 *       value.
 *
 *   NOTES
 *       Returns zero outside the current active DMA interval; otherwise returns
 *       the precomputed cycle-diagram slot identifier.
 *
 ****************************************************************************/

int is_bitplane_dma (unsigned int hpos)
{
    if (fetch_state == fetch_not_started || (int)hpos < thisline_decision.plfleft)
	return 0;
    if ((passed_plfstop == 3 && (int)hpos >= thisline_decision.plfright)
	|| hpos >= estimated_last_fetch_cycle)
	return 0;
    return curr_diagram[(hpos - cycle_diagram_shift) & fetchstart_mask];
}

/****i* januae-custom-raster/is_bitplane_dma_inline
 *
 *   NAME
 *       is_bitplane_dma_inline()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int is_bitplane_dma_inline (unsigned int hpos)
 *
 *   FUNCTION
 *       Inline variant of bitplane DMA slot identification.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *
 *   RESULT
 *       Returns zero when no fetch is active, or the current cycle-diagram slot
 *       value.
 *
 *   NOTES
 *       Returns the same cycle-diagram slot result and zero outside the active
 *       DMA interval as is_bitplane_dma().
 *
 ****************************************************************************/

STATIC_INLINE int is_bitplane_dma_inline (unsigned int hpos)
{
    if (fetch_state == fetch_not_started || (int)hpos < thisline_decision.plfleft)
	return 0;
    if ((passed_plfstop == 3 && (int)hpos >= thisline_decision.plfright)
	|| hpos >= estimated_last_fetch_cycle)
	return 0;
    return curr_diagram[(hpos - cycle_diagram_shift) & fetchstart_mask];
}

/****i* januae-custom-raster/BPLxPTH
 *
 *   NAME
 *       BPLxPTH()
 *
 *   SYNOPSIS
 *       static void BPLxPTH (unsigned int hpos, uae_u16 v, int num)
 *
 *   FUNCTION
 *       Write the high halfword of a numbered bitplane DMA pointer.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *       num -- zero-based bitplane pointer index
 *
 *   NOTES
 *       Synchronizes line and fetch state before preserving the pointer low
 *       halfword.
 *
 ****************************************************************************/

static void BPLxPTH (unsigned int hpos, uae_u16 v, int num)
{
    decide_line (hpos);
    decide_fetch (hpos);
    bplpt[num] = (bplpt[num] & 0xffff) | ((uae_u32)v << 16);
    //write_log("%d:%d:BPL%dPTH %08.8X\n", hpos, vpos, num, v);
}
/****i* januae-custom-raster/BPLxPTL
 *
 *   NAME
 *       BPLxPTL()
 *
 *   SYNOPSIS
 *       static void BPLxPTL (unsigned int hpos, uae_u16 v, int num)
 *
 *   FUNCTION
 *       Write the low halfword of a numbered bitplane DMA pointer.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *       num -- zero-based bitplane pointer index
 *
 *   NOTES
 *       For a concurrent DMA fetch on a nonzero plane, adds 2 << fetchmode to
 *       the new offset; clears the low address bit.
 *
 ****************************************************************************/

static void BPLxPTL (unsigned int hpos, uae_u16 v, int num)
{
    int delta = 0;
    decide_line (hpos);
    decide_fetch (hpos);
    /* fix for "bitplane dma fetch at the same time while updating BPLxPTL" */
    /* fixes "3v Demo" by Cave and "New Year Demo" by Phoenix */
    if (is_bitplane_dma(hpos - 1) == num + 1 && num > 0)
	delta = 2 << fetchmode;
    bplpt[num] = (bplpt[num] & ~0xffff) | ((v + delta) & 0xfffe);
    //write_log("%d:%d:BPL%dPTL %08.8X\n", hpos, vpos, num, v);
}

/****i* januae-custom-raster/BPLCON0
 *
 *   NAME
 *       BPLCON0()
 *
 *   SYNOPSIS
 *       static void BPLCON0 (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Write primary bitplane mode control and refresh affected raster
 *       decisions.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Masks unsupported OCS/ECS mode bits; synchronizes line/fetch/blitter,
 *       tracks interlace and HAM, updates fetch geometry and DIW.
 *
 ****************************************************************************/

static void BPLCON0 (unsigned int hpos, uae_u16 v)
{
    if (! (currprefs.chipset_mask & CSMASK_ECS_DENISE))
	v &= ~0x00F1;
    else if (! (currprefs.chipset_mask & CSMASK_AGA))
	v &= ~0x00B1;

    if (bplcon0 == v)
	return;

    if ((bplcon0 & 2) && !(v & 2)) {
	vpos_previous = vpos;
	hpos_previous = hpos;
    }

    if ((v & 4) && !interlace_seen)
	interlace_started = 2;

    ddf_change = vpos;
    decide_line (hpos);
    decide_fetch (hpos);
    decide_blitter (hpos);

    bplcon0 = v;
    record_register_change (hpos, 0x100, v);

#ifdef AGA
    if (currprefs.chipset_mask & CSMASK_AGA) {
	decide_sprites (hpos);
	expand_sprres ();
    }
#endif

    expand_fmodes ();
    calcdiw ();
    estimate_last_fetch_cycle (hpos);
}

/****i* januae-custom-raster/BPLCON1
 *
 *   NAME
 *       BPLCON1()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void BPLCON1 (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Write bitplane scroll delay control.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Masks the high bits when AGA is absent and synchronizes line and fetch
 *       decisions before assigning the changed value.
 *
 ****************************************************************************/

STATIC_INLINE void BPLCON1 (unsigned int hpos, uae_u16 v)
{
    if (!(currprefs.chipset_mask & CSMASK_AGA))
	v &= 0xff;
    if (bplcon1 == v)
	return;
    ddf_change = vpos;
    decide_line (hpos);
    decide_fetch (hpos);
    bplcon1 = v;
}

/****i* januae-custom-raster/BPLCON2
 *
 *   NAME
 *       BPLCON2()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void BPLCON2 (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Write playfield priority and collision control.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Masks unsupported non-AGA bits and records the register change for line
 *       rendering.
 *
 ****************************************************************************/

STATIC_INLINE void BPLCON2 (unsigned int hpos, uae_u16 v)
{
    if (!(currprefs.chipset_mask & CSMASK_AGA))
	v &= 0x7f;
    if (bplcon2 == v)
	return;
    decide_line (hpos);
    bplcon2 = v;
    record_register_change (hpos, 0x104, v);
}

#ifdef AGA
/****i* januae-custom-raster/BPLCON3
 *
 *   NAME
 *       BPLCON3()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void BPLCON3 (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Write the AGA-specific bitplane and sprite control register.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Compiled with AGA; returns without effect on a non-AGA active chipset
 *       and updates sprite resolution and register-change history.
 *
 ****************************************************************************/

STATIC_INLINE void BPLCON3 (unsigned int hpos, uae_u16 v)
{
    if (! (currprefs.chipset_mask & CSMASK_AGA))
	return;
    if (bplcon3 == v)
	return;
    decide_line (hpos);
    decide_sprites (hpos);
    bplcon3 = v;
    expand_sprres ();
    record_register_change (hpos, 0x106, v);
}

/****i* januae-custom-raster/BPLCON4
 *
 *   NAME
 *       BPLCON4()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void BPLCON4 (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Write the AGA-specific sprite color-bank control register.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Compiled with AGA; returns without effect on a non-AGA active chipset
 *       and records the timed register write.
 *
 ****************************************************************************/

STATIC_INLINE void BPLCON4 (unsigned int hpos, uae_u16 v)
{
    if (! (currprefs.chipset_mask & CSMASK_AGA))
	return;
    if (bplcon4 == v)
	return;
    decide_line (hpos);
    bplcon4 = v;
    record_register_change (hpos, 0x10c, v);
}
#endif

/****i* januae-custom-raster/BPL1MOD
 *
 *   NAME
 *       BPL1MOD()
 *
 *   SYNOPSIS
 *       static void BPL1MOD (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Update odd-bitplane modulo for the following DMA row.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Forces even addressing and synchronizes line and fetch state only when
 *       the effective modulo changes.
 *
 ****************************************************************************/

static void BPL1MOD (unsigned int hpos, uae_u16 v)
{
    v &= ~1;
    if ((uae_s16)bpl1mod == (uae_s16)v)
	return;
    decide_line (hpos);
    decide_fetch (hpos);
    bpl1mod = v;
}

/****i* januae-custom-raster/BPL2MOD
 *
 *   NAME
 *       BPL2MOD()
 *
 *   SYNOPSIS
 *       static void BPL2MOD (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Update even-bitplane modulo for the following DMA row.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Forces even addressing and synchronizes line and fetch state only when
 *       the effective modulo changes.
 *
 ****************************************************************************/

static void BPL2MOD (unsigned int hpos, uae_u16 v)
{
    v &= ~1;
    if ((uae_s16)bpl2mod == (uae_s16)v)
	return;
    decide_line (hpos);
    decide_fetch (hpos);
    bpl2mod = v;
}

/****i* januae-custom-raster/BPL1DAT
 *
 *   NAME
 *       BPL1DAT()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void BPL1DAT (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Latch a CPU-written first bitplane data word.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Synchronizes line state and calls maybe_first_bpl1dat() so CPU-fed
 *       display output can start before DMA.
 *
 ****************************************************************************/

STATIC_INLINE void BPL1DAT (unsigned int hpos, uae_u16 v)
{
    decide_line (hpos);
    bpl1dat = v;

    maybe_first_bpl1dat (hpos);
}

/****i* januae-custom-raster/DIWSTRT
 *
 *   NAME
 *       DIWSTRT()
 *
 *   SYNOPSIS
 *       static void DIWSTRT (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Write display-window start position.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Clears the DIWHIGH-written extension flag and recalculates the window
 *       after synchronizing the line.
 *
 ****************************************************************************/

static void DIWSTRT (unsigned int hpos, uae_u16 v)
{
    if (diwstrt == v && ! diwhigh_written)
	return;
    decide_line (hpos);
    diwhigh_written = 0;
    diwstrt = v;
    calcdiw ();
}

/****i* januae-custom-raster/DIWSTOP
 *
 *   NAME
 *       DIWSTOP()
 *
 *   SYNOPSIS
 *       static void DIWSTOP (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Write display-window stop position.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Clears the DIWHIGH-written extension flag and recalculates the window
 *       after synchronizing the line.
 *
 ****************************************************************************/

static void DIWSTOP (unsigned int hpos, uae_u16 v)
{
    if (diwstop == v && ! diwhigh_written)
	return;
    decide_line (hpos);
    diwhigh_written = 0;
    diwstop = v;
    calcdiw ();
}

/****i* januae-custom-raster/DIWHIGH
 *
 *   NAME
 *       DIWHIGH()
 *
 *   SYNOPSIS
 *       static void DIWHIGH (int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Write ECS high display-window coordinate extension bits.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Ignored without ECS Agnus; records that extended positions are active
 *       and recalculates DIW.
 *
 ****************************************************************************/

static void DIWHIGH (int hpos, uae_u16 v)
{
    if (! (currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	return;
    if (diwhigh_written && diwhigh == v)
	return;
    decide_line (hpos);
    diwhigh_written = 1;
    diwhigh = v;
    calcdiw ();
}

/****i* januae-custom-raster/DDFSTRT
 *
 *   NAME
 *       DDFSTRT()
 *
 *   SYNOPSIS
 *       static void DDFSTRT (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Write and align bitplane fetch start position.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Clears low address bits according to OCS/ECS Agnus, remembers write
 *       position for fetch-delay handling and recalculates DIW.
 *
 ****************************************************************************/

static void DDFSTRT (unsigned int hpos, uae_u16 v)
{
    v &= 0xfe;
    if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	v &= 0xfc;
    if (ddfstrt == v && hpos != plfstrt - 2)
	return;
    ddf_change = vpos;
    decide_line (hpos);
    ddfstrt_old_hpos = hpos;
    ddfstrt_old_vpos = vpos;
    ddfstrt = v;
    calcdiw ();
    if (ddfstop > 0xD4 && (ddfstrt & 4) == 4) {
	static int last_warned;
	last_warned = (last_warned + 1) & 4095;
	if (last_warned == 0)
	    write_log ("WARNING! Very strange DDF values (%x %x).\n", ddfstrt, ddfstop);
    }
}

/****i* januae-custom-raster/DDFSTOP
 *
 *   NAME
 *       DDFSTOP()
 *
 *   SYNOPSIS
 *       static void DDFSTOP (unsigned int hpos, uae_u16 v)
 *
 *   FUNCTION
 *       Write and align bitplane fetch stop position.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       v -- new register value
 *
 *   NOTES
 *       Synchronizes line/fetch/blitter, recalculates the fetch window and
 *       revises the final-fetch estimate when DMA is in progress.
 *
 ****************************************************************************/

static void DDFSTOP (unsigned int hpos, uae_u16 v)
{
    v &= 0xfe;
    if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	v &= 0xfc;
    if (ddfstop == v)
	return;
    decide_line (hpos);
    decide_fetch (hpos);
    decide_blitter (hpos);
    ddfstop = v;
    calcdiw ();
    if (fetch_state != fetch_not_started)
	estimate_last_fetch_cycle (hpos);
    if (ddfstop > 0xD4 && (ddfstrt & 4) == 4) {
	static int last_warned;
	if (last_warned == 0)
	    write_log ("WARNING! Very strange DDF values (%x).\n", ddfstop);
	last_warned = (last_warned + 1) & 4095;
    }
}

/****i* januae-custom-raster/FMODE
 *
 *   NAME
 *       FMODE()
 *
 *   SYNOPSIS
 *       static void FMODE (uae_u16 v)
 *
 *   FUNCTION
 *       Write AGA fetch mode and recompute fetch geometry and sprite width.
 *
 *   INPUTS
 *       v -- new register value
 *
 *   NOTES
 *       Forces zero on non-AGA chipsets; maps FMODE low two bits to fetchmode
 *       0/1/2 and recalculates DIW.
 *
 ****************************************************************************/

static void FMODE (uae_u16 v)
{
    if (! (currprefs.chipset_mask & CSMASK_AGA))
	v = 0;
    ddf_change = vpos;
    fmode = v;
    sprite_width = GET_SPRITEWIDTH (fmode);
    switch (fmode & 3) {
    case 0:
	fetchmode = 0;
	break;
    case 1:
    case 2:
	fetchmode = 1;
	break;
    case 3:
	fetchmode = 2;
	break;
    }
    expand_fmodes ();
    calcdiw ();
}

/****i* januae-custom-blitter-sprite-copper/BLTADAT
 *
 *   NAME
 *       BLTADAT()
 *
 *   SYNOPSIS
 *       static void BLTADAT (uae_u16 v)
 *
 *   FUNCTION
 *       Write the blitter A source-data latch after synchronizing pending blits.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Calls maybe_blit() at the current horizontal position in mode 0; does
 *       not apply A shift or first/last word masks here.
 *
 ****************************************************************************/

static void BLTADAT (uae_u16 v)
{
    maybe_blit (current_hpos(), 0);

    blt_info.bltadat = v;
}
/*
 * "Loading data shifts it immediately" says the HRM. Well, that may
 * be true for BLTBDAT, but not for BLTADAT - it appears the A data must be
 * loaded for every word so that AFWM and ALWM can be applied.
 */
/****i* januae-custom-blitter-sprite-copper/BLTBDAT
 *
 *   NAME
 *       BLTBDAT()
 *
 *   SYNOPSIS
 *       static void BLTBDAT (uae_u16 v)
 *
 *   FUNCTION
 *       Write the blitter B source-data latch and its shifted hold register.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Shifts the value left or right according to BLTCON1 bit 1 and its upper
 *       shift field; synchronizes a pending blit first.
 *
 ****************************************************************************/

static void BLTBDAT (uae_u16 v)
{
    maybe_blit (current_hpos(), 0);

    if (bltcon1 & 2)
	blt_info.bltbhold = v << (bltcon1 >> 12);
    else
	blt_info.bltbhold = v >> (bltcon1 >> 12);
    blt_info.bltbdat = v;
}
/****i* januae-custom-blitter-sprite-copper/BLTCDAT
 *
 *   NAME
 *       BLTCDAT()
 *
 *   SYNOPSIS
 *       static void BLTCDAT (uae_u16 v)
 *
 *   FUNCTION
 *       Write the blitter C source-data latch.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Synchronizes blitter activity before the write and invalidates the
 *       relevant cached blitter state with reset_blit(0).
 *
 ****************************************************************************/

static void BLTCDAT (uae_u16 v) { maybe_blit (current_hpos(), 0); blt_info.bltcdat = v; reset_blit (0); }

/****i* januae-custom-blitter-sprite-copper/BLTAMOD
 *
 *   NAME
 *       BLTAMOD()
 *
 *   SYNOPSIS
 *       static void BLTAMOD (uae_u16 v)
 *
 *   FUNCTION
 *       Set the signed modulo for blitter source A.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Clears the low address bit, casts the result to a signed 16-bit value
 *       and invalidates cached blitter state.
 *
 ****************************************************************************/

static void BLTAMOD (uae_u16 v) { maybe_blit (current_hpos(), 1); blt_info.bltamod = (uae_s16)(v & 0xFFFE); reset_blit (0); }
/****i* januae-custom-blitter-sprite-copper/BLTBMOD
 *
 *   NAME
 *       BLTBMOD()
 *
 *   SYNOPSIS
 *       static void BLTBMOD (uae_u16 v)
 *
 *   FUNCTION
 *       Set the signed modulo for blitter source B.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Clears the low address bit, casts the result to a signed 16-bit value
 *       and invalidates cached blitter state.
 *
 ****************************************************************************/

static void BLTBMOD (uae_u16 v) { maybe_blit (current_hpos(), 1); blt_info.bltbmod = (uae_s16)(v & 0xFFFE); reset_blit (0); }
/****i* januae-custom-blitter-sprite-copper/BLTCMOD
 *
 *   NAME
 *       BLTCMOD()
 *
 *   SYNOPSIS
 *       static void BLTCMOD (uae_u16 v)
 *
 *   FUNCTION
 *       Set the signed modulo for blitter source C.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Clears the low address bit, casts the result to a signed 16-bit value
 *       and invalidates cached blitter state.
 *
 ****************************************************************************/

static void BLTCMOD (uae_u16 v) { maybe_blit (current_hpos(), 1); blt_info.bltcmod = (uae_s16)(v & 0xFFFE); reset_blit (0); }
/****i* januae-custom-blitter-sprite-copper/BLTDMOD
 *
 *   NAME
 *       BLTDMOD()
 *
 *   SYNOPSIS
 *       static void BLTDMOD (uae_u16 v)
 *
 *   FUNCTION
 *       Set the signed modulo for blitter destination D.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Clears the low address bit, casts the result to a signed 16-bit value
 *       and invalidates cached blitter state.
 *
 ****************************************************************************/

static void BLTDMOD (uae_u16 v) { maybe_blit (current_hpos(), 1); blt_info.bltdmod = (uae_s16)(v & 0xFFFE); reset_blit (0); }

/****i* januae-custom-blitter-sprite-copper/BLTCON0
 *
 *   NAME
 *       BLTCON0()
 *
 *   SYNOPSIS
 *       static void BLTCON0 (uae_u16 v)
 *
 *   FUNCTION
 *       Write the primary blitter control word.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Derives the line-A shift from the high nibble and calls reset_blit(1)
 *       after synchronizing pending blits.
 *
 ****************************************************************************/

static void BLTCON0 (uae_u16 v) { maybe_blit (current_hpos(), 2); bltcon0 = v; blinea_shift = v >> 12; reset_blit (1); }
/* The next category is "Most useless hardware register".
 * And the winner is... */
/****i* januae-custom-blitter-sprite-copper/BLTCON0L
 *
 *   NAME
 *       BLTCON0L()
 *
 *   SYNOPSIS
 *       static void BLTCON0L (uae_u16 v)
 *
 *   FUNCTION
 *       Update the low byte of BLTCON0 on ECS Agnus and newer.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Returns without changing state when ECS Agnus is absent; retains the
 *       high byte and applies only bits 0 through 7.
 *
 ****************************************************************************/

static void BLTCON0L (uae_u16 v)
{
    if (! (currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	return;
    maybe_blit (current_hpos(), 2); bltcon0 = (bltcon0 & 0xFF00) | (v & 0xFF);
    reset_blit (1);
}
/****i* januae-custom-blitter-sprite-copper/BLTCON1
 *
 *   NAME
 *       BLTCON1()
 *
 *   SYNOPSIS
 *       static void BLTCON1 (uae_u16 v)
 *
 *   FUNCTION
 *       Write the secondary blitter control word.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Synchronizes pending blits and calls reset_blit(2), which differs from
 *       the reset mode used for BLTCON0.
 *
 ****************************************************************************/

static void BLTCON1 (uae_u16 v) { maybe_blit (current_hpos(), 2); bltcon1 = v; reset_blit (2); }

/****i* januae-custom-blitter-sprite-copper/BLTAFWM
 *
 *   NAME
 *       BLTAFWM()
 *
 *   SYNOPSIS
 *       static void BLTAFWM (uae_u16 v)
 *
 *   FUNCTION
 *       Write the first-word mask for the blitter A source.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Synchronizes pending work using maybe_blit(...,2) and then invalidates
 *       the cached state.
 *
 ****************************************************************************/

static void BLTAFWM (uae_u16 v) { maybe_blit (current_hpos(), 2); blt_info.bltafwm = v; reset_blit (0); }
/****i* januae-custom-blitter-sprite-copper/BLTALWM
 *
 *   NAME
 *       BLTALWM()
 *
 *   SYNOPSIS
 *       static void BLTALWM (uae_u16 v)
 *
 *   FUNCTION
 *       Write the last-word mask for the blitter A source.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Synchronizes pending work using maybe_blit(...,2) and then invalidates
 *       the cached state.
 *
 ****************************************************************************/

static void BLTALWM (uae_u16 v) { maybe_blit (current_hpos(), 2); blt_info.bltalwm = v; reset_blit (0); }

/****i* januae-custom-blitter-sprite-copper/BLTAPTH
 *
 *   NAME
 *       BLTAPTH()
 *
 *   SYNOPSIS
 *       static void BLTAPTH (uae_u16 v)
 *
 *   FUNCTION
 *       Write the high halfword of the blitter A source address.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Retains the current low halfword; synchronizes a pending blit before the
 *       update.
 *
 ****************************************************************************/

static void BLTAPTH (uae_u16 v) { maybe_blit (current_hpos(), 0); bltapt = (bltapt & 0xffff) | ((uae_u32)v << 16); }
/****i* januae-custom-blitter-sprite-copper/BLTAPTL
 *
 *   NAME
 *       BLTAPTL()
 *
 *   SYNOPSIS
 *       static void BLTAPTL (uae_u16 v)
 *
 *   FUNCTION
 *       Write the low halfword of the blitter A source address.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Clears bit zero of the low halfword, enforcing an even source address.
 *
 ****************************************************************************/

static void BLTAPTL (uae_u16 v) { maybe_blit (current_hpos(), 0); bltapt = (bltapt & ~0xffff) | (v & 0xFFFE); }
/****i* januae-custom-blitter-sprite-copper/BLTBPTH
 *
 *   NAME
 *       BLTBPTH()
 *
 *   SYNOPSIS
 *       static void BLTBPTH (uae_u16 v)
 *
 *   FUNCTION
 *       Write the high halfword of the blitter B source address.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Retains the current low halfword; synchronizes a pending blit before the
 *       update.
 *
 ****************************************************************************/

static void BLTBPTH (uae_u16 v) { maybe_blit (current_hpos(), 0); bltbpt = (bltbpt & 0xffff) | ((uae_u32)v << 16); }
/****i* januae-custom-blitter-sprite-copper/BLTBPTL
 *
 *   NAME
 *       BLTBPTL()
 *
 *   SYNOPSIS
 *       static void BLTBPTL (uae_u16 v)
 *
 *   FUNCTION
 *       Write the low halfword of the blitter B source address.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Clears bit zero of the low halfword, enforcing an even source address.
 *
 ****************************************************************************/

static void BLTBPTL (uae_u16 v) { maybe_blit (current_hpos(), 0); bltbpt = (bltbpt & ~0xffff) | (v & 0xFFFE); }
/****i* januae-custom-blitter-sprite-copper/BLTCPTH
 *
 *   NAME
 *       BLTCPTH()
 *
 *   SYNOPSIS
 *       static void BLTCPTH (uae_u16 v)
 *
 *   FUNCTION
 *       Write the high halfword of the blitter C source address.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Retains the current low halfword; synchronizes a pending blit before the
 *       update.
 *
 ****************************************************************************/

static void BLTCPTH (uae_u16 v) { maybe_blit (current_hpos(), 0); bltcpt = (bltcpt & 0xffff) | ((uae_u32)v << 16); }
/****i* januae-custom-blitter-sprite-copper/BLTCPTL
 *
 *   NAME
 *       BLTCPTL()
 *
 *   SYNOPSIS
 *       static void BLTCPTL (uae_u16 v)
 *
 *   FUNCTION
 *       Write the low halfword of the blitter C source address.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Clears bit zero of the low halfword, enforcing an even source address.
 *
 ****************************************************************************/

static void BLTCPTL (uae_u16 v) { maybe_blit (current_hpos(), 0); bltcpt = (bltcpt & ~0xffff) | (v & 0xFFFE); }
/****i* januae-custom-blitter-sprite-copper/BLTDPTH
 *
 *   NAME
 *       BLTDPTH()
 *
 *   SYNOPSIS
 *       static void BLTDPTH (uae_u16 v)
 *
 *   FUNCTION
 *       Write the high halfword of the blitter D destination address.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Retains the current low halfword; synchronizes a pending blit before the
 *       update.
 *
 ****************************************************************************/

static void BLTDPTH (uae_u16 v) { maybe_blit (current_hpos(), 0); bltdpt = (bltdpt & 0xffff) | ((uae_u32)v << 16); }
/****i* januae-custom-blitter-sprite-copper/BLTDPTL
 *
 *   NAME
 *       BLTDPTL()
 *
 *   SYNOPSIS
 *       static void BLTDPTL (uae_u16 v)
 *
 *   FUNCTION
 *       Write the low halfword of the blitter D destination address.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Clears bit zero of the low halfword, enforcing an even destination
 *       address.
 *
 ****************************************************************************/

static void BLTDPTL (uae_u16 v) { maybe_blit (current_hpos(), 0); bltdpt = (bltdpt & ~0xffff) | (v & 0xFFFE); }

/****i* januae-custom-blitter-sprite-copper/BLTSIZE
 *
 *   NAME
 *       BLTSIZE()
 *
 *   SYNOPSIS
 *       static void BLTSIZE (uae_u16 v)
 *
 *   FUNCTION
 *       Set legacy blitter width and height and start the operation.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Width occupies six bits and height the remaining ten; encoded zero means
 *       64 words or 1024 rows respectively.
 *
 ****************************************************************************/

static void BLTSIZE (uae_u16 v)
{
    maybe_blit (current_hpos(), 0);

    blt_info.vblitsize = v >> 6;
    blt_info.hblitsize = v & 0x3F;
    if (!blt_info.vblitsize) blt_info.vblitsize = 1024;
    if (!blt_info.hblitsize) blt_info.hblitsize = 64;
    do_blitter (current_hpos());
}

/****i* januae-custom-blitter-sprite-copper/BLTSIZV
 *
 *   NAME
 *       BLTSIZV()
 *
 *   SYNOPSIS
 *       static void BLTSIZV (uae_u16 v)
 *
 *   FUNCTION
 *       Stage the ECS Agnus extended blitter height.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Ignored without ECS Agnus. Stores the low 15 bits but does not launch a
 *       blit; BLTSIZH starts it.
 *
 ****************************************************************************/

static void BLTSIZV (uae_u16 v)
{
    if (! (currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	return;
    maybe_blit (current_hpos(), 0);
    blt_info.vblitsize = v & 0x7FFF;
}

/****i* januae-custom-blitter-sprite-copper/BLTSIZH
 *
 *   NAME
 *       BLTSIZH()
 *
 *   SYNOPSIS
 *       static void BLTSIZH (uae_u16 v)
 *
 *   FUNCTION
 *       Set the ECS Agnus extended blitter width and start the operation.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Ignored without ECS Agnus. A zero width selects 0x800 words and a zero
 *       staged height selects 32768 rows.
 *
 ****************************************************************************/

static void BLTSIZH (uae_u16 v)
{
    if (! (currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	return;
    maybe_blit (current_hpos(), 0);
    blt_info.hblitsize = v & 0x7FF;
    if (!blt_info.vblitsize)
	blt_info.vblitsize = 32768;
    if (!blt_info.hblitsize)
	blt_info.hblitsize = 0x800;
    do_blitter (current_hpos());
}

/****i* januae-custom-blitter-sprite-copper/spr_arm
 *
 *   NAME
 *       spr_arm()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void spr_arm (unsigned int num, int state)
 *
 *   FUNCTION
 *       Change a sprite channel armed flag and maintain the global armed count.
 *
 *   INPUTS
 *       num -- zero-based sprite channel number
 *       state -- new sprite armed state, zero to disarm and nonzero to arm
 *
 *   NOTES
 *       Setting an already armed sprite or clearing an already disarmed sprite
 *       does not change nr_armed.
 *
 ****************************************************************************/

STATIC_INLINE void spr_arm (unsigned int num, int state)
{
    switch (state) {
	case 0:
	    nr_armed -= spr[num].armed;
	    spr[num].armed = 0;
	    break;
	default:
            nr_armed += 1 - spr[num].armed;
	    spr[num].armed = 1;
	    break;
    }
}

/****i* januae-custom-blitter-sprite-copper/sprstartstop
 *
 *   NAME
 *       sprstartstop()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void sprstartstop (struct sprite *s)
 *
 *   FUNCTION
 *       Update a sprite DMA enable state at its programmed vertical boundaries.
 *
 *   INPUTS
 *       s -- pointer to a live sprite descriptor
 *
 *   NOTES
 *       At vstart enables DMA; at vstop disables it. The stop comparison is
 *       performed after the start comparison.
 *
 ****************************************************************************/

STATIC_INLINE void sprstartstop (struct sprite *s)
{
    if (vpos == s->vstart)
	s->dmastate = 1;
    if (vpos == s->vstop)
	s->dmastate = 0;
}

/****i* januae-custom-blitter-sprite-copper/SPRxCTLPOS
 *
 *   NAME
 *       SPRxCTLPOS()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void SPRxCTLPOS (unsigned int num)
 *
 *   FUNCTION
 *       Decode sprite horizontal and vertical geometry from position/control
 *       words.
 *
 *   INPUTS
 *       num -- zero-based sprite channel number
 *
 *   NOTES
 *       Supports ECS Agnus vertical extension and AGA horizontal extension; AGA
 *       superhires 35 ns increment is not modeled.
 *
 ****************************************************************************/

STATIC_INLINE void SPRxCTLPOS (unsigned int num)
{
    unsigned int sprxp;
    struct sprite *s = &spr[num];

    sprstartstop (s);
    sprxp = (sprpos[num] & 0xFF) * 2 + (sprctl[num] & 1);
    /* Quite a bit salad in this register... */
#ifdef AGA
    if (currprefs.chipset_mask & CSMASK_AGA) {
	/* We ignore the SHRES 35ns increment for now; SHRES support doesn't
	   work anyway, so we may as well restrict AGA sprites to a 70ns
	   resolution.  */
	sprxp <<= 1;
	sprxp |= (sprctl[num] >> 4) & 1;
    }
#endif
    s->xpos = sprxp;
    s->vstart = (sprpos[num] >> 8) | ((sprctl[num] << 6) & 0x100);
    s->vstop = (sprctl[num] >> 8) | ((sprctl[num] << 7) & 0x100);
    if (currprefs.chipset_mask & CSMASK_ECS_AGNUS) {
	s->vstart |= (sprctl[num] << 3) & 0x200;
	s->vstop |= (sprctl[num] << 4) & 0x200;
    }
    sprstartstop (s);
}

/****i* januae-custom-blitter-sprite-copper/SPRxCTL_1
 *
 *   NAME
 *       SPRxCTL_1()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void SPRxCTL_1 (uae_u16 v, unsigned int num, unsigned int
 *       hpos)
 *
 *   FUNCTION
 *       Store a sprite control word and recompute the channel timing.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *       hpos -- horizontal beam position for the current register update
 *
 *   NOTES
 *       Disarms the sprite before decoding updated geometry. Horizontal position
 *       is used only by conditional debug logging.
 *
 ****************************************************************************/

STATIC_INLINE void SPRxCTL_1 (uae_u16 v, unsigned int num, unsigned int hpos)
{
#if SPRITE_DEBUG > 0
    struct sprite *s = &spr[num];
#endif
    sprctl[num] = v;
    spr_arm (num, 0);
    SPRxCTLPOS (num);
#if SPRITE_DEBUG > 0
    if (vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY) {
	write_log ("%d:%d:SPR%dCTL %04.4X P=%06.6X VSTRT=%d VSTOP=%d HSTRT=%d D=%d A=%d CP=%x PC=%x\n",
	    vpos, hpos, num, v, s->pt, s->vstart, s->vstop, s->xpos, spr[num].dmastate, spr[num].armed, cop_state.ip, m68k_getpc (&regs));
    }
#endif

}
/****i* januae-custom-blitter-sprite-copper/SPRxPOS_1
 *
 *   NAME
 *       SPRxPOS_1()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void SPRxPOS_1 (uae_u16 v, unsigned int num, unsigned int
 *       hpos)
 *
 *   FUNCTION
 *       Store a sprite position word and recompute the channel timing.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *       hpos -- horizontal beam position for the current register update
 *
 *   NOTES
 *       Updates the derived sprite start/stop position and DMA state; horizontal
 *       position is used only for debug logging.
 *
 ****************************************************************************/

STATIC_INLINE void SPRxPOS_1 (uae_u16 v, unsigned int num, unsigned int hpos)
{
#if SPRITE_DEBUG > 0
    struct sprite *s = &spr[num];
#endif
    sprpos[num] = v;
    SPRxCTLPOS (num);
#if SPRITE_DEBUG > 0
    if (vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY) {
	write_log ("%d:%d:SPR%dPOS %04.4X P=%06.6X VSTRT=%d VSTOP=%d HSTRT=%d D=%d A=%d CP=%x PC=%x\n",
	    vpos, hpos, num, v, s->pt, s->vstart, s->vstop, s->xpos, spr[num].dmastate, spr[num].armed, cop_state.ip, m68k_getpc (&regs));
    }
#endif
}

/****i* januae-custom-blitter-sprite-copper/SPRxDATA_1
 *
 *   NAME
 *       SPRxDATA_1()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void SPRxDATA_1 (uae_u16 v, unsigned int num, unsigned int
 *       hpos)
 *
 *   FUNCTION
 *       Latch first sprite data word and arm the sprite for rendering.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *       hpos -- horizontal beam position for the current register update
 *
 *   NOTES
 *       With AGA enabled the value is duplicated across four fetch slots;
 *       horizontal position matters only to debug logging.
 *
 ****************************************************************************/

STATIC_INLINE void SPRxDATA_1 (uae_u16 v, unsigned int num, unsigned int hpos)
{
    sprdata[num][0] = v;
#ifdef AGA
    sprdata[num][1] = v;
    sprdata[num][2] = v;
    sprdata[num][3] = v;
#endif
    spr_arm (num, 1);
#if SPRITE_DEBUG > 1
    if (vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY) {
	write_log ("%d:%d:SPR%dDATA %04.4X P=%06.6X D=%d A=%d PC=%x\n",
	    vpos, hpos, num, v, spr[num].pt, spr[num].dmastate, spr[num].armed, m68k_getpc (&regs));
    }
#endif
}

/****i* januae-custom-blitter-sprite-copper/SPRxDATB_1
 *
 *   NAME
 *       SPRxDATB_1()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void SPRxDATB_1 (uae_u16 v, unsigned int num, unsigned int
 *       hpos)
 *
 *   FUNCTION
 *       Latch the second sprite data word.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *       hpos -- horizontal beam position for the current register update
 *
 *   NOTES
 *       With AGA enabled the value is duplicated across four fetch slots; unlike
 *       SPRxDATA_1 this routine does not arm the sprite.
 *
 ****************************************************************************/

STATIC_INLINE void SPRxDATB_1 (uae_u16 v, unsigned int num, unsigned int hpos)
{
    sprdatb[num][0] = v;
#ifdef AGA
    sprdatb[num][1] = v;
    sprdatb[num][2] = v;
    sprdatb[num][3] = v;
#endif
#if SPRITE_DEBUG > 1
    if (vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY) {
	write_log ("%d:%d:SPR%dDATB %04.4X P=%06.6X D=%d A=%d PC=%x\n",
	    vpos, hpos, num, v, spr[num].pt, spr[num].dmastate, spr[num].armed, m68k_getpc (&regs));
    }
#endif
}

/****i* januae-custom-blitter-sprite-copper/SPRxDATA
 *
 *   NAME
 *       SPRxDATA()
 *
 *   SYNOPSIS
 *       static void SPRxDATA (unsigned int hpos, uae_u16 v, unsigned int num)
 *
 *   FUNCTION
 *       Synchronize sprite decisions and write the first sprite data word.
 *
 *   INPUTS
 *       hpos -- horizontal beam position for the current register update
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *
 *   NOTES
 *       Calls decide_sprites(hpos) before SPRxDATA_1, preserving raster timing
 *       of midline register writes.
 *
 ****************************************************************************/

static void SPRxDATA (unsigned int hpos, uae_u16 v, unsigned int num) { decide_sprites (hpos); SPRxDATA_1 (v, num, hpos); }
/****i* januae-custom-blitter-sprite-copper/SPRxDATB
 *
 *   NAME
 *       SPRxDATB()
 *
 *   SYNOPSIS
 *       static void SPRxDATB (unsigned int hpos, uae_u16 v, unsigned int num)
 *
 *   FUNCTION
 *       Synchronize sprite decisions and write the second sprite data word.
 *
 *   INPUTS
 *       hpos -- horizontal beam position for the current register update
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *
 *   NOTES
 *       Calls decide_sprites(hpos) before SPRxDATB_1; does not arm the sprite by
 *       itself.
 *
 ****************************************************************************/

static void SPRxDATB (unsigned int hpos, uae_u16 v, unsigned int num) { decide_sprites (hpos); SPRxDATB_1 (v, num, hpos); }
/****i* januae-custom-blitter-sprite-copper/SPRxCTL
 *
 *   NAME
 *       SPRxCTL()
 *
 *   SYNOPSIS
 *       static void SPRxCTL (unsigned int hpos, uae_u16 v, unsigned int num)
 *
 *   FUNCTION
 *       Synchronize sprite decisions and write the sprite control word.
 *
 *   INPUTS
 *       hpos -- horizontal beam position for the current register update
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *
 *   NOTES
 *       Delegates to SPRxCTL_1 after preserving prior raster-line sprite
 *       decisions.
 *
 ****************************************************************************/

static void SPRxCTL  (unsigned int hpos, uae_u16 v, unsigned int num) { decide_sprites (hpos); SPRxCTL_1 (v, num, hpos); }
/****i* januae-custom-blitter-sprite-copper/SPRxPOS
 *
 *   NAME
 *       SPRxPOS()
 *
 *   SYNOPSIS
 *       static void SPRxPOS (unsigned int hpos, uae_u16 v, unsigned int num)
 *
 *   FUNCTION
 *       Synchronize sprite decisions and write the sprite position word.
 *
 *   INPUTS
 *       hpos -- horizontal beam position for the current register update
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *
 *   NOTES
 *       Delegates to SPRxPOS_1 after preserving prior raster-line sprite
 *       decisions.
 *
 ****************************************************************************/

static void SPRxPOS  (unsigned int hpos, uae_u16 v, unsigned int num) { decide_sprites (hpos); SPRxPOS_1 (v, num, hpos); }

/****i* januae-custom-blitter-sprite-copper/SPRxPTH
 *
 *   NAME
 *       SPRxPTH()
 *
 *   SYNOPSIS
 *       static void SPRxPTH (unsigned int hpos, uae_u16 v, unsigned int num)
 *
 *   FUNCTION
 *       Write the high halfword of a sprite DMA pointer.
 *
 *   INPUTS
 *       hpos -- horizontal beam position for the current register update
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *
 *   NOTES
 *       First calls decide_sprites(hpos); preserves the existing pointer low
 *       halfword.
 *
 ****************************************************************************/

static void SPRxPTH  (unsigned int hpos, uae_u16 v, unsigned int num)
{
    decide_sprites (hpos);
    spr[num].pt &= 0xffff;
    spr[num].pt |= (uae_u32)v << 16;
#if SPRITE_DEBUG > 0
    if (vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY) {
	write_log ("%d:%d:SPR%dPTH %06.6X\n", vpos, hpos, num, spr[num].pt);
    }
#endif
}

/****i* januae-custom-blitter-sprite-copper/SPRxPTL
 *
 *   NAME
 *       SPRxPTL()
 *
 *   SYNOPSIS
 *       static void SPRxPTL (unsigned int hpos, uae_u16 v, unsigned int num)
 *
 *   FUNCTION
 *       Write the low halfword of a sprite DMA pointer.
 *
 *   INPUTS
 *       hpos -- horizontal beam position for the current register update
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *
 *   NOTES
 *       First calls decide_sprites(hpos); unlike the blitter pointer low-half
 *       writes it does not mask bit zero.
 *
 ****************************************************************************/

static void SPRxPTL (unsigned int hpos, uae_u16 v, unsigned int num)
{
    decide_sprites (hpos);
    spr[num].pt &= ~0xffff;
    spr[num].pt |= v;
#if SPRITE_DEBUG > 0
     if (vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY) {
	write_log ("%d:%d:SPR%dPTL %06.6X\n", vpos, hpos, num, spr[num].pt);
     }
#endif
}

/****i* januae-custom-blitter-sprite-copper/CLXCON
 *
 *   NAME
 *       CLXCON()
 *
 *   SYNOPSIS
 *       static void CLXCON (uae_u16 v)
 *
 *   FUNCTION
 *       Set sprite/playfield collision-match and enable masks.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Selects six low-plane match bits and six enable bits from CLXCON,
 *       replacing the cached bitplane masks.
 *
 ****************************************************************************/

static void CLXCON (uae_u16 v)
{
    clxcon = v;
    clxcon_bpl_enable = (v >> 6) & 63;
    clxcon_bpl_match = v & 63;
}

/****i* januae-custom-blitter-sprite-copper/CLXCON2
 *
 *   NAME
 *       CLXCON2()
 *
 *   SYNOPSIS
 *       static void CLXCON2 (uae_u16 v)
 *
 *   FUNCTION
 *       Extend collision-match and enable masks for AGA bitplanes seven and
 *       eight.
 *
 *   INPUTS
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *
 *   NOTES
 *       Ignored when the active chipset lacks AGA; combines extension bits with
 *       cached CLXCON masks.
 *
 ****************************************************************************/

static void CLXCON2 (uae_u16 v)
{
    if (!(currprefs.chipset_mask & CSMASK_AGA))
	return;
    clxcon2 = v;
    clxcon_bpl_enable |= v & (0x40|0x80);
    clxcon_bpl_match |= (v & (0x01|0x02)) << 6;
}

/****i* januae-custom-blitter-sprite-copper/CLXDAT
 *
 *   NAME
 *       CLXDAT()
 *
 *   SYNOPSIS
 *       static uae_u16 CLXDAT (void)
 *
 *   FUNCTION
 *       Read and clear the latched sprite/playfield collision bits.
 *
 *   RESULT
 *       Returns the collision latch with bit 15 forced high, and then clears the
 *       latch.
 *
 *   NOTES
 *       Forces bit 15 of the returned word high, then clears the stored
 *       collision latch.
 *
 ****************************************************************************/

static uae_u16 CLXDAT (void)
{
    uae_u16 v = clxdat | 0x8000;
    clxdat = 0;
    return v;
}

#ifdef AGA

/****i* januae-custom-blitter-sprite-copper/COLOR_READ
 *
 *   NAME
 *       COLOR_READ()
 *
 *   SYNOPSIS
 *       static uae_u16 COLOR_READ (int num)
 *
 *   FUNCTION
 *       Read a 12-bit palette entry from the selected AGA color-bank half.
 *
 *   INPUTS
 *       num -- zero-based sprite channel number
 *
 *   RESULT
 *       Returns the selected 12-bit color or 0xFFFF when reading is unavailable.
 *
 *   NOTES
 *       Compiled only with AGA. Returns 0xFFFF when AGA or read mode is
 *       unavailable; BPLCON3 selects bank and nibble half.
 *
 ****************************************************************************/

static uae_u16 COLOR_READ (int num)
{
    int cr, cg, cb, colreg;
    uae_u16 cval;

    if (!(currprefs.chipset_mask & CSMASK_AGA) || !(bplcon2 & 0x0100))
	return 0xffff;

    colreg = ((bplcon3 >> 13) & 7) * 32 + num;
    cr = current_colors.color_regs_aga[colreg] >> 16;
    cg = (current_colors.color_regs_aga[colreg] >> 8) & 0xFF;
    cb = current_colors.color_regs_aga[colreg] & 0xFF;
    if (bplcon3 & 0x200)
	cval = ((cr & 15) << 8) | ((cg & 15) << 4) | ((cb & 15) << 0);
    else
	cval = ((cr >> 4) << 8) | ((cg >> 4) << 4) | ((cb >> 4) << 0);
    return cval;
}
#endif

/****i* januae-custom-blitter-sprite-copper/COLOR_WRITE
 *
 *   NAME
 *       COLOR_WRITE()
 *
 *   SYNOPSIS
 *       static void COLOR_WRITE (unsigned int hpos, uae_u16 v, int num)
 *
 *   FUNCTION
 *       Write an ECS or AGA palette entry with scanline change tracking.
 *
 *   INPUTS
 *       hpos -- horizontal beam position for the current register update
 *       v -- guest 16-bit value written to the addressed custom-chip register
 *       num -- zero-based sprite channel number
 *
 *   NOTES
 *       Masks value to 12 bits; in AGA read mode ignores writes. No change event
 *       is recorded when the effective color is unchanged.
 *
 ****************************************************************************/

static void COLOR_WRITE (unsigned int hpos, uae_u16 v, int num)
{
    v &= 0xFFF;
#ifdef AGA
    if (currprefs.chipset_mask & CSMASK_AGA) {
	unsigned int r,g,b;
	unsigned int cr,cg,cb;
	unsigned int colreg;
	uae_u32 cval;

	/* writing is disabled when RDRAM=1 */
	if (bplcon2 & 0x0100)
	    return;

	colreg = ((bplcon3 >> 13) & 7) * 32 + num;
	r = (v & 0xF00) >> 8;
	g = (v & 0xF0) >> 4;
	b = (v & 0xF) >> 0;
	cr =  current_colors.color_regs_aga[colreg] >> 16;
	cg = (current_colors.color_regs_aga[colreg] >> 8) & 0xFF;
	cb =  current_colors.color_regs_aga[colreg] & 0xFF;

	if (bplcon3 & 0x200) {
	    cr &= 0xF0; cr |= r;
	    cg &= 0xF0; cg |= g;
	    cb &= 0xF0; cb |= b;
	} else {
	    cr = r + (r << 4);
	    cg = g + (g << 4);
	    cb = b + (b << 4);
	}
	cval = (cr << 16) | (cg << 8) | cb;
	if (cval == current_colors.color_regs_aga[colreg])
	    return;

	/* Call this with the old table still intact. */
	record_color_change (hpos, colreg, cval);
	remembered_color_entry = -1;
	current_colors.color_regs_aga[colreg] = cval;
	current_colors.acolors[colreg] = CONVERT_RGB (cval);
   } else {
#endif
	if (current_colors.color_regs_ecs[num] == v)
	    return;
	/* Call this with the old table still intact. */
	record_color_change (hpos, num, v);
	remembered_color_entry = -1;
	current_colors.color_regs_ecs[num] = v;
	current_colors.acolors[num] = xcolors[v];
#ifdef AGA
    }
#endif
}

/* The copper code.  The biggest nightmare in the whole emulator.

   Alright.  The current theory:
   1. Copper moves happen 2 cycles after state READ2 is reached.
      It can't happen immediately when we reach READ2, because the
      data needs time to get back from the bus.  An additional 2
      cycles are needed for non-Agnus registers, to take into account
      the delay for moving data from chip to chip.
   2. As stated in the HRM, a WAIT really does need an extra cycle
      to wake up.  This is implemented by _not_ falling through from
      a successful wait to READ1, but by starting the next cycle.
      (Note: the extra cycle for the WAIT apparently really needs a
      free cycle; i.e. contention with the bitplane fetch can slow
      it down).
   3. Apparently, to compensate for the extra wake up cycle, a WAIT
      will use the _incremented_ horizontal position, so the WAIT
      cycle normally finishes two clocks earlier than the position
      it was waiting for.  The extra cycle then takes us to the
      position that was waited for.
      If the earlier cycle is busy with a bitplane, things change a bit.
      E.g., waiting for position 0x50 in a 6 plane display: In cycle
      0x4e, we fetch BPL5, so the wait wakes up in 0x50, the extra cycle
      takes us to 0x54 (since 0x52 is busy), then we have READ1/READ2,
      and the next register write is at 0x5c.
   4. The last cycle in a line is not usable for the copper.
   5. A 4 cycle delay also applies to the WAIT instruction.  This means
      that the second of two back-to-back WAITs (or a WAIT whose
      condition is immediately true) takes 8 cycles.
   6. This also applies to a SKIP instruction.  The copper does not
      fetch the next instruction while waiting for the second word of
      a WAIT or a SKIP to arrive.
   7. A SKIP also seems to need an unexplained additional two cycles
      after its second word arrives; this is _not_ a memory cycle (I
      think, the documentation is pretty clear on this).
   8. Two additional cycles are inserted when writing to COPJMP1/2.  */

/* Determine which cycles are available for the copper in a display
 * with a agiven number of planes.  */

/****i* januae-custom-blitter-sprite-copper/copper_cant_read
 *
 *   NAME
 *       copper_cant_read()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int copper_cant_read (unsigned int hpos)
 *
 *   FUNCTION
 *       Check whether the Copper can fetch an instruction at a given beam
 *       position.
 *
 *   INPUTS
 *       hpos -- horizontal beam position for the current register update
 *
 *   RESULT
 *       Returns nonzero when Copper fetch is unavailable; otherwise zero.
 *
 *   NOTES
 *       Blocks reads on the final horizontal slot and when bitplane DMA occupies
 *       the corresponding bus cycle.
 *
 ****************************************************************************/

STATIC_INLINE int copper_cant_read (unsigned int hpos)
{
    if (hpos + 1 >= maxhpos)
	return 1;
    return is_bitplane_dma_inline (hpos);
}

/****i* januae-custom-blitter-sprite-copper/dangerous_reg
 *
 *   NAME
 *       dangerous_reg()
 *
 *   SYNOPSIS
 *       STATIC_INLINE int dangerous_reg (int reg)
 *
 *   FUNCTION
 *       Classify a Copper register address using the simple safe-range rule.
 *
 *   INPUTS
 *       reg -- guest custom-register offset
 *
 *   RESULT
 *       Returns zero within the simple safe register range, otherwise one.
 *
 *   NOTES
 *       Returns zero only for addresses from 0xE0 through 0x1BE; this helper
 *       does not itself stop the Copper.
 *
 ****************************************************************************/

STATIC_INLINE int dangerous_reg (int reg)
{
    /* Safe:
     * Bitplane pointers, control registers, modulos and data.
     * Sprite pointers, control registers, and data.
     * Color registers.  */
    if (reg >= 0xE0 && reg < 0x1C0)
	return 0;
    return 1;
}

/****i* januae-custom-blitter-sprite-copper/test_copper_dangerous
 *
 *   NAME
 *       test_copper_dangerous()
 *
 *   SYNOPSIS
 *       static int test_copper_dangerous (unsigned int address)
 *
 *   FUNCTION
 *       Enforce Copper access restrictions for a target custom-register write.
 *
 *   INPUTS
 *       address -- guest custom-chip register address being checked
 *
 *   RESULT
 *       Returns 1 and stops the Copper for a prohibited write; otherwise returns
 *       0.
 *
 *   NOTES
 *       Uses COPCON and chipset capabilities to select the lower permitted
 *       register bound; on violation stops the Copper and clears its CPU special
 *       flag.
 *
 ****************************************************************************/

static int test_copper_dangerous (unsigned int address)
{
    if ((address & 0x1fe) < (copcon & 2 ? ((currprefs.chipset_mask & CSMASK_AGA) ? 0 : 0x40u) : 0x80u)) {
	cop_state.state = COP_stop;
	copper_enabled_thisline = 0;
	unset_special (&regs, SPCFLAG_COPPER);
	return 1;
    }
    return 0;
}

/****i* januae-custom-blitter-sprite-copper/perform_copper_write
 *
 *   NAME
 *       perform_copper_write()
 *
 *   SYNOPSIS
 *       static void perform_copper_write (unsigned int old_hpos)
 *
 *   FUNCTION
 *       Commit a delayed Copper MOVE or Copper list-jump strobe.
 *
 *   INPUTS
 *       old_hpos -- horizontal beam position at which the pending MOVE completes
 *
 *   NOTES
 *       Rejects protected addresses, handles 0x88/0x8A jump registers, and
 *       otherwise dispatches through custom_wput_1() at the prior beam position.
 *
 ****************************************************************************/

static void perform_copper_write (unsigned int old_hpos)
{
    unsigned int address = cop_state.saved_i1 & 0x1FE;

#ifdef DEBUGGER
    if (debug_copper)
	record_copper (cop_state.saved_ip - 4, old_hpos, vpos);
#endif

    if (test_copper_dangerous (address))
	return;

    if (address == 0x88) {
	cop_state.ip = cop1lc;
	cop_state.state = COP_strobe_delay;
    } else if (address == 0x8A) {
	cop_state.ip = cop2lc;
	cop_state.state = COP_strobe_delay;
    } else {
	custom_wput_1 (old_hpos, cop_state.saved_i1, cop_state.saved_i2, 0);
	cop_state.last_write = cop_state.saved_i1;
	cop_state.last_write_hpos = old_hpos;
	old_hpos++;
	if (cop_state.saved_i1 >= 0x140 && cop_state.saved_i1 < 0x180 && old_hpos >= SPR0_HPOS && old_hpos < SPR0_HPOS + 4 * MAX_SPRITES) {
	    //write_log ("%d:%d %04.4X:%04.4X\n", vpos, old_hpos, cop_state.saved_i1, cop_state.saved_i2);
	    do_sprites (old_hpos);
	}
    }
}

static int isagnus[]= {
    1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,1,1,1,1,0,0,0,0,0,0,0,0, /* 32 0x00 - 0x3e */
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, /* 27 0x40 - 0x74 */

    0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0, /* 21 */
    1,1,0,0,0,0,0,0,1,1,0,0,0,0,0,0,1,1,0,0,0,0,0,0,1,1,0,0,0,0,0,0, /* 32 0xa0 - 0xde */
    /* BPLxPTH/BPLxPTL */
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, /* 16 */
    /* BPLCON0-3,BPLMOD1-2 */
    0,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0, /* 16 */
    /* SPRxPTH/SPRxPTL */
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, /* 16 */
    /* SPRxPOS/SPRxCTL/SPRxDATA/SPRxDATB */
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    /* COLORxx */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* RESERVED */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/****i* januae-custom-blitter-sprite-copper/dump_copper
 *
 *   NAME
 *       dump_copper()
 *
 *   SYNOPSIS
 *       static void dump_copper (const char *error, unsigned int until_hpos)
 *
 *   FUNCTION
 *       Emit a bounded diagnostic snapshot of the current Copper state.
 *
 *   INPUTS
 *       error -- text prefix identifying the Copper diagnostic condition
 *       until_hpos -- horizontal beam position through which Copper execution
 *           advances
 *
 *   NOTES
 *       Uses a static counter initially set to ten; once negative, further calls
 *       produce no output.
 *
 ****************************************************************************/

static void dump_copper (const char *error, unsigned int until_hpos)
{
    static int warned = 10;

    if (warned < 0)
	return;
    warned--;
    write_log("%s: vpos=%d until_hpos=%d\n",
	error, vpos, until_hpos);
    write_log("cvcmp=%d chcmp=%d chpos=%d cvpos=%d ci1=%04.4X ci2=%04.4X\n",
	cop_state.vcmp,cop_state.hcmp,cop_state.hpos,cop_state.vpos,cop_state.saved_i1,cop_state.saved_i2);
    write_log("cstate=%d ip=%08.8X ev_copper=%d\n",
	cop_state.state,cop_state.ip,eventtab[ev_copper].active);
}

/****i* januae-custom-blitter-sprite-copper/update_copper
 *
 *   NAME
 *       update_copper()
 *
 *   SYNOPSIS
 *       static void update_copper (unsigned int until_hpos)
 *
 *   FUNCTION
 *       Advance the Copper state machine through the requested horizontal
 *       position.
 *
 *   INPUTS
 *       until_hpos -- horizontal beam position through which Copper execution
 *           advances
 *
 *   NOTES
 *       Models instruction fetch contention, MOVE delivery delay, WAIT/SKIP and
 *       blitter wait; clamps processing to the current line and persists
 *       cop_state.hpos.
 *
 ****************************************************************************/

static void update_copper (unsigned int until_hpos)
{
    unsigned int vp = vpos & (((cop_state.saved_i2 >> 8) & 0x7F) | 0x80);
    unsigned int c_hpos = cop_state.hpos;

    if (eventtab[ev_copper].active) {
	dump_copper ("error1",until_hpos);
	eventtab[ev_copper].active = 0;
	return;
    }

    if (cop_state.state == COP_wait && vp < cop_state.vcmp) {
	dump_copper ("error2",until_hpos);
	eventtab[ev_copper].active = 0;
	copper_enabled_thisline = 0;
	return;
    }

    until_hpos &= ~1;

    if (until_hpos > (maxhpos & ~1))
	until_hpos = maxhpos & ~1;

    until_hpos += 2;
    for (;;) {
	unsigned int old_hpos = c_hpos;
	unsigned int hp;

	if (c_hpos >= until_hpos)
	    break;

	/* So we know about the fetch state.  */
	decide_line (c_hpos);

	switch (cop_state.state) {
	case COP_read1_in2:
	    cop_state.state = COP_read1;
	    break;
	case COP_read1_wr_in2:
	    cop_state.state = COP_read1;
	    perform_copper_write (old_hpos);
	    /* That could have turned off the copper.  */
	    if (! copper_enabled_thisline)
		goto out;

	    break;
	case COP_read1_wr_in4:
	    cop_state.state = COP_read1_wr_in2;
	    break;
	case COP_read2_wr_in2:
	    cop_state.state = COP_read2;
	    perform_copper_write (old_hpos);
	    /* That could have turned off the copper.  */
	    if (! copper_enabled_thisline)
		goto out;

	    break;
	case COP_wait_in2:
	    cop_state.state = COP_wait1;
	    break;
	case COP_wait_in4:
	    cop_state.state = COP_wait_in2;
	    break;
	case COP_skip_in2:
	    cop_state.state = COP_skip1;
	    break;
	case COP_skip_in4:
	    cop_state.state = COP_skip_in2;
	    break;
	case COP_strobe_delay:
	    cop_state.state = COP_read1_in2;
	    break;

	default:
	    break;
	}

	c_hpos += 2;
	if (cop_state.strobe) {
	    if (cop_state.strobe > 0)
		cop_state.ip = cop_state.strobe == 1 ? cop1lc : cop2lc;
	    cop_state.strobe = 0;
	}

	switch (cop_state.state) {

	case COP_read1_wr_in4:
	    uae_abort ("COP_read1_wr_in4");

	case COP_read1_wr_in2:
	case COP_read1:
	    if (copper_cant_read (old_hpos))
		continue;
	    cop_state.i1 = chipmem_wget (cop_state.ip);
#ifdef CPUEMU_6
	    cycle_line[old_hpos] |= CYCLE_COPPER;
#endif
	    cop_state.ip += 2;
	    cop_state.state = cop_state.state == COP_read1 ? COP_read2 : COP_read2_wr_in2;
	    break;

	case COP_read2_wr_in2:
	    uae_abort ("read2_wr_in2");

	case COP_read2:
	    if (copper_cant_read (old_hpos))
		continue;
	    cop_state.i2 = chipmem_wget (cop_state.ip);
#ifdef CPUEMU_6
	    cycle_line[old_hpos] |= CYCLE_COPPER;
#endif
	    cop_state.ip += 2;
	    if (cop_state.ignore_next) {
		cop_state.ignore_next = 0;
		cop_state.state = COP_read1;
		break;
	    }

	    cop_state.saved_i1 = cop_state.i1;
	    cop_state.saved_i2 = cop_state.i2;
	    cop_state.saved_ip = cop_state.ip;

	    if (cop_state.i1 & 1) {
		if (cop_state.i2 & 1)
		    cop_state.state = COP_skip_in4;
		else
		    cop_state.state = COP_wait_in4;
	    } else {
		unsigned int reg = cop_state.i1 & 0x1FE;
		cop_state.state = isagnus[reg >> 1] ? COP_read1_wr_in2 : COP_read1_wr_in4;
	    }
	    break;

	case COP_wait1:
	    /* There's a nasty case here.  As stated in the "Theory" comment above, we
	       test against the incremented copper position.  I believe this means that
	       we have to increment the _vertical_ position at the last cycle in the line,
	       and set the horizontal position to 0.
	       Normally, this isn't going to make a difference, since we consider these
	       last cycles unavailable for the copper, so waking up in the last cycle has
	       the same effect as waking up at the start of the line.  However, there is
	       one possible problem:  If we're at 0xFFE0, any wait for an earlier position
	       must _not_ complete (since, in effect, the current position will be back
	       at 0/0).  This can be seen in the Superfrog copper list.
	       Things get monstrously complicated if we try to handle this "properly" by
	       incrementing vpos and setting c_hpos to 0.  Especially the various speedup
	       hacks really assume that vpos remains constant during one line.  Hence,
	       this hack: defer the entire decision until the next line if necessary.  */
	    if (c_hpos >= (maxhpos & ~1))
		break;

	    cop_state.state = COP_wait;

	    cop_state.vcmp = (cop_state.saved_i1 & (cop_state.saved_i2 | 0x8000)) >> 8;
	    cop_state.hcmp = (cop_state.saved_i1 & cop_state.saved_i2 & 0xFE);

	    vp = vpos & (((cop_state.saved_i2 >> 8) & 0x7F) | 0x80);

	    if (cop_state.saved_i1 == 0xFFFF && cop_state.saved_i2 == 0xFFFE) {
		cop_state.state = COP_stop;
		copper_enabled_thisline = 0;
		unset_special (&regs, SPCFLAG_COPPER);
		goto out;
	    }
	    if (vp < cop_state.vcmp) {
		copper_enabled_thisline = 0;
		unset_special (&regs, SPCFLAG_COPPER);
		goto out;
	    }

	    /* fall through */
	case COP_wait:
	    if (vp < cop_state.vcmp)
		uae_abort ("vp < cop_state.vcmp");
	    if (copper_cant_read (old_hpos))
		continue;

	    hp = c_hpos & (cop_state.saved_i2 & 0xFE);
	    if (vp == cop_state.vcmp && hp < cop_state.hcmp)
		break;

	    /* Now we know that the comparisons were successful.  We might still
	       have to wait for the blitter though.  */
	    if ((cop_state.saved_i2 & 0x8000) == 0 && (DMACONR() & 0x4000)) {
		/* We need to wait for the blitter.  */
		cop_state.state = COP_bltwait;
		copper_enabled_thisline = 0;
		unset_special (&regs, SPCFLAG_COPPER);
		goto out;
	    }

#ifdef DEBUGGER
	    if (debug_copper)
		record_copper (cop_state.ip - 4, old_hpos, vpos);
#endif

	    cop_state.state = COP_read1;
	    break;

	case COP_skip1:
	{
	    static int skipped_before;
	    unsigned int vcmp, hcmp, vp1, hp1;

	    if (! skipped_before) {
		skipped_before = 1;
		write_log ("Program uses Copper SKIP instruction.\n");
	    }

	    if (c_hpos >= (maxhpos & ~1))
		break;

	    vcmp = (cop_state.saved_i1 & (cop_state.saved_i2 | 0x8000)) >> 8;
	    hcmp = (cop_state.saved_i1 & cop_state.saved_i2 & 0xFE);
	    vp1 = vpos & (((cop_state.saved_i2 >> 8) & 0x7F) | 0x80);
	    hp1 = c_hpos & (cop_state.saved_i2 & 0xFE);

	    if ((vp1 > vcmp || (vp1 == vcmp && hp1 >= hcmp))
		&& ((cop_state.saved_i2 & 0x8000) != 0 || ! (DMACONR() & 0x4000)))
		cop_state.ignore_next = 1;
	    if (chipmem_wget (cop_state.ip) & 1) { /* FIXME: HACK!!! */
		/* copper never skips if following instruction is WAIT or another SKIP... */
		cop_state.ignore_next = 0;
	    }

	    cop_state.state = COP_read1;

	    if (cop_state.ignore_next && (chipmem_wget (cop_state.ip) & 1) == 0) {
		/* another undocumented copper feature:
		   copper stops if skipped instruction is MOVE to dangerous register...
		*/
		test_copper_dangerous (chipmem_wget(cop_state.ip));
	    }

#ifdef DEBUGGER
	    if (debug_copper)
		record_copper (cop_state.ip - 4, old_hpos, vpos);
#endif

	    break;
	}
	default:
	    break;
	}
    }

  out:
    cop_state.hpos = c_hpos;
}

/****i* januae-custom-blitter-sprite-copper/compute_spcflag_copper
 *
 *   NAME
 *       compute_spcflag_copper()
 *
 *   SYNOPSIS
 *       static void compute_spcflag_copper (void)
 *
 *   FUNCTION
 *       Recompute whether Copper execution should set the CPU special flag.
 *
 *   NOTES
 *       Disables scheduling when Copper DMA is off, the state is stopped or
 *       waiting for the blitter, or an unsatisfied vertical WAIT remains.
 *
 ****************************************************************************/

static void compute_spcflag_copper (void)
{
    copper_enabled_thisline = 0;
    unset_special (&regs, SPCFLAG_COPPER);
    if (! dmaen (DMA_COPPER) || cop_state.state == COP_stop || cop_state.state == COP_bltwait)
	return;

    if (cop_state.state == COP_wait) {
	unsigned int vp = vpos & (((cop_state.saved_i2 >> 8) & 0x7F) | 0x80);

	if (vp < cop_state.vcmp)
	    return;
    }
    copper_enabled_thisline = 1;

    if (! eventtab[ev_copper].active)
	set_special (&regs, SPCFLAG_COPPER);
}

/****i* januae-custom-blitter-sprite-copper/copper_handler
 *
 *   NAME
 *       copper_handler()
 *
 *   SYNOPSIS
 *       void copper_handler (void)
 *
 *   FUNCTION
 *       Transfer an active scheduled Copper event into CPU special-flag
 *       processing.
 *
 *   NOTES
 *       Sets SPCFLAG_COPPER and clears the event; aborts if invoked while the
 *       Copper is disabled on this line.
 *
 ****************************************************************************/

void copper_handler (void)
{
    /* This will take effect immediately, within the same cycle.  */
    set_special (&regs, SPCFLAG_COPPER);

    if (! copper_enabled_thisline)
	uae_abort ("copper_handler");

    eventtab[ev_copper].active = 0;
}

/****i* januae-custom-blitter-sprite-copper/blitter_done_notify
 *
 *   NAME
 *       blitter_done_notify()
 *
 *   SYNOPSIS
 *       void blitter_done_notify (void)
 *
 *   FUNCTION
 *       Resume a Copper WAIT that was suspended on blitter completion.
 *
 *   NOTES
 *       Only acts in COP_bltwait; records current beam coordinates, introduces a
 *       two-cycle wait-state transition, and recomputes scheduling.
 *
 ****************************************************************************/

void blitter_done_notify (void)
{
    if (cop_state.state != COP_bltwait)
	return;

    cop_state.hpos = current_hpos () & ~1;
    cop_state.vpos = vpos;
    /* apparently there is small delay until copper wakes up.. */
    cop_state.state = COP_wait_in2;
    compute_spcflag_copper ();
}

/****i* januae-custom-blitter-sprite-copper/do_copper
 *
 *   NAME
 *       do_copper()
 *
 *   SYNOPSIS
 *       void do_copper (void)
 *
 *   FUNCTION
 *       Advance the Copper emulation to the current CPU beam position.
 *
 *   NOTES
 *       Delegates to update_copper(current_hpos()) without changing the
 *       requested target beam position.
 *
 ****************************************************************************/

void do_copper (void)
{
    unsigned int hpos = current_hpos ();
    update_copper (hpos);
}

/* ADDR is the address that is going to be read/written; this access is
   the reason why we want to update the copper.  This function is also
   used from hsync_handler to finish up the line; for this case, we check
   hpos against maxhpos.  */
/****i* januae-custom-blitter-sprite-copper/sync_copper_with_cpu
 *
 *   NAME
 *       sync_copper_with_cpu()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void sync_copper_with_cpu (unsigned int hpos, int
 *       do_schedule)
 *
 *   FUNCTION
 *       Synchronize Copper progress before CPU register access or line
 *       completion.
 *
 *   INPUTS
 *       hpos -- horizontal beam position for the current register update
 *       do_schedule -- nonzero to recompute the event schedule when cancelling
 *           an event
 *
 *   NOTES
 *       Cancels an active Copper event, optionally schedules events, sets
 *       SPCFLAG_COPPER and advances Copper state only if enabled this line.
 *
 ****************************************************************************/

STATIC_INLINE void sync_copper_with_cpu (unsigned int hpos, int do_schedule)
{
    /* Need to let the copper advance to the current position.  */
    if (eventtab[ev_copper].active) {
	eventtab[ev_copper].active = 0;
	if (do_schedule)
	    events_schedule ();
	set_special (&regs, SPCFLAG_COPPER);
    }
    if (copper_enabled_thisline)
	update_copper (hpos);
}

/****i* januae-custom-frame-memory/sprite_fetch
 *
 *   NAME
 *       sprite_fetch()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u16 sprite_fetch (struct sprite *s, int dma, unsigned int hpos, int cycle, int mode)
 *
 *   FUNCTION
 *       Fetch a sprite word from chip memory when DMA is enabled.
 *
 *   INPUTS
 *       dma -- whether to read chip memory
 *
 *   RESULT
 *       The fetched word, or the preceding custom-bus value if DMA is disabled.
 *
 *   NOTES
 *       Advances the sprite pointer by two bytes in either case.
 *
 ****************************************************************************/

STATIC_INLINE uae_u16 sprite_fetch (struct sprite *s, int dma, unsigned int hpos, int cycle, int mode)
{
    uae_u16 data = last_custom_value;
    if (dma) {
	data = last_custom_value = chipmem_wget (s->pt);
#ifdef CPUEMU_6
	cycle_line[hpos] |= CYCLE_SPRITE;
#endif
    }
    s->pt += 2;
    return data;
}

/****i* januae-custom-frame-memory/do_sprites_1
 *
 *   NAME
 *       do_sprites_1()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void do_sprites_1 (unsigned int num, int cycle, unsigned int hpos)
 *
 *   FUNCTION
 *       Process one sprite DMA slot at the current beam position.
 *
 *   INPUTS
 *       num -- sprite index
 *       cycle -- DMA slot within the sprite
 *       hpos -- beam position
 *
 *   NOTES
 *       Updates the sprite DMA state and, when enabled, sprite data or control registers.
 *
 ****************************************************************************/

STATIC_INLINE void do_sprites_1 (unsigned int num, int cycle, unsigned int hpos)
{
    struct sprite *s = &spr[num];
    int dma, posctl = 0;
    uae_u16 data;

    if (vpos == sprite_vblank_endline)
	spr_arm (num, 0);

#if SPRITE_DEBUG > 3
    if (vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY)
	write_log("%d:%d:slot%d:%d\n", vpos, hpos, num, cycle);
#endif
    if (vpos == s->vstart) {
#if SPRITE_DEBUG > 0
	if (!s->dmastate && vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY)
	    write_log ("%d:%d:SPR%d START\n", vpos, hpos, num);
#endif
	s->dmastate = 1;
    }
    if (vpos == s->vstop || vpos == sprite_vblank_endline) {
#if SPRITE_DEBUG > 0
	if (s->dmastate && vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY)
	    write_log ("%d:%d:SPR%d STOP\n", vpos, hpos, num);
#endif
	s->dmastate = 0;
    }
    if (!dmaen (DMA_SPRITE))
	return;
    if (cycle && !s->dmacycle)
	return; /* Superfrog intro flashing bee fix */

    dma = hpos < plfstrt || diwstate != DIW_waiting_stop;
    if (vpos == s->vstop || vpos == sprite_vblank_endline) {
	s->dmastate = 0;
	posctl = 1;
	if (dma) {
	    data = sprite_fetch (s, dma, hpos, cycle, 0);
	    switch (sprite_width)
	    {
		case 64:
		sprite_fetch (s, dma, hpos, cycle, 0);
		sprite_fetch (s, dma, hpos, cycle, 0);
		case 32:
		sprite_fetch (s, dma, hpos, cycle, 0);
		break;
	    }
	} else {
	    data = cycle == 0 ? sprpos[num] : sprctl[num];
	}
#if SPRITE_DEBUG > 1
	if (vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY) {
	    write_log ("%d:%d:dma:P=%06.6X ", vpos, hpos, s->pt);
	}
#endif
	//write_log ("%d:%d: %04.4X=%04.4X\n", vpos, hpos, 0x140 + cycle * 2 + num * 8, data);
	if (cycle == 0) {
	    SPRxPOS_1 (data, num, hpos);
	    s->dmacycle = 1;
	} else {
	    SPRxCTL_1 (data, num, hpos);
	    s->dmastate = 0;
	    sprstartstop (s);
	}
    }
    if (s->dmastate && !posctl) {
	uae_u16 data;

	data = sprite_fetch (s, dma, hpos, cycle, 1);

#if SPRITE_DEBUG > 1
	if (vpos >= SPRITE_DEBUG_MINY && vpos <= SPRITE_DEBUG_MAXY) {
	    write_log ("%d:%d:dma:P=%06.6X ", vpos, hpos, s->pt);
	}
#endif
	if (cycle == 0) {
	    SPRxDATA_1 (dma ? data : sprdata[num][0], num, hpos);
	    s->dmacycle = 1;
	} else {
	    SPRxDATB_1 (dma ? data : sprdatb[num][0], num, hpos);
	    spr_arm (num, 1);
	}
#ifdef AGA
	switch (sprite_width)
	{
	    case 64:
	    {
		uae_u16 data32 = sprite_fetch (s, dma, hpos, cycle, 1);
		uae_u16 data641 = sprite_fetch (s, dma, hpos, cycle, 1);
		uae_u16 data642 = sprite_fetch (s, dma, hpos, cycle, 1);
		if (dma) {
		    if (cycle == 0) {
			sprdata[num][3] = data642;
			sprdata[num][2] = data641;
			sprdata[num][1] = data32;
		    } else {
			sprdatb[num][3] = data642;
			sprdatb[num][2] = data641;
			sprdatb[num][1] = data32;
		    }
		}
	    }
	    break;
	    case 32:
	    {
		uae_u16 data32 = sprite_fetch (s, dma, hpos, cycle, 1);
		if (dma) {
		    if (cycle == 0)
			sprdata[num][1] = data32;
		    else
			sprdatb[num][1] = data32;
		}
	    }
	    break;
	}
#endif
    }
}

/****i* januae-custom-frame-memory/do_sprites
 *
 *   NAME
 *       do_sprites()
 *
 *   SYNOPSIS
 *       static void do_sprites (unsigned int hpos)
 *
 *   FUNCTION
 *       Process sprite DMA slots up to the specified horizontal position.
 *
 *   INPUTS
 *       hpos -- end position for sprite DMA processing
 *
 *   NOTES
 *       Skips processing during the initial vertical blanking interval.
 *
 ****************************************************************************/

static void do_sprites (unsigned int hpos)
{
    unsigned int maxspr, minspr;
    unsigned int i;

    /* I don't know whether this is right. Some programs write the sprite pointers
     * directly at the start of the copper list. With the test against currvp, the
     * first two words of data are read on the second line in the frame. The problem
     * occurs when the program jumps to another copperlist a few lines further down
     * which _also_ writes the sprite pointer registers. This means that a) writing
     * to the sprite pointers sets the state to SPR_restart; or b) that sprite DMA
     * is disabled until the end of the vertical blanking interval. The HRM
     * isn't clear - it says that the vertical sprite position can be set to any
     * value, but this wouldn't be the first mistake... */
    /* Update: I modified one of the programs to write the sprite pointers the
     * second time only _after_ the VBlank interval, and it showed the same behaviour
     * as it did unmodified under UAE with the above check. This indicates that the
     * solution below is correct. */
    /* Another update: seems like we have to use the NTSC value here (see Sanity Turmoil
     * demo).  */
    /* Maximum for Sanity Turmoil: 27.
       Minimum for Sanity Arte: 22.  */
    if (vpos < sprite_vblank_endline)
	return;

#ifndef CUSTOM_SIMPLE
    maxspr = hpos;
    minspr = last_sprite_hpos;

    if (minspr >= SPR0_HPOS + MAX_SPRITES * 4 || maxspr < SPR0_HPOS)
	return;

    if (maxspr > SPR0_HPOS + MAX_SPRITES * 4)
	maxspr = SPR0_HPOS + MAX_SPRITES * 4;
    if (minspr < SPR0_HPOS)
	minspr = SPR0_HPOS;

    for (i = minspr; i < maxspr; i++) {
	int cycle = -1;
	unsigned int num = (i - SPR0_HPOS) / 4;
	switch ((i - SPR0_HPOS) & 3)
	    {
	    case 0:
	    cycle = 0;
	    spr[num].dmacycle = 0;
	    break;
	    case 2:
	    cycle = 1;
	    break;
	}
	if (cycle >= 0)
	    do_sprites_1 (num, cycle, i);
    }
    last_sprite_hpos = hpos;
#else
    for (i = 0; i < MAX_SPRITES * 2; i++) {
	spr[i / 2].dmacycle = 1;
	do_sprites_1 (i / 2, i & 1, 0);
    }
#endif
}

/****i* januae-custom-frame-memory/init_sprites
 *
 *   NAME
 *       init_sprites()
 *
 *   SYNOPSIS
 *       static void init_sprites (void)
 *
 *   FUNCTION
 *       Clear cached sprite position and control register arrays.
 *
 ****************************************************************************/

static void init_sprites (void)
{
    memset (sprpos, 0, sizeof sprpos);
    memset (sprctl, 0, sizeof sprctl);
}

/*
 * On systems without virtual memory or with low memory, we allocate the
 * sprite_entries and color_changes tables dynamically rather than having
 * them declared static. We don't initially allocate at their maximum sizes;
 * we start the tables off small and grow them as required.
 *
 * This function expands the tables if necessary.
 */
/****i* januae-custom-frame-memory/adjust_array_sizes
 *
 *   NAME
 *       adjust_array_sizes()
 *
 *   SYNOPSIS
 *       static void adjust_array_sizes (void)
 *
 *   FUNCTION
 *       Grow sprite and color-change tables when requested on systems without memory management.
 *
 *   NOTES
 *       Only active with OS_WITHOUT_MEMORY_MANAGEMENT; allocation failures are logged.
 *
 ****************************************************************************/

static void adjust_array_sizes (void)
{
#ifdef OS_WITHOUT_MEMORY_MANAGEMENT
    if (delta_sprite_entry) {
	void *p1;
	void *p2;
	int   mcc = max_sprite_entry + 50 + delta_sprite_entry;

	delta_sprite_entry = 0;

	p1 = realloc (sprite_entries[0], mcc * sizeof (struct sprite_entry));
	p2 = realloc (sprite_entries[1], mcc * sizeof (struct sprite_entry));

	if (p1 && p2) {
	    sprite_entries[0] = p1;
	    sprite_entries[1] = p2;

	    memset (&sprite_entries[0][max_sprite_entry], (mcc - max_sprite_entry) * sizeof(struct sprite_entry), 0);
	    memset (&sprite_entries[1][max_sprite_entry], (mcc - max_sprite_entry) * sizeof(struct sprite_entry), 0);

	    write_log ("New max_sprite_entry=%d\n", mcc);

	    max_sprite_entry = mcc;
	} else
	    write_log ("WARNING: Failed to enlarge sprite_entries table\n");
    }
    if (delta_color_change) {
	void *p1;
	void *p2;
	int   mcc = max_color_change + 200 + delta_color_change;

	delta_color_change = 0;

	p1 = realloc (color_changes[0], mcc * sizeof (struct color_change));
	p2 = realloc (color_changes[1], mcc * sizeof (struct color_change));

	if (p1 && p2) {
	    color_changes[0] = p1;
	    color_changes[1] = p2;

	    write_log ("New max_color_change=%d\n", mcc);

	    max_color_change = mcc;
	} else
	    write_log ("WARNING: Failed to enlarge color_changes table\n");
    }
#endif
}

/****i* januae-custom-frame-memory/init_hardware_frame
 *
 *   NAME
 *       init_hardware_frame()
 *
 *   SYNOPSIS
 *       static void init_hardware_frame (void)
 *
 *   FUNCTION
 *       Reset drawing and display-window tracking for a new frame.
 *
 *   NOTES
 *       Decrements the pending interlace transition count when positive.
 *
 ****************************************************************************/

static void init_hardware_frame (void)
{
    next_lineno = 0;
    nextline_how = nln_normal;
    diwstate = DIW_waiting_start;
    hdiwstate = DIW_waiting_start;
    ddfstate = DIW_waiting_start;
    if (interlace_started > 0)
	interlace_started--;
}

/****i* januae-custom-frame-memory/init_hardware_for_drawing_frame
 *
 *   NAME
 *       init_hardware_for_drawing_frame()
 *
 *   SYNOPSIS
 *       void init_hardware_for_drawing_frame (void)
 *
 *   FUNCTION
 *       Prepare drawing buffers and line state for the next frame.
 *
 *   NOTES
 *       Clears pixels belonging to previous sprite entries when available.
 *
 ****************************************************************************/

void init_hardware_for_drawing_frame (void)
{
    /* Avoid this code in the first frame after a customreset.  */
    if (prev_sprite_entries) {
	int first_pixel = prev_sprite_entries[0].first_pixel;
	int npixels = prev_sprite_entries[prev_next_sprite_entry].first_pixel - first_pixel;
	memset (spixels + first_pixel, 0, npixels * sizeof *spixels);
	memset (spixstate.bytes + first_pixel, 0, npixels * sizeof *spixstate.bytes);
    }
    prev_next_sprite_entry = next_sprite_entry;

    next_color_change = 0;
    next_sprite_entry = 0;
    next_color_entry = 0;
    remembered_color_entry = -1;

    adjust_array_sizes ();

    prev_sprite_entries = sprite_entries[current_change_set];
    curr_sprite_entries = sprite_entries[current_change_set ^ 1];
    prev_color_changes = color_changes[current_change_set];
    curr_color_changes = color_changes[current_change_set ^ 1];
    prev_color_tables = color_tables[current_change_set];
    curr_color_tables = color_tables[current_change_set ^ 1];

    prev_drawinfo = line_drawinfo[current_change_set];
    curr_drawinfo = line_drawinfo[current_change_set ^ 1];
    current_change_set ^= 1;

    color_src_match = color_dest_match = -1;

    /* Use both halves of the array in alternating fashion.  */
    curr_sprite_entries[0].first_pixel = current_change_set * MAX_SPR_PIXELS;
    next_sprite_forced = 1;
}

/*
 * Convert time from syncbase ticks to milliseconds.
 */
/****i* januae-custom-frame-memory/frame_time_to_ms
 *
 *   NAME
 *       frame_time_to_ms()
 *
 *   SYNOPSIS
 *       STATIC_INLINE double frame_time_to_ms (frame_time_t time)
 *
 *   FUNCTION
 *       Convert high-resolution timer ticks to milliseconds.
 *
 *   INPUTS
 *       time -- duration expressed in syncbase ticks
 *
 *   RESULT
 *       Duration in milliseconds.
 *
 ****************************************************************************/

STATIC_INLINE double frame_time_to_ms (frame_time_t time)
{
     return (double) time / (syncbase / 1000.0);
}

/*
 * Wait until end of frame in a system-friendly, non-CPU hogging way
 * if possible.
 */
/****i* januae-custom-frame-memory/framewait_friendly
 *
 *   NAME
 *       framewait_friendly()
 *
 *   SYNOPSIS
 *       static frame_time_t framewait_friendly (frame_time_t end_time)
 *
 *   FUNCTION
 *       Wait for the frame deadline using short sleeps followed by polling.
 *
 *   INPUTS
 *       end_time -- high-resolution timer deadline
 *
 *   RESULT
 *       Timer reading obtained after waiting or detecting an out-of-range interval.
 *
 *   NOTES
 *       Accumulates elapsed waiting time in idletime on the normal path.
 *
 ****************************************************************************/

static frame_time_t framewait_friendly (frame_time_t end_time)
{
    frame_time_t start_time = uae_gethrtime ();
    frame_time_t curr_time;
    frame_time_t time_left;

    /*
     * Use system sleep routine to get close to end_time
     */
    for (;;) {
	curr_time = uae_gethrtime ();
	time_left = end_time - curr_time;

	if (time_left < (-syncbase) || time_left > syncbase) {
	    /* Either timer has screwed up or we've badly overshot end_time */
	    write_log ("framewait overflow\n");
	    return curr_time;
	} else {
	    static int min_sleep = 3; /* Estimate of how long a 2ms sleep will actually take */

	    if (frame_time_to_ms (time_left) >= min_sleep) {
		static int count = 0;
		static int total = 0;

	        uae_msleep (2);

		/* Callibrate time we slept for and try to adjust to
		 * changing lantencies.
		 * TODO: This stuff needs tidying up, fine-tuning and some way
		 * of attempting to back-off busy-waiting.
		 */
		total += frame_time_to_ms (uae_gethrtime () - curr_time);
		count++;

		if ((count & 31) == 0) {
		    min_sleep = 1 + total / 32;
		    total = 0;
		    count = 0;
		}
	    } else
	        break;
	}
    }

    /*
     * Busy-wait remaining time.
     */
    do {
	curr_time = uae_gethrtime ();
	time_left = end_time - curr_time;

	if (time_left < (-syncbase) || time_left > syncbase) {
	    time_left = 0;
	}
    } while (time_left >= 0);

    idletime += curr_time - start_time;

    return curr_time;
}

/*
 * Wait for end of frame using CPU-hogging, polling method.
 */
/****i* januae-custom-frame-memory/framewait_busy
 *
 *   NAME
 *       framewait_busy()
 *
 *   SYNOPSIS
 *       static frame_time_t framewait_busy (frame_time_t end_time)
 *
 *   FUNCTION
 *       Poll the high-resolution timer until the frame deadline passes.
 *
 *   INPUTS
 *       end_time -- high-resolution timer deadline
 *
 *   RESULT
 *       Timer reading obtained after waiting or detecting an out-of-range interval.
 *
 *   NOTES
 *       Accumulates elapsed waiting time in idletime on the normal path.
 *
 ****************************************************************************/

static frame_time_t framewait_busy (frame_time_t end_time)
{
    frame_time_t start_time = uae_gethrtime ();
    frame_time_t curr_time;
    frame_time_t time_left;

    do {
	curr_time = uae_gethrtime ();
	time_left = end_time - curr_time;

	if (time_left < (-syncbase) || time_left > syncbase) {
	    /* Either timer has screwed up or we've badly overshot end_time */
	    write_log ("framewait overflow\n");
	    return curr_time;
	}
    } while (time_left >= 0);

    idletime += curr_time - start_time;

    return curr_time;
}

/****i* januae-custom-frame-memory/framewait
 *
 *   NAME
 *       framewait()
 *
 *   SYNOPSIS
 *       static void framewait (void)
 *
 *   FUNCTION
 *       Wait for the scheduled vertical-sync deadline and advance it.
 *
 *   NOTES
 *       The current implementation selects framewait_friendly unconditionally.
 *
 ****************************************************************************/

static void framewait (void)
{
    frame_time_t curr_time;

    /* TODO: Make this a compile-time option. */
    if (1)
	curr_time = framewait_friendly (vsyncmintime);
    else
	curr_time = framewait_busy (vsyncmintime);

    vsyncmintime = curr_time + vsynctime;
}

static frame_time_t frametime2;

/****i* januae-custom-frame-memory/fpscounter_reset
 *
 *   NAME
 *       fpscounter_reset()
 *
 *   SYNOPSIS
 *       void fpscounter_reset (void)
 *
 *   FUNCTION
 *       Reset frame-rate accounting and record the current timer value.
 *
 *   NOTES
 *       Marks the next two frames as unsuitable for timing statistics.
 *
 ****************************************************************************/

void fpscounter_reset (void)
{
    timeframes = 0;
    frametime2 = 0;
    bogusframe = 2;
    lastframetime = uae_gethrtime ();
    idletime = 0;
}

/****i* januae-custom-frame-memory/fpscounter
 *
 *   NAME
 *       fpscounter()
 *
 *   SYNOPSIS
 *       static void fpscounter (void)
 *
 *   FUNCTION
 *       Accumulate frame timing and refresh frame-rate statistics.
 *
 *   NOTES
 *       Skips accumulation while bogusframe is nonzero.
 *
 ****************************************************************************/

static void fpscounter (void)
{
    frame_time_t now, last;

    now = uae_gethrtime ();
    last = now - lastframetime;
    lastframetime = now;

    if (bogusframe)
	return;

    frametime += (1000*last/syncbase);
    frametime2 += last;
    timeframes++;
    if ((timeframes & 31) == 0) {
	double idle = 1000 - (idletime == 0 ? 0.0 : (double)idletime * 1000.0 / (vsynctime * 32.0));
	int fps = frametime2 == 0 ? 0 : syncbase * 32 / (frametime2 / 10);
	if (fps > 9999)
	    fps = 9999;
	if (idle < 0)
	    idle = 0;
	if (idle > 100 * 10)
	    idle = 100 * 10;
	if (fake_vblank_hz * 10 > fps) {
	    double mult = (double)fake_vblank_hz * 10.0 / fps;
	    idle *= mult;
	}
	if (turbo_emulation && idle < 100 * 10)
	    idle = 100 * 10;
	gui_fps (fps, (int)idle);
	frametime2 = 0;
	idletime = 0;
    }
}

/****i* januae-custom-timing-cycle/vsync_handler
 *
 *   NAME
 *       vsync_handler()
 *
 *   SYNOPSIS
 *       static void vsync_handler (void)
 *
 *   FUNCTION
 *       Complete vertical synchronization processing for the current frame.
 *
 *   NOTES
 *       Accounts for frame timing, delivers the vertical-blank interrupt, redraws the display, and resets Copper and drawing state.
 *
 ****************************************************************************/

static void vsync_handler (void)
{
    fpscounter ();

    if (!is_vsync ()
#ifdef AVIOUTPUT
	&& ((avioutput_framelimiter && avioutput_enabled) || !avioutput_enabled)
#endif
	) {
#ifdef JIT
	if (!compiled_code) {
#endif
	    if (currprefs.m68k_speed == -1) {
		frame_time_t curr_time = uae_gethrtime ();
		vsyncmintime += vsynctime;
		/* @@@ Mathias? How do you think we should do this? */
		/* If we are too far behind, or we just did a reset, adjust the
		 * needed time. */
		if ((curr_time - vsyncmintime) > 0 || rpt_did_reset)
		    vsyncmintime = curr_time + vsynctime;
		rpt_did_reset = 0;
	    } else {
		framewait ();
	    }
#ifdef JIT
	} else {
	    if (currprefs.m68k_speed == 0) {
		framewait ();
	    }
	}
#endif
    }

    if (bogusframe > 0)
	bogusframe--;

    gui_handle_events ();
    handle_events ();

    INTREQ (0x8020);
    if (bplcon0 & 4)
	lof ^= 0x8000;

#ifdef PICASSO96
    /* And now let's update the Picasso palette, if required */
#ifndef __AROS__
    DX_SetPalette_vsync();
#endif
    if (picasso_on)
	picasso_handle_vsync ();
#endif

   vsync_handle_redraw (lof, lof_changed);

    {
	static int cnt = 0;
	if (cnt == 0) {
	    /* resolution_check_change (); */
	    DISK_check_change ();
	    cnt = 5;
	}
	cnt--;
    }

#ifdef DEBUGGER
    if (debug_copper)
	record_copper_reset();
#endif

    /* For now, let's only allow this to change at vsync time.  It gets too
     * hairy otherwise.  */
    if ((beamcon0 & (0x20|0x80)) != (new_beamcon0 & (0x20|0x80)) || hack_vpos)
	init_hz ();

    lof_changed = 0;

    eventtab[ev_copper].active = 0;
    COPJMP (1);

    init_hardware_frame ();

    if (timehack_alive > 0)
	timehack_alive--;
    inputdevice_vsync ();
}

#ifdef JIT

#define N_LINES 8

/****i* januae-custom-timing-cycle/trigger_frh
 *
 *   NAME
 *       trigger_frh()
 *
 *   SYNOPSIS
 *       static __inline__ int trigger_frh(int v)
 *
 *   FUNCTION
 *       Test whether a scanline reaches the JIT frame-rate pacing interval.
 *
 *   INPUTS
 *       v -- current scanline number
 *
 *   RESULT
 *       Nonzero at a multiple of N_LINES; otherwise zero.
 *
 *   NOTES
 *       Available only with JIT.
 *
 ****************************************************************************/

static __inline__ int trigger_frh(int v)
{
    return (v & (N_LINES - 1)) == 0;
}

/****i* januae-custom-timing-cycle/diff32
 *
 *   NAME
 *       diff32()
 *
 *   SYNOPSIS
 *       static long int diff32 (frame_time_t x, frame_time_t y)
 *
 *   FUNCTION
 *       Calculate the signed difference between two frame-timer readings.
 *
 *   INPUTS
 *       x -- first timer reading
 *       y -- second timer reading
 *
 *   RESULT
 *       Signed difference converted to long int.
 *
 *   NOTES
 *       Available only with JIT.
 *
 ****************************************************************************/

static long int diff32 (frame_time_t x, frame_time_t y)
{
    return (long int)(x - y);
}

/****i* januae-custom-timing-cycle/frh_handler
 *
 *   NAME
 *       frh_handler()
 *
 *   SYNOPSIS
 *       static void frh_handler(void)
 *
 *   FUNCTION
 *       Advance the JIT frame pacing deadline when CPU speed is unrestricted.
 *
 *   NOTES
 *       Corrects an overdue deadline in scanline-sized increments; turbo mode limits the catch-up loop. Available only with JIT.
 *
 ****************************************************************************/

static void frh_handler(void)
{
    if (currprefs.m68k_speed == -1) {
	frame_time_t curr_time = uae_gethrtime ();
	vsyncmintime += vsynctime * N_LINES / maxvpos;
	/* @@@ Mathias? How do you think we should do this? */
	/* If we are too far behind, or we just did a reset, adjust the
	 * needed time. */
	if (rpt_did_reset) {
	    vsyncmintime = curr_time + vsynctime;
	    rpt_did_reset = 0;
	}
	/* Allow this to be one frame's worth of cycles out */
	while (diff32 (curr_time, vsyncmintime + vsynctime) > 0) {
	    vsyncmintime += vsynctime * N_LINES / maxvpos;
	    if (turbo_emulation)
		break;
	}
    }
}
#endif


/****i* januae-custom-timing-cycle/hsync_handler
 *
 *   NAME
 *       hsync_handler()
 *
 *   SYNOPSIS
 *       void hsync_handler (void)
 *
 *   FUNCTION
 *       Finish the current raster line and process horizontal synchronization.
 *
 *   NOTES
 *       Synchronizes Copper and rendering decisions, dispatches per-line device handlers, and advances raster and frame state.
 *
 ****************************************************************************/

void hsync_handler (void)
{
    static unsigned int ciahsync;
    unsigned int hpos = current_hpos ();

    sync_copper_with_cpu (maxhpos, 0);
    finish_decisions ();
    if (thisline_decision.plfleft != -1) {
	if (currprefs.collision_level > 1)
	    do_sprite_collisions ();
	if (currprefs.collision_level > 2)
	    do_playfield_collisions ();
    }
    hsync_record_line_state (next_lineno, nextline_how, thisline_changed);

    eventtab[ev_hsync].evtime += get_cycles () - eventtab[ev_hsync].oldcycles;
    eventtab[ev_hsync].oldcycles = get_cycles ();
    CIA_hsync_handler ();
#ifdef CD32
    AKIKO_hsync_handler ();
#endif

#ifdef PICASSO96
    picasso_handle_hsync ();
#endif
    ciahsync++;
    if (ciahsync >= (currprefs.ntscmode ? MAXVPOS_NTSC : MAXVPOS_PAL) * MAXHPOS_PAL / maxhpos) { /* not so perfect.. */
	CIA_vsync_handler ();
	ciahsync = 0;
    }

    /* reset light pen latch */
    if (vpos == sprite_vblank_endline)
	vpos_lpen = -1;

#ifdef CPUEMU_6
    if (currprefs.cpu_cycle_exact || currprefs.blitter_cycle_exact) {
	decide_blitter (hpos);
	memset (cycle_line, 0, sizeof cycle_line);
	cycle_line[1] = CYCLE_REFRESH;
	cycle_line[3] = CYCLE_REFRESH;
	cycle_line[5] = CYCLE_REFRESH;
	cycle_line[7] = CYCLE_REFRESH;
    }
#endif
    if ((currprefs.chipset_mask & CSMASK_AGA) || (!currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	last_custom_value = rand ();
    else
	last_custom_value = 0xffff;

    if (!currprefs.blitter_cycle_exact && bltstate != BLT_done && dmaen (DMA_BITPLANE) && diwstate == DIW_waiting_stop)
	blitter_slowdown (thisline_decision.plfleft, thisline_decision.plfright - (16 << fetchmode),
	    cycle_diagram_total_cycles[fmode][GET_RES (bplcon0)][GET_PLANES_LIMIT (bplcon0)],
	    cycle_diagram_free_cycles[fmode][GET_RES (bplcon0)][GET_PLANES_LIMIT (bplcon0)]);

    if (currprefs.produce_sound)
	audio_hsync (1);

    hardware_line_completed (next_lineno);

    /* In theory only an equality test is needed here - but if a program
       goes haywire with the VPOSW register, it can cause us to miss this,
       with vpos going into the thousands (and all the nasty consequences
       this has).  */

    if (++vpos >= (maxvpos + (lof == 0 ? 0 : 1))) {
	if (bplcon0 & 8) {
	    vpos_lpen = vpos - 1;
	    hpos_lpen = maxhpos;
	}
	vpos = 0;
	vsync_handler ();
    }

    DISK_hsync (maxhpos);

#ifdef JIT
    if (compiled_code) {
	if (currprefs.m68k_speed == -1) {
	    static int count = 0;
	    count++;
	    if (trigger_frh(count)) {
		frh_handler();
	    }
	    is_lastline = trigger_frh(count+1) && ! rpt_did_reset;
	} else {
	    is_lastline=0;
	}
    } else {
#endif
	is_lastline = (vpos + 1 == (maxvpos + (lof == 0 ? 0 : 1))) && (currprefs.m68k_speed == -1) && ! rpt_did_reset;
#ifdef JIT
    }
#endif

    if ((bplcon0 & 4) && currprefs.gfx_linedbl)
	notice_interlace_seen ();

    if (!nodraw ()) {
	unsigned int lineno = vpos;
	nextline_how = nln_normal;
	if (currprefs.gfx_linedbl) {
	    lineno *= 2;
	    nextline_how = currprefs.gfx_linedbl == 1 ? nln_doubled : nln_nblack;
	    if (bplcon0 & 4) {
		if (!lof) {
		    lineno++;
		    nextline_how = nln_lower;
		} else {
		    nextline_how = nln_upper;
		}
	    }
	}
	next_lineno = lineno;
	reset_decisions ();
    }
#ifdef FILESYS
    if (uae_int_requested) {
	set_uae_int_flag ();
	INTREQ (0x8000 | 0x0008);
    }
#endif
    /* See if there's a chance of a copper wait ending this line.  */
    cop_state.hpos = 0;
    cop_state.last_write = 0;
    compute_spcflag_copper ();
    inputdevice_hsync ();
    serial_hsynchandler ();
    misc_hsync_stuff ();
#ifdef CUSTOM_SIMPLE
    do_sprites (0);
#endif

    hsync_counter++;
    //copper_check (2);
}

/* from j.h */
void j_reset(void);

/* custom reset seems to be called, if amigaOS does its own
 * reset, after a crash for example.
 * we need to reset all our daemons here, too.
 */
/****i* januae-custom-frame-memory/customreset
 *
 *   NAME
 *       customreset()
 *
 *   SYNOPSIS
 *       void customreset (void)
 *
 *   FUNCTION
 *       Reset custom-chip emulation state and associated subsystems.
 *
 *   NOTES
 *       Calls j_reset on AROS; selected reset behavior depends on build options and savestate_state.
 *
 ****************************************************************************/

void customreset (void) {

    int zero = 0;

    write_log ("reset at %x\n", m68k_getpc (&regs));

#ifdef __AROS__
    j_reset();
#endif

    hsync_counter = 0;
    if (! savestate_state) {
	currprefs.chipset_mask = changed_prefs.chipset_mask;
	if ((currprefs.chipset_mask & CSMASK_AGA) == 0) {
	    unsigned int i;
	    for (i = 0; i < 32; i++) {
		current_colors.color_regs_ecs[i] = 0;
		current_colors.acolors[i] = xcolors[0];
	    }
#ifdef AGA
	} else {
	    unsigned int i;
	    for (i = 0; i < 256; i++) {
		current_colors.color_regs_aga[i] = 0;
		current_colors.acolors[i] = CONVERT_RGB (zero);
	    }
#endif
	}

	clxdat = 0;

	/* Clear the armed flags of all sprites.  */
	memset (spr, 0, sizeof spr);
	nr_armed = 0;

	dmacon = intena = 0;

	copcon = 0;
	DSKLEN (0, 0);

	bplcon0 = 0;
	bplcon4 = 0x11; /* Get AGA chipset into ECS compatibility mode */
	bplcon3 = 0xC00;

	FMODE (0);
	CLXCON (0);
    }

#ifdef AUTOCONFIG
    expamem_reset ();
#endif
    a1000_reset ();
    DISK_reset ();
    CIA_reset ();
#ifdef JIT
    compemu_reset ();
#endif
    unset_special (&regs, ~(SPCFLAG_BRK | SPCFLAG_MODE_CHANGE));

    vpos = 0;

    inputdevice_reset ();
    timehack_alive = 0;

    curr_sprite_entries = 0;
    prev_sprite_entries = 0;
    sprite_entries[0][0].first_pixel = 0;
    sprite_entries[1][0].first_pixel = MAX_SPR_PIXELS;
    sprite_entries[0][1].first_pixel = 0;
    sprite_entries[1][1].first_pixel = MAX_SPR_PIXELS;
    memset (spixels, 0, 2 * MAX_SPR_PIXELS * sizeof *spixels);
    memset (&spixstate, 0, sizeof spixstate);

    bltstate = BLT_done;
    cop_state.state = COP_stop;
    diwstate = DIW_waiting_start;
    hdiwstate = DIW_waiting_start;
    set_cycles (0);

    new_beamcon0 = currprefs.ntscmode ? 0x00 : 0x20;
    hack_vpos = 0;
    init_hz ();
    vpos_lpen = -1;

    audio_reset ();
    if (savestate_state != STATE_RESTORE) {
	/* must be called after audio_reset */
	adkcon = 0;
	serial_uartbreak (0);
	audio_update_adkmasks ();
    }

    init_sprites ();

    init_hardware_frame ();
    drawing_init ();

    reset_decisions ();

    bogusframe = 1;

    sprite_buffer_res = currprefs.chipset_mask & CSMASK_AGA ? RES_HIRES : RES_LORES;
    if (savestate_state == STATE_RESTORE) {
        unsigned int i;
	uae_u16 v;
	uae_u32 vv;

	audio_update_adkmasks ();
	INTENA (0);
	INTREQ (0);
	COPJMP (1);
	v = bplcon0;
	BPLCON0 (0, 0);
	BPLCON0 (0, v);
	FMODE (fmode);
	if (!(currprefs.chipset_mask & CSMASK_AGA)) {
	    for (i = 0 ; i < 32 ; i++)  {
		vv = current_colors.color_regs_ecs[i];
		current_colors.color_regs_ecs[i] = -1;
		record_color_change (0, i, vv);
		remembered_color_entry = -1;
		current_colors.color_regs_ecs[i] = vv;
		current_colors.acolors[i] = xcolors[vv];
	    }
#ifdef AGA
	} else {
	    for(i = 0 ; i < 256 ; i++)  {
		vv = current_colors.color_regs_aga[i];
		current_colors.color_regs_aga[i] = -1;
		record_color_change (0, i, vv);
		remembered_color_entry = -1;
		current_colors.color_regs_aga[i] = vv;
		current_colors.acolors[i] = CONVERT_RGB(vv);
	    }
#endif
	}
	CLXCON (clxcon);
	CLXCON2 (clxcon2);
	calcdiw ();
	write_log ("State restored\n");
	dumpcustom ();
	for (i = 0; i < 8; i++)
	    nr_armed += spr[i].armed != 0;
	if (! currprefs.produce_sound) {
	    eventtab[ev_audio].active = 0;
	    events_schedule ();
	}
    }
    expand_sprres ();

    #ifdef ACTION_REPLAY
    /* Doing this here ensures we can use the 'reset' command from within AR */
    action_replay_reset ();
    #endif
    #if defined(ENFORCER)
    enforcer_disable();
    #endif
}

/****i* januae-custom-frame-memory/dumpcustom
 *
 *   NAME
 *       dumpcustom()
 *
 *   SYNOPSIS
 *       void dumpcustom (void)
 *
 *   FUNCTION
 *       Write selected custom-chip registers and frame timing statistics to the log.
 *
 *   NOTES
 *       Emits frame averages only when timed frames have been recorded.
 *
 ****************************************************************************/

void dumpcustom (void)
{
    write_log ("DMACON: %x INTENA: %x INTREQ: %x VPOS: %x HPOS: %x\n", DMACONR(),
	       (unsigned int)intena, (unsigned int)intreq, (unsigned int)vpos, (unsigned int)current_hpos());
    write_log ("COP1LC: %08lx, COP2LC: %08lx COPPTR: %08lx\n", (unsigned long)cop1lc, (unsigned long)cop2lc, cop_state.ip);
    write_log ("DIWSTRT: %04x DIWSTOP: %04x DDFSTRT: %04x DDFSTOP: %04x\n",
	       (unsigned int)diwstrt, (unsigned int)diwstop, (unsigned int)ddfstrt, (unsigned int)ddfstop);
    write_log ("BPLCON 0: %04x 1: %04x 2: %04x 3: %04x 4: %04x\n", bplcon0, bplcon1, bplcon2, bplcon3, bplcon4);
    if (timeframes) {
	write_log ("Average frame time: %f ms [frames: %d time: %d]\n",
		   (double)frametime / timeframes, timeframes, frametime);
	if (total_skipped)
	    write_log ("Skipped frames: %d\n", total_skipped);
    }
    /*for (i=0; i<256; i++) if (blitcount[i]) write_log ("minterm %x = %d\n",i,blitcount[i]);  blitter debug */
}

/****i* januae-custom-frame-memory/gen_custom_tables
 *
 *   NAME
 *       gen_custom_tables()
 *
 *   SYNOPSIS
 *       static void gen_custom_tables (void)
 *
 *   FUNCTION
 *       Initialize sprite expansion, merge, and collision lookup tables.
 *
 ****************************************************************************/

static void gen_custom_tables (void)
{
    int i;
    for (i = 0; i < 256; i++) {
	sprtaba[i] = ((((i >> 7) & 1) << 0)
		      | (((i >> 6) & 1) << 2)
		      | (((i >> 5) & 1) << 4)
		      | (((i >> 4) & 1) << 6)
		      | (((i >> 3) & 1) << 8)
		      | (((i >> 2) & 1) << 10)
		      | (((i >> 1) & 1) << 12)
		      | (((i >> 0) & 1) << 14));
	sprtabb[i] = sprtaba[i] * 2;
	sprite_ab_merge[i] = (((i & 15) ? 1 : 0)
			      | ((i & 240) ? 2 : 0));
    }
    for (i = 0; i < 16; i++) {
	clxmask[i] = (((i & 1) ? 0xF : 0x3)
		      | ((i & 2) ? 0xF0 : 0x30)
		      | ((i & 4) ? 0xF00 : 0x300)
		      | ((i & 8) ? 0xF000 : 0x3000));
	sprclx[i] = (((i & 0x3) == 0x3 ? 1 : 0)
		     | ((i & 0x5) == 0x5 ? 2 : 0)
		     | ((i & 0x9) == 0x9 ? 4 : 0)
		     | ((i & 0x6) == 0x6 ? 8 : 0)
		     | ((i & 0xA) == 0xA ? 16 : 0)
		     | ((i & 0xC) == 0xC ? 32 : 0)) << 9;
    }
}

/* mousehack is now in "filesys boot rom" */
/****i* januae-custom-frame-memory/mousehack_helper_old
 *
 *   NAME
 *       mousehack_helper_old()
 *
 *   SYNOPSIS
 *       static uae_u32 REGPARAM2 mousehack_helper_old (struct TrapContext *ctx)
 *
 *   FUNCTION
 *       Return zero from the obsolete mouse trap helper.
 *
 *   INPUTS
 *       ctx -- trap context, unused
 *
 *   RESULT
 *       Zero.
 *
 *   NOTES
 *       The active mouse helper is provided by the filesystem boot ROM.
 *
 ****************************************************************************/

static uae_u32 REGPARAM2 mousehack_helper_old (struct TrapContext *ctx)
{
    return 0;
}

/****i* januae-custom-frame-memory/allocate_sprite_tables
 *
 *   NAME
 *       allocate_sprite_tables()
 *
 *   SYNOPSIS
 *       static int allocate_sprite_tables (void)
 *
 *   FUNCTION
 *       Allocate sprite and color-change buffers where dynamic tables are required.
 *
 *   RESULT
 *       Nonzero on success; zero when a required allocation fails.
 *
 *   NOTES
 *       Allocations are conditional on OS_WITHOUT_MEMORY_MANAGEMENT.
 *
 ****************************************************************************/

static int allocate_sprite_tables (void)
{
#ifdef OS_WITHOUT_MEMORY_MANAGEMENT
    int num;

    delta_sprite_entry = 0;
    delta_color_change = 0;

    if (!sprite_entries[0]) {
        max_sprite_entry = DEFAULT_MAX_SPRITE_ENTRY;
        max_color_change = DEFAULT_MAX_COLOR_CHANGE;

	for (num = 0; num < 2; num++) {
	    sprite_entries[num] = xmalloc (max_sprite_entry * sizeof (struct sprite_entry));
	    color_changes[num] = xmalloc (max_color_change * sizeof (struct color_change));

	    if (sprite_entries[num] && color_changes[num]) {
		memset (sprite_entries[num], 0, max_sprite_entry * sizeof (struct sprite_entry));
		memset (color_changes[num], 0, max_color_change * sizeof (struct color_change));
	    } else
	    	return 0;
	}
    }

    if (!spixels) {
	spixels = xmalloc (2 * MAX_SPR_PIXELS * sizeof *spixels);
	if (!spixels)
	    return 0;
    }
#endif
    return 1;
}

/****i* januae-custom-frame-memory/custom_init
 *
 *   NAME
 *       custom_init()
 *
 *   SYNOPSIS
 *       int custom_init (void)
 *
 *   FUNCTION
 *       Initialize custom-chip drawing tables and optional ROM trap callbacks.
 *
 *   RESULT
 *       Nonzero on success; zero if sprite-table allocation fails.
 *
 *   NOTES
 *       On AROS with AUTOCONFIG, installs the Janus daemon trap callback.
 *
 ****************************************************************************/

int custom_init (void)
{
    if (!allocate_sprite_tables()) {
	gui_message ("Not enough memory for sprite tables\n");
	return 0;
    }

#ifdef AUTOCONFIG
    {
	uaecptr pos;
	pos = here ();

	org (RTAREA_BASE+0xFF70);
	calltrap (deftrap (mousehack_helper_old));
	dw (RTS);

	org (RTAREA_BASE+0xFFA0);
	calltrap (deftrap (timehack_helper));
	dw (RTS);

#ifdef __AROS__
	/* init janus-daemon callbacks */
	org (RTAREA_BASE+0xFF90);
	calltrap (deftrap (aroshack_helper));
	dw (RTS);
#endif

	org (pos);
    }
#endif

    gen_custom_tables ();
    build_blitfilltable ();

    drawing_init ();

    create_cycle_diagram_table ();

    /* We have to do this somewhere... */
    syncbase = uae_gethrtimebase ();

    return 1;
}

/* Custom chip memory bank */

static uae_u32 custom_lget (uaecptr) REGPARAM;
static uae_u32 custom_wget (uaecptr) REGPARAM;
static uae_u32 custom_bget (uaecptr) REGPARAM;
static void custom_lput (uaecptr, uae_u32) REGPARAM;
static void custom_wput (uaecptr, uae_u32) REGPARAM;
static void custom_bput (uaecptr, uae_u32) REGPARAM;

addrbank custom_bank = {
    custom_lget, custom_wget, custom_bget,
    custom_lput, custom_wput, custom_bput,
    default_xlate, default_check, NULL
};

/****i* januae-custom-frame-memory/custom_wget_1
 *
 *   NAME
 *       custom_wget_1()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u32 REGPARAM2 custom_wget_1 (uaecptr addr, int noput)
 *
 *   FUNCTION
 *       Read the selected custom-chip word register.
 *
 *   INPUTS
 *       addr -- register address
 *       noput -- suppress writeback on reads of write-only registers
 *
 *   RESULT
 *       Register value or the last custom-bus value.
 *
 *   NOTES
 *       Reads of write-only registers may dispatch a register write when noput is zero.
 *
 ****************************************************************************/

STATIC_INLINE uae_u32 REGPARAM2 custom_wget_1 (uaecptr addr, int noput)
{
    uae_u16 v;
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    addr &= 0xfff;
#ifdef CUSTOM_DEBUG
    write_log ("%d:%d:wget: %04.4X=%04.4X pc=%p\n", current_hpos(), vpos, addr, addr & 0x1fe, m68k_getpc (&regs));
#endif
    switch (addr & 0x1fe) {
     case 0x002: v = DMACONR (); break;
     case 0x004: v = VPOSR (); break;
     case 0x006: v = VHPOSR (); break;

     case 0x00A: v = JOY0DAT (); break;
     case 0x00C: v = JOY1DAT (); break;
     case 0x00E: v = CLXDAT (); break;
     case 0x010: v = ADKCONR (); break;

     case 0x012: v = POT0DAT (); break;
     case 0x014: v = POT1DAT (); break;
     case 0x016: v = POTGOR (); break;
     case 0x018: v = SERDATR (); break;
     case 0x01A: v = DSKBYTR (current_hpos ()); break;
     case 0x01C: v = INTENAR (); break;
     case 0x01E: v = INTREQR (); break;
     case 0x07C: v = DENISEID (); break;

     case 0x02E: v = 0xffff; break; /* temporary hack */

#ifdef AGA
     case 0x180: case 0x182: case 0x184: case 0x186: case 0x188: case 0x18A:
     case 0x18C: case 0x18E: case 0x190: case 0x192: case 0x194: case 0x196:
     case 0x198: case 0x19A: case 0x19C: case 0x19E: case 0x1A0: case 0x1A2:
     case 0x1A4: case 0x1A6: case 0x1A8: case 0x1AA: case 0x1AC: case 0x1AE:
     case 0x1B0: case 0x1B2: case 0x1B4: case 0x1B6: case 0x1B8: case 0x1BA:
     case 0x1BC: case 0x1BE:
	v = COLOR_READ ((addr & 0x3E) / 2);
	break;
#endif

     default:
	/* reading write-only register causes write with last value in bus */
	v = last_custom_value;
	if (!noput) {
	    int r;
	    unsigned int hpos = current_hpos ();
	    decide_line (hpos);
	    decide_fetch (hpos);
	    decide_blitter (hpos);
	    v = last_custom_value;
	    r = custom_wput_1 (hpos, addr, v, 1);
	}
	return v;
    }
    return v;
}

/****i* januae-custom-frame-memory/custom_wget2
 *
 *   NAME
 *       custom_wget2()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u32 custom_wget2 (uaecptr addr)
 *
 *   FUNCTION
 *       Synchronize the Copper and read a custom-chip word.
 *
 *   INPUTS
 *       addr -- register address
 *
 *   RESULT
 *       Word value, including the CPU-level-specific special address handling.
 *
 ****************************************************************************/

STATIC_INLINE uae_u32 custom_wget2 (uaecptr addr)
{
    uae_u32 v;
    sync_copper_with_cpu (current_hpos (), 1);
    if (currprefs.cpu_level >= 2) {
	if (addr >= 0xde0000 && addr <= 0xdeffff)
	    return 0x7f7f;
	if (addr >= 0xdd0000 && addr <= 0xddffff)
	     return 0xffff;
    }
    v = custom_wget_1 (addr, 0);
#ifdef ACTION_REPLAY
#ifdef ACTION_REPLAY_COMMON
    addr &= 0x1ff;
    ar_custom[addr + 0] = (uae_u8)(v >> 8);
    ar_custom[addr + 1] = (uae_u8)(v);
#endif
#endif
    return v;
}

/****i* januae-custom-frame-memory/custom_wget
 *
 *   NAME
 *       custom_wget()
 *
 *   SYNOPSIS
 *       uae_u32 REGPARAM2 custom_wget (uaecptr addr)
 *
 *   FUNCTION
 *       Read a custom-chip word, combining adjacent aligned reads for odd addresses.
 *
 *   INPUTS
 *       addr -- memory address
 *
 *   RESULT
 *       Word value.
 *
 ****************************************************************************/

uae_u32 REGPARAM2 custom_wget (uaecptr addr)
{
    uae_u32 v;

    if (addr & 1) {
	/* think about move.w $dff005,d0.. (68020+ only) */
	addr &= ~1;
	v = custom_wget2 (addr) << 8;
	v |= custom_wget2 (addr + 2) >> 8;
	return v;
    }
    return custom_wget2 (addr);
 }

/****i* januae-custom-frame-memory/custom_bget
 *
 *   NAME
 *       custom_bget()
 *
 *   SYNOPSIS
 *       uae_u32 REGPARAM2 custom_bget (uaecptr addr)
 *
 *   FUNCTION
 *       Read a byte from the selected half of a custom-chip word.
 *
 *   INPUTS
 *       addr -- memory address
 *
 *   RESULT
 *       Selected byte.
 *
 ****************************************************************************/

uae_u32 REGPARAM2 custom_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    return custom_wget2 (addr & ~1) >> (addr & 1 ? 0 : 8);
}

/****i* januae-custom-frame-memory/custom_lget
 *
 *   NAME
 *       custom_lget()
 *
 *   SYNOPSIS
 *       uae_u32 REGPARAM2 custom_lget (uaecptr addr)
 *
 *   FUNCTION
 *       Read two successive custom-chip words as a longword.
 *
 *   INPUTS
 *       addr -- memory address
 *
 *   RESULT
 *       Combined 32-bit value.
 *
 ****************************************************************************/

uae_u32 REGPARAM2 custom_lget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    return ((uae_u32)custom_wget (addr) << 16) | custom_wget (addr + 2);
}

/****i* januae-custom-frame-memory/custom_wput_1
 *
 *   NAME
 *       custom_wput_1()
 *
 *   SYNOPSIS
 *       int REGPARAM2 custom_wput_1 (unsigned int hpos, uaecptr addr, uae_u32 value, int noget)
 *
 *   FUNCTION
 *       Dispatch a custom-chip register write at the specified beam position.
 *
 *   INPUTS
 *       hpos -- current horizontal beam position
 *       addr -- custom register address
 *       value -- value written to the register
 *       noget -- suppress fallback reads for unhandled register addresses when nonzero.
 *
 *   RESULT
 *       Zero for a handled register write; one when the address takes the default path.
 *
 *   NOTES
 *       Only the low nine address bits and low 16 value bits are used. An unhandled address may trigger a custom register read when noget is zero.
 *
 ****************************************************************************/

int REGPARAM2 custom_wput_1 (unsigned int hpos, uaecptr addr, uae_u32 value, int noget)
{
    addr &= 0x1FE;
    value &= 0xffff;
#ifdef ACTION_REPLAY
#ifdef ACTION_REPLAY_COMMON
    ar_custom[addr+0]=(uae_u8)(value>>8);
    ar_custom[addr+1]=(uae_u8)(value);
#endif
#endif
    last_custom_value = value;
    switch (addr) {
     case 0x00E: CLXDAT (); break;

     case 0x020: DSKPTH (value); break;
     case 0x022: DSKPTL (value); break;
     case 0x024: DSKLEN (value, hpos); break;
     case 0x026: DSKDAT (value); break;

     case 0x02A: VPOSW (value); break;
     case 0x02E: COPCON (value); break;
     case 0x030: SERDAT (value); break;
     case 0x032: SERPER (value); break;
     case 0x034: POTGO (value); break;
     case 0x040: BLTCON0 (value); break;
     case 0x042: BLTCON1 (value); break;

     case 0x044: BLTAFWM (value); break;
     case 0x046: BLTALWM (value); break;

     case 0x050: BLTAPTH (value); break;
     case 0x052: BLTAPTL (value); break;
     case 0x04C: BLTBPTH (value); break;
     case 0x04E: BLTBPTL (value); break;
     case 0x048: BLTCPTH (value); break;
     case 0x04A: BLTCPTL (value); break;
     case 0x054: BLTDPTH (value); break;
     case 0x056: BLTDPTL (value); break;

     case 0x058: BLTSIZE (value); break;

     case 0x064: BLTAMOD (value); break;
     case 0x062: BLTBMOD (value); break;
     case 0x060: BLTCMOD (value); break;
     case 0x066: BLTDMOD (value); break;

     case 0x070: BLTCDAT (value); break;
     case 0x072: BLTBDAT (value); break;
     case 0x074: BLTADAT (value); break;

     case 0x07E: DSKSYNC (hpos, value); break;

     case 0x080: COP1LCH (value); break;
     case 0x082: COP1LCL (value); break;
     case 0x084: COP2LCH (value); break;
     case 0x086: COP2LCL (value); break;

     case 0x088: COPJMP (1); break;
     case 0x08A: COPJMP (2); break;

     case 0x08E: DIWSTRT (hpos, value); break;
     case 0x090: DIWSTOP (hpos, value); break;
     case 0x092: DDFSTRT (hpos, value); break;
     case 0x094: DDFSTOP (hpos, value); break;

     case 0x096: DMACON (hpos, value); break;
     case 0x098: CLXCON (value); break;
     case 0x09A: INTENA (value); break;
     case 0x09C: INTREQ (value); break;
     case 0x09E: ADKCON (hpos, value); break;

     case 0x0A0: AUDxLCH (0, value); break;
     case 0x0A2: AUDxLCL (0, value); break;
     case 0x0A4: AUDxLEN (0, value); break;
     case 0x0A6: AUDxPER (0, value); break;
     case 0x0A8: AUDxVOL (0, value); break;
     case 0x0AA: AUDxDAT (0, value); break;

     case 0x0B0: AUDxLCH (1, value); break;
     case 0x0B2: AUDxLCL (1, value); break;
     case 0x0B4: AUDxLEN (1, value); break;
     case 0x0B6: AUDxPER (1, value); break;
     case 0x0B8: AUDxVOL (1, value); break;
     case 0x0BA: AUDxDAT (1, value); break;

     case 0x0C0: AUDxLCH (2, value); break;
     case 0x0C2: AUDxLCL (2, value); break;
     case 0x0C4: AUDxLEN (2, value); break;
     case 0x0C6: AUDxPER (2, value); break;
     case 0x0C8: AUDxVOL (2, value); break;
     case 0x0CA: AUDxDAT (2, value); break;

     case 0x0D0: AUDxLCH (3, value); break;
     case 0x0D2: AUDxLCL (3, value); break;
     case 0x0D4: AUDxLEN (3, value); break;
     case 0x0D6: AUDxPER (3, value); break;
     case 0x0D8: AUDxVOL (3, value); break;
     case 0x0DA: AUDxDAT (3, value); break;

     case 0x0E0: BPLxPTH (hpos, value, 0); break;
     case 0x0E2: BPLxPTL (hpos, value, 0); break;
     case 0x0E4: BPLxPTH (hpos, value, 1); break;
     case 0x0E6: BPLxPTL (hpos, value, 1); break;
     case 0x0E8: BPLxPTH (hpos, value, 2); break;
     case 0x0EA: BPLxPTL (hpos, value, 2); break;
     case 0x0EC: BPLxPTH (hpos, value, 3); break;
     case 0x0EE: BPLxPTL (hpos, value, 3); break;
     case 0x0F0: BPLxPTH (hpos, value, 4); break;
     case 0x0F2: BPLxPTL (hpos, value, 4); break;
     case 0x0F4: BPLxPTH (hpos, value, 5); break;
     case 0x0F6: BPLxPTL (hpos, value, 5); break;
     case 0x0F8: BPLxPTH (hpos, value, 6); break;
     case 0x0FA: BPLxPTL (hpos, value, 6); break;
     case 0x0FC: BPLxPTH (hpos, value, 7); break;
     case 0x0FE: BPLxPTL (hpos, value, 7); break;

     case 0x100: BPLCON0 (hpos, value); break;
     case 0x102: BPLCON1 (hpos, value); break;
     case 0x104: BPLCON2 (hpos, value); break;
#ifdef AGA
     case 0x106: BPLCON3 (hpos, value); break;
#endif

     case 0x108: BPL1MOD (hpos, value); break;
     case 0x10A: BPL2MOD (hpos, value); break;
#ifdef AGA
     case 0x10E: CLXCON2 (value); break;
#endif

     case 0x110: BPL1DAT (hpos, value); break;

     case 0x180: case 0x182: case 0x184: case 0x186: case 0x188: case 0x18A:
     case 0x18C: case 0x18E: case 0x190: case 0x192: case 0x194: case 0x196:
     case 0x198: case 0x19A: case 0x19C: case 0x19E: case 0x1A0: case 0x1A2:
     case 0x1A4: case 0x1A6: case 0x1A8: case 0x1AA: case 0x1AC: case 0x1AE:
     case 0x1B0: case 0x1B2: case 0x1B4: case 0x1B6: case 0x1B8: case 0x1BA:
     case 0x1BC: case 0x1BE:
	COLOR_WRITE (hpos, value & 0xFFF, (addr & 0x3E) / 2);
	break;
     case 0x120: case 0x124: case 0x128: case 0x12C:
     case 0x130: case 0x134: case 0x138: case 0x13C:
	SPRxPTH (hpos, value, (addr - 0x120) / 4);
	break;
     case 0x122: case 0x126: case 0x12A: case 0x12E:
     case 0x132: case 0x136: case 0x13A: case 0x13E:
	SPRxPTL (hpos, value, (addr - 0x122) / 4);
	break;
     case 0x140: case 0x148: case 0x150: case 0x158:
     case 0x160: case 0x168: case 0x170: case 0x178:
	SPRxPOS (hpos, value, (addr - 0x140) / 8);
	break;
     case 0x142: case 0x14A: case 0x152: case 0x15A:
     case 0x162: case 0x16A: case 0x172: case 0x17A:
	SPRxCTL (hpos, value, (addr - 0x142) / 8);
	break;
     case 0x144: case 0x14C: case 0x154: case 0x15C:
     case 0x164: case 0x16C: case 0x174: case 0x17C:
	SPRxDATA (hpos, value, (addr - 0x144) / 8);
	break;
     case 0x146: case 0x14E: case 0x156: case 0x15E:
     case 0x166: case 0x16E: case 0x176: case 0x17E:
	SPRxDATB (hpos, value, (addr - 0x146) / 8);
	break;

     case 0x36: JOYTEST (value); break;
     case 0x5A: BLTCON0L (value); break;
     case 0x5C: BLTSIZV (value); break;
     case 0x5E: BLTSIZH (value); break;
     case 0x1E4: DIWHIGH (hpos, value); break;
#ifdef AGA
     case 0x10C: BPLCON4 (hpos, value); break;
#endif

#ifndef CUSTOM_SIMPLE
     case 0x1DC: BEAMCON0 (value); break;
     case 0x1C0: if (htotal != value) { htotal = value; varsync (); } break;
     case 0x1C2: if (hsstop != value) { hsstop = value; varsync (); } break;
     case 0x1C4: if (hbstrt != value) { hbstrt = value; varsync (); } break;
     case 0x1C6: if (hbstop != value) { hbstop = value; varsync (); } break;
     case 0x1C8: if (vtotal != value) { vtotal = value; varsync (); } break;
     case 0x1CA: if (vsstop != value) { vsstop = value; varsync (); } break;
     case 0x1CC: if (vbstrt < value || vbstrt > value + 1) { vbstrt = value; varsync (); } break;
     case 0x1CE: if (vbstop < value || vbstop > value + 1) { vbstop = value; varsync (); } break;
     case 0x1DE: if (hsstrt != value) { hsstrt = value; varsync (); } break;
     case 0x1E0: if (vsstrt != value) { vsstrt = value; varsync (); } break;
     case 0x1E2: if (hcenter != value) { hcenter = value; varsync (); } break;
#endif

#ifdef AGA
     case 0x1FC: FMODE (value); break;
#endif

     /* writing to read-only register causes read access */
     default:
	if (!noget)
	    custom_wget_1 (addr, 1);
	if (!(currprefs.chipset_mask & CSMASK_AGA) && (currprefs.chipset_mask & CSMASK_ECS_AGNUS))
	    last_custom_value = 0xffff;
	return 1;
    }
    return 0;
}

/****i* januae-custom-frame-memory/custom_wput
 *
 *   NAME
 *       custom_wput()
 *
 *   SYNOPSIS
 *       void REGPARAM2 custom_wput (uaecptr addr, uae_u32 value)
 *
 *   FUNCTION
 *       Synchronize the Copper and write a custom-chip register word.
 *
 *   INPUTS
 *       addr -- register address
 *       value -- word to write
 *
 ****************************************************************************/

void REGPARAM2 custom_wput (uaecptr addr, uae_u32 value)
{
    unsigned int hpos = current_hpos ();
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
#ifdef CUSTOM_DEBUG
    write_log ("%d:%d:wput: %04.4X %04.4X pc=%p\n", hpos, vpos, addr & 0x01fe, value & 0xffff, m68k_getpc (&regs));
#endif
    sync_copper_with_cpu (hpos, 1);
    custom_wput_1 (hpos, addr, value, 0);
}

/****i* januae-custom-frame-memory/custom_bput
 *
 *   NAME
 *       custom_bput()
 *
 *   SYNOPSIS
 *       void REGPARAM2 custom_bput (uaecptr addr, uae_u32 value)
 *
 *   FUNCTION
 *       Replicate the supplied byte across a custom-chip register word and write it.
 *
 *   INPUTS
 *       addr -- register address
 *       value -- byte value
 *
 *   NOTES
 *       Logs a bounded number of byte-write diagnostics for selected program counters.
 *
 ****************************************************************************/

void REGPARAM2 custom_bput (uaecptr addr, uae_u32 value)
{
    static int warned;

    uae_u16 rval = (value << 8) | (value & 0xFF);
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    custom_wput (addr & ~1, rval);
    if (warned < 10) {
	if (m68k_getpc (&regs) < 0xe00000 || m68k_getpc (&regs) >= 0x10000000) {
	    write_log ("Byte put to custom register %04.4X PC=%08.8X\n", addr, m68k_getpc (&regs));
	    warned++;
	}
    }
}

/****i* januae-custom-frame-memory/custom_lput
 *
 *   NAME
 *       custom_lput()
 *
 *   SYNOPSIS
 *       void REGPARAM2 custom_lput(uaecptr addr, uae_u32 value)
 *
 *   FUNCTION
 *       Write a longword as two successive custom-chip register words.
 *
 *   INPUTS
 *       addr -- register address
 *       value -- 32-bit value
 *
 ****************************************************************************/

void REGPARAM2 custom_lput(uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    custom_wput (addr & 0xfffe, value >> 16);
    custom_wput ((addr + 2) & 0xfffe, (uae_u16)value);
}



#ifdef SAVESTATE

/****i* januae-custom-frame-memory/custom_prepare_savestate
 *
 *   NAME
 *       custom_prepare_savestate()
 *
 *   SYNOPSIS
 *       void custom_prepare_savestate (void)
 *
 *   FUNCTION
 *       Prepare custom-chip state for serialization.
 *
 *   NOTES
 *       Currently performs no operation.
 *
 ****************************************************************************/

void custom_prepare_savestate (void)
{
}

#define RB restore_u8 ()
#define RW restore_u16 ()
#define RL restore_u32 ()

/****i* januae-custom-frame-memory/restore_custom
 *
 *   NAME
 *       restore_custom()
 *
 *   SYNOPSIS
 *       const uae_u8 *restore_custom (const uae_u8 *src)
 *
 *   FUNCTION
 *       Restore the custom-chip register state from serialized data.
 *
 *   INPUTS
 *       src -- pointer to the serialized custom-chip state.
 *
 *   RESULT
 *       Pointer immediately following the consumed state data.
 *
 *   NOTES
 *       Resets audio state and restores the disk controller state. Requires SAVESTATE; the caller supplies a valid serialized buffer.
 *
 ****************************************************************************/

const uae_u8 *restore_custom (const uae_u8 *src)
{
    uae_u16 dsklen, dskbytr;
    int dskpt;
    unsigned int i;

    audio_reset ();

    changed_prefs.chipset_mask = currprefs.chipset_mask = RL;
    RW;				/* 000 ? */
    RW;				/* 002 DMACONR */
    RW;				/* 004 VPOSR */
    RW;				/* 006 VHPOSR */
    RW;				/* 008 DSKDATR (dummy register) */
    RW;				/* 00A JOY0DAT */
    RW;				/* 00C JOY1DAT */
    clxdat = RW;		/* 00E CLXDAT */
    RW;				/* 010 ADKCONR */
    RW;				/* 012 POT0DAT* */
    RW;				/* 014 POT1DAT* */
    RW;				/* 016 POTINP* */
    RW;				/* 018 SERDATR* */
    dskbytr = RW;		/* 01A DSKBYTR */
    RW;				/* 01C INTENAR */
    RW;				/* 01E INTREQR */
    dskpt = RL;			/* 020-022 DSKPT */
    dsklen = RW;		/* 024 DSKLEN */
    RW;				/* 026 DSKDAT */
    RW;				/* 028 REFPTR */
    lof = RW;			/* 02A VPOSW */
    RW;				/* 02C VHPOSW */
    COPCON(RW);			/* 02E COPCON */
    RW;				/* 030 SERDAT* */
    RW;				/* 032 SERPER* */
    POTGO(RW);			/* 034 POTGO */
    RW;				/* 036 JOYTEST* */
    RW;				/* 038 STREQU */
    RW;				/* 03A STRVHBL */
    RW;				/* 03C STRHOR */
    RW;				/* 03E STRLONG */
    BLTCON0(RW);		/* 040 BLTCON0 */
    BLTCON1(RW);		/* 042 BLTCON1 */
    BLTAFWM(RW);		/* 044 BLTAFWM */
    BLTALWM(RW);		/* 046 BLTALWM */
    BLTCPTH(RL);		/* 048-04B BLTCPT */
    BLTBPTH(RL);		/* 04C-04F BLTBPT */
    BLTAPTH(RL);		/* 050-053 BLTAPT */
    BLTDPTH(RL);		/* 054-057 BLTDPT */
    RW;				/* 058 BLTSIZE */
    RW;				/* 05A BLTCON0L */
    blt_info.vblitsize = RW;	/* 05C BLTSIZV */
    blt_info.hblitsize = RW;	/* 05E BLTSIZH */
    BLTCMOD(RW);		/* 060 BLTCMOD */
    BLTBMOD(RW);		/* 062 BLTBMOD */
    BLTAMOD(RW);		/* 064 BLTAMOD */
    BLTDMOD(RW);		/* 066 BLTDMOD */
    RW;				/* 068 ? */
    RW;				/* 06A ? */
    RW;				/* 06C ? */
    RW;				/* 06E ? */
    BLTCDAT(RW);		/* 070 BLTCDAT */
    BLTBDAT(RW);		/* 072 BLTBDAT */
    BLTADAT(RW);		/* 074 BLTADAT */
    RW;				/* 076 ? */
    RW;				/* 078 ? */
    RW;				/* 07A ? */
    RW;				/* 07C LISAID */
    DSKSYNC(-1, RW);		/* 07E DSKSYNC */
    cop1lc = RL;		/* 080/082 COP1LC */
    cop2lc = RL;		/* 084/086 COP2LC */
    RW;				/* 088 ? */
    RW;				/* 08A ? */
    RW;				/* 08C ? */
    diwstrt = RW;		/* 08E DIWSTRT */
    diwstop = RW;		/* 090 DIWSTOP */
    ddfstrt = RW;		/* 092 DDFSTRT */
    ddfstop = RW;		/* 094 DDFSTOP */
    dmacon = RW & ~(0x2000|0x4000); /* 096 DMACON */
    CLXCON(RW);			/* 098 CLXCON */
    intena = RW;		/* 09A INTENA */
    intreq = RW;		/* 09C INTREQ */
    adkcon = RW;		/* 09E ADKCON */
    for (i = 0; i < 8; i++)
	bplpt[i] = RL;
    bplcon0 = RW;		/* 100 BPLCON0 */
    bplcon1 = RW;		/* 102 BPLCON1 */
    bplcon2 = RW;		/* 104 BPLCON2 */
    bplcon3 = RW;		/* 106 BPLCON3 */
    bpl1mod = RW;		/* 108 BPL1MOD */
    bpl2mod = RW;		/* 10A BPL2MOD */
    bplcon4 = RW;		/* 10C BPLCON4 */
    clxcon2 = RW;		/* 10E CLXCON2* */
    for(i = 0; i < 8; i++)
	RW;			/*     BPLXDAT */
    for(i = 0; i < 32; i++)
	current_colors.color_regs_ecs[i] = RW; /* 180 COLORxx */
    htotal = RW;		/* 1C0 HTOTAL */
    hsstop = RW;		/* 1C2 HSTOP ? */
    hbstrt = RW;		/* 1C4 HBSTRT ? */
    hbstop = RW;		/* 1C6 HBSTOP ? */
    vtotal = RW;		/* 1C8 VTOTAL */
    vsstop = RW;		/* 1CA VSSTOP */
    vbstrt = RW;		/* 1CC VBSTRT */
    vbstop = RW;		/* 1CE VBSTOP */
    RW;				/* 1D0 ? */
    RW;				/* 1D2 ? */
    RW;				/* 1D4 ? */
    RW;				/* 1D6 ? */
    RW;				/* 1D8 ? */
    RW;				/* 1DA ? */
    new_beamcon0 = RW;		/* 1DC BEAMCON0 */
    hsstrt = RW;		/* 1DE HSSTRT */
    vsstrt = RW;		/* 1E0 VSSTT  */
    hcenter = RW;		/* 1E2 HCENTER */
    diwhigh = RW;		/* 1E4 DIWHIGH */
    if (diwhigh & 0x8000)
	diwhigh_written = 1;
    diwhigh &= 0x7fff;
    RW;				/* 1E6 ? */
    RW;				/* 1E8 ? */
    RW;				/* 1EA ? */
    RW;				/* 1EC ? */
    RW;				/* 1EE ? */
    RW;				/* 1F0 ? */
    RW;				/* 1F2 ? */
    RW;				/* 1F4 ? */
    RW;				/* 1F6 ? */
    RW;				/* 1F8 ? */
    RW;				/* 1FA ? */
    fmode = RW;			/* 1FC FMODE */
    last_custom_value = RW;	/* 1FE ? */

    DISK_restore_custom (dskpt, dsklen, dskbytr);

    return src;
}

#endif /* SAVESTATE */

#if defined SAVESTATE || defined DEBUGGER

#define SB save_u8
#define SW save_u16
#define SL save_u32

extern uae_u16 serper;

uae_u8 *save_custom (uae_u32 *len, uae_u8 *dstptr, int full)
{
    uae_u8 *dstbak, *dst;
    unsigned int i;
    uae_u32 dskpt;
    uae_u16 dsklen, dsksync, dskbytr;

    DISK_save_custom (&dskpt, &dsklen, &dsksync, &dskbytr);

    if (dstptr)
	dstbak = dst = dstptr;
    else
	dstbak = dst = malloc (8 + 256 * 2);

    SL (currprefs.chipset_mask);
    SW (0);			/* 000 ? */
    SW (dmacon);		/* 002 DMACONR */
    SW (VPOSR());		/* 004 VPOSR */
    SW (VHPOSR());		/* 006 VHPOSR */
    SW (0);			/* 008 DSKDATR */
    SW (JOY0DAT());		/* 00A JOY0DAT */
    SW (JOY1DAT());		/* 00C JOY1DAT */
    SW (clxdat);		/* 00E CLXDAT */
    SW (ADKCONR());		/* 010 ADKCONR */
    SW (POT0DAT());		/* 012 POT0DAT */
    SW (POT0DAT());		/* 014 POT1DAT */
    SW (0)	;		/* 016 POTINP * */
    SW (0);			/* 018 SERDATR * */
    SW (dskbytr);		/* 01A DSKBYTR */
    SW (INTENAR());		/* 01C INTENAR */
    SW (INTREQR());		/* 01E INTREQR */
    SL (dskpt);			/* 020-023 DSKPT */
    SW (dsklen);		/* 024 DSKLEN */
    SW (0);			/* 026 DSKDAT */
    SW (0);			/* 028 REFPTR */
    SW (lof);			/* 02A VPOSW */
    SW (0);			/* 02C VHPOSW */
    SW (copcon);		/* 02E COPCON */
    SW (serper);		/* 030 SERDAT * */
    SW (serdat);		/* 032 SERPER * */
    SW (potgo_value);		/* 034 POTGO */
    SW (0);			/* 036 JOYTEST * */
    SW (0);			/* 038 STREQU */
    SW (0);			/* 03A STRVBL */
    SW (0);			/* 03C STRHOR */
    SW (0);			/* 03E STRLONG */
    SW (bltcon0);		/* 040 BLTCON0 */
    SW (bltcon1);		/* 042 BLTCON1 */
    SW (blt_info.bltafwm);	/* 044 BLTAFWM */
    SW (blt_info.bltalwm);	/* 046 BLTALWM */
    SL (bltcpt);		/* 048-04B BLTCPT */
    SL (bltbpt);		/* 04C-04F BLTCPT */
    SL (bltapt);		/* 050-053 BLTCPT */
    SL (bltdpt);		/* 054-057 BLTCPT */
    SW (0);			/* 058 BLTSIZE */
    SW (0);			/* 05A BLTCON0L (use BLTCON0 instead) */
    SW (blt_info.vblitsize);	/* 05C BLTSIZV */
    SW (blt_info.hblitsize);	/* 05E BLTSIZH */
    SW (blt_info.bltcmod);	/* 060 BLTCMOD */
    SW (blt_info.bltbmod);	/* 062 BLTBMOD */
    SW (blt_info.bltamod);	/* 064 BLTAMOD */
    SW (blt_info.bltdmod);	/* 066 BLTDMOD */
    SW (0);			/* 068 ? */
    SW (0);			/* 06A ? */
    SW (0);			/* 06C ? */
    SW (0);			/* 06E ? */
    SW (blt_info.bltcdat);	/* 070 BLTCDAT */
    SW (blt_info.bltbdat);	/* 072 BLTBDAT */
    SW (blt_info.bltadat);	/* 074 BLTADAT */
    SW (0);			/* 076 ? */
    SW (0);			/* 078 ? */
    SW (0);			/* 07A ? */
    SW (DENISEID());		/* 07C DENISEID/LISAID */
    SW (dsksync);		/* 07E DSKSYNC */
    SL (cop1lc);		/* 080-083 COP1LC */
    SL (cop2lc);		/* 084-087 COP2LC */
    SW (0);			/* 088 ? */
    SW (0);			/* 08A ? */
    SW (0);			/* 08C ? */
    SW (diwstrt);		/* 08E DIWSTRT */
    SW (diwstop);		/* 090 DIWSTOP */
    SW (ddfstrt);		/* 092 DDFSTRT */
    SW (ddfstop);		/* 094 DDFSTOP */
    SW (dmacon);		/* 096 DMACON */
    SW (clxcon);		/* 098 CLXCON */
    SW (intena);		/* 09A INTENA */
    SW (intreq);		/* 09C INTREQ */
    SW (adkcon);		/* 09E ADKCON */
    for (i = 0; full && i < 32; i++)
	SW (0);
    for (i = 0; i < 8; i++)
	SL (bplpt[i]);		/* 0E0-0FE BPLxPT */
    SW (bplcon0);		/* 100 BPLCON0 */
    SW (bplcon1);		/* 102 BPLCON1 */
    SW (bplcon2);		/* 104 BPLCON2 */
    SW (bplcon3);		/* 106 BPLCON3 */
    SW (bpl1mod);		/* 108 BPL1MOD */
    SW (bpl2mod);		/* 10A BPL2MOD */
    SW (bplcon4);		/* 10C BPLCON4 */
    SW (clxcon2);		/* 10E CLXCON2 */
    for (i = 0;i < 8; i++)
	SW (0);			/* 110 BPLxDAT */
    if (full) {
	for (i = 0; i < 8; i++) {
	    SL (spr[i].pt);	    /* 120-13E SPRxPT */
	    SW (sprpos[i]);	    /* 1x0 SPRxPOS */
	    SW (sprctl[i]);	    /* 1x2 SPRxPOS */
	    SW (sprdata[i][0]);	    /* 1x4 SPRxDATA */
	    SW (sprdatb[i][0]);	    /* 1x6 SPRxDATB */
	}
    }
    for ( i = 0; i < 32; i++)
	SW (current_colors.color_regs_ecs[i]); /* 180-1BE COLORxx */
    SW (htotal);		/* 1C0 HTOTAL */
    SW (hsstop);		/* 1C2 HSTOP*/
    SW (hbstrt);		/* 1C4 HBSTRT */
    SW (hbstop);		/* 1C6 HBSTOP */
    SW (vtotal);		/* 1C8 VTOTAL */
    SW (vsstop);		/* 1CA VSSTOP */
    SW (vbstrt);		/* 1CC VBSTRT */
    SW (vbstop);		/* 1CE VBSTOP */
    SW (0);			/* 1D0 */
    SW (0);			/* 1D2 */
    SW (0);			/* 1D4 */
    SW (0);			/* 1D6 */
    SW (0);			/* 1D8 */
    SW (0);			/* 1DA */
    SW (beamcon0);		/* 1DC BEAMCON0 */
    SW (hsstrt);		/* 1DE HSSTRT */
    SW (vsstrt);		/* 1E0 VSSTRT */
    SW (hcenter);		/* 1E2 HCENTER */
    SW (diwhigh | (diwhigh_written ? 0x8000 : 0)); /* 1E4 DIWHIGH */
    SW (0);			/* 1E6 */
    SW (0);			/* 1E8 */
    SW (0);			/* 1EA */
    SW (0);			/* 1EC */
    SW (0);			/* 1EE */
    SW (0);			/* 1F0 */
    SW (0);			/* 1F2 */
    SW (0);			/* 1F4 */
    SW (0);			/* 1F6 */
    SW (0);			/* 1F8 */
    SW (0);			/* 1FA */
    SW (fmode);			/* 1FC FMODE */
    SW (last_custom_value);	/* 1FE */

    *len = dst - dstbak;
    return dstbak;
}

#endif /* SAVESTATE || DEBUGGER */

#ifdef SAVESTATE

/****i* januae-custom-frame-memory/restore_custom_agacolors
 *
 *   NAME
 *       restore_custom_agacolors()
 *
 *   SYNOPSIS
 *       const uae_u8 *restore_custom_agacolors (const uae_u8 *src)
 *
 *   FUNCTION
 *       Read the saved 256-entry AGA color register table.
 *
 *   INPUTS
 *       src -- pointer to serialized AGA color values.
 *
 *   RESULT
 *       Pointer immediately after the 256 consumed 32-bit values.
 *
 *   NOTES
 *       When AGA is disabled at build time, the values are consumed without changing the color registers. Requires SAVESTATE.
 *
 ****************************************************************************/

const uae_u8 *restore_custom_agacolors (const uae_u8 *src)
{
    unsigned int i;

    for (i = 0; i < 256; i++)
#ifdef AGA
	current_colors.color_regs_aga[i] = RL;
#else
	RL;
#endif
    return src;
}

uae_u8 *save_custom_agacolors (uae_u32 *len, uae_u8 *dstptr)
{
    uae_u8 *dstbak, *dst;
    unsigned int i;

    if (dstptr)
	dstbak = dst = dstptr;
    else
	dstbak = dst = malloc (256 * 4);
    for (i = 0; i < 256; i++)
#ifdef AGA
    SL (current_colors.color_regs_aga[i]);
#else
    SL (0);
#endif
    *len = dst - dstbak;
    return dstbak;
}

/****i* januae-custom-frame-memory/restore_custom_sprite
 *
 *   NAME
 *       restore_custom_sprite()
 *
 *   SYNOPSIS
 *       const uae_u8 *restore_custom_sprite (unsigned int num, const uae_u8 *src)
 *
 *   FUNCTION
 *       Restore the saved state of one custom-chip sprite.
 *
 *   INPUTS
 *       num -- sprite index
 *       src -- pointer to serialized sprite state.
 *
 *   RESULT
 *       Pointer immediately following the consumed sprite state.
 *
 *   NOTES
 *       Restores the sprite data pointer, position, control, data words and armed state. The caller must supply a valid sprite index and serialized buffer. Requires SAVESTATE.
 *
 ****************************************************************************/

const uae_u8 *restore_custom_sprite (unsigned int num, const uae_u8 *src)
{
    spr[num].pt = RL;		/* 120-13E SPRxPT */
    sprpos[num] = RW;		/* 1x0 SPRxPOS */
    sprctl[num] = RW;		/* 1x2 SPRxPOS */
    sprdata[num][0] = RW;	/* 1x4 SPRxDATA */
    sprdatb[num][0] = RW;	/* 1x6 SPRxDATB */
    sprdata[num][1] = RW;
    sprdatb[num][1] = RW;
    sprdata[num][2] = RW;
    sprdatb[num][2] = RW;
    sprdata[num][3] = RW;
    sprdatb[num][3] = RW;
    spr[num].armed = RB;
    return src;
}

uae_u8 *save_custom_sprite (unsigned int num, uae_u32 *len, uae_u8 *dstptr)
{
    uae_u8 *dstbak, *dst;

    if (dstptr)
	dstbak = dst = dstptr;
    else
	dstbak = dst = malloc (25);
    SL (spr[num].pt);		/* 120-13E SPRxPT */
    SW (sprpos[num]);		/* 1x0 SPRxPOS */
    SW (sprctl[num]);		/* 1x2 SPRxPOS */
    SW (sprdata[num][0]);	/* 1x4 SPRxDATA */
    SW (sprdatb[num][0]);	/* 1x6 SPRxDATB */
    SW (sprdata[num][1]);
    SW (sprdatb[num][1]);
    SW (sprdata[num][2]);
    SW (sprdatb[num][2]);
    SW (sprdata[num][3]);
    SW (sprdatb[num][3]);
    SB (spr[num].armed ? 1 : 0);
    *len = dst - dstbak;
    return dstbak;
}

#endif /* SAVESTATE */


/****i* januae-custom-frame-memory/check_prefs_changed_custom
 *
 *   NAME
 *       check_prefs_changed_custom()
 *
 *   SYNOPSIS
 *       void check_prefs_changed_custom (void)
 *
 *   FUNCTION
 *       Apply changed custom-chip and input-device preferences.
 *
 *   NOTES
 *       Reinitializes the custom chipset when selected chipset, vsync, or video-standard settings change.
 *
 ****************************************************************************/

void check_prefs_changed_custom (void)
{
    currprefs.gfx_framerate = changed_prefs.gfx_framerate;
    if (inputdevice_config_change_test ()) {
	inputdevice_copyconfig (&changed_prefs, &currprefs);
	inputdevice_updateconfig (&currprefs);
    }
    currprefs.immediate_blits = changed_prefs.immediate_blits;
    currprefs.collision_level = changed_prefs.collision_level;

    if (currprefs.chipset_mask != changed_prefs.chipset_mask ||
	currprefs.gfx_vsync != changed_prefs.gfx_vsync ||
	currprefs.ntscmode != changed_prefs.ntscmode) {
	currprefs.gfx_vsync = changed_prefs.gfx_vsync;
	currprefs.chipset_mask = changed_prefs.chipset_mask;
	if (currprefs.ntscmode != changed_prefs.ntscmode) {
	    currprefs.ntscmode = changed_prefs.ntscmode;
	    new_beamcon0 = currprefs.ntscmode ? 0x00 : 0x20;
	}
	init_custom ();
    }
#ifdef GFXFILTER
    currprefs.gfx_filter_horiz_zoom = changed_prefs.gfx_filter_horiz_zoom;
    currprefs.gfx_filter_vert_zoom = changed_prefs.gfx_filter_vert_zoom;
    currprefs.gfx_filter_horiz_offset = changed_prefs.gfx_filter_horiz_offset;
    currprefs.gfx_filter_vert_offset = changed_prefs.gfx_filter_vert_offset;
    currprefs.gfx_filter_scanlines = changed_prefs.gfx_filter_scanlines;
    currprefs.gfx_filter_filtermode = changed_prefs.gfx_filter_filtermode;
#endif
}

#ifdef CPUEMU_6

/****i* januae-custom-timing-cycle/sync_copper
 *
 *   NAME
 *       sync_copper()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void sync_copper (unsigned int hpos)
 *
 *   FUNCTION
 *       Advance the Copper through the requested horizontal position when required.
 *
 *   INPUTS
 *       hpos -- horizontal position through which Copper execution is updated
 *
 *   NOTES
 *       Clears an active scheduled Copper event before updating. Available only with CPUEMU_6.
 *
 ****************************************************************************/

STATIC_INLINE void sync_copper (unsigned int hpos)
{
    if (eventtab[ev_copper].active) {
	eventtab[ev_copper].active = 0;
	update_copper (hpos);
	return;
    }
    if (copper_enabled_thisline)
	update_copper (hpos);
}

/****i* januae-custom-timing-cycle/decide_fetch_ce
 *
 *   NAME
 *       decide_fetch_ce()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void decide_fetch_ce (unsigned int hpos)
 *
 *   FUNCTION
 *       Update cycle-exact bitplane fetch decisions near a display-fetch change.
 *
 *   INPUTS
 *       hpos -- horizontal position for the fetch decision
 *
 *   NOTES
 *       Updates only on the fetch-change line or the following line before the frame limit. Available only with CPUEMU_6.
 *
 ****************************************************************************/

STATIC_INLINE void decide_fetch_ce (unsigned int hpos)
{
    if ((ddf_change == vpos || ddf_change + 1 == vpos) && vpos < maxvpos)
	decide_fetch (hpos);
}

/****i* januae-custom-timing-cycle/dma_cycle
 *
 *   NAME
 *       dma_cycle()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void dma_cycle (void)
 *
 *   FUNCTION
 *       Wait until a CPU bus cycle is available and mark it as CPU-owned.
 *
 *   NOTES
 *       Accounts for Copper, bitplane and blitter activity while advancing emulated cycles. Available only with CPUEMU_6.
 *
 ****************************************************************************/

STATIC_INLINE void dma_cycle (void)
{
    unsigned int hpos;
    static int bnasty;

    for (;;) {
	int bpldma;
	int blitpri = dmaen (DMA_BLITPRI);
	do_cycles (1 * CYCLE_UNIT);
	hpos = current_hpos ();
	sync_copper (hpos);
	decide_line (hpos);
	decide_fetch_ce (hpos);
	bpldma = is_bitplane_dma (hpos);
	if (bltstate != BLT_done) {
	    if (!blitpri && bnasty >= 3 && !cycle_line[hpos] && !bpldma) {
		bnasty = 0;
		break;
	    }
	    decide_blitter (hpos);
	    if (dmaen(DMA_BLITTER))
		bnasty++;
	}
	if (cycle_line[hpos] == 0 && !bpldma)
	    break;
	/* bus was allocated to dma channel, wait for next cycle.. */
    }
    bnasty = 0;
    cycle_line[hpos] |= CYCLE_CPU;
}

/****i* januae-custom-timing-cycle/wait_cpu_cycle_read
 *
 *   NAME
 *       wait_cpu_cycle_read()
 *
 *   SYNOPSIS
 *       uae_u32 wait_cpu_cycle_read (uaecptr addr, int mode)
 *
 *   FUNCTION
 *       Read chip memory during an available cycle-exact CPU bus slot.
 *
 *   INPUTS
 *       addr -- emulated memory address
 *       mode -- positive for word, zero for byte, negative for no read
 *
 *   RESULT
 *       Read value, or zero when mode is negative.
 *
 *   NOTES
 *       Consumes the arbitration cycle and one following CPU cycle. Available only with CPUEMU_6.
 *
 ****************************************************************************/

uae_u32 wait_cpu_cycle_read (uaecptr addr, int mode)
{
    uae_u32 v = 0;
    dma_cycle ();
    if (mode > 0)
	v = get_word (addr);
    else if (mode == 0)
	v = get_byte (addr);
    do_cycles (1 * CYCLE_UNIT);
    return v;
}

/****i* januae-custom-timing-cycle/wait_cpu_cycle_write
 *
 *   NAME
 *       wait_cpu_cycle_write()
 *
 *   SYNOPSIS
 *       void wait_cpu_cycle_write (uaecptr addr, int mode, uae_u32 v)
 *
 *   FUNCTION
 *       Write chip memory during an available cycle-exact CPU bus slot.
 *
 *   INPUTS
 *       addr -- emulated memory address
 *       mode -- positive for word, zero for byte, negative for no write
 *       v -- value to store
 *
 *   NOTES
 *       Consumes the arbitration cycle and one following CPU cycle. Available only with CPUEMU_6.
 *
 ****************************************************************************/

void wait_cpu_cycle_write (uaecptr addr, int mode, uae_u32 v)
{
    dma_cycle ();
    if (mode > 0)
	put_word (addr, v);
    else if (mode == 0)
	put_byte (addr, v);
    do_cycles (1 * CYCLE_UNIT);
}

/****i* januae-custom-timing-cycle/do_cycles_ce
 *
 *   NAME
 *       do_cycles_ce()
 *
 *   SYNOPSIS
 *       void do_cycles_ce (long cycles)
 *
 *   FUNCTION
 *       Advance emulation in cycle-exact steps while updating chip activity.
 *
 *   INPUTS
 *       cycles -- amount of emulated time to advance
 *
 *   NOTES
 *       Synchronizes Copper, line, bitplane fetch and blitter decisions after each step. Available only with CPUEMU_6.
 *
 ****************************************************************************/

void do_cycles_ce (long cycles)
{
    unsigned int hpos;
    while (cycles > 0) {
	do_cycles (1 * CYCLE_UNIT);
	cycles -= CYCLE_UNIT;
	hpos = current_hpos ();
	sync_copper (hpos);
	decide_line (hpos);
	decide_fetch_ce (hpos);
	decide_blitter (hpos);
    }
}

#endif
