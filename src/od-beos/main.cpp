/*
 * UAE - The Un*x Amiga Emulator
 *
 * Copyright 2004 Richard Drummond
 *
 * Start-up and support functions for BeOS target
 */

extern "C" {
#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "xwin.h"
#include "debug.h"

#include "signal.h"

#ifdef USE_SDL
# include <SDL.h>
#endif
}

#ifndef USE_SDL
# include "gfxdep/be-UAE.h"
#endif


#ifndef USE_SDL
int  argcCopy;
char **argvCopy;

void freeCopiedArgs();
void copyArgs(int argc,char **argv);

/****i* januae-od-beos/freeCopiedArgs *************************************
*
*   NAME
*       freeCopiedArgs()
*
*   SYNOPSIS
*       void freeCopiedArgs();
*
*   FUNCTION
*       Free all copied command-line strings and their pointer array.
*
*   NOTES
*       Uses argcCopy and argvCopy populated by copyArgs(). Available
*       without USE_SDL; the pointers are not reset.
*
*************************************************************************
*
*/
void freeCopiedArgs()
{
	for(int i = 0;i < argcCopy;i++)
		free(argvCopy[i]);

	delete[] argvCopy;
}

/****i* januae-od-beos/copyArgs *******************************************
*
*   NAME
*       copyArgs()
*
*   SYNOPSIS
*       void copyArgs (int argc,char **argv);
*
*   FUNCTION
*       Duplicate the argument vector into argvCopy and terminate the vector
*       with NULL.
*
*   INPUTS
*       argc - Number of command-line arguments.
*       argv - Source argument vector.
*
*   NOTES
*       Allocates the vector with new[] and copies each string using
*       strdup(). Ownership remains with this backend until
*       freeCopiedArgs(). Only compiled without USE_SDL.
*
*************************************************************************
*
*/
void copyArgs (int argc,char **argv)
{
	argcCopy = argc;
	argvCopy = new char *[argcCopy+1];

	for(int i = 0;i < argc;i++)
		argvCopy[i] = strdup(argv[i]);

	argvCopy[argc] = NULL;
}
#endif

/****i* januae-od-beos/main ***********************************************
*
*   NAME
*       main()
*
*   SYNOPSIS
*       int main (int argc, char *argv[]);
*
*   FUNCTION
*       Start the BeOS emulator. The non-SDL path constructs and runs a UAE
*       application and restores the display workspace; the SDL path
*       delegates to real_main().
*
*   INPUTS
*       argc - Command-line argument count.
*       argv - Command-line argument vector.
*
*   RESULT
*       Always 0 after the chosen startup path returns.
*
*   NOTES
*       The non-SDL path copies and later releases its argument strings.
*
*************************************************************************
*
*/
int main (int argc, char *argv[])
{
#ifndef USE_SDL
/*
 * If we're not using SDL for anything, we would initialize our
 * application object here. On the other hand, if you are using SDL,
 * we can't because SDL creates its own application object.
 */
	// Copy command line arguments
	copyArgs(argc,argv);

    UAE uae;
    uae.Run();

	freeCopiedArgs();

	// restore original workspace resolution (if it has changed)
	restoreWorkspaceResolution();
#else
    real_main(argc, argv);
#endif
    return 0;
}

/****i* januae-od-beos/sigbrkhandler **************************************
*
*   NAME
*       sigbrkhandler()
*
*   SYNOPSIS
*       static void sigbrkhandler (int foo);
*
*   FUNCTION
*       Activate the debugger on SIGINT when debugger support is compiled
*       in.
*
*   INPUTS
*       foo - Unused signal number.
*
*************************************************************************
*
*/
static void sigbrkhandler (int foo)
{
#ifdef DEBUGGER
    activate_debugger ();
#endif
}

/****i* januae-od-beos/setup_brkhandler ***********************************
*
*   NAME
*       setup_brkhandler()
*
*   SYNOPSIS
*       void setup_brkhandler (void);
*
*   FUNCTION
*       Install sigbrkhandler() as the BeOS SIGINT handler with sigaction().
*
*   NOTES
*       Uses an empty signal mask and zero flags. The sigaction() result is
*       not checked.
*
*************************************************************************
*
*/
void setup_brkhandler (void)
{
    struct sigaction sa;
    sa.sa_handler = sigbrkhandler;
    sa.sa_flags = 0;
    sigemptyset (&sa.sa_mask);
    sigaction (SIGINT, &sa, NULL);
}

/****i* januae-od-beos/target_save_options ********************************
*
*   NAME
*       target_save_options()
*
*   SYNOPSIS
*       void target_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Write no BeOS-specific preferences.
*
*   INPUTS
*       f - Unused output stream.
*       p - Unused preference set.
*
*************************************************************************
*
*/
void target_save_options (FILE *f, const struct uae_prefs *p)
{
}

/****i* januae-od-beos/target_parse_option ********************************
*
*   NAME
*       target_parse_option()
*
*   SYNOPSIS
*       int target_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Decline every BeOS-specific configuration option.
*
*   INPUTS
*       p - Unused preference set.
*       option - Unused option name.
*       value - Unused option value.
*
*   RESULT
*       Always 0.
*
*************************************************************************
*
*/
int target_parse_option (struct uae_prefs *p, const char *option, const char *value)
{
    return 0;
}

/****i* januae-od-beos/target_default_options *****************************
*
*   NAME
*       target_default_options()
*
*   SYNOPSIS
*       void target_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Leave the BeOS preference defaults unchanged.
*
*   INPUTS
*       p - Unused preference set.
*
*************************************************************************
*
*/
void target_default_options (struct uae_prefs *p)
{
}
