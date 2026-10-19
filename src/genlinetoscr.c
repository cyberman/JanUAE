/*
 * E-UAE - The portable Amiga Emulator
 *
 * Generate pixel output code.
 *
 * (c) 2006 Richard Drummond
 */

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

/* Output for big-endian target if true, little-endian is false. */
int do_bigendian;

typedef enum
{
    DEPTH_8BPP,
    DEPTH_16BPP,
    DEPTH_32BPP,
} DEPTH_T;
#define DEPTH_MAX DEPTH_32BPP

/****i* januae-linetoscr/get_depth_str ***************************************
*
*   NAME
*       get_depth_str() -- Return the numeric suffix for a pixel depth.
*
*   SYNOPSIS
*       static const char *get_depth_str (DEPTH_T bpp);
*
*   FUNCTION
*       Map the requested output depth to the suffix used in generated
*       routine names.
*
*   INPUTS
*       bpp - Output pixel depth.
*
*   RESULT
*       The string literal "8", "16", or "32". Other values select "32".
*
****************************************************************************
*
*/

static const char *get_depth_str (DEPTH_T bpp)
{
   if (bpp == DEPTH_8BPP)
	return "8";
   else if (bpp == DEPTH_16BPP)
	return "16";
   else
	return "32";
}

/****i* januae-linetoscr/get_depth_type_str **********************************
*
*   NAME
*       get_depth_type_str() -- Return the output pixel type name.
*
*   SYNOPSIS
*       static const char *get_depth_type_str (DEPTH_T bpp);
*
*   FUNCTION
*       Select the C type emitted for destination pixels of the requested
*       depth.
*
*   INPUTS
*       bpp - Output pixel depth.
*
*   RESULT
*       The string literal "uae_u8", "uae_u16", or "uae_u32". Other values
*       select "uae_u32".
*
****************************************************************************
*
*/

static const char *get_depth_type_str (DEPTH_T bpp)
{
    if (bpp == DEPTH_8BPP)
	return "uae_u8";
    else if (bpp == DEPTH_16BPP)
	return "uae_u16";
    else
	return "uae_u32";
}


typedef enum
{
    HMODE_NORMAL,
    HMODE_DOUBLE,
    HMODE_HALVE,
} HMODE_T;
#define HMODE_MAX HMODE_HALVE

/****i* januae-linetoscr/get_hmode_str ***************************************
*
*   NAME
*       get_hmode_str() -- Return the horizontal-scaling name suffix.
*
*   SYNOPSIS
*       static const char *get_hmode_str (HMODE_T hmode);
*
*   FUNCTION
*       Select the suffix used to distinguish doubled and halved horizontal
*       output.
*
*   INPUTS
*       hmode - Horizontal pixel-scaling mode.
*
*   RESULT
*       "_stretch1" for HMODE_DOUBLE, "_shrink1" for HMODE_HALVE, or the
*       empty string otherwise.
*
****************************************************************************
*
*/

static const char *get_hmode_str (HMODE_T hmode)
{
    if (hmode == HMODE_DOUBLE)
	return "_stretch1";
   else if (hmode == HMODE_HALVE)
	return "_shrink1";
   else
	return "";
}


typedef enum
{
    CMODE_NORMAL,
    CMODE_DUALPF,
    CMODE_EXTRAHB,
    CMODE_HAM
} CMODE_T;
#define CMODE_MAX CMODE_HAM


static FILE *outfile;
static unsigned int outfile_indent = 0;

/****i* januae-linetoscr/set_outfile *****************************************
*
*   NAME
*       set_outfile() -- Select the destination for generated C source.
*
*   SYNOPSIS
*       void set_outfile (FILE *f);
*
*   FUNCTION
*       Store the stream used by the output helpers. The function does not
*       open, flush, or close the stream.
*
*   INPUTS
*       f - Writable stream that remains open while output is generated.
*
****************************************************************************
*
*/

void set_outfile (FILE *f)
{
    outfile = f;
}

/****i* januae-linetoscr/set_indent ******************************************
*
*   NAME
*       set_indent() -- Set the indentation width for generated lines.
*
*   SYNOPSIS
*       int set_indent (int indent);
*
*   FUNCTION
*       Replace the number of leading spaces emitted by outln() and outlnf().
*
*   INPUTS
*       indent - Nonnegative number of spaces to prepend to subsequent lines.
*
*   RESULT
*       The previous indentation width.
*
*   NOTES
*       A negative indent converts to a large unsigned width; the function
*       does not validate it.
*
****************************************************************************
*
*/

int set_indent (int indent)
{
    int old_indent = outfile_indent;
    outfile_indent = indent;
    return old_indent;
}

/****i* januae-linetoscr/outln ***********************************************
*
*   NAME
*       outln() -- Write an indented line of generated source.
*
*   SYNOPSIS
*       void outln (const char *s);
*
*   FUNCTION
*       Emit the current number of indentation spaces, followed by the
*       supplied text and a newline.
*
*   INPUTS
*       s - NUL-terminated text to write to the stream selected by
*       set_outfile().
*
*   NOTES
*       The output stream must be valid. Stream write errors are not
*       reported.
*
****************************************************************************
*
*/

void outln (const char *s)
{
    unsigned int i;
    for (i = 0; i < outfile_indent; i++)
	fputc (' ', outfile);
    fprintf (outfile, "%s\n", s);
}

/****i* januae-linetoscr/outlnf **********************************************
*
*   NAME
*       outlnf() -- Write a formatted line of generated source.
*
*   SYNOPSIS
*       void outlnf (const char *s, ...);
*
*   FUNCTION
*       Emit the current indentation, format the supplied arguments with
*       vfprintf(), and append a newline.
*
*   INPUTS
*       s - NUL-terminated printf-style format string.
*       ... - Values required by the format string.
*
*   NOTES
*       The output stream must be valid. Stream write errors are not
*       reported.
*
****************************************************************************
*
*/

void outlnf (const char *s, ...)
{
    va_list ap;
    unsigned int i;
    for (i = 0; i < outfile_indent; i++)
	fputc (' ', outfile);
    va_start (ap, s);
    vfprintf (outfile, s, ap);
    fputc ('\n', outfile);
}

/****i* januae-linetoscr/out_linetoscr_decl **********************************
*
*   NAME
*       out_linetoscr_decl() -- Emit a scanline converter declaration.
*
*   SYNOPSIS
*       static void out_linetoscr_decl (DEPTH_T bpp, HMODE_T hmode, int aga);
*
*   FUNCTION
*       Write the NOINLINE linetoscr function header with depth and scaling
*       suffixes; append the AGA suffix when selected.
*
*   INPUTS
*       bpp - Destination pixel depth.
*       hmode - Horizontal scaling mode.
*       aga - Nonzero to select the AGA converter name.
*
****************************************************************************
*
*/

static void out_linetoscr_decl (DEPTH_T bpp, HMODE_T hmode, int aga)
{
    outlnf ("static int NOINLINE linetoscr_%s%s%s (int spix, int dpix, int stoppos)",
	    get_depth_str (bpp),
	    get_hmode_str (hmode), aga ? "_aga" : "");
}

/****i* januae-linetoscr/out_linetoscr_do_srcpix *****************************
*
*   NAME
*       out_linetoscr_do_srcpix() -- Emit source-pixel loading statements.
*
*   SYNOPSIS
*       static void out_linetoscr_do_srcpix (DEPTH_T bpp, HMODE_T hmode, int aga, CMODE_T cmode);
*
*   FUNCTION
*       Load the current indexed pixel, applying the AGA XOR value outside
*       dual-playfield mode. HAM without AGA emits no indexed-pixel load.
*
*   INPUTS
*       bpp - Destination depth; unused by this helper.
*       hmode - Horizontal scaling; unused by this helper.
*       aga - Nonzero for AGA output.
*       cmode - Chipset color mode being generated.
*
****************************************************************************
*
*/

static void out_linetoscr_do_srcpix (DEPTH_T bpp, HMODE_T hmode, int aga, CMODE_T cmode)
{
    if (aga && cmode != CMODE_DUALPF)
	outln ( 	"    spix_val = pixdata.apixels[spix] ^ xor_val;");
    else if (cmode != CMODE_HAM)
	outln ( 	"    spix_val = pixdata.apixels[spix];");
}

/****i* januae-linetoscr/out_linetoscr_do_dstpix *****************************
*
*   NAME
*       out_linetoscr_do_dstpix() -- Emit chipset-specific destination color conversion.
*
*   SYNOPSIS
*       static void out_linetoscr_do_dstpix (DEPTH_T bpp, HMODE_T hmode, int aga, CMODE_T cmode);
*
*   FUNCTION
*       Write the HAM, dual-playfield, extra-half-bright, or normal palette
*       conversion selected by cmode and aga. AGA dual-playfield output
*       includes sprite-color selection.
*
*   INPUTS
*       bpp - Destination depth; unused by this helper.
*       hmode - Horizontal scaling; unused by this helper.
*       aga - Nonzero to generate AGA conversion.
*       cmode - Chipset color mode being generated.
*
****************************************************************************
*
*/

static void out_linetoscr_do_dstpix (DEPTH_T bpp, HMODE_T hmode, int aga, CMODE_T cmode)
{
    if (aga && cmode == CMODE_HAM)
	outln (	        "    dpix_val = CONVERT_RGB (ham_linebuf[spix]);");
    else if (cmode == CMODE_HAM)
	outln (		"    dpix_val = xcolors[ham_linebuf[spix]];");
    else if (aga && cmode == CMODE_DUALPF) {
	outln (		"    if (spriteagadpfpixels[spix]) {");
	outln (		"        dpix_val = colors_for_drawing.acolors[spriteagadpfpixels[spix]];");
	outln (		"        spriteagadpfpixels[spix] = 0;");
	outln (		"    } else {");
	outln (		"        unsigned int val = lookup[spix_val];");
	outln (		"        if (lookup_no[spix_val] == 2)");
	outln (		"            val += dblpfofs[bpldualpf2of];");
	outln (		"	 val ^= xor_val;");
	outln (		"        dpix_val = colors_for_drawing.acolors[val];");
	outln (		"    }");
    } else if (cmode == CMODE_DUALPF) {
	outln (		"    dpix_val = colors_for_drawing.acolors[lookup[spix_val]];");
    } else if (aga && cmode == CMODE_EXTRAHB) {
	outln (		"    if (spix_val >= 32 && spix_val < 64) {");
	outln (		"        unsigned int c = (colors_for_drawing.color_regs_aga[spix_val - 32] >> 1) & 0x7F7F7F;");
	outln (		"        dpix_val = CONVERT_RGB (c);");
	outln (		"    } else");
	outln (		"        dpix_val = colors_for_drawing.acolors[spix_val];");
    } else if (cmode == CMODE_EXTRAHB) {
	outln (		"    if (spix_val <= 31)");
	outln (		"        dpix_val = colors_for_drawing.acolors[spix_val];");
	outln (		"    else");
	outln (		"        dpix_val = xcolors[(colors_for_drawing.color_regs_ecs[spix_val - 32] >> 1) & 0x777];");
    } else
	outln (		"    dpix_val = colors_for_drawing.acolors[spix_val];");
}

/****i* januae-linetoscr/out_linetoscr_do_incspix ****************************
*
*   NAME
*       out_linetoscr_do_incspix() -- Emit source-pixel advancement.
*
*   SYNOPSIS
*       static void out_linetoscr_do_incspix (DEPTH_T bpp, HMODE_T hmode, int aga, CMODE_T cmode);
*
*   FUNCTION
*       Advance spix by two source pixels in halved mode or by one source
*       pixel otherwise.
*
*   INPUTS
*       bpp - Destination depth; unused by this helper.
*       hmode - Horizontal scaling mode.
*       aga - Chipset selection; unused by this helper.
*       cmode - Color mode; unused by this helper.
*
****************************************************************************
*
*/

static void out_linetoscr_do_incspix (DEPTH_T bpp, HMODE_T hmode, int aga, CMODE_T cmode)
{
    if (hmode == HMODE_HALVE)
	outln (		"    spix += 2;");
    else
	outln (		"    spix++;");
}

/****i* januae-linetoscr/out_linetoscr_mode **********************************
*
*   NAME
*       out_linetoscr_mode() -- Emit the inner scanline conversion loop.
*
*   SYNOPSIS
*       static void out_linetoscr_mode (DEPTH_T bpp, HMODE_T hmode, int aga, CMODE_T cmode);
*
*   FUNCTION
*       Write the selected color conversion and pixel writes. In 16-bit
*       non-doubled modes, handle odd alignment and pack pairs of pixels into
*       32-bit stores according to do_bigendian. Doubled modes duplicate
*       destination pixels.
*
*   INPUTS
*       bpp - Destination pixel depth.
*       hmode - Horizontal scaling mode.
*       aga - Nonzero for AGA color handling.
*       cmode - Color interpretation of source pixels.
*
*   NOTES
*       The generated code assumes its caller supplies valid source and
*       destination ranges.
*
****************************************************************************
*
*/

static void out_linetoscr_mode (DEPTH_T bpp, HMODE_T hmode, int aga, CMODE_T cmode)
{
    int old_indent = set_indent (8);

    if (aga && cmode == CMODE_DUALPF) {
	outln (        "int *lookup    = bpldualpfpri ? dblpf_ind2_aga : dblpf_ind1_aga;");
	outln (        "int *lookup_no = bpldualpfpri ? dblpf_2nd2     : dblpf_2nd1;");
    } else if (cmode == CMODE_DUALPF)
	outln (        "int *lookup = bpldualpfpri ? dblpf_ind2 : dblpf_ind1;");


    /* TODO: add support for combining pixel writes in 8-bpp modes. */

    if (bpp == DEPTH_16BPP && hmode != HMODE_DOUBLE) {
	outln (		"int rem;");
	outln (		"if (((long)&buf[dpix]) & 2) {");
	outln (		"    uae_u32 spix_val;");
	outln (		"    uae_u32 dpix_val;");

	out_linetoscr_do_srcpix (bpp, hmode, aga, cmode);
	out_linetoscr_do_dstpix (bpp, hmode, aga, cmode);
	out_linetoscr_do_incspix (bpp, hmode, aga, cmode);

	outln (		"    buf[dpix++] = dpix_val;");
	outln (		"}");
	outln (		"if (dpix >= stoppos)");
	outln (		"    return spix;");
	outln (		"rem = (((long)&buf[stoppos]) & 2);");
	outln (		"if (rem)");
	outln (		"    stoppos--;");
    }


    outln (		"while (dpix < stoppos) {");
    outln (		"    uae_u32 spix_val;");
    outln (		"    uae_u32 dpix_val;");
    outln (		"    uae_u32 out_val;");
    outln (		"");

    out_linetoscr_do_srcpix (bpp, hmode, aga, cmode);
    out_linetoscr_do_dstpix (bpp, hmode, aga, cmode);
    out_linetoscr_do_incspix (bpp, hmode, aga, cmode);

    outln (		"    out_val = dpix_val;");

    if (hmode != HMODE_DOUBLE && bpp == DEPTH_16BPP) {
	out_linetoscr_do_srcpix (bpp, hmode, aga, cmode);
	out_linetoscr_do_dstpix (bpp, hmode, aga, cmode);
	out_linetoscr_do_incspix (bpp, hmode, aga, cmode);

	if (do_bigendian)
	    outln (	"    out_val = (out_val << 16) | (dpix_val & 0xFFFF);");
	else
	    outln (	"    out_val = (out_val & 0xFFFF) | (dpix_val << 16);");
    }

    if (hmode == HMODE_DOUBLE) {
	if (bpp == DEPTH_8BPP) {
	    outln (	"    *((uae_u16 *)&buf[dpix]) = (uae_u16) out_val;");
	    outln (	"    dpix += 2;");
	} else if (bpp == DEPTH_16BPP) {
	    outln (	"    *((uae_u32 *)&buf[dpix]) = out_val;");
	    outln (	"    dpix += 2;");
	} else {
	    outln (	"    buf[dpix++] = out_val;");
	    outln (	"    buf[dpix++] = out_val;");
	}
    } else {
	if (bpp == DEPTH_16BPP) {
	    outln (	"    *((uae_u32 *)&buf[dpix]) = out_val;");
	    outln (	"    dpix += 2;");
	 } else
	    outln (	"    buf[dpix++] = out_val;");
    }

    outln (		"}");


    if (bpp == DEPTH_16BPP && hmode != HMODE_DOUBLE) {
	outln (		"if (rem) {");
	outln (		"    uae_u32 spix_val;");
	outln (		"    uae_u32 dpix_val;");

	out_linetoscr_do_srcpix (bpp, hmode, aga, cmode);
	out_linetoscr_do_dstpix (bpp, hmode, aga, cmode);
	out_linetoscr_do_incspix (bpp, hmode, aga, cmode);

	outln (		"    buf[dpix++] = dpix_val;");
	outln (		"}");
    }

    set_indent (old_indent);

    return;
}

/****i* januae-linetoscr/out_linetoscr ***************************************
*
*   NAME
*       out_linetoscr() -- Emit one scanline conversion routine.
*
*   SYNOPSIS
*       static void out_linetoscr (DEPTH_T bpp, HMODE_T hmode, int aga);
*
*   FUNCTION
*       Write a complete linetoscr function that selects HAM, dual-playfield,
*       extra-half-bright, or normal conversion at runtime and returns the
*       resulting source-pixel position. Wrap AGA variants in an AGA
*       preprocessor guard.
*
*   INPUTS
*       bpp - Destination pixel depth.
*       hmode - Horizontal scaling mode.
*       aga - Nonzero to emit an AGA-guarded routine.
*
****************************************************************************
*
*/

static void out_linetoscr (DEPTH_T bpp, HMODE_T hmode, int aga)
{
    if (aga)
	outln  ("#ifdef AGA");

    out_linetoscr_decl (bpp, hmode, aga);
    outln  (	"{");

    outlnf (	"    %s *buf = (%s *) xlinebuffer;", get_depth_type_str (bpp), get_depth_type_str (bpp));
    if (aga)
	outln (	"    uae_u8 xor_val = (uae_u8)(dp_for_drawing->bplcon4 >> 8);");
    outln  (	"");

    outln  (	"    if (dp_for_drawing->ham_seen) {");
    out_linetoscr_mode (bpp, hmode, aga, CMODE_HAM);
    outln  (	"    } else if (bpldualpf) {");
    out_linetoscr_mode (bpp, hmode, aga, CMODE_DUALPF);
    outln  (	"    } else if (bplehb) {");
    out_linetoscr_mode (bpp, hmode, aga, CMODE_EXTRAHB);
    outln  (	"    } else {");
    out_linetoscr_mode (bpp, hmode, aga, CMODE_NORMAL);

    outln  (	"    }\n");
    outln  (	"    return spix;");
    outln  (	"}");

    if (aga)
	outln (	"#endif");
    outln  (	"");
}

/****i* januae-linetoscr/main ************************************************
*
*   NAME
*       main() -- Generate the scanline conversion source file.
*
*   SYNOPSIS
*       int main (int argc, char *argv[]);
*
*   FUNCTION
*       Write all depth, chipset, and horizontal-scaling variants to standard
*       output. The exact -b option selects big-endian packing of paired
*       16-bit pixels; other arguments are ignored.
*
*   INPUTS
*       argc - Number of command-line arguments.
*       argv - Argument vector; argv[1] through argv[argc - 1] are inspected.
*
*   RESULT
*       Zero after generating the output.
*
*   NOTES
*       Output write failures are not checked.
*
****************************************************************************
*
*/

int main (int argc, char *argv[])
{
   DEPTH_T bpp;
   int aga;
   HMODE_T hmode;
   int i;

    do_bigendian = 0;

    for (i = 1; i < argc; i++) {
	if (argv[i][0] != '-')
	    continue;
	if (argv[i][1] == 'b' && argv[i][2] == '\0')
	    do_bigendian = 1;
    }

   set_outfile (stdout);

   outln ("/*");
   outln (" * E-UAE - The portable Amiga emulator.");
   outln (" *");
   outln (" * This file was generated by genlinetoscr. Don't edit.");
   outln (" */");
   outln ("");

   for (bpp = DEPTH_8BPP; bpp <= DEPTH_MAX; bpp++) {
	for (aga = 0; aga <= 1 ; aga++) {
	    for (hmode = HMODE_NORMAL; hmode <= HMODE_MAX; hmode++)
		out_linetoscr (bpp, hmode, aga);
	}
    }
    return 0;
}
