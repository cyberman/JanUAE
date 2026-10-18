/************************************************************************ 
 *
 * Copyright 2004-2006 Richard Drummond
 * Copyright 2009      Oliver Brunner - aros<at>oliver-brunner.de
 *
 * This file is part of Janus-UAE.
 *
 * Janus-UAE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Janus-UAE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Janus-UAE. If not, see <http://www.gnu.org/licenses/>.
 *
 * $Id$
 *
 * Start-up and support functions for Amiga target
 *
 ************************************************************************/


#include "sysconfig.h"
#include "sysdeps.h"

#include <libraries/asl.h>
#include <libraries/mui.h>

#define  __USE_BASETYPE__
#include <proto/exec.h>
#undef   __USE_BASETYPE__
#include <exec/execbase.h>


#include "options.h"
#include "uae.h"
#include "xwin.h"
#include "debug.h"
#include "j.h"

#include "signal.h"


#ifdef HAVE_LIBRARIES_CYBERGRAPHICS_H
# define CGX_CGX_H <libraries/cybergraphics.h>
# define USE_CYBERGFX           /* define this to have cybergraphics support */
#else
# ifdef HAVE_CYBERGRAPHX_CYBERGRAPHICS_H
#  define USE_CYBERGFX
#  define CGX_CGX_H <cybergraphx/cybergraphics.h>
# endif
#endif
#ifdef USE_CYBERGFX
# if defined __MORPHOS__ || defined __AROS__ || defined __amigaos4__
#  define USE_CYBERGFX_V41
# endif
#endif

#ifdef USE_SDL
# include <SDL.h>
#endif

/* Get compiler/libc to enlarge stack to this size - if possible */
#if defined __PPC__ || defined __ppc__ || defined POWERPC || defined __POWERPC__
# define MIN_STACK_SIZE  (64 * 1024)
#else
# define MIN_STACK_SIZE  (32 * 1024)
#endif

#if defined __libnix__ || defined __ixemul__
/* libnix requires that we link against the swapstack.o module */
unsigned int __stack = MIN_STACK_SIZE;
#else
# if !defined __MORPHOS__ && !defined __AROS__
/* clib2 minimum stack size. Use this on OS3.x and OS4.0. */
unsigned int __stack_size = MIN_STACK_SIZE;
# endif
#endif

//struct IntuitionBase    *IntuitionBase = NULL;
//struct DosLibrary       *DOSBase = NULL;
//struct GfxBase          *GfxBase = NULL;
struct Library          *LayersBase   = NULL;
struct Library          *AslBase      = NULL;
struct Library          *CyberGfxBase = NULL;
struct Library          *GadToolsBase = NULL;
struct Library          *UtilityBase  = NULL;
struct Library          *MUIMasterBase= NULL;

struct AslIFace *IAsl;
struct GraphicsIFace *IGraphics;
struct LayersIFace *ILayers;
struct IntuitionIFace *IIntuition;
struct CyberGfxIFace *ICyberGfx;

struct Device *TimerBase;
#ifdef __amigaos4__
struct Library *ExpansionBase;
struct TimerIFace *ITimer;
struct ExpansionIFace *IExpansion;
#endif

extern int os39;

/* close all libraries */
/****i* januae-main/free_libs **********************************************
*
*   NAME
*       free_libs() -- Release selected host libraries.
*
*   SYNOPSIS
*       static void free_libs (void);
*
*   FUNCTION
*       Close the recorded ASL, GadTools, CyberGraphics, Graphics, Layers,
*       Intuition and DOS libraries when their bases are non-NULL. Release
*       the timer and expansion interfaces on AmigaOS 4.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Each closed library base is cleared. The implementation does not
*       close every library opened by init_libs().
*
****************************************************************************
*
*/

static void free_libs (void) {

  JWLOG("free_libs()\n");

#ifdef __amigaos4__
    if (ITimer)
	DropInterface ((struct Interface *)ITimer);
    if (IExpansion)
	DropInterface ((struct Interface *)IExpansion);
    if (ExpansionBase) {
	CloseLibrary (ExpansionBase);
	ExpansionBase=NULL;
    }
#endif

    if (AslBase) {
	CloseLibrary(AslBase);
	AslBase = NULL;
    }

    if (GadToolsBase) {
	CloseLibrary(GadToolsBase);
	GadToolsBase = NULL;
    }

    if (CyberGfxBase) {
	CloseLibrary(CyberGfxBase);
	CyberGfxBase = NULL;
    }

    if (GfxBase) {
	CloseLibrary ((struct Library *)GfxBase);
	GfxBase = NULL;
    }
    if (LayersBase) {
	CloseLibrary (LayersBase);
	LayersBase = NULL;
    }
    if (IntuitionBase) {
	CloseLibrary ((struct Library *)IntuitionBase);
	IntuitionBase = NULL;
    }
    if (DOSBase) {
	CloseLibrary ((struct Library *)DOSBase);
	IntuitionBase = NULL;
    }

  JWLOG("free_libs() done\n");
}

/**********************************************
 * init_libs
 *
 * Init all amiga libraries here.
 * Really do it here.
 * We have a lot of threads running, they
 * all depend on valid library pointers.
 *
 * Keep in mind: Prohibit gtk-mui
 * from opening its own libraries!
 ***********************************************/
/****i* januae-main/init_libs **********************************************
*
*   NAME
*       init_libs() -- Initialize native runtime dependencies.
*
*   SYNOPSIS
*       static int init_libs (void);
*
*   FUNCTION
*       Check the Exec version, open the core AmigaOS libraries required by
*       the emulator, obtain AmigaOS 4 interfaces where applicable, locate
*       timer.device and open the MUI master library.
*
*   INPUTS
*       None.
*
*   RESULT
*       1 after the required initialization path succeeds; 0 when a required
*       dependency or interface is unavailable.
*
*   NOTES
*       CyberGraphics initialization is conditional on USE_CYBERGFX. Opened
*       resources remain available to worker tasks.
*
****************************************************************************
*
*/

static int init_libs (void) {

  ENTER

  if (((struct ExecBase *)SysBase)->LibNode.lib_Version < 36) {
    write_log ("UAE needs OS 2.0+ !\n");
    return 0;
  }
  os39 = (((struct ExecBase *)SysBase)->LibNode.lib_Version >= 39);

  DOSBase = OpenLibrary ("dos.library", 36L);
  if (!DOSBase) {
    write_log ("No dos.library\n");
    return 0;
  }

  IntuitionBase = (void*) OpenLibrary ("intuition.library", 0L);
  if (!IntuitionBase) {
    write_log ("No intuition.library ?\n");
    return 0;
  } else {
#ifdef __amigaos4__
    IIntuition = (struct IntuitionIFace *) GetInterface ((struct Library *) IntuitionBase, "main", 1, NULL);
    if (!IIntuition) {
      CloseLibrary ((struct Library *) IntuitionBase);
      IntuitionBase = 0;
      return 0;
    }
#endif
  }

  GfxBase = (void*) OpenLibrary ("graphics.library", 0L);
  if (!GfxBase) {
    write_log ("No graphics.library ?\n");
    return 0;
  } else {
#ifdef __amigaos4__
    IGraphics = (struct GraphicsIFace *) GetInterface ((struct Library *) GfxBase, "main", 1, NULL);
    if (!IGraphics) {
      CloseLibrary ((struct Library *) GfxBase);
      GfxBase = 0;
	return 0;
      }
#endif
  }

  LayersBase = OpenLibrary ("layers.library", 0L);
  if (!LayersBase) {
    write_log ("No layers.library\n");
    return 0;
  } else {
#ifdef __amigaos4__
    ILayers = (struct LayersIFace *) GetInterface (LayersBase, "main", 1, NULL);
    if (!ILayers) {
      CloseLibrary (LayersBase);
      LayersBase = 0;
      return 0;
    }
#endif
  }

#ifdef USE_CYBERGFX
  if (!CyberGfxBase) {
    CyberGfxBase = OpenLibrary ("cybergraphics.library", 40);
#ifdef __amigaos4__
    if (CyberGfxBase) {
      ICyberGfx = (struct CyberGfxIFace *) GetInterface (CyberGfxBase, "main", 1, NULL);
      if (!ICyberGfx) {
	CloseLibrary (CyberGfxBase);
	CyberGfxBase = 0;
      }
    }
#endif
  }
#endif

  GadToolsBase = OpenLibrary( "gadtools.library", 37L );
  if(!GadToolsBase) {
    write_log ("gadtools.library missing? Needed for j-uae menu support!\n");
    return 0;
  }

  UtilityBase = OpenLibrary( "utility.library", 0L );
  if(!UtilityBase) {
    write_log ("utility.library missing? Needed for gtk-mui!\n");
    return 0;
  }



  TimerBase = (struct Device *) FindName(&SysBase->DeviceList, "timer.device");

#ifdef __amigaos4__
  ITimer = (struct TimerIFace *) GetInterface((struct Library *)TimerBase, "main", 1, 0);

  ExpansionBase = OpenLibrary ("expansion.library", 0);
  if (ExpansionBase) {
    IExpansion = (struct ExpansionIFace *) GetInterface(ExpansionBase, "main", 1, 0);
  }

  if(!ITimer || !IExpansion) {
    return 0;
  }
#endif

  if (!AslBase) {
    AslBase = OpenLibrary (AslName, 36);
    if (!AslBase) {
      write_log ("Can't open asl.library v36.\n");
      return 0;
    } else {
#ifdef __amigaos4__
      IAsl = (struct AslIFace *) GetInterface ((struct Library *)AslBase, "main", 1, NULL);
      if (!IAsl) {
	CloseLibrary (AslBase);
	AslBase = 0;
	write_log ("Can't get asl.library interface\n");
      }
#endif
    }
#ifdef __amigaos4__
  } else {
    IAsl->Obtain ();
#endif
  }

  MUIMasterBase = (struct Library *) OpenLibrary (MUIMASTER_NAME, 19L);
  if (!MUIMasterBase) {
    write_log ("No Zune? muimaster.library missing?\n");
    return 0;
  }


  LEAVE
  /* success */
  return 1;
}

static int fromWB;
static FILE *logfile;

/************************************************
 *
 * main()
 *
 ************************************************
 *
 * Amiga-specific main entry
 *
 * Open all libraries,
 * Call real_main,
 * Close all libraries again.
 ************************************************/
void stop_proxy_thread (void);

/****i* januae-main/main ***************************************************
*
*   NAME
*       main() -- Enter the Amiga-host emulator runtime.
*
*   SYNOPSIS
*       int main (int argc, char *argv[]);
*
*   FUNCTION
*       Select a log file for Workbench launches, initialize native
*       libraries, start the splash task and optional SDL host support, then
*       invoke real_main(). Release selected libraries on normal return.
*
*   INPUTS
*       argc - Argument count (zero for Workbench startup). argv - Program
*       arguments.
*
*   RESULT
*       0 on normal return from real_main().
*
*   NOTES
*       Failure to initialize required libraries calls exit(1).
*
****************************************************************************
*
*/

int main (int argc, char *argv[]) {

  fromWB = argc == 0;

  if (fromWB) {
    set_logfile ("T:J-UAE.log");
  }

  if(!init_libs ()) {
    free_libs();
    write_log("Unable to open required libraries. Sorry.\n");
    exit(1);
  }

  aros_splash_start_thread();

#ifdef USE_SDL
  init_sdl ();
#endif

  real_main (argc, argv);

  if (fromWB) {
    set_logfile (0);
  }

  free_libs();

  return 0;
}

/*
 * Handle CTRL-C signals
 */
/****i* januae-main/sigbrkhandler ******************************************
*
*   NAME
*       sigbrkhandler() -- Handle the host interrupt signal.
*
*   SYNOPSIS
*       static RETSIGTYPE sigbrkhandler(int foo);
*
*   FUNCTION
*       Activate the emulator debugger when DEBUGGER is enabled; otherwise
*       perform no action.
*
*   INPUTS
*       foo - Signal number; not examined.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static RETSIGTYPE sigbrkhandler(int foo)
{
#ifdef DEBUGGER
    activate_debugger ();
#endif
}

/****i* januae-main/setup_brkhandler ***************************************
*
*   NAME
*       setup_brkhandler() -- Register the host interrupt handler.
*
*   SYNOPSIS
*       void setup_brkhandler (void);
*
*   FUNCTION
*       Install sigbrkhandler() for SIGINT using sigaction with SA_RESTART
*       when available, otherwise use signal().
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void setup_brkhandler (void)
{
#ifdef HAVE_SIGACTION
    struct sigaction sa;
    sa.sa_handler = (void*)sigbrkhandler;
    sa.sa_flags = 0;
    sa.sa_flags = SA_RESTART;
    sigemptyset (&sa.sa_mask);
    sigaction (SIGINT, &sa, NULL);
#else
    signal (SIGINT,sigbrkhandler);
#endif
}


/*
 * Handle target-specific cfgfile options
 */
/****i* januae-main/target_save_options ************************************
*
*   NAME
*       target_save_options() -- Handle target-specific configuration
*       output.
*
*   SYNOPSIS
*       void target_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Return without writing any target-specific configuration entries.
*
*   INPUTS
*       f - Configuration output stream; unused. p - Current preferences;
*       unused.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void target_save_options (FILE *f, const struct uae_prefs *p)
{
}

/****i* januae-main/target_parse_option ************************************
*
*   NAME
*       target_parse_option() -- Recognize target-specific configuration
*       options.
*
*   SYNOPSIS
*       int target_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Decline every target-specific option without modifying preferences.
*
*   INPUTS
*       p - Preferences; unused. option - Option name; unused. value -
*       Option value; unused.
*
*   RESULT
*       0 for every call.
*
****************************************************************************
*
*/

int target_parse_option (struct uae_prefs *p, const char *option, const char *value)
{
    return 0;
}

/****i* januae-main/target_default_options *********************************
*
*   NAME
*       target_default_options() -- Initialize target-specific preference
*       defaults.
*
*   SYNOPSIS
*       void target_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Return without changing any target-specific preference fields.
*
*   INPUTS
*       p - Preferences structure; unused.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void target_default_options (struct uae_prefs *p)
{
}
