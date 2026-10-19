 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Win32 thread support
  *
  * Copyright 1997 Mathias Ortmann
  * Copyright 2005 Richard Drummond
  *
  */

#include "thread.h"

/****i* januae-thread/uae_sem_init *****************************************
*
*   NAME
*       uae_sem_init()
*
*   SYNOPSIS
*       void uae_sem_init (HANDLE *event, int manual_reset, int
*       initial_state);
*
*   FUNCTION
*       Initialize or reset a Windows event object used as a semaphore.
*
*   INPUTS
*       event - Storage for an existing or new event handle.
*       manual_reset - Reset mode used when creating an event.
*       initial_state - Desired initial event signal state.
*
*   NOTES
*       If the handle already exists, updates its signal state; otherwise
*       calls CreateEvent. The call does not report creation failure.
*
****************************************************************************
*
*/
void uae_sem_init (HANDLE *event, int manual_reset, int initial_state)
{
    if (*event) {
	if (initial_state)
	    SetEvent (*event);
	else
	    ResetEvent( *event );
    } else
	*event = CreateEvent (NULL, manual_reset, initial_state, NULL);
}

/****i* januae-thread/uae_sem_wait *****************************************
*
*   NAME
*       uae_sem_wait()
*
*   SYNOPSIS
*       void uae_sem_wait (HANDLE *event);
*
*   FUNCTION
*       Wait indefinitely for a Windows event to become signaled.
*
*   INPUTS
*       event - Pointer to an event handle.
*
*   NOTES
*       Does not inspect the WaitForSingleObject return value.
*
****************************************************************************
*
*/
void uae_sem_wait (HANDLE *event)
{
    WaitForSingleObject (*event, INFINITE);
}

/****i* januae-thread/uae_sem_post *****************************************
*
*   NAME
*       uae_sem_post()
*
*   SYNOPSIS
*       void uae_sem_post (HANDLE * event);
*
*   FUNCTION
*       Signal a Windows event object.
*
*   INPUTS
*       event - Pointer to an event handle.
*
*   NOTES
*       Calls SetEvent without testing its return status.
*
****************************************************************************
*
*/
void uae_sem_post (HANDLE * event)
{
    SetEvent (*event);
}

/****i* januae-thread/uae_sem_trywait **************************************
*
*   NAME
*       uae_sem_trywait()
*
*   SYNOPSIS
*       int uae_sem_trywait (HANDLE * event);
*
*   FUNCTION
*       Try to acquire a signaled Windows event without blocking.
*
*   INPUTS
*       event - Pointer to an event handle.
*
*   RESULT
*       Zero for WAIT_OBJECT_0; otherwise -1.
*
*   NOTES
*       Calls WaitForSingleObject with a zero timeout.
*
****************************************************************************
*
*/
int uae_sem_trywait (HANDLE * event)
{
    return WaitForSingleObject (*event, 0) == WAIT_OBJECT_0 ? 0 : -1;
}

/****i* januae-thread/uae_sem_close ****************************************
*
*   NAME
*       uae_sem_close()
*
*   SYNOPSIS
*       void uae_sem_close (HANDLE * event);
*
*   FUNCTION
*       Close an existing Windows event handle and clear its storage.
*
*   INPUTS
*       event - Pointer to a handle.
*
*   NOTES
*       Does nothing for a null handle; does not report CloseHandle failure.
*
****************************************************************************
*
*/
void uae_sem_close (HANDLE * event)
{
    if (*event) {
	CloseHandle (*event);
	*event = NULL;
    }
}


typedef unsigned (__stdcall *BEGINTHREADEX_FUNCPTR)(void *);

/****i* januae-thread/uae_start_thread *************************************
*
*   NAME
*       uae_start_thread()
*
*   SYNOPSIS
*       int uae_start_thread (void *(*f)(void *), void *arg, DWORD *foo);
*
*   FUNCTION
*       Create a Windows thread executing the supplied entry point.
*
*   INPUTS
*       f - Thread entry point.
*       arg - Argument for the entry point.
*       foo - Destination for the thread identifier.
*
*   RESULT
*       One when _beginthreadex returns a handle; otherwise zero.
*
*   NOTES
*       Requests THREAD_PRIORITY_HIGHEST on a successful handle. This
*       function does not close that handle or verify the priority change.
*
****************************************************************************
*
*/
int uae_start_thread (void *(*f)(void *), void *arg, DWORD *foo)
{
    HANDLE hThread;
    int result = 1;

    hThread = (HANDLE)_beginthreadex (NULL, 0, (BEGINTHREADEX_FUNCPTR)f, arg, 0, foo);
    if (hThread)
	SetThreadPriority (hThread, THREAD_PRIORITY_HIGHEST);
    else
	result = 0;
    return result;
}

/****i* januae-thread/uae_set_thread_priority ******************************
*
*   NAME
*       uae_set_thread_priority()
*
*   SYNOPSIS
*       void uae_set_thread_priority (int pri);
*
*   FUNCTION
*       Accept a requested thread priority without applying it.
*
*   INPUTS
*       pri - Requested priority.
*
*   NOTES
*       This backend provides an empty implementation.
*
****************************************************************************
*
*/
void uae_set_thread_priority (int pri)
{
}
