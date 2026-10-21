 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Wrapper for platform-specific sleep routine
  *
  * Copyright 2003-2005 Richard Drummond
  */

# ifdef TARGET_AMIGAOS
#  include <dos/dos.h>
#  include <proto/exec.h>
#  include <proto/dos.h>
#  include <clib/alib_protos.h>
# else
#  ifdef USE_SDL
#   include <SDL_timer.h>
#  endif
# endif

#define ONE_THOUSAND	1000
#define ONE_MILLION	(1000 * 1000)

/* The following would offer no advantage since we always call use_msleep() with
 * a constant and all arithmetic can be done at compile time. */

/*
 * void msleep (int ms)
 *
 * Sleep for ms milliseconds using an appropriate system-dependent sleep
 * functions.
 */
# if 0 //defined _WIN32
#  define uae_msleep(msecs) Sleep (msecs)
# else
#  if defined TARGET_AMIGAOS
#   if defined __amigaos4__ || defined __MORPHOS__ 
#    define uae_msleep(msecs) TimeDelay (0, msecs / ONE_THOUSAND, (msecs % ONE_THOUSAND) * ONE_THOUSAND)
#   else
#    define uae_msleep(msecs) Delay (msecs <= 20 ? 1 : msecs/20);
#   endif
#  else
#   ifdef HAVE_NANOSLEEP
#    define uae_msleep(msecs) \
	    { \
		if (msecs < 1000) { \
		    struct timespec t = { 0, (msecs) * ONE_MILLION }; \
		    nanosleep (&t, 0); \
		} else { \
		    int secs      = msecs / ONE_THOUSAND; \
		    int millisecs = msecs % ONE_THOUSAND; \
		    struct timespec t = { secs, millisecs * ONE_MILLION }; \
		    nanosleep (&t, 0); \
		} \
	    }
#   else
#    ifdef HAVE_USLEEP
#     define uae_msleep(msecs) usleep (msecs * ONE_THOUSAND)
#    else
#     ifdef USE_SDL
#      define uae_msleep(msecs) SDL_Delay (msecs)
#     else
#      error "No system sleep function found"
#     endif
#    endif
#   endif
#  endif
# endif

void sleep_test (void);
