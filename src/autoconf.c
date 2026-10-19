 /*
  * UAE - The Un*x Amiga Emulator
  *
  * AutoConfig devices
  *
  * Copyright 1995, 1996 Bernd Schmidt
  * Copyright 1996 Ed Hanway
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "events.h"
#include "newcpu.h"
#include "autoconf.h"
#include "traps.h"
#include "version.h"

/* Commonly used autoconfig strings */

uaecptr EXPANSION_explibname, EXPANSION_doslibname, EXPANSION_uaeversion;
uaecptr EXPANSION_uaedevname, EXPANSION_explibbase = 0, EXPANSION_haveV36;
uaecptr EXPANSION_bootcode, EXPANSION_nullfunc;
uaecptr EXPANSION_cddevice;

/* ROM tag area memory access */

uae_u8 *rtarea;

static uae_u32 rtarea_lget (uaecptr) REGPARAM;
static uae_u32 rtarea_wget (uaecptr) REGPARAM;
static uae_u32 rtarea_bget (uaecptr) REGPARAM;
static void rtarea_lput (uaecptr, uae_u32) REGPARAM;
static void rtarea_wput (uaecptr, uae_u32) REGPARAM;
static void rtarea_bput (uaecptr, uae_u32) REGPARAM;
static uae_u8 *rtarea_xlate (uaecptr) REGPARAM;

addrbank rtarea_bank = {
    rtarea_lget, rtarea_wget, rtarea_bget,
    rtarea_lput, rtarea_wput, rtarea_bput,
    rtarea_xlate, default_check, NULL
};

/****i* januae-autoconf/rtarea_xlate *************************************
*
*   NAME
*       rtarea_xlate() -- Translate a ROM-area address to host memory.
*
*   SYNOPSIS
*       uae_u8 REGPARAM2 *rtarea_xlate (uaecptr addr);
*
*   FUNCTION
*       Return the address of the byte selected by the low 16 bits of
*       addr in the allocated ROM/trap area.
*
*   INPUTS
*       addr - Guest address to translate.
*
*   RESULT
*       Pointer into rtarea; no address validation is performed.
*
**************************************************************************
*
*/

uae_u8 REGPARAM2 *rtarea_xlate (uaecptr addr)
{
    addr &= 0xFFFF;
    return rtarea + addr;
}

/****i* januae-autoconf/rtarea_lget **************************************
*
*   NAME
*       rtarea_lget() -- Read a 32-bit value from the ROM/trap area.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 rtarea_lget (uaecptr addr);
*
*   FUNCTION
*       Combine two 16-bit reads in big-endian order.
*
*   INPUTS
*       addr - Guest address of the first word.
*
*   RESULT
*       The assembled 32-bit value.
*
*   NOTES
*       On JIT builds, marks a special-memory read. Each word read is
*       separately masked to the low 16 address bits.
*
**************************************************************************
*
*/

uae_u32 REGPARAM2 rtarea_lget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    addr &= 0xFFFF;
    return (uae_u32)(rtarea_wget (addr) << 16) + rtarea_wget (addr+2);
}

/****i* januae-autoconf/rtarea_wget **************************************
*
*   NAME
*       rtarea_wget() -- Read a 16-bit value from the ROM/trap area.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 rtarea_wget (uaecptr addr);
*
*   FUNCTION
*       Combine two consecutive bytes in big-endian order after masking
*       addr to 16 bits.
*
*   INPUTS
*       addr - Guest address of the first byte.
*
*   RESULT
*       The zero-extended 16-bit value.
*
*   NOTES
*       On JIT builds, marks a special-memory read. The second byte
*       access is not separately bounds-checked.
*
**************************************************************************
*
*/

uae_u32 REGPARAM2 rtarea_wget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    addr &= 0xFFFF;
    return (rtarea[addr]<<8) + rtarea[addr+1];
}

/****i* januae-autoconf/rtarea_bget **************************************
*
*   NAME
*       rtarea_bget() -- Read a byte from the ROM/trap area.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 rtarea_bget (uaecptr addr);
*
*   FUNCTION
*       Return the byte at the low 16 bits of addr.
*
*   INPUTS
*       addr - Guest address to read.
*
*   RESULT
*       The zero-extended byte value.
*
*   NOTES
*       On JIT builds, marks a special-memory read.
*
**************************************************************************
*
*/

uae_u32 REGPARAM2 rtarea_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_READ;
#endif
    addr &= 0xFFFF;
    return rtarea[addr];
}

/****i* januae-autoconf/rtarea_lput **************************************
*
*   NAME
*       rtarea_lput() -- Handle a 32-bit write to the ROM/trap area.
*
*   SYNOPSIS
*       void REGPARAM2 rtarea_lput (uaecptr addr, uae_u32 value);
*
*   FUNCTION
*       Discard the write without changing the ROM-area buffer.
*
*   INPUTS
*       addr - Requested guest address.
*       value - Value to discard.
*
*   NOTES
*       On JIT builds, marks a special-memory write.
*
**************************************************************************
*
*/

void REGPARAM2 rtarea_lput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
}

/****i* januae-autoconf/rtarea_wput **************************************
*
*   NAME
*       rtarea_wput() -- Handle a 16-bit write to the ROM/trap area.
*
*   SYNOPSIS
*       void REGPARAM2 rtarea_wput (uaecptr addr, uae_u32 value);
*
*   FUNCTION
*       Discard the write without changing the ROM-area buffer.
*
*   INPUTS
*       addr - Requested guest address.
*       value - Value to discard.
*
*   NOTES
*       On JIT builds, marks a special-memory write.
*
**************************************************************************
*
*/

void REGPARAM2 rtarea_wput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
}

/****i* januae-autoconf/rtarea_bput **************************************
*
*   NAME
*       rtarea_bput() -- Handle a byte write to the ROM/trap area.
*
*   SYNOPSIS
*       void REGPARAM2 rtarea_bput (uaecptr addr, uae_u32 value);
*
*   FUNCTION
*       Discard the write without changing the ROM-area buffer.
*
*   INPUTS
*       addr - Requested guest address.
*       value - Value to discard.
*
*   NOTES
*       On JIT builds, marks a special-memory write.
*
**************************************************************************
*
*/

void REGPARAM2 rtarea_bput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= SPECIAL_MEM_WRITE;
#endif
}


/* some quick & dirty code to fill in the rt area and save me a lot of
 * scratch paper
 */

static int rt_addr;
static int rt_straddr;

/****i* januae-autoconf/addr *********************************************
*
*   NAME
*       addr() -- Convert a ROM-area offset to a guest address.
*
*   SYNOPSIS
*       uae_u32 addr (int ptr);
*
*   FUNCTION
*       Add RTAREA_BASE to the supplied offset.
*
*   INPUTS
*       ptr - Byte offset within the ROM/trap area.
*
*   RESULT
*       The resulting guest address.
*
**************************************************************************
*
*/

uae_u32 addr (int ptr)
{
    return (uae_u32)ptr + RTAREA_BASE;
}

/****i* januae-autoconf/db ***********************************************
*
*   NAME
*       db() -- Append a byte to the generated ROM-area code.
*
*   SYNOPSIS
*       void db (uae_u8 data);
*
*   FUNCTION
*       Write data at the current code cursor and advance it by one
*       byte.
*
*   INPUTS
*       data - Byte to emit.
*
*   NOTES
*       The caller is responsible for keeping the code cursor within the
*       allocated area.
*
**************************************************************************
*
*/

void db (uae_u8 data)
{
    rtarea[rt_addr++] = data;
}

/****i* januae-autoconf/dw ***********************************************
*
*   NAME
*       dw() -- Append a big-endian word to the ROM-area code.
*
*   SYNOPSIS
*       void dw (uae_u16 data);
*
*   FUNCTION
*       Write the high and low bytes at the current code cursor and
*       advance it by two bytes.
*
*   INPUTS
*       data - Word to emit.
*
*   NOTES
*       The caller is responsible for keeping the code cursor within the
*       allocated area.
*
**************************************************************************
*
*/

void dw (uae_u16 data)
{
    rtarea[rt_addr++] = (uae_u8)(data >> 8);
    rtarea[rt_addr++] = (uae_u8)data;
}

/****i* januae-autoconf/dl ***********************************************
*
*   NAME
*       dl() -- Append a big-endian longword to the ROM-area code.
*
*   SYNOPSIS
*       void dl (uae_u32 data);
*
*   FUNCTION
*       Write the four bytes from most to least significant at the
*       current code cursor.
*
*   INPUTS
*       data - Longword to emit.
*
*   NOTES
*       Advances the code cursor by four bytes without a bounds check.
*
**************************************************************************
*
*/

void dl (uae_u32 data)
{
    rtarea[rt_addr++] = data >> 24;
    rtarea[rt_addr++] = data >> 16;
    rtarea[rt_addr++] = data >> 8;
    rtarea[rt_addr++] = data;
}

/* store strings starting at the end of the rt area and working
 * backward.  store pointer at current address
 */

/****i* januae-autoconf/ds ***********************************************
*
*   NAME
*       ds() -- Store a NUL-terminated string in the ROM-area string
*       region.
*
*   SYNOPSIS
*       uae_u32 ds (const char *str);
*
*   FUNCTION
*       Move the string cursor downward by strlen(str) plus one, then
*       copy the string there.
*
*   INPUTS
*       str - NUL-terminated host string to copy.
*
*   RESULT
*       Guest address of the copied string.
*
*   NOTES
*       No NULL, capacity or overlap check is performed. The code and
*       string cursors share rtarea.
*
**************************************************************************
*
*/

uae_u32 ds (const char *str)
{
    int len = strlen (str) + 1;

    rt_straddr -= len;
    strcpy ((char *)rtarea + rt_straddr, str);

    return addr (rt_straddr);
}

/****i* januae-autoconf/calltrap *****************************************
*
*   NAME
*       calltrap() -- Emit an emulator trap instruction.
*
*   SYNOPSIS
*       void calltrap (uae_u32 n);
*
*   FUNCTION
*       Append the low 16 bits of 0xA000 + n as a big-endian word.
*
*   INPUTS
*       n - Trap number returned by the trap registry.
*
*   NOTES
*       The trap number is not range-checked here.
*
**************************************************************************
*
*/

void calltrap (uae_u32 n)
{
    dw (0xA000 + n);
}

/****i* januae-autoconf/org **********************************************
*
*   NAME
*       org() -- Set the ROM-area code cursor to a guest address.
*
*   SYNOPSIS
*       void org (uae_u32 a);
*
*   FUNCTION
*       Replace the current cursor with a minus RTAREA_BASE.
*
*   INPUTS
*       a - Absolute guest address for the next emitted byte.
*
*   NOTES
*       No range check is performed.
*
**************************************************************************
*
*/

void org (uae_u32 a)
{
    rt_addr = a - RTAREA_BASE;
}

/****i* januae-autoconf/here *********************************************
*
*   NAME
*       here() -- Get the current ROM-area code address.
*
*   SYNOPSIS
*       uae_u32 here (void);
*
*   FUNCTION
*       Convert the current code cursor to a guest address using
*       RTAREA_BASE.
*
*   RESULT
*       Guest address of the next byte to be emitted.
*
**************************************************************************
*
*/

uae_u32 here (void)
{
    return addr (rt_addr);
}

/****i* januae-autoconf/align ********************************************
*
*   NAME
*       align() -- Align the ROM-area code cursor.
*
*   SYNOPSIS
*       void align (int b);
*
*   FUNCTION
*       Round the cursor upward using (cursor + b - 1) & ~(b - 1).
*
*   INPUTS
*       b - Alignment boundary.
*       callers must provide a positive power of two.
*
*   NOTES
*       The function does not validate b.
*
**************************************************************************
*
*/

void align (int b)
{
    rt_addr = (rt_addr + b - 1) & ~(b - 1);
}

/****i* januae-autoconf/nullfunc *****************************************
*
*   NAME
*       nullfunc() -- Handle the default emulator trap.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 nullfunc (TrapContext *context);
*
*   FUNCTION
*       Write a diagnostic message to the host log.
*
*   INPUTS
*       context - Trap context.
*       not examined.
*
*   RESULT
*       Zero.
*
**************************************************************************
*
*/

static uae_u32 REGPARAM2 nullfunc (TrapContext *context)
{
    write_log ("Null function called\n");
    return 0;
}

/****i* januae-autoconf/getchipmemsize ***********************************
*
*   NAME
*       getchipmemsize() -- Return the allocated chip-memory size to a
*       guest trap.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 getchipmemsize (TrapContext *context);
*
*   FUNCTION
*       Read allocated_chipmem from the emulator memory state.
*
*   INPUTS
*       context - Trap context.
*       not examined.
*
*   RESULT
*       The current allocated_chipmem value.
*
**************************************************************************
*
*/

static uae_u32 REGPARAM2 getchipmemsize (TrapContext *context)
{
    return allocated_chipmem;
}

/****i* januae-autoconf/uae_puts *****************************************
*
*   NAME
*       uae_puts() -- Print a guest string through the host standard
*       output.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 uae_puts (TrapContext *context);
*
*   FUNCTION
*       Translate guest register A0 to a host address and pass it to
*       puts().
*
*   INPUTS
*       context - Trap register context containing the string address in
*       A0.
*
*   RESULT
*       Zero, irrespective of the result of puts().
*
*   NOTES
*       The caller must supply an accessible NUL-terminated string.
*
**************************************************************************
*
*/

static uae_u32 REGPARAM2 uae_puts (TrapContext *context)
{
    puts ((char *)get_real_address (m68k_areg (&context->regs, 0)));
    return 0;
}

/****i* januae-autoconf/rtarea_init_mem **********************************
*
*   NAME
*       rtarea_init_mem() -- Allocate storage for the ROM/trap area.
*
*   SYNOPSIS
*       static void rtarea_init_mem (void);
*
*   FUNCTION
*       Allocate 0x10000 bytes with mapped_malloc() and assign the
*       mapping to rtarea_bank.baseaddr.
*
*   NOTES
*       Allocation failure is logged and terminates the process with
*       abort().
*
**************************************************************************
*
*/

static void rtarea_init_mem (void)
{
    rtarea = mapped_malloc (0x10000, "rtarea");
    if (!rtarea) {
	write_log ("virtual memory exhausted (rtarea)!\n");
	abort ();
    }
    rtarea_bank.baseaddr = rtarea;
}

/****i* januae-autoconf/rtarea_init **************************************
*
*   NAME
*       rtarea_init() -- Initialize the emulator ROM/trap area.
*
*   SYNOPSIS
*       void rtarea_init (void);
*
*   FUNCTION
*       Initialize the trap registry and ROM-area storage, place shared
*       strings and built-in trap stubs, and initialize extended traps.
*
*   NOTES
*       Installs filesystem trap code when FILESYS is enabled. Memory
*       allocation failure aborts execution.
*
**************************************************************************
*
*/

void rtarea_init (void)
{
    uae_u32 a;
    char uaever[100];

    rt_straddr = 0xFF00 - 2;
    rt_addr = 0;

    init_traps ();

    rtarea_init_mem ();

    sprintf (uaever, "uae-%d.%d.%d", UAEMAJOR, UAEMINOR, UAESUBREV);

    EXPANSION_uaeversion = ds (uaever);
    EXPANSION_explibname = ds ("expansion.library");
    EXPANSION_doslibname = ds ("dos.library");
    EXPANSION_uaedevname = ds ("uae.device");

    deftrap (NULL); /* Generic emulator trap */

    EXPANSION_nullfunc = here ();
    calltrap (deftrap (nullfunc));
    dw (RTS);

    a = here();
   /* Dummy trap - removing this breaks the filesys emulation. */
    org (RTAREA_BASE + 0xFF00);
    calltrap (deftrap2 (nullfunc, TRAPFLAG_NO_RETVAL, ""));

    org (RTAREA_BASE + 0xFF80);
    calltrap (deftrap2 (getchipmemsize, TRAPFLAG_DORET, ""));

    org (RTAREA_BASE + 0xFF10);
    calltrap (deftrap2 (uae_puts, TRAPFLAG_NO_RETVAL, ""));
    dw (RTS);

    org (a);

#ifdef FILESYS
    filesys_install_code ();
#endif

    init_extended_traps ();
}

volatile int uae_int_requested = 0;

/****i* januae-autoconf/set_uae_int_flag *********************************
*
*   NAME
*       set_uae_int_flag() -- Publish the requested emulator interrupt
*       flags in ROM-area memory.
*
*   SYNOPSIS
*       void set_uae_int_flag (void);
*
*   FUNCTION
*       Store the low byte of uae_int_requested at ROM-area offset
*       0xFFFB.
*
*   NOTES
*       Requires an initialized rtarea buffer.
*
**************************************************************************
*
*/

void set_uae_int_flag (void)
{
    rtarea[0xFFFB] = uae_int_requested;
}

/****i* januae-autoconf/rtarea_setup *************************************
*
*   NAME
*       rtarea_setup() -- Perform the ROM-area setup hook.
*
*   SYNOPSIS
*       void rtarea_setup (void);
*
*   FUNCTION
*       This implementation is empty and does not change emulator state.
*
**************************************************************************
*
*/

void rtarea_setup (void)
{
}
