 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Support for the Mute sound system
  *
  * Copyright 1997 Bernd Schmidt
  * Copyright 2003 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "gensound.h"
#include "sounddep/sound.h"

/****i* januae-sd-none/init_sound ******************************************
*
*   NAME
*       init_sound()
*
*   SYNOPSIS
*       int init_sound (void);
*
*   FUNCTION
*       Disable audio generation in the current preferences without opening
*       a device.
*
*   RESULT
*       Always 1.
*
****************************************************************************
*
*/
int init_sound (void)
{
    currprefs.produce_sound = 0;
    return 1;
}

/****i* januae-sd-none/setup_sound *****************************************
*
*   NAME
*       setup_sound()
*
*   SYNOPSIS
*       int setup_sound (void);
*
*   FUNCTION
*       Disable audio generation in the current preferences without probing
*       a device.
*
*   RESULT
*       Always 1.
*
****************************************************************************
*
*/
int setup_sound (void)
{
    currprefs.produce_sound = 0;
    return 1;
}

/****i* januae-sd-none/close_sound *****************************************
*
*   NAME
*       close_sound()
*
*   SYNOPSIS
*       void close_sound (void);
*
*   FUNCTION
*       Perform no sound-device cleanup.
*
****************************************************************************
*
*/
void close_sound (void)
{
}

/****i* januae-sd-none/update_sound ****************************************
*
*   NAME
*       update_sound()
*
*   SYNOPSIS
*       void update_sound (int freq);
*
*   FUNCTION
*       Perform no sample-rate update.
*
*   INPUTS
*       freq - Unused.
*
****************************************************************************
*
*/
void update_sound (int freq)
{
}

/****i* januae-sd-none/reset_sound *****************************************
*
*   NAME
*       reset_sound()
*
*   SYNOPSIS
*       void reset_sound (void);
*
*   FUNCTION
*       Perform no sound reset.
*
****************************************************************************
*
*/
void reset_sound (void)
{
}

/****i* januae-sd-none/pause_sound *****************************************
*
*   NAME
*       pause_sound()
*
*   SYNOPSIS
*       void pause_sound (void);
*
*   FUNCTION
*       Perform no sound pause.
*
****************************************************************************
*
*/
void pause_sound (void)
{
}

/****i* januae-sd-none/resume_sound ****************************************
*
*   NAME
*       resume_sound()
*
*   SYNOPSIS
*       void resume_sound (void);
*
*   FUNCTION
*       Perform no sound resume.
*
****************************************************************************
*
*/
void resume_sound (void)
{
}

/****i* januae-sd-none/sound_volume ****************************************
*
*   NAME
*       sound_volume()
*
*   SYNOPSIS
*       void sound_volume (int dir);
*
*   FUNCTION
*       Perform no volume adjustment.
*
*   INPUTS
*       dir - Unused.
*
****************************************************************************
*
*/
void sound_volume (int dir)
{
}

/****i* januae-sd-none/audio_default_options *******************************
*
*   NAME
*       audio_default_options()
*
*   SYNOPSIS
*       void audio_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Perform no default-option changes.
*
*   INPUTS
*       p - Unused.
*
****************************************************************************
*
*/
void audio_default_options (struct uae_prefs *p)
{
}

/****i* januae-sd-none/audio_save_options **********************************
*
*   NAME
*       audio_save_options()
*
*   SYNOPSIS
*       void audio_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Write no backend configuration options.
*
*   INPUTS
*       f - Unused.
*       p - Unused.
*
****************************************************************************
*
*/
void audio_save_options (FILE *f, const struct uae_prefs *p)
{
}

/****i* januae-sd-none/audio_parse_option **********************************
*
*   NAME
*       audio_parse_option()
*
*   SYNOPSIS
*       int audio_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Decline backend configuration options.
*
*   INPUTS
*       p - Unused.
*       option - Unused.
*       value - Unused.
*
*   RESULT
*       Always 0.
*
****************************************************************************
*
*/
int audio_parse_option (struct uae_prefs *p, const char *option, const char *value)
{
    return 0;
}
