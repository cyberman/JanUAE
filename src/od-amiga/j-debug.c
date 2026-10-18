/************************************************************************ 
 *
 * Copyright 2009 Oliver Brunner - aros<at>oliver-brunner.de
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
 * Helper function to write to AROS console from the amigaOS side.
 *
 ************************************************************************/

#include "j.h"

#define M68K_DEBUG 1

/****i* januae/ad_debug ****************************************************
*
*   NAME
*       ad_debug() -- Forward guest diagnostics to the host.
*
*   SYNOPSIS
*       uae_u32 ad_debug(ULONG *m68k_results);
*
*   FUNCTION
*       Resolve a guest text pointer and write the message to the AROS
*       debugging console with an m68k prefix.
*
*   INPUTS
*       m68k_results - Guest address of a zero-terminated diagnostic string.
*
*   RESULT
*       TRUE.
*
*   NOTES
*       The routine expects guest text to be accessible as a host string.
*
****************************************************************************
*
*/

uae_u32 ad_debug(ULONG *m68k_results) {

//#if M68K_DEBUG
  kprintf("m68k/%s",(char *) get_real_address(m68k_results));
//#endif

  return TRUE;
}
