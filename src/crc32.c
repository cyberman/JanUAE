
#include "sysconfig.h"
#include "sysdeps.h"

#include "crc32.h"

static uae_u32 crc_table32[256];
static uae_u16 crc_table16[256];

/****i* januae-crc32/make_crc_table ****************************************
*
*   NAME
*       make_crc_table() -- Initialize the CRC lookup tables.
*
*   SYNOPSIS
*       static void make_crc_table (void);
*
*   FUNCTION
*       Generate 256 entries each for reflected CRC-32 using polynomial
*       0xEDB88320 and CRC-16 using polynomial 0x1021.
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

static void make_crc_table (void)
{
    uae_u32 c;
    uae_u16 w;
    unsigned int n, k;
    for (n = 0; n < 256; n++) {
	c = (uae_u32) n;
	w = n << 8;
	for (k = 0; k < 8; k++) {
	    c = (c >> 1) ^ (c & 1 ? 0xedb88320 : 0);
	    w = (w << 1) ^ ((w & 0x8000) ? 0x1021 : 0);
	}
	crc_table32[n] = c;
	crc_table16[n] = w;
    }
}

/****i* januae-crc32/get_crc32 *********************************************
*
*   NAME
*       get_crc32() -- Compute a reflected CRC-32 checksum.
*
*   SYNOPSIS
*       uae_u32 get_crc32 (const uae_u8 *buf, unsigned int len);
*
*   FUNCTION
*       Initialize the shared lookup tables when needed, process all source
*       bytes with the reflected CRC-32 recurrence and invert the final
*       accumulator.
*
*   INPUTS
*       buf - Input byte array; len - Number of bytes to include.
*
*   RESULT
*       The computed unsigned CRC-32 value, using an initial accumulator of
*       0xFFFFFFFF.
*
****************************************************************************
*
*/

uae_u32 get_crc32 (const uae_u8 *buf, unsigned int len)
{
    uae_u32 crc;

    if (!crc_table32[1])
	make_crc_table ();

    crc = 0xffffffff;
    while (len-- > 0)
	crc = crc_table32[(crc ^ (*buf++)) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffff;
}

/****i* januae-crc32/get_crc16 *********************************************
*
*   NAME
*       get_crc16() -- Compute a CRC-16 checksum.
*
*   SYNOPSIS
*       uae_u16 get_crc16 (const uae_u8 *buf, unsigned int len);
*
*   FUNCTION
*       Initialize the shared lookup tables when needed and fold the input
*       bytes through the CRC-16 lookup recurrence.
*
*   INPUTS
*       buf - Input byte array; len - Number of bytes to include.
*
*   RESULT
*       The computed unsigned CRC-16 value, using an initial accumulator of
*       0xFFFF.
*
****************************************************************************
*
*/

uae_u16 get_crc16 (const uae_u8 *buf, unsigned int len)
{
    uae_u16 crc;
    if (!crc_table32[1])
	make_crc_table ();

    crc = 0xffff;
    while (len-- > 0)
	crc = (crc << 8) ^ crc_table16[((crc >> 8) ^ (*buf++)) & 0xff];
    return crc;
}
