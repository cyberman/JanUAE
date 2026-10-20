 /*
  * UAE - The Un*x Amiga Emulator
  *
  * OS-specific memory support functions
  *
  * Copyright 2004 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "memory.h"

#ifdef JIT

/****i* januae-od-win32/cache_alloc ***************************************
*
*   NAME
*       cache_alloc()
*
*   SYNOPSIS
*       void *cache_alloc (int size);
*
*   FUNCTION
*       Allocate JIT cache storage with malloc().
*
*   INPUTS
*       size - Requested byte count.
*
*   RESULT
*       Allocated pointer, or NULL on allocation failure.
*
*   NOTES
*       Available only with JIT. No executable-memory permission request is
*       made here.
*
*************************************************************************
*
*/
void *cache_alloc (int size)
{
    return malloc (size);
}

/****i* januae-od-win32/cache_free ****************************************
*
*   NAME
*       cache_free()
*
*   SYNOPSIS
*       void cache_free (void *cache);
*
*   FUNCTION
*       Release a JIT cache allocation with free().
*
*   INPUTS
*       cache - Allocated buffer, or NULL.
*
*   NOTES
*       Available only with JIT.
*
*************************************************************************
*
*/
void cache_free (void *cache)
{
    free (cache);
}

#endif
