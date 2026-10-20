 /*
  * UAE - The Un*x Amiga Emulator
  *
  * MC68000 emulation
  *
  * (c) 1995 Bernd Schmidt
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "events.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "cpu_prefetch.h"
#include "autoconf.h"
#include "traps.h"
#include "ersatz.h"
#include "debug.h"
#include "gui.h"
#include "savestate.h"
#include "blitter.h"
#include "ar.h"
#include "sleep.h"

#ifdef JIT
extern uae_u8* compiled_code;
#include "compemu.h"
#endif

#define f_out fprintf
#define console_out printf

/* Opcode of faulting instruction */
static uae_u16 last_op_for_exception_3;
/* PC at fault time */
static uaecptr last_addr_for_exception_3;
/* Address that generated the exception */
static uaecptr last_fault_for_exception_3;
/* read (0) or write (1) access */
static int last_writeaccess_for_exception_3;
/* instruction (1) or data (0) access */
static int last_instructionaccess_for_exception_3;

const int areg_byteinc[] = { 1,1,1,1,1,1,1,2 };
const int imm8_table[] = { 8,1,2,3,4,5,6,7 };

int movem_index1[256];
int movem_index2[256];
int movem_next[256];

#ifdef FPUEMU
int fpp_movem_index1[256];
int fpp_movem_index2[256];
int fpp_movem_next[256];
#endif

cpuop_func *cpufunctbl[65536];

static int warned_cpu68020;


#define COUNT_INSTRS 0

#if COUNT_INSTRS
static unsigned long int instrcount[65536];
static uae_u16 opcodenums[65536];

/****i* januae-cpu/compfn *************************************************************
*
*   NAME
*       compfn() -- Compare instruction counts.
*
*   SYNOPSIS
*       static int compfn (const void *el1, const void *el2);
*
*   FUNCTION
*       Compare the counter values for two opcode indices used by the
*       instruction-count sorting operation. The comparison returns the result
*       of a less-than test, not a three-way ordering.
*
*   INPUTS
*       el1 - Pointer to the first opcode index.
*       el2 - Pointer to the second opcode index.
*
***************************************************************************
*
*/

static int compfn (const void *el1, const void *el2)
{
    return instrcount[*(const uae_u16 *)el1] < instrcount[*(const uae_u16 *)el2];
}

/****i* januae-cpu/icountfilename *****************************************************
*
*   NAME
*       icountfilename() -- Choose an instruction-count filename.
*
*   SYNOPSIS
*       static char *icountfilename (void);
*
*   FUNCTION
*       Return the INSNCOUNT environment variable when set, otherwise select a
*       default filename based on COUNT_INSTRS.
*
***************************************************************************
*
*/

static char *icountfilename (void)
{
    char *name = getenv ("INSNCOUNT");
    if (name)
	return name;
    return COUNT_INSTRS == 2 ? "frequent.68k" : "insncount";
}

/****i* januae-cpu/dump_counts-1 *****************************************************
*
*   NAME
*       dump_counts() -- Write instruction-count statistics.
*
*   SYNOPSIS
*       void dump_counts (void);
*
*   FUNCTION
*       When COUNT_INSTRS is enabled, sort instruction opcodes by their
*       accumulated counts and write the nonzero entries to a file. The
*       alternative build definition performs no work.
*
***************************************************************************
*
*/

void dump_counts (void)
{
    FILE *f = fopen (icountfilename (), "w");
    unsigned long int total;
    int i;

    write_log ("Writing instruction count file...\n");
    for (i = 0; i < 65536; i++) {
	opcodenums[i] = i;
	total += instrcount[i];
    }
    qsort (opcodenums, 65536, sizeof(uae_u16), compfn);

    fprintf (f, "Total: %lu\n", total);
    for (i=0; i < 65536; i++) {
	unsigned long int cnt = instrcount[opcodenums[i]];
	struct instr *dp;
	struct mnemolookup *lookup;
	if (!cnt)
	    break;
	dp = table68k + opcodenums[i];
	for (lookup = lookuptab;lookup->mnemo != dp->mnemo; lookup++)
	    ;
	fprintf (f, "%04x: %lu %s\n", opcodenums[i], cnt, lookup->name);
    }
    fclose (f);
}
#else
/****i* januae-cpu/dump_counts-2 *****************************************************
*
*   NAME
*       dump_counts() -- Write instruction-count statistics.
*
*   SYNOPSIS
*       void dump_counts (void);
*
*   FUNCTION
*       When COUNT_INSTRS is enabled, sort instruction opcodes by their
*       accumulated counts and write the nonzero entries to a file. The
*       alternative build definition performs no work.
*
***************************************************************************
*
*/

void dump_counts (void)
{
}
#endif

/****i* januae-cpu/count_instr ********************************************************
*
*   NAME
*       count_instr() -- Record an executed opcode.
*
*   SYNOPSIS
*       STATIC_INLINE void count_instr (unsigned int opcode);
*
*   FUNCTION
*       Increment the instruction or merged-handler counter according to
*       COUNT_INSTRS; leave the counters untouched when instruction counting is
*       disabled.
*
*   INPUTS
*       opcode - Encoded 68k instruction.
*
***************************************************************************
*
*/

STATIC_INLINE void count_instr (unsigned int opcode)
{
#if COUNT_INSTRS == 2
    if (table68k[opcode].handler != -1)
	instrcount[table68k[opcode].handler]++;
#elif COUNT_INSTRS == 1
    instrcount[opcode]++;
#endif
}

static unsigned long op_illg_1 (uae_u32 opcode, struct regstruct *regs) REGPARAM;

/****i* januae-cpu/op_illg_1 **********************************************************
*
*   NAME
*       op_illg_1() -- Dispatch an unsupported instruction.
*
*   SYNOPSIS
*       static unsigned long REGPARAM2 op_illg_1 (uae_u32 opcode, struct
*       regstruct *regs);
*
*   FUNCTION
*       Forward the opcode to the illegal-instruction handler and return four
*       CPU cycles.
*
*   INPUTS
*       opcode - Encoded 68k instruction.
*       regs - Register state read or modified by the operation.
*
***************************************************************************
*
*/

static unsigned long REGPARAM2 op_illg_1 (uae_u32 opcode, struct regstruct *regs)
{
    op_illg (opcode, regs);
    return 4;
}

/****i* januae-cpu/build_cpufunctbl ***************************************************
*
*   NAME
*       build_cpufunctbl() -- Select and construct opcode dispatch.
*
*   SYNOPSIS
*       static void build_cpufunctbl (void);
*
*   FUNCTION
*       Select the instruction-handler table for the configured CPU model and
*       prefetch or cycle-exact mode, fill all 65536 dispatch slots, and resolve
*       shared instruction handlers. Abort when no applicable table or
*       referenced handler exists.
*
***************************************************************************
*
*/

static void build_cpufunctbl (void)
{
    int i, opcnt;
    unsigned long opcode;
    const struct cputbl *tbl = 0;

    switch (currprefs.cpu_level) {
#ifdef CPUEMU_0
#ifndef CPUEMU_68000_ONLY
	case 4:
	case 6:
	tbl = op_smalltbl_0_ff;
	break;
	case 3:
	tbl = op_smalltbl_1_ff;
	break;
	case 2:
	tbl = op_smalltbl_2_ff;
	break;
	case 1:
	tbl = op_smalltbl_3_ff;
	break;
#endif
#endif
	case 0:
	tbl = op_smalltbl_4_ff;
#ifdef CPUEMU_5
	if (currprefs.cpu_compatible)
	    tbl = op_smalltbl_5_ff; /* prefetch */
#endif
#ifdef CPUEMU_6
	if (currprefs.cpu_cycle_exact)
	    tbl = op_smalltbl_6_ff; /* prefetch and cycle-exact */
#endif
	break;
    }

    if (tbl == 0) {
	write_log ("no CPU emulation cores available!");
	abort ();
    }

    for (opcode = 0; opcode < 65536; opcode++)
	cpufunctbl[opcode] = op_illg_1;
    for (i = 0; tbl[i].handler != NULL; i++)
	cpufunctbl[tbl[i].opcode] = tbl[i].handler;

    opcnt = 0;
    for (opcode = 0; opcode < 65536; opcode++) {
	cpuop_func *f;

	if (table68k[opcode].mnemo == i_ILLG || table68k[opcode].clev > currprefs.cpu_level)
	    continue;

	if (table68k[opcode].handler != -1) {
	    f = cpufunctbl[table68k[opcode].handler];
	    if (f == op_illg_1)
		abort();
	    cpufunctbl[opcode] = f;
	    opcnt++;
	}
    }
    write_log ("Building CPU function table, %d opcodes (%d %d %d).\n",
	opcnt, currprefs.cpu_level,
	currprefs.cpu_cycle_exact ? -1 : currprefs.cpu_compatible ? 1 : 0,
	currprefs.address_space_24);
#ifdef JIT
    build_comp ();
#endif
}

/****i* januae-cpu/fill_prefetch_slow *************************************************
*
*   NAME
*       fill_prefetch_slow() -- Load the CPU prefetch words.
*
*   SYNOPSIS
*       void fill_prefetch_slow (struct regstruct *regs);
*
*   FUNCTION
*       Read the two instruction words at the current PC and the following word
*       into ir and irc; use cycle-exact reads when that build and configuration
*       support them.
*
*   INPUTS
*       regs - Register state read or modified by the operation.
*
***************************************************************************
*
*/

void fill_prefetch_slow (struct regstruct *regs)
{
#ifdef CPUEMU_6
    if (currprefs.cpu_cycle_exact) {
	regs->ir  = get_word_ce (m68k_getpc (regs));
	regs->irc = get_word_ce (m68k_getpc (regs) + 2);
    } else {
#endif
	regs->ir  = get_word (m68k_getpc (regs));
	regs->irc = get_word (m68k_getpc (regs) + 2);
#ifdef CPUEMU_6
    }
#endif
}

unsigned long cycles_mask, cycles_val;

/****i* januae-cpu/update_68k_cycles **************************************************
*
*   NAME
*       update_68k_cycles() -- Update CPU cycle scaling.
*
*   SYNOPSIS
*       static void update_68k_cycles (void);
*
*   FUNCTION
*       Set the global cycle mask and value from m68k_speed, disabling masking
*       when the requested speed is below one.
*
***************************************************************************
*
*/

static void update_68k_cycles (void)
{
    cycles_mask = 0;
    cycles_val = currprefs.m68k_speed;
    if (currprefs.m68k_speed < 1) {
	cycles_mask = 0xFFFFFFFF;
	cycles_val = 0;
    }
}

/****i* januae-cpu/check_prefs_changed_cpu ********************************************
*
*   NAME
*       check_prefs_changed_cpu() -- Apply pending CPU preferences.
*
*   SYNOPSIS
*       void check_prefs_changed_cpu (void);
*
*   FUNCTION
*       Apply changed CPU model, compatibility, cycle-exact and speed options;
*       rebuild handlers and request exit from the current opcode loop when a
*       mode change requires it. Update the CPU idle preference separately.
*
***************************************************************************
*
*/

void check_prefs_changed_cpu (void)
{
    if (currprefs.cpu_level != changed_prefs.cpu_level
	|| currprefs.cpu_compatible != changed_prefs.cpu_compatible
	|| currprefs.cpu_cycle_exact != changed_prefs.cpu_cycle_exact) {

	if (!currprefs.cpu_compatible && changed_prefs.cpu_compatible)
	    fill_prefetch_slow (&regs);

	currprefs.cpu_level = changed_prefs.cpu_level;
	currprefs.cpu_compatible = changed_prefs.cpu_compatible;
	currprefs.cpu_cycle_exact = changed_prefs.cpu_cycle_exact;
	currprefs.blitter_cycle_exact = changed_prefs.cpu_cycle_exact;
	build_cpufunctbl ();

	/* Force opcode handler loop to exit. We don't want it to
	 * carry on running with the old opcode handler table. */
	set_special (&regs, SPCFLAG_MODE_CHANGE);
    }
    if (currprefs.m68k_speed != changed_prefs.m68k_speed) {
	currprefs.m68k_speed = changed_prefs.m68k_speed;
	reset_frame_rate_hack ();
	update_68k_cycles ();
	/* Force opcode handler loop to exit. We don't want it to
	 * carry on running with cached values of cycles_mask and
	 * cycles_val.
	 */
	set_special (&regs, SPCFLAG_MODE_CHANGE);
    }
    if (currprefs.cpu_idle != changed_prefs.cpu_idle) {
	currprefs.cpu_idle = changed_prefs.cpu_idle;
    }
}

/****i* januae-cpu/init_m68k **********************************************************
*
*   NAME
*       init_m68k() -- Initialize CPU decoding and execution.
*
*   SYNOPSIS
*       void init_m68k (void);
*
*   FUNCTION
*       Build MOVEM bit lookup tables, initialize instruction decoding and
*       merged dispatch handlers, set the address-space mask and CPU timing, and
*       initialize JIT preferences when enabled.
*
***************************************************************************
*
*/

void init_m68k (void)
{
    int i;

    update_68k_cycles ();

    for (i = 0 ; i < 256 ; i++) {
	int j;
	for (j = 0 ; j < 8 ; j++) {
		if (i & (1 << j)) break;
	}
	movem_index1[i] = j;
	movem_index2[i] = 7-j;
	movem_next[i] = i & (~(1 << j));
    }
#ifdef FPUEMU
    for (i = 0 ; i < 256 ; i++) {
	int j;
	for (j = 7 ; j >= 0 ; j--) {
		if (i & (1 << j)) break;
	}
	fpp_movem_index1[i] = 7-j;
	fpp_movem_index2[i] = j;
	fpp_movem_next[i] = i & (~(1 << j));
    }
#endif
#if COUNT_INSTRS
    {
	FILE *f = fopen (icountfilename (), "r");
	memset (instrcount, 0, sizeof instrcount);
	if (f) {
	    uae_u32 opcode, count, total;
	    char name[20];
	    write_log ("Reading instruction count file...\n");
	    fscanf (f, "Total: %lu\n", &total);
	    while (fscanf (f, "%lx: %lu %s\n", &opcode, &count, name) == 3) {
		instrcount[opcode] = count;
	    }
	    fclose(f);
	}
    }
#endif
    write_log ("Building CPU table for configuration: 68");
    regs.address_space_mask = 0xffffffff;
    if (currprefs.cpu_compatible > 0) {
	if (currprefs.address_space_24 && currprefs.cpu_level > 3)
	    currprefs.address_space_24 = 0;
	if (currprefs.address_space_24 && currprefs.cpu_level > 1)
	    write_log ("EC");
    }
    switch (currprefs.cpu_level) {
    case 1:
	write_log ("010");
	break;
    case 2:
	write_log ("020");
	break;
    case 3:
	write_log ("020/881");
	break;
    case 4:
	/* Who is going to miss the MMU anyway...? :-)  */
	write_log ("040");
	break;
    case 6:
	/* Who is going to miss the MMU anyway...? :-)  */
	write_log ("060");
	break;
    default:
	write_log ("000");
	break;
    }
    if (currprefs.cpu_cycle_exact) {
	if (currprefs.cpu_level == 0)
	    write_log (" prefetch and cycle-exact");
	else
	    write_log (" ~cycle-exact");
    } else if (currprefs.cpu_compatible)
	write_log (" prefetch");
    if (currprefs.address_space_24) {
	regs.address_space_mask = 0x00ffffff;
	write_log (" 24-bit addressing");
    }
    write_log ("\n");

    read_table68k ();
    do_merges ();

    write_log ("%d CPU functions\n", nr_cpuop_funcs);

    build_cpufunctbl ();

#ifdef JIT
    /* We need to check whether NATMEM settings have changed
     * before starting the CPU */
    check_prefs_changed_comp ();
#endif

    reset_frame_rate_hack ();
    update_68k_cycles ();
}

struct regstruct regs;
static uae_s32 m68kpc_offset;

#define get_ibyte_1(regs, o) get_byte((regs)->pc + ((regs)->pc_p - (regs)->pc_oldp) + (o) + 1)
#define get_iword_1(regs, o) get_word((regs)->pc + ((regs)->pc_p - (regs)->pc_oldp) + (o))
#define get_ilong_1(regs, o) get_long((regs)->pc + ((regs)->pc_p - (regs)->pc_oldp) + (o))

/****i* januae-cpu/ShowEA *************************************************************
*
*   NAME
*       ShowEA() -- Format a decoded effective address.
*
*   SYNOPSIS
*       static uae_s32 ShowEA (void *f, uae_u16 opcode, int reg, amodes mode,
*       wordsizes size, char *buf, uae_u32 *eaddr, int safemode);
*
*   FUNCTION
*       Decode an instruction operand using its addressing mode, operand size
*       and register, format its textual representation, and optionally return
*       the computed effective address.
*
*   INPUTS
*       f - Output stream or output handle selected by the caller.
*       opcode - Encoded 68k instruction.
*       reg - Register selection within the effective-address mode.
*       mode - Effective-address mode.
*       size - Operand width.
*       buf - Caller-provided output buffer.
*       eaddr - Optional effective-address output.
*       safemode - Select safe operand inspection.
*
***************************************************************************
*
*/

static uae_s32 ShowEA (void *f, uae_u16 opcode, int reg, amodes mode, wordsizes size, char *buf, uae_u32 *eaddr, int safemode)
{
    uae_u16 dp;
    uae_s8 disp8;
    uae_s16 disp16;
    int r;
    uae_u32 dispreg;
    uaecptr addr = 0;
    uae_s32 offset = 0;
    char buffer[80];

    switch (mode){
     case Dreg:
	sprintf (buffer, "D%d", reg);
	break;
     case Areg:
	sprintf (buffer, "A%d", reg);
	break;
     case Aind:
	sprintf (buffer, "(A%d)", reg);
	addr = regs.regs[reg + 8];
	break;
     case Aipi:
	sprintf (buffer, "(A%d)+", reg);
	addr = regs.regs[reg + 8];
	break;
     case Apdi:
	sprintf (buffer, "-(A%d)", reg);
	addr = regs.regs[reg + 8];
	break;
     case Ad16:
	{
	    char offtxt[80];
	    disp16 = get_iword_1 (&regs, m68kpc_offset); m68kpc_offset += 2;
	    if (disp16 < 0)
		sprintf (offtxt, "-$%04x", -disp16);
	    else
		sprintf (offtxt, "$%04x", disp16);
	    addr = m68k_areg (&regs,reg) + disp16;
	    sprintf (buffer, "(A%d,%s) == $%08x", reg, offtxt, (unsigned long)addr);
	}
	break;
     case Ad8r:
	dp = get_iword_1 (&regs, m68kpc_offset); m68kpc_offset += 2;
	disp8 = dp & 0xFF;
	r = (dp & 0x7000) >> 12;
	dispreg = dp & 0x8000 ? m68k_areg (&regs,r) : m68k_dreg (&regs,r);
	if (!(dp & 0x800)) dispreg = (uae_s32)(uae_s16)(dispreg);
	dispreg <<= (dp >> 9) & 3;

	if (dp & 0x100) {
	    uae_s32 outer = 0, disp = 0;
	    uae_s32 base = m68k_areg (&regs,reg);
	    char name[10];
	    sprintf (name, "A%d, ", reg);
	    if (dp & 0x80) { base = 0; name[0] = 0; }
	    if (dp & 0x40) dispreg = 0;
	    if ((dp & 0x30) == 0x20) { disp = (uae_s32)(uae_s16)get_iword_1 (&regs, m68kpc_offset); m68kpc_offset += 2; }
	    if ((dp & 0x30) == 0x30) { disp = get_ilong_1 (&regs, m68kpc_offset); m68kpc_offset += 4; }
	    base += disp;

	    if ((dp & 0x3) == 0x2) { outer = (uae_s32)(uae_s16)get_iword_1 (&regs, m68kpc_offset); m68kpc_offset += 2; }
	    if ((dp & 0x3) == 0x3) { outer = get_ilong_1 (&regs, m68kpc_offset); m68kpc_offset += 4; }

	    if (!(dp & 4)) base += dispreg;
	    if ((dp & 3) && !safemode) base = get_long (base);
	    if (dp & 4) base += dispreg;

	    addr = base + outer;
	    sprintf (buffer, "(%s%c%d.%c*%d+%d)+%d == $%08x", name,
		     dp & 0x8000 ? 'A' : 'D', r, dp & 0x800 ? 'L' : 'W',
		     1 << ((dp >> 9) & 3),
		     disp, outer, addr);
	} else {
	  addr = m68k_areg (&regs,reg) + (uae_s32)((uae_s8)disp8) + dispreg;
	  sprintf (buffer, "(A%d, %c%d.%c*%d, $%02x) == $%08x", reg,
		   dp & 0x8000 ? 'A' : 'D', r, dp & 0x800 ? 'L' : 'W',
		   1 << ((dp >> 9) & 3), disp8, addr);
	}
	break;
     case PC16:
	addr = m68k_getpc (&regs) + m68kpc_offset;
	disp16 = get_iword_1 (&regs, m68kpc_offset); m68kpc_offset += 2;
	addr += (uae_s16)disp16;
	sprintf (buffer, "(PC,$%04x) == $%08x", disp16 & 0xffff, addr);
	break;
     case PC8r:
	addr = m68k_getpc (&regs) + m68kpc_offset;
	dp = get_iword_1 (&regs, m68kpc_offset); m68kpc_offset += 2;
	disp8 = dp & 0xFF;
	r = (dp & 0x7000) >> 12;
	dispreg = dp & 0x8000 ? m68k_areg (&regs,r) : m68k_dreg (&regs,r);
	if (!(dp & 0x800)) dispreg = (uae_s32)(uae_s16)(dispreg);
	dispreg <<= (dp >> 9) & 3;

	if (dp & 0x100) {
	    uae_s32 outer = 0,disp = 0;
	    uae_s32 base = addr;
	    char name[10];
	    sprintf (name, "PC, ");
	    if (dp & 0x80) { base = 0; name[0] = 0; }
	    if (dp & 0x40) dispreg = 0;
	    if ((dp & 0x30) == 0x20) { disp = (uae_s32)(uae_s16)get_iword_1 (&regs, m68kpc_offset); m68kpc_offset += 2; }
	    if ((dp & 0x30) == 0x30) { disp = get_ilong_1 (&regs, m68kpc_offset); m68kpc_offset += 4; }
	    base += disp;

	    if ((dp & 0x3) == 0x2) { outer = (uae_s32)(uae_s16)get_iword_1 (&regs, m68kpc_offset); m68kpc_offset += 2; }
	    if ((dp & 0x3) == 0x3) { outer = get_ilong_1 (&regs, m68kpc_offset); m68kpc_offset += 4; }

	    if (!(dp & 4)) base += dispreg;
	    if ((dp & 3) && !safemode) base = get_long (base);
	    if (dp & 4) base += dispreg;

	    addr = base + outer;
	    sprintf (buffer, "(%s%c%d.%c*%d+%d)+%d == $%08x", name,
		     dp & 0x8000 ? 'A' : 'D', r, dp & 0x800 ? 'L' : 'W',
		     1 << ((dp >> 9) & 3),
		     disp, outer, addr);
	} else {
	  addr += (uae_s32)((uae_s8)disp8) + dispreg;
	  sprintf (buffer, "(PC, %c%d.%c*%d, $%02x) == $%08x", dp & 0x8000 ? 'A' : 'D',
		   r, dp & 0x800 ? 'L' : 'W',  1 << ((dp >> 9) & 3),
		   disp8, addr);
	}
	break;
     case absw:
	addr = (uae_s32)(uae_s16)get_iword_1 (&regs, m68kpc_offset);
	sprintf (buffer,"$%08x", (unsigned long)addr);
	m68kpc_offset += 2;
	break;
     case absl:
	addr = get_ilong_1 (&regs, m68kpc_offset);
	sprintf (buffer,"$%0lx", (unsigned long)addr);
	m68kpc_offset += 4;
	break;
     case imm:
	switch (size){
	 case sz_byte:
	    sprintf (buffer, "#$%02x", (unsigned int)(get_iword_1 (&regs, m68kpc_offset) & 0xff));
	    m68kpc_offset += 2;
	    break;
	 case sz_word:
	    sprintf (buffer, "#$%04x", (unsigned int)(get_iword_1 (&regs, m68kpc_offset) & 0xffff));
	    m68kpc_offset += 2;
	    break;
	 case sz_long:
	    sprintf (buffer, "#$%08x", (unsigned int)(get_ilong_1 (&regs, m68kpc_offset) & 0xffffffff));
	    m68kpc_offset += 4;
	    break;
	 default:
	    break;
	}
	break;
     case imm0:
	offset = (uae_s32)(uae_s8)get_iword_1 (&regs, m68kpc_offset);
	m68kpc_offset += 2;
	sprintf (buffer, "#$%02x", (unsigned int)(offset & 0xff));
	break;
     case imm1:
	offset = (uae_s32)(uae_s16)get_iword_1 (&regs, m68kpc_offset);
	m68kpc_offset += 2;
	buffer[0] = 0;
	sprintf (buffer,"#$%04x", (unsigned int)(offset & 0xffff));
	break;
     case imm2:
	offset = (uae_s32)get_ilong_1 (&regs, m68kpc_offset);
	m68kpc_offset += 4;
	sprintf (buffer, "#$%08x", (unsigned int)offset);
	break;
     case immi:
	offset = (uae_s32)(uae_s8)(reg & 0xff);
	sprintf (buffer, "#$%08x", (unsigned int)offset);
	break;
     default:
	break;
    }
    if (buf == 0)
	f_out (f, "%s", buffer);
    else
	strcat (buf, buffer);
    if (eaddr)
	*eaddr = addr;
    return offset;
}


/****i* januae-cpu/get_disp_ea_020 ****************************************************
*
*   NAME
*       get_disp_ea_020() -- Compute a 68020 indexed effective address.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 get_disp_ea_020 (struct regstruct *regs, uae_u32 base,
*       uae_u32 dp);
*
*   FUNCTION
*       Decode the 68020 index extension, including scaling, base suppression
*       and memory-indirect forms, then calculate the resulting effective
*       address.
*
*   INPUTS
*       regs - Register state read or modified by the operation.
*       base - Base effective address.
*       dp - Index and displacement extension word.
*
***************************************************************************
*
*/

uae_u32 REGPARAM2 get_disp_ea_020 (struct regstruct *regs, uae_u32 base, uae_u32 dp)
{
    int reg = (dp >> 12) & 15;
    uae_s32 regd = regs->regs[reg];
    if ((dp & 0x800) == 0)
	regd = (uae_s32)(uae_s16)regd;
    regd <<= (dp >> 9) & 3;
    if (dp & 0x100) {
	uae_s32 outer = 0;
	if (dp & 0x80) base = 0;
	if (dp & 0x40) regd = 0;

	if ((dp & 0x30) == 0x20) base += (uae_s32)(uae_s16) next_iword (regs);
	if ((dp & 0x30) == 0x30) base += next_ilong (regs);

	if ((dp & 0x3) == 0x2) outer = (uae_s32)(uae_s16) next_iword (regs);
	if ((dp & 0x3) == 0x3) outer = next_ilong (regs);

	if ((dp & 0x4) == 0) base += regd;
	if (dp & 0x3) base = get_long (base);
	if (dp & 0x4) base += regd;

	return base + outer;
    } else {
	return base + (uae_s32)((uae_s8)dp) + regd;
    }
}

/****i* januae-cpu/get_disp_ea_000 ****************************************************
*
*   NAME
*       get_disp_ea_000() -- Compute a 68000 indexed effective address.
*
*   SYNOPSIS
*       uae_u32 REGPARAM2 get_disp_ea_000 (struct regstruct *regs, uae_u32 base,
*       uae_u32 dp);
*
*   FUNCTION
*       Add the signed eight-bit displacement and selected index register value
*       to the base address using 68000 indexing rules.
*
*   INPUTS
*       regs - Register state read or modified by the operation.
*       base - Base effective address.
*       dp - Index and displacement extension word.
*
***************************************************************************
*
*/

uae_u32 REGPARAM2 get_disp_ea_000 (struct regstruct *regs, uae_u32 base, uae_u32 dp)
{
    int reg = (dp >> 12) & 15;
    uae_s32 regd = regs->regs[reg];
#if 1
    if ((dp & 0x800) == 0)
	regd = (uae_s32)(uae_s16)regd;
    return base + (uae_s8)dp + regd;
#else
    /* Branch-free code... benchmark this again now that
     * things are no longer inline.  */
    uae_s32 regd16;
    uae_u32 mask;
    mask = ((dp & 0x800) >> 11) - 1;
    regd16 = (uae_s32)(uae_s16)regd;
    regd16 &= mask;
    mask = ~mask;
    base += (uae_s8)dp;
    regd &= mask;
    regd |= regd16;
    return base + regd;
#endif
}

/****i* januae-cpu/MakeSR *************************************************************
*
*   NAME
*       MakeSR() -- Assemble the status register.
*
*   SYNOPSIS
*       void REGPARAM2 MakeSR (struct regstruct *regs);
*
*   FUNCTION
*       Pack supervisor, trace, interrupt-mask and condition-code state from the
*       register structure into the 68k status register.
*
*   INPUTS
*       regs - Register state read or modified by the operation.
*
***************************************************************************
*
*/

void REGPARAM2 MakeSR (struct regstruct *regs)
{
#if 0
    assert((regs.t1 & 1) == regs->t1);
    assert((regs.t0 & 1) == regs->t0);
    assert((regs.s & 1) == regs->s);
    assert((regs.m & 1) == regs->m);
    assert((XFLG & 1) == XFLG);
    assert((NFLG & 1) == NFLG);
    assert((ZFLG & 1) == ZFLG);
    assert((VFLG & 1) == VFLG);
    assert((CFLG & 1) == CFLG);
#endif
    regs->sr = ((regs->t1 << 15) | (regs->t0 << 14)
	       | (regs->s << 13) | (regs->m << 12) | (regs->intmask << 8)
	       | (GET_XFLG(&regs->ccrflags) << 4) | (GET_NFLG(&regs->ccrflags) << 3)
	       | (GET_ZFLG(&regs->ccrflags) << 2) | (GET_VFLG(&regs->ccrflags) << 1)
	       |  GET_CFLG(&regs->ccrflags));
}

/****i* januae-cpu/MakeFromSR *********************************************************
*
*   NAME
*       MakeFromSR() -- Apply a status register value.
*
*   SYNOPSIS
*       void REGPARAM2 MakeFromSR (struct regstruct *regs);
*
*   FUNCTION
*       Unpack the status register into CPU status and condition flags; account
*       for supervisor and master-stack transitions when the mode changes.
*
*   INPUTS
*       regs - Register state read or modified by the operation.
*
***************************************************************************
*
*/

void REGPARAM2 MakeFromSR (struct regstruct *regs)
{
    int oldm = regs->m;
    int olds = regs->s;

    SET_XFLG (&regs->ccrflags, (regs->sr >> 4) & 1);
    SET_NFLG (&regs->ccrflags, (regs->sr >> 3) & 1);
    SET_ZFLG (&regs->ccrflags, (regs->sr >> 2) & 1);
    SET_VFLG (&regs->ccrflags, (regs->sr >> 1) & 1);
    SET_CFLG (&regs->ccrflags,  regs->sr & 1);
    if (regs->t1 == ((regs->sr >> 15) & 1) &&
	regs->t0 == ((regs->sr >> 14) & 1) &&
	regs->s  == ((regs->sr >> 13) & 1) &&
	regs->m  == ((regs->sr >> 12) & 1) &&
	regs->intmask == ((regs->sr >> 8) & 7))
	    return;
    regs->t1 = (regs->sr >> 15) & 1;
    regs->t0 = (regs->sr >> 14) & 1;
    regs->s  = (regs->sr >> 13) & 1;
    regs->m  = (regs->sr >> 12) & 1;
    regs->intmask = (regs->sr >> 8) & 7;
    if (currprefs.cpu_level >= 2) {
	if (olds != regs->s) {
	    if (olds) {
		if (oldm)
		    regs->msp = m68k_areg (regs, 7);
		else
		    regs->isp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs->usp;
	    } else {
		regs->usp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs->m ? regs->msp : regs->isp;
	    }
	} else if (olds && oldm != regs->m) {
	    if (oldm) {
		regs->msp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs->isp;
	    } else {
		regs->isp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs->msp;
	    }
	}
    } else {
	regs->t0 = regs->m = 0;
	if (olds != regs->s) {
	    if (olds) {
		regs->isp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs->usp;
	    } else {
		regs->usp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs->isp;
	    }
	}
    }

    set_special (regs, SPCFLAG_INT);
    if (regs->t1 || regs->t0)
	set_special (regs, SPCFLAG_TRACE);
    else
    	/* Keep SPCFLAG_DOTRACE, we still want a trace exception for
	   SR-modifying instructions (including STOP).  */
	unset_special (regs, SPCFLAG_TRACE);
}

/****i* januae-cpu/trace_exception ****************************************************
*
*   NAME
*       trace_exception() -- Record exception trace state.
*
*   SYNOPSIS
*       STATIC_INLINE void trace_exception (struct regstruct *regs, unsigned int
*       nr);
*
*   FUNCTION
*       Clear pending trace special flags, retain a deferred trace request for
*       selected exception vectors, and clear the CPU trace-mode flags.
*
*   INPUTS
*       regs - Register state read or modified by the operation.
*       nr - Exception vector number.
*
***************************************************************************
*
*/

STATIC_INLINE void trace_exception (struct regstruct *regs, unsigned int nr)
{
    unset_special (regs, SPCFLAG_TRACE | SPCFLAG_DOTRACE);
    if (regs->t1 && !regs->t0) {
	/* Trace stays pending if exception is div by zero, chk,
	 * trapv or trap #x
	 */
	if (nr == 5 || nr == 6 || nr ==  7 || (nr >= 32 && nr <= 47))
	    set_special (regs, SPCFLAG_DOTRACE);
    }
    regs->t1 = regs->t0 = regs->m = 0;
}

/****i* januae-cpu/exception_debug ****************************************************
*
*   NAME
*       exception_debug() -- Report an exception to the debugger.
*
*   SYNOPSIS
*       static void exception_debug (int nr);
*
*   FUNCTION
*       Emit the exception information used by the debugger's exception
*       reporting path when the corresponding debug conditions are enabled.
*
*   INPUTS
*       nr - Exception vector number.
*
***************************************************************************
*
*/

static void exception_debug (int nr)
{
#ifdef DEBUGGER
    if (!exception_debugging)
	return;
    console_out ("uae_Exception %d, PC=%08x\n", nr, m68k_getpc (&regs));
#endif
}

#ifdef CPUEMU_6

/* cycle-exact exception handler, 68000 only */

/****i* januae-cpu/uae_Exception_ce ***************************************************
*
*   NAME
*       uae_Exception_ce() -- Build a cycle-exact exception frame.
*
*   SYNOPSIS
*       STATIC_INLINE void uae_Exception_ce (int nr, struct regstruct *regs,
*       uaecptr oldpc);
*
*   FUNCTION
*       Perform exception entry using cycle-exact memory and timing operations,
*       including vector access, stack frame construction and updated CPU state.
*
*   INPUTS
*       nr - Exception vector number.
*       regs - Register state read or modified by the operation.
*       oldpc - Program counter associated with the exception.
*
***************************************************************************
*
*/

STATIC_INLINE void uae_Exception_ce (int nr, struct regstruct *regs, uaecptr oldpc)
{
    uae_u32 currpc = m68k_getpc (regs), newpc;
    int c;
    int sv = regs->s;

    exception_debug (nr);
    MakeSR (regs);

    c = 0;
    switch (nr)
    {
	case 2: /* bus */
	case 3: /* address */
	c = 6;
	break;
	case 4: /* illegal instruction */
	c = 6;
	break;
	case 5: /* divide by zero */
	c = 10;
	break;
	case 6: /* chk */
	c = 12;
	break;
	case 7: /* trapv */
	c = 6;
	break;
	case 8: /* privilege */
	c = 6;
	break;
	case 9: /* trace */
	c = 6;
	break;
	case 25: /* interrupts */
	case 26:
	case 27:
	case 28:
	case 29:
	case 30:
	case 31:
	c = 12;
	break;
	case 32: /* traps */
	case 33:
	case 34:
	case 35:
	case 36:
	case 37:
	case 38:
	case 39:
	case 40:
	case 41:
	case 42:
	case 43:
	case 44:
	case 45:
	case 46:
	case 47:
	c = 6;
	break;
    }
    /* some delays are interleaved with stack pushes, not bothered yet..
     */
    if (c)
	do_cycles (c * CYCLE_UNIT / 2);

    if (!regs->s) {
	regs->usp = m68k_areg (regs, 7);
	m68k_areg (regs, 7) = regs->isp;
	regs->s = 1;
    }
    if (nr == 2 || nr == 3) { /* 2=bus error,3=address error */
	uae_u16 mode = (sv ? 4 : 0) | (last_instructionaccess_for_exception_3 ? 2 : 1);
	mode |= last_writeaccess_for_exception_3 ? 0 : 16;
	m68k_areg (regs, 7) -= 14;
	/* fixme: bit3=I/N */
	put_word_ce (m68k_areg (regs, 7) + 12, last_addr_for_exception_3);
	put_word_ce (m68k_areg (regs, 7) + 8,  regs->sr);
	put_word_ce (m68k_areg (regs, 7) + 10, last_addr_for_exception_3 >> 16);
	put_word_ce (m68k_areg (regs, 7) + 6,  last_op_for_exception_3);
	put_word_ce (m68k_areg (regs, 7) + 4,  last_fault_for_exception_3);
	put_word_ce (m68k_areg (regs, 7) + 0,  mode);
	put_word_ce (m68k_areg (regs, 7) + 2,  last_fault_for_exception_3 >> 16);
#if 0
	write_log ("uae_Exception %d at %08x -> %08x!\n", nr, currpc, get_long (4 * nr));
#endif
	goto kludge_me_do;
    }
    m68k_areg (regs, 7) -= 6;
    put_word_ce (m68k_areg (regs, 7) + 4, currpc);
    put_word_ce (m68k_areg (regs, 7) + 0, regs->sr);
    put_word_ce (m68k_areg (regs, 7) + 2, currpc >> 16);
kludge_me_do:
    newpc = get_word_ce (4 * nr) << 16;
    newpc |= get_word_ce (4 * nr + 2);
    if (newpc & 1) {
	if (nr == 2 || nr == 3)
	    uae_reset (1); /* there is nothing else we can do.. */
	else
	    exception3 (regs->ir, m68k_getpc (regs), newpc);
	return;
    }
    m68k_setpc (regs, newpc);
    fill_prefetch_slow (regs);
    set_special (regs, SPCFLAG_END_COMPILE);
    trace_exception (regs, nr);
}
#endif

/****i* januae-cpu/uae_Exception_normal ***********************************************
*
*   NAME
*       uae_Exception_normal() -- Build a normal exception frame.
*
*   SYNOPSIS
*       STATIC_INLINE void uae_Exception_normal (int nr, struct regstruct *regs,
*       uaecptr oldpc);
*
*   FUNCTION
*       Perform exception entry for the non-cycle-exact CPU mode, saving the
*       appropriate status and return address and loading the exception vector.
*
*   INPUTS
*       nr - Exception vector number.
*       regs - Register state read or modified by the operation.
*       oldpc - Program counter associated with the exception.
*
***************************************************************************
*
*/

STATIC_INLINE void uae_Exception_normal (int nr, struct regstruct *regs, uaecptr oldpc)
{
    uae_u32 currpc = m68k_getpc (regs), newpc;
    int sv = regs->s;

    exception_debug (nr);
    MakeSR (regs);

    if (!regs->s) {
	regs->usp = m68k_areg (regs, 7);
	if (currprefs.cpu_level >= 2)
	    m68k_areg (regs, 7) = regs->m ? regs->msp : regs->isp;
	else
	    m68k_areg (regs, 7) = regs->isp;
	regs->s = 1;
    }
    if (currprefs.cpu_level > 0) {
	if (nr == 2 || nr == 3) {
	    int i;
	    if (currprefs.cpu_level >= 4) { /* 68040 */
		if (nr == 2) {
		    for (i = 0 ; i < 18 ; i++) {
			m68k_areg (regs, 7) -= 2;
			put_word (m68k_areg (regs, 7), 0);
		    }
		    m68k_areg (regs, 7) -= 4;
		    put_long (m68k_areg (regs, 7), last_fault_for_exception_3);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0x0140 | (sv ? 6 : 2)); /* SSW */
		    m68k_areg (regs, 7) -= 4;
		    put_long (m68k_areg (regs, 7), last_addr_for_exception_3);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0x7000 + nr * 4);
		} else {
		    m68k_areg (regs, 7) -= 4;
		    put_long (m68k_areg (regs, 7), last_fault_for_exception_3);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0x2000 + nr * 4);
		}
	    } else {
		uae_u16 ssw = (sv ? 4 : 0) | (last_instructionaccess_for_exception_3 ? 2 : 1);
		ssw |= last_writeaccess_for_exception_3 ? 0 : 0x40;
		ssw |= 0x20;
		for (i = 0 ; i < 36; i++) {
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0);
		}
		m68k_areg (regs, 7) -= 4;
		put_long (m68k_areg (regs, 7), last_fault_for_exception_3);
		m68k_areg (regs, 7) -= 2;
		put_word (m68k_areg (regs, 7), 0);
		m68k_areg (regs, 7) -= 2;
		put_word (m68k_areg (regs, 7), 0);
		m68k_areg (regs, 7) -= 2;
		put_word (m68k_areg (regs, 7), 0);
		m68k_areg (regs, 7) -= 2;
		put_word (m68k_areg (regs, 7), ssw);
		m68k_areg (regs, 7) -= 2;
		put_word (m68k_areg (regs, 7), 0xb000 + nr * 4);
	    }
#if 0
	    write_log ("uae_Exception %d (%08x) at %08x -> %08x!\n", nr, oldpc, currpc, get_long (regs->vbr + 4*nr));
#endif
	} else if (nr ==5 || nr == 6 || nr == 7 || nr == 9) {
	    m68k_areg (regs, 7) -= 4;
	    put_long (m68k_areg (regs, 7), oldpc);
	    m68k_areg (regs, 7) -= 2;
	    put_word (m68k_areg (regs, 7), 0x2000 + nr * 4);
	} else if (regs->m && nr >= 24 && nr < 32) { /* M + Interrupt */
	    m68k_areg (regs, 7) -= 2;
	    put_word  (m68k_areg (regs, 7), nr * 4);
	    m68k_areg (regs, 7) -= 4;
	    put_long  (m68k_areg (regs, 7), currpc);
	    m68k_areg (regs, 7) -= 2;
	    put_word  (m68k_areg (regs, 7), regs->sr);
	    regs->sr |= (1 << 13);
	    regs->msp = m68k_areg (regs, 7);
	    m68k_areg (regs, 7) = regs->isp;
	    m68k_areg (regs, 7) -= 2;
	    put_word  (m68k_areg (regs, 7), 0x1000 + nr * 4);
	} else {
	    m68k_areg (regs, 7) -= 2;
	    put_word (m68k_areg (regs, 7), nr * 4);
	}
    } else if (nr == 2 || nr == 3) {
	uae_u16 mode = (sv ? 4 : 0) | (last_instructionaccess_for_exception_3 ? 2 : 1);
	mode |= last_writeaccess_for_exception_3 ? 0 : 16;
	m68k_areg (regs, 7) -= 14;
	/* fixme: bit3=I/N */
	put_word (m68k_areg (regs, 7) + 0,  mode);
	put_long (m68k_areg (regs, 7) + 2,  last_fault_for_exception_3);
	put_word (m68k_areg (regs, 7) + 6,  last_op_for_exception_3);
	put_word (m68k_areg (regs, 7) + 8,  regs->sr);
	put_long (m68k_areg (regs, 7) + 10, last_addr_for_exception_3);
#if 0
	write_log ("uae_Exception %d (%08x) at %08x -> %08x!\n", nr, oldpc, currpc, get_long (regs->vbr + 4*nr));
#endif
	goto kludge_me_do;
    }
    m68k_areg (regs, 7) -= 4;
    put_long  (m68k_areg (regs, 7), currpc);
    m68k_areg (regs, 7) -= 2;
    put_word  (m68k_areg (regs, 7), regs->sr);
kludge_me_do:
    newpc = get_long (regs->vbr + 4 * nr);
    if (newpc & 1) {
	if (nr == 2 || nr == 3)
	    uae_reset (1); /* there is nothing else we can do.. */
	else
	    exception3 (regs->ir, m68k_getpc (regs), newpc);
	return;
    }
    m68k_setpc (regs, newpc);
    set_special (regs, SPCFLAG_END_COMPILE);
    fill_prefetch_slow (regs);
    trace_exception (regs, nr);
}

/****i* januae-cpu/uae_Exception ******************************************************
*
*   NAME
*       uae_Exception() -- Dispatch an emulated CPU exception.
*
*   SYNOPSIS
*       void REGPARAM2 uae_Exception (int nr, struct regstruct *regs, uaecptr
*       oldpc);
*
*   FUNCTION
*       Select the cycle-exact or ordinary exception-entry implementation
*       according to the current CPU mode.
*
*   INPUTS
*       nr - Exception vector number.
*       regs - Register state read or modified by the operation.
*       oldpc - Program counter associated with the exception.
*
***************************************************************************
*
*/

void REGPARAM2 uae_Exception (int nr, struct regstruct *regs, uaecptr oldpc)
{
#if 0
    if (1 || nr < 24)
	write_log ("exception %2d %08x %08x (%04x %04x)\n",
	    nr, oldpc, m68k_getpc (regs), intena, intreq);
#endif
#ifdef CPUEMU_6
    if (currprefs.cpu_cycle_exact && currprefs.cpu_level == 0)
	uae_Exception_ce (nr, regs, oldpc);
    else
#endif
	uae_Exception_normal (nr, regs, oldpc);
}

/****i* januae-cpu/service_interrupt **************************************************
*
*   NAME
*       service_interrupt() -- Enter an eligible interrupt.
*
*   SYNOPSIS
*       STATIC_INLINE void service_interrupt (unsigned int level, struct
*       regstruct *regs);
*
*   FUNCTION
*       If the incoming level exceeds the current mask, leave STOP state, enter
*       the matching autovector exception and update the interrupt mask.
*
*   INPUTS
*       level - Requested interrupt level.
*       regs - Register state read or modified by the operation.
*
***************************************************************************
*
*/

STATIC_INLINE void service_interrupt (unsigned int level, struct regstruct *regs)
{
    if (level > regs->intmask) {

	regs->stopped = 0;
	unset_special (regs, SPCFLAG_STOP);

	uae_Exception (level + 24, regs, 0);

	regs->intmask = level;
    }
}

/*
 * Directly cause an interrupt to occur.
 *
 * This probably ain't a great idea, because this could allow an interrupt
 * to be forced before an opcode is completed and the PC updated.
 */
/****i* januae-cpu/Interrupt **********************************************************
*
*   NAME
*       Interrupt() -- Request an immediate CPU interrupt.
*
*   SYNOPSIS
*       void Interrupt (unsigned int level);
*
*   FUNCTION
*       Assert that the level is below eight and call the interrupt service path
*       for the global CPU state. The request is not deferred until instruction
*       completion.
*
*   INPUTS
*       level - Requested interrupt level.
*
***************************************************************************
*
*/

void Interrupt (unsigned int level)
{
    assert (level < 8);
    service_interrupt (level, &regs);
}

static uae_u32 caar, cacr, itt0, itt1, dtt0, dtt1, tc, mmusr, urp, srp, buscr, pcr;

#ifndef CPUEMU_68000_ONLY

/****i* januae-cpu/movec_illg *********************************************************
*
*   NAME
*       movec_illg() -- Validate a control-register selector.
*
*   SYNOPSIS
*       static int movec_illg (int regno);
*
*   FUNCTION
*       Decide whether a MOVEC control-register selector is invalid for the
*       configured CPU level.
*
*   INPUTS
*       regno - Encoded CPU control-register selector.
*
***************************************************************************
*
*/

static int movec_illg (int regno)
{
    int regno2 = regno & 0x7ff;
    if (currprefs.cpu_level == 1) { /* 68010 */
	if (regno2 < 2)
	    return 0;
	return 1;
    }
    if (currprefs.cpu_level == 2 || currprefs.cpu_level == 3) { /* 68020 */
	if (regno == 3) return 1; /* 68040 only */
	/* 4 is >=68040, but 0x804 is in 68020 */
	if (regno2 < 4 || regno == 0x804)
	    return 0;
	return 1;
    }
    if (currprefs.cpu_level >= 4) { /* 68040 */
	if (regno == 0x802) return 1; /* 68020 only */
	if (regno2 < 8) return 0;
	if (currprefs.cpu_level == 6 && regno2 == 8) /* 68060 only */
	    return 0;
	return 1;
    }
    return 1;
}

/****i* januae-cpu/m68k_move2c ********************************************************
*
*   NAME
*       m68k_move2c() -- Write an emulated CPU control register.
*
*   SYNOPSIS
*       int m68k_move2c (int regno, uae_u32 *regp);
*
*   FUNCTION
*       Validate the MOVEC selector, mask model-dependent writable fields and
*       update the selected control register; perform JIT cache operations when
*       applicable.
*
*   INPUTS
*       regno - Encoded CPU control-register selector.
*       regp - Input or output control-register value.
*
***************************************************************************
*
*/

int m68k_move2c (int regno, uae_u32 *regp)
{
//    write_log ("move2c %04x <- %08x\n", regno, *regp);
    if (movec_illg (regno)) {
	op_illg (0x4E7B, &regs);
	return 0;
    } else {
	switch (regno) {
	case 0: regs.sfc = *regp & 7; break;
	case 1: regs.dfc = *regp & 7; break;
	case 2:
	    cacr = *regp & (currprefs.cpu_level < 4 ? 0x3 : (currprefs.cpu_level == 4 ? 0x80008000 : 0xf8800e00));
#ifdef JIT
	    if (currprefs.cpu_level < 4) {
		set_cache_state (cacr & 1);
		if (*regp & 0x08) {
		    flush_icache (1);
		}
	    } else {
		set_cache_state ((cacr & 0x8000) || 0);
		if (*regp & 0x08) {   /* Just to be on the safe side */
		    flush_icache (2);
		}
	    }
#endif
	    break;
	case 3: tc = *regp & 0xc000; break;
	    /* Mask out fields that should be zero.  */
	case 4: itt0 = *regp & 0xffffe364; break;
	case 5: itt1 = *regp & 0xffffe364; break;
	case 6: dtt0 = *regp & 0xffffe364; break;
	case 7: dtt1 = *regp & 0xffffe364; break;
	case 8: buscr = *regp & 0xf0000000; break;

	case 0x800: regs.usp = *regp; break;
	case 0x801: regs.vbr = *regp; break;
	case 0x802: caar = *regp & 0xfc; break;
	case 0x803: regs.msp = *regp; if (regs.m == 1) m68k_areg (&regs, 7) = regs.msp; break;
	case 0x804: regs.isp = *regp; if (regs.m == 0) m68k_areg (&regs, 7) = regs.isp; break;
	case 0x805: mmusr = *regp; break;
	case 0x806: urp = *regp; break;
	case 0x807: srp = *regp; break;
	case 0x808: pcr = *regp & (0x40 | 2 | 1); break;
	default:
	    op_illg (0x4E7B, &regs);
	    return 0;
	}
    }
    return 1;
}

/****i* januae-cpu/m68k_movec2 ********************************************************
*
*   NAME
*       m68k_movec2() -- Read an emulated CPU control register.
*
*   SYNOPSIS
*       int m68k_movec2 (int regno, uae_u32 *regp);
*
*   FUNCTION
*       Validate the MOVEC selector and copy the selected CPU control register
*       value into the caller's output word.
*
*   INPUTS
*       regno - Encoded CPU control-register selector.
*       regp - Input or output control-register value.
*
***************************************************************************
*
*/

int m68k_movec2 (int regno, uae_u32 *regp)
{
//    write_log ("movec2 %04x ", regno);
    if (movec_illg (regno)) {
	op_illg (0x4E7A, &regs);
	return 0;
    } else {
	switch (regno) {
	case 0: *regp = regs.sfc; break;
	case 1: *regp = regs.dfc; break;
	case 2: *regp = cacr; break;
	case 3: *regp = tc; break;
	case 4: *regp = itt0; break;
	case 5: *regp = itt1; break;
	case 6: *regp = dtt0; break;
	case 7: *regp = dtt1; break;
	case 8: *regp = buscr; break;

	case 0x800: *regp = regs.usp; break;
	case 0x801: *regp = regs.vbr; break;
	case 0x802: *regp = caar; break;
	case 0x803: *regp = regs.m == 1 ? m68k_areg (&regs, 7) : regs.msp; break;
	case 0x804: *regp = regs.m == 0 ? m68k_areg (&regs, 7) : regs.isp; break;
	case 0x805: *regp = mmusr; break;
	case 0x806: *regp = urp; break;
	case 0x807: *regp = srp; break;
	case 0x808: *regp = 0x04300100 | pcr; break;

	default:
	    op_illg (0x4E7A, &regs);
	    return 0;
	}
    }
//    write_log ("-> %08x\n", *regp);
    return 1;
}

/****i* januae-cpu/div_unsigned *******************************************************
*
*   NAME
*       div_unsigned() -- Divide an unsigned 64-bit value.
*
*   SYNOPSIS
*       STATIC_INLINE int div_unsigned(uae_u32 src_hi, uae_u32 src_lo, uae_u32
*       div, uae_u32 *quot, uae_u32 *rem);
*
*   FUNCTION
*       Divide a high/low 32-bit dividend by a 32-bit divisor using a
*       shift-subtract loop. Return an overflow indication without updating the
*       output words when the quotient will not fit.
*
*   INPUTS
*       src_hi - High word of the dividend.
*       src_lo - Low word of the dividend.
*       div - Unsigned divisor.
*       quot - Output quotient.
*       rem - Output remainder.
*
***************************************************************************
*
*/

STATIC_INLINE int
div_unsigned(uae_u32 src_hi, uae_u32 src_lo, uae_u32 div, uae_u32 *quot, uae_u32 *rem)
{
	uae_u32 q = 0, cbit = 0;
	int i;

	if (div <= src_hi) {
	    return 1;
	}
	for (i = 0 ; i < 32 ; i++) {
		cbit = src_hi & 0x80000000ul;
		src_hi <<= 1;
		if (src_lo & 0x80000000ul) src_hi++;
		src_lo <<= 1;
		q = q << 1;
		if (cbit || div <= src_hi) {
			q |= 1;
			src_hi -= div;
		}
	}
	*quot = q;
	*rem = src_hi;
	return 0;
}

/****i* januae-cpu/m68k_divl **********************************************************
*
*   NAME
*       m68k_divl() -- Execute long division.
*
*   SYNOPSIS
*       void m68k_divl (uae_u32 opcode, uae_u32 src, uae_u16 extra, uaecptr
*       oldpc);
*
*   FUNCTION
*       Emulate the signed or unsigned long-division form selected by the
*       extension word, updating destination registers and CPU flags or raising
*       division exceptions as implemented.
*
*   INPUTS
*       opcode - Encoded 68k instruction.
*       src - Input operand or saved-state cursor.
*       extra - Instruction extension word.
*       oldpc - Program counter associated with the exception.
*
***************************************************************************
*
*/

void m68k_divl (uae_u32 opcode, uae_u32 src, uae_u16 extra, uaecptr oldpc)
{
#if defined(uae_s64)
    if (src == 0) {
	uae_Exception (5, &regs, oldpc);
	return;
    }
    if (extra & 0x800) {
	/* signed variant */
	uae_s64 a = (uae_s64)(uae_s32)m68k_dreg (&regs, (extra >> 12) & 7);
	uae_s64 quot, rem;

	if (extra & 0x400) {
	    a &= 0xffffffffu;
	    a |= (uae_s64)m68k_dreg (&regs, extra & 7) << 32;
	}
	rem = a % (uae_s64)(uae_s32)src;
	quot = a / (uae_s64)(uae_s32)src;
	if ((quot & UVAL64(0xffffffff80000000)) != 0
	    && (quot & UVAL64(0xffffffff80000000)) != UVAL64(0xffffffff80000000))
	{
	    SET_VFLG (&regs.ccrflags, 1);
	    SET_NFLG (&regs.ccrflags, 1);
	    SET_CFLG (&regs.ccrflags, 0);
	} else {
	    if (((uae_s32)rem < 0) != ((uae_s64)a < 0)) rem = -rem;
	    SET_VFLG (&regs.ccrflags, 0);
	    SET_CFLG (&regs.ccrflags, 0);
	    SET_ZFLG (&regs.ccrflags, ((uae_s32)quot) == 0);
	    SET_NFLG (&regs.ccrflags, ((uae_s32)quot) < 0);
	    m68k_dreg (&regs, extra & 7) = (uae_u32)rem;
	    m68k_dreg (&regs, (extra >> 12) & 7) = (uae_u32)quot;
	}
    } else {
	/* unsigned */
	uae_u64 a = (uae_u64)(uae_u32)m68k_dreg (&regs, (extra >> 12) & 7);
	uae_u64 quot, rem;

	if (extra & 0x400) {
	    a &= 0xffffffffu;
	    a |= (uae_u64)m68k_dreg (&regs, extra & 7) << 32;
	}
	rem = a % (uae_u64)src;
	quot = a / (uae_u64)src;
	if (quot > 0xffffffffu) {
	    SET_VFLG (&regs.ccrflags, 1);
	    SET_NFLG (&regs.ccrflags, 1);
	    SET_CFLG (&regs.ccrflags, 0);
	} else {
	    SET_VFLG (&regs.ccrflags, 0);
	    SET_CFLG (&regs.ccrflags, 0);
	    SET_ZFLG (&regs.ccrflags, ((uae_s32)quot) == 0);
	    SET_NFLG (&regs.ccrflags, ((uae_s32)quot) < 0);
	    m68k_dreg (&regs, extra & 7) = (uae_u32)rem;
	    m68k_dreg (&regs, (extra >> 12) & 7) = (uae_u32)quot;
	}
    }
#else
    if (src == 0) {
	uae_Exception (5, &regs, oldpc);
	return;
    }
    if (extra & 0x800) {
	/* signed variant */
	uae_s32 lo = (uae_s32)m68k_dreg (&regs, (extra >> 12) & 7);
	uae_s32 hi = lo < 0 ? -1 : 0;
	uae_s32 save_high;
	uae_u32 quot, rem;
	uae_u32 sign;

	if (extra & 0x400) {
	    hi = (uae_s32)m68k_dreg (&regs, extra & 7);
	}
	save_high = hi;
	sign = (hi ^ src);
	if (hi < 0) {
	    hi = ~hi;
	    lo = -lo;
	    if (lo == 0) hi++;
	}
	if ((uae_s32)src < 0) src = -src;
	if (div_unsigned(hi, lo, src, &quot, &rem) ||
	    (sign & 0x80000000) ? quot > 0x80000000 : quot > 0x7fffffff) {
	    SET_VFLG (&regs.ccrflags, 1);
	    SET_NFLG (&regs.ccrflags, 1);
	    SET_CFLG (&regs.ccrflags, 0);
	} else {
	    if (sign & 0x80000000) quot = -quot;
	    if (((uae_s32)rem < 0) != (save_high < 0)) rem = -rem;
	    SET_VFLG (&regs.ccrflags, 0);
	    SET_CFLG (&regs.ccrflags, 0);
	    SET_ZFLG (&regs.ccrflags, ((uae_s32)quot) == 0);
	    SET_NFLG (&regs.ccrflags, ((uae_s32)quot) < 0);
	    m68k_dreg (&regs, extra & 7) = rem;
	    m68k_dreg (&regs, (extra >> 12) & 7) = quot;
	}
    } else {
	/* unsigned */
	uae_u32 lo = (uae_u32)m68k_dreg (&regs, (extra >> 12) & 7);
	uae_u32 hi = 0;
	uae_u32 quot, rem;

	if (extra & 0x400) {
	    hi = (uae_u32)m68k_dreg (&regs, extra & 7);
	}
	if (div_unsigned(hi, lo, src, &quot, &rem)) {
	    SET_VFLG (&regs.ccrflags, 1);
	    SET_NFLG (&regs.ccrflags, 1);
	    SET_CFLG (&regs.ccrflags, 0);
	} else {
	    SET_VFLG (&regs.ccrflags, 0);
	    SET_CFLG (&regs.ccrflags, 0);
	    SET_ZFLG (&regs.ccrflags, ((uae_s32)quot) == 0);
	    SET_NFLG (&regs.ccrflags, ((uae_s32)quot) < 0);
	    m68k_dreg (&regs, extra & 7) = rem;
	    m68k_dreg (&regs, (extra >> 12) & 7) = quot;
	}
    }
#endif
}

/****i* januae-cpu/mul_unsigned *******************************************************
*
*   NAME
*       mul_unsigned() -- Multiply two unsigned 32-bit values.
*
*   SYNOPSIS
*       STATIC_INLINE void mul_unsigned (uae_u32 src1, uae_u32 src2, uae_u32
*       *dst_hi, uae_u32 *dst_lo);
*
*   FUNCTION
*       Calculate the 64-bit product and write its high and low words through
*       the supplied pointers.
*
*   INPUTS
*       src1 - First multiplicand.
*       src2 - Second multiplicand.
*       dst_hi - Output high product word.
*       dst_lo - Output low product word.
*
***************************************************************************
*
*/

STATIC_INLINE void
mul_unsigned (uae_u32 src1, uae_u32 src2, uae_u32 *dst_hi, uae_u32 *dst_lo)
{
	uae_u32 r0 = (src1 & 0xffff) * (src2 & 0xffff);
	uae_u32 r1 = ((src1 >> 16) & 0xffff) * (src2 & 0xffff);
	uae_u32 r2 = (src1 & 0xffff) * ((src2 >> 16) & 0xffff);
	uae_u32 r3 = ((src1 >> 16) & 0xffff) * ((src2 >> 16) & 0xffff);
	uae_u32 lo;

	lo = r0 + ((r1 << 16) & 0xffff0000ul);
	if (lo < r0) r3++;
	r0 = lo;
	lo = r0 + ((r2 << 16) & 0xffff0000ul);
	if (lo < r0) r3++;
	r3 += ((r1 >> 16) & 0xffff) + ((r2 >> 16) & 0xffff);
	*dst_lo = lo;
	*dst_hi = r3;
}

/****i* januae-cpu/m68k_mull **********************************************************
*
*   NAME
*       m68k_mull() -- Execute long multiplication.
*
*   SYNOPSIS
*       void m68k_mull (uae_u32 opcode, uae_u32 src, uae_u16 extra);
*
*   FUNCTION
*       Apply the extension-word register selection and signed or unsigned
*       multiplication rules, updating the result register or pair and condition
*       codes.
*
*   INPUTS
*       opcode - Encoded 68k instruction.
*       src - Input operand or saved-state cursor.
*       extra - Instruction extension word.
*
***************************************************************************
*
*/

void m68k_mull (uae_u32 opcode, uae_u32 src, uae_u16 extra)
{
#if defined(uae_s64)
    if (extra & 0x800) {
	/* signed variant */
	uae_s64 a = (uae_s64)(uae_s32)m68k_dreg (&regs, (extra >> 12) & 7);

	a *= (uae_s64)(uae_s32)src;
	SET_VFLG (&regs.ccrflags, 0);
	SET_CFLG (&regs.ccrflags, 0);
	SET_ZFLG (&regs.ccrflags, a == 0);
	SET_NFLG (&regs.ccrflags, a < 0);
	if (extra & 0x400)
	    m68k_dreg (&regs, extra & 7) = (uae_u32)(a >> 32);
	else if ((a & UVAL64(0xffffffff80000000)) != 0
		 && (a & UVAL64(0xffffffff80000000)) != UVAL64(0xffffffff80000000))
	{
	    SET_VFLG (&regs.ccrflags, 1);
	}
	m68k_dreg (&regs, (extra >> 12) & 7) = (uae_u32)a;
    } else {
	/* unsigned */
	uae_u64 a = (uae_u64)(uae_u32)m68k_dreg (&regs, (extra >> 12) & 7);

	a *= (uae_u64)src;
	SET_VFLG (&regs.ccrflags, 0);
	SET_CFLG (&regs.ccrflags, 0);
	SET_ZFLG (&regs.ccrflags, a == 0);
	SET_NFLG (&regs.ccrflags, ((uae_s64)a) < 0);
	if (extra & 0x400)
	    m68k_dreg (&regs, extra & 7) = (uae_u32)(a >> 32);
	else if ((a & UVAL64(0xffffffff00000000)) != 0) {
	    SET_VFLG (&regs.ccrflags, 1);
	}
	m68k_dreg (&regs, (extra >> 12) & 7) = (uae_u32)a;
    }
#else
    if (extra & 0x800) {
	/* signed variant */
	uae_s32 src1,src2;
	uae_u32 dst_lo,dst_hi;
	uae_u32 sign;

	src1 = (uae_s32)src;
	src2 = (uae_s32)m68k_dreg (&regs, (extra >> 12) & 7);
	sign = (src1 ^ src2);
	if (src1 < 0) src1 = -src1;
	if (src2 < 0) src2 = -src2;
	mul_unsigned((uae_u32)src1,(uae_u32)src2,&dst_hi,&dst_lo);
	if (sign & 0x80000000) {
		dst_hi = ~dst_hi;
		dst_lo = -dst_lo;
		if (dst_lo == 0) dst_hi++;
	}
	SET_VFLG (&regs.ccrflags, 0);
	SET_CFLG (&regs.ccrflags, 0);
	SET_ZFLG (&regs.ccrflags, dst_hi == 0 && dst_lo == 0);
	SET_NFLG (&regs.ccrflags, ((uae_s32)dst_hi) < 0);
	if (extra & 0x400)
	    m68k_dreg (&regs, extra & 7) = dst_hi;
	else if ((dst_hi != 0 || (dst_lo & 0x80000000) != 0)
		 && ((dst_hi & 0xffffffff) != 0xffffffff
		     || (dst_lo & 0x80000000) != 0x80000000))
	{
	    SET_VFLG (&regs.ccrflags, 1);
	}
	m68k_dreg (&regs, (extra >> 12) & 7) = dst_lo;
    } else {
	/* unsigned */
	uae_u32 dst_lo,dst_hi;

	mul_unsigned(src,(uae_u32)m68k_dreg (&regs, (extra >> 12) & 7),&dst_hi,&dst_lo);

	SET_VFLG (&regs.ccrflags, 0);
	SET_CFLG (&regs.ccrflags, 0);
	SET_ZFLG (&regs.ccrflags, dst_hi == 0 && dst_lo == 0);
	SET_NFLG (&regs.ccrflags, ((uae_s32)dst_hi) < 0);
	if (extra & 0x400)
	    m68k_dreg (&regs, extra & 7) = dst_hi;
	else if (dst_hi != 0) {
	    SET_VFLG (&regs.ccrflags, 1);
	}
	m68k_dreg (&regs, (extra >> 12) & 7) = dst_lo;
    }
#endif
}

#endif


/****i* januae-cpu/m68k_reset *********************************************************
*
*   NAME
*       m68k_reset() -- Initialize CPU register state after reset.
*
*   SYNOPSIS
*       void m68k_reset (void);
*
*   FUNCTION
*       Initialize the instruction and stack pointers from the Kickstart reset
*       vectors, reset condition and control registers, and fill the instruction
*       prefetch state. During SaveState recovery, restore state without reading
*       the reset vectors.
*
***************************************************************************
*
*/

void m68k_reset (void)
{
    regs.kick_mask = 0x00F80000;
    regs.spcflags = 0;

#ifdef SAVESTATE
    if (savestate_state == STATE_RESTORE || savestate_state == STATE_REWIND) {
	m68k_setpc (&regs, regs.pc);
	/* MakeFromSR() must not swap stack pointer */
	regs.s = (regs.sr >> 13) & 1;
	MakeFromSR (&regs);
	/* set stack pointer */
	if (regs.s)
	    m68k_areg (&regs, 7) = regs.isp;
	else
	    m68k_areg (&regs, 7) = regs.usp;
	return;
    }
#endif

    m68k_areg  (&regs, 7) = get_long (0x00f80000);
    m68k_setpc (&regs, get_long (0x00f80004));
    regs.s = 1;
    regs.m = 0;
    regs.stopped = 0;
    regs.t1 = 0;
    regs.t0 = 0;
    SET_ZFLG (&regs.ccrflags, 0);
    SET_XFLG (&regs.ccrflags, 0);
    SET_CFLG (&regs.ccrflags, 0);
    SET_VFLG (&regs.ccrflags, 0);
    SET_NFLG (&regs.ccrflags, 0);
    regs.intmask = 7;
    regs.vbr = regs.sfc = regs.dfc = 0;
#ifdef FPUEMU
    regs.fpcr = regs.fpsr = regs.fpiar = 0;
    regs.fp_result=1;
    regs.irc = 0xffff;
#endif
    fill_prefetch_slow (&regs);

    warned_cpu68020 = 0;
}

/****i* januae-cpu/in_rom *************************************************************
*
*   NAME
*       in_rom() -- Test for Kickstart address space.
*
*   SYNOPSIS
*       STATIC_INLINE int in_rom (uaecptr pc);
*
*   FUNCTION
*       Return whether the 24-bit-mapped PC lies in the primary Kickstart ROM
*       region at F80000.
*
*   INPUTS
*       pc - Emulated program counter.
*
***************************************************************************
*
*/

STATIC_INLINE int in_rom (uaecptr pc)
{
    return (munge24 (pc) & 0xFFF80000) == 0xF80000;
}

/****i* januae-cpu/in_rtarea **********************************************************
*
*   NAME
*       in_rtarea() -- Test for emulator runtime address space.
*
*   SYNOPSIS
*       STATIC_INLINE int in_rtarea (uaecptr pc);
*
*   FUNCTION
*       Return whether the 24-bit-mapped PC lies in the emulated runtime trap
*       area.
*
*   INPUTS
*       pc - Emulated program counter.
*
***************************************************************************
*
*/

STATIC_INLINE int in_rtarea (uaecptr pc)
{
    return (munge24 (pc) & 0xFFFF0000) == RTAREA_BASE;
}

/****i* januae-cpu/op_illg ************************************************************
*
*   NAME
*       op_illg() -- Handle an illegal or special emulator opcode.
*
*   SYNOPSIS
*       unsigned long REGPARAM2 op_illg (uae_u32 opcode, struct regstruct
*       *regs);
*
*   FUNCTION
*       Recognize selected Kickstart and runtime-area trap encodings before
*       raising an illegal-instruction or line-A/line-F exception. Return the
*       fixed handler cycle count.
*
*   INPUTS
*       opcode - Encoded 68k instruction.
*       regs - Register state read or modified by the operation.
*
***************************************************************************
*
*/

unsigned long REGPARAM2 op_illg (uae_u32 opcode, struct regstruct *regs)
{
    uaecptr pc = m68k_getpc (regs);
    static int warned;
    int inrom = in_rom (pc);
    int inrt = in_rtarea (pc);

    if (cloanto_rom && (opcode & 0xF100) == 0x7100) {
	m68k_dreg  (regs, (opcode >> 9) & 7) = (uae_s8)(opcode & 0xFF);
	m68k_incpc (regs, 2);
	fill_prefetch_slow (regs);
	return 4;
    }

    if (opcode == 0x4E7B && get_long (0x10) == 0 && inrom && !warned_cpu68020) {
	gui_message ("Your Kickstart requires a 68020 CPU");
	warned_cpu68020 = 1;
	uae_restart (-1, NULL);
    }

#ifdef AUTOCONFIG
    if (opcode == 0xFF0D) {
	if (inrom) {
	    /* This is from the dummy Kickstart replacement */
	    uae_u16 arg = get_iword (regs, 2);
	    m68k_incpc (regs, 4);
	    ersatz_perform (arg);
	    fill_prefetch_slow (regs);
	    return 4;
	} else if (inrt) {
	    /* User-mode STOP replacement */
	    m68k_setstopped (regs, 1);
	    return 4;
	}
    }

    if ((opcode & 0xF000) == 0xA000 && inrt) {
	/* Calltrap. */
	m68k_incpc (regs, 2);
	m68k_handle_trap (opcode & 0xFFF, regs);
	fill_prefetch_slow (regs);
	return 4;
    }
#endif

    if ((opcode & 0xF000) == 0xF000) {
	if (warned < 20) {
 	    write_log ("B-Trap %04x at %08x (%p)\n", opcode, m68k_getpc (regs) + m68kpc_offset, regs->pc_p);
	    warned++;
	}
	uae_Exception (0xB, regs, 0);
	return 4;
    }
    if ((opcode & 0xF000) == 0xA000) {
#ifdef AUTOCONFIG
	if (inrt) {
	    /* Calltrap. */
	    m68k_handle_trap (opcode & 0xFFF, regs);
	}
#endif
	uae_Exception (0xA, regs, 0);
	return 4;
    }
    if (warned < 20) {
	write_log ("Illegal instruction: %04x at %08x -> %08x\n", opcode, pc, get_long (regs->vbr + 0x10));
	warned++;
    }

    uae_Exception (4, regs, 0);
    return 4;
}

#ifdef CPUEMU_0

/****i* januae-cpu/mmu_op *************************************************************
*
*   NAME
*       mmu_op() -- Interpret selected MMU instruction encodings.
*
*   SYNOPSIS
*       void mmu_op(uae_u32 opcode, struct regstruct *regs, uae_u16 extra);
*
*   FUNCTION
*       Handle the recognized PFLUSH and PTEST encodings by logging or clearing
*       MMU status; dispatch all other encodings as illegal instructions. This
*       is not full MMU emulation.
*
*   INPUTS
*       opcode - Encoded 68k instruction.
*       regs - Register state read or modified by the operation.
*       extra - Instruction extension word.
*
***************************************************************************
*
*/

void mmu_op(uae_u32 opcode, struct regstruct *regs, uae_u16 extra)
{
    if ((opcode & 0xFE0) == 0x0500) {
	/* PFLUSH */
	mmusr = 0;
	write_log ("PFLUSH @ %08x\n", m68k_getpc (regs));
    } else if ((opcode & 0x0FD8) == 0x548) {
	/* PTEST */
	write_log ("PTEST @ %08x\n", m68k_getpc (regs));
    } else
	op_illg (opcode, regs);
}

#endif

static uaecptr last_trace_ad = 0;

/****i* januae-cpu/do_trace ***********************************************************
*
*   NAME
*       do_trace() -- Advance pending CPU trace exceptions.
*
*   SYNOPSIS
*       static NOINLINE void do_trace (struct regstruct *regs);
*
*   FUNCTION
*       Examine trace mode and the next instruction to schedule a trace
*       exception at the appropriate PC when a trace condition is met.
*
*   INPUTS
*       regs - Register state read or modified by the operation.
*
***************************************************************************
*
*/

static NOINLINE void do_trace (struct regstruct *regs)
{
    if (regs->t0 && currprefs.cpu_level >= 2) {
	uae_u16 opcode;
	/* should also include TRAP, CHK, SR modification FPcc */
	/* probably never used so why bother */
	/* We can afford this to be inefficient... */
	m68k_setpc (regs, m68k_getpc (regs));
	fill_prefetch_slow (regs);
	opcode = get_word (regs->pc);
	if (   opcode == 0x4e72 		/* RTE */
	    || opcode == 0x4e74 		/* RTD */
	    || opcode == 0x4e75 		/* RTS */
	    || opcode == 0x4e77 		/* RTR */
	    || opcode == 0x4e76 		/* TRAPV */
	    || (opcode & 0xffc0) == 0x4e80 	/* JSR */
	    || (opcode & 0xffc0) == 0x4ec0 	/* JMP */
	    || (opcode & 0xff00) == 0x6100 	/* BSR */
	    || ((opcode & 0xf000) == 0x6000	/* Bcc */
		&& cctrue (&regs->ccrflags, (opcode >> 8) & 0xf))
	    || ((opcode & 0xf0f0) == 0x5050 /* DBcc */
		&& !cctrue (&regs->ccrflags, (opcode >> 8) & 0xf)
		&& (uae_s16) m68k_dreg (regs, opcode & 7) != 0))
	{
	    last_trace_ad = m68k_getpc (regs);
	    unset_special (regs, SPCFLAG_TRACE);
	    set_special (regs, SPCFLAG_DOTRACE);
	}
    } else if (regs->t1) {
	last_trace_ad = m68k_getpc (regs);
	unset_special (regs, SPCFLAG_TRACE);
	set_special (regs, SPCFLAG_DOTRACE);
    }
}

/*
 * Handle the STOP opcode.
 *
 * The 68k CPU stays in stopped state, essentially doing
 * nothing, until an IRQ arrives,
 */
/****i* januae-cpu/do_stop ************************************************************
*
*   NAME
*       do_stop() -- Wait while the emulated CPU is stopped.
*
*   SYNOPSIS
*       static NOINLINE void do_stop (struct regstruct *regs);
*
*   FUNCTION
*       Advance hardware cycles and Copper activity until an interrupt, stop
*       cancellation or execution-mode change ends the STOP state; optionally
*       sleep the host during idle periods.
*
*   INPUTS
*       regs - Register state read or modified by the operation.
*
***************************************************************************
*
*/

static NOINLINE void do_stop (struct regstruct *regs)
{
    unsigned int stop_count = 0;

    do {
	/* An IRQ breaks the STOP state. */
	if (regs->spcflags & (SPCFLAG_INT | SPCFLAG_DOINT))
	    break;

	/* E-UAE wants us to stop interpeting opcodes for some reason -
	 * perhaps to handle changes in settings - and this will
	 * implicitly break the STOP state.
	 */
	if ((regs->spcflags & (SPCFLAG_BRK | SPCFLAG_MODE_CHANGE)))
	    break;

	if (currprefs.cpu_idle && currprefs.m68k_speed != 0) {
	    /* The CPU idle thang. We attempt to stop hogging the host
	     * CPU when the emulated 68k ain't doing nothing.
	     */
	    static int          sleep_countdown;
	    static unsigned int lvpos;

	    /* Check that we are not passed the end a frame. If
	     * we are passed, then no IRQs will occur to get us out
	     * of the STOP state, so we don't want to sleep.
	     *
	     * We could have used !is_lastline here, but the JIT mucks
	     * around with that for its own evil purposes.
	     */
	    if (lvpos != vpos) {
		lvpos = vpos;

		stop_count++; /* Count of STOP cycles for this execution of STOP. */

		sleep_countdown--;
		if (sleep_countdown < 0) {

		    sleep_countdown = currprefs.cpu_idle;

		    /* Avoid small numbers of STOP cycles. Arbitrary number here. */
		    if (stop_count > 32) {

		        frame_time_t curr_time = uae_gethrtime ();

			uae_msleep (1);

			idletime += uae_gethrtime() - curr_time;
		    }
	        }
	    }
	}

	do_cycles (4 * CYCLE_UNIT);

	if (regs->spcflags & SPCFLAG_COPPER)
	    do_copper ();

    } while (regs->spcflags & SPCFLAG_STOP);
}


/****i* januae-cpu/do_specialties *****************************************************
*
*   NAME
*       do_specialties() -- Process pending special CPU states.
*
*   SYNOPSIS
*       STATIC_INLINE int do_specialties (int cycles, struct regstruct *regs);
*
*   FUNCTION
*       Service trace, STOP, DMA-related cycle work, interrupt delivery, break
*       or mode-change flags and optional cartridge monitor hooks. Return
*       nonzero when the opcode execution loop must exit.
*
*   INPUTS
*       cycles - Cycles already consumed by the current instruction.
*       regs - Register state read or modified by the operation.
*
***************************************************************************
*
*/

STATIC_INLINE int do_specialties (int cycles, struct regstruct *regs)
{
    #ifdef ACTION_REPLAY
    #ifdef ACTION_REPLAY_HRTMON
    if ((regs->spcflags & SPCFLAG_ACTION_REPLAY) && hrtmon_flag != ACTION_REPLAY_INACTIVE) {
	int isinhrt = (m68k_getpc (regs) >= hrtmem_start && m68k_getpc (regs) < hrtmem_start + hrtmem_size);
	/* exit from HRTMon? */
	if(hrtmon_flag == ACTION_REPLAY_HIDE && !isinhrt) hrtmon_hide();
	/* HRTMon breakpoint? (not via IRQ7) */
	if(hrtmon_flag == ACTION_REPLAY_IDLE && isinhrt) hrtmon_breakenter();
	if(hrtmon_flag == ACTION_REPLAY_ACTIVE && isinhrt) hrtmon_flag = ACTION_REPLAY_HIDE;
	if(hrtmon_flag == ACTION_REPLAY_ACTIVATE) hrtmon_enter();
	if(!(regs->spcflags & ~SPCFLAG_ACTION_REPLAY)) return 0;
    }
    #endif
    if ((regs->spcflags & SPCFLAG_ACTION_REPLAY) && action_replay_flag != ACTION_REPLAY_INACTIVE )
    {
	/*if(action_replay_flag == ACTION_REPLAY_ACTIVE && !is_ar_pc_in_rom())*/
	/*	write_log ("PC:%p\n", m68k_getpc (regs));*/

	if(action_replay_flag == ACTION_REPLAY_ACTIVATE || action_replay_flag == ACTION_REPLAY_DORESET)
	    action_replay_enter();
	if(action_replay_flag == ACTION_REPLAY_HIDE && !is_ar_pc_in_rom())
	{
	    action_replay_hide();
	    unset_special (regs, SPCFLAG_ACTION_REPLAY);
	}
	if (action_replay_flag == ACTION_REPLAY_WAIT_PC )
	{
	    /*write_log ("Waiting for PC: %p, current PC= %p\n", wait_for_pc, m68k_getpc (regs));*/
	    if (m68k_getpc (regs) == wait_for_pc)
	    {
		action_replay_flag = ACTION_REPLAY_ACTIVATE; /* Activate after next instruction. */
	    }
	}
    }
    #endif

    if (regs->spcflags & SPCFLAG_COPPER)
	do_copper ();

    /*n_spcinsns++;*/
#ifdef JIT
    unset_special (regs, SPCFLAG_END_COMPILE);   /* has done its job */
#endif

    while ((regs->spcflags & SPCFLAG_BLTNASTY) && dmaen (DMA_BLITTER) && cycles > 0 && !currprefs.blitter_cycle_exact) {
	unsigned int c = blitnasty ();
	if (c > 0) {
	    cycles -= c * CYCLE_UNIT * 2;
	    if (cycles < CYCLE_UNIT)
		cycles = 0;
	} else
	    c = 4;
	do_cycles (c * CYCLE_UNIT);
	if (regs->spcflags & SPCFLAG_COPPER)
	    do_copper ();
    }

    if (regs->spcflags & SPCFLAG_DOTRACE)
	uae_Exception (9, regs, last_trace_ad);
    if (regs->spcflags & SPCFLAG_TRAP) {
	unset_special (regs, SPCFLAG_TRAP);
	uae_Exception (3, regs, 0);
    }

    if (regs->spcflags & SPCFLAG_STOP)
	do_stop (regs);

    if (regs->spcflags & SPCFLAG_TRACE)
	do_trace (regs);

    /* An interrupt request takes at least 2 cycles (maybe 4) to reach the CPU, and
     * there are programs that require this delay - which is not too surprising...
     *
     * In non-cycle-exact mode we handle this by separating the interrupt request
     * pending (SPCFLAG_INT) and interrupt request arrived (SPCFLAG_DOINT) events.
     * This ensures that there's always a delay of one opcode (and so at least 2
     * machine cycles) between the interrupt controller requesting an interrupt
     * and us servicing it here.
     *
     * In cycle-exact mode, there's just one event (SPCFLAG_INT) and the delay is
     * handled internally by the interrupt controller code in custom.c - intlev()
     * and friends.
     */
    if ((regs->spcflags & SPCFLAG_DOINT) ||
	(currprefs.cpu_cycle_exact && (regs->spcflags & SPCFLAG_INT))) {

	int intr = intlev ();

	unset_special (regs, SPCFLAG_DOINT);

	if (intr != -1)
	    service_interrupt (intr, regs);
    }

    if ((regs->spcflags & SPCFLAG_INT) && !currprefs.cpu_cycle_exact) {
	unset_special (regs, SPCFLAG_INT);
	set_special (regs, SPCFLAG_DOINT);
    }

    if ((regs->spcflags & (SPCFLAG_BRK | SPCFLAG_MODE_CHANGE))) {
	unset_special (regs, SPCFLAG_BRK | SPCFLAG_MODE_CHANGE);
	return 1;
    }
    return 0;
}


#ifndef CPUEMU_5

/****i* januae-cpu/m68k_run_1-1 ******************************************************
*
*   NAME
*       m68k_run_1() -- Run the 68000 prefetch execution loop.
*
*   SYNOPSIS
*       static void m68k_run_1 (void);
*
*   FUNCTION
*       In CPUEMU_5 builds, dispatch opcodes from the emulated instruction
*       prefetch register and account for cycles and special events; the
*       alternative build definition is empty.
*
***************************************************************************
*
*/

static void m68k_run_1 (void)
{
}

#else

/* It's really sad to have two almost identical functions for this, but we
   do it all for performance... :(
   This version emulates 68000's prefetch "cache" */
/****i* januae-cpu/m68k_run_1-2 ******************************************************
*
*   NAME
*       m68k_run_1() -- Run the 68000 prefetch execution loop.
*
*   SYNOPSIS
*       static void m68k_run_1 (void);
*
*   FUNCTION
*       In CPUEMU_5 builds, dispatch opcodes from the emulated instruction
*       prefetch register and account for cycles and special events; the
*       alternative build definition is empty.
*
***************************************************************************
*
*/

static void m68k_run_1 (void)
{
    struct regstruct *r = &regs;
    unsigned long cycles_mask_local = cycles_mask;
    unsigned long cycles_val_local  = cycles_val;

    for (;;) {
	unsigned long cycles;
	uae_u32 opcode = r->ir;
	count_instr (opcode);

	cycles = (*cpufunctbl[opcode])(opcode, r);

	cycles &= cycles_mask_local;
	cycles |= cycles_val_local;
	do_cycles (cycles);
	if (r->spcflags) {
	    if (do_specialties (cycles, r))
		return;
	}
    }
}

#endif /* CPUEMU_5 */

#ifndef CPUEMU_6

/****i* januae-cpu/m68k_run_1_ce-1 ***************************************************
*
*   NAME
*       m68k_run_1_ce() -- Run the cycle-exact 68000 loop.
*
*   SYNOPSIS
*       static void m68k_run_1_ce (void);
*
*   FUNCTION
*       In CPUEMU_6 builds, execute instructions from the prefetch register and
*       process special events after each instruction; the alternative build
*       definition is empty.
*
***************************************************************************
*
*/

static void m68k_run_1_ce (void)
{
}

#else

/* cycle-exact m68k_run() */

/****i* januae-cpu/m68k_run_1_ce-2 ***************************************************
*
*   NAME
*       m68k_run_1_ce() -- Run the cycle-exact 68000 loop.
*
*   SYNOPSIS
*       static void m68k_run_1_ce (void);
*
*   FUNCTION
*       In CPUEMU_6 builds, execute instructions from the prefetch register and
*       process special events after each instruction; the alternative build
*       definition is empty.
*
***************************************************************************
*
*/

static void m68k_run_1_ce (void)
{
    struct regstruct *r = &regs;

    for (;;) {
	uae_u32 opcode = r->ir;

	(*cpufunctbl[opcode])(opcode, r);
	if (r->spcflags) {
	    if (do_specialties (0, r))
		return;
	}
    }
}
#endif /* CPUEMU_6 */

#ifdef JIT  /* Completely different run_2 replacement */

/****i* januae-cpu/do_nothing *********************************************************
*
*   NAME
*       do_nothing() -- Advance zero host-independent CPU cycles.
*
*   SYNOPSIS
*       void do_nothing(void);
*
*   FUNCTION
*       Invoke the cycle accounting routine with a zero-cycle request; compiled
*       only with JIT.
*
***************************************************************************
*
*/

void do_nothing(void)
{
    /* What did you expect this to do? */
    do_cycles(0);
    /* I bet you didn't expect *that* ;-) */
}

/****i* januae-cpu/exec_nostats *******************************************************
*
*   NAME
*       exec_nostats() -- Execute an interpreter block for JIT.
*
*   SYNOPSIS
*       void exec_nostats(void);
*
*   FUNCTION
*       Dispatch opcodes until a block-ending opcode or pending special state;
*       account for emulated cycles without compiling a new block.
*
***************************************************************************
*
*/

void exec_nostats(void)
{
    struct regstruct *r = &regs;
    unsigned long cycles_mask_local = cycles_mask;
    unsigned long cycles_val_local  = cycles_val;

    for (;;)
    {
	int new_cycles;
	uae_u32 opcode = get_iword (r, 0);

	new_cycles = (*cpufunctbl[opcode])(opcode, r);

	new_cycles &= cycles_mask_local;
	new_cycles |= cycles_val_local;
	do_cycles (new_cycles);

	if (end_block(opcode) || r->spcflags)
	    return; /* We will deal with the spcflags in the caller */
    }
}

static int triggered;

/****i* januae-cpu/execute_normal *****************************************************
*
*   NAME
*       execute_normal() -- Collect and compile an interpreted block.
*
*   SYNOPSIS
*       void execute_normal(void);
*
*   FUNCTION
*       Interpret instructions while recording instruction locations and
*       special-memory effects; hand the resulting history to the JIT compiler
*       at a block boundary or instruction limit.
*
***************************************************************************
*
*/

void execute_normal(void)
{
    struct regstruct *r = &regs;
    unsigned long cycles_mask_local = cycles_mask;
    unsigned long cycles_val_local  = cycles_val;
    int blocklen;
    cpu_history pc_hist[MAXRUN];
    int new_cycles;
    int total_cycles;

    if (check_for_cache_miss())
	return;

    total_cycles = 0;
    blocklen = 0;
    start_pc_p = r->pc_oldp;
    start_pc   = r->pc;

    for (;;) {
	/* Take note: This is the do-it-normal loop */
	uae_u32 opcode = get_iword (r, 0);

	special_mem = DISTRUST_CONSISTENT_MEM;
	pc_hist[blocklen].location = (uae_u16*)r->pc_p;

	new_cycles = (*cpufunctbl[opcode])(opcode, r);

	new_cycles &= cycles_mask_local;
	new_cycles |= cycles_val_local;
	do_cycles (new_cycles);
	total_cycles += new_cycles;
	pc_hist[blocklen].specmem = special_mem;
	blocklen++;
	if (end_block(opcode) || blocklen >= MAXRUN || r->spcflags) {
	    compile_block(pc_hist,blocklen,total_cycles);
	    return; /* We will deal with the spcflags in the caller */
	}
	/* No need to check regs.spcflags, because if they were set,
	   we'd have ended up inside that "if" */
    }
}

typedef void compiled_handler(void);

/****i* januae-cpu/m68k_run_2a ********************************************************
*
*   NAME
*       m68k_run_2a() -- Execute compiled JIT blocks.
*
*   SYNOPSIS
*       static void m68k_run_2a (void);
*
*   FUNCTION
*       Call the installed compiled handler repeatedly, processing special CPU
*       flags after each return until execution must yield.
*
***************************************************************************
*
*/

static void m68k_run_2a (void)
{
    for (;;) {
#if defined X86_ASSEMBLY
	__asm__ __volatile__(
	    "\tpush %%ebp\n\tcall *%0\n\tpop %%ebp"  /* FIXME */
	    :: "m" (cache_tags[cacheline(regs.pc_p)].handler)
	    : "%edx", "%ecx", "%eax",
	    "%esi", "%ebx", "%edi", "%ebp", "memory", "cc");
#else
	((compiled_handler*)(pushall_call_handler))();
#endif
	/* Whenever we return from that, we should check spcflags */
	if (regs.spcflags) {
	    if (do_specialties (0, &regs)) {
		return;
	    }
	}
    }
}
#endif /* JIT */

#ifndef CPUEMU_0

/****i* januae-cpu/m68k_run_2-1 ******************************************************
*
*   NAME
*       m68k_run_2() -- Run the basic opcode interpreter.
*
*   SYNOPSIS
*       static void m68k_run_2 (void);
*
*   FUNCTION
*       In CPUEMU_0 builds, fetch and dispatch successive opcodes, charge cycles
*       and handle special conditions; the alternative build definition is
*       empty.
*
***************************************************************************
*
*/

static void m68k_run_2 (void)
{
}

/****i* januae-cpu/m68k_run_2p-1 *****************************************************
*
*   NAME
*       m68k_run_2p() -- Run an interpreter with emulated prefetch.
*
*   SYNOPSIS
*       static void m68k_run_2p (void);
*
*   FUNCTION
*       In CPUEMU_0 builds, retain a two-word prefetch stream when fetching
*       opcodes, dispatch instruction handlers and process cycles and special
*       conditions; the alternative build definition is empty.
*
***************************************************************************
*
*/

static void m68k_run_2p (void)
{
}

#else

/* emulate simple prefetch  */
/****i* januae-cpu/m68k_run_2p-2 *****************************************************
*
*   NAME
*       m68k_run_2p() -- Run an interpreter with emulated prefetch.
*
*   SYNOPSIS
*       static void m68k_run_2p (void);
*
*   FUNCTION
*       In CPUEMU_0 builds, retain a two-word prefetch stream when fetching
*       opcodes, dispatch instruction handlers and process cycles and special
*       conditions; the alternative build definition is empty.
*
***************************************************************************
*
*/

static void m68k_run_2p (void)
{
    uae_u32 prefetch, prefetch_pc;
    struct regstruct *r = &regs;
    unsigned long cycles_mask_local = cycles_mask;
    unsigned long cycles_val_local  = cycles_val;

    prefetch_pc = m68k_getpc (r);
    prefetch = get_long (prefetch_pc);
    for (;;) {
	unsigned long cycles;
	uae_u32 opcode;
	uae_u32 pc = m68k_getpc (r);
	if (pc == prefetch_pc)
	    opcode = prefetch >> 16;
	else if (pc == prefetch_pc + 2)
	    opcode = prefetch & 0xffff;
	else
	    opcode = get_word (pc);

	count_instr (opcode);

	prefetch_pc = m68k_getpc (r) + 2;
	prefetch = get_long (prefetch_pc);
	cycles = (*cpufunctbl[opcode])(opcode, r);
	cycles &= cycles_mask_local;
	cycles |= cycles_val_local;
	do_cycles (cycles);
	if (r->spcflags) {
	    if (do_specialties (cycles, r))
		return;
	}
    }
}

/* Same thing, but don't use prefetch to get opcode.  */
/****i* januae-cpu/m68k_run_2-2 ******************************************************
*
*   NAME
*       m68k_run_2() -- Run the basic opcode interpreter.
*
*   SYNOPSIS
*       static void m68k_run_2 (void);
*
*   FUNCTION
*       In CPUEMU_0 builds, fetch and dispatch successive opcodes, charge cycles
*       and handle special conditions; the alternative build definition is
*       empty.
*
***************************************************************************
*
*/

static void m68k_run_2 (void)
{
    struct regstruct *r = &regs;
    unsigned long cycles_mask_local = cycles_mask;
    unsigned long cycles_val_local  = cycles_val;

    for (;;) {
	unsigned long cycles;
	uae_u32 opcode = get_iword (r, 0);
	count_instr (opcode);

	cycles = (*cpufunctbl[opcode])(opcode, r);

	cycles &= cycles_mask_local;
	cycles |= cycles_val_local;
	do_cycles (cycles);
	if (r->spcflags) {
	    if (do_specialties (cycles, r))
		return;
	}
    }
}

#endif /* CPUEMU_0 */


/****i* januae-cpu/exception2_handle **************************************************
*
*   NAME
*       exception2_handle() -- Enter a pending bus-error exception.
*
*   SYNOPSIS
*       static void exception2_handle (uaecptr addr, uaecptr fault);
*
*   FUNCTION
*       Copy stored bus-error address information into the exception state and
*       enter exception vector two.
*
*   INPUTS
*       addr - Emulated address, or the address to inspect.
*       fault - Faulting address.
*
***************************************************************************
*
*/

static void exception2_handle (uaecptr addr, uaecptr fault)
{
    last_addr_for_exception_3 = addr;
    last_fault_for_exception_3 = fault;
    last_writeaccess_for_exception_3 = 0;
    last_instructionaccess_for_exception_3 = 0;
    uae_Exception (2, &regs, addr);
}

/****i* januae-cpu/m68k_go ************************************************************
*
*   NAME
*       m68k_go() -- Run the CPU until a state change is pending.
*
*   SYNOPSIS
*       void m68k_go (int may_quit);
*
*   FUNCTION
*       Choose the JIT, cycle-exact, compatible-prefetch or basic interpreter
*       loop for the active preferences; process debug and panic states between
*       loop invocations.
*
*   INPUTS
*       may_quit - Legacy entry argument; unused by this implementation.
*
***************************************************************************
*
*/

void m68k_go (int may_quit)
{
    for (;;) {
	void (*run_func)(void);

#ifdef DEBUGGER
	if (debugging)
	    debug ();
#endif

	if (regs.panic) {
	    regs.panic = 0;
	    /* program jumped to non-existing memory and cpu was >= 68020 */
	    get_real_address (regs.isp); /* stack in no one's land? -> reboot */
	    if (regs.isp & 1)
		regs.panic = 1;
	    if (!regs.panic)
		exception2_handle (regs.panic_pc, regs.panic_addr);
	    if (regs.panic) {
		/* system is very badly confused */
		write_log ("double bus error or corrupted stack, forcing reboot..\n");
		regs.panic = 0;
		uae_reset (1);
	    }
	}

#ifndef JIT
	run_func = (currprefs.cpu_level == 0 && currprefs.cpu_cycle_exact	? m68k_run_1_ce :
		    currprefs.cpu_level == 0 && currprefs.cpu_compatible	? m68k_run_1 :
		    currprefs.cpu_compatible					? m68k_run_2p :
										  m68k_run_2);
#else
	run_func = (currprefs.cpu_cycle_exact && currprefs.cpu_level == 0	? m68k_run_1_ce :
		    currprefs.cpu_compatible > 0 && currprefs.cpu_level == 0	? m68k_run_1 :
		    currprefs.cpu_level >= 2 && currprefs.cachesize		? m68k_run_2a :
		    currprefs.cpu_compatible					? m68k_run_2p :
										  m68k_run_2);
#endif
	run_func ();

	if (uae_state_change_pending ())
	    break;
    }
}


#if defined(DEBUGGER) || defined (ENFORCER)

static const char * const ccnames[] =
{ "T","F","HI","LS","CC","CS","NE","EQ",
  "VC","VS","PL","MI","GE","LT","GT","LE" };

/****i* januae-cpu/m68k_disasm_2 ******************************************************
*
*   NAME
*       m68k_disasm_2() -- Disassemble instructions with addressing details.
*
*   SYNOPSIS
*       static void m68k_disasm_2 (void *f, uaecptr addr, uaecptr *nextpc, int
*       cnt, uae_u32 *seaddr, uae_u32 *deaddr, int safemode);
*
*   FUNCTION
*       Decode and format successive instructions from an emulated address,
*       optionally reporting source and destination effective addresses and the
*       next PC.
*
*   INPUTS
*       f - Output stream or output handle selected by the caller.
*       addr - Emulated address, or the address to inspect.
*       nextpc - Optional output for the address following the last decoded
*                instruction.
*       cnt - Number of instructions to decode.
*       seaddr - Optional source operand address output.
*       deaddr - Optional destination operand address output.
*       safemode - Select safe operand inspection.
*
***************************************************************************
*
*/

static void m68k_disasm_2 (void *f, uaecptr addr, uaecptr *nextpc, int cnt, uae_u32 *seaddr, uae_u32 *deaddr, int safemode)
{
    uaecptr newpc = 0;
    m68kpc_offset = addr - m68k_getpc (&regs);

    while (cnt-- > 0) {
	char instrname[100], *ccpt;
	int i;
	uae_u32 opcode;
	struct mnemolookup *lookup;
	struct instr *dp;
	int oldpc;

	oldpc = m68kpc_offset;
	opcode = get_iword_1 (&regs, m68kpc_offset);
	if (cpufunctbl[opcode] == op_illg_1) {
	    opcode = 0x4AFC;
	}
	dp = table68k + opcode;
	for (lookup = lookuptab;lookup->mnemo != dp->mnemo; lookup++)
	    ;

	f_out (f, "%08x ", m68k_getpc (&regs) + m68kpc_offset);
	m68kpc_offset += 2;

	strcpy (instrname, lookup->name);
	ccpt = strstr (instrname, "cc");
	if (ccpt != 0) {
	    strncpy (ccpt, ccnames[dp->cc], 2);
	}
	switch (dp->size){
	 case sz_byte: strcat (instrname, ".B "); break;
	 case sz_word: strcat (instrname, ".W "); break;
	 case sz_long: strcat (instrname, ".L "); break;
	 default: strcat (instrname, "   "); break;
	}

	if (dp->suse) {
	    newpc = m68k_getpc (&regs) + m68kpc_offset;
	    newpc += ShowEA (0, opcode, dp->sreg, dp->smode, dp->size, instrname, seaddr, safemode);
	}
	if (dp->suse && dp->duse)
	    strcat (instrname, ",");
	if (dp->duse) {
	    newpc = m68k_getpc (&regs) + m68kpc_offset;
	    newpc += ShowEA (0, opcode, dp->dreg, dp->dmode, dp->size, instrname, deaddr, safemode);
	}

	for (i = 0; i < (m68kpc_offset - oldpc) / 2; i++) {
	    f_out (f, "%04x ", get_iword_1 (&regs, oldpc + i * 2));
	}
	while (i++ < 5)
	    f_out (f, "     ");
	f_out (f, instrname);

	if (ccpt != 0) {
	    if (deaddr)
		*deaddr = newpc;
	    if (cctrue (&regs.ccrflags, dp->cc))
		f_out (f, " == %08x (TRUE)", newpc);
	    else
		f_out (f, " == %08x (FALSE)", newpc);
	} else if ((opcode & 0xff00) == 0x6100) { /* BSR */
	    if (deaddr)
		*deaddr = newpc;
	    f_out (f, " == %08x", newpc);
	}
	f_out (f, "\n");
    }
    if (nextpc)
	*nextpc = m68k_getpc (&regs) + m68kpc_offset;
}

/****i* januae-cpu/m68k_disasm_ea *****************************************************
*
*   NAME
*       m68k_disasm_ea() -- Disassemble and report operand addresses.
*
*   SYNOPSIS
*       void m68k_disasm_ea (void *f, uaecptr addr, uaecptr *nextpc, int cnt,
*       uae_u32 *seaddr, uae_u32 *deaddr);
*
*   FUNCTION
*       Invoke the internal disassembler while allowing effective-address
*       results to be returned to the caller.
*
*   INPUTS
*       f - Output stream or output handle selected by the caller.
*       addr - Emulated address, or the address to inspect.
*       nextpc - Optional output for the address following the last decoded
*                instruction.
*       cnt - Number of instructions to decode.
*       seaddr - Optional source operand address output.
*       deaddr - Optional destination operand address output.
*
***************************************************************************
*
*/

void m68k_disasm_ea (void *f, uaecptr addr, uaecptr *nextpc, int cnt, uae_u32 *seaddr, uae_u32 *deaddr)
{
    m68k_disasm_2 (f, addr, nextpc, cnt, seaddr, deaddr, 1);
}
/****i* januae-cpu/m68k_disasm ********************************************************
*
*   NAME
*       m68k_disasm() -- Disassemble instructions to an output stream.
*
*   SYNOPSIS
*       void m68k_disasm (void *f, uaecptr addr, uaecptr *nextpc, int cnt);
*
*   FUNCTION
*       Invoke the internal disassembler without requesting operand addresses.
*
*   INPUTS
*       f - Output stream or output handle selected by the caller.
*       addr - Emulated address, or the address to inspect.
*       nextpc - Optional output for the address following the last decoded
*                instruction.
*       cnt - Number of instructions to decode.
*
***************************************************************************
*
*/

void m68k_disasm (void *f, uaecptr addr, uaecptr *nextpc, int cnt)
{
    m68k_disasm_2 (f, addr, nextpc, cnt, NULL, NULL, 0);
}

/*************************************************************
 Disasm the m68kcode at the given address into instrname
 and instrcode
*************************************************************/
/****i* januae-cpu/sm68k_disasm *******************************************************
*
*   NAME
*       sm68k_disasm() -- Format an instruction into caller buffers.
*
*   SYNOPSIS
*       void sm68k_disasm (char *instrname, char *instrcode, uaecptr addr,
*       uaecptr *nextpc);
*
*   FUNCTION
*       Decode the instruction at the specified address, placing mnemonic and
*       instruction words into the supplied output buffers and optionally
*       returning the next PC.
*
*   INPUTS
*       instrname - Caller-provided mnemonic output buffer.
*       instrcode - Caller-provided opcode text output buffer.
*       addr - Emulated address, or the address to inspect.
*       nextpc - Optional output for the address following the last decoded
*                instruction.
*
***************************************************************************
*
*/

void sm68k_disasm (char *instrname, char *instrcode, uaecptr addr, uaecptr *nextpc)
{
    char *ccpt;
    uae_u32 opcode;
    struct mnemolookup *lookup;
    struct instr *dp;
    int oldpc;

    uaecptr newpc = 0;

    m68kpc_offset = addr - m68k_getpc (&regs);

    oldpc = m68kpc_offset;
    opcode = get_iword_1 (&regs, m68kpc_offset);
    if (cpufunctbl[opcode] == op_illg_1) {
	opcode = 0x4AFC;
    }
    dp = table68k + opcode;
    for (lookup = lookuptab;lookup->mnemo != dp->mnemo; lookup++);

    m68kpc_offset += 2;

    strcpy (instrname, lookup->name);
    ccpt = strstr (instrname, "cc");
    if (ccpt != 0) {
	strncpy (ccpt, ccnames[dp->cc], 2);
    }
    switch (dp->size){
	case sz_byte: strcat (instrname, ".B "); break;
	case sz_word: strcat (instrname, ".W "); break;
	case sz_long: strcat (instrname, ".L "); break;
	default: strcat (instrname, "   "); break;
    }

    if (dp->suse) {
	newpc = m68k_getpc (&regs) + m68kpc_offset;
	newpc += ShowEA (0, opcode, dp->sreg, dp->smode, dp->size, instrname, NULL, 0);
    }
    if (dp->suse && dp->duse)
	strcat (instrname, ",");
    if (dp->duse) {
	newpc = m68k_getpc (&regs) + m68kpc_offset;
	newpc += ShowEA (0, opcode, dp->dreg, dp->dmode, dp->size, instrname, NULL, 0);
    }

    if (instrcode)
    {
	int i;
	for (i = 0; i < (m68kpc_offset - oldpc) / 2; i++)
	{
	    sprintf (instrcode, "%04x ", get_iword_1 (&regs, oldpc + i * 2));
	    instrcode += strlen(instrcode);
	}
    }

    if (nextpc)
	*nextpc = m68k_getpc (&regs) + m68kpc_offset;
}

/****i* januae-cpu/m68k_dumpstate-1 **************************************************
*
*   NAME
*       m68k_dumpstate() -- Write CPU register and instruction state.
*
*   SYNOPSIS
*       void m68k_dumpstate (void *f, uaecptr *nextpc);
*
*   FUNCTION
*       When debugger or Enforcer output is enabled, print CPU registers,
*       status, optional FPU and prefetch information and the next instruction;
*       the alternative build definition does nothing.
*
*   INPUTS
*       f - Output stream or output handle selected by the caller.
*       nextpc - Optional output for the address following the last decoded
*                instruction.
*
***************************************************************************
*
*/

void m68k_dumpstate (void *f, uaecptr *nextpc)
{
    int i;

#ifndef WIN32
    if (!f)
	f = stderr;
#endif

    for (i = 0; i < 8; i++){
	f_out (f, "D%d: %08x ", i, m68k_dreg (&regs, i));
	if ((i & 3) == 3) f_out (f, "\n");
    }
    for (i = 0; i < 8; i++){
	f_out (f, "A%d: %08x ", i, m68k_areg (&regs, i));
	if ((i & 3) == 3) f_out (f, "\n");
    }
    if (regs.s == 0) regs.usp = m68k_areg (&regs, 7);
    if (regs.s && regs.m) regs.msp = m68k_areg (&regs, 7);
    if (regs.s && regs.m == 0) regs.isp = m68k_areg (&regs, 7);
    f_out (f, "USP=%08x ISP=%08x MSP=%08x VBR=%08x\n",
	     regs.usp,regs.isp,regs.msp,regs.vbr);
    f_out (f, "T=%d%d S=%d M=%d X=%d N=%d Z=%d V=%d C=%d IMASK=%d\n",
	     regs.t1, regs.t0, regs.s, regs.m,
	     GET_XFLG(&regs.ccrflags), GET_NFLG(&regs.ccrflags),
	     GET_ZFLG(&regs.ccrflags), GET_VFLG(&regs.ccrflags),
	     GET_CFLG(&regs.ccrflags), regs.intmask);
#ifdef FPUEMU
    if (currprefs.cpu_level >= 2) {
	uae_u32 fpsr;
	for (i = 0; i < 8; i++){
	    f_out (f, "FP%d: %g ", i, regs.fp[i]);
	    if ((i & 3) == 3) f_out (f, "\n");
	}
	fpsr = fpp_get_fpsr (&regs);
	f_out (f, "N=%d Z=%d I=%d NAN=%d\n",
		(fpsr & 0x8000000) != 0,
		(fpsr & 0x4000000) != 0,
		(fpsr & 0x2000000) != 0,
		(fpsr & 0x1000000) != 0);
    }
#endif
    if (currprefs.cpu_compatible) {
	struct instr *dp;
	struct mnemolookup *lookup1, *lookup2;
	dp = table68k + regs.irc;
	for (lookup1 = lookuptab; lookup1->mnemo != dp->mnemo; lookup1++);
	dp = table68k + regs.ir;
	for (lookup2 = lookuptab; lookup2->mnemo != dp->mnemo; lookup2++);
	f_out (f, "prefetch %04x (%s) %04x (%s)\n", regs.irc, lookup1->name, regs.ir, lookup2->name);
    }

    m68k_disasm (f, m68k_getpc (&regs), nextpc, 1);
    if (nextpc)
	f_out (f, "next PC: %08x\n", *nextpc);
}

#else

/****i* januae-cpu/m68k_dumpstate-2 **************************************************
*
*   NAME
*       m68k_dumpstate() -- Write CPU register and instruction state.
*
*   SYNOPSIS
*       void m68k_dumpstate (void *f, uaecptr *nextpc);
*
*   FUNCTION
*       When debugger or Enforcer output is enabled, print CPU registers,
*       status, optional FPU and prefetch information and the next instruction;
*       the alternative build definition does nothing.
*
*   INPUTS
*       f - Output stream or output handle selected by the caller.
*       nextpc - Optional output for the address following the last decoded
*                instruction.
*
***************************************************************************
*
*/

void m68k_dumpstate (void *f, uaecptr *nextpc)
{
}

#endif


#ifdef SAVESTATE

/* CPU save/restore code */

#define CPUTYPE_EC 1
#define CPUMODE_HALT 1

/****i* januae-cpu/restore_cpu ********************************************************
*
*   NAME
*       restore_cpu() -- Restore CPU state from SaveState.
*
*   SYNOPSIS
*       const uae_u8 *restore_cpu (const uae_u8 *src);
*
*   FUNCTION
*       Read saved CPU model, flags, registers, control state and compatible
*       machine flags through the SaveState read helpers. Return the input
*       pointer used by the save framework.
*
*   INPUTS
*       src - Input operand or saved-state cursor.
*
***************************************************************************
*
*/

const uae_u8 *restore_cpu (const uae_u8 *src)
{
    unsigned int i, model, flags;
    uae_u32 l;

    model = restore_u32 ();
    switch (model) {
    case 68000:
	changed_prefs.cpu_level = 0;
	break;
    case 68010:
	changed_prefs.cpu_level = 1;
	break;
    case 68020:
	changed_prefs.cpu_level = 2;
	break;
    case 68040:
	changed_prefs.cpu_level = 4;
	break;
    case 68060:
	changed_prefs.cpu_level = 6;
	break;
    default:
	write_log ("Unknown cpu type %d\n", model);
	break;
    }

    flags = restore_u32 ();
    changed_prefs.address_space_24 = 0;
    if (flags & CPUTYPE_EC)
	changed_prefs.address_space_24 = 1;

    //kprintf("oli: address_space_24: %d\n",changed_prefs.address_space_24);
    if (model > 68000)
	changed_prefs.cpu_compatible = 0;
    currprefs.cpu_level = changed_prefs.cpu_level;
    currprefs.address_space_24 = changed_prefs.address_space_24;
    currprefs.cpu_compatible = changed_prefs.cpu_compatible;
    currprefs.cpu_cycle_exact = changed_prefs.cpu_cycle_exact;
    currprefs.blitter_cycle_exact = changed_prefs.blitter_cycle_exact;
    for (i = 0; i < 15; i++)
	regs.regs[i] = restore_u32 ();
    regs.pc = restore_u32 ();
    regs.irc = restore_u16 ();
    regs.ir = restore_u16 ();
    regs.usp = restore_u32 ();
    regs.isp = restore_u32 ();
    regs.sr = restore_u16 ();
    l = restore_u32 ();
    if (l & CPUMODE_HALT) {
	regs.stopped = 1;
	set_special (&regs, SPCFLAG_STOP);
    } else
	regs.stopped = 0;
    if (model >= 68010) {
	regs.dfc = restore_u32 ();
	regs.sfc = restore_u32 ();
	regs.vbr = restore_u32 ();
    }
    if (model >= 68020) {
	caar = restore_u32 ();
	cacr = restore_u32 ();
	regs.msp = restore_u32 ();
	/* A500 speed in 68020 mode isn't too logical.. */
	if (changed_prefs.m68k_speed == 0)
	    currprefs.m68k_speed = changed_prefs.m68k_speed = -1;
    }
    write_log ("CPU %d%s%03d, PC=%08x\n",
	       model / 1000, flags & 1 ? "EC" : "", model % 1000, regs.pc);

    return src;
}

/****i* januae-cpu/restore_cpu_finish *************************************************
*
*   NAME
*       restore_cpu_finish() -- Complete CPU state restoration.
*
*   SYNOPSIS
*       void restore_cpu_finish (void);
*
*   FUNCTION
*       Finalize restored processor state after the saved register data has been
*       read.
*
***************************************************************************
*
*/

void restore_cpu_finish (void)
{
    init_m68k ();
    m68k_setpc (&regs, regs.pc);
}

static const int cpumodel[] = { 68000, 68010, 68020, 68020, 68040, 68060 };

/****i* januae-cpu/save_cpu ***********************************************************
*
*   NAME
*       save_cpu() -- Serialize CPU state for SaveState.
*
*   SYNOPSIS
*       uae_u8 *save_cpu (uae_u32 *len, uae_u8 *dstptr);
*
*   FUNCTION
*       Write the configured CPU model and register/control state into an
*       existing or newly allocated SaveState buffer and provide its length when
*       requested.
*
*   INPUTS
*       len - Optional serialized-length output.
*       dstptr - Optional existing serialization buffer.
*
***************************************************************************
*
*/

uae_u8 *save_cpu (uae_u32 *len, uae_u8 *dstptr)
{
    uae_u8 *dstbak, *dst;
    unsigned int model, i;

    if (dstptr)
	dstbak = dst = dstptr;
    else
	dstbak = dst = malloc(4+4+15*4+4+4+4+4+2+4+4+4+4+4+4+4);
    model = cpumodel[currprefs.cpu_level];
    save_u32 (model);					/* MODEL */
    save_u32 (currprefs.address_space_24 ? 1 : 0);	/* FLAGS */
    for (i = 0; i < 15; i++) save_u32 (regs.regs[i]);	/* D0-D7 A0-A6 */
    save_u32 (m68k_getpc (&regs));			/* PC */
    save_u16 (regs.irc);				/* prefetch */
    save_u16 (regs.ir);					/* instruction prefetch */
    MakeSR (&regs);
    save_u32 (!regs.s ? regs.regs[15] : regs.usp);	/* USP */
    save_u32 (regs.s ? regs.regs[15] : regs.isp);	/* ISP */
    save_u16 (regs.sr);				/* SR/CCR */
    save_u32 (regs.stopped ? CPUMODE_HALT : 0);	/* flags */
    if (model >= 68010) {
	save_u32 (regs.dfc);				/* DFC */
	save_u32 (regs.sfc);				/* SFC */
	save_u32 (regs.vbr);				/* VBR */
    }
    if (model >= 68020) {
	save_u32 (caar);				/* CAAR */
	save_u32 (cacr);				/* CACR */
	save_u32 (regs.msp);				/* MSP */
    }
    *len = dst - dstbak;
    return dstbak;
}

#endif /* SAVESTATE */

/****i* januae-cpu/exception3f ********************************************************
*
*   NAME
*       exception3f() -- Record an address-error exception.
*
*   SYNOPSIS
*       static void exception3f (uae_u32 opcode, uaecptr addr, uaecptr fault,
*       int writeaccess, int instructionaccess);
*
*   FUNCTION
*       Save the faulting opcode, instruction PC, fault address and read/write
*       or instruction-access attributes before entering the address-error
*       vector.
*
*   INPUTS
*       opcode - Encoded 68k instruction.
*       addr - Emulated address, or the address to inspect.
*       fault - Faulting address.
*       writeaccess - Nonzero for a write access.
*       instructionaccess - Nonzero for an instruction fetch.
*
***************************************************************************
*
*/

static void exception3f (uae_u32 opcode, uaecptr addr, uaecptr fault, int writeaccess, int instructionaccess)
{
    last_addr_for_exception_3 = addr;
    last_fault_for_exception_3 = fault;
    last_op_for_exception_3 = opcode;
    last_writeaccess_for_exception_3 = writeaccess;
    last_instructionaccess_for_exception_3 = instructionaccess;
    uae_Exception (3, &regs, fault);
}

/****i* januae-cpu/exception3 *********************************************************
*
*   NAME
*       exception3() -- Signal a data address error.
*
*   SYNOPSIS
*       void exception3 (uae_u32 opcode, uaecptr addr, uaecptr fault);
*
*   FUNCTION
*       Forward the faulting opcode and address information as a data-access
*       address error.
*
*   INPUTS
*       opcode - Encoded 68k instruction.
*       addr - Emulated address, or the address to inspect.
*       fault - Faulting address.
*
***************************************************************************
*
*/

void exception3 (uae_u32 opcode, uaecptr addr, uaecptr fault)
{
    exception3f (opcode, addr, fault, 0, 0);
}

/****i* januae-cpu/exception3i ********************************************************
*
*   NAME
*       exception3i() -- Signal an instruction address error.
*
*   SYNOPSIS
*       void exception3i (uae_u32 opcode, uaecptr addr, uaecptr fault);
*
*   FUNCTION
*       Forward the faulting opcode and address information as an
*       instruction-fetch address error.
*
*   INPUTS
*       opcode - Encoded 68k instruction.
*       addr - Emulated address, or the address to inspect.
*       fault - Faulting address.
*
***************************************************************************
*
*/

void exception3i (uae_u32 opcode, uaecptr addr, uaecptr fault)
{
    exception3f (opcode, addr, fault, 0, 1);
}

/****i* januae-cpu/exception2 *********************************************************
*
*   NAME
*       exception2() -- Defer a bus-error exception.
*
*   SYNOPSIS
*       void exception2 (uaecptr addr, uaecptr fault);
*
*   FUNCTION
*       Record the pending bus-error state, force the execution loop to stop and
*       redirect the PC to the reboot ROM address; also terminate JIT
*       compilation when enabled.
*
*   INPUTS
*       addr - Emulated address, or the address to inspect.
*       fault - Faulting address.
*
***************************************************************************
*
*/

void exception2 (uaecptr addr, uaecptr fault)
{
    write_log ("delayed exception2!\n");
    regs.panic_pc = m68k_getpc (&regs);
    regs.panic_addr = addr;
    regs.panic = 2;
    set_special (&regs, SPCFLAG_BRK);
    m68k_setpc (&regs, 0xf80000);
#ifdef JIT
    set_special (&regs, SPCFLAG_END_COMPILE);
#endif
    fill_prefetch_slow (&regs);
}

/****i* januae-cpu/cpureset ***********************************************************
*
*   NAME
*       cpureset() -- Reset custom-chip state for RESET opcode.
*
*   SYNOPSIS
*       void cpureset (void);
*
*   FUNCTION
*       Invoke customreset for the guest RESET instruction. The historical
*       special-case RESET/JMP implementation is disabled.
*
***************************************************************************
*
*/

void cpureset (void)
{
    customreset ();
#if 0
    uae_u16 ins;
    if (currprefs.cpu_level == 0 && (currprefs.cpu_compatible || currprefs.cpu_cycle_exact)) {
	customreset ();
	return;
    }
    ins = get_word (m68k_getpc (&regs) + 2);
    if ((ins & ~7) == 0x4ed0) {
	int reg = ins & 7;
	uae_u32 addr = m68k_areg (regs, reg);
	write_log ("reset/jmp (ax) combination emulated\n");
	customreset ();
	if (addr < 0x80000)
	    addr += 0xf80000;
	m68k_setpc (&regs, addr);
    }
#endif
}


/*

 The routines below take dividend and divisor as parameters.
 They return 0 if division by zero, or exact number of cycles otherwise.

 The number of cycles returned assumes a register operand.
 Effective address time must be added if memory operand.

 For 68000 only (not 68010, 68012, 68020, etc).
 Probably valid for 68008 after adding the extra prefetch cycle.


 Best and worst cases are for register operand:
 (Note the difference with the documented range.)


 DIVU:

 Overflow (always): 10 cycles.
 Worst case: 136 cycles.
 Best case: 76 cycles.


 DIVS:

 Absolute overflow: 16-18 cycles.
 Signed overflow is not detected prematurely.

 Worst case: 156 cycles.
 Best case without signed overflow: 122 cycles.
 Best case with signed overflow: 120 cycles


 */


//
// DIVU
// Unsigned division
//

/****i* januae-cpu/getDivu68kCycles_2 *************************************************
*
*   NAME
*       getDivu68kCycles_2() -- Count unsigned 68000 division cycles.
*
*   SYNOPSIS
*       STATIC_INLINE int getDivu68kCycles_2(uae_u32 dividend, uae_u16 divisor);
*
*   FUNCTION
*       Calculate the instruction cycles for the specified unsigned
*       register-operand division using the implementation's bit-step algorithm.
*
*   INPUTS
*       dividend - Value to divide.
*       divisor - Value used as the divisor.
*
***************************************************************************
*
*/

STATIC_INLINE int getDivu68kCycles_2(uae_u32 dividend, uae_u16 divisor)
{
	int mcycles;
	uae_u32 hdivisor;
	int i;

	if(divisor == 0)
		return 0;

	// Overflow
	if((dividend >> 16) >= divisor)
		return (mcycles = 5) * 2;

	mcycles = 38;
	hdivisor = divisor << 16;

	for( i = 0; i < 15; i++)
	{
		uae_u32 temp;
		temp = dividend;

		dividend <<= 1;

		// If carry from shift
		if((uae_s32)temp < 0)
		{
			dividend -= hdivisor;
		}
		else
		{
			mcycles += 2;
			if(dividend >= hdivisor)
			{
				dividend -= hdivisor;
				mcycles--;
			}
		}
	}
	return mcycles * 2;
}
/****i* januae-cpu/getDivu68kCycles ***************************************************
*
*   NAME
*       getDivu68kCycles() -- Return unsigned division cycle count.
*
*   SYNOPSIS
*       int getDivu68kCycles(uae_u32 dividend, uae_u16 divisor);
*
*   FUNCTION
*       Return the unsigned division helper result minus four cycles. A zero
*       divisor produces minus four because the helper returns zero.
*
*   INPUTS
*       dividend - Value to divide.
*       divisor - Value used as the divisor.
*
***************************************************************************
*
*/

int getDivu68kCycles(uae_u32 dividend, uae_u16 divisor)
{
    int v = getDivu68kCycles_2(dividend, divisor) - 4;
//    write_log ("U%d ", v);
    return v;
}

//
// DIVS
// Signed division
//

/****i* januae-cpu/getDivs68kCycles_2 *************************************************
*
*   NAME
*       getDivs68kCycles_2() -- Count signed 68000 division cycles.
*
*   SYNOPSIS
*       STATIC_INLINE int getDivs68kCycles_2(uae_s32 dividend, uae_s16 divisor);
*
*   FUNCTION
*       Calculate signed division timing including sign and overflow-dependent
*       work for the specified register operands.
*
*   INPUTS
*       dividend - Value to divide.
*       divisor - Value used as the divisor.
*
***************************************************************************
*
*/

STATIC_INLINE int getDivs68kCycles_2(uae_s32 dividend, uae_s16 divisor)
{
	int mcycles;
	uae_u32 aquot;
	int i;

	if(divisor == 0)
		return 0;

	mcycles = 6;

	if( dividend < 0)
		mcycles++;

	// Check for absolute overflow
	if(((uae_u32)abs(dividend) >> 16) >= (uae_u16)abs(divisor))
	{
		return (mcycles + 2) * 2;
	}

	// Absolute quotient
	aquot = (uae_u32) abs(dividend) / (uae_u16)abs(divisor);

	mcycles += 55;

	if(divisor >= 0)
	{
		if(dividend >= 0)
			mcycles--;
		else
			mcycles++;
	}

	// Count 15 msbits in absolute of quotient

	for( i = 0; i < 15; i++)
	{
		if((uae_s16)aquot >= 0)
			mcycles++;
		aquot <<= 1;
	}

	return mcycles * 2;
}
/****i* januae-cpu/getDivs68kCycles ***************************************************
*
*   NAME
*       getDivs68kCycles() -- Return signed division cycle count.
*
*   SYNOPSIS
*       int getDivs68kCycles(uae_s32 dividend, uae_s16 divisor);
*
*   FUNCTION
*       Return the signed division helper result minus four cycles. A zero
*       divisor produces minus four because the helper returns zero.
*
*   INPUTS
*       dividend - Value to divide.
*       divisor - Value used as the divisor.
*
***************************************************************************
*
*/

int getDivs68kCycles(uae_s32 dividend, uae_s16 divisor)
{
    int v = getDivs68kCycles_2(dividend, divisor) - 4;
//    write_log ("S%d ", v);
    return v;
}
