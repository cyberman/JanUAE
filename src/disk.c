/************************************************************************ 
 *
 * disk.c
 *
 * Floppy disk emulation
 *
 * Copyright 1995 Hannu Rummukainen
 * Copyright 1995-2001 Bernd Schmidt
 * Copyright 2000-2003 Toni Wilen
 *
 * High Density Drive Handling by Dr. Adil Temel (C) 2001 [atemel1@hotmail.com]
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

#include "uae.h"
#include "options.h"
#include "memory.h"
#include "events.h"
#include "custom.h"
#include "custom_private.h"
#include "ersatz.h"
#include "disk.h"
#include "gui.h"
#include "zfile.h"
#include "newcpu.h"
#include "osemu.h"
#include "execlib.h"
#include "savestate.h"
#include "cia.h"
#include "debug.h"
#ifdef FDI2RAW
#include "fdi2raw.h"
#endif
#include "catweasel.h"
#include "driveclick.h"
#include "fsdb.h"
#ifdef CAPS
#ifdef _WIN32
#include "caps/caps_win32.h"
#else
#include "caps.h"
#endif
#endif
#include "crc32.h"

#include <ctype.h>

static int longwritemode = 0;

/* support HD floppies */
#define FLOPPY_DRIVE_HD
/* writable track length with normal 2us bitcell/300RPM motor (PAL) */
#define FLOPPY_WRITE_LEN (currprefs.ntscmode ? (12798 / 2) : (12668 / 2)) /* 12667 PAL, 12797 NTSC */
#define FLOPPY_WRITE_MAXLEN 0x3800
/* This works out to 350 */
#define FLOPPY_GAP_LEN (FLOPPY_WRITE_LEN - 11 * 544)
/* (cycles/bitcell) << 8, normal = ((2us/280ns)<<8) = ~1830 */
#define NORMAL_FLOPPY_SPEED (currprefs.ntscmode ? 1810 : 1829)
/* max supported floppy drives, for small memory systems */
#define MAX_FLOPPY_DRIVES 4

#ifdef FLOPPY_DRIVE_HD
#define DDHDMULT 2
#else
#define DDHDMULT 1
#endif

#undef DEBUG_DRIVE_ID


/* UAE-1ADF (ADF_EXT2)
 * W reserved
 * W number of tracks (default 2*80=160)
 *
 * W reserved
 * W type, 0=normal AmigaDOS track, 1 = raw MFM
 * L available space for track in bytes (must be even)
 * L track length in bits
 */

static int side, direction;
static uae_u8 selected = 15, disabled;

static uae_u8 writebuffer[544 * 11 * DDHDMULT];

#define DISK_INDEXSYNC 1
#define DISK_WORDSYNC 2
#define DISK_MOTORDELAY 4
#define DISK_REVOLUTION 8 /* 8,16,32,64 */

#define DSKREADY_TIME 4
#define DSKREADY_DOWN_TIME 10

static int diskevent_flag;
static int disk_sync_cycle;

#if 0
#define MAX_DISK_WORDS_PER_LINE 50 /* depends on floppy_speed */
static uae_u32 dma_tab[MAX_DISK_WORDS_PER_LINE + 1];
#endif
static int dskdmaen, dsklength, dsklength2, dsklen;
static uae_u16 dskbytr_val;
static uae_u32 dskpt;
static int dma_enable, bitoffset;
static uae_u16 word, dsksync;
/* Always carried through to the next line.  */
static unsigned int disk_hpos;
static int disk_jitter;

typedef enum { TRACK_AMIGADOS, TRACK_RAW, TRACK_RAW1, TRACK_PCDOS } image_tracktype;
typedef struct {
    uae_u16 len;
    uae_u32 offs;
    unsigned int bitlen, track;
    unsigned int sync;
    image_tracktype type;
} trackid;

#define MAX_TRACKS (2 * 83)

/* We have three kinds of Amiga floppy drives
 * - internal A500/A2000 drive:
 *   ID is always DRIVE_ID_NONE (S.T.A.G expects this)
 * - HD drive (A3000/A4000):
 *   ID is DRIVE_ID_35DD if DD floppy is inserted or drive is empty
 *   ID is DRIVE_ID_35HD if HD floppy is inserted
 * - regular external drive:
 *   ID is always DRIVE_ID_35DD
 */

#define DRIVE_ID_NONE  0x00000000
#define DRIVE_ID_35DD  0xFFFFFFFF
#define DRIVE_ID_35HD  0xAAAAAAAA
#define DRIVE_ID_525SD 0x55555555 /* 40 track 5.25 drive , kickstart does not recognize this */

typedef enum { ADF_NORMAL, ADF_EXT1, ADF_EXT2, ADF_FDI, ADF_IPF, ADF_CATWEASEL, ADF_PCDOS } drive_filetype;
typedef struct {
    struct zfile *diskfile;
    struct zfile *writediskfile;
    drive_filetype filetype;
    trackid trackdata[MAX_TRACKS];
    trackid writetrackdata[MAX_TRACKS];
    int buffered_cyl, buffered_side;
    unsigned int cyl;
    int motoroff;
    int motordelay; /* dskrdy needs some clock cycles before it changes after switching off motor */
    int state;
    int wrprot;
    uae_u16 bigmfmbuf[0x4000 * DDHDMULT];
    uae_u16 tracktiming[0x4000 * DDHDMULT];
    int multi_revolution;
    int skipoffset;
    unsigned int mfmpos;
    unsigned int indexoffset;
    unsigned int tracklen;
    unsigned int prevtracklen;
    int trackspeed;
    int dmalen;
    unsigned int num_tracks, write_num_tracks, num_secs;
    unsigned int hard_num_cyls;
    int dskchange;
    int dskchange_time;
    int dskready;
    int dskready_time;
    int dskready_down_time;
    int steplimit;
    frame_time_t steplimitcycle;
    int indexhack, indexhackmode;
    int ddhd; /* 1=DD 2=HD */
    int drive_id_scnt; /* drive id shift counter */
    int idbit;
    unsigned long drive_id; /* drive id to be reported */
    char newname[256]; /* storage space for new filename during eject delay */
    uae_u32 crc32;
#ifdef FDI2RAW
    FDI *fdi;
#endif
    int useturbo;
    int floppybitcounter; /* number of bits left */
#ifdef CATWEASEL
    catweasel_drive *catweasel;
#else
    int catweasel;
#endif
} drive;

int disk_debug_logging;
int disk_debug_mode;
int disk_debug_track = -1;

#define MIN_STEPLIMIT_CYCLE (CYCLE_UNIT * 150)

static uae_u16 bigmfmbufw[0x4000 * DDHDMULT];

#ifndef SAVE_MEMORY
static drive floppy[MAX_FLOPPY_DRIVES];
#else
static drive *floppy;
#endif

#define MAX_PREVIOUS_FLOPPIES 99

#ifndef MAX_DPATH
#define MAX_DPATH 256
#endif

static char dfxhistory[MAX_PREVIOUS_FLOPPIES][MAX_DPATH];

static uae_u8 exeheader[]={0x00,0x00,0x03,0xf3,0x00,0x00,0x00,0x00};
static uae_u8 bootblock[]={
    0x44,0x4f,0x53,0x00,0xc0,0x20,0x0f,0x19,0x00,0x00,0x03,0x70,0x43,0xfa,0x00,0x18,
    0x4e,0xae,0xff,0xa0,0x4a,0x80,0x67,0x0a,0x20,0x40,0x20,0x68,0x00,0x16,0x70,0x00,
    0x4e,0x75,0x70,0xff,0x60,0xfa,0x64,0x6f,0x73,0x2e,0x6c,0x69,0x62,0x72,0x61,0x72,
    0x79
};

#define FS_OFS_DATABLOCKSIZE 488
#define FS_FLOPPY_BLOCKSIZE 512
#define FS_EXTENSION_BLOCKS 72
#define FS_FLOPPY_TOTALBLOCKS 1760
#define FS_FLOPPY_RESERVED 2

/* taken either from disk.c or from catweasel.c */
void mfmcode (uae_u16 *mfm, unsigned int words);

/****i* januae-disk/writeimageblock
*
*   NAME
*       writeimageblock() -- Write one disk-image sector.
*
*   SYNOPSIS
*       static void writeimageblock (struct zfile *dst, uae_u8 *sector, int
*       offset);
*
*   FUNCTION
*       Seek to the supplied byte offset in the destination image and write one
*       FS_FLOPPY_BLOCKSIZE sector. File errors are not propagated.
*
*   INPUTS
*       dst - Destination image or output storage.
*       sector - 512-byte filesystem sector buffer.
*       offset - Byte offset within the target image or sector.
*
***************************************************************************
*
*/

static void writeimageblock (struct zfile *dst, uae_u8 *sector, int offset)
{
    zfile_fseek (dst, offset, SEEK_SET);
    zfile_fwrite (sector, FS_FLOPPY_BLOCKSIZE, 1, dst);
}

/****i* januae-disk/disk_checksum
*
*   NAME
*       disk_checksum() -- Compute an Amiga filesystem block checksum.
*
*   SYNOPSIS
*       static void disk_checksum (const uae_u8 *p, uae_u8 *c);
*
*   FUNCTION
*       Sum big-endian 32-bit words across one filesystem block and store the
*       negated sum as four big-endian bytes.
*
*   INPUTS
*       p - Input bytes or selected preferences, according to the signature.
*       c - Four-byte checksum destination.
*
***************************************************************************
*
*/

static void disk_checksum (const uae_u8 *p, uae_u8 *c)
{
    uae_u32 cs = 0;
    int i;
    for (i = 0; i < FS_FLOPPY_BLOCKSIZE; i+= 4)
	cs += (p[i] << 24) | (p[i+1] << 16) | (p[i+2] << 8) | (p[i+3] << 0);
    cs = -cs;
    c[0] = cs >> 24; c[1] = cs >> 16; c[2] = cs >> 8; c[3] = cs >> 0;
}

/****i* januae-disk/dirhash
*
*   NAME
*       dirhash() -- Calculate an OFS directory hash.
*
*   SYNOPSIS
*       static int dirhash (const char *name);
*
*   FUNCTION
*       Iterate the case-folded filename through the filesystem hash function
*       and reduce it to the header hash-table size.
*
*   INPUTS
*       name - Image pathname or requested name.
*
*   RESULT
*       Hash bucket index.
*
***************************************************************************
*
*/

static int dirhash (const char *name)
{
    unsigned long hash;
    unsigned int i;

    hash = strlen (name);
    for (i = 0; i < strlen (name); i++) {
	hash = hash * 13;
	hash = hash + toupper (name[i]);
	hash = hash & 0x7ff;
    }
    hash = hash % ((FS_FLOPPY_BLOCKSIZE / 4) - 56);
    return hash;
}

/****i* januae-disk/disk_date
*
*   NAME
*       disk_date() -- Encode the current Amiga filesystem date.
*
*   SYNOPSIS
*       static void disk_date (uae_u8 *p);
*
*   FUNCTION
*       Convert the host local time into Amiga days, minutes and ticks and write
*       its three big-endian fields.
*
*   INPUTS
*       p - Input bytes or selected preferences, according to the signature.
*
***************************************************************************
*
*/

static void disk_date (uae_u8 *p)
{
    time_t t;
    struct tm *today;
    int year, days, minutes, ticks;
    char tmp[10];
    time (&t);
    today = localtime (&t);
    strftime (tmp, sizeof (tmp), "%Y", today);
    year = atol (tmp);
    strftime (tmp, sizeof (tmp), "%j", today);
    days = atol (tmp) - 1;
    strftime (tmp, sizeof (tmp), "%H", today);
    minutes = atol (tmp) * 60;
    strftime (tmp, sizeof (tmp), "%M", today);
    minutes += atol (tmp);
    strftime (tmp, sizeof (tmp), "%S", today);
    ticks = atol (tmp) * 50;
    while (year > 1978) {
	if ( !(year % 100) ? !(year % 400) : !(year % 4) ) days++;
	days += 365;
	year--;
    }
    p[0] = days >> 24; p[1] = days >> 16; p[2] = days >> 8; p[3] = days >> 0;
    p[4] = minutes >> 24; p[5] = minutes >> 16; p[6] = minutes >> 8; p[7] = minutes >> 0;
    p[8] = ticks >> 24; p[9] = ticks >> 16; p[10] = ticks >> 8; p[11] = ticks >> 0;
}

/****i* januae-disk/createbootblock
*
*   NAME
*       createbootblock() -- Initialize an AmigaDOS boot block.
*
*   SYNOPSIS
*       static void createbootblock (uae_u8 *sector, int bootable);
*
*   FUNCTION
*       Zero one sector, write the DOS signature and optionally copy the
*       embedded executable boot code.
*
*   INPUTS
*       sector - 512-byte filesystem sector buffer.
*       bootable - Nonzero to install the embedded boot code.
*
***************************************************************************
*
*/

static void createbootblock (uae_u8 *sector, int bootable)
{
    memset (sector, 0, FS_FLOPPY_BLOCKSIZE);
    memcpy (sector, "DOS", 3);
    if (bootable)
	memcpy (sector, bootblock, sizeof (bootblock));
}

/****i* januae-disk/createrootblock
*
*   NAME
*       createrootblock() -- Initialize an OFS root directory block.
*
*   SYNOPSIS
*       static void createrootblock (uae_u8 *sector, const char *disk_name);
*
*   FUNCTION
*       Set root block identifiers, the volume name and timestamps in the
*       supplied sector. The name is copied without checking its length.
*
*   INPUTS
*       sector - 512-byte filesystem sector buffer.
*       disk_name - Name written into the disk label.
*
***************************************************************************
*
*/

static void createrootblock (uae_u8 *sector, const char *disk_name)
{
    memset (sector, 0, FS_FLOPPY_BLOCKSIZE);
    sector[0+3] = 2;
    sector[12+3] = 0x48;
    sector[312] = sector[313] = sector[314] = sector[315] = (uae_u8)0xff;
    sector[316+2] = 881 >> 8; sector[316+3] = 881 & 255;
    sector[432] = strlen (disk_name);
    strcpy ((char *)sector + 433, disk_name);
    sector[508 + 3] = 1;
    disk_date (sector + 420);
    memcpy (sector + 472, sector + 420, 3 * 4);
    memcpy (sector + 484, sector + 420, 3 * 4);
}

/****i* januae-disk/getblock
*
*   NAME
*       getblock() -- Allocate the next free floppy-image block.
*
*   SYNOPSIS
*       static int getblock (uae_u8 *bitmap);
*
*   FUNCTION
*       Find the first zero entry before the bitmap terminator and mark it
*       allocated.
*
*   INPUTS
*       bitmap - Disk-image block allocation bitmap.
*
*   RESULT
*       Allocated block index, or -1 when the terminator is reached.
*
***************************************************************************
*
*/

static int getblock (uae_u8 *bitmap)
{
    int i = 0;
    while (bitmap[i] != 0xff) {
	if (bitmap[i] == 0) {
	    bitmap[i] = 1;
	    return i;
	}
	i++;
    }
    return -1;
}

/****i* januae-disk/pl
*
*   NAME
*       pl() -- Store a big-endian 32-bit sector field.
*
*   SYNOPSIS
*       static void pl (uae_u8 *sector, int offset, uae_u32 v);
*
*   FUNCTION
*       Write the four bytes of the supplied value at the requested offset
*       within a sector.
*
*   INPUTS
*       sector - 512-byte filesystem sector buffer.
*       offset - Byte offset within the target image or sector.
*       v - Register value, byte value or integer to write, according to the
*       signature.
*
***************************************************************************
*
*/

static void pl (uae_u8 *sector, int offset, uae_u32 v)
{
    sector[offset + 0] = v >> 24;
    sector[offset + 1] = v >> 16;
    sector[offset + 2] = v >> 8;
    sector[offset + 3] = v >> 0;
}

/****i* januae-disk/createdirheaderblock
*
*   NAME
*       createdirheaderblock() -- Create a directory-header sector.
*
*   SYNOPSIS
*       static int createdirheaderblock (uae_u8 *sector, int parent, const char
*       *filename, uae_u8 *bitmap);
*
*   FUNCTION
*       Allocate a bitmap entry and populate the directory type, parent, name
*       and timestamp fields. Filename length is not checked against the sector
*       space.
*
*   INPUTS
*       sector - 512-byte filesystem sector buffer.
*       parent - Parent directory block number.
*       filename - Name of the filesystem entry.
*       bitmap - Disk-image block allocation bitmap.
*
*   RESULT
*       Allocated block index, or -1 if the bitmap has no free entry.
*
***************************************************************************
*
*/

static int createdirheaderblock (uae_u8 *sector, int parent, const char *filename, uae_u8 *bitmap)
{
    int block = getblock (bitmap);

    memset (sector, 0, FS_FLOPPY_BLOCKSIZE);
    pl (sector, 0, 2);
    pl (sector, 4, block);
    disk_date (sector + 512 - 92);
    sector[512 - 80] = strlen (filename);
    strcpy ((char *)sector + 512 - 79, filename);
    pl (sector, 512 - 12, parent);
    pl (sector, 512 - 4, 2);
    return block;
}

/****i* januae-disk/createfileheaderblock
*
*   NAME
*       createfileheaderblock() -- Write an OFS file and its header blocks.
*
*   SYNOPSIS
*       static int createfileheaderblock (struct zfile *z,uae_u8 *sector, int
*       parent, const char *filename, struct zfile *src, uae_u8 *bitmap);
*
*   FUNCTION
*       Allocate file data, extension and header blocks for the input file,
*       populate metadata and write sectors into the destination disk image. The
*       routine assumes sufficient bitmap capacity.
*
*   INPUTS
*       z - Destination floppy image stream.
*       sector - 512-byte filesystem sector buffer.
*       parent - Parent directory block number.
*       filename - Name of the filesystem entry.
*       src - Input stream, input bytes or SaveState buffer, according to the
*       signature.
*       bitmap - Disk-image block allocation bitmap.
*
*   RESULT
*       Index of the file header block, subject to allocation and I/O
*       limitations.
*
***************************************************************************
*
*/

static int createfileheaderblock (struct zfile *z,uae_u8 *sector, int parent, const char *filename, struct zfile *src, uae_u8 *bitmap)
{
    uae_u8 sector2[FS_FLOPPY_BLOCKSIZE];
    uae_u8 sector3[FS_FLOPPY_BLOCKSIZE];
    int block = getblock (bitmap);
    int datablock = getblock (bitmap);
    int datasec = 1;
    int extensions;
    int extensionblock, extensioncounter;
    int size;

    zfile_fseek (src, 0, SEEK_END);
    size = zfile_ftell (src);
    zfile_fseek (src, 0, SEEK_SET);
    extensions = (size + FS_OFS_DATABLOCKSIZE - 1) / FS_OFS_DATABLOCKSIZE;

    memset (sector, 0, FS_FLOPPY_BLOCKSIZE);
    pl (sector, 0, 2);
    pl (sector, 4, block);
    pl (sector, 8, extensions > FS_EXTENSION_BLOCKS ? FS_EXTENSION_BLOCKS : extensions);
    pl (sector, 16, datablock);
    pl (sector, FS_FLOPPY_BLOCKSIZE - 188, size);
    disk_date (sector + FS_FLOPPY_BLOCKSIZE - 92);
    sector[FS_FLOPPY_BLOCKSIZE - 80] = strlen (filename);
    strcpy ((char *)(sector + FS_FLOPPY_BLOCKSIZE - 79), filename);
    pl (sector, FS_FLOPPY_BLOCKSIZE - 12, parent);
    pl (sector, FS_FLOPPY_BLOCKSIZE - 4, -3);
    extensioncounter = 0;
    extensionblock = 0;

    while (size > 0) {
	int datablock2 = datablock;
	int extensionblock2 = extensionblock;
	if (extensioncounter == FS_EXTENSION_BLOCKS) {
	    extensioncounter = 0;
	    extensionblock = getblock (bitmap);
	    if (datasec > FS_EXTENSION_BLOCKS + 1) {
		pl (sector3, 8, FS_EXTENSION_BLOCKS);
		pl (sector3, FS_FLOPPY_BLOCKSIZE - 8, extensionblock);
		pl (sector3, 4, extensionblock2);
		disk_checksum (sector3, sector3 + 20);
		writeimageblock (z, sector3, extensionblock2 * FS_FLOPPY_BLOCKSIZE);
	    } else {
		pl (sector, 512 - 8, extensionblock);
	    }
	    memset (sector3, 0, FS_FLOPPY_BLOCKSIZE);
	    pl (sector3, 0, 16);
	    pl (sector3, FS_FLOPPY_BLOCKSIZE - 12, block);
	    pl (sector3, FS_FLOPPY_BLOCKSIZE - 4, -3);
	}
	memset (sector2, 0, FS_FLOPPY_BLOCKSIZE);
	pl (sector2, 0, 8);
	pl (sector2, 4, block);
	pl (sector2, 8, datasec++);
	pl (sector2, 12, size > FS_OFS_DATABLOCKSIZE ? FS_OFS_DATABLOCKSIZE : size);
	zfile_fread (sector2 + 24, size > FS_OFS_DATABLOCKSIZE ? FS_OFS_DATABLOCKSIZE : size, 1, src);
	size -= FS_OFS_DATABLOCKSIZE;
	datablock = 0;
	if (size > 0) datablock = getblock (bitmap);
	pl (sector2, 16, datablock);
	disk_checksum (sector2, sector2 + 20);
	writeimageblock (z, sector2, datablock2 * FS_FLOPPY_BLOCKSIZE);
	if (datasec <= FS_EXTENSION_BLOCKS + 1)
	    pl (sector, 512 - 204 - extensioncounter * 4, datablock2);
	else
	    pl (sector3, 512 - 204 - extensioncounter * 4, datablock2);
	extensioncounter++;
    }
    if (datasec > FS_EXTENSION_BLOCKS) {
	pl (sector3, 8, extensioncounter);
	disk_checksum (sector3, sector3 + 20);
	writeimageblock (z, sector3, extensionblock * FS_FLOPPY_BLOCKSIZE);
    }
    disk_checksum (sector, sector + 20);
    writeimageblock (z, sector, block * FS_FLOPPY_BLOCKSIZE);
    return block;
}

/****i* januae-disk/createbitmapblock
*
*   NAME
*       createbitmapblock() -- Encode an OFS free-block bitmap.
*
*   SYNOPSIS
*       static void createbitmapblock (uae_u8 *sector, uae_u8 *bitmap);
*
*   FUNCTION
*       Pack the allocation entries into bitmap bytes and compute the filesystem
*       block checksum.
*
*   INPUTS
*       sector - 512-byte filesystem sector buffer.
*       bitmap - Disk-image block allocation bitmap.
*
***************************************************************************
*
*/

static void createbitmapblock (uae_u8 *sector, uae_u8 *bitmap)
{
    uae_u8 mask;
    int i, j;
    memset (sector, 0, FS_FLOPPY_BLOCKSIZE);
    for (i = FS_FLOPPY_RESERVED; i < FS_FLOPPY_TOTALBLOCKS; i += 8) {
	mask = 0;
	for (j = 0; j < 8; j++) {
	    if (bitmap[i + j]) mask |= 1 << j;
	}
	sector[4 + i / 8] = mask;
    }
    disk_checksum (sector, sector + 0);
}

/****i* januae-disk/createimagefromexe
*
*   NAME
*       createimagefromexe() -- Build a bootable AmigaDOS image from an executable.
*
*   SYNOPSIS
*       static int createimagefromexe (struct zfile *src, struct zfile *dst);
*
*   FUNCTION
*       Create a disk containing the executable, an s/startup-sequence file,
*       filesystem metadata and an allocation bitmap.
*
*   INPUTS
*       src - Input stream, input bytes or SaveState buffer, according to the
*       signature.
*       dst - Destination image or output storage.
*
*   RESULT
*       Nonzero if the image is created; zero if the executable cannot fit on
*       the floppy.
*
***************************************************************************
*
*/

static int createimagefromexe (struct zfile *src, struct zfile *dst)
{
    uae_u8 sector1[FS_FLOPPY_BLOCKSIZE], sector2[FS_FLOPPY_BLOCKSIZE];
    uae_u8 bitmap[FS_FLOPPY_TOTALBLOCKS + 8];
    int exesize;
    int blocksize = FS_OFS_DATABLOCKSIZE;
    int blocks, extensionblocks;
    int totalblocks;
    int fblock1, dblock1;
    const char *fname1 = "runme.exe";
    const char *fname2 = "startup-sequence";
    const char *dirname1 = "s";
    struct zfile *ss;

    memset (bitmap, 0, sizeof (bitmap));
    zfile_fseek (src, 0, SEEK_END);
    exesize = zfile_ftell (src);
    blocks = (exesize + blocksize - 1) / blocksize;
    extensionblocks = (blocks + FS_EXTENSION_BLOCKS - 1) / FS_EXTENSION_BLOCKS;
    /* bootblock=2, root=1, bitmap=1, startup-sequence=1+1, exefileheader=1 */
    totalblocks = 2 + 1 + 1 + 2 + 1 + blocks + extensionblocks;
    if (totalblocks > FS_FLOPPY_TOTALBLOCKS)
	return 0;

    bitmap[880] = 1;
    bitmap[881] = 1;
    bitmap[0] = 1;
    bitmap[1] = 1;

    dblock1 = createdirheaderblock (sector2, 880, dirname1, bitmap);
    ss = zfile_fopen_empty (fname1, strlen (fname1));
    zfile_fwrite (fname1, strlen (fname1), 1, ss);
    fblock1 = createfileheaderblock (dst, sector1,  dblock1, fname2, ss, bitmap);
    zfile_fclose (ss);
    pl (sector2, 24 + dirhash (fname2) * 4, fblock1);
    disk_checksum (sector2, sector2 + 20);
    writeimageblock (dst, sector2, dblock1 * FS_FLOPPY_BLOCKSIZE);

    fblock1 = createfileheaderblock (dst, sector1, 880, fname1, src, bitmap);

    createrootblock (sector1, "empty");
    pl (sector1, 24 + dirhash (fname1) * 4, fblock1);
    pl (sector1, 24 + dirhash (dirname1) * 4, dblock1);
    disk_checksum (sector1, sector1 + 20);
    writeimageblock (dst, sector1, 880 * FS_FLOPPY_BLOCKSIZE);

    createbitmapblock (sector1, bitmap);
    writeimageblock (dst, sector1, 881 * FS_FLOPPY_BLOCKSIZE);

    createbootblock (sector1, 1);
    writeimageblock (dst, sector1, 0 * FS_FLOPPY_BLOCKSIZE);

    return 1;
}

/****i* januae-disk/get_floppy_speed
*
*   NAME
*       get_floppy_speed() -- Calculate effective emulated floppy bit timing.
*
*   SYNOPSIS
*       static int get_floppy_speed (void);
*
*   FUNCTION
*       Scale NORMAL_FLOPPY_SPEED by the floppy-speed preference; preference
*       values at or below ten select the default 100 percent rate.
*
*   RESULT
*       Scaled track bit timing value.
*
***************************************************************************
*
*/

static int get_floppy_speed (void)
{
    int m = currprefs.floppy_speed;
    if (m <= 10)
	m = 100;
    m = NORMAL_FLOPPY_SPEED * 100 / m;
    return m;
}

#ifdef DEBUG_DRIVE_ID
static const char *drive_id_name (drive *drv)
    {
    switch (drv->drive_id) {
    case DRIVE_ID_35HD : return "3.5HD";
    case DRIVE_ID_525SD: return "5.25SD";
    case DRIVE_ID_35DD : return "3.5DD";
    }
    return "UNKNOWN";
}
#endif

/* Simulate exact behaviour of an A3000T 3.5 HD disk drive.
 * The drive reports to be a 3.5 DD drive whenever there is no
 * disk or a 3.5 DD disk is inserted. Only 3.5 HD drive id is reported
 * when a real 3.5 HD disk is inserted. -Adil
 */
/****i* januae-disk/drive_settype_id
*
*   NAME
*       drive_settype_id() -- Update the emulated drive-identification code.
*
*   SYNOPSIS
*       static void drive_settype_id (drive *drv);
*
*   FUNCTION
*       Choose the drive ID from the configured mechanism type and inserted
*       medium density, including the 3.5-inch HD-to-DD identification behavior.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
***************************************************************************
*
*/

static void drive_settype_id (drive *drv)
{
    int t = currprefs.dfxtype[drv - &floppy[0]];

    switch (t)
    {
	case DRV_35_HD:
#ifdef FLOPPY_DRIVE_HD
	if (!drv->diskfile || drv->ddhd <= 1)
	    drv->drive_id = DRIVE_ID_35DD;
	else
	    drv->drive_id = DRIVE_ID_35HD;
#else
	drv->drive_id = DRIVE_ID_35DD;
#endif
	break;
	case DRV_35_DD_ESCOM:
	case DRV_35_DD:
	default:
	drv->drive_id = DRIVE_ID_35DD;
	break;
	case DRV_525_SD:
	drv->drive_id = DRIVE_ID_525SD;
	break;
    }
#ifdef DEBUG_DRIVE_ID
    write_log ("drive_settype_id: DF%d: set to %s\n", drv-floppy, drive_id_name (drv));
#endif
}

/****i* januae-disk/drive_image_free
*
*   NAME
*       drive_image_free() -- Release the current image resources.
*
*   SYNOPSIS
*       static void drive_image_free (drive *drv);
*
*   FUNCTION
*       Close the image and overlay streams and free image-format-specific CAPS
*       or FDI state when those backends are compiled.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
***************************************************************************
*
*/

static void drive_image_free (drive *drv)
{
    switch (drv->filetype)
    {
	case ADF_IPF:
#ifdef CAPS
	caps_unloadimage (drv - floppy);
#endif
	break;
	case ADF_FDI:
#ifdef FDI2RAW
	fdi2raw_header_free (drv->fdi);
	drv->fdi = 0;
#endif
	break;
	default:
	    break;
    }
    drv->filetype = -1;
    zfile_fclose (drv->diskfile);
    drv->diskfile = 0;
    zfile_fclose (drv->writediskfile);
    drv->writediskfile = 0;
}

static int drive_insert (drive * drv, struct uae_prefs *p, int dnum, const char *fname);

/****i* januae-disk/reset_drive
*
*   NAME
*       reset_drive() -- Reset one drive's hardware and GUI state.
*
*   SYNOPSIS
*       static void reset_drive (int i);
*
*   FUNCTION
*       Clear runtime fields, apply the configured disabled state, refresh
*       identification and prepare the track timing for the selected unit.
*
*   INPUTS
*       i - Zero-based floppy-unit index.
*
***************************************************************************
*
*/

static void reset_drive (int i)
{
    drive *drv = &floppy[i];
    drive_image_free (drv);
    drv->motoroff = 1;
    disabled &= ~(1 << i);
    gui_data.drive_disabled[i] = 0;
    gui_data.df[i][0] = 0;
    gui_data.crc32[i] = 0;
    if (currprefs.dfxtype[i] < 0) {
	disabled |= 1 << i;
	gui_data.drive_disabled[i] = 1;
    }
    /* most internal Amiga floppy drives won't enable
     * diskready until motor is running at full speed
     * and next indexsync has been passed
     */
    drv->indexhackmode = 0;
    if (i == 0 && currprefs.dfxtype[i] == 0)
	drv->indexhackmode = 1;
    drv->dskchange_time = 0;
    drv->buffered_cyl = -1;
    drv->buffered_side = -1;
    gui_led (i + 1, 0);
    drive_settype_id (drv);
    strcpy (currprefs.df[i], changed_prefs.df[i]);
    drv->newname[0] = 0;
    if (!drive_insert (drv, &currprefs, i, currprefs.df[i]))
	disk_eject (i);
}

/* code for track display */
/****i* januae-disk/update_drive_gui
*
*   NAME
*       update_drive_gui() -- Publish drive state when it changes.
*
*   SYNOPSIS
*       static void update_drive_gui (int num);
*
*   FUNCTION
*       Compare the motor, track, side, image CRC and write state with the
*       displayed values; update the shared GUI snapshot only when different.
*
*   INPUTS
*       num - Zero-based floppy-unit index.
*
***************************************************************************
*
*/

static void update_drive_gui (int num)
{
    drive *drv = floppy + num;
    int writ = dskdmaen == 3 && drv->state && !(selected & (1 << num));

    if (drv->state == gui_data.drive_motor[num]
	&& drv->cyl == gui_data.drive_track[num]
	&& side == gui_data.drive_side
	&& drv->crc32 == gui_data.crc32[num]
	&& writ == gui_data.drive_writing[num])
	return;
    strcpy (gui_data.df[num], currprefs.df[num]);
    gui_data.crc32[num] = drv->crc32;
    gui_data.drive_motor[num] = drv->state;
    gui_data.drive_track[num] = drv->cyl;
    gui_data.drive_side = side;
    gui_data.drive_writing[num] = writ;
    gui_ledstate &= ~(2 << num);
    if (drv->state)
	gui_ledstate |= 2 << num;
    gui_led (num + 1, gui_data.drive_motor[num]);
}

static void drive_fill_bigbuf (drive * drv,int);

/****i* januae-disk/DISK_validate_filename
*
*   NAME
*       DISK_validate_filename() -- Open and examine a disk image.
*
*   SYNOPSIS
*       struct zfile *DISK_validate_filename (const char *fname, int leave_open,
*       int *wrprot, uae_u32 *crc32);
*
*   FUNCTION
*       Open the named image for writing when possible, otherwise for reading,
*       optionally compute its CRC and return the opened stream only when
*       requested.
*
*   INPUTS
*       fname - Disk image pathname.
*       leave_open - Nonzero to return an opened image stream.
*       wrprot - Optional output for image write protection.
*       crc32 - Optional output for the image CRC32.
*
*   RESULT
*       With leave_open true, an opened stream or NULL; with leave_open false,
*       the sentinel pointer (struct zfile *)1 if the path exists, or NULL
*       otherwise. The sentinel must not be dereferenced.
*
***************************************************************************
*
*/

struct zfile *DISK_validate_filename (const char *fname, int leave_open, int *wrprot, uae_u32 *crc32)
{
    if (crc32)
	*crc32 = 0;
    if (leave_open) {
	struct zfile *f = zfile_fopen (fname, "r+b");
	if (f) {
	    if (wrprot)
		*wrprot = 0;
	} else {
	    if (wrprot)
		*wrprot = 1;
	    f = zfile_fopen (fname, "rb");
	}
	if (f && crc32)
	    *crc32 = zfile_crc32 (f);
	return f;
    } else {
	if (zfile_exists (fname)) {
	    if (wrprot)
		*wrprot = 0;
	    if (crc32) {
		struct zfile *f = zfile_fopen (fname, "rb");
		if (f)
		    *crc32 = zfile_crc32 (f);
		zfile_fclose (f);
	    }
	    return (void*)1;
	} else {
	    if (wrprot)
		*wrprot = 1;
	    return 0;
	}
    }
}

/****i* januae-disk/updatemfmpos
*
*   NAME
*       updatemfmpos() -- Preserve the stream position across track lengths.
*
*   SYNOPSIS
*       static void updatemfmpos (drive *drv);
*
*   FUNCTION
*       Rescale the previous MFM position when a previous track length exists,
*       then wrap it to the current track length. A zero track length is not
*       guarded.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
***************************************************************************
*
*/

static void updatemfmpos (drive *drv)
{
    if (drv->prevtracklen)
	drv->mfmpos = drv->mfmpos * (drv->tracklen * 1000 / drv->prevtracklen) / 1000;
    drv->mfmpos %= drv->tracklen;
    drv->prevtracklen = drv->tracklen;
}

/****i* januae-disk/track_reset
*
*   NAME
*       track_reset() -- Initialize an unformatted MFM track buffer.
*
*   SYNOPSIS
*       static void track_reset (drive *drv);
*
*   FUNCTION
*       Reset density-dependent track length and timing, invalidate the buffered
*       side and fill the MFM buffer with the default gap pattern.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
***************************************************************************
*
*/

static void track_reset (drive *drv)
{
    drv->tracklen = FLOPPY_WRITE_LEN * drv->ddhd * 2 * 8;
    drv->trackspeed = get_floppy_speed ();
    drv->buffered_side = -1;
    drv->skipoffset = -1;
    drv->tracktiming[0] = 0;
    memset (drv->bigmfmbuf, 0xaa, FLOPPY_WRITE_LEN * 2 * drv->ddhd);
    updatemfmpos (drv);
}

/****i* januae-disk/read_header_ext2
*
*   NAME
*       read_header_ext2() -- Read an extended ADF track directory.
*
*   SYNOPSIS
*       static unsigned int read_header_ext2 (struct zfile *diskfile, trackid
*       *trackdata, unsigned int *num_tracks, int *ddhd);
*
*   FUNCTION
*       Verify the UAE-1ADF signature and load each track's type, capacity, bit
*       count and starting offset. No bounds check is made against the caller's
*       track array.
*
*   INPUTS
*       diskfile - Disk image stream.
*       trackdata - Caller-supplied track-description array.
*       num_tracks - Output for the number of tracks.
*       ddhd - Disk density multiplier or its optional output.
*
*   RESULT
*       One for a recognized UAE-1ADF header, or zero for a mismatched
*       signature. File I/O errors are not independently reported.
*
***************************************************************************
*
*/

static unsigned int read_header_ext2 (struct zfile *diskfile, trackid *trackdata, unsigned int *num_tracks, int *ddhd)
{
    uae_u8 buffer[2 + 2 + 4 + 4];
    trackid *tid;
    unsigned int offs;
    unsigned int i;

    zfile_fseek (diskfile, 0, SEEK_SET);
    zfile_fread (buffer, 1, 8, diskfile);
    if (strncmp ((char*)buffer, "UAE-1ADF", 8))
	return 0;
    zfile_fread (buffer, 1, 4, diskfile);
    *num_tracks = buffer[2] * 256 + buffer[3];
    offs = 8 + 2 + 2 + (*num_tracks) * (2 + 2 + 4 + 4);

    for (i = 0; i < (*num_tracks); i++) {
	tid = trackdata + i;
	zfile_fread (buffer, 2 + 2 + 4 + 4, 1, diskfile);
	tid->type = buffer[2] * 256 + buffer[3];
	tid->len = buffer[5] * 65536 + buffer[6] * 256 + buffer[7];
	tid->bitlen = buffer[9] * 65536 + buffer[10] * 256 + buffer[11];
	tid->offs = offs;
	if (tid->len > 20000 && ddhd)
	    *ddhd = 2;
	tid->track = i;
	offs += tid->len;
    }
    return 1;
}

/****i* januae-disk/DISK_get_saveimagepath
*
*   NAME
*       DISK_get_saveimagepath() -- Construct a writable overlay-image pathname.
*
*   SYNOPSIS
*       char *DISK_get_saveimagepath (const char *name);
*
*   FUNCTION
*       Build the save-image filename from the original image name using a
*       shared static buffer. The routine copies the input into fixed-size local
*       arrays without explicit length validation.
*
*   INPUTS
*       name - Image pathname or requested name.
*
*   RESULT
*       Pointer to a static path buffer overwritten by the next call.
*
***************************************************************************
*
*/

char *DISK_get_saveimagepath (const char *name)
{
    static char name1[MAX_DPATH];
    char name2[MAX_DPATH];
    int i;

    strcpy (name2, name);
    i = strlen (name2) - 1;
    while (i > 0) {
	if (name2[i] == '.') {
	    name2[i] = 0;
	    break;
	}
	i--;
    }
    while (i > 0) {
	if (name2[i] == '/' || name2[i] == '\\'
#ifdef TARGET_AMIGAOS
	    || name2[i] == ':'
#endif
	    ) {
	    i++;
	    break;
	}
	i--;
    }

    sprintf (name1, "%s%s_save.adf", prefs_get_attr ("floppy_path"), name2 + i);

    return name1;
}

/****i* januae-disk/getwritefile
*
*   NAME
*       getwritefile() -- Open the writable sidecar disk image.
*
*   SYNOPSIS
*       static struct zfile *getwritefile (const char *name, int *wrprot);
*
*   FUNCTION
*       Derive the overlay path and request an open image stream with the
*       reported write-protection state.
*
*   INPUTS
*       name - Image pathname or requested name.
*       wrprot - Optional output for image write protection.
*
*   RESULT
*       Opened overlay stream, or NULL on failure.
*
***************************************************************************
*
*/

static struct zfile *getwritefile (const char *name, int *wrprot)
{
    return DISK_validate_filename (DISK_get_saveimagepath (name), 1, wrprot, NULL);
}

/****i* januae-disk/iswritefileempty
*
*   NAME
*       iswritefileempty() -- Determine whether a sidecar contains saved tracks.
*
*   SYNOPSIS
*       static int iswritefileempty (const char *name);
*
*   FUNCTION
*       Open the overlay and inspect its extended-image track directory to
*       decide whether any track contains data.
*
*   INPUTS
*       name - Image pathname or requested name.
*
*   RESULT
*       Nonzero when the overlay cannot be opened or contains no track data;
*       zero when saved track data is present or the header is not recognized.
*
***************************************************************************
*
*/

static int iswritefileempty (const char *name)
{
    struct zfile *zf;
    int wrprot;
    uae_u8 buffer[8];
    trackid td[MAX_TRACKS];
    unsigned int tracks, i, ret;
    int ddhd;

    zf = getwritefile (name, &wrprot);
    if (!zf) return 1;
    zfile_fread (buffer, sizeof (char), 8, zf);
    if (strncmp ((char *) buffer, "UAE-1ADF", 8))
	return 0;
    ret = read_header_ext2 (zf, td, &tracks, &ddhd);
    zfile_fclose (zf);
    if (!ret)
	return 1;
    for (i = 0; i < tracks; i++) {
	if (td[i].bitlen) return 0;
    }
    return 1;
}

/****i* januae-disk/openwritefile
*
*   NAME
*       openwritefile() -- Prepare the selected drive's writable overlay.
*
*   SYNOPSIS
*       static int openwritefile (drive *drv, int create);
*
*   FUNCTION
*       Open and validate an extended-ADF sidecar or create one when the image
*       format requires a separate write stream.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*       create - Nonzero to request a write-overlay creation path.
*
*   RESULT
*       Nonzero if a suitable writable image is attached; zero when no usable
*       write image is available.
*
***************************************************************************
*
*/

static int openwritefile (drive *drv, int create)
{
    int wrprot = 0;

    drv->writediskfile = getwritefile (currprefs.df[drv - &floppy[0]], &wrprot);
    if (drv->writediskfile) {
	drv->wrprot = wrprot;
	if (!read_header_ext2 (drv->writediskfile, drv->writetrackdata, &drv->write_num_tracks, 0)) {
	    zfile_fclose (drv->writediskfile);
	    drv->writediskfile = 0;
	    drv->wrprot = 1;
	} else {
	    if (drv->write_num_tracks > drv->num_tracks)
		drv->num_tracks = drv->write_num_tracks;
	}
    } else if (zfile_iscompressed (drv->diskfile)) {
	drv->wrprot = 1;
    }
    return drv->writediskfile ? 1 : 0;
}

/****i* januae-disk/diskfile_iswriteprotect
*
*   NAME
*       diskfile_iswriteprotect() -- Determine image write protection and drive type.
*
*   SYNOPSIS
*       static int diskfile_iswriteprotect (const char *fname, int
*       *needwritefile, drive_type *drvtype);
*
*   FUNCTION
*       Inspect image access, compression and recognized file formats and report
*       whether separate write storage is required.
*
*   INPUTS
*       fname - Disk image pathname.
*       needwritefile - Output indicating whether a write overlay is needed.
*       drvtype - Output for drive mechanism type.
*
*   RESULT
*       Nonzero when the original image is considered write protected.
*
***************************************************************************
*
*/

static int diskfile_iswriteprotect (const char *fname, int *needwritefile, drive_type *drvtype)
{
    struct zfile *zf1, *zf2;
    int wrprot1 = 0, wrprot2 = 1;
    unsigned char buffer[25];

    *needwritefile = 0;
    *drvtype = DRV_35_DD;
    zf1 = DISK_validate_filename (fname, 1, &wrprot1, NULL);
    if (!zf1)
	return 1;
    if (zfile_iscompressed (zf1)) {
	wrprot1 = 1;
	*needwritefile = 1;
    }

    zf2 = getwritefile (fname, &wrprot2);
    zfile_fclose (zf2);
    zfile_fread (buffer, sizeof (char), 25, zf1);
    zfile_fclose (zf1);

    if (strncmp ((char *) buffer, "CAPS", 4) == 0) {
	*needwritefile = 1;
	return wrprot2;
    }
    if (strncmp ((char *) buffer, "Formatted Disk Image file", 25) == 0) {
	*needwritefile = 1;
	return wrprot2;
    }
    if (strncmp ((char *) buffer, "UAE-1ADF", 8) == 0) {
	if (wrprot1)
	    return wrprot2;
	return wrprot1;
    }
    if (strncmp ((char *) buffer, "UAE--ADF", 8) == 0) {
	*needwritefile = 1;
	return wrprot2;
    }
    if (memcmp (exeheader, buffer, sizeof (exeheader)) == 0)
	return 0;
    if (wrprot1)
	return wrprot2;
    return wrprot1;
}

/****i* januae-disk/drive_insert
*
*   NAME
*       drive_insert() -- Attach an image and determine its track format.
*
*   SYNOPSIS
*       static int drive_insert (drive * drv, struct uae_prefs *p, int dnum,
*       const char *fname);
*
*   FUNCTION
*       Release the previous medium, open the requested image, identify
*       ADF/extended/PC/IPF/FDI variants and initialize geometry and track data.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*       p - Input bytes or selected preferences, according to the signature.
*       dnum - Zero-based target floppy-unit index.
*       fname - Disk image pathname.
*
*   RESULT
*       Nonzero when the drive image is accepted; zero on an unsuccessful image
*       insertion.
*
***************************************************************************
*
*/

static int drive_insert (drive * drv, struct uae_prefs *p, int dnum, const char *fname)
{
    unsigned char buffer[2 + 2 + 4 + 4];
    trackid *tid;
    int size;

    drive_image_free (drv);
    drv->diskfile = DISK_validate_filename (fname, 1, &drv->wrprot, &drv->crc32);
    drv->ddhd = 1;
    drv->num_secs = 0;
    drv->hard_num_cyls = p->dfxtype[dnum] == DRV_525_SD ? 40 : 80;
    drv->tracktiming[0] = 0;
    drv->useturbo = 0;
    drv->indexoffset = 0;

    if (!drv->motoroff) {
	drv->dskready_time = DSKREADY_TIME;
	drv->dskready_down_time = 0;
    }

    if (drv->diskfile == 0 && !drv->catweasel) {
	track_reset (drv);
	return 0;
    }
    strncpy (currprefs.df[dnum], fname, 255);
    currprefs.df[dnum][255] = 0;
    strncpy (changed_prefs.df[dnum], fname, 255);
    changed_prefs.df[dnum][255] = 0;
    strcpy (drv->newname, fname);
    gui_filename (dnum, fname);

    memset (buffer, 0, sizeof (buffer));
    size = 0;
    if (drv->diskfile) {
	zfile_fread (buffer, sizeof (char), 8, drv->diskfile);
	zfile_fseek (drv->diskfile, 0, SEEK_END);
	size = zfile_ftell (drv->diskfile);
	zfile_fseek (drv->diskfile, 0, SEEK_SET);
    }


    if (drv->catweasel) {

	drv->wrprot = 1;
	drv->filetype = ADF_CATWEASEL;
	drv->num_tracks = 80;
	drv->ddhd = 1;

#ifdef CAPS
    } else if (strncmp ((char *) buffer, "CAPS", 4) == 0) {

	unsigned int num_tracks;

	drv->wrprot = 1;
	if (!caps_loadimage (drv->diskfile, drv - floppy, &num_tracks)) {
	    zfile_fclose (drv->diskfile);
	    drv->diskfile = 0;
	    return 0;
	}
	drv->num_tracks = num_tracks;
	drv->filetype = ADF_IPF;
#endif
#ifdef FDI2RAW
    } else if ((drv->fdi = fdi2raw_header (drv->diskfile))) {

	drv->wrprot = 1;
	drv->num_tracks = fdi2raw_get_last_track (drv->fdi);
	drv->num_secs = fdi2raw_get_num_sector (drv->fdi);
	drv->filetype = ADF_FDI;
#endif
    } else if (strncmp ((char *) buffer, "UAE-1ADF", 8) == 0) {

	read_header_ext2 (drv->diskfile, drv->trackdata, &drv->num_tracks, &drv->ddhd);
	drv->filetype = ADF_EXT2;
	drv->num_secs = 11;
	if (drv->ddhd > 1)
	    drv->num_secs = 22;

    } else if (strncmp ((char *) buffer, "UAE--ADF", 8) == 0) {
	int offs = 160 * 4 + 8;
	int i;

	drv->wrprot = 1;
	drv->filetype = ADF_EXT1;
	drv->num_tracks = 160;
	drv->num_secs = 11;

	zfile_fseek (drv->diskfile, 8, SEEK_SET);
	for (i = 0; i < 160; i++) {
	    tid = &drv->trackdata[i];
	    zfile_fread (buffer, 4, 1, drv->diskfile);
	    tid->sync = buffer[0] * 256 + buffer[1];
	    tid->len = buffer[2] * 256 + buffer[3];
	    tid->offs = offs;
	    if (tid->sync == 0) {
		tid->type = TRACK_AMIGADOS;
		tid->bitlen = 0;
	    } else {
		tid->type = TRACK_RAW1;
		tid->bitlen = tid->len * 8;
	    }
	    offs += tid->len;
	}

    } else if (memcmp (exeheader, buffer, sizeof (exeheader)) == 0) {

	unsigned int i;
	struct zfile *z = zfile_fopen_empty ("", 512 * 1760);
	createimagefromexe (drv->diskfile, z);
	drv->filetype = ADF_NORMAL;
	zfile_fclose (drv->diskfile);
	drv->diskfile = z;
	drv->num_tracks = 160;
	drv->num_secs = 11;
	for (i = 0; i < drv->num_tracks; i++) {
	    tid = &drv->trackdata[i];
	    tid->type = TRACK_AMIGADOS;
	    tid->len = 512 * drv->num_secs;
	    tid->bitlen = 0;
	    tid->offs = i * 512 * drv->num_secs;
	}
	drv->useturbo = 1;

    } else if (size == 720 * 1024 || size == 1440 * 1024) {
	/* PC formatted image */
	unsigned int i;

	drv->filetype = ADF_PCDOS;
	drv->num_tracks = 160;
	drv->ddhd = size == 1440 * 1024 ? 2 : 1;
	drv->num_secs = drv->ddhd == 2 ? 18 : 9;
	for (i = 0; i < drv->num_tracks; i++) {
	    tid = &drv->trackdata[i];
	    tid->type = TRACK_PCDOS;
	    tid->len = 512 * drv->num_secs;
	    tid->bitlen = 0;
	    tid->offs = i * 512 * drv->num_secs;
	}

    } else {
	unsigned int i;

	drv->filetype = ADF_NORMAL;

	/* High-density disk? */
	if (size >= 160 * 22 * 512) {
	    drv->num_tracks = size / (512 * (drv->num_secs = 22));
	    drv->ddhd = 2;
	} else
	    drv->num_tracks = size / (512 * (drv->num_secs = 11));

	if (drv->num_tracks > MAX_TRACKS)
	    write_log ("Your diskfile is too big!\n");
	for (i = 0; i < drv->num_tracks; i++) {
	    tid = &drv->trackdata[i];
	    tid->type = TRACK_AMIGADOS;
	    tid->len = 512 * drv->num_secs;
	    tid->bitlen = 0;
	    tid->offs = i * 512 * drv->num_secs;
	}
    }

    openwritefile (drv, 0);

    drive_settype_id (drv); /* Set DD or HD drive */
    drive_fill_bigbuf (drv, 1);
    drv->mfmpos = (rand () | (rand () << 16)) % drv->tracklen;
    drv->prevtracklen = 0;
#ifdef DRIVESOUND
    driveclick_insert (drv - floppy, 0);
#endif
    return 1;
}

/****i* januae-disk/rand_shifter
*
*   NAME
*       rand_shifter() -- Advance the disk serial shifter with random bits.
*
*   SYNOPSIS
*       static void rand_shifter (void);
*
*   FUNCTION
*       Shift one to eight pseudo-random bits into the shared data word and wrap
*       the bit offset modulo sixteen.
*
***************************************************************************
*
*/

static void rand_shifter (void)
{
    unsigned int r = ((rand () >> 4) & 7) + 1;
    while (r-- > 0) {
	word <<= 1;
	word |= (rand () & 0x1000) ? 1 : 0;
	bitoffset++;
	bitoffset &= 15;
    }
}

/****i* januae-disk/drive_empty
*
*   NAME
*       drive_empty() -- Report whether a drive lacks an inserted disk.
*
*   SYNOPSIS
*       STATIC_INLINE int drive_empty (const drive * drv);
*
*   FUNCTION
*       Consult a Catweasel device's change state when compiled and present;
*       otherwise test the image-stream pointer.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
*   RESULT
*       Nonzero if empty according to the selected backend.
*
***************************************************************************
*
*/

STATIC_INLINE int drive_empty (const drive * drv)
{
#ifdef CATWEASEL
    if (drv->catweasel)
	return catweasel_disk_changed (drv->catweasel) == 0;
#endif
    return drv->diskfile == 0;
}

/****i* januae-disk/drive_step
*
*   NAME
*       drive_step() -- Advance the selected head one cylinder.
*
*   SYNOPSIS
*       static void drive_step (drive * drv);
*
*   FUNCTION
*       Apply the current direction and step limit, update the track coordinate
*       and optionally drive-click or Catweasel behavior.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
***************************************************************************
*
*/

static void drive_step (drive * drv)
{
#if 0
    if (drv->dskready) {
	drv->dskready = 0;
	drv->dskready_time = DSKREADY_TIME;
    }
#endif
#ifdef CATWEASEL
    if (drv->catweasel) {
	int dir = direction ? -1 : 1;
	catweasel_step (drv->catweasel, dir);
	drv->cyl += dir;
	if (drv->cyl < 0)
	    drv->cyl = 0;
	write_log ("%d -> %d\n", dir, drv->cyl);
	return;
    }
#endif
    if (drv->steplimit && get_cycles () - drv->steplimitcycle < MIN_STEPLIMIT_CYCLE) {
	if (disk_debug_logging > 1)
	    write_log (" step ignored %d", (int)((get_cycles () - drv->steplimitcycle) / CYCLE_UNIT));
	return;
    }
    /* A1200's floppy drive needs at least 30 raster lines between steps
     * but we'll use very small value for better compatibility with faster CPU emulation
     * (stupid trackloaders with CPU delay loops)
     */
    drv->steplimit = 10;
    drv->steplimitcycle = get_cycles ();
    if (!drive_empty (drv))
	drv->dskchange = 0;
    if (direction) {
	if (drv->cyl) {
	    drv->cyl--;
#ifdef DRIVESOUND
	    driveclick_click (drv - floppy, drv->cyl);
#endif
	}
/*	else
	    write_log ("program tried to step beyond track zero\n");
	    "no-click" programs does that
*/
  } else {
	unsigned int maxtrack = drv->hard_num_cyls;
	if (drv->cyl < maxtrack + 3) {
	    drv->cyl++;
#ifdef CATWEASEL
	    if (drv->catweasel)
		catweasel_step (drv->catweasel, 1);
#endif
	}
	if (drv->cyl >= maxtrack)
	    write_log ("program tried to step over track %d\n", maxtrack);
#ifdef DRIVESOUND
	driveclick_click (drv - floppy, drv->cyl);
#endif
    }
    rand_shifter ();
    if (disk_debug_logging > 1)
	write_log (" ->step %d", drv->cyl);
}

/****i* januae-disk/drive_track0
*
*   NAME
*       drive_track0() -- Report the track-zero sensor.
*
*   SYNOPSIS
*       static int drive_track0 (drive * drv);
*
*   FUNCTION
*       Read Catweasel track-zero status when available, otherwise compare the
*       emulated cylinder with zero.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
*   RESULT
*       Nonzero when the mechanism reports track zero.
*
***************************************************************************
*
*/

static int drive_track0 (drive * drv)
{
#ifdef CATWEASEL
    if (drv->catweasel)
	return catweasel_track0 (drv->catweasel);
#endif
    return drv->cyl == 0;
}

/****i* januae-disk/drive_writeprotected
*
*   NAME
*       drive_writeprotected() -- Report whether the drive rejects writes.
*
*   SYNOPSIS
*       static int drive_writeprotected (drive * drv);
*
*   FUNCTION
*       Treat Catweasel media as protected and otherwise test the image
*       write-protect bit or absence of an image.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
*   RESULT
*       Nonzero for a write-protected or empty drive.
*
***************************************************************************
*
*/

static int drive_writeprotected (drive * drv)
{
#ifdef CATWEASEL
    if (drv->catweasel)
	return 1;
#endif
    return drv->wrprot || drv->diskfile == NULL;
}

/****i* januae-disk/drive_running
*
*   NAME
*       drive_running() -- Report drive motor state.
*
*   SYNOPSIS
*       static int drive_running (drive * drv);
*
*   FUNCTION
*       Return the inverse of the emulated motor-off flag.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
*   RESULT
*       Nonzero when the motor is enabled.
*
***************************************************************************
*
*/

static int drive_running (drive * drv)
{
    return !drv->motoroff;
}

/****i* januae-disk/drive_motor
*
*   NAME
*       drive_motor() -- Change motor and readiness timing.
*
*   SYNOPSIS
*       static void drive_motor (drive * drv, int off);
*
*   FUNCTION
*       Apply a motor transition, reset drive-ID timing on stop and optionally
*       notify drive-sound support. The caller supplies the motor-off state.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*       off - Nonzero to switch the motor off.
*
***************************************************************************
*
*/

static void drive_motor (drive * drv, int off)
{
    if (drv->motoroff && !off) {
	drv->dskready_time = DSKREADY_TIME;
	rand_shifter ();
#ifdef DRIVESOUND
	driveclick_motor (drv - floppy, drv->dskready_down_time == 0 ? 2 : 1);
#endif
	if (disk_debug_logging > 1)
	    write_log (" ->motor on");
    }
    if (!drv->motoroff && off) {
	drv->drive_id_scnt = 0; /* Reset id shift reg counter */
	drv->dskready_down_time = DSKREADY_DOWN_TIME;
#ifdef DRIVESOUND
	driveclick_motor (drv - floppy, 0);
#endif
#ifdef DEBUG_DRIVE_ID
	write_log ("drive_motor: Selected DF%d: reset id shift reg.\n",drv-floppy);
#endif
	if (disk_debug_logging > 1)
	    write_log (" ->motor off");
	if (currprefs.cpu_level <= 1) {
	    drv->motordelay = 1;
	    diskevent_flag = DISK_MOTORDELAY;
	    eventtab[ev_disk].oldcycles = get_cycles ();
	    eventtab[ev_disk].evtime = get_cycles () + 30 * CYCLE_UNIT;
	    eventtab[ev_disk].active = 1;
	    events_schedule ();
	}
    }
    drv->motoroff = off;
    if (drv->motoroff) {
	drv->dskready = 0;
	drv->dskready_time = 0;
    }
#ifdef CATWEASEL
    if (drv->catweasel)
	catweasel_set_motor (drv->catweasel, !drv->motoroff);
#endif
}

/****i* januae-disk/read_floppy_data
*
*   NAME
*       read_floppy_data() -- Read bytes from a track's image region.
*
*   SYNOPSIS
*       static void read_floppy_data (struct zfile *diskfile, trackid *tid, int
*       offset, void *dst, int len);
*
*   FUNCTION
*       Seek to the track's base offset plus the local offset and copy the
*       requested byte count. A zero count is ignored; I/O errors are not
*       returned.
*
*   INPUTS
*       diskfile - Disk image stream.
*       tid - Track description containing its image offset.
*       offset - Byte offset within the target image or sector.
*       dst - Destination image or output storage.
*       len - Byte or bit count, or optional serialized-length output, according
*       to the signature.
*
***************************************************************************
*
*/

static void read_floppy_data (struct zfile *diskfile, trackid *tid, int offset, void *dst, int len)
{
    if (len == 0)
	return;
    zfile_fseek (diskfile, tid->offs + offset, SEEK_SET);
    zfile_fread (dst, 1, len, diskfile);
}

/* Megalomania does not like zero MFM words... */
/****i* januae-disk/mfmcode
*
*   NAME
*       mfmcode() -- Insert MFM clock bits into data words.
*
*   SYNOPSIS
*       void mfmcode (uae_u16 *mfm, unsigned int words);
*
*   FUNCTION
*       Walk the provided word array, adding clock bits between data bits while
*       carrying the prior word across positions.
*
*   INPUTS
*       mfm - MFM word buffer to update in place.
*       words - Number of MFM data words to encode.
*
***************************************************************************
*
*/

void mfmcode (uae_u16 *mfm, unsigned int words)
{
    uae_u32 lastword = 0;
    while (words--) {
	uae_u32 v = *mfm;
	uae_u32 lv = (lastword << 16) | v;
	uae_u32 nlv = 0x55555555 & ~lv;
	uae_u32 mfmbits = (nlv << 1) & (nlv >> 1);
	*mfm++ = v | mfmbits;
	lastword = v;
    }
}

static uae_u8 mfmencodetable[16] = {
    0x2a, 0x29, 0x24, 0x25, 0x12, 0x11, 0x14, 0x15,
    0x4a, 0x49, 0x44, 0x45, 0x52, 0x51, 0x54, 0x55
};


/****i* januae-disk/dos_encode_byte
*
*   NAME
*       dos_encode_byte() -- Encode one PC DOS byte as an MFM word.
*
*   SYNOPSIS
*       static uae_u16 dos_encode_byte (uae_u8 byte);
*
*   FUNCTION
*       Use the two four-bit table entries and apply the initial MFM clock bit
*       convention.
*
*   INPUTS
*       byte - Unencoded data byte.
*
*   RESULT
*       Encoded 16-bit MFM word.
*
***************************************************************************
*
*/

static uae_u16 dos_encode_byte (uae_u8 byte)
{
    uae_u8 b2, b1;
    uae_u16 word;

    b1 = byte;
    b2 = b1 >> 4;
    b1 &= 15;
    word = mfmencodetable[b2] <<8 | mfmencodetable[b1];
    return (word | ((word & (256 | 64)) ? 0 : 128));
}

/****i* januae-disk/mfmcoder
*
*   NAME
*       mfmcoder() -- Encode a sequence of PC DOS bytes into MFM words.
*
*   SYNOPSIS
*       static uae_u16 *mfmcoder (uae_u8 *src, uae_u16 *dest, int len);
*
*   FUNCTION
*       Append encoded words and adjust each clock bit from the immediately
*       preceding destination word. The destination must have an accessible
*       preceding word.
*
*   INPUTS
*       src - Input stream, input bytes or SaveState buffer, according to the
*       signature.
*       dest - Output MFM buffer or emulated memory address, according to the
*       signature.
*       len - Byte or bit count, or optional serialized-length output, according
*       to the signature.
*
*   RESULT
*       Pointer immediately after the last encoded word.
*
***************************************************************************
*
*/

static uae_u16 *mfmcoder (uae_u8 *src, uae_u16 *dest, int len)
{
    int i;

    for (i = 0; i < len; i++) {
	*dest = dos_encode_byte (*src++);
	*dest |= ((dest[-1] & 1)||(*dest & 0x4000)) ? 0: 0x8000;
	dest++;
    }
    return dest;
}

/****i* januae-disk/decode_pcdos
*
*   NAME
*       decode_pcdos() -- Construct the selected track's PC DOS MFM stream.
*
*   SYNOPSIS
*       static void decode_pcdos (drive *drv);
*
*   FUNCTION
*       Encode the track sectors with PC-style address marks, gaps and CRC
*       values into the drive's MFM buffer.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
***************************************************************************
*
*/

static void decode_pcdos (drive *drv)
{
    unsigned int i;
    unsigned int tr = drv->cyl * 2 + side;
    uae_u16 *dstmfmbuf, *mfm2;
    uae_u8 secbuf[700];
    uae_u16 crc16;
    trackid *ti = drv->trackdata + tr;

    mfm2 = drv->bigmfmbuf;
    *mfm2++ = 0x9254;
    memset (secbuf, 0x4e, 80); // 94
    memset (secbuf + 80, 0x00, 12); // 12
    secbuf[92] = 0xc2;
    secbuf[93] = 0xc2;
    secbuf[94] = 0xc2;
    secbuf[95] = 0xfc;
    memset (secbuf + 96, 0x4e, 50); // 50
    dstmfmbuf = mfmcoder (secbuf, mfm2, 146);
    mfm2[92] = 0x5224;
    mfm2[93] = 0x5224;
    mfm2[94] = 0x5224;
    for (i = 0; i < drv->num_secs; i++) {
	mfm2 = dstmfmbuf;
	memset (secbuf, 0x00, 12);
	secbuf[12] = 0xa1;
	secbuf[13] = 0xa1;
	secbuf[14] = 0xa1;
	secbuf[15] = 0xfe;
	secbuf[16] = drv->cyl;
	secbuf[17] = side;
	secbuf[18] = 1 + i;
	secbuf[19] = 2; // 128 << 2 = 512
	crc16 = get_crc16 (secbuf + 12, 3 + 1 + 4);
	secbuf[20] = crc16 >> 8;
	secbuf[21] = crc16 & 0xff;
	memset (secbuf + 22, 0x4e, 22);
	memset (secbuf + 44, 0x00, 12);
	secbuf[56] = 0xa1;
	secbuf[57] = 0xa1;
	secbuf[58] = 0xa1;
	secbuf[59] = 0xfb;
	read_floppy_data (drv->diskfile, ti, i * 512, &secbuf[60], 512);
	crc16 = get_crc16(secbuf + 56, 3 + 1 + 512);
	secbuf[60 + 512] = crc16 >> 8;
	secbuf[61 + 512] = crc16 & 0xff;
	memset (secbuf + 512 + 62, 0x4e, 76 / drv->ddhd);
	dstmfmbuf = mfmcoder (secbuf, mfm2, 60 + 512 + 2 + 76 / drv->ddhd);
	mfm2[12] = 0x4489;
	mfm2[13] = 0x4489;
	mfm2[14] = 0x4489;
	mfm2[56] = 0x4489;
	mfm2[57] = 0x4489;
	mfm2[58] = 0x4489;
    }
    for (i = 0; i < 200; i++)
	*dstmfmbuf++ = 0x9254;
    drv->skipoffset = 0;
    drv->tracklen = (dstmfmbuf - drv->bigmfmbuf) * 16;
    if (disk_debug_logging > 0)
	write_log ("pcdos read track %d\n", tr);
}

/****i* januae-disk/decode_amigados
*
*   NAME
*       decode_amigados() -- Construct the selected track's AmigaDOS MFM stream.
*
*   SYNOPSIS
*       static void decode_amigados (drive *drv);
*
*   FUNCTION
*       Encode AmigaDOS sector headers, odd/even data words, checksums and track
*       gaps from the mounted image.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
***************************************************************************
*
*/

static void decode_amigados (drive *drv)
{
    /* Normal AmigaDOS format track */
    unsigned int tr = drv->cyl * 2 + side;
    unsigned int sec;
    int dstmfmoffset = 0;
    uae_u16 *dstmfmbuf = drv->bigmfmbuf;
    int len = drv->num_secs * 544 + FLOPPY_GAP_LEN;

    trackid *ti = drv->trackdata + tr;
    memset (dstmfmbuf, 0xaa, len * 2);
    dstmfmoffset += FLOPPY_GAP_LEN;
    drv->skipoffset = (FLOPPY_GAP_LEN * 8) / 3 * 2;
    drv->tracklen = len * 2 * 8;

    for (sec = 0; sec < drv->num_secs; sec++) {
	uae_u8 secbuf[544];
	uae_u16 mfmbuf[544];
	int i;
	uae_u32 deven, dodd;
	uae_u32 hck = 0, dck = 0;

	secbuf[0] = secbuf[1] = 0x00;
	secbuf[2] = secbuf[3] = 0xa1;
	secbuf[4] = 0xff;
	secbuf[5] = tr;
	secbuf[6] = sec;
	secbuf[7] = drv->num_secs - sec;

	for (i = 8; i < 24; i++)
	    secbuf[i] = 0;

	read_floppy_data (drv->diskfile, ti, sec * 512, &secbuf[32], 512);

	mfmbuf[0] = mfmbuf[1] = 0xaaaa;
	mfmbuf[2] = mfmbuf[3] = 0x4489;

	deven = ((secbuf[4] << 24) | (secbuf[5] << 16)
		     | (secbuf[6] << 8) | (secbuf[7]));
	dodd = deven >> 1;
	deven &= 0x55555555;
	dodd &= 0x55555555;

	mfmbuf[4] = dodd >> 16;
	mfmbuf[5] = dodd;
	mfmbuf[6] = deven >> 16;
	mfmbuf[7] = deven;

	for (i = 8; i < 48; i++)
	    mfmbuf[i] = 0xaaaa;
	for (i = 0; i < 512; i += 4) {
	    deven = ((secbuf[i + 32] << 24) | (secbuf[i + 33] << 16)
			 | (secbuf[i + 34] << 8) | (secbuf[i + 35]));
	    dodd = deven >> 1;
	    deven &= 0x55555555;
	    dodd &= 0x55555555;
	    mfmbuf[(i >> 1) + 32] = dodd >> 16;
	    mfmbuf[(i >> 1) + 33] = dodd;
	    mfmbuf[(i >> 1) + 256 + 32] = deven >> 16;
	    mfmbuf[(i >> 1) + 256 + 33] = deven;
	}

	for (i = 4; i < 24; i += 2)
	    hck ^= (mfmbuf[i] << 16) | mfmbuf[i + 1];

	deven = dodd = hck;
	dodd >>= 1;
	mfmbuf[24] = dodd >> 16;
	mfmbuf[25] = dodd;
	mfmbuf[26] = deven >> 16;
	mfmbuf[27] = deven;

	for (i = 32; i < 544; i += 2)
	    dck ^= (mfmbuf[i] << 16) | mfmbuf[i + 1];

	deven = dodd = dck;
	dodd >>= 1;
	mfmbuf[28] = dodd >> 16;
	mfmbuf[29] = dodd;
	mfmbuf[30] = deven >> 16;
	mfmbuf[31] = deven;
	mfmcode (mfmbuf + 4, 544 - 4);

	for (i = 0; i < 544; i++) {
	    dstmfmbuf[dstmfmoffset % len] = mfmbuf[i];
	    dstmfmoffset++;
	}
     }

    if (disk_debug_logging > 0)
	write_log ("amigados read track %d\n", tr);
}

/****i* januae-disk/drive_fill_bigbuf
*
*   NAME
*       drive_fill_bigbuf() -- Load or construct the current MFM track.
*
*   SYNOPSIS
*       static void drive_fill_bigbuf (drive * drv, int force);
*
*   FUNCTION
*       Reuse an already buffered cylinder and side unless forced, otherwise
*       load the selected track from the image or format-specific backend.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*       force - Nonzero to bypass the cached track.
*
***************************************************************************
*
*/

static void drive_fill_bigbuf (drive * drv, int force)
{
    unsigned int tr = drv->cyl * 2 + side;
    trackid *ti = drv->trackdata + tr;

    if ((!drv->diskfile && !drv->catweasel) || tr >= drv->num_tracks) {
	track_reset (drv);
	return;
    }

    if (!force && drv->catweasel) {
	drv->buffered_cyl = -1;
	return;
    }

    if (!force && drv->buffered_cyl == (int)drv->cyl && drv->buffered_side == side)
	return;
    drv->indexoffset = 0;
    drv->multi_revolution = 0;
    drv->tracktiming[0] = 0;
    drv->skipoffset = -1;

    if (drv->writediskfile && drv->writetrackdata[tr].bitlen > 0) {
	unsigned int i;
	trackid *wti = &drv->writetrackdata[tr];
	drv->tracklen = wti->bitlen;
	read_floppy_data (drv->writediskfile, wti, 0, drv->bigmfmbuf, (wti->bitlen + 7) / 8);
	for (i = 0; i < (drv->tracklen + 15) / 16; i++) {
	    uae_u16 *mfm = drv->bigmfmbuf + i;
	    uae_u8 *data = (uae_u8 *) mfm;
	    *mfm = 256 * *data + *(data + 1);
	}
	if (disk_debug_logging > 0)
	    write_log ("track %d, length %d read from \"saveimage\"\n", tr, drv->tracklen);
    } else if (drv->filetype == ADF_CATWEASEL) {
#ifdef CATWEASEL
	drv->tracklen = 0;
	if (!catweasel_disk_changed (drv->catweasel)) {
	    drv->tracklen = catweasel_fillmfm (drv->catweasel, drv->bigmfmbuf, side, drv->ddhd, 0);
	}
	drv->buffered_cyl = -1;
	if (!drv->tracklen) {
	    track_reset (drv);
	    return;
	}
#endif
    } else if (drv->filetype == ADF_IPF) {

#ifdef CAPS
	caps_loadtrack (drv->bigmfmbuf, drv->tracktiming, drv - floppy, tr, &drv->tracklen, &drv->multi_revolution, &drv->skipoffset);
#endif

    } else if (drv->filetype == ADF_FDI) {

#ifdef FDI2RAW
	fdi2raw_loadtrack (drv->fdi, drv->bigmfmbuf, drv->tracktiming, tr, &drv->tracklen, &drv->indexoffset, &drv->multi_revolution, 1);
#endif

    } else if (ti->type == TRACK_PCDOS) {

	decode_pcdos (drv);

    } else if (ti->type == TRACK_AMIGADOS) {

	decode_amigados (drv);

    } else {
	unsigned int i;
	int base_offset = ti->type == TRACK_RAW ? 0 : 1;
	drv->tracklen = ti->bitlen + 16 * base_offset;
	drv->bigmfmbuf[0] = ti->sync;
	read_floppy_data (drv->diskfile, ti, 0, drv->bigmfmbuf + base_offset, (ti->bitlen + 7) / 8);
	for (i = base_offset; i < (drv->tracklen + 15) / 16; i++) {
	    uae_u16 *mfm = drv->bigmfmbuf + i;
	    uae_u8 *data = (uae_u8 *) mfm;
	    *mfm = 256 * *data + *(data + 1);
	}
	if (disk_debug_logging > 1)
	    write_log ("rawtrack %d image offset=%x\n", tr, ti->offs);
    }
    drv->buffered_side = side;
    drv->buffered_cyl = drv->cyl;
    if (drv->tracklen == 0) {
	drv->tracklen = FLOPPY_WRITE_LEN * drv->ddhd * 2 * 8;
	memset (drv->bigmfmbuf, 0, FLOPPY_WRITE_LEN * 2 * drv->ddhd);
    }
    drv->trackspeed = get_floppy_speed () * drv->tracklen / (2 * 8 * FLOPPY_WRITE_LEN * drv->ddhd);
    updatemfmpos (drv);
}

/* Update ADF_EXT2 track header */
/****i* januae-disk/diskfile_update
*
*   NAME
*       diskfile_update() -- Update an extended-ADF track descriptor.
*
*   SYNOPSIS
*       static void diskfile_update (struct zfile *diskfile, trackid *ti, int
*       len, uae_u8 type);
*
*   FUNCTION
*       Write the recorded track type, length and bit count into the image
*       header and clear unused reserved track space. Allocation and I/O
*       failures are not reported.
*
*   INPUTS
*       diskfile - Disk image stream.
*       ti - Track directory entry to update.
*       len - Byte or bit count, or optional serialized-length output, according
*       to the signature.
*       type - Requested image or track format identifier.
*
***************************************************************************
*
*/

static void diskfile_update (struct zfile *diskfile, trackid *ti, int len, uae_u8 type)
{
    uae_u8 buf[2 + 2 + 4 + 4], *zerobuf;

    ti->bitlen = len;
    zfile_fseek (diskfile, 8 + 4 + (2 + 2 + 4 + 4) * ti->track, SEEK_SET);
    memset (buf, 0, sizeof buf);
    ti->type = type;
    buf[3] = ti->type;
    do_put_mem_long ((uae_u32 *) (buf + 4), ti->len);
    do_put_mem_long ((uae_u32 *) (buf + 8), ti->bitlen);
    zfile_fwrite (buf, sizeof (buf), 1, diskfile);
    if (ti->len > (len + 7) / 8) {
	zerobuf = malloc (ti->len);
	memset (zerobuf, 0, ti->len);
	zfile_fseek (diskfile, ti->offs, SEEK_SET);
	zfile_fwrite (zerobuf, 1, ti->len, diskfile);
	free (zerobuf);
    }
    if (disk_debug_logging > 0)
	write_log ("track %d, raw track length %d written (total size %d)\n", ti->track, (ti->bitlen + 7) / 8, ti->len);
}

#define MFMMASK 0x55555555
/****i* januae-disk/getmfmword
*
*   NAME
*       getmfmword() -- Read a shifted 16-bit MFM word.
*
*   SYNOPSIS
*       static uae_u16 getmfmword (const uae_u16 *mbuf, unsigned int shift);
*
*   FUNCTION
*       Join adjacent raw MFM words using the specified bit shift.
*
*   INPUTS
*       mbuf - MFM word buffer.
*       shift - Bit shift within MFM words.
*
*   RESULT
*       Shifted 16-bit word.
*
***************************************************************************
*
*/

static uae_u16 getmfmword (const uae_u16 *mbuf, unsigned int shift)
{
    return (mbuf[0] << shift) | (mbuf[1] >> (16 - shift));
}

//#if !defined(CATWEASEL)
/****i* januae-disk/getmfmlong
*
*   NAME
*       getmfmlong() -- Read a shifted 32-bit MFM data field.
*
*   SYNOPSIS
*       static uae_u32 getmfmlong (const uae_u16 *mbuf, unsigned int shift);
*
*   FUNCTION
*       Combine two shifted MFM words and mask clock positions from the result.
*
*   INPUTS
*       mbuf - MFM word buffer.
*       shift - Bit shift within MFM words.
*
*   RESULT
*       32-bit MFM data field without clock bits.
*
***************************************************************************
*
*/

static uae_u32 getmfmlong (const uae_u16 *mbuf, unsigned int shift)
{
    return ((getmfmword (mbuf, shift) << 16) | getmfmword (mbuf + 1, shift)) & MFMMASK;
}
//#endif
/****i* januae-disk/decode_buffer
*
*   NAME
*       decode_buffer() -- Decode AmigaDOS sectors from a raw MFM track.
*
*   SYNOPSIS
*       static int decode_buffer (uae_u16 *mbuf, unsigned int cyl, unsigned int
*       drvsec, unsigned int ddhd, int filetype, unsigned int *drvsecp, int
*       checkmode);
*
*   FUNCTION
*       Find sync words, validate sector IDs and checksums and copy accepted
*       sectors into the shared write buffer; checkmode changes validation
*       behavior.
*
*   INPUTS
*       mbuf - MFM word buffer.
*       cyl - Selected cylinder number.
*       drvsec - Sectors per track.
*       ddhd - Disk density multiplier or its optional output.
*       filetype - Disk image format identifier.
*       drvsecp - Optional decoded-sector-count output.
*       checkmode - Nonzero to validate sectors without the ordinary write path.
*
*   RESULT
*       Nonzero when the sector stream is rejected; zero for a decodable track.
*
***************************************************************************
*
*/

static int decode_buffer (uae_u16 *mbuf, unsigned int cyl, unsigned int drvsec, unsigned int ddhd, int filetype, unsigned int *drvsecp, int checkmode)
{
    unsigned int i;
    int secwritten = 0;
    unsigned int fwlen = FLOPPY_WRITE_LEN * ddhd;
    unsigned int length = 2 * fwlen;
    uae_u32 odd, even, chksum, id, dlong;
    uae_u8 *secdata;
    uae_u8 secbuf[544];
    uae_u16 *mend = mbuf + length;
    char sectable[22];
    unsigned int shift = 0;

    memset (sectable, 0, sizeof (sectable));
    memcpy (mbuf + fwlen, mbuf, fwlen * sizeof (uae_u16));
    mend -= (4 + 16 + 8 + 512);
    while (secwritten < (int)drvsec) {
	unsigned int trackoffs;

	while (getmfmword (mbuf, shift) != 0x4489) {
	    if (mbuf >= mend)
		return 1;
	    shift++;
	    if (shift == 16) {
		shift = 0;
		mbuf++;
	    }
	}
	while (getmfmword (mbuf, shift) == 0x4489) {
	    if (mbuf >= mend)
		return 1;
	    mbuf++;
	}

	odd = getmfmlong (mbuf, shift);
	even = getmfmlong (mbuf + 2, shift);
	mbuf += 4;
	id = (odd << 1) | even;

	trackoffs = (id & 0xff00) >> 8;
	if (trackoffs + 1 > drvsec) {
	    write_log ("Disk decode: weird sector number %d\n", trackoffs);
	    if (filetype == ADF_EXT2)
		return 2;
	    continue;
	}
	chksum = odd ^ even;
	for (i = 0; i < 4; i++) {
	    odd = getmfmlong (mbuf, shift);
	    even = getmfmlong (mbuf + 8, shift);
	    mbuf += 2;

	    dlong = (odd << 1) | even;
	    if (dlong && !checkmode) {
		if (filetype == ADF_EXT2)
		    return 6;
		secwritten = -200;
	    }
	    chksum ^= odd ^ even;
	}   /* could check here if the label is nonstandard */
	mbuf += 8;
	odd = getmfmlong (mbuf, shift);
	even = getmfmlong (mbuf + 2, shift);
	mbuf += 4;
	if (((odd << 1) | even) != chksum || ((id & 0x00ff0000) >> 16) != (unsigned int)(cyl * 2 + side)) {
	    write_log ("Disk decode: checksum error on sector %d header\n", trackoffs);
	    if (filetype == ADF_EXT2)
		return 3;
	    continue;
	}
	odd = getmfmlong (mbuf, shift);
	even = getmfmlong (mbuf + 2, shift);
	mbuf += 4;
	chksum = (odd << 1) | even;
	secdata = secbuf + 32;
	for (i = 0; i < 128; i++) {
	    odd = getmfmlong (mbuf, shift);
	    even = getmfmlong (mbuf + 256, shift);
	    mbuf += 2;
	    dlong = (odd << 1) | even;
	    *secdata++ = dlong >> 24;
	    *secdata++ = dlong >> 16;
	    *secdata++ = dlong >> 8;
	    *secdata++ = dlong;
	    chksum ^= odd ^ even;
	}
	if (chksum) {
	    write_log ("Disk decode: sector %d, data checksum error\n", trackoffs);
	    if (filetype == ADF_EXT2)
		return 4;
	    continue;
	}
	mbuf += 256;
	sectable[trackoffs] = 1;
	secwritten++;
	memcpy (writebuffer + trackoffs * 512, secbuf + 32, 512);
    }
    if (filetype == ADF_EXT2 && (secwritten == 0 || secwritten < 0))
	return 5;
    if (secwritten == 0)
	write_log ("Disk decode: unsupported format\n");
    if (secwritten < 0)
	write_log ("Disk decode: sector labels ignored\n");
    *drvsecp = drvsec;
    return 0;
}

/****i* januae-disk/mfmdecode
*
*   NAME
*       mfmdecode() -- Decode one MFM byte and advance the cursor.
*
*   SYNOPSIS
*       static uae_u8 mfmdecode (uae_u16 **mfmp, int shift);
*
*   FUNCTION
*       Strip clock positions from one shifted MFM word, assemble eight data
*       bits and move the supplied word pointer forward.
*
*   INPUTS
*       mfmp - Address of MFM cursor, advanced in place.
*       shift - Bit shift within MFM words.
*
*   RESULT
*       Decoded eight-bit data value.
*
***************************************************************************
*
*/

static uae_u8 mfmdecode (uae_u16 **mfmp, int shift)
{
    uae_u16 mfm = getmfmword (*mfmp, shift);
    uae_u8 out = 0;
    unsigned int i;

    (*mfmp)++;
    mfm &= 0x5555;
    for (i = 0; i < 8; i++) {
	out >>= 1;
	if (mfm & 1)
	    out |= 0x80;
	mfm >>= 2;
    }
    return out;
}

/****i* januae-disk/drive_write_pcdos
*
*   NAME
*       drive_write_pcdos() -- Write valid PC DOS sectors from a modified MFM track.
*
*   SYNOPSIS
*       static int drive_write_pcdos (drive *drv);
*
*   FUNCTION
*       Locate address marks, verify sector CRC values and update corresponding
*       image sectors. Corrupt sectors are reported but skipped.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
*   RESULT
*       Zero after processing the track; an incomplete write is not necessarily
*       reported as failure.
*
***************************************************************************
*
*/

static int drive_write_pcdos (drive *drv)
{
    int i;
    unsigned int drvsec = drv->num_secs;
    unsigned int fwlen = FLOPPY_WRITE_LEN * drv->ddhd;
    int length = 2 * fwlen;
    uae_u16 *mbuf = drv->bigmfmbuf;
    uae_u16 *mend = mbuf + length;
    unsigned int secwritten = 0, shift = 0;
    int sector = -1;
    char sectable[18];
    uae_u8 secbuf[3 + 1 + 512];
    uae_u8 mark;
    uae_u16 crc;

    memset (sectable, 0, sizeof (sectable));
    memcpy (mbuf + fwlen, mbuf, fwlen * sizeof (uae_u16));
    mend -= 518;
    secbuf[0] = secbuf[1] = secbuf[2] = 0xa1;
    secbuf[3] = 0xfb;

    while (secwritten < drvsec) {
	while (getmfmword (mbuf, shift) != 0x4489) {
	    if (mbuf >= mend)
		return 1;
	    shift++;
	    if (shift == 16) {
		shift = 0;
		mbuf++;
	    }
	}
	while (getmfmword (mbuf, shift) == 0x4489) {
	    if (mbuf >= mend)
		return 1;
	    mbuf++;
	}
	mark = mfmdecode (&mbuf, shift);
	if (mark == 0xfe) {
	    uae_u8 tmp[8];
	    uae_u8 cyl, head, size;

	    cyl = mfmdecode (&mbuf, shift);
	    head = mfmdecode (&mbuf, shift);
	    sector = mfmdecode (&mbuf, shift);
	    size = mfmdecode (&mbuf, shift);
	    crc = (mfmdecode (&mbuf, shift) << 8) | mfmdecode (&mbuf, shift);

	    tmp[0] = 0xa1; tmp[1] = 0xa1; tmp[2] = 0xa1; tmp[3] = mark;
	    tmp[4] = cyl; tmp[5] = head; tmp[6] = sector; tmp[7] = size;
	    if (get_crc16 (tmp, 8) != crc || cyl != drv->cyl || head != side || size != 2 || sector < 1 || sector > (int)drv->num_secs) {
		write_log ("PCDOS: track %d, corrupted sector header\n", drv->cyl * 2 + side);
		return 1;
	    }
	    sector--;
	    continue;
	}
	if (mark != 0xfb) {
	    write_log ("PCDOS: track %d: unknown address mark %02.2X\n", drv->cyl * 2 + side, mark);
	    continue;
	}
	if (sector < 0)
	    continue;
	for (i = 0; i < 512; i++)
	    secbuf[i + 4] = mfmdecode (&mbuf, shift);
	crc = (mfmdecode (&mbuf, shift) << 8) | mfmdecode (&mbuf, shift);
	if (get_crc16(secbuf, 3 + 1 + 512) != crc) {
	    write_log ("PCDOS: track %d, sector %d data checksum error\n",
		drv->cyl * 2 + side, sector + 1);
	    continue;
	}
	sectable[sector] = 1;
	secwritten++;
	zfile_fseek (drv->diskfile, drv->trackdata[drv->cyl * 2 + side].offs + sector * 512, SEEK_SET);
	zfile_fwrite (secbuf + 4, sizeof (uae_u8), 512, drv->diskfile);
	write_log ("PCDOS: track %d sector %d written\n", drv->cyl * 2 + side, sector + 1);
	sector = -1;
    }
    if (secwritten != drv->num_secs)
	write_log ("PCDOS: track %d, %d corrupted sectors ignored\n",
	    drv->cyl * 2 + side, drv->num_secs - secwritten);
    return 0;
}

#if !defined(CATWEASEL)
/****i* januae-disk/drive_write_adf_amigados
*
*   NAME
*       drive_write_adf_amigados() -- Write decoded AmigaDOS track sectors to an ADF image.
*
*   SYNOPSIS
*       static int drive_write_adf_amigados (drive *drv);
*
*   FUNCTION
*       Decode the selected MFM track and copy valid 512-byte sectors into the
*       image, updating extended-track metadata when necessary. Not compiled
*       when CATWEASEL is defined.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
*   RESULT
*       Zero on the normal write path, or 2 when the track cannot be decoded.
*
***************************************************************************
*
*/

static int drive_write_adf_amigados (drive *drv)
{
    unsigned int drvsec, i;

    if (decode_buffer (drv->bigmfmbuf, drv->cyl, drv->num_secs, drv->ddhd, drv->filetype, &drvsec, 0))
	return 2;
    if (!drvsec)
	return 2;

    if (drv->filetype == ADF_EXT2)
	diskfile_update (drv->diskfile, &drv->trackdata[drv->cyl * 2 + side], drvsec * 512 * 8, TRACK_AMIGADOS);
    for (i = 0; i < drvsec; i++) {
	zfile_fseek (drv->diskfile, drv->trackdata[drv->cyl * 2 + side].offs + i * 512, SEEK_SET);
	zfile_fwrite (writebuffer + i * 512, sizeof (uae_u8), 512, drv->diskfile);
    }

    return 0;
}
#endif
/* write raw track to disk file */
/****i* januae-disk/drive_write_ext2
*
*   NAME
*       drive_write_ext2() -- Persist a raw MFM track in extended ADF form.
*
*   SYNOPSIS
*       static int drive_write_ext2 (uae_u16 *bigmfmbuf, struct zfile *diskfile,
*       trackid *ti, int tracklen);
*
*   FUNCTION
*       Update the track directory and write the raw MFM buffer, truncating the
*       physical payload to the image's reserved capacity when necessary.
*
*   INPUTS
*       bigmfmbuf - Raw MFM track words.
*       diskfile - Disk image stream.
*       ti - Track directory entry to update.
*       tracklen - Track bit count.
*
*   RESULT
*       Returns 1 after writing, without propagating low-level file errors.
*
***************************************************************************
*
*/

static int drive_write_ext2 (uae_u16 *bigmfmbuf, struct zfile *diskfile, trackid *ti, int tracklen)
{
    int len, i;

    len = (tracklen + 7) / 8;
    if (len > ti->len) {
	write_log ("disk raw write: image file's track %d is too small (%d < %d)!\n", ti->track, ti->len, len);
	len = ti->len;
    }
    diskfile_update (diskfile, ti, tracklen, TRACK_RAW);
    for (i = 0; i < ti->len / 2; i++) {
	uae_u16 *mfm = bigmfmbuf + i;
	uae_u16 *mfmw = bigmfmbufw + i;
	uae_u8 *data = (uae_u8 *) mfm;
	*mfmw = 256 * *data + *(data + 1);
    }
    zfile_fseek (diskfile, ti->offs, SEEK_SET);
    zfile_fwrite (bigmfmbufw, 1, len, diskfile);
    return 1;
}

/****i* januae-disk/drive_write_data
*
*   NAME
*       drive_write_data() -- Persist the selected drive's pending MFM modifications.
*
*   SYNOPSIS
*       static void drive_write_data (drive * drv);
*
*   FUNCTION
*       Respect write protection, record a writable overlay when present and
*       dispatch AmigaDOS, PC DOS or raw extended-track writing as supported.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
***************************************************************************
*
*/

static void drive_write_data (drive * drv)
{
    int ret = -1;
    static int warned;

    if (drive_writeprotected (drv)) {
	/* read original track back because we didn't really write anything */
	drv->buffered_side = 2;
	return;
    }
    if (drv->writediskfile) {
	drive_write_ext2 (drv->bigmfmbuf, drv->writediskfile, &drv->writetrackdata[drv->cyl * 2 + side],
	    longwritemode ? (unsigned int)dsklength2 * 8 : drv->tracklen);
    }
    switch (drv->filetype) {
    case ADF_NORMAL:
#if defined(CATWEASEL)
	if ( 0 ) {
#else
	if (drive_write_adf_amigados (drv)) {
#endif
	    if (!warned)
#if 0
		notify_user (NUMSG_NEEDEXT2);
#endif
	    warned = 1;
	}
	return;
    case ADF_EXT1:
	break;
    case ADF_EXT2:
#if !defined(CATWEASEL)
	if (!longwritemode)
	    ret = drive_write_adf_amigados (drv);
#endif
	if (ret) {
	    write_log ("not an amigados track %d (error %d), writing as raw track\n", drv->cyl * 2 + side, ret);
	    drive_write_ext2 (drv->bigmfmbuf, drv->diskfile, &drv->trackdata[drv->cyl * 2 + side],
		longwritemode ? (unsigned int)dsklength2 * 8 : drv->tracklen);
	}
	return;
    case ADF_IPF:
	break;
    case ADF_PCDOS:
	ret = drive_write_pcdos (drv);
	if (ret)
	    write_log ("not a PC formatted track %d (error %d)\n", drv->cyl * 2 + side, ret);
	break;
    case ADF_FDI:
    case ADF_CATWEASEL:
	break;
    }
    drv->tracktiming[0] = 0;
}

/****i* januae-disk/drive_eject
*
*   NAME
*       drive_eject() -- Detach a disk and reset insertion state.
*
*   SYNOPSIS
*       static void drive_eject (drive * drv);
*
*   FUNCTION
*       Release image resources, mark a media-change event, clear ready timing
*       and refresh the emulated drive identifier.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
***************************************************************************
*
*/

static void drive_eject (drive * drv)
{
#ifdef DRIVESOUND
    driveclick_insert (drv - floppy, 1);
#endif
    drive_image_free (drv);
    drv->dskchange = 1;
    drv->ddhd = 1;
    drv->dskchange_time = 0;
    drv->dskready = 0;
    drv->dskready_time = 0;
    drv->dskready_down_time = 0;
    drv->crc32 = 0;
    drive_settype_id (drv); /* Back to 35 DD */
    if (disk_debug_logging > 0)
	write_log ("eject drive %d\n", drv - &floppy[0]);
}

/* We use this function if we have no Kickstart ROM.
 * No error checking - we trust our luck. */
/****i* januae-disk/DISK_ersatz_read
*
*   NAME
*       DISK_ersatz_read() -- Read a boot sector into emulated memory.
*
*   SYNOPSIS
*       void DISK_ersatz_read (int tr, int sec, uaecptr dest);
*
*   FUNCTION
*       Copy 512 bytes from the given track and sector of drive zero directly
*       into the real-address mapping of the guest destination. A mounted
*       drive-zero image is assumed.
*
*   INPUTS
*       tr - Track index in the mounted image.
*       sec - Sector index in the selected track.
*       dest - Output MFM buffer or emulated memory address, according to the
*       signature.
*
***************************************************************************
*
*/

void DISK_ersatz_read (int tr, int sec, uaecptr dest)
{
    uae_u8 *dptr = get_real_address (dest);
    zfile_fseek (floppy[0].diskfile, floppy[0].trackdata[tr].offs + sec * 512, SEEK_SET);
    zfile_fread (dptr, 1, 512, floppy[0].diskfile);
}

/* type: 0=regular, 1=ext2adf */
/* adftype: 0=DD,1=HD,2=525SD */
/****i* januae-disk/disk_creatediskfile
*
*   NAME
*       disk_creatediskfile() -- Create a blank DD/HD or extended-ADF image.
*
*   SYNOPSIS
*       void disk_creatediskfile (char *name, int type, drive_type adftype,
*       const char *disk_name);
*
*   FUNCTION
*       Initialize an empty disk image with the requested geometry, format and
*       disk name, writing the filesystem blocks or extended-image header as
*       selected.
*
*   INPUTS
*       name - Image pathname or requested name.
*       type - Requested image or track format identifier.
*       adftype - Requested floppy mechanism geometry.
*       disk_name - Name written into the disk label.
*
***************************************************************************
*
*/

void disk_creatediskfile (char *name, int type, drive_type adftype, const char *disk_name)
{
    struct zfile *f;
    int i, l, file_size, tracks, track_len;
    uae_u8 *chunk = NULL;
    uae_u8 tmp[3*4];

    if (disk_name == NULL || strlen (disk_name) == 0)
	disk_name = "empty";

    if (type == 1)
	tracks = 2 * 83;
    else
	tracks = 2 * 80;
    file_size = 880 * 1024;
    track_len = FLOPPY_WRITE_LEN * 2;
    if (adftype == 1) {
	file_size *= 2;
	track_len *= 2;
    } else if (adftype == 2) {
	file_size /= 2;
	tracks /= 2;
    }

    f = zfile_fopen (name, "wb");
    chunk = xmalloc (16384);
    if (f && chunk) {
	memset (chunk,0,16384);
	if (type == 0 && adftype < 2) {
	    for (i = 0; i < file_size; i += 11264) {
		memset (chunk, 0, 11264);
		if (i == 0) {
		    /* boot block */
		    strcpy ((char *) chunk, "DOS");
		} else if (i == file_size / 2) {
		    int block = file_size / 1024;
		    /* root block */
		    chunk[0+3] = 2;
		    chunk[12+3] = 0x48;
		    chunk[312] = chunk[313] = chunk[314] = chunk[315] = (uae_u8)0xff;
		    chunk[316+2] = (block + 1) >> 8; chunk[316+3] = (block + 1) & 255;
		    chunk[432] = strlen (disk_name);
		    strcpy ((char *)chunk + 433, disk_name);
		    chunk[508 + 3] = 1;
		    disk_date (chunk + 420);
		    memcpy (chunk + 472, chunk + 420, 3 * 4);
		    memcpy (chunk + 484, chunk + 420, 3 * 4);
		    disk_checksum (chunk, chunk + 20);
		    /* bitmap block */
		    memset (chunk + 512 + 4, 0xff, 2 * file_size / (1024 * 8));
		    if (adftype == 0)
			chunk[512 + 0x72] = 0x3f;
		    else
			chunk[512 + 0xdc] = 0x3f;
		    disk_checksum (chunk + 512, chunk + 512);

		}
		zfile_fwrite (chunk, 11264, 1, f);
	    }
	} else {
	    l = track_len;
	    zfile_fwrite ("UAE-1ADF", 8, 1, f);
	    tmp[0] = 0; tmp[1] = 0; /* flags (reserved) */
	    tmp[2] = 0; tmp[3] = tracks; /* number of tracks */
	    zfile_fwrite (tmp, 4, 1, f);
	    tmp[0] = 0; tmp[1] = 0; /* flags (reserved) */
	    tmp[2] = 0; tmp[3] = 1; /* track type */
	    tmp[4] = 0; tmp[5] = 0; tmp[6]=(uae_u8)(l >> 8); tmp[7] = (uae_u8)l;
	    tmp[8] = 0; tmp[9] = 0; tmp[10] = 0; tmp[11] = 0;
	    for (i = 0; i < tracks; i++) zfile_fwrite (tmp, sizeof (tmp), 1, f);
	    for (i = 0; i < tracks; i++) zfile_fwrite (chunk, l, 1, f);
	}
    }
    free (chunk);
    zfile_fclose (f);
}

/****i* januae-disk/disk_getwriteprotect
*
*   NAME
*       disk_getwriteprotect() -- Query write protection for an image pathname.
*
*   SYNOPSIS
*       int disk_getwriteprotect (const char *name);
*
*   FUNCTION
*       Delegate format and access inspection to the disk-image write-protection
*       classifier.
*
*   INPUTS
*       name - Image pathname or requested name.
*
*   RESULT
*       Nonzero when the named image is considered write protected.
*
***************************************************************************
*
*/

int disk_getwriteprotect (const char *name)
{
    int needwritefile;
    drive_type drvtype;
    return diskfile_iswriteprotect (name, &needwritefile, &drvtype);
}

/****i* januae-disk/diskfile_readonly
*
*   NAME
*       diskfile_readonly() -- Attempt to update host file read-only status.
*
*   SYNOPSIS
*       static void diskfile_readonly (const char *name, int readonly);
*
*   FUNCTION
*       Read host mode bits but leave the intended mode transformation disabled;
*       consequently no chmod is performed in the current implementation.
*
*   INPUTS
*       name - Image pathname or requested name.
*       readonly - Requested file read-only state; currently has no effect.
*
***************************************************************************
*
*/

static void diskfile_readonly (const char *name, int readonly)
{
    struct stat st;
    int mode, oldmode;

    if (stat (name, &st))
	return;
    oldmode = mode = st.st_mode;
//    mode &= ~FILEFLAG_WRITE;
//    if (!readonly) mode |= FILEFLAG_WRITE;
    if (mode != oldmode)
	chmod (name, mode);
}

/****i* januae-disk/setdskchangetime
*
*   NAME
*       setdskchangetime() -- Schedule a deferred disk-change event.
*
*   SYNOPSIS
*       static void setdskchangetime (drive *drv, int dsktime);
*
*   FUNCTION
*       Postpone an insertion notification after nearby pending drive changes to
*       avoid simultaneous disk-change transitions.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*       dsktime - Requested disk-change delay.
*
***************************************************************************
*
*/

static void setdskchangetime (drive *drv, int dsktime)
{
    int i;
    /* prevent multiple disk insertions at the same time */
    if (drv->dskchange_time > 0)
	return;
    for (i = 0; i < MAX_FLOPPY_DRIVES; i++) {
	if (&floppy[i] != drv && floppy[i].dskchange_time > 0 && floppy[i].dskchange_time + 5 >= dsktime) {
	    dsktime = floppy[i].dskchange_time + 5;
	}
    }
    drv->dskchange_time = dsktime;
    if (disk_debug_logging > 0)
	write_log ("delayed insert enable %d\n", dsktime);
}

/****i* januae-disk/DISK_reinsert
*
*   NAME
*       DISK_reinsert() -- Schedule reinsertion of an existing drive.
*
*   SYNOPSIS
*       void DISK_reinsert (int num);
*
*   FUNCTION
*       Eject the drive's current image state and request a delayed insertion
*       event for the selected unit.
*
*   INPUTS
*       num - Zero-based floppy-unit index.
*
***************************************************************************
*
*/

void DISK_reinsert (int num)
{
    drive_eject (&floppy[num]);
    setdskchangetime (&floppy[num], 20);
}

/****i* januae-disk/disk_setwriteprotect
*
*   NAME
*       disk_setwriteprotect() -- Change write protection through image and overlay settings.
*
*   SYNOPSIS
*       int disk_setwriteprotect (int num, const char *name, int protect);
*
*   FUNCTION
*       Reopen the image and sidecar as appropriate, update the selected drive's
*       protection state and reject unavailable image paths.
*
*   INPUTS
*       num - Zero-based floppy-unit index.
*       name - Image pathname or requested name.
*       protect - Nonzero to request write protection.
*
*   RESULT
*       Nonzero on accepted changes; zero when the image cannot be opened.
*
***************************************************************************
*
*/

int disk_setwriteprotect (int num, const char *name, int protect)
{
    int needwritefile, oldprotect;
    struct zfile *zf1, *zf2;
    int wrprot1, wrprot2, i;
    char *name2;
    drive_type drvtype;

    oldprotect = diskfile_iswriteprotect (name, &needwritefile, &drvtype);
    zf1 = DISK_validate_filename (name, 1, &wrprot1, NULL);
    if (!zf1)
	return 0;
    if (zfile_iscompressed (zf1))
	wrprot1 = 1;
    zfile_fclose (zf1);
    zf2 = getwritefile (name, &wrprot2);
    name2 = DISK_get_saveimagepath (name);

    if (needwritefile && zf2 == 0)
	disk_creatediskfile (name2, 1, drvtype, NULL);
    zfile_fclose (zf2);
    if (protect && iswritefileempty (name)) {
	for (i = 0; i < MAX_FLOPPY_DRIVES; i++) {
	    if (!strcmp (name, floppy[i].newname))
		drive_eject (&floppy[i]);
	}
	unlink (name2);
    }

    if (!needwritefile)
	diskfile_readonly (name, protect);
    diskfile_readonly (name2, protect);
    DISK_reinsert (num);
    return 1;
}

/****i* januae-disk/disk_eject
*
*   NAME
*       disk_eject() -- Eject a unit and update configuration.
*
*   SYNOPSIS
*       void disk_eject (int num);
*
*   FUNCTION
*       Clear configured and pending image names, detach the medium and publish
*       the new state to the GUI.
*
*   INPUTS
*       num - Zero-based floppy-unit index.
*
***************************************************************************
*
*/

void disk_eject (int num)
{
    gui_filename (num, "");
    drive_eject (floppy + num);
    *currprefs.df[num] = *changed_prefs.df[num] = 0;
    floppy[num].newname[0] = 0;
    update_drive_gui (num);
}

/****i* januae-disk/DISK_history_add
*
*   NAME
*       DISK_history_add() -- Add, move or clear a recent disk-image entry.
*
*   SYNOPSIS
*       int DISK_history_add (const char *name, int idx);
*
*   FUNCTION
*       Validate a pathname before adding it, or clear the specified history
*       slot when the name pointer is NULL.
*
*   INPUTS
*       name - Image pathname or requested name.
*       idx - History entry index, or special insertion index.
*
*   RESULT
*       Nonzero for a stored or cleared entry, zero for rejected input.
*
***************************************************************************
*
*/

int DISK_history_add (const char *name, int idx)
{
    int i;

    if (idx >= MAX_PREVIOUS_FLOPPIES)
	return 0;
    if (name == NULL) {
	dfxhistory[idx][0] = 0;
	return 1;
    }
    if (name[0] == 0)
	return 0;
    if (!zfile_exists (name))
	return 0;
    if (idx >= 0) {
	if (idx >= MAX_PREVIOUS_FLOPPIES)
	    return 0;
	dfxhistory[idx][0] = 0;
	for (i = 0; i < MAX_PREVIOUS_FLOPPIES; i++) {
	    if (!strcmp (dfxhistory[i], name))
		return 0;
	}
	strcpy (dfxhistory[idx], name);
	return 1;
    }
    for (i = 0; i < MAX_PREVIOUS_FLOPPIES; i++) {
	if (!strcmp (dfxhistory[i], name)) {
	    while (i < MAX_PREVIOUS_FLOPPIES - 1) {
		strcpy (dfxhistory[i], dfxhistory[i + 1]);
		i++;
	    }
	    dfxhistory[MAX_PREVIOUS_FLOPPIES - 1][0] = 0;
	    break;
	}
    }
    for (i = MAX_PREVIOUS_FLOPPIES - 2; i >= 0; i--)
	strcpy (dfxhistory[i + 1], dfxhistory[i]);
    strcpy (dfxhistory[0], name);
    return 1;
}

/****i* januae-disk/DISK_history_get
*
*   NAME
*       DISK_history_get() -- Return an entry from the recent-image history.
*
*   SYNOPSIS
*       char *DISK_history_get (int idx);
*
*   FUNCTION
*       Index the shared history table. The implementation only checks the upper
*       bound; negative indices are not rejected.
*
*   INPUTS
*       idx - History entry index, or special insertion index.
*
*   RESULT
*       Pointer to a mutable history entry, or NULL for an index above the
*       limit.
*
***************************************************************************
*
*/

char *DISK_history_get (int idx)
{
    if (idx >= MAX_PREVIOUS_FLOPPIES)
	return 0;
    return dfxhistory[idx];
}

/****i* januae-disk/disk_insert_2
*
*   NAME
*       disk_insert_2() -- Request an ordinary or forced image insertion.
*
*   SYNOPSIS
*       static void disk_insert_2 (int num, const char *name, int forced);
*
*   FUNCTION
*       Apply an immediate forced insertion or record a pending image change
*       after updating preferences and recent-image history.
*
*   INPUTS
*       num - Zero-based floppy-unit index.
*       name - Image pathname or requested name.
*       forced - Nonzero to insert the image immediately.
*
***************************************************************************
*
*/

static void disk_insert_2 (int num, const char *name, int forced)
{
    drive *drv = floppy + num;

    if (forced) {
	drive_insert (drv, &currprefs, num, name);
	return;
    }
    if (!strcmp (currprefs.df[num], name))
	return;
    strcpy (drv->newname, name);
    strcpy (currprefs.df[num], name);
    DISK_history_add (name, -1);
    if (name[0] == 0) {
	disk_eject (num);
    } else if (!drive_empty (drv) || drv->dskchange_time > 0) {
	drive_eject (drv);
	/* set dskchange_time, disk_insert() will be
	 * called from DISK_check_change() after 2 second delay
	 * this makes sure that all programs detect disk change correctly
	 */
	setdskchangetime (drv, 20);
    } else {
	setdskchangetime (drv, 1);
    }
}

/****i* januae-disk/disk_insert
*
*   NAME
*       disk_insert() -- Request insertion of a disk image.
*
*   SYNOPSIS
*       void disk_insert (int num, const char *name);
*
*   FUNCTION
*       Pass the requested pathname to the normal deferred insertion path.
*
*   INPUTS
*       num - Zero-based floppy-unit index.
*       name - Image pathname or requested name.
*
***************************************************************************
*
*/

void disk_insert (int num, const char *name)
{
    disk_insert_2 (num, name, 0);
}

/****i* januae-disk/disk_insert_force
*
*   NAME
*       disk_insert_force() -- Insert a disk image without the normal deferral.
*
*   SYNOPSIS
*       void disk_insert_force (int num, const char *name);
*
*   FUNCTION
*       Invoke the forced branch of the shared insertion helper; the image
*       stream is examined immediately.
*
*   INPUTS
*       num - Zero-based floppy-unit index.
*       name - Image pathname or requested name.
*
***************************************************************************
*
*/

void disk_insert_force (int num, const char *name)
{
    disk_insert_2 (num, name, 1);
}

/****i* januae-disk/DISK_check_change
*
*   NAME
*       DISK_check_change() -- Apply updated floppy configuration and image choices.
*
*   SYNOPSIS
*       void DISK_check_change (void);
*
*   FUNCTION
*       Synchronize speed, drive types, inserted images and deferred media
*       changes from changed preferences.
*
***************************************************************************
*
*/

void DISK_check_change (void)
{
    int i;

    if (currprefs.floppy_speed != changed_prefs.floppy_speed)
	currprefs.floppy_speed = changed_prefs.floppy_speed;
    for (i = 0; i < MAX_FLOPPY_DRIVES; i++) {
	drive *drv = floppy + i;

	if (currprefs.dfxtype[i] != changed_prefs.dfxtype[i]) {
	    currprefs.dfxtype[i] = changed_prefs.dfxtype[i];
	    reset_drive (i);
	}
	if (strcmp (currprefs.df[i], changed_prefs.df[i]))
	    disk_insert (i, changed_prefs.df[i]);

	if (drv->dskready_down_time > 0)
	    drv->dskready_down_time--;
#if 1
	/* emulate drive motor turn on time */
	if (drv->dskready_time) {
	    drv->dskready_time--;
	    if (drv->dskready_time == 0)
		drv->dskready = 1;
	}
#endif
	/* delay until new disk image is inserted */
	if (drv->dskchange_time) {
	    drv->dskchange_time--;
	    if (drv->dskchange_time == 0) {
		drive_insert (drv, &currprefs, i, drv->newname);
		if (disk_debug_logging > 0)
		    write_log ("delayed insert, drive %d, image '%s'\n", i, drv->newname);
		update_drive_gui (i);

	    }
	}
    }
}

/****i* januae-disk/disk_empty
*
*   NAME
*       disk_empty() -- Report whether the selected floppy unit is empty.
*
*   SYNOPSIS
*       int disk_empty (int num);
*
*   FUNCTION
*       Delegate to the drive backend's empty-media check.
*
*   INPUTS
*       num - Zero-based floppy-unit index.
*
*   RESULT
*       Nonzero if the drive is empty.
*
***************************************************************************
*
*/

int disk_empty (int num)
{
    return drive_empty (floppy + num);
}

/****i* januae-disk/tobin
*
*   NAME
*       tobin() -- Format a byte as eight binary digits.
*
*   SYNOPSIS
*       static char *tobin (uae_u8 v);
*
*   FUNCTION
*       Write the bits most-significant first into a shared, null-terminated
*       static character buffer.
*
*   INPUTS
*       v - Register value, byte value or integer to write, according to the
*       signature.
*
*   RESULT
*       Pointer to static storage replaced by the next call.
*
***************************************************************************
*
*/

static char *tobin (uae_u8 v)
{
    int i;
    static char buf[10];
    for ( i = 7; i >= 0; i--)
	buf[7 - i] = v & (1 << i) ? '1' : '0';
    buf[8] = 0;
    return buf;
}

/****i* januae-disk/DISK_select
*
*   NAME
*       DISK_select() -- Apply disk-control selection and stepping signals.
*
*   SYNOPSIS
*       void DISK_select (uae_u8 data);
*
*   FUNCTION
*       Decode the drive-select, side, direction, step pulse and motor bits,
*       updating the shared hardware state and selected drive mechanisms.
*
*   INPUTS
*       data - Raw floppy-control signal byte.
*
***************************************************************************
*
*/

void DISK_select (uae_u8 data)
{
    unsigned int step_pulse, lastselected, dr;
    static uae_u8 prevdata;
    static unsigned int step;

    if (disk_debug_logging > 1)
	write_log ("%08.8X %02.2X %s", m68k_getpc (&regs), data, tobin (data));

    lastselected = selected;
    selected = (data >> 3) & 15;
    side = 1 - ((data >> 2) & 1);
    direction = (data >> 1) & 1;
    step_pulse = data & 1;

    if ((prevdata & 0x80) != (data & 0x80)) {
	for (dr = 0; dr < 4; dr++) {
	    if (floppy[dr].indexhackmode > 1 && !(selected & (1 << dr))) {
		floppy[dr].indexhack = 1;
		if (disk_debug_logging > 1)
		    write_log (" indexhack!");
	    }
	}
    }

    if (disk_debug_logging > 1) {
	write_log (" %d%d%d%d% ", (selected & 1) ? 0 : 1, (selected & 2) ? 0 : 1, (selected & 4) ? 0 : 1, (selected & 8) ? 0 : 1);
	if ((prevdata & 0x80) != (data & 0x80))
	    write_log (" dskmotor %d ", (data & 0x80) ? 1 : 0);
	if ((prevdata & 0x02) != (data & 0x02))
	    write_log (" direct %d ", (data & 0x02) ? 1 : 0);
	if ((prevdata & 0x04) != (data & 0x04))
	    write_log (" side %d ", (data & 0x04) ? 1 : 0);
    }

    selected |= disabled;

    if (step != step_pulse) {
	if (disk_debug_logging > 1)
	    write_log (" dskstep %d ", step_pulse);
	step = step_pulse;
	if (step && !savestate_state) {
	    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
		if (!(selected & (1 << dr))) {
		    drive_step (floppy + dr);
		    if (floppy[dr].indexhackmode > 1 && (data & 0x80))
			floppy[dr].indexhack = 1;
		}
	    }
	}
    }
    if (!savestate_state) {
	for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	    drive *drv = floppy + dr;
	    /* motor on/off workings tested with small assembler code on real Amiga 1200. */
	    /* motor/id flipflop is set only when drive select goes from high to low */
	    if (!(selected & (1 << dr)) && (lastselected & (1 << dr)) ) {
		drv->drive_id_scnt++;
		drv->drive_id_scnt &= 31;
		drv->idbit = (drv->drive_id & (1L << (31 - drv->drive_id_scnt))) ? 1 : 0;
		if ((prevdata & 0x80) == 0 || (data & 0x80) == 0) {
		    /* motor off: if motor bit = 0 in prevdata or data -> turn motor on */
		    drive_motor (drv, 0);
		} else if (prevdata & 0x80) {
		    /* motor on: if motor bit = 1 in prevdata only (motor flag state in data has no effect)
			-> turn motor off */
		    drive_motor (drv, 1);
		}
		if (currprefs.dfxtype[dr] == DRV_35_DD) {
		    if (dr == 0) /* A500/A2000 internal drive always returns 0 */
			drv->idbit = 0;
		    else /* regular external DD drive always returns 1 */
			drv->idbit = 1;
		}
#ifdef DEBUG_DRIVE_ID
		write_log ("DISK_status: sel %d id %s (%08.8X) [0x%08lx, bit #%02d: %d]\n",
		    dr, drive_id_name (drv), drv->drive_id, drv->drive_id << drv->drive_id_scnt, 31 - drv->drive_id_scnt, drv->idbit);
#endif
	    }
	}
    }
    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	floppy[dr].state = (!(selected & (1 << dr))) | !floppy[dr].motoroff;
	update_drive_gui (dr);
    }
    prevdata = data;
    if (disk_debug_logging > 1)
	write_log ("\n");
}

/****i* januae-disk/DISK_status
*
*   NAME
*       DISK_status() -- Read the combined floppy status input bits.
*
*   SYNOPSIS
*       uae_u8 DISK_status (void);
*
*   FUNCTION
*       Combine ready, track-zero, disk-change, write-protect and drive-ID
*       signals for the currently selected mechanisms.
*
*   RESULT
*       Eight-bit status value with the controller's active-low signals.
*
***************************************************************************
*
*/

uae_u8 DISK_status (void)
{
    uae_u8 st = 0x3c;
    unsigned int dr;

    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	drive *drv = floppy + dr;
	if (!(selected & (1 << dr))) {
	    if (drive_running (drv)) {
		if (drv->catweasel) {
#ifdef CATWEASEL
		    if (catweasel_diskready (drv->catweasel))
			st &= ~0x20;
#endif
		} else {
		    if (drv->dskready && !drv->indexhack && currprefs.dfxtype[dr] != DRV_35_DD_ESCOM)
			st &= ~0x20;
		}
	    } else {
		/* report drive ID */
		if (drv->idbit && currprefs.dfxtype[dr] != DRV_35_DD_ESCOM)
		    st &= ~0x20;
		/* dskrdy needs some cycles after switching the motor off.. (Pro Tennis Tour) */
		if (drv->motordelay) {
		    write_log ("MOTORDELAY! %x\n", m68k_getpc (&regs));
		    st &= ~0x20;
		    drv->motordelay = 0;
		}
	    }
	    if (drive_track0 (drv))
		st &= ~0x10;
	    if (drive_writeprotected (drv))
		st &= ~8;
	    if (drv->catweasel) {
#ifdef CATWEASEL
		if (catweasel_disk_changed (drv->catweasel))
		    st &= ~4;
#endif
	    } else if (drv->dskchange && currprefs.dfxtype[dr] != DRV_525_SD) {
		st &= ~4;
	    }
	}
    }
    return st;
}

/****i* januae-disk/unformatted
*
*   NAME
*       unformatted() -- Check whether the selected track has recorded data.
*
*   SYNOPSIS
*       STATIC_INLINE int unformatted (const drive *drv);
*
*   FUNCTION
*       Treat out-of-range tracks and extended-ADF tracks with zero bit length
*       as unformatted.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
*   RESULT
*       Nonzero for a missing or unformatted track.
*
***************************************************************************
*
*/

STATIC_INLINE int unformatted (const drive *drv)
{
    unsigned int tr = drv->cyl * 2 + side;
    if (tr >= drv->num_tracks)
	return 1;
    if (drv->filetype == ADF_EXT2 && drv->trackdata[tr].bitlen == 0)
	return 1;
    return 0;
}

/* get one bit from MFM bit stream */
/****i* januae-disk/getonebit
*
*   NAME
*       getonebit() -- Read one bit from an MFM track.
*
*   SYNOPSIS
*       STATIC_INLINE uae_u32 getonebit (uae_u16 * mfmbuf, unsigned int mfmpos);
*
*   FUNCTION
*       Select the addressed word and extract its most-significant-bit-first
*       data position.
*
*   INPUTS
*       mfmbuf - Raw MFM bitstream.
*       mfmpos - Bit position within the current track.
*
*   RESULT
*       Zero or one.
*
***************************************************************************
*
*/

STATIC_INLINE uae_u32 getonebit (uae_u16 * mfmbuf, unsigned int mfmpos)
{
    uae_u16 *buf;

    buf = &mfmbuf[mfmpos >> 4];
    return (buf[0] & (1 << (15 - (mfmpos & 15)))) ? 1 : 0;
}

/****i* januae-disk/dumpdisk
*
*   NAME
*       dumpdisk() -- Log current floppy-controller state.
*
*   SYNOPSIS
*       void dumpdisk (void);
*
*   FUNCTION
*       Write drive states and sample MFM words to the debug log; no
*       caller-owned result is produced.
*
***************************************************************************
*
*/

void dumpdisk (void)
{
    int i, j, k;
    uae_u16 w;

    for (i = 0; i < MAX_FLOPPY_DRIVES; i++) {
	drive *drv = &floppy[i];
	if (!(disabled & (1 << i))) {
	    write_log ("Drive %d: motor %s cylinder %2d sel %s %s mfmpos %d/%d\n",
		i, drv->motoroff ? "off" : " on", drv->cyl, (selected & (1 << i)) ? "no" : "yes",
		drive_writeprotected (drv) ? "ro" : "rw", drv->mfmpos, drv->tracklen);
	    w = word;
	    for (j = 0; j < 15; j++) {
		write_log ("%04.4X ", w);
		for (k = 0; k < 16; k++) {
		    w <<= 1;
		    w |= getonebit (drv->bigmfmbuf, drv->mfmpos + j * 16 + k);
		}
	    }
	    write_log ("\n");
	}
    }
    write_log ("side %d, dma %d, bitoffset %d, word %04.4X, dskbytr %04.4X adkcon %04.4X dsksync %04.4X\n", side, dskdmaen, bitoffset, word, dskbytr_val, adkcon, dsksync);
}

/****i* januae-disk/disk_dmafinished
*
*   NAME
*       disk_dmafinished() -- Finish the current disk DMA operation.
*
*   SYNOPSIS
*       static void disk_dmafinished (void);
*
*   FUNCTION
*       Set the disk DMA completion interrupt request and clear active DMA and
*       long-write state.
*
***************************************************************************
*
*/

static void disk_dmafinished (void)
{
    INTREQ (0x8002);
    longwritemode = 0;
    dskdmaen = 0;
    if (disk_debug_logging > 0) {
	unsigned int dr;
	write_log ("disk dma finished %08.8X MFMpos=", dskpt);
	for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++)
	    write_log ("%d%s", floppy[dr].mfmpos, dr < MAX_FLOPPY_DRIVES - 1 ? "," : "");
	write_log ("\n");
    }
}

/****i* januae-disk/fetchnextrevolution
*
*   NAME
*       fetchnextrevolution() -- Load the next revolution of a multi-revolution image.
*
*   SYNOPSIS
*       static void fetchnextrevolution (drive *drv);
*
*   FUNCTION
*       Recalculate track timing and request a fresh CAPS or FDI revolution when
*       the corresponding backend and format are available.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
***************************************************************************
*
*/

static void fetchnextrevolution (drive *drv)
{
    drv->trackspeed = get_floppy_speed () * drv->tracklen / (2 * 8 * FLOPPY_WRITE_LEN * drv->ddhd);
    if (!drv->multi_revolution)
	return;
    switch (drv->filetype)
    {
	case ADF_IPF:
#ifdef CAPS
	caps_loadrevolution (drv->bigmfmbuf, drv - floppy, drv->cyl * 2 + side, &drv->tracklen);
#endif
	break;
	case ADF_FDI:
#ifdef FDI2RAW
	fdi2raw_loadrevolution (drv->fdi, drv->bigmfmbuf, drv->tracktiming, drv->cyl * 2 + side, &drv->tracklen, 1);
#endif
	break;
	default:
	break;
    }
}

/****i* januae-disk/DISK_handler
*
*   NAME
*       DISK_handler() -- Dispatch scheduled floppy-controller events.
*
*   SYNOPSIS
*       void DISK_handler (void);
*
*   FUNCTION
*       Advance disk state to the scheduled cycle, rotate selected tracks and
*       assert pending word-sync or index-sync interrupts.
*
***************************************************************************
*
*/

void DISK_handler (void)
{
    int flag = diskevent_flag;
    eventtab[ev_disk].active = 0;
    DISK_update (disk_sync_cycle);
    if (flag & (DISK_REVOLUTION << 0))
	fetchnextrevolution (&floppy[0]);
    if (flag & (DISK_REVOLUTION << 1))
	fetchnextrevolution (&floppy[1]);
    if (flag & (DISK_REVOLUTION << 2))
	fetchnextrevolution (&floppy[2]);
    if (flag & (DISK_REVOLUTION << 3))
	fetchnextrevolution (&floppy[3]);
    if (flag & DISK_WORDSYNC)
	INTREQ (0x8000 | 0x1000);
    if (flag & DISK_INDEXSYNC)
	cia_diskindex ();
    floppy[0].motordelay = floppy[1].motordelay = floppy[2].motordelay = floppy[3].motordelay = 0;
#if 0
	{
	unsigned int i;
	for (i = 0; i < MAX_FLOPPY_DRIVES; i++) {
	    drive *drv = &floppy[i];
	    if (drv->dskready_time) {
		drv->dskready_time--;
		if (drv->dskready_time == 0) {
		    drv->dskready = 1;
		    if (disk_debug_logging > 0)
			 write_log ("%d: %d\n", i, drv->mfmpos);
		}
	    }
	}
	}
#endif
}

/****i* januae-disk/disk_doupdate_write
*
*   NAME
*       disk_doupdate_write() -- Advance write DMA and selected drive MFM positions.
*
*   SYNOPSIS
*       static void disk_doupdate_write (drive * drv, int floppybits);
*
*   FUNCTION
*       Consume elapsed bit time, stream DMA data and update active selected
*       mechanisms according to motor and DMA state.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*       floppybits - Elapsed bitcell time for the update.
*
***************************************************************************
*
*/

static void disk_doupdate_write (drive * drv, int floppybits)
{
    int dr;
    int drives[4];

    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	drive *drv2 = &floppy[dr];
	drives[dr] = 0;
	if (drv2->motoroff)
	    continue;
	if (selected & (1 << dr))
	    continue;
	drives[dr] = 1;
    }

    while (floppybits >= drv->trackspeed) {
	for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	    if (drives[dr]) {
		floppy[dr].mfmpos++;
		floppy[dr].mfmpos %= drv->tracklen;
	    }
	}
	if ((dmacon & 0x210) == 0x210 && dskdmaen == 3 && dsklength > 0) {
	    bitoffset++;
	    bitoffset &= 15;
	    if (!bitoffset) {
		for (dr = 0; dr < MAX_FLOPPY_DRIVES ; dr++) {
		    drive *drv2 = &floppy[dr];
		    if (drives[dr])
			drv2->bigmfmbuf[drv2->mfmpos >> 4] = get_word (dskpt);
		}
		dskpt += 2;
		dsklength--;
		if (dsklength == 0) {
		    disk_dmafinished ();
		    for (dr = 0; dr < MAX_FLOPPY_DRIVES ; dr++) {
			drive *drv2 = &floppy[dr];
			if (drives[dr])
			    drive_write_data (drv2);
		    }
		}
	    }
	}
	floppybits -= drv->trackspeed;
    }
}

/****i* januae-disk/updatetrackspeed
*
*   NAME
*       updatetrackspeed() -- Apply per-track variable-bit-rate timing.
*
*   SYNOPSIS
*       static void updatetrackspeed (drive *drv, unsigned int mfmpos);
*
*   FUNCTION
*       Scale the drive's bit time using the recorded track timing table, when
*       disk DMA is not in the active write phase.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*       mfmpos - Bit position within the current track.
*
***************************************************************************
*
*/

static void updatetrackspeed (drive *drv, unsigned int mfmpos)
{
    if (dskdmaen < 3) {
	uae_u16 *p = drv->tracktiming;
	p += mfmpos / 8;
	drv->trackspeed = get_floppy_speed () * drv->tracklen / (2 * 8 * FLOPPY_WRITE_LEN * drv->ddhd);
	drv->trackspeed = drv->trackspeed * p[0] / 1000;
	if (drv->trackspeed < 700 || drv->trackspeed > 3000) {
	    static int warned;
	    warned++;
	    if (warned < 50)
		write_log ("corrupted trackspeed value %d\n", drv->trackspeed);
//	    assert (0);
	}
    }
}

/****i* januae-disk/disk_doupdate_predict
*
*   NAME
*       disk_doupdate_predict() -- Predict the next floppy event within a scanline.
*
*   SYNOPSIS
*       STATIC_INLINE void disk_doupdate_predict (drive * drv, unsigned int
*       startcycle, int write);
*
*   FUNCTION
*       Advance a local MFM model until the next sync, index or revolution
*       boundary and schedule the corresponding event; write mode affects
*       bitstream handling.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*       startcycle - Starting cycle within the current scanline.
*       write - Nonzero to predict a write operation.
*
***************************************************************************
*
*/

STATIC_INLINE void disk_doupdate_predict (drive * drv, unsigned int startcycle, int write)
{
    unsigned int firstcycle = startcycle;
    unsigned int endcycle = maxhpos << 8;
    uae_u32 tword = word;
    unsigned int mfmpos = drv->mfmpos;
    int is_empty       = drive_empty (drv);
    int is_unformatted = unformatted (drv);
    unsigned int event_flag = 0;

    while (startcycle < endcycle && !event_flag) {
	if (drv->tracktiming[0])
	    updatetrackspeed (drv, mfmpos);
	if (!write) {
	    tword <<= 1;
	    if (!is_empty) {
		if (is_unformatted)
		    tword |= (rand () & 0x1000) ? 1 : 0;
		else
		    tword |= getonebit (drv->bigmfmbuf, mfmpos);
	    }
	    if ((tword & 0xffff) == dsksync)
		event_flag |= DISK_WORDSYNC;
	}
	mfmpos++;
	mfmpos %= drv->tracklen;
	if (mfmpos == 0)
	    event_flag |= DISK_REVOLUTION << (drv - floppy);
	if (mfmpos == drv->indexoffset)
	    event_flag |= DISK_INDEXSYNC;
	if (!write && (int)mfmpos == drv->skipoffset) {
	    int skipcnt = disk_jitter;
	    while (skipcnt-- > 0) {
		mfmpos++;
		mfmpos %= drv->tracklen;
		if (mfmpos == 0)
		    event_flag |= DISK_REVOLUTION << (drv - floppy);
		if (mfmpos == drv->indexoffset)
		    event_flag |= DISK_INDEXSYNC;
	    }
	}
	startcycle += drv->trackspeed;
    }
    if (drv->tracktiming[0])
	updatetrackspeed (drv, drv->mfmpos);
    diskevent_flag = event_flag;
    if (event_flag) {
	disk_sync_cycle = startcycle >> 8;
	eventtab[ev_disk].oldcycles = get_cycles ();
	eventtab[ev_disk].evtime = get_cycles () + startcycle - firstcycle;
	eventtab[ev_disk].active = 1;
	events_schedule ();
    }
}

#ifdef CPUEMU_6
extern uae_u8 cycle_line[256];
#endif

/****i* januae-disk/disk_doupdate_read
*
*   NAME
*       disk_doupdate_read() -- Advance the selected drive's read shifter.
*
*   SYNOPSIS
*       static void disk_doupdate_read (drive * drv, int floppybits);
*
*   FUNCTION
*       Consume elapsed bit times from the MFM track into the shared word and
*       track cursor, including disk sync detection and DMA conditions.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*       floppybits - Elapsed bitcell time for the update.
*
***************************************************************************
*
*/

static void disk_doupdate_read (drive * drv, int floppybits)
{
//    int j = 0, k = 1, l = 0;

/*
    uae_u16 *mfmbuf = drv->bigmfmbuf;
    dsksync = 0x4444;
    adkcon |= 0x400;
    drv->mfmpos = 0;
    memset (mfmbuf, 0, 1000);
    cycles = 0x1000000;
    // 4444 4444 4444 aaaa aaaaa 4444 4444 4444
    // 4444 aaaa aaaa 4444
    mfmbuf[0] = 0x4444;
    mfmbuf[1] = 0x4444;
    mfmbuf[2] = 0x4444;
    mfmbuf[3] = 0xaaaa;
    mfmbuf[4] = 0xaaaa;
    mfmbuf[5] = 0x4444;
    mfmbuf[6] = 0x4444;
    mfmbuf[7] = 0x4444;
*/

    int is_empty       = drive_empty (drv);
    int is_unformatted = unformatted (drv);

    while (floppybits >= drv->trackspeed) {
	if (drv->tracktiming[0])
	    updatetrackspeed (drv, drv->mfmpos);
	word <<= 1;
	if (!is_empty) {
	    if (is_unformatted)
		word |= (rand () & 0x1000) ? 1 : 0;
	    else
		word |= getonebit (drv->bigmfmbuf, drv->mfmpos);
	}

	drv->mfmpos++;
	drv->mfmpos %= drv->tracklen;
	if (drv->mfmpos == drv->indexoffset) {
	    if (disk_debug_logging > 1 && drv->indexhack)
		write_log ("indexhack cleared\n");
	    drv->indexhack = 0;
	}
	if ((int)drv->mfmpos == drv->skipoffset) {
	    drv->mfmpos += disk_jitter;
	    drv->mfmpos %= drv->tracklen;
	}
	if (bitoffset == 15 && dma_enable && dskdmaen == 2 && dsklength >= 0) {
	    if (dsklength > 0) {
		do_chipmem_wput (dskpt, word);
		dskpt += 2;
#ifdef CPUEMU_6
		cycle_line[7] |= CYCLE_MISC;
		cycle_line[9] |= CYCLE_MISC;
#endif
	    }
#if 0
	    dma_tab[j++] = word;
	    if (j == MAX_DISK_WORDS_PER_LINE - 1) {
		write_log ("Bug: Disk DMA buffer overflow!\n");
		j--;
	    }
#endif
	    dsklength--;
	    if (dsklength <= 0)
		disk_dmafinished ();
	}
	if ((bitoffset & 7) == 7) {
	    dskbytr_val = word & 0xff;
	    dskbytr_val |= 0x8000;
	}
	if (word == dsksync) {
	    if (adkcon & 0x400)
		bitoffset = 15;
	    if (dskdmaen) {
		if (disk_debug_logging && dma_enable == 0)
		    write_log ("Sync match, DMA started at %d\n", drv->mfmpos);
		dma_enable = 1;
	    }
	}
	bitoffset++;
	bitoffset &= 15;
	floppybits -= drv->trackspeed;
    }
#if 0
    dma_tab[j] = 0xffffffff;
#endif
}

/****i* januae-disk/disk_dma_debugmsg
*
*   NAME
*       disk_dma_debugmsg() -- Log disk DMA control registers.
*
*   SYNOPSIS
*       static void disk_dma_debugmsg (void);
*
*   FUNCTION
*       Report DMA length, sync word, memory pointer, ADKCON and the current
*       guest program counter.
*
***************************************************************************
*
*/

static void disk_dma_debugmsg (void)
{
    write_log ("LEN=%04.4X (%d) SYNC=%04.4X PT=%08.8X ADKCON=%04.4X PC=%08.8X\n",
	dsklength, dsklength, (adkcon & 0x400) ? dsksync : 0xffff, dskpt, adkcon, m68k_getpc (&regs));
}

#if 0
/* disk DMA fetch happens on real Amiga at the beginning of next horizontal line
   (cycles 9, 11 and 13 according to hardware manual) We transfer all DMA'd
   data at cycle 0. I don't think any program cares about this small difference.
*/
static void dodmafetch (void)
{
    unsigned int i;

    i = 0;
    while (dma_tab[i] != 0xffffffff && dskdmaen != 3 && (dmacon & 0x210) == 0x210) {
	put_word (dskpt, dma_tab[i++]);
	dskpt += 2;
    }
    dma_tab[0] = 0xffffffff;
}
#endif

/* this is very unoptimized. DSKBYTR is used very rarely, so it should not matter. */

/****i* januae-disk/DSKBYTR
*
*   NAME
*       DSKBYTR() -- Read the emulated DSKBYTR hardware register.
*
*   SYNOPSIS
*       uae_u16 DSKBYTR (unsigned int hpos);
*
*   FUNCTION
*       Advance disk timing to the supplied horizontal position, return status
*       and data bits, and clear the latched byte-ready flag.
*
*   INPUTS
*       hpos - Current horizontal beam position.
*
*   RESULT
*       Current disk byte/status register value.
*
***************************************************************************
*
*/

uae_u16 DSKBYTR (unsigned int hpos)
{
    uae_u16 v;

    DISK_update (hpos);
    v = dskbytr_val;
    dskbytr_val &= ~0x8000;
    if (word == dsksync)
	v |= 0x1000;
    if (dskdmaen && (dmacon & 0x210) == 0x210)
	v |= 0x4000;
    if (dsklen & 0x4000)
	v |= 0x2000;
    if (disk_debug_logging > 1)
	write_log ("DSKBYTR=%04.4X hpos=%d\n", v, hpos);
#ifdef DEBUGGER
    if (disk_debug_mode & DISK_DEBUG_PIO) {
	unsigned int dr;
	for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	    drive *drv = &floppy[dr];
	    if (drv->motoroff)
		continue;
	    if (!(selected & (1 << dr))) {
		if (disk_debug_track < 0 || disk_debug_track == 2 * (int)drv->cyl + side) {
		    disk_dma_debugmsg ();
		    write_log ("DSKBYTR=%04.4X\n", v);
		    activate_debugger ();
		    break;
		}
	    }
	}
    }
#endif

    return v;
}

/****i* januae-disk/DISK_start
*
*   NAME
*       DISK_start() -- Initialize a newly armed disk DMA operation.
*
*   SYNOPSIS
*       static void DISK_start (void);
*
*   FUNCTION
*       Adjust active selected drives' MFM cursor and timing for read or write
*       DMA; absent media and special track types use their respective path.
*
***************************************************************************
*
*/

static void DISK_start (void)
{
    unsigned int dr;
    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	drive *drv = &floppy[dr];
	if (!(selected & (1 << dr))) {
	    unsigned int tr = drv->cyl * 2 + side;
	    trackid *ti = drv->trackdata + tr;

	    if (dskdmaen == 3) {
		drv->tracklen = longwritemode ? FLOPPY_WRITE_MAXLEN : FLOPPY_WRITE_LEN * drv->ddhd * 8 * 2;
		drv->trackspeed = get_floppy_speed ();
		drv->skipoffset = -1;
		updatemfmpos (drv);
	    }
	    /* Ugh.  A nasty hack.  Assume ADF_EXT1 tracks are always read
	       from the start.  */
	    if (ti->type == TRACK_RAW1)
		drv->mfmpos = 0;
	    if (drv->catweasel)
		drive_fill_bigbuf (drv, 1);
	}
	drv->floppybitcounter = 0;
    }
    dma_enable = (adkcon & 0x400) ? 0 : 1;
}

static int linecounter;

/****i* januae-disk/DISK_hsync
*
*   NAME
*       DISK_hsync() -- Advance disk hardware at horizontal synchronization.
*
*   SYNOPSIS
*       void DISK_hsync (unsigned int tohpos);
*
*   FUNCTION
*       Age step limits, handle pending DMA completion delay and advance the
*       floppy state for the current scanline.
*
*   INPUTS
*       tohpos - Target horizontal beam position.
*
***************************************************************************
*
*/

void DISK_hsync (unsigned int tohpos)
{
    unsigned int dr;

    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	drive *drv = &floppy[dr];
	if (drv->steplimit)
	    drv->steplimit--;
    }
    if (linecounter) {
	linecounter--;
	if (! linecounter)
	    disk_dmafinished ();
	return;
    }
    DISK_update (tohpos);
}

/****i* januae-disk/DISK_update
*
*   NAME
*       DISK_update() -- Advance the disk controller to a horizontal position.
*
*   SYNOPSIS
*       void DISK_update (unsigned int tohpos);
*
*   FUNCTION
*       Consume elapsed subcycles, update selected rotating tracks and predict
*       any subsequent sync or index events.
*
*   INPUTS
*       tohpos - Target horizontal beam position.
*
***************************************************************************
*
*/

void DISK_update (unsigned int tohpos)
{
    unsigned int dr;
    int cycles = (int)((tohpos << 8) - disk_hpos);

    disk_jitter = ((rand () >> 4) & 3) + 1;
    if (disk_jitter > 2)
	disk_jitter = 1;
    if (cycles <= 0)
	return;
    disk_hpos += cycles;
    if (disk_hpos >= (maxhpos << 8))
	disk_hpos -= maxhpos << 8;

#if 0
    dodmafetch ();
#endif

    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	drive *drv = &floppy[dr];

	if (drv->motoroff)
	    continue;
	drv->floppybitcounter += cycles;
	if (selected & (1 << dr)) {
	    drv->mfmpos += drv->floppybitcounter / drv->trackspeed;
	    drv->mfmpos %= drv->tracklen;
	    drv->floppybitcounter %= drv->trackspeed;
	    continue;
	}
	drive_fill_bigbuf (drv, 0);
	drv->mfmpos %= drv->tracklen;
    }
    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	drive *drv = &floppy[dr];
	if (drv->motoroff)
	    continue;
	if (selected & (1 << dr))
	    continue;
	if (dskdmaen == 3) {
	    disk_doupdate_write (drv, drv->floppybitcounter);
	    disk_doupdate_predict (drv, disk_hpos, 1);
	} else {
	    disk_doupdate_read (drv, drv->floppybitcounter);
	    disk_doupdate_predict (drv, disk_hpos, 0);
	}
	drv->floppybitcounter %= drv->trackspeed;
	break;
    }
}

/****i* januae-disk/DSKLEN
*
*   NAME
*       DSKLEN() -- Write the disk DMA length and control register.
*
*   SYNOPSIS
*       void DSKLEN (uae_u16 v, unsigned int hpos);
*
*   FUNCTION
*       Advance to the write position, implement the double-write DMA start
*       sequence and handle cancellation and read/write selection.
*
*   INPUTS
*       v - Register value, byte value or integer to write, according to the
*       signature.
*       hpos - Current horizontal beam position.
*
***************************************************************************
*
*/

void DSKLEN (uae_u16 v, unsigned int hpos)
{
    unsigned int dr, prev = dsklen;

    DISK_update (hpos);
    if ((v & 0x8000) && (dsklen & 0x8000)) {
	dskdmaen = 2;
	DISK_start ();
    }
    if (!(v & 0x8000)) {
	if (dskdmaen) {
	    /* Megalomania and Knightmare does this */
	    if (disk_debug_logging > 0 && dskdmaen == 2)
		write_log ("warning: Disk read DMA aborted, %d words left PC=%x\n", dsklength, m68k_getpc (&regs));
	    if (dskdmaen == 3)
		write_log ("warning: Disk write DMA aborted, %d words left PC=%x\n", dsklength, m68k_getpc (&regs));
	    dskdmaen = 0;
	}
    }
    dsklen = v;
    dsklength2 = dsklength = dsklen & 0x3fff;

    if (dskdmaen == 0)
	return;

    if ((v & 0x4000) && (prev & 0x4000)) {
	if (dsklength == 0)
	    return;
	if (dsklength == 1) {
	    disk_dmafinished ();
	    return;
	}
	dskdmaen = 3;
	DISK_start ();
    }

    if (dsklength == 1)
	dsklength = 0;

#ifdef DEBUGGER
    if (((disk_debug_mode & DISK_DEBUG_DMA_READ) && dskdmaen == 2) ||
	((disk_debug_mode & DISK_DEBUG_DMA_WRITE) && dskdmaen == 3))
    {
	for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	    drive *drv = &floppy[dr];
	    if (drv->motoroff)
		continue;
	    if (!(selected & (1 << dr))) {
		if (disk_debug_track < 0 || disk_debug_track == 2 * (int)drv->cyl + side) {
		    disk_dma_debugmsg ();
		    activate_debugger ();
		    break;
		}
	    }
	}
    }
#endif

    if (disk_debug_logging > 0) {
	for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	    drive *drv = &floppy[dr];
	    if (drv->motoroff)
		continue;
	    if ((selected & (1 << dr)) == 0)
		break;
	}
	if (dr == 4) {
	    write_log ("disk %s DMA started but no drive selected!\n",
	       dskdmaen == 3 ? "write" : "read");
	} else {
	    write_log ("disk %s DMA started, drv=%x track %d mfmpos %d\n",
		dskdmaen == 3 ? "write" : "read", selected ^ 15,
		floppy[dr].cyl * 2 + side, floppy[dr].mfmpos);
	}
	disk_dma_debugmsg ();
    }

    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++)
	update_drive_gui (dr);

    /* Try to make floppy access from Kickstart faster.  */
    if (dskdmaen != 2 && dskdmaen != 3)
	return;
    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	drive *drv = &floppy[dr];
	if (selected & (1 << dr))
	    continue;
	if (drv->filetype != ADF_NORMAL)
	    break;
    }
    if (dr < MAX_FLOPPY_DRIVES) /* no turbo mode if any selected drive has non-standard ADF */
	return;
    {
	int done = 0;
	for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	    drive *drv = &floppy[dr];
	    unsigned int pos;

	    if (drv->motoroff)
		continue;
	    if (!drv->useturbo && currprefs.floppy_speed > 0)
		continue;
	    if (selected & (1 << dr))
		continue;

	    pos = drv->mfmpos & ~15;
	    drive_fill_bigbuf (drv, 0);

	    if (dskdmaen == 2) { /* TURBO read */

		if (adkcon & 0x400) {
		    unsigned int i;
		    for (i = 0; i < drv->tracklen; i += 16) {
			pos += 16;
			pos %= drv->tracklen;
			if (drv->bigmfmbuf[pos >> 4] == dsksync) {
			    /* must skip first disk sync marker */
			    pos += 16;
			    pos %= drv->tracklen;
			    break;
			}
		    }
		    if (i >= drv->tracklen)
			return;
		}
		while (dsklength-- > 0) {
		    put_word (dskpt, drv->bigmfmbuf[pos >> 4]);
		    dskpt += 2;
		    pos += 16;
		    pos %= drv->tracklen;
		}
		INTREQ (0x9000);
		done = 1;

	    } else if (dskdmaen == 3) { /* TURBO write */

		int i;
		for (i = 0; i < dsklength; i++) {
		    drv->bigmfmbuf[pos >> 4] = get_word (dskpt + i * 2);
		    pos += 16;
		    pos %= drv->tracklen;
		}
		drive_write_data (drv);
		done = 1;
	    }
	}
	if (done) {
	    linecounter = 2;
	    dskdmaen = 0;
	    return;
	}
    }
}

/****i* januae-disk/DSKSYNC
*
*   NAME
*       DSKSYNC() -- Set the disk sync word.
*
*   SYNOPSIS
*       void DSKSYNC (unsigned int hpos, uae_u16 v);
*
*   FUNCTION
*       Update disk timing before changing the sync comparison value; a write of
*       the existing value is ignored.
*
*   INPUTS
*       hpos - Current horizontal beam position.
*       v - Register value, byte value or integer to write, according to the
*       signature.
*
***************************************************************************
*
*/

void DSKSYNC (unsigned int hpos, uae_u16 v)
{
    if (v == dsksync)
	return;
    DISK_update (hpos);
    dsksync = v;
}

/****i* januae-disk/DSKDAT
*
*   NAME
*       DSKDAT() -- Handle a direct write to the disk data register.
*
*   SYNOPSIS
*       void DSKDAT (uae_u16 v);
*
*   FUNCTION
*       Log the first five direct register writes and suppress later repeated
*       diagnostics; this path does not transmit the supplied word.
*
*   INPUTS
*       v - Register value, byte value or integer to write, according to the
*       signature.
*
***************************************************************************
*
*/

void DSKDAT (uae_u16 v)
{
    static int count = 0;
    if (count < 5) {
	count++;
	write_log ("%04.4X written to DSKDAT. Not good. PC=%08.8X", v, m68k_getpc (&regs));
	if (count == 5)
	    write_log ("(further messages suppressed)");

	write_log ("\n");
    }
}

/****i* januae-disk/DSKPTH
*
*   NAME
*       DSKPTH() -- Write the upper disk DMA pointer word.
*
*   SYNOPSIS
*       void DSKPTH (uae_u16 v);
*
*   FUNCTION
*       Replace the high sixteen bits of the emulated disk DMA address.
*
*   INPUTS
*       v - Register value, byte value or integer to write, according to the
*       signature.
*
***************************************************************************
*
*/

void DSKPTH (uae_u16 v)
{
    dskpt = (dskpt & 0xffff) | ((uae_u32) v << 16);
}

/****i* januae-disk/DSKPTL
*
*   NAME
*       DSKPTL() -- Write the lower disk DMA pointer word.
*
*   SYNOPSIS
*       void DSKPTL (uae_u16 v);
*
*   FUNCTION
*       Replace the low sixteen bits of the emulated disk DMA address.
*
*   INPUTS
*       v - Register value, byte value or integer to write, according to the
*       signature.
*
***************************************************************************
*
*/

void DSKPTL (uae_u16 v)
{
    dskpt = (dskpt & ~0xffff) | (v);
}

/****i* januae-disk/DISK_free
*
*   NAME
*       DISK_free() -- Release the floppy subsystem's image resources.
*
*   SYNOPSIS
*       void DISK_free (void);
*
*   FUNCTION
*       Release all attached image state and free the drive array only when
*       SAVE_MEMORY allocates it dynamically.
*
***************************************************************************
*
*/

void DISK_free (void)
{
#ifdef SAVE_MEMORY
    if (floppy) {
#endif
	unsigned int dr;

	for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	    drive *drv = &floppy[dr];
	    drive_image_free (drv);
	}
#ifdef SAVE_MEMORY
	free (floppy);
	floppy = 0;
    }
#endif
}

/****i* januae-disk/DISK_init
*
*   NAME
*       DISK_init() -- Initialize emulated floppy mechanisms.
*
*   SYNOPSIS
*       void DISK_init (void);
*
*   FUNCTION
*       Allocate drive structures when configured, initialize drive IDs and
*       attach media specified by current preferences.
*
***************************************************************************
*
*/

void DISK_init (void)
{
    unsigned int dr;

#if 0
    dma_tab[0] = 0xffffffff;
#endif

#ifdef SAVE_MEMORY
    floppy = malloc (sizeof (drive) * MAX_FLOPPY_DRIVES);
    if (!floppy)
	abort ();
    memset (floppy, 0, sizeof (drive) * MAX_FLOPPY_DRIVES);
#endif
    for (dr = 0; dr < MAX_FLOPPY_DRIVES; dr++) {
	drive *drv = &floppy[dr];
	/* reset all drive types to 3.5 DD */
	drive_settype_id (drv);
	if (!drive_insert (drv, &currprefs, dr, currprefs.df[dr]))
	    disk_eject (dr);
    }
    if (disk_empty (0))
	write_log ("No disk in drive 0.\n");
}

/****i* januae-disk/DISK_reset
*
*   NAME
*       DISK_reset() -- Reset floppy controller and drives.
*
*   SYNOPSIS
*       void DISK_reset (void);
*
*   FUNCTION
*       Clear disk cycle and DMA state and reset each configured mechanism; do
*       nothing during an active SaveState restore.
*
***************************************************************************
*
*/

void DISK_reset (void)
{
    int i;

    if (savestate_state)
	return;

    //floppy[0].catweasel = &cwc.drives[0];
    disk_hpos = 0;
    dskdmaen = 0;
    disabled = 0;
    for (i = 0; i < MAX_FLOPPY_DRIVES; i++)
	reset_drive (i);
}

/****i* januae-disk/DISK_examine_image
*
*   NAME
*       DISK_examine_image() -- Inspect a floppy image's AmigaDOS boot sector.
*
*   SYNOPSIS
*       int DISK_examine_image (struct uae_prefs *p, int num, uae_u32 *crc32);
*
*   FUNCTION
*       Mount the selected image temporarily, calculate CRC, decode its initial
*       sectors and classify the boot block signature and checksum.
*
*   INPUTS
*       p - Input bytes or selected preferences, according to the signature.
*       num - Zero-based floppy-unit index.
*       crc32 - Optional output for the image CRC32.
*
*   RESULT
*       1 on open failure, 2 on MFM decode failure, 3 on boot checksum failure,
*       4 on unknown DOS signature, 10-12 for recognized DOS variants.
*
***************************************************************************
*
*/

int DISK_examine_image (struct uae_prefs *p, int num, uae_u32 *crc32)
{
    unsigned int drvsec;
    unsigned int ret, i;
    drive *drv = &floppy[num];
    uae_u32 dos = 0;
    uae_u32 crc, crc2;

    ret = 0;
    drv->cyl = 0;
    side = 0;
    *crc32 = 0;
    if (!drive_insert (drv, p, num, p->df[num]))
	return 1;
    if (!drv->diskfile)
	return 1;
    *crc32 = zfile_crc32 (drv->diskfile);
    if (decode_buffer (drv->bigmfmbuf, drv->cyl, 11, drv->ddhd, drv->filetype, &drvsec, 1)) {
	ret = 2;
	goto end;
    }
    crc = crc2 = 0;
    for (i = 0; i < 1024; i += 4) {
	uae_u32 v = (writebuffer[i] << 24) | (writebuffer[i + 1] << 16) | (writebuffer[i + 2] << 8) | writebuffer[i + 3];
	if (i == 0)
	    dos = v;
	if (i == 4) {
	    crc2 = v;
	    v = 0;
	}
	if (crc + v < crc)
	    crc++;
	crc += v;
    }
    crc ^= 0xffffffff;
    if (crc != crc2)
	return 3;
    if (dos == 0x444f5300)
	ret = 10;
    else if (dos == 0x444f5301 || dos == 0x444f5302 || dos == 0x444f5303)
	ret = 11;
    else if (dos == 0x444f5304 || dos == 0x444f5305 || dos == 0x444f5306 || dos == 0x444f5307)
	ret = 12;
    else
	ret = 4;
end:
    drive_image_free (drv);
    return ret;
}


/* Disk save/restore code */

#if defined SAVESTATE || defined DEBUGGER

/****i* januae-disk/DISK_save_custom
*
*   NAME
*       DISK_save_custom() -- Copy disk-controller register values to optional outputs.
*
*   SYNOPSIS
*       void DISK_save_custom (uae_u32 *pdskpt, uae_u16 *pdsklength, uae_u16
*       *pdsksync, uae_u16 *pdskbytr);
*
*   FUNCTION
*       Write the DMA pointer, current length, sync word and byte register to
*       non-NULL caller pointers. Compiled only with SAVESTATE or DEBUGGER.
*
*   INPUTS
*       pdskpt - DMA pointer value or optional output, according to the
*       signature.
*       pdsklength - DMA word count or optional output.
*       pdsksync - Optional disk sync-word output.
*       pdskbytr - Byte/status register or optional output.
*
***************************************************************************
*
*/

void DISK_save_custom (uae_u32 *pdskpt, uae_u16 *pdsklength, uae_u16 *pdsksync, uae_u16 *pdskbytr)
{
    if (pdskpt) *pdskpt = dskpt;
    if (pdsklength) *pdsklength = dsklength;
    if (pdsksync) *pdsksync = dsksync;
    if (pdskbytr) *pdskbytr = dskbytr_val;
}

#endif /* SAVESTATE || DEBUGGER */

#ifdef SAVESTATE

/****i* januae-disk/DISK_restore_custom
*
*   NAME
*       DISK_restore_custom() -- Restore selected disk-controller registers.
*
*   SYNOPSIS
*       void DISK_restore_custom (uae_u32 pdskpt, uae_u16 pdsklength, uae_u16
*       pdskbytr);
*
*   FUNCTION
*       Assign the DMA pointer, length and DSKBYTR register from the saved
*       values; DSKSYNC is not restored by this function. Requires SAVESTATE.
*
*   INPUTS
*       pdskpt - DMA pointer value or optional output, according to the
*       signature.
*       pdsklength - DMA word count or optional output.
*       pdskbytr - Byte/status register or optional output.
*
***************************************************************************
*
*/

void DISK_restore_custom (uae_u32 pdskpt, uae_u16 pdsklength, uae_u16 pdskbytr)
{
    dskpt = pdskpt;
    dsklength = pdsklength;
    dskbytr_val = pdskbytr;
}

/****i* januae-disk/restore_disk
*
*   NAME
*       restore_disk() -- Restore one drive's SaveState record.
*
*   SYNOPSIS
*       const uae_u8 *restore_disk (unsigned int num, const uae_u8 *src);
*
*   FUNCTION
*       Read drive identification and media state from the SaveState stream,
*       restore the disk pathname and reopen the image. Requires SAVESTATE.
*
*   INPUTS
*       num - Zero-based floppy-unit index.
*       src - Input stream, input bytes or SaveState buffer, according to the
*       signature.
*
*   RESULT
*       Pointer following the variable-length saved disk pathname.
*
***************************************************************************
*
*/

const uae_u8 *restore_disk (unsigned int num, const uae_u8 *src)
{
    drive *drv;
    int state, dfxtype;

    drv = &floppy[num];
    disabled &= ~(1 << num);
    drv->drive_id = restore_u32 ();
    drv->motoroff = 1;
    drv->idbit = 0;
    state = restore_u8 ();
    if (state & 2) {
	disabled |= 1 << num;
	if (changed_prefs.nr_floppies > (int)num)
	    changed_prefs.nr_floppies = num;
	changed_prefs.dfxtype[num] = -1;
    } else {
	drv->motoroff = (state & 1) ? 0 : 1;
	drv->idbit = (state & 4) ? 1 : 0;
	if (changed_prefs.nr_floppies < (int)num)
	    changed_prefs.nr_floppies = num;
	switch (drv->drive_id)
	{
	    case DRIVE_ID_35HD:
	    dfxtype = DRV_35_HD;
	    break;
	    case DRIVE_ID_525SD:
	    dfxtype = DRV_525_SD;
	    break;
	    default:
	    dfxtype = DRV_35_DD;
	    break;
	}
	changed_prefs.dfxtype[num] = dfxtype;
    }
    drv->cyl = restore_u8 ();
    drv->dskready = restore_u8 ();
    drv->drive_id_scnt = restore_u8 ();
    drv->mfmpos = restore_u32 ();
    drv->dskchange = 0;
    drv->dskchange_time = 0;
    restore_u32 ();
    strncpy (changed_prefs.df[num], (const char *)src, 255);
    src += strlen ((const char *)src) + 1;
    drive_insert (floppy + num, &currprefs, num, changed_prefs.df[num]);
    if (drive_empty (floppy + num))
	drv->dskchange = 1;

    return src;
}

/****i* januae-disk/getadfcrc
*
*   NAME
*       getadfcrc() -- Compute the CRC32 of the inserted image.
*
*   SYNOPSIS
*       static uae_u32 getadfcrc (drive *drv);
*
*   FUNCTION
*       Read the complete image into a temporary allocation and compute its
*       CRC32. Allocation failures and missing images yield zero.
*
*   INPUTS
*       drv - Selected drive state; owned by the subsystem.
*
*   RESULT
*       CRC32, or zero when no image is available or allocation fails.
*
***************************************************************************
*
*/

static uae_u32 getadfcrc (drive *drv)
{
    uae_u8 *b;
    uae_u32 crc32;
    int size;

    if (!drv->diskfile)
	return 0;
    zfile_fseek (drv->diskfile, 0, SEEK_END);
    size = zfile_ftell (drv->diskfile);
    b = malloc (size);
    if (!b)
	return 0;
    zfile_fseek (drv->diskfile, 0, SEEK_SET);
    zfile_fread (b, 1, size, drv->diskfile);
    crc32 = get_crc32 (b, size);
    free (b);
    return crc32;
}

/****i* januae-disk/save_disk
*
*   NAME
*       save_disk() -- Serialize one drive's SaveState record.
*
*   SYNOPSIS
*       uae_u8 *save_disk (unsigned int num, uae_u32 *len, uae_u8 *dstptr);
*
*   FUNCTION
*       Write drive type, flags, cylinder, ready state, bit position, image CRC
*       and configured pathname to the supplied or allocated buffer. Requires
*       SAVESTATE.
*
*   INPUTS
*       num - Zero-based floppy-unit index.
*       len - Byte or bit count, or optional serialized-length output, according
*       to the signature.
*       dstptr - Optional caller-provided serialization buffer.
*
*   RESULT
*       Pointer to the record buffer, allocated internally when dstptr is NULL.
*
***************************************************************************
*
*/

uae_u8 *save_disk (unsigned int num, uae_u32 *len, uae_u8 *dstptr)
{
    uae_u8 *dstbak,*dst;
    drive *drv;

    drv = &floppy[num];
    if (dstptr)
	dstbak = dst = dstptr;
    else
	dstbak = dst = malloc (2+1+1+1+1+4+4+256);
    save_u32 (drv->drive_id);	    /* drive type ID */
    save_u8 ((drv->motoroff ? 0:1) | ((disabled & (1 << num)) ? 2 : 0) | (drv->idbit ? 4 : 0));
    save_u8 (drv->cyl);		    /* cylinder */
    save_u8 (drv->dskready);	    /* dskready */
    save_u8 (drv->drive_id_scnt);   /* id mode position */
    save_u32 (drv->mfmpos);	    /* disk position */
    save_u32 (getadfcrc (drv));	    /* CRC of disk image */
    strcpy ((char *) dst, currprefs.df[num]);/* image name */
    dst += strlen ((const char *)dst) + 1;

    *len = dst - dstbak;
    return dstbak;
}

/* internal floppy controller variables */

/****i* januae-disk/restore_floppy
*
*   NAME
*       restore_floppy() -- Restore global disk-controller SaveState fields.
*
*   SYNOPSIS
*       const uae_u8 *restore_floppy (const uae_u8 *src);
*
*   FUNCTION
*       Read the serialized serial word, bit offset, sync/DMA flags and
*       horizontal position. Requires SAVESTATE.
*
*   INPUTS
*       src - Input stream, input bytes or SaveState buffer, according to the
*       signature.
*
*   RESULT
*       The supplied src pointer, which is not advanced by this routine.
*
***************************************************************************
*
*/

const uae_u8 *restore_floppy (const uae_u8 *src)
{
    word = restore_u16 ();
    bitoffset = restore_u8 ();
    dma_enable = restore_u8 ();
    disk_hpos = restore_u8 () << 8;
    dskdmaen = restore_u8 ();
    restore_u16 ();
    //word |= restore_u16 () << 16;

    return src;
}

/****i* januae-disk/save_floppy
*
*   NAME
*       save_floppy() -- Serialize global disk-controller SaveState fields.
*
*   SYNOPSIS
*       uae_u8 *save_floppy (uae_u32 *len, uae_u8 *dstptr);
*
*   FUNCTION
*       Write the serial shifter, bit offset, DMA flags and horizontal position
*       to the supplied or allocated destination. Requires SAVESTATE.
*
*   INPUTS
*       len - Byte or bit count, or optional serialized-length output, according
*       to the signature.
*       dstptr - Optional caller-provided serialization buffer.
*
*   RESULT
*       Pointer to the record buffer, allocated internally when dstptr is NULL.
*
***************************************************************************
*
*/

uae_u8 *save_floppy (uae_u32 *len, uae_u8 *dstptr)
{
    uae_u8 *dstbak, *dst;

    /* flush dma buffer before saving */
#if 0
    dodmafetch ();
#endif
    if (dstptr)
	dstbak = dst = dstptr;
    else
	dstbak = dst = malloc (2 + 1 + 1 + 1 + 1 + 2);
    save_u16 (word);		/* current fifo (low word) */
    save_u8 (bitoffset);	/* dma bit offset */
    save_u8 (dma_enable);	/* disk sync found */
    save_u8 (disk_hpos >> 8);	/* next bit read position */
    save_u8 (dskdmaen);		/* dma status */
    save_u16 (0);		/* was current fifo (high word), but it was wrong???? */

    *len = dst - dstbak;
    return dstbak;
}

#endif /* SAVESTATE */
