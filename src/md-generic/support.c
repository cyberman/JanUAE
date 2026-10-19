 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Miscellaneous machine dependent support functions and definitions
  *
  * Copyright 1996 Bernd Schmidt
  * Copyright 2004-2005 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "machdep/m68k.h"

/****i* januae-machine/machdep_init ****************************************
*
*   NAME
*       machdep_init()
*
*   SYNOPSIS
*       void machdep_init (void);
*
*   FUNCTION
*       Perform machine-dependent startup initialization.
*
*   NOTES
*       This backend performs no initialization.
*
****************************************************************************
*
*/
void machdep_init (void)
{
}

/****i* januae-machine/machdep_save_options ********************************
*
*   NAME
*       machdep_save_options()
*
*   SYNOPSIS
*       void machdep_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Save machine-dependent configuration options.
*
*   INPUTS
*       f - Configuration output stream.
*       p - Emulator preferences.
*
*   NOTES
*       This backend writes no options.
*
****************************************************************************
*
*/
void machdep_save_options (FILE *f, const struct uae_prefs *p)
{
}

/****i* januae-machine/machdep_parse_option ********************************
*
*   NAME
*       machdep_parse_option()
*
*   SYNOPSIS
*       int machdep_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Handle a machine-dependent configuration entry.
*
*   INPUTS
*       p - Emulator preferences.
*       option - Option key.
*       value - Option value.
*
*   RESULT
*       Always zero; no option is recognized.
*
****************************************************************************
*
*/
int machdep_parse_option (struct uae_prefs *p, const char *option, const char *value)
{
    return 0;
}

/****i* januae-machine/machdep_default_options *****************************
*
*   NAME
*       machdep_default_options()
*
*   SYNOPSIS
*       void machdep_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Initialize machine-dependent default preferences.
*
*   INPUTS
*       p - Preferences to initialize.
*
*   NOTES
*       This backend makes no changes.
*
****************************************************************************
*
*/
void machdep_default_options (struct uae_prefs *p)
{
}
