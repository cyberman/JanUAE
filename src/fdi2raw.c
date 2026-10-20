/*

  FDI to raw bit stream	converter
  Copyright (c)	2001 by	Toni Wilen <twilen@arabuusimiehet.com>
  FDI 2.0 support
  Copyright (c)	2003-2004 by Toni Wilen	<twilen@arabuusimiehet.com>
		    and	Vincent	Joguin

  FDI format created by	Vincent	"ApH" Joguin

  Tiny changes - function type fixes, multiple drives, addition	of
  get_last_head	and C++	callability - by Thomas	Harte, 2001,
  T.Harte@excite.co.uk


  This program is free software; you can redistribute it and/or	modify it
  under	the terms of the GNU General Public License as published by the	Free
  Software Foundation; either version 2	of the License,	or (at your option)
  any later version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY;	without	even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A	PARTICULAR PURPOSE.  See the GNU General Public	License	for
  more details.

  You should have received a copy of the GNU General Public License along
  with this program; if	not, write to the Free Software	Foundation, Inc.,
  59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

*/

#ifdef FDI2RAW

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* IF UAE */
#include "sysconfig.h"
#include "sysdeps.h"
#include "zfile.h"
/* ELSE	*/
//#include "types.h"

#include "fdi2raw.h"

#undef DEBUG
#define VERBOSE
#undef VERBOSE

#include <assert.h>

#ifdef DEBUG
static const char *datalog (const uae_u8 *src, unsigned int len)
{
	static char buf[1000];
	static int offset;
	int i =	0, offset2;

	offset2	= offset;
	buf[offset++]='\'';
	while (len--) {
		sprintf	(buf + offset, "%02.2X", src[i]);
		offset += 2;
		i++;
		if (i >	10) break;
	}
	buf[offset++]='\'';
	buf[offset++] =	0;
	if (offset >= 900) offset = 0;
	return buf + offset2;
}
#else
/****i* januae-fdi/datalog
*
*   NAME
*       datalog() -- Provide the non-debug diagnostic formatter.
*
*   SYNOPSIS
*       static const char *datalog (const uae_u8 *src, unsigned int len);
*
*   FUNCTION
*       Return the empty string in builds of this file; DEBUG is explicitly
*       undefined.
*
*   INPUTS
*       src - Source bytes or compressed pulse stream; caller-owned.
*       len - Length of the indicated input or output sequence.
*
*   RESULT
*       Pointer to a static empty string.
*
***************************************************************************
*
*/

static const char *datalog (const uae_u8 *src, unsigned int len) { return ""; }
#endif

#ifdef DEBUG
#define	debuglog write_log
#else
#define debuglog(...) do { ; } while (0)
#endif
#ifdef VERBOSE
#define	outlog write_log
#else
#define outlog(...) do { ; } while (0)
#endif

#ifdef DEBUG
static int fdi_allocated;
static void fdi_free (void *p)
{
	int size;
	if (!p)
		return;
	size = ((int*)p)[-1];
	fdi_allocated -= size;
	write_log ("%d freed (%d)\n", size, fdi_allocated);
	free ((int*)p -	1);
}
static void *fdi_malloc	(int size)
{
	void *p	= xmalloc (size	+ sizeof (int));
	((int*)p)[0] = size;
	fdi_allocated += size;
	write_log ("%d allocated (%d)\n", size,	fdi_allocated);
	return (int*)p + 1;
}
#else
#define	fdi_free free
#define	fdi_malloc xmalloc
#endif

#define	MAX_SRC_BUFFER 4194304
#define	MAX_DST_BUFFER 40000
#define	MAX_MFM_SYNC_BUFFER 60000
#define	MAX_TIMING_BUFFER 400000
#define	MAX_TRACKS 166

struct fdi_cache {
	uae_u32 *avgp, *minp, *maxp;
	uae_u8 *idxp;
	unsigned int avg_free, idx_free, min_free, max_free;
	uae_u32 totalavg, pulses, maxidx, indexoffset;
	unsigned int weakbits;
	int lowlevel;
};

struct fdi {
	uae_u8 *track_src_buffer;
	uae_u8 *track_src;
	int track_src_len;
	uae_u8 *track_dst_buffer;
	uae_u8 *track_dst;
	uae_u16	*track_dst_buffer_timing;
	uae_u8 track_len;
	uae_u8 track_type;
	unsigned int current_track;
	unsigned int last_track;
	unsigned int last_head;
	unsigned int rotation_speed;
	unsigned int bit_rate;
	int disk_type;
	int write_protect;
	int err;
	uae_u8 header[2048];
	unsigned int track_offsets[MAX_TRACKS];
	struct zfile *file;
	int out;
	int mfmsync_offset;
	int *mfmsync_buffer;
	/* sector described only */
	int index_offset;
	int encoding_type;
	/* bit handling	*/
	int nextdrop;
	struct fdi_cache cache[MAX_TRACKS];
};

#define	get_u32(x) ((((x)[0])<<24)|(((x)[1])<<16)|(((x)[2])<<8)|((x)[3]))
#define	get_u24(x) ((((x)[0])<<16)|(((x)[1])<<8)|((x)[2]))
/****i* januae-fdi/put_u32
*
*   NAME
*       put_u32() -- Store a big-endian 32-bit value.
*
*   SYNOPSIS
*       STATIC_INLINE void put_u32 (uae_u8 *d, uae_u32 v);
*
*   FUNCTION
*       Write the four bytes of v to the caller-supplied output buffer.
*
*   INPUTS
*       d - Output buffer, large enough for four bytes or the decoded data.
*       v - Input value to encode.
*
***************************************************************************
*
*/

STATIC_INLINE void put_u32 (uae_u8 *d, uae_u32 v)
{
	d[0] = v >> 24;
	d[1] = v >> 16;
	d[2] = v >> 8;
	d[3] = v;
}

struct node {
	uae_u16	v;
	struct node *left;
	struct node *right;
};
typedef	struct node NODE;

static uae_u8 temp, temp2;

/****i* januae-fdi/expand_tree
*
*   NAME
*       expand_tree() -- Decode a Huffman tree shape.
*
*   SYNOPSIS
*       static const uae_u8 *expand_tree (const uae_u8 *stream, NODE *node);
*
*   FUNCTION
*       Recursively construct left and right child nodes from the shared bit
*       reader; leaf nodes have no allocated children. The caller frees the
*       allocated tree.
*
*   INPUTS
*       stream - FDI input cursor in a valid encoded stream.
*       node - Node within the allocated Huffman tree.
*
*   RESULT
*       Pointer to the first unread compressed input byte.
*
***************************************************************************
*
*/

static const uae_u8 *expand_tree (const uae_u8 *stream, NODE *node)
{
	if (temp & temp2) {
		fdi_free (node->left);
		node->left = 0;
		fdi_free (node->right);
		node->right = 0;
		temp2 >>= 1;
		if (!temp2) {
			temp = *stream++;
			temp2 =	0x80;
		}
		return stream;
	} else {
		const uae_u8 *stream_temp;
		temp2 >>= 1;
		if (!temp2) {
			temp = *stream++;
			temp2 =	0x80;
		}
		node->left = fdi_malloc	(sizeof	(NODE));
		memset (node->left, 0, sizeof (NODE));
		stream_temp = expand_tree (stream, node->left);
		node->right = fdi_malloc (sizeof (NODE));
		memset (node->right, 0,	sizeof (NODE));
		return expand_tree (stream_temp, node->right);
	}
}

/****i* januae-fdi/values_tree8
*
*   NAME
*       values_tree8() -- Assign eight-bit Huffman leaf values.
*
*   SYNOPSIS
*       static const uae_u8 *values_tree8 (const uae_u8 *stream, NODE *node);
*
*   FUNCTION
*       Visit the decoded tree in traversal order and consume one byte for each
*       leaf.
*
*   INPUTS
*       stream - FDI input cursor in a valid encoded stream.
*       node - Node within the allocated Huffman tree.
*
*   RESULT
*       Pointer past the last consumed value byte.
*
***************************************************************************
*
*/

static const uae_u8 *values_tree8 (const uae_u8 *stream, NODE *node)
{
	if (node->left == 0) {
		node->v	= *stream++;
		return stream;
	} else {
		const uae_u8 *stream_temp = values_tree8 (stream, node->left);
		return values_tree8 (stream_temp, node->right);
	}
}

/****i* januae-fdi/values_tree16
*
*   NAME
*       values_tree16() -- Assign sixteen-bit Huffman leaf values.
*
*   SYNOPSIS
*       static const uae_u8 *values_tree16 (const uae_u8 *stream, NODE *node);
*
*   FUNCTION
*       Visit the decoded tree and consume a big-endian two-byte value for each
*       leaf.
*
*   INPUTS
*       stream - FDI input cursor in a valid encoded stream.
*       node - Node within the allocated Huffman tree.
*
*   RESULT
*       Pointer past the last consumed value.
*
***************************************************************************
*
*/

static const uae_u8 *values_tree16 (const uae_u8 *stream, NODE *node)
{
	if (node->left == 0) {
		uae_u16	high_8_bits = (*stream++) << 8;
		node->v	= high_8_bits |	(*stream++);
		return stream;
	} else {
		const uae_u8 *stream_temp = values_tree16 (stream, node->left);
		return values_tree16 (stream_temp, node->right);
	}
}

/****i* januae-fdi/free_nodes
*
*   NAME
*       free_nodes() -- Release a decoded Huffman subtree.
*
*   SYNOPSIS
*       static void free_nodes (NODE *node);
*
*   FUNCTION
*       Recursively free child nodes and the supplied root using the FDI
*       allocator. A null node is accepted.
*
*   INPUTS
*       node - Node within the allocated Huffman tree.
*
***************************************************************************
*
*/

static void free_nodes (NODE *node)
{
	if (node) {
		free_nodes (node->left);
		free_nodes (node->right);
		fdi_free (node);
	}
}

/****i* januae-fdi/sign_extend16
*
*   NAME
*       sign_extend16() -- Sign-extend an encoded 16-bit value.
*
*   SYNOPSIS
*       STATIC_INLINE uae_u32 sign_extend16 (uae_u32 v);
*
*   FUNCTION
*       Copy bit 15 into the upper 16 bits of the unsigned integer without
*       masking the lower value first.
*
*   INPUTS
*       v - Input value to encode.
*
*   RESULT
*       Unsigned representation of the sign-extended value.
*
***************************************************************************
*
*/

STATIC_INLINE uae_u32 sign_extend16 (uae_u32 v)
{
	if (v &	0x8000)
		v |= 0xffff0000;
	return v;
}

/****i* januae-fdi/sign_extend8
*
*   NAME
*       sign_extend8() -- Sign-extend an encoded eight-bit value.
*
*   SYNOPSIS
*       STATIC_INLINE uae_u32 sign_extend8 (uae_u32 v);
*
*   FUNCTION
*       Copy bit 7 into the upper 24 bits of the unsigned integer without masking
*       the lower value first.
*
*   INPUTS
*       v - Input value to encode.
*
*   RESULT
*       Unsigned representation of the sign-extended value.
*
***************************************************************************
*
*/

STATIC_INLINE uae_u32 sign_extend8 (uae_u32 v)
{
	if (v &	0x80)
		v |= 0xffffff00;
	return v;
}

/****i* januae-fdi/fdi_decode
*
*   NAME
*       fdi_decode() -- Decompress Huffman-coded pulse fields.
*
*   SYNOPSIS
*       static void fdi_decode (const uae_u8 *stream, unsigned int size, uae_u8
*       *out);
*
*   FUNCTION
*       Read successive packed substreams, build and free their Huffman trees and
*       combine shifted 8- or 16-bit values into a 32-bit output array. The
*       source byte count is not independently bounded.
*
*   INPUTS
*       stream - FDI input cursor in a valid encoded stream.
*       size - Number of decoded pulse entries or bytes as defined by the
*       routine.
*       out - Caller-provided decoded data or cell-timing buffer.
*
***************************************************************************
*
*/

static void fdi_decode (const uae_u8 *stream, unsigned int size, uae_u8 *out)
{
	uae_u8 sign_extend, sixteen_bit, sub_stream_shift;
	NODE root;
	NODE *current_node;

	memset (out, 0,	size * 4);
	sub_stream_shift = 1;
	while (sub_stream_shift) {
		unsigned int i;

		//sub-stream header decode
		sign_extend = *stream++;
		sub_stream_shift = sign_extend & 0x7f;
		sign_extend &= 0x80;
		sixteen_bit = (*stream++) & 0x80;

		//huffman tree architecture decode
		temp = *stream++;
		temp2 =	0x80;
		stream = expand_tree (stream, &root);
		if (temp2 == 0x80)
			stream--;

		//huffman output values	decode
		if (sixteen_bit)
			stream = values_tree16 (stream,	&root);
		else
			stream = values_tree8 (stream, &root);

		//sub-stream data decode
		temp2 =	0;
		for (i = 0; i <	size; i++) {
			uae_u32	v;
			uae_u8 decode =	1;
			current_node = &root;
			while (decode) {
				if (current_node->left == 0) {
					decode = 0;
				} else {
					temp2 >>= 1;
					if (!temp2) {
						temp2 =	0x80;
						temp = *stream++;
					}
					if (temp & temp2)
						current_node = current_node->right;
					else
						current_node = current_node->left;
				}
			}
			v = ((uae_u32*)out)[i];
			if (sign_extend) {
				if (sixteen_bit)
					v |= sign_extend16 (current_node->v) <<	sub_stream_shift;
				else
					v |= sign_extend8 (current_node->v) << sub_stream_shift;
			} else {
				v |= current_node->v <<	sub_stream_shift;
			}
			((uae_u32*)out)[i] = v;
		}
		free_nodes (root.left);
		free_nodes (root.right);
	}
}


/****i* januae-fdi/decode_raw_track
*
*   NAME
*       decode_raw_track() -- Copy a raw FDI track bitstream.
*
*   SYNOPSIS
*       static unsigned int decode_raw_track (FDI *fdi);
*
*   FUNCTION
*       Read the track's 32-bit bit count and copy the corresponding bytes into
*       the decoder output. The source cursor advances by the copied byte count.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
*   RESULT
*       Advertised bit count of the track.
*
***************************************************************************
*
*/

static unsigned int decode_raw_track (FDI *fdi)
{
	unsigned int size = get_u32(fdi->track_src);
	memcpy (fdi->track_dst,	fdi->track_src,	(size +	7) >> 3);
	fdi->track_src += (size	+ 7) >>	3;
	return size;
}

/****i* januae-fdi/zxx
*
*   NAME
*       zxx() -- Handle an unrecognized track type.
*
*   SYNOPSIS
*       static void zxx (FDI *fdi);
*
*   FUNCTION
*       Emit an optional verbose diagnostic. This handler does not itself set the
*       decoder error flag.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void zxx	(FDI *fdi)
{
	outlog ("track %d: unknown track type 0x%02.2X\n", fdi->current_track, fdi->track_type);
//	return -1;
}
/****i* januae-fdi/zyy
*
*   NAME
*       zyy() -- Handle an unsupported track type.
*
*   SYNOPSIS
*       static void zyy (FDI *fdi);
*
*   FUNCTION
*       Emit an optional verbose diagnostic. This handler does not itself set the
*       decoder error flag.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void zyy	(FDI *fdi)
{
	outlog ("track %d: unsupported track type 0x%02.2X\n", fdi->current_track, fdi->track_type);
//	return -1;
}
/****i* januae-fdi/track_empty
*
*   NAME
*       track_empty() -- Handle an empty track descriptor.
*
*   SYNOPSIS
*       static void track_empty (FDI *fdi);
*
*   FUNCTION
*       Leave the output and decoder state unchanged.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void track_empty	(FDI *fdi)
{
//	return 0;
}

/****i* januae-fdi/dxx
*
*   NAME
*       dxx() -- Reject an unknown sector-command type.
*
*   SYNOPSIS
*       static void dxx (FDI *fdi);
*
*   FUNCTION
*       Emit an optional diagnostic and set the decoder error flag.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void dxx	(FDI *fdi)
{
	outlog ("\ntrack %d: unknown sector described type 0x%02.2X\n",	fdi->current_track, fdi->track_type);
	fdi->err = 1;
}
/****i* januae-fdi/dyy
*
*   NAME
*       dyy() -- Reject an unsupported sector-command type.
*
*   SYNOPSIS
*       static void dyy (FDI *fdi);
*
*   FUNCTION
*       Emit an optional diagnostic and set the decoder error flag.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void dyy	(FDI *fdi)
{
	outlog ("\ntrack %d: unsupported sector	described 0x%02.2X\n", fdi->current_track, fdi->track_type);
	fdi->err = 1;
}
/****i* januae-fdi/add_mfm_sync_bit
*
*   NAME
*       add_mfm_sync_bit() -- Record an MFM clock-bit position.
*
*   SYNOPSIS
*       static void add_mfm_sync_bit (FDI *fdi);
*
*   FUNCTION
*       Append the current output position to the sync-position array and advance
*       the bit cursor, unless a pending drop suppresses it. Error conditions set
*       the decoder flag.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void add_mfm_sync_bit (FDI *fdi)
{
	if (fdi->nextdrop) {
		fdi->nextdrop =	0;
		return;
	}
	fdi->mfmsync_buffer[fdi->mfmsync_offset++] = fdi->out;
	if (fdi->out ==	0) {
		outlog ("illegal position for mfm sync bit, offset=%d\n",fdi->out);
		fdi->err = 1;
	}
	if (fdi->mfmsync_offset	>= MAX_MFM_SYNC_BUFFER)	{
		fdi->mfmsync_offset = 0;
		outlog ("mfmsync buffer	overflow\n");
		fdi->err = 1;
	}
	fdi->out++;
}

#define	BIT_BYTEOFFSET ((fdi->out) >> 3)
#define	BIT_BITOFFSET (7-((fdi->out)&7))

/****i* januae-fdi/bit_add
*
*   NAME
*       bit_add() -- Append one bit to the decoded track.
*
*   SYNOPSIS
*       static void bit_add (FDI *fdi, int bit);
*
*   FUNCTION
*       Write the requested bit at the current output cursor, then advance it. A
*       pending drop suppresses the write; overflow marks an error and resets the
*       cursor to one.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       bit - Zero or nonzero data bit to append.
*
***************************************************************************
*
*/

static void bit_add (FDI *fdi, int bit)
{
	if (fdi->nextdrop) {
		fdi->nextdrop =	0;
		return;
	}
	fdi->track_dst[BIT_BYTEOFFSET] &= ~(1 << BIT_BITOFFSET);
	if (bit)
		fdi->track_dst[BIT_BYTEOFFSET] |= (1 <<	BIT_BITOFFSET);
	fdi->out++;
	if (fdi->out >=	MAX_DST_BUFFER * 8) {
		outlog ("destination buffer overflow\n");
		fdi->err = 1;
		fdi->out = 1;
	}
}
/****i* januae-fdi/bit_mfm_add
*
*   NAME
*       bit_mfm_add() -- Append a bit with MFM clock tracking.
*
*   SYNOPSIS
*       static void bit_mfm_add (FDI *fdi, int bit);
*
*   FUNCTION
*       Record the clock-bit position and then emit the data bit using the
*       pending-drop rules.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       bit - Zero or nonzero data bit to append.
*
***************************************************************************
*
*/

static void bit_mfm_add	(FDI *fdi, int bit)
{
	add_mfm_sync_bit (fdi);
	bit_add	(fdi, bit);
}
/****i* januae-fdi/bit_drop_next
*
*   NAME
*       bit_drop_next() -- Suppress the next output bit.
*
*   SYNOPSIS
*       static void bit_drop_next (FDI *fdi);
*
*   FUNCTION
*       Mark one pending bit for omission; a prior dedrop request cancels the
*       operation.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void bit_drop_next (FDI *fdi)
{
	if (fdi->nextdrop > 0) {
		outlog ("multiple bit_drop_next() called");
	} else if (fdi->nextdrop < 0) {
		fdi->nextdrop =	0;
		debuglog (":DNN:");
		return;
	}
	debuglog (":DN:");
	fdi->nextdrop =	1;
}

/****i* januae-fdi/bit_dedrop
*
*   NAME
*       bit_dedrop() -- Cancel the next drop request.
*
*   SYNOPSIS
*       static void bit_dedrop (FDI *fdi);
*
*   FUNCTION
*       Set the pending-drop state to the sentinel that makes the following
*       bit_drop_next() ineffective.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void bit_dedrop (FDI *fdi)
{
	if (fdi->nextdrop) {
		outlog ("bit_drop_next called before bit_dedrop");
	}
	fdi->nextdrop =	-1;
	debuglog (":BDD:");
}

/****i* januae-fdi/byte_add
*
*   NAME
*       byte_add() -- Append an encoded byte as raw bits.
*
*   SYNOPSIS
*       static void byte_add (FDI *fdi, uae_u8 v);
*
*   FUNCTION
*       Append the eight bits of v, most significant first.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       v - Input value to encode.
*
***************************************************************************
*
*/

static void byte_add (FDI *fdi,	uae_u8 v)
{
	int i;
	for (i = 7; i >= 0; i--)
		bit_add	(fdi, v	& (1 <<	i));
}
/****i* januae-fdi/word_add
*
*   NAME
*       word_add() -- Append an encoded big-endian word.
*
*   SYNOPSIS
*       static void word_add (FDI *fdi, uae_u16 v);
*
*   FUNCTION
*       Append the high and low bytes of v through the raw-bit writer.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       v - Input value to encode.
*
***************************************************************************
*
*/

static void word_add (FDI *fdi,	uae_u16	v)
{
	byte_add (fdi, (uae_u8)(v >> 8));
	byte_add (fdi, (uae_u8)v);
}
/****i* januae-fdi/byte_mfm_add
*
*   NAME
*       byte_mfm_add() -- Append an unencoded byte as MFM data bits.
*
*   SYNOPSIS
*       static void byte_mfm_add (FDI *fdi, uae_u8 v);
*
*   FUNCTION
*       For each data bit, track its preceding MFM clock position and append its
*       value.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       v - Input value to encode.
*
***************************************************************************
*
*/

static void byte_mfm_add (FDI *fdi, uae_u8 v)
{
	int i;
	for (i = 7; i >= 0; i--)
		bit_mfm_add (fdi, v & (1 << i));
}
/****i* januae-fdi/bytes_mfm_add
*
*   NAME
*       bytes_mfm_add() -- Append a repeated MFM data byte.
*
*   SYNOPSIS
*       static void bytes_mfm_add (FDI *fdi, uae_u8 v, unsigned int len);
*
*   FUNCTION
*       Encode the supplied byte len times using the MFM-aware bit writer.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       v - Input value to encode.
*       len - Length of the indicated input or output sequence.
*
***************************************************************************
*
*/

static void bytes_mfm_add (FDI *fdi, uae_u8 v, unsigned int len)
{
	unsigned int i;
	for (i = 0; i <	len; i++) byte_mfm_add (fdi, v);
}
/****i* januae-fdi/word_post_mfm_add
*
*   NAME
*       word_post_mfm_add() -- Re-encode an MFM word's data bits.
*
*   SYNOPSIS
*       static void word_post_mfm_add (FDI *fdi, uae_u16 v);
*
*   FUNCTION
*       Process bits 14, 12 through 0 of the supplied word, replacing clock cells
*       according to the current output context.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       v - Input value to encode.
*
***************************************************************************
*
*/

static void word_post_mfm_add (FDI *fdi, uae_u16 v)
{
	int i;
	for (i = 14; i >= 0; i -= 2)
		bit_mfm_add (fdi, v & (1 << i));
}

/****i* januae-fdi/s00
*
*   NAME
*       s00() -- Emit sector-command zero bit.
*
*   SYNOPSIS
*       static void s00(FDI *fdi);
*
*   FUNCTION
*       Append a single zero bit to the current track.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s00(FDI *fdi) { bit_add (fdi, 0); }
/****i* januae-fdi/s01
*
*   NAME
*       s01() -- Emit sector-command one bit.
*
*   SYNOPSIS
*       static void s01(FDI *fdi);
*
*   FUNCTION
*       Append a single one bit to the current track.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s01(FDI *fdi) { bit_add (fdi, 1); }
/****i* januae-fdi/s02
*
*   NAME
*       s02() -- Emit the Amiga MFM sync word.
*
*   SYNOPSIS
*       static void s02(FDI *fdi);
*
*   FUNCTION
*       Append the raw 0x4489 word to the current track.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s02(FDI *fdi) { word_add (fdi, 0x4489); }
/****i* januae-fdi/s03
*
*   NAME
*       s03() -- Emit the IBM index sync word.
*
*   SYNOPSIS
*       static void s03(FDI *fdi);
*
*   FUNCTION
*       Append the raw 0x5224 word to the current track.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s03(FDI *fdi) { word_add (fdi, 0x5224); }
/****i* januae-fdi/s04
*
*   NAME
*       s04() -- Register a sector-command MFM sync cell.
*
*   SYNOPSIS
*       static void s04(FDI *fdi);
*
*   FUNCTION
*       Record one clock-bit position and advance the current bit cursor.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s04(FDI *fdi) { add_mfm_sync_bit (fdi); }
/****i* januae-fdi/s08
*
*   NAME
*       s08() -- Expand a repeated encoded byte.
*
*   SYNOPSIS
*       static void s08(FDI *fdi);
*
*   FUNCTION
*       Read a count and encoded byte from the track stream; count zero
*       represents 256 bytes. Append the byte without further MFM encoding.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s08(FDI *fdi)
{
	unsigned int bytes = *fdi->track_src++;
	uae_u8 byte = *fdi->track_src++;
	if (bytes == 0)	bytes =	256;
	debuglog ("s08:len=%d,data=%02.2X",bytes,byte);
	while(bytes--) byte_add	(fdi, byte);
}
/****i* januae-fdi/s09
*
*   NAME
*       s09() -- Expand a repeated unencoded byte.
*
*   SYNOPSIS
*       static void s09(FDI *fdi);
*
*   FUNCTION
*       Read a count and data byte, suppress the next clock bit and MFM-encode
*       the repeated data. Zero count means 256 bytes.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s09(FDI *fdi)
{
	unsigned int bytes = *fdi->track_src++;
	uae_u8 byte = *fdi->track_src++;
	if (bytes == 0)	bytes =	256;
	bit_drop_next (fdi);
	debuglog ("s09:len=%d,data=%02.2X",bytes,byte);
	while(bytes--) byte_mfm_add (fdi, byte);
}
/****i* januae-fdi/s0a
*
*   NAME
*       s0a() -- Copy a short raw-bit field.
*
*   SYNOPSIS
*       static void s0a(FDI *fdi);
*
*   FUNCTION
*       Read the 16-bit bit count and copy that many packed bits from the
*       sector-command stream.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s0a(FDI *fdi)
{
	int i, bits = (fdi->track_src[0] << 8) | fdi->track_src[1];
	uae_u8 b;
	fdi->track_src += 2;
	debuglog ("s0a:bits=%d,data=%s", bits, datalog (fdi->track_src, (bits + 7) / 8));
	while (bits >= 8) {
		byte_add (fdi, *fdi->track_src++);
		bits -=	8;
	}
	if (bits > 0) {
		i = 7;
		b = *fdi->track_src++;
		while (bits--) {
			bit_add	(fdi, b	& (1 <<	i));
			i--;
		}
	}
}
/****i* januae-fdi/s0b
*
*   NAME
*       s0b() -- Copy a long raw-bit field.
*
*   SYNOPSIS
*       static void s0b(FDI *fdi);
*
*   FUNCTION
*       Read a 16-bit field length, add 65536 bits and append that many packed
*       raw bits.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s0b(FDI *fdi)
{
	int i, bits = ((fdi->track_src[0] << 8)	| fdi->track_src[1]) + 65536;
	uae_u8 b;
	fdi->track_src += 2;
	debuglog ("s0b:bits=%d,data=%s", bits, datalog (fdi->track_src, (bits + 7) / 8));
	while (bits >= 8) {
		byte_add (fdi, *fdi->track_src++);
		bits -=	8;
	}
	if (bits > 0) {
		i = 7;
		b = *fdi->track_src++;
		while (bits--) {
			bit_add	(fdi, b	& (1 <<	i));
			i--;
		}
	}
}
/****i* januae-fdi/s0c
*
*   NAME
*       s0c() -- MFM-encode a short bit field.
*
*   SYNOPSIS
*       static void s0c(FDI *fdi);
*
*   FUNCTION
*       Read a 16-bit bit count, request a clock-cell drop and MFM-encode that
*       many data bits.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s0c(FDI *fdi)
{
	int i, bits = (fdi->track_src[0] << 8) | fdi->track_src[1];
	uae_u8 b;
	fdi->track_src += 2;
	bit_drop_next (fdi);
	debuglog ("s0c:bits=%d,data=%s", bits, datalog (fdi->track_src, (bits + 7) / 8));
	while (bits >= 8) {
		byte_mfm_add (fdi, *fdi->track_src++);
		bits -=	8;
	}
	if (bits > 0) {
		i = 7;
		b = *fdi->track_src++;
		while(bits--) {
			bit_mfm_add (fdi, b & (1 << i));
			i--;
		}
	}
}
/****i* januae-fdi/s0d
*
*   NAME
*       s0d() -- MFM-encode a long bit field.
*
*   SYNOPSIS
*       static void s0d(FDI *fdi);
*
*   FUNCTION
*       Read a 16-bit bit count, add 65536, request a clock-cell drop and
*       MFM-encode the supplied data bits.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s0d(FDI *fdi)
{
	int i, bits = ((fdi->track_src[0] << 8)	| fdi->track_src[1]) + 65536;
	uae_u8 b;
	fdi->track_src += 2;
	bit_drop_next (fdi);
	debuglog ("s0d:bits=%d,data=%s", bits, datalog (fdi->track_src, (bits + 7) / 8));
	while (bits >= 8) {
		byte_mfm_add (fdi, *fdi->track_src++);
		bits -=	8;
	}
	if (bits > 0) {
		i = 7;
		b = *fdi->track_src++;
		while(bits--) {
			bit_mfm_add (fdi, b & (1 << i));
			i--;
		}
	}
}

/* ***** */
/* AMIGA */
/* ***** */

/* just	for testing integrity of Amiga sectors */

/****i* januae-fdi/rotateonebit
*
*   NAME
*       rotateonebit() -- Rotate a byte range by a bit shift.
*
*   SYNOPSIS
*       static void rotateonebit (uae_u8 *start, uae_u8 *end, unsigned int
*       shift);
*
*   FUNCTION
*       Shift each byte to the left while importing high bits from the following
*       byte, including a read beyond the supplied end pointer. Shift zero is a
*       no-op.
*
*   INPUTS
*       start - Start of a writable byte region.
*       end - Last byte of the region; the routine also reads its successor.
*       shift - Bit shift applied to successive bytes.
*
***************************************************************************
*
*/

static void rotateonebit (uae_u8 *start, uae_u8	*end, unsigned int shift)
{
	if (shift == 0)
		return;
	while (start <=	end) {
		start[0] <<= shift;
		start[0] |= start[1] >>	(8 - shift);
		start++;
	}
}

static int check_offset;
/****i* januae-fdi/getmfmword
*
*   NAME
*       getmfmword() -- Read a possibly bit-shifted MFM word.
*
*   SYNOPSIS
*       static uae_u16 getmfmword (const uae_u8 *mbuf);
*
*   FUNCTION
*       Read two or three source bytes as directed by the shared check_offset and
*       assemble a 16-bit word.
*
*   INPUTS
*       mbuf - Source MFM bytes or output MFM word buffer as used by this
*       function.
*
*   RESULT
*       MFM word at the current alignment.
*
***************************************************************************
*
*/

static uae_u16 getmfmword (const uae_u8 *mbuf)
{
	uae_u32	v;

	v = (mbuf[0] <<	8) | (mbuf[1] << 0);
	if (check_offset == 0)
		return v;
	v <<= 8;
	v |= mbuf[2];
	v >>= check_offset;
	return v;
}

#define	MFMMASK	0x55555555
/****i* januae-fdi/getmfmlong
*
*   NAME
*       getmfmlong() -- Combine two MFM words into data positions.
*
*   SYNOPSIS
*       static uae_u32 getmfmlong (const uae_u8 *mbuf);
*
*   FUNCTION
*       Read two adjacent aligned MFM words and retain only odd bit positions
*       using MFMMASK.
*
*   INPUTS
*       mbuf - Source MFM bytes or output MFM word buffer as used by this
*       function.
*
*   RESULT
*       Masked 32-bit MFM word.
*
***************************************************************************
*
*/

static uae_u32 getmfmlong (const uae_u8 *mbuf)
{
	return ((getmfmword (mbuf) << 16) | getmfmword (mbuf + 2)) & MFMMASK;
}

/****i* januae-fdi/amiga_check_track
*
*   NAME
*       amiga_check_track() -- Inspect decoded Amiga sectors.
*
*   SYNOPSIS
*       static int amiga_check_track (FDI *fdi);
*
*   FUNCTION
*       Scan the generated track for sector syncs, headers and data checksums,
*       collecting the presence of eleven sectors. This checker is not invoked by
*       the current loadtrack path.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
*   RESULT
*       Nonzero if the expected sectors pass its checks; zero otherwise.
*
***************************************************************************
*
*/

static int amiga_check_track (FDI *fdi)
{
	int i, j, secwritten = 0;
	int fwlen = fdi->out / 8;
	int length = 2 * fwlen;
	int drvsec = 11;
	uae_u32	odd, even, chksum, id, dlong;
	uae_u8 *secdata;
	uae_u8 secbuf[544];
	uae_u8 bigmfmbuf[60000];
	uae_u8 *mbuf, *mbuf2, *mend;
	char sectable[22];
	uae_u8 *raw = fdi->track_dst_buffer;
	int slabel, off;
	int ok = 1;

	memset (bigmfmbuf, 0, sizeof (bigmfmbuf));
	mbuf = bigmfmbuf;
	check_offset = 0;
	for (i = 0; i <	(fdi->out + 7) / 8; i++)
		*mbuf++	= raw[i];
	off = fdi->out & 7;
#if 1
	if (off	> 0) {
		mbuf--;
		*mbuf &= ~((1 << (8 - off)) - 1);
	}
	j = 0;
	while (i < (fdi->out + 7) / 8 +	600) {
		*mbuf++	|= (raw[j] >> off) | ((raw[j + 1]) << (8 - off));
		j++;
		i++;
	}
#endif
	mbuf = bigmfmbuf;

	memset (sectable, 0, sizeof (sectable));
	//memcpy (mbuf + fwlen,	mbuf, fwlen * sizeof (uae_u16));
	mend = bigmfmbuf + length;
	mend -=	(4 + 16	+ 8 + 512);

	while (secwritten < drvsec) {
		int trackoffs;

		for (;;) {
			rotateonebit (bigmfmbuf, mend, 1);
			if (getmfmword (mbuf) == 0)
				break;
			if (secwritten == 10) {
				mbuf[0]	= 0x44;
				mbuf[1]	= 0x89;
			}
//			check_offset++;
			if (check_offset > 7) {
				check_offset = 0;
				mbuf++;
				if (mbuf >= mend || *mbuf == 0)
					break;
			}
			if (getmfmword (mbuf) == 0x4489)
				break;
		}
		if (mbuf >= mend || *mbuf == 0)
			break;

		rotateonebit (bigmfmbuf, mend, check_offset);
		check_offset = 0;

		while (getmfmword (mbuf) == 0x4489)
			mbuf+= 1 * 2;
		mbuf2 =	mbuf + 8;

		odd = getmfmlong (mbuf);
		even = getmfmlong (mbuf	+ 2 * 2);
		mbuf +=	4 * 2;
		id = (odd << 1)	| even;

		trackoffs = (id	& 0xff00) >> 8;
		if (trackoffs +	1 > drvsec) {
			outlog ("illegal sector offset %d\n",trackoffs);
			ok = 0;
			mbuf = mbuf2;
			continue;
		}
		if ((id	>> 24) != 0xff)	{
			outlog ("sector %d format type %02.2X?\n", trackoffs, id >> 24);
			ok = 0;
		}
		chksum = odd ^ even;
		slabel = 0;
		for (i = 0; i <	4; i++)	{
			odd = getmfmlong (mbuf);
			even = getmfmlong (mbuf	+ 8 * 2);
			mbuf +=	2* 2;

			dlong =	(odd <<	1) | even;
			if (dlong) slabel = 1;
			chksum ^= odd ^	even;
		}
		mbuf +=	8 * 2;
		odd = getmfmlong (mbuf);
		even = getmfmlong (mbuf	+ 2 * 2);
		mbuf +=	4 * 2;
		if (((odd << 1)	| even)	!= chksum) {
			outlog ("sector %d header crc error\n", trackoffs);
			ok = 0;
			mbuf = mbuf2;
			continue;
		}
		outlog ("sector %d header crc ok\n", trackoffs);
		if (((id & 0x00ff0000) >> 16) != (uae_u32)fdi->current_track) {
			outlog ("illegal track number %d <> %d\n", fdi->current_track, (id & 0x00ff0000) >> 16);
			ok++;
			mbuf = mbuf2;
			continue;
		}
		odd = getmfmlong (mbuf);
		even = getmfmlong (mbuf	+ 2 * 2);
		mbuf +=	4 * 2;
		chksum = (odd << 1) | even;
		secdata	= secbuf + 32;
		for (i = 0; i <	128; i++) {
			odd = getmfmlong (mbuf);
			even = getmfmlong (mbuf	+ 256 *	2);
			mbuf +=	2 * 2;
			dlong =	(odd <<	1) | even;
			*secdata++ = (uae_u8) (dlong >>	24);
			*secdata++ = (uae_u8) (dlong >>	16);
			*secdata++ = (uae_u8) (dlong >>	8);
			*secdata++ = (uae_u8) dlong;
			chksum ^= odd ^	even;
		}
		mbuf +=	256 * 2;
		if (chksum) {
			outlog ("sector %d data checksum error\n",trackoffs);
			ok = 0;
		} else if (sectable[trackoffs])	{
			outlog ("sector %d already found?\n", trackoffs);
			mbuf = mbuf2;
		} else {
			outlog ("sector %d ok\n",trackoffs);
			if (slabel) outlog ("(non-empty sector header)\n");
			sectable[trackoffs] = 1;
			secwritten++;
			if (trackoffs == 9)
				mbuf +=	0x228;
		}
	}
	for (i = 0; i <	drvsec;	i++) {
		if (!sectable[i]) {
			outlog ("sector %d missing\n", i);
			ok = 0;
		}
	}
	return ok;
}

/****i* januae-fdi/amiga_data_raw
*
*   NAME
*       amiga_data_raw() -- Encode raw Amiga sector data with supplied checksum.
*
*   SYNOPSIS
*       static void amiga_data_raw (FDI *fdi, uae_u8 *secbuf, uae_u8 *crc,
*       unsigned int len);
*
*   FUNCTION
*       Emit four checksum bytes, or zeros when crc is null, followed by len
*       source bytes through the MFM-aware writer.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       secbuf - Source sector bytes.
*       crc - Optional stored checksum bytes; NULL requests generated or zero
*       values.
*       len - Length of the indicated input or output sequence.
*
***************************************************************************
*
*/

static void amiga_data_raw (FDI	*fdi, uae_u8 *secbuf, uae_u8 *crc, unsigned int len)
{
	unsigned int i;
	uae_u8 crcbuf[4];

	if (!crc) {
		memset (crcbuf,	0, 4);
	} else {
		memcpy (crcbuf,	crc ,4);
	}
	for (i = 0; i <	4; i++)
		byte_mfm_add (fdi, crcbuf[i]);
	for (i = 0; i <	len; i++)
		byte_mfm_add (fdi, secbuf[i]);
}

/****i* januae-fdi/amiga_data
*
*   NAME
*       amiga_data() -- Encode an Amiga sector payload and checksum.
*
*   SYNOPSIS
*       static void amiga_data (FDI *fdi, const uae_u8 *secbuf);
*
*   FUNCTION
*       Split 512 data bytes into odd/even bitplanes, derive their XOR checksum
*       and emit the resulting MFM-encoded words.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       secbuf - Source sector bytes.
*
***************************************************************************
*
*/

static void amiga_data (FDI *fdi, const uae_u8 *secbuf)
{
	uae_u16	mfmbuf[4 + 512];
	uae_u32	dodd, deven, dck;
	unsigned int i;

	for (i = 0; i <	512; i += 4) {
		deven =	((secbuf[i + 0]	<< 24) | (secbuf[i + 1]	<< 16)
		 | (secbuf[i + 2] << 8)	| (secbuf[i + 3]));
		dodd = deven >>	1;
		deven &= 0x55555555;
		dodd &=	0x55555555;
		mfmbuf[(i >> 1)	+ 4] = (uae_u16) (dodd >> 16);
		mfmbuf[(i >> 1)	+ 5] = (uae_u16) dodd;
		mfmbuf[(i >> 1)	+ 256 +	4] = (uae_u16) (deven >> 16);
		mfmbuf[(i >> 1)	+ 256 +	5] = (uae_u16) deven;
	}
	dck = 0;
	for (i = 4; i <	4 + 512; i += 2)
		dck ^= (mfmbuf[i] << 16) | mfmbuf[i + 1];
	deven =	dodd = dck;
	dodd >>= 1;
	deven &= 0x55555555;
	dodd &=	0x55555555;
	mfmbuf[0] = (uae_u16) (dodd >> 16);
	mfmbuf[1] = (uae_u16) dodd;
	mfmbuf[2] = (uae_u16) (deven >>	16);
	mfmbuf[3] = (uae_u16) deven;

	for (i = 0; i <	4 + 512; i ++)
		word_post_mfm_add (fdi,	mfmbuf[i]);
}

/****i* januae-fdi/amiga_sector_header
*
*   NAME
*       amiga_sector_header() -- Encode an Amiga sector header.
*
*   SYNOPSIS
*       static void amiga_sector_header (FDI *fdi, const uae_u8 *header, const
*       uae_u8 *data, unsigned int sector, unsigned int untilgap);
*
*   FUNCTION
*       Write Amiga sync words, an explicit or generated four-byte sector header,
*       a 16-byte label and their odd/even checksum.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       header - Optional four-byte Amiga sector header; NULL synthesizes it.
*       data - Source sector bytes or optional 16-byte header label.
*       sector - Zero-based Amiga sector number.
*       untilgap - Remaining sectors before the track gap.
*
***************************************************************************
*
*/

static void amiga_sector_header	(FDI *fdi, const uae_u8 *header, const uae_u8 *data, unsigned int sector, unsigned int untilgap)
{
	uae_u8 headerbuf[4], databuf[16];
	uae_u32	deven, dodd, hck;
	uae_u16	mfmbuf[24];
	unsigned int i;

	byte_mfm_add (fdi, 0);
	byte_mfm_add (fdi, 0);
	word_add (fdi, 0x4489);
	word_add (fdi, 0x4489);
	if (header) {
		memcpy (headerbuf, header, 4);
	} else {
		headerbuf[0] = 0xff;
		headerbuf[1] = (uae_u8)fdi->current_track;
		headerbuf[2] = (uae_u8)sector;
		headerbuf[3] = (uae_u8)untilgap;
	}
	if (data)
		memcpy (databuf, data, 16);
	else
		memset (databuf, 0, 16);

	deven =	((headerbuf[0] << 24) |	(headerbuf[1] << 16)
		| (headerbuf[2]	<< 8) |	(headerbuf[3]));
	dodd = deven >>	1;
	deven &= 0x55555555;
	dodd &=	0x55555555;
	mfmbuf[0] = (uae_u16) (dodd >> 16);
	mfmbuf[1] = (uae_u16) dodd;
	mfmbuf[2] = (uae_u16) (deven >>	16);
	mfmbuf[3] = (uae_u16) deven;
	for (i = 0; i <	16; i += 4) {
		deven =	((databuf[i] <<	24) | (databuf[i + 1] << 16)
			| (databuf[i + 2] << 8)	| (databuf[i + 3]));
		dodd = deven >>	1;
		deven &= 0x55555555;
		dodd &=	0x55555555;
		mfmbuf[(i >> 1)	+ 0 + 4] = (uae_u16) (dodd >> 16);
		mfmbuf[(i >> 1)	+ 0 + 5] = (uae_u16) dodd;
		mfmbuf[(i >> 1)	+ 8 + 4] = (uae_u16) (deven >> 16);
		mfmbuf[(i >> 1)	+ 8 + 5] = (uae_u16) deven;
	}
	hck = 0;
	for (i = 0; i <	4 + 16;	i += 2)
		hck ^= (mfmbuf[i] << 16) | mfmbuf[i + 1];
	deven =	dodd = hck;
	dodd >>= 1;
	deven &= 0x55555555;
	dodd &=	0x55555555;
	mfmbuf[20] = (uae_u16) (dodd >>	16);
	mfmbuf[21] = (uae_u16) dodd;
	mfmbuf[22] = (uae_u16) (deven >> 16);
	mfmbuf[23] = (uae_u16) deven;

	for (i = 0; i <	4 + 16 + 4; i ++)
		word_post_mfm_add (fdi,	mfmbuf[i]);
}

/****i* januae-fdi/s20
*
*   NAME
*       s20() -- Emit an extended Amiga sector header with label.
*
*   SYNOPSIS
*       static void s20(FDI *fdi);
*
*   FUNCTION
*       Drop the next clock cell, read four header bytes and a 16-byte label, and
*       emit their encoded sector header.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s20(FDI *fdi)
{
	bit_drop_next (fdi);
	debuglog ("s20:header=%s,data=%s", datalog (fdi->track_src, 4), datalog (fdi->track_src + 4, 16));
	amiga_sector_header (fdi, fdi->track_src, fdi->track_src + 4, 0, 0);
	fdi->track_src += 4 + 16;
}
/****i* januae-fdi/s21
*
*   NAME
*       s21() -- Emit an extended Amiga sector header.
*
*   SYNOPSIS
*       static void s21(FDI *fdi);
*
*   FUNCTION
*       Drop the next clock cell and encode the supplied four-byte sector header
*       without a label.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s21(FDI *fdi)
{
	bit_drop_next (fdi);
	debuglog ("s21:header=%s", datalog (fdi->track_src, 4));
	amiga_sector_header (fdi, fdi->track_src, 0, 0,	0);
	fdi->track_src += 4;
}
/****i* januae-fdi/s22
*
*   NAME
*       s22() -- Emit a short Amiga sector header.
*
*   SYNOPSIS
*       static void s22(FDI *fdi);
*
*   FUNCTION
*       Drop the next clock cell, read sector number and sectors-to-gap, and
*       synthesize the remaining header fields.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s22(FDI *fdi)
{
	bit_drop_next (fdi);
	debuglog ("s22:sector=%d,untilgap=%d", fdi->track_src[0], fdi->track_src[1]);
	amiga_sector_header (fdi, 0, 0,	fdi->track_src[0], fdi->track_src[1]);
	fdi->track_src += 2;
}
/****i* januae-fdi/s23
*
*   NAME
*       s23() -- Emit a standard Amiga sector payload.
*
*   SYNOPSIS
*       static void s23(FDI *fdi);
*
*   FUNCTION
*       MFM-encode 512 bytes with a computed sector data checksum.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s23(FDI *fdi)
{
	debuglog ("s23:data=%s", datalog (fdi->track_src, 512));
	amiga_data (fdi, fdi->track_src);
	fdi->track_src += 512;
}
/****i* januae-fdi/s24
*
*   NAME
*       s24() -- Emit variable-length Amiga data with zero checksum.
*
*   SYNOPSIS
*       static void s24(FDI *fdi);
*
*   FUNCTION
*       Read a size exponent and encode 128 times two to that exponent bytes with
*       a zero checksum.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s24(FDI *fdi)
{
	int shift = *fdi->track_src++;
	debuglog ("s24:shift=%d,data=%s", shift, datalog (fdi->track_src, 128 << shift));
	amiga_data_raw (fdi, fdi->track_src, 0,	128 << shift);
	fdi->track_src += 128 << shift;
}
/****i* januae-fdi/s25
*
*   NAME
*       s25() -- Emit variable-length Amiga data with stored checksum.
*
*   SYNOPSIS
*       static void s25(FDI *fdi);
*
*   FUNCTION
*       Read the size exponent, four checksum bytes and data, then MFM-encode the
*       provided checksum and payload without correcting them.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s25(FDI *fdi)
{
	int shift = *fdi->track_src++;
	debuglog ("s25:shift=%d,crc=%s,data=%s", shift, datalog (fdi->track_src, 4), datalog (fdi->track_src + 4, 128 << shift));
	amiga_data_raw (fdi, fdi->track_src + 4, fdi->track_src, 128 <<	shift);
	fdi->track_src += 4 + (128 << shift);
}
/****i* januae-fdi/s26
*
*   NAME
*       s26() -- Emit a complete extended Amiga sector.
*
*   SYNOPSIS
*       static void s26(FDI *fdi);
*
*   FUNCTION
*       Read and encode a four-byte header followed by 512 bytes of data with a
*       computed checksum.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s26(FDI *fdi)
{
	s21 (fdi);
	debuglog ("s26:data=%s", datalog (fdi->track_src, 512));
	amiga_data (fdi, fdi->track_src);
	fdi->track_src += 512;
}
/****i* januae-fdi/s27
*
*   NAME
*       s27() -- Emit a complete short Amiga sector.
*
*   SYNOPSIS
*       static void s27(FDI *fdi);
*
*   FUNCTION
*       Read a two-byte sector descriptor followed by 512 bytes of data with a
*       computed checksum.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s27(FDI *fdi)
{
	s22 (fdi);
	debuglog ("s27:data=%s", datalog (fdi->track_src, 512));
	amiga_data (fdi, fdi->track_src);
	fdi->track_src += 512;
}

/* *** */
/* IBM */
/* *** */

/****i* januae-fdi/ibm_crc
*
*   NAME
*       ibm_crc() -- Update the IBM CRC accumulator.
*
*   SYNOPSIS
*       static uae_u16 ibm_crc (uae_u8 byte, int reset);
*
*   FUNCTION
*       Optionally reset the static CRC to 0xCDB4, then process the supplied byte
*       with polynomial 0x1021. This function maintains shared state between
*       calls.
*
*   INPUTS
*       byte - Next data byte for CRC processing.
*       reset - Nonzero to reinitialize the shared CRC before processing.
*
*   RESULT
*       Updated 16-bit CRC.
*
***************************************************************************
*
*/

static uae_u16 ibm_crc (uae_u8 byte, int reset)
{
	static uae_u16 crc;
	int i;

	if (reset) crc = 0xcdb4;
	for (i = 0; i <	8; i++)	{
		if (crc	& 0x8000) {
			crc <<=	1;
			if (!(byte & 0x80)) crc	^= 0x1021;
		} else {
			crc <<=	1;
			if (byte & 0x80) crc ^=	0x1021;
		}
		byte <<= 1;
	}
	return crc;
}

/****i* januae-fdi/ibm_data
*
*   NAME
*       ibm_data() -- Encode an IBM sector data field.
*
*   SYNOPSIS
*       static void ibm_data (FDI *fdi, const uae_u8 *data, uae_u8 *crc, unsigned
*       int len);
*
*   FUNCTION
*       Write three MFM sync words, the data mark, len payload bytes and either
*       the caller's CRC bytes or a computed CRC.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       data - Source sector bytes or optional 16-byte header label.
*       crc - Optional stored checksum bytes; NULL requests generated or zero
*       values.
*       len - Length of the indicated input or output sequence.
*
***************************************************************************
*
*/

static void ibm_data (FDI *fdi,	const uae_u8 *data, uae_u8 *crc, unsigned int len)
{
	unsigned int i;
	uae_u8 crcbuf[2];
	uae_u16	crcv = 0;

	word_add (fdi, 0x4489);
	word_add (fdi, 0x4489);
	word_add (fdi, 0x4489);
	byte_mfm_add (fdi, 0xfb);
	ibm_crc	(0xfb, 1);
	for (i = 0; i <	len; i++) {
		byte_mfm_add (fdi, data[i]);
		crcv = ibm_crc (data[i], 0);
	}
	if (!crc) {
		crc = crcbuf;
		crc[0] = (uae_u8)(crcv >> 8);
		crc[1] = (uae_u8)crcv;
	}
	byte_mfm_add (fdi, crc[0]);
	byte_mfm_add (fdi, crc[1]);
}

/****i* januae-fdi/ibm_sector_header
*
*   NAME
*       ibm_sector_header() -- Encode an IBM sector ID field.
*
*   SYNOPSIS
*       static void ibm_sector_header (FDI *fdi, const uae_u8 *data, const uae_u8
*       *crc, int secnum, int pre);
*
*   FUNCTION
*       Optionally prepend the standard gap, then encode an explicit or
*       synthesized CHRN header and a caller-supplied or computed CRC.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       data - Source sector bytes or optional 16-byte header label.
*       crc - Optional stored checksum bytes; NULL requests generated or zero
*       values.
*       secnum - Sector number; negative selects the caller-provided CHRN bytes.
*       pre - Nonzero to prepend the standard sector-ID gap.
*
***************************************************************************
*
*/

static void ibm_sector_header (FDI *fdi, const uae_u8 *data, const uae_u8 *crc, int secnum, int pre)
{
	uae_u8 secbuf[5];
	uae_u8 crcbuf[2];
	uae_u16	crcv;
	int i;

	if (pre)
		bytes_mfm_add (fdi, 0, 12);
	word_add (fdi, 0x4489);
	word_add (fdi, 0x4489);
	word_add (fdi, 0x4489);
	secbuf[0] = 0xfe;
	if (secnum >= 0) {
		secbuf[1] = (uae_u8)(fdi->current_track / 2);
		secbuf[2] = (uae_u8)(fdi->current_track % 2);
		secbuf[3] = (uae_u8)secnum;
		secbuf[4] = 2;
	} else {
		memcpy (secbuf + 1, data, 4);
	}
	ibm_crc	(secbuf[0], 1);
	ibm_crc	(secbuf[1], 0);
	ibm_crc	(secbuf[2], 0);
	ibm_crc	(secbuf[3], 0);
	crcv = ibm_crc (secbuf[4], 0);
	if (crc) {
		memcpy (crcbuf,	crc, 2);
	} else {
		crcbuf[0] = (uae_u8)(crcv >> 8);
		crcbuf[1] = (uae_u8)crcv;
	}
	/* data	*/
	for (i = 0;i < 5; i++)
		byte_mfm_add (fdi, secbuf[i]);
	/* crc */
	byte_mfm_add (fdi, crcbuf[0]);
	byte_mfm_add (fdi, crcbuf[1]);
}

/****i* januae-fdi/s10
*
*   NAME
*       s10() -- Emit the standard IBM index address mark.
*
*   SYNOPSIS
*       static void s10 (FDI *fdi);
*
*   FUNCTION
*       Drop the next clock bit, emit twelve zero bytes, three 0x5224 syncs and
*       the 0xFC mark.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s10 (FDI *fdi)
{
	bit_drop_next (fdi);
	bytes_mfm_add (fdi, 0, 12);
	word_add (fdi, 0x5224);
	word_add (fdi, 0x5224);
	word_add (fdi, 0x5224);
	byte_mfm_add (fdi, 0xfc);
}
/****i* januae-fdi/s11
*
*   NAME
*       s11() -- Emit the standard IBM index pregap.
*
*   SYNOPSIS
*       static void s11 (FDI *fdi);
*
*   FUNCTION
*       Emit a 78-byte 0x4E gap, the index mark and a 50-byte 0x4E gap, adjusting
*       the pending clock drop.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s11 (FDI *fdi)
{
	bit_drop_next (fdi);
	bytes_mfm_add (fdi, 0x4e, 78);
	bit_dedrop (fdi);
	s10 (fdi);
	bytes_mfm_add (fdi, 0x4e, 50);
}
/****i* januae-fdi/s12
*
*   NAME
*       s12() -- Emit an Atari-style pregap.
*
*   SYNOPSIS
*       static void s12 (FDI *fdi);
*
*   FUNCTION
*       Drop the next clock cell and emit 78 MFM-encoded 0x4E gap bytes.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s12 (FDI *fdi)
{
	bit_drop_next (fdi);
	bytes_mfm_add (fdi, 0x4e, 78);
}
/****i* januae-fdi/s13
*
*   NAME
*       s13() -- Emit an extended IBM sector header.
*
*   SYNOPSIS
*       static void s13 (FDI *fdi);
*
*   FUNCTION
*       Read four CHRN bytes and generate the standard header with pregap and a
*       computed CRC.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s13 (FDI *fdi)
{
	bit_drop_next (fdi);
	debuglog ("s13:header=%s", datalog (fdi->track_src, 4));
	ibm_sector_header (fdi,	fdi->track_src,	0, -1, 1);
	fdi->track_src += 4;
}
/****i* januae-fdi/s14
*
*   NAME
*       s14() -- Emit a compact extended IBM header.
*
*   SYNOPSIS
*       static void s14 (FDI *fdi);
*
*   FUNCTION
*       Read four CHRN bytes and generate the header without the pregap.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s14 (FDI *fdi)
{
	debuglog ("s14:header=%s", datalog (fdi->track_src, 4));
	ibm_sector_header (fdi,	fdi->track_src,	0, -1, 0);
	fdi->track_src += 4;
}
/****i* januae-fdi/s15
*
*   NAME
*       s15() -- Emit a short IBM sector header.
*
*   SYNOPSIS
*       static void s15 (FDI *fdi);
*
*   FUNCTION
*       Read the sector number and synthesize remaining CHRN fields with standard
*       pregap and checksum.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s15 (FDI *fdi)
{
	bit_drop_next (fdi);
	debuglog ("s15:sector=%d", *fdi->track_src);
	ibm_sector_header (fdi,	0, 0, *fdi->track_src++, 1);
}
/****i* januae-fdi/s16
*
*   NAME
*       s16() -- Emit a compact short IBM header.
*
*   SYNOPSIS
*       static void s16 (FDI *fdi);
*
*   FUNCTION
*       Read the sector number and generate the synthesized ID without a pregap.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s16 (FDI *fdi)
{
	debuglog ("s16:track=%d", *fdi->track_src);
	ibm_sector_header (fdi,	0, 0, *fdi->track_src++, 0);
}
/****i* januae-fdi/s17
*
*   NAME
*       s17() -- Emit an IBM header with explicit checksum.
*
*   SYNOPSIS
*       static void s17 (FDI *fdi);
*
*   FUNCTION
*       Read four CHRN bytes and two checksum bytes and encode the header without
*       correcting the stored CRC.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s17 (FDI *fdi)
{
	debuglog ("s17:header=%s,crc=%s", datalog (fdi->track_src, 4), datalog (fdi->track_src + 4, 2));
	ibm_sector_header (fdi,	fdi->track_src,	fdi->track_src + 4, -1,	0);
	fdi->track_src += 4 + 2;
}
/****i* januae-fdi/s18
*
*   NAME
*       s18() -- Emit a short IBM header with explicit checksum.
*
*   SYNOPSIS
*       static void s18 (FDI *fdi);
*
*   FUNCTION
*       Read sector number and two checksum bytes, synthesize CHRN fields and
*       preserve the stored CRC.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s18 (FDI *fdi)
{
	debuglog ("s18:sector=%d,header=%s", *fdi->track_src, datalog (fdi->track_src +	1, 4));
	ibm_sector_header (fdi,	0, fdi->track_src + 1, *fdi->track_src,	0);
	fdi->track_src += 1 + 4;
}
/****i* januae-fdi/s19
*
*   NAME
*       s19() -- Emit a standard IBM sector payload.
*
*   SYNOPSIS
*       static void s19 (FDI *fdi);
*
*   FUNCTION
*       Read 512 bytes of data and emit its encoded data field with a computed
*       CRC.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s19 (FDI *fdi)
{
	debuglog ("s19:data=%s", datalog (fdi->track_src , 512));
	ibm_data (fdi, fdi->track_src, 0, 512);
	fdi->track_src += 512;
}
/****i* januae-fdi/s1a
*
*   NAME
*       s1a() -- Emit a variable-length IBM data field.
*
*   SYNOPSIS
*       static void s1a (FDI *fdi);
*
*   FUNCTION
*       Read a size exponent and encode 128 times two to that exponent bytes with
*       a computed CRC.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s1a (FDI *fdi)
{
	int shift = *fdi->track_src++;
	debuglog ("s1a:shift=%d,data=%s", shift, datalog (fdi->track_src , 128 << shift));
	ibm_data (fdi, fdi->track_src, 0, 128 << shift);
	fdi->track_src += 128 << shift;
}
/****i* januae-fdi/s1b
*
*   NAME
*       s1b() -- Emit a variable-length IBM field with explicit CRC.
*
*   SYNOPSIS
*       static void s1b (FDI *fdi);
*
*   FUNCTION
*       Read size exponent, payload and two CRC bytes, preserving the supplied
*       checksum in the emitted field.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s1b (FDI *fdi)
{
	int shift = *fdi->track_src++;
	debuglog ("s1b:shift=%d,crc=%s,data=%s", shift,	datalog	(fdi->track_src	+ (128 << shift), 2), datalog (fdi->track_src ,	128 << shift));
	ibm_data (fdi, fdi->track_src, fdi->track_src +	(128 <<	shift),	128 << shift);
	fdi->track_src += (128 << shift) + 2;
}
/****i* januae-fdi/s1c
*
*   NAME
*       s1c() -- Emit a complete extended IBM sector.
*
*   SYNOPSIS
*       static void s1c (FDI *fdi);
*
*   FUNCTION
*       Read a CHRN header, add the standard inter-field gap and encode its
*       variable-length data with computed CRC.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s1c (FDI *fdi)
{
	int shift = fdi->track_src[3];
	s13 (fdi);
	bytes_mfm_add (fdi, 0x4e, 22);
	bytes_mfm_add (fdi, 0x00, 12);
	ibm_data (fdi, fdi->track_src, 0, 128 << shift);
	fdi->track_src += 128 << shift;
}
/****i* januae-fdi/s1d
*
*   NAME
*       s1d() -- Emit a complete short IBM sector.
*
*   SYNOPSIS
*       static void s1d (FDI *fdi);
*
*   FUNCTION
*       Read a short ID, emit the inter-field gap and encode a 512-byte data
*       field with computed CRC.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void s1d (FDI *fdi)
{
	s15 (fdi);
	bytes_mfm_add (fdi, 0x4e, 22);
	bytes_mfm_add (fdi, 0x00, 12);
	s19 (fdi);
}

/****i* januae-fdi/sff
*
*   NAME
*       sff() -- Terminate a sector-description command sequence.
*
*   SYNOPSIS
*       static void sff (FDI *fdi);
*
*   FUNCTION
*       Perform no output operation; the enclosing command decoder terminates
*       when it sees opcode 0xFF.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void sff (FDI *fdi)
{
}

typedef	void (*decode_described_track_func)(FDI*);

static decode_described_track_func const decode_sectors_described_track[] =
{
	s00,s01,s02,s03,s04,dxx,dxx,dxx,s08,s09,s0a,s0b,s0c,s0d,dxx,dxx, /* 00-0F */
	s10,s11,s12,s13,s14,s15,s16,s17,s18,s19,s1a,s1b,s1c,s1d,dxx,dxx, /* 10-1F */
	s20,s21,s22,s23,s24,s25,s26,s27,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* 20-2F */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* 30-3F */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* 40-4F */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* 50-5F */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* 60-6F */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* 70-7F */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* 80-8F */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* 90-9F */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* A0-AF */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* B0-BF */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* C0-CF */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* D0-DF */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx, /* E0-EF */
	dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,dxx,sff	 /* F0-FF */
};

/****i* januae-fdi/track_amiga
*
*   NAME
*       track_amiga() -- Generate an Amiga-sector track.
*
*   SYNOPSIS
*       static void track_amiga (struct fdi *fdi, unsigned int first_sector,
*       unsigned int max_sector);
*
*   FUNCTION
*       Emit max_sector sectors starting at first_sector, wrapping the sector
*       number, and append a 260-byte gap.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       first_sector - First sector number to encode in the track.
*       max_sector - Number of sectors to encode.
*
***************************************************************************
*
*/

static void track_amiga	(struct	fdi *fdi, unsigned int first_sector, unsigned int max_sector)
{
	unsigned int i;

	bit_add	(fdi, 0);
	bit_drop_next (fdi);
	for (i = 0; i <	max_sector; i++) {
		amiga_sector_header (fdi, 0, 0,	first_sector, max_sector - i);
		amiga_data (fdi, fdi->track_src	+ first_sector * 512);
		first_sector++;
		if (first_sector >= max_sector)	first_sector = 0;
	}
	bytes_mfm_add (fdi, 0, 260); /*	gap */
}
/****i* januae-fdi/track_atari_st
*
*   NAME
*       track_atari_st() -- Generate an Atari ST track.
*
*   SYNOPSIS
*       static void track_atari_st (struct fdi *fdi, unsigned int max_sector);
*
*   FUNCTION
*       Emit an IBM-style indexed track with max_sector 512-byte sectors and
*       layout-specific gap lengths; advance the input cursor.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       max_sector - Number of sectors to encode.
*
***************************************************************************
*
*/

static void track_atari_st (struct fdi *fdi, unsigned int max_sector)
{
	unsigned int i, gap3 = 0;
	uae_u8 *p = fdi->track_src;

	switch (max_sector) {
		case 9:
		gap3 = 40;
		break;
		case 10:
		gap3 = 24;
		break;
	}
	s15 (fdi);
	for (i = 0; i <	max_sector; i++) {
		byte_mfm_add (fdi, 0x4e);
		byte_mfm_add (fdi, 0x4e);
		ibm_sector_header (fdi,	0, 0, fdi->current_track, 1);
		ibm_data (fdi, p + i * 512, 0, 512);
		bytes_mfm_add (fdi, 0x4e, gap3);
	}
	bytes_mfm_add (fdi, 0x4e, 660 -	gap3);
	fdi->track_src += fdi->track_len * 256;
}
/****i* januae-fdi/track_pc
*
*   NAME
*       track_pc() -- Generate a PC-format track.
*
*   SYNOPSIS
*       static void track_pc (struct fdi *fdi, unsigned int max_sector);
*
*   FUNCTION
*       Emit an IBM-style indexed track with max_sector 512-byte sectors and a
*       selected gap length; advance the input cursor.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       max_sector - Number of sectors to encode.
*
***************************************************************************
*
*/

static void track_pc (struct fdi *fdi, unsigned int max_sector)
{
	unsigned int i, gap3;
	uae_u8 *p = fdi->track_src;

	switch (max_sector) {
		case 8:
		gap3 = 116;
		break;
		case 9:
		gap3 = 54;
		break;
		default:
		gap3 = 100; /* fixme */
		break;
	}
	s11 (fdi);
	for (i = 0; i <	max_sector; i++) {
		byte_mfm_add (fdi, 0x4e);
		byte_mfm_add (fdi, 0x4e);
		ibm_sector_header (fdi,	0, 0, fdi->current_track, 1);
		ibm_data (fdi, p + i * 512, 0, 512);
		bytes_mfm_add (fdi, 0x4e, gap3);
	}
	bytes_mfm_add (fdi, 0x4e, 600 -	gap3);
	fdi->track_src += fdi->track_len * 256;
}

/****i* januae-fdi/track_amiga_dd
*
*   NAME
*       track_amiga_dd() -- Generate an Amiga double-density track.
*
*   SYNOPSIS
*       static void track_amiga_dd (struct fdi *fdi);
*
*   FUNCTION
*       Use the starting sector encoded in track_len and generate eleven sectors,
*       then advance the source cursor by its low-nibble sector count.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void track_amiga_dd (struct fdi *fdi)
{
	uae_u8 *p = fdi->track_src;
	track_amiga (fdi, fdi->track_len >> 4, 11);
	fdi->track_src = p + (fdi->track_len & 15) * 512;
}
/****i* januae-fdi/track_amiga_hd
*
*   NAME
*       track_amiga_hd() -- Generate an Amiga high-density track.
*
*   SYNOPSIS
*       static void track_amiga_hd (struct fdi *fdi);
*
*   FUNCTION
*       Generate 22 Amiga sectors and advance the source cursor by track_len
*       times 256 bytes.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void track_amiga_hd (struct fdi *fdi)
{
	uae_u8 *p = fdi->track_src;
	track_amiga (fdi, 0, 22);
	fdi->track_src = p + fdi->track_len * 256;
}
/****i* januae-fdi/track_atari_st_9
*
*   NAME
*       track_atari_st_9() -- Generate a nine-sector Atari ST track.
*
*   SYNOPSIS
*       static void track_atari_st_9 (struct fdi *fdi);
*
*   FUNCTION
*       Select the Atari ST generator with nine 512-byte sectors.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void track_atari_st_9 (struct fdi *fdi)
{
	track_atari_st (fdi, 9);
}
/****i* januae-fdi/track_atari_st_10
*
*   NAME
*       track_atari_st_10() -- Generate a ten-sector Atari ST track.
*
*   SYNOPSIS
*       static void track_atari_st_10 (struct fdi *fdi);
*
*   FUNCTION
*       Select the Atari ST generator with ten 512-byte sectors.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void track_atari_st_10 (struct fdi *fdi)
{
	track_atari_st (fdi, 10);
}
/****i* januae-fdi/track_pc_8
*
*   NAME
*       track_pc_8() -- Generate an eight-sector PC track.
*
*   SYNOPSIS
*       static void track_pc_8 (struct fdi *fdi);
*
*   FUNCTION
*       Select the PC track generator with eight 512-byte sectors.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void track_pc_8 (struct fdi *fdi)
{
	track_pc (fdi, 8);
}
/****i* januae-fdi/track_pc_9
*
*   NAME
*       track_pc_9() -- Generate a nine-sector PC track.
*
*   SYNOPSIS
*       static void track_pc_9 (struct fdi *fdi);
*
*   FUNCTION
*       Select the PC track generator with nine 512-byte sectors.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void track_pc_9 (struct fdi *fdi)
{
	track_pc (fdi, 9);
}
/****i* januae-fdi/track_pc_15
*
*   NAME
*       track_pc_15() -- Generate a fifteen-sector PC track.
*
*   SYNOPSIS
*       static void track_pc_15 (struct fdi *fdi);
*
*   FUNCTION
*       Select the PC track generator with fifteen 512-byte sectors.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void track_pc_15	(struct	fdi *fdi)
{
	track_pc (fdi, 15);
}
/****i* januae-fdi/track_pc_18
*
*   NAME
*       track_pc_18() -- Generate an eighteen-sector PC track.
*
*   SYNOPSIS
*       static void track_pc_18 (struct fdi *fdi);
*
*   FUNCTION
*       Select the PC track generator with eighteen 512-byte sectors.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void track_pc_18	(struct	fdi *fdi)
{
	track_pc (fdi, 18);
}
/****i* januae-fdi/track_pc_36
*
*   NAME
*       track_pc_36() -- Generate a thirty-six-sector PC track.
*
*   SYNOPSIS
*       static void track_pc_36 (struct fdi *fdi);
*
*   FUNCTION
*       Select the PC track generator with thirty-six 512-byte sectors.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void track_pc_36	(struct	fdi *fdi)
{
	track_pc (fdi, 36);
}

typedef	void (*decode_normal_track_func)(FDI*);

static decode_normal_track_func const decode_normal_track[] =
{
	track_empty, /*	0 */
	track_amiga_dd,	track_amiga_hd,	/* 1-2 */
	track_atari_st_9, track_atari_st_10, /*	3-4 */
	track_pc_8, track_pc_9,	track_pc_15, track_pc_18, track_pc_36, /* 5-9 */
	zxx,zxx,zxx,zxx,zxx /* A-F */
};

/****i* januae-fdi/fix_mfm_sync
*
*   NAME
*       fix_mfm_sync() -- Fill recorded MFM clock cells.
*
*   SYNOPSIS
*       static void fix_mfm_sync (FDI *fdi);
*
*   FUNCTION
*       Visit the saved clock-cell positions and set or clear each clock bit
*       according to its adjacent data bits. The decoder trusts the saved
*       positions.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

static void fix_mfm_sync (FDI *fdi)
{
	int i, pos, off1, off2,	off3, mask1, mask2, mask3;

	for (i = 0; i <	fdi->mfmsync_offset; i++) {
		pos = fdi->mfmsync_buffer[i];
		off1 = (pos - 1) >> 3;
		off2 = (pos + 1) >> 3;
		off3 = pos >> 3;
		mask1 =	1 << (7	- ((pos	- 1) & 7));
		mask2 =	1 << (7	- ((pos	+ 1) & 7));
		mask3 =	1 << (7	- (pos & 7));
		if (!(fdi->track_dst[off1] & mask1) && !(fdi->track_dst[off2] &	mask2))
			fdi->track_dst[off3] |=	mask3;
		else
			fdi->track_dst[off3] &=	~mask3;
	}
}

/****i* januae-fdi/handle_sectors_described_track
*
*   NAME
*       handle_sectors_described_track() -- Interpret FDI sector-description commands.
*
*   SYNOPSIS
*       static int handle_sectors_described_track (FDI *fdi);
*
*   FUNCTION
*       Read the track encoding and index fields, dispatch description opcodes
*       until 0xFF, then fix recorded MFM clock bits. It checks the input cursor
*       after each command rather than before every read.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
*   RESULT
*       Generated bit count, or -1 for a decoder error or source overrun.
*
***************************************************************************
*
*/

static int handle_sectors_described_track (FDI *fdi)
{
	int oldout;
	uae_u8 *start_src = fdi->track_src ;
	fdi->encoding_type = *fdi->track_src++;
	fdi->index_offset = get_u32(fdi->track_src);
	fdi->index_offset >>= 8;
	fdi->track_src += 3;
	outlog ("sectors_described, index offset: %d\n",fdi->index_offset);

	do {
		fdi->track_type	= *fdi->track_src++;
		outlog ("%06.6X	%06.6X %02.2X:",fdi->track_src - start_src + 0x200, fdi->out/8,	fdi->track_type);
		oldout = fdi->out;
		decode_sectors_described_track[fdi->track_type](fdi);
		outlog (" %d\n", fdi->out - oldout);
		oldout = fdi->out;
		if (fdi->out < 0 || fdi->err) {
			outlog ("\nin %d bytes,	out %d bits\n",	fdi->track_src - fdi->track_src_buffer,	fdi->out);
			return -1;
		}
		if (fdi->track_src - fdi->track_src_buffer >= fdi->track_src_len) {
			outlog ("source	buffer overrun,	previous type: %02.2X\n", fdi->track_type);
			return -1;
		}
	} while	(fdi->track_type != 0xff);
	outlog ("\n");
	fix_mfm_sync (fdi);
	return fdi->out;
}

/****i* januae-fdi/fdi_decompress
*
*   NAME
*       fdi_decompress() -- Decode an FDI pulse-value stream.
*
*   SYNOPSIS
*       static uae_u8 *fdi_decompress (unsigned int pulses, uae_u8 *sizep, const
*       uae_u8 *src, unsigned int *dofree);
*
*   FUNCTION
*       Use its packed size and compression mode to choose an in-place 32-bit
*       conversion or an allocated Huffman output; set dofree only for newly
*       allocated storage.
*
*   INPUTS
*       pulses - Number of pulse samples to decompress or decode.
*       sizep - Packed three-byte stream descriptor.
*       src - Source bytes or compressed pulse stream; caller-owned.
*       dofree - Output ownership flag: set when a new result allocation is made.
*
*   RESULT
*       Decoded pulse pointer, or NULL for unsupported modes.
*
***************************************************************************
*
*/

static uae_u8 *fdi_decompress (unsigned int pulses, uae_u8 *sizep, const uae_u8 *src, unsigned int *dofree)
{
	uae_u32	size = get_u24 (sizep);
	uae_u32	*dst2;
	unsigned int len = size & 0x3fffff;
	uae_u8 *dst;
	unsigned int mode = size >> 22, i;

	*dofree	= 0;
	if (mode == 0 && pulses	* 2 > len)
		mode = 1;
	if (mode == 0) {
		dst2 = (uae_u32*)src;
		dst = (uae_u8*)src;
		for (i = 0; i <	pulses;	i++) {
			*dst2++	= get_u32 (src);
			src += 4;
		}
	} else if (mode	== 1) {
		dst = fdi_malloc (pulses * 4);
		*dofree	= 1;
		fdi_decode (src, pulses, dst);
	} else {
		dst = 0;
	}
	return dst;
}

/****i* januae-fdi/dumpstream
*
*   NAME
*       dumpstream() -- Accept an optional pulse-dump request.
*
*   SYNOPSIS
*       static void dumpstream(int track, uae_u8 *stream, int len);
*
*   FUNCTION
*       Perform no operation in the compiled implementation; the file-writing
*       code is disabled by an inner #if 0.
*
*   INPUTS
*       track - Zero-based FDI track index.
*       stream - FDI input cursor in a valid encoded stream.
*       len - Length of the indicated input or output sequence.
*
***************************************************************************
*
*/

static void dumpstream(int track, uae_u8 *stream, int len)
{
#if 0
    char name[100];
    FILE *f;

    sprintf (name, "track_%d.raw", track);
    f =	fopen(name, "wb");
    fwrite (stream, 1, len * 4,	f);
    fclose (f);
#endif
}

static int bitoffset;

/****i* januae-fdi/addbit
*
*   NAME
*       addbit() -- Append one pulse-decoded bit.
*
*   SYNOPSIS
*       STATIC_INLINE void addbit (uae_u8 *p, int bit);
*
*   FUNCTION
*       OR the supplied bit into the output byte at the shared bitoffset and
*       advance that offset. This writer does not check buffer capacity.
*
*   INPUTS
*       p - Heap allocation or destination byte array.
*       bit - Zero or nonzero data bit to append.
*
***************************************************************************
*
*/

STATIC_INLINE void addbit (uae_u8 *p, int bit)
{
	int off1 = bitoffset / 8;
	int off2 = bitoffset % 8;
	p[off1]	|= bit << (7 - off2);
	bitoffset++;
}


struct pulse_sample {
	unsigned long size;
	int number_of_bits;
};


#define	FDI_MAX_ARRAY 10 /* change this	value as you want */
static const int pulse_limitval = 15; /* tolerance of 15% */
static struct pulse_sample psarray[FDI_MAX_ARRAY];
static int array_index;
static unsigned	long total;
static int totaldiv;

/****i* januae-fdi/init_array
*
*   NAME
*       init_array() -- Initialize the pulse-size history.
*
*   SYNOPSIS
*       static void init_array(unsigned long standard_MFM_2_bit_cell_size, int
*       nb_of_bits);
*
*   FUNCTION
*       Set every history slot to the supplied nominal bit-cell size and count,
*       accumulating the global total and divisor and restarting the ring index.
*
*   INPUTS
*       standard_MFM_2_bit_cell_size - Nominal two-cell pulse size in source
*       timing units.
*       nb_of_bits - Number of bit cells represented by each history entry.
*
***************************************************************************
*
*/

static void init_array(unsigned	long standard_MFM_2_bit_cell_size, int nb_of_bits)
{
	int i;

	for (i = 0; i <	FDI_MAX_ARRAY; i++) {
		psarray[i].size	= standard_MFM_2_bit_cell_size;	// That	is (total track	length / 50000)	for Amiga double density
		total += psarray[i].size;
		psarray[i].number_of_bits = nb_of_bits;
		totaldiv += psarray[i].number_of_bits;
	}
	array_index = 0;
}


/****i* januae-fdi/fdi2_decode
*
*   NAME
*       fdi2_decode() -- Convert pulse measurements into track bits.
*
*   SYNOPSIS
*       static void fdi2_decode (FDI *fdi, unsigned long totalavg, uae_u32 *avgp,
*       uae_u32 *minp, uae_u32 *maxp, uae_u8 *idx, unsigned int maxidx, unsigned
*       int *indexoffsetp, unsigned int pulses, int mfm);
*
*   FUNCTION
*       Use average, minimum, maximum and index pulse data to select bit-cell
*       lengths, synthesize the bitstream and per-bit timing, and update the
*       optional index position. The active implementation is selected by #else
*       after a disabled decoder.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       totalavg - Total averaged pulse duration for one track.
*       avgp - Per-pulse average durations.
*       minp - Per-pulse minimum durations.
*       maxp - Per-pulse maximum durations.
*       idx - Per-pulse index/stability observations.
*       maxidx - Maximum stability weight for index samples.
*       indexoffsetp - Optional index position output, updated when detected.
*       pulses - Number of pulse samples to decompress or decode.
*       mfm - MFM timing mode requested by the caller.
*
***************************************************************************
*
*/

static void fdi2_decode (FDI *fdi, unsigned long totalavg, uae_u32 *avgp, uae_u32 *minp, uae_u32 *maxp, uae_u8 *idx, unsigned int maxidx, unsigned int *indexoffsetp, unsigned int pulses, int mfm)
{
	unsigned long adjust;
	unsigned long adjusted_pulse;
	unsigned long standard_MFM_2_bit_cell_size = totalavg /	50000;
	unsigned long standard_MFM_8_bit_cell_size = totalavg /	12500;
	unsigned int real_size, i, nexti, eodat, randval;
        int outstep;
	unsigned int indexoffset = *indexoffsetp;
	uae_u8 *d = fdi->track_dst_buffer;
	uae_u16	*pt = fdi->track_dst_buffer_timing;
	uae_u32 ref_pulse, pulse;
	long jitter;

	/* detects a long-enough stable pulse coming just after another stable pulse */
	i = 1;
	while ( (i < pulses) && ( (idx[i] < maxidx)
		|| (idx[i - 1] < maxidx)
		|| (minp[i] < (standard_MFM_2_bit_cell_size - (standard_MFM_2_bit_cell_size / 4))) ) )
			i++;
	if (i == pulses)  {
		outlog ("FDI: No stable and long-enough pulse in track.\n");
		return;
	}
	nexti = i;
	eodat = i;
	i--;
	adjust = 0;
	total =	0;
	totaldiv = 0;
	init_array(standard_MFM_2_bit_cell_size, 1 + mfm);
	bitoffset = 0;
	ref_pulse = 0;
	jitter = 0;
	outstep = -1;
	while (outstep < 2) {

		/* calculates the current average bitrate from previous decoded data */
		uae_u32	avg_size = (total << (2 + mfm)) / totaldiv; /* this is the new average size for one MFM	bit */
		/* uae_u32 avg_size = (uae_u32)((((float)total)*((float)(mfm+1))*4.0) / ((float)totaldiv)); */
		/* you can try tighter ranges than 25%,	or wider ranges. I would probably go for tighter... */
		if ((avg_size <	(standard_MFM_8_bit_cell_size -	(pulse_limitval	* standard_MFM_8_bit_cell_size / 100)))	||
			(avg_size > (standard_MFM_8_bit_cell_size + (pulse_limitval * standard_MFM_8_bit_cell_size / 100)))) {
				//init_array(standard_MFM_2_bit_cell_size, mfm + 1);
				avg_size = standard_MFM_8_bit_cell_size;
		}
		/* this	is to prevent the average value	from going too far
		* from the theoretical value, otherwise	it could progressively go to (2	*
		* real value), or (real	value /	2), etc. */

		/* gets the next long-enough pulse (this may require more than one pulse) */
		pulse = 0;
		while (pulse < ((avg_size / 4) - (avg_size / 16))) {
			uae_u32 avg_pulse, min_pulse, max_pulse;
			i++;
			if (i >= pulses)
				i = 0;
			if (i == nexti) {
				do {
					nexti++;
					if (nexti >= pulses)
						nexti = 0;
				} while (idx[nexti] < maxidx);
			}
			if (idx[i] >= maxidx) { /* stable pulse */
				avg_pulse = avgp[i] - jitter;
				min_pulse = minp[i];
				max_pulse = maxp[i];
				if (jitter >= 0)
					max_pulse -= jitter;
				else
					min_pulse -= jitter;
				if ((maxp[nexti] - avgp[nexti]) < (avg_pulse - min_pulse))
					min_pulse = avg_pulse - (maxp[nexti] - avgp[nexti]);
				if ((avgp[nexti] - minp[nexti]) < (max_pulse - avg_pulse))
					max_pulse = avg_pulse + (avgp[nexti] - minp[nexti]);
				if (min_pulse < ref_pulse)
					min_pulse = ref_pulse;
				randval = rand();
				if (randval < (RAND_MAX / 2)) {
					if (randval > (RAND_MAX / 4)) {
						if (randval <= (3 * (RAND_MAX / 8)))
							randval = (2 * randval) - (RAND_MAX /4);
						else
							randval = (4 * randval) - RAND_MAX;
					}
					jitter = 0 - (randval * (avg_pulse - min_pulse)) / RAND_MAX;
				} else {
					randval -= RAND_MAX / 2;
					if (randval > (RAND_MAX / 4)) {
						if (randval <= (3 * (RAND_MAX / 8)))
							randval = (2 * randval) - (RAND_MAX /4);
						else
							randval = (4 * randval) - RAND_MAX;
					}
					jitter = (randval * (max_pulse - avg_pulse)) / RAND_MAX;
				}
				avg_pulse += jitter;
				if ((avg_pulse < min_pulse) || (avg_pulse > max_pulse)) {
					outlog ("FDI: avg_pulse outside bounds! avg=%u min=%u max=%u\n", avg_pulse, min_pulse, max_pulse);
					outlog ("FDI: avgp=%u (%u) minp=%u (%u) maxp=%u (%u) jitter=%d i=%d ni=%d\n",
						avgp[i], avgp[nexti], minp[i], minp[nexti], maxp[i], maxp[nexti], jitter, i, nexti);
				}
				if (avg_pulse < ref_pulse)
					outlog ("FDI: avg_pulse < ref_pulse! (%u < %u)\n", avg_pulse, ref_pulse);
				pulse += avg_pulse - ref_pulse;
				ref_pulse = 0;
				if (i == eodat)
					outstep++;
			} else if ((unsigned int)rand () <= ((idx[i] * RAND_MAX) / maxidx)) {
				avg_pulse = avgp[i];
				min_pulse = minp[i];
				max_pulse = maxp[i];
				randval = rand();
				if (randval < (RAND_MAX / 2)) {
					if (randval > (RAND_MAX / 4)) {
						if (randval <= (3 * (RAND_MAX / 8)))
							randval = (2 * randval) - (RAND_MAX /4);
						else
							randval = (4 * randval) - RAND_MAX;
					}
					avg_pulse -= (randval * (avg_pulse - min_pulse)) / RAND_MAX;
				} else {
					randval -= RAND_MAX / 2;
					if (randval > (RAND_MAX / 4)) {
						if (randval <= (3 * (RAND_MAX / 8)))
							randval = (2 * randval) - (RAND_MAX /4);
						else
							randval = (4 * randval) - RAND_MAX;
					}
					avg_pulse += (randval * (max_pulse - avg_pulse)) / RAND_MAX;
				}
				if ((avg_pulse > ref_pulse) && (avg_pulse < (avgp[nexti] - jitter))) {
					pulse += avg_pulse - ref_pulse;
					ref_pulse = avg_pulse;
				}
			}
			if (outstep == 1 && indexoffset == i)
			    *indexoffsetp = bitoffset;
 		}

		/* gets the size in bits from the pulse width, considering the current average bitrate */
		adjusted_pulse = pulse;
		real_size = 0;
		if (mfm) {
		    while (adjusted_pulse >= avg_size) {
			    real_size += 4;
			    adjusted_pulse -= avg_size / 2;
		    }
		    adjusted_pulse <<= 3;
		    while (adjusted_pulse >= ((avg_size * 4) + (avg_size / 4))) {
			    real_size += 2;
			    adjusted_pulse -= avg_size * 2;
		    }
		    if (adjusted_pulse >= ((avg_size * 3) + (avg_size / 4))) {
			    if (adjusted_pulse <= ((avg_size * 4) - (avg_size / 4))) {
				    if ((2 * ((adjusted_pulse >> 2) - adjust)) <= ((2 * avg_size) - (avg_size / 4)))
					    real_size += 3;
				    else
					    real_size += 4;
			    } else
				    real_size += 4;
		    } else {
			    if (adjusted_pulse > ((avg_size * 3) - (avg_size / 4))) {
				    real_size += 3;
			    } else {
				    if (adjusted_pulse >= ((avg_size * 2) + (avg_size / 4))) {
					    if ((2 * ((adjusted_pulse >> 2) - adjust)) < (avg_size + (avg_size / 4)))
						    real_size += 2;
					    else
						    real_size += 3;
				    } else
					    real_size += 2;
			    }
		    }
		} else {
		    while (adjusted_pulse >= (2*avg_size))
		    {
			    real_size+=4;
			    adjusted_pulse-=avg_size;
		    }
		    adjusted_pulse<<=2;
		    while (adjusted_pulse >= ((avg_size*3)+(avg_size/4)))
		    {
			    real_size+=2;
			    adjusted_pulse-=avg_size*2;
		    }
		    if (adjusted_pulse >= ((avg_size*2)+(avg_size/4)))
		    {
			    if (adjusted_pulse <= ((avg_size*3)-(avg_size/4)))
			    {
				    if (((adjusted_pulse>>1)-adjust) < (avg_size+(avg_size/4)))
					    real_size+=2;
				    else
					    real_size+=3;
			    }
			    else
				    real_size+=3;
		    }
		    else
		    {
			    if (adjusted_pulse > ((avg_size*2)-(avg_size/4)))
				    real_size+=2;
			    else
			    {
				    if (adjusted_pulse >= (avg_size+(avg_size/4)))
				    {
					    if (((adjusted_pulse>>1)-adjust) <= (avg_size-(avg_size/4)))
						    real_size++;
					    else
						    real_size+=2;
				    }
				    else
					    real_size++;
			    }
		    }
		}

		/* after one pass to correctly initialize the average bitrate, outputs the bits */
		if (outstep == 1) {
		        unsigned int j;
			for (j = real_size; j > 1; j--)
				addbit (d, 0);
			addbit (d, 1);
			for (j = 0; j <	real_size; j++)
				*pt++ =	(uae_u16)(pulse	/ real_size);
		}

		/* prepares for the next pulse */
		adjust = ((real_size * avg_size) / (4 << mfm)) - pulse;
		total -= psarray[array_index].size;
		totaldiv -= psarray[array_index].number_of_bits;
		psarray[array_index].size = pulse;
		psarray[array_index].number_of_bits = real_size;
		total += pulse;
		totaldiv += real_size;
		array_index++;
		if (array_index	>= FDI_MAX_ARRAY)
			array_index = 0;
	}

	fdi->out = bitoffset;
}


/****i* januae-fdi/fdi2_celltiming
*
*   NAME
*       fdi2_celltiming() -- Convert pulse timing into output cell timing.
*
*   SYNOPSIS
*       static void fdi2_celltiming (FDI *fdi, unsigned long totalavg, unsigned
*       int bitoffset, uae_u16 *out);
*
*   FUNCTION
*       Average every eight decoded bit timings and scale them to units of the
*       track's average bit duration; duplicate the first output timing in the
*       two trailing entries.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       totalavg - Total averaged pulse duration for one track.
*       bitoffset - Number of generated bit cells to convert.
*       out - Caller-provided decoded data or cell-timing buffer.
*
***************************************************************************
*
*/

static void fdi2_celltiming (FDI *fdi, unsigned	long totalavg, unsigned int bitoffset, uae_u16 *out)
{
	uae_u16	*pt2, *pt;
	double avg_bit_len;
	unsigned int i;

	avg_bit_len = (double)totalavg / (double)bitoffset;
	pt2 = fdi->track_dst_buffer_timing;
	pt = out;
	for (i = 0; i <	bitoffset / 8; i++) {
		double v = (pt2[0] + pt2[1] + pt2[2] + pt2[3] +	pt2[4] + pt2[5]	+ pt2[6] + pt2[7]) / 8.0;
		v = 1000.0 * v / avg_bit_len;
		*pt++ = (uae_u16)v;
		pt2 += 8;
	}
	*pt++ = out[0];
	*pt = out[0];
}

/****i* januae-fdi/decode_lowlevel_track
*
*   NAME
*       decode_lowlevel_track() -- Read and cache a low-level FDI track.
*
*   SYNOPSIS
*       static int decode_lowlevel_track (FDI *fdi, unsigned int track, struct
*       fdi_cache *cache);
*
*   FUNCTION
*       Decode compressed pulse averages, optional extrema and index
*       measurements, determine stable pulses and index position, and store
*       buffer ownership flags in the per-track cache.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       track - Zero-based FDI track index.
*       cache - Per-track FDI cache populated on successful pulse decoding.
*
*   RESULT
*       One on cache creation; -1 for a zero pulse count or unsupported average
*       stream. Other decode failures are not uniformly checked.
*
***************************************************************************
*
*/

static int decode_lowlevel_track (FDI *fdi, unsigned int track, struct fdi_cache *cache)
{
	uae_u8 *p1, *d;
	uae_u32	*p2;
	uae_u32	*avgp, *minp = 0, *maxp	= 0;
	uae_u8 *idxp = 0;
	uae_u32	maxidx,	totalavg, weakbits;
	unsigned int i, j, len, pulses, indexoffset;
	unsigned int avg_free, min_free = 0, max_free = 0, idx_free;
	unsigned int idx_off1 = 0, idx_off2 = 0, idx_off3 = 0;

	d = fdi->track_dst;
	p1 = fdi->track_src;
	pulses = get_u32 (p1);
	if (!pulses)
		return -1;
	p1 += 4;
	len = 12;
	avgp = (uae_u32*)fdi_decompress	(pulses, p1 + 0, p1 + len, &avg_free);
	dumpstream(track, (uae_u8*)avgp, pulses);
	len += get_u24 (p1 + 0)	& 0x3fffff;
	if (!avgp)
		return -1;
	if (get_u24 (p1	+ 3) &&	get_u24	(p1 + 6)) {
		minp = (uae_u32*)fdi_decompress	(pulses, p1 + 3, p1 + len, &min_free);
		len += get_u24 (p1 + 3)	& 0x3fffff;
		maxp = (uae_u32*)fdi_decompress	(pulses, p1 + 6, p1 + len, &max_free);
		len += get_u24 (p1 + 6)	& 0x3fffff;
		/* Computes the real min and max values */
		for (i = 0; i < pulses; i++) {
			maxp[i] = avgp[i] + minp[i] - maxp[i];
			minp[i] = avgp[i] - minp[i];
		}
	} else {
		minp = avgp;
		maxp = avgp;
	}
	if (get_u24 (p1	+ 9)) {
		idx_off1 = 0;
		idx_off2 = 1;
		idx_off3 = 2;
		idxp = fdi_decompress (pulses, p1 + 9, p1 + len, &idx_free);
		if (idx_free) {
			if (idxp[0] == 0 && idxp[1] == 0) {
				idx_off1 = 2;
				idx_off2 = 3;
			} else {
				idx_off1 = 1;
				idx_off2 = 0;
			}
			idx_off3 = 4;
		}
	} else {
		idxp = fdi_malloc (pulses * 2);
		idx_free = 1;
		for (i = 0; i <	pulses;	i++) {
			idxp[i * 2 + 0]	= 2;
			idxp[i * 2 + 1]	= 0;
		}
		idxp[0]	= 1;
		idxp[1]	= 1;
	}

	maxidx = 0;
	indexoffset = 0;
	p1 = idxp;
	for (i = 0; i <	pulses;	i++) {
		if (p1[idx_off1] + p1[idx_off2]	> maxidx)
			maxidx = p1[idx_off1] +	p1[idx_off2];
		p1 += idx_off3;
	}
	p1 = idxp;
	for (i = 0; (i < pulses) && (p1[idx_off2] != 0); i++) /* falling edge, replace with idx_off1 for rising edge */
		p1 += idx_off3;
	if (i < pulses) {
		j = i;
		do {
			i++;
			p1 += idx_off3;
			if (i >= pulses) {
				i = 0;
				p1 = idxp;
			}
		} while ((i != j) && (p1[idx_off2] == 0)); /* falling edge, replace with idx_off1 for rising edge */
		if (i != j) /* index pulse detected */
		{
			while ((i != j) && (p1[idx_off1] > p1[idx_off2])) { /* falling edge, replace with "<" for rising edge */
				i++;
 				p1 += idx_off3;
				if (i >= pulses) {
					i = 0;
					p1 = idxp;
				}
			}
			if (i != j)
				indexoffset = i; /* index position detected */
		}
	}
	p1 = idxp;
	p2 = avgp;
	totalavg = 0;
	weakbits = 0;
	for (i = 0; i <	pulses;	i++) {
		uae_u32 sum = p1[idx_off1] + p1[idx_off2];
		if (sum >= maxidx) {
			totalavg += *p2;
		} else {
			weakbits++;
		}
		p2++;
		p1 += idx_off3;
		idxp[i] = sum;
	}
	len = totalavg / 100000;
	outlog ("totalavg=%u index=%d (%d) maxidx=%d weakbits=%d len=%d\n",
		totalavg, indexoffset, maxidx, weakbits, len);
	cache->avgp = avgp;
	cache->idxp = idxp;
	cache->minp = minp;
	cache->maxp = maxp;
	cache->avg_free = avg_free;
	cache->idx_free = idx_free;
	cache->min_free = min_free;
	cache->max_free = max_free;
	cache->totalavg = totalavg;
	cache->pulses = pulses;
	cache->maxidx = maxidx;
	cache->indexoffset = indexoffset;
	cache->weakbits = weakbits;
	cache->lowlevel = 1;

	return 1;
}

static const char fdiid[] = "Formatted Disk Image file";
static const unsigned int bit_rate_table[16] = { 125,150,250,300,500,1000 };

/****i* januae-fdi/fdi2raw_header_free
*
*   NAME
*       fdi2raw_header_free() -- Release an FDI decoder and its track caches.
*
*   SYNOPSIS
*       void fdi2raw_header_free (FDI *fdi);
*
*   FUNCTION
*       Free converter work buffers, owned pulse arrays in every cache and the
*       FDI context. The associated zfile is not closed here; a null FDI pointer
*       is not accepted.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
***************************************************************************
*
*/

void fdi2raw_header_free (FDI *fdi)
{
	int i;

	fdi_free (fdi->mfmsync_buffer);
	fdi_free (fdi->track_src_buffer);
	fdi_free (fdi->track_dst_buffer);
	fdi_free (fdi->track_dst_buffer_timing);
	for (i = 0; i <	MAX_TRACKS; i++) {
		struct fdi_cache *c = &fdi->cache[i];
		if (c->idx_free)
			fdi_free (c->idxp);
		if (c->avg_free)
			fdi_free (c->avgp);
		if (c->min_free)
			fdi_free (c->minp);
		if (c->max_free)
			fdi_free (c->maxp);
	}
	fdi_free (fdi);
	debuglog ("FREE: memory allocated %d\n", fdi_allocated);
}

/****i* januae-fdi/fdi2raw_get_last_track
*
*   NAME
*       fdi2raw_get_last_track() -- Read the track-count field.
*
*   SYNOPSIS
*       int fdi2raw_get_last_track (FDI *fdi);
*
*   FUNCTION
*       Return the stored last_track field, which is derived from cylinder count
*       and head count and capped at MAX_TRACKS.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
*   RESULT
*       Stored last_track value.
*
***************************************************************************
*
*/

int fdi2raw_get_last_track (FDI	*fdi)
{
	return fdi->last_track;
}

/****i* januae-fdi/fdi2raw_get_num_sector
*
*   NAME
*       fdi2raw_get_num_sector() -- Read default Amiga sectors per track.
*
*   SYNOPSIS
*       int fdi2raw_get_num_sector (FDI *fdi);
*
*   FUNCTION
*       Return 22 when header byte 152 equals 0x02, otherwise 11.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
*   RESULT
*       22 or 11 sectors.
*
***************************************************************************
*
*/

int fdi2raw_get_num_sector (FDI	*fdi)
{
	if (fdi->header[152] ==	0x02)
		return 22;
	return 11;
}

/****i* januae-fdi/fdi2raw_get_last_head
*
*   NAME
*       fdi2raw_get_last_head() -- Read the last head index.
*
*   SYNOPSIS
*       unsigned int fdi2raw_get_last_head (FDI *fdi);
*
*   FUNCTION
*       Return the last_head field as stored in the FDI header.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
*   RESULT
*       Last head index.
*
***************************************************************************
*
*/

unsigned int fdi2raw_get_last_head (FDI *fdi)
{
	return fdi->last_head;
}

/****i* januae-fdi/fdi2raw_get_rotation
*
*   NAME
*       fdi2raw_get_rotation() -- Read disk rotation information.
*
*   SYNOPSIS
*       unsigned int fdi2raw_get_rotation (FDI *fdi);
*
*   FUNCTION
*       Return the rotation_speed value derived from the FDI header.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
*   RESULT
*       Stored rotation value.
*
***************************************************************************
*
*/

unsigned int fdi2raw_get_rotation (FDI *fdi)
{
	return fdi->rotation_speed;
}

/****i* januae-fdi/fdi2raw_get_bit_rate
*
*   NAME
*       fdi2raw_get_bit_rate() -- Read the most recently selected track bitrate.
*
*   SYNOPSIS
*       unsigned int fdi2raw_get_bit_rate (FDI *fdi);
*
*   FUNCTION
*       Return the bit_rate value set during track loading.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
*   RESULT
*       Selected bitrate.
*
***************************************************************************
*
*/

unsigned int fdi2raw_get_bit_rate (FDI *fdi)
{
	return fdi->bit_rate;
}

/****i* januae-fdi/fdi2raw_get_type
*
*   NAME
*       fdi2raw_get_type() -- Read disk format type.
*
*   SYNOPSIS
*       int fdi2raw_get_type (FDI *fdi);
*
*   FUNCTION
*       Return the disk_type field from the FDI header.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
*   RESULT
*       Stored disk type.
*
***************************************************************************
*
*/

int fdi2raw_get_type (FDI *fdi)
{
	return fdi->disk_type;
}

/****i* januae-fdi/fdi2raw_get_write_protect
*
*   NAME
*       fdi2raw_get_write_protect() -- Read the image write-protect flag.
*
*   SYNOPSIS
*       int fdi2raw_get_write_protect (FDI *fdi);
*
*   FUNCTION
*       Return bit zero of the FDI write-protect header field.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*
*   RESULT
*       Zero or one.
*
***************************************************************************
*
*/

int fdi2raw_get_write_protect (FDI *fdi)
{
	return fdi->write_protect;
}

/****i* januae-fdi/fdi2raw_header
*
*   NAME
*       fdi2raw_header() -- Open and parse an FDI image header.
*
*   SYNOPSIS
*       FDI *fdi2raw_header(struct zfile *f);
*
*   FUNCTION
*       Read the 2048-byte header, verify the format marker and major version,
*       allocate work buffers and construct track offsets. The original file
*       cursor is restored; the caller retains ownership of f.
*
*   INPUTS
*       f - Caller-owned zfile containing the FDI image.
*
*   RESULT
*       Allocated FDI context on accepted header, or NULL on rejected header.
*
***************************************************************************
*
*/

FDI *fdi2raw_header(struct zfile *f)
{
	unsigned int i, offset, oldseek;
	uae_u8 type, size;
	FDI *fdi;

	debuglog ("ALLOC: memory allocated %d\n", fdi_allocated);
	fdi = fdi_malloc (sizeof(FDI));
	memset (fdi, 0,	sizeof (FDI));
	fdi->file = f;
	oldseek	= zfile_ftell (fdi->file);
	zfile_fseek (fdi->file,	0, SEEK_SET);
	zfile_fread (fdi->header, 2048,	1, fdi->file);
	zfile_fseek (fdi->file,	oldseek, SEEK_SET);
	if (memcmp (fdiid, fdi->header,	strlen (fdiid))) {
		fdi_free(fdi);
		return NULL;
	}
	if ((fdi->header[140] != 1 && fdi->header[140] != 2) ||	fdi->header[141] != 0) {
		fdi_free(fdi);
		return NULL;
	}

	fdi->mfmsync_buffer = fdi_malloc (MAX_MFM_SYNC_BUFFER *	sizeof(int));
	fdi->track_src_buffer =	fdi_malloc (MAX_SRC_BUFFER);
	fdi->track_dst_buffer =	fdi_malloc (MAX_DST_BUFFER);
	fdi->track_dst_buffer_timing = fdi_malloc (MAX_TIMING_BUFFER);

	fdi->last_track	= ((fdi->header[142] <<	8) + fdi->header[143]) + 1;
	fdi->last_track	*= fdi->header[144] + 1;
	if (fdi->last_track > MAX_TRACKS)
		fdi->last_track	= MAX_TRACKS;
	fdi->last_head = fdi->header[144];
	fdi->disk_type = fdi->header[145];
	fdi->rotation_speed = fdi->header[146] + 128;
	fdi->write_protect = fdi->header[147] &	1;
	outlog ("FDI version %d.%d\n", fdi->header[140], fdi->header[141]);
	outlog ("last_track=%d rotation_speed=%d\n", fdi->last_track, fdi->rotation_speed);

	offset = 512;
	i = fdi->last_track;
	if (i >	180) {
		offset += 512;
		i -= 180;
		while (i > 256)	{
			offset += 512;
			i -= 256;
		}
	}
	for (i = 0; i <	fdi->last_track; i++) {
		fdi->track_offsets[i] =	offset;
		type = fdi->header[152 + i * 2];
		size = fdi->header[152 + i * 2 + 1];
		if (type == 1)
			offset += (size	& 15) *	512;
		else if	((type & 0xc0) == 0x80)
			offset += (((type & 0x3f) << 8)	| size)	* 256;
		else
			offset += size * 256;
	}
	fdi->track_offsets[i] =	offset;

	return fdi;
}


/****i* januae-fdi/fdi2raw_loadrevolution_2
*
*   NAME
*       fdi2raw_loadrevolution_2() -- Render one cached low-level revolution.
*
*   SYNOPSIS
*       static int fdi2raw_loadrevolution_2 (FDI *fdi, uae_u16 *mfmbuf, uae_u16
*       *tracktiming, unsigned int track, unsigned int *tracklength, unsigned int
*       *indexoffsetp, int *multirev, int mfm);
*
*   FUNCTION
*       Re-decode cached pulse measurements into caller MFM words and cell
*       timings, updating tracklength and optional index and multi-revolution
*       outputs. Requires a populated cache.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       mfmbuf - Caller-provided destination for decoded 16-bit MFM words.
*       tracktiming - Caller buffer for decoded cell timing values.
*       track - Zero-based FDI track index.
*       tracklength - Output for the decoded track length in bits.
*       indexoffsetp - Optional index position output, updated when detected.
*       multirev - Optional output indicating weak-bit-driven multiple
*       revolutions.
*       mfm - MFM timing mode requested by the caller.
*
*   RESULT
*       One after rendering; no independent error result is returned by this
*       routine.
*
***************************************************************************
*
*/

static int fdi2raw_loadrevolution_2 (FDI *fdi, uae_u16 *mfmbuf, uae_u16 *tracktiming, unsigned int track, unsigned int *tracklength, unsigned int *indexoffsetp, int *multirev, int mfm)
{
	struct fdi_cache *cache = &fdi->cache[track];
	unsigned int len, i, idx;

	memset (fdi->track_dst_buffer, 0, MAX_DST_BUFFER);
	idx = cache->indexoffset;
	fdi2_decode (fdi, cache->totalavg,
		cache->avgp, cache->minp, cache->maxp, cache->idxp,
		cache->maxidx, &idx, cache->pulses, mfm);
	//fdi2_gcr_decode (fdi, totalavg, avgp, minp, maxp, idxp, idx_off1, idx_off2, idx_off3, maxidx, pulses);
	outlog ("track %d: nbits=%d avg len=%.2f weakbits=%d idx=%d\n",
		track, bitoffset, (double)cache->totalavg / bitoffset, cache->weakbits, cache->indexoffset);
	len = fdi->out;
	if (cache->weakbits >= 10 && multirev)
		*multirev = 1;
	*tracklength = len;

	for (i = 0; i <	(len + 15) / (2 * 8); i++) {
		uae_u8 *data = fdi->track_dst_buffer + i * 2;
		*mfmbuf++ = 256	* *data	+ *(data + 1);
	}
	fdi2_celltiming	(fdi, cache->totalavg, len, tracktiming);
	if (indexoffsetp)
		*indexoffsetp =	idx;
	return 1;
}

/****i* januae-fdi/fdi2raw_loadrevolution
*
*   NAME
*       fdi2raw_loadrevolution() -- Render a cached track revolution.
*
*   SYNOPSIS
*       int fdi2raw_loadrevolution (FDI *fdi, uae_u16 *mfmbuf, uae_u16
*       *tracktiming, unsigned int track, unsigned int *tracklength, int mfm);
*
*   FUNCTION
*       Call the low-level revolution decoder without requesting index position
*       or multi-revolution status.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       mfmbuf - Caller-provided destination for decoded 16-bit MFM words.
*       tracktiming - Caller buffer for decoded cell timing values.
*       track - Zero-based FDI track index.
*       tracklength - Output for the decoded track length in bits.
*       mfm - MFM timing mode requested by the caller.
*
*   RESULT
*       Result of the underlying revolution renderer.
*
***************************************************************************
*
*/

int fdi2raw_loadrevolution (FDI *fdi, uae_u16 *mfmbuf, uae_u16 *tracktiming, unsigned int track, unsigned int *tracklength, int mfm)
{
	return fdi2raw_loadrevolution_2 (fdi, mfmbuf, tracktiming, track, tracklength, 0, 0, mfm);
}

/****i* januae-fdi/fdi2raw_loadtrack
*
*   NAME
*       fdi2raw_loadtrack() -- Load and decode one FDI track.
*
*   SYNOPSIS
*       int fdi2raw_loadtrack (FDI *fdi, uae_u16 *mfmbuf, uae_u16 *tracktiming,
*       unsigned int track, unsigned int *tracklength, unsigned int
*       *indexoffsetp, int *multirev, int mfm);
*
*   FUNCTION
*       Read a track by its cached offsets, select pulse, raw, sector-described
*       or standard encoding, and fill caller MFM data and tracklength. Cached
*       low-level tracks use the revolution renderer.
*
*   INPUTS
*       fdi - Initialized FDI decoding context with its working buffers.
*       mfmbuf - Caller-provided destination for decoded 16-bit MFM words.
*       tracktiming - Caller buffer for decoded cell timing values.
*       track - Zero-based FDI track index.
*       tracklength - Output for the decoded track length in bits.
*       indexoffsetp - Optional index position output, updated when detected.
*       multirev - Optional output indicating weak-bit-driven multiple
*       revolutions.
*       mfm - MFM timing mode requested by the caller.
*
*   RESULT
*       Decoded bit count for non-pulse tracks; one for pulse decoding; zero on
*       the decoder error flag; -1 for an unsupported track type.
*
***************************************************************************
*
*/

int fdi2raw_loadtrack (FDI *fdi, uae_u16 *mfmbuf, uae_u16 *tracktiming, unsigned int track, unsigned int *tracklength, unsigned int *indexoffsetp, int *multirev, int mfm)
{
	uae_u8 *p;
	int outlen;
        unsigned int indexoffset = 0;
	struct fdi_cache *cache = &fdi->cache[track];

	if (cache->lowlevel)
		return fdi2raw_loadrevolution_2 (fdi, mfmbuf, tracktiming, track, tracklength, indexoffsetp, multirev, mfm);

	fdi->err = 0;
	fdi->track_src_len = fdi->track_offsets[track +	1] - fdi->track_offsets[track];
	zfile_fseek (fdi->file, fdi->track_offsets[track], SEEK_SET);
	zfile_fread (fdi->track_src_buffer, fdi->track_src_len, 1, fdi->file);
	memset (fdi->track_dst_buffer, 0, MAX_DST_BUFFER);
	fdi->track_dst_buffer_timing[0]	= 0;

	fdi->current_track = track;
	fdi->track_src = fdi->track_src_buffer;
	fdi->track_dst = fdi->track_dst_buffer;
	p = fdi->header + 152 + fdi->current_track * 2;
	fdi->track_type = *p++;
	fdi->track_len = *p++;
	fdi->bit_rate = 0;
	fdi->out = 0;
	fdi->mfmsync_offset = 0;

	if ((fdi->track_type & 0xf0) ==	0xf0 ||	(fdi->track_type & 0xf0) == 0xe0)
		fdi->bit_rate = bit_rate_table[fdi->track_type & 0x0f];
	else
		fdi->bit_rate = 250;

	outlog ("track %d: srclen: %d track_type: %02.2X, bitrate: %d\n",
		fdi->current_track, fdi->track_src_len,	fdi->track_type, fdi->bit_rate);

	if ((fdi->track_type & 0xc0) ==	0x80) {

		outlen = decode_lowlevel_track (fdi, track, cache);

	} else if ((fdi->track_type & 0xf0) == 0xf0) {

		outlen = decode_raw_track (fdi);

	} else if ((fdi->track_type & 0xf0) == 0xe0) {

		outlen = handle_sectors_described_track	(fdi);

	} else if ((fdi->track_type & 0xf0)) {

		zxx (fdi);
		outlen = -1;

	} else if (fdi->track_type < 0x10) {

		decode_normal_track[fdi->track_type](fdi);
		fix_mfm_sync (fdi);
		outlen = fdi->out;

	} else {

		zxx (fdi);
		outlen = -1;

	}

//	amiga_check_track (fdi);

	if (fdi->err)
		return 0;

	if (outlen > 0) {
		unsigned int i;

		if (cache->lowlevel)
			return fdi2raw_loadrevolution_2 (fdi, mfmbuf, tracktiming, track, tracklength, indexoffsetp, multirev, mfm);
		*tracklength = fdi->out;

		for (i = 0; i <	((*tracklength) + 15) / (2 * 8); i++) {
			uae_u8 *data = fdi->track_dst_buffer + i * 2;
			*mfmbuf++ = 256	* *data	+ *(data + 1);
		}
	}
	return outlen;
}

#endif
