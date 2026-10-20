 /*
  * UAE - The Un*x Amiga Emulator
  *
  * CD32 Akiko emulation
  *
  * - C2P
  * - NVRAM
  * - CDROM
  *
  * Copyright 2001, 2002 Toni Wilen
  *
  */

#include "sysconfig.h"

#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "events.h"
#include "savestate.h"
#include "blkdev.h"
#include "zfile.h"
#include "threaddep/thread.h"
#include "akiko.h"
#include "gui.h"
#include "sleep.h"
#include "cdrom.h"

#define AKIKO_DEBUG_NVRAM 0
#define AKIKO_DEBUG_IO 0
#define AKIKO_DEBUG_IO_CMD 0

int cd32_enabled;

//static int m68k_getpc(void) { return 0; }

/*
 * CD32 1Kb NVRAM (EEPROM) emulation
 *
 * NVRAM chip is 24C08 CMOS EEPROM (1024x8 bits = 1Kb)
 * Chip interface is I2C (2 wire serial)
 * Akiko addresses used:
 * 0xb80030: bit 7 = SCL (clock), 6 = SDA (data)
 * 0xb80032: 0xb80030 data direction register (0 = input, 1 = output)
 *
 * Because I don't have any experience on I2C, following code may be
 * unnecessarily complex and not 100% correct..
 */

enum i2c { I2C_WAIT, I2C_START, I2C_DEVICEADDR, I2C_WORDADDR, I2C_DATA };

/* size of EEPROM, don't try to change,
 * (hardcoded in Kickstart)
 */
#define NVRAM_SIZE 1024
/* max size of one write request */
#define NVRAM_PAGE_SIZE 16

static uae_u8 cd32_nvram[NVRAM_SIZE], nvram_writetmp[NVRAM_PAGE_SIZE];
static int nvram_address, nvram_writeaddr;
static int nvram_rw;
static int bitcounter = -1, direction = -1;
static uae_u8 nvram_byte;
static int scl_out, scl_in, scl_dir, oscl, sda_out, sda_in, sda_dir, osda;
static int sda_dir_nvram;
static int state = I2C_WAIT;

/****i* januae-akiko/nvram_write
 *
 *   NAME
 *       nvram_write()
 *
 *   SYNOPSIS
 *       static void nvram_write (unsigned int offset, unsigned int len)
 *
 *   FUNCTION
 *       Persist a slice of the emulated CD32 EEPROM to the configured flash file.
 *
 *   INPUTS
 *       offset -- file and EEPROM byte offset.
 *       len -- number of bytes to write.
 *
 *   NOTES
 *       If the file does not exist, creates it and first writes the full 1024-byte
 *       image. File-open and write failures are not reported.
 *
 ****************************************************************************/

static void nvram_write (unsigned int offset, unsigned int len)
{
    struct zfile *f = zfile_fopen (currprefs.flashfile, "rb+");
    if (!f) {
	f = zfile_fopen (currprefs.flashfile, "wb");
	if (!f) return;
	zfile_fwrite (cd32_nvram, NVRAM_SIZE, 1, f);
    }
    zfile_fseek (f, offset, SEEK_SET);
    zfile_fwrite (cd32_nvram + offset, len, 1, f);
    zfile_fclose (f);
}

/****i* januae-akiko/nvram_read
 *
 *   NAME
 *       nvram_read()
 *
 *   SYNOPSIS
 *       static void nvram_read (void)
 *
 *   FUNCTION
 *       Load the configured flash file into the 1024-byte emulated EEPROM.
 *
 *   NOTES
 *       The EEPROM is zero-filled before reading. A missing or unreadable file leaves
 *       zeroed data; short reads are not reported.
 *
 ****************************************************************************/

static void nvram_read (void)
{
    struct zfile *f;

    f = zfile_fopen (currprefs.flashfile, "rb");
    memset (cd32_nvram, 0, NVRAM_SIZE);
    if (!f) return;
    zfile_fread (cd32_nvram, NVRAM_SIZE, 1, f);
    zfile_fclose (f);
}

/****i* januae-akiko/i2c_do
 *
 *   NAME
 *       i2c_do()
 *
 *   SYNOPSIS
 *       static void i2c_do (void)
 *
 *   FUNCTION
 *       Advance the emulated EEPROM serial protocol on clock and data transitions.
 *
 *   NOTES
 *       Recognizes START and STOP sequences, device and word addresses, data
 *       transfers and ACK; a completed write stores the containing 16-byte page.
 *
 ****************************************************************************/

static void i2c_do (void)
{
#if AKIKO_DEBUG_NVRAM
    int i;
#endif
    sda_in = 1;
    if (!sda_dir_nvram && scl_out && oscl) {
	if (!sda_out && osda) { /* START-condition? */
	    state = I2C_DEVICEADDR;
	    bitcounter = 0;
	    direction = -1;
#if AKIKO_DEBUG_NVRAM
	    write_log ("START\n");
#endif
	    return;
	} else if(sda_out && !osda) { /* STOP-condition? */
	    state = I2C_WAIT;
	    bitcounter = -1;
#if AKIKO_DEBUG_NVRAM
	    write_log ("STOP\n");
#endif
	    if (direction > 0) {
		memcpy (cd32_nvram + (nvram_address & ~(NVRAM_PAGE_SIZE - 1)), nvram_writetmp, NVRAM_PAGE_SIZE);
		nvram_write (nvram_address & ~(NVRAM_PAGE_SIZE - 1), NVRAM_PAGE_SIZE);
		direction = -1;
#if AKIKO_DEBUG_NVRAM
		write_log ("NVRAM write address %04.4X:", nvram_address & ~(NVRAM_PAGE_SIZE - 1));
		for (i = 0; i < NVRAM_PAGE_SIZE; i++)
		    write_log ("%02.2X", nvram_writetmp[i]);
		write_log ("\n");

#endif
	    }
	    return;
	}
    }
    if (bitcounter >= 0) {
	if (direction) {
	    /* Amiga -> NVRAM */
	    if (scl_out && !oscl) {
		if (bitcounter == 8) {
#if AKIKO_DEBUG_NVRAM
		    write_log ("RB %02.2X ", nvram_byte, m68k_getpc());
#endif
		    sda_in = 0; /* ACK */
		    if (direction > 0) {
			nvram_writetmp[nvram_writeaddr++] = nvram_byte;
			nvram_writeaddr &= 15;
			bitcounter = 0;
		    } else {
			bitcounter = -1;
		    }
		} else {
		    //write_log("NVRAM received bit %d, offset %d\n", sda_out, bitcounter);
		    nvram_byte <<= 1;
		    nvram_byte |= sda_out;
		    bitcounter++;
		}
	    }
	} else {
	    /* NVRAM -> Amiga */
	    if (scl_out && !oscl && bitcounter < 8) {
		if (bitcounter == 0)
		    nvram_byte = cd32_nvram[nvram_address];
		sda_dir_nvram = 1;
		sda_in = (nvram_byte & 0x80) ? 1 : 0;
		//write_log("NVRAM sent bit %d, offset %d\n", sda_in, bitcounter);
		nvram_byte <<= 1;
		bitcounter++;
		if (bitcounter == 8) {
#if AKIKO_DEBUG_NVRAM
		    write_log ("NVRAM sent byte %02.2X address %04.4X PC=%08.8X\n", cd32_nvram[nvram_address], nvram_address, m68k_getpc());
#endif
		    nvram_address++;
		    nvram_address &= NVRAM_SIZE - 1;
		    sda_dir_nvram = 0;
		}
	    }
	    if(!sda_out && sda_dir && !scl_out) /* ACK from Amiga */
		bitcounter = 0;
	}
	if (bitcounter >= 0) return;
    }
    switch (state)
    {
	case I2C_DEVICEADDR:
	if ((nvram_byte & 0xf0) != 0xa0) {
	    write_log ("WARNING: I2C_DEVICEADDR: device address != 0xA0\n");
	    state = I2C_WAIT;
	    return;
	}
	nvram_rw = (nvram_byte & 1) ? 0 : 1;
	if (nvram_rw) {
	    /* 2 high address bits, only fetched if WRITE = 1 */
	    nvram_address &= 0xff;
	    nvram_address |= ((nvram_byte >> 1) & 3) << 8;
	    state = I2C_WORDADDR;
	    direction = -1;
	} else {
	    state = I2C_DATA;
	    direction = 0;
	    sda_dir_nvram = 1;
	}
	bitcounter = 0;
#if AKIKO_DEBUG_NVRAM
	write_log ("I2C_DEVICEADDR: rw %d, address %02.2Xxx PC=%08.8X\n", nvram_rw, nvram_address >> 8, m68k_getpc());
#endif
	break;
	case I2C_WORDADDR:
	nvram_address &= 0x300;
	nvram_address |= nvram_byte;
#if AKIKO_DEBUG_NVRAM
	write_log ("I2C_WORDADDR: address %04.4X PC=%08.8X\n", nvram_address, m68k_getpc());
#endif
	if (direction < 0) {
	    memcpy (nvram_writetmp, cd32_nvram + (nvram_address & ~(NVRAM_PAGE_SIZE - 1)), NVRAM_PAGE_SIZE);
	    nvram_writeaddr = nvram_address & (NVRAM_PAGE_SIZE - 1);
	}
	state = I2C_DATA;
	bitcounter = 0;
	direction = 1;
	break;
    }
}

/****i* januae-akiko/akiko_nvram_write
 *
 *   NAME
 *       akiko_nvram_write()
 *
 *   SYNOPSIS
 *       static void akiko_nvram_write (int offset, uae_u32 v)
 *
 *   FUNCTION
 *       Update the emulated SCL/SDA signals or their direction register.
 *
 *   INPUTS
 *       offset -- 0 for signal levels; 2 for direction bits.
 *       v -- register byte; bits 7 and 6 carry SCL and SDA.
 *
 *   NOTES
 *       Signal changes invoke the EEPROM protocol state machine. Other offsets have
 *       no effect.
 *
 ****************************************************************************/

static void akiko_nvram_write (int offset, uae_u32 v)
{
    int sda;
    switch (offset)
    {
	case 0:
	oscl = scl_out;
	scl_out = (v & 0x80) ? 1 : 0;
	osda = sda_out;
	sda_out = (v & 0x40) ? 1 : 0;
	break;
	case 2:
	scl_dir = (v & 0x80) ? 1 : 0;
	sda_dir = (v & 0x40) ? 1 : 0;
	break;
	default:
	return;
    }
    sda = sda_out;
    if (oscl != scl_out || osda != sda) {
	i2c_do ();
	oscl = scl_out;
	osda = sda;
    }
}

/****i* januae-akiko/akiko_nvram_read
 *
 *   NAME
 *       akiko_nvram_read()
 *
 *   SYNOPSIS
 *       static uae_u32 akiko_nvram_read (int offset)
 *
 *   FUNCTION
 *       Read the emulated EEPROM serial data or direction bits.
 *
 *   INPUTS
 *       offset -- 0 for SCL/SDA levels; 2 for direction bits.
 *
 *   RESULT
 *       Bit 7 represents SCL and bit 6 represents SDA; other bits are clear.
 *
 *   NOTES
 *       Reads of an unsupported offset return zero.
 *
 ****************************************************************************/

static uae_u32 akiko_nvram_read (int offset)
{
    uae_u32 v = 0;
    switch (offset)
    {
	case 0:
	if (!scl_dir)
	    v |= scl_in ? 0x80 : 0x00;
	    else
	    v |= scl_out ? 0x80 : 0x00;
	if (!sda_dir)
	    v |= sda_in ? 0x40 : 0x00;
	    else
	    v |= sda_out ? 0x40 : 0x00;
	break;
	case 2:
	v |= scl_dir ? 0x80 : 0x00;
	v |= sda_dir ? 0x40 : 0x00;
	break;
    }
    return v;
}

/* CD32 Chunky to Planar hardware emulation
 * Akiko addresses used:
 * 0xb80038-0xb8003b
 */

static uae_u32 akiko_buffer[8];
static int akiko_read_offset, akiko_write_offset;
static uae_u32 akiko_result[8];

/****i* januae-akiko/akiko_c2p_do
 *
 *   NAME
 *       akiko_c2p_do()
 *
 *   SYNOPSIS
 *       static void akiko_c2p_do (void)
 *
 *   FUNCTION
 *       Transpose eight 32-bit chunky input words into eight bitplane output words.
 *
 *   NOTES
 *       Replaces all eight entries of akiko_result; input words are read in reverse
 *       array order.
 *
 ****************************************************************************/

static void akiko_c2p_do (void)
{
    int i;

    for (i = 0; i < 8; i++) akiko_result[i] = 0;
    /* FIXME: better c2p algoritm than this piece of crap.... */
    for (i = 0; i < 8 * 32; i++) {
	if (akiko_buffer[7 - (i >> 5)] & (1 << (i & 31)))
	    akiko_result[i & 7] |= 1 << (i >> 3);
    }
}

/****i* januae-akiko/akiko_c2p_write
 *
 *   NAME
 *       akiko_c2p_write()
 *
 *   SYNOPSIS
 *       static void akiko_c2p_write (int offset, uae_u32 v)
 *
 *   FUNCTION
 *       Store a byte in the emulated Akiko chunky-to-planar input FIFO.
 *
 *   INPUTS
 *       offset -- byte offset within a 32-bit word, from 0 through 3.
 *       v -- byte value to insert.
 *
 *   NOTES
 *       Writing offset 3 clears the current input word; offset 0 advances its
 *       eight-word ring index and every write resets the output index.
 *
 ****************************************************************************/

static void akiko_c2p_write (int offset, uae_u32 v)
{
    if (offset == 3) akiko_buffer[akiko_write_offset] = 0;
    akiko_buffer[akiko_write_offset] |= v << ( 8 * (3 - offset));
    if (offset == 0) {
	akiko_write_offset++;
	akiko_write_offset &= 7;
    }
    akiko_read_offset = 0;
}

/****i* januae-akiko/akiko_c2p_read
 *
 *   NAME
 *       akiko_c2p_read()
 *
 *   SYNOPSIS
 *       static uae_u32 akiko_c2p_read (int offset)
 *
 *   FUNCTION
 *       Read the selected byte from the chunky-to-planar conversion output.
 *
 *   INPUTS
 *       offset -- byte offset within a 32-bit word, from 0 through 3.
 *
 *   RESULT
 *       The shifted conversion word; the caller uses its low byte.
 *
 *   NOTES
 *       The first read at offset 3 triggers conversion. Offset 0 advances the
 *       eight-word read index and resets the write index.
 *
 ****************************************************************************/

static uae_u32 akiko_c2p_read (int offset)
{
    uae_u32 v;

    if (akiko_read_offset == 0 && offset == 3)
	akiko_c2p_do ();
    akiko_write_offset = 0;
    v = akiko_result[akiko_read_offset];
    if (offset == 0) {
	akiko_read_offset++;
	akiko_read_offset &= 7;
    }
    return v >> (8 * (3 - offset));
}

/* CD32 CDROM hardware emulation
 * Akiko addresses used:
 * 0xb80004-0xb80028
 *
 * I can't believe cd.device and custom loaders are fooled to think
 * this piece of crap emulates real CD32 CDROM controller and drive :)
 */

#define CDSTATUS_FRAME		    0x80000000
#define CDSTATUS_DATA_AVAILABLE	    0x10000000
#define CDSTATUS_DATASECTOR_ERROR   0x08000000 /* ?? */
#define CDSTATUS_DATASECTOR	    0x04000000
#define CDSTATUS_UNKNOWN	    0x02000000

#define CDS_ERROR 0x80
#define CDS_PLAYING 0x08

static uae_u32 cdrom_status1, cdrom_status2;
static uae_u8 cdrom_status3;
static uae_u32 cdrom_address1, cdrom_address2;
static uae_u32 cdrom_longmask;
static uae_u32 cdrom_readmask_r, cdrom_readmask_w;
static uae_u8 cdrom_command_offset_complete; /* 0x19 */
static uae_u8 cdrom_command_offset_todo; /* 0x1d */
static uae_u8 cdrom_result_complete; /* 0x1a */
static uae_u8 cdrom_result_last_pos; /* 0x1f */
static uae_u8 cdrom_result_buffer[32];
static uae_u8 cdrom_command_buffer[32];
static uae_u8 cdrom_command;

#define MAX_TOC_ENTRIES 103 /* tracks 1-99, A0,A1 and A2 */
static int cdrom_toc_entries;
static int cdrom_toc_counter;
static uae_u8 cdrom_toc_buffer[MAX_TOC_ENTRIES*13];

static int cdrom_disk, cdrom_paused, cdrom_playing;
static int cdrom_command_active;
static int cdrom_command_length;
static int cdrom_checksum_error;
static int cdrom_data_offset, cdrom_speed, cdrom_sector_counter;
static int cdrom_current_sector;
static int cdrom_data_end, cdrom_leadout;
static int cdrom_dosomething;

static uae_u8 *sector_buffer_1, *sector_buffer_2;
static int sector_buffer_sector_1, sector_buffer_sector_2;
#define SECTOR_BUFFER_SIZE 64
static uae_u8 *sector_buffer_info_1, *sector_buffer_info_2;

static int unitnum = -1;

/****i* januae-akiko/frombcd
 *
 *   NAME
 *       frombcd()
 *
 *   SYNOPSIS
 *       static uae_u8 frombcd (uae_u8 v)
 *
 *   FUNCTION
 *       Decode a packed two-digit BCD byte into an integer.
 *
 *   INPUTS
 *       v -- packed BCD input.
 *
 *   RESULT
 *       Ten times its high nibble plus its low nibble.
 *
 *   NOTES
 *       No BCD digit validation is performed.
 *
 ****************************************************************************/

static uae_u8 frombcd (uae_u8 v)
{
    return (v >> 4) * 10 + (v & 15);
}

/****i* januae-akiko/tobcd
 *
 *   NAME
 *       tobcd()
 *
 *   SYNOPSIS
 *       static uae_u8 tobcd (uae_u8 v)
 *
 *   FUNCTION
 *       Encode an integer as a packed two-digit BCD byte.
 *
 *   INPUTS
 *       v -- value to encode.
 *
 *   RESULT
 *       Packed quotient by ten and remainder as high and low nibbles.
 *
 *   NOTES
 *       Values outside two decimal digits are not rejected.
 *
 ****************************************************************************/

static uae_u8 tobcd (uae_u8 v)
{
    return ((v / 10) << 4) | (v % 10);
}

/****i* januae-akiko/fromlongbcd
 *
 *   NAME
 *       fromlongbcd()
 *
 *   SYNOPSIS
 *       static int fromlongbcd (uae_u8 *p)
 *
 *   FUNCTION
 *       Decode three consecutive BCD bytes into a packed minute/second/frame integer.
 *
 *   INPUTS
 *       p -- pointer to three BCD-encoded bytes.
 *
 *   RESULT
 *       The three decoded decimal values in bits 16, 8 and 0.
 *
 ****************************************************************************/

static int fromlongbcd (uae_u8 *p)
{
    return (frombcd (p[0]) << 16) | (frombcd (p[1]) << 8) | (frombcd (p[2])  << 0);
}

/* convert minutes, seconds and frames -> logical sector number */
/****i* januae-akiko/msf2lsn
 *
 *   NAME
 *       msf2lsn()
 *
 *   SYNOPSIS
 *       static int msf2lsn (int msf)
 *
 *   FUNCTION
 *       Convert a packed minute/second/frame position into a logical CD sector
 *       number.
 *
 *   INPUTS
 *       msf -- minute, second and frame fields in bits 16, 8 and 0.
 *
 *   RESULT
 *       Frame position at 75 frames per second minus the 150-frame lead-in, clamped
 *       to zero.
 *
 ****************************************************************************/

static int msf2lsn (int msf)
{
    int sector = (((msf >> 16) & 0xff) * 60 * 75 + ((msf >> 8) & 0xff) * 75 + ((msf >> 0) & 0xff)) - 150;
    if (sector < 0)
	sector = 0;
    return sector;
}

/* convert logical sector number -> minutes, seconds and frames */
/****i* januae-akiko/lsn2msf
 *
 *   NAME
 *       lsn2msf()
 *
 *   SYNOPSIS
 *       static int lsn2msf (int sectors)
 *
 *   FUNCTION
 *       Convert a logical CD sector number into a packed minute/second/frame
 *       position.
 *
 *   INPUTS
 *       sectors -- logical sector index.
 *
 *   RESULT
 *       Minute, second and frame fields with the 150-frame lead-in included.
 *
 ****************************************************************************/

static int lsn2msf (int sectors)
{
    int msf;
    sectors += 150;
    msf = (sectors / (75 * 60)) << 16;
    msf |= ((sectors / 75) % 60) << 8;
    msf |= (sectors % 75) << 0;
    return msf;
}

/****i* januae-akiko/cdaudiostop
 *
 *   NAME
 *       cdaudiostop()
 *
 *   SYNOPSIS
 *       static void cdaudiostop (void)
 *
 *   FUNCTION
 *       Stop emulated CD audio playback and clear playback and pause flags.
 *
 *   NOTES
 *       When a device unit is selected, sends pause, stop and pause commands to the
 *       CD backend.
 *
 ****************************************************************************/

static void cdaudiostop (void)
{
    cdrom_playing = 0;
    cdrom_paused = 0;
    if (unitnum < 0)
	return;
    sys_command_pause (DF_IOCTL, unitnum, 0);
    sys_command_stop (DF_IOCTL, unitnum);
    sys_command_pause (DF_IOCTL, unitnum, 1);
}

static uae_u32 last_play_end;
/****i* januae-akiko/cd_play_audio
 *
 *   NAME
 *       cd_play_audio()
 *
 *   SYNOPSIS
 *       static int cd_play_audio (uae_u32 startmsf, uae_u32 endmsf, int scan)
 *
 *   FUNCTION
 *       Request CD audio playback through the selected drive.
 *
 *   INPUTS
 *       startmsf -- packed start position.
 *       endmsf -- packed end position, or 0xffffffff to reuse the last end.
 *       scan -- scan mode forwarded to the backend.
 *
 *   RESULT
 *       The result of sys_command_play().
 *
 *   NOTES
 *       An explicit end position becomes the saved end for the next playback request.
 *
 ****************************************************************************/

static int cd_play_audio (uae_u32 startmsf, uae_u32 endmsf, int scan)
{
    if (endmsf == 0xffffffff)
	endmsf = last_play_end;
    else
	last_play_end = endmsf;
    return sys_command_play (DF_IOCTL, unitnum,startmsf, endmsf, scan);
}


/* read qcode */
static uae_u32 last_play_pos;
/****i* januae-akiko/cd_qcode
 *
 *   NAME
 *       cd_qcode()
 *
 *   SYNOPSIS
 *       static int cd_qcode (uae_u8 *d)
 *
 *   FUNCTION
 *       Translate the backend subchannel-Q status into the Akiko result layout.
 *
 *   INPUTS
 *       d -- writable buffer for 11 bytes, or NULL to update the saved playback
 *       position only.
 *
 *   RESULT
 *       Always zero.
 *
 *   NOTES
 *       Clears the supplied buffer before querying the backend. Unsupported or
 *       unavailable status leaves it zero; successful status populates BCD
 *       track/index/position fields.
 *
 ****************************************************************************/

static int cd_qcode (uae_u8 *d)
{
    const uae_u8 *buf, *s;
    uae_u8 as;

    if (d)
	memset (d, 0, 11);
    last_play_pos = 0;
    buf = sys_command_qcode (DF_IOCTL, unitnum);
    if (!buf)
	return 0;
    as = buf[1];
    if (as != 0x11 && as != 0x12 && as != 0x13 && as != 0x15) /* audio status ok? */
	return 0;
    s = buf + 4;
    last_play_pos = (s[9] << 16) | (s[10] << 8) | (s[11] << 0);
    if (!d)
	return 0;
    /* ??? */
    d[0] = 0;
    /* CtlAdr */
    d[1] = (s[1] >> 4) | (s[1] << 4);
    /* Track */
    d[2] = tobcd (s[2]);
    /* Index */
    d[3] = tobcd (s[3]);
    /* TrackPos */
    d[4] = tobcd (s[9]);
    d[5] = tobcd (s[10]);
    d[6] = tobcd (s[11]);
    /* DiskPos */
    d[7] = 0;
    d[8] = tobcd (s[5]);
    d[9] = tobcd (s[6]);
    d[10] = tobcd (s[7]);
    if (as == 0x15) {
	/* Make sure end of disc position is passed.
	 */
	int lsn = msf2lsn ((s[5] << 16) | (s[6] << 8) | (s[7] << 0));
	int msf = lsn2msf (cdrom_leadout);
	if (lsn >= cdrom_leadout || cdrom_leadout - lsn < 10) {
	    d[8] = tobcd ((uae_u8)(msf >> 16));
	    d[9] = tobcd ((uae_u8)(msf >> 8));
	    d[10] = tobcd ((uae_u8)(msf >> 0));
	}
    }

    return 0;
}

/* read toc */
/****i* januae-akiko/cdrom_toc
 *
 *   NAME
 *       cdrom_toc()
 *
 *   SYNOPSIS
 *       static int cdrom_toc (void)
 *
 *   FUNCTION
 *       Load the selected CD drive table of contents into Akiko TOC entries.
 *
 *   RESULT
 *       Zero on success; one if the backend provides no TOC; minus one if the entry
 *       count exceeds the fixed table.
 *
 *   NOTES
 *       Derives the data-track end and lead-out from available track records; the
 *       table can hold at most 103 entries.
 *
 ****************************************************************************/

static int cdrom_toc (void)
{
    int i, j;
    int datatrack = 0, secondtrack = 0;
    uae_u8 *d;
    const uae_u8 *buf, *s;

    cdrom_toc_counter = -1;
    cdrom_toc_entries = 0;
    buf = sys_command_toc (DF_IOCTL, unitnum);
    if (!buf)
	return 1;
    i = (buf[0] << 8) | (buf[1] << 0);
    i -= 2;
    i /= 11;
    if (i > MAX_TOC_ENTRIES)
	return -1;
    memset (cdrom_toc_buffer, 0, MAX_TOC_ENTRIES * 13);
    cdrom_data_end = -1;
    for (j = 0; j < i; j++) {
	s = buf + 4 + j * 11;
	d = &cdrom_toc_buffer[j * 13];
	d[1] = (s[1] >> 4) | (s[1] << 4);
	d[3] = s[3] < 100 ? tobcd(s[3]) : s[3];
	d[8] = tobcd (s[8]);
	d[9] = tobcd (s[9]);
	d[10] = tobcd (s[10]);
	if (s[3] == 1 && (s[1] & 0x0f) == 0x04)
	    datatrack = 1;
	if (s[3] == 2)
	    secondtrack = msf2lsn ((s[8] << 16) | (s[9] << 8) | (s[10] << 0));
	if (s[3] == 0xa2)
	    cdrom_leadout = msf2lsn ((s[8] << 16) | (s[9] << 8) | (s[10] << 0));
    }
    if (datatrack) {
	if (secondtrack)
	    cdrom_data_end = secondtrack;
	else
	    cdrom_data_end = cdrom_leadout;
    }
    cdrom_toc_entries = i;
    return 0;
}

/* open device */
/****i* januae-akiko/sys_cddev_open
 *
 *   NAME
 *       sys_cddev_open()
 *
 *   SYNOPSIS
 *       static int sys_cddev_open (void)
 *
 *   FUNCTION
 *       Choose and open the preferred CD-ROM device unit for Akiko emulation.
 *
 *   RESULT
 *       Zero when device information is obtained; one if no candidate or device
 *       information is available.
 *
 *   NOTES
 *       Prefers a recognized CD32/CDTV data disc, otherwise an audio disc or the
 *       first CD-ROM unit. A failed final reopen is logged but not by itself treated
 *       as failure.
 *
 ****************************************************************************/

static int sys_cddev_open (void)
{
    int first = -1;
    struct device_info di1, *di2;
    int cd32unit = -1;
    int audiounit = -1;

    for (unitnum = 0; unitnum < MAX_TOTAL_DEVICES; unitnum++) {
	if (sys_command_open (DF_IOCTL, unitnum)) {
	    di2 = sys_command_info (DF_IOCTL, unitnum, &di1);
	    if (di2 && di2->type == INQ_ROMD) {
		write_log ("%s: ", di2->label);
		if (first < 0)
		    first = unitnum;
		if (!cdrom_toc ()) {
		    if (cdrom_data_end > 0) {
			const uae_u8 *p = sys_command_read (DF_IOCTL, unitnum, 16);
			if (p) {
			    if (!memcmp (p + 8, "CDTV", 4) || !memcmp (p + 8, "CD32", 4)) {
				write_log ("CD32 or CDTV\n");
				if (cd32unit < 0)
				    cd32unit = unitnum;
			    } else {
				write_log ("non CD32/CDTV data CD\n");
			    }
			} else {
			    write_log ("read error\n");
			}
		    } else {
			write_log ("Audio CD\n");
			if (audiounit < 0)
			    audiounit = unitnum;
		    }
		} else {
		    write_log ("can't read TOC\n");
		}
	    }
	    sys_command_close (DF_IOCTL, unitnum);
	}
    }
    unitnum = audiounit;
    if (cd32unit >= 0)
	unitnum = cd32unit;
    if (unitnum < 0)
	unitnum = first;
    if (unitnum < 0)
	return 1;
    if (!sys_command_open (DF_IOCTL, unitnum))
	write_log ("re-opening unit %d failed!\n", unitnum);
    di2 = sys_command_info (DF_IOCTL, unitnum, &di1);
    if (!di2) {
	write_log ("unit %d info failed\n", unitnum);
	sys_command_close (DF_IOCTL, unitnum);
	return 1;
    }
    write_log ("using drive %s (unit %d, media %d)\n", di2->label, unitnum, di2->media_inserted);
    /* make sure CD audio is not playing */
    cdaudiostop ();
    return 0;
}

/* close device */
/****i* januae-akiko/sys_cddev_close
 *
 *   NAME
 *       sys_cddev_close()
 *
 *   SYNOPSIS
 *       static void sys_cddev_close (void)
 *
 *   FUNCTION
 *       Stop audio and close the selected CD-ROM backend unit.
 *
 ****************************************************************************/

static void sys_cddev_close (void)
{
    cdaudiostop ();
    sys_command_close (DF_IOCTL, unitnum);
}

static int command_lengths[] = { 1,2,1,1,12,2,1,1,4,1,-1,-1,-1,-1,-1 };

/****i* januae-akiko/cdrom_return_data
 *
 *   NAME
 *       cdrom_return_data()
 *
 *   SYNOPSIS
 *       static void cdrom_return_data (int len)
 *
 *   FUNCTION
 *       Write a command result and its checksum into the emulated result ring.
 *
 *   INPUTS
 *       len -- number of result bytes to write.
 *
 *   NOTES
 *       Ignores nonpositive lengths. Advances the eight-bit completion offset and
 *       sets the data-available status flag.
 *
 ****************************************************************************/

static void cdrom_return_data (int len)
{
    uae_u32 cmd_buf = cdrom_address2;
    int i;
    uae_u8 checksum;

    if (len <= 0) return;
#if AKIKO_DEBUG_IO_CMD
    write_log ("OUT:");
#endif
    checksum = 0xff;
    for (i = 0; i < len; i++) {
	checksum -= cdrom_result_buffer[i];
	put_byte (cmd_buf + ((cdrom_result_complete + i) & 0xff), cdrom_result_buffer[i]);
#if AKIKO_DEBUG_IO_CMD
	write_log ("%02.2X ", cdrom_result_buffer[i]);
#endif
    }
    put_byte (cmd_buf + ((cdrom_result_complete + len) & 0xff), checksum);
#if AKIKO_DEBUG_IO_CMD
    write_log ("%02.2X\n", checksum);
#endif
    cdrom_result_complete += len + 1;
    cdrom_status1 |= CDSTATUS_DATA_AVAILABLE;
}

/****i* januae-akiko/cdrom_command_something
 *
 *   NAME
 *       cdrom_command_something()
 *
 *   SYNOPSIS
 *       static int cdrom_command_something (void)
 *
 *   FUNCTION
 *       Handle the unimplemented Akiko CD-ROM command placeholder.
 *
 *   RESULT
 *       Zero; no result bytes are produced.
 *
 *   NOTES
 *       The caller separately sets the flag used to emit successive TOC entries.
 *
 ****************************************************************************/

static int cdrom_command_something (void)
{
    return 0;
}

/****i* januae-akiko/cdrom_command_media_status
 *
 *   NAME
 *       cdrom_command_media_status()
 *
 *   SYNOPSIS
 *       static int cdrom_command_media_status (void)
 *
 *   FUNCTION
 *       Place the selected drive media-present status into the Akiko result buffer.
 *
 *   RESULT
 *       Two result bytes.
 *
 *   NOTES
 *       Queries the backend device information without checking for a NULL result.
 *
 ****************************************************************************/

static int cdrom_command_media_status (void)
{
    struct device_info di;

    cdrom_result_buffer[0] = 10;
    cdrom_result_buffer[1] = sys_command_info (DF_IOCTL, unitnum, &di)->media_inserted ?  1 : 0;
    return 2;
}

/* check if cd drive door is open or closed */
/****i* januae-akiko/cdrom_command_door_status
 *
 *   NAME
 *       cdrom_command_door_status()
 *
 *   SYNOPSIS
 *       static int cdrom_command_door_status (void)
 *
 *   FUNCTION
 *       Build the Akiko disc-present result and refresh the drive TOC.
 *
 *   RESULT
 *       Twenty result bytes.
 *
 *   NOTES
 *       Updates cdrom_disk from the selected unit state; unit information is
 *       dereferenced without a NULL check.
 *
 ****************************************************************************/

static int cdrom_command_door_status (void)
{
    struct device_info di;
    if (unitnum >= 0 && !sys_command_info (DF_IOCTL, unitnum, &di)->media_inserted) {
	cdrom_result_buffer[1] = 0x80;
	cdrom_disk = 0;
    } else {
	cdrom_result_buffer[1] = 1;
	cdrom_disk = 1;
    }
    if (unitnum >= 0)
	cdrom_toc ();
    cdrom_result_buffer[0] = cdrom_command;
    return 20;
}

/* return one TOC entry */
/****i* januae-akiko/cdrom_return_toc_entry
 *
 *   NAME
 *       cdrom_return_toc_entry()
 *
 *   SYNOPSIS
 *       static int cdrom_return_toc_entry (void)
 *
 *   FUNCTION
 *       Copy the current 13-byte TOC entry into an Akiko command result.
 *
 *   RESULT
 *       Fifteen result bytes.
 *
 *   NOTES
 *       Without TOC entries, reports CDS_ERROR. Otherwise advances the entry index
 *       and wraps at the entry count.
 *
 ****************************************************************************/

static int cdrom_return_toc_entry (void)
{
    cdrom_result_buffer[0] = 6;
    if (cdrom_toc_entries == 0) {
	cdrom_result_buffer[1] = CDS_ERROR;
	return 15;
    }
    cdrom_result_buffer[1] = 0;
    memcpy (cdrom_result_buffer + 2, cdrom_toc_buffer + cdrom_toc_counter * 13, 13);
    cdrom_toc_counter++;
    if (cdrom_toc_counter >= cdrom_toc_entries)
	cdrom_toc_counter = 0;
    return 15;
}

/* pause CD audio */
/****i* januae-akiko/cdrom_command_pause
 *
 *   NAME
 *       cdrom_command_pause()
 *
 *   SYNOPSIS
 *       static int cdrom_command_pause (void)
 *
 *   FUNCTION
 *       Pause CD audio playback if it is active and not already paused.
 *
 *   RESULT
 *       Two result bytes.
 *
 *   NOTES
 *       Records the previous playback state and resets TOC iteration.
 *
 ****************************************************************************/

static int cdrom_command_pause (void)
{
    cdrom_toc_counter = -1;
    cdrom_result_buffer[0] = cdrom_command;
    cdrom_result_buffer[1] = cdrom_playing ? CDS_PLAYING : 0;
    if (!cdrom_playing)
	return 2;
    if (cdrom_paused)
	return 2;
    sys_command_pause (DF_IOCTL, unitnum,1);
    cdrom_paused = 1;
    return 2;
}

/* unpause CD audio */
/****i* januae-akiko/cdrom_command_unpause
 *
 *   NAME
 *       cdrom_command_unpause()
 *
 *   SYNOPSIS
 *       static int cdrom_command_unpause (void)
 *
 *   FUNCTION
 *       Resume CD audio playback only when it is currently paused and active.
 *
 *   RESULT
 *       Two result bytes.
 *
 ****************************************************************************/

static int cdrom_command_unpause (void)
{
    cdrom_result_buffer[0] = cdrom_command;
    cdrom_result_buffer[1] = cdrom_playing ? CDS_PLAYING : 0;
    if (!cdrom_paused)
	return 2;
    if (!cdrom_playing)
	return 2;
    cdrom_paused = 0;
    sys_command_pause (DF_IOCTL, unitnum,0);
    return 2;
}

/* seek	head/play CD audio/read	data sectors */
/****i* januae-akiko/cdrom_command_multi
 *
 *   NAME
 *       cdrom_command_multi()
 *
 *   SYNOPSIS
 *       static int cdrom_command_multi (void)
 *
 *   FUNCTION
 *       Handle the combined CD seek, data-read and audio-play command.
 *
 *   RESULT
 *       Two result bytes.
 *
 *   NOTES
 *       Decodes BCD positions, selects playback or sector transfer, and reports
 *       missing media. Audio playback errors are represented in the result flags.
 *
 ****************************************************************************/

static int cdrom_command_multi (void)
{
    int seekpos = fromlongbcd (cdrom_command_buffer + 1);
    int endpos = fromlongbcd (cdrom_command_buffer + 4);

    if (cdrom_playing)
	cdaudiostop ();
    cdrom_speed = (cdrom_command_buffer[8] & 0x40) ? 2 : 1;
    cdrom_result_buffer[0] = cdrom_command;
    cdrom_result_buffer[1] = 0;
    if (!cdrom_disk) {
	cdrom_result_buffer[1] |= CDS_ERROR;
	return 2;
    }

    if (cdrom_command_buffer[7] == 0x80) {    /* data read */
	int cdrom_data_offset_end = msf2lsn (endpos);
	cdrom_data_offset = msf2lsn (seekpos);
#if AKIKO_DEBUG_IO_CMD
	write_log ("READ DATA FROM %06.6X (%d) TO %06.6X (%d) SPEED=%dx\n", seekpos, cdrom_data_offset, endpos, cdrom_data_offset_end, cdrom_speed);
#endif
	cdrom_result_buffer[1] |= 0x02;
    } else if (cdrom_command_buffer[10] & 4) { /* play audio */
	int scan = 0;
	if (cdrom_command_buffer[7] & 0x04)
	    scan = 1;
	else if (cdrom_command_buffer[7] & 0x08)
	    scan = -1;
#if AKIKO_DEBUG_IO_CMD
	write_log ("PLAY FROM %06.6X to %06.6X SCAN=%d\n", seekpos, endpos, scan);
#endif
	if (!cd_play_audio (seekpos, endpos, 0)) {
	    cdrom_result_buffer[1] = CDS_ERROR;
	} else {
	    cdrom_playing = 1;
	    cdrom_result_buffer[1] |= CDS_PLAYING;
	}
    } else {
#if AKIKO_DEBUG_IO_CMD
	write_log ("SEEKTO %06.6X\n",seekpos);
#endif
	if (seekpos < 150)
	    cdrom_toc_counter = 0;
	else
	    cdrom_toc_counter = -1;
    }
    return 2;
}

/* return subq entry */
/****i* januae-akiko/cdrom_command_subq
 *
 *   NAME
 *       cdrom_command_subq()
 *
 *   SYNOPSIS
 *       static int cdrom_command_subq (void)
 *
 *   FUNCTION
 *       Prepare an Akiko CD subchannel-Q command response.
 *
 *   RESULT
 *       Fifteen result bytes.
 *
 *   NOTES
 *       Delegates to cd_qcode(), which always returns zero; a missing backend status
 *       is not reflected as an error by this command.
 *
 ****************************************************************************/

static int cdrom_command_subq (void)
{
    cdrom_result_buffer[0] = cdrom_command;
    cdrom_result_buffer[1] = 0;
    if (cd_qcode (cdrom_result_buffer + 2))
	cdrom_result_buffer[1] = CDS_ERROR;
    return 15;
}

/****i* januae-akiko/cdrom_run_command
 *
 *   NAME
 *       cdrom_run_command()
 *
 *   SYNOPSIS
 *       static void cdrom_run_command (void)
 *
 *   FUNCTION
 *       Fetch a pending command and checksum from the emulated CD command ring.
 *
 *   NOTES
 *       Reads the command only when no other command is active; unknown command
 *       lengths default to one byte. Records checksum failure for later reporting.
 *
 ****************************************************************************/

static void cdrom_run_command (void)
{
    uae_u32 cmd_buf = cdrom_address2 + 0x200;
    int i, cmd_len;
    uae_u8 checksum;

    for (;;) {
	if (cdrom_command_active)
	    return;
	if (cdrom_command_offset_complete == cdrom_command_offset_todo)
	    return;
	cdrom_command = get_byte (cmd_buf + cdrom_command_offset_complete);
	if ((cdrom_command & 0xf0) == 0)
	    return;
	cdrom_checksum_error = 0;
	cmd_len = command_lengths[cdrom_command & 0x0f];
	if (cmd_len < 0) {
#if AKIKO_DEBUG_IO_CMD
	    write_log ("unknown command\n");
#endif
	    cmd_len = 1;
	}
#if AKIKO_DEBUG_IO_CMD
	write_log ("IN:");
#endif
	checksum = 0;
	for (i = 0; i < cmd_len + 1; i++) {
	    cdrom_command_buffer[i] = get_byte (cmd_buf + ((cdrom_command_offset_complete + i) & 0xff));
	    checksum += cdrom_command_buffer[i];
#if AKIKO_DEBUG_IO_CMD
	    write_log ("%02.2X ", cdrom_command_buffer[i]);
#endif
	}
	if (checksum!=0xff) {
#if AKIKO_DEBUG_IO_CMD
	    write_log (" checksum error");
#endif
	    cdrom_checksum_error = 1;
	}
#if AKIKO_DEBUG_IO_CMD
	write_log ("\n");
#endif
	cdrom_command_active = 1;
	cdrom_command_length = cmd_len;
	return;
    }
}

/****i* januae-akiko/cdrom_run_command_run
 *
 *   NAME
 *       cdrom_run_command_run()
 *
 *   SYNOPSIS
 *       static void cdrom_run_command_run (void)
 *
 *   FUNCTION
 *       Execute the active Akiko CD command and, when applicable, write its result.
 *
 *   NOTES
 *       Dispatches the low command nibble. Unsupported commands produce no result;
 *       checksum failure sets the error bit on results that are produced.
 *
 ****************************************************************************/

static void cdrom_run_command_run (void)
{
    int len;

    cdrom_command_offset_complete = (cdrom_command_offset_complete + cdrom_command_length + 1) & 0xff;
    memset (cdrom_result_buffer, 0, sizeof(cdrom_result_buffer));
    switch (cdrom_command & 0x0f)
    {
	case 2:
	len = cdrom_command_pause ();
	break;
	case 3:
	len = cdrom_command_unpause ();
	break;
	case 4:
	len = cdrom_command_multi ();
	break;
	case 5:
	cdrom_dosomething = 1;
	len = cdrom_command_something ();
	break;
	case 6:
	len = cdrom_command_subq ();
	break;
	case 7:
	len = cdrom_command_door_status ();
	break;
	default:
	len = 0;
	break;
    }
    if (len == 0)
	return;
    if (cdrom_checksum_error)
	cdrom_result_buffer[1] |= 0x80;
    cdrom_return_data (len);
}

static uae_sem_t akiko_sem;

/* DMA transfer one CD sector */
/****i* januae-akiko/cdrom_run_read
 *
 *   NAME
 *       cdrom_run_read()
 *
 *   SYNOPSIS
 *       static void cdrom_run_read (void)
 *
 *   FUNCTION
 *       Transfer one buffered CD data sector into emulated DMA memory when requested.
 *
 *   NOTES
 *       Requires enabled transfer control, a writable mask slot and a valid sector
 *       offset. Uses the Akiko semaphore when examining the shared sector cache.
 *
 ****************************************************************************/

static void cdrom_run_read (void)
{
    int i, j, sector;
    int read = 0;
    uae_u8 buf[2352];
    int sec;

    if (!(cdrom_longmask & 0x04000000))
	return;
    if (!cdrom_readmask_w)
	return;
    if (cdrom_data_offset<0)
	return;
    j = cdrom_sector_counter & 15;
    if (unitnum >= 0 && cdrom_readmask_w & (1 << j)) {
	uae_sem_wait (&akiko_sem);
	sector = cdrom_current_sector = cdrom_data_offset + cdrom_sector_counter;
	sec = sector - sector_buffer_sector_1;
	if (sector_buffer_sector_1 >= 0 && sec >= 0 && sec < SECTOR_BUFFER_SIZE) {
	    if (sector_buffer_info_1[sec] != 0xff && sector_buffer_info_1[sec] != 0) {
		memcpy (buf + 16, sector_buffer_1 + sec * 2048, 2048);
		encode_l2 (buf, sector + 150);
		buf[0] = 0;
		buf[1] = 0;
		buf[2] = 0;
		buf[3] = cdrom_sector_counter;
		for (i = 0; i < 2352; i++)
		    put_byte (cdrom_address1 + j * 4096 + i, buf[i]);
		cdrom_readmask_r |= 1 << j;
	    }
	    if (sector_buffer_info_1[sec] != 0xff)
		sector_buffer_info_1[sec]--;
	} else {
	    uae_sem_post (&akiko_sem);
	    return;
	}
        uae_sem_post (&akiko_sem);

#if AKIKO_DEBUG_IO_CMD
	write_log ("read sector=%d, scnt=%d -> %d\n", cdrom_data_offset, cdrom_sector_counter, sector);
#endif
	cdrom_readmask_w &= ~(1 << j);
    }
    cdrom_sector_counter++;
    if (cdrom_readmask_w == 0)
	cdrom_status1 |= CDSTATUS_DATASECTOR;
}



/****i* januae-akiko/akiko_handler
 *
 *   NAME
 *       akiko_handler()
 *
 *   SYNOPSIS
 *       static void akiko_handler (void)
 *
 *   FUNCTION
 *       Poll the selected CD drive for media changes and pending TOC results.
 *
 *   NOTES
 *       Returns when the previous command result remains pending; the media-state
 *       poll interval is counted in handler invocations.
 *
 ****************************************************************************/

static void akiko_handler (void)
{
    static int mediacheckcnt;
    static int lastmediastate = -1;
    struct device_info di;

    if (unitnum < 0)
	return;
    if (cdrom_result_complete > cdrom_result_last_pos && cdrom_result_complete - cdrom_result_last_pos < 100) {
	cdrom_status1 |= CDSTATUS_DATA_AVAILABLE;
	return;
    }
    if (cdrom_result_last_pos < cdrom_result_complete)
	return;
    if (mediacheckcnt > 0)
	mediacheckcnt--;
    if (mediacheckcnt == 0) {
	int media = sys_command_info (DF_IOCTL, unitnum, &di)->media_inserted;
	mediacheckcnt = 312 * 50 * 2;
	if (media != lastmediastate) {
	    write_log ("media changed = %d\n", media);
	    lastmediastate = cdrom_disk = media;
	    cdrom_return_data (cdrom_command_media_status ());
	    cdrom_toc ();
	     /* do not remove! first try may fail */
	    cdrom_toc ();
	    return;
	}
    }
    if (cdrom_toc_counter >= 0 && !cdrom_command_active && cdrom_dosomething) {
	cdrom_return_data (cdrom_return_toc_entry ());
	cdrom_dosomething--;
	return;
    }
}

/****i* januae-akiko/akiko_internal
 *
 *   NAME
 *       akiko_internal()
 *
 *   SYNOPSIS
 *       static void akiko_internal (void)
 *
 *   FUNCTION
 *       Advance pending Akiko CD command acquisition and dispatch.
 *
 *   NOTES
 *       Decrements the active-command delay and dispatches it when the delay reaches
 *       zero.
 *
 ****************************************************************************/

static void akiko_internal (void)
{
    cdrom_run_command ();
    if (cdrom_command_active > 0) {
	cdrom_command_active--;
	if (!cdrom_command_active)
	    cdrom_run_command_run ();
    }
}

extern int cd32_enabled;

/****i* januae-akiko/AKIKO_hsync_handler
 *
 *   NAME
 *       AKIKO_hsync_handler()
 *
 *   SYNOPSIS
 *       void AKIKO_hsync_handler (void)
 *
 *   FUNCTION
 *       Service the emulated CD32 Akiko hardware from horizontal synchronization.
 *
 *   NOTES
 *       Does nothing unless cd32_enabled is set. Periodically advances CD sector DMA
 *       and the frame status flag, then services commands and media status.
 *
 ****************************************************************************/

void AKIKO_hsync_handler (void)
{
    static int framecounter;

    if (!cd32_enabled)
	return;
    framecounter--;
    if (framecounter <= 0) {
	if (cdrom_playing || cdrom_toc_counter > 0)
	    gui_cd_led (1);
	cdrom_run_read ();
	framecounter = 1000000 / (74 * 75 * cdrom_speed);
	cdrom_status1 |= CDSTATUS_FRAME;
    }
    akiko_internal ();
    akiko_handler ();
}


static volatile int akiko_thread_running;

/* cdrom data buffering thread */
/****i* januae-akiko/akiko_thread
 *
 *   NAME
 *       akiko_thread()
 *
 *   SYNOPSIS
 *       static void *akiko_thread (void *null)
 *
 *   FUNCTION
 *       Populate the rotating CD sector cache in a background CD-backend thread.
 *
 *   INPUTS
 *       null -- unused thread argument.
 *
 *   RESULT
 *       NULL when the worker exits.
 *
 *   NOTES
 *       Opens and closes the thread-specific CD interface. Swaps cached sector
 *       buffers under akiko_sem and sets akiko_thread_running to -1 on exit.
 *
 ****************************************************************************/

static void *akiko_thread (void *null)
{
    int i;
    uae_u8 *tmp1;
    uae_u8 *tmp2;
    int tmp3;
    int offset;
    int sector;

    sys_command_open_thread (DF_IOCTL, unitnum);
    while(akiko_thread_running) {
	uae_sem_wait (&akiko_sem);
	sector = cdrom_current_sector;
	for (i = 0; i < SECTOR_BUFFER_SIZE; i++) {
	    if (sector_buffer_info_1[i] == 0xff) break;
	}
	if (cdrom_data_end > 0 && sector >= 0 && (sector_buffer_sector_1 < 0 || sector < sector_buffer_sector_1 || sector >= sector_buffer_sector_1 + SECTOR_BUFFER_SIZE * 2 / 3 || i != SECTOR_BUFFER_SIZE)) {

	    uae_sem_post (&akiko_sem);

	    memset (sector_buffer_info_2, 0, SECTOR_BUFFER_SIZE);
#if AKIKO_DEBUG_IO_CMD
	    write_log("filling buffer sector=%d (max=%d)\n", sector, cdrom_data_end);
#endif
	    sector_buffer_sector_2 = sector;
	    offset = 0;
	    while (offset < SECTOR_BUFFER_SIZE) {
		const uae_u8 *p = 0;
		if (sector < cdrom_data_end)
		    p = sys_command_read (DF_IOCTL, unitnum, sector);
		if (p)
		    memcpy (sector_buffer_2 + offset * 2048, p, 2048);
		sector_buffer_info_2[offset] = p ? 3 : 0;
		offset++;
		sector++;
	    }

	    uae_sem_wait (&akiko_sem);

	    tmp1 = sector_buffer_info_1;
	    sector_buffer_info_1 = sector_buffer_info_2;
	    sector_buffer_info_2 = tmp1;
	    tmp2 = sector_buffer_1;
	    sector_buffer_1 = sector_buffer_2;
	    sector_buffer_2 = tmp2;
	    tmp3 = sector_buffer_sector_1;
	    sector_buffer_sector_1 = sector_buffer_sector_2;
	    sector_buffer_sector_2 = tmp3;
	}
	uae_sem_post (&akiko_sem);
	uae_msleep (10);
    }
    sys_command_close_thread (DF_IOCTL, unitnum);
    akiko_thread_running = -1;
    return 0;
}

/****i* januae-akiko/akiko_get_long
 *
 *   NAME
 *       akiko_get_long()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u8 akiko_get_long (uae_u32 v, int offset)
 *
 *   FUNCTION
 *       Extract a selected big-endian byte from a 32-bit Akiko register.
 *
 *   INPUTS
 *       v -- 32-bit register value.
 *       offset -- byte index 0 through 3.
 *
 *   RESULT
 *       Selected byte.
 *
 ****************************************************************************/

STATIC_INLINE uae_u8 akiko_get_long (uae_u32 v, int offset)
{
    return v >> ((3 - offset) * 8);
}

/****i* januae-akiko/akiko_put_long
 *
 *   NAME
 *       akiko_put_long()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void akiko_put_long (uae_u32 *p, int offset, int v)
 *
 *   FUNCTION
 *       Replace a selected big-endian byte in a 32-bit Akiko register.
 *
 *   INPUTS
 *       p -- writable register word.
 *       offset -- byte index 0 through 3.
 *       v -- new register-byte value.
 *
 ****************************************************************************/

STATIC_INLINE void akiko_put_long (uae_u32 *p, int offset, int v)
{
    *p &= ~(0xff << ((3 - offset) * 8));
    *p |= v << ((3 - offset) * 8);
}

/****i* januae-akiko/akiko_bget2
 *
 *   NAME
 *       akiko_bget2()
 *
 *   SYNOPSIS
 *       STATIC_INLINE uae_u32 akiko_bget2 (uaecptr addr, int msg)
 *
 *   FUNCTION
 *       Read an emulated Akiko register and advance pending CD commands.
 *
 *   INPUTS
 *       addr -- memory-mapped Akiko address; only its low 16 bits are decoded.
 *       msg -- whether register-read debugging may be emitted.
 *
 *   RESULT
 *       Selected register value, or zero for unrecognized offsets.
 *
 *   NOTES
 *       Handles controller state, command pointers, EEPROM lines and chunky-to-planar
 *       output.
 *
 ****************************************************************************/

STATIC_INLINE uae_u32 akiko_bget2 (uaecptr addr, int msg)
{
    uae_u8 v;

    addr &= 0xffff;

    switch (addr)
    {
	/* "CAFE" = Akiko identification.
	 * Kickstart ignores Akiko C2P if this ID isn't correct */
	case 0x02:
	v = 0xCA;
	break;
	case 0x03:
	v = 0xFE;
	break;

	/* CDROM control */
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07:
	v = akiko_get_long (cdrom_status1, addr - 0x04);
	break;
	case 0x08:
	case 0x09:
	case 0x0a:
	case 0x0b:
	v = akiko_get_long (cdrom_status2, addr - 0x08);
	break;
	case 0x10:
	case 0x11:
	case 0x12:
	case 0x13:
	v = akiko_get_long (cdrom_address1, addr - 0x10);
	break;
	case 0x14:
	case 0x15:
	case 0x16:
	case 0x17:
	v = akiko_get_long (cdrom_address2, addr - 0x14);
	break;
	case 0x18:
	v = cdrom_status3;
	break;
	case 0x19:
	v = cdrom_command_offset_complete;
	break;
	case 0x1a:
	v = cdrom_result_complete;
	break;
	case 0x1f:
	v = cdrom_result_last_pos;
	break;
	case 0x20:
	case 0x21:
	v = akiko_get_long (cdrom_readmask_r, addr - 0x20 + 2);
	break;
	case 0x24:
	case 0x25:
	case 0x26:
	case 0x27:
	v = akiko_get_long (cdrom_longmask, addr - 0x24);
	break;

	/* NVRAM */
	case 0x30:
	case 0x31:
	case 0x32:
	case 0x33:
	v =  akiko_nvram_read (addr - 0x30);
	break;

	/* C2P */
	case 0x38:
	case 0x39:
	case 0x3a:
	case 0x3b:
	v = akiko_c2p_read (addr - 0x38);
	break;

	default:
	write_log ("akiko_bget: unknown address %08.8X\n", addr);
	v = 0;
	break;
    }
    akiko_internal ();

    if (msg && addr < 0x30 && AKIKO_DEBUG_IO)
	write_log ("akiko_bget %08.8X: %08.8X %02.2X\n", m68k_getpc (&regs), addr, v & 0xff);

    return v;
}

/****i* januae-akiko/akiko_bget
 *
 *   NAME
 *       akiko_bget()
 *
 *   SYNOPSIS
 *       uae_u32 REGPARAM2 akiko_bget (uaecptr addr)
 *
 *   FUNCTION
 *       Read one byte through the Akiko memory-bank interface.
 *
 *   INPUTS
 *       addr -- emulated register address.
 *
 *   RESULT
 *       Result of akiko_bget2() with diagnostic flag enabled.
 *
 ****************************************************************************/

uae_u32 REGPARAM2 akiko_bget (uaecptr addr)
{
    return akiko_bget2 (addr, 1);
}

/****i* januae-akiko/akiko_wget
 *
 *   NAME
 *       akiko_wget()
 *
 *   SYNOPSIS
 *       uae_u32 REGPARAM2 akiko_wget (uaecptr addr)
 *
 *   FUNCTION
 *       Read a big-endian 16-bit Akiko register value using two byte accesses.
 *
 *   INPUTS
 *       addr -- emulated register address.
 *
 *   RESULT
 *       Assembled 16-bit value.
 *
 *   NOTES
 *       Each byte access advances the internal CD command state.
 *
 ****************************************************************************/

uae_u32 REGPARAM2 akiko_wget (uaecptr addr)
{
    uae_u16 v;

    addr &= 0xffff;
    v = akiko_bget2 (addr + 1, 0);
    v |= akiko_bget2 (addr + 0, 0) << 8;

    if (addr < 0x30 && AKIKO_DEBUG_IO)
	write_log ("akiko_wget %08.8X: %08.8X %04.4X\n", m68k_getpc (&regs), addr, v & 0xffff);

    return v;
}

/****i* januae-akiko/akiko_lget
 *
 *   NAME
 *       akiko_lget()
 *
 *   SYNOPSIS
 *       uae_u32 REGPARAM2 akiko_lget (uaecptr addr)
 *
 *   FUNCTION
 *       Read a big-endian 32-bit Akiko register value using four byte accesses.
 *
 *   INPUTS
 *       addr -- emulated register address.
 *
 *   RESULT
 *       Assembled 32-bit value.
 *
 *   NOTES
 *       Each byte access advances the internal CD command state.
 *
 ****************************************************************************/

uae_u32 REGPARAM2 akiko_lget (uaecptr addr)
{
    uae_u32 v;

    addr &= 0xffff;
    v = akiko_bget2 (addr + 3, 0);
    v |= akiko_bget2 (addr + 2, 0) << 8;
    v |= akiko_bget2 (addr + 1, 0) << 16;
    v |= akiko_bget2 (addr + 0, 0) << 24;

    if (addr < 0x30 && (addr != 4 && addr != 8) && AKIKO_DEBUG_IO)
	write_log ("akiko_lget %08.8X: %08.8X %08.8X\n", m68k_getpc (&regs), addr, v);

    return v;
}

/****i* januae-akiko/akiko_bput2
 *
 *   NAME
 *       akiko_bput2()
 *
 *   SYNOPSIS
 *       STATIC_INLINE void REGPARAM2 akiko_bput2 (uaecptr addr, uae_u32 v, int msg)
 *
 *   FUNCTION
 *       Write one byte to an emulated Akiko register and advance pending CD commands.
 *
 *   INPUTS
 *       addr -- emulated register address, masked to 16 bits.
 *       v -- low byte of the value to write.
 *       msg -- whether write diagnostics may be emitted.
 *
 *   NOTES
 *       Updates CD controller flags, DMA masks, command pointers, EEPROM state or
 *       chunky-to-planar input depending on the offset.
 *
 ****************************************************************************/

STATIC_INLINE void REGPARAM2 akiko_bput2 (uaecptr addr, uae_u32 v, int msg)
{
    uae_u32 tmp;

    addr &= 0xffff;
    v &= 0xff;
    if(msg && addr < 0x30 && AKIKO_DEBUG_IO)
	write_log ("akiko_bput %08.8X: %08.8X=%02.2X\n", m68k_getpc (&regs), addr, v & 0xff);

    switch (addr)
    {
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07:
	akiko_put_long (&cdrom_status1, addr - 0x04, v);
	break;
	case 0x08:
	case 0x09:
	case 0x0a:
	case 0x0b:
	akiko_put_long (&cdrom_status2, addr - 0x08, v);
	if (addr == 8)
	    cdrom_status1 &= cdrom_status2;
	break;
	case 0x10:
	case 0x11:
	case 0x12:
	case 0x13:
	akiko_put_long (&cdrom_address1, addr - 0x10, v);
	break;
	case 0x14:
	case 0x15:
	case 0x16:
	case 0x17:
	akiko_put_long (&cdrom_address2, addr - 0x14, v);
	break;
	case 0x18:
	cdrom_status3 = v;
	break;
	case 0x19:
	cdrom_command_offset_complete = v;
	break;
	case 0x1a:
	cdrom_result_complete = v;
	break;
	case 0x1d:
	cdrom_command_offset_todo = v;
	break;
	case 0x1f:
	cdrom_result_last_pos = v;
	break;
	case 0x20:
	cdrom_readmask_w |= (v << 8);
	cdrom_readmask_r &= 0x00ff;
	break;
	case 0x21:
	cdrom_readmask_w |= (v << 0);
	cdrom_readmask_r &= 0xff00;
	break;
	case 0x24:
	case 0x25:
	case 0x26:
	case 0x27:
	tmp = cdrom_longmask;
	akiko_put_long (&cdrom_longmask, addr - 0x24, v);
	if ((cdrom_longmask & 0x04000000) && !(tmp & 0x04000000))
	    cdrom_sector_counter = 0;
	break;

	case 0x30:
	case 0x31:
	case 0x32:
	case 0x33:
	akiko_nvram_write (addr - 0x30, v);
	break;

	case 0x38:
	case 0x39:
	case 0x3a:
	case 0x3b:
	akiko_c2p_write (addr - 0x38, v);
	break;

	default:
	write_log ("akiko_bput: unknown address %08.8X\n", addr);
	break;
    }
    akiko_internal ();
}

/****i* januae-akiko/akiko_bput
 *
 *   NAME
 *       akiko_bput()
 *
 *   SYNOPSIS
 *       void REGPARAM2 akiko_bput (uaecptr addr, uae_u32 v)
 *
 *   FUNCTION
 *       Write one byte through the Akiko memory-bank interface.
 *
 *   INPUTS
 *       addr -- emulated register address.
 *       v -- value; only the low byte is used.
 *
 ****************************************************************************/

void REGPARAM2 akiko_bput (uaecptr addr, uae_u32 v)
{
    akiko_bput2 (addr, v, 1);
}

/****i* januae-akiko/akiko_wput
 *
 *   NAME
 *       akiko_wput()
 *
 *   SYNOPSIS
 *       void REGPARAM2 akiko_wput (uaecptr addr, uae_u32 v)
 *
 *   FUNCTION
 *       Write a big-endian 16-bit Akiko value through two byte accesses.
 *
 *   INPUTS
 *       addr -- emulated register address, masked to 12 bits.
 *       v -- value to write.
 *
 *   NOTES
 *       The low byte is written before the high byte; each access advances CD command
 *       processing.
 *
 ****************************************************************************/

void REGPARAM2 akiko_wput (uaecptr addr, uae_u32 v)
{
    addr &= 0xfff;

    if((addr < 0x30 && AKIKO_DEBUG_IO))
	write_log("akiko_wput %08.8X: %08.8X=%04.4X\n", m68k_getpc (&regs), addr, v & 0xffff);
    akiko_bput2 (addr + 1, v & 0xff, 0);
    akiko_bput2 (addr + 0, v >> 8, 0);
}

/****i* januae-akiko/akiko_lput
 *
 *   NAME
 *       akiko_lput()
 *
 *   SYNOPSIS
 *       void REGPARAM2 akiko_lput (uaecptr addr, uae_u32 v)
 *
 *   FUNCTION
 *       Write a big-endian 32-bit Akiko value through four byte accesses.
 *
 *   INPUTS
 *       addr -- emulated register address.
 *       v -- value to write.
 *
 *   NOTES
 *       The least significant byte is written first, with a command-state update
 *       after every byte access.
 *
 ****************************************************************************/

void REGPARAM2 akiko_lput (uaecptr addr, uae_u32 v)
{
    addr &= 0xffff;

    if(addr < 0x30 && AKIKO_DEBUG_IO)
	write_log("akiko_lput %08.8X: %08.8X=%08.8X\n", m68k_getpc (&regs), addr, v);
    akiko_bput2 (addr + 3, (v >> 0) & 0xff, 0);
    akiko_bput2 (addr + 2, (v >> 8) & 0xff, 0);
    akiko_bput2 (addr + 1, (v >> 16) & 0xff, 0);
    akiko_bput2 (addr + 0, (v >> 24) & 0xff, 0);
}

static uae_thread_id akiko_tid;


static int cdromok = 0;

/****i* januae-akiko/akiko_reset
 *
 *   NAME
 *       akiko_reset()
 *
 *   SYNOPSIS
 *       void akiko_reset (void)
 *
 *   FUNCTION
 *       Reset Akiko EEPROM protocol state and release active CD resources.
 *
 *   NOTES
 *       Stops CD audio, reloads the EEPROM image, terminates the sector worker and
 *       frees allocated sector buffers when CD support was initialized.
 *
 ****************************************************************************/

void akiko_reset (void)
{
    cdaudiostop ();
    nvram_read ();
    state = I2C_WAIT;
    bitcounter = -1;
    direction = -1;

    cdrom_speed = 1;
    cdrom_current_sector = -1;

    if (akiko_thread_running > 0) {
	akiko_thread_running = 0;
	while(akiko_thread_running == 0)
	    uae_msleep (10);
	akiko_thread_running = 0;
    }

    if (cdromok) {
	if (unitnum >= 0)
	    sys_cddev_close ();
	unitnum = -1;
	free (sector_buffer_1);
	free (sector_buffer_2);
	free (sector_buffer_info_1);
	free (sector_buffer_info_2);
	sector_buffer_1 = 0;
	sector_buffer_2 = 0;
	sector_buffer_info_1 = 0;
	sector_buffer_info_2 = 0;
	cdromok = 0;
    }
}

extern uae_u8 *extendedkickmemory;

static const uae_u8 patchdata[] = {0x0c,0x82,0x00,0x00,0x03,0xe8,0x64,0x00,0x00,0x46};

/****i* januae-akiko/patchrom
 *
 *   NAME
 *       patchrom()
 *
 *   SYNOPSIS
 *       static void patchrom (void)
 *
 *   FUNCTION
 *       Replace one extended Kickstart delay-loop instruction sequence.
 *
 *   NOTES
 *       Scans the first 512 KiB of extendedkickmemory only on its first call. Logs
 *       success or failure; later calls do not repeat the scan.
 *
 ****************************************************************************/

static void patchrom (void)
{
    static int patch_done = 0;
    unsigned int i;

    if (!patch_done) {
	uae_u8 *p;

	patch_done = 1;

	p = (uae_u8*)extendedkickmemory;

	for (i = 0; i < 524288 - sizeof (patchdata); i++) {
	    if (!memcmp (p + i, patchdata, sizeof(patchdata))) {
		p[i + 6] = 0x4e;
		p[i + 7] = 0x71;
		p[i + 8] = 0x4e;
		p[i + 9] = 0x71;
		write_log ("Extended ROM delay loop patched at 0x%p\n", i + 6 + 0xe00000);
		return;
	    }
	}
	write_log ("Couldn't patch extended ROM\n");
    }
}

/****i* januae-akiko/akiko_free
 *
 *   NAME
 *       akiko_free()
 *
 *   SYNOPSIS
 *       void akiko_free (void)
 *
 *   FUNCTION
 *       Release Akiko CD-ROM and EEPROM runtime state by invoking akiko_reset().
 *
 ****************************************************************************/

void akiko_free (void)
{
    akiko_reset ();
}

/****i* januae-akiko/akiko_init
 *
 *   NAME
 *       akiko_init()
 *
 *   SYNOPSIS
 *       int akiko_init (void)
 *
 *   FUNCTION
 *       Initialize the Akiko CD backend, sector cache, semaphore and worker.
 *
 *   RESULT
 *       Zero if device_func_init() reports no device support; otherwise one.
 *
 *   NOTES
 *       Disc detection may fail without changing the return value. Cache allocations
 *       and thread creation are not checked for success.
 *
 ****************************************************************************/

int akiko_init (void)
{
    if (cdromok == 0) {
	unitnum = -1;
	if (!device_func_init(DEVICE_TYPE_ANY)) {
	    write_log ("no CDROM support\n");
	    return 0;
	}
	if (!sys_cddev_open ()) {
	    cdromok = 1;
	    sector_buffer_1 = malloc (SECTOR_BUFFER_SIZE * 2048);
	    sector_buffer_2 = malloc (SECTOR_BUFFER_SIZE * 2048);
	    sector_buffer_info_1 = malloc (SECTOR_BUFFER_SIZE);
	    sector_buffer_info_2 = malloc (SECTOR_BUFFER_SIZE);
	    sector_buffer_sector_1 = -1;
	    sector_buffer_sector_2 = -1;
	    patchrom ();
	}
    }
    if (!savestate_state) {
	cdrom_playing = cdrom_paused = 0;
	cdrom_data_offset = -1;
	uae_sem_init (&akiko_sem, 0, 1);
    }
    if (cdromok && !akiko_thread_running) {
	akiko_thread_running = 1;
	uae_start_thread (akiko_thread, 0, &akiko_tid);
    }
    return 1;
}


#ifdef SAVESTATE

/****i* januae-akiko/save_akiko
 *
 *   NAME
 *       save_akiko()
 *
 *   SYNOPSIS
 *       uae_u8 *save_akiko (uae_u32 *len)
 *
 *   FUNCTION
 *       Serialize Akiko CD control, EEPROM signal directions, C2P and audio-playback
 *       state.
 *
 *   INPUTS
 *       len -- receives the reported serialized length.
 *
 *   RESULT
 *       Newly allocated save buffer.
 *
 *   NOTES
 *       Requires SAVESTATE. The local destination pointer is never advanced while
 *       save helpers write data, so *len is set to zero.
 *
 ****************************************************************************/

uae_u8 *save_akiko (uae_u32 *len)
{
    uae_u8 *dstbak, *dst;
    unsigned int i;

    dstbak = dst = malloc (1000);
    save_u16 (0);
    save_u16 (0xCAFE);
    save_u32 (cdrom_status1);
    save_u32 (cdrom_status2);
    save_u32 (0);
    save_u32 (cdrom_address1);
    save_u32 (cdrom_address2);
    save_u8 (cdrom_status3);
    save_u8 (cdrom_command_offset_complete);
    save_u8 (cdrom_result_complete);
    save_u8 (0);
    save_u8 (0);
    save_u8 (cdrom_command_offset_todo);
    save_u8 (0);
    save_u8 (cdrom_result_last_pos);
    save_u16 ((uae_u16)cdrom_readmask_w);
    save_u16 (0);
    save_u32 (cdrom_longmask);
    save_u32 (0);
    save_u32 (0);
    save_u32 ((scl_dir ? 0x8000 : 0) | (sda_dir ? 0x4000 : 0));
    save_u32 (0);
    save_u32 (0);

    for (i = 0; i < 8; i++)
	save_u32 (akiko_buffer[i]);
    save_u8 ((uae_u8)akiko_read_offset);
    save_u8 ((uae_u8)akiko_write_offset);

    save_u32 ((cdrom_playing ? 1 : 0) | (cdrom_paused ? 2 : 0));
    if (cdrom_playing)
	cd_qcode (0);
    save_u32 (last_play_pos);
    save_u32 (last_play_end);
    save_u8 ((uae_u8)cdrom_toc_counter);

    *len = dst - dstbak;
    return dstbak;
}

/****i* januae-akiko/restore_akiko
 *
 *   NAME
 *       restore_akiko()
 *
 *   SYNOPSIS
 *       const uae_u8 *restore_akiko (const uae_u8 *src)
 *
 *   FUNCTION
 *       Restore saved Akiko controller, C2P and CD audio playback state.
 *
 *   INPUTS
 *       src -- caller-provided source pointer, not advanced by this routine.
 *
 *   RESULT
 *       The original src pointer, unchanged.
 *
 *   NOTES
 *       Requires SAVESTATE. Reads from the save-state helper stream and may issue a
 *       CD audio play request.
 *
 ****************************************************************************/

const uae_u8 *restore_akiko (const uae_u8 *src)
{
    uae_u32 v;
    unsigned int i;

    restore_u16 ();
    restore_u16 ();
    cdrom_status1 = restore_u32 ();
    cdrom_status2 = restore_u32 ();
    restore_u32 ();
    cdrom_address1 = restore_u32 ();
    cdrom_address2 = restore_u32 ();
    cdrom_status3 = restore_u8 ();
    cdrom_command_offset_complete = restore_u8 ();
    cdrom_result_complete = restore_u8 ();
    restore_u8 ();
    restore_u8 ();
    cdrom_command_offset_todo = restore_u8 ();
    restore_u8 ();
    cdrom_result_last_pos = restore_u8 ();
    cdrom_readmask_w = restore_u16 ();
    restore_u16 ();
    cdrom_longmask = restore_u32 ();
    restore_u32 ();
    restore_u32 ();
    v = restore_u32 ();
    scl_dir = (v & 0x8000) ? 1 : 0;
    sda_dir = (v & 0x4000) ? 1 : 0;
    restore_u32 ();
    restore_u32 ();

    for (i = 0; i < 8; i++)
	akiko_buffer[i] = restore_u32 ();
    akiko_read_offset = restore_u8 ();
    akiko_write_offset = restore_u8 ();
    akiko_c2p_do ();

    cdrom_playing = cdrom_paused = 0;
    v = restore_u32 ();
    if (v & 1)
	cdrom_playing = 1;
    if (v & 2)
	cdrom_paused = 1;
    last_play_pos = restore_u32 ();
    last_play_end = restore_u32 ();
    cdrom_toc_counter = restore_u8 ();
    if (cdrom_toc_counter == 255)
	cdrom_toc_counter = -1;
    if (cdrom_playing)
	sys_command_play (DF_IOCTL, unitnum, last_play_pos, last_play_end, 0);

    return src;
}

#endif /* SAVESTATE */


/****i* januae-akiko/akiko_entergui
 *
 *   NAME
 *       akiko_entergui()
 *
 *   SYNOPSIS
 *       void akiko_entergui (void)
 *
 *   FUNCTION
 *       Pause CD audio through the backend while entering the emulator GUI.
 *
 *   NOTES
 *       The request is sent only while cdrom_playing is set; it does not update
 *       cdrom_paused.
 *
 ****************************************************************************/

void akiko_entergui (void)
{
    if (cdrom_playing)
	sys_command_pause (DF_IOCTL, unitnum, 1);
}
/****i* januae-akiko/akiko_exitgui
 *
 *   NAME
 *       akiko_exitgui()
 *
 *   SYNOPSIS
 *       void akiko_exitgui (void)
 *
 *   FUNCTION
 *       Resume CD audio through the backend when leaving the emulator GUI.
 *
 *   NOTES
 *       The request is sent only while cdrom_playing is set; it does not update
 *       cdrom_paused.
 *
 ****************************************************************************/

void akiko_exitgui (void)
{
    if (cdrom_playing)
	sys_command_pause (DF_IOCTL, unitnum, 0);
}
