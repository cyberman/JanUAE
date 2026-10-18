/************************************************************************ 
 *
 * Amiga mouse interface
 *
 * Copyright 1996,1997,1998 Samuel Devulder.
 * Copyright 2003-2007 Richard Drummond
 * Copyright 2009-2010 Oliver Brunner - aros<at>oliver-brunner.de
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

# ifdef __amigaos4__
#  define __USE_BASETYPE__
# endif

#include <intuition/pointerclass.h>
#include <proto/layers.h>

/****************************************************************************/

#include "sysconfig.h"
#include "sysdeps.h"

#include "td-amigaos/thread.h"

#if defined(CATWEASEL)
#include "catweasel.h"
#endif

//#define JW_ENTER_ENABLED  1
#define JWTRACING_ENABLED 1
#include "od-amiga/j.h"

#include "uae.h"
#include "ami.h"

#ifdef __amigaos4__ 
static int mouseGrabbed;
static int grabTicks;
#define GRAB_TIMEOUT 50
#endif

/****************************************************************************/

POINTER_STATE pointer_state;
int gMouseState=0;

static APTR blank_pointer;
static POINTER_STATE get_pointer_state (const struct Window *w, int mousex, int mousey);

/****************************************************************************/

/*
 * Initializes a pointer object containing a blank pointer image.
 * Used for hiding the mouse pointer
 */
/****i* januae-ami-mouse/init_pointer **************************************
*
*   NAME
*       init_pointer() -- Create the blank pointer image.
*
*   SYNOPSIS
*       void init_pointer (void);
*
*   FUNCTION
*       Construct a one-pixel blank bitmap and request a POINTERCLASS object
*       for hiding the host pointer.
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

void init_pointer (void) {

    static struct BitMap bitmap;
    static UWORD	 row[2] = {0, 0};

    InitBitMap (&bitmap, 2, 16, 1);
    bitmap.Planes[0] = (PLANEPTR) &row[0];
    bitmap.Planes[1] = (PLANEPTR) &row[1];

    blank_pointer = NewObject (NULL, POINTERCLASS,
			       POINTERA_BitMap,	(ULONG)&bitmap,
			       POINTERA_WordWidth,	1,
			       TAG_DONE);

    if (!blank_pointer)
	write_log ("Warning: Unable to allocate blank mouse pointer.\n");
}

/*
 * Free up blank pointer object
 */
/****i* januae-ami-mouse/free_pointer **************************************
*
*   NAME
*       free_pointer() -- Dispose of the blank pointer object.
*
*   SYNOPSIS
*       void free_pointer (void);
*
*   FUNCTION
*       Dispose of the cached POINTERCLASS object when allocated and clear
*       its pointer.
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

void free_pointer (void) {

    if (blank_pointer) {
	DisposeObject (blank_pointer);
	blank_pointer = NULL;
    }
}

/*
 * Hide mouse pointer for window
 */
/****i* januae-ami-mouse/hide_pointer **************************************
*
*   NAME
*       hide_pointer() -- Hide the pointer in a host window.
*
*   SYNOPSIS
*       void hide_pointer (struct Window *w);
*
*   FUNCTION
*       Create the blank pointer object if needed, set the pointer state to
*       inside and apply the blank object to the given window.
*
*   INPUTS
*       w - Window in which the pointer is hidden.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       If object creation fails, the window pointer is not modified.
*
****************************************************************************
*
*/

void hide_pointer (struct Window *w) {

  if(!blank_pointer) {
    init_pointer();
  }

  if(blank_pointer) {
    pointer_state=INSIDE_WINDOW;
    SetWindowPointer (w, WA_Pointer, (ULONG)blank_pointer, TAG_DONE);
  }
}

/*
 * Restore default mouse pointer for window
 */
/****i* januae-ami-mouse/show_pointer **************************************
*
*   NAME
*       show_pointer() -- Restore the default pointer in a host window.
*
*   SYNOPSIS
*       void show_pointer (struct Window *w);
*
*   FUNCTION
*       Set the pointer state to outside and restore the default pointer on
*       the supplied window.
*
*   INPUTS
*       w - Window whose pointer is restored.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void show_pointer (struct Window *w) {

  pointer_state=OUTSIDE_WINDOW;
  SetWindowPointer (w, WA_Pointer, 0, TAG_DONE);
}

/* 
 * show/hide pointer according to the current position
 *
 * x,y are relative to windows topleft edge
 */
/****i* januae-ami-mouse/update_pointer ************************************
*
*   NAME
*       update_pointer() -- Update pointer display for its current position.
*
*   SYNOPSIS
*       void update_pointer(struct Window *w, int x, int y);
*
*   FUNCTION
*       Compare the inferred pointer location with the stored pointer state,
*       then use the stored state to select a hide or show operation on the
*       global emulator window.
*
*   INPUTS
*       w - Supplied window, unused. x - Pointer X coordinate. y - Pointer Y
*       coordinate.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       This implementation acts on global W rather than the supplied
*       window.
*
****************************************************************************
*
*/

void update_pointer(struct Window *w, int x, int y) {

  POINTER_STATE new_state = get_pointer_state (W, x, y);

  if (new_state != pointer_state) {
  //  pointer_state = new_state;
    if (pointer_state == INSIDE_WINDOW)
      hide_pointer (W);
    else
      show_pointer (W);
    }
}


#ifdef __amigaos4__ 
/*
 * Grab mouse pointer under OS4.0. Needs to be called periodically
 * to maintain grabbed status.
 */
/****i* januae-ami-mouse/grab_pointer **************************************
*
*   NAME
*       grab_pointer() -- Refresh the AmigaOS 4 mouse-grab region.
*
*   SYNOPSIS
*       static void grab_pointer (struct Window *w);
*
*   FUNCTION
*       Set window mouse limits to the interior of the global host window
*       and update focus grabbing according to mouseGrabbed.
*
*   INPUTS
*       w - Supplied window, unused.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Compiled only for __amigaos4__.
*
****************************************************************************
*
*/

static void grab_pointer (struct Window *w) {

    struct IBox box = {
	W->BorderLeft,
	W->BorderTop,
	W->Width  - W->BorderLeft - W->BorderRight,
	W->Height - W->BorderTop  - W->BorderBottom
    };

    SetWindowAttrs (W, WA_MouseLimits, &box, sizeof box);
    SetWindowAttrs (W, WA_GrabFocus, mouseGrabbed ? GRAB_TIMEOUT : 0, sizeof (ULONG));
}
#endif

/****************************************************************************/

/****i* januae-ami-mouse/get_pointer_state *********************************
*
*   NAME
*       get_pointer_state() -- Determine whether the pointer occupies the
*       host window.
*
*   SYNOPSIS
*       static POINTER_STATE get_pointer_state (const struct Window *w, int
*       mousex, int mousey);
*
*   FUNCTION
*       Test the supplied coordinates against the client-area bounds and
*       check whether the top layer at the pointer position belongs to that
*       window.
*
*   INPUTS
*       w - Host window. mousex - Window-relative X coordinate. mousey -
*       Window-relative Y coordinate.
*
*   RESULT
*       INSIDE_WINDOW for the unobscured client area; otherwise
*       OUTSIDE_WINDOW.
*
****************************************************************************
*
*/

static POINTER_STATE get_pointer_state (const struct Window *w, int mousex, int mousey) {

    POINTER_STATE new_state = OUTSIDE_WINDOW;

    /*
     * Is pointer within the bounds of the inner window?
     */
    if ((mousex >= w->BorderLeft)
     && (mousey >= w->BorderTop)
     && (mousex < (w->Width - w->BorderRight))
     && (mousey < (w->Height - w->BorderBottom))) {
	/*
	 * Yes. Now check whetehr the window is obscured by
	 * another window at the pointer position
	 */
	struct Screen *scr = w->WScreen;
	struct Layer  *layer;

	/* Find which layer the pointer is in */
	LockLayerInfo (&scr->LayerInfo);
	layer = WhichLayer (&scr->LayerInfo, scr->MouseX, scr->MouseY);
	UnlockLayerInfo (&scr->LayerInfo);

	/* Is this layer our window's layer? */
	if (layer == w->WLayer) {
	    /*
	     * Yes. Therefore, pointer is inside the window.
	     */
	    new_state = INSIDE_WINDOW;
	}
    }
    return new_state;
}

/****i* januae-ami-mouse/toggle_mousegrab **********************************
*
*   NAME
*       toggle_mousegrab() -- Toggle host mouse capture.
*
*   SYNOPSIS
*       void toggle_mousegrab (void);
*
*   FUNCTION
*       Switch the AmigaOS 4 mouse-grab flag and refresh the global window
*       limits when a window exists.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Other builds log that mouse grabbing is unsupported.
*
****************************************************************************
*
*/

void toggle_mousegrab (void) {

#ifdef __amigaos4__ 
    mouseGrabbed = 1 - mouseGrabbed;
    grabTicks    = GRAB_TIMEOUT;
    if (W)
	grab_pointer (W);
#else
    write_log ("Mouse grab not supported\n");
#endif
}

/****************************************************************************
 *
 * Mouse inputdevice functions
 */

#define MAX_BUTTONS     3
#define MAX_AXES        3
#define FIRST_AXIS      0
#define FIRST_BUTTON    MAX_AXES

/****i* januae-ami-mouse/init_mouse ****************************************
*
*   NAME
*       init_mouse() -- Initialize host mouse input callbacks.
*
*   SYNOPSIS
*       static int init_mouse (void);
*
*   FUNCTION
*       Report successful initialization without allocating per-device
*       state.
*
*   INPUTS
*       None.
*
*   RESULT
*       1.
*
****************************************************************************
*
*/

static int init_mouse (void)
{
   return 1;
}

/****i* januae-ami-mouse/close_mouse ***************************************
*
*   NAME
*       close_mouse() -- Close host mouse input callbacks.
*
*   SYNOPSIS
*       static void close_mouse (void);
*
*   FUNCTION
*       Return without releasing per-device mouse resources.
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

static void close_mouse (void)
{
   return;
}

/****i* januae-ami-mouse/acquire_mouse *************************************
*
*   NAME
*       acquire_mouse() -- Acquire a host mouse device.
*
*   SYNOPSIS
*       static int acquire_mouse (unsigned int num, int flags);
*
*   FUNCTION
*       Report successful acquisition without changing input ownership.
*
*   INPUTS
*       num - Mouse index; ignored. flags - Acquisition flags; ignored.
*
*   RESULT
*       1.
*
****************************************************************************
*
*/

static int acquire_mouse (unsigned int num, int flags)
{
   return 1;
}

/****i* januae-ami-mouse/unacquire_mouse ***********************************
*
*   NAME
*       unacquire_mouse() -- Release a host mouse device.
*
*   SYNOPSIS
*       static void unacquire_mouse (unsigned int num);
*
*   FUNCTION
*       Return without changing input ownership.
*
*   INPUTS
*       num - Mouse index; ignored.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void unacquire_mouse (unsigned int num)
{
   return;
}

/****i* januae-ami-mouse/get_mouse_num *************************************
*
*   NAME
*       get_mouse_num() -- Report the host mouse count.
*
*   SYNOPSIS
*       static unsigned int get_mouse_num (void);
*
*   FUNCTION
*       Return one available mouse regardless of current window state.
*
*   INPUTS
*       None.
*
*   RESULT
*       1.
*
****************************************************************************
*
*/

static unsigned int get_mouse_num (void)
{
    return 1;
}

/****i* januae-ami-mouse/get_mouse_name ************************************
*
*   NAME
*       get_mouse_name() -- Return the host mouse name.
*
*   SYNOPSIS
*       static const char *get_mouse_name (unsigned int mouse);
*
*   FUNCTION
*       Return the fixed name for the host mouse device.
*
*   INPUTS
*       mouse - Mouse index; ignored.
*
*   RESULT
*       Pointer to the static string "Default mouse".
*
****************************************************************************
*
*/

static const char *get_mouse_name (unsigned int mouse)
{
    return "Default mouse";
}

/****i* januae-ami-mouse/get_mouse_widget_num ******************************
*
*   NAME
*       get_mouse_widget_num() -- Report mouse axis and button widgets.
*
*   SYNOPSIS
*       static unsigned int get_mouse_widget_num (unsigned int mouse);
*
*   FUNCTION
*       Count the configured three axes and three buttons.
*
*   INPUTS
*       mouse - Mouse index; ignored.
*
*   RESULT
*       6.
*
****************************************************************************
*
*/

static unsigned int get_mouse_widget_num (unsigned int mouse)
{
    return MAX_AXES + MAX_BUTTONS;
}

/****i* januae-ami-mouse/get_mouse_widget_first ****************************
*
*   NAME
*       get_mouse_widget_first() -- Locate the first widget of a mouse type.
*
*   SYNOPSIS
*       static int get_mouse_widget_first (unsigned int mouse, int type);
*
*   FUNCTION
*       Return the axis or button index where that class begins.
*
*   INPUTS
*       mouse - Mouse index; ignored. type - Widget class.
*
*   RESULT
*       0 for IDEV_WIDGET_AXIS, 3 for IDEV_WIDGET_BUTTON, or -1 otherwise.
*
****************************************************************************
*
*/

static int get_mouse_widget_first (unsigned int mouse, int type)
{
    switch (type) {
        case IDEV_WIDGET_BUTTON:
            return FIRST_BUTTON;
        case IDEV_WIDGET_AXIS:
            return FIRST_AXIS;
    }
    return -1;
}

/****i* januae-ami-mouse/get_mouse_widget_type *****************************
*
*   NAME
*       get_mouse_widget_type() -- Describe a mouse input widget.
*
*   SYNOPSIS
*       static int get_mouse_widget_type (unsigned int mouse, unsigned int
*       num, char *name, uae_u32 *code);
*
*   FUNCTION
*       Classify indexes zero through two as axes and three through five as
*       buttons; optionally write a text label.
*
*   INPUTS
*       mouse - Mouse index; ignored. num - Widget index. name - Optional
*       output for a label. code - Event-code destination; unused.
*
*   RESULT
*       IDEV_WIDGET_AXIS, IDEV_WIDGET_BUTTON or IDEV_WIDGET_NONE.
*
****************************************************************************
*
*/

static int get_mouse_widget_type (unsigned int mouse, unsigned int num, char *name, uae_u32 *code)
{
    if (num >= MAX_AXES && num < MAX_AXES + MAX_BUTTONS) {
        if (name)
            sprintf (name, "Button %d", num + 1 + MAX_AXES);
        return IDEV_WIDGET_BUTTON;
    } else if (num < MAX_AXES) {
        if (name)
            sprintf (name, "Axis %d", num + 1);
        return IDEV_WIDGET_AXIS;
    }
    return IDEV_WIDGET_NONE;
}

/****i* januae-ami-mouse/read_mouse ****************************************
*
*   NAME
*       read_mouse() -- Poll optional Catweasel mouse input.
*
*   SYNOPSIS
*       static void read_mouse (void);
*
*   FUNCTION
*       With CATWEASEL support enabled, forward detected relative axes and
*       button transitions to the emulator input device.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Regular host mouse events are processed by handle_events_W().
*       Without CATWEASEL this callback performs no operation.
*
****************************************************************************
*
*/

static void read_mouse (void)
{
    /* We handle mouse input in handle_events() */

#if defined(CATWEASEL)
	/* ... unless we're using a Catweasel of course. */
	int cx, cy, cbuttons;
	static int sLastButtons = 0;

	if (catweasel_read_mouse (1, &cx, &cy, &cbuttons)) 
	{
		if (cx)
			setmousestate (0, 0, cx, 0);
		if (cy)
			setmousestate (0, 1, cy, 0);

		if ( (gMouseState & 0x0E) != (cbuttons & 0x0E) )
		{
			if ( (sLastButtons & 8) != (cbuttons & 8) )
			{
				gMouseState = (cbuttons & 8);
				setmousebuttonstate (0, 0, gMouseState & 8);
			}
			if ( (sLastButtons & 4) != (cbuttons & 4) )
			{
				gMouseState = (cbuttons & 4);
				setmousebuttonstate (0, 1, gMouseState & 4);
			}
			if ( (sLastButtons & 2) != (cbuttons & 2) )
			{
				gMouseState = (cbuttons & 2);
				setmousebuttonstate (0, 2, gMouseState & 2);
			}
		}
//write_log( "mouse state cx: %x cy: %x buttons: %x\n ", cx, cy, cbuttons);
		sLastButtons = cbuttons;
	}	

#endif
}

struct inputdevice_functions inputdevicefunc_mouse = {
    init_mouse,
    close_mouse,
    acquire_mouse,
    unacquire_mouse,
    read_mouse,
    get_mouse_num,
    get_mouse_name,
    get_mouse_widget_num,
    get_mouse_widget_type,
    get_mouse_widget_first
};

/*
 * Default inputdevice config for mouse
 */
/****i* januae-ami-mouse/input_get_default_mouse ***************************
*
*   NAME
*       input_get_default_mouse() -- Install default mouse event mappings.
*
*   SYNOPSIS
*       void input_get_default_mouse (struct uae_input_device *uid);
*
*   FUNCTION
*       Map three mouse axes and three button events to the first emulator
*       mouse and enable that device.
*
*   INPUTS
*       uid - Mutable emulator input-device configuration array.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void input_get_default_mouse (struct uae_input_device *uid)
{
    /* Supports only one mouse for now */
    uid[0].eventid[ID_AXIS_OFFSET + 0][0]   = INPUTEVENT_MOUSE1_HORIZ;
    uid[0].eventid[ID_AXIS_OFFSET + 1][0]   = INPUTEVENT_MOUSE1_VERT;
    uid[0].eventid[ID_AXIS_OFFSET + 2][0]   = INPUTEVENT_MOUSE1_WHEEL;
    uid[0].eventid[ID_BUTTON_OFFSET + 0][0] = INPUTEVENT_JOY1_FIRE_BUTTON;
    uid[0].eventid[ID_BUTTON_OFFSET + 1][0] = INPUTEVENT_JOY1_2ND_BUTTON;
    uid[0].eventid[ID_BUTTON_OFFSET + 2][0] = INPUTEVENT_JOY1_3RD_BUTTON;
    uid[0].enabled = 1;
}

/****i* januae-ami-mouse/mousehack_allowed *********************************
*
*   NAME
*       mousehack_allowed() -- Report availability of mouse-hack mode.
*
*   SYNOPSIS
*       int mousehack_allowed (void);
*
*   FUNCTION
*       Return the fixed disabled indication.
*
*   INPUTS
*       None.
*
*   RESULT
*       0.
*
****************************************************************************
*
*/

int mousehack_allowed (void)
{
    return 0;
}

