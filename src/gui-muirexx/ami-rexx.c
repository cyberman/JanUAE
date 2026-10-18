 /*
  * UAE - The Un*x Amiga Emulator
  *
  * ami-rexx.c
  *
  * Copyright 1996 Samuel Devulder.
  * Copyright 2004-2006 Richard Drummond
  *
  * History:
  *     05/09/97: Added UAEEXE support.
  *	04/03/98: Added event-hack for SOUND(). Added do_inhibit_frame().
  *	03/05/98: Modified currprefs to changed_prefs where necessary.
  */

/****************************************************************************/

#if defined(POWERUP)
# include <exec/exec.h> /* sam: I've added this or else I have
			 * warnings about missing struct def. */
#ifndef USE_CLIB
#  include <powerup/ppcproto/exec.h>
#  include <powerup/ppcproto/rexxsyslib.h>
#  include <powerup/ppcproto/dos.h>
# else
#  include <clib/exec_protos.h>
#  include <clib/alib_protos.h>
# endif
#else
# include <proto/exec.h>
# include <proto/dos.h>
# include <proto/rexxsyslib.h>
# include <proto/dos.h>
# ifndef __SASC
#  include <clib/alib_protos.h>
# endif
#endif

#include <rexx/storage.h>
#include <rexx/errors.h>

/* this prevent a conflict between <rexx/rexxio.h> and <sys/dirent.h> */
#undef DT_DIR

/****************************************************************************/

#include "sysconfig.h"
#include "sysdeps.h"
#include "uae.h"
#include "options.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "disk.h"
#include "audio.h"
#include "uaeexe.h"
#include "xwin.h"
#include "drawing.h"
#include "debug.h"
#include "version.h"

#include <ctype.h>

/****************************************************************************/

#define UAE_PORTNAME    "UAE"
#define RESULT_LEN      1024
#define PORT_LEN        80
#define CMD_LEN         1024

typedef struct {
    char port[PORT_LEN];
    char cmd_on[CMD_LEN];
    char cmd_off[CMD_LEN];
} gui_rexx_s;

enum {
    LED_POW, LED_DF0, LED_DF1, LED_DF2, LED_DF3,
    NAME_DF0, NAME_DF1, NAME_DF2, NAME_DF3,
    ON_EXIT,
    GUI_REXX_MAX
};

/****************************************************************************/

int  rexx_init (void);
void rexx_exit (void);
void rexx_led (int led, int on);
void rexx_filename (unsigned int num, const char *name);
void rexx_handle_events (void);

/****************************************************************************/

#if defined __GNUC__ && !defined __amigaos4__ && !defined __morphos__
typedef struct RxsLib REXXBASE;
#else
typedef struct Library REXXBASE;
#endif

REXXBASE		*RexxSysBase = NULL;

#ifdef __amigaos4__
struct RexxSysIFace	*IRexxSys;
#endif

static struct MsgPort   *ARexxPort;
static gui_rexx_s        gui_rexx[GUI_REXX_MAX];
static char              RESULT[RESULT_LEN];
static int               led_state[5];

static int   ADDRESS    (const char *hostname, const char *cmd);
static int   matchstr   (const char **line, const char *pat);
static void  extractstr (const char **line, char *result, unsigned int len);
static int   matchnum   (const char **line);

/****************************************************************************/

extern ULONG frame_num;                                  /* ami-win.c */

/****************************************************************************/

/****i* januae-ami-rexx/rexx_init ******************************************
*
*   NAME
*       rexx_init() -- Install the emulator ARexx command port.
*
*   SYNOPSIS
*       int rexx_init (void);
*
*   FUNCTION
*       Open rexxsyslib.library, acquire an OS4 interface when required,
*       reject an existing UAE port, create the named command port and
*       service initially queued commands.
*
*   INPUTS
*       None.
*
*   RESULT
*       1 when the UAE command port is created; otherwise 0.
*
****************************************************************************
*
*/

int rexx_init (void)
{
    RexxSysBase = (REXXBASE *) OpenLibrary ("rexxsyslib.library", 0L);
    if (!RexxSysBase) {
	write_log ("Can't find rexxsyslib.library!\n");
	return 0;
    }
#ifdef __amigaos4__
    IRexxSys = (struct RexxSysIFace *) GetInterface (RexxSysBase, "main", 1, NULL);
    if (!IRexxSys) {
	CloseLibrary (RexxSysBase);
	RexxSysBase = 0;
	return 0;
    }
#endif

    if (FindPort(UAE_PORTNAME)) {
	write_log ("Port \"%s\" already exists!\n", UAE_PORTNAME);
	return 0;
    }
    ARexxPort = CreatePort (UAE_PORTNAME, 0);
    if (!ARexxPort) {
	write_log ("Failed to open AREXX port \"%s\"!\n", UAE_PORTNAME);
	return 0;
    }
    write_log ("Rexx port \"%s\" installed.\n", UAE_PORTNAME);
    rexx_handle_events ();
    return 1;
}

/****************************************************************************/

/****i* januae-ami-rexx/rexx_exit ******************************************
*
*   NAME
*       rexx_exit() -- Shut down the emulator ARexx interface.
*
*   SYNOPSIS
*       void rexx_exit (void);
*
*   FUNCTION
*       Optionally send the configured ON_EXIT command, reject and reply to
*       queued messages, remove the command port, and release ARexx library
*       resources.
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

void rexx_exit (void)
{
    if (ARexxPort) {
	struct RexxMsg *msg;
	gui_rexx_s *gui = &gui_rexx[ON_EXIT];

	if (gui->port[0] && gui->cmd_on[0]) {
	    if (ADDRESS(gui->port, gui->cmd_on) != RC_OK) {
		write_log ("%s:%s:%s\n", gui->port,
					 gui->cmd_on,
					 RESULT);
	    }
	    gui->port[0] = '\0';
	}
	Forbid ();
	while ((msg = (struct RexxMsg*)GetMsg (ARexxPort))) {
	   msg->rm_Result1 = RC_ERROR;
	   msg->rm_Result2 = 0;
	   ReplyMsg ((void*)msg);
	}
	DeletePort (ARexxPort);
	Permit ();
	ARexxPort = NULL;
    }
#ifdef __amigaos4__
    if (IRexxSys) {
	DropInterface ((struct Interface *)IRexxSys);
	IRexxSys = NULL;
    }
#endif
    if (RexxSysBase) {
	CloseLibrary ((void*)RexxSysBase);
	RexxSysBase = NULL;
    }
}

/****************************************************************************/

/****i* januae-ami-rexx/EJECT **********************************************
*
*   NAME
*       EJECT() -- Process an ARexx floppy-eject command.
*
*   SYNOPSIS
*       static int EJECT (const char *line);
*
*   FUNCTION
*       Parse a drive number, eject media from an enabled drive, and place
*       the outcome in the shared response buffer.
*
*   INPUTS
*       line - Command arguments containing the drive number.
*
*   RESULT
*       RC_OK for an enabled drive from 0 through 3; otherwise RC_WARN.
*
****************************************************************************
*
*/

static int EJECT (const char *line)
{
    unsigned int drive = matchnum (&line);
    int result = RC_WARN;

    if (drive < 4) {
	if (currprefs.dfxtype[drive] >= 0) {
	    disk_eject (drive);
	    sprintf (RESULT, "Drive %d ejected.", drive);
	    result = RC_OK;
	} else
	    sprintf (RESULT, "Drive %d is disabled.", drive);
    }
    return result;
}

/****************************************************************************/

/****i* januae-ami-rexx/INSERT *********************************************
*
*   NAME
*       INSERT() -- Process an ARexx floppy-insert request.
*
*   SYNOPSIS
*       static int INSERT (const char *line);
*
*   FUNCTION
*       Parse an enabled drive index and copy the supplied image filename
*       into changed_prefs for that drive.
*
*   INPUTS
*       line - Drive number followed by the floppy image path.
*
*   RESULT
*       RC_OK when the drive is enabled and in range; otherwise RC_WARN.
*
*   NOTES
*       Media changes are applied through preferences rather than inserted
*       synchronously.
*
****************************************************************************
*
*/

static int INSERT (const char *line)
{
    unsigned int drive = matchnum (&line);
    int result = RC_WARN;

    if (drive < 4) {
	if (currprefs.dfxtype[drive] >= 0) {
	    char buff[256];
	    extractstr (&line, buff, 256);
	    strcpy (changed_prefs.df[drive], buff);
	    result = RC_OK;
	} else
	    sprintf (RESULT, "Drive %d is disabled.", drive);
    }
    return result;
}

/****************************************************************************/

/****i* januae-ami-rexx/QUIT ***********************************************
*
*   NAME
*       QUIT() -- Request emulator termination.
*
*   SYNOPSIS
*       static void QUIT (void);
*
*   FUNCTION
*       Call uae_quit() to request the emulator shutdown sequence.
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

static void QUIT (void)
{
    uae_quit ();
}

/****************************************************************************/

/****i* januae-ami-rexx/QUERY **********************************************
*
*   NAME
*       QUERY() -- Read an ARexx-visible emulator property.
*
*   SYNOPSIS
*       static int QUERY (const char *line);
*
*   FUNCTION
*       Report selected LED and floppy filename values, display state, frame
*       rate, frame counter or sound mode in the shared reply string.
*
*   INPUTS
*       line - Property selector following QUERY.
*
*   RESULT
*       RC_OK for recognized selectors; RC_ERROR for an unknown selector.
*
*   NOTES
*       The FAKEJOYSTICK selector has no current value, and FRAMENUM depends
*       on USE_AMIGA_GFX.
*
****************************************************************************
*
*/

static int QUERY (const char *line)
{
    const char *res = NULL;

    if      (matchstr (&line, "LED_POW"))      res = led_state[0] ? "1" : "0";
    else if (matchstr (&line, "LED_DF0"))      res = led_state[1] ? "1" : "0";
    else if (matchstr (&line, "LED_DF1"))      res = led_state[2] ? "1" : "0";
    else if (matchstr (&line, "LED_DF2"))      res = led_state[3] ? "1" : "0";
    else if (matchstr (&line, "LED_DF3"))      res = led_state[4] ? "1" : "0";
    else if (matchstr (&line, "NAME_DF0"))     res = currprefs.df[0];
    else if (matchstr (&line, "NAME_DF1"))     res = currprefs.df[1];
    else if (matchstr (&line, "NAME_DF2"))     res = currprefs.df[2];
    else if (matchstr (&line, "NAME_DF3"))     res = currprefs.df[3];
    else if (matchstr (&line, "FAKEJOYSTICK")) res = 0; //res = currprefs.fake_joystick ? "1" : "0";
    else if (matchstr (&line, "DISPLAY"))      res = inhibit_frame ? "0" : "1";
    else if (matchstr (&line, "FRAMERATE")) {
	sprintf (RESULT, "%d", currprefs.gfx_framerate);
        return RC_OK;
    } else if (matchstr (&line, "FRAMENUM")) {
#ifdef USE_AMIGA_GFX
	sprintf (RESULT, "%u", frame_num);
#else
	sprintf (RESULT, "%d", 0);
#endif
	return RC_OK;
    } else if (matchstr (&line, "SOUND")) {
	sprintf (RESULT, "%d", sound_available ? currprefs.produce_sound : -1);
	return RC_OK;
    } else return RC_ERROR;

    if (res)
	strncpy (RESULT, res, RESULT_LEN);

    return RC_OK;
}

/****************************************************************************/

/****i* januae-ami-rexx/FEEDBACK *******************************************
*
*   NAME
*       FEEDBACK() -- Configure an ARexx feedback action.
*
*   SYNOPSIS
*       static int FEEDBACK (const char *line);
*
*   FUNCTION
*       Identify a supported LED, floppy-name or ON_EXIT notification slot
*       and store its destination port and optional on/off command strings.
*
*   INPUTS
*       line - Notification selector and ADDRESS or COMMAND options.
*
*   RESULT
*       RC_OK for a recognized slot; otherwise RC_ERROR.
*
****************************************************************************
*
*/

static int FEEDBACK (const char *line)
{
    gui_rexx_s *gui = NULL;

    if      (matchstr (&line, "LED_POW"))  gui = &gui_rexx[LED_POW];
    else if (matchstr (&line, "LED_DF0"))  gui = &gui_rexx[LED_DF0];
    else if (matchstr (&line, "LED_DF1"))  gui = &gui_rexx[LED_DF1];
    else if (matchstr (&line, "LED_DF2"))  gui = &gui_rexx[LED_DF2];
    else if (matchstr (&line, "LED_DF3"))  gui = &gui_rexx[LED_DF3];
    else if (matchstr (&line, "NAME_DF0")) gui = &gui_rexx[NAME_DF0];
    else if (matchstr (&line, "NAME_DF1")) gui = &gui_rexx[NAME_DF1];
    else if (matchstr (&line, "NAME_DF2")) gui = &gui_rexx[NAME_DF2];
    else if (matchstr (&line, "NAME_DF3")) gui = &gui_rexx[NAME_DF3];
    else if (matchstr (&line, "ON_EXIT"))  gui = &gui_rexx[ON_EXIT];
    else return RC_ERROR;

    while(1) {
	if (matchstr (&line, "ADDRESS") ||
	    matchstr (&line, "PORT")) {
	    extractstr (&line, gui->port, PORT_LEN);
	} else if (matchstr (&line,"COMMAND") ||
		   matchstr (&line,"CMD") ||
		   matchstr (&line,"CMD_ON")) {
	    extractstr (&line, gui->cmd_on,  CMD_LEN);
	} else if (matchstr(&line,"CMD_OFF")) {
	    extractstr(&line, gui->cmd_off, CMD_LEN);
	} else break;
    }
    return RC_OK;
}

/****************************************************************************/

/****i* januae-ami-rexx/GET_VERSION ****************************************
*
*   NAME
*       GET_VERSION() -- Format requested emulator version information.
*
*   SYNOPSIS
*       static int GET_VERSION (const char *line);
*
*   FUNCTION
*       Write the package identification, numeric version, author, or Amiga
*       port attribution to the shared response buffer.
*
*   INPUTS
*       line - STRING, NUM, AUTHOR or PORT selector.
*
*   RESULT
*       RC_OK for a recognized selector; otherwise RC_ERROR.
*
****************************************************************************
*
*/

static int GET_VERSION (const char *line)
{
    if (matchstr (&line, "STRING")) {
	sprintf (RESULT,
		 PACKAGE_NAME "-" PACKAGE_VERSION " by Richard Drummond & contributors, "
		 "Amiga Port based on work by Samuel Devulder.");
    } else if (matchstr (&line, "NUM")) {
	sprintf (RESULT, "%d", UAEMAJOR * 10000 + UAEMINOR * 100 + UAESUBREV);
    } else if (matchstr (&line, "AUTHOR")) {
	sprintf (RESULT," by Richard Drummond & contributors");
    } else if (matchstr (&line, "PORT")) {
	sprintf (RESULT, "Amiga Port based on work by Samuel Devulder");
    } else return RC_ERROR;
    return RC_OK;
}

/****************************************************************************/

/****i* januae-ami-rexx/FRAMERATE ******************************************
*
*   NAME
*       FRAMERATE() -- Set the pending emulator frame-rate preference.
*
*   SYNOPSIS
*       static int FRAMERATE (const char *line);
*
*   FUNCTION
*       Parse the requested value and assign changed_prefs.gfx_framerate
*       when the value is between 1 and 20 inclusive.
*
*   INPUTS
*       line - Decimal frame-rate argument.
*
*   RESULT
*       RC_OK for an accepted value; otherwise RC_WARN with an explanatory
*       reply.
*
****************************************************************************
*
*/

static int FRAMERATE (const char *line)
{
    int num;
    num = matchnum (&line);
    if (num >= 1 && num <= 20)
	changed_prefs.gfx_framerate = num;
    else {
	sprintf (RESULT,"Invalid frame rate: %d\n", num);
	return RC_WARN;
    }
    return RC_OK;
}

/****************************************************************************/

/****i* januae-ami-rexx/FAKEJOYSTICK ***************************************
*
*   NAME
*       FAKEJOYSTICK() -- Provide the legacy fake-joystick command.
*
*   SYNOPSIS
*       static int FAKEJOYSTICK (const char *line);
*
*   FUNCTION
*       Perform no joystick configuration. The former command parser is
*       commented out.
*
*   INPUTS
*       line - Ignored command arguments.
*
*   RESULT
*       Always returns 0.
*
****************************************************************************
*
*/

static int FAKEJOYSTICK (const char *line)
{
/*    if     (matchstr(&line,"ON"))     changed_prefs.fake_joystick = 2;
    else if(matchstr(&line,"OFF"))    changed_prefs.fake_joystick = 0;
    else if(matchstr(&line,"TOGGLE")) changed_prefs.fake_joystick =
                                      currprefs.fake_joystick?0:2;
    else return RC_ERROR;
    return RC_OK; */

    return 0;
}

/****************************************************************************/

/****i* januae-ami-rexx/DISPLAY ********************************************
*
*   NAME
*       DISPLAY() -- Control emulator frame-display inhibition.
*
*   SYNOPSIS
*       static int DISPLAY (const char *line);
*
*   FUNCTION
*       Interpret ON, OFF or TOGGLE and update the scroll-lock display
*       inhibition flag.
*
*   INPUTS
*       line - ON, OFF or TOGGLE argument.
*
*   RESULT
*       RC_OK for a recognized argument; otherwise RC_ERROR.
*
****************************************************************************
*
*/

static int DISPLAY (const char *line)
{
    if      (matchstr (&line, "ON"))     clear_inhibit_frame (IHF_SCROLLLOCK);
    else if (matchstr (&line, "OFF"))    set_inhibit_frame (IHF_SCROLLLOCK);
    else if (matchstr (&line, "TOGGLE")) toggle_inhibit_frame (IHF_SCROLLLOCK);
    else return RC_ERROR;
    return RC_OK;
}

/****************************************************************************/

/****i* januae-ami-rexx/SOUND **********************************************
*
*   NAME
*       SOUND() -- Control the emulator sound-production mode.
*
*   SYNOPSIS
*       static int SOUND (const char *line);
*
*   FUNCTION
*       When sound is available, interpret ON, OFF, BEST or TOGGLE and
*       modify currprefs.produce_sound.
*
*   INPUTS
*       line - Requested sound mode.
*
*   RESULT
*       RC_OK for an accepted mode, RC_WARN without sound, or RC_ERROR for
*       an unknown mode.
*
****************************************************************************
*
*/

static int SOUND (const char *line)
{
    if (!sound_available) {
	sprintf (RESULT, "Sound not available!");
	return RC_WARN;
    }

    if      (matchstr (&line, "ON"))     currprefs.produce_sound = 2;
    else if (matchstr (&line, "OFF"))    currprefs.produce_sound = 1;
    else if (matchstr (&line, "BEST"))   currprefs.produce_sound = 3;
    else if (matchstr (&line, "TOGGLE"))
	currprefs.produce_sound = currprefs.produce_sound <= 1 ? 2 : 1;
    else return RC_ERROR;

    /* sam: the next 2 lines is a hack and I don't like it */
/*    eventtab[ev_sample].active = (changed_prefs.produce_sound>=2)?1:0;
    events_schedule ();
*/
    return RC_OK;
}

/****************************************************************************/

/****i* januae-ami-rexx/DEBUG **********************************************
*
*   NAME
*       DEBUG() -- Request activation of the emulator debugger.
*
*   SYNOPSIS
*       static int DEBUG (void);
*
*   FUNCTION
*       Activate the debugger when compiled with DEBUGGER; otherwise leave
*       it unchanged.
*
*   INPUTS
*       None.
*
*   RESULT
*       RC_OK with DEBUGGER enabled; otherwise RC_ERROR.
*
****************************************************************************
*
*/

static int DEBUG (void)
{
#ifdef DEBUGGER
    activate_debugger ();
    return RC_OK;
#else
    return RC_ERROR;
#endif
}

/****************************************************************************/

/****i* januae-ami-rexx/UAEEXE *********************************************
*
*   NAME
*       UAEEXE() -- Execute an emulator-side CLI command.
*
*   SYNOPSIS
*       static int UAEEXE (const char *line);
*
*   FUNCTION
*       Pass the command text to uaeexe() and set a failure message when
*       execution reports an error.
*
*   INPUTS
*       line - CLI command line to execute.
*
*   RESULT
*       RC_OK when uaeexe() reports success; otherwise RC_WARN.
*
****************************************************************************
*
*/

static int UAEEXE (const char *line)
{
    if (uaeexe (line)) {
	sprintf (RESULT, "Remote CLI failed!");
	return RC_WARN;
    }
    return RC_OK;
}

/****************************************************************************/

/****i* januae-ami-rexx/process_cmd ****************************************
*
*   NAME
*       process_cmd() -- Dispatch a received ARexx command.
*
*   SYNOPSIS
*       static int process_cmd (const char *line);
*
*   FUNCTION
*       Clear the shared result string, match supported UAE command
*       keywords, invoke the appropriate handler, and request quit or reset
*       for those control commands.
*
*   INPUTS
*       line - Command string received through the UAE port.
*
*   RESULT
*       The matched command handler status, RC_OK for quit or reset, or
*       RC_ERROR for an unknown command.
*
****************************************************************************
*
*/

static int process_cmd (const char *line)
{
    RESULT[0] = '\0';
    if      (matchstr (&line, "EJECT"))        return EJECT (line);
    else if (matchstr (&line, "INSERT"))       return INSERT (line);
    else if (matchstr (&line, "QUERY"))        return QUERY (line);
    else if (matchstr (&line, "FEEDBACK"))     return FEEDBACK (line);
    else if (matchstr (&line, "VERSION"))      return GET_VERSION (line);
    else if (matchstr (&line, "BYE"))          QUIT ();
    else if (matchstr (&line, "QUIT"))         QUIT ();
    else if (matchstr (&line, "DEBUG"))        return DEBUG ();
    else if (matchstr (&line, "RESET"))        m68k_reset ();
    else if (matchstr (&line, "DISPLAY"))      return DISPLAY (line);
    else if (matchstr (&line, "FRAMERATE"))    return FRAMERATE( line);
    else if (matchstr (&line, "FAKEJOYSTICK")) return FAKEJOYSTICK (line);
    else if (matchstr (&line, "SOUND"))        return SOUND (line);
    else if (matchstr (&line, "UAEEXE"))       return UAEEXE (line);
    else return RC_ERROR;
    return RC_OK;
}

/****************************************************************************/

/****i* januae-ami-rexx/rexx_handle_events *********************************
*
*   NAME
*       rexx_handle_events() -- Process messages waiting on the UAE ARexx
*       port.
*
*   SYNOPSIS
*       void rexx_handle_events (void);
*
*   FUNCTION
*       Receive command messages, dispatch their first argument, attach a
*       result string when requested and reply to the command sender.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Messages without the RXCOMM action are logged and skipped by the
*       current implementation.
*
****************************************************************************
*
*/

void rexx_handle_events (void)
{
    struct RexxMsg *msg;
    while ((msg = (struct RexxMsg*)GetMsg (ARexxPort))) {
	if (!(msg->rm_Action & RXCOMM)) {
	    write_log ("Unknown action '%08X' recieved!\n",
		       msg->rm_Action);
	    continue;
	}
	msg->rm_Result1 = process_cmd (msg->rm_Args[0]);
	msg->rm_Result2 = 0;
	if (msg->rm_Action & RXFF_RESULT) {
	    int len = strlen (RESULT); /* holger: trick for powerup */
	    msg->rm_Result2 = (LONG)CreateArgstring (RESULT,len);
	}
	ReplyMsg ((void*)msg);
    }
}

/****************************************************************************/

/****i* januae-ami-rexx/rexx_led *******************************************
*
*   NAME
*       rexx_led() -- Update an ARexx LED and emit configured feedback.
*
*   SYNOPSIS
*       void rexx_led (int led, int on);
*
*   FUNCTION
*       Store one of five power/floppy LED states; when both on and off
*       commands and a destination exist, send the selected feedback
*       command.
*
*   INPUTS
*       led - LED index from 0 through 4. on - New state.
*
*   RESULT
*       No value is returned. Indices outside 0 through 4 are ignored.
*
****************************************************************************
*
*/

void rexx_led (int led, int on)
{
    gui_rexx_s *gui = NULL;

    if (led < 0 || led > 4)
	return;

    led_state[led] = on;

    if (led == 0) gui = &gui_rexx[LED_POW];
    if (led == 1) gui = &gui_rexx[LED_DF0];
    if (led == 2) gui = &gui_rexx[LED_DF1];
    if (led == 3) gui = &gui_rexx[LED_DF2];
    if (led == 4) gui = &gui_rexx[LED_DF3];

    if (gui->port[0] && gui->cmd_on[0] && gui->cmd_off[0]) {
	if (ADDRESS (gui->port, on ? gui->cmd_on : gui->cmd_off) != RC_OK) {
	    write_log ("%s:%s:%s\n", gui->port,
				     on ? gui->cmd_on : gui->cmd_off,
				     RESULT);
	}
    }
}

/****************************************************************************/

/****i* januae-ami-rexx/rexx_filename **************************************
*
*   NAME
*       rexx_filename() -- Notify an ARexx receiver of a floppy filename.
*
*   SYNOPSIS
*       void rexx_filename (unsigned int num, const char *filename);
*
*   FUNCTION
*       For drives 0 through 3, format the configured filename feedback
*       command with the supplied path and send it to the designated
*       receiver.
*
*   INPUTS
*       num - Floppy drive index. filename - Drive filename.
*
*   RESULT
*       No value is returned. Indices above 3 are ignored.
*
****************************************************************************
*
*/

void rexx_filename (unsigned int num, const char *filename)
{
    gui_rexx_s *gui = NULL;

    if (num > 3)
	return;
    gui = &gui_rexx[NAME_DF0 + num];
    if (gui->port[0] && gui->cmd_on[0]) {
	char buf[CMD_LEN];
	sprintf (buf, gui->cmd_on, filename);
	if (ADDRESS (gui->port, buf) != RC_OK) {
	    write_log ("%s:%s:%s\n", gui->port, buf, RESULT);
	}
    }
}

/****************************************************************************/
/* send a message to an AREXX port.
 */
/****i* januae-ami-rexx/ADDRESS ********************************************
*
*   NAME
*       ADDRESS() -- Send a command to a named ARexx destination.
*
*   SYNOPSIS
*       static int ADDRESS (const char *hostname, const char *cmd);
*
*   FUNCTION
*       Use SystemTagList() for the special COMMAND destination; otherwise
*       locate the named port, exchange an RXCOMM message and store a
*       returned result string.
*
*   INPUTS
*       hostname - Receiver port name or COMMAND. cmd - Command text.
*
*   RESULT
*       SystemTagList() status for COMMAND, the receiver response code for a
*       completed ARexx request, or RC_WARN when exchange cannot be
*       established.
*
****************************************************************************
*
*/

static int ADDRESS (const char *hostname, const char *cmd)
{
    struct MsgPort *RexxPort,
		   *ReplyPort;
    struct RexxMsg *HostMsg,
		   *answer;
    int result = RC_WARN;

    if (!stricmp (hostname, "COMMAND"))
	return SystemTagList(cmd,NULL);

    if ((RexxPort = (void *)FindPort (hostname))) {
	if ((ReplyPort = (void *)CreateMsgPort ())) {
	    if ((HostMsg = CreateRexxMsg (ReplyPort, NULL, hostname))) {
		unsigned int len = strlen (cmd); /* holger: trick for powerup */
		if ((HostMsg->rm_Args[0] = CreateArgstring ((char *)cmd, len))) {
		    HostMsg->rm_Action = RXCOMM | RXFF_RESULT;
		    PutMsg (RexxPort, (void*)HostMsg);
		    WaitPort (ReplyPort);
		    while (!(answer = (void *)GetMsg (ReplyPort)));
		    result = answer->rm_Result1;
		    if (result == RC_OK) {
			if (answer->rm_Result2) {
			    strncpy (RESULT,(char *)answer->rm_Result2, RESULT_LEN);
			    DeleteArgstring ((char *)answer->rm_Result2);
			} else RESULT[0] = '\0';
		    }
		    DeleteArgstring (HostMsg->rm_Args[0]);
		} else strcpy (RESULT, "Can't create argstring!");
		DeleteRexxMsg (HostMsg);
	    } else strcpy (RESULT, "Can't create rexx message!");
	    DeleteMsgPort (ReplyPort);
	} else strcpy (RESULT, "Can't alloc reply port!");
    } else sprintf (RESULT, "Port \"%s\" not found!", hostname);
    return result;
}

/****************************************************************************/
/* argument parsing routines
 */
/****i* januae-ami-rexx/matchstr *******************************************
*
*   NAME
*       matchstr() -- Consume a case-insensitive command token.
*
*   SYNOPSIS
*       static int matchstr (const char **line, const char *pat);
*
*   FUNCTION
*       Skip leading whitespace, optionally consume an opening quote,
*       compare the requested token, and advance the input pointer only on a
*       complete match.
*
*   INPUTS
*       line - In/out command cursor. pat - Expected keyword.
*
*   RESULT
*       1 on a complete match; otherwise 0.
*
****************************************************************************
*
*/

static int matchstr (const char **line, const char *pat)
{
    const char *s = *line;
    char match = 0;

    while (isspace (*s))
	++s;
    if (*s == '\"' || *s == '\'')
	match = *s++;
    while (*s && (tolower (*s) == tolower (*pat)) && (!match || *s != match))
	{++s;++pat;}
    if (match && *s == match && s[1])
	++s;
    if (!*pat && (!*s || isspace (*s))) {
	while (isspace (*s))
	    ++s;
	*line = s;
	return 1;
    }
    return 0;
}

/****************************************************************************/

/****i* januae-ami-rexx/extractstr *****************************************
*
*   NAME
*       extractstr() -- Copy the next command argument into a buffer.
*
*   SYNOPSIS
*       static void extractstr (const char **line, char *result, unsigned
*       int len);
*
*   FUNCTION
*       Skip leading space, interpret an optional quote delimiter and quote
*       escapes, copy at most len minus one characters, and advance the
*       input cursor.
*
*   INPUTS
*       line - In/out command cursor. result - Destination string. len -
*       Destination capacity.
*
*   RESULT
*       No value is returned. A terminating NUL is written.
*
****************************************************************************
*
*/

static void extractstr (const char **line, char *result, unsigned int len)
{
    const char *s = *line;
    char match = 0;

    while (isspace(*s))
	++s;

    if (*s == '\"' || *s == '\'')
	match = *s++;
    while (*s && *s != match) {
	if (*s == '\\' && (s[1] == '\'' || s[1] == '\"'))
	    ++s;
	if (len > 1) {
	    *result++ = *s;
	    --len;
	}
	++s;
	if (!match && isspace (*s))
	    break;
    }
    if (match && *s == match)
	++s;
    while (isspace (*s))
	++s;

    *result  = '\0';
    *line    = s;
}

/****************************************************************************/

/****i* januae-ami-rexx/matchnum *******************************************
*
*   NAME
*       matchnum() -- Parse a signed decimal command argument.
*
*   SYNOPSIS
*       static int matchnum (const char **line);
*
*   FUNCTION
*       Skip whitespace and optional quotes or sign, consume decimal digits
*       and update the command cursor.
*
*   INPUTS
*       line - In/out command cursor.
*
*   RESULT
*       The parsed signed integer; returns zero when no digits are consumed.
*
****************************************************************************
*
*/

static int matchnum (const char **line)
{
    const char *s = *line;
    char match = 0;
    int sign = 1, num = 0;

    while (isspace (*s))
	++s;
    if (*s == '\"' || *s == '\'')
	match = *s++;
    if (*s == '-') {
	sign = -1;
	++s;
    }
    if (*s == '+')
	++s;
    while (isspace (*s))
	++s;
    while (*s >= '0' && *s <= '9')
	num = num * 10 + (*s++ - '0');
    if (match && *s == match && s[1])
	++s;
    while (isspace (*s))
	++s;
    *line = s;
    return sign > 0 ? num : -num;
}

/****************************************************************************/

#ifdef POWERUP
/* sam: those function should be in the ppc version of the unexisting
        libamiga.a */
#define NEWLIST(l) ((l)->lh_Head = (struct Node *)&(l)->lh_Tail, \
		    /*(l)->lh_Tail = NULL,*/ \
		    (l)->lh_TailPred = (struct Node *)&(l)->lh_Head)

/****i* januae-ami-rexx/CreatePort *****************************************
*
*   NAME
*       CreatePort() -- Create an Exec message port on the PowerUP target.
*
*   SYNOPSIS
*       struct MsgPort *CreatePort (STRPTR name, LONG pri);
*
*   FUNCTION
*       Allocate a signal and a public message-port structure, initialize
*       its message list, and register the port if a name was supplied.
*
*   INPUTS
*       name - Optional public port name. pri - Message-port priority.
*
*   RESULT
*       The created message port, or NULL on allocation failure.
*
*   NOTES
*       Compiled only with POWERUP.
*
****************************************************************************
*
*/

struct MsgPort *CreatePort (STRPTR name, LONG pri)
{
    struct MsgPort *port = NULL;
    UBYTE portsig;

    if ((BYTE)(portsig = AllocSignal (-1)) >= 0) {
	if (!(port = PPCAllocMem (sizeof (struct MsgPort), MEMF_CLEAR | MEMF_PUBLIC)))
	    FreeSignal(portsig);
	else {
	    port->mp_Node.ln_Type = NT_MSGPORT;
	    port->mp_Node.ln_Pri  = pri;
	    port->mp_Node.ln_Name = name;
	    /* done via AllocMem
	     * port->mp_Flags = PA_SIGNAL;
	     */
	    port->mp_SigBit  = portsig;
	    port->mp_SigTask = FindTask (NULL);
	    NEWLIST (&port->mp_MsgList);
	    if (port->mp_Node.ln_Name)
		AddPort(port);
	}
    }
    return port;
}

/****i* januae-ami-rexx/DeletePort *****************************************
*
*   NAME
*       DeletePort() -- Release a PowerUP message port.
*
*   SYNOPSIS
*       void DeletePort (struct MsgPort *port);
*
*   FUNCTION
*       Remove a named port from the public port list, invalidate its key
*       fields, free its signal and release its memory.
*
*   INPUTS
*       port - Message port to delete.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Compiled only with POWERUP.
*
****************************************************************************
*
*/

void DeletePort (struct MsgPort *port)
{
    int i;

    if (port->mp_Node.ln_Name != NULL)
	RemPort (port);
    i = -1;
    port->mp_Node.ln_Type = i;
    port->mp_MsgList.lh_Head = (struct Node *)i;
    FreeSignal (port->mp_SigBit);
    PPCFreeMem (port, sizeof (struct MsgPort));
}

/****i* januae-ami-rexx/CreateExtIO ****************************************
*
*   NAME
*       CreateExtIO() -- Allocate a PowerUP extended I/O request.
*
*   SYNOPSIS
*       struct IORequest *CreateExtIO (struct MsgPort *port, long iosize);
*
*   FUNCTION
*       Allocate public cleared request storage and populate its reply port,
*       message type and length.
*
*   INPUTS
*       port - Reply message port. iosize - Request size.
*
*   RESULT
*       The new I/O request, or NULL when allocation cannot succeed.
*
*   NOTES
*       Compiled only with POWERUP.
*
****************************************************************************
*
*/

struct IORequest *CreateExtIO (struct MsgPort *port, long iosize)
{
    struct IORequest *ioreq=NULL;

    if (port && (ioreq = PPCAllocMem (iosize, MEMF_CLEAR | MEMF_PUBLIC))) {
	ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
	ioreq->io_Message.mn_ReplyPort    = port;
	ioreq->io_Message.mn_Length       = iosize;
    }
    return ioreq;
}

/****i* januae-ami-rexx/CreateStdIO ****************************************
*
*   NAME
*       CreateStdIO() -- Allocate a standard PowerUP I/O request.
*
*   SYNOPSIS
*       struct IOStdReq *CreateStdIO (struct MsgPort *port);
*
*   FUNCTION
*       Call CreateExtIO() with the size of struct IOStdReq and return its
*       result.
*
*   INPUTS
*       port - Reply port for the I/O request.
*
*   RESULT
*       The allocated standard I/O request or NULL.
*
*   NOTES
*       Compiled only with POWERUP.
*
****************************************************************************
*
*/

struct IOStdReq *CreateStdIO (struct MsgPort *port)
{
    return (struct IOStdReq *)CreateExtIO (port, sizeof (struct IOStdReq));
}

/****i* januae-ami-rexx/DeleteExtIO ****************************************
*
*   NAME
*       DeleteExtIO() -- Release a PowerUP extended I/O request.
*
*   SYNOPSIS
*       void DeleteExtIO (struct IORequest *ioreq);
*
*   FUNCTION
*       Invalidate the request message type, device and unit pointers, then
*       release memory using the recorded message length.
*
*   INPUTS
*       ioreq - Extended I/O request to free.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Compiled only with POWERUP.
*
****************************************************************************
*
*/

void DeleteExtIO (struct IORequest *ioreq)
{
    int i;

    i=-1;
    ioreq->io_Message.mn_Node.ln_Type = i;
    ioreq->io_Device = (struct Device *)i;
    ioreq->io_Unit = (struct Unit *)i;
    PPCFreeMem (ioreq, ioreq->io_Message.mn_Length);
}

/****i* januae-ami-rexx/DeleteStdIO ****************************************
*
*   NAME
*       DeleteStdIO() -- Release a standard PowerUP I/O request.
*
*   SYNOPSIS
*       void DeleteStdIO (struct IORequest *ioreq);
*
*   FUNCTION
*       Delegate to DeleteExtIO() for request invalidation and release.
*
*   INPUTS
*       ioreq - Standard I/O request to free.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Compiled only with POWERUP.
*
****************************************************************************
*
*/

void DeleteStdIO (struct IORequest *ioreq)
{
    DeleteExtIO (ioreq);
}

#endif
