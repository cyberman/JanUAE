
#include "sysconfig.h"
#include "sysdeps.h"

#include <windows.h>

#define SHOW_CONSOLE 0

static int consoleopen = 0;
static HANDLE stdinput;
static HANDLE stdoutput;

FILE *debugfile = NULL;
int console_logging;

#define WRITE_LOG_BUF_SIZE 4096

/* console functions for debugger */

/****i* januae-win32/openconsole *******************************************
*
*   NAME
*       openconsole()
*
*   SYNOPSIS
*       static void openconsole (void);
*
*   FUNCTION
*       Allocate and initialize the process console on first use.
*
*   NOTES
*       Caches the standard handles and sets console input mode. Marks the
*       console open without checking whether all Windows calls succeeded.
*
****************************************************************************
*
*/
static void openconsole (void)
{
    if (consoleopen)
	return;

    AllocConsole ();
    stdinput  = GetStdHandle (STD_INPUT_HANDLE);
    stdoutput = GetStdHandle (STD_OUTPUT_HANDLE);
    SetConsoleMode (stdinput, ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT |
			      ENABLE_ECHO_INPUT | ENABLE_PROCESSED_OUTPUT);
    consoleopen = 1;
}

/****i* januae-win32/console_out *******************************************
*
*   NAME
*       console_out()
*
*   SYNOPSIS
*       void console_out (const char *format,...);
*
*   FUNCTION
*       Format text and write it to the Windows console.
*
*   INPUTS
*       format - printf-style format.
*       ... - Format arguments.
*
*   NOTES
*       Opens the console on demand. Formats into a fixed 4096-byte stack
*       buffer; truncation and termination are not checked before strlen()
*       and WriteConsole().
*
****************************************************************************
*
*/
void console_out (const char *format,...)
{
    va_list parms;
    char buffer[WRITE_LOG_BUF_SIZE];
    DWORD temp;

    va_start (parms, format);
    _vsnprintf (buffer, WRITE_LOG_BUF_SIZE - 1, format, parms);
    va_end (parms);

    openconsole ();

    WriteConsole (stdoutput, buffer, strlen (buffer), &temp, 0);
}

/****i* januae-win32/console_get *******************************************
*
*   NAME
*       console_get()
*
*   SYNOPSIS
*       int console_get (char *out, int maxlen);
*
*   FUNCTION
*       Read console characters until carriage return or the requested
*       limit.
*
*   INPUTS
*       out - Output buffer for characters and a trailing zero.
*       maxlen - Maximum number of characters to read.
*
*   RESULT
*       Number of characters advanced in the output buffer.
*
*   NOTES
*       Requires an initialized console. Uses ReadConsole without checking
*       its result. Writes a zero terminator even when the entire maxlen
*       capacity has been consumed.
*
****************************************************************************
*
*/
int console_get (char *out, int maxlen)
{
    DWORD len;
    DWORD totallen = 0;

    while (maxlen > 0) {
	ReadConsole (stdinput, out, 1, &len, 0);
	if (*out == 13)
	    break;
	out++;
	maxlen--;
	totallen++;
    }
    *out = 0;
    return totallen;
}

/****i* januae-win32/console_flush *****************************************
*
*   NAME
*       console_flush()
*
*   SYNOPSIS
*       void console_flush (void);
*
*   FUNCTION
*       Provide a no-op console flush operation.
*
*   NOTES
*       Does not flush either console stream.
*
****************************************************************************
*
*/
void console_flush (void)
{
}

/****i* januae-win32/write_dlog ********************************************
*
*   NAME
*       write_dlog()
*
*   SYNOPSIS
*       void write_dlog (const char *format, ...);
*
*   FUNCTION
*       Format and deliver a diagnostic log message.
*
*   INPUTS
*       format - printf-style format.
*       ... - Format arguments.
*
*   NOTES
*       Writes to the console when console_logging is enabled and to
*       debugfile when non-null. Formatted output and Windows write results
*       are not checked.
*
****************************************************************************
*
*/
void write_dlog (const char *format, ...)
{
    int count;
    DWORD numwritten;
    char buffer[WRITE_LOG_BUF_SIZE];

    va_list parms;
    va_start (parms, format);
    count = _vsnprintf (buffer, WRITE_LOG_BUF_SIZE - 1, format, parms);
    if (SHOW_CONSOLE || console_logging) {
	openconsole ();
	WriteConsole (stdoutput, buffer, strlen (buffer), &numwritten, 0);
    }
    if (debugfile) {
	fprintf (debugfile, buffer);
	fflush (debugfile);
    }
    va_end (parms);
}

/****i* januae-win32/write_log *********************************************
*
*   NAME
*       write_log()
*
*   SYNOPSIS
*       void write_log (const char *format, ...);
*
*   FUNCTION
*       Format and deliver a standard emulator log message.
*
*   INPUTS
*       format - printf-style format.
*       ... - Format arguments.
*
*   NOTES
*       Uses the same console and debugfile destinations as write_dlog().
*       The fixed-size format buffer may not be zero-terminated after
*       truncation.
*
****************************************************************************
*
*/
void write_log (const char *format, ...)
{
    int count;
    DWORD numwritten;
    char buffer[WRITE_LOG_BUF_SIZE];

    va_list parms;
    va_start (parms, format);
    count = _vsnprintf (buffer, WRITE_LOG_BUF_SIZE - 1, format, parms);
    if (SHOW_CONSOLE || console_logging) {
	openconsole ();
	WriteConsole (stdoutput, buffer, strlen(buffer), &numwritten, 0);
    }
    if (debugfile) {
	fprintf (debugfile, buffer);
	fflush (debugfile);
    }
    va_end (parms);
}

/****i* januae-win32/flush_log *********************************************
*
*   NAME
*       flush_log()
*
*   SYNOPSIS
*       void flush_log (void);
*
*   FUNCTION
*       Flush the configured debug output file.
*
*   NOTES
*       Does nothing when debugfile is null; does not check fflush() errors.
*
****************************************************************************
*
*/
void flush_log (void)
{
    if (debugfile)
	fflush (debugfile);
}

/****i* januae-win32/f_out *************************************************
*
*   NAME
*       f_out()
*
*   SYNOPSIS
*       void f_out (void *f, const char *format, ...);
*
*   FUNCTION
*       Route formatted output to the log or the console.
*
*   INPUTS
*       f - Zero to select write_log(); nonzero to select console output.
*       format - printf-style format.
*       ... - Format arguments.
*
*   NOTES
*       A null f forwards the already formatted buffer as a format string to
*       write_log(); a non-null f writes to the console. The null branch
*       omits va_end(), and formatting and output errors are unchecked.
*
****************************************************************************
*
*/
void f_out (void *f, const char *format, ...)
{
    int count;
    DWORD numwritten;
    char buffer[WRITE_LOG_BUF_SIZE];

    va_list parms;
    va_start (parms, format);
    count = _vsnprintf (buffer, WRITE_LOG_BUF_SIZE - 1, format, parms);
    if (f == 0)
	write_log (buffer);
    else {
	openconsole ();
	WriteConsole (stdoutput, buffer, strlen (buffer), &numwritten, 0);
	va_end (parms);
    }
}
