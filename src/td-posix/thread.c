 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Threading support, using pthreads
  *
  * Copyright 1997 Bernd Schmidt
  * Copyright 2004 Richard Drummond
  *
  * This handles initialization when using named semaphores.
  * Idea stolen from SDL.
  */

#include <pthread.h>
#include <semaphore.h>

#include "sysconfig.h"
#include "sysdeps.h"

#include "threaddep/thread.h"

#ifdef USE_NAMED_SEMAPHORES

/****i* januae-thread/uae_sem_init *****************************************
*
*   NAME
*       uae_sem_init()
*
*   SYNOPSIS
*       int uae_sem_init (uae_sem_t *sem, int pshared, unsigned int value);
*
*   FUNCTION
*       Create and immediately unlink a named POSIX semaphore.
*
*   INPUTS
*       sem - Semaphore wrapper to initialize.
*       pshared - Unused by this implementation.
*       value - Initial count.
*
*   RESULT
*       Zero on success; -1 if sem_open fails.
*
*   NOTES
*       Compiled only with USE_NAMED_SEMAPHORES. Names include the process
*       ID and an incrementing static serial number. On failure sem->sem is
*       set to zero.
*
****************************************************************************
*
*/
int uae_sem_init (uae_sem_t *sem, int pshared, unsigned int value)
{
    char name[32];
    static int semno = 0;
    int result = 0;

    sprintf (name, "/uaesem-%d-%d", getpid (), semno++);

    if ((sem->sem = sem_open (name, O_CREAT, 0600, value)) != (sem_t *)SEM_FAILED)
	sem_unlink (name);
    else {
	sem->sem = 0;
	result = -1;
    }
    return result;
}

#else

/* Satisfy those linkers that complain about empty linker libs */
void thread_dummy (void);

/****i* januae-thread/thread_dummy *****************************************
*
*   NAME
*       thread_dummy()
*
*   SYNOPSIS
*       void thread_dummy (void);
*
*   FUNCTION
*       Provide a linker-visible stub when named semaphores are disabled.
*
*   NOTES
*       Compiled only when USE_NAMED_SEMAPHORES is not defined.
*
****************************************************************************
*
*/
void thread_dummy (void)
{
}

#endif
