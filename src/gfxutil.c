 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Common code needed by all the various graphics systems.
  *
  * (c) 1996 Bernd Schmidt, Ed Hanway, Samuel Devulder
  */

#include "sysconfig.h"
#include "sysdeps.h"
#include "custom.h"
#include "xwin.h"
#include "uae_endian.h"

#define	RED 	0
#define	GRN	1
#define	BLU	2

/*
 * dither matrix
 */
static uae_u8 dither[4][4] =
{
  { 0, 8, 2, 10 },
  { 12, 4, 14, 6 },
  { 3, 11, 1, 9 },
  { 14 /* 15 */, 7, 13, 5 }
};


/****i* januae-gfxutil/doMask *************************************************
*
*   NAME
*       doMask() -- Map a component value into a pixel mask.
*
*   SYNOPSIS
*       uae_u32 doMask (int p, int bits, int shift);
*
*   FUNCTION
*       Take the most significant bits of the left-shifted component value
*       and place them at the requested bit position.
*
*   INPUTS
*       p - Component value.
*       bits - Number of component bits; zero produces zero.
*       shift - Destination bit offset.
*
*   RESULT
*       Shifted component bit field.
*
****************************************************************************
*
*/

uae_u32 doMask (int p, int bits, int shift)
{
    /* scale to 0..255, shift to align msb with mask, and apply mask */
    uae_u32 val = p << 24;
    if (!bits)
	return 0;
    val >>= (32 - bits);
    val <<= shift;

    return val;
}

/****i* januae-gfxutil/bits_in_mask *******************************************
*
*   NAME
*       bits_in_mask() -- Count set bits in a pixel mask.
*
*   SYNOPSIS
*       int bits_in_mask (unsigned long mask);
*
*   FUNCTION
*       Count the one-bits in mask.
*
*   INPUTS
*       mask - Bit mask to examine.
*
*   RESULT
*       Number of set bits; zero for an empty mask.
*
****************************************************************************
*
*/

int bits_in_mask (unsigned long mask)
{
    int n = 0;
    while (mask) {
	n += mask & 1;
	mask >>= 1;
    }
    return n;
}

/****i* januae-gfxutil/mask_shift *********************************************
*
*   NAME
*       mask_shift() -- Find the least significant bit of a mask.
*
*   SYNOPSIS
*       int mask_shift (unsigned long mask);
*
*   FUNCTION
*       Count the zero-bits below the least significant set bit.
*
*   INPUTS
*       mask - Nonzero bit mask.
*
*   RESULT
*       Index of the least significant set bit.
*
*   NOTES
*       An all-zero mask never terminates the shift loop and is not a
*       supported input.
*
****************************************************************************
*
*/

int mask_shift (unsigned long mask)
{
    int n = 0;
    while (!(mask & 1)) {
	n++;
	mask >>= 1;
    }
    return n;
}

/****i* januae-gfxutil/doMask256 **********************************************
*
*   NAME
*       doMask256() -- Map an eight-bit component into a pixel mask.
*
*   SYNOPSIS
*       unsigned int doMask256 (int p, int bits, int shift);
*
*   FUNCTION
*       Replicate an eight-bit intensity across a 32-bit word, retain the
*       requested high bits and shift them into position.
*
*   INPUTS
*       p - Eight-bit component intensity.
*       bits - Number of component bits to retain.
*       shift - Destination bit offset.
*
*   RESULT
*       Shifted component bit field.
*
****************************************************************************
*
*/

unsigned int doMask256 (int p, int bits, int shift)
{
    /* p is a value from 0 to 255 (Amiga color value)
     * shift to align msb with mask, and apply mask */

    unsigned int val = p * 0x01010101UL;
    val >>= (32 - bits);
    val <<= shift;

    return val;
}

/****i* januae-gfxutil/doColor ************************************************
*
*   NAME
*       doColor() -- Pack a component for the AGA color tables.
*
*   SYNOPSIS
*       static unsigned int doColor (int i, int bits, int shift);
*
*   FUNCTION
*       Discard low input bits when the destination component width is less
*       than eight, then shift the retained value into position.
*
*   INPUTS
*       i - Component value.
*       bits - Destination component width.
*       shift - Destination bit offset.
*
*   RESULT
*       Packed component value.
*
****************************************************************************
*
*/

static unsigned int doColor (int i, int bits, int shift)
{
    int shift2;
    if(bits >= 8) shift2 = 0; else shift2 = 8 - bits;
    return (i >> shift2) << shift;
}

/****i* januae-gfxutil/doAlpha ************************************************
*
*   NAME
*       doAlpha() -- Pack the constant alpha component.
*
*   SYNOPSIS
*       static unsigned int doAlpha (int alpha, int bits, int shift);
*
*   FUNCTION
*       Mask alpha to the requested width and shift it to the destination
*       position.
*
*   INPUTS
*       alpha - Alpha value.
*       bits - Alpha component width.
*       shift - Destination bit offset.
*
*   RESULT
*       Packed alpha field.
*
****************************************************************************
*
*/

static unsigned int doAlpha (int alpha, int bits, int shift)
{
    return (alpha & ((1 << bits) - 1)) << shift;
}


/****i* januae-gfxutil/alloc_colors64k ****************************************
*
*   NAME
*       alloc_colors64k() -- Build direct-color lookup tables.
*
*   SYNOPSIS
*       void alloc_colors64k (int rw, int gw, int bw, int rs, int gs, int bs, int aw, int as, int alpha, int byte_swap);
*
*   FUNCTION
*       Convert every 12-bit RGB color into its host pixel representation in
*       xcolors. When AGA is enabled, also populate the per-channel 256-entry
*       lookup tables.
*
*   INPUTS
*       rw, gw, bw - Red, green and blue bit widths.
*       rs, gs, bs - Destination shifts for the color channels.
*       aw, as - Alpha width and shift.
*       alpha - Constant alpha value.
*       byte_swap - Nonzero to reverse byte order in each 16- or 32-bit
*       pixel.
*
*   RESULT
*       None.
*
*   NOTES
*       For formats of at most 16 bits per pixel, each packed pixel is
*       duplicated into both halves of the lookup-table entry.
*
****************************************************************************
*
*/

void alloc_colors64k (int rw, int gw, int bw, int rs, int gs, int bs, int aw, int as, int alpha, int byte_swap)
{
    unsigned int i;
    unsigned int bpp = rw + gw + bw + aw;

    for (i = 0; i < 4096; i++) {
	int r = ((i >> 8) << 4) | (i >> 8);
	int g = (((i >> 4) & 0xf) << 4) | ((i >> 4) & 0x0f);
	int b = ((i & 0xf) << 4) | (i & 0x0f);

	xcolors[i] = doMask(r, rw, rs) | doMask(g, gw, gs) | doMask(b, bw, bs) | doAlpha (alpha, aw, as);

        if (byte_swap) {
	    if (bpp <= 16)
		xcolors[i] = bswap_16 (xcolors[i]);
	    else
		xcolors[i] = bswap_32 (xcolors[i]);
	}

        if (bpp <= 16) {
	    /* Fill upper 16 bits of each colour value
	     * with a copy of the colour. */
	    xcolors[i] = xcolors[i] * 0x00010001;
	}
    }

#ifdef AGA
    /* create AGA color tables */
    for (i = 0; i < 256; i++) {
	xredcolors  [i] = doColor (i, rw, rs) | doAlpha (alpha, aw, as);
	xgreencolors[i] = doColor (i, gw, gs) | doAlpha (alpha, aw, as);
	xbluecolors [i] = doColor (i, bw, bs) | doAlpha (alpha, aw, as);

        if (byte_swap) {
	    if (bpp <= 16) {
		xredcolors  [i] = bswap_16 (xredcolors[i]);
		xgreencolors[i] = bswap_16 (xgreencolors[i]);
		xbluecolors [i] = bswap_16 (xbluecolors[i]);
	    } else {
		xredcolors  [i] = bswap_32 (xredcolors[i]);
		xgreencolors[i] = bswap_32 (xgreencolors[i]);
		xbluecolors [i] = bswap_32 (xbluecolors[i]);
	    }
	}

        if (bpp <= 16) {
	    /* Fill upper 16 bits of each colour value with
	     * a copy of the colour. */
	    xredcolors  [i] = xredcolors  [i] * 0x00010001;
	    xgreencolors[i] = xgreencolors[i] * 0x00010001;
	    xbluecolors [i] = xbluecolors [i] * 0x00010001;
	}
    }
#endif
}

static int color_diff[4096];
static int newmaxcol = 0;

/****i* januae-gfxutil/setup_maxcol *******************************************
*
*   NAME
*       setup_maxcol() -- Set the indexed-color palette budget.
*
*   SYNOPSIS
*       void setup_maxcol (int max);
*
*   FUNCTION
*       Store the maximum palette size for the next alloc_colors256() call.
*
*   INPUTS
*       max - Requested number of colors; zero selects 256.
*
*   RESULT
*       None.
*
*   NOTES
*       The value is not validated here; alloc_colors256() requires enough
*       entries for at least two levels of each color component.
*
****************************************************************************
*
*/

void setup_maxcol (int max)
{
    newmaxcol = max;
}

/****i* januae-gfxutil/alloc_colors256 ****************************************
*
*   NAME
*       alloc_colors256() -- Build an indexed-color translation table.
*
*   SYNOPSIS
*       void alloc_colors256 (allocfunc_type allocfunc);
*
*   FUNCTION
*       Allocate a coarse RGB palette through allocfunc, map all 4096 RGB444
*       colors to palette entries, and use remaining entries for the largest
*       approximations.
*
*   INPUTS
*       allocfunc - Callback that allocates a palette entry from three
*       four-bit color components and writes its pixel index.
*
*   RESULT
*       None.
*
*   NOTES
*       Uses the budget from setup_maxcol(), or 256 when unset. Allocation
*       failure for the temporary map terminates the process; callback
*       results are not checked.
*
****************************************************************************
*
*/

void alloc_colors256 (allocfunc_type allocfunc)
{
    int nb_cols[3]; /* r,g,b */
    int maxcol = newmaxcol == 0 ? 256 : newmaxcol;
    int i,j,k,l;

    xcolnr *map;

    map = (xcolnr *)malloc (sizeof(xcolnr) * maxcol);
    if (!map) {
	write_log ("Not enough mem for colormap!\n");
	abort ();
    }

    /*
     * compute #cols per components
     */
    for (i = 1; i*i*i <= maxcol; ++i)
	;
    --i;

    nb_cols[RED] = i;
    nb_cols[GRN] = i;
    nb_cols[BLU] = i;

    /*
     * set the colormap
     */
    l = 0;
    for (i = 0; i < nb_cols[RED]; ++i) {
	int r = (i * 15) / (nb_cols[RED] - 1);
	for (j = 0; j < nb_cols[GRN]; ++j) {
	    int g = (j * 15) / (nb_cols[GRN] - 1);
	    for (k = 0; k < nb_cols[BLU]; ++k) {
		int b = (k * 15) / (nb_cols[BLU] - 1);
		int result;
		result = allocfunc (r, g, b, map + l);
		l++;
	    }
	}
    }
/*    printf("%d color(s) lost\n",maxcol - l);*/

    /*
     * for each component compute the mapping
     */
    {
	int diffr, diffg, diffb, maxdiff = 0, won = 0, lost;
	int r, d = 8;
	for (r = 0; r < 16; ++r) {
	    int cr, g, q;

	    k = nb_cols[RED]-1;
	    cr = (r * k) / 15;
	    q = (r * k) % 15;
	    if (q > d && cr < k) ++cr;
	    diffr = abs (cr*k - r);
	    for (g = 0; g < 16; ++g) {
		int cg, b;

		k = nb_cols[GRN]-1;
		cg = (g * k) / 15;
		q  = (g * k) % 15;
		if (q > d && cg < k) ++cg;
		diffg = abs (cg*k - g);
		for (b = 0; b < 16; ++b) {
		    int cb, rgb = (r << 8) | (g << 4) | b;

		    k = nb_cols[BLU]-1;
		    cb = (b * k) / 15;
		    q = (b * k) % 15;
		    if (q > d && cb < k) ++cb;
		    diffb = abs (cb*k - b);
		    xcolors[rgb] = map[(cr * nb_cols[GRN] + cg) * nb_cols[BLU] + cb];
		    color_diff[rgb] = diffr + diffg + diffb;
		    if (color_diff[rgb] > maxdiff)
			maxdiff = color_diff[rgb];
		}
	    }
	}
	while (maxdiff > 0 && l < maxcol) {
	    int newmaxdiff = 0;
	    lost = 0; won++;
	    for (r = 15; r >= 0; r--) {
		int g;

		for (g = 15; g >= 0; g--) {
		    int b;

		    for (b = 15; b >= 0; b--) {
			int rgb = (r << 8) | (g << 4) | b;

			if (color_diff[rgb] == maxdiff) {
			    int result;

			    if (l >= maxcol)
				lost++;
			    else {
				result = allocfunc (r, g, b, xcolors + rgb);
				l++;
			    }
			    color_diff[rgb] = 0;
			} else if (color_diff[rgb] > newmaxdiff)
				newmaxdiff = color_diff[rgb];

		    }
		}
	    }
	    maxdiff = newmaxdiff;
	}
/*	printf("%d color(s) lost, %d stages won\n",lost, won);*/
    }
    free (map);

    for (i = 0; i < 4096; i++)
	xcolors[i] = xcolors[i] * 0x01010101;
}

/*
 * This dithering process works by letting UAE run internaly in 12bit
 * mode and doing the dithering on the fly when rendering to the display.
 * The dithering algorithm is quite fast but uses lot of memory (4*8*2^12 =
 * 128Kb). I don't think that is a trouble right now, but when UAE will
 * emulate AGA and work internaly in 24bit mode, that dithering algorithm
 * will need 4*8*2^24 = 512Mb. Obviously that fast algorithm will not be
 * tractable. However, we could then use an other algorithm, slower, but
 * far more reasonable (I am thinking about the one that is used in DJPEG).
 */

uae_u8 cidx[4][8*4096]; /* fast, but memory hungry =:-( */

/*
 * Compute dithering structures
 */
/****i* januae-gfxutil/setup_greydither_maxcol ********************************
*
*   NAME
*       setup_greydither_maxcol() -- Prepare an ordered grayscale dither table.
*
*   SYNOPSIS
*       void setup_greydither_maxcol (int maxcol, allocfunc_type allocfunc);
*
*   FUNCTION
*       Allocate maxcol grayscale entries and populate cidx with a
*       four-by-four ordered-dither mapping of RGB444 colors.
*
*   INPUTS
*       maxcol - Number of grayscale entries; must be at least two.
*       allocfunc - Palette-allocation callback receiving equal red, green
*       and blue components.
*
*   RESULT
*       None.
*
*   NOTES
*       The callback status is ignored. Failure to allocate the temporary map
*       terminates the process.
*
****************************************************************************
*
*/

void setup_greydither_maxcol (int maxcol, allocfunc_type allocfunc)
{
    int i,j,k;
    xcolnr *map;

    for (i = 0; i < 4096; i++)
	xcolors[i] = i << 16 | i;

    map = (xcolnr *)malloc (sizeof(xcolnr) * maxcol);
    if (!map) {
	write_log ("Not enough mem for colormap!\n");
	abort();
    }

    /*
     * set the colormap
     */
    for (i = 0; i < maxcol; ++i) {
	int c, result;
	c = (15 * i + (maxcol-1)/2) / (maxcol - 1);
	result = allocfunc(c, c, c, map + i);
	/* @@@ check for errors */
    }

    /*
     * for each componant compute the mapping
     */
    for (i = 0; i < 4; ++i) {
	for (j = 0; j < 4; ++j) {
	    int r, d = dither[i][j]*17;
	    for (r = 0; r<16; ++r) {
		int g;
		for (g = 0; g < 16; ++g) {
		    int  b;
		    for (b = 0; b < 16; ++b) {
			int rgb = (r << 8) | (g << 4) | b;
			int c,p,q;

			c = (77  * r +
			     151 * g +
			     28  * b) / 15; /* c in 0..256 */

			k = maxcol-1;
			p = (c * k) / 256;
			q = (c * k) % 256;
			if (q /*/ k*/> d /*/ k*/ && p < k) ++p;
/* sam:                      ^^^^^^^ */
/*  It seems that produces better output */
			cidx[i][rgb + (j+4)*4096] =
			    cidx[i][rgb + j*4096] = (uae_u8)map[p];
		    }
		}
	    }
	}
    }
    free (map);
}

/****i* januae-gfxutil/setup_greydither ***************************************
*
*   NAME
*       setup_greydither() -- Configure grayscale dithering by output depth.
*
*   SYNOPSIS
*       void setup_greydither (int bits, allocfunc_type allocfunc);
*
*   FUNCTION
*       Delegate to setup_greydither_maxcol() with a palette size of 1 <<
*       bits.
*
*   INPUTS
*       bits - Output index width; the selected palette size must be at least
*       two.
*       allocfunc - Grayscale palette-allocation callback.
*
*   RESULT
*       None.
*
****************************************************************************
*
*/

void setup_greydither (int bits, allocfunc_type allocfunc)
{
    setup_greydither_maxcol(1 << bits, allocfunc);
}

/****i* januae-gfxutil/setup_dither *******************************************
*
*   NAME
*       setup_dither() -- Prepare an ordered RGB dither table.
*
*   SYNOPSIS
*       void setup_dither (int bits, allocfunc_type allocfunc);
*
*   FUNCTION
*       Allocate an RGB palette and populate cidx with pixel indices for the
*       four-by-four threshold matrix and every RGB444 color.
*
*   INPUTS
*       bits - Output index width; the palette must accommodate at least two
*       levels per component.
*       allocfunc - Callback that allocates one palette entry from three
*       four-bit components.
*
*   RESULT
*       None.
*
*   NOTES
*       Temporary map allocation failure terminates the process. The
*       additional component-value allocation and callback results are not
*       checked.
*
****************************************************************************
*
*/

void setup_dither (int bits, allocfunc_type allocfunc)
{
    int nb_cols[3]; /* r,g,b */
    int maxcol = 1 << bits;
    int i,j,k,l;

    xcolnr *map;
    int *redvals, *grnvals, *bluvals;

    map = (xcolnr *)malloc (sizeof(xcolnr) * maxcol);
    if (!map) {
	write_log ("Not enough mem for colormap!\n");
	abort();
    }

    for (i = 0; i < 4096; i++)
	xcolors[i] = i << 16 | i;

    /*
     * compute #cols per components
     */
    for (i = 1; i*i*i <= maxcol; ++i)
	;
    --i;

    nb_cols[RED] = i;
    nb_cols[GRN] = i;
    nb_cols[BLU] = i;

    if (nb_cols[RED]*(++i)*nb_cols[BLU] <= maxcol) {
	nb_cols[GRN] = i;
	if ((i)*nb_cols[GRN]*nb_cols[BLU] <= maxcol)
	    nb_cols[RED] = i;
    }

    redvals = (int *)malloc (sizeof(int) * maxcol);
    grnvals = redvals + nb_cols[RED];
    bluvals = grnvals + nb_cols[GRN];
    /*
     * set the colormap
     */
    l = 0;
    for (i = 0; i < nb_cols[RED]; ++i) {
	int r = (i * 15) / (nb_cols[RED] - 1);
	redvals[i] = r;
	for (j = 0; j < nb_cols[GRN]; ++j) {
	    int g = (j * 15) / (nb_cols[GRN] - 1);
	    grnvals[j] = g;
	    for (k = 0; k < nb_cols[BLU]; ++k) {
		int b = (k * 15) / (nb_cols[BLU] - 1);
		int result;
		bluvals[k] = b;
		result = allocfunc(r, g, b, map + l);
		l++;
	    }
	}
    }
/*    write_log ("%d color(s) lost\n",maxcol - l);*/

    /*
     * for each component compute the mapping
     */
    {
	int r;
	for (r = 0; r < 16; ++r) {
	    int g;
	    for (g = 0; g < 16; ++g) {
		int b;
		for (b = 0; b < 16; ++b) {
		    int rgb = (r << 8) | (g << 4) | b;

		    for (i = 0; i < 4; ++i) for (j = 0; j < 4; ++j) {
			int d = dither[i][j];
			int cr, cg, cb, k, q;
#if 0 /* Slightly different algorithm. Needs some tuning. */
			int rederr = 0, grnerr = 0, bluerr = 0;

			k  = nb_cols[RED]-1;
			cr = r * k / 15;
			q  = r * k - 15*cr;
			if (cr < 0)
			    cr = 0;
			else if (q / k > d / k && rederr <= 0)
			    ++cr;
			if (cr > k) cr = k;
			rederr += redvals[cr]-r;

			k  = nb_cols[GRN]-1;
			cg = g * k / 15;
			q  = g * k - 15*cg;
			if (cg < 0)
			    cg = 0;
			else if (q / k > d / k && grnerr <= 0)
			    ++cg;
			if (cg > k) cg = k;
			grnerr += grnvals[cg]-g;

			k  = nb_cols[BLU]-1;
			cb = b * k / 15;
			q  = b * k - 15*cb;
			if (cb < 0)
			    cb = 0;
			else if (q / k > d / k && bluerr <= 0)
			    ++cb;
			if (cb > k) cb = k;
			bluerr += bluvals[cb]-b;
#else
			k  = nb_cols[RED]-1;
			cr = r * k / 15;
			q  = r * k - 15*cr;
			if (cr < 0)
			    cr = 0;
			else if (q /*/ k*/ > d /*/ k*/)
			    ++cr;
			if (cr > k) cr = k;

			k  = nb_cols[GRN]-1;
			cg = g * k / 15;
			q  = g * k - 15*cg;
			if (cg < 0)
			    cg = 0;
			else if (q /*/ k*/ > d /*/ k*/)
			    ++cg;
			if (cg > k) cg = k;

			k  = nb_cols[BLU]-1;
			cb = b * k / 15;
			q  = b * k - 15*cb;
			if (cb < 0)
			    cb = 0;
			else if (q /*/ k*/ > d /*/ k*/)
			    ++cb;
			if (cb > k) cb = k;
#endif
			cidx[i][rgb + (j+4)*4096] = cidx[i][rgb + j*4096] = (uae_u8)map[(cr*nb_cols[GRN]+cg)*nb_cols[BLU]+cb];
		    }
		}
	    }
	}
    }
    free (redvals);
    free (map);
}

#if !defined X86_ASSEMBLY
/*
 * Dither the line.
 * Make sure you call this only with (len & 3) == 0, or you'll just make
 * yourself unhappy.
 */

/****i* januae-gfxutil/DitherLine *********************************************
*
*   NAME
*       DitherLine() -- Dither one scanline of RGB444 pixels.
*
*   SYNOPSIS
*       void DitherLine (uae_u8 *l, uae_u16 *r4g4b4, int x, int y, uae_s16 len, int bits);
*
*   FUNCTION
*       Convert successive groups of four RGB444 pixels using the table
*       selected by x and y; pack one-, two- or four-bit indices, or write
*       one index per byte for eight-bit output.
*
*   INPUTS
*       l - Destination pixel-index bytes.
*       r4g4b4 - Source RGB444 color indexes.
*       x - Horizontal dither-phase coordinate.
*       y - Vertical dither-phase coordinate.
*       len - Pixel count, required to be a nonnegative multiple of four.
*       bits - Packed index width: 1, 2, 4 or 8.
*
*   RESULT
*       None.
*
*   NOTES
*       The C implementation is compiled only when X86_ASSEMBLY is not
*       defined. Initialize cidx before rendering.
*
****************************************************************************
*
*/

void DitherLine (uae_u8 *l, uae_u16 *r4g4b4, int x, int y, uae_s16 len, int bits)
{
    uae_u8 *dith = cidx[y&3]+(x&3)*4096;
    uae_u8 d = 0;
    int bitsleft = 8;

    if (bits == 8) {
	while (len > 0) {
	    *l++ = dith[0*4096 + *r4g4b4++];
	    *l++ = dith[1*4096 + *r4g4b4++];
	    *l++ = dith[2*4096 + *r4g4b4++];
	    *l++ = dith[3*4096 + *r4g4b4++];
	    len -= 4;
	}
	return;
    }

    while (len) {
	int v;
	v = dith[0*4096 + *r4g4b4++];
	bitsleft -= bits;
	d |= (v << bitsleft);
	if (!bitsleft)
	    *l++ = d, bitsleft = 8, d = 0;

	v = dith[1*4096 + *r4g4b4++];
	bitsleft -= bits;
	d |= (v << bitsleft);
	if (!bitsleft)
	    *l++ = d, bitsleft = 8, d = 0;

	v = dith[2*4096 + *r4g4b4++];
	bitsleft -= bits;
	d |= (v << bitsleft);
	if (!bitsleft)
	    *l++ = d, bitsleft = 8, d = 0;

	v = dith[3*4096 + *r4g4b4++];
	bitsleft -= bits;
	d |= (v << bitsleft);
	if (!bitsleft)
	    *l++ = d, bitsleft = 8, d = 0;
	len -= 4;
    }
}
#endif
