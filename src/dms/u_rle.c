
/*
 *     xDMS  v1.3  -  Portable DMS archive unpacker  -  Public Domain
 *     Written by     Andre Rodrigues de la Rocha  <adlroc@usa.net>
 *
 *     Run Length Encoding decompression function used in most
 *     modes after decompression by other algorithm
 *
 */

#include <string.h>
#include "cdata.h"
#include "u_rle.h"



/****i* januae-dms/Unpack_RLE **********************************************
*
*   NAME
*       Unpack_RLE()
*
*   SYNOPSIS
*       USHORT Unpack_RLE(UCHAR *in, UCHAR *out, USHORT origsize);
*
*   FUNCTION
*       Expand DMS run-length tokens, treating 0x90 followed by zero as a
*       literal 0x90.
*
*   INPUTS
*       in - RLE-encoded input bytes.
*       out - Destination for expanded bytes.
*       origsize - Required uncompressed length.
*
*   RESULT
*       Zero on completion; 1 when a run would extend beyond the requested
*       output size.
*
*   NOTES
*       A run length of 0xff introduces a 16-bit count. The function has no
*       packed-input length argument.
*
****************************************************************************
*
*/
USHORT Unpack_RLE(UCHAR *in, UCHAR *out, USHORT origsize){
	USHORT n;
	UCHAR a,b, *outend;

	outend = out+origsize;
	while (out<outend){
		if ((a = *in++) != 0x90)
			*out++ = a;
		else if (!(b = *in++))
			*out++ = a;
		else {
			a = *in++;
			if (b == 0xff) {
				n = *in++;
				n = (USHORT)((n<<8) + *in++);
			} else
				n = b;
			if (out+n > outend) return 1;
			memset(out,a,(size_t) n);
			out += n;
		}
	}
	return 0;
}


