 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Support for Linux/USS sound
  *
  * Copyright 1997 Bernd Schmidt
  * Copyright 2003-2007 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "gensound.h"
#include "sounddep/sound.h"

#include <sys/ioctl.h>

#ifdef HAVE_SYS_SOUNDCARD_H
#include <sys/soundcard.h>
#elif defined HAVE_MACHINE_SOUNDCARD_H
#include <machine/soundcard.h>
#else
#error "Something went wrong during configuration."
#endif

int sound_fd;
static int have_sound = 0;
static unsigned long formats;

uae_u16 sndbuffer[44100];
uae_u16 *sndbufpt;
int sndbufsize;

/****i* januae-sd-uss/exact_log2 *******************************************
*
*   NAME
*       exact_log2()
*
*   SYNOPSIS
*       static int exact_log2 (int v);
*
*   FUNCTION
*       Compute the position of the highest set bit by repeatedly shifting
*       the argument right.
*
*   INPUTS
*       v - Input integer.
*
*   RESULT
*       The number of right shifts before the value reaches zero.
*
*   NOTES
*       For positive inputs this is floor(log2(v)); the function does not
*       validate its input.
*
****************************************************************************
*
*/
static int exact_log2 (int v)
{
    int l = 0;
    while ((v >>= 1) != 0)
	l++;
    return l;
}

/****i* januae-sd-uss/close_sound ******************************************
*
*   NAME
*       close_sound()
*
*   SYNOPSIS
*       void close_sound (void);
*
*   FUNCTION
*       Close the OSS device descriptor if this backend is marked active.
*
*   NOTES
*       The have_sound flag is not cleared and the descriptor is not reset.
*
****************************************************************************
*
*/
void close_sound (void)
{
    if (have_sound)
	close (sound_fd);
}

/****i* januae-sd-uss/setup_sound ******************************************
*
*   NAME
*       setup_sound()
*
*   SYNOPSIS
*       int setup_sound (void);
*
*   FUNCTION
*       Probe /dev/dsp and its supported PCM formats to report OSS sound
*       availability.
*
*   RESULT
*       1 when a format query succeeds, or when opening reports EBUSY;
*       otherwise 0.
*
*   NOTES
*       An EBUSY result is deliberately treated as availability. A
*       successful probe closes its descriptor.
*
****************************************************************************
*
*/
int setup_sound (void)
{
    sound_fd = open ("/dev/dsp", O_WRONLY);

    sound_available = 0;

    if (sound_fd < 0) {
	perror ("Can't open /dev/dsp");
	if (errno == EBUSY) {
	    /* We can hope, can't we ;) */
	    sound_available = 1;
	    return 1;
	}
	return 0;
    }

    if (ioctl (sound_fd, SNDCTL_DSP_GETFMTS, &formats) == -1) {
	perror ("ioctl failed - can't use sound");
	close (sound_fd);
	return 0;
    }

    sound_available = 1;
    close (sound_fd);
    return 1;
}

/****i* januae-sd-uss/init_sound *******************************************
*
*   NAME
*       init_sound()
*
*   SYNOPSIS
*       int init_sound (void);
*
*   FUNCTION
*       Open /dev/dsp, configure PCM width, stereo, rate and fragment size,
*       then select the shared sample handler.
*
*   RESULT
*       1 on successful initialization; otherwise 0.
*
*   NOTES
*       Accepts rates within ten percent of the requested rate. Several
*       ioctl results are unchecked, and some failure paths return without
*       closing the open descriptor.
*
****************************************************************************
*
*/
int init_sound (void)
{
    int tmp;
    int rate;
    int dspbits;
    int bufsize;

    sound_fd = open ("/dev/dsp", O_WRONLY);
    have_sound = !(sound_fd < 0);
    if (! have_sound) {
	perror ("Can't open /dev/dsp");
	if (errno != EBUSY)
	    sound_available = 0;
	return 0;
    }
    if (ioctl (sound_fd, SNDCTL_DSP_GETFMTS, &formats) == -1) {
	perror ("ioctl failed - can't use sound");
	close (sound_fd);
	have_sound = 0;
	return 0;
    }

    dspbits = currprefs.sound_bits;
    ioctl (sound_fd, SNDCTL_DSP_SAMPLESIZE, &dspbits);
    ioctl (sound_fd, SOUND_PCM_READ_BITS, &dspbits);
    if (dspbits != currprefs.sound_bits) {
	fprintf (stderr, "Can't use sound with %d bits\n", currprefs.sound_bits);
	return 0;
    }

    tmp = currprefs.sound_stereo;
    ioctl (sound_fd, SNDCTL_DSP_STEREO, &tmp);

    rate = currprefs.sound_freq;
    ioctl (sound_fd, SNDCTL_DSP_SPEED, &rate);
    ioctl (sound_fd, SOUND_PCM_READ_RATE, &rate);
    /* Some soundcards have a bit of tolerance here. */
    if (rate < currprefs.sound_freq * 90 / 100 || rate > currprefs.sound_freq * 110 / 100) {
	fprintf (stderr, "Can't use sound with desired frequency %d\n", currprefs.sound_freq);
	return 0;
    }

    bufsize = rate * currprefs.sound_latency * (dspbits / 8) * (currprefs.sound_stereo ? 2 : 1) / 1000;

    tmp = 0x00040000 + exact_log2 (bufsize);
    ioctl (sound_fd, SNDCTL_DSP_SETFRAGMENT, &tmp);
    ioctl (sound_fd, SNDCTL_DSP_GETBLKSIZE, &sndbufsize);

    obtainedfreq = currprefs.sound_freq;

    if (dspbits == 16) {
	/* Will this break horribly on bigendian machines? Possible... Not any more - Rich */
	if (!(formats & AFMT_S16_NE))
	    return 0;
	init_sound_table16 ();
	sample_handler = currprefs.sound_stereo ? sample16s_handler : sample16_handler;
    } else {
	if (!(formats & AFMT_U8))
	    return 0;
	init_sound_table8 ();
	sample_handler = currprefs.sound_stereo ? sample8s_handler : sample8_handler;
    }
    sound_available = 1;
    printf ("Sound driver found and configured for %d bits at %d Hz, buffer is %d bytes (%d ms).\n",
	    dspbits, rate, sndbufsize, sndbufsize * 1000 / (rate * dspbits / 8 * (currprefs.sound_stereo ? 2 : 1)));
    sndbufpt = sndbuffer;

    return 1;
}

/****i* januae-sd-uss/reset_sound ******************************************
*
*   NAME
*       reset_sound()
*
*   SYNOPSIS
*       void reset_sound (void);
*
*   FUNCTION
*       Perform no OSS audio reset.
*
****************************************************************************
*
*/
void reset_sound (void)
{
}

/****i* januae-sd-uss/pause_sound ******************************************
*
*   NAME
*       pause_sound()
*
*   SYNOPSIS
*       void pause_sound (void);
*
*   FUNCTION
*       Perform no OSS pause operation.
*
****************************************************************************
*
*/
void pause_sound (void)
{
}

/****i* januae-sd-uss/resume_sound *****************************************
*
*   NAME
*       resume_sound()
*
*   SYNOPSIS
*       void resume_sound (void);
*
*   FUNCTION
*       Perform no OSS resume operation.
*
****************************************************************************
*
*/
void resume_sound (void)
{
}

/****i* januae-sd-uss/sound_volume *****************************************
*
*   NAME
*       sound_volume()
*
*   SYNOPSIS
*       void sound_volume (int dir);
*
*   FUNCTION
*       Perform no OSS volume adjustment.
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

/****i* januae-sd-uss/audio_default_options ********************************
*
*   NAME
*       audio_default_options()
*
*   SYNOPSIS
*       void audio_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Perform no OSS-specific default-option changes.
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

/****i* januae-sd-uss/audio_save_options ***********************************
*
*   NAME
*       audio_save_options()
*
*   SYNOPSIS
*       void audio_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Write no OSS-specific configuration options.
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

/****i* januae-sd-uss/audio_parse_option ***********************************
*
*   NAME
*       audio_parse_option()
*
*   SYNOPSIS
*       int audio_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Decline OSS-specific configuration options.
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
