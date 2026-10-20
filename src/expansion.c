 /*
  * UAE - The Un*x Amiga Emulator
  *
  * AutoConfig (tm) Expansions (ZorroII/III)
  *
  * Copyright 1996,1997 Stefan Reinauer <stepan@linux.de>
  * Copyright 1997 Brian King <Brian_King@Mitel.com>
  *   - added gfxcard code
  *
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "autoconf.h"
#include "filesys.h"
#include "xwin.h"
#include "p96.h"
#include "savestate.h"
#include "zfile.h"
#include "catweasel.h"

#define MAX_EXPANSION_BOARDS	8

/* ********************************************************** */
/* 00 / 02 */
/* er_Type */

#define Z2_MEM_8MB	0x00 /* Size of Memory Block */
#define Z2_MEM_4MB	0x07
#define Z2_MEM_2MB	0x06
#define Z2_MEM_1MB	0x05
#define Z2_MEM_512KB	0x04
#define Z2_MEM_256KB	0x03
#define Z2_MEM_128KB	0x02
#define Z2_MEM_64KB	0x01
/* extended definitions */
#define Z2_MEM_16MB	0x00
#define Z2_MEM_32MB	0x01
#define Z2_MEM_64MB	0x02
#define Z2_MEM_128MB	0x03
#define Z2_MEM_256MB	0x04
#define Z2_MEM_512MB	0x05
#define Z2_MEM_1GB	0x06

#define chainedconfig	0x08 /* Next config is part of the same card */
#define rom_card	0x10 /* ROM vector is valid */
#define add_memory	0x20 /* Link RAM into free memory list */

#define zorroII		0xc0 /* Type of Expansion Card */
#define zorroIII	0x80

/* ********************************************************** */
/* 04 - 06 & 10-16 */

/* Manufacturer */
#define commodore_g	 513 /* Commodore Braunschweig (Germany) */
#define commodore	 514 /* Commodore West Chester */
#define gvp		2017 /* GVP */
#define ass		2102 /* Advanced Systems & Software */
#define hackers_id	2011 /* Special ID for test cards */

/* Card Type */
#define commodore_a2091	     3 /* A2091 / A590 Card from C= */
#define commodore_a2091_ram 10 /* A2091 / A590 Ram on HD-Card */
#define commodore_a2232	    70 /* A2232 Multiport Expansion */
#define ass_nexus_scsi	     1 /* Nexus SCSI Controller */

#define gvp_series_2_scsi   11
#define gvp_iv_24_gfx	    32

/* ********************************************************** */
/* 08 - 0A  */
/* er_Flags */
#define Z3_MEM_64KB	0x02
#define Z3_MEM_128KB	0x03
#define Z3_MEM_256KB	0x04
#define Z3_MEM_512KB	0x05
#define Z3_MEM_1MB	0x06 /* Zorro III card subsize */
#define Z3_MEM_2MB	0x07
#define Z3_MEM_4MB	0x08
#define Z3_MEM_6MB	0x09
#define Z3_MEM_8MB	0x0a
#define Z3_MEM_10MB	0x0b
#define Z3_MEM_12MB	0x0c
#define Z3_MEM_14MB	0x0d
#define Z3_MEM_16MB	0x00
#define Z3_MEM_AUTO	0x01
#define Z3_MEM_defunct1	0x0e
#define Z3_MEM_defunct2	0x0f

#define force_z3	0x10 /* *MUST* be set if card is Z3 */
#define ext_size	0x20 /* Use extended size table for bits 0-2 of er_Type */
#define no_shutup	0x40 /* Card cannot receive Shut_up_forever */
#define care_addr	0x80 /* Adress HAS to be $200000-$9fffff */

/* ********************************************************** */
/* 40-42 */
/* ec_interrupt (unused) */

#define enable_irq	0x01 /* enable Interrupt */
#define reset_card	0x04 /* Reset of Expansion Card - must be 0 */
#define card_int2	0x10 /* READ ONLY: IRQ 2 active */
#define card_irq6	0x20 /* READ ONLY: IRQ 6 active */
#define card_irq7	0x40 /* READ ONLY: IRQ 7 active */
#define does_irq	0x80 /* READ ONLY: Card currently throws IRQ */

/* ********************************************************** */

/* ROM defines (DiagVec) */

#define rom_4bit	(0x00<<14) /* ROM width */
#define rom_8bit	(0x01<<14)
#define rom_16bit	(0x02<<14)

#define rom_never	(0x00<<12) /* Never run Boot Code */
#define rom_install	(0x01<<12) /* run code at install time */
#define rom_binddrv	(0x02<<12) /* run code with binddrivers */

uaecptr ROM_filesys_resname, ROM_filesys_resid;
uaecptr ROM_filesys_diagentry;
uaecptr ROM_hardfile_resname, ROM_hardfile_resid;
uaecptr ROM_hardfile_init;

/* ********************************************************** */

static void (*card_init[MAX_EXPANSION_BOARDS]) (void);
static void (*card_map[MAX_EXPANSION_BOARDS]) (void);

static int ecard;

/* ********************************************************** */

/* Please note: ZorroIII implementation seems to work different
 * than described in the HRM. This claims that ZorroIII config
 * address is 0xff000000 while the ZorroII config space starts
 * at 0x00e80000. In reality, both, Z2 and Z3 cards are
 * configured in the ZorroII config space. Kickstart 3.1 doesn't
 * even do a single read or write access to the ZorroIII space.
 * The original Amiga include files tell the same as the HRM.
 * ZorroIII: If you set ext_size in er_Flags and give a Z2-size
 * in er_Type you can very likely add some ZorroII address space
 * to a ZorroIII card on a real Amiga. This is not implemented
 * yet.
 *  -- Stefan
 *
 * Surprising that 0xFF000000 isn't used. Maybe it depends on the
 * ROM. Anyway, the HRM says that Z3 cards may appear in Z2 config
 * space, so what we are doing here is correct.
 *  -- Bernd
 */

/* Autoconfig address space at 0xE80000 */
static uae_u8 expamem[65536];

static uae_u8 expamem_lo;
static uae_u16 expamem_hi;

/*
 *  Dummy entries to show that there's no card in a slot
 */

/****i* januae-expansion/expamem_map_clear **********************************
*
*   NAME
*       expamem_map_clear() -- Report mapping of an absent expansion card.
*
*   SYNOPSIS
*       static void expamem_map_clear (void);
*
*   FUNCTION
*       Log an unexpected attempt to map an empty AutoConfig slot.
*
****************************************************************************
*
*/

static void expamem_map_clear (void)
{
    write_log ("expamem_map_clear() got called. Shouldn't happen.\n");
}

/****i* januae-expansion/expamem_init_clear *********************************
*
*   NAME
*       expamem_init_clear() -- Mark the AutoConfig window as empty.
*
*   SYNOPSIS
*       static void expamem_init_clear (void);
*
*   FUNCTION
*       Fill the 64 KiB expansion configuration image with 0xff.
*
****************************************************************************
*
*/

static void expamem_init_clear (void)
{
    memset (expamem, 0xff, sizeof expamem);
}
/****i* januae-expansion/expamem_init_clear2 ********************************
*
*   NAME
*       expamem_init_clear2() -- Terminate AutoConfig card enumeration.
*
*   SYNOPSIS
*       static void expamem_init_clear2 (void);
*
*   FUNCTION
*       Clear the configuration image and set the current card index to the
*       last supported slot.
*
****************************************************************************
*
*/

static void expamem_init_clear2 (void)
{
    expamem_init_clear();
    ecard = MAX_EXPANSION_BOARDS - 1;
}

static uae_u32 expamem_lget (uaecptr) REGPARAM;
static uae_u32 expamem_wget (uaecptr) REGPARAM;
static uae_u32 expamem_bget (uaecptr) REGPARAM;
static void expamem_lput (uaecptr, uae_u32) REGPARAM;
static void expamem_wput (uaecptr, uae_u32) REGPARAM;
static void expamem_bput (uaecptr, uae_u32) REGPARAM;

addrbank expamem_bank = {
    expamem_lget, expamem_wget, expamem_bget,
    expamem_lput, expamem_wput, expamem_bput,
    default_xlate, default_check, NULL
};

/****i* januae-expansion/expamem_lget ***************************************
*
*   NAME
*       expamem_lget() -- Read unsupported AutoConfig longword.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 expamem_lget (uaecptr addr);
*
*   FUNCTION
*       Log the unsupported longword read and flag a special read in JIT
*       builds.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       0xffffffff.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 expamem_lget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    write_log ("warning: READ.L from address $%lx \n", addr);
    return 0xfffffffful;
}

/****i* januae-expansion/expamem_wget ***************************************
*
*   NAME
*       expamem_wget() -- Read unsupported AutoConfig word.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 expamem_wget (uaecptr addr);
*
*   FUNCTION
*       Log the unsupported word read and flag a special read in JIT
*       builds.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       0xffff.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 expamem_wget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    write_log ("warning: READ.W from address $%lx \n", addr);
    return 0xffff;
}

/****i* januae-expansion/expamem_bget ***************************************
*
*   NAME
*       expamem_bget() -- Read an AutoConfig byte.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 expamem_bget (uaecptr addr);
*
*   FUNCTION
*       Return the byte at the low 16 bits of the guest address in the
*       AutoConfig image.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The selected configuration byte.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 expamem_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    addr &= 0xFFFF;
    return expamem[addr];
}

/****i* januae-expansion/expamem_write **************************************
*
*   NAME
*       expamem_write() -- Encode an AutoConfig register byte.
*
*   SYNOPSIS
*       static void REGPARAM2 expamem_write (uaecptr addr, uae_u32 value);
*
*   FUNCTION
*       Store a byte as high- and low-nibble fields separated by two bytes;
*       complement normal configuration fields but not the type or
*       interrupt fields.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       value - Value to write.
*
*   NOTES
*       The destination offset is masked to 16 bits; this routine does not
*       check that addr + 2 remains inside the image.
*
****************************************************************************
*
*/

static void REGPARAM2 expamem_write (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    addr &= 0xffff;
    if (addr == 00 || addr == 02 || addr == 0x40 || addr == 0x42) {
	expamem[addr] = (value & 0xf0);
	expamem[addr + 2] = (value & 0x0f) << 4;
    } else {
	expamem[addr] = ~(value & 0xf0);
	expamem[addr + 2] = ~((value & 0x0f) << 4);
    }
}

/****i* januae-expansion/expamem_type ***************************************
*
*   NAME
*       expamem_type() -- Identify the current expansion-bus type.
*
*   SYNOPSIS
*       static int REGPARAM2 expamem_type (void);
*
*   FUNCTION
*       Decode the board type from the two nibble fields at offsets 0 and
*       2.
*
*   RESULT
*       Type bits masked by 0xc0 (Zorro II or Zorro III where configured).
*
****************************************************************************
*
*/

static int REGPARAM2 expamem_type (void)
{
    return ((expamem[0] | expamem[2] >> 4) & 0xc0);
}

/****i* januae-expansion/expamem_lput ***************************************
*
*   NAME
*       expamem_lput() -- Handle an unsupported AutoConfig longword write.
*
*   SYNOPSIS
*       static void REGPARAM2 expamem_lput (uaecptr addr, uae_u32 value);
*
*   FUNCTION
*       Log the write without changing the AutoConfig registers; mark
*       special JIT memory access.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       value - Value to write.
*
****************************************************************************
*
*/

static void REGPARAM2 expamem_lput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    write_log ("warning: WRITE.L to address $%lx : value $%lx\n", addr, value);
}

/****i* januae-expansion/expamem_wput ***************************************
*
*   NAME
*       expamem_wput() -- Configure a Zorro III board address.
*
*   SYNOPSIS
*       static void REGPARAM2 expamem_wput (uaecptr addr, uae_u32 value);
*
*   FUNCTION
*       For a Zorro III write at offset 0x44, derive the high address, map
*       the current board and advance AutoConfig enumeration. Log other
*       writes when the board is not Zorro III.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       value - Value to write.
*
*   NOTES
*       This routine does not modify a Zorro III board for word offsets
*       other than 0x44.
*
****************************************************************************
*
*/

static void REGPARAM2 expamem_wput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    if (expamem_type() != zorroIII)
	write_log ("warning: WRITE.W to address $%lx : value $%x\n", addr, value);
    else {
	switch (addr & 0xff) {
	 case 0x44:
	    if (expamem_type() == zorroIII) {
		// +Bernd Roesch
		value = value - 0x3000;  // maps to 0x10000000
		chipmem_wput (regs.regs[11] + 0x20, value);
		chipmem_wput (regs.regs[11] + 0x28, value);
		// -Bernd Roesch
		expamem_hi = value;
		(*card_map[ecard]) ();
		write_log ("   Card %d (Zorro%s) done.\n", ecard + 1, expamem_type() == 0xc0 ? "II" : "III");
		++ecard;
		if (ecard < MAX_EXPANSION_BOARDS)
		    (*card_init[ecard]) ();
		else
		    expamem_init_clear2 ();
	    }
	    break;
	}
    }
}

/****i* januae-expansion/expamem_bput ***************************************
*
*   NAME
*       expamem_bput() -- Handle AutoConfig control and address bytes.
*
*   SYNOPSIS
*       static void REGPARAM2 expamem_bput (uaecptr addr, uae_u32 value);
*
*   FUNCTION
*       Process base-address bytes, board shutdown and Zorro II mapping
*       requests. Advance to the next card after mapping or shutdown;
*       remember Zorro III low address bytes.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       value - Value to write.
*
*   NOTES
*       The address is decoded using its low eight bits; unsupported
*       offsets have no effect.
*
****************************************************************************
*
*/

static void REGPARAM2 expamem_bput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    switch (addr & 0xff) {
     case 0x30:
     case 0x32:
	expamem_hi = 0;
	expamem_lo = 0;
	expamem_write (0x48, 0x00);
	break;

     case 0x48:
	if (expamem_type () == zorroII) {
	    expamem_hi = value & 0xFF;
	    (*card_map[ecard]) ();
	    write_log ("   Card %d (Zorro%s) done.\n", ecard + 1, expamem_type() == 0xc0 ? "II" : "III");
	    ++ecard;
	    if (ecard < MAX_EXPANSION_BOARDS)
		(*card_init[ecard]) ();
	    else
		expamem_init_clear2 ();
	} else if (expamem_type() == zorroIII)
	    expamem_lo = value;
	break;

     case 0x4a:
	if (expamem_type () == zorroII)
	    expamem_lo = value;
	break;

     case 0x4c:
	write_log ("   Card %d (Zorro %s) had no success.\n", ecard + 1, expamem_type() == 0xc0 ? "II" : "III");
	++ecard;
	if (ecard < MAX_EXPANSION_BOARDS)
	    (*card_init[ecard]) ();
	else
	    expamem_init_clear2 ();
	break;
    }
}

/* ********************************************************** */

/*
 *  Fast Memory
 */

static uae_u32 fastmem_mask;

static uae_u32 fastmem_lget (uaecptr) REGPARAM;
static uae_u32 fastmem_wget (uaecptr) REGPARAM;
static uae_u32 fastmem_bget (uaecptr) REGPARAM;
static void fastmem_lput (uaecptr, uae_u32) REGPARAM;
static void fastmem_wput (uaecptr, uae_u32) REGPARAM;
static void fastmem_bput (uaecptr, uae_u32) REGPARAM;
static int fastmem_check (uaecptr addr, uae_u32 size) REGPARAM;
static uae_u8 *fastmem_xlate (uaecptr addr) REGPARAM;

static uae_u32 fastmem_start; /* Determined by the OS */
static uae_u8 *fastmemory;

/****i* januae-expansion/fastmem_lget ***************************************
*
*   NAME
*       fastmem_lget() -- Read a longword from Zorro II fast RAM.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 fastmem_lget (uaecptr addr);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and read the
*       longword from the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The longword value, returned as uae_u32.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 fastmem_lget (uaecptr addr)
{
    uae_u8 *m;
    addr -= fastmem_start & fastmem_mask;
    addr &= fastmem_mask;
    m = fastmemory + addr;
    return do_get_mem_long ((uae_u32 *)m);
}

/****i* januae-expansion/fastmem_wget ***************************************
*
*   NAME
*       fastmem_wget() -- Read a word from Zorro II fast RAM.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 fastmem_wget (uaecptr addr);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and read the
*       word from the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The word value, returned as uae_u32.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 fastmem_wget (uaecptr addr)
{
    uae_u8 *m;
    addr -= fastmem_start & fastmem_mask;
    addr &= fastmem_mask;
    m = fastmemory + addr;
    return do_get_mem_word ((uae_u16 *)m);
}

/****i* januae-expansion/fastmem_bget ***************************************
*
*   NAME
*       fastmem_bget() -- Read a byte from Zorro II fast RAM.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 fastmem_bget (uaecptr addr);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and read the
*       byte from the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The byte value, returned as uae_u32.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 fastmem_bget (uaecptr addr)
{
    addr -= fastmem_start & fastmem_mask;
    addr &= fastmem_mask;
    return fastmemory[addr];
}

/****i* januae-expansion/fastmem_lput ***************************************
*
*   NAME
*       fastmem_lput() -- Write a longword to Zorro II fast RAM.
*
*   SYNOPSIS
*       void REGPARAM2 fastmem_lput (uaecptr addr, uae_u32 l);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and store the
*       longword in the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       l - Longword to write.
*
****************************************************************************
*
*/

void REGPARAM2 fastmem_lput (uaecptr addr, uae_u32 l)
{
    uae_u8 *m;
    addr -= fastmem_start & fastmem_mask;
    addr &= fastmem_mask;
    m = fastmemory + addr;
    do_put_mem_long ((uae_u32 *)m, l);
}

/****i* januae-expansion/fastmem_wput ***************************************
*
*   NAME
*       fastmem_wput() -- Write a word to Zorro II fast RAM.
*
*   SYNOPSIS
*       void REGPARAM2 fastmem_wput (uaecptr addr, uae_u32 w);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and store the
*       word in the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       w - Word to write.
*
****************************************************************************
*
*/

void REGPARAM2 fastmem_wput (uaecptr addr, uae_u32 w)
{
    uae_u8 *m;
    addr -= fastmem_start & fastmem_mask;
    addr &= fastmem_mask;
    m = fastmemory + addr;
    do_put_mem_word ((uae_u16 *)m, w);
}

/****i* januae-expansion/fastmem_bput ***************************************
*
*   NAME
*       fastmem_bput() -- Write a byte to Zorro II fast RAM.
*
*   SYNOPSIS
*       void REGPARAM2 fastmem_bput (uaecptr addr, uae_u32 b);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and store the
*       byte in the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       b - Byte to write.
*
****************************************************************************
*
*/

void REGPARAM2 fastmem_bput (uaecptr addr, uae_u32 b)
{
    addr -= fastmem_start & fastmem_mask;
    addr &= fastmem_mask;
    fastmemory[addr] = b;
}

/****i* januae-expansion/fastmem_check **************************************
*
*   NAME
*       fastmem_check() -- Check a Zorro II fast RAM range.
*
*   SYNOPSIS
*       static int REGPARAM2 fastmem_check (uaecptr addr, uae_u32 size);
*
*   FUNCTION
*       Normalize the guest address using the bank base and mask, then
*       compare the end of the requested range with allocated_fastmem.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       size - Requested extent in bytes.
*
*   RESULT
*       Nonzero when offset + size is within the allocation, otherwise
*       zero.
*
*   NOTES
*       The addition is not independently guarded against integer overflow.
*
****************************************************************************
*
*/

static int REGPARAM2 fastmem_check (uaecptr addr, uae_u32 size)
{
    addr -= fastmem_start & fastmem_mask;
    addr &= fastmem_mask;
    return (addr + size) <= allocated_fastmem;
}

/****i* januae-expansion/fastmem_xlate **************************************
*
*   NAME
*       fastmem_xlate() -- Translate a Zorro II fast RAM guest address.
*
*   SYNOPSIS
*       static uae_u8 REGPARAM2 *fastmem_xlate (uaecptr addr);
*
*   FUNCTION
*       Normalize the guest address using the bank base and mask and return
*       a direct pointer into fastmemory.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       Host pointer into the allocated bank.
*
*   NOTES
*       No independent allocation-bounds check is performed.
*
****************************************************************************
*
*/

static uae_u8 REGPARAM2 *fastmem_xlate (uaecptr addr)
{
    addr -= fastmem_start & fastmem_mask;
    addr &= fastmem_mask;
    return fastmemory + addr;
}

addrbank fastmem_bank = {
    fastmem_lget, fastmem_wget, fastmem_bget,
    fastmem_lput, fastmem_wput, fastmem_bput,
    fastmem_xlate, fastmem_check, NULL
};


#ifdef CATWEASEL

/*
 * Catweasel ZorroII
 */

static uae_u32 catweasel_lget (uaecptr) REGPARAM;
static uae_u32 catweasel_wget (uaecptr) REGPARAM;
static uae_u32 catweasel_bget (uaecptr) REGPARAM;
static void catweasel_lput (uaecptr, uae_u32) REGPARAM;
static void catweasel_wput (uaecptr, uae_u32) REGPARAM;
static void catweasel_bput (uaecptr, uae_u32) REGPARAM;
static int catweasel_check (uaecptr addr, uae_u32 size) REGPARAM;
static uae_u8 *catweasel_xlate (uaecptr addr) REGPARAM;

static uae_u32 catweasel_mask;
static uae_u32 catweasel_start;

/****i* januae-expansion/catweasel_lget *************************************
*
*   NAME
*       catweasel_lget() -- Handle a Catweasel longword read.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 catweasel_lget (uaecptr addr);
*
*   FUNCTION
*       Log the unsupported longword register access and mark a special JIT
*       read.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       Zero.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 catweasel_lget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    write_log("catweasel_lget @%08.8X!\n",addr);
    return 0;
}

/****i* januae-expansion/catweasel_wget *************************************
*
*   NAME
*       catweasel_wget() -- Handle a Catweasel word read.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 catweasel_wget (uaecptr addr);
*
*   FUNCTION
*       Log the unsupported word register access and mark a special JIT
*       read.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       Zero.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 catweasel_wget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    write_log("catweasel_wget @%08.8X!\n",addr);
    return 0;
}

/****i* januae-expansion/catweasel_bget *************************************
*
*   NAME
*       catweasel_bget() -- Read a Catweasel register byte.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 catweasel_bget (uaecptr addr);
*
*   FUNCTION
*       Normalize the address against the Catweasel base and address mask,
*       then invoke the hardware byte-read helper.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       Byte returned by catweasel_do_bget().
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 catweasel_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    addr -= catweasel_start & catweasel_mask;
    addr &= catweasel_mask;
    return catweasel_do_bget (addr);
}

/****i* januae-expansion/catweasel_lput *************************************
*
*   NAME
*       catweasel_lput() -- Handle a Catweasel longword write.
*
*   SYNOPSIS
*       static void REGPARAM2 catweasel_lput (uaecptr addr, uae_u32 l);
*
*   FUNCTION
*       Log the unsupported longword register write and mark a special JIT
*       write.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       l - Longword to write.
*
****************************************************************************
*
*/

static void REGPARAM2 catweasel_lput (uaecptr addr, uae_u32 l)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    write_log("catweasel_lput @%08.8X=%08.8X!\n",addr,l);
}

/****i* januae-expansion/catweasel_wput *************************************
*
*   NAME
*       catweasel_wput() -- Handle a Catweasel word write.
*
*   SYNOPSIS
*       static void REGPARAM2 catweasel_wput (uaecptr addr, uae_u32 w);
*
*   FUNCTION
*       Log the unsupported word register write and mark a special JIT
*       write.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       w - Word to write.
*
****************************************************************************
*
*/

static void REGPARAM2 catweasel_wput (uaecptr addr, uae_u32 w)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    write_log("catweasel_wput @%08.8X=%04.4X!\n",addr,w);
}

/****i* januae-expansion/catweasel_bput *************************************
*
*   NAME
*       catweasel_bput() -- Write a Catweasel register byte.
*
*   SYNOPSIS
*       static void REGPARAM2 catweasel_bput (uaecptr addr, uae_u32 b);
*
*   FUNCTION
*       Normalize the address against the Catweasel base and address mask,
*       then invoke the hardware byte-write helper.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       b - Byte to write.
*
****************************************************************************
*
*/

static void REGPARAM2 catweasel_bput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    addr -= catweasel_start & catweasel_mask;
    addr &= catweasel_mask;
    catweasel_do_bput (addr, b);
}

/****i* januae-expansion/catweasel_check ************************************
*
*   NAME
*       catweasel_check() -- Reject direct Catweasel memory mapping.
*
*   SYNOPSIS
*       static int REGPARAM2 catweasel_check (uaecptr addr, uae_u32 size);
*
*   FUNCTION
*       Log a requested address-range check and decline direct mapping.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       size - Requested extent in bytes.
*
*   RESULT
*       Zero for all ranges.
*
****************************************************************************
*
*/

static int REGPARAM2 catweasel_check (uaecptr addr, uae_u32 size)
{
    write_log ("catweasel_check @%08.8X size %08.8X\n", addr, size);
    return 0;
}

/****i* januae-expansion/catweasel_xlate ************************************
*
*   NAME
*       catweasel_xlate() -- Reject Catweasel pointer translation.
*
*   SYNOPSIS
*       static uae_u8 REGPARAM2 *catweasel_xlate (uaecptr addr);
*
*   FUNCTION
*       Log a pointer-translation request without returning a host mapping.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       NULL.
*
****************************************************************************
*
*/

static uae_u8 REGPARAM2 *catweasel_xlate (uaecptr addr)
{
    write_log ("catweasel_xlate @%08.8X size %08.8X\n", addr);
    return 0;
}

static addrbank catweasel_bank = {
    catweasel_lget, catweasel_wget, catweasel_bget,
    catweasel_lput, catweasel_wput, catweasel_bput,
    catweasel_xlate, catweasel_check, NULL
};

/****i* januae-expansion/expamem_map_catweasel ******************************
*
*   NAME
*       expamem_map_catweasel() -- Map the configured Catweasel address
*       bank.
*
*   SYNOPSIS
*       static void expamem_map_catweasel (void);
*
*   FUNCTION
*       Compute the board base from AutoConfig address bytes and map one 64
*       KiB bank.
*
****************************************************************************
*
*/

static void expamem_map_catweasel (void)
{
    catweasel_start = ((expamem_hi | (expamem_lo >> 4)) << 16);
    map_banks (&catweasel_bank, catweasel_start >> 16, 1, 0);
    write_log ("Catweasel MK%d: mapped @$%lx\n", cwc.type, catweasel_start);
}

/****i* januae-expansion/expamem_init_catweasel *****************************
*
*   NAME
*       expamem_init_catweasel() -- Build Catweasel AutoConfig registers.
*
*   SYNOPSIS
*       static void expamem_init_catweasel (void);
*
*   FUNCTION
*       Set the configured Catweasel product, vendor, bus size and address
*       mask according to the detected hardware type.
*
****************************************************************************
*
*/

static void expamem_init_catweasel (void)
{
    uae_u8 productid = cwc.type == CATWEASEL_TYPE_MK3 ? 66 : 200;
    uae_u16 vendorid = cwc.type == CATWEASEL_TYPE_MK3 ? 4626 : 5001;

    catweasel_mask = (cwc.type == CATWEASEL_TYPE_MK3) ? 0xffff : 0x1ffff;

    expamem_init_clear();

    expamem_write (0x00, (cwc.type == CATWEASEL_TYPE_MK3 ? Z2_MEM_64KB : Z2_MEM_128KB) | zorroII);

    expamem_write (0x04, productid);

    expamem_write (0x08, no_shutup);

    expamem_write (0x10, vendorid >> 8);
    expamem_write (0x14, vendorid & 0xff);

    expamem_write (0x18, 0x00); /* ser.no. Byte 0 */
    expamem_write (0x1c, 0x00); /* ser.no. Byte 1 */
    expamem_write (0x20, 0x00); /* ser.no. Byte 2 */
    expamem_write (0x24, 0x00); /* ser.no. Byte 3 */

    expamem_write (0x28, 0x00); /* Rom-Offset hi */
    expamem_write (0x2c, 0x00); /* ROM-Offset lo */

    expamem_write (0x40, 0x00); /* Ctrl/Statusreg.*/
}

#endif

#ifdef CDTV
/*
 * CDTV DMAC
 */

//#define CDTV_DEBUG

static uae_u32 dmac_lget (uaecptr) REGPARAM;
static uae_u32 dmac_wget (uaecptr) REGPARAM;
static uae_u32 dmac_bget (uaecptr) REGPARAM;
static void dmac_lput (uaecptr, uae_u32) REGPARAM;
static void dmac_wput (uaecptr, uae_u32) REGPARAM;
static void dmac_bput (uaecptr, uae_u32) REGPARAM;

static uae_u32 dmac_start = 0xe90000;
static uae_u8 dmacmemory[0x100];

static int cdtv_command_len;
static uae_u8 cdtv_command_buf[6];

/****i* januae-expansion/cdtv_interrupt *************************************
*
*   NAME
*       cdtv_interrupt() -- Request a CDTV interrupt.
*
*   SYNOPSIS
*       static void cdtv_interrupt (int v);
*
*   FUNCTION
*       Set status bits in the emulated DMAC register array and request
*       interrupt level 6.
*
*   INPUTS
*       v - Interrupt event code used for logging.
*
*   NOTES
*       The v argument is logged but does not alter the reported status.
*
****************************************************************************
*
*/

static void cdtv_interrupt (int v)
{
    write_log ("cdtv int %d\n", v);
    dmacmemory[0x41] = (1 << 4) | (1 << 6);
    Interrupt (6);
}

/****i* januae-expansion/dmac_lget ******************************************
*
*   NAME
*       dmac_lget() -- Read a CDTV DMAC longword.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 dmac_lget (uaecptr addr);
*
*   FUNCTION
*       Combine consecutive word reads into a longword value, following the
*       DMAC register read path.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       32-bit value.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 dmac_lget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
#ifdef CDTV_DEBUG
    write_log ("dmac_lget %08.8X\n", addr);
#endif
    return (dmac_wget (addr) << 16) | dmac_wget (addr + 2);
}

/****i* januae-expansion/dmac_wget ******************************************
*
*   NAME
*       dmac_wget() -- Read a CDTV DMAC word.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 dmac_wget (uaecptr addr);
*
*   FUNCTION
*       Combine consecutive byte reads into a word value, following the
*       DMAC register read path.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       16-bit value.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 dmac_wget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
#ifdef CDTV_DEBUG
    write_log ("dmac_wget %08.8X PC=%X\n", addr, m68k_getpc (&regs));
#endif
    return (dmac_bget (addr) << 8) | dmac_bget (addr + 1);
}

/****i* januae-expansion/dmac_bget ******************************************
*
*   NAME
*       dmac_bget() -- Read a CDTV DMAC register byte.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 dmac_bget (uaecptr addr);
*
*   FUNCTION
*       Translate the device base and return an emulated register byte,
*       except that offset 0xa3 returns one.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The selected DMAC register value or one at offset 0xa3.
*
*   NOTES
*       The register array has 256 elements, but this access masks the
*       offset to 16 bits rather than checking it against the array size.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 dmac_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
#ifdef CDTV_DEBUG
    write_log ("dmac_bget %08.8X PC=%X\n", addr, m68k_getpc (&regs));
#endif
    addr -= dmac_start;
    addr &= 65535;
    switch (addr)
    {
	case 0xa3:
	return 1;
    }
    return dmacmemory[addr];
}

/****i* januae-expansion/dmac_lput ******************************************
*
*   NAME
*       dmac_lput() -- Write a CDTV DMAC longword.
*
*   SYNOPSIS
*       static void REGPARAM2 dmac_lput (uaecptr addr, uae_u32 l);
*
*   FUNCTION
*       Split a longword into two word writes at adjacent word addresses.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       l - Longword to write.
*
****************************************************************************
*
*/

static void REGPARAM2 dmac_lput (uaecptr addr, uae_u32 l)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
#ifdef CDTV_DEBUG
    write_log ("dmac_lput %08.8X = %08.8X\n", addr, l);
#endif
    dmac_wput (addr, l >> 16);
    dmac_wput (addr + 2, l);
}

/****i* januae-expansion/dmac_wput ******************************************
*
*   NAME
*       dmac_wput() -- Write a CDTV DMAC word.
*
*   SYNOPSIS
*       static void REGPARAM2 dmac_wput (uaecptr addr, uae_u32 w);
*
*   FUNCTION
*       Issue two DMAC byte writes from the high and low halves of the
*       word.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       w - Word to write.
*
*   NOTES
*       Both byte writes use the same address; they are not issued at
*       consecutive addresses.
*
****************************************************************************
*
*/

static void REGPARAM2 dmac_wput (uaecptr addr, uae_u32 w)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
#ifdef CDTV_DEBUG
    write_log ("dmac_wput %04.4X = %04.4X\n", addr, w & 65535);
#endif
    dmac_bput (addr, w >> 8);
    dmac_bput (addr, w);
}

/****i* januae-expansion/dmac_bput ******************************************
*
*   NAME
*       dmac_bput() -- Handle a CDTV DMAC byte write.
*
*   SYNOPSIS
*       static void REGPARAM2 dmac_bput (uaecptr addr, uae_u32 b);
*
*   FUNCTION
*       Normalize the DMAC register address and mark a special write in JIT
*       builds.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       b - Byte to write.
*
*   NOTES
*       Register storage and command/interrupt handling occur only when
*       CDTV_DEBUG is defined; without that flag the byte write has no
*       device-register effect.
*
****************************************************************************
*
*/

static void REGPARAM2 dmac_bput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
#ifdef CDTV_DEBUG
    write_log ("dmac_bput %08.8X = %02.2X PC=%X\n", addr, b & 255, m68k_getpc (&regs));
#endif
    addr -= dmac_start;
    addr &= 65535;
#ifdef CDTV_DEBUG
    dmacmemory[addr] = b;
    switch (addr)
    {
	case 0xa1:
	if (cdtv_command_len >= sizeof (cdtv_command_buf))
	    cdtv_command_len = sizeof (cdtv_command_buf) - 1;
	cdtv_command_buf[cdtv_command_len++] = b;
	if (cdtv_command_len == 6) {
	    cdtv_interrupt (1);
	}
	break;
	case 0xa5:
	cdtv_command_len = 0;
	break;
	case 0xe4:
	dmacmemory[0x41] = 0;
	write_log ("cdtv interrupt cleared\n");
	activate_debugger();
	break;
    }
#endif
}

addrbank dmac_bank = {
    dmac_lget, dmac_wget, dmac_bget,
    dmac_lput, dmac_wput, dmac_bput,
    default_xlate, default_check, NULL
};

#endif

/*
 * Filesystem device ROM
 * This is very simple, the Amiga shouldn't be doing things with it.
 */

#ifdef FILESYS

static uae_u32 filesys_lget (uaecptr) REGPARAM;
static uae_u32 filesys_wget (uaecptr) REGPARAM;
static uae_u32 filesys_bget (uaecptr) REGPARAM;
static void filesys_lput (uaecptr, uae_u32) REGPARAM;
static void filesys_wput (uaecptr, uae_u32) REGPARAM;
static void filesys_bput (uaecptr, uae_u32) REGPARAM;

static uae_u32 filesys_start; /* Determined by the OS */
uae_u8 *filesysory;

/****i* januae-expansion/filesys_lget ***************************************
*
*   NAME
*       filesys_lget() -- Read a longword from the filesystem ROM.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 filesys_lget (uaecptr addr);
*
*   FUNCTION
*       Translate the address into the 64 KiB filesystem ROM image and
*       fetch the longword value; mark a special JIT read.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The longword ROM value, returned as uae_u32.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 filesys_lget (uaecptr addr)
{
    uae_u8 *m;
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    addr -= filesys_start & 65535;
    addr &= 65535;
    m = filesysory + addr;
    return do_get_mem_long ((uae_u32 *)m);
}

/****i* januae-expansion/filesys_wget ***************************************
*
*   NAME
*       filesys_wget() -- Read a word from the filesystem ROM.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 filesys_wget (uaecptr addr);
*
*   FUNCTION
*       Translate the address into the 64 KiB filesystem ROM image and
*       fetch the word value; mark a special JIT read.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The word ROM value, returned as uae_u32.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 filesys_wget (uaecptr addr)
{
    uae_u8 *m;
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    addr -= filesys_start & 65535;
    addr &= 65535;
    m = filesysory + addr;
    return do_get_mem_word ((uae_u16 *)m);
}

/****i* januae-expansion/filesys_bget ***************************************
*
*   NAME
*       filesys_bget() -- Read a byte from the filesystem ROM.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 filesys_bget (uaecptr addr);
*
*   FUNCTION
*       Translate the address into the 64 KiB filesystem ROM image and
*       fetch the byte value; mark a special JIT read.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The byte ROM value, returned as uae_u32.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 filesys_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    addr -= filesys_start & 65535;
    addr &= 65535;
    return filesysory[addr];
}

/****i* januae-expansion/filesys_lput ***************************************
*
*   NAME
*       filesys_lput() -- Reject a filesystem ROM longword write.
*
*   SYNOPSIS
*       static void REGPARAM2 filesys_lput (uaecptr addr, uae_u32 l);
*
*   FUNCTION
*       Log the attempted write, leaving the filesystem ROM unchanged.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       l - Longword to write.
*
****************************************************************************
*
*/

static void REGPARAM2 filesys_lput (uaecptr addr, uae_u32 l)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    write_log ("filesys_lput called PC=%p\n", m68k_getpc (&regs));
}

/****i* januae-expansion/filesys_wput ***************************************
*
*   NAME
*       filesys_wput() -- Reject a filesystem ROM word write.
*
*   SYNOPSIS
*       static void REGPARAM2 filesys_wput (uaecptr addr, uae_u32 w);
*
*   FUNCTION
*       Log the attempted write, leaving the filesystem ROM unchanged.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       w - Word to write.
*
****************************************************************************
*
*/

static void REGPARAM2 filesys_wput (uaecptr addr, uae_u32 w)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    write_log ("filesys_wput called PC=%p\n", m68k_getpc (&regs));
}

/****i* januae-expansion/filesys_bput ***************************************
*
*   NAME
*       filesys_bput() -- Stop after a filesystem ROM byte write.
*
*   SYNOPSIS
*       static void REGPARAM2 filesys_bput (uaecptr addr, uae_u32 b);
*
*   FUNCTION
*       Log an unexpected byte write to the filesystem ROM and request
*       emulator termination.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       b - Byte to write.
*
*   NOTES
*       This path reports the legacy Kickstart compatibility warning; it
*       does not store the byte.
*
****************************************************************************
*
*/

static void REGPARAM2 filesys_bput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
    write_log ("filesys_bput called. This usually means that you are using\n");
    write_log ("Kickstart 1.2. Please give UAE the \"-a\" option next time\n");
    write_log ("you start it. If you are _not_ using Kickstart 1.2, then\n");
    write_log ("there's a bug somewhere.\n");
    write_log ("Exiting...\n");
    uae_quit ();
}

static addrbank filesys_bank = {
    filesys_lget, filesys_wget, filesys_bget,
    filesys_lput, filesys_wput, filesys_bput,
    default_xlate, default_check, NULL
};

#endif // FILESYS

/*
 *  Z3fastmem Memory
 */

static uae_u32 z3fastmem_mask;

static uae_u32 z3fastmem_lget (uaecptr) REGPARAM;
static uae_u32 z3fastmem_wget (uaecptr) REGPARAM;
static uae_u32 z3fastmem_bget (uaecptr) REGPARAM;
static void z3fastmem_lput (uaecptr, uae_u32) REGPARAM;
static void z3fastmem_wput (uaecptr, uae_u32) REGPARAM;
static void z3fastmem_bput (uaecptr, uae_u32) REGPARAM;
static int z3fastmem_check (uaecptr addr, uae_u32 size) REGPARAM;
static uae_u8 *z3fastmem_xlate (uaecptr addr) REGPARAM;

static uae_u32 z3fastmem_start; /* Determined by the OS */
static uae_u8 *z3fastmem;

/****i* januae-expansion/z3fastmem_lget *************************************
*
*   NAME
*       z3fastmem_lget() -- Read a longword from Zorro III fast RAM.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 z3fastmem_lget (uaecptr addr);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and read the
*       longword from the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The longword value, returned as uae_u32.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 z3fastmem_lget (uaecptr addr)
{
    uae_u8 *m;
    addr -= z3fastmem_start & z3fastmem_mask;
    addr &= z3fastmem_mask;
    m = z3fastmem + addr;
    return do_get_mem_long ((uae_u32 *)m);
}

/****i* januae-expansion/z3fastmem_wget *************************************
*
*   NAME
*       z3fastmem_wget() -- Read a word from Zorro III fast RAM.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 z3fastmem_wget (uaecptr addr);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and read the
*       word from the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The word value, returned as uae_u32.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 z3fastmem_wget (uaecptr addr)
{
    uae_u8 *m;
    addr -= z3fastmem_start & z3fastmem_mask;
    addr &= z3fastmem_mask;
    m = z3fastmem + addr;
    return do_get_mem_word ((uae_u16 *)m);
}

/****i* januae-expansion/z3fastmem_bget *************************************
*
*   NAME
*       z3fastmem_bget() -- Read a byte from Zorro III fast RAM.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 z3fastmem_bget (uaecptr addr);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and read the
*       byte from the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       The byte value, returned as uae_u32.
*
****************************************************************************
*
*/

uae_u32 REGPARAM2 z3fastmem_bget (uaecptr addr)
{
    addr -= z3fastmem_start & z3fastmem_mask;
    addr &= z3fastmem_mask;
    return z3fastmem[addr];
}

/****i* januae-expansion/z3fastmem_lput *************************************
*
*   NAME
*       z3fastmem_lput() -- Write a longword to Zorro III fast RAM.
*
*   SYNOPSIS
*       void REGPARAM2 z3fastmem_lput (uaecptr addr, uae_u32 l);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and store the
*       longword in the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       l - Longword to write.
*
****************************************************************************
*
*/

void REGPARAM2 z3fastmem_lput (uaecptr addr, uae_u32 l)
{
    uae_u8 *m;
    addr -= z3fastmem_start & z3fastmem_mask;
    addr &= z3fastmem_mask;
    m = z3fastmem + addr;
    do_put_mem_long ((uae_u32 *)m, l);
}

/****i* januae-expansion/z3fastmem_wput *************************************
*
*   NAME
*       z3fastmem_wput() -- Write a word to Zorro III fast RAM.
*
*   SYNOPSIS
*       void REGPARAM2 z3fastmem_wput (uaecptr addr, uae_u32 w);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and store the
*       word in the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       w - Word to write.
*
****************************************************************************
*
*/

void REGPARAM2 z3fastmem_wput (uaecptr addr, uae_u32 w)
{
    uae_u8 *m;
    addr -= z3fastmem_start & z3fastmem_mask;
    addr &= z3fastmem_mask;
    m = z3fastmem + addr;
    do_put_mem_word ((uae_u16 *)m, w);
}

/****i* januae-expansion/z3fastmem_bput *************************************
*
*   NAME
*       z3fastmem_bput() -- Write a byte to Zorro III fast RAM.
*
*   SYNOPSIS
*       void REGPARAM2 z3fastmem_bput (uaecptr addr, uae_u32 b);
*
*   FUNCTION
*       Subtract the configured bank base, mask the address and store the
*       byte in the allocated bank.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       b - Byte to write.
*
****************************************************************************
*
*/

void REGPARAM2 z3fastmem_bput (uaecptr addr, uae_u32 b)
{
    addr -= z3fastmem_start & z3fastmem_mask;
    addr &= z3fastmem_mask;
    z3fastmem[addr] = b;
}

/****i* januae-expansion/z3fastmem_check ************************************
*
*   NAME
*       z3fastmem_check() -- Check a Zorro III fast RAM range.
*
*   SYNOPSIS
*       static int REGPARAM2 z3fastmem_check (uaecptr addr, uae_u32 size);
*
*   FUNCTION
*       Normalize the guest address using the bank base and mask, then
*       compare the end of the requested range with allocated_z3fastmem.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*       size - Requested extent in bytes.
*
*   RESULT
*       Nonzero when offset + size is within the allocation, otherwise
*       zero.
*
*   NOTES
*       The addition is not independently guarded against integer overflow.
*
****************************************************************************
*
*/

static int REGPARAM2 z3fastmem_check (uaecptr addr, uae_u32 size)
{
    addr -= z3fastmem_start & z3fastmem_mask;
    addr &= z3fastmem_mask;
    return (addr + size) <= allocated_z3fastmem;
}

/****i* januae-expansion/z3fastmem_xlate ************************************
*
*   NAME
*       z3fastmem_xlate() -- Translate a Zorro III fast RAM guest address.
*
*   SYNOPSIS
*       static uae_u8 REGPARAM2 *z3fastmem_xlate (uaecptr addr);
*
*   FUNCTION
*       Normalize the guest address using the bank base and mask and return
*       a direct pointer into z3fastmem.
*
*   INPUTS
*       addr - Guest address of the memory operation.
*
*   RESULT
*       Host pointer into the allocated bank.
*
*   NOTES
*       No independent allocation-bounds check is performed.
*
****************************************************************************
*
*/

static uae_u8 REGPARAM2 *z3fastmem_xlate (uaecptr addr)
{
    addr -= z3fastmem_start & z3fastmem_mask;
    addr &= z3fastmem_mask;
    return z3fastmem + addr;
}

addrbank z3fastmem_bank = {
    z3fastmem_lget, z3fastmem_wget, z3fastmem_bget,
    z3fastmem_lput, z3fastmem_wput, z3fastmem_bput,
    z3fastmem_xlate, z3fastmem_check, NULL
};

/* Z3-based UAEGFX-card */
uae_u32 gfxmem_mask; /* for memory.c */
uae_u8 *gfxmemory;
uae_u32 gfxmem_start;

/* ********************************************************** */

/*
 *     Expansion Card (ZORRO II) for 1/2/4/8 MB of Fast Memory
 */

/****i* januae-expansion/expamem_map_fastcard *******************************
*
*   NAME
*       expamem_map_fastcard() -- Map Zorro II fast memory.
*
*   SYNOPSIS
*       static void expamem_map_fastcard (void);
*
*   FUNCTION
*       Compute the card base from the configuration-address bytes and map
*       the allocated fast-memory banks there.
*
****************************************************************************
*
*/

static void expamem_map_fastcard (void)
{
    fastmem_start = ((expamem_hi | (expamem_lo >> 4)) << 16);
    map_banks (&fastmem_bank, fastmem_start >> 16, allocated_fastmem >> 16, allocated_fastmem);
    write_log ("Fastcard: mapped @$%lx: %dMB fast memory\n", fastmem_start, allocated_fastmem >> 20);
}

/****i* januae-expansion/expamem_init_fastcard ******************************
*
*   NAME
*       expamem_init_fastcard() -- Advertise a Zorro II fast-memory card.
*
*   SYNOPSIS
*       static void expamem_init_fastcard (void);
*
*   FUNCTION
*       Populate the AutoConfig image for the allocated 1, 2, 4 or 8 MiB
*       fast-memory board.
*
****************************************************************************
*
*/

static void expamem_init_fastcard (void)
{
    expamem_init_clear();
    if (allocated_fastmem == 0x100000)
	expamem_write (0x00, Z2_MEM_1MB + add_memory + zorroII);
    else if (allocated_fastmem == 0x200000)
	expamem_write (0x00, Z2_MEM_2MB + add_memory + zorroII);
    else if (allocated_fastmem == 0x400000)
	expamem_write (0x00, Z2_MEM_4MB + add_memory + zorroII);
    else if (allocated_fastmem == 0x800000)
	expamem_write (0x00, Z2_MEM_8MB + add_memory + zorroII);

    expamem_write (0x08, care_addr);

    expamem_write (0x04, 1);

    expamem_write (0x10, hackers_id >> 8);
    expamem_write (0x14, hackers_id & 0xff);

    expamem_write (0x18, 0x00); /* ser.no. Byte 0 */
    expamem_write (0x1c, 0x00); /* ser.no. Byte 1 */
    expamem_write (0x20, 0x00); /* ser.no. Byte 2 */
    expamem_write (0x24, 0x01); /* ser.no. Byte 3 */

    expamem_write (0x28, 0x00); /* Rom-Offset hi */
    expamem_write (0x2c, 0x00); /* ROM-Offset lo */

    expamem_write (0x40, 0x00); /* Ctrl/Statusreg.*/
}

/* ********************************************************** */

#ifdef FILESYS

/*
 * Filesystem device
 */

/****i* januae-expansion/expamem_map_filesys ********************************
*
*   NAME
*       expamem_map_filesys() -- Map the filesystem expansion ROM.
*
*   SYNOPSIS
*       static void expamem_map_filesys (void);
*
*   FUNCTION
*       Map the 64 KiB filesystem bank and publish its diagnostic-area
*       address in the emulator ROM.
*
****************************************************************************
*
*/

static void expamem_map_filesys (void)
{
    uaecptr a;

    filesys_start = ((expamem_hi | (expamem_lo >> 4)) << 16);
    map_banks (&filesys_bank, filesys_start >> 16, 1, 0);
    write_log ("Filesystem: mapped memory @$%lx.\n", filesys_start);
    /* 68k code needs to know this. */
    a = here ();
    org (RTAREA_BASE+0xFFFC);
    dl (filesys_start + 0x2000);
    org (a);
}

/****i* januae-expansion/expamem_init_filesys *******************************
*
*   NAME
*       expamem_init_filesys() -- Prepare the filesystem ROM AutoConfig
*       image.
*
*   SYNOPSIS
*       static void expamem_init_filesys (void);
*
*   FUNCTION
*       Encode the Zorro II filesystem-ROM identity, diagnostic area and
*       guest jump vectors; copy the first 0x3000 bytes into the filesystem
*       ROM backing store.
*
****************************************************************************
*
*/

static void expamem_init_filesys (void)
{
    /* struct DiagArea - the size has to be large enough to store several device ROMTags */
    uae_u8 diagarea[] = { 0x90, 0x00, /* da_Config, da_Flags */
                          0x02, 0x00, /* da_Size */
                          0x01, 0x00, /* da_DiagPoint */
                          0x01, 0x06  /* da_BootPoint */
    };

    expamem_init_clear();
    expamem_write (0x00, Z2_MEM_64KB | rom_card | zorroII);

    expamem_write (0x08, no_shutup);

    expamem_write (0x04, 2);
    expamem_write (0x10, hackers_id >> 8);
    expamem_write (0x14, hackers_id & 0xff);

    expamem_write (0x18, 0x00); /* ser.no. Byte 0 */
    expamem_write (0x1c, 0x00); /* ser.no. Byte 1 */
    expamem_write (0x20, 0x00); /* ser.no. Byte 2 */
    expamem_write (0x24, 0x01); /* ser.no. Byte 3 */

    /* er_InitDiagVec */
    expamem_write (0x28, 0x10); /* Rom-Offset hi */
    expamem_write (0x2c, 0x00); /* ROM-Offset lo */

    expamem_write (0x40, 0x00); /* Ctrl/Statusreg.*/

    /* Build a DiagArea */
    memcpy (expamem + 0x1000, diagarea, sizeof diagarea);

    /* Call DiagEntry */
    do_put_mem_word ((uae_u16 *)(expamem + 0x1100), 0x4EF9); /* JMP */
    do_put_mem_long ((uae_u32 *)(expamem + 0x1102), ROM_filesys_diagentry);

    /* What comes next is a plain bootblock */
    do_put_mem_word ((uae_u16 *)(expamem + 0x1106), 0x4EF9); /* JMP */
    do_put_mem_long ((uae_u32 *)(expamem + 0x1108), EXPANSION_bootcode);

    memcpy (filesysory, expamem, 0x3000);
}

#endif

/*
 * Zorro III expansion memory
 */

/****i* januae-expansion/expamem_map_z3fastmem ******************************
*
*   NAME
*       expamem_map_z3fastmem() -- Relocate the Zorro III fast-memory bank.
*
*   SYNOPSIS
*       static void expamem_map_z3fastmem (void);
*
*   FUNCTION
*       Compare the programmed board base with the current base; when
*       different, unmap the old range using the dummy bank and map fast
*       RAM at the new base.
*
*   NOTES
*       An unchanged base does not trigger a new map operation.
*
****************************************************************************
*
*/

static void expamem_map_z3fastmem (void)
{
	unsigned int z3fs = ((expamem_hi | (expamem_lo >> 4)) << 16);

	if (z3fastmem_start != z3fs) {
		write_log("WARNING: Z3FAST mapping changed from $%lx to $%lx\n", z3fastmem_start, z3fs);
		map_banks(&dummy_bank, z3fastmem_start >> 16, currprefs.z3fastmem_size >> 16,
			allocated_z3fastmem);
		z3fastmem_start = z3fs;
	    map_banks (&z3fastmem_bank, z3fastmem_start >> 16, currprefs.z3fastmem_size >> 16,
	       allocated_z3fastmem);
	}
    write_log ("Fastmem (32bit): mapped @$%lx: %d MB Zorro III fast memory \n",
	       z3fastmem_start, allocated_z3fastmem / 0x100000);
}

/****i* januae-expansion/expamem_init_z3fastmem *****************************
*
*   NAME
*       expamem_init_z3fastmem() -- Advertise and initially map Zorro III
*       fast RAM.
*
*   SYNOPSIS
*       static void expamem_init_z3fastmem (void);
*
*   FUNCTION
*       Encode the allocated Zorro III board size and identification, set
*       the initial base to 0x10000000, and install the bank mapping.
*
****************************************************************************
*
*/

static void expamem_init_z3fastmem (void)
{
    int code = (allocated_z3fastmem == 0x100000 ? Z2_MEM_1MB
		: allocated_z3fastmem == 0x200000 ? Z2_MEM_2MB
		: allocated_z3fastmem == 0x400000 ? Z2_MEM_4MB
		: allocated_z3fastmem == 0x800000 ? Z2_MEM_8MB
		: allocated_z3fastmem == 0x1000000 ? Z2_MEM_16MB
		: allocated_z3fastmem == 0x2000000 ? Z2_MEM_32MB
		: allocated_z3fastmem == 0x4000000 ? Z2_MEM_64MB
		: allocated_z3fastmem == 0x8000000 ? Z2_MEM_128MB
		: allocated_z3fastmem == 0x10000000 ? Z2_MEM_256MB
		: allocated_z3fastmem == 0x20000000 ? Z2_MEM_512MB
		: Z2_MEM_1GB);
    expamem_init_clear();
    expamem_write (0x00, add_memory | zorroIII | code);

    expamem_write (0x08, no_shutup | force_z3 | (allocated_z3fastmem > 0x800000 ? ext_size : Z3_MEM_AUTO));

    expamem_write (0x04, 3);

    expamem_write (0x10, hackers_id >> 8);
    expamem_write (0x14, hackers_id & 0xff);

    expamem_write (0x18, 0x00); /* ser.no. Byte 0 */
    expamem_write (0x1c, 0x00); /* ser.no. Byte 1 */
    expamem_write (0x20, 0x00); /* ser.no. Byte 2 */
    expamem_write (0x24, 0x01); /* ser.no. Byte 3 */

    expamem_write (0x28, 0x00); /* Rom-Offset hi */
    expamem_write (0x2c, 0x00); /* ROM-Offset lo */

    expamem_write (0x40, 0x00); /* Ctrl/Statusreg.*/

    z3fastmem_start = 0x10000000;

    map_banks (&z3fastmem_bank, z3fastmem_start >> 16, currprefs.z3fastmem_size >> 16,
	       allocated_z3fastmem);

}

#ifdef PICASSO96
/*
 *  Fake Graphics Card (ZORRO III) - BDK
 */

/****i* januae-expansion/expamem_map_gfxcard ********************************
*
*   NAME
*       expamem_map_gfxcard() -- Map emulated Picasso96 graphics memory.
*
*   SYNOPSIS
*       static void expamem_map_gfxcard (void);
*
*   FUNCTION
*       Compute the Zorro III address from AutoConfig and map the allocated
*       graphics-memory bank.
*
****************************************************************************
*
*/

static void expamem_map_gfxcard (void)
{
    gfxmem_start = ((expamem_hi | (expamem_lo >> 4)) << 16);
    map_banks (&gfxmem_bank, gfxmem_start >> 16, allocated_gfxmem >> 16, allocated_gfxmem);
    write_log ("UAEGFX-card: mapped @$%lx \n", gfxmem_start);
}

/****i* januae-expansion/expamem_init_gfxcard *******************************
*
*   NAME
*       expamem_init_gfxcard() -- Advertise a virtual Zorro III graphics
*       card.
*
*   SYNOPSIS
*       static void expamem_init_gfxcard (void);
*
*   FUNCTION
*       Populate the AutoConfig image for the selected 1, 2, 4 or 8 MiB
*       graphics-memory size.
*
****************************************************************************
*
*/

static void expamem_init_gfxcard (void)
{
    expamem_init_clear();
    expamem_write (0x00, zorroIII);

    switch (allocated_gfxmem) {
     case 0x00100000:
	expamem_write (0x08, no_shutup | force_z3 | Z3_MEM_1MB);
	break;
     case 0x00200000:
	expamem_write (0x08, no_shutup | force_z3 | Z3_MEM_2MB);
	break;
     case 0x00400000:
	expamem_write (0x08, no_shutup | force_z3 | Z3_MEM_4MB);
	break;
     case 0x00800000:
	expamem_write (0x08, no_shutup | force_z3 | Z3_MEM_8MB);
	break;
    }

    expamem_write (0x04, 96);

    expamem_write (0x10, hackers_id >> 8);
    expamem_write (0x14, hackers_id & 0xff);

    expamem_write (0x18, 0x00); /* ser.no. Byte 0 */
    expamem_write (0x1c, 0x00); /* ser.no. Byte 1 */
    expamem_write (0x20, 0x00); /* ser.no. Byte 2 */
    expamem_write (0x24, 0x01); /* ser.no. Byte 3 */

    expamem_write (0x28, 0x00); /* Rom-Offset hi */
    expamem_write (0x2c, 0x00); /* ROM-Offset lo */

    expamem_write (0x40, 0x00); /* Ctrl/Statusreg.*/
}
#endif

#ifdef SAVESTATE
static size_t fast_filepos, z3_filepos, p96_filepos;
#endif

/****i* januae-expansion/allocate_expamem ***********************************
*
*   NAME
*       allocate_expamem() -- Synchronize expansion RAM allocations.
*
*   SYNOPSIS
*       static void allocate_expamem (void);
*
*   FUNCTION
*       Apply changed memory-size preferences, free and allocate fast,
*       Zorro III and graphics memory as required, and bind their
*       memory-bank base pointers.
*
*   NOTES
*       Failed allocations set the corresponding allocated size to zero.
*       During SaveState restoration this routine also loads and remaps
*       saved RAM.
*
****************************************************************************
*
*/

static void allocate_expamem (void)
{
    currprefs.fastmem_size = changed_prefs.fastmem_size;
    currprefs.z3fastmem_size = changed_prefs.z3fastmem_size;
    currprefs.gfxmem_size = changed_prefs.gfxmem_size;

    if (allocated_fastmem != currprefs.fastmem_size) {
	if (fastmemory)
	    mapped_free (fastmemory);
	fastmemory = 0;
	allocated_fastmem = currprefs.fastmem_size;
	fastmem_mask = allocated_fastmem - 1;

	if (allocated_fastmem) {
	    fastmemory = mapped_malloc (allocated_fastmem, "fast");
	    if (fastmemory == 0) {
		write_log ("Out of memory for fastmem card.\n");
		allocated_fastmem = 0;
	    }
	}
	clearexec ();
    }
    if (allocated_z3fastmem != currprefs.z3fastmem_size) {
	if (z3fastmem)
	    mapped_free (z3fastmem);
	z3fastmem = 0;

	allocated_z3fastmem = currprefs.z3fastmem_size;
	z3fastmem_mask = allocated_z3fastmem - 1;

	if (allocated_z3fastmem) {
	    z3fastmem = mapped_malloc (allocated_z3fastmem, "z3");
	    if (z3fastmem == 0) {
		write_log ("Out of memory for 32 bit fast memory.\n");
		allocated_z3fastmem = 0;
	    }
	}
	clearexec ();
    }
    if (allocated_gfxmem != currprefs.gfxmem_size) {
	if (gfxmemory)
	    mapped_free (gfxmemory);
	gfxmemory = 0;

	allocated_gfxmem = currprefs.gfxmem_size;
	gfxmem_mask = allocated_gfxmem - 1;

	if (allocated_gfxmem) {
	    gfxmemory = mapped_malloc (allocated_gfxmem, "gfx");
	    if (gfxmemory == 0) {
		write_log ("Out of memory for graphics card memory\n");
		allocated_gfxmem = 0;
	    }
	}
	clearexec ();
    }

    z3fastmem_bank.baseaddr = z3fastmem;
    fastmem_bank.baseaddr = fastmemory;

#ifdef SAVESTATE
    if (savestate_state == STATE_RESTORE) {
	if (allocated_fastmem > 0) {
	    restore_ram (fast_filepos, fastmemory);
	    map_banks (&fastmem_bank, fastmem_start >> 16, currprefs.fastmem_size >> 16,
		       allocated_fastmem);
	}
	if (allocated_z3fastmem > 0) {
	    restore_ram (z3_filepos, z3fastmem);
	    map_banks (&z3fastmem_bank, z3fastmem_start >> 16, currprefs.z3fastmem_size >> 16,
		       allocated_z3fastmem);
	}
#ifdef PICASSO96
	if (allocated_gfxmem > 0 && gfxmem_start > 0) {
	    restore_ram (p96_filepos, gfxmemory);
	    map_banks (&gfxmem_bank, gfxmem_start >> 16, currprefs.gfxmem_size >> 16,
		       allocated_gfxmem);
	}
#endif
    }
#endif /* SAVESTATE */
}

extern int cdtv_enabled;

/****i* januae-expansion/expamem_reset **************************************
*
*   NAME
*       expamem_reset() -- Rebuild the AutoConfig enumeration sequence.
*
*   SYNOPSIS
*       void expamem_reset (void);
*
*   FUNCTION
*       Synchronize expansion RAM and collect present memory, filesystem,
*       graphics and Catweasel cards into the enumeration arrays;
*       initialize the first card.
*
*   NOTES
*       Filesystem mounting is suppressed for certain old Kickstart
*       versions and when no filesystem units are configured.
*
****************************************************************************
*
*/

void expamem_reset (void)
{
    int do_mount = 1;
    int cardno = 0;
    ecard = 0;

    allocate_expamem ();

#ifdef CDTV
    if (cdtv_enabled)
        map_banks (&dmac_bank, dmac_start >> 16, 0x10000 >> 16, 0x10000);
#endif

    /* check if Kickstart version is below 1.3 */
    if (! ersatzkickfile && kickstart_version
	&& (/* Kickstart 1.0 & 1.1! */
	    kickstart_version == 0xFFFF
	    /* Kickstart < 1.3 */
	    || kickstart_version < 34))
    {
	/* warn user */
	write_log ("Kickstart version is below 1.3!  Disabling autoconfig devices.\n");
	do_mount = 0;
    }
#ifdef FILESYS
    /* No need for filesystem stuff if there aren't any mounted.  */
    if (nr_units (currprefs.mountinfo) == 0)
	do_mount = 0;
#endif

    if (fastmemory != NULL) {
	card_init[cardno] = expamem_init_fastcard;
	card_map[cardno++] = expamem_map_fastcard;
    }
    if (z3fastmem != NULL) {
	card_init[cardno] = expamem_init_z3fastmem;
	card_map[cardno++] = expamem_map_z3fastmem;
    }
#ifdef PICASSO96
    if (gfxmemory != NULL) {
	card_init[cardno] = expamem_init_gfxcard;
	card_map[cardno++] = expamem_map_gfxcard;
    }
#endif
#ifdef FILESYS
    if (do_mount && ! ersatzkickfile) {
	card_init[cardno] = expamem_init_filesys;
	card_map[cardno++] = expamem_map_filesys;
    }
#endif
#ifdef CATWEASEL
    if (catweasel_init ()) {
        card_init[cardno] = expamem_init_catweasel;
        card_map[cardno++] = expamem_map_catweasel;
    }
#endif
    while (cardno < MAX_EXPANSION_BOARDS) {
	card_init[cardno] = expamem_init_clear;
	card_map[cardno++] = expamem_map_clear;
    }

    (*card_init[0]) ();
}

/****i* januae-expansion/expansion_init *************************************
*
*   NAME
*       expansion_init() -- Initialize expansion-memory bookkeeping.
*
*   SYNOPSIS
*       void expansion_init (void);
*
*   FUNCTION
*       Clear expansion-bank state, allocate configured expansion RAM and,
*       with FILESYS, allocate the 64 KiB filesystem ROM backing store.
*
*   NOTES
*       When the filesystem ROM allocation fails, this implementation logs
*       an error and exits with status zero.
*
****************************************************************************
*
*/

void expansion_init (void)
{
    z3fastmem = 0;
    gfxmemory = 0;
    fastmemory = 0;
    allocated_z3fastmem = 0;
    allocated_gfxmem = 0;
    allocated_fastmem = 0;
    fastmem_mask = fastmem_start = 0;
    fastmemory = 0;
    gfxmem_mask = gfxmem_start = 0;
    gfxmemory = 0;
#ifdef CATWEASEL
    catweasel_mask = catweasel_start = 0;
#endif
#ifdef FILESYS
    filesys_start = 0;
    filesysory = 0;
#endif
    z3fastmem_mask = z3fastmem_start = 0;
    z3fastmem = 0;

    allocate_expamem ();

#ifdef FILESYS
    filesysory = (uae_u8 *) mapped_malloc (0x10000, "filesys");
    if (!filesysory) {
	write_log ("virtual memory exhausted (filesysory)!\n");
	exit (0);
    }
    filesys_bank.baseaddr = (uae_u8*)filesysory;
#endif
}

/****i* januae-expansion/expansion_cleanup **********************************
*
*   NAME
*       expansion_cleanup() -- Release emulated expansion-memory resources.
*
*   SYNOPSIS
*       void expansion_cleanup (void);
*
*   FUNCTION
*       Free allocated fast, Zorro III, graphics and optional filesystem
*       memory; release Catweasel resources when enabled.
*
****************************************************************************
*
*/

void expansion_cleanup (void)
{
    if (fastmemory)
	mapped_free (fastmemory);
    if (z3fastmem)
	mapped_free (z3fastmem);
    if (gfxmemory)
	mapped_free (gfxmemory);
#ifdef FILESYS
    if (filesysory)
	mapped_free (filesysory);
    filesysory = 0;
#endif
    fastmemory = 0;
    z3fastmem = 0;
    gfxmemory = 0;
#ifdef CATWEASEL
    catweasel_free ();
#endif
}


#ifdef SAVESTATE

/* State save/restore code.  */

/****i* januae-expansion/save_fram ******************************************
*
*   NAME
*       save_fram() -- Expose Zorro II fast RAM for SaveState.
*
*   SYNOPSIS
*       uae_u8 *save_fram (uae_u32 *len);
*
*   FUNCTION
*       Write the allocated size to len and provide the current Zorro II
*       fast RAM storage without copying it.
*
*   INPUTS
*       len - Pointer receiving the length, or the saved image size.
*
*   RESULT
*       Pointer to fastmemory (which may be NULL).
*
*   NOTES
*       The caller does not own the returned memory.
*
****************************************************************************
*
*/

uae_u8 *save_fram (uae_u32 *len)
{
    *len = allocated_fastmem;
    return fastmemory;
}

/****i* januae-expansion/save_zram ******************************************
*
*   NAME
*       save_zram() -- Expose Zorro III fast RAM for SaveState.
*
*   SYNOPSIS
*       uae_u8 *save_zram (uae_u32 *len);
*
*   FUNCTION
*       Write the allocated size to len and provide the current Zorro III
*       fast RAM storage without copying it.
*
*   INPUTS
*       len - Pointer receiving the length, or the saved image size.
*
*   RESULT
*       Pointer to z3fastmem (which may be NULL).
*
*   NOTES
*       The caller does not own the returned memory.
*
****************************************************************************
*
*/

uae_u8 *save_zram (uae_u32 *len)
{
    *len = allocated_z3fastmem;
    return z3fastmem;
}

/****i* januae-expansion/save_pram ******************************************
*
*   NAME
*       save_pram() -- Expose graphics RAM for SaveState.
*
*   SYNOPSIS
*       uae_u8 *save_pram (uae_u32 *len);
*
*   FUNCTION
*       Write the allocated size to len and provide the current graphics
*       RAM storage without copying it.
*
*   INPUTS
*       len - Pointer receiving the length, or the saved image size.
*
*   RESULT
*       Pointer to gfxmemory (which may be NULL).
*
*   NOTES
*       The caller does not own the returned memory.
*
****************************************************************************
*
*/

uae_u8 *save_pram (uae_u32 *len)
{
    *len = allocated_gfxmem;
    return gfxmemory;
}

/****i* januae-expansion/restore_fram ***************************************
*
*   NAME
*       restore_fram() -- Stage Zorro II fast RAM restoration.
*
*   SYNOPSIS
*       void restore_fram (uae_u32 len, size_t filepos);
*
*   FUNCTION
*       Record the saved-file position and update
*       changed_prefs.fastmem_size with the supplied image size.
*
*   INPUTS
*       len - Pointer receiving the length, or the saved image size.
*       filepos - File position of the saved RAM contents.
*
*   NOTES
*       The RAM contents are restored later during allocation, not by this
*       routine.
*
****************************************************************************
*
*/

void restore_fram (uae_u32 len, size_t filepos)
{
    fast_filepos = filepos;
    changed_prefs.fastmem_size = len;
}

/****i* januae-expansion/restore_zram ***************************************
*
*   NAME
*       restore_zram() -- Stage Zorro III fast RAM restoration.
*
*   SYNOPSIS
*       void restore_zram (uae_u32 len, size_t filepos);
*
*   FUNCTION
*       Record the saved-file position and update
*       changed_prefs.z3fastmem_size with the supplied image size.
*
*   INPUTS
*       len - Pointer receiving the length, or the saved image size.
*       filepos - File position of the saved RAM contents.
*
*   NOTES
*       The RAM contents are restored later during allocation, not by this
*       routine.
*
****************************************************************************
*
*/

void restore_zram (uae_u32 len, size_t filepos)
{
    z3_filepos = filepos;
    changed_prefs.z3fastmem_size = len;
}

/****i* januae-expansion/restore_pram ***************************************
*
*   NAME
*       restore_pram() -- Stage graphics RAM restoration.
*
*   SYNOPSIS
*       void restore_pram (uae_u32 len, size_t filepos);
*
*   FUNCTION
*       Record the saved-file position and update changed_prefs.gfxmem_size
*       with the supplied image size.
*
*   INPUTS
*       len - Pointer receiving the length, or the saved image size.
*       filepos - File position of the saved RAM contents.
*
*   NOTES
*       The RAM contents are restored later during allocation, not by this
*       routine.
*
****************************************************************************
*
*/

void restore_pram (uae_u32 len, size_t filepos)
{
    p96_filepos = filepos;
    changed_prefs.gfxmem_size = len;
}

/****i* januae-expansion/save_expansion *************************************
*
*   NAME
*       save_expansion() -- Serialize expansion-board base addresses.
*
*   SYNOPSIS
*       uae_u8 *save_expansion (uae_u32 *len, uae_u8 *dstptr);
*
*   FUNCTION
*       Store the configured Zorro II, Zorro III and graphics-memory base
*       addresses as three 32-bit fields.
*
*   INPUTS
*       len - Pointer receiving the length, or the saved image size.
*       dstptr - Optional output buffer; NULL selects the internal buffer.
*
*   RESULT
*       Pointer to the caller buffer when supplied, or a shared static
*       buffer.
*
*   NOTES
*       The output length is set to 12 bytes. The caller must provide at
*       least 12 writable bytes when dstptr is non-NULL.
*
****************************************************************************
*
*/

uae_u8 *save_expansion (uae_u32 *len, uae_u8 *dstptr)
{
    static uae_u8 t[20];
    uae_u8 *dst = t, *dstbak = t;
    if (dstptr)
	dst = dstbak = dstptr;
    save_u32 (fastmem_start);
    save_u32 (z3fastmem_start);
    save_u32 (gfxmem_start);
    *len = 4 + 4 + 4;
    return dstbak;
}

/****i* januae-expansion/restore_expansion **********************************
*
*   NAME
*       restore_expansion() -- Restore expansion-board base addresses.
*
*   SYNOPSIS
*       const uae_u8 *restore_expansion (const uae_u8 *src);
*
*   FUNCTION
*       Read three 32-bit values through the SaveState stream and replace
*       the current fast, Zorro III and graphics-memory bases.
*
*   INPUTS
*       src - Input pointer supplied by the SaveState caller.
*
*   RESULT
*       Pointer advanced past the three restored 32-bit fields.
*
*   NOTES
*       The restore_u32() macros advance src while reading each field.
*
****************************************************************************
*
*/

const uae_u8 *restore_expansion (const uae_u8 *src)
{
    fastmem_start = restore_u32 ();
    z3fastmem_start = restore_u32 ();
    gfxmem_start = restore_u32 ();
    return src;
}

#endif /* SAVESTATE */
