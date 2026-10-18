
#include <exec/memory.h>

#include "trap.h"
#ifndef __AROS__
ULONG (*calltrap)(ULONG __asm("d0"),
                  ULONG __asm("d1"),
		  APTR  __asm("a0")) = (APTR) AROSTRAPBASE;

#else

/* AROSTRAPBASE 0xF0FF90 */

/****i* januae/calltrap ****************************************************
*
*   NAME
*       calltrap() -- invoke the JanUAE guest-to-host trap using three
*       register arguments.
*
*   SYNOPSIS
*       ULONG calltrap(ULONG arg1, ULONG arg2, ULONG *arg3);
*
*   FUNCTION
*       Load the primary service into D0, the service-specific second
*       argument into D1, and the guest parameter address into A0. Execute
*       the JanUAE trap at 0xF0FF90 and return its D0 result.
*
*   INPUTS
*       arg1 - Primary service loaded into D0.
*       arg2 - Service-dependent value loaded into D1.
*       arg3 - Guest parameter/result address loaded into A0.
*
*   RESULT
*       Returns the D0 value produced by the trap.
*
*   NOTES
*       AROS m68k implementation. D1 is interpreted by the selected service;
*       it is not always a buffer size. The non-AROS branch uses a
*       register-bound function pointer.
*
****************************************************************************
*
*/

ULONG calltrap(ULONG arg1, ULONG arg2, ULONG *arg3) {
  ULONG ret=0;

   /* Inline assembly is always somewhat ugly.. */
   /* AROSTRAPBASE is 0xF0FF90 */

  asm volatile ("movem.l %%d0-%%d7/%%a0-%%a6,-(%%SP)\n" \
                "move.l %1, %%d0\n" \
                "move.l %2, %%d1\n" \
                "move.l %3, %%a0\n" \
                "jsr 0xF0FF90\n" \
                "move.l %%d0, %0\n" \
                "movem.l (%%SP)+, %%d0-%%d7/%%a0-%%a6\n" \
                             : "=g"(ret)                        /* %0 */
                             : "g"(arg1), "g"(arg2), "g"(arg3)  /* %1, %2, %3 */
                             :                                  /* nothing clobbered */
                             );

  return ret;
}

/****i* januae/calltrap_d01_a0_d2345 ***************************************
*
*   NAME
*       calltrap_d01_a0_d2345() -- invoke the JanUAE guest-to-host trap with
*       an extended register argument set.
*
*   SYNOPSIS
*       ULONG calltrap_d01_a0_d2345(ULONG r_d0, ULONG r_d1, ULONG r_a0,
*       ULONG r_d2, ULONG r_d3, ULONG r_d4, ULONG r_d5);
*
*   FUNCTION
*       Load D0, D1, A0, and D2 through D5 from the supplied arguments.
*       Execute the JanUAE trap at 0xF0FF90 and return its D0 result while
*       restoring the registers saved by the assembly wrapper.
*
*   INPUTS
*       r_d0 - Value loaded into D0.
*       r_d1 - Value loaded into D1.
*       r_a0 - Guest address loaded into A0.
*       r_d2 - Value loaded into D2.
*       r_d3 - Value loaded into D3.
*       r_d4 - Value loaded into D4.
*       r_d5 - Value loaded into D5.
*
*   RESULT
*       Returns the D0 value produced by the trap.
*
*   NOTES
*       AROS m68k implementation. The selected service defines the register
*       argument contract.
*
****************************************************************************
*
*/

ULONG calltrap_d01_a0_d2345(ULONG r_d0, ULONG r_d1, ULONG r_a0, ULONG r_d2, ULONG r_d3, ULONG r_d4, ULONG r_d5){
  ULONG ret=0;

   /* Inline assembly is always somewhat ugly.. */
   /* AROSTRAPBASE is 0xF0FF90 */

  /* DebOut("r_d0 %d, r_d1 %d, r_a0 %lx, r_d2 %d, r_d3 %d, r_d4 %d, r_d5 %d)\n", r_d0, r_d1, r_a0, r_d2, r_d3, r_d4, r_d5); */

  asm volatile ("movem.l %%d0-%%d7/%%a0-%%a6,-(%%SP)\n" \
                "move.l %1, %%d0\n" \
                "move.l %2, %%d1\n" \
                "move.l %3, %%a0\n" \
                "move.l %4, %%d2\n" \
                "move.l %5, %%d3\n" \
                "move.l %6, %%d4\n" \
                "move.l %7, %%d5\n" \
                "jsr 0xF0FF90\n" \
                "move.l %%d0, %0\n" \
                "movem.l (%%SP)+, %%d0-%%d7/%%a0-%%a6\n" \
                             : "=r"(ret)                        /* %0 */
                             : "m"(r_d0), "m"(r_d1), "m"(r_a0), "m"(r_d2), "m"(r_d3), "m"(r_d4), "m"(r_d5)  
                             :                                  /* nothing clobbered */
                             );

  return ret;

}
#endif


