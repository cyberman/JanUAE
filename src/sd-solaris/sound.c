 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Support for Solaris sound
  *
  * Copyright 1996, 1997 Manfred Thole
  * Copyright 2007       Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "events.h"
#include "custom.h"
#include "audio.h"
#include "gensound.h"
#include "sounddep/sound.h"

#if !defined HAVE_SYS_AUDIOIO_H

/* SunOS 4.1.x */
#include <sys/ioctl.h>
#include <sun/audioio.h>
#ifndef AUDIO_ENCODING_LINEAR
#define AUDIO_ENCODING_LINEAR (3)
#endif

#else

/* SunOS 5.x/NetBSD */
#include <sys/audioio.h>
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#endif

int sndbufsize;
int sound_fd;
static int have_sound;
uae_u16 sndbuffer[44100];
uae_u16 *sndbufpt;

/****i* januae-sd-solaris/clearbuffer **************************************
*
*   NAME
*       clearbuffer()
*
*   SYNOPSIS
*       static void clearbuffer (void);
*
*   FUNCTION
*       Zero the full shared Solaris audio buffer.
*
****************************************************************************
*
*/
static void clearbuffer (void)
{
    memset (sndbuffer, 0 , sizeof (sndbuffer));
}

/****i* januae-sd-solaris/close_sound **************************************
*
*   NAME
*       close_sound()
*
*   SYNOPSIS
*       void close_sound(void);
*
*   FUNCTION
*       Close the /dev/audio descriptor when this backend is marked active.
*
*   NOTES
*       The have_sound flag and descriptor are not reset.
*
****************************************************************************
*
*/
void close_sound(void)
{
    if (have_sound)
	close (sound_fd);
}

/****i* januae-sd-solaris/setup_sound **************************************
*
*   NAME
*       setup_sound()
*
*   SYNOPSIS
*       int setup_sound (void);
*
*   FUNCTION
*       Open /dev/audio for output and record whether the descriptor was
*       acquired.
*
*   RESULT
*       1 if opening succeeds; otherwise 0.
*
*   NOTES
*       A successful probe keeps /dev/audio open for subsequent
*       init_sound(). It does not close the descriptor.
*
****************************************************************************
*
*/
int setup_sound (void)
{
    sound_fd = open ("/dev/audio", O_WRONLY);
    have_sound = !(sound_fd < 0);
    if (!have_sound)
      return 0;

    sound_available = 1;
    return 1;
}

/****i* januae-sd-solaris/init_sound ***************************************
*
*   NAME
*       init_sound()
*
*   SYNOPSIS
*       int init_sound (void);
*
*   FUNCTION
*       Configure the previously opened audio device for mono playback,
*       choose mu-law or linear sample output and initialize the shared
*       sound buffer.
*
*   RESULT
*       1 after a successful AUDIO_SETINFO ioctl; otherwise 0.
*
*   NOTES
*       The device is always configured for one channel, although buffer
*       sizing consults the stereo preference. This function assumes
*       setup_sound() opened the device.
*
****************************************************************************
*
*/
int init_sound (void)
{
    int rate, dspbits;

    struct audio_info sfd_info;

    rate = currprefs.sound_freq;
    dspbits = currprefs.sound_bits;
    AUDIO_INITINFO(&sfd_info);
    sfd_info.play.sample_rate = rate;
    sfd_info.play.channels = 1;
    sfd_info.play.precision = dspbits;
    sfd_info.play.encoding = (dspbits == 8 ) ? AUDIO_ENCODING_ULAW : AUDIO_ENCODING_LINEAR;
    if (ioctl (sound_fd, AUDIO_SETINFO, &sfd_info)) {
	write_log ("Can't use sample rate %d with %d bits, %s.\n", rate, dspbits, (dspbits == 8) ? "ulaw" : "linear");
	return 0;
    }

    init_sound_table16 ();

    if (dspbits == 8)
	sample_handler = sample_ulaw_handler;
    else
	sample_handler = sample16_handler;

    sndbufpt = sndbuffer;
    obtainedfreq = rate;

    sound_available = 1;
    sndbufsize = rate * currprefs.sound_latency * (dspbits / 8) * (currprefs.sound_stereo ? 2 : 1) / 1000;
    sndbufsize = (sndbufsize + 1) & ~1;

    write_log ("Sound driver found and configured for %d bits, %s at %d Hz, buffer is %d bytes.\n",
	       dspbits, (dspbits ==8) ? "ulaw" : "linear", rate, sndbufsize);
    return 1;
}

/****i* januae-sd-solaris/reset_sound **************************************
*
*   NAME
*       reset_sound()
*
*   SYNOPSIS
*       void reset_sound (void);
*
*   FUNCTION
*       Zero the full shared Solaris sound buffer.
*
****************************************************************************
*
*/
void reset_sound (void)
{
   clearbuffer ();
}

/****i* januae-sd-solaris/pause_sound **************************************
*
*   NAME
*       pause_sound()
*
*   SYNOPSIS
*       void pause_sound (void);
*
*   FUNCTION
*       Zero the shared sound buffer without pausing the audio device.
*
****************************************************************************
*
*/
void pause_sound (void)
{
   clearbuffer ();
}

/****i* januae-sd-solaris/resume_sound *************************************
*
*   NAME
*       resume_sound()
*
*   SYNOPSIS
*       void resume_sound (void);
*
*   FUNCTION
*       Perform no Solaris audio resume operation.
*
****************************************************************************
*
*/
void resume_sound (void)
{
}

/****i* januae-sd-solaris/sound_volume *************************************
*
*   NAME
*       sound_volume()
*
*   SYNOPSIS
*       void sound_volume (int dir);
*
*   FUNCTION
*       Perform no Solaris volume adjustment.
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

/****i* januae-sd-solaris/audio_default_options ****************************
*
*   NAME
*       audio_default_options()
*
*   SYNOPSIS
*       void audio_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Perform no Solaris-specific default-option changes.
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

/****i* januae-sd-solaris/audio_save_options *******************************
*
*   NAME
*       audio_save_options()
*
*   SYNOPSIS
*       void audio_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Write no Solaris-specific configuration options.
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

/****i* januae-sd-solaris/audio_parse_option *******************************
*
*   NAME
*       audio_parse_option()
*
*   SYNOPSIS
*       int audio_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Decline Solaris-specific configuration options.
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
