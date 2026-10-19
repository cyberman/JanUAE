 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Dummy thread support
  *
  * Copyright 2004 Richard Drummond
  *
  */

/* Stop OS X linker complaing about an empty linker object */
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
*       Provide an otherwise empty thread-backend object for the linker.
*
*   NOTES
*       Does not create a thread or perform synchronization.
*
****************************************************************************
*
*/
void thread_dummy (void)
{
}
