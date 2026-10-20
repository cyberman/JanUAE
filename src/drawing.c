 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Screen drawing functions
  *
  * Copyright 1995-2000 Bernd Schmidt
  * Copyright 1995 Alessandro Bissacco
  * Copyright 2000,2001 Toni Wilen
  *
  * $Id$
  */

/* There are a couple of concepts of "coordinates" in this file.
   - DIW coordinates
   - DDF coordinates (essentially cycles, resolution lower than lores by a factor of 2)
   - Pixel coordinates
     * in the Amiga's resolution as determined by BPLCON0 ("Amiga coordinates")
     * in the window resolution as determined by the preferences ("window coordinates").
     * in the window resolution, and with the origin being the topmost left corner of
       the window ("native coordinates")
   One note about window coordinates.  The visible area depends on the width of the
   window, and the centering code.  The first visible horizontal window coordinate is
   often _not_ 0, but the value of VISIBLE_LEFT_BORDER instead.

   One important thing to remember: DIW coordinates are in the lowest possible
   resolution.

   To prevent extremely bad things (think pixels cut in half by window borders) from
   happening, all ports should restrict window widths to be multiples of 16 pixels.  */

#include "sysconfig.h"
#include "sysdeps.h"

#include <ctype.h>
#include <assert.h>

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "xwin.h"
#include "gui.h"
#include "p96.h"
#include "drawing.h"
#ifdef JIT
# include "compemu.h"
#endif
#include "savestate.h"

#ifdef __AROS__
#include "od-amiga/j.h"
#endif

int lores_factor, lores_shift;

/* The shift factor to apply when converting between Amiga coordinates and window
   coordinates.  Zero if the resolution is the same, positive if window coordinates
   have a higher resolution (i.e. we're stretching the image), negative if window
   coordinates have a lower resolution (i.e. we're shrinking the image).  */
static int res_shift;

int interlace_seen;
#define AUTO_LORES_FRAMES 10
static int can_use_lores = 0;

/* Lookup tables for dual playfields.  The dblpf_*1 versions are for the case
   that playfield 1 has the priority, dbplpf_*2 are used if playfield 2 has
   priority.  If we need an array for non-dual playfield mode, it has no number.  */
/* The dbplpf_ms? arrays contain a shift value.  plf_spritemask is initialized
   to contain two 16 bit words, with the appropriate mask if pf1 is in the
   foreground being at bit offset 0, the one used if pf2 is in front being at
   offset 16.  */

static int dblpf_ms1[256], dblpf_ms2[256], dblpf_ms[256];
static int dblpf_ind1[256], dblpf_ind2[256];

static int dblpf_2nd1[256], dblpf_2nd2[256];

static int dblpfofs[] = { 0, 2, 4, 8, 16, 32, 64, 128 };

static int sprite_offs[256];

static uae_u32 clxtab[256];

/* Video buffer description structure. Filled in by the graphics system
 * dependent code. */

struct vidbuf_description gfxvidinfo;

/* OCS/ECS color lookup table. */
xcolnr xcolors[4096];

#ifdef AGA
static uae_u8 spriteagadpfpixels[MAX_PIXELS_PER_LINE * 2]; /* AGA dualplayfield sprite */
/* AGA mode color lookup tables */
unsigned int xredcolors[256], xgreencolors[256], xbluecolors[256];
static int dblpf_ind1_aga[256], dblpf_ind2_aga[256];
#else
static uae_u8 spriteagadpfpixels[1];
static int dblpf_ind1_aga[1], dblpf_ind2_aga[1];
#endif

struct color_entry colors_for_drawing;

/* The size of these arrays is pretty arbitrary; it was chosen to be "more
   than enough".  The coordinates used for indexing into these arrays are
   almost, but not quite, Amiga coordinates (there's a constant offset).  */
union {
    /* Let's try to align this thing. */
    double uupzuq;
    long int cruxmedo;
    uae_u8 apixels[MAX_PIXELS_PER_LINE * 2];
    uae_u16 apixels_w[MAX_PIXELS_PER_LINE * 2 / 2];
    uae_u32 apixels_l[MAX_PIXELS_PER_LINE * 2 / 4];
} pixdata;

#ifdef OS_WITHOUT_MEMORY_MANAGEMENT
uae_u16 *spixels;
#else
uae_u16 spixels[2 * MAX_SPR_PIXELS];
#endif

/* Eight bits for every pixel.  */
union sps_union spixstate;

static uae_u32 ham_linebuf[MAX_PIXELS_PER_LINE * 2];

uae_u8 *xlinebuffer;

/*static*/ int *amiga2aspect_line_map, *native2amiga_line_map;
static uae_u8 *row_map[MAX_VIDHEIGHT + 1];
static int max_drawn_amiga_line;

/* line_draw_funcs: pfield_do_linetoscr, pfield_do_fill_line, decode_ham */
typedef void (*line_draw_func)(int, int);

#define LINE_UNDECIDED 1
#define LINE_DECIDED 2
#define LINE_DECIDED_DOUBLE 3
#define LINE_AS_PREVIOUS 4
#define LINE_BLACK 5
#define LINE_REMEMBERED_AS_BLACK 6
#define LINE_DONE 7
#define LINE_DONE_AS_PREVIOUS 8
#define LINE_REMEMBERED_AS_PREVIOUS 9

static char linestate[(MAXVPOS + 1) * 2 + 1];

uae_u8 line_data[(MAXVPOS + 1) * 2][MAX_PLANES * MAX_WORDS_PER_LINE * 2];

/* Centering variables.  */
/*static*/ int min_diwstart, max_diwstop;
/* The visible window: VISIBLE_LEFT_BORDER contains the left border of the visible
   area, VISIBLE_RIGHT_BORDER the right border.  These are in window coordinates.  */
/*static*/ int visible_left_border, visible_right_border;
/*static*/ int linetoscr_x_adjust_bytes;
/*static*/ int thisframe_y_adjust;
/*static*/ int thisframe_y_adjust_real, max_ypos_thisframe, min_ypos_for_screen;
/*static*/ int extra_y_adjust;
int thisframe_first_drawn_line, thisframe_last_drawn_line;

/* A frame counter that forces a redraw after at least one skipped frame in
   interlace mode.  */
static int last_redraw_point;

static int first_drawn_line, last_drawn_line;
static int first_block_line, last_block_line;

#define NO_BLOCK -3

/* These are generated by the drawing code from the line_decisions array for
   each line that needs to be drawn.  These are basically extracted out of
   bit fields in the hardware registers.  */
static int bplehb, bplham, bpldualpf, bpldualpfpri, bpldualpf2of, bplplanecnt, bplres;
static int plf1pri, plf2pri;
static uae_u32 plf_sprite_mask;
static int sbasecol[2] = { 16, 16 };
static int brdsprt, brdblank;

int picasso_requested_on;
int picasso_on;

int inhibit_frame;

int framecnt = 0;
static int frame_redraw_necessary;
static int picasso_redraw_necessary;

/****i* januae-drawing/count_frame
*
*   NAME
*       count_frame() -- Advance the frame-skip counter.
*
*   SYNOPSIS
*       STATIC_INLINE void count_frame (void);
*
*   FUNCTION
*       Increment framecnt and wrap it to zero at the configured graphics
*       frame-rate threshold; the counter is not clamped against an invalid
*       threshold.
*
***************************************************************************
*
*/

STATIC_INLINE void count_frame (void)
{
    framecnt++;
    if (framecnt >= currprefs.gfx_framerate)
	framecnt = 0;
}

/****i* januae-drawing/coord_native_to_amiga_x
*
*   NAME
*       coord_native_to_amiga_x() -- Map a native horizontal coordinate to the emulated display.
*
*   SYNOPSIS
*       int coord_native_to_amiga_x (int x);
*
*   FUNCTION
*       Apply the current left-border offset and low-resolution scaling, then
*       translate to the hardware display origin.
*
*   INPUTS
*       x - Horizontal destination or source coordinate in native pixels, as
*       applicable.
*
*   RESULT
*       Horizontal coordinate in the emulated display coordinate system.
*
***************************************************************************
*
*/

int coord_native_to_amiga_x (int x)
{
  int res;
//  kprintf("FOO: coord_native_to_amiga_x(%d)\n",x);
    x += visible_left_border;
    x <<= (1 - lores_shift);
    res=x + 2 * DISPLAY_LEFT_SHIFT - 2 * DIW_DDF_OFFSET;
//  kprintf("FOO: res: %d\n",res);
    return res;
}

/*********************************************
 * coord_native_to_amiga_y
 *
 * is also used in od-amiga/j-mouse.c
 *
 *********************************************/
/****i* januae-drawing/coord_native_to_amiga_y
*
*   NAME
*       coord_native_to_amiga_y() -- Map a native scanline to an Amiga scanline.
*
*   SYNOPSIS
*       int coord_native_to_amiga_y (int y);
*
*   FUNCTION
*       Clamp the input to the configured native screen range and look up the
*       emulated line with the current vertical adjustment. A valid
*       native2amiga_line_map must already exist.
*
*   INPUTS
*       y - Vertical line or font-row coordinate.
*
*   RESULT
*       Mapped scanline after adjustment; an unmapped native line can contribute
*       a negative map entry.
*
***************************************************************************
*
*/

int coord_native_to_amiga_y (int y) {
  int res;

  //JWLOG("coord_native_to_amiga_y(%d)\n",y);

  /* better clip it here! */
  if(y > gfxvidinfo.height) {
    y=gfxvidinfo.height - 1; /* last row */
  }
  if(y<0) {
    y=0;
  }
  //JWLOG("coord_native_to_amiga_y() => %d\n",y);
  res=native2amiga_line_map[y] + thisframe_y_adjust - minfirstline;

  return res;
}

/****i* januae-drawing/res_shift_from_window
*
*   NAME
*       res_shift_from_window() -- Scale a horizontal window distance.
*
*   SYNOPSIS
*       STATIC_INLINE int res_shift_from_window (int x);
*
*   FUNCTION
*       Apply the current signed resolution shift using an integer right shift
*       for nonnegative res_shift and a left shift otherwise.
*
*   INPUTS
*       x - Horizontal destination or source coordinate in native pixels, as
*       applicable.
*
*   RESULT
*       Scaled integer coordinate.
*
***************************************************************************
*
*/

STATIC_INLINE int res_shift_from_window (int x)
{
    if (res_shift >= 0)
	return x >> res_shift;
    return x << -res_shift;
}

/****i* januae-drawing/res_shift_from_amiga
*
*   NAME
*       res_shift_from_amiga() -- Scale an Amiga horizontal distance.
*
*   SYNOPSIS
*       STATIC_INLINE int res_shift_from_amiga (int x);
*
*   FUNCTION
*       Apply the signed resolution shift to the supplied coordinate; this
*       implementation uses the same shifts as res_shift_from_window.
*
*   INPUTS
*       x - Horizontal destination or source coordinate in native pixels, as
*       applicable.
*
*   RESULT
*       Scaled integer coordinate.
*
***************************************************************************
*
*/

STATIC_INLINE int res_shift_from_amiga (int x)
{
    if (res_shift >= 0)
	return x >> res_shift;
    return x << -res_shift;
}

/****i* januae-drawing/notice_screen_contents_lost
*
*   NAME
*       notice_screen_contents_lost() -- Request reconstruction of the visible image.
*
*   SYNOPSIS
*       void notice_screen_contents_lost (void);
*
*   FUNCTION
*       Mark both the Picasso and native frame as requiring redraw; the native
*       redraw counter is set to two.
*
***************************************************************************
*
*/

void notice_screen_contents_lost (void)
{
    picasso_redraw_necessary = 1;
    frame_redraw_necessary = 2;
}

static struct decision *dp_for_drawing;
static struct draw_info *dip_for_drawing;

/* Record DIW of the current line for use by centering code.  */
/****i* januae-drawing/record_diw_line
*
*   NAME
*       record_diw_line() -- Accumulate horizontal display-window extrema.
*
*   SYNOPSIS
*       void record_diw_line (int first, int last);
*
*   FUNCTION
*       Expand the tracked DIW minimum and maximum with the supplied first and
*       last positions; this does not write pixels.
*
*   INPUTS
*       first - First DIW position of the line.
*       last - Last DIW position of the line.
*
***************************************************************************
*
*/

void record_diw_line (int first, int last)
{
    if (last > max_diwstop)
	max_diwstop = last;
    if (first < min_diwstart)
	min_diwstart = first;
}

/*
 * Screen update macros/functions
 */

/* The important positions in the line: where do we start drawing the left border,
   where do we start drawing the playfield, where do we start drawing the right border.
   All of these are forced into the visible window (VISIBLE_LEFT_BORDER .. VISIBLE_RIGHT_BORDER).
   PLAYFIELD_START and PLAYFIELD_END are in window coordinates.  */
static int playfield_start, playfield_end;
static int real_playfield_start, real_playfield_end;

static int pixels_offset;
static int src_pixel;
/* How many pixels in window coordinates which are to the left of the left border.  */
static int unpainted;

/* Initialize the variables necessary for drawing a line.
 * This involves setting up start/stop positions and display window
 * borders.  */
/****i* januae-drawing/pfield_init_linetoscr
*
*   NAME
*       pfield_init_linetoscr() -- Establish one line's visible playfield boundaries.
*
*   SYNOPSIS
*       static void pfield_init_linetoscr (void);
*
*   FUNCTION
*       Derive clipped playfield and source-pixel offsets from the current
*       decision and draw-info records. AGA border sprites may extend the draw
*       area; sprite data outside the fetched display region is cleared.
*
***************************************************************************
*
*/

static void pfield_init_linetoscr (void)
{
    /* First, get data fetch start/stop in DIW coordinates.  */
    int ddf_left = dp_for_drawing->plfleft * 2 + DIW_DDF_OFFSET;
    int ddf_right = dp_for_drawing->plfright * 2 + DIW_DDF_OFFSET;
    /* Compute datafetch start/stop in pixels; native display coordinates.  */
    int native_ddf_left = coord_hw_to_window_x (ddf_left);
    int native_ddf_right = coord_hw_to_window_x (ddf_right);

    int linetoscr_diw_start = dp_for_drawing->diwfirstword;
    int linetoscr_diw_end = dp_for_drawing->diwlastword;

    if (dip_for_drawing->nr_sprites == 0) {
	if (linetoscr_diw_start < native_ddf_left)
	    linetoscr_diw_start = native_ddf_left;
	if (linetoscr_diw_end > native_ddf_right)
	    linetoscr_diw_end = native_ddf_right;
    }

    /* Perverse cases happen. */
    if (linetoscr_diw_end < linetoscr_diw_start)
	linetoscr_diw_end = linetoscr_diw_start;

    playfield_start = linetoscr_diw_start;
    playfield_end = linetoscr_diw_end;

    if (playfield_start < visible_left_border)
	playfield_start = visible_left_border;
    if (playfield_start > visible_right_border)
	playfield_start = visible_right_border;
    if (playfield_end < visible_left_border)
	playfield_end = visible_left_border;
    if (playfield_end > visible_right_border)
	playfield_end = visible_right_border;

    real_playfield_end = playfield_end;
    real_playfield_start = playfield_start;
#ifdef AGA
    if (brdsprt && dip_for_drawing->nr_sprites) {
	int min = visible_right_border, max = visible_left_border, i;
	for (i = 0; i < dip_for_drawing->nr_sprites; i++) {
	    int x;
	    x = curr_sprite_entries[dip_for_drawing->first_sprite_entry + i].pos;
	    if (x < min) min = x;
	    x = curr_sprite_entries[dip_for_drawing->first_sprite_entry + i].max;
	    if (x > max) max = x;
	}
	min += (DIW_DDF_OFFSET - DISPLAY_LEFT_SHIFT) << (2 - lores_shift);
	max += (DIW_DDF_OFFSET - DISPLAY_LEFT_SHIFT) << (2 - lores_shift);
	if (min < playfield_start)
	    playfield_start = min;
	if (playfield_start < visible_left_border)
	    playfield_start = visible_left_border;
	if (max > playfield_end)
	    playfield_end = max;
	if (playfield_end > visible_right_border)
	    playfield_end = visible_right_border;
    }
#endif
    /* Now, compute some offsets.  */

    res_shift = lores_shift - bplres;
    ddf_left -= DISPLAY_LEFT_SHIFT;
    ddf_left <<= bplres;
    pixels_offset = MAX_PIXELS_PER_LINE - ddf_left;

    unpainted = visible_left_border < playfield_start ? 0 : visible_left_border - playfield_start;
    src_pixel = MAX_PIXELS_PER_LINE + res_shift_from_window (playfield_start - native_ddf_left + unpainted);

    if (dip_for_drawing->nr_sprites == 0)
	return;
    /* Must clear parts of apixels.  */
    if (linetoscr_diw_start < native_ddf_left) {
	int size = res_shift_from_window (native_ddf_left - linetoscr_diw_start);
	linetoscr_diw_start = native_ddf_left;
	memset (pixdata.apixels + MAX_PIXELS_PER_LINE - size, 0, size);
    }
    if (linetoscr_diw_end > native_ddf_right) {
	int pos = res_shift_from_window (native_ddf_right - native_ddf_left);
	int size = res_shift_from_window (linetoscr_diw_end - native_ddf_right);
	linetoscr_diw_start = native_ddf_left;
	memset (pixdata.apixels + MAX_PIXELS_PER_LINE + pos, 0, size);
    }
}

/****i* januae-drawing/fill_line_8
*
*   NAME
*       fill_line_8() -- Fill an 8-bit destination interval with border color.
*
*   SYNOPSIS
*       static void fill_line_8 (uae_u8 *buf, unsigned int start, unsigned int
*       stop);
*
*   FUNCTION
*       Write color-zero pixels or blanking black to a half-open range of 8-bit
*       framebuffer elements, combining aligned groups of four bytes.
*
*   INPUTS
*       buf - Caller-owned framebuffer line to fill.
*       start - Inclusive first destination pixel or window coordinate.
*       stop - Exclusive last destination pixel or window coordinate.
*
***************************************************************************
*
*/

static void fill_line_8 (uae_u8 *buf, unsigned int start, unsigned int stop)
{
    uae_u8 *b = (uae_u8 *)buf;
    unsigned int i;
    unsigned int rem = 0;
    xcolnr col = brdblank ? 0 : colors_for_drawing.acolors[0];
    while (((long)&b[start]) & 3) {
	b[start++] = (uae_u8)col;
	if (start == stop)
	   return;
    }
    if (((long)&b[stop]) & 3) {
	rem = ((long)&b[stop]) & 3;
	stop -= rem;
    }
    for (i = start; i < stop; i += 4) {
	uae_u32 *b2 = (uae_u32 *)&b[i];
	*b2 = col;
    }
    while (rem--)
	b[stop++] = (uae_u8) col;
}

/****i* januae-drawing/fill_line_16
*
*   NAME
*       fill_line_16() -- Fill a 16-bit destination interval with border color.
*
*   SYNOPSIS
*       static void fill_line_16 (uae_u8 *buf, unsigned int start, unsigned int
*       stop);
*
*   FUNCTION
*       Fill a half-open interval of 16-bit pixels with the selected border
*       value, using aligned pairs where possible.
*
*   INPUTS
*       buf - Caller-owned framebuffer line to fill.
*       start - Inclusive first destination pixel or window coordinate.
*       stop - Exclusive last destination pixel or window coordinate.
*
***************************************************************************
*
*/

static void fill_line_16 (uae_u8 *buf, unsigned int start, unsigned int stop)
{
    uae_u16 *b = (uae_u16 *)buf;
    unsigned int i;
    unsigned int rem = 0;
    xcolnr col = brdblank ? 0 : colors_for_drawing.acolors[0];
    if (((long)&b[start]) & 1)
	b[start++] = (uae_u16) col;
    if (start >= stop)
	return;
    if (((long)&b[stop]) & 1) {
	rem++;
	stop--;
    }
    for (i = start; i < stop; i += 2) {
	uae_u32 *b2 = (uae_u32 *)&b[i];
	*b2 = col;
    }
    if (rem)
	b[stop] = (uae_u16)col;
}

/****i* januae-drawing/fill_line_32
*
*   NAME
*       fill_line_32() -- Fill a 32-bit destination interval with border color.
*
*   SYNOPSIS
*       static void fill_line_32 (uae_u8 *buf, unsigned int start, unsigned int
*       stop);
*
*   FUNCTION
*       Store the current border or blanking color in each 32-bit pixel of the
*       specified half-open interval.
*
*   INPUTS
*       buf - Caller-owned framebuffer line to fill.
*       start - Inclusive first destination pixel or window coordinate.
*       stop - Exclusive last destination pixel or window coordinate.
*
***************************************************************************
*
*/

static void fill_line_32 (uae_u8 *buf, unsigned int start, unsigned int stop)
{
    uae_u32 *b = (uae_u32 *)buf;
    unsigned int i;
    xcolnr col = brdblank ? 0 : colors_for_drawing.acolors[0];
    for (i = start; i < stop; i++)
	b[i] = col;
}

/****i* januae-drawing/fill_line
*
*   NAME
*       fill_line() -- Fill the current screen line with its border color.
*
*   SYNOPSIS
*       STATIC_INLINE void fill_line (void);
*
*   FUNCTION
*       Fill the active line buffer using the current visible width, pixel size
*       and left-border offset. The fast path writes full 32-bit groups and
*       assumes a valid display buffer.
*
***************************************************************************
*
*/

STATIC_INLINE void fill_line (void)
{
    int shift;
    int nints, nrem;
    int *start;
    xcolnr val;

    shift = 0;
    if (gfxvidinfo.pixbytes == 2)
	shift = 1;
    if (gfxvidinfo.pixbytes == 4)
	shift = 2;

    nints = gfxvidinfo.width >> (2-shift);
    nrem = nints & 7;
    nints &= ~7;
    start = (int *)(((char *)xlinebuffer) + (visible_left_border << shift));
#ifdef AGA
    val = brdblank ? 0 : colors_for_drawing.acolors[0];
#else
    val = colors_for_drawing.acolors[0];
#endif
    for (; nints > 0; nints -= 8, start += 8) {
	*start = val;
	*(start+1) = val;
	*(start+2) = val;
	*(start+3) = val;
	*(start+4) = val;
	*(start+5) = val;
	*(start+6) = val;
	*(start+7) = val;
    }

    switch (nrem) {
     case 7:
	*start++ = val;
     case 6:
	*start++ = val;
     case 5:
	*start++ = val;
     case 4:
	*start++ = val;
     case 3:
	*start++ = val;
     case 2:
	*start++ = val;
     case 1:
	*start = val;
    }
}

#include "linetoscr.c"

/****i* januae-drawing/pfield_do_linetoscr
*
*   NAME
*       pfield_do_linetoscr() -- Copy decoded playfield indices into display pixels.
*
*   SYNOPSIS
*       static void pfield_do_linetoscr (int start, int stop);
*
*   FUNCTION
*       Dispatch the selected AGA or OCS/ECS conversion for 8-, 16- or 32-bit
*       destination pixels and the active stretch/shrink ratio. Update the
*       consumed source-pixel index; unsupported pixel sizes have no conversion
*       case.
*
*   INPUTS
*       start - Inclusive first destination pixel or window coordinate.
*       stop - Exclusive last destination pixel or window coordinate.
*
***************************************************************************
*
*/

static void pfield_do_linetoscr (int start, int stop)
{
#ifdef AGA
    if (currprefs.chipset_mask & CSMASK_AGA) {
	if (res_shift == 0)
	    switch (gfxvidinfo.pixbytes) {
	    case 1: src_pixel = linetoscr_8_aga (src_pixel, start, stop); break;
	    case 2: src_pixel = linetoscr_16_aga (src_pixel, start, stop); break;
	    case 4: src_pixel = linetoscr_32_aga (src_pixel, start, stop); break;
	    }
	else if (res_shift > 0)
	    switch (gfxvidinfo.pixbytes) {
	    case 1: src_pixel = linetoscr_8_stretch1_aga (src_pixel, start, stop); break;
	    case 2: src_pixel = linetoscr_16_stretch1_aga (src_pixel, start, stop); break;
	    case 4: src_pixel = linetoscr_32_stretch1_aga (src_pixel, start, stop); break;
	    }
	else if (res_shift < 0)
	    switch (gfxvidinfo.pixbytes) {
	    case 1: src_pixel = linetoscr_8_shrink1_aga (src_pixel, start, stop); break;
	    case 2: src_pixel = linetoscr_16_shrink1_aga (src_pixel, start, stop); break;
	    case 4: src_pixel = linetoscr_32_shrink1_aga (src_pixel, start, stop); break;
	    }
    } else {
#endif
	if (res_shift == 0)
	    switch (gfxvidinfo.pixbytes) {
	    case 1: src_pixel = linetoscr_8 (src_pixel, start, stop); break;
	    case 2: src_pixel = linetoscr_16 (src_pixel, start, stop); break;
	    case 4: src_pixel = linetoscr_32 (src_pixel, start, stop); break;
	    }
	else if (res_shift > 0)
	    switch (gfxvidinfo.pixbytes) {
	    case 1: src_pixel = linetoscr_8_stretch1 (src_pixel, start, stop); break;
	    case 2: src_pixel = linetoscr_16_stretch1 (src_pixel, start, stop); break;
	    case 4: src_pixel = linetoscr_32_stretch1 (src_pixel, start, stop); break;
	    }
	else if (res_shift < 0)
	    switch (gfxvidinfo.pixbytes) {
	    case 1: src_pixel = linetoscr_8_shrink1 (src_pixel, start, stop); break;
	    case 2: src_pixel = linetoscr_16_shrink1 (src_pixel, start, stop); break;
	    case 4: src_pixel = linetoscr_32_shrink1 (src_pixel, start, stop); break;
	    }
#ifdef AGA
    }
#endif
}

/****i* januae-drawing/pfield_do_fill_line
*
*   NAME
*       pfield_do_fill_line() -- Fill a border interval at the selected pixel depth.
*
*   SYNOPSIS
*       static void pfield_do_fill_line (int start, int stop);
*
*   FUNCTION
*       Dispatch an 8-, 16- or 32-bit border fill for the current scanline
*       buffer. Unsupported pixel depths cause no write.
*
*   INPUTS
*       start - Inclusive first destination pixel or window coordinate.
*       stop - Exclusive last destination pixel or window coordinate.
*
***************************************************************************
*
*/

static void pfield_do_fill_line (int start, int stop)
{
    switch (gfxvidinfo.pixbytes) {
    case 1: fill_line_8 (xlinebuffer, start, stop); break;
    case 2: fill_line_16 (xlinebuffer, start, stop); break;
    case 4: fill_line_32 (xlinebuffer, start, stop); break;
    }
}

/****i* januae-drawing/dummy_worker
*
*   NAME
*       dummy_worker() -- Ignore a scanline interval.
*
*   SYNOPSIS
*       static void dummy_worker (int start, int stop);
*
*   FUNCTION
*       Intentionally do nothing. Used when replaying color changes solely to
*       advance HAM decoding state or hardware settings.
*
*   INPUTS
*       start - Inclusive first destination pixel or window coordinate.
*       stop - Exclusive last destination pixel or window coordinate.
*
***************************************************************************
*
*/

static void dummy_worker (int start, int stop)
{
}

static unsigned int ham_lastcolor;

static int ham_decode_pixel;

/* Decode HAM in the invisible portion of the display (left of VISIBLE_LEFT_BORDER),
   but don't draw anything in.  This is done to prepare HAM_LASTCOLOR for later,
   when decode_ham runs.  */
/****i* januae-drawing/init_ham_decoding
*
*   NAME
*       init_ham_decoding() -- Initialize hold-and-modify color state for a line.
*
*   SYNOPSIS
*       static void init_ham_decoding (void);
*
*   FUNCTION
*       Set the initial palette color and decode any invisible pixels preceding
*       the visible border, preserving HAM color carry into the visible
*       interval. Handle OCS/ECS HAM6 and conditional AGA HAM6/HAM8.
*
***************************************************************************
*
*/

static void init_ham_decoding (void)
{
    int unpainted_amiga = res_shift_from_window (unpainted);
    ham_decode_pixel = src_pixel;
    ham_lastcolor = color_reg_get (&colors_for_drawing, 0);

    if (! bplham || (bplplanecnt != 6 && ((currprefs.chipset_mask & CSMASK_AGA) == 0 || bplplanecnt != 8))) {
	if (unpainted_amiga > 0) {
	    int pv = pixdata.apixels[ham_decode_pixel + unpainted_amiga - 1];
#ifdef AGA
	    if (currprefs.chipset_mask & CSMASK_AGA)
		ham_lastcolor = colors_for_drawing.color_regs_aga[pv];
	    else
#endif
		ham_lastcolor = colors_for_drawing.color_regs_ecs[pv];
	}
#ifdef AGA
    } else if (currprefs.chipset_mask & CSMASK_AGA) {
	if (bplplanecnt == 8) { /* AGA mode HAM8 */
	    while (unpainted_amiga-- > 0) {
		int pv = pixdata.apixels[ham_decode_pixel++];
		switch (pv & 0x3) {
		case 0x0: ham_lastcolor = colors_for_drawing.color_regs_aga[pv >> 2]; break;
		case 0x1: ham_lastcolor &= 0xFFFF03; ham_lastcolor |= (pv & 0xFC); break;
		case 0x2: ham_lastcolor &= 0x03FFFF; ham_lastcolor |= (pv & 0xFC) << 16; break;
		case 0x3: ham_lastcolor &= 0xFF03FF; ham_lastcolor |= (pv & 0xFC) << 8; break;
		}
	    }
	} else if (bplplanecnt == 6) { /* AGA mode HAM6 */
	    while (unpainted_amiga-- > 0) {
		int pv = pixdata.apixels[ham_decode_pixel++];
		switch (pv & 0x30) {
		case 0x00: ham_lastcolor = colors_for_drawing.color_regs_aga[pv]; break;
		case 0x10: ham_lastcolor &= 0xFFFF00; ham_lastcolor |= (pv & 0xF) << 4; break;
		case 0x20: ham_lastcolor &= 0x00FFFF; ham_lastcolor |= (pv & 0xF) << 20; break;
		case 0x30: ham_lastcolor &= 0xFF00FF; ham_lastcolor |= (pv & 0xF) << 12; break;
		}
	    }
	}
#endif
    } else {
	if (bplplanecnt == 6) { /* OCS/ECS mode HAM6 */
	    while (unpainted_amiga-- > 0) {
		int pv = pixdata.apixels[ham_decode_pixel++];
		switch (pv & 0x30) {
		case 0x00: ham_lastcolor = colors_for_drawing.color_regs_ecs[pv]; break;
		case 0x10: ham_lastcolor &= 0xFF0; ham_lastcolor |= (pv & 0xF); break;
		case 0x20: ham_lastcolor &= 0x0FF; ham_lastcolor |= (pv & 0xF) << 8; break;
		case 0x30: ham_lastcolor &= 0xF0F; ham_lastcolor |= (pv & 0xF) << 4; break;
		}
	    }
	}
    }
}

/****i* januae-drawing/decode_ham
*
*   NAME
*       decode_ham() -- Decode a visible interval of HAM color indices.
*
*   SYNOPSIS
*       static void decode_ham (int pix, int stoppos);
*
*   FUNCTION
*       Advance the current HAM pixel cursor, converting indexed or HAM6/HAM8
*       values into full colors in ham_linebuf. The prior HAM color persists
*       across successive intervals.
*
*   INPUTS
*       pix - Starting visible window coordinate.
*       stoppos - Ending visible window coordinate.
*
***************************************************************************
*
*/

static void decode_ham (int pix, int stoppos)
{
    int todraw_amiga = res_shift_from_window (stoppos - pix);

    if (! bplham || (bplplanecnt != 6 && ((currprefs.chipset_mask & CSMASK_AGA) == 0 || bplplanecnt != 8))) {
	while (todraw_amiga-- > 0) {
	    int pv = pixdata.apixels[ham_decode_pixel];
#ifdef AGA
	    if (currprefs.chipset_mask & CSMASK_AGA)
		ham_lastcolor = colors_for_drawing.color_regs_aga[pv];
	    else
#endif
		ham_lastcolor = colors_for_drawing.color_regs_ecs[pv];

	    ham_linebuf[ham_decode_pixel++] = ham_lastcolor;
	}
#ifdef AGA
    } else if (currprefs.chipset_mask & CSMASK_AGA) {
	if (bplplanecnt == 8) { /* AGA mode HAM8 */
	    while (todraw_amiga-- > 0) {
		int pv = pixdata.apixels[ham_decode_pixel];
		switch (pv & 0x3) {
		case 0x0: ham_lastcolor = colors_for_drawing.color_regs_aga[pv >> 2]; break;
		case 0x1: ham_lastcolor &= 0xFFFF03; ham_lastcolor |= (pv & 0xFC); break;
		case 0x2: ham_lastcolor &= 0x03FFFF; ham_lastcolor |= (pv & 0xFC) << 16; break;
		case 0x3: ham_lastcolor &= 0xFF03FF; ham_lastcolor |= (pv & 0xFC) << 8; break;
		}
		ham_linebuf[ham_decode_pixel++] = ham_lastcolor;
	    }
	} else if (bplplanecnt == 6) { /* AGA mode HAM6 */
	    while (todraw_amiga-- > 0) {
		int pv = pixdata.apixels[ham_decode_pixel];
		switch (pv & 0x30) {
		case 0x00: ham_lastcolor = colors_for_drawing.color_regs_aga[pv]; break;
		case 0x10: ham_lastcolor &= 0xFFFF00; ham_lastcolor |= (pv & 0xF) << 4; break;
		case 0x20: ham_lastcolor &= 0x00FFFF; ham_lastcolor |= (pv & 0xF) << 20; break;
		case 0x30: ham_lastcolor &= 0xFF00FF; ham_lastcolor |= (pv & 0xF) << 12; break;
		}
		ham_linebuf[ham_decode_pixel++] = ham_lastcolor;
	    }
	}
#endif
    } else {
	if (bplplanecnt == 6) { /* OCS/ECS mode HAM6 */
	    while (todraw_amiga-- > 0) {
		int pv = pixdata.apixels[ham_decode_pixel];
		switch (pv & 0x30) {
		case 0x00: ham_lastcolor = colors_for_drawing.color_regs_ecs[pv]; break;
		case 0x10: ham_lastcolor &= 0xFF0; ham_lastcolor |= (pv & 0xF); break;
		case 0x20: ham_lastcolor &= 0x0FF; ham_lastcolor |= (pv & 0xF) << 8; break;
		case 0x30: ham_lastcolor &= 0xF0F; ham_lastcolor |= (pv & 0xF) << 4; break;
		}
		ham_linebuf[ham_decode_pixel++] = ham_lastcolor;
	    }
	}
    }
}

/****i* januae-drawing/gen_pfield_tables
*
*   NAME
*       gen_pfield_tables() -- Build sprite and dual-playfield lookup tables.
*
*   SYNOPSIS
*       static void gen_pfield_tables (void);
*
*   FUNCTION
*       Populate the 256-entry dual-playfield priority, sprite-mask and
*       collision lookup arrays, including optional AGA priority tables. The
*       OCS/ECS sprite encoding for indices above 127 is preserved.
*
***************************************************************************
*
*/

static void gen_pfield_tables (void)
{
    int i;

    /* For now, the AGA stuff is broken in the dual playfield case. We encode
     * sprites in dpf mode by ORing the pixel value with 0x80. To make dual
     * playfield rendering easy, the lookup tables contain are made linear for
     * values >= 128. That only works for OCS/ECS, though. */

    for (i = 0; i < 256; i++) {
	int plane1 = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4) | ((i >> 3) & 8);
	int plane2 = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4) | ((i >> 4) & 8);

	dblpf_2nd1[i] = plane1 == 0 ? (plane2 == 0 ? 0 : 2) : 1;
	dblpf_2nd2[i] = plane2 == 0 ? (plane1 == 0 ? 0 : 1) : 2;

#ifdef AGA
	dblpf_ind1_aga[i] = plane1 == 0 ? plane2 : plane1;
	dblpf_ind2_aga[i] = plane2 == 0 ? plane1 : plane2;
#endif

	dblpf_ms1[i] = plane1 == 0 ? (plane2 == 0 ? 16 : 8) : 0;
	dblpf_ms2[i] = plane2 == 0 ? (plane1 == 0 ? 16 : 0) : 8;
	dblpf_ms[i] = i == 0 ? 16 : 8;
	if (plane2 > 0)
	    plane2 += 8;
	dblpf_ind1[i] = i >= 128 ? i & 0x7F : (plane1 == 0 ? plane2 : plane1);
	dblpf_ind2[i] = i >= 128 ? i & 0x7F : (plane2 == 0 ? plane1 : plane2);

	sprite_offs[i] = (i & 15) ? 0 : 2;

	clxtab[i] = ((((i & 3) && (i & 12)) << 9)
		     | (((i & 3) && (i & 48)) << 10)
		     | (((i & 3) && (i & 192)) << 11)
		     | (((i & 12) && (i & 48)) << 12)
		     | (((i & 12) && (i & 192)) << 13)
		     | (((i & 48) && (i & 192)) << 14));

    }
}

#define SPRITE_DEBUG 0

/* When looking at this function and the ones that inline it, bear in mind
   what an optimizing compiler will do with this code.  All callers of this
   function only pass in constant arguments (except for E).  This means
   that many of the if statements will go away completely after inlining.  */
/****i* januae-drawing/draw_sprites_1
*
*   NAME
*       draw_sprites_1() -- Composite a sprite interval into playfield pixel data.
*
*   SYNOPSIS
*       STATIC_INLINE void draw_sprites_1 (struct sprite_entry *e, int ham, int
*       dualpf, int doubling, int skip, int has_attach, int aga);
*
*   FUNCTION
*       Mask sprite pairs against the current playfield priority, resolve
*       attached sprites and HAM or dual-playfield palette paths, and write the
*       selected indices or colors. Scaling and AGA behavior are controlled by
*       the caller's flags.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*       ham - Nonzero selects the hold-and-modify color path.
*       dualpf - Nonzero selects dual-playfield priority handling.
*       doubling - Nonzero duplicates each sprite pixel horizontally.
*       skip - Shift controlling skipped input sprite pixels.
*       has_attach - Nonzero indicates an attached sprite pair.
*       aga - Nonzero selects AGA-specific color and priority handling.
*
***************************************************************************
*
*/

STATIC_INLINE void draw_sprites_1 (struct sprite_entry *e, int ham, int dualpf,
    int doubling, int skip, int has_attach, int aga)
{
    int *shift_lookup = dualpf ? (bpldualpfpri ? dblpf_ms2 : dblpf_ms1) : dblpf_ms;
    uae_u16 *buf = spixels + e->first_pixel;
    uae_u8 *stbuf = spixstate.bytes + e->first_pixel;
    int pos, window_pos;
#ifdef AGA
    uae_u8 xor_val = (uae_u8)(dp_for_drawing->bplcon4 >> 8);
#endif

    buf -= e->pos;
    stbuf -= e->pos;

    window_pos = e->pos + ((DIW_DDF_OFFSET - DISPLAY_LEFT_SHIFT) << (aga ? 1 : 0));
    if (skip)
	window_pos >>= 1;
    else if (doubling)
	window_pos <<= 1;
    window_pos += pixels_offset;
    for (pos = e->pos; pos < e->max; pos += 1 << skip) {
	int maskshift, plfmask;
	unsigned int v = buf[pos];

	/* The value in the shift lookup table is _half_ the shift count we
	   need.  This is because we can't shift 32 bits at once (undefined
	   behaviour in C).  */
	maskshift = shift_lookup[pixdata.apixels[window_pos]];
	plfmask = (plf_sprite_mask >> maskshift) >> maskshift;
	v &= ~plfmask;
	if (v != 0 || SPRITE_DEBUG) {
	    unsigned int vlo, vhi, col;
	    unsigned int v1 = v & 255;
	    /* OFFS determines the sprite pair with the highest priority that has
	       any bits set.  E.g. if we have 0xFF00 in the buffer, we have sprite
	       pairs 01 and 23 cleared, and pairs 45 and 67 set, so OFFS will
	       have a value of 4.
	       2 * OFFS is the bit number in V of the sprite pair, and it also
	       happens to be the color offset for that pair.  */
	    int offs;
	    if (v1 == 0)
		offs = 4 + sprite_offs[v >> 8];
	    else
		offs = sprite_offs[v1];

	    /* Shift highest priority sprite pair down to bit zero.  */
	    v >>= offs * 2;
	    v &= 15;
#if SPRITE_DEBUG > 0
	    v ^= 8;
#endif

	    if (has_attach && (stbuf[pos] & (3 << offs))) {
		col = v;
		if (aga)
		    col += sbasecol[1];
		else
		    col += 16;
	    } else {
		/* This sequence computes the correct color value.  We have to select
		   either the lower-numbered or the higher-numbered sprite in the pair.
		   We have to select the high one if the low one has all bits zero.
		   If the lower-numbered sprite has any bits nonzero, (VLO - 1) is in
		   the range of 0..2, and with the mask and shift, VHI will be zero.
		   If the lower-numbered sprite is zero, (VLO - 1) is a mask of
		   0xFFFFFFFF, and we select the bits of the higher numbered sprite
		   in VHI.
		   This is _probably_ more efficient than doing it with branches.  */
		vlo = v & 3;
		vhi = (v & (vlo - 1)) >> 2;
		col = (vlo | vhi);
		if (aga) {
		    if (vhi > 0)
			col += sbasecol[1];
		    else
			col += sbasecol[0];
		} else {
		    col += 16;
		}
		col += (offs * 2);
	    }
	    if (dualpf) {
#ifdef AGA
		if (aga) {
		    spriteagadpfpixels[window_pos] = col;
		    if (doubling)
			spriteagadpfpixels[window_pos + 1] = col;
		} else {
#endif
		    col += 128;
		    if (doubling)
			pixdata.apixels_w[window_pos >> 1] = col | (col << 8);
		    else
			pixdata.apixels[window_pos] = col;
#ifdef AGA
		}
#endif
	    } else if (ham) {
		col = color_reg_get (&colors_for_drawing, col);
#ifdef AGA
		if (aga)
		    col ^= xor_val;
#endif
		ham_linebuf[window_pos] = col;
		if (doubling)
		    ham_linebuf[window_pos + 1] = col;
	    } else {
#ifdef AGA
		if (aga)
		    col ^= xor_val;
#endif
		if (doubling)
		    pixdata.apixels_w[window_pos >> 1] = col | (col << 8);
		else
		    pixdata.apixels[window_pos] = col;
	    }
	}
	window_pos += 1 << doubling;
    }
}

/* See comments above.  Do not touch if you don't know what's going on.
 * (We do _not_ want the following to be inlined themselves).  */
/* lores bitplane, lores sprites */
/****i* januae-drawing/draw_sprites_normal_sp_lo_nat
*
*   NAME
*       draw_sprites_normal_sp_lo_nat() -- Composite a specialized normal sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_normal_sp_lo_nat (struct sprite_entry
*       *e);
*
*   FUNCTION
*       Composite the current sprite entry using normal palette,
*       single-playfield priority, low-resolution bitplanes and unattached
*       sprite decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_normal_sp_lo_nat (struct sprite_entry *e) { draw_sprites_1	(e, 0, 0, 0, 0, 0, 0); }
/****i* januae-drawing/draw_sprites_normal_dp_lo_nat
*
*   NAME
*       draw_sprites_normal_dp_lo_nat() -- Composite a specialized normal sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_normal_dp_lo_nat (struct sprite_entry
*       *e);
*
*   FUNCTION
*       Composite the current sprite entry using normal palette, dual-playfield
*       priority, low-resolution bitplanes and unattached sprite decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_normal_dp_lo_nat (struct sprite_entry *e) { draw_sprites_1	(e, 0, 1, 0, 0, 0, 0); }
/****i* januae-drawing/draw_sprites_ham_sp_lo_nat
*
*   NAME
*       draw_sprites_ham_sp_lo_nat() -- Composite a specialized ham sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_ham_sp_lo_nat (struct sprite_entry
*       *e);
*
*   FUNCTION
*       Composite the current sprite entry using hold-and-modify,
*       single-playfield priority, low-resolution bitplanes and unattached
*       sprite decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_ham_sp_lo_nat (struct sprite_entry *e) { draw_sprites_1	(e, 1, 0, 0, 0, 0, 0); }
/****i* januae-drawing/draw_sprites_normal_sp_lo_at
*
*   NAME
*       draw_sprites_normal_sp_lo_at() -- Composite a specialized normal sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_normal_sp_lo_at (struct sprite_entry
*       *e);
*
*   FUNCTION
*       Composite the current sprite entry using normal palette,
*       single-playfield priority, low-resolution bitplanes and attached sprite
*       decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_normal_sp_lo_at (struct sprite_entry *e) { draw_sprites_1	(e, 0, 0, 0, 0, 1, 0); }
/****i* januae-drawing/draw_sprites_normal_dp_lo_at
*
*   NAME
*       draw_sprites_normal_dp_lo_at() -- Composite a specialized normal sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_normal_dp_lo_at (struct sprite_entry
*       *e);
*
*   FUNCTION
*       Composite the current sprite entry using normal palette, dual-playfield
*       priority, low-resolution bitplanes and attached sprite decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_normal_dp_lo_at (struct sprite_entry *e) { draw_sprites_1	(e, 0, 1, 0, 0, 1, 0); }
/****i* januae-drawing/draw_sprites_ham_sp_lo_at
*
*   NAME
*       draw_sprites_ham_sp_lo_at() -- Composite a specialized ham sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_ham_sp_lo_at (struct sprite_entry *e);
*
*   FUNCTION
*       Composite the current sprite entry using hold-and-modify,
*       single-playfield priority, low-resolution bitplanes and attached sprite
*       decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_ham_sp_lo_at (struct sprite_entry *e) { draw_sprites_1		(e, 1, 0, 0, 0, 1, 0); }
/* hires bitplane, lores sprites */
/****i* januae-drawing/draw_sprites_normal_sp_hi_nat
*
*   NAME
*       draw_sprites_normal_sp_hi_nat() -- Composite a specialized normal sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_normal_sp_hi_nat (struct sprite_entry
*       *e);
*
*   FUNCTION
*       Composite the current sprite entry using normal palette,
*       single-playfield priority, high-resolution bitplanes and unattached
*       sprite decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_normal_sp_hi_nat (struct sprite_entry *e) { draw_sprites_1	(e, 0, 0, 1, 0, 0, 0); }
/****i* januae-drawing/draw_sprites_normal_dp_hi_nat
*
*   NAME
*       draw_sprites_normal_dp_hi_nat() -- Composite a specialized normal sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_normal_dp_hi_nat (struct sprite_entry
*       *e);
*
*   FUNCTION
*       Composite the current sprite entry using normal palette, dual-playfield
*       priority, high-resolution bitplanes and unattached sprite decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_normal_dp_hi_nat (struct sprite_entry *e) { draw_sprites_1	(e, 0, 1, 1, 0, 0, 0); }
/****i* januae-drawing/draw_sprites_ham_sp_hi_nat
*
*   NAME
*       draw_sprites_ham_sp_hi_nat() -- Composite a specialized ham sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_ham_sp_hi_nat (struct sprite_entry
*       *e);
*
*   FUNCTION
*       Composite the current sprite entry using hold-and-modify,
*       single-playfield priority, high-resolution bitplanes and unattached
*       sprite decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_ham_sp_hi_nat (struct sprite_entry *e) { draw_sprites_1	(e, 1, 0, 1, 0, 0, 0); }
/****i* januae-drawing/draw_sprites_normal_sp_hi_at
*
*   NAME
*       draw_sprites_normal_sp_hi_at() -- Composite a specialized normal sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_normal_sp_hi_at (struct sprite_entry
*       *e);
*
*   FUNCTION
*       Composite the current sprite entry using normal palette,
*       single-playfield priority, high-resolution bitplanes and attached sprite
*       decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_normal_sp_hi_at (struct sprite_entry *e) { draw_sprites_1	(e, 0, 0, 1, 0, 1, 0); }
/****i* januae-drawing/draw_sprites_normal_dp_hi_at
*
*   NAME
*       draw_sprites_normal_dp_hi_at() -- Composite a specialized normal sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_normal_dp_hi_at (struct sprite_entry
*       *e);
*
*   FUNCTION
*       Composite the current sprite entry using normal palette, dual-playfield
*       priority, high-resolution bitplanes and attached sprite decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_normal_dp_hi_at (struct sprite_entry *e) { draw_sprites_1	(e, 0, 1, 1, 0, 1, 0); }
/****i* januae-drawing/draw_sprites_ham_sp_hi_at
*
*   NAME
*       draw_sprites_ham_sp_hi_at() -- Composite a specialized ham sprite path.
*
*   SYNOPSIS
*       static void NOINLINE draw_sprites_ham_sp_hi_at (struct sprite_entry *e);
*
*   FUNCTION
*       Composite the current sprite entry using hold-and-modify,
*       single-playfield priority, high-resolution bitplanes and attached sprite
*       decoding.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

static void NOINLINE draw_sprites_ham_sp_hi_at (struct sprite_entry *e) { draw_sprites_1		(e, 1, 0, 1, 0, 1, 0); }

#ifdef AGA
/* not very optimized */
/****i* januae-drawing/draw_sprites_aga
*
*   NAME
*       draw_sprites_aga() -- Render an AGA sprite using the active bitplane resolution.
*
*   SYNOPSIS
*       STATIC_INLINE void draw_sprites_aga (struct sprite_entry *e);
*
*   FUNCTION
*       Select doubling or skipping from RES_HIRES and the playfield resolution,
*       then composite the entry with AGA color and priority handling. Available
*       only in AGA builds.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

STATIC_INLINE void draw_sprites_aga (struct sprite_entry *e)
{
    int diff = RES_HIRES - bplres;
    if (diff > 0)
	draw_sprites_1 (e, dp_for_drawing->ham_seen, bpldualpf, 0, diff, e->has_attached, 1);
    else
	draw_sprites_1 (e, dp_for_drawing->ham_seen, bpldualpf, -diff, 0, e->has_attached, 1);
}
#endif

/****i* januae-drawing/draw_sprites_ecs
*
*   NAME
*       draw_sprites_ecs() -- Select an OCS/ECS sprite compositor.
*
*   SYNOPSIS
*       STATIC_INLINE void draw_sprites_ecs (struct sprite_entry *e);
*
*   FUNCTION
*       Choose the specialized attached/nonattached, HAM/dual-playfield and
*       low-/high-resolution sprite path for the current entry.
*
*   INPUTS
*       e - Sprite entry with pixel range, position and attachment state.
*
***************************************************************************
*
*/

STATIC_INLINE void draw_sprites_ecs (struct sprite_entry *e)
{
    if (e->has_attached)
	if (bplres == 1)
	    if (dp_for_drawing->ham_seen)
		draw_sprites_ham_sp_hi_at (e);
	    else
		if (bpldualpf)
		    draw_sprites_normal_dp_hi_at (e);
		else
		    draw_sprites_normal_sp_hi_at (e);
	else
	    if (dp_for_drawing->ham_seen)
		draw_sprites_ham_sp_lo_at (e);
	    else
		if (bpldualpf)
		    draw_sprites_normal_dp_lo_at (e);
		else
		    draw_sprites_normal_sp_lo_at (e);
    else
	if (bplres == 1)
	    if (dp_for_drawing->ham_seen)
		draw_sprites_ham_sp_hi_nat (e);
	    else
		if (bpldualpf)
		    draw_sprites_normal_dp_hi_nat (e);
		else
		    draw_sprites_normal_sp_hi_nat (e);
	else
	    if (dp_for_drawing->ham_seen)
		draw_sprites_ham_sp_lo_nat (e);
	    else
		if (bpldualpf)
		    draw_sprites_normal_dp_lo_nat (e);
		else
		    draw_sprites_normal_sp_lo_nat (e);
}

#ifdef AGA
/* clear possible bitplane data outside DIW area */
/****i* januae-drawing/clear_bitplane_border_aga
*
*   NAME
*       clear_bitplane_border_aga() -- Clear excess bitplane indices behind border sprites.
*
*   SYNOPSIS
*       static void clear_bitplane_border_aga (void);
*
*   FUNCTION
*       Zero the decoded pixel-buffer intervals outside the real playfield but
*       inside the expanded sprite border. Available only when AGA support is
*       compiled.
*
***************************************************************************
*
*/

static void clear_bitplane_border_aga (void)
{
    int len, shift = lores_shift - bplres;
    uae_u8 v = 0;
    if (shift < 0) {
	shift = -shift;
	len = (real_playfield_start - playfield_start) << shift;
	memset (pixdata.apixels + pixels_offset + (playfield_start << shift), v, len);
	len = (playfield_end - real_playfield_end) << shift;
	memset (pixdata.apixels + pixels_offset + (real_playfield_end << shift), v, len);
    } else {
	len = (real_playfield_start - playfield_start) >> shift;
	memset (pixdata.apixels + pixels_offset + (playfield_start >> shift), v, len);
	len = (playfield_end - real_playfield_end) >> shift;
	memset (pixdata.apixels + pixels_offset + (real_playfield_end >> shift), v, len);
    }
}
#endif

/* emulate OCS/ECS only undocumented "SWIV" hardware feature */
/****i* januae-drawing/weird_bitplane_fix
*
*   NAME
*       weird_bitplane_fix() -- Apply an OCS/ECS special playfield masking behavior.
*
*   SYNOPSIS
*       static void weird_bitplane_fix (void);
*
*   FUNCTION
*       Clamp certain decoded playfield pixel indices above sixteen to sixteen
*       over the current visible fetch region; this is used by a specific
*       non-AGA priority configuration.
*
***************************************************************************
*
*/

static void weird_bitplane_fix (void)
{
    int i, shift = lores_shift - bplres;

    if (shift < 0) {
	shift = -shift;
	for (i = playfield_start << lores_shift; i < playfield_end << lores_shift; i++) {
	    if (pixdata.apixels[pixels_offset + i] > 16)
		pixdata.apixels[pixels_offset + i] = 16;
	}
    } else {
	for (i = playfield_start >> lores_shift; i < playfield_end >> lores_shift; i++) {
	    if (pixdata.apixels[pixels_offset + i] > 16)
		pixdata.apixels[pixels_offset + i] = 16;
	}
    }
}

#define MERGE(a,b,mask,shift) do {\
    uae_u32 tmp = mask & (a ^ (b >> shift)); \
    a ^= tmp; \
    b ^= (tmp << shift); \
} while (0)

#define GETLONG(P) (*(uae_u32 *)P)

/* We use the compiler's inlining ability to ensure that PLANES is in effect a compile time
   constant.  That will cause some unnecessary code to be optimized away.
   Don't touch this if you don't know what you are doing.  */
/****i* januae-drawing/pfield_doline_1
*
*   NAME
*       pfield_doline_1() -- Transpose planar source words into chunky pixel indices.
*
*   SYNOPSIS
*       STATIC_INLINE void pfield_doline_1 (uae_u32 *pixels, int wordcount, int
*       planes);
*
*   FUNCTION
*       Read the requested number of 32-pixel words from one to eight bitplane
*       streams, interleave their bits and advance their source pointers. Planes
*       seven and eight require AGA support.
*
*   INPUTS
*       pixels - Writable chunky-pixel destination buffer.
*       wordcount - Number of source words per bitplane to convert.
*       planes - Number of bitplanes to interleave.
*
***************************************************************************
*
*/

STATIC_INLINE void pfield_doline_1 (uae_u32 *pixels, int wordcount, int planes)
{
    while (wordcount-- > 0) {
	uae_u32 b0, b1, b2, b3, b4, b5, b6, b7;

	b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, b7 = 0;
	switch (planes) {
#ifdef AGA
	case 8: b0 = GETLONG ((uae_u32 *)real_bplpt[7]); real_bplpt[7] += 4;
	case 7: b1 = GETLONG ((uae_u32 *)real_bplpt[6]); real_bplpt[6] += 4;
#endif
	case 6: b2 = GETLONG ((uae_u32 *)real_bplpt[5]); real_bplpt[5] += 4;
	case 5: b3 = GETLONG ((uae_u32 *)real_bplpt[4]); real_bplpt[4] += 4;
	case 4: b4 = GETLONG ((uae_u32 *)real_bplpt[3]); real_bplpt[3] += 4;
	case 3: b5 = GETLONG ((uae_u32 *)real_bplpt[2]); real_bplpt[2] += 4;
	case 2: b6 = GETLONG ((uae_u32 *)real_bplpt[1]); real_bplpt[1] += 4;
	case 1: b7 = GETLONG ((uae_u32 *)real_bplpt[0]); real_bplpt[0] += 4;
	}

	MERGE (b0, b1, 0x55555555, 1);
	MERGE (b2, b3, 0x55555555, 1);
	MERGE (b4, b5, 0x55555555, 1);
	MERGE (b6, b7, 0x55555555, 1);

	MERGE (b0, b2, 0x33333333, 2);
	MERGE (b1, b3, 0x33333333, 2);
	MERGE (b4, b6, 0x33333333, 2);
	MERGE (b5, b7, 0x33333333, 2);

	MERGE (b0, b4, 0x0f0f0f0f, 4);
	MERGE (b1, b5, 0x0f0f0f0f, 4);
	MERGE (b2, b6, 0x0f0f0f0f, 4);
	MERGE (b3, b7, 0x0f0f0f0f, 4);

	MERGE (b0, b1, 0x00ff00ff, 8);
	MERGE (b2, b3, 0x00ff00ff, 8);
	MERGE (b4, b5, 0x00ff00ff, 8);
	MERGE (b6, b7, 0x00ff00ff, 8);

	MERGE (b0, b2, 0x0000ffff, 16);
	do_put_mem_long (pixels, b0);
	do_put_mem_long (pixels + 4, b2);
	MERGE (b1, b3, 0x0000ffff, 16);
	do_put_mem_long (pixels + 2, b1);
	do_put_mem_long (pixels + 6, b3);
	MERGE (b4, b6, 0x0000ffff, 16);
	do_put_mem_long (pixels + 1, b4);
	do_put_mem_long (pixels + 5, b6);
	MERGE (b5, b7, 0x0000ffff, 16);
	do_put_mem_long (pixels + 3, b5);
	do_put_mem_long (pixels + 7, b7);
	pixels += 8;
    }
}

/* See above for comments on inlining.  These functions should _not_
   be inlined themselves.  */
/****i* januae-drawing/pfield_doline_n1
*
*   NAME
*       pfield_doline_n1() -- Convert 1-plane captured bitplane words.
*
*   SYNOPSIS
*       static void NOINLINE pfield_doline_n1 (uae_u32 *data, int count);
*
*   FUNCTION
*       Call the planar-to-chunky converter with a fixed 1-plane count for the
*       requested word span. Advances the selected source-plane pointers.
*
*   INPUTS
*       data - Writable chunky-pixel destination buffer.
*       count - Number of 32-pixel bitplane word groups.
*
***************************************************************************
*
*/

static void NOINLINE pfield_doline_n1 (uae_u32 *data, int count) { pfield_doline_1 (data, count, 1); }
/****i* januae-drawing/pfield_doline_n2
*
*   NAME
*       pfield_doline_n2() -- Convert 2-plane captured bitplane words.
*
*   SYNOPSIS
*       static void NOINLINE pfield_doline_n2 (uae_u32 *data, int count);
*
*   FUNCTION
*       Call the planar-to-chunky converter with a fixed 2-plane count for the
*       requested word span. Advances the selected source-plane pointers.
*
*   INPUTS
*       data - Writable chunky-pixel destination buffer.
*       count - Number of 32-pixel bitplane word groups.
*
***************************************************************************
*
*/

static void NOINLINE pfield_doline_n2 (uae_u32 *data, int count) { pfield_doline_1 (data, count, 2); }
/****i* januae-drawing/pfield_doline_n3
*
*   NAME
*       pfield_doline_n3() -- Convert 3-plane captured bitplane words.
*
*   SYNOPSIS
*       static void NOINLINE pfield_doline_n3 (uae_u32 *data, int count);
*
*   FUNCTION
*       Call the planar-to-chunky converter with a fixed 3-plane count for the
*       requested word span. Advances the selected source-plane pointers.
*
*   INPUTS
*       data - Writable chunky-pixel destination buffer.
*       count - Number of 32-pixel bitplane word groups.
*
***************************************************************************
*
*/

static void NOINLINE pfield_doline_n3 (uae_u32 *data, int count) { pfield_doline_1 (data, count, 3); }
/****i* januae-drawing/pfield_doline_n4
*
*   NAME
*       pfield_doline_n4() -- Convert 4-plane captured bitplane words.
*
*   SYNOPSIS
*       static void NOINLINE pfield_doline_n4 (uae_u32 *data, int count);
*
*   FUNCTION
*       Call the planar-to-chunky converter with a fixed 4-plane count for the
*       requested word span. Advances the selected source-plane pointers.
*
*   INPUTS
*       data - Writable chunky-pixel destination buffer.
*       count - Number of 32-pixel bitplane word groups.
*
***************************************************************************
*
*/

static void NOINLINE pfield_doline_n4 (uae_u32 *data, int count) { pfield_doline_1 (data, count, 4); }
/****i* januae-drawing/pfield_doline_n5
*
*   NAME
*       pfield_doline_n5() -- Convert 5-plane captured bitplane words.
*
*   SYNOPSIS
*       static void NOINLINE pfield_doline_n5 (uae_u32 *data, int count);
*
*   FUNCTION
*       Call the planar-to-chunky converter with a fixed 5-plane count for the
*       requested word span. Advances the selected source-plane pointers.
*
*   INPUTS
*       data - Writable chunky-pixel destination buffer.
*       count - Number of 32-pixel bitplane word groups.
*
***************************************************************************
*
*/

static void NOINLINE pfield_doline_n5 (uae_u32 *data, int count) { pfield_doline_1 (data, count, 5); }
/****i* januae-drawing/pfield_doline_n6
*
*   NAME
*       pfield_doline_n6() -- Convert 6-plane captured bitplane words.
*
*   SYNOPSIS
*       static void NOINLINE pfield_doline_n6 (uae_u32 *data, int count);
*
*   FUNCTION
*       Call the planar-to-chunky converter with a fixed 6-plane count for the
*       requested word span. Advances the selected source-plane pointers.
*
*   INPUTS
*       data - Writable chunky-pixel destination buffer.
*       count - Number of 32-pixel bitplane word groups.
*
***************************************************************************
*
*/

static void NOINLINE pfield_doline_n6 (uae_u32 *data, int count) { pfield_doline_1 (data, count, 6); }
#ifdef AGA
/****i* januae-drawing/pfield_doline_n7
*
*   NAME
*       pfield_doline_n7() -- Convert 7-plane captured bitplane words.
*
*   SYNOPSIS
*       static void NOINLINE pfield_doline_n7 (uae_u32 *data, int count);
*
*   FUNCTION
*       Call the planar-to-chunky converter with a fixed 7-plane count for the
*       requested word span. Requires AGA compilation.
*
*   INPUTS
*       data - Writable chunky-pixel destination buffer.
*       count - Number of 32-pixel bitplane word groups.
*
***************************************************************************
*
*/

static void NOINLINE pfield_doline_n7 (uae_u32 *data, int count) { pfield_doline_1 (data, count, 7); }
/****i* januae-drawing/pfield_doline_n8
*
*   NAME
*       pfield_doline_n8() -- Convert 8-plane captured bitplane words.
*
*   SYNOPSIS
*       static void NOINLINE pfield_doline_n8 (uae_u32 *data, int count);
*
*   FUNCTION
*       Call the planar-to-chunky converter with a fixed 8-plane count for the
*       requested word span. Requires AGA compilation.
*
*   INPUTS
*       data - Writable chunky-pixel destination buffer.
*       count - Number of 32-pixel bitplane word groups.
*
***************************************************************************
*
*/

static void NOINLINE pfield_doline_n8 (uae_u32 *data, int count) { pfield_doline_1 (data, count, 8); }
#endif

/****i* januae-drawing/pfield_doline
*
*   NAME
*       pfield_doline() -- Convert one captured bitplane line into pixel indices.
*
*   SYNOPSIS
*       static void pfield_doline (int lineno);
*
*   FUNCTION
*       Select a specialized planar-to-chunky routine for the current plane
*       count and draw length, clearing the destination for zero planes. Under
*       SMART_UPDATE, source pointers reference the recorded line data.
*
*   INPUTS
*       lineno - Emulated or native line number, as expected by the function.
*
***************************************************************************
*
*/

static void pfield_doline (int lineno)
{
    int wordcount = dp_for_drawing->plflinelen;
    uae_u32 *data = pixdata.apixels_l + MAX_PIXELS_PER_LINE/4;

#ifdef SMART_UPDATE
#define DATA_POINTER(n) (line_data[lineno] + (n)*MAX_WORDS_PER_LINE*2)
    real_bplpt[0] = DATA_POINTER (0);
    real_bplpt[1] = DATA_POINTER (1);
    real_bplpt[2] = DATA_POINTER (2);
    real_bplpt[3] = DATA_POINTER (3);
    real_bplpt[4] = DATA_POINTER (4);
    real_bplpt[5] = DATA_POINTER (5);
#ifdef AGA
    real_bplpt[6] = DATA_POINTER (6);
    real_bplpt[7] = DATA_POINTER (7);
#endif
#endif

    switch (bplplanecnt) {
    default: break;
    case 0: memset (data, 0, wordcount * 32); break;
    case 1: pfield_doline_n1 (data, wordcount); break;
    case 2: pfield_doline_n2 (data, wordcount); break;
    case 3: pfield_doline_n3 (data, wordcount); break;
    case 4: pfield_doline_n4 (data, wordcount); break;
    case 5: pfield_doline_n5 (data, wordcount); break;
    case 6: pfield_doline_n6 (data, wordcount); break;
#ifdef AGA
    case 7: pfield_doline_n7 (data, wordcount); break;
    case 8: pfield_doline_n8 (data, wordcount); break;
#endif
    }
}

/****i* januae-drawing/init_row_map
*
*   NAME
*       init_row_map() -- Map native scanlines to frame-buffer row addresses.
*
*   SYNOPSIS
*       void init_row_map (void);
*
*   FUNCTION
*       Populate row_map from the graphics buffer and byte stride, including one
*       trailing row pointer. Abort if the native height exceeds MAX_VIDHEIGHT;
*       a NULL buffer is logged but still used in pointer arithmetic.
*
***************************************************************************
*
*/

void init_row_map (void)
{
    int i, j;
    if (gfxvidinfo.height > MAX_VIDHEIGHT) {
	write_log ("Resolution too high, aborting\n");
	abort ();
    }

    if(!gfxvidinfo.bufmem) {
	write_log ("WARNING: init_row_map: gfxvidinfo.bufmem is NULL !? \n");
    }
    j = 0;
    for (i = 0; i < gfxvidinfo.height + 1; i++, j += gfxvidinfo.rowbytes)
	row_map[i] = gfxvidinfo.bufmem + j;
}

/****i* januae-drawing/init_aspect_maps
*
*   NAME
*       init_aspect_maps() -- Rebuild vertical aspect and native scanline maps.
*
*   SYNOPSIS
*       void init_aspect_maps (void);
*
*   FUNCTION
*       Allocate forward and reverse line maps for current height, aspect
*       correction and line doubling; mark skipped or offscreen positions with
*       minus one. Returns without changing maps if screen height is zero.
*       Allocation failures are not checked.
*
***************************************************************************
*
*/

void init_aspect_maps (void)
{
    int i, maxl;
    double native_lines_per_amiga_line;

    if (gfxvidinfo.height == 0)
	/* Do nothing if the gfx driver hasn't initialized the screen yet */
	return;

    if (native2amiga_line_map)
	free (native2amiga_line_map);
    if (amiga2aspect_line_map)
	free (amiga2aspect_line_map);

    /* At least for this array the +1 is necessary. */
    amiga2aspect_line_map = (int *)malloc (sizeof (int) * (MAXVPOS + 1) * 2 + 1);
    native2amiga_line_map = (int *)malloc (sizeof (int) * gfxvidinfo.height);

    if (currprefs.gfx_correct_aspect)
	native_lines_per_amiga_line = ((double)gfxvidinfo.height
				       * (currprefs.gfx_lores ? 320 : 640)
				       / (currprefs.gfx_linedbl ? 512 : 256)
				       / gfxvidinfo.width);
    else
	native_lines_per_amiga_line = 1;

    maxl = (MAXVPOS + 1) * (currprefs.gfx_linedbl ? 2 : 1);
    min_ypos_for_screen = minfirstline << (currprefs.gfx_linedbl ? 1 : 0);
    max_drawn_amiga_line = -1;
    for (i = 0; i < maxl; i++) {
	int v = (int) ((i - min_ypos_for_screen) * native_lines_per_amiga_line);
	if (v >= gfxvidinfo.height && max_drawn_amiga_line == -1)
	    max_drawn_amiga_line = i - min_ypos_for_screen;
	if (i < min_ypos_for_screen || v >= gfxvidinfo.height)
	    v = -1;
	amiga2aspect_line_map[i] = v;
    }
    if (currprefs.gfx_linedbl)
	max_drawn_amiga_line >>= 1;

    if (currprefs.gfx_ycenter && !(currprefs.gfx_correct_aspect)) {
	/* @@@ verify maxvpos vs. MAXVPOS */
	extra_y_adjust = (gfxvidinfo.height - (maxvpos << (currprefs.gfx_linedbl ? 1 : 0))) >> 1;
	if (extra_y_adjust < 0)
	    extra_y_adjust = 0;
    }

    for (i = 0; i < gfxvidinfo.height; i++)
	native2amiga_line_map[i] = -1;

    if (native_lines_per_amiga_line < 1) {
	/* Must omit drawing some lines. */
	for (i = maxl - 1; i > min_ypos_for_screen; i--) {
	    if (amiga2aspect_line_map[i] == amiga2aspect_line_map[i-1]) {
		if (currprefs.gfx_linedbl && (i & 1) == 0 && amiga2aspect_line_map[i+1] != -1) {
		    /* If only the first line of a line pair would be omitted,
		     * omit the second one instead to avoid problems with line
		     * doubling. */
		    amiga2aspect_line_map[i] = amiga2aspect_line_map[i+1];
		    amiga2aspect_line_map[i+1] = -1;
		} else
		    amiga2aspect_line_map[i] = -1;
	    }
	}
    }

    for (i = maxl-1; i >= min_ypos_for_screen; i--) {
	int j;
	if (amiga2aspect_line_map[i] == -1)
	    continue;
	for (j = amiga2aspect_line_map[i]; j < gfxvidinfo.height && native2amiga_line_map[j] == -1; j++)
	    native2amiga_line_map[j] = i >> (currprefs.gfx_linedbl ? 1 : 0);
    }
}

/*
 * A raster line has been built in the graphics buffer. Tell the graphics code
 * to do anything necessary to display it.
 */
/****i* januae-drawing/do_flush_line_1
*
*   NAME
*       do_flush_line_1() -- Queue or flush one native scanline.
*
*   SYNOPSIS
*       static void do_flush_line_1 (int lineno);
*
*   FUNCTION
*       Update the earliest and latest drawn-line bounds, then flush the line
*       immediately or aggregate adjacent lines into a graphics backend block
*       according to maxblocklines.
*
*   INPUTS
*       lineno - Emulated or native line number, as expected by the function.
*
***************************************************************************
*
*/

static void do_flush_line_1 (int lineno) {

    if (lineno < first_drawn_line)
	first_drawn_line = lineno;
    if (lineno > last_drawn_line)
	last_drawn_line = lineno;

    if (gfxvidinfo.maxblocklines == 0)
	flush_line (lineno);
    else {
	if ((last_block_line + 2) < lineno) {
	    if (first_block_line != NO_BLOCK)
		flush_block (first_block_line, last_block_line);
	    first_block_line = lineno;
	}
	last_block_line = lineno;
	if (last_block_line - first_block_line >= gfxvidinfo.maxblocklines) {
	    flush_block (first_block_line, last_block_line);
	    first_block_line = last_block_line = NO_BLOCK;
	}
    }
}

/****i* januae-drawing/do_flush_line
*
*   NAME
*       do_flush_line() -- Flush one native scanline through the block controller.
*
*   SYNOPSIS
*       STATIC_INLINE void do_flush_line (int lineno);
*
*   FUNCTION
*       Forward the native scanline number to do_flush_line_1 for tracking and
*       submission.
*
*   INPUTS
*       lineno - Emulated or native line number, as expected by the function.
*
***************************************************************************
*
*/

STATIC_INLINE void do_flush_line (int lineno) {

  //JWLOG("lineno: %d\n",lineno);

  do_flush_line_1 (lineno);

}

/*
 * One drawing frame has been finished. Tell the graphics code about it.
 * Note that the actual flush_screen() call is a no-op for all reasonable
 * systems.
 */

/****i* januae-drawing/do_flush_screen
*
*   NAME
*       do_flush_screen() -- Submit accumulated screen updates and release the lock.
*
*   SYNOPSIS
*       STATIC_INLINE void do_flush_screen (int start, int stop);
*
*   FUNCTION
*       Flush a pending line block, unlock the display, then issue a visible
*       range update or a synchronization-only flush when vertical
*       synchronization is active.
*
*   INPUTS
*       start - Inclusive first destination pixel or window coordinate.
*       stop - Exclusive last destination pixel or window coordinate.
*
***************************************************************************
*
*/

STATIC_INLINE void do_flush_screen (int start, int stop)
{
    /* TODO: this flush operation is executed outside locked state!
       Should be corrected.
       (sjo 26.9.99) */

    if (gfxvidinfo.maxblocklines != 0 && first_block_line != NO_BLOCK) {
	flush_block (first_block_line, last_block_line);
    }
    unlockscr ();
    if (start <= stop)
	flush_screen (start, stop);
    else if (is_vsync ())
	flush_screen (0, 0); /* vsync mode */
}

static int drawing_color_matches;
static enum { color_match_acolors, color_match_full } color_match_type;

/* Set up colors_for_drawing to the state at the beginning of the currently drawn
   line.  Try to avoid copying color tables around whenever possible.  */
/****i* januae-drawing/adjust_drawing_colors
*
*   NAME
*       adjust_drawing_colors() -- Select the color table for the current draw line.
*
*   SYNOPSIS
*       static void adjust_drawing_colors (int ctable, int need_full);
*
*   FUNCTION
*       Reuse or copy a cached color table. Copy only host pixel colors when
*       full hardware registers are not needed; upgrade to a full table on
*       demand.
*
*   INPUTS
*       ctable - Index of the captured color table.
*       need_full - Nonzero requests complete hardware color-register state.
*
***************************************************************************
*
*/

static void adjust_drawing_colors (int ctable, int need_full)
{
    if (drawing_color_matches != ctable) {
	if (need_full) {
	    color_reg_cpy (&colors_for_drawing, curr_color_tables + ctable);
	    color_match_type = color_match_full;
	} else {
	    memcpy (colors_for_drawing.acolors, curr_color_tables[ctable].acolors,
		sizeof colors_for_drawing.acolors);
	    color_match_type = color_match_acolors;
	}
	drawing_color_matches = ctable;
    } else if (need_full && color_match_type != color_match_full) {
	color_reg_cpy (&colors_for_drawing, &curr_color_tables[ctable]);
	color_match_type = color_match_full;
    }
}

/* We only save hardware registers during the hardware frame. Now, when
 * drawing the frame, we expand the data into a slightly more useful
 * form. */
/****i* januae-drawing/pfield_expand_dp_bplcon
*
*   NAME
*       pfield_expand_dp_bplcon() -- Derive effective display mode from captured control registers.
*
*   SYNOPSIS
*       static void pfield_expand_dp_bplcon (void);
*
*   FUNCTION
*       Update bitplane resolution, plane count, HAM/EHB modes, dual-playfield
*       priority and sprite masking. When compiled for AGA, include additional
*       color, border and sprite controls.
*
***************************************************************************
*
*/

static void pfield_expand_dp_bplcon (void)
{
    bplres = dp_for_drawing->bplres;
    bplplanecnt = dp_for_drawing->nr_planes;
    bplham = dp_for_drawing->ham_seen;

    if (bplres > 0)
	can_use_lores = 0;
    if (currprefs.chipset_mask & CSMASK_AGA) {
	/* The KILLEHB bit exists in ECS, but is apparently meant for Genlock
	 * stuff, and it's set by some demos (e.g. Andromeda Seven Seas) */
	bplehb = ((dp_for_drawing->bplcon0 & 0x7010) == 0x6000 && !(dp_for_drawing->bplcon2 & 0x200));
    } else {
	bplehb = (dp_for_drawing->bplcon0 & 0xFC00) == 0x6000;
    }
    plf1pri = dp_for_drawing->bplcon2 & 7;
    plf2pri = (dp_for_drawing->bplcon2 >> 3) & 7;
    plf_sprite_mask = 0xFFFF0000 << (4 * plf2pri);
    plf_sprite_mask |= (0xFFFF << (4 * plf1pri)) & 0xFFFF;
    bpldualpf = (dp_for_drawing->bplcon0 & 0x400) == 0x400;
    bpldualpfpri = (dp_for_drawing->bplcon2 & 0x40) == 0x40;
#ifdef AGA
    bpldualpf2of = (dp_for_drawing->bplcon3 >> 10) & 7;
    sbasecol[0] = ((dp_for_drawing->bplcon4 >> 4) & 15) << 4;
    sbasecol[1] = ((dp_for_drawing->bplcon4 >> 0) & 15) << 4;

    brdsprt = (currprefs.chipset_mask & CSMASK_AGA) && (dp_for_drawing->bplcon0 & 1) && (dp_for_drawing->bplcon3 & 0x02);
    /* FIXME: we must update top and bottom borders when BRDBLANK changes */
    brdblank = (currprefs.chipset_mask & CSMASK_ECS_DENISE) && (dp_for_drawing->bplcon0 & 1) && (dp_for_drawing->bplcon3 & 0x20);
    if (brdblank)
	brdsprt = 0;
#endif
}
/****i* januae-drawing/pfield_expand_dp_bplcon2
*
*   NAME
*       pfield_expand_dp_bplcon2() -- Replay a captured bitplane-control register write.
*
*   SYNOPSIS
*       static void pfield_expand_dp_bplcon2 (int regno, int v);
*
*   FUNCTION
*       Apply a supported BPLCON0/BPLCON2 write, or BPLCON3/BPLCON4 with AGA
*       enabled, then recompute effective playfield mode and resolution shift.
*       Unsupported register numbers leave the captured fields unchanged.
*
*   INPUTS
*       regno - Encoded bitplane-control register number.
*       v - New bitplane-control register value.
*
***************************************************************************
*
*/

static void pfield_expand_dp_bplcon2 (int regno, int v)
{
    regno -= 0x1000;
    switch (regno)
    {
        case 0x100:
        dp_for_drawing->bplcon0 = v;
        dp_for_drawing->bplres = GET_RES(v);
        dp_for_drawing->nr_planes = GET_PLANES(v);
	dp_for_drawing->ham_seen = !! (v & 0x800);
        break;
        case 0x104:
        dp_for_drawing->bplcon2 = v;
        break;
#ifdef AGA
	case 0x106:
        dp_for_drawing->bplcon3 = v;
        break;
        case 0x108:
        dp_for_drawing->bplcon4 = v;
        break;
#endif
    }
    pfield_expand_dp_bplcon ();
    res_shift = lores_shift - bplres;
}

/****i* januae-drawing/do_color_changes
*
*   NAME
*       do_color_changes() -- Render a scanline while replaying raster register changes.
*
*   SYNOPSIS
*       STATIC_INLINE void do_color_changes (line_draw_func worker_border,
*       line_draw_func worker_pfield);
*
*   FUNCTION
*       Partition the visible interval at recorded color-change positions,
*       dispatch border or playfield workers, and update palette or BPLCON state
*       for the following interval.
*
*   INPUTS
*       worker_border - Callback to render border intervals.
*       worker_pfield - Callback to render playfield intervals.
*
***************************************************************************
*
*/

STATIC_INLINE void do_color_changes (line_draw_func worker_border, line_draw_func worker_pfield)
{
    int i;
    int lastpos = visible_left_border;
    int endpos = visible_left_border + gfxvidinfo.width;

    for (i = dip_for_drawing->first_color_change; i <= dip_for_drawing->last_color_change; i++) {
	int regno = curr_color_changes[i].regno;
	unsigned int value = curr_color_changes[i].value;
	int nextpos, nextpos_in_range;
	if (i == dip_for_drawing->last_color_change)
	    nextpos = endpos;
	else
	    nextpos = coord_hw_to_window_x (curr_color_changes[i].linepos * 2);

	nextpos_in_range = nextpos;
	if (nextpos > endpos)
	    nextpos_in_range = endpos;

	if (nextpos_in_range > lastpos) {
	    if (lastpos < playfield_start) {
		int t = nextpos_in_range <= playfield_start ? nextpos_in_range : playfield_start;
		(*worker_border) (lastpos, t);
		lastpos = t;
	    }
	}
	if (nextpos_in_range > lastpos) {
	    if (lastpos >= playfield_start && lastpos < playfield_end) {
		int t = nextpos_in_range <= playfield_end ? nextpos_in_range : playfield_end;
		(*worker_pfield) (lastpos, t);
		lastpos = t;
	    }
	}
	if (nextpos_in_range > lastpos) {
	    if (lastpos >= playfield_end)
		(*worker_border) (lastpos, nextpos_in_range);
	    lastpos = nextpos_in_range;
	}
	if (i != dip_for_drawing->last_color_change) {
	    if (regno >= 0x1000) {
		pfield_expand_dp_bplcon2 (regno, value);
	    } else {
		color_reg_set (&colors_for_drawing, regno, value);
		colors_for_drawing.acolors[regno] = getxcolor (value);
	    }
	}
	if (lastpos >= endpos)
	    break;
    }
}

/* move color changes in horizontal cycles 0 to HBLANK_OFFSET to previous line
 * cycles 0 to HBLANK_OFFSET must be visible in right border
 */
/****i* januae-drawing/mungedip
*
*   NAME
*       mungedip() -- Move early horizontal blank changes into the preceding line.
*
*   SYNOPSIS
*       static void mungedip (int lineno);
*
*   FUNCTION
*       Advance the last-color-change boundary to absorb events before
*       HBLANK_OFFSET from the next draw-info record, updating both records'
*       counts and first-entry indices.
*
*   INPUTS
*       lineno - Emulated or native line number, as expected by the function.
*
***************************************************************************
*
*/

static void mungedip (int lineno)
{
    int i = dip_for_drawing->last_color_change;
    struct draw_info *dip_for_drawing_next = curr_drawinfo + (lineno + 1);
    if (dip_for_drawing_next->first_color_change == 0)
	dip_for_drawing_next = curr_drawinfo + (lineno + 2);
    while (i < dip_for_drawing_next->last_color_change) {
	int regno = curr_color_changes[i].regno;
	int hpos = curr_color_changes[i].linepos;
	if (regno < 0)
	    break;
	if (hpos >= HBLANK_OFFSET)
	    break;
	curr_color_changes[i].linepos += maxhpos + 2;
	dip_for_drawing->last_color_change++;
	dip_for_drawing->nr_color_changes++;
	dip_for_drawing_next->first_color_change++;
	dip_for_drawing_next->nr_color_changes--;
	i++;
    }
}

enum double_how {
    dh_buf,
    dh_line,
    dh_emerg
};

/****i* januae-drawing/pfield_draw_line
*
*   NAME
*       pfield_draw_line() -- Compose and submit one emulated video line.
*
*   SYNOPSIS
*       STATIC_INLINE void pfield_draw_line (int lineno, int gfx_ypos, int
*       follow_ypos);
*
*   FUNCTION
*       Respect remembered, black, doubled and decided line states; convert
*       bitplanes and HAM, composite sprites, apply color changes and submit the
*       resulting native line or pair.
*
*   INPUTS
*       lineno - Emulated or native line number, as expected by the function.
*       gfx_ypos - Destination native scanline.
*       follow_ypos - Optional doubled native line, or -1 for none.
*
***************************************************************************
*
*/

STATIC_INLINE void pfield_draw_line (int lineno, int gfx_ypos, int follow_ypos)
{
    static int warned = 0;
    int border = 0;
    int do_double = 0;
    enum double_how dh;

    dp_for_drawing = line_decisions + lineno;
    dip_for_drawing = curr_drawinfo + lineno;
    mungedip (lineno);
    switch (linestate[lineno]) {
    case LINE_REMEMBERED_AS_PREVIOUS:
	if (!warned)
	    write_log ("Shouldn't get here... this is a bug.\n"), warned++;
	return;

    case LINE_BLACK:
	linestate[lineno] = LINE_REMEMBERED_AS_BLACK;
	border = 2;
	break;

    case LINE_REMEMBERED_AS_BLACK:
	return;

    case LINE_AS_PREVIOUS:
	dp_for_drawing--;
	dip_for_drawing--;
	linestate[lineno] = LINE_DONE_AS_PREVIOUS;
	if (!dp_for_drawing->valid)
	    return;
	if (dp_for_drawing->plfleft == -1)
	    border = 1;
	break;

    case LINE_DONE_AS_PREVIOUS:
	/* fall through */
    case LINE_DONE:
	return;

    case LINE_DECIDED_DOUBLE:
	if (follow_ypos != -1) {
	    do_double = 1;
	    linestate[lineno + 1] = LINE_DONE_AS_PREVIOUS;
	}

	/* fall through */
    default:
	if (dp_for_drawing->plfleft == -1)
	    border = 1;
	linestate[lineno] = LINE_DONE;
	break;
    }

    dh = dh_line;
    xlinebuffer = gfxvidinfo.linemem;
    if (xlinebuffer == 0 && do_double
	&& (border == 0 || (border != 1 && dip_for_drawing->nr_color_changes > 0)))
	xlinebuffer = gfxvidinfo.emergmem, dh = dh_emerg;
    if (xlinebuffer == 0)
	xlinebuffer = row_map[gfx_ypos], dh = dh_buf;
    xlinebuffer -= linetoscr_x_adjust_bytes;

    if (border == 0) {

	pfield_expand_dp_bplcon ();

	if (bplres == RES_LORES && ! currprefs.gfx_lores)
	    currprefs.gfx_lores = 2;

	pfield_init_linetoscr ();
	pfield_doline (lineno);

	adjust_drawing_colors (dp_for_drawing->ctable, dp_for_drawing->ham_seen || bplehb);

	/* The problem is that we must call decode_ham() BEFORE we do the
	   sprites. */
	if (! border && dp_for_drawing->ham_seen) {
	    init_ham_decoding ();
	    if (dip_for_drawing->nr_color_changes == 0) {
		/* The easy case: need to do HAM decoding only once for the
		 * full line. */
		decode_ham (visible_left_border, visible_right_border);
	    } else /* Argh. */ {
		do_color_changes (dummy_worker, decode_ham);
		adjust_drawing_colors (dp_for_drawing->ctable, dp_for_drawing->ham_seen || bplehb);
	    }
	    bplham = dp_for_drawing->ham_at_start;
	}
	if (plf2pri > 5 && bplplanecnt == 5 && !(currprefs.chipset_mask & CSMASK_AGA))
	    weird_bitplane_fix ();

	{
	    if (dip_for_drawing->nr_sprites) {
		int i;
#ifdef AGA
		if (brdsprt)
		    clear_bitplane_border_aga ();
#endif
		for (i = 0; i < dip_for_drawing->nr_sprites; i++) {
#ifdef AGA
		    if (currprefs.chipset_mask & CSMASK_AGA)
			draw_sprites_aga (curr_sprite_entries + dip_for_drawing->first_sprite_entry + i);
		    else
#endif
			draw_sprites_ecs (curr_sprite_entries + dip_for_drawing->first_sprite_entry + i);
		}
	    }
	}

	do_color_changes (pfield_do_fill_line, pfield_do_linetoscr);
	if (dh == dh_emerg)
	    memcpy (row_map[gfx_ypos], xlinebuffer + linetoscr_x_adjust_bytes, gfxvidinfo.pixbytes * gfxvidinfo.width);

	do_flush_line (gfx_ypos);
	if (do_double) {
	    if (dh == dh_emerg)
		memcpy (row_map[follow_ypos], xlinebuffer + linetoscr_x_adjust_bytes, gfxvidinfo.pixbytes * gfxvidinfo.width);
	    else if (dh == dh_buf)
		memcpy (row_map[follow_ypos], row_map[gfx_ypos], gfxvidinfo.pixbytes * gfxvidinfo.width);
	    do_flush_line (follow_ypos);
	}
	if (currprefs.gfx_lores == 2)
	    currprefs.gfx_lores = 0;

    } else if (border == 1) {

	adjust_drawing_colors (dp_for_drawing->ctable, 0);

	if (dip_for_drawing->nr_color_changes == 0) {
	    fill_line ();
	    do_flush_line (gfx_ypos);

	    if (do_double) {
		if (dh == dh_buf) {
		    xlinebuffer = row_map[follow_ypos] - linetoscr_x_adjust_bytes;
		    fill_line ();
		}
		/* If dh == dh_line, do_flush_line will re-use the rendered line
		 * from linemem.  */
		do_flush_line (follow_ypos);
	    }
	    return;
	}

	playfield_start = visible_right_border;
	playfield_end = visible_right_border;

	do_color_changes (pfield_do_fill_line, pfield_do_fill_line);

	if (dh == dh_emerg)
	    memcpy (row_map[gfx_ypos], xlinebuffer + linetoscr_x_adjust_bytes, gfxvidinfo.pixbytes * gfxvidinfo.width);

	do_flush_line (gfx_ypos);
	if (do_double) {
	    if (dh == dh_emerg)
		memcpy (row_map[follow_ypos], xlinebuffer + linetoscr_x_adjust_bytes, gfxvidinfo.pixbytes * gfxvidinfo.width);
	    else if (dh == dh_buf)
		memcpy (row_map[follow_ypos], row_map[gfx_ypos], gfxvidinfo.pixbytes * gfxvidinfo.width);
	    /* If dh == dh_line, do_flush_line will re-use the rendered line
	     * from linemem.  */
	    do_flush_line (follow_ypos);
	}

    } else {

	xcolnr tmp = colors_for_drawing.acolors[0];
	colors_for_drawing.acolors[0] = getxcolor (0);
	fill_line ();
	do_flush_line (gfx_ypos);
	colors_for_drawing.acolors[0] = tmp;

    }
}

/****i* januae-drawing/center_image
*
*   NAME
*       center_image() -- Compute horizontal and vertical viewport placement.
*
*   SYNOPSIS
*       static void center_image (void);
*
*   FUNCTION
*       Adjust visible borders, native buffer offsets and frame vertical
*       position from the observed display window and centering preferences.
*       Request redraw when placement changes.
*
***************************************************************************
*
*/

static void center_image (void)
{
    int prev_x_adjust = visible_left_border;
    int prev_y_adjust = thisframe_y_adjust;

    //JWLOG("CMOUSE: currprefs.gfx_xcenter: %d\n",currprefs.gfx_xcenter);
    if (currprefs.gfx_xcenter) {
	int w = gfxvidinfo.width;
	//JWLOG("CMOUSE: min_diwstart: %d\n",min_diwstart);

	if (max_diwstop - min_diwstart < w && currprefs.gfx_xcenter == 2)
	    /* Try to center. */
	    visible_left_border = (max_diwstop - min_diwstart - w) / 2 + min_diwstart;
	else
	    visible_left_border = max_diwstop - w - (max_diwstop - min_diwstart - w) / 2;
	visible_left_border &= ~((1 << lores_shift) - 1);

	/* Would the old value be good enough? If so, leave it as it is if we want to
	 * be clever. */
	if (currprefs.gfx_xcenter == 2) {
	    if (visible_left_border < prev_x_adjust && prev_x_adjust < min_diwstart && min_diwstart - visible_left_border <= 32)
		visible_left_border = prev_x_adjust;
	}
    } else {
	/* gfx_center_horizontal=none */
	visible_left_border = max_diwlastword - gfxvidinfo.width;
	//JWLOG("CMOUSE: max_diwlastword: %d\n",max_diwlastword);
    }

    if (visible_left_border > max_diwlastword - 32)
	visible_left_border = max_diwlastword - 32;
    if (visible_left_border < 0)
	visible_left_border = 0;

    linetoscr_x_adjust_bytes = visible_left_border * gfxvidinfo.pixbytes;

    visible_right_border = visible_left_border + gfxvidinfo.width;
    if (visible_right_border > max_diwlastword)
	visible_right_border = max_diwlastword;

    thisframe_y_adjust = minfirstline;
    if (currprefs.gfx_ycenter && thisframe_first_drawn_line != -1) {

	if (thisframe_last_drawn_line - thisframe_first_drawn_line < max_drawn_amiga_line && currprefs.gfx_ycenter == 2)
	    thisframe_y_adjust = (thisframe_last_drawn_line - thisframe_first_drawn_line - max_drawn_amiga_line) / 2 + thisframe_first_drawn_line;
	else
	    thisframe_y_adjust = thisframe_first_drawn_line + ((thisframe_last_drawn_line - thisframe_first_drawn_line) - max_drawn_amiga_line) / 2;

	/* Would the old value be good enough? If so, leave it as it is if we want to
	 * be clever. */
	if (currprefs.gfx_ycenter == 2) {
	    if (thisframe_y_adjust != prev_y_adjust
		&& prev_y_adjust <= thisframe_first_drawn_line
		&& prev_y_adjust + max_drawn_amiga_line > thisframe_last_drawn_line)
		thisframe_y_adjust = prev_y_adjust;
	}
	/* Make sure the value makes sense */
	if (thisframe_y_adjust + max_drawn_amiga_line > (int)maxvpos)
	    thisframe_y_adjust = maxvpos - max_drawn_amiga_line;
	if (thisframe_y_adjust < (int)minfirstline)
	    thisframe_y_adjust = minfirstline;
    }
    thisframe_y_adjust_real = thisframe_y_adjust << (currprefs.gfx_linedbl ? 1 : 0);
    max_ypos_thisframe = (maxvpos - thisframe_y_adjust) << (currprefs.gfx_linedbl ? 1 : 0);

    /* @@@ interlace_seen used to be (bplcon0 & 4), but this is probably
     * better.  */
    if (prev_x_adjust != visible_left_border || prev_y_adjust != thisframe_y_adjust)
	frame_redraw_necessary |= interlace_seen && currprefs.gfx_linedbl ? 2 : 1;

#if 0
  JWLOG("CMOUSE: visible_left_border: %d\n",visible_left_border);
  JWLOG("CMOUSE: visible_right_border: %d\n",visible_right_border);
  JWLOG("CMOUSE: thisframe_y_adjust: %d\n",thisframe_y_adjust);
  JWLOG("CMOUSE: thisframe_y_adjust_real: %d\n",thisframe_y_adjust_real);
  JWLOG("CMOUSE: extra_y_adjust: %d\n",extra_y_adjust);
  JWLOG("CMOUSE: visible_right_border: %d\n",visible_right_border);
  JWLOG("CMOUSE: =================================================\n");
#endif

    max_diwstop = 0;
    min_diwstart = 10000;
    //JWLOG("exit\n");
}

/****i* januae-drawing/lores_reset
*
*   NAME
*       lores_reset() -- Set horizontal scaling from low-resolution preferences.
*
*   SYNOPSIS
*       static void lores_reset (void);
*
*   FUNCTION
*       Assign the low-resolution factor and shift according to
*       currprefs.gfx_lores.
*
***************************************************************************
*
*/

static void lores_reset (void)
{
    lores_factor = currprefs.gfx_lores ? 1 : 2;
    lores_shift = currprefs.gfx_lores ? 0 : 1;
}

/****i* januae-drawing/init_drawing_frame
*
*   NAME
*       init_drawing_frame() -- Prepare line states and viewport for a new frame.
*
*   SYNOPSIS
*       static void init_drawing_frame (void);
*
*   FUNCTION
*       Initialize hardware drawing state, clear or preserve remembered line
*       markers, reset flush bounds and redraw counters, and recenter the
*       visible image.
*
***************************************************************************
*
*/

static void init_drawing_frame (void)
{
    int i, maxline;

    if (can_use_lores > AUTO_LORES_FRAMES && 0) {
	lores_factor = 1;
	lores_shift = 0;
    } else {
	can_use_lores++;
	lores_reset ();
    }

    init_hardware_for_drawing_frame ();

    if (thisframe_first_drawn_line == -1)
	thisframe_first_drawn_line = minfirstline;
    if (thisframe_first_drawn_line > thisframe_last_drawn_line)
	thisframe_last_drawn_line = thisframe_first_drawn_line;

    maxline = currprefs.gfx_linedbl ? (maxvpos + 1) * 2 + 1 : (maxvpos + 1) + 1;
#ifdef SMART_UPDATE
    for (i = 0; i < maxline; i++) {
	switch (linestate[i]) {
	 case LINE_DONE_AS_PREVIOUS:
	    linestate[i] = LINE_REMEMBERED_AS_PREVIOUS;
	    break;
	 case LINE_REMEMBERED_AS_BLACK:
	    break;
	 default:
	    linestate[i] = LINE_UNDECIDED;
	    break;
	}
    }
#else
    memset (linestate, LINE_UNDECIDED, maxline);
#endif
    last_drawn_line = 0;
    first_drawn_line = 32767;

    first_block_line = last_block_line = NO_BLOCK;
    if (currprefs.test_drawing_speed)
	frame_redraw_necessary = 1;
    else if (frame_redraw_necessary)
	frame_redraw_necessary--;

    center_image ();

    thisframe_first_drawn_line = -1;
    thisframe_last_drawn_line = -1;

    drawing_color_matches = -1;
}

/*
 * Some code to put status information on the screen.
 */

#define TD_PADX 10
#define TD_PADY 2
#define TD_WIDTH 32
#define TD_LED_WIDTH 24
#define TD_LED_HEIGHT 4

#define TD_RIGHT 1
#define TD_BOTTOM 2

static int td_pos = (TD_RIGHT|TD_BOTTOM);

#define TD_NUM_WIDTH 7
#define TD_NUM_HEIGHT 7

#define TD_TOTAL_HEIGHT (TD_PADY * 2 + TD_NUM_HEIGHT)

#define NUMBERS_NUM 14

#define TD_BORDER 0x333

static const char *numbers = { /* ugly  0123456789CHD% */
"+++++++--++++-+++++++++++++++++-++++++++++++++++++++++++++++++++++++++++++++-++++++-++++----++---+"
"+xxxxx+--+xx+-+xxxxx++xxxxx++x+-+x++xxxxx++xxxxx++xxxxx++xxxxx++xxxxx++xxxx+-+x++x+-+xxx++-+xx+-+x"
"+x+++x+--++x+-+++++x++++++x++x+++x++x++++++x++++++++++x++x+++x++x+++x++x++++-+x++x+-+x++x+--+x++x+"
"+x+-+x+---+x+-+xxxxx++xxxxx++xxxxx++xxxxx++xxxxx+--++x+-+xxxxx++xxxxx++x+----+xxxx+-+x++x+----+x+-"
"+x+++x+---+x+-+x++++++++++x++++++x++++++x++x+++x+--+x+--+x+++x++++++x++x++++-+x++x+-+x++x+---+x+x+"
"+xxxxx+---+x+-+xxxxx++xxxxx+----+x++xxxxx++xxxxx+--+x+--+xxxxx++xxxxx++xxxx+-+x++x+-+xxx+---+x++xx"
"+++++++---+++-++++++++++++++----+++++++++++++++++--+++--++++++++++++++++++++-++++++-++++----------"
};

/****i* januae-drawing/putpixel
*
*   NAME
*       putpixel() -- Write a status-overlay pixel at the current destination depth.
*
*   SYNOPSIS
*       STATIC_INLINE void putpixel (int x, xcolnr c8);
*
*   FUNCTION
*       Store the selected color in the active scanline buffer for 8-, 16- or
*       32-bit pixels. The 24-bit case is intentionally unsupported.
*
*   INPUTS
*       x - Horizontal destination or source coordinate in native pixels, as
*       applicable.
*       c8 - Host pixel value; converted according to output depth.
*
***************************************************************************
*
*/

STATIC_INLINE void putpixel (int x, xcolnr c8)
{
    switch (gfxvidinfo.pixbytes)
    {
    case 1:
	xlinebuffer[x] = (uae_u8)c8;
	break;
    case 2:
    {
	uae_u16 *p = (uae_u16 *)xlinebuffer + x;
	*p = (uae_u16)c8;
	break;
    }
    case 3:
	/* no 24 bit yet */
	break;
    case 4:
    {
	uae_u32 *p = (uae_u32 *)xlinebuffer + x;
	*p = c8;
	break;
    }
    }
}

/****i* januae-drawing/write_tdnumber
*
*   NAME
*       write_tdnumber() -- Render one row of a status-overlay glyph.
*
*   SYNOPSIS
*       static void write_tdnumber (int x, int y, int num);
*
*   FUNCTION
*       Interpret the requested glyph row from the fixed bitmap font and draw
*       its foreground and background pixels; other glyph characters leave
*       pixels unchanged.
*
*   INPUTS
*       x - Horizontal destination or source coordinate in native pixels, as
*       applicable.
*       y - Vertical line or font-row coordinate.
*       num - Index into the fixed status-overlay character set.
*
***************************************************************************
*
*/

static void write_tdnumber (int x, int y, int num)
{
    int j;
    const char *numptr;

    numptr = numbers + num * TD_NUM_WIDTH + NUMBERS_NUM * TD_NUM_WIDTH * y;
    for (j = 0; j < TD_NUM_WIDTH; j++) {
	if (*numptr == 'x')
	    putpixel (x + j, xcolors[0xfff]);
	else if (*numptr == '+')
	    putpixel (x + j, xcolors[0x000]);
	numptr++;
    }
}

/****i* januae-drawing/draw_status_line
*
*   NAME
*       draw_status_line() -- Draw one row of the on-screen drive and device indicators.
*
*   SYNOPSIS
*       static void draw_status_line (int line);
*
*   FUNCTION
*       Render LED shapes, labels and counters in the configured screen corner,
*       using current GUI activity data and the status font.
*
*   INPUTS
*       line - Native output scanline for the status overlay.
*
***************************************************************************
*
*/

static void draw_status_line (int line)
{
    int x_start, y, j, led;

    if (td_pos & TD_RIGHT)
	x_start = gfxvidinfo.width - TD_PADX - NUM_LEDS * TD_WIDTH;
    else
	x_start = TD_PADX;

    y = line - (gfxvidinfo.height - TD_TOTAL_HEIGHT);
    xlinebuffer = gfxvidinfo.linemem;
    if (xlinebuffer == 0)
	xlinebuffer = row_map[line];

    for (led = 0; led < NUM_LEDS; led++) {
	int side, pos, num1 = -1, num2 = -1, num3 = -1, num4 = -1;
	int x, off_rgb, on_rgb, c, on = 0, am = 2;
	if (led >= 1 && led <= 4) {
	    int pled = led - 1;
	    int track = gui_data.drive_track[pled];
	    pos = 5 + pled;
	    on_rgb = 0x0c0;
	    off_rgb = 0x030;
	    if (!gui_data.drive_disabled[pled]) {
		num1 = -1;
		num2 = track / 10;
		num3 = track % 10;
		on = gui_data.drive_motor[pled];
		if (gui_data.drive_writing[pled])
		    on_rgb = 0xc00;
	    }
	    side = gui_data.drive_side;
	} else if (led == 0) {
	    pos = 2;
	    on = gui_data.powerled;
	    on_rgb = 0xc00;
	    off_rgb = 0x300;
	} else if (led == 5) {
	    pos = 4;
	    on = gui_data.cd;
	    on_rgb = 0x00c;
	    off_rgb = 0x003;
	    num1 = -1;
	    num2 = 10;
	    num3 = 12;
	} else if (led == 6) {
	    pos = 3;
	    on = gui_data.hd;
	    on_rgb = on == 2 ? 0xc00 : 0x00c;
	    off_rgb = 0x003;
	    num1 = -1;
	    num2 = 11;
	    num3 = 12;
	} else if (led == 7) {
	    int fps = (gui_data.fps + 5) / 10;
	    pos = 1;
	    on_rgb = 0x000;
	    off_rgb = 0x000;
	    num1 = fps / 100;
	    num2 = (fps - num1 * 100) / 10;
	    num3 = fps % 10;
	    am = 3;
	    if (num1 == 0)
		am = 2;
	} else if (led == 8) {
	    int idle = (gui_data.idle + 5) / 10;
	    pos = 0;
	    on_rgb = 0x000;
	    off_rgb = 0x000;
	    num1 = idle / 100;
	    num2 = (idle - num1 * 100) / 10;
	    num3 = idle % 10;
	    num4 = num1 == 0 ? 13 : -1;
	    am = 3;
	}
	else {
	  /* o1i: pos and off_rgb were used uninitialized here ? */
	  pos=0;
	  off_rgb = 0x000;
	  on_rgb = 0x000;
	}
	c = xcolors[on ? on_rgb : off_rgb];
	if (y == 0 || y == TD_TOTAL_HEIGHT - 1)
	    c = xcolors[TD_BORDER];

	x = x_start + pos * TD_WIDTH;
	putpixel (x - 1, xcolors[TD_BORDER]);
	for (j = 0; j < TD_LED_WIDTH; j++)
	    putpixel (x + j, c);
	putpixel (x + j, xcolors[TD_BORDER]);

	if (y >= TD_PADY && y - TD_PADY < TD_NUM_HEIGHT) {
	    if (num3 >= 0) {
		x += (TD_LED_WIDTH - am * TD_NUM_WIDTH) / 2;
		if (num1 > 0) {
		    write_tdnumber (x, y - TD_PADY, num1);
		    x += TD_NUM_WIDTH;
		}
		write_tdnumber (x, y - TD_PADY, num2);
		x += TD_NUM_WIDTH;
		write_tdnumber (x, y - TD_PADY, num3);
		x += TD_NUM_WIDTH;
		if (num4 > 0)
		    write_tdnumber (x, y - TD_PADY, num4);
	    }
	}
    }
}

/**************************************************
 * finish_drawing_frame
 *
 * seems to draw the content of the frame to the
 * uae window
 *
 * So if we are in rootless mode, we don't draw
 * anything here.
 **************************************************/
/****i* januae-drawing/finish_drawing_frame
*
*   NAME
*       finish_drawing_frame() -- Render the completed native frame and flush it to the host.
*
*   SYNOPSIS
*       void finish_drawing_frame (void);
*
*   FUNCTION
*       Skip drawing after the AROS main window is closed; otherwise lock the
*       screen, draw decided lines under SMART_UPDATE, optionally render status
*       LEDs and flush. If locking fails, request a full redraw.
*
***************************************************************************
*
*/

void finish_drawing_frame (void) {

  int i;

/* GTKMUI.. */
#ifdef __AROS__
  if(uae_main_window_closed) {
    return;
  }
#endif

  JWLOG("not uae_main_window_closed\n");

    if (! lockscr ()) {
	notice_screen_contents_lost ();
	return;
    }

#ifndef SMART_UPDATE
    /* @@@ This isn't exactly right yet. FIXME */
    if (!interlace_seen)
	do_flush_screen (first_drawn_line, last_drawn_line);
    else
	unlockscr ();
    return;
#endif
    for (i = 0; i < max_ypos_thisframe; i++) {
	int where;
	int i1 = i + min_ypos_for_screen;
	int line = i + thisframe_y_adjust_real;

	if (linestate[line] == LINE_UNDECIDED)
	    break;

	where = amiga2aspect_line_map[i1];
	if (where >= gfxvidinfo.height)
	    break;
	if (where == -1)
	    continue;

	pfield_draw_line (line, where, amiga2aspect_line_map[i1 + 1]);
    }
    if (currprefs.leds_on_screen) {
	for (i = 0; i < TD_TOTAL_HEIGHT; i++) {
	    int line = gfxvidinfo.height - TD_TOTAL_HEIGHT + i;
	    draw_status_line (line);
	    do_flush_line (line);
	}
    }

    do_flush_screen (first_drawn_line, last_drawn_line);
}

/****i* januae-drawing/hardware_line_completed
*
*   NAME
*       hardware_line_completed() -- Handle completion of one emulated hardware scanline.
*
*   SYNOPSIS
*       void hardware_line_completed (int lineno);
*
*   FUNCTION
*       Without SMART_UPDATE, map the completed Amiga line to a native position
*       and draw it when visible. Under SMART_UPDATE this function performs no
*       work.
*
*   INPUTS
*       lineno - Emulated or native line number, as expected by the function.
*
***************************************************************************
*
*/

void hardware_line_completed (int lineno)
{
#ifndef SMART_UPDATE
    {
	int i, where;
	/* l is the line that has been finished for drawing. */
	i = lineno - thisframe_y_adjust_real;
	if (i >= 0 && i < max_ypos_thisframe) {
	    where = amiga2aspect_line_map[i+min_ypos_for_screen];
	    if (where < gfxvidinfo.height && where != -1)
		pfield_draw_line (lineno, where, amiga2aspect_line_map[i+min_ypos_for_screen+1]);
	}
    }
#endif
}

/****i* januae-drawing/check_picasso
*
*   NAME
*       check_picasso() -- Apply a pending Picasso96 display mode switch.
*
*   SYNOPSIS
*       STATIC_INLINE void check_picasso (void);
*
*   FUNCTION
*       When PICASSO96 is compiled, refresh an invalidated Picasso image, switch
*       the visible graphics state and request fresh colors and a full redraw;
*       otherwise this function is empty.
*
***************************************************************************
*
*/

STATIC_INLINE void check_picasso (void)
{
#ifdef PICASSO96
    if (picasso_on && picasso_redraw_necessary)
	picasso_refresh (1);
    picasso_redraw_necessary = 0;

    if (picasso_requested_on == picasso_on)
	return;

    picasso_on = picasso_requested_on;

    if (!picasso_on)
	clear_inhibit_frame (IHF_PICASSO);
    else
	set_inhibit_frame (IHF_PICASSO);

    gfx_set_picasso_state (picasso_on);
    picasso_enablescreen (picasso_requested_on);

    notice_screen_contents_lost ();
    notice_new_xcolors ();
#endif
}

/****i* januae-drawing/redraw_frame
*
*   NAME
*       redraw_frame() -- Force a drawing pass and request screen flush.
*
*   SYNOPSIS
*       void redraw_frame (void);
*
*   FUNCTION
*       Reset drawn-line bounds, render a frame, then explicitly flush the
*       screen with a zero-length range.
*
***************************************************************************
*
*/

void redraw_frame (void)
{
    last_drawn_line = 0;
    first_drawn_line = 32767;
    finish_drawing_frame ();
    flush_screen (0, 0);
}

/****i* januae-drawing/vsync_handle_redraw
*
*   NAME
*       vsync_handle_redraw() -- Finish a drawing interval at vertical synchronization.
*
*   SYNOPSIS
*       void vsync_handle_redraw (int long_frame, int lof_changed);
*
*   FUNCTION
*       Complete a frame when interlace timing permits, handle queued state
*       restoration and preference updates, update frame skipping and Picasso
*       state, and start the next drawing frame when due.
*
*   INPUTS
*       long_frame - Nonzero reports a long video frame.
*       lof_changed - Nonzero reports a change of long/short-field state.
*
***************************************************************************
*
*/

void vsync_handle_redraw (int long_frame, int lof_changed)
{
    last_redraw_point++;
    if (lof_changed || ! interlace_seen || last_redraw_point >= 2 || long_frame) {
	last_redraw_point = 0;

	if (framecnt == 0)
	    finish_drawing_frame ();

	interlace_seen = 0;

	/* At this point, we have finished both the hardware and the
	 * drawing frame. Essentially, we are outside of all loops and
	 * can do some things which would cause confusion if they were
	 * done at other times.
	 */

#ifdef SAVESTATE
	if (savestate_state == STATE_DORESTORE) {
	    savestate_state = STATE_RESTORE;
	    reset_drawing ();
	    uae_reset (0);
	} else if (savestate_state == STATE_DOREWIND) {
	    savestate_state = STATE_REWIND;
	    reset_drawing ();
	    uae_reset (0);
	}
#endif

	if (uae_state_change_pending ()) {
	    set_special (&regs, SPCFLAG_BRK);
	    init_drawing_frame ();
	    return;
	}

#ifdef SAVESTATE
	savestate_capture (0);
#endif
	count_frame ();
	check_picasso ();

	if (check_prefs_changed_gfx ()) {
	    reset_drawing ();
	    init_row_map ();
	    init_aspect_maps ();
	    notice_screen_contents_lost ();
	    notice_new_xcolors ();
	}

	check_prefs_changed_audio ();
#ifdef JIT
	check_prefs_changed_comp ();
#endif
	check_prefs_changed_custom ();
	check_prefs_changed_cpu ();

	if (inhibit_frame != 0)
	    framecnt = 1;

	if (framecnt == 0)
	    init_drawing_frame ();
    } else {
	if (is_vsync ())
	    flush_screen (0, 0); /* vsync mode */
    }
    gui_hd_led (0);
    gui_cd_led (0);
#ifdef AVIOUTPUT
    frame_drawn ();
#endif
}

/****i* januae-drawing/hsync_record_line_state
*
*   NAME
*       hsync_record_line_state() -- Record the rendering status of a completed hardware line.
*
*   SYNOPSIS
*       void hsync_record_line_state (int lineno, enum nln_how how, int
*       changed);
*
*   FUNCTION
*       If frame skipping is inactive, update one or two scanline state entries
*       according to normal, doubled, black, upper or lower transition state and
*       whether content has changed.
*
*   INPUTS
*       lineno - Emulated or native line number, as expected by the function.
*       how - Recorded raster-line transition category.
*       changed - Nonzero indicates changed rendered contents.
*
***************************************************************************
*
*/

void hsync_record_line_state (int lineno, enum nln_how how, int changed)
{
    char *state;

    if (framecnt != 0)
	return;

    state = linestate + lineno;
    changed += frame_redraw_necessary;

    switch (how) {
     case nln_normal:
	*state = changed ? LINE_DECIDED : LINE_DONE;
	break;
     case nln_doubled:
	*state = changed ? LINE_DECIDED_DOUBLE : LINE_DONE;
	changed += state[1] != LINE_REMEMBERED_AS_PREVIOUS;
	state[1] = changed ? LINE_AS_PREVIOUS : LINE_DONE_AS_PREVIOUS;
	break;
     case nln_nblack:
	*state = changed ? LINE_DECIDED : LINE_DONE;
	if (state[1] != LINE_REMEMBERED_AS_BLACK)
	    state[1] = LINE_BLACK;
	break;
     case nln_lower:
	if (state[-1] == LINE_UNDECIDED)
	    state[-1] = LINE_BLACK;
	*state = changed ? LINE_DECIDED : LINE_DONE;
	break;
     case nln_upper:
	*state = changed ? LINE_DECIDED : LINE_DONE;
	if (state[1] == LINE_UNDECIDED
	    || state[1] == LINE_REMEMBERED_AS_PREVIOUS
	    || state[1] == LINE_AS_PREVIOUS)
	    state[1] = LINE_BLACK;
	break;
    }
}

/****i* januae-drawing/notice_interlace_seen
*
*   NAME
*       notice_interlace_seen() -- Mark the current video frame as interlaced.
*
*   SYNOPSIS
*       void notice_interlace_seen (void);
*
*   FUNCTION
*       Set the frame-level interlace marker, influencing the frame-flush and
*       line-doubling logic.
*
***************************************************************************
*
*/

void notice_interlace_seen (void)
{
    interlace_seen = 1;
}

/****i* januae-drawing/reset_drawing
*
*   NAME
*       reset_drawing() -- Reset viewport mappings and rendering state.
*
*   SYNOPSIS
*       void reset_drawing (void);
*
*   FUNCTION
*       Clear scanline decisions and sprite buffers, reconstruct row and aspect
*       maps, initialize a drawing frame and require a complete screen redraw.
*
***************************************************************************
*
*/

void reset_drawing (void)
{
    unsigned int i;

    max_diwstop = 0;

    lores_reset ();

    for (i = 0; i < sizeof linestate / sizeof *linestate; i++)
	linestate[i] = LINE_UNDECIDED;

    init_aspect_maps ();

    init_row_map();

    last_redraw_point = 0;

    memset (spixels, 0, sizeof spixels);
    memset (&spixstate, 0, sizeof spixstate);

    init_drawing_frame ();

    notice_screen_contents_lost ();
}

/****i* januae-drawing/drawing_init
*
*   NAME
*       drawing_init() -- Initialize the video drawing subsystem.
*
*   SYNOPSIS
*       void drawing_init (void);
*
*   FUNCTION
*       Build playfield lookup tables, initialize optional Picasso96 screen
*       state and reset drawing.
*
***************************************************************************
*
*/

void drawing_init (void)
{
    gen_pfield_tables ();

#ifdef PICASSO96
    InitPicasso96 ();
    picasso_on = 0;
    picasso_requested_on = 0;

    /* FIXME: Ensure native screen is displayed, not picasso screen. The shouldn't
     * be necessary here, but at the moment it's required if UAE is reset when
     * displaying a P96 screen.
     */
    gfx_set_picasso_state (0);
#endif

    inhibit_frame = 0;

    /* Removing this call can break restoring from saved state. */
    reset_drawing ();
}
