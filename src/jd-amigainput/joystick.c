 /*
  * E-UAE - The portable Amiga Emulator
  *
  * AmigaInput joystick driver
  *
  * Copyright 2005 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "inputdevice.h"

#include <amigainput/amigainput.h>
#include <proto/exec.h>
#include <proto/amigainput.h>

#define MAX_JOYSTICKS                   MAX_INPUT_DEVICES
#define MAX_AXES                        2
#define MAX_BUTTONS                     12

struct Library   *AIN_Base;
struct AIN_IFace *IAIN;

/*
 * Per-joystick data private to driver
 */
struct joystick
{
    AIN_DeviceID        id;
    const char         *name;

    AIN_DeviceHandle   *handle;
    APTR                context;

    uint32              axisCount;
    uint32              buttonCount;

    uint32              axisBufferOffset[MAX_AXES];
    int32               axisData[MAX_AXES];

    uint32              buttonBufferOffset[MAX_BUTTONS];
    int32               buttonData[MAX_BUTTONS];
};


static APTR            joystickContext;
static uint32          joystickCount;
static struct joystick joystickList [MAX_JOYSTICKS];


/****i* januae-amigainput/openAmigaInput
 *
 *   NAME
 *       openAmigaInput()
 *
 *   SYNOPSIS
 *       static BOOL openAmigaInput (void)
 *
 *   FUNCTION
 *       Open the AmigaInput library and obtain its main interface.
 *
 *   RESULT
 *       Returns TRUE when the library and interface are available, FALSE otherwise.
 *
 *   NOTES
 *       Requests AmigaInput.library version 51 and interface 'main' version 1. A
 *       failed interface acquisition closes the library. A missing library is
 *       logged.
 *
 ****************************************************************************/

static BOOL openAmigaInput (void)
{
    AIN_Base = OpenLibrary ("AmigaInput.library", 51);

    if (AIN_Base) {
	IAIN = (struct AIN_IFace *) GetInterface (AIN_Base, "main", 1, NULL);

	if (!IAIN) {
	    CloseLibrary (AIN_Base);
	    AIN_Base = NULL;
	}
    } else
	write_log ("Failed to open AmigaInput.library\n");

    return AIN_Base != NULL;
}

/****i* januae-amigainput/closeAmigaInput
 *
 *   NAME
 *       closeAmigaInput()
 *
 *   SYNOPSIS
 *       static void closeAmigaInput (void)
 *
 *   FUNCTION
 *       Release the AmigaInput interface and library held by the driver.
 *
 *   NOTES
 *       Drops the interface before closing the library; does not release joystick
 *       device handles, delete the context or free stored device names.
 *
 ****************************************************************************/

static void closeAmigaInput (void)
{
    if (IAIN) {
	DropInterface ((struct Interface *)IAIN);
	IAIN = NULL;
    }

    if (AIN_Base) {
	CloseLibrary (AIN_Base);
	AIN_Base = NULL;
    }
}


/* A handy container to encapsulate the information we
 * need when enumerating joysticks on the system.
 */
struct enumPacket
{
     APTR             context;
     uint32          *count;
     struct joystick *joyList;
};

/*
 * Callback to enumerate joysticks
 */
/****i* januae-amigainput/enumerateJoysticks
 *
 *   NAME
 *       enumerateJoysticks()
 *
 *   SYNOPSIS
 *       static BOOL enumerateJoysticks (AIN_Device *device, void *UserData)
 *
 *   FUNCTION
 *       Inspect an enumerated AmigaInput device and record its joystick controls.
 *
 *   RESULT
 *       Returns the final axis/button offset-query result, or FALSE when the device is not accepted.
 *
 *   NOTES
 *       UserData is an enumPacket with a live context, count and array. Only
 *       AINDT_JOYSTICK is accepted, with at most two axes and twelve buttons. The
 *       device name is duplicated. Because result starts FALSE, a zero-axis device
 *       cannot pass button-offset queries; failed queries do not free the duplicated
 *       name. Enumeration capacity is MAX_INPUT_DEVICES.
 *
 ****************************************************************************/

static BOOL enumerateJoysticks (AIN_Device *device, void *UserData)
{
    APTR             context =  ((struct enumPacket *)UserData)->context;
    uint32          *count   =  ((struct enumPacket *)UserData)->count;
    struct joystick *joy     = &((struct enumPacket *)UserData)->joyList[*count];

    BOOL result = FALSE;

    if (*count < MAX_JOYSTICKS) {
	if (device->Type == AINDT_JOYSTICK) {

	    unsigned int i;

	    joy->context     = context;
	    joy->id          = device->DeviceID;
	    joy->name        = my_strdup (device->DeviceName);
	    joy->axisCount   = device->NumAxes;
	    joy->buttonCount = device->NumButtons;

	    if (joy->axisCount   > MAX_AXES)    joy->axisCount   = MAX_AXES;
	    if (joy->buttonCount > MAX_BUTTONS) joy->buttonCount = MAX_BUTTONS;

	    /* Query offsets in ReadDevice buffer for axes' data */
	    for (i = 0; i < joy->axisCount; i++)
		result =           AIN_Query (joy->context, joy->id, AINQ_AXIS_OFFSET, i,
					      &(joy->axisBufferOffset[i]), 4);

	    /* Query offsets in ReadDevice buffer for buttons' data */
	    for (i = 0; i < joy->buttonCount; i++)
		result = result && AIN_Query (joy->context, joy->id, AINQ_BUTTON_OFFSET, i,
					      &(joy->buttonBufferOffset[i]), 4);

	    if (result) {
		write_log ("Found joystick #%d (AI ID=%d) '%s' with %d axes, %d buttons\n",
			   *count, joy->id, joy->name, joy->axisCount, joy->buttonCount);

		(*count)++;
	    }
	}
    }

    return result;
}


/****i* januae-amigainput/init_joysticks
 *
 *   NAME
 *       init_joysticks()
 *
 *   SYNOPSIS
 *       static int init_joysticks (void)
 *
 *   FUNCTION
 *       Create an AmigaInput context and enumerate usable joystick devices.
 *
 *   RESULT
 *       Returns 1 after context creation and enumeration, or 0 if already initialized or initialization fails.
 *
 *   NOTES
 *       Opening the library precedes context creation. Enumeration success is not
 *       required for a return value of 1. A failed context creation leaves the
 *       opened library/interface unclosed.
 *
 ****************************************************************************/

static int init_joysticks (void)
{
    int i;
    int success = 0;

    if (!joystickContext) {

    	if (openAmigaInput ()) {

	    joystickContext = AIN_CreateContext (1, NULL);
	    if (joystickContext) {

		struct enumPacket packet = {
		    joystickContext, &joystickCount, &joystickList[0]
		};

		AIN_EnumDevices (joystickContext, enumerateJoysticks, &packet);

		write_log ("Found %d joysticks\n", joystickCount);
	       
	        success = 1;
            }
	}
    }

    return success;
}

/****i* januae-amigainput/close_joysticks
 *
 *   NAME
 *       close_joysticks()
 *
 *   SYNOPSIS
 *       static void close_joysticks (void)
 *
 *   FUNCTION
 *       Release acquired joystick handles and destroy the enumeration context.
 *
 *   NOTES
 *       Resets joystickCount, but does not call closeAmigaInput() and does not free
 *       the per-device duplicated names.
 *
 ****************************************************************************/

static void close_joysticks (void)
{
    unsigned int i = joystickCount;

    while (i-- > 0) {
	struct joystick *joy = &joystickList[i];

	if (joy->handle) {
	    AIN_ReleaseDevice (joy->context, joy->handle);
	    joy->handle = 0;
	}
    }
    joystickCount = 0;

    AIN_DeleteContext (joystickContext);
    joystickContext = 0;
}

#define BUFFER_OFFSET(buffer, offset)	(((int32 *)buffer)[offset])

/****i* januae-amigainput/read_joy
 *
 *   NAME
 *       read_joy()
 *
 *   SYNOPSIS
 *       static void read_joy (unsigned int num)
 *
 *   FUNCTION
 *       Poll one acquired AmigaInput device and forward changed control states to UAE.
 *
 *   NOTES
 *       num must identify an enumerated joystick. Updates axes only when changed,
 *       using range 32767; buttons are converted to 0/1. No events are delivered if
 *       no handle is acquired or AIN_ReadDevice fails.
 *
 ****************************************************************************/

static void read_joy (unsigned int num)
{
    struct joystick *joy = &joystickList[num];

    if (joy->handle) {
	void *buffer;

	/*
	 * Poll device for data
	 */
	if (AIN_ReadDevice (joy->context, joy->handle, &buffer)) {
	    unsigned int i;

	    /* Extract axis data from buffer and notify UAE of any changes
	     * in axis state
	     */
	    for (i = 0; i < joy->axisCount; i++) {
		int axisdata = BUFFER_OFFSET (buffer, joy->axisBufferOffset[i]);

		if (axisdata != joy->axisData[i]) {
		    setjoystickstate (num, i, axisdata, 32767);
		    joy->axisData[i] = axisdata;
		}
	    }

	    /* Extract button data from buffer and notify SDL of any changes
	     * in button state
	     *
	     * Note: We don't support analog buttons.
	     */
	    for (i = 0; i < joy->buttonCount; i++) {
		int buttondata = BUFFER_OFFSET (buffer, joy->buttonBufferOffset[i]);

		if (buttondata != joy->buttonData[i]) {
		    setjoybuttonstate (num, i, buttondata ? 1 : 0);
		    joy->buttonData[i] = buttondata;
		}
	    }

	}
    }
}

/*
 * Query number of joysticks attached to system
 */
/****i* januae-amigainput/get_joystick_count
 *
 *   NAME
 *       get_joystick_count()
 *
 *   SYNOPSIS
 *       static unsigned int get_joystick_count (void)
 *
 *   FUNCTION
 *       Report the number of accepted AmigaInput joystick devices.
 *
 *   RESULT
 *       Returns joystickCount as maintained by enumeration and shutdown.
 *
 ****************************************************************************/

static unsigned int get_joystick_count (void)
{
    return joystickCount;
}

/*
 * Query number of 'widgets' supported by joystick #joynum
 */
/****i* januae-amigainput/get_joystick_widget_num
 *
 *   NAME
 *       get_joystick_widget_num()
 *
 *   SYNOPSIS
 *       static unsigned int get_joystick_widget_num (unsigned int joynum)
 *
 *   FUNCTION
 *       Report the axis and button controls of one enumerated joystick.
 *
 *   RESULT
 *       Returns the sum of the recorded axis and button counts.
 *
 *   NOTES
 *       joynum must be a valid index; the function performs no bounds check.
 *
 ****************************************************************************/

static unsigned int get_joystick_widget_num (unsigned int joynum)
{
    return joystickList[joynum].axisCount + joystickList[joynum].buttonCount;
}

/*
 * Query type of widget #widgetnum on joystick #joynum
 */
/****i* januae-amigainput/get_joystick_widget_type
 *
 *   NAME
 *       get_joystick_widget_type()
 *
 *   SYNOPSIS
 *       static int get_joystick_widget_type (unsigned int joynum, unsigned int widgetnum, char *name, uae_u32 *code)
 *
 *   FUNCTION
 *       Classify a joystick widget and optionally format its display name.
 *
 *   RESULT
 *       Returns IDEV_WIDGET_AXIS, IDEV_WIDGET_BUTTON or IDEV_WIDGET_NONE.
 *
 *   NOTES
 *       joynum must be valid. widgetnum addresses axes first, then buttons. name,
 *       when non-NULL, must be large enough for the generated label. The code output
 *       parameter is not assigned.
 *
 ****************************************************************************/

static int get_joystick_widget_type (unsigned int joynum, unsigned int widgetnum, char *name, uae_u32 *code)
{
    struct joystick *joy = &joystickList[joynum];

    if (widgetnum >= joy->axisCount && widgetnum < joy->axisCount + joy->buttonCount) {
	if (name)
	    sprintf (name, "Button %d", widgetnum + 1 - joy->axisCount);
	return IDEV_WIDGET_BUTTON;
    } else if (widgetnum < joy->axisCount) {
	if (name)
	    sprintf (name, "Axis %d", widgetnum + 1);
	return IDEV_WIDGET_AXIS;
    }
    return IDEV_WIDGET_NONE;
}

/****i* januae-amigainput/get_joystick_widget_first
 *
 *   NAME
 *       get_joystick_widget_first()
 *
 *   SYNOPSIS
 *       static int get_joystick_widget_first (unsigned int joynum, int type)
 *
 *   FUNCTION
 *       Find the initial widget offset for a requested control type.
 *
 *   RESULT
 *       Returns 0 for axes, axisCount for buttons, or -1 for other types.
 *
 *   NOTES
 *       joynum must identify an enumerated joystick.
 *
 ****************************************************************************/

static int get_joystick_widget_first (unsigned int joynum, int type)
{
    switch (type) {
	case IDEV_WIDGET_BUTTON:
	    return joystickList[joynum].axisCount;
	case IDEV_WIDGET_AXIS:
	    return 0;
    }
    return -1;
}

/****i* januae-amigainput/get_joystick_name
 *
 *   NAME
 *       get_joystick_name()
 *
 *   SYNOPSIS
 *       static const char *get_joystick_name (unsigned int joynum)
 *
 *   FUNCTION
 *       Return the stored name of an enumerated joystick.
 *
 *   RESULT
 *       Returns the name pointer recorded during enumeration.
 *
 *   NOTES
 *       The returned pointer is owned by the driver; no copy is made or bounds check
 *       performed.
 *
 ****************************************************************************/

static const char *get_joystick_name (unsigned int joynum)
{
    return (char *)joystickList[joynum].name;
}

/****i* januae-amigainput/read_joysticks
 *
 *   NAME
 *       read_joysticks()
 *
 *   SYNOPSIS
 *       static void read_joysticks (void)
 *
 *   FUNCTION
 *       Poll all accepted joystick devices for changed axes and buttons.
 *
 *   NOTES
 *       Polls joystick indices in descending order. Devices without an acquired
 *       handle do not produce events.
 *
 ****************************************************************************/

static void read_joysticks (void)
{
    unsigned int i = joystickCount;

    while (i > 0)
	read_joy (--i);
}

/****i* januae-amigainput/acquire_joy
 *
 *   NAME
 *       acquire_joy()
 *
 *   SYNOPSIS
 *       static int acquire_joy (unsigned int joynum, int flags)
 *
 *   FUNCTION
 *       Acquire an AmigaInput device handle for an enumerated joystick.
 *
 *   RESULT
 *       Returns 1 when AIN_ObtainDevice succeeds, otherwise 0.
 *
 *   NOTES
 *       joynum must be valid. flags is unused. Existing handles are not released
 *       before reacquisition; a failed request is logged.
 *
 ****************************************************************************/

static int acquire_joy (unsigned int joynum, int flags)
{
    struct joystick *joy = &joystickList[joynum];
    int result = 0;

    joy->handle = AIN_ObtainDevice (joy->context, joy->id);

    if (joy->handle)
        result = 1;
    else
        write_log ("Failed to acquire joy\n");
   return result;
}

/****i* januae-amigainput/unacquire_joy
 *
 *   NAME
 *       unacquire_joy()
 *
 *   SYNOPSIS
 *       static void unacquire_joy (unsigned int joynum)
 *
 *   FUNCTION
 *       Release an acquired AmigaInput handle for one joystick.
 *
 *   NOTES
 *       joynum must be valid. A non-NULL handle is released and then cleared; an
 *       absent handle is ignored.
 *
 ****************************************************************************/

static void unacquire_joy (unsigned int joynum)
{
    struct joystick *joy = &joystickList[joynum];

    if (joy->handle) {
           AIN_ReleaseDevice (joy->context, joy->handle);
           joy->handle = 0;
    }
}

struct inputdevice_functions inputdevicefunc_joystick = {
    init_joysticks,
    close_joysticks,
    acquire_joy,
    unacquire_joy,
    read_joysticks,
    get_joystick_count,
    get_joystick_name,
    get_joystick_widget_num,
    get_joystick_widget_type,
    get_joystick_widget_first
};

/*
 * Set default inputdevice config
 */
/****i* januae-amigainput/input_get_default_joystick
 *
 *   NAME
 *       input_get_default_joystick()
 *
 *   SYNOPSIS
 *       void input_get_default_joystick (struct uae_input_device *uid)
 *
 *   FUNCTION
 *       Populate default UAE joystick mappings for enumerated AmigaInput devices.
 *
 *   NOTES
 *       uid must have entries for every enumerated joystick, and at least one entry
 *       even when none is found. Maps two axes and the first three buttons to
 *       alternating joystick ports, then unconditionally enables uid[0].
 *
 ****************************************************************************/

void input_get_default_joystick (struct uae_input_device *uid)
{
    unsigned int i, port;

    for (i = 0; i < joystickCount; i++) {
	port = i & 1;
	uid[i].eventid[ID_AXIS_OFFSET + 0][0]   = port ? INPUTEVENT_JOY2_HORIZ : INPUTEVENT_JOY1_HORIZ;
	uid[i].eventid[ID_AXIS_OFFSET + 1][0]   = port ? INPUTEVENT_JOY2_VERT  : INPUTEVENT_JOY1_VERT;
	uid[i].eventid[ID_BUTTON_OFFSET + 0][0] = port ? INPUTEVENT_JOY2_FIRE_BUTTON : INPUTEVENT_JOY1_FIRE_BUTTON;
	uid[i].eventid[ID_BUTTON_OFFSET + 1][0] = port ? INPUTEVENT_JOY2_2ND_BUTTON  : INPUTEVENT_JOY1_2ND_BUTTON;
	uid[i].eventid[ID_BUTTON_OFFSET + 2][0] = port ? INPUTEVENT_JOY2_3RD_BUTTON  : INPUTEVENT_JOY1_3RD_BUTTON;
    }
    uid[0].enabled = 1;
}
