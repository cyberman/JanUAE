/************************************************************************ 
 *
 * Joystick emulation for AmigaOS using lowlevel.library
 *
 * Copyright 1996, 1997 Samuel Devulder
 * Copyright 2003-2005  Richard Drummond
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
 ************************************************************************/

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "inputdevice.h"

#include <libraries/lowlevel.h>
#include <proto/exec.h>
#include <proto/lowlevel.h>

struct Library *LowLevelBase=NULL;
struct LowLevelIFace *ILowLevel;

static unsigned int nr_joysticks;

#define MAX_BUTTONS  2
#define MAX_AXLES    2
#define FIRST_AXLE   0
#define FIRST_BUTTON 2


/****i* januae-joystick/init_joysticks *************************************
*
*   NAME
*       init_joysticks() -- Initialize lowlevel joystick input.
*
*   SYNOPSIS
*       static int init_joysticks (void);
*
*   FUNCTION
*       Open lowlevel.library (with host-specific version requirements) and
*       advertise two ports when the library is available.
*
*   INPUTS
*       None.
*
*   RESULT
*       1 when lowlevel.library is available; otherwise 0.
*
*   NOTES
*       AROS reuses an already opened LowLevelBase. AmigaOS 4 also obtains
*       the main interface.
*
****************************************************************************
*
*/

static int init_joysticks (void)
{
#ifdef __AROS__
    /* open it only once */
    if(!LowLevelBase) {
      LowLevelBase = (struct Library *) OpenLibrary ("lowlevel.library", 37);
    }
#else
    LowLevelBase = (struct Library *) OpenLibrary ("lowlevel.library", 39);
#endif
#ifdef __amigaos4__
    if (LowLevelBase) {
	ILowLevel = (struct LowLevelIFace *)GetInterface (LowLevelBase, "main", 1, NULL);
	if (!ILowLevel) {
	    CloseLibrary (LowLevelBase);
	    LowLevelBase = 0;
	}
    }
#endif

    if (LowLevelBase) {
	nr_joysticks = 2;
	return 1;
    } else {
	nr_joysticks = 0;
	return 0;
    }
}

/****i* januae-joystick/close_joysticks ************************************
*
*   NAME
*       close_joysticks() -- Close lowlevel joystick support.
*
*   SYNOPSIS
*       static void close_joysticks (void);
*
*   FUNCTION
*       Close lowlevel.library when open and clear LowLevelBase.
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

static void close_joysticks (void)
{
    if (LowLevelBase) {
	CloseLibrary (LowLevelBase);
	LowLevelBase = NULL;
    }
}

/****i* januae-joystick/acquire_joy ****************************************
*
*   NAME
*       acquire_joy() -- Acquire a joystick input port.
*
*   SYNOPSIS
*       static int acquire_joy (unsigned int num, int flags);
*
*   FUNCTION
*       Report successful acquisition without changing port state.
*
*   INPUTS
*       num - Port index; ignored. flags - Acquisition flags; ignored.
*
*   RESULT
*       1.
*
****************************************************************************
*
*/

static int acquire_joy (unsigned int num, int flags)
{
    return 1;
}

/****i* januae-joystick/unacquire_joy **************************************
*
*   NAME
*       unacquire_joy() -- Release a joystick input port.
*
*   SYNOPSIS
*       static void unacquire_joy (unsigned int num);
*
*   FUNCTION
*       Return without changing port state.
*
*   INPUTS
*       num - Port index; ignored.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void unacquire_joy (unsigned int num)
{
}

/****i* januae-joystick/read_joy *******************************************
*
*   NAME
*       read_joy() -- Read a lowlevel joystick port.
*
*   SYNOPSIS
*       static void read_joy (unsigned int nr);
*
*   FUNCTION
*       Read the direction and two button flags from lowlevel.library and
*       forward axis and button values when the port is available.
*       Optionally read Catweasel joystick data.
*
*   INPUTS
*       nr - Joystick port index.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void read_joy (unsigned int nr)
{
    if (LowLevelBase != NULL) {
        /* ReadJoyPort is not available for AROS ATM (?) */
	ULONG state = ReadJoyPort (nr);

	if ((state & JP_TYPE_MASK) != JP_TYPE_NOTAVAIL) {
	    int x = 0, y = 0;

	    if (state & JPF_JOY_UP)
	        y = -1;
	    else if (state & JPF_JOY_DOWN)
	        y = 1;
	    if (state & JPF_JOY_LEFT)
	        x = -1;
	    else if (state & JPF_JOY_RIGHT)
	        x = 1;

	    setjoystickstate (nr, 0, x, 1);
	    setjoystickstate (nr, 1, y, 1);

	    setjoybuttonstate (nr, 0, state & JPF_BUTTON_RED);
	    setjoybuttonstate (nr, 1, state & JPF_BUTTON_BLUE);
	}
    }

#if defined(CATWEASEL)
    if(currprefs.catweasel_joy) {	
	uae_u8 tDirection, tButtons;
	int x = 0, y = 0, state=0;

	if(catweasel_read_joystick( nr, &tDirection, &tButtons )) {

	    if ( (tDirection & 1) == 0)
		x = 1;
	    if ( (tDirection & 2) == 0 )
		x = -1;
	    if ( (tDirection & 4) == 0 )
		y = 1;
	    if ( (tDirection & 8) == 0 )
		y = -1;

	    if ( tButtons )
		state |= JPF_BUTTON_RED;

	    setjoystickstate (nr, 0, x, 1);
	    setjoystickstate (nr, 1, y, 1);

	    setjoybuttonstate (nr, 0, state & JPF_BUTTON_RED);
	    setjoybuttonstate (nr, 1, state & JPF_BUTTON_BLUE);
	}
		
      }
#endif
}

/****i* januae-joystick/read_joysticks *************************************
*
*   NAME
*       read_joysticks() -- Poll both joystick ports.
*
*   SYNOPSIS
*       static void read_joysticks (void);
*
*   FUNCTION
*       Invoke read_joy() for ports zero and one.
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

static void read_joysticks (void)
{
    read_joy (0);
    read_joy (1);
}

/****i* januae-joystick/get_joystick_num ***********************************
*
*   NAME
*       get_joystick_num() -- Report the initialized joystick count.
*
*   SYNOPSIS
*       static unsigned int get_joystick_num (void);
*
*   FUNCTION
*       Return the number of ports made available by init_joysticks().
*
*   INPUTS
*       None.
*
*   RESULT
*       Current value of nr_joysticks (zero or two).
*
****************************************************************************
*
*/

static unsigned int get_joystick_num (void)
{
    return nr_joysticks;
}

/****i* januae-joystick/get_joystick_name **********************************
*
*   NAME
*       get_joystick_name() -- Format a joystick port name.
*
*   SYNOPSIS
*       static const char *get_joystick_name (unsigned int joy);
*
*   FUNCTION
*       Write the requested port index into the shared static name buffer.
*
*   INPUTS
*       joy - Joystick port index.
*
*   RESULT
*       Pointer to a shared buffer containing "Joy port N".
*
*   NOTES
*       Later calls overwrite the returned buffer.
*
****************************************************************************
*
*/

static const char *get_joystick_name (unsigned int joy)
{
    static char name[16];
    sprintf (name, "Joy port %d", joy);
    return name;
}

/****i* januae-joystick/get_joystick_widget_num ****************************
*
*   NAME
*       get_joystick_widget_num() -- Report joystick axes and buttons.
*
*   SYNOPSIS
*       static unsigned int get_joystick_widget_num (unsigned int joy);
*
*   FUNCTION
*       Count two axes and two buttons per joystick port.
*
*   INPUTS
*       joy - Joystick port index; ignored.
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

/****i* januae-joystick/get_joystick_widget_type ***************************
*
*   NAME
*       get_joystick_widget_type() -- Describe a joystick axis or button.
*
*   SYNOPSIS
*       static int get_joystick_widget_type (unsigned int joy, unsigned int
*       num, char *name, uae_u32 *code);
*
*   FUNCTION
*       Classify indexes zero and one as axes, two and three as buttons, and
*       optionally format a label.
*
*   INPUTS
*       joy - Port index; ignored. num - Widget index. name - Optional label
*       destination. code - Event-code destination; unused.
*
*   RESULT
*       IDEV_WIDGET_AXIS, IDEV_WIDGET_BUTTON or IDEV_WIDGET_NONE.
*
****************************************************************************
*
*/

static int get_joystick_widget_type (unsigned int joy, unsigned int num, char *name, uae_u32 *code)
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

/****i* januae-joystick/get_joystick_widget_first **************************
*
*   NAME
*       get_joystick_widget_first() -- Locate the first widget of a joystick
*       type.
*
*   SYNOPSIS
*       static int get_joystick_widget_first (unsigned int joy, int type);
*
*   FUNCTION
*       Return the first axis or button widget index for a requested class.
*
*   INPUTS
*       joy - Port index; ignored. type - Widget class.
*
*   RESULT
*       0 for IDEV_WIDGET_AXIS, 2 for IDEV_WIDGET_BUTTON, or -1 otherwise.
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

/*
 * Set default inputdevice config for Amiga joysticks
 */
/****i* januae-joystick/input_get_default_joystick *************************
*
*   NAME
*       input_get_default_joystick() -- Install default joystick mappings.
*
*   SYNOPSIS
*       void input_get_default_joystick (struct uae_input_device *uid);
*
*   FUNCTION
*       Map each reported port to the corresponding emulator horizontal and
*       vertical axes and button events; enable the first input device.
*
*   INPUTS
*       uid - Mutable input-device configuration array.
*
*   RESULT
*       No value is returned.
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
