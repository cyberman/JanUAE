 /*
  * UAE - The Un*x Amiga Emulator
  *
  * GUI interface (to be done).
  * Calls AREXX interface.
  *
  * Copyright 1996 Bernd Schmidt, Samuel Devulder
  * Copyright 2004-2006 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "gui.h"
#include "disk.h"
#include "savestate.h"

#include <intuition/intuition.h>
#include <libraries/asl.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <dos/dosextens.h>

/****************************************************************************/

extern int  rexx_init (void);
extern void rexx_exit (void);
extern void rexx_led (int led, int on);          /* ami-rexx.c */
extern void rexx_filename (int num, const char *name);
extern void rexx_handle_events (void);
extern void main_window_led (int led, int on);   /* ami-win.c */

/****************************************************************************/

extern struct AslIFace *IAsl;

/* File dialog types */
#define FILEDIALOG_INSERT_DF0    0
#define FILEDIALOG_INSERT_DF1    1
#define FILEDIALOG_INSERT_DF2    2
#define FILEDIALOG_INSERT_DF3    3
#define FILEDIALOG_LOAD_STATE    4
#define FILEDIALOG_SAVE_STATE    5
#define FILEDIALOG_MAX           6

#define FILEDIALOG_DRIVE(x) ((x)-FILEDIALOG_INSERT_DF0)

/* For remembering last directory used in file requesters */

static char *last_floppy_dir;
static char *last_savestate_dir;

/****i* januae-ami-gui/free_last_floppy_dir ********************************
*
*   NAME
*       free_last_floppy_dir() -- Release the cached floppy requester
*       directory.
*
*   SYNOPSIS
*       static void free_last_floppy_dir (void);
*
*   FUNCTION
*       Free the saved floppy-directory string and clear its pointer when
*       one exists.
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

static void free_last_floppy_dir (void)
{
    if (last_floppy_dir) {
	free (last_floppy_dir);
	last_floppy_dir = 0;
    }
}

/****i* januae-ami-gui/free_last_savestate_dir *****************************
*
*   NAME
*       free_last_savestate_dir() -- Release the cached state requester
*       directory.
*
*   SYNOPSIS
*       static void free_last_savestate_dir (void);
*
*   FUNCTION
*       Free the saved state-directory string and clear its pointer when one
*       exists.
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

static void free_last_savestate_dir (void)
{
    if (last_savestate_dir) {
	free (last_savestate_dir);
	last_savestate_dir = 0;
    }
}

/****i* januae-ami-gui/get_last_floppy_dir *********************************
*
*   NAME
*       get_last_floppy_dir() -- Obtain the initial floppy requester
*       directory.
*
*   SYNOPSIS
*       static const char *get_last_floppy_dir (void);
*
*   FUNCTION
*       Initialize the cached directory from the floppy_path preference on
*       first access and register its eventual cleanup.
*
*   INPUTS
*       None.
*
*   RESULT
*       The cached directory string, or NULL if no string is available.
*
****************************************************************************
*
*/

static const char *get_last_floppy_dir (void)
{
    if (!last_floppy_dir) {
	static int done = 0;
	unsigned int len;

	if (!done) {
	    done = 1;
	    atexit (free_last_floppy_dir);
	}

        last_floppy_dir = my_strdup (prefs_get_attr ("floppy_path"));
    }
    return last_floppy_dir;
}

/****i* januae-ami-gui/get_last_savestate_dir ******************************
*
*   NAME
*       get_last_savestate_dir() -- Obtain the initial state requester
*       directory.
*
*   SYNOPSIS
*       static const char *get_last_savestate_dir (void);
*
*   FUNCTION
*       Initialize the cached directory from the savestate_path preference
*       on first access and register its eventual cleanup.
*
*   INPUTS
*       None.
*
*   RESULT
*       The cached directory string, or NULL if no string is available.
*
****************************************************************************
*
*/

static const char *get_last_savestate_dir (void)
{
    if (!last_savestate_dir) {
	static int done = 0;
	unsigned int len;

	if (!done) {
	    done = 1;
	    atexit (free_last_savestate_dir);
	}

        last_savestate_dir = my_strdup (prefs_get_attr ("savestate_path"));
    }
    return last_savestate_dir;
}

/****i* januae-ami-gui/set_last_floppy_dir *********************************
*
*   NAME
*       set_last_floppy_dir() -- Replace the cached floppy requester
*       directory.
*
*   SYNOPSIS
*       static void set_last_floppy_dir (const char *path);
*
*   FUNCTION
*       Discard the previous floppy-directory string and copy a nonempty
*       replacement when provided.
*
*   INPUTS
*       path - New directory path, or NULL to clear the cached value.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void set_last_floppy_dir (const char *path)
{
    if (last_floppy_dir) {
	free (last_floppy_dir);
	last_floppy_dir = 0;
    }

    if (path) {
	unsigned int len = strlen (path);
	if (len) {
	    last_floppy_dir = malloc (len + 1);
	    if (last_floppy_dir)
		strcpy (last_floppy_dir, path);
	}
    }
}

/****i* januae-ami-gui/set_last_savestate_dir ******************************
*
*   NAME
*       set_last_savestate_dir() -- Replace the cached state requester
*       directory.
*
*   SYNOPSIS
*       static void set_last_savestate_dir (const char *path);
*
*   FUNCTION
*       Discard the previous state-directory string and copy a nonempty
*       replacement when provided.
*
*   INPUTS
*       path - New directory path, or NULL to clear the cached value.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void set_last_savestate_dir (const char *path)
{
    if (last_savestate_dir) {
	free (last_savestate_dir);
	last_savestate_dir = 0;
    }

    if (path) {
	unsigned int len = strlen (path);
	if (len) {
	    last_savestate_dir = malloc (len + 1);
	    if (last_savestate_dir)
		strcpy (last_savestate_dir, path);
	}
    }
}

/****i* januae-ami-gui/do_file_dialog **************************************
*
*   NAME
*       do_file_dialog() -- Run the host file requester for a GUI command.
*
*   SYNOPSIS
*       static void do_file_dialog (unsigned int type);
*
*   FUNCTION
*       Present an ASL requester for a selected floppy drive or saved state.
*       On acceptance, update the selected floppy path, save emulator state,
*       or schedule state restoration.
*
*   INPUTS
*       type - FILEDIALOG_INSERT_DF0 through FILEDIALOG_SAVE_STATE.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The floppy-selection branch updates changed_prefs and the cached
*       state directory; it does not insert media immediately. Invalid type
*       values are ignored.
*
****************************************************************************
*
*/

static void do_file_dialog (unsigned int type)
{
    struct FileRequester *FileRequest;
    struct Window *win;

    char buf[80];
    char path[512];

    const char *req_prompt;
    const char *req_pattern = 0;
    const char *req_lastdir;
    int         req_do_save = FALSE;

#ifdef __amigaos4__
    int release_asl = 0;
#endif

    if (type >= FILEDIALOG_MAX)
	return;

    if (!AslBase) {
	AslBase = OpenLibrary ("asl.library", 36);
	if (!AslBase) {
	    write_log ("Can't open asl.library v36.\n");
	    return;
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
        release_asl = 1;
#endif
    }

    FileRequest = AllocAslRequest (ASL_FileRequest, NULL);
    if (!FileRequest) {
	write_log ("Unable to allocate file requester.\n");
	return;
    }

    /* Find this task's default window */
    win = ((struct Process *) FindTask (NULL))->pr_WindowPtr;
    if (win == (struct Window *)-1)
	win = 0;

    /*
     * Prepare requester.
     */
    switch (type) {

	default: /* to stop GCC complaining */
	case FILEDIALOG_INSERT_DF0:
	case FILEDIALOG_INSERT_DF1:
	case FILEDIALOG_INSERT_DF2:
	case FILEDIALOG_INSERT_DF3:
	    sprintf (buf, "Select image to insert in drive DF%d:", FILEDIALOG_DRIVE(type));
	    req_prompt = buf;
	    req_pattern = "(#?.(ad(f|z)|dms|ipf|zip)#?|df?|?)";
	    req_lastdir = get_last_floppy_dir ();
	    break;

	case FILEDIALOG_SAVE_STATE:
	    req_prompt = "Select file to save emulator state to\n";
	    req_pattern = "#?.uss";
	    req_lastdir = get_last_savestate_dir ();
	    req_do_save = TRUE;
	    break;

	case FILEDIALOG_LOAD_STATE:
	    req_prompt = "Select saved state file to load";
	    req_pattern = "#?.uss";
	    req_lastdir = get_last_savestate_dir ();
	    break;
    }

    /*
     * Do the file request.
     */
    if (AslRequestTags (FileRequest,
			ASLFR_TitleText,      req_prompt,
			ASLFR_InitialDrawer,  req_lastdir,
			ASLFR_InitialPattern, req_pattern,
			ASLFR_DoPatterns,     req_pattern != 0,
			ASLFR_DoSaveMode,     req_do_save,
			ASLFR_RejectIcons,    TRUE,
			ASLFR_Window,         win,
			TAG_DONE)) {

	/*
	 * User selected a file.
	 *
	 * Construct file path to selected image.
	 */
	strcpy (path, FileRequest->fr_Drawer);
	if (strlen (path) && !(path[strlen (path) - 1] == ':' || path[strlen (path) - 1] == '/'))
	    strcat (path, "/");
	strcat (path, FileRequest->fr_File);

        /*
	 * Process selected file.
	 */
	switch (type) {

	    default: /* to stop GCC complaining */
	    case FILEDIALOG_INSERT_DF0:
	    case FILEDIALOG_INSERT_DF1:
	    case FILEDIALOG_INSERT_DF2:
	    case FILEDIALOG_INSERT_DF3:
		set_last_savestate_dir (FileRequest->fr_Drawer);
		strcpy (changed_prefs.df[FILEDIALOG_DRIVE(type)], path);
		break;

	    case FILEDIALOG_SAVE_STATE:
		set_last_savestate_dir (FileRequest->fr_Drawer);
		savestate_initsave (path, 1);
		save_state (path, "Description");
		break;

	    case FILEDIALOG_LOAD_STATE:
		set_last_savestate_dir (FileRequest->fr_Drawer);
		savestate_initsave (path, 1);
		savestate_state = STATE_DORESTORE;
		write_log ("Restoring state from '%s'...\n", path);
		break;
	}
    }

    FreeAslRequest (FileRequest);

#ifdef __amigaos4__
    if (release_asl)
        IAsl->Release ();
#endif

    return;
}

/****************************************************************************/

/****i* januae-ami-gui/gui_init ********************************************
*
*   NAME
*       gui_init() -- Provide the GUI initialization entry point.
*
*   SYNOPSIS
*       void gui_init (int argc, char **argv);
*
*   FUNCTION
*       Perform no initialization. The arguments are ignored.
*
*   INPUTS
*       argc - Unused argument count. argv - Unused argument vector.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void gui_init (int argc, char **argv)
{
}

/****************************************************************************/

static int have_rexx = 0;

/****i* januae-ami-gui/gui_open ********************************************
*
*   NAME
*       gui_open() -- Initialize the optional ARexx command port.
*
*   SYNOPSIS
*       int gui_open (void);
*
*   FUNCTION
*       Attempt to initialize the ARexx interface once until successful;
*       register the ARexx cleanup function after a successful
*       initialization.
*
*   INPUTS
*       None.
*
*   RESULT
*       Always returns -1, independent of ARexx initialization success.
*
****************************************************************************
*
*/

int gui_open (void)
{
    if (!have_rexx) {
	have_rexx = rexx_init ();

	if (have_rexx)
	   atexit (rexx_exit);
    }
    return -1;
}

/****************************************************************************/

/****i* januae-ami-gui/gui_exit ********************************************
*
*   NAME
*       gui_exit() -- Provide the GUI shutdown entry point.
*
*   SYNOPSIS
*       void gui_exit (void);
*
*   FUNCTION
*       Perform no operation. The ARexx cleanup function is registered
*       separately at process exit.
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

void gui_exit (void)
{
}

/****************************************************************************/

/****i* januae-ami-gui/gui_update ******************************************
*
*   NAME
*       gui_update() -- Provide the GUI update entry point.
*
*   SYNOPSIS
*       int gui_update (void);
*
*   FUNCTION
*       Perform no GUI refresh or state propagation.
*
*   INPUTS
*       None.
*
*   RESULT
*       Always returns 0.
*
****************************************************************************
*
*/

int gui_update (void)
{
    return 0;
}

/****************************************************************************/

/****i* januae-ami-gui/gui_led *********************************************
*
*   NAME
*       gui_led() -- Forward a GUI LED state to ARexx.
*
*   SYNOPSIS
*       void gui_led (int led, int on);
*
*   FUNCTION
*       Pass the LED index and state to rexx_led() when ARexx initialization
*       succeeded.
*
*   INPUTS
*       led - LED index. on - LED state.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void gui_led (int led, int on)
{
    if (have_rexx)
	rexx_led (led, on);
}

/****************************************************************************/

/****i* januae-ami-gui/gui_filename ****************************************
*
*   NAME
*       gui_filename() -- Forward a drive filename to ARexx.
*
*   SYNOPSIS
*       void gui_filename (int num, const char *name);
*
*   FUNCTION
*       Send the drive index and filename to rexx_filename() only when the
*       ARexx interface is active.
*
*   INPUTS
*       num - Drive index. name - Drive filename.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void gui_filename (int num, const char *name)
{
    if (have_rexx)
	rexx_filename (num, name);
}

/****************************************************************************/

/****i* januae-ami-gui/gui_handle_events ***********************************
*
*   NAME
*       gui_handle_events() -- Service pending ARexx messages.
*
*   SYNOPSIS
*       void gui_handle_events (void);
*
*   FUNCTION
*       Dispatch pending ARexx port messages when the interface is active.
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

void gui_handle_events (void)
{
    if (have_rexx)
	rexx_handle_events();
}

/****************************************************************************/

/****i* januae-ami-gui/gui_notify_state ************************************
*
*   NAME
*       gui_notify_state() -- Provide the GUI state-notification hook.
*
*   SYNOPSIS
*       void gui_notify_state (int state);
*
*   FUNCTION
*       Ignore the supplied emulator state; no notification is implemented.
*
*   INPUTS
*       state - New emulator state; unused.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void gui_notify_state (int state)
{
}

/****************************************************************************/

/****i* januae-ami-gui/gui_hd_led ******************************************
*
*   NAME
*       gui_hd_led() -- Update the GUI hard-disk activity indicator.
*
*   SYNOPSIS
*       void gui_hd_led (int led);
*
*   FUNCTION
*       Retain the previous active indication for several zero-valued
*       updates, then update gui_data.hd and forward state changes through
*       gui_led().
*
*   INPUTS
*       led - New hard-disk indicator value.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The outgoing GUI LED index is 5.
*
****************************************************************************
*
*/

void gui_hd_led (int led)
{
    static int resetcounter;

    int old = gui_data.hd;

    if (led == 0) {
	resetcounter--;
	if (resetcounter > 0)
	    return;
    }

    gui_data.hd = led;
    resetcounter = 6;
    if (old != gui_data.hd)
	gui_led (5, gui_data.hd);
}

/****************************************************************************/

/****i* januae-ami-gui/gui_cd_led ******************************************
*
*   NAME
*       gui_cd_led() -- Update the GUI CD-ROM activity indicator.
*
*   SYNOPSIS
*       void gui_cd_led (int led);
*
*   FUNCTION
*       Retain the previous active indication for several zero-valued
*       updates, then update gui_data.cd and forward state changes through
*       gui_led().
*
*   INPUTS
*       led - New CD-ROM indicator value.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The outgoing GUI LED index is 6.
*
****************************************************************************
*
*/

void gui_cd_led (int led)
{
    static int resetcounter;

    int old = gui_data.cd;
    if (led == 0) {
	resetcounter--;
	if (resetcounter > 0)
	    return;
    }

    gui_data.cd = led;
    resetcounter = 6;
    if (old != gui_data.cd)
	gui_led (6, gui_data.cd);
}

/****************************************************************************/

/****i* januae-ami-gui/gui_fps *********************************************
*
*   NAME
*       gui_fps() -- Store frame-rate and idle metrics.
*
*   SYNOPSIS
*       void gui_fps (int fps, int idle);
*
*   FUNCTION
*       Update the fps and idle members of the shared GUI data structure.
*
*   INPUTS
*       fps - Reported frame rate. idle - Reported idle metric.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void gui_fps (int fps, int idle)
{
    gui_data.fps  = fps;
    gui_data.idle = idle;
}

/****************************************************************************/

/****i* januae-ami-gui/gui_display *****************************************
*
*   NAME
*       gui_display() -- Select an emulator file-dialog action.
*
*   SYNOPSIS
*       void gui_display (int shortcut);
*
*   FUNCTION
*       Map shortcuts 0 through 3 to floppy requesters, 4 to loading saved
*       state, and 5 to saving state; ignore all other shortcuts.
*
*   INPUTS
*       shortcut - Requested GUI file-dialog action.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void gui_display (int shortcut)
{
    switch (shortcut) {

	case 0:
	case 1:
	case 2:
	case 3:
	    do_file_dialog (FILEDIALOG_INSERT_DF0 + shortcut);
	    break;
	case 4:
	    do_file_dialog (FILEDIALOG_LOAD_STATE);
	    break;
	case 5:
	    do_file_dialog (FILEDIALOG_SAVE_STATE);
	    break;
	default:
	    ;
    }
}

/****************************************************************************/

/****i* januae-ami-gui/gui_message *****************************************
*
*   NAME
*       gui_message() -- Display and log a GUI information message.
*
*   SYNOPSIS
*       void gui_message (const char *format,...);
*
*   FUNCTION
*       Format the variadic message, show it in an Intuition EasyRequest
*       attached to the current task window when possible, and write the
*       text to the log.
*
*   INPUTS
*       format - printf-style message format followed by its arguments.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void gui_message (const char *format,...)
{
    char msg[2048];
    va_list parms;
    struct EasyStruct req;
    struct Window *win;

    va_start (parms,format);
    vsprintf ( msg, format, parms);
    va_end (parms);

    /* Find this task's default window */
    win = ((struct Process *) FindTask (NULL))->pr_WindowPtr;
    if (win == (struct Window *)-1)
	win = 0;

    req.es_StructSize   = sizeof req;
    req.es_Flags        = 0;
    req.es_Title        = (char *) PACKAGE_NAME " Information";
    req.es_TextFormat   = (char *) msg;
    req.es_GadgetFormat = (char *) "Okay";
    EasyRequest (win, &req, NULL, NULL);

    write_log (msg);
}
