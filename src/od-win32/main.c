/*
 * UAE - The Un*x Amiga Emulator
 *
 * Start-up and support functions used by Win32
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include <windows.h>

#include "uae.h"
#include "options.h"
#include "debug.h"

#include <SDL_main.h>

#include <signal.h>

/****i* januae-win32/sigbrkhandler *****************************************
*
*   NAME
*       sigbrkhandler()
*
*   SYNOPSIS
*       static RETSIGTYPE sigbrkhandler (int foo);
*
*   FUNCTION
*       Respond to a SIGINT break request.
*
*   INPUTS
*       foo - Received signal number in the C variant.
*
*   NOTES
*       Activates the debugger only in DEBUGGER builds. Reinstalls the
*       handler on the non-Unix branch. The C++ variant has a variadic
*       signature.
*
****************************************************************************
*
*/
#ifdef __cplusplus
static RETSIGTYPE sigbrkhandler(...)
#else
static RETSIGTYPE sigbrkhandler (int foo)
#endif
{
#ifdef DEBUGGER
    activate_debugger ();
#endif

#if !defined(__unix) || defined(__NeXT__)
    signal (SIGINT, sigbrkhandler);
#endif
}

/****i* januae-win32/setup_brkhandler **************************************
*
*   NAME
*       setup_brkhandler()
*
*   SYNOPSIS
*       void setup_brkhandler (void);
*
*   FUNCTION
*       Register the emulator SIGINT handler.
*
*   NOTES
*       Uses sigaction on Unix targets other than NeXT; otherwise installs a
*       signal handler with signal(). Registration errors are not reported.
*
****************************************************************************
*
*/
void setup_brkhandler (void)
{
#if defined(__unix) && !defined(__NeXT__)
    struct sigaction sa;
    sa.sa_handler = sigbrkhandler;
    sa.sa_flags = 0;
#ifdef SA_RESTART
    sa.sa_flags = SA_RESTART;
#endif
    sigemptyset (&sa.sa_mask);
    sigaction (SIGINT, &sa, NULL);
#else
    signal (SIGINT, sigbrkhandler);
#endif
}

HINSTANCE hInst;
char *start_path;
char start_path_data[MAX_DPATH];

int os_winnt, os_winnt_admin;

static OSVERSIONINFO osVersion;
static SYSTEM_INFO SystemInfo;

/****i* januae-win32/isadminpriv *******************************************
*
*   NAME
*       isadminpriv()
*
*   SYNOPSIS
*       static int isadminpriv (void);
*
*   FUNCTION
*       Check whether the process token belongs to the built-in
*       Administrators group.
*
*   RESULT
*       Nonzero if a matching group SID is found; zero otherwise.
*
*   NOTES
*       Queries Windows token groups and compares their SIDs. Several early
*       failure paths return without closing the token or releasing
*       allocated storage.
*
****************************************************************************
*
*/
static int isadminpriv (void)
{
    DWORD i, dwSize = 0, dwResult = 0;
    HANDLE hToken;
    PTOKEN_GROUPS pGroupInfo;
    BYTE sidBuffer[100];
    PSID pSID = (PSID)&sidBuffer;
    SID_IDENTIFIER_AUTHORITY SIDAuth = {SECURITY_NT_AUTHORITY};
    int isadmin = 0;

    // Open a handle to the access token for the calling process.
    if (!OpenProcessToken (GetCurrentProcess (), TOKEN_QUERY, &hToken)) {
	write_log ("OpenProcessToken Error %u\n", GetLastError());
	return FALSE;
    }

    // Call GetTokenInformation to get the buffer size.
    if (!GetTokenInformation(hToken, TokenGroups, NULL, dwSize, &dwSize)) {
	dwResult = GetLastError();
	if (dwResult != ERROR_INSUFFICIENT_BUFFER) {
	    write_log ("GetTokenInformation Error %u\n", dwResult);
	    return FALSE;
	}
    }

    // Allocate the buffer.
    pGroupInfo = (PTOKEN_GROUPS) GlobalAlloc (GPTR, dwSize);

    // Call GetTokenInformation again to get the group information.
    if (!GetTokenInformation (hToken, TokenGroups, pGroupInfo, dwSize, &dwSize)) {
	write_log ("GetTokenInformation Error %u\n", GetLastError ());
	return FALSE;
    }

    // Create a SID for the BUILTIN\Administrators group.
    if (!AllocateAndInitializeSid (&SIDAuth, 2,
				    SECURITY_BUILTIN_DOMAIN_RID,
				    DOMAIN_ALIAS_RID_ADMINS,
				    0, 0, 0, 0, 0, 0,
				    &pSID)) {
	write_log ("AllocateAndInitializeSid Error %u\n", GetLastError ());
	return FALSE;
    }

    // Loop through the group SIDs looking for the administrator SID.
    for (i = 0; i < pGroupInfo->GroupCount; i++) {
	if (EqualSid (pSID, pGroupInfo->Groups[i].Sid))
	    isadmin = 1;
    }

    if (pSID)
	FreeSid (pSID);
    if (pGroupInfo)
	GlobalFree (pGroupInfo);

    return isadmin;
}

typedef void (CALLBACK* PGETNATIVESYSTEMINFO)(LPSYSTEM_INFO);
static PGETNATIVESYSTEMINFO pGetNativeSystemInfo;

/****i* januae-win32/osdetect **********************************************
*
*   NAME
*       osdetect()
*
*   SYNOPSIS
*       static int osdetect (void);
*
*   FUNCTION
*       Read Windows version and processor information and set the global OS
*       flags.
*
*   RESULT
*       Zero for an NT platform with a major version of four or earlier;
*       otherwise one.
*
*   NOTES
*       Updates os_winnt and os_winnt_admin. Uses GetNativeSystemInfo when
*       available; otherwise GetSystemInfo. GetVersionEx failure is not
*       reported as an error.
*
****************************************************************************
*
*/
static int osdetect (void)
{
    os_winnt = 0;
    os_winnt_admin = 0;

    pGetNativeSystemInfo = (PGETNATIVESYSTEMINFO)GetProcAddress (
	GetModuleHandle ("kernel32.dll"), "GetNativeSystemInfo");
    GetSystemInfo (&SystemInfo);
    if (pGetNativeSystemInfo)
	pGetNativeSystemInfo (&SystemInfo);
    osVersion.dwOSVersionInfoSize = sizeof (OSVERSIONINFO);
    if (GetVersionEx (&osVersion)) {
	if ((osVersion.dwPlatformId == VER_PLATFORM_WIN32_NT) &&
	    (osVersion.dwMajorVersion <= 4)) {
	    /* WinUAE not supported on this version of Windows... */
//	    char szWrongOSVersion[MAX_DPATH];
//	    WIN32GUI_LoadUIString(IDS_WRONGOSVERSION, szWrongOSVersion, MAX_DPATH);
//	    pre_gui_message(szWrongOSVersion);
	    return FALSE;
	}
	if (osVersion.dwPlatformId == VER_PLATFORM_WIN32_NT)
	    os_winnt = 1;
    }

    if (!os_winnt) {
	return 1;
    }
    os_winnt_admin = isadminpriv ();
    return 1;
}

//int _stdcall WinMain (HINSTANCE hInstance, HINSTANCE prev, LPSTR cmd, int show)
/****i* januae-win32/main **************************************************
*
*   NAME
*       main()
*
*   SYNOPSIS
*       int main (int argc, char **argv);
*
*   FUNCTION
*       Start the Win32 emulator entry point.
*
*   INPUTS
*       argc - Command-line argument count.
*       argv - Command-line argument vector.
*
*   RESULT
*       Always zero when the emulator entry point returns.
*
*   NOTES
*       Allocates start_path and obtains the executable directory, then
*       initializes SDL, runs real_main(), and frees start_path. Allocation
*       and module-path retrieval failures are not handled.
*
****************************************************************************
*
*/
int main (int argc, char **argv)
{
//    char **argv,
    char *posn;
//    int argc;

//    hInst = hInstance;
//    argc = __argc; argv = __argv;
    start_path = xmalloc( MAX_DPATH );
    GetModuleFileName( NULL, start_path, MAX_DPATH );
    if ((posn = strrchr( start_path, '\\')))
	*posn = 0;

    init_sdl ();

    real_main (argc, argv);

    free (start_path);
    return 0;
}

extern FILE *debugfile;

/****i* januae-win32/logging_init ******************************************
*
*   NAME
*       logging_init()
*
*   SYNOPSIS
*       void logging_init (void);
*
*   FUNCTION
*       Initialize or reopen the Win32 emulator log stream.
*
*   NOTES
*       When SINGLEFILE is disabled, the first call opens a configured log
*       or fallback boot log; the second call reopens only a configured log.
*       Later calls write a restart marker. File-open failures are not
*       reported.
*
****************************************************************************
*
*/
void logging_init (void)
{
    static int first;
    char debugfilename[MAX_DPATH];

    if (first > 1) {
	write_log ("** RESTART **\n");
	return;
    }
    if (first == 1) {
	if (debugfile)
	    fclose (debugfile);
        debugfile = 0;
    }
#ifndef SINGLEFILE
    if (currprefs.win32_logfile) {
	sprintf (debugfilename, "%swinuaelog.txt", start_path_data);
	if (!debugfile)
	    debugfile = fopen (debugfilename, "wt");
    } else if (!first) {
	sprintf (debugfilename, "%swinuaebootlog.txt", start_path_data);
	if (!debugfile)
	    debugfile = fopen (debugfilename, "wt");
    }
#endif
    first++;

//    write_log ("%s (%s %d.%d %s%s%s)", VersionStr, os_winnt ? "NT" : "W9X/ME",
//	osVersion.dwMajorVersion, osVersion.dwMinorVersion, osVersion.szCSDVersion,
//	strlen(osVersion.szCSDVersion) > 0 ? " " : "", os_winnt_admin ? "Admin" : "");
//    write_log (" %s %X.%X %d",
//	SystemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "32-bit x86" :
//	SystemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64 ? "IA64" :
//	SystemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "AMD64" : "Unknown",
//	SystemInfo.wProcessorLevel, SystemInfo.wProcessorRevision,
//	SystemInfo.dwNumberOfProcessors);
    write_log ("\n(c) 1995-2001 Bernd Schmidt   - Core UAE concept and implementation."
	       "\n(c) 1998-2005 Toni Wilen      - Win32 port, core code updates."
	       "\n(c) 1996-2001 Brian King      - Win32 port, Picasso96 RTG, and GUI."
	       "\n(c) 1996-1999 Mathias Ortmann - Win32 port and bsdsocket support."
	       "\n(c) 2000-2001 Bernd Meyer     - JIT engine."
	       "\n(c) 2000-2005 Bernd Roesch    - MIDI input, many fixes."
	       "\nPress F12 to show the Settings Dialog (GUI), Alt-F4 to quit."
	       "\nEnd+F1 changes floppy 0, End+F2 changes floppy 1, etc."
	       "\n");
//    write_log ("EXE: '%s'\nDATA: '%s'\n", start_path_exe, start_path_data);
}

/****i* januae-win32/filesys_init ******************************************
*
*   NAME
*       filesys_init()
*
*   SYNOPSIS
*       void filesys_init (void);
*
*   FUNCTION
*       Provide a no-op filesystem initialization entry point.
*
*   NOTES
*       This Win32 implementation performs no initialization.
*
****************************************************************************
*
*/
void filesys_init (void)
{
}

/****i* januae-win32/target_save_options ***********************************
*
*   NAME
*       target_save_options()
*
*   SYNOPSIS
*       void target_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Provide the Win32 target configuration writer.
*
*   INPUTS
*       f - Configuration output stream.
*       p - Emulator preferences.
*
*   NOTES
*       No target-specific options are written.
*
****************************************************************************
*
*/
void target_save_options (FILE *f, const struct uae_prefs *p)
{
}

/****i* januae-win32/target_parse_option ***********************************
*
*   NAME
*       target_parse_option()
*
*   SYNOPSIS
*       int target_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Handle an unrecognized Win32 target option.
*
*   INPUTS
*       p - Preferences object.
*       option - Option name.
*       value - Option value.
*
*   RESULT
*       Always zero.
*
*   NOTES
*       This backend does not recognize any target-specific options.
*
****************************************************************************
*
*/
int target_parse_option (struct uae_prefs *p, const char *option, const char *value)
{
    return 0;
}

/****i* januae-win32/target_default_options ********************************
*
*   NAME
*       target_default_options()
*
*   SYNOPSIS
*       void target_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Provide target-specific default initialization.
*
*   INPUTS
*       p - Preferences object.
*
*   NOTES
*       Does not change the preferences.
*
****************************************************************************
*
*/
void target_default_options (struct uae_prefs *p)
{
}
