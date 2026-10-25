 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Various stuff missing in some OSes.
  *
  * Copyright 1997 Bernd Schmidt
  */

#include "sysconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include "uae_string.h"
#include "uae_types.h"
#include "uae_malloc.h"
#include "writelog.h"

#ifndef HAVE_STRDUP

char *my_strdup (const char *s)
{
    /* The casts to char * are there to shut up the compiler on HPUX */
    char *x = (char*)xmalloc(strlen((char *)s) + 1);
    strcpy(x, (char *)s);
    return x;
}

#endif

/****i* januae-core-internal/xmalloc ***************************************************************
 *
 *   NAME
 *       xmalloc() -- Allocate an uninitialized buffer or terminate on allocation failure.
 *
 *   SYNOPSIS
 *       void *xmalloc (size_t n);
 *
 *   FUNCTION
 *       Returns the pointer supplied by malloc().
 *
 *   INPUTS
 *       n - Number of bytes to allocate.
 *
 *   NOTES
 *       If malloc() returns NULL, logs the requested size and aborts the process. The caller releases successful allocations with xfree() or free().
 *
 **************************************************************************
 *
 */

void *xmalloc (size_t n)
{
    void *a = malloc (n);
    if (a == NULL) {
	write_log ("xmalloc(%d): virtual memory exhausted\n", n);
	abort ();
    }
    return a;
}

/****i* januae-core-internal/xcalloc ***************************************************************
 *
 *   NAME
 *       xcalloc() -- Allocate zero-initialized storage or terminate on allocation failure.
 *
 *   SYNOPSIS
 *       void *xcalloc (size_t n, size_t size);
 *
 *   FUNCTION
 *       Returns the pointer supplied by calloc().
 *
 *   INPUTS
 *       n - Number of elements.
 *       size - Size of each element in bytes.
 *
 *   NOTES
 *       If calloc() returns NULL, logs the requested total size and aborts the process. The caller must free a successful allocation.
 *
 **************************************************************************
 *
 */

void *xcalloc (size_t n, size_t size)
{
    void *a = calloc (n, size);
    if (a == NULL) {
	write_log ("xcalloc(%d): virtual memory exhausted\n", n * size);
	abort ();
    }
    return a;
}

/****i* januae-core-internal/xfree *****************************************************************
 *
 *   NAME
 *       xfree() -- Release storage allocated by the emulator allocation helpers.
 *
 *   SYNOPSIS
 *       void xfree (void *ptr);
 *
 *   FUNCTION
 *       Calls free() with the supplied pointer.
 *
 *   INPUTS
 *       ptr - Allocation pointer to release, or NULL.
 *
 *   NOTES
 *       Accepts NULL with the semantics of the C library free().
 *
 **************************************************************************
 *
 */

void xfree (void *ptr)
{
    free (ptr);
}
