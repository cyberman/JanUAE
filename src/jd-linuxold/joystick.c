 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Joystick emulation for Linux and BSD. They share too much code to
  * split this file.
  *
  * This uses the deprecated 0.x Linux joystick API.
  *
  * Copyright 1997 Bernd Schmidt
  * Copyright 1998 Krister Walfridsson
  * Copyright 2003-2006 Richard Drummond
  * Copyright 2004 Nick Seow (Alternative Linux joystick device path)
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "inputdevice.h"

#define JS_MAXPATHLEN  20      // Longest device name would be "/dev/input/js"
char js_prefix[JS_MAXPATHLEN]; // Joystick device, which varies, except number

#ifdef HAVE_MACHINE_JOYSTICK_H

/* The BSD way.  */

# include <machine/joystick.h>
typedef struct joystick uae_joystick_t;

#define JS_DEVNAME_PREFIX "joy"

#else

/* The Linux way.  */

/* There are too many different versions of <linux/joystick.h>.  Rather
 * than trying to work correctly with all of them, we duplicate the
 * necessary definitions here.  */
typedef struct
{
    int buttons;
    int x;
    int y;
} uae_joystick_t;

#define JS_DEVNAME_PREFIX "js" // Try this first

#endif

/* Hard code these for the old joystick API */
#define MAX_BUTTONS  2
#define MAX_AXLES    2
#define FIRST_AXLE   0
#define FIRST_BUTTON 2

static unsigned int nr_joysticks;

static int js0, js1;

struct joy_range
{
    int minx, maxx, miny, maxy;
    int centrex, centrey;
} range0, range1;


/****i* januae-jd-linuxold/read_joy ****************************************
*
*   NAME
*       read_joy() -- Poll a legacy joystick.
*
*   SYNOPSIS
*       static void read_joy (unsigned int nr);
*
*   FUNCTION
*       Read one joystick state record, update the observed axis extrema and
*       midpoint, and submit axis and button state to the emulator.
*
*   INPUTS
*       nr - Device index selecting the first or second file descriptor.
*
*   NOTES
*       An incomplete or failed read sends no input. This backend uses the
*       obsolete 0.x joystick API; a missing first device is not represented
*       as a vacant index.
*
****************************************************************************
*
*/
static void read_joy (unsigned int nr)
{
    uae_joystick_t buffer;
    int len;
    int fd = nr == 0 ? js0 : js1;
    struct joy_range *r = nr == 0 ? &range0 : &range1;

    if (currprefs.input_selected_setting == 0) {
	if (jsem_isjoy (0, &currprefs) != (int)nr && jsem_isjoy (1, &currprefs) != (int)nr)
	    return;
    }

    len = read(fd, &buffer, sizeof(buffer));
    if (len != sizeof(buffer))
	return;

    /* According to old 0.x JS API, we don't know the range
     * or the centre for either axis, so we try to work these
     * out as we go along.
     *
     * Must be a better way to do this . . .
     */
    if (buffer.x < r->minx) r->minx = buffer.x;
    if (buffer.y < r->miny) r->miny = buffer.y;
    if (buffer.x > r->maxx) r->maxx = buffer.x;
    if (buffer.y > r->maxy) r->maxy = buffer.y;

    r->centrex = (r->maxx-r->minx)/2 + r->minx;
    r->centrey = (r->maxy-r->miny)/2 + r->miny;

    /* Translate these values to be centred on 0 and
     * feed 'em to the inputdevice system */
    setjoystickstate (nr, 0, buffer.x - r->centrex, r->centrex );
    setjoystickstate (nr, 1, buffer.y - r->centrey, r->centrey );

#ifdef HAVE_MACHINE_JOYSTICK_H
    setjoybuttonstate (nr, 0, buffer.b1);
    setjoybuttonstate (nr, 1, buffer.b2);
#else
    setjoybuttonstate (nr, 0, buffer.buttons & 1);
    setjoybuttonstate (nr, 1, buffer.buttons & 2);
#endif
}

/****i* januae-jd-linuxold/init_joysticks **********************************
*
*   NAME
*       init_joysticks() -- Open legacy joystick devices.
*
*   SYNOPSIS
*       static int init_joysticks(void);
*
*   FUNCTION
*       Try the first two device nodes for the selected platform; on Linux,
*       retry in /dev/input if neither primary node opens. Reset the
*       observed range estimates.
*
*   RESULT
*       Always 1, including when no device opens.
*
*   NOTES
*       Only two devices and two axes per device are supported; the
*       enumeration count does not preserve missing device indices.
*
****************************************************************************
*
*/
static int init_joysticks(void)
{
    char js_path[JS_MAXPATHLEN]; // temporary buffer for device name
    nr_joysticks = 0;
    js0 = -1; js1 = -1;

    snprintf (js_prefix, JS_MAXPATHLEN, "/dev/%s", JS_DEVNAME_PREFIX);

    snprintf (js_path, JS_MAXPATHLEN, "%s0", js_prefix);
    if ((js0 = open (js_path, O_RDONLY)) >= 0)
	nr_joysticks++;

    snprintf (js_path, JS_MAXPATHLEN, "%s1", js_prefix);
    if ((js1 = open (js_path, O_RDONLY)) >= 0)
	nr_joysticks++;

#ifdef __linux__
   if (nr_joysticks == 0) {
        /*
	 * If we haven't found any joysticks yet,
	 * look for /dev/input/js* nodes
	 */
	sprintf (js_prefix, "/dev/input/%s", JS_DEVNAME_PREFIX);

	snprintf (js_path, JS_MAXPATHLEN, "%s0", js_prefix);
	if ((js0 = open (js_path, O_RDONLY)) >= 0)
	    nr_joysticks++;

	snprintf (js_path, JS_MAXPATHLEN, "%s1", js_prefix);
	if ((js1 = open (js_path, O_RDONLY)) >= 0)
	    nr_joysticks++;
    }
#endif

    write_log ("Found %d joystick(s)\n", nr_joysticks);

    range0.minx = INT_MAX;
    range0.maxx = INT_MIN;
    range0.miny = INT_MAX;
    range0.maxy = INT_MIN;
    range1.minx = INT_MAX;
    range1.maxx = INT_MIN;
    range1.miny = INT_MAX;
    range1.maxy = INT_MIN;
    range0.centrex = 0;
    range1.centrey = 0;
    return 1;
}

/****i* januae-jd-linuxold/close_joysticks *********************************
*
*   NAME
*       close_joysticks() -- Close legacy joystick descriptors.
*
*   SYNOPSIS
*       static void close_joysticks(void);
*
*   FUNCTION
*       Close either device descriptor when it is nonnegative.
*
*   NOTES
*       The saved descriptor numbers and device count are not reset.
*
****************************************************************************
*
*/
static void close_joysticks(void)
{
    if (js0 >= 0)
	close (js0);
    if (js1 >= 0)
	close (js1);
}

/****i* januae-jd-linuxold/get_joystick_num ********************************
*
*   NAME
*       get_joystick_num() -- Get the legacy joystick count.
*
*   SYNOPSIS
*       static unsigned int get_joystick_num (void);
*
*   FUNCTION
*       Return the number of device opens counted by the last
*       initialization.
*
*   RESULT
*       Detected device count, at most two.
*
****************************************************************************
*
*/
static unsigned int get_joystick_num (void)
{
    return nr_joysticks;
}

/****i* januae-jd-linuxold/acquire_joy *************************************
*
*   NAME
*       acquire_joy() -- Accept a legacy joystick acquisition.
*
*   SYNOPSIS
*       static int acquire_joy (unsigned int num, int flags);
*
*   FUNCTION
*       Return success without opening a descriptor or checking the
*       requested index.
*
*   INPUTS
*       num - Ignored.
*       flags - Ignored.
*
*   RESULT
*       Always 1.
*
****************************************************************************
*
*/
static int acquire_joy (unsigned int num, int flags)
{
    return 1;
}

/****i* januae-jd-linuxold/unacquire_joy ***********************************
*
*   NAME
*       unacquire_joy() -- Release a legacy joystick acquisition.
*
*   SYNOPSIS
*       static void unacquire_joy (unsigned int num);
*
*   FUNCTION
*       Perform no operation; file descriptors are closed by
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

/****i* januae-jd-linuxold/read_joysticks **********************************
*
*   NAME
*       read_joysticks() -- Poll legacy joysticks.
*
*   SYNOPSIS
*       static void read_joysticks (void);
*
*   FUNCTION
*       Attempt one state read for each counted joystick.
*
*   NOTES
*       Read errors are ignored by the poll loop.
*
****************************************************************************
*
*/
static void read_joysticks (void)
{
    unsigned int i;
    for (i = 0; i < get_joystick_num(); i++)
	read_joy (i);
}

/****i* januae-jd-linuxold/get_joystick_name *******************************
*
*   NAME
*       get_joystick_name() -- Format a legacy joystick name.
*
*   SYNOPSIS
*       static const char *get_joystick_name (unsigned int joy);
*
*   FUNCTION
*       Format a one-based label and the selected device-node path in a
*       shared static character buffer.
*
*   INPUTS
*       joy - Zero-based joystick index.
*
*   RESULT
*       Pointer to a static buffer, overwritten by the next call.
*
*   NOTES
*       The function does not check that the selected device was opened.
*
****************************************************************************
*
*/
static const char *get_joystick_name (unsigned int joy)
{
    static char name[100];
    sprintf (name, "%d: %s%d", joy + 1, js_prefix, joy);
    return name;
}

/****i* januae-jd-linuxold/get_joystick_widget_num *************************
*
*   NAME
*       get_joystick_widget_num() -- Get the legacy widget count.
*
*   SYNOPSIS
*       static unsigned int get_joystick_widget_num (unsigned int joy);
*
*   FUNCTION
*       Report the fixed two-axis, two-button widget layout.
*
*   INPUTS
*       joy - Ignored.
*
*   RESULT
*       4.
*
****************************************************************************
*
*/
static unsigned int get_joystick_widget_num (unsigned int joy)
{
    return MAX_AXLES + MAX_BUTTONS;
}

/****i* januae-jd-linuxold/get_joystick_widget_type ************************
*
*   NAME
*       get_joystick_widget_type() -- Classify a legacy joystick widget.
*
*   SYNOPSIS
*       static int get_joystick_widget_type (unsigned int joy, unsigned int
*       num, char *name, uae_u32 *dummy);
*
*   FUNCTION
*       Interpret indices zero and one as axes and indices two and three as
*       buttons; optionally write a label.
*
*   INPUTS
*       joy - Ignored.
*       num - Widget index.
*       name - Optional writable buffer for the formatted label.
*       dummy - Unused.
*
*   RESULT
*       IDEV_WIDGET_AXIS, IDEV_WIDGET_BUTTON, or IDEV_WIDGET_NONE.
*
*   NOTES
*       The label buffer size is not checked.
*
****************************************************************************
*
*/
static int get_joystick_widget_type (unsigned int joy, unsigned int num, char *name, uae_u32 *dummy)
{
    if (num >= MAX_AXLES && num < MAX_AXLES+MAX_BUTTONS) {
	if (name)
	    sprintf (name, "Button %d", num + 1 - MAX_AXLES);
	return IDEV_WIDGET_BUTTON;
    } else if (num < MAX_AXLES) {
	if (name)
	    sprintf (name, "Axis %d", num + 1);
	return IDEV_WIDGET_AXIS;
    }
    return IDEV_WIDGET_NONE;
}

/****i* januae-jd-linuxold/get_joystick_widget_first ***********************
*
*   NAME
*       get_joystick_widget_first() -- Find the first legacy widget of a kind.
*
*   SYNOPSIS
*       static int get_joystick_widget_first (unsigned int joy, int type);
*
*   FUNCTION
*       Return the fixed index of the first axis or button.
*
*   INPUTS
*       joy - Ignored.
*       type - IDEV_WIDGET_AXIS or IDEV_WIDGET_BUTTON.
*
*   RESULT
*       0 for axes, 2 for buttons, or -1 for an unsupported type.
*
****************************************************************************
*
*/
static int get_joystick_widget_first (unsigned int joy, int type)
{
    switch (type) {
	case IDEV_WIDGET_BUTTON:
	    return FIRST_BUTTON;
	case IDEV_WIDGET_AXIS:
	    return FIRST_AXLE;
    }

    return -1;
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

/****i* januae-jd-linuxold/input_get_default_joystick **********************
*
*   NAME
*       input_get_default_joystick() -- Set default legacy joystick mappings.
*
*   SYNOPSIS
*       void input_get_default_joystick (struct uae_input_device *uid);
*
*   FUNCTION
*       Map two axes and three button events for each counted joystick to
*       alternating emulated ports, then enable entry zero.
*
*   INPUTS
*       uid - Writable input-device configuration array with at least one
*       entry.
*
*   NOTES
*       The third button mapping is configured even though the backend
*       reports only two physical buttons.
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
