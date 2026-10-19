 /*
  * UAE - The Un*x Amiga Emulator
  *
  * SDL Joystick code
  *
  * Copyright 1997 Bernd Schmidt
  * Copyright 1998 Krister Walfridsson
  * Copyright 2003-2005 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "inputdevice.h"
#include <SDL.h>

static unsigned int nr_joysticks;
static int initialized;

struct joyinfo {
    SDL_Joystick *joy;
    unsigned int axles;
    unsigned int buttons;
};

static struct joyinfo joys[MAX_INPUT_DEVICES];


/****i* januae-jd-sdl/read_joy *********************************************
*
*   NAME
*       read_joy() -- Poll one SDL joystick.
*
*   SYNOPSIS
*       static void read_joy (unsigned int nr);
*
*   FUNCTION
*       Read all reported axes and buttons from the opened SDL handle and
*       send their values to the emulator input layer.
*
*   INPUTS
*       nr - Index of an initialized, opened SDL joystick.
*
*   NOTES
*       With the default input setting, unassigned joysticks are skipped.
*       The SDL handle is not checked for NULL here.
*
****************************************************************************
*
*/
static void read_joy (unsigned int nr)
{
    unsigned int num, i, axes, axis;
    SDL_Joystick *joy;

    if (currprefs.input_selected_setting == 0) {
	if (jsem_isjoy (0, &currprefs) != (int)nr && jsem_isjoy (1, &currprefs) != (int)nr)
	    return;
    }
    joy = joys[nr].joy;
    axes = SDL_JoystickNumAxes (joy);
    for (i = 0; i < axes; i++) {
	axis = SDL_JoystickGetAxis (joy, i);
	setjoystickstate (nr, i, axis, 32767);
    }

    num = SDL_JoystickNumButtons (joy);
    for (i = 0; i < num; i++) {
	int bs = SDL_JoystickGetButton (joy, i) ? 1 : 0;
	setjoybuttonstate (nr, i, bs);
    }
}

/****i* januae-jd-sdl/get_joystick_num *************************************
*
*   NAME
*       get_joystick_num() -- Get the SDL joystick count.
*
*   SYNOPSIS
*       static unsigned int get_joystick_num (void);
*
*   FUNCTION
*       Return the number of joystick slots retained after initialization.
*
*   RESULT
*       Number of retained devices, limited to MAX_INPUT_DEVICES.
*
****************************************************************************
*
*/
static unsigned int get_joystick_num (void)
{
    return nr_joysticks;
}

/****i* januae-jd-sdl/get_joystick_widget_num ******************************
*
*   NAME
*       get_joystick_widget_num() -- Get an SDL joystick widget count.
*
*   SYNOPSIS
*       static unsigned int get_joystick_widget_num (unsigned int joy);
*
*   FUNCTION
*       Add the axis and button counts cached during initialization.
*
*   INPUTS
*       joy - Index of a retained joystick.
*
*   RESULT
*       Number of axes plus buttons.
*
*   NOTES
*       The index is not checked.
*
****************************************************************************
*
*/
static unsigned int get_joystick_widget_num (unsigned int joy)
{
    return joys[joy].axles + joys[joy].buttons;
}

/****i* januae-jd-sdl/get_joystick_widget_type *****************************
*
*   NAME
*       get_joystick_widget_type() -- Identify an SDL joystick widget.
*
*   SYNOPSIS
*       static int get_joystick_widget_type (unsigned int joy, unsigned int
*       num, char *name, uae_u32 *code);
*
*   FUNCTION
*       Classify an index within the axes-first, buttons-second widget
*       sequence; optionally write a display name.
*
*   INPUTS
*       joy - Index of a retained joystick.
*       num - Widget index.
*       name - Optional writable buffer for a formatted label.
*       code - Unused.
*
*   RESULT
*       IDEV_WIDGET_AXIS, IDEV_WIDGET_BUTTON, or IDEV_WIDGET_NONE.
*
*   NOTES
*       No bounds are available for the caller-provided name buffer.
*
****************************************************************************
*
*/
static int get_joystick_widget_type (unsigned int joy, unsigned int num, char *name, uae_u32 *code)
{
    if (num >= joys[joy].axles && num < joys[joy].axles + joys[joy].buttons) {
	if (name)
	    sprintf (name, "Button %d", num + 1 - joys[joy].axles);
	return IDEV_WIDGET_BUTTON;
    } else if (num < joys[joy].axles) {
	if (name)
	    sprintf (name, "Axis %d", num + 1);
	return IDEV_WIDGET_AXIS;
    }
    return IDEV_WIDGET_NONE;
}

/****i* januae-jd-sdl/get_joystick_widget_first ****************************
*
*   NAME
*       get_joystick_widget_first() -- Find the first SDL widget of a kind.
*
*   SYNOPSIS
*       static int get_joystick_widget_first (unsigned int joy, int type);
*
*   FUNCTION
*       Return the first axis or button index in the SDL widget sequence.
*
*   INPUTS
*       joy - Index of a retained joystick.
*       type - IDEV_WIDGET_AXIS or IDEV_WIDGET_BUTTON.
*
*   RESULT
*       Zero for axes, the cached axis count for buttons, or -1 for an
*       unsupported type.
*
****************************************************************************
*
*/
static int get_joystick_widget_first (unsigned int joy, int type)
{
    switch (type) {
	case IDEV_WIDGET_BUTTON:
	    return joys[joy].axles;
	case IDEV_WIDGET_AXIS:
	    return 0;
    }
    return -1;
}

/****i* januae-jd-sdl/get_joystick_name ************************************
*
*   NAME
*       get_joystick_name() -- Get an SDL joystick name.
*
*   SYNOPSIS
*       static const char *get_joystick_name (unsigned int joy);
*
*   FUNCTION
*       Request the device name from SDL for the given joystick index.
*
*   INPUTS
*       joy - SDL joystick index.
*
*   RESULT
*       Name pointer returned by SDL; it is not allocated by this function.
*
*   NOTES
*       No copy is made.
*
****************************************************************************
*
*/
static const char *get_joystick_name (unsigned int joy)
{
    return SDL_JoystickName (joy);
}

/****i* januae-jd-sdl/read_joysticks ***************************************
*
*   NAME
*       read_joysticks() -- Poll the SDL joystick backend.
*
*   SYNOPSIS
*       static void read_joysticks (void);
*
*   FUNCTION
*       Refresh SDL joystick state and forward each retained device to
*       read_joy().
*
*   NOTES
*       No SDL update is performed when the retained joystick count is zero.
*
****************************************************************************
*
*/
static void read_joysticks (void)
{
    if (get_joystick_num ()) {
	unsigned int i;
	SDL_JoystickUpdate ();
	for (i = 0; i < get_joystick_num (); i++)
	    read_joy (i);
    }
}

/****i* januae-jd-sdl/init_joysticks ***************************************
*
*   NAME
*       init_joysticks() -- Initialize SDL joystick input.
*
*   SYNOPSIS
*       static int init_joysticks (void);
*
*   FUNCTION
*       Initialize SDL joystick support, enumerate up to MAX_INPUT_DEVICES
*       devices, open handles, and cache axis and button counts.
*
*   RESULT
*       1 after successful first initialization; 0 if SDL initialization
*       fails or the backend is already marked initialized.
*
*   NOTES
*       SDL_JoystickOpen() and subsequent handle queries are not checked
*       individually for failure.
*
****************************************************************************
*
*/
static int init_joysticks (void)
{
    int success = 0;

    if (!initialized) {
	if (SDL_InitSubSystem (SDL_INIT_JOYSTICK) == 0) {
	    unsigned int i;

	    nr_joysticks = SDL_NumJoysticks ();
	    write_log ("Found %d joystick(s)\n", nr_joysticks);

	    if (nr_joysticks > MAX_INPUT_DEVICES)
		nr_joysticks = MAX_INPUT_DEVICES;

	    for (i = 0; i < get_joystick_num (); i++) {
		joys[i].joy     = SDL_JoystickOpen (i);
		joys[i].axles   = SDL_JoystickNumAxes (joys[i].joy);
		joys[i].buttons = SDL_JoystickNumButtons (joys[i].joy);
	    }
	    success = initialized = 1;
	} else
	    write_log ("Failed to initialize joysticks\n");
    }

    return success;
}

/****i* januae-jd-sdl/close_joysticks **************************************
*
*   NAME
*       close_joysticks() -- Close the SDL joystick backend.
*
*   SYNOPSIS
*       static void close_joysticks (void);
*
*   FUNCTION
*       Close retained SDL handles, clear the joystick count, and quit the
*       joystick subsystem if it was initialized.
*
****************************************************************************
*
*/
static void close_joysticks (void)
{
    unsigned int i;
    for (i = 0; i < nr_joysticks; i++) {
	SDL_JoystickClose (joys[i].joy);
	joys[i].joy = 0;
    }
    nr_joysticks = 0;

    if (initialized) {
	SDL_QuitSubSystem (SDL_INIT_JOYSTICK);
	initialized = 0;
    }
}

/****i* januae-jd-sdl/acquire_joy ******************************************
*
*   NAME
*       acquire_joy() -- Check SDL joystick availability.
*
*   SYNOPSIS
*       static int acquire_joy (unsigned int num, int flags);
*
*   FUNCTION
*       Accept a joystick index within the current retained count; no
*       additional acquisition is performed.
*
*   INPUTS
*       num - Candidate joystick index.
*       flags - Ignored.
*
*   RESULT
*       Nonzero if num is within the retained count; otherwise zero.
*
****************************************************************************
*
*/
static int acquire_joy (unsigned int num, int flags)
{
    return num < get_joystick_num ();
}

/****i* januae-jd-sdl/unacquire_joy ****************************************
*
*   NAME
*       unacquire_joy() -- Release an SDL joystick acquisition.
*
*   SYNOPSIS
*       static void unacquire_joy (unsigned int num);
*
*   FUNCTION
*       Perform no operation; SDL handles remain open until
*       close_joysticks().
*
*   INPUTS
*       num - Ignored.
*
****************************************************************************
*
*/
static void unacquire_joy (unsigned int num)
{
}

struct inputdevice_functions inputdevicefunc_joystick = {
    init_joysticks,
    close_joysticks,
    acquire_joy,
    unacquire_joy,
    read_joysticks,
    get_joystick_num,
    get_joystick_name,
    get_joystick_widget_num,
    get_joystick_widget_type,
    get_joystick_widget_first
};

/****i* januae-jd-sdl/input_get_default_joystick ***************************
*
*   NAME
*       input_get_default_joystick() -- Set default SDL joystick mappings.
*
*   SYNOPSIS
*       void input_get_default_joystick (struct uae_input_device *uid);
*
*   FUNCTION
*       Map up to three buttons and two axes per retained joystick to
*       alternating emulated joystick ports; enable the first configuration
*       entry.
*
*   INPUTS
*       uid - Writable input-device configuration array with at least one
*       entry.
*
*   NOTES
*       uid[0] is enabled even if no SDL joysticks were found.
*
****************************************************************************
*
*/
void input_get_default_joystick (struct uae_input_device *uid)
{
    unsigned int i, port;

    for (i = 0; i < nr_joysticks; i++) {
        port = i & 1;
        uid[i].eventid[ID_AXIS_OFFSET + 0][0]   = port ? INPUTEVENT_JOY2_HORIZ : INPUTEVENT_JOY1_HORIZ;
        uid[i].eventid[ID_AXIS_OFFSET + 1][0]   = port ? INPUTEVENT_JOY2_VERT  : INPUTEVENT_JOY1_VERT;
        uid[i].eventid[ID_BUTTON_OFFSET + 0][0] = port ? INPUTEVENT_JOY2_FIRE_BUTTON : INPUTEVENT_JOY1_FIRE_BUTTON;
        uid[i].eventid[ID_BUTTON_OFFSET + 1][0] = port ? INPUTEVENT_JOY2_2ND_BUTTON  : INPUTEVENT_JOY1_2ND_BUTTON;
        uid[i].eventid[ID_BUTTON_OFFSET + 2][0] = port ? INPUTEVENT_JOY2_3RD_BUTTON  : INPUTEVENT_JOY1_3RD_BUTTON;
    }
    uid[0].enabled = 1;
}
