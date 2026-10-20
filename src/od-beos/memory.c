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

/****i* januae-od-beos/cache_alloc ****************************************
*
*   NAME
*       cache_alloc()
*
*   SYNOPSIS
*       void *cache_alloc (int size);
*
*   FUNCTION
*       Allocate a JIT cache buffer using malloc().
*
*   INPUTS
*       size - Requested allocation size in bytes.
*
*   RESULT
*       Allocated pointer, or NULL on failure.
*
*   NOTES
*       Available only with JIT. No executable-memory permission request is
*       performed here.
*
*************************************************************************
*
*/
void *cache_alloc (int size)
{
   return malloc (size);
}

/****i* januae-od-beos/cache_free *****************************************
*
*   NAME
*       cache_free()
*
*   SYNOPSIS
*       void cache_free (void *cache);
*
*   FUNCTION
*       Free a buffer returned by cache_alloc().
*
*   INPUTS
*       cache - Cache buffer, or NULL.
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
