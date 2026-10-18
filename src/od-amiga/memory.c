 /*
  * UAE - The Un*x Amiga Emulator
  *
  * OS-specific memory support functions
  *
  * Copyright 2004 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef JIT

/*
 * Allocate executable memory for JIT cache
 */
/****i* januae-memory/cache_alloc ******************************************
*
*   NAME
*       cache_alloc() -- Allocate JIT cache memory.
*
*   SYNOPSIS
*       void *cache_alloc (int size);
*
*   FUNCTION
*       Allocate a byte range with the C runtime allocator.
*
*   INPUTS
*       size - Number of bytes to allocate.
*
*   RESULT
*       Pointer returned by malloc, or NULL on allocation failure.
*
*   NOTES
*       Compiled only with JIT enabled. This implementation makes no
*       separate executable-memory request.
*
****************************************************************************
*
*/

void *cache_alloc (int size)
{
   return malloc (size);
}

/****i* januae-memory/cache_free *******************************************
*
*   NAME
*       cache_free() -- Release JIT cache memory.
*
*   SYNOPSIS
*       void cache_free (void *cache);
*
*   FUNCTION
*       Return a JIT cache allocation to the C runtime allocator.
*
*   INPUTS
*       cache - Pointer previously obtained from cache_alloc().
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Compiled only with JIT enabled.
*
****************************************************************************
*
*/

void cache_free (void *cache)
{
    free (cache);
}

#endif
