 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Joystick emulation stubs
  *
  * Copyright 1997 Bernd Schmidt
  * Copyright 2003-2005 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "inputdevice.h"

/****i* januae-jd-none/init_joysticks **************************************
*
*   NAME
*       init_joysticks() -- Initialize the null joystick backend.
*
*   SYNOPSIS
*       static int init_joysticks (void);
*
*   FUNCTION
*       Report initialization success without discovering or opening any
*       device.
*
*   RESULT
*       Always 1.
*
****************************************************************************
*
*/
static int init_joysticks (void)
{
   return 1;
}

/****i* januae-jd-none/close_joysticks *************************************
*
*   NAME
*       close_joysticks() -- Close the null joystick backend.
*
*   SYNOPSIS
*       static void close_joysticks (void);
*
*   FUNCTION
*       Perform no operation; no joystick resources are acquired.
*
****************************************************************************
*
*/
static void close_joysticks (void)
{
}

/****i* januae-jd-none/acquire_joystick ************************************
*
*   NAME
*       acquire_joystick() -- Reject null-backend joystick acquisition.
*
*   SYNOPSIS
*       static int acquire_joystick (unsigned int num, int flags);
*
*   FUNCTION
*       Decline every requested joystick because no device exists.
*
*   INPUTS
*       num - Ignored.
*       flags - Ignored.
*
*   RESULT
*       Always 0.
*
****************************************************************************
*
*/
static int acquire_joystick (unsigned int num, int flags)
{
    return 0;
}

/****i* januae-jd-none/unacquire_joystick **********************************
*
*   NAME
*       unacquire_joystick() -- Release a null-backend joystick.
*
*   SYNOPSIS
*       static void unacquire_joystick (unsigned int num);
*
*   FUNCTION
*       Perform no operation.
*
*   INPUTS
*       num - Ignored.
*
****************************************************************************
*
*/
static void unacquire_joystick (unsigned int num)
{
}

/****i* januae-jd-none/read_joysticks **************************************
*
*   NAME
*       read_joysticks() -- Poll the null joystick backend.
*
*   SYNOPSIS
*       static void read_joysticks (void);
*
*   FUNCTION
*       Perform no input polling.
*
****************************************************************************
*
*/
static void read_joysticks (void)
{
}

/****i* januae-jd-none/get_joystick_num ************************************
*
*   NAME
*       get_joystick_num() -- Get the null-backend joystick count.
*
*   SYNOPSIS
*       static unsigned int get_joystick_num (void);
*
*   FUNCTION
*       Report that the backend supplies no joystick devices.
*
*   RESULT
*       Always 0.
*
****************************************************************************
*
*/
static unsigned int get_joystick_num (void)
{
    return 0;
}

/****i* januae-jd-none/get_joystick_name ***********************************
*
*   NAME
*       get_joystick_name() -- Get a null-backend joystick name.
*
*   SYNOPSIS
*       static const char *get_joystick_name (unsigned int joy);
*
*   FUNCTION
*       Report the absence of a joystick name.
*
*   INPUTS
*       joy - Ignored.
*
*   RESULT
*       NULL.
*
****************************************************************************
*
*/
static const char *get_joystick_name (unsigned int joy)
{
    return 0;
}

/****i* januae-jd-none/get_joystick_widget_num *****************************
*
*   NAME
*       get_joystick_widget_num() -- Get the null-backend widget count.
*
*   SYNOPSIS
*       static unsigned int get_joystick_widget_num (unsigned int joy);
*
*   FUNCTION
*       Report that no joystick widgets exist.
*
*   INPUTS
*       joy - Ignored.
*
*   RESULT
*       Always 0.
*
****************************************************************************
*
*/
static unsigned int get_joystick_widget_num (unsigned int joy)
{
    return 0;
}

/****i* januae-jd-none/get_joystick_widget_type ****************************
*
*   NAME
*       get_joystick_widget_type() -- Classify a null-backend widget.
*
*   SYNOPSIS
*       static int get_joystick_widget_type (unsigned int joy, unsigned int
*       num, char *name, uae_u32 *code);
*
*   FUNCTION
*       Report that no widget is available without modifying the output
*       parameters.
*
*   INPUTS
*       joy - Ignored.
*       num - Ignored.
*       name - Unused.
*       code - Unused.
*
*   RESULT
*       Always IDEV_WIDGET_NONE.
*
****************************************************************************
*
*/
static int get_joystick_widget_type (unsigned int joy, unsigned int num, char *name, uae_u32 *code)
{
    return IDEV_WIDGET_NONE;
}

/****i* januae-jd-none/get_joystick_widget_first ***************************
*
*   NAME
*       get_joystick_widget_first() -- Find a null-backend widget of a kind.
*
*   SYNOPSIS
*       static int get_joystick_widget_first (unsigned int joy, int type);
*
*   FUNCTION
*       Report that no widget index exists.
*
*   INPUTS
*       joy - Ignored.
*       type - Ignored.
*
*   RESULT
*       Always -1.
*
****************************************************************************
*
*/
static int get_joystick_widget_first (unsigned int joy, int type)
{
    return -1;
}

struct inputdevice_functions inputdevicefunc_joystick = {
    init_joysticks,
    close_joysticks,
    acquire_joystick,
    unacquire_joystick,
    read_joysticks,
    get_joystick_num,
    get_joystick_name,
    get_joystick_widget_num,
    get_joystick_widget_type,
    get_joystick_widget_first
};

/****i* januae-jd-none/input_get_default_joystick **************************
*
*   NAME
*       input_get_default_joystick() -- Disable default joystick input.
*
*   SYNOPSIS
*       void input_get_default_joystick (struct uae_input_device *uid);
*
*   FUNCTION
*       Disable the first configuration entry in the null backend.
*
*   INPUTS
*       uid - Writable input-device configuration array with at least one
*       entry.
*
****************************************************************************
*
*/
void input_get_default_joystick (struct uae_input_device *uid)
{
    uid[0].enabled = 0;
}
