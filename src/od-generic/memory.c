 /*
  * UAE - The Un*x Amiga Emulator
  *
  * OS-specific memory support functions
  *
  * Copyright 2004 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "include/memory.h"

#ifdef JIT

#include <sys/mman.h>

/****i* januae-od-generic/cache_alloc *************************************
*
*   NAME
*       cache_alloc()
*
*   SYNOPSIS
*       void *cache_alloc (int size);
*
*   FUNCTION
*       Allocate page-aligned JIT cache storage with valloc(), then request
*       read, write and execute access with mprotect().
*
*   INPUTS
*       size - Requested byte count; values below one page are raised to
*       getpagesize().
*
*   RESULT
*       Pointer returned by valloc(), or NULL on allocation failure.
*
*   NOTES
*       The result of mprotect() is ignored; a non-NULL result does not
*       guarantee executable access. Available only with JIT.
*
*************************************************************************
*
*/
void *cache_alloc (int size)
{
   void *cache;

   size = size < getpagesize() ? getpagesize() : size;

   if ((cache = valloc (size)))
	mprotect (cache, size, PROT_READ|PROT_WRITE|PROT_EXEC);

   return cache;
}

/****i* januae-od-generic/cache_free **************************************
*
*   NAME
*       cache_free()
*
*   SYNOPSIS
*       void cache_free (void *cache);
*
*   FUNCTION
*       Release cache storage previously obtained from cache_alloc().
*
*   INPUTS
*       cache - Allocated cache pointer, or NULL.
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

#ifdef NATMEM_OFFSET
/****i* januae-od-generic/init_shm ****************************************
*
*   NAME
*       init_shm()
*
*   SYNOPSIS
*       void init_shm (void);
*
*   FUNCTION
*       Set canbang to 1 to enable the native-memory-offset path.
*
*   NOTES
*       Compiled only when both JIT and NATMEM_OFFSET are enabled.
*
*************************************************************************
*
*/
void init_shm (void)
{
    canbang = 1;
}
#endif

#endif
