/*
 * UAE - The Un*x Amiga Emulator
 *
 * Read 68000 CPU specs from file "table68k"
 *
 * Copyright 1995,1996 Bernd Schmidt
 */

#include "sysconfig.h"
#include <stdlib.h>
#include "uae_string.h"
#include "uae_types.h"
#include "uae_malloc.h"
#include "writelog.h"
#include <ctype.h>

#include "readcpu.h"

/*
 * You can specify numbers from 0 to 5 here. It is possible that higher
 * numbers will make the CPU emulation slightly faster, but if the setting
 * is too high, you will run out of memory while compiling.
 * Best to leave this as it is.
 */
#ifndef CPU_EMU_SIZE
# define CPU_EMU_SIZE 0
#endif

int nr_cpuop_funcs;

struct mnemolookup lookuptab[] = {
    { i_ILLG, "ILLEGAL" },
    { i_OR, "OR" },
    { i_CHK, "CHK" },
    { i_CHK2, "CHK2" },
    { i_AND, "AND" },
    { i_EOR, "EOR" },
    { i_ORSR, "ORSR" },
    { i_ANDSR, "ANDSR" },
    { i_EORSR, "EORSR" },
    { i_SUB, "SUB" },
    { i_SUBA, "SUBA" },
    { i_SUBX, "SUBX" },
    { i_SBCD, "SBCD" },
    { i_ADD, "ADD" },
    { i_ADDA, "ADDA" },
    { i_ADDX, "ADDX" },
    { i_ABCD, "ABCD" },
    { i_NEG, "NEG" },
    { i_NEGX, "NEGX" },
    { i_NBCD, "NBCD" },
    { i_CLR, "CLR" },
    { i_NOT, "NOT" },
    { i_TST, "TST" },
    { i_BTST, "BTST" },
    { i_BCHG, "BCHG" },
    { i_BCLR, "BCLR" },
    { i_BSET, "BSET" },
    { i_CMP, "CMP" },
    { i_CMPM, "CMPM" },
    { i_CMPA, "CMPA" },
    { i_MVPRM, "MVPRM" },
    { i_MVPMR, "MVPMR" },
    { i_MOVE, "MOVE" },
    { i_MOVEA, "MOVEA" },
    { i_MVSR2, "MVSR2" },
    { i_MV2SR, "MV2SR" },
    { i_SWAP, "SWAP" },
    { i_EXG, "EXG" },
    { i_EXT, "EXT" },
    { i_MVMEL, "MVMEL" },
    { i_MVMLE, "MVMLE" },
    { i_TRAP, "TRAP" },
    { i_MVR2USP, "MVR2USP" },
    { i_MVUSP2R, "MVUSP2R" },
    { i_NOP, "NOP" },
    { i_RESET, "RESET" },
    { i_RTE, "RTE" },
    { i_RTD, "RTD" },
    { i_LINK, "LINK" },
    { i_UNLK, "UNLK" },
    { i_RTS, "RTS" },
    { i_STOP, "STOP" },
    { i_TRAPV, "TRAPV" },
    { i_RTR, "RTR" },
    { i_JSR, "JSR" },
    { i_JMP, "JMP" },
    { i_BSR, "BSR" },
    { i_Bcc, "Bcc" },
    { i_LEA, "LEA" },
    { i_PEA, "PEA" },
    { i_DBcc, "DBcc" },
    { i_Scc, "Scc" },
    { i_DIVU, "DIVU" },
    { i_DIVS, "DIVS" },
    { i_MULU, "MULU" },
    { i_MULS, "MULS" },
    { i_ASR, "ASR" },
    { i_ASL, "ASL" },
    { i_LSR, "LSR" },
    { i_LSL, "LSL" },
    { i_ROL, "ROL" },
    { i_ROR, "ROR" },
    { i_ROXL, "ROXL" },
    { i_ROXR, "ROXR" },
    { i_ASRW, "ASRW" },
    { i_ASLW, "ASLW" },
    { i_LSRW, "LSRW" },
    { i_LSLW, "LSLW" },
    { i_ROLW, "ROLW" },
    { i_RORW, "RORW" },
    { i_ROXLW, "ROXLW" },
    { i_ROXRW, "ROXRW" },

    { i_MOVE2C, "MOVE2C" },
    { i_MOVEC2, "MOVEC2" },
    { i_CAS, "CAS" },
    { i_CAS2, "CAS2" },
    { i_MULL, "MULL" },
    { i_DIVL, "DIVL" },
    { i_BFTST, "BFTST" },
    { i_BFEXTU, "BFEXTU" },
    { i_BFCHG, "BFCHG" },
    { i_BFEXTS, "BFEXTS" },
    { i_BFCLR, "BFCLR" },
    { i_BFFFO, "BFFFO" },
    { i_BFSET, "BFSET" },
    { i_BFINS, "BFINS" },
    { i_PACK, "PACK" },
    { i_UNPK, "UNPK" },
    { i_TAS, "TAS" },
    { i_BKPT, "BKPT" },
    { i_CALLM, "CALLM" },
    { i_RTM, "RTM" },
    { i_TRAPcc, "TRAPcc" },
    { i_MOVES, "MOVES" },
    { i_FPP, "FPP" },
    { i_FDBcc, "FDBcc" },
    { i_FScc, "FScc" },
    { i_FTRAPcc, "FTRAPcc" },
    { i_FBcc, "FBcc" },
    { i_FBcc, "FBcc" },
    { i_FSAVE, "FSAVE" },
    { i_FRESTORE, "FRESTORE" },

    { i_CINVL, "CINVL" },
    { i_CINVP, "CINVP" },
    { i_CINVA, "CINVA" },
    { i_CPUSHL, "CPUSHL" },
    { i_CPUSHP, "CPUSHP" },
    { i_CPUSHA, "CPUSHA" },
    { i_MOVE16, "MOVE16" },

    { i_MMUOP, "MMUOP" },
    { i_ILLG, "" },
};

struct instr *table68k;

/****i* januae-readcpu/specialcase ***************************************
*
*   NAME
*       specialcase() -- Apply model-specific instruction-level exceptions.
*
*   SYNOPSIS
*       static int specialcase (uae_u16 opcode, int cpu_lev);
*
*   FUNCTION
*       Raise the minimum CPU level to 68020 for the TST and CMPI
*       addressing-mode combinations recognized here. Other opcodes retain
*       their supplied level.
*
*   INPUTS
*       opcode - 16-bit instruction word.
*       cpu_lev - Minimum CPU level from the instruction definition.
*
*   RESULT
*       The supplied level or 2 for a recognized 68020-only case.
*
*   NOTES
*       A supplied level of 2 or greater is returned without decoding.
*
****************************************************************************
*
*/

static int specialcase (uae_u16 opcode, int cpu_lev)
{
    int mode = (opcode >> 3) & 7;
    int reg = opcode & 7;

    if (cpu_lev >= 2)
	return cpu_lev;
    /* TST.W A0, TST.L A0, TST.x (d16,PC) and TST.x (d8,PC,Xn) are 68020+ only */
    if ((opcode & 0xff00) == 0x4a00) {
	if (mode == 7 && (reg == 4 || reg == 2 || reg == 3))
	    return 2;
	if (mode == 1) /* Ax */
	    return 2;
    }
    /* CMPI.W #x,(d16,PC) and CMPI.W #x,(d8,PC,Xn) are 68020+ only */
    if ((opcode & 0xff00) == 0x0c00) {
	if (mode == 7 && (reg == 2 || reg == 3))
	    return 2;
    }
    return cpu_lev;
}

/****i* januae-readcpu/mode_from_str *************************************
*
*   NAME
*       mode_from_str() -- Decode an instruction addressing-mode name.
*
*   SYNOPSIS
*       static amodes mode_from_str (const char *str);
*
*   FUNCTION
*       Map a four-character addressing-mode prefix to its amodes value.
*
*   INPUTS
*       str - String beginning with a supported mode identifier.
*
*   RESULT
*       The corresponding addressing-mode constant.
*
*   NOTES
*       Only the first four characters are compared. An unrecognized
*       identifier calls abort().
*
****************************************************************************
*
*/

static amodes mode_from_str (const char *str)
{
    if (strncmp (str, "Dreg", 4) == 0) return Dreg;
    if (strncmp (str, "Areg", 4) == 0) return Areg;
    if (strncmp (str, "Aind", 4) == 0) return Aind;
    if (strncmp (str, "Apdi", 4) == 0) return Apdi;
    if (strncmp (str, "Aipi", 4) == 0) return Aipi;
    if (strncmp (str, "Ad16", 4) == 0) return Ad16;
    if (strncmp (str, "Ad8r", 4) == 0) return Ad8r;
    if (strncmp (str, "absw", 4) == 0) return absw;
    if (strncmp (str, "absl", 4) == 0) return absl;
    if (strncmp (str, "PC16", 4) == 0) return PC16;
    if (strncmp (str, "PC8r", 4) == 0) return PC8r;
    if (strncmp (str, "Immd", 4) == 0) return imm;
    abort ();
    return 0;
}

/****i* januae-readcpu/mode_from_mr **************************************
*
*   NAME
*       mode_from_mr() -- Decode the mode/register fields of an effective address.
*
*   SYNOPSIS
*       STATIC_INLINE amodes mode_from_mr (int mode, int reg);
*
*   FUNCTION
*       Translate the encoded 68000 addressing-mode field and, for mode 7,
*       its register subfield into an amodes value.
*
*   INPUTS
*       mode - Effective-address mode field.
*       reg - Register subfield, used when mode is 7.
*
*   RESULT
*       The decoded addressing mode; am_illg for mode 7 with a register
*       value of 5, 6, or 7.
*
*   NOTES
*       Unsupported mode values call abort().
*
****************************************************************************
*
*/

STATIC_INLINE amodes mode_from_mr (int mode, int reg)
{
    switch (mode) {
     case 0: return Dreg;
     case 1: return Areg;
     case 2: return Aind;
     case 3: return Aipi;
     case 4: return Apdi;
     case 5: return Ad16;
     case 6: return Ad8r;
     case 7:
	switch (reg) {
	 case 0: return absw;
	 case 1: return absl;
	 case 2: return PC16;
	 case 3: return PC8r;
	 case 4: return imm;
	 case 5:
	 case 6:
	 case 7: return am_illg;
	}
    }
    abort ();
    return 0;
}

/****i* januae-readcpu/build_insn ****************************************
*
*   NAME
*       build_insn() -- Expand one instruction definition into opcode descriptors.
*
*   SYNOPSIS
*       static void build_insn (int insn);
*
*   FUNCTION
*       Enumerate the variable-bit combinations in defs68k[insn], decode
*       mnemonic, size, operands, CPU level and flag usage, and populate
*       matching entries in the global table68k array.
*
*   INPUTS
*       insn - Index into defs68k, in the range 0 through n_defs68k - 1.
*
*   RESULT
*       None. Matching entries in table68k are updated.
*
*   NOTES
*       Invalid operand syntax and unsupported mode strings may call
*       abort(). Duplicate opcode matches are reported with write_log().
*       Nonmatching variants are skipped.
*
****************************************************************************
*
*/

static void build_insn (int insn)
{
    int find = -1;
    int variants;
    int isjmp = 0;
    struct instr_def id;
    const char *opcstr;
    int i;

    int flaglive = 0, flagdead = 0;

    id = defs68k[insn];

    /* Note: We treat anything with unknown flags as a jump. That
       is overkill, but "the programmer" was lazy quite often, and
       *this* programmer can't be bothered to work out what can and
       can't trap. Usually, this will be overwritten with the gencomp
       based information, anyway. */

    for (i = 0; i < 5; i++) {
	switch (id.flaginfo[i].flagset){
	case fa_unset: break;
	case fa_isjmp: isjmp = 1; break;
	case fa_isbranch: isjmp = 1; break;
	case fa_zero: flagdead |= 1 << i; break;
	case fa_one: flagdead |= 1 << i; break;
	case fa_dontcare: flagdead |= 1 << i; break;
	case fa_unknown: isjmp = 1; flagdead = -1; goto out1;
	case fa_set: flagdead |= 1 << i; break;
	}
    }

  out1:
    for (i = 0; i < 5; i++) {
	switch (id.flaginfo[i].flaguse) {
	case fu_unused: break;
	case fu_isjmp: isjmp = 1; flaglive |= 1 << i; break;
	case fu_maybecc: isjmp = 1; flaglive |= 1 << i; break;
	case fu_unknown: isjmp = 1; flaglive |= 1 << i; break;
	case fu_used: flaglive |= 1 << i; break;
	}
    }

    opcstr = id.opcstr;
    for (variants = 0; variants < (1 << id.n_variable); variants++) {
	int bitcnt[lastbit];
	int bitval[lastbit];
	int bitpos[lastbit];
	int i;
	uae_u16 opc = id.bits;
	uae_u16 msk, vmsk;
	int pos = 0;
	int mnp = 0;
	int bitno = 0;
	char mnemonic[10];

	wordsizes sz = sz_long;
	int srcgather = 0, dstgather = 0;
	int usesrc = 0, usedst = 0;
	int srctype = 0;
	int srcpos = -1, dstpos = -1;

	amodes srcmode = am_unknown, destmode = am_unknown;
	int srcreg = -1, destreg = -1;

	for (i = 0; i < lastbit; i++)
	    bitcnt[i] = bitval[i] = 0;

	vmsk = 1 << id.n_variable;

	for (i = 0, msk = 0x8000; i < 16; i++, msk >>= 1) {
	    if (!(msk & id.mask)) {
		int currbit = id.bitpos[bitno++];
		int bit_set;
		vmsk >>= 1;
		bit_set = variants & vmsk ? 1 : 0;
		if (bit_set)
		    opc |= msk;
		bitpos[currbit] = 15 - i;
		bitcnt[currbit]++;
		bitval[currbit] <<= 1;
		bitval[currbit] |= bit_set;
	    }
	}

	if (bitval[bitj] == 0) bitval[bitj] = 8;
	/* first check whether this one does not match after all */
	if (bitval[bitz] == 3 || bitval[bitC] == 1)
	    continue;
	if (bitcnt[bitI] && (bitval[bitI] == 0x00 || bitval[bitI] == 0xff))
	    continue;

	/* bitI and bitC get copied to biti and bitc */
	if (bitcnt[bitI]) {
	    bitval[biti] = bitval[bitI]; bitpos[biti] = bitpos[bitI];
	}
	if (bitcnt[bitC])
	    bitval[bitc] = bitval[bitC];

	pos = 0;
	while (opcstr[pos] && !isspace(opcstr[pos])) {
	    if (opcstr[pos] == '.') {
		pos++;
		switch (opcstr[pos]) {

		case 'B': sz = sz_byte; break;
		case 'W': sz = sz_word; break;
		case 'L': sz = sz_long; break;
		case 'z':
		    switch (bitval[bitz]) {
		    case 0: sz = sz_byte; break;
		    case 1: sz = sz_word; break;
		    case 2: sz = sz_long; break;
		    default: abort();
		    }
		    break;
		default: abort();
		}
	    } else {
		mnemonic[mnp] = opcstr[pos];
		if (mnemonic[mnp] == 'f') {
		    find = -1;
		    switch (bitval[bitf]) {
		    case 0: mnemonic[mnp] = 'R'; break;
		    case 1: mnemonic[mnp] = 'L'; break;
		    default: abort();
		    }
		}
		mnp++;
	    }
	    pos++;
	}
	mnemonic[mnp] = 0;

	/* now, we have read the mnemonic and the size */
	while (opcstr[pos] && isspace(opcstr[pos]))
	    pos++;

	/* A goto a day keeps the D******a away. */
	if (opcstr[pos] == 0)
	    goto endofline;

	/* parse the source address */
	usesrc = 1;
	switch (opcstr[pos++]) {
	case 'D':
	    srcmode = Dreg;
	    switch (opcstr[pos++]) {
	    case 'r': srcreg = bitval[bitr]; srcgather = 1; srcpos = bitpos[bitr]; break;
	    case 'R': srcreg = bitval[bitR]; srcgather = 1; srcpos = bitpos[bitR]; break;
	    default: abort();
	    }
	    break;
	case 'A':
	    srcmode = Areg;
	    switch (opcstr[pos++]) {
	    case 'r': srcreg = bitval[bitr]; srcgather = 1; srcpos = bitpos[bitr]; break;
	    case 'R': srcreg = bitval[bitR]; srcgather = 1; srcpos = bitpos[bitR]; break;
	    default: abort();
	    }
	    switch (opcstr[pos]) {
	    case 'p': srcmode = Apdi; pos++; break;
	    case 'P': srcmode = Aipi; pos++; break;
	    }
	    break;
	case 'L':
	    srcmode = absl;
	    break;
	case '#':
	    switch (opcstr[pos++]) {
	    case 'z': srcmode = imm; break;
	    case '0': srcmode = imm0; break;
	    case '1': srcmode = imm1; break;
	    case '2': srcmode = imm2; break;
	    case 'i': srcmode = immi; srcreg = (uae_s32)(uae_s8)bitval[biti];
		if (CPU_EMU_SIZE < 4) {
		    /* Used for branch instructions */
		    srctype = 1;
		    srcgather = 1;
		    srcpos = bitpos[biti];
		}
		break;
	    case 'j': srcmode = immi; srcreg = bitval[bitj];
		if (CPU_EMU_SIZE < 3) {
		    /* 1..8 for ADDQ/SUBQ and rotshi insns */
		    srcgather = 1;
		    srctype = 3;
		    srcpos = bitpos[bitj];
		}
		break;
	    case 'J': srcmode = immi; srcreg = bitval[bitJ];
		if (CPU_EMU_SIZE < 5) {
		    /* 0..15 */
		    srcgather = 1;
		    srctype = 2;
		    srcpos = bitpos[bitJ];
		}
		break;
	    case 'k': srcmode = immi; srcreg = bitval[bitk];
		if (CPU_EMU_SIZE < 3) {
		    srcgather = 1;
		    srctype = 4;
		    srcpos = bitpos[bitk];
		}
		break;
	    case 'K': srcmode = immi; srcreg = bitval[bitK];
		if (CPU_EMU_SIZE < 5) {
		    /* 0..15 */
		    srcgather = 1;
		    srctype = 5;
		    srcpos = bitpos[bitK];
		}
		break;
	    case 'p': srcmode = immi; srcreg = bitval[bitK];
		if (CPU_EMU_SIZE < 5) {
		    /* 0..3 */
		    srcgather = 1;
		    srctype = 7;
		    srcpos = bitpos[bitp];
		}
		break;
	    default: abort();
	    }
	    break;
	case 'd':
	    srcreg = bitval[bitD];
	    srcmode = mode_from_mr(bitval[bitd],bitval[bitD]);
	    if (srcmode == am_illg)
		continue;
	    if (CPU_EMU_SIZE < 2 &&
		(srcmode == Areg || srcmode == Dreg || srcmode == Aind
		 || srcmode == Ad16 || srcmode == Ad8r || srcmode == Aipi
		 || srcmode == Apdi))
		{
		    srcgather = 1; srcpos = bitpos[bitD];
		}
	    if (opcstr[pos] == '[') {
		pos++;
		if (opcstr[pos] == '!') {
		    /* exclusion */
		    do {
			pos++;
			if (mode_from_str(opcstr+pos) == srcmode)
			    goto nomatch;
			pos += 4;
		    } while (opcstr[pos] == ',');
		    pos++;
		} else {
		    if (opcstr[pos+4] == '-') {
			/* replacement */
			if (mode_from_str(opcstr+pos) == srcmode)
			    srcmode = mode_from_str(opcstr+pos+5);
			else
			    goto nomatch;
			pos += 10;
		    } else {
			/* normal */
			while(mode_from_str(opcstr+pos) != srcmode) {
			    pos += 4;
			    if (opcstr[pos] == ']')
				goto nomatch;
			    pos++;
			}
			while(opcstr[pos] != ']') pos++;
			pos++;
			break;
		    }
		}
	    }
	    /* Some addressing modes are invalid as destination */
	    if (srcmode == imm || srcmode == PC16 || srcmode == PC8r)
		goto nomatch;
	    break;
	case 's':
	    srcreg = bitval[bitS];
	    srcmode = mode_from_mr(bitval[bits],bitval[bitS]);

	    if (srcmode == am_illg)
		continue;
	    if (CPU_EMU_SIZE < 2 &&
		(srcmode == Areg || srcmode == Dreg || srcmode == Aind
		 || srcmode == Ad16 || srcmode == Ad8r || srcmode == Aipi
		 || srcmode == Apdi))
		{
		    srcgather = 1; srcpos = bitpos[bitS];
		}
	    if (opcstr[pos] == '[') {
		pos++;
		if (opcstr[pos] == '!') {
		    /* exclusion */
		    do {
			pos++;
			if (mode_from_str(opcstr+pos) == srcmode)
			    goto nomatch;
			pos += 4;
		    } while (opcstr[pos] == ',');
		    pos++;
		} else {
		    if (opcstr[pos+4] == '-') {
			/* replacement */
			if (mode_from_str(opcstr+pos) == srcmode)
			    srcmode = mode_from_str(opcstr+pos+5);
			else
			    goto nomatch;
			pos += 10;
		    } else {
			/* normal */
			while(mode_from_str(opcstr+pos) != srcmode) {
			    pos += 4;
			    if (opcstr[pos] == ']')
				goto nomatch;
			    pos++;
			}
			while(opcstr[pos] != ']') pos++;
			pos++;
		    }
		}
	    }
	    break;
	default: abort();
	}
	/* safety check - might have changed */
	if (srcmode != Areg && srcmode != Dreg && srcmode != Aind
	    && srcmode != Ad16 && srcmode != Ad8r && srcmode != Aipi
	    && srcmode != Apdi && srcmode != immi)
	    {
		srcgather = 0;
	    }
	if (srcmode == Areg && sz == sz_byte)
	    goto nomatch;

	if (opcstr[pos] != ',')
	    goto endofline;
	pos++;

	/* parse the destination address */
	usedst = 1;
	switch (opcstr[pos++]) {
	case 'D':
	    destmode = Dreg;
	    switch (opcstr[pos++]) {
	    case 'r': destreg = bitval[bitr]; dstgather = 1; dstpos = bitpos[bitr]; break;
	    case 'R': destreg = bitval[bitR]; dstgather = 1; dstpos = bitpos[bitR]; break;
	    default: abort();
	    }
            if (dstpos < 0 || dstpos >= 32)
		abort ();
	    break;
	case 'A':
	    destmode = Areg;
	    switch (opcstr[pos++]) {
	    case 'r': destreg = bitval[bitr]; dstgather = 1; dstpos = bitpos[bitr]; break;
	    case 'R': destreg = bitval[bitR]; dstgather = 1; dstpos = bitpos[bitR]; break;
	    case 'x': destreg = 0; dstgather = 0; dstpos = 0; break;
	    default: abort();
	    }
            if (dstpos < 0 || dstpos >= 32)
		abort ();
	    switch (opcstr[pos]) {
	    case 'p': destmode = Apdi; pos++; break;
	    case 'P': destmode = Aipi; pos++; break;
	    }
	    break;
	case 'L':
	    destmode = absl;
	    break;
	case '#':
	    switch (opcstr[pos++]) {
	    case 'z': destmode = imm; break;
	    case '0': destmode = imm0; break;
	    case '1': destmode = imm1; break;
	    case '2': destmode = imm2; break;
	    case 'i': destmode = immi; destreg = (uae_s32)(uae_s8)bitval[biti]; break;
	    case 'j': destmode = immi; destreg = bitval[bitj]; break;
	    case 'J': destmode = immi; destreg = bitval[bitJ]; break;
	    case 'k': destmode = immi; destreg = bitval[bitk]; break;
	    case 'K': destmode = immi; destreg = bitval[bitK]; break;
	    default: abort();
	    }
	    break;
	case 'd':
	    destreg = bitval[bitD];
	    destmode = mode_from_mr(bitval[bitd],bitval[bitD]);
	    if (destmode == am_illg)
		continue;
	    if (CPU_EMU_SIZE < 1 &&
		(destmode == Areg || destmode == Dreg || destmode == Aind
		 || destmode == Ad16 || destmode == Ad8r || destmode == Aipi
		 || destmode == Apdi))
		{
		    dstgather = 1; dstpos = bitpos[bitD];
		}

	    if (opcstr[pos] == '[') {
		pos++;
		if (opcstr[pos] == '!') {
		    /* exclusion */
		    do {
			pos++;
			if (mode_from_str(opcstr+pos) == destmode)
			    goto nomatch;
			pos += 4;
		    } while (opcstr[pos] == ',');
		    pos++;
		} else {
		    if (opcstr[pos+4] == '-') {
			/* replacement */
			if (mode_from_str(opcstr+pos) == destmode)
			    destmode = mode_from_str(opcstr+pos+5);
			else
			    goto nomatch;
			pos += 10;
		    } else {
			/* normal */
			while(mode_from_str(opcstr+pos) != destmode) {
			    pos += 4;
			    if (opcstr[pos] == ']')
				goto nomatch;
			    pos++;
			}
			while(opcstr[pos] != ']') pos++;
			pos++;
			break;
		    }
		}
	    }
	    /* Some addressing modes are invalid as destination */
	    if (destmode == imm || destmode == PC16 || destmode == PC8r)
		goto nomatch;
	    break;
	case 's':
	    destreg = bitval[bitS];
	    destmode = mode_from_mr(bitval[bits],bitval[bitS]);

	    if (destmode == am_illg)
		continue;
	    if (CPU_EMU_SIZE < 1 &&
		(destmode == Areg || destmode == Dreg || destmode == Aind
		 || destmode == Ad16 || destmode == Ad8r || destmode == Aipi
		 || destmode == Apdi))
		{
		    dstgather = 1; dstpos = bitpos[bitS];
		}

	    if (opcstr[pos] == '[') {
		pos++;
		if (opcstr[pos] == '!') {
		    /* exclusion */
		    do {
			pos++;
			if (mode_from_str(opcstr+pos) == destmode)
			    goto nomatch;
			pos += 4;
		    } while (opcstr[pos] == ',');
		    pos++;
		} else {
		    if (opcstr[pos+4] == '-') {
			/* replacement */
			if (mode_from_str(opcstr+pos) == destmode)
			    destmode = mode_from_str(opcstr+pos+5);
			else
			    goto nomatch;
			pos += 10;
		    } else {
			/* normal */
			while(mode_from_str(opcstr+pos) != destmode) {
			    pos += 4;
			    if (opcstr[pos] == ']')
				goto nomatch;
			    pos++;
			}
			while(opcstr[pos] != ']') pos++;
			pos++;
		    }
		}
	    }
	    break;
	default: abort();
	}
	/* safety check - might have changed */
	if (destmode != Areg && destmode != Dreg && destmode != Aind
	    && destmode != Ad16 && destmode != Ad8r && destmode != Aipi
	    && destmode != Apdi)
	    {
		dstgather = 0;
	    }

	if (destmode == Areg && sz == sz_byte)
	    goto nomatch;
#if 0
	if (sz == sz_byte && (destmode == Aipi || destmode == Apdi)) {
	    dstgather = 0;
	}
#endif
      endofline:
	/* now, we have a match */
	if (table68k[opc].mnemo != i_ILLG)
	    write_log ("Double match: %x: %s\n", opc, opcstr);
	if (find == -1) {
	    for (find = 0;; find++) {
		if (strcmp(mnemonic, lookuptab[find].name) == 0) {
		    table68k[opc].mnemo = lookuptab[find].mnemo;
		    break;
		}
		if (strlen(lookuptab[find].name) == 0) abort();
	    }
	}
	else {
	    table68k[opc].mnemo = lookuptab[find].mnemo;
	}
	table68k[opc].cc = bitval[bitc];
	if (table68k[opc].mnemo == i_BTST
	    || table68k[opc].mnemo == i_BSET
	    || table68k[opc].mnemo == i_BCLR
	    || table68k[opc].mnemo == i_BCHG)
	    {
		sz = destmode == Dreg ? sz_long : sz_byte;
	    }
	table68k[opc].size = sz;
	table68k[opc].sreg = srcreg;
	table68k[opc].dreg = destreg;
	table68k[opc].smode = srcmode;
	table68k[opc].dmode = destmode;
	table68k[opc].spos = srcgather ? srcpos : -1;
	table68k[opc].dpos = dstgather ? dstpos : -1;
	table68k[opc].suse = usesrc;
	table68k[opc].duse = usedst;
	table68k[opc].stype = srctype;
	table68k[opc].plev = id.plevel;
	table68k[opc].clev = specialcase(opc, id.cpulevel);

#if 0
	for (i = 0; i < 5; i++) {
	    table68k[opc].flaginfo[i].flagset = id.flaginfo[i].flagset;
	    table68k[opc].flaginfo[i].flaguse = id.flaginfo[i].flaguse;
	}
#endif
	table68k[opc].flagdead = flagdead;
	table68k[opc].flaglive = flaglive;
	table68k[opc].isjmp = isjmp;
      nomatch:
	/* FOO! */;
    }
}


/****f* januae-readcpu/read_table68k *************************************
*
*   NAME
*       read_table68k() -- Rebuild the 65536-entry instruction decode table.
*
*   SYNOPSIS
*       void read_table68k (void);
*
*   FUNCTION
*       Release the previous table68k allocation, allocate a new table,
*       initialize each mnemonic to i_ILLG and handler to -1, then expand
*       all definitions from defs68k into the table.
*
*   INPUTS
*       None. Reads defs68k and n_defs68k.
*
*   RESULT
*       None. The global table68k pointer refers to the new table.
*
*   NOTES
*       Any prior table68k pointer becomes invalid when this function
*       begins. Other descriptor fields are assigned only for matched
*       instructions; invalid entries must not be treated as initialized
*       instruction descriptors.
*
****************************************************************************
*
*/

void read_table68k (void)
{
    int i;

    free (table68k);
    table68k = (struct instr *)xmalloc (65536 * sizeof (struct instr));
    for (i = 0; i < 65536; i++) {
	table68k[i].mnemo = i_ILLG;
	table68k[i].handler = -1;
    }
    for (i = 0; i < n_defs68k; i++) {
	build_insn (i);
    }
}

static int mismatch;

/****i* januae-readcpu/handle_merges *************************************
*
*   NAME
*       handle_merges() -- Identify equivalent register variants of an opcode.
*
*   SYNOPSIS
*       static void handle_merges (long int opcode);
*
*   FUNCTION
*       Visit the variable source- and destination-register combinations
*       represented by opcode. If the decoded instruction properties
*       match, set the variant handler field to the representative opcode;
*       otherwise increment the global mismatch counter.
*
*   INPUTS
*       opcode - Representative opcode index, from 0 through 65535.
*
*   RESULT
*       None. Updates table68k handler fields and mismatch.
*
*   NOTES
*       Requires a table populated by read_table68k(). It is called
*       for valid representative opcodes by do_merges().
*
****************************************************************************
*
*/

static void handle_merges (long int opcode)
{
    uae_u16 smsk;
    uae_u16 dmsk;
    int sbitdst, dstend;
    int srcreg, dstreg;

    if (table68k[opcode].spos == -1) {
	sbitdst = 1; smsk = 0;
    } else {
	switch (table68k[opcode].stype) {
	 case 0:
	    smsk = 7; sbitdst = 8; break;
	 case 1:
	    smsk = 255; sbitdst = 256; break;
	 case 2:
	    smsk = 15; sbitdst = 16; break;
	 case 3:
	    smsk = 7; sbitdst = 8; break;
	 case 4:
	    smsk = 7; sbitdst = 8; break;
	 case 5:
	    smsk = 63; sbitdst = 64; break;
	 case 7:
	    smsk = 3; sbitdst = 4; break;
	 default:
	    smsk = 0; sbitdst = 0;
	    abort();
	    break;
	}
	smsk <<= table68k[opcode].spos;
    }
    if (table68k[opcode].dpos == -1) {
	dstend = 1; dmsk = 0;
    } else {
	dmsk = 7 << table68k[opcode].dpos;
	dstend = 8;
    }
    for (srcreg=0; srcreg < sbitdst; srcreg++) {
	for (dstreg=0; dstreg < dstend; dstreg++) {
	    uae_u16 code = (uae_u16)opcode;

	    code = (code & ~smsk) | (srcreg << table68k[opcode].spos);
	    code = (code & ~dmsk) | (dstreg << table68k[opcode].dpos);

	    /* Check whether this is in fact the same instruction.
	     * The instructions should never differ, except for the
	     * Bcc.(BW) case. */
	    if (table68k[code].mnemo != table68k[opcode].mnemo
		|| table68k[code].size != table68k[opcode].size
		|| table68k[code].suse != table68k[opcode].suse
		|| table68k[code].duse != table68k[opcode].duse)
	    {
		mismatch++; continue;
	    }
	    if (table68k[opcode].suse
		&& (table68k[opcode].spos != table68k[code].spos
		    || table68k[opcode].smode != table68k[code].smode
		    || table68k[opcode].stype != table68k[code].stype))
	    {
		mismatch++; continue;
	    }
	    if (table68k[opcode].duse
		&& (table68k[opcode].dpos != table68k[code].dpos
		    || table68k[opcode].dmode != table68k[code].dmode))
	    {
		mismatch++; continue;
	    }

	    if (code != opcode)
		table68k[code].handler = opcode;
	}
    }
}

/****f* januae-readcpu/do_merges *****************************************
*
*   NAME
*       do_merges() -- Merge equivalent opcode entries and count handlers.
*
*   SYNOPSIS
*       void do_merges (void);
*
*   FUNCTION
*       Reset the mismatch count, visit valid opcodes without a handler,
*       and merge their equivalent register variants. Store the number
*       of representative instructions in nr_cpuop_funcs.
*
*   INPUTS
*       None. Operates on table68k.
*
*   RESULT
*       None. Updates table68k, nr_cpuop_funcs and the mismatch count.
*
*   NOTES
*       Call read_table68k() before merging. Handler assignments persist
*       until the instruction table is rebuilt.
*
****************************************************************************
*
*/

void do_merges (void)
{
    long int opcode;
    int nr = 0;
    mismatch = 0;
    for (opcode = 0; opcode < 65536; opcode++) {
	if (table68k[opcode].handler != -1 || table68k[opcode].mnemo == i_ILLG)
	    continue;
	nr++;
	handle_merges (opcode);
    }
    nr_cpuop_funcs = nr;
}

/****f* januae-readcpu/get_no_mismatches *********************************
*
*   NAME
*       get_no_mismatches() -- Return the current opcode merge mismatch count.
*
*   SYNOPSIS
*       int get_no_mismatches (void);
*
*   FUNCTION
*       Read the mismatch counter last reset by do_merges().
*
*   INPUTS
*       None.
*
*   RESULT
*       Number of mismatching variant comparisons recorded by merging.
*
*   NOTES
*       The counter is not reset by this call.
*
****************************************************************************
*
*/

int get_no_mismatches (void)
{
    return mismatch;
}
