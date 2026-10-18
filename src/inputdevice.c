 /*
  * UAE - The Un*x Amiga Emulator
  *
  * joystick/mouse emulation
  *
  * Copyright 2001-2006 Toni Wilen
  *
  * new fetures:
  * - very configurable (and very complex to configure :)
  * - supports multiple native input devices (joysticks and mice)
  * - supports mapping joystick/mouse buttons to keys and vice versa
  * - joystick mouse emulation (supports both ports)
  * - supports parallel port joystick adapter
  * - full cd32 pad support (supports both ports)
  * - fully backward compatible with old joystick/mouse configuration
  *
  */

//#define DONGLE_DEBUG

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "keyboard.h"
#include "keybuf.h"
#include "custom.h"
#include "xwin.h"
#include "drawing.h"
#include "memory.h"
#include "events.h"
#include "newcpu.h"
#include "autoconf.h"
#include "inputdevice.h"
#include "uae.h"
#include "p96.h"
#include "catweasel.h"
#include "debug.h"
#include "ar.h"
#include "gui.h"
#include "disk.h"
#include "audio.h"
#include "savestate.h"

#include <ctype.h>

#define DIR_LEFT 1
#define DIR_RIGHT 2
#define DIR_UP 4
#define DIR_DOWN 8

struct inputevent {
    const char *confname;
    const char *name;
    int allow_mask;
    int type;
    int unit;
    int data;
};

#define JOYBUTTON_1 0 /* fire/left mousebutton */
#define JOYBUTTON_2 1 /* 2nd/right mousebutton */
#define JOYBUTTON_3 2 /* 3rd/middle mousebutton */
#define JOYBUTTON_CD32_PLAY 3
#define JOYBUTTON_CD32_RWD 4
#define JOYBUTTON_CD32_FFW 5
#define JOYBUTTON_CD32_GREEN 6
#define JOYBUTTON_CD32_YELLOW 7
#define JOYBUTTON_CD32_RED 8
#define JOYBUTTON_CD32_BLUE 9

#define INPUTEVENT_JOY1_CD32_FIRST INPUTEVENT_JOY1_CD32_PLAY
#define INPUTEVENT_JOY2_CD32_FIRST INPUTEVENT_JOY2_CD32_PLAY
#define INPUTEVENT_JOY1_CD32_LAST INPUTEVENT_JOY1_CD32_BLUE
#define INPUTEVENT_JOY2_CD32_LAST INPUTEVENT_JOY2_CD32_BLUE

/* event masks */
#define AM_KEY 1 /* keyboard allowed */
#define AM_JOY_BUT 2 /* joystick buttons allowed */
#define AM_JOY_AXIS 4 /* joystick axis allowed */
#define AM_MOUSE_BUT 8 /* mouse buttons allowed */
#define AM_MOUSE_AXIS 16 /* mouse direction allowed */
#define AM_AF 32 /* supports autofire */
#define AM_INFO 64 /* information data for gui */
#define AM_DUMMY 128 /* placeholder */
#define AM_K (AM_KEY|AM_JOY_BUT|AM_MOUSE_BUT|AM_AF) /* generic button/switch */

/* event flags */
#define ID_FLAG_AUTOFIRE 1

#define DEFEVENT(A, B, C, D, E, F) {#A, B, C, D, E, F },
const struct inputevent events[] = {
{0, 0, AM_K, 0, 0, 0},
#include "inputevents.def"
{0, 0, 0, 0, 0, 0}
};
#undef DEFEVENT

static struct inputdevice_functions idev[3];

static int sublevdir[2][MAX_INPUT_SUB_EVENT];

#if 0
struct uae_input_device2 {
    uae_u32 buttonmask;
    int states[MAX_INPUT_DEVICE_EVENTS / 2];
};
#endif

static struct uae_input_device2 joysticks2[MAX_INPUT_DEVICES];
struct uae_input_device2 mice2[MAX_INPUT_DEVICES];

static uae_u8 mouse_settings_reset[MAX_INPUT_SETTINGS][MAX_INPUT_DEVICES];
static uae_u8 joystick_settings_reset[MAX_INPUT_SETTINGS][MAX_INPUT_DEVICES];

/****i* januae-inputdevice/isdevice ****************************************
*
*   NAME
*       isdevice() -- Determine whether an input device has assigned events.
*
*   SYNOPSIS
*       static int isdevice (const struct uae_input_device *id);
*
*   FUNCTION
*       Scan the configured event slots and subevent entries for a positive
*       event identifier.
*
*   INPUTS
*       id - Input device configuration to examine.
*
*   RESULT
*       Nonzero if at least one event is assigned; zero otherwise.
*
****************************************************************************
*
*/

static int isdevice (const struct uae_input_device *id)
{
    int i, j;
    for (i = 0; i < MAX_INPUT_DEVICE_EVENTS; i++) {
	for (j = 0; j < MAX_INPUT_SUB_EVENT; j++) {
	    if (id->eventid[i][j] > 0)
		return 1;
	}
    }
    return 0;
}

/****i* januae-inputdevice/inputdevice_uaelib ******************************
*
*   NAME
*       inputdevice_uaelib() -- Dispatch a named UAE input event from a
*       command.
*
*   SYNOPSIS
*       int inputdevice_uaelib (const char *s, const char *parm);
*
*   FUNCTION
*       Find a matching configuration event name and forward the parsed
*       numeric state to the input-event dispatcher.
*
*   INPUTS
*       s - Event configuration name; parm - Numeric state represented as a
*       string.
*
*   RESULT
*       One if the event name is recognized; zero otherwise.
*
****************************************************************************
*
*/

int inputdevice_uaelib (const char *s, const char *parm)
{
    int i;

    for (i = 1; events[i].name; i++) {
	if (!strcmp (s, events[i].confname)) {
	    handle_input_event (i, atol (parm), 1, 0);
	    return 1;
	}
    }
    return 0;
}

static struct uae_input_device *joysticks;
struct uae_input_device *mice;
static struct uae_input_device *keyboards;
static struct uae_input_device_kbr_default *keyboard_default;

static double mouse_axis[MAX_INPUT_DEVICES][MAX_INPUT_DEVICE_EVENTS];
static double oldm_axis[MAX_INPUT_DEVICES][MAX_INPUT_DEVICE_EVENTS];

static int mouse_x[MAX_INPUT_DEVICES], mouse_y[MAX_INPUT_DEVICE_EVENTS];
static int mouse_delta[MAX_INPUT_DEVICES][MAX_INPUT_DEVICE_EVENTS];
static int mouse_deltanoreset[MAX_INPUT_DEVICES][MAX_INPUT_DEVICE_EVENTS];
static int joybutton[MAX_INPUT_DEVICES];
static unsigned int joydir[MAX_INPUT_DEVICE_EVENTS];
static int joydirpot[MAX_INPUT_DEVICE_EVENTS][2];
static int mouse_frame_x[2], mouse_frame_y[2];

static int mouse_port[2];
static int cd32_shifter[2];
static int cd32_pad_enabled[2];
static int parport_joystick_enabled;
static int oldmx[4], oldmy[4];
static int oleft[4], oright[4], otop[4], obot[4];
static int potgo_hsync;

static int use_joysticks[MAX_INPUT_DEVICES];
static int use_mice[MAX_INPUT_DEVICES];
static int use_keyboards[MAX_INPUT_DEVICES];

#define INPUT_QUEUE_SIZE 16
struct input_queue_struct {
    int event, storedstate, state, max, framecnt, nextframecnt;
};
static struct input_queue_struct input_queue[INPUT_QUEUE_SIZE];

/****i* januae-inputdevice/out_config **************************************
*
*   NAME
*       out_config() -- Write a numbered input configuration property.
*
*   SYNOPSIS
*       static void out_config (FILE *f, int id, int num, const char *s1,
*       const char *s2);
*
*   FUNCTION
*       Serialize the setting number, device label, device index and
*       property value as a configuration line.
*
*   INPUTS
*       f - Output file; id - Setting identifier; num - Device number; s1 -
*       Property prefix; s2 - Property value.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void out_config (FILE *f, int id, int num, const char *s1, const char *s2)
{
    cfgfile_write (f, "input.%d.%s%d=%s\n", id, s1, num, s2);
}

/****i* januae-inputdevice/write_config2 ***********************************
*
*   NAME
*       write_config2() -- Serialize the mappings of one axis or button.
*
*   SYNOPSIS
*       static void write_config2 (FILE *f, int idnum, int i, int offset,
*       const char *tmp1, const struct uae_input_device *id);
*
*   FUNCTION
*       Enumerate the subevent assignments for one control, append named
*       event mappings and flags, and write the resulting configuration
*       property when populated.
*
*   INPUTS
*       f - Output file; idnum - Setting index; i - Control index; offset -
*       Event-array offset; tmp1 - Property prefix; id - Input device
*       configuration.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void write_config2 (FILE *f, int idnum, int i, int offset, const char *tmp1, const struct uae_input_device *id)
{
    char tmp2[200], *p;
    int event, got, j, k;
    const char *custom;

    p = tmp2;
    got = 0;
    for (j = 0; j < MAX_INPUT_SUB_EVENT; j++) {
	event = id->eventid[i + offset][j];
	custom = id->custom[i + offset][j];
	if (custom == NULL && event <= 0) {
	    for (k = j + 1; k < MAX_INPUT_SUB_EVENT; k++) {
		if (id->eventid[i + offset][k] > 0) break;
	    }
	    if (k == MAX_INPUT_SUB_EVENT)
		break;
	}
	if (p > tmp2) {
	    *p++ = ',';
	    *p = 0;
	}
	if (custom)
	    sprintf (p, "'%s'.%d", custom, id->flags[i + offset][j]);
	else if (event <= 0)
	    sprintf (p, "NULL");
	else
	    sprintf (p, "%s.%d", events[event].confname, id->flags[i + offset][j]);
	p += strlen (p);
    }
    if (p > tmp2)
	out_config (f, idnum, i, tmp1, tmp2);
}

/****i* januae-inputdevice/write_config ************************************
*
*   NAME
*       write_config() -- Write a configured joystick or mouse device.
*
*   SYNOPSIS
*       static void write_config (FILE *f, int idnum, int devnum, const char
*       *name, const struct uae_input_device *id, const struct
*       uae_input_device2 *id2);
*
*   FUNCTION
*       Skip devices without assignments; otherwise write the enabled state
*       followed by configured axes and buttons.
*
*   INPUTS
*       f - Output file; idnum - Setting index; devnum - Device index; name
*       - Device category; id - Event assignments; id2 - Auxiliary device
*       state.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void write_config (FILE *f, int idnum, int devnum, const char *name, const struct uae_input_device *id, const struct uae_input_device2 *id2)
{
    char tmp1[100];
    int i;

    if (!isdevice (id))
	return;
    cfgfile_write (f, "input.%d.%s.%d.disabled=%d\n", idnum, name, devnum, id->enabled ? 0 : 1);
    sprintf (tmp1, "%s.%d.axis.", name, devnum);
    for (i = 0; i < ID_AXIS_TOTAL; i++)
	write_config2 (f, idnum, i, ID_AXIS_OFFSET, tmp1, id);
    sprintf (tmp1, "%s.%d.button." ,name, devnum);
    for (i = 0; i < ID_BUTTON_TOTAL; i++)
	write_config2 (f, idnum, i, ID_BUTTON_OFFSET, tmp1, id);
}

/****i* januae-inputdevice/kbrlabel ****************************************
*
*   NAME
*       kbrlabel() -- Normalize a keyboard event label for configuration.
*
*   SYNOPSIS
*       static void kbrlabel (char *s);
*
*   FUNCTION
*       Uppercase the supplied label in place and replace spaces with
*       underscores.
*
*   INPUTS
*       s - Writable, zero-terminated label buffer.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void kbrlabel (char *s)
{
    while (*s) {
	*s = toupper(*s);
	if (*s == ' ') *s = '_';
	s++;
    }
}

/****i* januae-inputdevice/write_kbr_config ********************************
*
*   NAME
*       write_kbr_config() -- Write nondefault keyboard event assignments.
*
*   SYNOPSIS
*       static void write_kbr_config (FILE *f, int idnum, int devnum, const
*       struct uae_input_device *kbr);
*
*   FUNCTION
*       Compare keyboard mappings with the installed default translation
*       table and emit named properties for the configured key slots.
*
*   INPUTS
*       f - Output file; idnum - Setting index; devnum - Keyboard index; kbr
*       - Keyboard configuration.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       When no default keyboard translation table is installed, no keyboard
*       mappings are written.
*
****************************************************************************
*
*/

static void write_kbr_config (FILE *f, int idnum, int devnum, const struct uae_input_device *kbr)
{
    char tmp1[200], tmp2[200], tmp3[200], *p;
    int i, j, k, evt, skip;

    if (!keyboard_default)
	return;
    i = 0;
    while (i < MAX_INPUT_DEVICE_EVENTS && kbr->extra[i][0] >= 0) {
	skip = 0;
	k = 0;
	while (keyboard_default[k].scancode >= 0) {
	    if (keyboard_default[k].scancode == kbr->extra[i][0]) {
		skip = 1;
		for (j = 1; j < MAX_INPUT_SUB_EVENT; j++) {
		    if (kbr->flags[i][j] || kbr->eventid[i][j] > 0)
			skip = 0;
		}
		if (keyboard_default[k].event != kbr->eventid[i][0] || kbr->flags[i][0] != 0)
		    skip = 0;
		break;
	    }
	    k++;
	}
	if (kbr->eventid[i][0] == 0 && kbr->flags[i][0] == 0 && keyboard_default[k].scancode < 0)
	    skip = 1;
	if (skip) {
	    i++;
	    continue;
	}
	p = tmp2;
	p[0] = 0;
	for (j = 0; j < MAX_INPUT_SUB_EVENT; j++) {
	    const char *custom = kbr->custom[i][j];
	    evt = kbr->eventid[i][j];
	    if (custom == NULL && evt <= 0) {
		for (k = j + 1; k < MAX_INPUT_SUB_EVENT; k++) {
		    if (kbr->eventid[i][k] > 0) break;
		}
		if (k == MAX_INPUT_SUB_EVENT)
		    break;
	    }
	    if (p > tmp2) {
		*p++ = ',';
		*p = 0;
	    }
	    if (custom)
		sprintf (p, "'%s'.%d", custom, kbr->flags[i][j]);
	    else if (evt > 0)
		sprintf (p, "%s.%d", events[evt].confname, kbr->flags[i][j]);
	    else
		strcat (p, "NULL");
	    p += strlen(p);
	}
	sprintf (tmp3, "%d", kbr->extra[i][0]);
	kbrlabel (tmp3);
	sprintf (tmp1, "keyboard.%d.button.%s", devnum, tmp3);
	cfgfile_write (f, "input.%d.%s=%s\n", idnum, tmp1, tmp2[0] ? tmp2 : "NULL");
	i++;
    }
}

/****i* januae-inputdevice/write_inputdevice_config ************************
*
*   NAME
*       write_inputdevice_config() -- Save the emulator input-device
*       configuration.
*
*   SYNOPSIS
*       void write_inputdevice_config (const struct uae_prefs *p, FILE *f);
*
*   FUNCTION
*       Write global input sensitivity, deadzone, speed and autofire
*       settings, then serialize joystick, mouse and keyboard mappings for
*       each configuration set.
*
*   INPUTS
*       p - Preferences to serialize; f - Destination configuration file.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void write_inputdevice_config (const struct uae_prefs *p, FILE *f)
{
    int i, id;

    cfgfile_write (f, "input.config=%d\n", p->input_selected_setting);
    cfgfile_write (f, "input.joymouse_speed_analog=%d\n", p->input_joymouse_multiplier);
    cfgfile_write (f, "input.joymouse_speed_digital=%d\n", p->input_joymouse_speed);
    cfgfile_write (f, "input.joymouse_deadzone=%d\n", p->input_joymouse_deadzone);
    cfgfile_write (f, "input.joystick_deadzone=%d\n", p->input_joystick_deadzone);
    cfgfile_write (f, "input.mouse_speed=%d\n", p->input_mouse_speed);
    cfgfile_write (f, "input.autofire=%d\n", p->input_autofire_framecnt);
    for (id = 1; id <= MAX_INPUT_SETTINGS; id++) {
	for (i = 0; i < MAX_INPUT_DEVICES; i++)
	    write_config (f, id, i, "joystick", &p->joystick_settings[id][i], &joysticks2[i]);
	for (i = 0; i < MAX_INPUT_DEVICES; i++)
	    write_config (f, id, i, "mouse", &p->mouse_settings[id][i], &mice2[i]);
	for (i = 0; i < MAX_INPUT_DEVICES; i++)
	    write_kbr_config (f, id, i, &p->keyboard_settings[id][i]);
    }
}

/****i* januae-inputdevice/getnum ******************************************
*
*   NAME
*       getnum() -- Consume a numeric input configuration field.
*
*   SYNOPSIS
*       static int getnum (const char **pp);
*
*   FUNCTION
*       Read an integer from the current text position and advance past its
*       period or comma separator.
*
*   INPUTS
*       pp - In/out pointer to the current configuration text.
*
*   RESULT
*       Parsed integer value.
*
****************************************************************************
*
*/

static int getnum (const char **pp)
{
    const char *p = *pp;
    int v = atol (p);

    while (*p != 0 && *p !='.' && *p != ',') p++;
    if (*p == '.' || *p == ',') p++;
    *pp = p;
    return v;
}

/****i* januae-inputdevice/getstring ***************************************
*
*   NAME
*       getstring() -- Consume a string input configuration field.
*
*   SYNOPSIS
*       static char *getstring (const char **pp);
*
*   FUNCTION
*       Copy characters up to the next period, comma or terminator into a
*       reusable static buffer, then advance the caller cursor.
*
*   INPUTS
*       pp - In/out pointer to the current configuration text.
*
*   RESULT
*       Pointer to the internal field buffer, or NULL for an empty remaining
*       string.
*
*   NOTES
*       A subsequent call overwrites the same static buffer.
*
****************************************************************************
*
*/

static char *getstring (const char **pp)
{
    int i;
    static char str[1000];
    const char *p = *pp;

    if (*p == 0)
	return 0;
    i = 0;
    while (*p != 0 && *p !='.' && *p != ',')
	str[i++] = *p++;
    if (*p == '.' || *p == ',')
	p++;
    str[i] = 0;
    *pp = p;
    return str;
}

/****i* januae-inputdevice/reset_inputdevice_config ************************
*
*   NAME
*       reset_inputdevice_config() -- Clear input-device reset markers.
*
*   SYNOPSIS
*       void reset_inputdevice_config (struct uae_prefs *pr);
*
*   FUNCTION
*       Zero the joystick and mouse configuration-reset tracking arrays.
*
*   INPUTS
*       pr - Preferences parameter, not accessed by this implementation.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void reset_inputdevice_config (struct uae_prefs *pr)
{
    memset (joystick_settings_reset, 0, sizeof (joystick_settings_reset));
    memset (mouse_settings_reset, 0, sizeof (mouse_settings_reset));
}

/****i* januae-inputdevice/clear_id ****************************************
*
*   NAME
*       clear_id() -- Reset one input device assignment table.
*
*   SYNOPSIS
*       static void clear_id (struct uae_input_device *id);
*
*   FUNCTION
*       Release custom command strings in nondebug builds, clear the device
*       mapping structure, and enable the resulting empty device.
*
*   INPUTS
*       id - Input device configuration to clear.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void clear_id (struct uae_input_device *id)
{
#ifndef _DEBUG
    int i, j;
    for (i = 0; i < MAX_INPUT_DEVICE_EVENTS; i++) {
	for (j = 0; j < MAX_INPUT_SUB_EVENT; j++)
	    free (id->custom[i][j]);
    }
#endif
    memset (id, 0, sizeof (struct uae_input_device));
    id->enabled = 1;
}

/****i* januae-inputdevice/read_inputdevice_config *************************
*
*   NAME
*       read_inputdevice_config() -- Apply a serialized input configuration
*       property.
*
*   SYNOPSIS
*       void read_inputdevice_config (struct uae_prefs *pr, const char
*       *option, const char *value);
*
*   FUNCTION
*       Parse a property beneath the input configuration namespace, update
*       global control settings or device event mappings, and store custom
*       subevent commands where provided.
*
*   INPUTS
*       pr - Destination preferences; option - Full configuration key; value
*       - Encoded property value.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Unrecognized or invalid properties may be ignored without a separate
*       error result.
*
****************************************************************************
*
*/

void read_inputdevice_config (struct uae_prefs *pr, const char *option, const char *value)
{
    struct uae_input_device *id = 0;
    const struct inputevent *ie;
    int devnum, num, button = 0, joystick, flags, i, subnum, idnum, keynum = 0;
    int mask;
    const char *p, *p2;
    char *custom;

    option += 6; /* "input." */
    p = getstring (&option);
    if (!strcasecmp (p, "config"))
	pr->input_selected_setting = atol (value);
    if (!strcasecmp (p, "joymouse_speed_analog"))
	pr->input_joymouse_multiplier = atol (value);
    if (!strcasecmp (p, "joymouse_speed_digital"))
	pr->input_joymouse_speed = atol (value);
    if (!strcasecmp (p, "joystick_deadzone"))
	pr->input_joystick_deadzone = atol (value);
    if (!strcasecmp (p, "joymouse_deadzone"))
	pr->input_joymouse_deadzone = atol (value);
    if (!strcasecmp (p, "mouse_speed"))
	pr->input_mouse_speed = atol (value);
    if (!strcasecmp (p, "autofire"))
	pr->input_autofire_framecnt = atol (value);
    idnum = atol (p);
    if (idnum <= 0 || idnum > MAX_INPUT_SETTINGS)
	return;
    if (memcmp (option, "mouse.", 6) == 0) {
	p = option + 6;
	devnum = getnum (&p);
	if (devnum < 0 || devnum >= MAX_INPUT_DEVICES)
	    return;
	id = &pr->mouse_settings[idnum][devnum];
	if (!mouse_settings_reset[idnum][devnum])
	    clear_id (id);
	mouse_settings_reset[idnum][devnum] = 1;
	joystick = 0;
    } else if (memcmp (option, "joystick.", 9) == 0) {
	p = option + 9;
	devnum = getnum (&p);
	if (devnum < 0 || devnum >= MAX_INPUT_DEVICES)
	    return;
	id = &pr->joystick_settings[idnum][devnum];
	if (!joystick_settings_reset[idnum][devnum])
	    clear_id (id);
	joystick_settings_reset[idnum][devnum] = 1;
	joystick = 1;
    } else if (memcmp (option, "keyboard.", 9) == 0) {
	joystick = -1;
	p = option + 9;
	devnum = getnum (&p);
	if (devnum < 0 || devnum >= MAX_INPUT_DEVICES)
	    return;
	id = &pr->keyboard_settings[idnum][devnum];
    }
    if (!id)
	return;
    p2 = getstring (&p);
    if (!p2)
	return;
    if (!strcmp (p2, "disabled")) {
	int disabled;
	p = value;
	disabled = getnum (&p);
	id->enabled = disabled == 0 ? 1 : 0;
	return;
    }

    if (joystick < 0) {
	num = getnum (&p);
	keynum = 0;
	while (id->extra[keynum][0] >= 0) {
	    if (id->extra[keynum][0] == num)
		break;
	    keynum++;
	}
	if (id->extra[keynum][0] < 0)
	    return;
    } else {
	button = -1;
	if (!strcmp (p2, "axis"))
	    button = 0;
	else if(!strcmp (p2, "button"))
	    button = 1;
	if (button < 0)
	    return;
	num = getnum (&p);
    }
    p = value;

    custom = NULL;
    for (subnum = 0; subnum < MAX_INPUT_SUB_EVENT; subnum++) {
	free (custom);
	custom = NULL;
	p2 = getstring (&p);
	if (!p2)
	    break;
	i = 1;
	while (events[i].name) {
	    if (!strcmp (events[i].confname, p2))
		break;
	    i++;
	}
	ie = &events[i];
	if (!ie->name) {
	    ie = &events[0];
	    if (strlen (p2) > 2 && p2[0] == '\'' && p2[strlen (p2) - 1] == '\'') {
		custom = my_strdup (p2 + 1);
		custom[strlen (custom) - 1] = 0;
	    }
	}
	flags = getnum (&p);
	if (custom == NULL && ie->name == NULL) {
	    if (!strcmp(p2, "NULL")) {
		id->eventid[keynum][subnum] = 0;
		id->flags[keynum][subnum] = 0;
	    }
	    continue;
	}
	if (joystick < 0) {
	    if (!(ie->allow_mask & AM_K))
		continue;
	    id->eventid[keynum][subnum] = ie - events;
	    id->flags[keynum][subnum] = flags;
	    free (id->custom[keynum][subnum]);
	    id->custom[keynum][subnum] = custom;
	} else  if (button) {
	    if (joystick)
		mask = AM_JOY_BUT;
	    else
		mask = AM_MOUSE_BUT;
	    if (!(ie->allow_mask & mask))
		continue;
	    id->eventid[num + ID_BUTTON_OFFSET][subnum] = ie - events;
	    id->flags[num + ID_BUTTON_OFFSET][subnum] = flags;
	    free (id->custom[num + ID_BUTTON_OFFSET][subnum]);
	    id->custom[num + ID_BUTTON_OFFSET][subnum] = custom;
	} else {
	    if (joystick)
		mask = AM_JOY_AXIS;
	    else
		mask = AM_MOUSE_AXIS;
	    if (!(ie->allow_mask & mask))
		continue;
	    id->eventid[num + ID_AXIS_OFFSET][subnum] = ie - events;
	    id->flags[num + ID_AXIS_OFFSET][subnum] = flags;
	    free (id->custom[num + ID_AXIS_OFFSET][subnum]);
	    id->custom[num + ID_AXIS_OFFSET][subnum] = custom;
	}
	custom = NULL;
    }
    free (custom);
}

static int ievent_alive = 0;
static int lastmx, lastmy;

/****i* januae-inputdevice/mousehack_alive *********************************
*
*   NAME
*       mousehack_alive() -- Report recent guest mouse-integration activity.
*
*   SYNOPSIS
*       int mousehack_alive (void);
*
*   FUNCTION
*       Test whether the frame-based guest mouse activity counter remains
*       positive.
*
*   INPUTS
*       None.
*
*   RESULT
*       Nonzero while the activity counter is positive; zero otherwise.
*
****************************************************************************
*
*/

int mousehack_alive (void)
{
    return ievent_alive > 0;
}

/****i* januae-inputdevice/mousehack_enable ********************************
*
*   NAME
*       mousehack_enable() -- Enable the guest mouse helper in resident
*       code.
*
*   SYNOPSIS
*       static void mousehack_enable (void);
*
*   FUNCTION
*       If mouse integration is supported and the resident helper flag is
*       not yet set, write its enable byte in the emulator resident area.
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

static void mousehack_enable (void)
{
    if (!mousehack_allowed ())
	return;
    if (rtarea[get_long (RTAREA_BASE + 40) + 12 - 1])
	return;
    rtarea[get_long (RTAREA_BASE + 40) + 12 - 1] = 1;
}

/****i* januae-inputdevice/mousehack_helper ********************************
*
*   NAME
*       mousehack_helper() -- Publish the current guest-relative mouse
*       coordinates.
*
*   SYNOPSIS
*       static void mousehack_helper (void);
*
*   FUNCTION
*       Derive coordinates from the latest host mouse position, account for
*       Picasso96 offsets when active, and update the resident mouse
*       coordinate fields.
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

static void mousehack_helper (void)
{
    int mousexpos, mouseypos;
    uae_u8 *p;

    if (!mousehack_allowed ())
	return;
#ifdef PICASSO96
    if (picasso_on) {
	mousexpos = lastmx - picasso96_state.XOffset;
	mouseypos = lastmy - picasso96_state.YOffset;
    } else
#endif
    {
	mouseypos = coord_native_to_amiga_y (lastmy) << 1;
	mousexpos = coord_native_to_amiga_x (lastmx);
    }
    if (!mousehack_allowed ())
	mousexpos = mouseypos = 0;
    p = rtarea + get_long (RTAREA_BASE + 40) + 12;
    p[0] = mousexpos >> 8;
    p[1] = mousexpos;
    p[2] = mouseypos >> 8;
    p[3] = mouseypos;
}

/****i* januae-inputdevice/adjust ******************************************
*
*   NAME
*       adjust() -- Clamp a signed movement value to the joystick range.
*
*   SYNOPSIS
*       STATIC_INLINE int adjust (int val);
*
*   FUNCTION
*       Restrict a movement component to the inclusive range from minus 127
*       to 127.
*
*   INPUTS
*       val - Movement component to constrain.
*
*   RESULT
*       Clamped movement component.
*
****************************************************************************
*
*/

STATIC_INLINE int adjust (int val)
{
    if (val > 127)
	return 127;
    else if (val < -127)
	return -127;
    return val;
}

/****i* januae-inputdevice/getbuttonstate **********************************
*
*   NAME
*       getbuttonstate() -- Read the emulated joystick button bit.
*
*   SYNOPSIS
*       int getbuttonstate (int joy, int button);
*
*   FUNCTION
*       Mask the requested button position from the stored joystick button
*       state.
*
*   INPUTS
*       joy - Joystick index; button - Button bit index.
*
*   RESULT
*       Masked button bit; zero when clear.
*
****************************************************************************
*
*/

int getbuttonstate (int joy, int button)
{
    return joybutton[joy] & (1 << button);
}

/****i* januae-inputdevice/mouseupdate *************************************
*
*   NAME
*       mouseupdate() -- Integrate pending mouse movement for the frame.
*
*   SYNOPSIS
*       static void mouseupdate (int pct);
*
*   FUNCTION
*       Apply the requested percentage of pending mouse deltas, update
*       emulated coordinates, and consume resettable deltas while preserving
*       the frame reference positions.
*
*   INPUTS
*       pct - Percentage of accumulated relative movement to integrate.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void mouseupdate (int pct)
{
    int v, i;

    for (i = 0; i < 2; i++) {

	v = mouse_delta[i][0] * pct / 100;
	mouse_x[i] += v;
	if (!mouse_deltanoreset[i][0])
	    mouse_delta[i][0] -= v;

	v = mouse_delta[i][1] * pct / 100;
	mouse_y[i] += v;
	if (!mouse_deltanoreset[i][1])
	    mouse_delta[i][1] -= v;

	v = mouse_delta[i][2] * pct / 100;
	if (v > 0)
	    record_key (0x7a << 1);
	else if (v < 0)
	    record_key (0x7b << 1);
	if (!mouse_deltanoreset[i][2])
	    mouse_delta[i][2] = 0;

	if (mouse_frame_x[i] - mouse_x[i] > 127)
	    mouse_x[i] = mouse_frame_x[i] - 127;
	if (mouse_frame_x[i] - mouse_x[i] < -127)
	    mouse_x[i] = mouse_frame_x[i] + 127;

	if (mouse_frame_y[i] - mouse_y[i] > 127)
	    mouse_y[i] = mouse_frame_y[i] - 127;
	if (mouse_frame_y[i] - mouse_y[i] < -127)
	    mouse_y[i] = mouse_frame_y[i] + 127;

	if (pct == 100) {
	    if (!mouse_deltanoreset[i][0])
		mouse_delta[i][0] = 0;
	    if (!mouse_deltanoreset[i][1])
		mouse_delta[i][1] = 0;
	    if (!mouse_deltanoreset[i][2])
		mouse_delta[i][2] = 0;
	    mouse_frame_x[i] = mouse_x[i];
	    mouse_frame_y[i] = mouse_y[i];
	}

    }
}

static unsigned int input_read, input_vpos;

/****i* januae-inputdevice/readinput ***************************************
*
*   NAME
*       readinput() -- Poll the host pointing-device inputs at scan
*       intervals.
*
*   SYNOPSIS
*       static void readinput (void);
*
*   FUNCTION
*       Read the joystick and mouse backends when the current video position
*       crosses the polling boundary, then integrate mouse movement and
*       update poll bookkeeping.
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

static void readinput (void)
{
    if (!input_read && (vpos & ~31) != (input_vpos & ~31)) {
	idev[IDTYPE_JOYSTICK].read ();
	idev[IDTYPE_MOUSE].read ();
	mouseupdate ((vpos - input_vpos) * 100 / maxvpos);
	input_vpos = vpos;
    }
    if (input_read) {
	input_vpos = vpos;
	input_read = 0;
    }
}

/****i* januae-inputdevice/getjoystate *************************************
*
*   NAME
*       getjoystate() -- Construct an emulated joystick position register
*       value.
*
*   SYNOPSIS
*       int getjoystate (int joy);
*
*   FUNCTION
*       Update host input, combine configured joystick directions with mouse
*       quadrature coordinates where selected, and encode the resulting
*       JOYDAT bits.
*
*   INPUTS
*       joy - Emulated joystick port.
*
*   RESULT
*       Encoded joystick position and direction value.
*
****************************************************************************
*
*/

int getjoystate (int joy)
{
    int left = 0, right = 0, top = 0, bot = 0;
    uae_u16 v = 0;

    readinput ();
    if (joydir[joy] & DIR_LEFT)
	left = 1;
    if (joydir[joy] & DIR_RIGHT)
	right = 1;
    if (joydir[joy] & DIR_UP)
	top = 1;
    if (joydir[joy] & DIR_DOWN)
	bot = 1;
    v = (uae_u8)mouse_x[joy] | (mouse_y[joy] << 8);
    if (left || right || top || bot || !mouse_port[joy]) {
	if (left)
	    top = !top;
	if (right)
	    bot = !bot;
	v &= ~0x0303;
	v |= bot | (right << 1) | (top << 8) | (left << 9);
    }
#ifdef DONGLE_DEBUG
    if (notinrom ())
	write_log ("JOY%dDAT %04.4X %s\n", joy, v, debuginfo (0));
#endif
    return v;
}

/****i* januae-inputdevice/JOY0DAT *****************************************
*
*   NAME
*       JOY0DAT() -- Read the first emulated joystick data register.
*
*   SYNOPSIS
*       uae_u16 JOY0DAT (void);
*
*   FUNCTION
*       Return the current JOYDAT encoding for emulated port zero.
*
*   INPUTS
*       None.
*
*   RESULT
*       Sixteen-bit JOY0DAT value.
*
****************************************************************************
*
*/

uae_u16 JOY0DAT (void)
{
    return getjoystate (0);
}

/****i* januae-inputdevice/JOY1DAT *****************************************
*
*   NAME
*       JOY1DAT() -- Read the second emulated joystick data register.
*
*   SYNOPSIS
*       uae_u16 JOY1DAT (void);
*
*   FUNCTION
*       Return the current JOYDAT encoding for emulated port one.
*
*   INPUTS
*       None.
*
*   RESULT
*       Sixteen-bit JOY1DAT value.
*
****************************************************************************
*
*/

uae_u16 JOY1DAT (void)
{
    return getjoystate (1);
}

/****i* januae-inputdevice/JOYTEST *****************************************
*
*   NAME
*       JOYTEST() -- Set high-order mouse counter bits from the test
*       register.
*
*   SYNOPSIS
*       void JOYTEST (uae_u16 v);
*
*   FUNCTION
*       Preserve the low two bits of both emulated mouse counters and
*       replace their remaining bits using the corresponding fields of the
*       supplied test value.
*
*   INPUTS
*       v - Emulated JOYTEST register value.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void JOYTEST (uae_u16 v)
{
    mouse_x[0] &= 3;
    mouse_y[0] &= 3;
    mouse_x[1] &= 3;
    mouse_y[1] &= 3;
    mouse_x[0] |= v & 0xFC;
    mouse_x[1] |= v & 0xFC;
    mouse_y[0] |= (v >> 8) & 0xFC;
    mouse_y[1] |= (v >> 8) & 0xFC;
    mouse_frame_x[0] = mouse_x[0];
    mouse_frame_y[0] = mouse_y[0];
    mouse_frame_x[1] = mouse_x[1];
    mouse_frame_y[1] = mouse_y[1];
}

/****i* januae-inputdevice/parconvert **************************************
*
*   NAME
*       parconvert() -- Apply joystick directions to parallel-port input
*       bits.
*
*   SYNOPSIS
*       static uae_u8 parconvert (uae_u8 v, int jd, int shift);
*
*   FUNCTION
*       Clear direction-associated bits in the supplied port value for each
*       asserted joystick direction.
*
*   INPUTS
*       v - Initial input value; jd - Joystick direction mask; shift -
*       Output bit offset.
*
*   RESULT
*       Parallel-port value with active joystick direction bits cleared.
*
****************************************************************************
*
*/

static uae_u8 parconvert (uae_u8 v, int jd, int shift)
{
    if (jd & DIR_UP)
	v &= ~(1 << shift);
    if (jd & DIR_DOWN)
	v &= ~(2 << shift);
    if (jd & DIR_LEFT)
	v &= ~(4 << shift);
    if (jd & DIR_RIGHT)
	v &= ~(8 << shift);
    return v;
}

/* io-pins floating: dir=1 -> return data, dir=0 -> always return 1 */
/****i* januae-inputdevice/handle_parport_joystick *************************
*
*   NAME
*       handle_parport_joystick() -- Read emulated parallel-port joystick
*       lines.
*
*   SYNOPSIS
*       uae_u8 handle_parport_joystick (int port, uae_u8 pra, uae_u8 dra);
*
*   FUNCTION
*       Combine port data with data-direction bits and, when parallel
*       joysticks are enabled, apply the corresponding direction and button
*       states.
*
*   INPUTS
*       port - Parallel-port register selector; pra - Port data; dra - Data
*       direction.
*
*   RESULT
*       Emulated parallel-port input value.
*
*   NOTES
*       An unsupported port selector terminates execution through abort().
*
****************************************************************************
*
*/

uae_u8 handle_parport_joystick (int port, uae_u8 pra, uae_u8 dra)
{
    uae_u8 v;
    switch (port)
    {
	case 0:
	v = (pra & dra) | (dra ^ 0xff);
	if (parport_joystick_enabled) {
	    v = parconvert (v, joydir[2], 0);
	    v = parconvert (v, joydir[3], 4);
	}
	return v;
	case 1:
	v = ((pra & dra) | (dra ^ 0xff)) & 0x7;
	if (parport_joystick_enabled) {
	    if (getbuttonstate (2, 0)) v &= ~4;
	    if (getbuttonstate (3, 0)) v &= ~1;
	}
	return v;
	default:
	abort ();
	return 0;
    }
}

/****i* januae-inputdevice/handle_joystick_buttons *************************
*
*   NAME
*       handle_joystick_buttons() -- Construct the emulated joystick
*       fire-button mask.
*
*   SYNOPSIS
*       uae_u8 handle_joystick_buttons (uae_u8 dra);
*
*   FUNCTION
*       Read ordinary or CD32-specific button states and combine them with
*       the relevant POTGO line directions.
*
*   INPUTS
*       dra - CIA port data-direction register.
*
*   RESULT
*       Encoded fire-button mask for the first two emulated ports.
*
****************************************************************************
*
*/

uae_u8 handle_joystick_buttons (uae_u8 dra)
{
    uae_u8 but = 0;
    int i;

    for (i = 0; i < 2; i++) {
	if (cd32_pad_enabled[i]) {
	    uae_u16 p5dir = 0x0200 << (i * 4);
	    uae_u16 p5dat = 0x0100 << (i * 4);
	    but |= 0x40 << i;
	    /* Red button is connected to fire when p5 is 1 or floating */
	    if (!(potgo_value & p5dir) || ((potgo_value & p5dat) && (potgo_value & p5dir))) {
		if (getbuttonstate (i, JOYBUTTON_CD32_RED))
		    but &= ~(0x40 << i);
	    }
	} else if (!getbuttonstate (i, JOYBUTTON_1)) {
	    but |= 0x40 << i;
	}
    }
    return but;
}

/* joystick 1 button 1 is used as a output for incrementing shift register */
/****i* januae-inputdevice/handle_cd32_joystick_cia ************************
*
*   NAME
*       handle_cd32_joystick_cia() -- Advance emulated CD32 controller shift
*       registers.
*
*   SYNOPSIS
*       void handle_cd32_joystick_cia (uae_u8 pra, uae_u8 dra);
*
*   FUNCTION
*       Detect changes on the CIA button output lines and update each
*       enabled CD32 controller serial-button shift position.
*
*   INPUTS
*       pra - CIA port data; dra - CIA port direction.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void handle_cd32_joystick_cia (uae_u8 pra, uae_u8 dra)
{
    static int oldstate[2];
    int i;

    for (i = 0; i < 2; i++) {
	uae_u8 but = 0x40 << i;
	uae_u16 p5dir = 0x0200 << (i * 4); /* output enable P5 */
	uae_u16 p5dat = 0x0100 << (i * 4); /* data P5 */
	if (!(potgo_value & p5dir) || !(potgo_value & p5dat)) {
	    if ((dra & but) && (pra & but) != oldstate[i]) {
		if (!(pra & but)) {
		    cd32_shifter[i]--;
		    if (cd32_shifter[i] < 0)
			cd32_shifter[i] = 0;
		}
	    }
	}
	oldstate[i] = pra & but;
    }
}

/* joystick port 1 button 2 is input for button state */
/****i* januae-inputdevice/handle_joystick_potgor **************************
*
*   NAME
*       handle_joystick_potgor() -- Apply joystick and CD32 button states to
*       POTGOR.
*
*   SYNOPSIS
*       static uae_u16 handle_joystick_potgor (uae_u16 potgor);
*
*   FUNCTION
*       Resolve emulated P5 and P9 input levels from ordinary joystick
*       buttons and the CD32 serial-button state.
*
*   INPUTS
*       potgor - Initial POTGOR register state.
*
*   RESULT
*       POTGOR value with controller input levels applied.
*
****************************************************************************
*
*/

static uae_u16 handle_joystick_potgor (uae_u16 potgor)
{
    int i;

    for (i = 0; i < 2; i++) {
	uae_u16 p9dir = 0x0800 << (i * 4); /* output enable P9 */
	uae_u16 p9dat = 0x0400 << (i * 4); /* data P9 */
	uae_u16 p5dir = 0x0200 << (i * 4); /* output enable P5 */
	uae_u16 p5dat = 0x0100 << (i * 4); /* data P5 */

	if (mouse_port[i]) {
	    /* official Commodore mouse has pull-up resistors in button lines
	     * NOTE: 3rd party mice may not have pullups! */
	    if (!(potgo_value & p5dir))
		potgor |= p5dat;
	    if (!(potgo_value & p9dir))
		potgor |= p9dat;
	}
	if (potgo_hsync < 0) {
	    /* first 10 or so lines after potgo has started
	     * forces input-lines to zero
	     */
	    if (!(potgo_value & p5dir))
		potgor &= ~p5dat;
	    if (!(potgo_value & p9dir))
		potgor &= ~p9dat;
	}

	if (cd32_pad_enabled[i]) {
	    /* p5 is floating in input-mode */
	    potgor &= ~p5dat;
	    potgor |= potgo_value & p5dat;
	    if (!(potgo_value & p9dir))
		potgor |= p9dat;
	    /* P5 output and 1 -> shift register is kept reset (Blue button) */
	    if ((potgo_value & p5dir) && (potgo_value & p5dat))
		cd32_shifter[i] = 8;
	    /* shift at 1 == return one, >1 = return button states */
	    if (cd32_shifter[i] == 0)
		potgor &= ~p9dat; /* shift at zero == return zero */
	    if (cd32_shifter[i] >= 2 && (joybutton[i] & ((1 << JOYBUTTON_CD32_PLAY) << (cd32_shifter[i] - 2))))
		potgor &= ~p9dat;
	} else {
	    if (getbuttonstate (i, JOYBUTTON_3))
		potgor &= ~p5dat;
	    if (getbuttonstate (i, JOYBUTTON_2))
		potgor &= ~p9dat;
	}
    }
    return potgor;
}

uae_u16 potgo_value;
static uae_u16 potdats[2];
static int inputdelay;

/****i* januae-inputdevice/inputdevice_hsync *******************************
*
*   NAME
*       inputdevice_hsync() -- Update emulated potentiometers and delayed
*       input reads.
*
*   SYNOPSIS
*       void inputdevice_hsync (void);
*
*   FUNCTION
*       Advance horizontal-sync potentiometer timing, refresh POTDAT
*       counters and schedule deferred joystick or keyboard polling when its
*       delay expires.
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

void inputdevice_hsync (void)
{
    int joy;

    for (joy = 0; joy < 2; joy++) {
	if (potgo_hsync >= 0) {
	    int active;

	    active = 0;
	    if ((potgo_value >> 9) & 1) /* output? */
		active = ((potgo_value >> 8) & 1) ? 0 : 1;
	    if (potgo_hsync < joydirpot[joy][0])
		active = 1;
	    if (getbuttonstate (joy, JOYBUTTON_3))
		active = 1;
	    if (active)
		potdats[joy] = ((potdats[joy] + 1) & 0xFF) | (potdats[joy] & 0xFF00);

	    active = 0;
	    if ((potgo_value >> 11) & 1) /* output? */
		active = ((potgo_value >> 10) & 1) ? 0 : 1;
	    if (potgo_hsync < joydirpot[joy][1])
		active = 1;
	    if (getbuttonstate (joy, JOYBUTTON_2))
		active = 1;
	    if (active)
		potdats[joy] += 0x100;
	}
    }
    potgo_hsync++;
    if (potgo_hsync > 255)
	potgo_hsync = 255;


#ifdef CATWEASEL
    catweasel_hsync ();
#endif
    if (inputdelay > 0) {
	inputdelay--;
	if (inputdelay == 0) {
	    idev[IDTYPE_JOYSTICK].read ();
	    idev[IDTYPE_KEYBOARD].read ();
	}
    }
}

/****i* januae-inputdevice/POT0DAT *****************************************
*
*   NAME
*       POT0DAT() -- Read the first emulated potentiometer data register.
*
*   SYNOPSIS
*       uae_u16 POT0DAT (void);
*
*   FUNCTION
*       Return the stored POTDAT counter value for the first joystick port.
*
*   INPUTS
*       None.
*
*   RESULT
*       Sixteen-bit POT0DAT value.
*
****************************************************************************
*
*/

uae_u16 POT0DAT (void)
{
    return potdats[0];
}
/****i* januae-inputdevice/POT1DAT *****************************************
*
*   NAME
*       POT1DAT() -- Read the second emulated potentiometer data register.
*
*   SYNOPSIS
*       uae_u16 POT1DAT (void);
*
*   FUNCTION
*       Return the stored POTDAT counter value for the second joystick port.
*
*   INPUTS
*       None.
*
*   RESULT
*       Sixteen-bit POT1DAT value.
*
****************************************************************************
*
*/

uae_u16 POT1DAT (void)
{
    return potdats[1];
}

/* direction=input, data pin floating, last connected logic level or previous status
 *                  written when direction was ouput
 *                  otherwise it is currently connected logic level.
 * direction=output, data pin is current value, forced to zero if joystick button is pressed
 * it takes some tens of microseconds before data pin changes state
 */

/****i* januae-inputdevice/POTGO *******************************************
*
*   NAME
*       POTGO() -- Update potentiometer directions and output levels.
*
*   SYNOPSIS
*       void POTGO (uae_u16 v);
*
*   FUNCTION
*       Merge incoming POTGO direction and data bits, update CD32
*       serial-button state, and reset potentiometer counters when the start
*       bit is set.
*
*   INPUTS
*       v - New POTGO register value.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void POTGO (uae_u16 v)
{
    int i;

#ifdef DONGLE_DEBUG
    if (notinrom ())
	write_log ("POTGO %04.4X %s\n", v, debuginfo(0));
#endif
    potgo_value = potgo_value & 0x5500; /* keep state of data bits */
    potgo_value |= v & 0xaa00; /* get new direction bits */
    for (i = 0; i < 8; i += 2) {
	uae_u16 dir = 0x0200 << i;
	if (v & dir) {
	    uae_u16 data = 0x0100 << i;
	    potgo_value &= ~data;
	    potgo_value |= v & data;
	}
    }
    for (i = 0; i < 2; i++) {
	if (cd32_pad_enabled[i]) {
	    uae_u16 p5dir = 0x0200 << (i * 4); /* output enable P5 */
	    uae_u16 p5dat = 0x0100 << (i * 4); /* data P5 */
	    if ((potgo_value & p5dir) && (potgo_value & p5dat))
		cd32_shifter[i] = 8;
	}
    }
    if (v & 1) {
	potdats[0] = potdats[1] = 0;
	potgo_hsync = -15;
    }
}

/****i* januae-inputdevice/POTGOR ******************************************
*
*   NAME
*       POTGOR() -- Read the emulated potentiometer input lines.
*
*   SYNOPSIS
*       uae_u16 POTGOR (void);
*
*   FUNCTION
*       Combine the stored POTGO state with controller-dependent input
*       levels and expose only the POTGOR data bits.
*
*   INPUTS
*       None.
*
*   RESULT
*       Sixteen-bit POTGOR input value.
*
****************************************************************************
*
*/

uae_u16 POTGOR (void)
{
    uae_u16 v = handle_joystick_potgor (potgo_value) & 0x5500;
#ifdef DONGLE_DEBUG
    if (notinrom ())
	write_log ("POTGOR %04.4X %s\n", v, debuginfo(0));
#endif
    return v;
}

/****i* januae-inputdevice/check_input_queue *******************************
*
*   NAME
*       check_input_queue() -- Locate a scheduled input event.
*
*   SYNOPSIS
*       static int check_input_queue (int event);
*
*   FUNCTION
*       Search the fixed-size deferred-event queue for an entry with the
*       supplied event identifier.
*
*   INPUTS
*       event - Emulator input event identifier.
*
*   RESULT
*       Queue slot index if present; minus one otherwise.
*
****************************************************************************
*
*/

static int check_input_queue (int event)
{
    const struct input_queue_struct *iq;
    int i;
    for (i = 0; i < INPUT_QUEUE_SIZE; i++) {
	iq = &input_queue[i];
	if (iq->event == event)
	    return i;
    }
    return -1;
}

/****i* januae-inputdevice/queue_input_event *******************************
*
*   NAME
*       queue_input_event() -- Schedule or cancel an input event.
*
*   SYNOPSIS
*       static void queue_input_event (int event, int state, int max, int
*       framecnt, int autofire);
*
*   FUNCTION
*       Reuse or allocate a deferred-event slot, store the requested state
*       and frame delay, and configure autofire rescheduling when requested.
*
*   INPUTS
*       event - Event identifier; state - Input state or negative
*       cancellation value; max - Input magnitude limit; framecnt - Delay in
*       frames; autofire - Repeat mode.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void queue_input_event (int event, int state, int max, int framecnt, int autofire)
{
    struct input_queue_struct *iq;
    int i = check_input_queue (event);

    if (event <= 0)
	return;
    if (state < 0 && i >= 0) {
	iq = &input_queue[i];
	iq->nextframecnt = -1;
	iq->framecnt = -1;
	iq->event = 0;
	if (iq->state == 0)
	    handle_input_event (event, 0, 1, 0);
    } else if (i < 0) {
	for (i = 0; i < INPUT_QUEUE_SIZE; i++) {
	    iq = &input_queue[i];
	    if (iq->framecnt < 0) break;
	}
	if (i == INPUT_QUEUE_SIZE) {
	    write_log ("input queue overflow\n");
	    return;
	}
	iq->event = event;
	iq->state = iq->storedstate = state;
	iq->max = max;
	iq->framecnt = framecnt;
	iq->nextframecnt = autofire > 0 ? framecnt : -1;
    }
}

static uae_u8 keybuf[256];
static int inputcode_pending, inputcode_pending_state;

/****i* januae-inputdevice/inputdevice_release_all_keys ********************
*
*   NAME
*       inputdevice_release_all_keys() -- Release all recorded keyboard
*       keys.
*
*   SYNOPSIS
*       void inputdevice_release_all_keys (void);
*
*   FUNCTION
*       Walk the host key-down map and enqueue release transitions for each
*       key still recorded as pressed.
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

void inputdevice_release_all_keys (void)
{
   int i;

   for (i = 0; i < 0x80; i++) {
	if (keybuf[i] != 0) {
	    keybuf[i] = 0;
	    record_key (i << 1|1);
	}
   }
}


/****i* januae-inputdevice/inputdevice_add_inputcode ***********************
*
*   NAME
*       inputdevice_add_inputcode() -- Store a pending emulator control
*       command.
*
*   SYNOPSIS
*       void inputdevice_add_inputcode (int code, int state);
*
*   FUNCTION
*       Replace the single pending input-code slot and its associated state.
*
*   INPUTS
*       code - Emulator control code; state - Requested control state.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void inputdevice_add_inputcode (int code, int state)
{
    inputcode_pending = code;
    inputcode_pending_state = state;
}

/****i* januae-inputdevice/inputdevice_do_keyboard *************************
*
*   NAME
*       inputdevice_do_keyboard() -- Forward a keyboard transition or
*       emulator control code.
*
*   SYNOPSIS
*       void inputdevice_do_keyboard (int code, int state);
*
*   FUNCTION
*       Record ordinary key transitions and their pressed state, process the
*       emulator reset chord, or queue extended control codes for frame-time
*       dispatch.
*
*   INPUTS
*       code - Raw keyboard code or special input code; state - Nonzero for
*       pressed, zero for released.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void inputdevice_do_keyboard (int code, int state)
{
    if (code < 0x80) {
	uae_u8 key = code | (state ? 0x00 : 0x80);
	keybuf[key & 0x7f] = (key & 0x80) ? 0 : 1;
	if (((keybuf[AK_CTRL] || keybuf[AK_RCTRL]) && keybuf[AK_LAMI] && keybuf[AK_RAMI]) || key == AK_RESETWARNING) {
	    int r = keybuf[AK_LALT] | keybuf[AK_RALT];
	    memset (keybuf, 0, sizeof (keybuf));
	    uae_reset (r);
	}
	record_key ((uae_u8)((key << 1) | (key >> 7)));
	return;
    }

    inputdevice_add_inputcode (code, state);
}

/****i* januae-inputdevice/inputdevice_handle_inputcode ********************
*
*   NAME
*       inputdevice_handle_inputcode() -- Execute the pending emulator
*       control command.
*
*   SYNOPSIS
*       void inputdevice_handle_inputcode (void);
*
*   FUNCTION
*       Consume the pending special input code and dispatch its configured
*       emulator action, including reset, media, display, speed and sound
*       controls.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       A pending code of zero produces no action; compilation options
*       determine which special commands are available.
*
****************************************************************************
*
*/

void inputdevice_handle_inputcode (void)
{
    int code = inputcode_pending;
    int state = inputcode_pending_state;
    inputcode_pending = 0;
    if (code == 0)
	return;
    if (vpos != 0)
	write_log ("inputcode=%d but vpos = %d", code, vpos);

#ifdef ARCADIA
    switch (code)
    {
    case AKS_ARCADIADIAGNOSTICS:
	arcadia_flag &= ~1;
	arcadia_flag |= state ? 1 : 0;
	break;
    case AKS_ARCADIAPLY1:
	arcadia_flag &= ~4;
	arcadia_flag |= state ? 4 : 0;
	break;
    case AKS_ARCADIAPLY2:
	arcadia_flag &= ~2;
	arcadia_flag |= state ? 2 : 0;
	break;
    case AKS_ARCADIACOIN1:
	if (state)
	    arcadia_coin[0]++;
	break;
    case AKS_ARCADIACOIN2:
	if (state)
	    arcadia_coin[1]++;
	break;
    }
#endif

    if (!state)
	return;
    switch (code)
    {
    case AKS_ENTERGUI:
	gui_display (-1);
	break;
    case AKS_SCREENSHOT:
	screenshot (1);
	break;
#ifdef ACTION_REPLAY
    case AKS_FREEZEBUTTON:
	action_replay_freeze ();
	break;
#endif
    case AKS_FLOPPY0:
	gui_display (0);
	break;
    case AKS_FLOPPY1:
	gui_display (1);
	break;
    case AKS_FLOPPY2:
	gui_display (2);
	break;
    case AKS_FLOPPY3:
	gui_display (3);
	break;
    case AKS_EFLOPPY0:
	disk_eject (0);
	break;
    case AKS_EFLOPPY1:
	disk_eject (1);
	break;
    case AKS_EFLOPPY2:
	disk_eject (2);
	break;
    case AKS_EFLOPPY3:
	disk_eject (3);
	break;
    case AKS_IRQ7:
	Interrupt (7);
	break;
    case AKS_PAUSE:
	if (uae_get_state () == UAE_STATE_PAUSED)
	    uae_resume ();
	else
	    uae_pause ();
	break;
    case AKS_WARP:
	warpmode (-1);
	break;
    case AKS_INHIBITSCREEN:
	toggle_inhibit_frame (IHF_SCROLLLOCK);
	break;
#ifdef SAVESTATE
    case AKS_STATEREWIND:
	savestate_dorewind(1);
#endif
	break;
    case AKS_VOLDOWN:
	audio_volume (-1);
	break;
    case AKS_VOLUP:
	audio_volume (1);
	break;
    case AKS_VOLMUTE:
	audio_volume (0);
	break;
    case AKS_QUIT:
	uae_quit ();
	break;
    case AKS_SOFTRESET:
	uae_reset (0);
	break;
    case AKS_HARDRESET:
	uae_reset (1);
	break;
#ifdef SAVESTATE
    case AKS_STATESAVEQUICK:
    case AKS_STATESAVEQUICK1:
    case AKS_STATESAVEQUICK2:
    case AKS_STATESAVEQUICK3:
    case AKS_STATESAVEQUICK4:
    case AKS_STATESAVEQUICK5:
    case AKS_STATESAVEQUICK6:
    case AKS_STATESAVEQUICK7:
    case AKS_STATESAVEQUICK8:
    case AKS_STATESAVEQUICK9:
	savestate_quick ((code - AKS_STATESAVEQUICK) / 2, 1);
	break;
    case AKS_STATERESTOREQUICK:
    case AKS_STATERESTOREQUICK1:
    case AKS_STATERESTOREQUICK2:
    case AKS_STATERESTOREQUICK3:
    case AKS_STATERESTOREQUICK4:
    case AKS_STATERESTOREQUICK5:
    case AKS_STATERESTOREQUICK6:
    case AKS_STATERESTOREQUICK7:
    case AKS_STATERESTOREQUICK8:
    case AKS_STATERESTOREQUICK9:
	savestate_quick ((code - AKS_STATERESTOREQUICK) / 2, 0);
	break;
#endif
    case AKS_TOGGLEFULLSCREEN:
	toggle_fullscreen ();
	break;
    case AKS_TOGGLEMOUSEGRAB:
	toggle_mousegrab ();
	break;
#ifdef DEBUGGER
    case AKS_ENTERDEBUGGER:
	activate_debugger ();
	break;
#endif
    case AKS_STATESAVEDIALOG:
	gui_display (5);
	break;
	case AKS_STATERESTOREDIALOG:
	gui_display (4);
	break;
    case AKS_INCRFRAMERATE:
	if (currprefs.gfx_framerate < 20)
	    changed_prefs.gfx_framerate = currprefs.gfx_framerate + 1;
	break;
    case AKS_DECRFRAMERATE:
	if (currprefs.gfx_framerate > 1)
	    changed_prefs.gfx_framerate = currprefs.gfx_framerate - 1;
	break;
    case AKS_SWITCHINTERPOL:
	switch_audio_interpol ();
    break;
    }
}

/****i* januae-inputdevice/handle_input_event ******************************
*
*   NAME
*       handle_input_event() -- Apply an emulator input event to the
*       emulated hardware.
*
*   SYNOPSIS
*       int handle_input_event (int nr, int state, int max, int autofire);
*
*   FUNCTION
*       Interpret the selected event descriptor, update joystick, mouse,
*       keyboard or other emulated input state, and optionally schedule
*       autofire transitions.
*
*   INPUTS
*       nr - Input event identifier; state - Current control state; max -
*       Magnitude reference; autofire - Repetition request.
*
*   RESULT
*       One for a positive event identifier that is processed; zero for a
*       nonpositive identifier.
*
****************************************************************************
*
*/

int handle_input_event (int nr, int state, int max, int autofire)
{
    const struct inputevent *ie;
    int joy;

    if (nr <= 0)
	return 0;
    ie = &events[nr];
    if (autofire) {
	if (state)
	    queue_input_event (nr, state, max, currprefs.input_autofire_framecnt, 1);
	else
	    queue_input_event (nr, -1, 0, 0, 1);
    }
    switch (ie->unit)
    {
	break;
	case 1: /* ->JOY1 */
	case 2: /* ->JOY2 */
	case 3: /* ->Parallel port joystick adapter port #1 */
	case 4: /* ->Parallel port joystick adapter port #2 */
	    joy = ie->unit - 1;
	    if (ie->type & 4) {
		if (state)
		    joybutton[joy] |= 1 << ie->data;
		else
		    joybutton[joy] &= ~(1 << ie->data);
	    } else if (ie->type & 8) {
		/* real mouse / analog stick mouse emulation */
		int delta;
		int deadzone = currprefs.input_joymouse_deadzone * max / 100;
		if (max) {
		    if (state < deadzone && state > -deadzone) {
			state = 0;
		    } else if (state < 0) {
			state += deadzone;
		    } else {
			state -= deadzone;
		    }
		    max -= deadzone;
		    delta = state * currprefs.input_joymouse_multiplier / max;
		} else {
		    delta = state;
		}
		mouse_delta[joy][ie->data] += delta;

	    } else if (ie->type & 32) { /* button mouse emulation vertical */

		int speed = currprefs.input_joymouse_speed;
		if (state && (ie->data & DIR_UP)) {
		    mouse_delta[joy][1] = -speed;
		    mouse_deltanoreset[joy][1] = 1;
		} else if (state && (ie->data & DIR_DOWN)) {
		    mouse_delta[joy][1] = speed;
		    mouse_deltanoreset[joy][1] = 1;
		} else
		    mouse_deltanoreset[joy][1] = 0;

	    } else if (ie->type & 64) { /* button mouse emulation horizontal */

		int speed = currprefs.input_joymouse_speed;
		if (state && (ie->data & DIR_LEFT)) {
		    mouse_delta[joy][0] = -speed;
		    mouse_deltanoreset[joy][0] = 1;
		} else if (state && (ie->data & DIR_RIGHT)) {
		    mouse_delta[joy][0] = speed;
		    mouse_deltanoreset[joy][0] = 1;
		} else
		    mouse_deltanoreset[joy][0] = 0;

	    } else if (ie->type & 128) { /* analog (paddle) */
		int deadzone = currprefs.input_joymouse_deadzone * max / 100;
		if (max) {
		    if (state < deadzone && state > -deadzone) {
			state = 0;
		    } else if (state < 0) {
			state += deadzone;
		    } else {
			state -= deadzone;
		    }
		    state = state * max / (max - deadzone);
		}
		state = state / 256 + 128;
		joydirpot[joy][ie->data] = state;
	    } else {
		int left = oleft[joy], right = oright[joy], top = otop[joy], bot = obot[joy];
		if (ie->type & 16) {
		    /* button to axis mapping */
		    if (ie->data & DIR_LEFT) left = oleft[joy] = state ? 1 : 0;
		    if (ie->data & DIR_RIGHT) right = oright[joy] = state ? 1 : 0;
		    if (ie->data & DIR_UP) top = otop[joy] = state ? 1 : 0;
		    if (ie->data & DIR_DOWN) bot = obot[joy] = state ? 1 : 0;
		} else {
		    /* "normal" joystick axis */
		    int deadzone = currprefs.input_joystick_deadzone * max / 100;
		    int neg, pos;
		    if (state < deadzone && state > -deadzone)
			state = 0;
		    neg = state < 0 ? 1 : 0;
		    pos = state > 0 ? 1 : 0;
		    if (ie->data & DIR_LEFT) left = oleft[joy] = neg;
		    if (ie->data & DIR_RIGHT) right = oright[joy] = pos;
		    if (ie->data & DIR_UP) top = otop[joy] = neg;
		    if (ie->data & DIR_DOWN) bot = obot[joy] = pos;
		}
		joydir[joy] = 0;
		if (left) joydir[joy] |= DIR_LEFT;
		if (right) joydir[joy] |= DIR_RIGHT;
		if (top) joydir[joy] |= DIR_UP;
		if (bot) joydir[joy] |= DIR_DOWN;
	    }
	break;
	case 0: /* ->KEY */
	    inputdevice_do_keyboard (ie->data, state);
	break;
    }
    return 1;
}

/****i* januae-inputdevice/inputdevice_vsync *******************************
*
*   NAME
*       inputdevice_vsync() -- Advance the input subsystem by one video
*       frame.
*
*   SYNOPSIS
*       void inputdevice_vsync (void);
*
*   FUNCTION
*       Service queued autofire transitions, poll selected backends, process
*       pending special input codes and age the guest mouse activity
*       counter.
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

void inputdevice_vsync (void)
{
    struct input_queue_struct *iq;
    int i;

    for (i = 0; i < INPUT_QUEUE_SIZE; i++) {
	iq = &input_queue[i];
	if (iq->framecnt > 0) {
	    iq->framecnt--;
	    if (iq->framecnt == 0) {
		if (iq->state) iq->state = 0; else iq->state = iq->storedstate;
		handle_input_event (iq->event, iq->state, iq->max, 0);
		iq->framecnt = iq->nextframecnt;
	    }
	}
    }
    mouseupdate (100);
    inputdelay = rand () % (maxvpos == 0 ? 1 : maxvpos - 1);
    idev[IDTYPE_MOUSE].read ();
    input_read = 1;
    input_vpos = 0;
    inputdevice_handle_inputcode ();
    if (ievent_alive > 0)
	ievent_alive--;
#ifdef ARCADIA
    if (arcadia_rom)
	arcadia_vsync ();
#endif
}

/****i* januae-inputdevice/inputdevice_reset *******************************
*
*   NAME
*       inputdevice_reset() -- Reset the guest mouse activity marker.
*
*   SYNOPSIS
*       void inputdevice_reset (void);
*
*   FUNCTION
*       Clear the frame-based mouse integration activity counter.
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

void inputdevice_reset (void)
{
    ievent_alive = 0;
}

/****i* januae-inputdevice/setbuttonstateall *******************************
*
*   NAME
*       setbuttonstateall() -- Apply one mapped physical button transition.
*
*   SYNOPSIS
*       void setbuttonstateall (struct uae_input_device *id, struct
*       uae_input_device2 *id2, int button, int state);
*
*   FUNCTION
*       Update a device button mask, resolve each configured subevent and
*       dispatch corresponding emulator events with their mapping flags.
*
*   INPUTS
*       id - Device mappings; id2 - Runtime device state; button - Physical
*       button index; state - Button state or impulse value.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void setbuttonstateall (struct uae_input_device *id, struct uae_input_device2 *id2, int button, int state)
{
    int event, autofire, i;
    uae_u32 mask = 1 << button;
    uae_u32 omask = id2->buttonmask & mask;
    uae_u32 nmask = (state ? 1 : 0) << button;
    char *custom;
  //kprintf("FW: setmousebuttonstateall..\n");

    if (button >= ID_BUTTON_TOTAL)
	return;
  //kprintf("FW: setmousebuttonstateall.. 2\n");
    for (i = 0; i < MAX_INPUT_SUB_EVENT; i++) {
	event = id->eventid[ID_BUTTON_OFFSET + button][sublevdir[state <= 0 ? 1 : 0][i]];
	custom = id->custom[ID_BUTTON_OFFSET + button][sublevdir[state <= 0 ? 1 : 0][i]];
	if (event <= 0 && custom == NULL)
	    continue;
	autofire = (id->flags[ID_BUTTON_OFFSET + button][sublevdir[state <= 0 ? 1 : 0][i]] & ID_FLAG_AUTOFIRE) ? 1 : 0;
	if (state < 0) {
	    handle_input_event (event, 1, 1, 0);
	    queue_input_event (event, 0, 1, 1, 0); /* send release event next frame */
	} else {
	    if ((omask ^ nmask) & mask)
		handle_input_event (event, state, 1, autofire);
	}
    }
    if ((omask ^ nmask) & mask) {
	if (state)
	    id2->buttonmask |= mask;
	else
	    id2->buttonmask &= ~mask;
    }
  //kprintf("FW: setmousebuttonstateall.. exit\n");
}


/* - detect required number of joysticks and mice from configuration data
 * - detect if CD32 pad emulation is needed
 * - detect device type in ports (mouse or joystick)
 */

/****i* januae-inputdevice/iscd32 ******************************************
*
*   NAME
*       iscd32() -- Identify a CD32 controller event mapping.
*
*   SYNOPSIS
*       static int iscd32 (int ei);
*
*   FUNCTION
*       Check the event against the two CD32 controller event ranges and
*       enable the corresponding emulated port when matched.
*
*   INPUTS
*       ei - Input event identifier.
*
*   RESULT
*       One or two for the matching CD32 port; zero for other events.
*
****************************************************************************
*
*/

static int iscd32 (int ei)
{
    if (ei >= INPUTEVENT_JOY1_CD32_FIRST && ei <= INPUTEVENT_JOY1_CD32_LAST) {
	cd32_pad_enabled[0] = 1;
	return 1;
    }
    if (ei >= INPUTEVENT_JOY2_CD32_FIRST && ei <= INPUTEVENT_JOY2_CD32_LAST) {
	cd32_pad_enabled[1] = 1;
	return 2;
    }
    return 0;
}

/****i* januae-inputdevice/isparport ***************************************
*
*   NAME
*       isparport() -- Identify a parallel-port joystick mapping.
*
*   SYNOPSIS
*       static int isparport (int ei);
*
*   FUNCTION
*       Enable parallel-port joystick emulation when the event belongs to
*       the supported parallel joystick range.
*
*   INPUTS
*       ei - Input event identifier.
*
*   RESULT
*       One for a parallel-port joystick event; zero otherwise.
*
****************************************************************************
*
*/

static int isparport (int ei)
{
    if (ei > INPUTEVENT_PAR_JOY1_START && ei < INPUTEVENT_PAR_JOY_END) {
	parport_joystick_enabled = 1;
	return 1;
    }
    return 0;
}

/****i* januae-inputdevice/ismouse *****************************************
*
*   NAME
*       ismouse() -- Identify an emulated mouse-port event.
*
*   SYNOPSIS
*       static int ismouse (int ei);
*
*   FUNCTION
*       Check for a mouse event on either emulated port and mark that port
*       as mouse-driven.
*
*   INPUTS
*       ei - Input event identifier.
*
*   RESULT
*       One or two for the corresponding mouse port; zero otherwise.
*
****************************************************************************
*
*/

static int ismouse (int ei)
{
    if (ei >= INPUTEVENT_MOUSE1_FIRST && ei <= INPUTEVENT_MOUSE1_LAST) {
	mouse_port[0] = 1;
	return 1;
    }
    if (ei >= INPUTEVENT_MOUSE2_FIRST && ei <= INPUTEVENT_MOUSE2_LAST) {
	mouse_port[1] = 1;
	return 2;
    }
    return 0;
}

#ifdef CD32
extern int cd32_enabled;
#endif

/****i* januae-inputdevice/scanevents **************************************
*
*   NAME
*       scanevents() -- Discover active devices and specialized controller
*       modes.
*
*   SYNOPSIS
*       static void scanevents (struct uae_prefs *p);
*
*   FUNCTION
*       Inspect the selected joystick, mouse and keyboard mappings, mark
*       required host devices, and determine mouse-port, parallel-joystick
*       and CD32 usage.
*
*   INPUTS
*       p - Selected input preferences.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void scanevents (struct uae_prefs *p)
{
    unsigned int i;
    int j, k, ei;
    const struct inputevent *e;
    unsigned int n_joy = idev[IDTYPE_JOYSTICK].get_num();
    unsigned int n_mouse = idev[IDTYPE_MOUSE].get_num();

    cd32_pad_enabled[0] = cd32_pad_enabled[1] = 0;
    parport_joystick_enabled = 0;
    mouse_port[0] = mouse_port[1] = 0;
    for (i = 0; i < MAX_INPUT_DEVICE_EVENTS; i++)
	joydir[i] = 0;

    for (i = 0; i < MAX_INPUT_DEVICES; i++) {
	use_joysticks[i] = 0;
	use_mice[i] = 0;
	for (k = 0; k < MAX_INPUT_SUB_EVENT; k++) {
	    for (j = 0; j < ID_BUTTON_TOTAL; j++) {

		if (joysticks[i].enabled && i < n_joy) {
		    ei = joysticks[i].eventid[ID_BUTTON_OFFSET + j][k];
		    e = &events[ei];
		    iscd32 (ei);
		    isparport (ei);
		    ismouse (ei);
		    if (joysticks[i].eventid[ID_BUTTON_OFFSET + j][k] > 0)
			use_joysticks[i] = 1;
		}
		if (mice[i].enabled && i < n_mouse) {
		    ei = mice[i].eventid[ID_BUTTON_OFFSET + j][k];
		    e = &events[ei];
		    iscd32 (ei);
		    isparport (ei);
		    ismouse (ei);
		    if (mice[i].eventid[ID_BUTTON_OFFSET + j][k] > 0)
			use_mice[i] = 1;
		}

	    }

	    for (j = 0; j < ID_AXIS_TOTAL; j++) {

		if (joysticks[i].enabled && i < n_joy) {
		    ei = joysticks[i].eventid[ID_AXIS_OFFSET + j][k];
		    iscd32 (ei);
		    isparport (ei);
		    ismouse (ei);
		    if (ei > 0)
			use_joysticks[i] = 1;
		}
		if (mice[i].enabled && i < n_mouse) {
		    ei = mice[i].eventid[ID_AXIS_OFFSET + j][k];
		    iscd32 (ei);
		    isparport (ei);
		    ismouse (ei);
		    if (ei > 0)
			use_mice[i] = 1;
		}
	    }
	}
    }
    for (i = 0; i < MAX_INPUT_DEVICES; i++) {
	use_keyboards[i] = 0;
	if (keyboards[i].enabled && i < idev[IDTYPE_KEYBOARD].get_num()) {
	    j = 0;
	    while (keyboards[i].extra[j][0] >= 0) {
		use_keyboards[i] = 1;
		for (k = 0; k < MAX_INPUT_SUB_EVENT; k++) {
		    ei = keyboards[i].eventid[j][k];
		    iscd32 (ei);
		    isparport (ei);
		    ismouse (ei);
		}
		j++;
	    }
	}
    }
}

#ifdef CD32
/****i* januae-inputdevice/setcd32 *****************************************
*
*   NAME
*       setcd32() -- Install standard CD32 button mappings for a joystick.
*
*   SYNOPSIS
*       static void setcd32 (int joy, int n);
*
*   FUNCTION
*       Assign the CD32 red, blue, yellow, green, transport and play event
*       identifiers to the selected joystick button slots.
*
*   INPUTS
*       joy - Physical joystick index; n - Emulated controller port.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Available only in builds with CD32 support.
*
****************************************************************************
*
*/

static void setcd32 (int joy, int n)
{
    joysticks[joy].eventid[ID_BUTTON_OFFSET + 0][0] = n ? INPUTEVENT_JOY2_CD32_RED : INPUTEVENT_JOY1_CD32_RED;
    joysticks[joy].eventid[ID_BUTTON_OFFSET + 1][0] = n ? INPUTEVENT_JOY2_CD32_BLUE : INPUTEVENT_JOY1_CD32_BLUE;
    joysticks[joy].eventid[ID_BUTTON_OFFSET + 2][0] = n ? INPUTEVENT_JOY2_CD32_YELLOW : INPUTEVENT_JOY1_CD32_YELLOW;
    joysticks[joy].eventid[ID_BUTTON_OFFSET + 3][0] = n ? INPUTEVENT_JOY2_CD32_GREEN : INPUTEVENT_JOY1_CD32_GREEN;
    joysticks[joy].eventid[ID_BUTTON_OFFSET + 4][0] = n ? INPUTEVENT_JOY2_CD32_FFW : INPUTEVENT_JOY1_CD32_FFW;
    joysticks[joy].eventid[ID_BUTTON_OFFSET + 5][0] = n ? INPUTEVENT_JOY2_CD32_RWD : INPUTEVENT_JOY1_CD32_RWD;
    joysticks[joy].eventid[ID_BUTTON_OFFSET + 6][0] = n ? INPUTEVENT_JOY2_CD32_PLAY :  INPUTEVENT_JOY1_CD32_PLAY;
}
#endif

int compatibility_device[2];

/****i* januae-inputdevice/compatibility_mode ******************************
*
*   NAME
*       compatibility_mode() -- Construct legacy joystick and mouse port
*       mappings.
*
*   SYNOPSIS
*       static void compatibility_mode (struct uae_prefs *prefs);
*
*   FUNCTION
*       Clear generated compatibility mappings, decode the selected legacy
*       port devices, and assign standard mouse, joystick or CD32 events to
*       available host devices.
*
*   INPUTS
*       prefs - Preferences defining the legacy port selections.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void compatibility_mode (struct uae_prefs *prefs)
{
    int joy, i;
    int used[4] = { 0, 0, 0, 0};

    compatibility_device[0] = -1;
    compatibility_device[1] = -1;
    for (i = 0; i < MAX_INPUT_DEVICES; i++) {
	memset (&mice[i], 0, sizeof (*mice));
	memset (&joysticks[i], 0, sizeof (*joysticks));
    }

    if ((joy = jsem_ismouse (0, prefs)) >= 0) {
	mice[joy].eventid[ID_AXIS_OFFSET + 0][0] = INPUTEVENT_MOUSE1_HORIZ;
	mice[joy].eventid[ID_AXIS_OFFSET + 1][0] = INPUTEVENT_MOUSE1_VERT;
	mice[joy].eventid[ID_AXIS_OFFSET + 2][0] = INPUTEVENT_MOUSE1_WHEEL;
	mice[joy].eventid[ID_BUTTON_OFFSET + 0][0] = INPUTEVENT_JOY1_FIRE_BUTTON;
	mice[joy].eventid[ID_BUTTON_OFFSET + 1][0] = INPUTEVENT_JOY1_2ND_BUTTON;
	mice[joy].eventid[ID_BUTTON_OFFSET + 2][0] = INPUTEVENT_JOY1_3RD_BUTTON;
	mice[joy].eventid[ID_BUTTON_OFFSET + 3][0] = INPUTEVENT_KEY_ALT_LEFT;
	mice[joy].eventid[ID_BUTTON_OFFSET + 3][1] = INPUTEVENT_KEY_CURSOR_LEFT;
	mice[joy].eventid[ID_BUTTON_OFFSET + 4][0] = INPUTEVENT_KEY_ALT_LEFT;
	mice[joy].eventid[ID_BUTTON_OFFSET + 4][1] = INPUTEVENT_KEY_CURSOR_RIGHT;
	mice[joy].enabled = 1;
    }
    if ((joy = jsem_ismouse (1, prefs)) >= 0) {
	mice[joy].eventid[ID_AXIS_OFFSET + 0][0] = INPUTEVENT_MOUSE2_HORIZ;
	mice[joy].eventid[ID_AXIS_OFFSET + 1][0] = INPUTEVENT_MOUSE2_VERT;
	mice[joy].eventid[ID_BUTTON_OFFSET + 0][0] = INPUTEVENT_JOY2_FIRE_BUTTON;
	mice[joy].eventid[ID_BUTTON_OFFSET + 1][0] = INPUTEVENT_JOY2_2ND_BUTTON;
	mice[joy].eventid[ID_BUTTON_OFFSET + 2][0] = INPUTEVENT_JOY2_3RD_BUTTON;
	mice[joy].enabled = 1;
    }

    joy = jsem_isjoy (1, prefs);
    if (joy >= 0) {
	joysticks[joy].eventid[ID_AXIS_OFFSET + 0][0] = INPUTEVENT_JOY2_HORIZ;
	joysticks[joy].eventid[ID_AXIS_OFFSET + 1][0] = INPUTEVENT_JOY2_VERT;
	used[joy] = 1;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 0][0] = INPUTEVENT_JOY2_FIRE_BUTTON;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 1][0] = INPUTEVENT_JOY2_2ND_BUTTON;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 2][0] = INPUTEVENT_JOY2_3RD_BUTTON;
#ifdef CD32
	if (cd32_enabled)
	    setcd32 (joy, 1);
#endif
	joysticks[joy].enabled = 1;
    }

    joy = jsem_isjoy (0, prefs);
    if (joy >= 0) {
	used[joy] = 1;
	joysticks[joy].eventid[ID_AXIS_OFFSET + 0][0] = INPUTEVENT_JOY1_HORIZ;
	joysticks[joy].eventid[ID_AXIS_OFFSET + 1][0] = INPUTEVENT_JOY1_VERT;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 0][0] = INPUTEVENT_JOY1_FIRE_BUTTON;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 1][0] = INPUTEVENT_JOY1_2ND_BUTTON;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 2][0] = INPUTEVENT_JOY1_3RD_BUTTON;
	joysticks[joy].enabled = 1;
    }

    for (joy = 0; used[joy]; joy++);
    if (JSEM_ISANYKBD (0, prefs)) {
	joysticks[joy].eventid[ID_AXIS_OFFSET +  0][0] = INPUTEVENT_JOY1_HORIZ;
	joysticks[joy].eventid[ID_AXIS_OFFSET +  1][0] = INPUTEVENT_JOY1_VERT;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 0][0] = INPUTEVENT_JOY1_FIRE_BUTTON;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 1][0] = INPUTEVENT_JOY1_2ND_BUTTON;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 2][0] = INPUTEVENT_JOY1_3RD_BUTTON;
	joysticks[joy].enabled = 1;
	used[joy] = 1;
	compatibility_device[0] = joy;
    }
    for (joy = 0; used[joy]; joy++);
    if (JSEM_ISANYKBD (1, prefs)) {
	joysticks[joy].eventid[ID_AXIS_OFFSET +  0][0] = INPUTEVENT_JOY2_HORIZ;
	joysticks[joy].eventid[ID_AXIS_OFFSET +  1][0] = INPUTEVENT_JOY2_VERT;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 0][0] = INPUTEVENT_JOY2_FIRE_BUTTON;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 1][0] = INPUTEVENT_JOY2_2ND_BUTTON;
	joysticks[joy].eventid[ID_BUTTON_OFFSET + 2][0] = INPUTEVENT_JOY2_3RD_BUTTON;
#ifdef CD32
	if (cd32_enabled)
	    setcd32 (joy, 1);
#endif
	joysticks[joy].enabled = 1;
	used[joy] = 1;
	compatibility_device[1] = joy;
    }
}

/****i* januae-inputdevice/inputdevice_updateconfig ************************
*
*   NAME
*       inputdevice_updateconfig() -- Activate updated input assignments.
*
*   SYNOPSIS
*       void inputdevice_updateconfig (struct uae_prefs *prefs);
*
*   FUNCTION
*       Reinitialize runtime button and movement state, select input device
*       configuration sets, apply legacy compatibility mappings when
*       selected, update keyboard joystick bindings and rescan active
*       events.
*
*   INPUTS
*       prefs - Input preferences to activate.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void inputdevice_updateconfig (struct uae_prefs *prefs)
{
    int i;

    if (currprefs.jport0 != changed_prefs.jport0
	|| currprefs.jport1 != changed_prefs.jport1) {
	currprefs.jport0 = changed_prefs.jport0;
	currprefs.jport1 = changed_prefs.jport1;
    }
    joybutton[0] = joybutton[1] = 0;
    joydir[0] = joydir[1] = 0;
    oldmx[0] = oldmx[1] = -1;
    oldmy[0] = oldmy[1] = -1;
    cd32_shifter[0] = cd32_shifter[1] = 8;
    oleft[0] = oleft[1] = 0;
    oright[0] = oright[1] = 0;
    otop[0] = otop[1] = 0;
    obot[0] = obot[1] = 0;
    for (i = 0; i < 2; i++) {
	mouse_deltanoreset[i][0] = 0;
	mouse_delta[i][0] = 0;
	mouse_deltanoreset[i][1] = 0;
	mouse_delta[i][1] = 0;
	mouse_deltanoreset[i][2] = 0;
	mouse_delta[i][2] = 0;
    }
    memset (keybuf, 0, sizeof (keybuf));

    for (i = 0; i < INPUT_QUEUE_SIZE; i++)
	input_queue[i].framecnt = input_queue[i].nextframecnt = -1;

    for (i = 0; i < MAX_INPUT_SUB_EVENT; i++) {
	sublevdir[0][i] = i;
	sublevdir[1][i] = MAX_INPUT_SUB_EVENT - i - 1;
    }

    joysticks = prefs->joystick_settings[prefs->input_selected_setting];
    mice = prefs->mouse_settings[prefs->input_selected_setting];
    keyboards = prefs->keyboard_settings[prefs->input_selected_setting];

    memset (joysticks2, 0, sizeof (joysticks2));
    memset (mice2, 0, sizeof (mice2));
    if (prefs->input_selected_setting == 0)
	compatibility_mode (prefs);

    joystick_setting_changed ();

    scanevents (prefs);

#ifdef CD32
    if (currprefs.input_selected_setting == 0 && cd32_enabled)
	cd32_pad_enabled[1] = 1;
#endif

    mousehack_enable ();
}

/****i* januae-inputdevice/set_kbr_default *********************************
*
*   NAME
*       set_kbr_default() -- Install default keyboard mappings for a
*       setting.
*
*   SYNOPSIS
*       static void set_kbr_default (struct uae_prefs *p, int index, int
*       num);
*
*   FUNCTION
*       Walk the selected keyboard devices and translate host scan codes to
*       emulator events using the installed default keyboard table.
*
*   INPUTS
*       p - Destination preferences; index - Configuration setting; num -
*       Keyboard mapping selector.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void set_kbr_default (struct uae_prefs *p, int index, int num)
{
    int k, l;
    unsigned int i, j;
    struct uae_input_device_kbr_default *trans = keyboard_default;
    struct uae_input_device *kbr;
    struct inputdevice_functions *id = &idev[IDTYPE_KEYBOARD];
    uae_u32 scancode;

    if (!trans)
	return;
    for (j = 0; j < MAX_INPUT_DEVICES; j++) {
	kbr = &p->keyboard_settings[index][j];
	for (i = 0; i < MAX_INPUT_DEVICE_EVENTS; i++) {
	    memset (kbr, 0, sizeof (struct uae_input_device));
	    kbr->extra[i][0] = -1;
	}
	if (j < id->get_num ()) {
	    if (j == 0)
		kbr->enabled = 1;
	    for (i = 0; i < id->get_widget_num (num); i++) {
		id->get_widget_type (num, i, 0, &scancode);
		kbr->extra[i][0] = scancode;
		l = 0;
		while (trans[l].scancode >= 0) {
		    if (kbr->extra[i][0] == trans[l].scancode) {
			for (k = 0; k < MAX_INPUT_SUB_EVENT; k++) {
			    if (kbr->eventid[i][k] == 0) break;
			}
			if (k == MAX_INPUT_SUB_EVENT) {
			    write_log ("corrupt default keyboard mappings\n");
			    return;
			}
			kbr->eventid[i][k] = trans[l].event;
			break;
		    }
		    l++;
		}
	    }
	}
    }
}

/****i* januae-inputdevice/inputdevice_default_prefs ***********************
*
*   NAME
*       inputdevice_default_prefs() -- Initialize default input preferences
*       and mappings.
*
*   SYNOPSIS
*       void inputdevice_default_prefs (struct uae_prefs *p);
*
*   FUNCTION
*       Initialize host input backends, assign default deadzones, speeds and
*       autofire timing, then populate keyboard, mouse and joystick maps for
*       every settings bank.
*
*   INPUTS
*       p - Preferences to initialize.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void inputdevice_default_prefs (struct uae_prefs *p)
{
    int i;

    inputdevice_init ();
    p->input_joymouse_multiplier = 20;
    p->input_joymouse_deadzone = 33;
    p->input_joystick_deadzone = 33;
    p->input_joymouse_speed = 10;
    p->input_mouse_speed = 100;
    p->input_autofire_framecnt = 10;
    for (i = 0; i <= MAX_INPUT_SETTINGS; i++) {
	set_kbr_default (p, i, 0);
	input_get_default_mouse (p->mouse_settings[i]);
	input_get_default_joystick (p->joystick_settings[i]);
    }
}

/****i* januae-inputdevice/inputdevice_setkeytranslation *******************
*
*   NAME
*       inputdevice_setkeytranslation() -- Install the default keyboard
*       translation table.
*
*   SYNOPSIS
*       void inputdevice_setkeytranslation (struct
*       uae_input_device_kbr_default *trans);
*
*   FUNCTION
*       Store the supplied keyboard scan-code translation table as the
*       global source for default mappings.
*
*   INPUTS
*       trans - Translation table to retain.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The caller retains ownership of the referenced table.
*
****************************************************************************
*
*/

void inputdevice_setkeytranslation (struct uae_input_device_kbr_default *trans)
{
    keyboard_default = trans;
}

/****i* januae-inputdevice/inputdevice_translatekeycode ********************
*
*   NAME
*       inputdevice_translatekeycode() -- Translate a host scan-code
*       transition.
*
*   SYNOPSIS
*       int inputdevice_translatekeycode (int keyboard, int scancode, int
*       state);
*
*   FUNCTION
*       Find a matching scan code in the selected keyboard mapping, dispatch
*       its subevents in the defined press or release order, and honor
*       configured autofire flags.
*
*   INPUTS
*       keyboard - Keyboard index; scancode - Host scan code; state - Key
*       transition state.
*
*   RESULT
*       One when the scan code is found; zero otherwise.
*
****************************************************************************
*
*/

int inputdevice_translatekeycode (int keyboard, int scancode, int state)
{
    struct uae_input_device *na = &keyboards[keyboard];
    int j, k;

    if (!keyboards || scancode < 0)
	return 0;
    j = 0;
    while (na->extra[j][0] >= 0) {
	if (na->extra[j][0] == scancode) {
	    for (k = 0; k < MAX_INPUT_SUB_EVENT; k++) {/* send key release events in reverse order */
		int autofire = (na->flags[j][sublevdir[state == 0 ? 1 : 0][k]] & ID_FLAG_AUTOFIRE) ? 1 : 0;
		int event = na->eventid[j][sublevdir[state == 0 ? 1 : 0][k]];
		handle_input_event (event, state, 1, autofire);
	    }
	    return 1;
	}
	j++;
    }
    return 0;
}

/****i* januae-inputdevice/inputdevice_init ********************************
*
*   NAME
*       inputdevice_init() -- Initialize the host input device backends.
*
*   SYNOPSIS
*       void inputdevice_init (void);
*
*   FUNCTION
*       Install joystick, mouse and keyboard backend function tables and
*       invoke each backend initialization callback.
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

void inputdevice_init (void)
{
    idev[IDTYPE_JOYSTICK] = inputdevicefunc_joystick;
    idev[IDTYPE_JOYSTICK].init ();
    idev[IDTYPE_MOUSE] = inputdevicefunc_mouse;
    idev[IDTYPE_MOUSE].init ();
    idev[IDTYPE_KEYBOARD] = inputdevicefunc_keyboard;
    idev[IDTYPE_KEYBOARD].init ();
}

/****i* januae-inputdevice/inputdevice_close *******************************
*
*   NAME
*       inputdevice_close() -- Close the host input device backends.
*
*   SYNOPSIS
*       void inputdevice_close (void);
*
*   FUNCTION
*       Call the registered close callback for joystick, mouse and keyboard
*       backends.
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

void inputdevice_close (void)
{
    idev[IDTYPE_JOYSTICK].close ();
    idev[IDTYPE_MOUSE].close ();
    idev[IDTYPE_KEYBOARD].close ();
}

/****i* januae-inputdevice/get_uid *****************************************
*
*   NAME
*       get_uid() -- Resolve a device configuration from a backend.
*
*   SYNOPSIS
*       static struct uae_input_device *get_uid (const struct
*       inputdevice_functions *id, int devnum);
*
*   FUNCTION
*       Select the joystick, mouse or keyboard configuration array
*       corresponding to the provided input backend.
*
*   INPUTS
*       id - Backend function table; devnum - Index within that backend.
*
*   RESULT
*       Pointer to the device configuration, or NULL for an unrecognized
*       backend.
*
****************************************************************************
*
*/

static struct uae_input_device *get_uid (const struct inputdevice_functions *id, int devnum)
{
    struct uae_input_device *uid = 0;
    if (id == &idev[IDTYPE_JOYSTICK]) {
	uid = &joysticks[devnum];
    } else if (id == &idev[IDTYPE_MOUSE]) {
	uid = &mice[devnum];
    } else if (id == &idev[IDTYPE_KEYBOARD]) {
	uid = &keyboards[devnum];
    }
    return uid;
}

/****i* januae-inputdevice/get_event_data **********************************
*
*   NAME
*       get_event_data() -- Read an input control subevent mapping.
*
*   SYNOPSIS
*       static int get_event_data (const struct inputdevice_functions *id,
*       int devnum, int num, int *eventid, char **custom, int *flags, int
*       sub);
*
*   FUNCTION
*       Convert a physical widget index to its axis, button or key slot and
*       return the selected event identifier, custom command and flags.
*
*   INPUTS
*       id - Backend; devnum - Backend-local device index; num - Widget
*       index; eventid - Event output; custom - Custom command output; flags
*       - Mapping flags output; sub - Subevent slot.
*
*   RESULT
*       Internal mapping slot index, or minus one for an unsupported widget
*       type.
*
****************************************************************************
*
*/

static int get_event_data (const struct inputdevice_functions *id, int devnum, int num, int *eventid, char **custom, int *flags, int sub)
{
    const struct uae_input_device *uid = get_uid (id, devnum);
    int type = id->get_widget_type (devnum, num, 0, 0);
    int i;
    if (type == IDEV_WIDGET_BUTTON) {
	i = num - id->get_widget_first (devnum, type) + ID_BUTTON_OFFSET;
	*eventid = uid->eventid[i][sub];
	*flags = uid->flags[i][sub];
	*custom = uid->custom[i][sub];
	return i;
    } else if (type == IDEV_WIDGET_AXIS) {
	i = num - id->get_widget_first (devnum, type) + ID_AXIS_OFFSET;
	*eventid = uid->eventid[i][sub];
	*flags = uid->flags[i][sub];
	*custom = uid->custom[i][sub];
	return i;
    } else if (type == IDEV_WIDGET_KEY) {
	i = num - id->get_widget_first (devnum, type);
	*eventid = uid->eventid[i][sub];
	*flags = uid->flags[i][sub];
	*custom = uid->custom[i][sub];
	return i;
    }
    return -1;
}

/****i* januae-inputdevice/put_event_data **********************************
*
*   NAME
*       put_event_data() -- Replace an input control subevent mapping.
*
*   SYNOPSIS
*       static int put_event_data (const struct inputdevice_functions *id,
*       int devnum, int num, int eventid, const char *custom, int flags, int
*       sub);
*
*   FUNCTION
*       Resolve the widget mapping slot, release its previous custom command
*       and store the new event, flags and optional copied command string.
*
*   INPUTS
*       id - Backend; devnum - Backend-local device index; num - Widget
*       index; eventid - New event; custom - Optional command; flags -
*       Mapping flags; sub - Subevent index.
*
*   RESULT
*       Internal mapping slot index, or minus one for an unsupported widget
*       type.
*
****************************************************************************
*
*/

static int put_event_data (const struct inputdevice_functions *id, int devnum, int num, int eventid, const char *custom, int flags, int sub)
{
    struct uae_input_device *uid = get_uid (id, devnum);
    int type = id->get_widget_type (devnum, num, 0, 0);
    int i;
    if (type == IDEV_WIDGET_BUTTON) {
	i = num - id->get_widget_first (devnum, type) + ID_BUTTON_OFFSET;
	free (uid->custom[i][sub]);
	uid->eventid[i][sub] = eventid;
	uid->flags[i][sub] = flags;
	uid->custom[i][sub] = custom ? my_strdup (custom) : NULL;
	return i;
    } else if (type == IDEV_WIDGET_AXIS) {
	i = num - id->get_widget_first (devnum, type) + ID_AXIS_OFFSET;
	free (uid->custom[i][sub]);
	uid->eventid[i][sub] = eventid;
	uid->flags[i][sub] = flags;
	uid->custom[i][sub] = custom ? my_strdup (custom) : NULL;
	return i;
    } else if (type == IDEV_WIDGET_KEY) {
	i = num - id->get_widget_first (devnum, type);
	free (uid->custom[i][sub]);
	uid->eventid[i][sub] = eventid;
	uid->flags[i][sub] = flags;
	uid->custom[i][sub] = custom ? my_strdup (custom) : NULL;
	return i;
    }
    return -1;
}

/****i* januae-inputdevice/is_event_used ***********************************
*
*   NAME
*       is_event_used() -- Check whether another widget uses a mapped event.
*
*   SYNOPSIS
*       static int is_event_used (const struct inputdevice_functions *id,
*       int devnum, unsigned int isnum, int isevent);
*
*   FUNCTION
*       Search all controls and subevent slots on one device for the
*       requested event, excluding the specified widget.
*
*   INPUTS
*       id - Backend; devnum - Backend-local device index; isnum - Excluded
*       widget; isevent - Event identifier.
*
*   RESULT
*       One when another widget contains the event; zero otherwise.
*
****************************************************************************
*
*/

static int is_event_used (const struct inputdevice_functions *id, int devnum, unsigned int isnum, int isevent)
{
    unsigned int num;
    int event, flag, sub;
    char *custom;

    for (num = 0; num < id->get_widget_num (devnum); num++) {
	for (sub = 0; sub < MAX_INPUT_SUB_EVENT; sub++) {
	    if (get_event_data (id, devnum, num, &event, &custom, &flag, sub) >= 0) {
		if (event == isevent && isnum != num)
		    return 1;
	    }
	}
    }
    return 0;
}

/****i* januae-inputdevice/inputdevice_get_device_index ********************
*
*   NAME
*       inputdevice_get_device_index() -- Convert a global device index to a
*       backend-local index.
*
*   SYNOPSIS
*       int inputdevice_get_device_index (unsigned int devnum);
*
*   FUNCTION
*       Subtract preceding joystick and mouse device counts to resolve a
*       global index within its corresponding backend.
*
*   INPUTS
*       devnum - Global input device index.
*
*   RESULT
*       Device index within its backend, or minus one outside the reported
*       device ranges.
*
****************************************************************************
*
*/

int inputdevice_get_device_index (unsigned int devnum)
{
    if (devnum < idev[IDTYPE_JOYSTICK].get_num())
	return devnum;
    else if (devnum < idev[IDTYPE_JOYSTICK].get_num() + idev[IDTYPE_MOUSE].get_num())
	return devnum - idev[IDTYPE_JOYSTICK].get_num();
    else if (devnum < idev[IDTYPE_JOYSTICK].get_num() + idev[IDTYPE_MOUSE].get_num() + idev[IDTYPE_KEYBOARD].get_num())
	return devnum - idev[IDTYPE_JOYSTICK].get_num() - idev[IDTYPE_MOUSE].get_num();
    else
	return -1;
}

/****i* januae-inputdevice/gettype *****************************************
*
*   NAME
*       gettype() -- Determine the backend category of a global input
*       device.
*
*   SYNOPSIS
*       static int gettype (unsigned int devnum);
*
*   FUNCTION
*       Compare the global index against joystick, mouse and keyboard device
*       ranges.
*
*   INPUTS
*       devnum - Global input device index.
*
*   RESULT
*       Backend type identifier, or minus one if the index is outside all
*       ranges.
*
****************************************************************************
*
*/

static int gettype (unsigned int devnum)
{
    if (devnum < idev[IDTYPE_JOYSTICK].get_num())
	return IDTYPE_JOYSTICK;
    else if (devnum < idev[IDTYPE_JOYSTICK].get_num() + idev[IDTYPE_MOUSE].get_num())
	return IDTYPE_MOUSE;
    else if (devnum < idev[IDTYPE_JOYSTICK].get_num() + idev[IDTYPE_MOUSE].get_num() + idev[IDTYPE_KEYBOARD].get_num())
	return IDTYPE_KEYBOARD;
    else
	return -1;
}

/****i* januae-inputdevice/getidf ******************************************
*
*   NAME
*       getidf() -- Resolve the backend function table for a device.
*
*   SYNOPSIS
*       static struct inputdevice_functions *getidf (int devnum);
*
*   FUNCTION
*       Index the installed input backend table using the category returned
*       by gettype().
*
*   INPUTS
*       devnum - Global input device index.
*
*   RESULT
*       Pointer to the selected backend function table.
*
****************************************************************************
*
*/

static struct inputdevice_functions *getidf (int devnum)
{
    return &idev[gettype (devnum)];
}


/* returns number of devices of type "type" */
/****i* januae-inputdevice/inputdevice_get_device_total ********************
*
*   NAME
*       inputdevice_get_device_total() -- Count devices of the requested
*       input category.
*
*   SYNOPSIS
*       int inputdevice_get_device_total (int type);
*
*   FUNCTION
*       Invoke the selected backend device-count callback.
*
*   INPUTS
*       type - Backend category identifier.
*
*   RESULT
*       Number of devices reported by the backend.
*
****************************************************************************
*
*/

int inputdevice_get_device_total (int type)
{
    return idev[type].get_num ();
}
/* returns the name of device */
/****i* januae-inputdevice/inputdevice_get_device_name *********************
*
*   NAME
*       inputdevice_get_device_name() -- Return the name of a host input
*       device.
*
*   SYNOPSIS
*       const char *inputdevice_get_device_name (int type, int devnum);
*
*   FUNCTION
*       Delegate to the requested backend device-name callback.
*
*   INPUTS
*       type - Backend category; devnum - Index within that category.
*
*   RESULT
*       Device name supplied by the backend.
*
****************************************************************************
*
*/

const char *inputdevice_get_device_name (int type, int devnum)
{
    return idev[type].get_name (devnum);
}
/* returns state (enabled/disabled) */
/****i* januae-inputdevice/inputdevice_get_device_status *******************
*
*   NAME
*       inputdevice_get_device_status() -- Read whether a host input device
*       is enabled.
*
*   SYNOPSIS
*       int inputdevice_get_device_status (int devnum);
*
*   FUNCTION
*       Resolve its active configuration using the global device index and
*       return the enabled field.
*
*   INPUTS
*       devnum - Global input device index.
*
*   RESULT
*       Stored enabled state of the device.
*
****************************************************************************
*
*/

int inputdevice_get_device_status (int devnum)
{
    const struct inputdevice_functions *idf = getidf (devnum);
    struct uae_input_device *uid = get_uid (idf, inputdevice_get_device_index (devnum));
    return uid->enabled;
}

/* set state (enabled/disabled) */
/****i* januae-inputdevice/inputdevice_set_device_status *******************
*
*   NAME
*       inputdevice_set_device_status() -- Set whether a host input device
*       is enabled.
*
*   SYNOPSIS
*       void inputdevice_set_device_status (int devnum, int enabled);
*
*   FUNCTION
*       Resolve the device configuration by global index and replace its
*       enabled state.
*
*   INPUTS
*       devnum - Global input device index; enabled - New enabled state.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void inputdevice_set_device_status (int devnum, int enabled)
{
    const struct inputdevice_functions *idf = getidf (devnum);
    struct uae_input_device *uid = get_uid (idf, inputdevice_get_device_index (devnum));
    uid->enabled = enabled;
}

/* returns number of axis/buttons and keys from selected device */
/****i* januae-inputdevice/inputdevice_get_widget_num **********************
*
*   NAME
*       inputdevice_get_widget_num() -- Count controls exposed by a host
*       input device.
*
*   SYNOPSIS
*       int inputdevice_get_widget_num (int devnum);
*
*   FUNCTION
*       Resolve the device backend and query its widget-count callback.
*
*   INPUTS
*       devnum - Global input device index.
*
*   RESULT
*       Number of controls reported by the backend.
*
****************************************************************************
*
*/

int inputdevice_get_widget_num (int devnum)
{
    const struct inputdevice_functions *idf = getidf (devnum);
    return idf->get_widget_num (inputdevice_get_device_index (devnum));
}

/****i* januae-inputdevice/get_ename ***************************************
*
*   NAME
*       get_ename() -- Format a mapped emulator event name.
*
*   SYNOPSIS
*       static void get_ename (const struct inputevent *ie, char *out);
*
*   FUNCTION
*       Write the event label and append the key code for keyboard-class
*       events.
*
*   INPUTS
*       ie - Event descriptor; out - Destination text buffer.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       A NULL output pointer causes the routine to return without writing.
*
****************************************************************************
*
*/

static void get_ename (const struct inputevent *ie, char *out)
{
    if (!out)
	return;
    if (ie->allow_mask == AM_K)
	sprintf (out, "%s (0x%02.2X)", ie->name, ie->data);
    else
	strcpy (out, ie->name);
}

/****i* januae-inputdevice/inputdevice_iterate *****************************
*
*   NAME
*       inputdevice_iterate() -- Enumerate assignable emulator events for a
*       control.
*
*   SYNOPSIS
*       int inputdevice_iterate (int devnum, int num, char *name, int *af);
*
*   FUNCTION
*       Advance an internal event iterator, filter candidates by widget
*       type, write the selected event label and return its autofire
*       eligibility.
*
*   INPUTS
*       devnum - Global device index; num - Widget index; name - Output
*       label; af - Autofire output.
*
*   RESULT
*       One when a candidate event is available; zero when iteration is
*       exhausted.
*
****************************************************************************
*
*/

int inputdevice_iterate (int devnum, int num, char *name, int *af)
{
    const struct inputdevice_functions *idf = getidf (devnum);
    static int id_iterator;
    const struct inputevent *ie;
    int mask, data, flags, type;
    int devindex = inputdevice_get_device_index (devnum);
    char *custom;

    *af = 0;
    *name = 0;
    for (;;) {
	ie = &events[++id_iterator];
	if (!ie->confname) {
	    id_iterator = 0;
	    return 0;
	}
	mask = 0;
	type = idf->get_widget_type (devindex, num, 0, 0);
	if (type == IDEV_WIDGET_BUTTON) {
	    if (idf == &idev[IDTYPE_JOYSTICK]) {
		mask |= AM_JOY_BUT;
	    } else {
		mask |= AM_MOUSE_BUT;
	    }
	} else if (type == IDEV_WIDGET_AXIS) {
	    if (idf == &idev[IDTYPE_JOYSTICK]) {
		mask |= AM_JOY_AXIS;
	    } else {
		mask |= AM_MOUSE_AXIS;
	    }
	} else if (type == IDEV_WIDGET_KEY) {
	    mask |= AM_K;
	}
	if (ie->allow_mask & AM_INFO) {
	    const struct inputevent *ie2 = ie + 1;
	    while (!(ie2->allow_mask & AM_INFO)) {
		if (is_event_used (idf, devindex, ie2 - ie, -1)) {
		    ie2++;
		    continue;
		}
		if (ie2->allow_mask & mask) break;
		ie2++;
	    }
	    if (!(ie2->allow_mask & AM_INFO))
		mask |= AM_INFO;
	}
	if (!(ie->allow_mask & mask))
	    continue;
	get_event_data (idf, devindex, num, &data, &custom, &flags, 0);
	get_ename (ie, name);
	*af = (flags & ID_FLAG_AUTOFIRE) ? 1 : 0;
	return 1;
    }
}

/****i* januae-inputdevice/inputdevice_get_mapped_name *********************
*
*   NAME
*       inputdevice_get_mapped_name() -- Read the currently assigned event
*       for a control.
*
*   SYNOPSIS
*       int inputdevice_get_mapped_name (int devnum, int num, int *pflags,
*       char *name, char *custom, int sub);
*
*   FUNCTION
*       Look up the selected mapping and populate the name, custom command
*       and capability flags for its subevent slot.
*
*   INPUTS
*       devnum - Global device index; num - Widget index; pflags - Output
*       capability flags; name - Output event name; custom - Output custom
*       command; sub - Subevent index.
*
*   RESULT
*       Assigned event identifier, or zero when there is no mapping.
*
****************************************************************************
*
*/

int inputdevice_get_mapped_name (int devnum, int num, int *pflags, char *name, char *custom, int sub)
{
    const struct inputdevice_functions *idf = getidf (devnum);
    const struct uae_input_device *uid = get_uid (idf, inputdevice_get_device_index (devnum));
    int flags = 0, flag, data;
    int devindex = inputdevice_get_device_index (devnum);
    char *customp = NULL;

    if (name)
	strcpy (name, "<none>");
    if (custom)
	custom[0] = 0;
    if (pflags)
	*pflags = 0;
    if (uid == 0 || num < 0)
	return 0;
    if (get_event_data (idf, devindex, num, &data, &customp, &flag, sub) < 0)
	return 0;
    if (customp && custom)
	sprintf (custom, "\"%s\"", customp);
    if (flag & ID_FLAG_AUTOFIRE)
	flags |= IDEV_MAPPED_AUTOFIRE_SET;
    if (!data)
	return 0;
    if (events[data].allow_mask & AM_AF)
	flags |= IDEV_MAPPED_AUTOFIRE_POSSIBLE;
    if (pflags)
	*pflags = flags;
    get_ename (&events[data], name);
    return data;
}

/****i* januae-inputdevice/inputdevice_set_mapping *************************
*
*   NAME
*       inputdevice_set_mapping() -- Assign an emulator event to a host
*       control.
*
*   SYNOPSIS
*       int inputdevice_set_mapping (int devnum, int num, const char *name,
*       char *custom, int af, int sub);
*
*   FUNCTION
*       Resolve the event name and widget eligibility, set its autofire flag
*       where supported, and store the new mapping and optional custom
*       command.
*
*   INPUTS
*       devnum - Global device index; num - Widget index; name - Event name;
*       custom - Optional command; af - Autofire selection; sub - Subevent
*       index.
*
*   RESULT
*       One if the mapping is accepted; zero otherwise.
*
****************************************************************************
*
*/

int inputdevice_set_mapping (int devnum, int num, const char *name, char *custom, int af, int sub)
{
    const struct inputdevice_functions *idf = getidf (devnum);
    const struct uae_input_device *uid = get_uid (idf, inputdevice_get_device_index (devnum));
    int eid, data, flag, amask;
    char ename[256];
    int devindex = inputdevice_get_device_index (devnum);

    if (uid == 0 || num < 0)
	return 0;
    if (name) {
	eid = 1;
	while (events[eid].name) {
	    get_ename (&events[eid], ename);
	    if (!strcmp(ename, name)) break;
	    eid++;
	}
	if (!events[eid].name)
	    return 0;
	if (events[eid].allow_mask & AM_INFO)
	    return 0;
    } else {
	eid = 0;
    }
    if (get_event_data (idf, devindex, num, &data, &custom, &flag, sub) < 0)
	return 0;
    if (data >= 0) {
	amask = events[eid].allow_mask;
	flag &= ~ID_FLAG_AUTOFIRE;
	if (amask & AM_AF)
	    flag |= af ? ID_FLAG_AUTOFIRE : 0;
	put_event_data (idf, devindex, num, eid, custom, flag, sub);
	return 1;
    }
    return 0;
}

/****i* januae-inputdevice/inputdevice_get_widget_type *********************
*
*   NAME
*       inputdevice_get_widget_type() -- Return a host control category and
*       optional label.
*
*   SYNOPSIS
*       int inputdevice_get_widget_type (int devnum, int num, char *name);
*
*   FUNCTION
*       Delegate to the backend widget-description callback using the
*       resolved backend-local device index.
*
*   INPUTS
*       devnum - Global device index; num - Widget index; name - Optional
*       output label.
*
*   RESULT
*       Backend widget type identifier.
*
****************************************************************************
*
*/

int inputdevice_get_widget_type (int devnum, int num, char *name)
{
    const struct inputdevice_functions *idf = getidf (devnum);
    return idf->get_widget_type (inputdevice_get_device_index (devnum), num, name, 0);
}

static int config_change;

/****i* januae-inputdevice/inputdevice_config_change ***********************
*
*   NAME
*       inputdevice_config_change() -- Mark the input configuration as
*       changed.
*
*   SYNOPSIS
*       void inputdevice_config_change (void);
*
*   FUNCTION
*       Set the one-bit pending configuration-change indicator.
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

void inputdevice_config_change (void)
{
    config_change = 1;
}

/****i* januae-inputdevice/inputdevice_config_change_test ******************
*
*   NAME
*       inputdevice_config_change_test() -- Read and clear the pending
*       configuration-change flag.
*
*   SYNOPSIS
*       int inputdevice_config_change_test (void);
*
*   FUNCTION
*       Return the stored change indicator and reset it to zero.
*
*   INPUTS
*       None.
*
*   RESULT
*       Prior value of the configuration-change indicator.
*
****************************************************************************
*
*/

int inputdevice_config_change_test (void)
{
    int v = config_change;
    config_change = 0;
    return v;
}

/****i* januae-inputdevice/inputdevice_copyconfig **************************
*
*   NAME
*       inputdevice_copyconfig() -- Copy input preferences and activate the
*       destination.
*
*   SYNOPSIS
*       void inputdevice_copyconfig (const struct uae_prefs *src, struct
*       uae_prefs *dst);
*
*   FUNCTION
*       Copy global input tuning values and configured device arrays into
*       the destination preferences, then activate the copied mappings.
*
*   INPUTS
*       src - Source preferences; dst - Destination preferences.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void inputdevice_copyconfig (const struct uae_prefs *src, struct uae_prefs *dst)
{
    int i, j;

    dst->input_selected_setting = src->input_selected_setting;
    dst->input_joymouse_multiplier = src->input_joymouse_multiplier;
    dst->input_joymouse_deadzone = src->input_joymouse_deadzone;
    dst->input_joystick_deadzone = src->input_joystick_deadzone;
    dst->input_joymouse_speed = src->input_joymouse_speed;
    dst->input_mouse_speed = src->input_mouse_speed;
    dst->input_autofire_framecnt = src->input_autofire_framecnt;
    dst->jport0 = src->jport0;
    dst->jport1 = src->jport1;

    for (i = 0; i < MAX_INPUT_SETTINGS + 1; i++) {
	for (j = 0; j < MAX_INPUT_DEVICES; j++) {
	    memcpy (&dst->joystick_settings[i][j], &src->joystick_settings[i][j], sizeof (struct uae_input_device));
	    memcpy (&dst->mouse_settings[i][j], &src->mouse_settings[i][j], sizeof (struct uae_input_device));
	    memcpy (&dst->keyboard_settings[i][j], &src->keyboard_settings[i][j], sizeof (struct uae_input_device));
	}
    }

    inputdevice_updateconfig (dst);
}

/****i* januae-inputdevice/inputdevice_swap_ports **************************
*
*   NAME
*       inputdevice_swap_ports() -- Exchange assigned emulated joystick port
*       events.
*
*   SYNOPSIS
*       void inputdevice_swap_ports (struct uae_prefs *p, int devnum);
*
*   FUNCTION
*       Inspect a device mapping and replace events directed at the first
*       joystick port with equivalent events for the second port, and
*       conversely.
*
*   INPUTS
*       p - Preferences argument; devnum - Global device index.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void inputdevice_swap_ports (struct uae_prefs *p, int devnum)
{
    const struct inputdevice_functions *idf = getidf (devnum);
    struct uae_input_device *uid = get_uid (idf, inputdevice_get_device_index (devnum));
    int i, j, k, event, unit;
    const struct inputevent *ie, *ie2;

    for (i = 0; i < MAX_INPUT_DEVICE_EVENTS; i++) {
	for (j = 0; j < MAX_INPUT_SUB_EVENT; j++) {
	    event = uid->eventid[i][j];
	    if (event <= 0)
		continue;
	    ie = &events[event];
	    if (ie->unit <= 0)
		continue;
	    unit = ie->unit;
	    k = 1;
	    while (events[k].confname) {
		ie2 = &events[k];
		if (ie2->type == ie->type && ie2->data == ie->data && ie2->unit - 1 == ((ie->unit - 1) ^ 1) && ie2->allow_mask == ie->allow_mask) {
		    uid->eventid[i][j] = k;
		    break;
		}
		k++;
	    }
	}
    }
}

/****i* januae-inputdevice/inputdevice_copy_single_config ******************
*
*   NAME
*       inputdevice_copy_single_config() -- Copy one input settings bank to
*       another.
*
*   SYNOPSIS
*       void inputdevice_copy_single_config (struct uae_prefs *p, int src,
*       int dst, int devnum);
*
*   FUNCTION
*       Copy joystick, mouse or keyboard assignment arrays between settings
*       banks, optionally limiting the copy to the backend type of one
*       device.
*
*   INPUTS
*       p - Preferences containing both banks; src - Source setting; dst -
*       Destination setting; devnum - Device selector, or negative for all
*       types.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void inputdevice_copy_single_config (struct uae_prefs *p, int src, int dst, int devnum)
{
    if (src == dst)
	return;
    if (devnum < 0 || gettype (devnum) == IDTYPE_JOYSTICK)
	memcpy (p->joystick_settings[dst], p->joystick_settings[src], sizeof (struct uae_input_device) * MAX_INPUT_DEVICES);
    if (devnum < 0 || gettype (devnum) == IDTYPE_MOUSE)
	memcpy (p->mouse_settings[dst], p->mouse_settings[src], sizeof (struct uae_input_device) * MAX_INPUT_DEVICES);
    if (devnum < 0 || gettype (devnum) == IDTYPE_KEYBOARD)
	memcpy (p->keyboard_settings[dst], p->keyboard_settings[src], sizeof (struct uae_input_device) * MAX_INPUT_DEVICES);
}

/****i* januae-inputdevice/inputdevice_acquire *****************************
*
*   NAME
*       inputdevice_acquire() -- Acquire all enabled host input devices.
*
*   SYNOPSIS
*       void inputdevice_acquire (void);
*
*   FUNCTION
*       Release existing acquisitions, then call joystick, mouse and
*       keyboard acquire callbacks for devices marked active by the event
*       scan.
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

void inputdevice_acquire (void)
{
    int i;

    inputdevice_unacquire ();
    for (i = 0; i < MAX_INPUT_DEVICES; i++) {
	if (use_joysticks[i])
	    idev[IDTYPE_JOYSTICK].acquire (i, 0);
    }
    for (i = 0; i < MAX_INPUT_DEVICES; i++) {
	if (use_mice[i])
	    idev[IDTYPE_MOUSE].acquire (i, 0);
    }
    for (i = 0; i < MAX_INPUT_DEVICES; i++) {
	if (use_keyboards[i])
	    idev[IDTYPE_KEYBOARD].acquire (i, 0);
    }
}

/****i* januae-inputdevice/inputdevice_unacquire ***************************
*
*   NAME
*       inputdevice_unacquire() -- Release every host input device.
*
*   SYNOPSIS
*       void inputdevice_unacquire (void);
*
*   FUNCTION
*       Call the unacquire callback for each supported joystick, mouse and
*       keyboard index.
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

void inputdevice_unacquire (void)
{
    int i;

    for (i = 0; i < MAX_INPUT_DEVICES; i++)
	idev[IDTYPE_JOYSTICK].unacquire (i);
    for (i = 0; i < MAX_INPUT_DEVICES; i++)
	idev[IDTYPE_MOUSE].unacquire (i);
    for (i = 0; i < MAX_INPUT_DEVICES; i++)
	idev[IDTYPE_KEYBOARD].unacquire (i);
}

/* Call this function when host machine's joystick/joypad/etc button state changes
 * This function translates button events to Amiga joybutton/joyaxis/keyboard events
 */

/* button states:
 * state = -1 -> mouse wheel turned or similar (button without release)
 * state = 1 -> button pressed
 * state = 0 -> button released
 */

/****i* januae-inputdevice/setjoybuttonstate *******************************
*
*   NAME
*       setjoybuttonstate() -- Submit a single joystick button transition.
*
*   SYNOPSIS
*       void setjoybuttonstate (int joy, int button, int state);
*
*   FUNCTION
*       Ignore disabled joysticks and forward the normalized button state to
*       the configured mapping dispatcher.
*
*   INPUTS
*       joy - Physical joystick index; button - Button index; state -
*       Pressed or released state.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void setjoybuttonstate (int joy, int button, int state)
{
    if (!joysticks[joy].enabled)
	return;
    setbuttonstateall (&joysticks[joy], &joysticks2[joy], button, state ? 1 : 0);
}

/* buttonmask = 1 = normal toggle button, 0 = mouse wheel turn or similar
 */
/****i* januae-inputdevice/setjoybuttonstateall ****************************
*
*   NAME
*       setjoybuttonstateall() -- Submit joystick button states from bit
*       masks.
*
*   SYNOPSIS
*       void setjoybuttonstateall (int joy, uae_u32 buttonbits, uae_u32
*       buttonmask);
*
*   FUNCTION
*       Apply level-based transitions for buttons selected by the mask and
*       single-shot impulses for unmasked asserted buttons.
*
*   INPUTS
*       joy - Physical joystick index; buttonbits - Active button bits;
*       buttonmask - Buttons with persistent state.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void setjoybuttonstateall (int joy, uae_u32 buttonbits, uae_u32 buttonmask)
{
    int i;

    if (!joysticks[joy].enabled)
	return;
    for (i = 0; i < ID_BUTTON_TOTAL; i++) {
	if (buttonmask & (1 << i))
	    setbuttonstateall (&joysticks[joy], &joysticks2[joy], i, (buttonbits & (1 << i)) ? 1 : 0);
	else if (buttonbits & (1 << i))
	    setbuttonstateall (&joysticks[joy], &joysticks2[joy], i, -1);
    }
}
/* mouse buttons (just like joystick buttons)
 */
/****i* januae-inputdevice/setmousebuttonstateall **************************
*
*   NAME
*       setmousebuttonstateall() -- Submit mouse button states from bit
*       masks.
*
*   SYNOPSIS
*       void setmousebuttonstateall (int mouse, uae_u32 buttonbits, uae_u32
*       buttonmask);
*
*   FUNCTION
*       Dispatch masked mouse button levels and unmasked one-shot button
*       events through the mapped event table.
*
*   INPUTS
*       mouse - Physical mouse index; buttonbits - Active button bits;
*       buttonmask - Buttons with persistent state.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void setmousebuttonstateall (int mouse, uae_u32 buttonbits, uae_u32 buttonmask)
{
    int i;

    if (!mice[mouse].enabled)
	return;
    for (i = 0; i < ID_BUTTON_TOTAL; i++) {
	if (buttonmask & (1 << i))
	    setbuttonstateall (&mice[mouse], &mice2[mouse], i, (buttonbits & (1 << i)) ? 1 : 0);
	else if (buttonbits & (1 << i))
	    setbuttonstateall (&mice[mouse], &mice2[mouse], i, -1);
    }
}

/****i* januae-inputdevice/setmousebuttonstate *****************************
*
*   NAME
*       setmousebuttonstate() -- Submit a single mouse button transition.
*
*   SYNOPSIS
*       void setmousebuttonstate (int mouse, int button, int state);
*
*   FUNCTION
*       Ignore disabled mice and dispatch the indicated button state through
*       the mouse mapping configuration.
*
*   INPUTS
*       mouse - Physical mouse index; button - Button index; state - Press,
*       release or impulse state.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void setmousebuttonstate (int mouse, int button, int state)
{
  //kprintf("FW: setmousebuttonstate..\n");
    if (!mice[mouse].enabled)
	return;
    setbuttonstateall (&mice[mouse], &mice2[mouse], button, state);
}

/* same for joystick axis (analog or digital)
 * (0 = center, -max = full left/top, max = full right/bottom)
 */
/****i* januae-inputdevice/setjoystickstate ********************************
*
*   NAME
*       setjoystickstate() -- Submit a joystick axis position.
*
*   SYNOPSIS
*       void setjoystickstate (int joy, int axis, int state, int max);
*
*   FUNCTION
*       Apply the configured deadzone, compare the axis with its previous
*       state, dispatch mapped subevents and remember the new position.
*
*   INPUTS
*       joy - Physical joystick index; axis - Axis index; state - Signed
*       position; max - Magnitude scale.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void setjoystickstate (int joy, int axis, int state, int max)
{
    struct uae_input_device *id = &joysticks[joy];
    struct uae_input_device2 *id2 = &joysticks2[joy];
    int deadzone = currprefs.input_joymouse_deadzone * max / 100;
    int i, v1, v2;

    if (!joysticks[joy].enabled)
	return;
    v1 = state;
    v2 = id2->states[axis];
    if (v1 < deadzone && v1 > -deadzone)
	v1 = 0;
    if (v2 < deadzone && v2 > -deadzone)
	v2 = 0;
    if (v1 == v2)
	return;
    for (i = 0; i < MAX_INPUT_SUB_EVENT; i++)
	handle_input_event (id->eventid[ID_AXIS_OFFSET + axis][i], state, max,
	id->flags[ID_AXIS_OFFSET + axis][i]);
    id2->states[axis] = state;
}

/****i* januae-inputdevice/setmousestate ***********************************
*
*   NAME
*       setmousestate() -- Submit absolute or relative mouse axis movement.
*
*   SYNOPSIS
*       void setmousestate (int mouse, int axis, int data, int isabs);
*
*   FUNCTION
*       Convert host coordinates or deltas into the configured mouse speed
*       scale, preserve fractional movement, dispatch axis events and update
*       guest mouse coordinates.
*
*   INPUTS
*       mouse - Physical mouse index; axis - Axis index; data - Motion
*       value; isabs - Nonzero for absolute coordinates.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void setmousestate (int mouse, int axis, int data, int isabs)
{
    int i, v;
    double *mouse_p, *oldm_p, d, diff;
    struct uae_input_device *id = &mice[mouse];
    static double fract1[MAX_INPUT_DEVICES][MAX_INPUT_DEVICE_EVENTS];
    static double fract2[MAX_INPUT_DEVICES][MAX_INPUT_DEVICE_EVENTS];

    if (!mice[mouse].enabled)
	return;
    d = 0;
    mouse_p = &mouse_axis[mouse][axis];
    oldm_p = &oldm_axis[mouse][axis];
    if (!isabs) {
	*oldm_p = *mouse_p;
	*mouse_p += data;
	d = (*mouse_p - *oldm_p) * currprefs.input_mouse_speed / 100.0;
    } else {
	d = data - (int)(*oldm_p);
	*oldm_p = data;
	*mouse_p += d;
	if (axis == 0)
	    lastmx = data;
	else
	    lastmy = data;
    }
    v = (int)(d > 0 ? d + 0.5 : d - 0.5);
    fract1[mouse][axis] += d;
    fract2[mouse][axis] += v;
    diff = fract2[mouse][axis] - fract1[mouse][axis];
    if (diff > 1 || diff < -1) {
	v -= (int)diff;
	fract2[mouse][axis] -= diff;
    }
    for (i = 0; i < MAX_INPUT_SUB_EVENT; i++)
	handle_input_event (id->eventid[ID_AXIS_OFFSET + axis][i], v, 0, 0);
    mousehack_helper ();
}

/****i* januae-inputdevice/warpmode ****************************************
*
*   NAME
*       warpmode() -- Change the emulator warp-speed state.
*
*   SYNOPSIS
*       void warpmode (int mode);
*
*   FUNCTION
*       Toggle or explicitly set turbo emulation, adjust frame-rate
*       preferences, pause or resume audio as appropriate and recalculate
*       video synchronization timing.
*
*   INPUTS
*       mode - Negative to toggle, zero to disable or positive to enable
*       warp mode.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void warpmode (int mode)
{
    if (mode < 0) {
	if (turbo_emulation) {
	    changed_prefs.gfx_framerate = currprefs.gfx_framerate = turbo_emulation;
	    turbo_emulation = 0;
	}  else {
	    turbo_emulation = currprefs.gfx_framerate;
	}
    } else if (mode == 0 && turbo_emulation > 0) {
	changed_prefs.gfx_framerate = currprefs.gfx_framerate = turbo_emulation;
	turbo_emulation = 0;
    } else if (mode > 0 && !turbo_emulation) {
	turbo_emulation = currprefs.gfx_framerate;
    }
    if (turbo_emulation) {
	if (!currprefs.cpu_cycle_exact && !currprefs.blitter_cycle_exact)
	    changed_prefs.gfx_framerate = currprefs.gfx_framerate = 10;
	audio_pause ();
    } else {
	audio_resume ();
    }
    compute_vsynctime ();
}

/****i* januae-inputdevice/jsem_isjoy **************************************
*
*   NAME
*       jsem_isjoy() -- Resolve a configured port to a joystick device.
*
*   SYNOPSIS
*       int jsem_isjoy (int port, const struct uae_prefs *p);
*
*   FUNCTION
*       Decode the selected legacy port value and validate the resulting
*       joystick index against the available device count.
*
*   INPUTS
*       port - Emulated port number; p - Preferences containing port
*       assignments.
*
*   RESULT
*       Joystick device index, or minus one when the port is not mapped to
*       an available joystick.
*
****************************************************************************
*
*/

int jsem_isjoy (int port, const struct uae_prefs *p)
{
    int v = JSEM_DECODEVAL (port, p);
    if (v < JSEM_JOYS)
	return -1;
    v -= JSEM_JOYS;
    if (v >= inputdevice_get_device_total (IDTYPE_JOYSTICK))
	return -1;
    return v;
}

/****i* januae-inputdevice/jsem_ismouse ************************************
*
*   NAME
*       jsem_ismouse() -- Resolve a configured port to a mouse device.
*
*   SYNOPSIS
*       int jsem_ismouse (int port, const struct uae_prefs *p);
*
*   FUNCTION
*       Decode the selected legacy port value and validate the resulting
*       mouse index against the available device count.
*
*   INPUTS
*       port - Emulated port number; p - Preferences containing port
*       assignments.
*
*   RESULT
*       Mouse device index, or minus one when no available mouse is
*       selected.
*
****************************************************************************
*
*/

int jsem_ismouse (int port, const struct uae_prefs *p)
{
    int v = JSEM_DECODEVAL (port, p);
    if (v < JSEM_MICE)
	return -1;
    v -= JSEM_MICE;
    if (v >= inputdevice_get_device_total (IDTYPE_MOUSE))
	return -1;
    return v;
}

/****i* januae-inputdevice/jsem_iskbdjoy ***********************************
*
*   NAME
*       jsem_iskbdjoy() -- Resolve a configured port to a keyboard joystick
*       layout.
*
*   SYNOPSIS
*       int jsem_iskbdjoy (int port, const struct uae_prefs *p);
*
*   FUNCTION
*       Decode the selected legacy port value and check that it lies within
*       the supported keyboard joystick layout range.
*
*   INPUTS
*       port - Emulated port number; p - Preferences containing port
*       assignments.
*
*   RESULT
*       Keyboard joystick layout index, or minus one when the selection is
*       not a keyboard joystick layout.
*
****************************************************************************
*
*/

int jsem_iskbdjoy (int port, const struct uae_prefs *p)
{
    int v = JSEM_DECODEVAL (port, p);
    if (v < JSEM_KBDLAYOUT)
	return -1;
    v -= JSEM_KBDLAYOUT;
    if (v >= JSEM_LASTKBD)
	return -1;
    return v;
}
