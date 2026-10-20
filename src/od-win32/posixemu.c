/*
 * UAE - The Un*x Amiga Emulator
 *
 * Win32 interface
 *
 * Copyright 1997 Mathias Ortmann
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include <windows.h>
#include <sys/timeb.h>

#include "posixemu.h"
#include "filesys.h"

static DWORD lasterror;


#ifndef HAVE_GETTIMEOFDAY
/****i* januae-win32/gettimeofday ******************************************
*
*   NAME
*       gettimeofday()
*
*   SYNOPSIS
*       void gettimeofday (struct timeval *tv, void *blah);
*
*   FUNCTION
*       Read the current host time into a POSIX timeval.
*
*   INPUTS
*       tv - Output time structure.
*       blah - Unused compatibility argument.
*
*   NOTES
*       When HAVE_GETTIMEOFDAY is undefined, reads ftime() and converts
*       milliseconds to microseconds. It does not report clock-read
*       failures.
*
****************************************************************************
*
*/
void gettimeofday (struct timeval *tv, void *blah)
{
    struct timeb time;
    ftime (&time);

    tv->tv_sec  = time.time;
    tv->tv_usec = time.millitm * 1000;
}
#endif

#ifndef HAVE_TRUNCATE
/****i* januae-win32/truncate **********************************************
*
*   NAME
*       truncate()
*
*   SYNOPSIS
*       int truncate (const char *name, long int len);
*
*   FUNCTION
*       Set an existing file length using Windows file operations.
*
*   INPUTS
*       name - File to resize.
*       len - Requested length from the beginning of the file.
*
*   RESULT
*       Zero on success; -1 on failure.
*
*   NOTES
*       Only built without HAVE_TRUNCATE. Opens the file, seeks and calls
*       SetEndOfFile; records GetLastError in a private variable on failure.
*       This implementation does not set errno or expose that private error.
*
****************************************************************************
*
*/
int truncate (const char *name, long int len)
{
    HANDLE hFile;
    BOOL bResult = FALSE;
    int result = -1;

    if ((hFile = CreateFile (name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)) != INVALID_HANDLE_VALUE) {
	if (SetFilePointer (hFile, len, NULL, FILE_BEGIN) == (DWORD)len) {
	    if (SetEndOfFile (hFile) == TRUE)
		result = 0;
        } else {
	    write_log ("SetFilePointer() failure for %s to posn %d\n", name, len);
	}
	CloseHandle (hFile);
    } else {
	write_log( "CreateFile() failed to open %s\n", name );
    }

    if (result == -1)
	lasterror = GetLastError ();
    return result;
}
#endif

/****i* januae-win32/isspecialdrive ****************************************
*
*   NAME
*       isspecialdrive()
*
*   SYNOPSIS
*       int isspecialdrive (const char *name);
*
*   FUNCTION
*       Check whether Windows reports a drive path as not ready.
*
*   INPUTS
*       name - Path to inspect.
*
*   RESULT
*       One for ERROR_NOT_READY, minus one for another attribute error, or
*       zero otherwise.
*
*   NOTES
*       Temporarily suppresses critical-error dialogs while querying file
*       attributes, then restores the prior error mode.
*
****************************************************************************
*
*/
int isspecialdrive (const char *name)
{
    DWORD v;
    DWORD err;
    
    DWORD last = SetErrorMode (SEM_FAILCRITICALERRORS);

    v = GetFileAttributes (name);
    err = GetLastError ();
    
    SetErrorMode (last);
    
    if (v != INVALID_FILE_ATTRIBUTES)
	return 0;
    if (err == ERROR_NOT_READY)
	return 1;
    if (err)
	return -1;
    return 0;
}
