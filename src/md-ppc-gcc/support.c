 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Miscellaneous machine dependent support functions and definitions
  *
  * Copyright 1996 Bernd Schmidt
  * Copyright 2003-2005 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "sleep.h"
#include "machdep/rpt.h"
#include "machdep/m68k.h"

#ifndef HAVE_SYNC
# define sync()
#endif

frame_time_t timebase;

#ifdef __linux__
/****i* januae-machine/find_timebase ***************************************
*
*   NAME
*       find_timebase()
*
*   SYNOPSIS
*       static uae_u32 find_timebase (const char *path);
*
*   FUNCTION
*       Read a PowerPC timebase-frequency property from an Open Firmware
*       device-tree directory.
*
*   INPUTS
*       path - Directory containing a PowerPC device node.
*
*   RESULT
*       Raw frequency value read from timebase-frequency, or zero when not
*       found.
*
*   NOTES
*       Selects the first entry beginning with PowerPC,. The read count and
*       byte ordering are not validated.
*
****************************************************************************
*
*/
static uae_u32 find_timebase (const char *path)
{
    const char ppc_device_node[] = "PowerPC,";
    uae_u32 tbc_freq = 0;
    int found = 0;

    DIR *dir = opendir (path);
    if (dir) {
	struct dirent *dirent;

	do {
	    dirent = readdir (dir);
	    if (dirent) {
		found = !strncmp (dirent->d_name, ppc_device_node, strlen (ppc_device_node));
		if (found)
		    break;
	    }
        }  while (dirent);

	if (found) {
	    /* We've found the PowerPC node in the OF tree, now construct
	     * a path to the timebase frequency node so we can read it */
	    const char  tbc_freq_node[] = "/timebase-frequency";
	    char       *tbc_freq_path;

	    if ((tbc_freq_path = (char *) malloc (strlen (path) + strlen (dirent->d_name) +
						  strlen (tbc_freq_node) + 1))) {
		int tbcfd;

		strcpy (tbc_freq_path, path);
		strcat (tbc_freq_path, dirent->d_name);
		strcat (tbc_freq_path, tbc_freq_node);

		tbcfd = open (tbc_freq_path, O_RDONLY);

		if (tbcfd >= 0)  {
		   read (tbcfd, &tbc_freq, sizeof (uae_u32));
		   close (tbcfd);
		}

		free (tbc_freq_path);
	     }
	}
	closedir (dir);
    }
    return tbc_freq;
}

/****i* januae-machine/machdep_linux_gettimebase ***************************
*
*   NAME
*       machdep_linux_gettimebase()
*
*   SYNOPSIS
*       static frame_time_t machdep_linux_gettimebase (void);
*
*   FUNCTION
*       Look up the PowerPC timebase frequency in the Linux Open Firmware
*       tree.
*
*   RESULT
*       Frequency from the first matching device-tree lookup, or zero.
*
*   NOTES
*       Tries /proc/device-tree/ and then /proc/device-tree/cpus/.
*
****************************************************************************
*
*/
static frame_time_t machdep_linux_gettimebase (void)
{
    uae_u32 tbc_freq = 0;

    tbc_freq     = find_timebase ("/proc/device-tree/");
    if (!tbc_freq)
	tbc_freq = find_timebase ("/proc/device-tree/cpus/");

    return tbc_freq;
}
#endif

#ifdef __MACH__
# include <mach/mach_time.h>

/****i* januae-machine/machdep_mach_gettimebase ****************************
*
*   NAME
*       machdep_mach_gettimebase()
*
*   SYNOPSIS
*       static frame_time_t machdep_mach_gettimebase (void);
*
*   FUNCTION
*       Convert the Mach timebase ratio to counter ticks per second.
*
*   RESULT
*       One billion times denom divided by numer.
*
*   NOTES
*       Available only with __MACH__; the Mach API return status is not
*       checked.
*
****************************************************************************
*
*/
static frame_time_t machdep_mach_gettimebase (void)
{
    struct mach_timebase_info tbinfo;

    mach_timebase_info(&tbinfo);

    /* time (in nanosecs) = ticks * tbinfo.numer / tbinfo.numer
     *
     * Thus we get the number of ticks per second as follows:
     */
    return (frame_time_t)(1e9 * tbinfo.denom / tbinfo.numer);
}

#endif /* __MACH__ */


#ifdef __amigaos4__
# include <proto/timer.h>
# include <proto/expansion.h>
# include <expansion/expansion.h>

/****i* januae-machine/machdep_amigaos4_gettimebase ************************
*
*   NAME
*       machdep_amigaos4_gettimebase()
*
*   SYNOPSIS
*       static frame_time_t machdep_amigaos4_gettimebase (void);
*
*   FUNCTION
*       Obtain the AmigaONE E-clock frequency through AmigaOS4 timer
*       services.
*
*   RESULT
*       Clock frequency for MACHINETYPE_AMIGAONE; otherwise zero.
*
*   NOTES
*       Available only with __amigaos4__.
*
****************************************************************************
*
*/
static frame_time_t machdep_amigaos4_gettimebase (void)
{
    uae_u32 result = 0;
    uint32 machine = MACHINETYPE_UNKNOWN;

    GetMachineInfoTags(GMIT_Machine, &machine, TAG_DONE);

    if (machine == MACHINETYPE_AMIGAONE) {
	struct EClockVal etime;
	result = ReadEClock (&etime);
    }
    return result;
}

#endif /* __amigaos4__ */


#ifdef __MORPHOS__
# include <proto/timer.h>

/****i* januae-machine/machdep_morphos_gettimebase *************************
*
*   NAME
*       machdep_morphos_gettimebase()
*
*   SYNOPSIS
*       static frame_time_t machdep_morphos_gettimebase (void);
*
*   FUNCTION
*       Read the processor clock frequency through MorphOS timer services.
*
*   RESULT
*       Value returned by ReadCPUClock().
*
*   NOTES
*       Available only with __MORPHOS__.
*
****************************************************************************
*
*/
static frame_time_t machdep_morphos_gettimebase (void)
{
    UQUAD cputime;

    frame_time_t result = ReadCPUClock (&cputime);

    return result;
}
#endif /* __MORPHOS__ */


/****i* januae-machine/machdep_calibrate_timebase **************************
*
*   NAME
*       machdep_calibrate_timebase()
*
*   SYNOPSIS
*       static frame_time_t machdep_calibrate_timebase (void);
*
*   FUNCTION
*       Estimate counter ticks per second across five one-second sleeps.
*
*   RESULT
*       Elapsed counter ticks divided by five.
*
*   NOTES
*       Fallback for unavailable platform timebase sources; the measurement
*       is approximate.
*
****************************************************************************
*
*/
static frame_time_t machdep_calibrate_timebase (void)
{
    const int num_loops = 5;
    frame_time_t last_time;
    frame_time_t best_time;
    int i;

    write_log ("Calibrating timebase...\n");
    flush_log ();

    sync ();
    last_time = read_processor_time ();
    for (i = 0; i < num_loops; i++)
	uae_msleep (1000);
    best_time = read_processor_time () - last_time;

    return best_time / num_loops;
}

/****i* januae-machine/machdep_inithrtimer *********************************
*
*   NAME
*       machdep_inithrtimer()
*
*   SYNOPSIS
*       int machdep_inithrtimer (void);
*
*   FUNCTION
*       Initialize the PowerPC high-resolution timer frequency.
*
*   RESULT
*       Always one.
*
*   NOTES
*       Selects the platform-specific timebase source, falling back to a
*       five-second calibration when the result is zero. Subsequent calls
*       reuse the cached value.
*
****************************************************************************
*
*/
int machdep_inithrtimer (void)
{
    static int done = 0;

    if (!done) {
#	ifdef __MACH__
            timebase = machdep_mach_gettimebase ();
#	elif __amigaos4__
            timebase = machdep_amigaos4_gettimebase ();
#	elif __linux__
	    timebase = machdep_linux_gettimebase ();
#       elif __MORPHOS__
	    timebase = machdep_morphos_gettimebase ();
#endif

	if (!timebase)
	    timebase = machdep_calibrate_timebase ();

	write_log ("Timebase frequency: %.6f MHz\n", timebase / 1e6);

	done = 1;
    }
    return 1;
}

/****i* januae-machine/machdep_gethrtimebase *******************************
*
*   NAME
*       machdep_gethrtimebase()
*
*   SYNOPSIS
*       frame_time_t machdep_gethrtimebase (void);
*
*   FUNCTION
*       Return the cached PowerPC timebase frequency.
*
*   RESULT
*       Current timebase value.
*
*   NOTES
*       Returns the current global timebase, including zero before
*       initialization.
*
****************************************************************************
*
*/
frame_time_t machdep_gethrtimebase (void)
{
    return timebase;
}

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
*       No initialization is performed.
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
*       Write the PowerPC timebase preference to the configuration file.
*
*   INPUTS
*       f - Configuration output stream.
*       p - Emulator preferences.
*
*   NOTES
*       Writes the MACHDEP_NAME-prefixed use_tbc option.
*
****************************************************************************
*
*/
void machdep_save_options (FILE *f, const struct uae_prefs *p)
{
    cfgfile_write (f, MACHDEP_NAME ".use_tbc=%s\n", p->use_processor_clock ? "yes" : "no");
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
*       Parse the PowerPC timebase configuration option.
*
*   INPUTS
*       p - Preferences to update.
*       option - Configuration key.
*       value - Value to parse.
*
*   RESULT
*       Return value from cfgfile_yesno().
*
*   NOTES
*       Recognizes use_tbc through cfgfile_yesno().
*
****************************************************************************
*
*/
int machdep_parse_option (struct uae_prefs *p, const char *option, const char *value)
{
    return cfgfile_yesno (option, value, "use_tbc", &p->use_processor_clock);
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
*       Enable PowerPC timebase timing in default preferences.
*
*   INPUTS
*       p - Preferences to initialize.
*
*   NOTES
*       Sets use_processor_clock to 1.
*
****************************************************************************
*
*/
void machdep_default_options (struct uae_prefs *p)
{
    p->use_processor_clock = 1;
}
