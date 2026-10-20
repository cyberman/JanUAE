/*
 * UAE - The Un*x Amiga Emulator
 *
 * Copyright 2004 Richard Drummond
 * 
 * Start-up and support functions used by Linux/Unix targets
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "debug.h"

/*
 * Handle break signal
 */
#include <signal.h>

/****i* januae-od-generic/sigbrkhandler ***********************************
*
*   NAME
*       sigbrkhandler()
*
*   SYNOPSIS
*       static RETSIGTYPE sigbrkhandler (int foo);
*
*   FUNCTION
*       Handle SIGINT by activating the emulator debugger when debugger
*       support is enabled.
*
*   INPUTS
*       foo - Unused signal number in the C signature.
*
*   NOTES
*       The C++ signature uses an ellipsis. On non-Unix or NeXT targets the
*       handler reinstalls itself before returning.
*
*************************************************************************
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

/****i* januae-od-generic/setup_brkhandler ********************************
*
*   NAME
*       setup_brkhandler()
*
*   SYNOPSIS
*       void setup_brkhandler (void);
*
*   FUNCTION
*       Install the SIGINT handler. Use sigaction() on Unix except NeXT, and
*       signal() on other targets.
*
*   NOTES
*       The Unix path requests SA_RESTART when available. Handler
*       installation failures are not reported.
*
*************************************************************************
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

/****i* januae-od-generic/target_save_options *****************************
*
*   NAME
*       target_save_options()
*
*   SYNOPSIS
*       void target_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Leave the target-specific configuration output unchanged.
*
*   INPUTS
*       f - Unused destination stream.
*       p - Unused preference set.
*
*************************************************************************
*
*/
void target_save_options (FILE *f, const struct uae_prefs *p)
{
}

/****i* januae-od-generic/target_parse_option *****************************
*
*   NAME
*       target_parse_option()
*
*   SYNOPSIS
*       int target_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Decline all target-specific configuration options.
*
*   INPUTS
*       p - Unused preference set.
*       option - Unused option name.
*       value - Unused option value.
*
*   RESULT
*       Always 0, indicating that no target-specific option was handled.
*
*************************************************************************
*
*/
int target_parse_option (struct uae_prefs *p, const char *option, const char *value)
{
    return 0;
}

/****i* januae-od-generic/target_default_options **************************
*
*   NAME
*       target_default_options()
*
*   SYNOPSIS
*       void target_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Leave the supplied preferences unchanged.
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
