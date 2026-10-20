 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Debugger
  *
  * (c) 1995 Bernd Schmidt
  *
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include <ctype.h>
#include <signal.h>

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "cpu_prefetch.h"
#include "debug.h"
#include "cia.h"
#include "xwin.h"
#include "identify.h"
#include "disk.h"
#include "savestate.h"
#include "autoconf.h"
#include "filesys.h"
#include "akiko.h"
#include "inputdevice.h"
#include "audio.h"

static int debugger_active;
static uaecptr skipaddr_start, skipaddr_end;
static int skipaddr_doskip;
static uae_u32 skipins;
static int do_skip;
#ifdef SAVESTATE
static int debug_rewind;
#endif
static int memwatch_enabled, memwatch_triggered;
int debugging;
int exception_debugging;
int debug_copper;
int debug_sprite_mask = 0xff;
static uaecptr debug_copper_pc;

extern int audio_channel_mask;

static FILE *logfile;

#ifndef _WIN32
#define console_out               printf
#define console_flush()           fflush( stdout )
#define console_get( input, len ) fgets( input, len, stdin )
#endif


#define MAX_HIST 100

unsigned int firsthist = 0;
unsigned int lasthist = 0;

static struct regstruct history[MAX_HIST];


/****i* januae-debugger/activate_debugger ***********************************
*
*   NAME
*       activate_debugger() -- Request debugger entry.
*
*   SYNOPSIS
*       void activate_debugger (void);
*
*   FUNCTION
*       If debugging is available, leave full-screen mode where possible,
*       close an active instruction-trace file and request a CPU break.
*
*   NOTES
*       If full-screen mode cannot be exited, the function logs a message and
*       does not activate the debugger.
*
****************************************************************************
*
*/

void activate_debugger (void)
{
    if (debuggable ()) {
	if (is_fullscreen ()) {
	    toggle_fullscreen ();
	    if (is_fullscreen ()) {
		write_log ("Cannot activate debugger in full-screen mode\n");
		return;
            }
        }

	if (logfile)
	    fclose (logfile);
	logfile = 0;
	do_skip = 0;
	if (debugger_active)
	    return;
	debugger_active = 1;
	set_special (&regs, SPCFLAG_BRK);
	debugging = 1;
   }
}


static const char help[] = {
    "          HELP for UAE Debugger\n"
    "         -----------------------\n\n"
    "  g [<address>]         Start execution at the current address or <address>\n"
    "  c                     Dump state of the CIA, disk drives and custom registers\n"
    "  r                     Dump state of the CPU\n"
    "  m <address> [<lines>] Memory dump starting at <address>\n"
    "  d <address> [<lines>] Disassembly starting at <address>\n"
    "  t [instructions]      Step one or more instructions\n"
    "  z                     Step through one instruction - useful for JSR, DBRA etc\n"
    "  f                     Step forward until PC in RAM (\"boot block finder\")\n"
    "  f <address>           Add/remove breakpoint\n"
    "  fi                    Step forward until PC points to RTS/RTD or RTE\n"
    "  fi <opcode>           Step forward until PC points to <opcode>\n"
    "  fl                    List breakpoints\n"
    "  fd                    Remove all breakpoints\n"
    "  f <addr1> <addr2>     Step forward until <addr1> <= PC <= <addr2>\n"
    "  e [<addr>]            Dump contents of all custom registers\n"
    "  i [<addr>]            Dump contents of interrupt and trap vectors\n"
    "  o <0-2|addr> [<lines>]View memory as Copper instructions\n"
    "  od                    Enable/disable Copper vpos/hpos tracing\n"
    "  ot                    Copper single step trace\n"
    "  ob <addr>             Copper breakpoint\n"
    "  O                     Display bitplane offsets\n"
    "  O <plane> <offset>    Offset a bitplane\n"
    "  H[H] <cnt>            Show PC history (HH=full CPU info) <cnt> instructions\n"
    "  C <value>             Search for values like energy or lifes in games\n"
    "  W <address> <value>   Write into Amiga memory\n"
    "  w <num> <address> <length> <R/W/RW> [<value>]\n"
    "                        Add/remove memory watchpoints\n"
    "  wd                    Enable illegal access logger\n"
    "  S <file> <addr> <n>   Save a block of Amiga memory\n"
    "  s <string>/<values> [<addr>] [<length>]\n"
    "                        Search for string/bytes\n"
    "  T                     Show exec tasks and their PCs\n"
    "  h,?                   Show this help page\n"
#ifdef SAVESTATE
    "  b                     Step to previous state capture position\n"
#endif
    "  am <channel mask>     Enable or disable audio channels\n"
    "  sm <sprite mask>      Enable or disable sprites\n"
    "  di <mode> [<track>]   Break on disk access. R=DMA read,W=write,RW=both,P=PIO\n"
    "                        Also enables extended disk logging\n"
    "  q                     Quit the emulator. You don't want to use this command.\n\n"
};

/****i* januae-debugger/debug_help ******************************************
*
*   NAME
*       debug_help() -- Display debugger command help.
*
*   SYNOPSIS
*       static void debug_help (void);
*
*   FUNCTION
*       Write the built-in debugger command reference to the console.
*
****************************************************************************
*
*/

static void debug_help (void)
{
    console_out (help);
}



/****i* januae-debugger/ignore_ws *******************************************
*
*   NAME
*       ignore_ws() -- Skip leading white space.
*
*   SYNOPSIS
*       static void ignore_ws (const char **c);
*
*   FUNCTION
*       Advance the caller-owned command cursor over isspace() characters
*       without consuming the first non-space character.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
****************************************************************************
*
*/

static void ignore_ws (const char **c)
{
    while (**c && isspace (**c))
        (*c)++;
}

static uae_u32 readint (const char **c);
/****i* januae-debugger/readhex *********************************************
*
*   NAME
*       readhex() -- Read a hexadecimal command argument.
*
*   SYNOPSIS
*       static uae_u32 readhex (const char **c);
*
*   FUNCTION
*       Skip white space and accumulate hexadecimal digits at the command
*       cursor. A leading ! or _ redirects parsing to decimal readint().
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
*   RESULT
*       Parsed unsigned 32-bit value.
*
*   NOTES
*       A missing digit yields zero. The cursor remains at the first
*       character not consumed.
*
****************************************************************************
*
*/

static uae_u32 readhex (const char **c)
{
    uae_u32 val = 0;
    char nc;

    ignore_ws (c);
    if (**c == '!' || **c == '_') {
	(*c)++;
	return readint (c);
    }
    while (isxdigit (nc = **c)) {
	(*c)++;
	val *= 16;
	nc = toupper (nc);
	if (isdigit (nc)) {
	    val += nc - '0';
	} else {
	    val += nc - 'A' + 10;
	}
    }
    return val;
}

/****i* januae-debugger/readint *********************************************
*
*   NAME
*       readint() -- Read a decimal command argument.
*
*   SYNOPSIS
*       static uae_u32 readint (const char **c);
*
*   FUNCTION
*       Skip white space and parse decimal digits, or use readhex() after a
*       leading $ or 0x prefix.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
*   RESULT
*       Parsed unsigned 32-bit value.
*
*   NOTES
*       A leading minus is consumed but the implementation assigns a positive
*       sign. No range error is reported.
*
****************************************************************************
*
*/

static uae_u32 readint (const char **c)
{
    uae_u32 val = 0;
    char nc;
    int sign = 1;

    ignore_ws (c);
    if (**c == '$') {
	(*c)++;
	return readhex (c);
    }
    if (**c == '0' && toupper ((*c)[1]) == 'X') {
	(*c) += 2;
	return readhex (c);
    }
    if (**c == '-') {
	sign = 1;
	(*c)++;
    }
    while (isdigit (nc = **c)) {
	(*c)++;
	val *= 10;
	val += nc - '0';
    }
    return val * sign;
}

/****i* januae-debugger/next_char *******************************************
*
*   NAME
*       next_char() -- Consume the next command character.
*
*   SYNOPSIS
*       static char next_char (const char **c);
*
*   FUNCTION
*       Skip leading white space, return the current character and advance
*       the command cursor.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
*   RESULT
*       Character at the cursor before advancement.
*
*   NOTES
*       The cursor is advanced even when the character is the string
*       terminator.
*
****************************************************************************
*
*/

static char next_char (const char **c)
{
    ignore_ws (c);
    return *(*c)++;
}

/****i* januae-debugger/peek_next_char **************************************
*
*   NAME
*       peek_next_char() -- Inspect the character after the cursor.
*
*   SYNOPSIS
*       static char peek_next_char (const char **c);
*
*   FUNCTION
*       Read the next character from the supplied command pointer without
*       advancing it.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
*   RESULT
*       Character one byte beyond the current cursor.
*
****************************************************************************
*
*/

static char peek_next_char (const char **c)
{
    const char *pc = *c;
    return pc[1];
}

/****i* januae-debugger/more_params *****************************************
*
*   NAME
*       more_params() -- Check for remaining command input.
*
*   SYNOPSIS
*       static int more_params (const char **c);
*
*   FUNCTION
*       Skip leading white space and test whether the command cursor has
*       reached the string terminator.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
*   RESULT
*       Nonzero when input remains, otherwise zero.
*
****************************************************************************
*
*/

static int more_params (const char **c)
{
    ignore_ws (c);
    return (**c) != 0;
}

/****i* januae-debugger/next_string *****************************************
*
*   NAME
*       next_string() -- Copy a command token.
*
*   SYNOPSIS
*       static int next_string (const char **c, char *out, unsigned int max,
*       int forceupper);
*
*   FUNCTION
*       Copy up to max - 1 characters into out, stopping after white space or
*       the length limit; optionally convert copied characters to upper case.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*       out - Writable destination string.
*       max - Capacity of the destination string in bytes.
*       forceupper - Nonzero to uppercase each copied character.
*
*   RESULT
*       Length of the resulting NUL-terminated string.
*
*   NOTES
*       The routine initially writes a terminator to out, so max must
*       describe a writable buffer of at least one byte. It stops on ASCII
*       space rather than all white-space characters.
*
****************************************************************************
*
*/

static int next_string (const char **c, char *out, unsigned int max, int forceupper)
{
    char *p = out;

    *p = 0;
    while (**c != 0) {
	if (**c == 32) {
	    ignore_ws (c);
	    return strlen (out);
	}
	*p = next_char (c);
	if (forceupper)
	    *p = toupper (*p);
	*++p = 0;
	max--;
	if (max <= 1)
	    break;
    }
    return strlen (out);
}

/****i* januae-debugger/nextaddr ********************************************
*
*   NAME
*       nextaddr() -- Advance through configured RAM regions.
*
*   SYNOPSIS
*       static uae_u32 nextaddr (uae_u32 addr);
*
*   FUNCTION
*       Advance a guest address, jumping between chip, fast and slow RAM at
*       their configured region boundaries. The all-ones address selects the
*       first search endpoint.
*
*   INPUTS
*       addr - Current guest search address, or the end sentinel.
*
*   RESULT
*       Next guest address, or 0xffffffff at the end of the configured search
*       progression.
*
*   NOTES
*       The all-ones sentinel initially selects the last configured RAM
*       endpoint, not address zero.
*
****************************************************************************
*
*/

static uae_u32 nextaddr (uae_u32 addr)
{
    if (addr == 0xffffffff) {
	if (currprefs.bogomem_size)
	    return 0xc00000 + currprefs.bogomem_size;
	if (currprefs.fastmem_size)
	    return 0x200000 + currprefs.fastmem_size;
	return currprefs.chipmem_size;
    }
    if (addr == currprefs.chipmem_size) {
	if (currprefs.fastmem_size)
	    return 0x200000;
	else if (currprefs.bogomem_size)
	    return 0xc00000;
	return 0xffffffff;
    }
    if (addr == 0x200000 + currprefs.fastmem_size) {
	if (currprefs.bogomem_size)
	    return 0xc00000;
	return 0xffffffff;
    }
    if (addr == 0xc00000 + currprefs.bogomem_size)
	return 0xffffffff;
    return addr + 1;
}

/****i* januae-debugger/dumpmem *********************************************
*
*   NAME
*       dumpmem() -- Display hexadecimal and character memory rows.
*
*   SYNOPSIS
*       static void dumpmem (uaecptr addr, uaecptr *nxmem, unsigned int
*       lines);
*
*   FUNCTION
*       Read sixteen guest bytes per row, format their hexadecimal values and
*       printable characters, and advance the next-dump address.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*       nxmem - Pointer receiving the next dump address.
*       lines - Number of output lines.
*
*   NOTES
*       The caller supplies storage for the resulting nxmem address; guest
*       reads use get_byte().
*
****************************************************************************
*
*/

static void dumpmem (uaecptr addr, uaecptr *nxmem, unsigned int lines)
{
    char line[80];
    int cols = 8;
    for (;lines--;) {
	int i;
	sprintf (line, "%08x ", addr);
	for (i = 0; i < cols; i++) {
	    uae_u8 b1 = get_byte (addr + 0);
	    uae_u8 b2 = get_byte (addr + 1);
	    addr += 2;
	    sprintf (line + 9 + i * 5, "%02x%02x ", b1, b2);
	    line[9 + cols * 5 + 1 + i * 2 + 0] = b1 >= 32 && b1 < 127 ? b1 : '.';
	    line[9 + cols * 5 + 1 + i * 2 + 1] = b2 >= 32 && b2 < 127 ? b2 : '.';
	}
	line[9 + cols * 5] = ' ';
	line[9 + cols * 5 + 1 + 2 * cols] = 0;
	console_out ("%s", line);
	console_out ("\n");
    }
    *nxmem = addr;
}

/****i* januae-debugger/dump_custom_regs ************************************
*
*   NAME
*       dump_custom_regs() -- Display current custom-chip register values.
*
*   SYNOPSIS
*       static void dump_custom_regs (void);
*
*   FUNCTION
*       Capture custom-register and four audio-channel SaveState images,
*       merge the audio register fields and print paired register names and
*       values.
*
*   NOTES
*       The function frees the allocated SaveState snapshots and assumes the
*       snapshot routines return usable buffers.
*
****************************************************************************
*
*/

static void dump_custom_regs (void)
{
    unsigned int len;
    unsigned int i, j, end;
    uae_u8 *p1, *p2, *p3, *p4;

    p1 = p2 = save_custom (&len, 0, 1);
    p1 += 4; // skip chipset type
    for (i = 0; i < 4; i++) {
	p4 = p1 + 0xa0 + i * 16;
	p3 = save_audio (i, &len, 0);
	p4[0] = p3[12];
	p4[1] = p3[13];
	p4[2] = p3[14];
	p4[3] = p3[15];
	p4[4] = p3[4];
	p4[5] = p3[5];
	p4[6] = p3[8];
	p4[7] = p3[9];
	p4[8] = 0;
	p4[9] = p3[1];
	p4[10] = p3[10];
	p4[11] = p3[11];
	free (p3);
    }
    end = 0;
    while (custd[end].name)
	end++;
    end++;
    end /= 2;
    for (i = 0; i < end; i++) {
	uae_u16 v1, v2;
	int addr1, addr2;
	j = end + i;
	addr1 = custd[i].adr & 0x1ff;
	addr2 = custd[j].adr & 0x1ff;
	v1 = (p1[addr1 + 0] << 8) | p1[addr1 + 1];
	v2 = (p1[addr2 + 0] << 8) | p1[addr2 + 1];
	console_out ("%3.3X %s\t%4.4X\t%3.3X %s\t%4.4X\n",
	    addr1, custd[i].name, v1,
	    addr2, custd[j].name, v2);
    }
    free (p2);
}

/****i* januae-debugger/dump_vectors ****************************************
*
*   NAME
*       dump_vectors() -- Display exception and trap vectors.
*
*   SYNOPSIS
*       static void dump_vectors (uaecptr addr);
*
*   FUNCTION
*       Print the known interrupt and trap vector names and the guest
*       longwords at their vector addresses. Use regs.vbr when addr is
*       0xffffffff.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*
*   NOTES
*       The reset vector at address four is read without adding the supplied
*       vector base.
*
****************************************************************************
*
*/

static void dump_vectors (uaecptr addr)
{
    unsigned int i = 0, j = 0;

    if (addr == 0xffffffff)
	addr = regs.vbr;

    while (int_labels[i].name || trap_labels[j].name) {
	if (int_labels[i].name) {
	    console_out ("$%08X: %s  \t $%08X\t", int_labels[i].adr + addr,
		int_labels[i].name, get_long (int_labels[i].adr + (int_labels[i].adr == 4 ? 0 : addr)));
	    i++;
	} else {
	    console_out ("\t\t\t\t");
	}
	if (trap_labels[j].name) {
	    console_out ("$%08X: %s  \t $%08X", trap_labels[j].adr + addr,
	       trap_labels[j].name, get_long (trap_labels[j].adr + addr));
	    j++;
	}
	console_out ("\n");
    }
}

/****i* januae-debugger/disassemble_wait ************************************
*
*   NAME
*       disassemble_wait() -- Describe a Copper WAIT or SKIP comparison.
*
*   SYNOPSIS
*       static void disassemble_wait (uae_u32 insn);
*
*   FUNCTION
*       Decode vertical and horizontal position masks and the
*       blitter-finished-disable bit from an encoded Copper instruction, and
*       print the resulting comparison.
*
*   INPUTS
*       insn - Encoded 32-bit Copper instruction.
*
****************************************************************************
*
*/

static void disassemble_wait (uae_u32 insn)
{
    unsigned int vp, hp, ve, he, bfd, v_mask, h_mask;

    vp = (insn & 0xff000000) >> 24;
    hp = (insn & 0x00fe0000) >> 16;
    ve = (insn & 0x00007f00) >> 8;
    he = (insn & 0x000000fe);
    bfd = (insn & 0x00008000) >> 15;

    /* bit15 can never be masked out*/
    v_mask = vp & (ve | 0x80);
    h_mask = hp & he;
    if (v_mask > 0) {
	console_out ("vpos ");
	if (ve != 0x7f) {
	    console_out ("& 0x%02x ", ve);
	}
	console_out (">= 0x%02x", v_mask);
    }
    if (he > 0) {
	if (v_mask > 0) {
	    console_out (" and");
	}
	console_out (" hpos ");
	if (he != 0xfe) {
	    console_out ("& 0x%02x ", he);
	}
	console_out (">= 0x%02x", h_mask);
    } else {
	console_out (", ignore horizontal");
    }

    console_out (".\n                        \t; VP %02x, VE %02x; HP %02x, HE %02x; BFD %d\n",
	     vp, ve, hp, he, bfd);
}

#define NR_COPPER_RECORDS 40000
/* Record copper activity for the debugger.  */
struct cop_record
{
    unsigned int hpos, vpos;
    uaecptr      addr;
};
static struct cop_record *cop_record[2];
static unsigned int nr_cop_records[2], curr_cop_set;

/****i* januae-debugger/record_copper_reset *********************************
*
*   NAME
*       record_copper_reset() -- Begin a fresh Copper trace frame.
*
*   SYNOPSIS
*       void record_copper_reset (void);
*
*   FUNCTION
*       Alternate between two Copper-record buffers and reset the record
*       count for the selected buffer.
*
****************************************************************************
*
*/

void record_copper_reset (void)
{
/* Start a new set of copper records.  */
    curr_cop_set ^= 1;
    nr_cop_records[curr_cop_set] = 0;
}

/****i* januae-debugger/record_copper ***************************************
*
*   NAME
*       record_copper() -- Record a Copper fetch and check trace triggers.
*
*   SYNOPSIS
*       void record_copper (uaecptr addr, unsigned int hpos, unsigned int
*       vpos);
*
*   FUNCTION
*       Append an instruction address and raster position until the
*       40000-record limit, then request debugger activation for armed trace
*       or matching address breakpoints.
*
*   INPUTS
*       addr - Guest address of the fetched Copper instruction.
*       hpos - Horizontal beam position of the fetch.
*       vpos - Vertical beam position of the fetch.
*
*   NOTES
*       Both record buffers are allocated on first use without checking
*       allocation failures; additional records beyond the limit are
*       discarded.
*
****************************************************************************
*
*/

void record_copper (uaecptr addr, unsigned int hpos, unsigned int vpos)
{
    unsigned int t = nr_cop_records[curr_cop_set];
    if (!cop_record[0]) {
	cop_record[0] = malloc (NR_COPPER_RECORDS * sizeof (struct cop_record));
	cop_record[1] = malloc (NR_COPPER_RECORDS * sizeof (struct cop_record));
    }
    if (t < NR_COPPER_RECORDS) {
	cop_record[curr_cop_set][t].addr = addr;
	cop_record[curr_cop_set][t].hpos = hpos;
	cop_record[curr_cop_set][t].vpos = vpos;
	nr_cop_records[curr_cop_set] = t + 1;
    }
    if (debug_copper & 2) { /* trace */
	debug_copper &= ~2;
	activate_debugger();
    }
    if ((debug_copper & 4) && addr >= debug_copper_pc && addr <= debug_copper_pc + 3) {
	debug_copper &= ~4;
	activate_debugger();
    }
}

/****i* januae-debugger/find_copper_record **********************************
*
*   NAME
*       find_copper_record() -- Find the last frame's Copper fetch position.
*
*   SYNOPSIS
*       static int find_copper_record (uaecptr addr, unsigned int *phpos,
*       unsigned int *pvpos);
*
*   FUNCTION
*       Search the previously active record buffer for the first entry with
*       the specified guest address.
*
*   INPUTS
*       addr - Copper instruction guest address to look up.
*       phpos - Output pointer receiving horizontal position.
*       pvpos - Output pointer receiving vertical position.
*
*   RESULT
*       One if found, otherwise zero.
*
*   NOTES
*       On success, writes the recorded horizontal and vertical positions to
*       the supplied output pointers.
*
****************************************************************************
*
*/

static int find_copper_record (uaecptr addr, unsigned int *phpos, unsigned int *pvpos)
{
    unsigned int s = curr_cop_set ^ 1;
    unsigned int t = nr_cop_records[s];
    unsigned int i;

    for (i = 0; i < t; i++) {
	if (cop_record[s][i].addr == addr) {
	    *phpos = cop_record[s][i].hpos;
	    *pvpos = cop_record[s][i].vpos;
	    return 1;
	}
    }
    return 0;
}

/* simple decode copper by Mark Cox */
/****i* januae-debugger/decode_copper_insn **********************************
*
*   NAME
*       decode_copper_insn() -- Disassemble one Copper instruction.
*
*   SYNOPSIS
*       static void decode_copper_insn (uae_u32 insn, uaecptr addr);
*
*   FUNCTION
*       Display the instruction words, optional recorded raster position,
*       current Copper-pointer marker and decoded MOVE, WAIT or SKIP
*       operation.
*
*   INPUTS
*       insn - Encoded 32-bit Copper instruction.
*       addr - Guest address containing the Copper instruction.
*
*   NOTES
*       The instruction is supplied as two packed 16-bit words. An
*       unrecognized instruction type causes abort().
*
****************************************************************************
*
*/

static void decode_copper_insn (uae_u32 insn, uaecptr addr)
{
    uae_u32 insn_type = insn & 0x00010001;
    unsigned int hpos, vpos;
    char here = ' ';
    char record[] = "          ";
    if (find_copper_record (addr, &hpos, &vpos)) {
	sprintf (record, " [%03x %03x]", vpos, hpos);
    }

    if (get_copper_address(-1) >= addr && get_copper_address(-1) <= addr + 3)
	here = '*';

    console_out ("%c%08x: %04x %04x%s\t; ", here, addr, insn >> 16, insn & 0xFFFF, record);

    switch (insn_type) {
    case 0x00010000: /* WAIT insn */
	console_out ("Wait for ");
	disassemble_wait (insn);

	if (insn == 0xfffffffe)
	    console_out ("                           \t; End of Copperlist\n");

	break;

    case 0x00010001: /* SKIP insn */
	console_out ("Skip if ");
	disassemble_wait (insn);
	break;

    case 0x00000000:
    case 0x00000001: /* MOVE insn */
	{
	    uae_u32 addr = (insn >> 16) & 0x1fe;
	    int i = 0;
	    while (custd[i].name) {
		if (custd[i].adr == addr + 0xdff000)
		    break;
		i++;
	    }
	    if (custd[i].name)
		console_out ("%s := 0x%04x\n", custd[i].name, insn & 0xffff);
	    else
		console_out ("%04x := 0x%04x\n", addr, insn & 0xffff);
	}
	break;

    default:
	abort ();
    }

}

/****i* januae-debugger/decode_copperlist ***********************************
*
*   NAME
*       decode_copperlist() -- Display a consecutive Copper instruction
*       sequence.
*
*   SYNOPSIS
*       static uaecptr decode_copperlist (uaecptr address, unsigned int
*       nolines);
*
*   FUNCTION
*       Read and decode nolines longwords from guest memory beginning at
*       address without treating the end-of-list instruction as a terminator.
*
*   INPUTS
*       address - First guest instruction address.
*       nolines - Number of Copper instructions to decode.
*
*   RESULT
*       Address immediately following the decoded instruction sequence.
*
****************************************************************************
*
*/

static uaecptr decode_copperlist (uaecptr address, unsigned int nolines)
{
    uae_u32 insn;
    while (nolines-- > 0) {
	insn = get_long (address);
	decode_copper_insn (insn, address);
	address += 4;
    }
    return address;
    /* You may wonder why I don't stop this at the end of the copperlist?
     * Well, often nice things are hidden at the end and it is debatable the actual
     * values that mean the end of the copperlist */
}

/****i* januae-debugger/copper_debugger *************************************
*
*   NAME
*       copper_debugger() -- Execute a Copper debugger subcommand.
*
*   SYNOPSIS
*       static int copper_debugger (const char **c);
*
*   FUNCTION
*       Toggle Copper logging, arm a one-instruction trace or address
*       breakpoint, or display Copper instructions at an explicit, current or
*       previously used address.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
*   RESULT
*       One when execution should resume for an armed trace; otherwise zero.
*
*   NOTES
*       Subcommands d, t and b operate on the global debug_copper flags.
*
****************************************************************************
*
*/

static int copper_debugger (const char **c)
{
    static uaecptr nxcopper;
    uae_u32 maddr;
    unsigned int lines;

    if (**c == 'd') {
	next_char (c);
	if (debug_copper)
	    debug_copper = 0;
	else
	    debug_copper = 1;
	console_out ("Copper debugger %s.\n", debug_copper ? "enabled" : "disabled");
    } else if (**c == 't') {
	debug_copper = 1|2;
	return 1;
    } else if (**c == 'b') {
	(*c)++;
	debug_copper = 1|4;
	if (more_params (c)) {
	    debug_copper_pc = readhex (c);
	    console_out ("Copper breakpoint @0x%8.8x\n", debug_copper_pc);
	} else {
	    debug_copper &= ~4;
	}
    } else {
	if (more_params (c)) {
	    maddr = readhex (c);
	    if (maddr == 1 || maddr == 2)
		maddr = get_copper_address (maddr);
	    else if (maddr == 0)
		maddr = get_copper_address (-1);
	} else
	    maddr = nxcopper;

	if (more_params (c))
	    lines = readhex (c);
	else
	    lines = 20;

	nxcopper = decode_copperlist (maddr, lines);
    }
    return 0;
}

/* cheat-search by Holger Jakob */
/****i* januae-debugger/cheatsearch *****************************************
*
*   NAME
*       cheatsearch() -- Narrow candidate addresses for a changing game
*       value.
*
*   SYNOPSIS
*       static void cheatsearch (const char **c);
*
*   FUNCTION
*       On the first invocation, scan allocated chip RAM for a matching
*       big-endian longword and retain up to 255 addresses. On the next
*       invocation, compare those addresses with a new value and discard the
*       candidate list.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
*   NOTES
*       The second search checks every one of the 255 slots, including
*       uninitialized entries beyond the found-count; candidate storage is
*       freed only after that search.
*
****************************************************************************
*
*/

static void cheatsearch (const char **c)
{
    uae_u8 *p = get_real_address (0);
    static uae_u32 *vlist = NULL;
    uae_u32 ptr;
    uae_u32 val = 0;
    uae_u32 type = 0; /* not yet */
    uae_u32 count = 0;
    uae_u32 fcount = 0;
    uae_u32 full = 0;

    ignore_ws (c);
    val = readhex (c);
    if (vlist == NULL) {
	vlist = malloc (256 * 4);
	if (vlist != 0) {
	    for (count = 0; count < 255; count++)
		vlist[count] = 0;
	    count = 0;
	    for (ptr = 0; ptr < allocated_chipmem - 40; ptr += 2, p += 2) {
		if (ptr >= 0x438 && p[3] == (val & 0xff)
		    && p[2] == (val >> 8 & 0xff)
		    && p[1] == (val >> 16 & 0xff)
		    && p[0] == (val >> 24 & 0xff))
		{
		    if (count < 255) {
			vlist[count++]=ptr;
			console_out ("%08x: %x%x%x%x\n",ptr,p[0],p[1],p[2],p[3]);
		    } else
			full = 1;
		}
	    }
	    console_out ("Found %d possible addresses with %d\n",count,val);
	    console_out ("Now continue with 'g' and use 'C' with a different value\n");
	}
    } else {
	for (count = 0; count<255; count++) {
	    if (p[vlist[count]+3] == (val & 0xff)
		&& p[vlist[count]+2] == (val>>8 & 0xff)
		&& p[vlist[count]+1] == (val>>16 & 0xff)
		&& p[vlist[count]] == (val>>24 & 0xff))
	    {
		fcount++;
		console_out ("%08x: %x%x%x%x\n", vlist[count], p[vlist[count]],
			p[vlist[count]+1], p[vlist[count]+2], p[vlist[count]+3]);
	    }
	}
	console_out ("%d hits of %d found\n",fcount,val);
	free (vlist);
	vlist = NULL;
    }
}

#define BREAKPOINT_TOTAL 8
struct breakpoint_node {
    uaecptr addr;
    int enabled;
};
static struct breakpoint_node bpnodes[BREAKPOINT_TOTAL];

static addrbank **debug_mem_banks;
#define MEMWATCH_TOTAL 4
struct memwatch_node {
    uaecptr addr;
    unsigned int size;
    int rw;
    uae_u32 val;
    int val_enabled;
    uae_u32 modval;
    int modval_written;
};
static struct memwatch_node mwnodes[MEMWATCH_TOTAL];
static struct memwatch_node mwhit;

static uae_u8 *illgdebug;
static int illgdebug_break;
extern int cdtv_enabled, cd32_enabled;

/****i* januae-debugger/illg_init *******************************************
*
*   NAME
*       illg_init() -- Build the illegal-memory-access permission image.
*
*   SYNOPSIS
*       static void illg_init (void);
*
*   FUNCTION
*       Allocate a 16 MiB permission table, initialize unassigned locations
*       as invalid and classify configured RAM, custom registers, CIAs, ROMs
*       and optional CDTV/CD32 or filesystem regions.
*
*   NOTES
*       The 24-bit permission image is global and reused by watched
*       guest-memory accesses. Failed allocation leaves the feature
*       unavailable.
*
****************************************************************************
*
*/

static void illg_init (void)
{
    unsigned int i;

    free (illgdebug);
    illgdebug = xmalloc (0x1000000);
    if (!illgdebug)
	return;
    memset (illgdebug, 3, 0x1000000);
    memset (illgdebug, 0, currprefs.chipmem_size);
    memset (illgdebug + 0xc00000, 0, currprefs.bogomem_size);
    memset (illgdebug + 0x200000, 0, currprefs.fastmem_size);
    i = 0;
    while (custd[i].name) {
	int rw = custd[i].rw;
	illgdebug[custd[i].adr] = rw;
	illgdebug[custd[i].adr + 1] = rw;
	i++;
    }
    for (i = 0; i < 16; i++) { /* CIAs */
	if (i == 11)
	    continue;
	illgdebug[0xbfe001 + i * 0x100] = 0;
	illgdebug[0xbfd000 + i * 0x100] = 0;
    }
    memset (illgdebug + 0xf80000, 1, 512 * 1024); /* KS ROM */
    memset (illgdebug + 0xdc0000, 0, 0x3f); /* clock */
#ifdef CDTV
    if (cdtv_enabled) {
	memset (illgdebug + 0xf00000, 1, 256 * 1024); /* CDTV ext ROM */
	memset (illgdebug + 0xdc8000, 0, 4096); /* CDTV batt RAM */
    }
#endif
#ifdef CD32
    if (cd32_enabled) {
	memset (illgdebug + AKIKO_BASE, 0, AKIKO_BASE_END - AKIKO_BASE);
	memset (illgdebug + 0xe00000, 1, 512 * 1024); /* CD32 ext ROM */
    }
#endif
    if (cloanto_rom)
	memset (illgdebug + 0xe00000, 1, 512 * 1024);
#ifdef FILESYS
    if (nr_units (currprefs.mountinfo) > 0) /* filesys "rom" */
	memset (illgdebug + RTAREA_BASE, 1, 0x10000);
#endif
}

/* add special custom register check here */
/****i* januae-debugger/illg_debug_check ************************************
*
*   NAME
*       illg_debug_check() -- Reserved hook for custom-register access
*       validation.
*
*   SYNOPSIS
*       static void illg_debug_check (uaecptr addr, int rw, int size, uae_u32
*       val);
*
*   FUNCTION
*       Return without inspecting the address, access kind, width or value.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*       rw - Nonzero for writes; zero for reads.
*       size - Size of the guest-memory region in bytes.
*       val - Access value for the memory operation.
*
*   NOTES
*       This function is an intentional no-op in the existing implementation.
*
****************************************************************************
*
*/

static void illg_debug_check (uaecptr addr, int rw, int size, uae_u32 val)
{
    return;
}

/****i* januae-debugger/illg_debug_do ***************************************
*
*   NAME
*       illg_debug_do() -- Log accesses that violate the permission image.
*
*   SYNOPSIS
*       static void illg_debug_do (uaecptr addr, int rw, int size, uae_u32
*       val);
*
*   FUNCTION
*       Inspect each byte of a guest memory access against the per-address
*       permission mask, log disallowed reads and writes and optionally
*       request debugger entry.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*       rw - Nonzero for writes; zero for reads.
*       size - Access width or address-range length in bytes.
*       val - Access value for the memory operation.
*
*   NOTES
*       Addresses at or above 16 MiB are treated as fully restricted; an
*       unset permission mask causes no diagnostic.
*
****************************************************************************
*
*/

static void illg_debug_do (uaecptr addr, int rw, int size, uae_u32 val)
{
    uae_u8 mask;
    uae_u32 pc = m68k_getpc (&regs);
    char rws = rw ? 'W' : 'R';
    int i;

    for (i = size - 1; i >= 0; i--) {
	uae_u8 v = val >> (i * 8);
	uae_u32 ad = addr + i;
	if (ad >= 0x1000000)
	    mask = 3;
	else
	    mask = illgdebug[ad];
	if (!mask)
	    continue;
	if (mask & 0x80) {
	    illg_debug_check (ad, rw, size, val);
	} else if ((mask & 3) == 3) {
	    if (rw)
		write_log ("RW: %8.8X=%2.2X %c PC=%8.8X\n", ad, v, rws, pc);
	    else
		write_log ("RW: %8.8X    %c PC=%8.8X\n", ad, rws, pc);
	    if (illgdebug_break)
		activate_debugger ();
	} else if ((mask & 1) && rw) {
	    write_log ("RO: %8.8X=%2.2X %c PC=%8.8X\n", ad, v, rws, pc);
	    if (illgdebug_break)
		activate_debugger ();
	} else if ((mask & 2) && !rw) {
	    write_log ("WO: %8.8X    %c PC=%8.8X\n", ad, rws, pc);
	    if (illgdebug_break)
		activate_debugger ();
	}
    }
}

/****i* januae-debugger/debug_mem_off ***************************************
*
*   NAME
*       debug_mem_off() -- Select a watched guest-memory bank.
*
*   SYNOPSIS
*       STATIC_INLINE uae_u8 debug_mem_off (uaecptr addr);
*
*   FUNCTION
*       Normalize the address to 24-bit space and return the bank number in
*       bits 16 through 23.
*
*   INPUTS
*       addr - Guest address to normalize to a bank index.
*
*   RESULT
*       Eight-bit memory-bank index.
*
****************************************************************************
*
*/

STATIC_INLINE uae_u8 debug_mem_off (uaecptr addr)
{
    return (munge24 (addr) >> 16) & 0xff;
}

/****i* januae-debugger/memwatch_func ***************************************
*
*   NAME
*       memwatch_func() -- Check one guest-memory access against watchpoints.
*
*   SYNOPSIS
*       static void memwatch_func (uaecptr addr, int rw, int size, uae_u32
*       val);
*
*   FUNCTION
*       Run illegal-access diagnostics when enabled, then test the four
*       configured read/write watchpoint ranges and optional value filters.
*       On a hit, store the access details and request a CPU break.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*       rw - Nonzero for writes; zero for reads.
*       size - Size of the guest access in bytes (1, 2 or 4).
*       val - Access value for the memory operation.
*
*   NOTES
*       For write-change watchpoints, the first matching write seeds the
*       comparison value and later equal writes are ignored.
*
****************************************************************************
*
*/

static void memwatch_func (uaecptr addr, int rw, int size, uae_u32 val)
{
    int i, brk;

    if (illgdebug)
	illg_debug_do (addr, rw, size, val);
    addr = munge24 (addr);
    for (i = 0; i < MEMWATCH_TOTAL; i++) {
	uaecptr addr2 = mwnodes[i].addr;
	uaecptr addr3 = addr2 + mwnodes[i].size;
	int rw2 = mwnodes[i].rw;

	brk = 0;
	if (mwnodes[i].size == 0)
	    continue;
	if (mwnodes[i].val_enabled && mwnodes[i].val != val)
	    continue;
	if (rw != rw2 && rw2 < 2)
	    continue;
	if (addr >= addr2 && addr < addr3)
	    brk = 1;
	if (!brk && size == 2 && (addr + 1 >= addr2 && addr + 1 < addr3))
	    brk = 1;
	if (!brk && size == 4 && ((addr + 2 >= addr2 && addr + 2 < addr3) || (addr + 3 >= addr2 && addr + 3 < addr3)))
	    brk = 1;
	if (brk && mwnodes[i].modval_written) {
	    if (!rw) {
		brk = 0;
	    } else if (mwnodes[i].modval_written == 1) {
		mwnodes[i].modval_written = 2;
		mwnodes[i].modval = val;
		brk = 0;
	    } else if (mwnodes[i].modval == val) {
		brk = 0;
	    }
	}
	if (brk) {
	    mwhit.addr = addr;
	    mwhit.rw = rw;
	    mwhit.size = size;
	    mwhit.val = 0;
	    if (mwhit.rw)
		mwhit.val = val;
	    memwatch_triggered = i + 1;
	    debugging = 1;
	    set_special (&regs, SPCFLAG_BRK);
	    break;
	}
    }
}

/****i* januae-debugger/debug_lget ******************************************
*
*   NAME
*       debug_lget() -- Read a watched longword.
*
*   SYNOPSIS
*       static REGPARAM2 uae_u32 debug_lget (uaecptr addr);
*
*   FUNCTION
*       Delegate the read to the saved original memory-bank handler, then
*       evaluate watchpoints using the returned longword.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*
*   RESULT
*       Value returned by the original bank handler.
*
*   NOTES
*       Requires initialized watched-bank forwarding state.
*
****************************************************************************
*
*/

static REGPARAM2 uae_u32 debug_lget (uaecptr addr)
{
    uae_u8 off = debug_mem_off (addr);
    uae_u32 v;
    v = debug_mem_banks[off]->lget(addr);
    memwatch_func (addr, 0, 4, v);
    return v;
}
/****i* januae-debugger/debug_wget ******************************************
*
*   NAME
*       debug_wget() -- Read a watched word.
*
*   SYNOPSIS
*       static REGPARAM2 uae_u32 debug_wget (uaecptr addr);
*
*   FUNCTION
*       Delegate the read to the saved original memory-bank handler, then
*       evaluate watchpoints using the returned word.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*
*   RESULT
*       Value returned by the original bank handler.
*
*   NOTES
*       Requires initialized watched-bank forwarding state.
*
****************************************************************************
*
*/

static REGPARAM2 uae_u32 debug_wget (uaecptr addr)
{
    uae_u8 off = debug_mem_off (addr);
    uae_u32 v;
    v = debug_mem_banks[off]->wget(addr);
    memwatch_func (addr, 0, 2, v);
    return v;
}
/****i* januae-debugger/debug_bget ******************************************
*
*   NAME
*       debug_bget() -- Read a watched byte.
*
*   SYNOPSIS
*       static REGPARAM2 uae_u32 debug_bget (uaecptr addr);
*
*   FUNCTION
*       Delegate the read to the saved original memory-bank handler, then
*       evaluate watchpoints using the returned byte.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*
*   RESULT
*       Value returned by the original bank handler.
*
*   NOTES
*       Requires initialized watched-bank forwarding state.
*
****************************************************************************
*
*/

static REGPARAM2 uae_u32 debug_bget (uaecptr addr)
{
    uae_u8 off = debug_mem_off (addr);
    uae_u32 v;
    v = debug_mem_banks[off]->bget(addr);
    memwatch_func (addr, 0, 1, v);
    return v;
}
/****i* januae-debugger/debug_lput ******************************************
*
*   NAME
*       debug_lput() -- Write a watched longword.
*
*   SYNOPSIS
*       static REGPARAM2 void debug_lput (uaecptr addr, uae_u32 v);
*
*   FUNCTION
*       Evaluate watchpoints for the supplied longword value, then delegate
*       the write to the saved original memory-bank handler.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*       v - Value written to guest memory.
*
*   NOTES
*       Requires initialized watched-bank forwarding state.
*
****************************************************************************
*
*/

static REGPARAM2 void debug_lput (uaecptr addr, uae_u32 v)
{
    uae_u8 off = debug_mem_off (addr);
    memwatch_func (addr, 1, 4, v);
    debug_mem_banks[off]->lput(addr, v);
}
/****i* januae-debugger/debug_wput ******************************************
*
*   NAME
*       debug_wput() -- Write a watched word.
*
*   SYNOPSIS
*       static REGPARAM2 void debug_wput (uaecptr addr, uae_u32 v);
*
*   FUNCTION
*       Evaluate watchpoints for the supplied word value, then delegate the
*       write to the saved original memory-bank handler.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*       v - Value written to guest memory.
*
*   NOTES
*       Requires initialized watched-bank forwarding state.
*
****************************************************************************
*
*/

static REGPARAM2 void debug_wput (uaecptr addr, uae_u32 v)
{
    uae_u8 off = debug_mem_off (addr);
    memwatch_func (addr, 1, 2, v);
    debug_mem_banks[off]->wput(addr, v);
}
/****i* januae-debugger/debug_bput ******************************************
*
*   NAME
*       debug_bput() -- Write a watched byte.
*
*   SYNOPSIS
*       static REGPARAM2 void debug_bput (uaecptr addr, uae_u32 v);
*
*   FUNCTION
*       Evaluate watchpoints for the supplied byte value, then delegate the
*       write to the saved original memory-bank handler.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*       v - Value written to guest memory.
*
*   NOTES
*       Requires initialized watched-bank forwarding state.
*
****************************************************************************
*
*/

static REGPARAM2 void debug_bput (uaecptr addr, uae_u32 v)
{
    uae_u8 off = debug_mem_off (addr);
    memwatch_func (addr, 1, 1, v);
    debug_mem_banks[off]->bput(addr, v);
}
/****i* januae-debugger/debug_check *****************************************
*
*   NAME
*       debug_check() -- Forward a watched bank range check.
*
*   SYNOPSIS
*       static REGPARAM2 int debug_check (uaecptr addr, uae_u32 size);
*
*   FUNCTION
*       Delegate address-range validation to the saved original memory bank.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*       size - Size of the guest-memory region in bytes.
*
*   RESULT
*       Original bank check result.
*
*   NOTES
*       Intended for the 24-bit address-space watchpoint mode.
*
****************************************************************************
*
*/

static REGPARAM2 int debug_check (uaecptr addr, uae_u32 size)
{
    return debug_mem_banks[munge24 (addr) >> 16]->check (addr, size);
}
/****i* januae-debugger/debug_xlate *****************************************
*
*   NAME
*       debug_xlate() -- Forward watched bank pointer translation.
*
*   SYNOPSIS
*       static REGPARAM2 uae_u8 *debug_xlate (uaecptr addr);
*
*   FUNCTION
*       Request the underlying original memory bank's direct address
*       translation.
*
*   INPUTS
*       addr - Guest memory address or instruction address.
*
*   RESULT
*       Pointer returned by the original bank handler.
*
*   NOTES
*       No allocation or independent range validation is performed.
*
****************************************************************************
*
*/

static REGPARAM2 uae_u8 *debug_xlate (uaecptr addr)
{
    return debug_mem_banks[munge24 (addr) >> 16]->xlateaddr (addr);
}

/****i* januae-debugger/deinitialize_memwatch *******************************
*
*   NAME
*       deinitialize_memwatch() -- Remove watched-bank forwarding hooks.
*
*   SYNOPSIS
*       static void deinitialize_memwatch (void);
*
*   FUNCTION
*       Restore all 256 original memory-bank descriptors, free their saved
*       copies and the illegal-access permission image, and clear the enabled
*       flag.
*
*   NOTES
*       Does nothing when watchpoint forwarding is not enabled.
*
****************************************************************************
*
*/

static void deinitialize_memwatch (void)
{
    unsigned int i;

    if (!memwatch_enabled)
	return;
    for (i = 0; i < 256; i++) {
	addrbank *a1 = debug_mem_banks[i];
	addrbank *a2 = mem_banks[i];
	memcpy (a2, a1, sizeof (addrbank));
	free (a1);
    }
    free (debug_mem_banks);
    debug_mem_banks = 0;
    memwatch_enabled = 0;
    free (illgdebug);
    illgdebug = 0;
}

/****i* januae-debugger/initialize_memwatch *********************************
*
*   NAME
*       initialize_memwatch() -- Install watched-bank forwarding hooks.
*
*   SYNOPSIS
*       static int initialize_memwatch (void);
*
*   FUNCTION
*       In 24-bit address mode, save the original 256 bank descriptors, then
*       replace their read, write, range-check and address-translation
*       callbacks with debugging wrappers.
*
*   RESULT
*       One after installation; zero when 24-bit address mode is disabled.
*
*   NOTES
*       The allocation calls are not checked for failure before their results
*       are used.
*
****************************************************************************
*
*/

static int initialize_memwatch (void)
{
    unsigned int i;
    addrbank *a1, *a2;

    if (!currprefs.address_space_24)
	return 0;
    debug_mem_banks = xmalloc (sizeof (addrbank*) * 256);
    for (i = 0; i < 256; i++) {
	a1 = debug_mem_banks[i] = xmalloc (sizeof (addrbank));
	a2 = mem_banks[i];
	memcpy (a1, a2, sizeof (addrbank));
    }
    for (i = 0; i < 256; i++) {
	a2 = mem_banks[i];
	a2->bget = debug_bget;
	a2->wget = debug_wget;
	a2->lget = debug_lget;
	a2->bput = debug_bput;
	a2->wput = debug_wput;
	a2->lput = debug_lput;
	a2->check = debug_check;
	a2->xlateaddr = debug_xlate;
    }
    memwatch_enabled = 1;
    return 1;
}

/****i* januae-debugger/memwatch_dump ***************************************
*
*   NAME
*       memwatch_dump() -- Display configured memory watchpoints.
*
*   SYNOPSIS
*       static void memwatch_dump (int num);
*
*   FUNCTION
*       Print the active watchpoint indices, address ranges, access
*       directions, optional value filters and write-change markers.
*
*   INPUTS
*       num - Watchpoint index, or negative to display all slots.
*
*   NOTES
*       A negative num selects all entries; a nonnegative num selects only
*       the matching slot.
*
****************************************************************************
*
*/

static void memwatch_dump (int num)
{
    unsigned int i;
    const struct memwatch_node *mwn;
    for (i = 0; i < MEMWATCH_TOTAL; i++) {
	if ((num >= 0 && num == (int)i) || (num < 0)) {
	    mwn = &mwnodes[i];
	    if (mwn->size == 0)
		continue;
	    console_out ("%d: %8.8X - %8.8X (%d) %s",
		i, mwn->addr, mwn->addr + (mwn->size - 1), mwn->size,
		mwn->rw == 0 ? "R" : (mwn->rw == 1 ? "W" : "RW"));
	    if (mwn->val_enabled)
		console_out (" =%X", mwn->val);
	    if (mwn->modval_written)
		console_out (" =M");
	    console_out ("\n");
	}
    }
}

/****i* januae-debugger/memwatch ********************************************
*
*   NAME
*       memwatch() -- Manage memory watchpoints and illegal-access logging.
*
*   SYNOPSIS
*       static void memwatch (const char **c);
*
*   FUNCTION
*       Enable watchpoint forwarding on first use; list, remove or configure
*       one of four watchpoints, disable forwarding, or manage illegal-access
*       logging and excluded addresses.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
*   NOTES
*       Watchpoints are only available in 24-bit address mode. The d command
*       may allocate a 16 MiB permission image; a following d command can
*       clear a range in that image.
*
****************************************************************************
*
*/

static void memwatch (const char **c)
{
    int num;
    struct memwatch_node *mwn;
    char nc;

    if (!memwatch_enabled) {
	if (!initialize_memwatch ()) {
	    console_out ("Memwatch breakpoints require 24-bit address space\n");
	    return;
	}
	console_out ("Memwatch breakpoints enabled\n");
    }

    ignore_ws (c);
    if (!more_params (c)) {
	memwatch_dump (-1);
	return;
    }
    nc = next_char (c);
    if (nc == '-') {
	deinitialize_memwatch ();
	console_out ("Memwatch breakpoints disabled\n");
	return;
    }
    if (nc == 'd') {
	if (illgdebug) {
	    ignore_ws (c);
	    if (more_params (c)) {
		uae_u32 addr = readhex (c);
		uae_u32 len = 1;
		if (more_params (c))
		    len = readhex (c);
		write_log ("cleared logging addresses %8.8X - %8.8X\n", addr, addr + len);
		while (len > 0) {
		    addr &= 0xffffff;
		    illgdebug[addr] = 0;
		    addr++;
		    len--;
		}
	    }
	} else {
	    illg_init ();
	    console_out ("Illegal memory access logging enabled\n");
	    ignore_ws (c);
	    illgdebug_break = 0;
	    if (more_params (c))
		illgdebug_break = 1;
	}
	return;
    }
    num = nc - '0';
    if (num < 0 || num >= MEMWATCH_TOTAL)
	return;
    mwn = &mwnodes[num];
    mwn->size = 0;
    ignore_ws (c);
    if (!more_params (c)) {
	console_out ("Memwatch %d removed\n", num);
	return;
    }
    mwn->addr = readhex (c);
    mwn->size = 1;
    mwn->rw = 2;
    mwn->val_enabled = 0;
    mwn->modval_written = 0;
    ignore_ws (c);
    if (more_params (c)) {
	mwn->size = readhex (c);
	ignore_ws (c);
	if (more_params (c)) {
	    char nc = toupper (next_char (c));
	    if (nc == 'W')
		mwn->rw = 1;
	    else if (nc == 'R' && toupper (**c) != 'W')
		mwn->rw = 0;
	    else if (nc == 'R' && toupper (**c) == 'W')
		next_char (c);
	    ignore_ws (c);
	    if (more_params (c)) {
		if (toupper (**c) == 'M') {
		    mwn->modval_written = 1;
		} else {
		    mwn->val = readhex (c);
		    mwn->val_enabled = 1;
		}
	    }
	}
    }
    memwatch_dump (num);
}

/****i* januae-debugger/writeintomem ****************************************
*
*   NAME
*       writeintomem() -- Write a numeric value to guest memory.
*
*   SYNOPSIS
*       static void writeintomem (const char **c);
*
*   FUNCTION
*       Parse a hexadecimal guest address and value, then select an 8-, 16-
*       or 32-bit guest write according to the value magnitude.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
*   NOTES
*       The width follows the numeric value, not an explicit command-size
*       parameter.
*
****************************************************************************
*
*/

static void writeintomem (const char **c)
{
    uae_u32 addr = 0;
    uae_u32 val = 0;
    char cc;

    ignore_ws (c);
    addr = readhex (c);
    ignore_ws (c);
    val = readhex (c);
    if (val > 0xffff) {
	put_long (addr, val);
	cc = 'L';
    } else if (val > 0xff) {
	put_word (addr, val);
	cc = 'W';
    } else {
	put_byte (addr, val);
	cc = 'B';
    }
    console_out ("Wrote %x (%u) at %08x.%c\n", val, val, addr, cc);
}

/****i* januae-debugger/show_exec_tasks *************************************
*
*   NAME
*       show_exec_tasks() -- Display Exec task lists.
*
*   SYNOPSIS
*       static void show_exec_tasks (void);
*
*   FUNCTION
*       Read the guest ExecBase pointer from address four, print the current
*       task and traverse the ready and waiting task lists while following
*       guest successor pointers.
*
*   NOTES
*       The traversal assumes valid guest task-list pointers and reads names
*       through direct guest-memory translation.
*
****************************************************************************
*
*/

static void show_exec_tasks (void)
{
    uaecptr execbase = get_long (4);
    uaecptr taskready = get_long (execbase + 406);
    uaecptr taskwait = get_long (execbase + 420);
    uaecptr node, end;
    console_out ("execbase at 0x%08lx\n", (unsigned long) execbase);
    console_out ("Current:\n");
    node = get_long (execbase + 276);
    console_out ("%08x: %08x %s\n", node, 0, get_real_address (get_long (node + 10)));
    console_out ("Ready:\n");
    node = get_long (taskready);
    end = get_long (taskready + 4);
    while (node) {
	console_out ("%08x: %08x %s\n", node, 0, get_real_address (get_long (node + 10)));
	node = get_long (node);
    }
    console_out ("Waiting:\n");
    node = get_long (taskwait);
    end = get_long (taskwait + 4);
    while (node) {
	console_out ("%08x: %08x %s\n", node, 0, get_real_address (get_long (node + 10)));
	node = get_long (node);
    }
}

static int trace_same_insn_count;
static uae_u8 trace_insn_copy[10];
static struct regstruct trace_prev_regs;
static uaecptr nextpc;

/****i* januae-debugger/instruction_breakpoint ******************************
*
*   NAME
*       instruction_breakpoint() -- Configure instruction tracing and
*       breakpoints.
*
*   SYNOPSIS
*       static int instruction_breakpoint (const char **c);
*
*   FUNCTION
*       Handle return-opcode tracing, list or clear eight breakpoints, toggle
*       an address breakpoint, configure an address-range skip or start an
*       instruction trace.
*
*   INPUTS
*       c - Input command cursor; advanced past consumed characters.
*
*   RESULT
*       One if execution should resume, otherwise zero.
*
*   NOTES
*       A sentinel address of 0xC0DEDBAD opens uae.trace and enables
*       instruction-state logging; the trace file-open result is not checked.
*
****************************************************************************
*
*/

static int instruction_breakpoint (const char **c)
{
    struct breakpoint_node *bpn;
    int i;

    if (more_params (c)) {
	char nc = toupper ((*c)[0]);
	if (nc == 'I') {
	    next_char (c);
	    if (more_params (c))
		skipins = readhex (c);
	    else
		skipins = 0x10000;
	    do_skip = 1;
	    skipaddr_doskip = 1;
	    return 1;
	} else if (nc == 'D' && (*c)[1] == 0) {
	    for (i = 0; i < BREAKPOINT_TOTAL; i++)
		bpnodes[i].enabled = 0;
	    console_out ("All breakpoints removed\n");
	    return 0;
	} else if (nc == 'L') {
	    int got = 0;
	    for (i = 0; i < BREAKPOINT_TOTAL; i++) {
		bpn = &bpnodes[i];
		if (!bpn->enabled)
		    continue;
		console_out ("%8X ", bpn->addr);
		got = 1;
	    }
	    if (!got)
		console_out ("No breakpoints\n");
	    else
		console_out ("\n");
	    return 0;
	}
	skipaddr_doskip = 1;
	skipaddr_start = readhex (c);
	if (more_params (c)) {
	    skipaddr_end = readhex (c);
	} else {
	    for (i = 0; i < BREAKPOINT_TOTAL; i++) {
		bpn = &bpnodes[i];
		if (bpn->enabled && bpn->addr == skipaddr_start) {
		    bpn->enabled = 0;
		    console_out ("Breakpoint removed\n");
		    skipaddr_start = 0xffffffff;
		    skipaddr_doskip = 0;
		    return 0;
		}
	    }
	    for (i = 0; i < BREAKPOINT_TOTAL; i++) {
		bpn = &bpnodes[i];
		if (bpn->enabled)
		    continue;
		bpn->addr = skipaddr_start;
		bpn->enabled = 1;
		console_out ("Breakpoint added\n");
		skipaddr_start = 0xffffffff;
		skipaddr_doskip = 0;
		break;
	    }
	    return 0;
	}
    }
    if (skipaddr_start == 0xC0DEDBAD) {
	trace_same_insn_count = 0;
	logfile = fopen ("uae.trace", "w");
	memcpy (trace_insn_copy, regs.pc_p, 10);
	memcpy (&trace_prev_regs, &regs, sizeof regs);
    }
    do_skip = 1;
    skipaddr_doskip = -1;
    return 1;
}

/****i* januae-debugger/savemem *********************************************
*
*   NAME
*       savemem() -- Save a region of guest memory to a host file.
*
*   SYNOPSIS
*       static void savemem (const char **cc);
*
*   FUNCTION
*       Parse a filename, hexadecimal start address and byte count, then read
*       guest memory one byte at a time and write it to a binary host file.
*
*   INPUTS
*       cc - Input command cursor; advanced past consumed characters.
*
*   NOTES
*       On a failed fopen(), the allocated filename is not freed. A short
*       write stops the copy and prints a diagnostic.
*
****************************************************************************
*
*/

static void savemem (const char **cc)
{
    uae_u8 b;
    uae_u32 src, src2, len, len2;
    const char *start;
    char *name = 0;
    int name_len;

    FILE *fp;

    if (!more_params (cc))
	goto S_argh;

    start = *cc;
    while (**cc != '\0' && !isspace (**cc))
	(*cc)++;
    if (!isspace (**cc))
	goto S_argh;
    name_len = *cc - start;
    name = malloc (name_len + 1);
    if (!name)
	goto S_argh;
    strncpy (name, start, name_len);
    name[name_len] = '\0';

    (*cc)++;
    if (!more_params (cc))
	goto S_argh;
    src2 = src = readhex (cc);
    if (!more_params (cc))
	goto S_argh;
    len2 = len = readhex (cc);

    fp = fopen (name, "wb");
    if (fp == NULL) {
	console_out ("Couldn't open file '%s'\n", name);
	return;
    }
    while (len > 0) {
	b = get_byte (src);
	src++;
	len--;
	if (fwrite (&b, 1, 1, fp) != 1) {
	    console_out ("Error writing file\n");
	    break;
	}
    }
    fclose (fp);
    if (len == 0)
	console_out ("Wrote %08X - %08X (%d bytes) to '%s'\n",
	    src2, src2 + len2 - 1, len2, name);
    free (name);
    return;
S_argh:
    if (name)
	free (name);
    console_out ("S-command needs more arguments!\n");
}

/****i* januae-debugger/searchmem *******************************************
*
*   NAME
*       searchmem() -- Search guest RAM for bytes or case-insensitive text.
*
*   SYNOPSIS
*       static void searchmem (const char **cc);
*
*   FUNCTION
*       Parse a quoted string or sequence of hexadecimal byte pairs,
*       optionally read a start and end address, then scan configured guest
*       memory and display matching addresses.
*
*   INPUTS
*       cc - Input command cursor; advanced past consumed characters.
*
*   NOTES
*       The 256-byte pattern buffer has no explicit length guard. The search
*       reports at most 101 matches before stopping.
*
****************************************************************************
*
*/

static void searchmem (const char **cc)
{
    int i, sslen, got, val, stringmode;
    uae_u8 ss[256];
    uae_u32 addr, endaddr;
    char nc;

    got = 0;
    sslen = 0;
    stringmode = 0;
    ignore_ws (cc);
    if (**cc == '"') {
	stringmode = 1;
	(*cc)++;
	while (**cc != '"' && **cc != 0) {
	    ss[sslen++] = tolower (**cc);
	    (*cc)++;
	}
	if (**cc != 0)
	    (*cc)++;
    } else {
	for (;;) {
	    if (**cc == 32 || **cc == 0)
		break;
	    nc = toupper (next_char (cc));
	    if (isspace (nc))
		break;
	    if (isdigit(nc))
		val = nc - '0';
	    else
		val = nc - 'A' + 10;
	    if (val < 0 || val > 15)
		return;
	    val *= 16;
	    if (**cc == 32 || **cc == 0)
		break;
	    nc = toupper (next_char (cc));
	    if (isspace (nc))
		break;
	    if (isdigit(nc))
		val += nc - '0';
	    else
		val += nc - 'A' + 10;
	    if (val < 0 || val > 255)
		return;
	    ss[sslen++] = (uae_u8)val;
	}
    }
    if (sslen == 0)
	return;
    ignore_ws (cc);
    addr = 0;
    endaddr = nextaddr (0xffffffff);
    if (more_params (cc)) {
	addr = readhex (cc);
	if (more_params (cc))
	    endaddr = readhex (cc);
    }
    console_out ("Searching from %08x to %08x..\n", addr, endaddr);
    while (addr < endaddr && addr != 0xffffffff) {
	for (i = 0; i < sslen; i++) {
	    uae_u8 b = get_byte (addr + i);
	    if (stringmode) {
		if (tolower (b) != ss[i])
		    break;
	    } else {
		if (b != ss[i])
		    break;
	    }
	}
	if (i == sslen) {
	    got++;
	    console_out (" %08x", addr);
	    if (got > 100) {
		console_out ("\nMore than 100 results, aborting..");
		break;
	    }
	}
	addr = nextaddr (addr);
    }
    if (!got)
	console_out ("nothing found");
    console_out ("\n");
}

#ifdef SAVESTATE
/****i* januae-debugger/staterecorder ***************************************
*
*   NAME
*       staterecorder() -- Handle a debugger SaveState rewind command.
*
*   SYNOPSIS
*       static int staterecorder (const char **cc);
*
*   FUNCTION
*       Request rewinding to a prior captured state when no subcommand is
*       supplied, or list available rewind positions for the l subcommand.
*
*   INPUTS
*       cc - Input command cursor; advanced past consumed characters.
*
*   RESULT
*       One when a rewind request succeeds and execution should resume;
*       otherwise zero.
*
*   NOTES
*       Compiled only with SAVESTATE.
*
****************************************************************************
*
*/

static int staterecorder (const char **cc)
{
    char nc;

    if (!more_params (cc)) {
	if (savestate_dorewind (1)) {
	    debug_rewind = 1;
	    return 1;
	}
	return 0;
    }
    nc = next_char (cc);
    if (nc == 'l') {
	savestate_listrewind ();
	return 0;
    }
    return 0;
}
#endif

/****i* januae-debugger/disk_debug ******************************************
*
*   NAME
*       disk_debug() -- Configure disk-access break logging.
*
*   SYNOPSIS
*       static void disk_debug (const char **inptr);
*
*   FUNCTION
*       Parse DMA-read, DMA-write and PIO mode letters, then optionally
*       select a track number and enable extended disk logging.
*
*   INPUTS
*       inptr - Input command cursor; advanced past consumed characters.
*
*   NOTES
*       Invalid or out-of-range track selections are normalized to -1,
*       meaning all tracks.
*
****************************************************************************
*
*/

static void disk_debug (const char **inptr)
{
    char parm[10];
    unsigned int i;

    disk_debug_logging = 2;
    disk_debug_mode = 0;
    disk_debug_track = -1;
    ignore_ws (inptr);
    if (!next_string (inptr, parm, sizeof (parm), 1))
	goto end;
    for (i = 0; i < strlen (parm); i++) {
	if (parm[i] == 'R')
	    disk_debug_mode |= DISK_DEBUG_DMA_READ;
	if (parm[i] == 'W')
	    disk_debug_mode |= DISK_DEBUG_DMA_WRITE;
	if (parm[i] == 'P')
	    disk_debug_mode |= DISK_DEBUG_PIO;
    }
    if (more_params (inptr))
	disk_debug_track = readint (inptr);
    if (disk_debug_track < 0 || disk_debug_track > 2 * 83)
	disk_debug_track = -1;
end:
    console_out ("disk breakpoint mode %c%c%c track %d\n",
	disk_debug_mode & DISK_DEBUG_DMA_READ ? 'R' : '-',
	disk_debug_mode & DISK_DEBUG_DMA_WRITE ? 'W' : '-',
	disk_debug_mode & DISK_DEBUG_PIO ? 'P' : '-',
	disk_debug_track);
}

/****i* januae-debugger/m68k_modify *****************************************
*
*   NAME
*       m68k_modify() -- Modify a debugger-visible CPU register.
*
*   SYNOPSIS
*       static void m68k_modify (const char **inptr);
*
*   FUNCTION
*       Parse a register name and hexadecimal value, update an address/data
*       register, instruction prefetch field, VBR or stack/status register,
*       and recalculate register state when required.
*
*   INPUTS
*       inptr - Input command cursor; advanced past consumed characters.
*
*   NOTES
*       Register indices after A or D are accepted as digits without an 0..7
*       range check; the CCR branch replaces only the low four SR bits.
*
****************************************************************************
*
*/

static void m68k_modify (const char **inptr)
{
    uae_u32 v;
    char parm[10];
    unsigned char c1, c2;

    if (!next_string (inptr, parm, sizeof (parm), 1))
	return;
    c1 = toupper (parm[0]);
    c2 = toupper (parm[1]);
    if (c1 == 'A' || c1 == 'D' || c1 == 'P') {
	if (!isdigit (c2))
	    return;
	c2 -= '0';
    }
    v = readhex (inptr);
    if (c1 == 'A')
	regs.regs[8 + c2] = v;
    else if (c1 == 'D')
	regs.regs[c2] = v;
    else if (c1 == 'P' && c2 == 0)
	regs.irc = v;
    else if (c1 == 'P' && c2 == 1)
	regs.ir = v;
    else if (!strcmp (parm, "VBR"))
	regs.vbr = v;
    else if (!strcmp (parm, "USP")) {
	regs.usp = v;
	MakeFromSR (&regs);
    } else if (!strcmp (parm, "ISP")) {
	regs.isp = v;
	MakeFromSR (&regs);
    } else if (!strcmp (parm, "MSP")) {
	regs.msp = v;
	MakeFromSR (&regs);
    } else if (!strcmp (parm, "SR")) {
	regs.sr = v;
	MakeFromSR (&regs);
    } else if (!strcmp (parm, "CCR")) {
	regs.sr = (regs.sr & ~15) | (v & 15);
	MakeFromSR (&regs);
    }
}

/****i* januae-debugger/debug_1 *********************************************
*
*   NAME
*       debug_1() -- Run the interactive debugger command loop.
*
*   SYNOPSIS
*       static void debug_1 (void);
*
*   FUNCTION
*       Display the CPU state, read console commands, perform inspection and
*       modification actions and resume or terminate emulation on the
*       appropriate command.
*
*   NOTES
*       An end-of-file result from console_get() causes the input loop to
*       retry rather than exit.
*
****************************************************************************
*
*/

static void debug_1 (void)
{
    char input[80];
    uaecptr nxdis, nxmem, addr;

    m68k_dumpstate (stdout, &nextpc);
    nxdis = nextpc; nxmem = 0;

    for (;;) {
	char cmd;
	const char *inptr;

	console_out (">");
	console_flush ();
	if (!console_get (input, 80))
	    continue;
	inptr = input;
	cmd = next_char (&inptr);
	switch (cmd) {
	case 'c': dumpcia (); dumpdisk (); dumpcustom (); break;
	case 'i':
	    addr = 0xffffffff;
	    if (more_params (&inptr))
		addr = readhex (&inptr);
	    dump_vectors (addr);
	break;
	case 'e': dump_custom_regs (); break;
	case 'r': if (more_params (&inptr))
		      m68k_modify (&inptr);
		  else
		      m68k_dumpstate (stdout, &nextpc);
	break;
	case 'C': cheatsearch (&inptr); break;
	case 'W': writeintomem (&inptr); break;
	case 'w': memwatch (&inptr); break;
	case 'S': savemem (&inptr); break;
	case 's':
	    if (*inptr == 'c') {
//		screenshot (1, 1);
	    } else if (*inptr == 'm') {
		next_char (&inptr);
		if (more_params (&inptr))
		    debug_sprite_mask = readint (&inptr);
		console_out ("sprite mask: %2.2X\n", debug_sprite_mask);
	    } else {
		searchmem (&inptr);
	    }
	break;
	case 'd':
	{
	    if (*inptr == 'i') {
		next_char (&inptr);
		disk_debug (&inptr);
	    } else {
		uae_u32 daddr;
		int count;
		if (more_params (&inptr))
		    daddr = readhex (&inptr);
		else
		    daddr = nxdis;
		if (more_params (&inptr))
		    count = readhex (&inptr);
		else
		    count = 10;
		m68k_disasm (stdout, daddr, &nxdis, count);
	    }
	}
	break;
	case 'T': show_exec_tasks (); break;
	case 't':
	    if (more_params (&inptr))
		skipaddr_doskip = readint (&inptr);
	    if (skipaddr_doskip <= 0 || skipaddr_doskip > 10000)
		skipaddr_doskip = 1;
	    set_special (&regs, SPCFLAG_BRK);
	    exception_debugging = 1;
	    return;
	case 'z':
	    skipaddr_start = nextpc;
	    skipaddr_doskip = 1;
	    do_skip = 1;
	    exception_debugging = 1;
	    return;

	case 'f':
	    if (instruction_breakpoint (&inptr))
		return;
	    break;

	case 'q': uae_quit();
	    debugger_active = 0;
	    debugging = 0;
	    return;

	case 'g':
	    if (more_params (&inptr)) {
		m68k_setpc (&regs, readhex (&inptr));
		fill_prefetch_slow (&regs);
	    }
	    debugger_active = 0;
	    debugging = 0;
	    exception_debugging = 0;
	    return;

	case 'H':
	{
	    unsigned int count, temp, badly;
	    uae_u32 oldpc = m68k_getpc (&regs);
	    struct regstruct save_regs = regs;

	    badly = 0;
	    if (inptr[0] == 'H') {
		badly = 1;
		inptr++;
	    }

	    if (more_params (&inptr))
		count = readhex (&inptr);
	    else
		count = 10;
	    temp = lasthist;
	    while (count-- > 0 && temp != firsthist) {
		if (temp == 0)
		    temp = MAX_HIST - 1;
		else
		    temp--;
	    }
	    while (temp != lasthist) {
		regs = history[temp];
		m68k_setpc (&regs, history[temp].pc);
		if (badly) {
		    m68k_dumpstate (stdout, NULL);
		} else {
		    m68k_disasm (stdout, history[temp].pc, NULL, 1);
		}
		if (++temp == MAX_HIST)
		    temp = 0;
	    }
	    regs = save_regs;
	    m68k_setpc (&regs, oldpc);
	}
	break;
	case 'm':
	{
	    uae_u32 maddr; int lines;
	    if (more_params (&inptr))
		maddr = readhex (&inptr);
	    else
		maddr = nxmem;
	    if (more_params (&inptr))
		lines = readhex (&inptr);
	    else
		lines = 20;
	    dumpmem (maddr, &nxmem, lines);
	}
	break;
	case 'o':
	{
	    if (copper_debugger(&inptr)) {
	        debugger_active = 0;
	        debugging = 0;
		return;
	    }
	    break;
	}
	case 'O':
	    if (more_params (&inptr)) {
		int plane = readint (&inptr);
		int offs = readint (&inptr);
		if (plane >= 0 && plane < 8)
		    bpl_off[plane] = offs;
	    } else {
		int i;
		for (i = 0; i < 8; i++)
		    console_out ("Plane %d offset %d\n", i, bpl_off[i]);
	    }
	    break;
#ifdef SAVESTATE
	case 'b':
	    if (staterecorder (&inptr))
		return;
	    break;
#endif
	case 'a':
	    if (more_params (&inptr)) {
		char nc = next_char (&inptr);
		if (nc == 'm')
		    audio_channel_mask = readint (&inptr);
	    }
	    break;
	case 'h':
	case '?':
	    debug_help ();
	break;
	}
    }
}

/****i* januae-debugger/addhistory ******************************************
*
*   NAME
*       addhistory() -- Record one CPU state in the debugger ring.
*
*   SYNOPSIS
*       static void addhistory (void);
*
*   FUNCTION
*       Copy the CPU register snapshot and current guest PC into the
*       100-entry history buffer, advancing and wrapping the read and write
*       indices.
*
*   NOTES
*       The oldest entry is discarded when the ring becomes full.
*
****************************************************************************
*
*/

static void addhistory (void)
{
    history[lasthist] = regs;
    history[lasthist].pc = m68k_getpc (&regs);
    if (++lasthist == MAX_HIST)
	lasthist = 0;
    if (lasthist == firsthist) {
	if (++firsthist == MAX_HIST) firsthist = 0;
    }
}

/****i* januae-debugger/debug ***********************************************
*
*   NAME
*       debug() -- Run the debugger dispatch and resume logic.
*
*   SYNOPSIS
*       void debug (void);
*
*   FUNCTION
*       Save a CPU history snapshot, test instruction, Copper and memory
*       watch triggers, pause input and audio around the command loop, then
*       restore them and arm remaining breakpoints.
*
*   NOTES
*       With SAVESTATE, an active state-save operation suppresses entry;
*       successful command-loop exit may capture a state when the configured
*       conditions permit.
*
****************************************************************************
*
*/

void debug (void)
{
    int i;

#ifdef SAVESTATE
    if (savestate_state)
	return;
#endif

    bogusframe = 1;
    addhistory ();

    if (do_skip && skipaddr_start == 0xC0DEDBAD) {
#if 0
	if (trace_same_insn_count > 0) {
	    if (memcmp (trace_insn_copy, regs.pc_p, 10) == 0
		&& memcmp (trace_prev_regs.regs, regs.regs, sizeof regs.regs) == 0)
	    {
		trace_same_insn_count++;
		return;
	    }
	}
	if (trace_same_insn_count > 1)
	    fprintf (logfile, "[ repeated %d times ]\n", trace_same_insn_count);
#endif
	m68k_dumpstate (logfile, &nextpc);
	trace_same_insn_count = 1;
	memcpy (trace_insn_copy, regs.pc_p, 10);
	memcpy (&trace_prev_regs, &regs, sizeof regs);
    }

    if (!memwatch_triggered) {
	if (do_skip) {
	    uae_u32 pc = munge24 (m68k_getpc (&regs));
	    uae_u16 opcode = (currprefs.cpu_compatible || currprefs.cpu_cycle_exact) ? regs.ir : get_word (pc);
	    int bp = 0;

	    for (i = 0; i < BREAKPOINT_TOTAL; i++) {
		if (!bpnodes[i].enabled)
		    continue;
		if (bpnodes[i].addr == pc) {
		    bp = 1;
		    console_out ("Breakpoint at %8.8X\n", pc);
		    break;
		}
	    }
	    if (skipaddr_doskip) {
		if (skipaddr_start == pc)
		    bp = 1;
		if (skipins != 0xffffffff) {
		    if (skipins == 0x10000) {
			if (opcode == 0x4e75 || opcode == 0x4e73 || opcode == 0x4e77)
			    bp = 1;
		    } else if (opcode == skipins)
			bp = 1;
		} else if (skipaddr_start == 0xffffffff && skipaddr_doskip < 0) {
		    if ((pc < 0xe00000 || pc >= 0x1000000) && opcode != 0x4ef9)
			bp = 1;
		} else if (skipaddr_start == 0xffffffff && skipaddr_doskip > 0) {
			bp = 1;
		} else if (skipaddr_end != 0xffffffff) {
		    if (pc >= skipaddr_start && pc < skipaddr_end)
			bp = 1;
		}
	    }
	    if (!bp) {
		set_special (&regs, SPCFLAG_BRK);
		return;
	    }
	}
    } else {
	write_log ("Memwatch %d: break at %8.8X.%c %c %8.8X\n", memwatch_triggered - 1, mwhit.addr,
	    mwhit.size == 1 ? 'B' : (mwhit.size == 2 ? 'W' : 'L'), mwhit.rw ? 'W' : 'R', mwhit.val);
	memwatch_triggered = 0;
    }
    if (skipaddr_doskip > 0) {
	skipaddr_doskip--;
	if (skipaddr_doskip > 0) {
	    set_special (&regs, SPCFLAG_BRK);
	    return;
	}
    }

    inputdevice_unacquire ();
    audio_pause ();
    do_skip = 0;
    skipaddr_start = 0xffffffff;
    skipaddr_end = 0xffffffff;
    skipins = 0xffffffff;
    skipaddr_doskip = 0;
    exception_debugging = 0;

#ifdef SAVESTATE
    debug_rewind = 0;
#if 0
    if (!currprefs.statecapture) {
	changed_prefs.statecapture = currprefs.statecapture = 1;
	savestate_init ();
    }
#endif
#endif

    debug_1 ();

#ifdef SAVESTATE
    if (!debug_rewind
#ifdef JIT	
	&& !currprefs.cachesize
#endif	
#ifdef FILESYS
	&& nr_units (currprefs.mountinfo) == 0
#endif
	) {
	savestate_capture (1);
    }
#endif

    for (i = 0; i < BREAKPOINT_TOTAL; i++) {
	if (bpnodes[i].enabled)
	    do_skip = 1;
    }
    if (do_skip) {
	set_special (&regs, SPCFLAG_BRK);
	debugging = 1;
    }
    audio_resume ();
    inputdevice_acquire ();
}
