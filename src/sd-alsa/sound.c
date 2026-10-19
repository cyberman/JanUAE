 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Support for Linux/ALSA sound
  *
  * Copyright 1997 Bernd Schmidt
  * Copyright 2004 Heikki Orsila
  * Copyright 2006-2007 Richard Drummond
  *
  * BUGS: certainly
  * TODO:
  * - if setup_sound() fails, there may still be hope to get the
  *   sound device, but we totally give up.. see sd-uss.
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "gensound.h"
#include "sounddep/sound.h"

#include <alsa/asoundlib.h>

char alsa_device[256];
int  alsa_verbose;

static int have_sound = 0;

uae_u16 sndbuffer[44100];
uae_u16 *sndbufpt;
int sndbufsize;

snd_pcm_t *alsa_playback_handle = 0;
int bytes_per_frame;


/****i* januae-sd-alsa/close_sound *****************************************
*
*   NAME
*       close_sound()
*
*   SYNOPSIS
*       void close_sound (void);
*
*   FUNCTION
*       Close the current ALSA playback handle and clear it if one exists.
*
****************************************************************************
*
*/
void close_sound (void)
{
  if (alsa_playback_handle) {
    snd_pcm_close (alsa_playback_handle);
    alsa_playback_handle = 0;
  }
}

/****i* januae-sd-alsa/open_sound ******************************************
*
*   NAME
*       open_sound()
*
*   SYNOPSIS
*       static int open_sound(void);
*
*   FUNCTION
*       Open the configured ALSA device for blocking playback and retain its
*       handle.
*
*   RESULT
*       The result of snd_pcm_open(), zero on success or a negative ALSA
*       error.
*
*   NOTES
*       The handle is owned by this backend until close_sound().
*
****************************************************************************
*
*/
static int open_sound(void)
{
  return snd_pcm_open (&alsa_playback_handle, alsa_device, SND_PCM_STREAM_PLAYBACK, 0);
}

/****i* januae-sd-alsa/setup_sound *****************************************
*
*   NAME
*       setup_sound()
*
*   SYNOPSIS
*       int setup_sound (void);
*
*   FUNCTION
*       Probe the configured ALSA playback device by opening and immediately
*       closing it; set sound_available on success.
*
*   RESULT
*       1 if opening succeeded; otherwise 0.
*
*   NOTES
*       An unavailable or busy device is reported as unavailable; this does
*       not reserve the device.
*
****************************************************************************
*
*/
int setup_sound (void)
{
  int err;
  sound_available = 0;
  if ((err = open_sound()) < 0) {
    /* TODO: if the pcm was busy, we should the same as sd-uss does.
       tell the caller that sound is available. in any other
       condition we should just return 0. */
    write_log ("Cannot open audio device: %s.\n", snd_strerror (err));
    return 0;
  }
  snd_pcm_close (alsa_playback_handle);
  alsa_playback_handle = 0;
  sound_available = 1;
  return 1;
}

/****i* januae-sd-alsa/set_hw_params ***************************************
*
*   NAME
*       set_hw_params()
*
*   SYNOPSIS
*       static int set_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t
*       *hw_params, unsigned int *rate, unsigned int channels,
*       snd_pcm_format_t format, unsigned int *buffer_time,
*       snd_pcm_uframes_t *buffer_frames, snd_pcm_uframes_t *period_frames);
*
*   FUNCTION
*       Request interleaved PCM access, sample format, channel count, a
*       nearby sample rate, buffer duration and approximately two periods,
*       then apply the hardware parameters.
*
*   INPUTS
*       pcm - Open ALSA playback handle.
*       hw_params - Writable ALSA hardware parameter structure.
*       rate - Requested rate, updated to the selected rate.
*       channels - Requested channel count.
*       format - Requested PCM sample format.
*       buffer_time - Requested buffer duration, updated to a nearby value.
*       buffer_frames - Receives the selected hardware buffer frame count.
*       period_frames - Receives the period frame count.
*
*   RESULT
*       Negative ALSA error on a checked setup failure; -EINVAL for a
*       one-period selection; otherwise 0.
*
*   NOTES
*       The final snd_pcm_hw_params() return value is not checked before
*       returning 0.
*
****************************************************************************
*
*/
static int set_hw_params(snd_pcm_t *pcm,
			 snd_pcm_hw_params_t *hw_params,
			 unsigned int *rate,
			 unsigned int channels,
			 snd_pcm_format_t format,
			 unsigned int *buffer_time,
			 snd_pcm_uframes_t *buffer_frames,
			 snd_pcm_uframes_t *period_frames)
{
    int err;
    unsigned int periods = 2;

    err = snd_pcm_hw_params_any (pcm, hw_params);
    if (err < 0)
	return err;
    err = snd_pcm_hw_params_set_access (pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0)
	return err;
    err = snd_pcm_hw_params_set_format (pcm, hw_params, format);
    if (err < 0)
	return err;
    err = snd_pcm_hw_params_set_channels (pcm, hw_params, channels);
    if (err < 0)
	return err;
    err = snd_pcm_hw_params_set_rate_near (pcm, hw_params, rate, 0);
    if (err < 0)
	return err;
    err = snd_pcm_hw_params_set_buffer_time_near (pcm, hw_params, buffer_time, NULL);
    if (err < 0)
	return err;
    snd_pcm_hw_params_get_buffer_size (hw_params, buffer_frames);
    err = snd_pcm_hw_params_set_periods_near (pcm, hw_params, &periods, NULL);
    if (err < 0)
       return err;
    if (periods == 1)
	return -EINVAL;
    err = snd_pcm_hw_params(pcm, hw_params);

    snd_pcm_hw_params_get_period_size (hw_params, period_frames, NULL);
    return 0;
}

/****i* januae-sd-alsa/set_sw_params ***************************************
*
*   NAME
*       set_sw_params()
*
*   SYNOPSIS
*       static int set_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t
*       *sw_params, snd_pcm_uframes_t buffer_frames, snd_pcm_uframes_t
*       period_frames);
*
*   FUNCTION
*       Configure ALSA playback start, availability and stop thresholds and
*       transfer alignment for the given frame counts.
*
*   INPUTS
*       pcm - Open ALSA playback handle.
*       sw_params - Writable software parameter structure.
*       buffer_frames - Configured buffer length in frames.
*       period_frames - Configured period length in frames.
*
*   RESULT
*       0 on success or a negative ALSA error.
*
*   NOTES
*       A zero period frame count is not guarded before division.
*
****************************************************************************
*
*/
static int set_sw_params(snd_pcm_t *pcm,
			 snd_pcm_sw_params_t *sw_params,
			 snd_pcm_uframes_t buffer_frames,
			 snd_pcm_uframes_t period_frames)
{
    int err;

    err = snd_pcm_sw_params_current (pcm, sw_params);
    if (err < 0)
	return err;
    err = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, (buffer_frames / period_frames) * period_frames);
    if (err < 0)
	return err;
    err = snd_pcm_sw_params_set_avail_min(pcm, sw_params, period_frames);
    if (err < 0)
	return err;
    err = snd_pcm_sw_params_set_stop_threshold(pcm, sw_params, buffer_frames);
    if (err < 0)
	return err;
    err = snd_pcm_sw_params_set_xfer_align(pcm, sw_params, 1);
    if (err < 0)
	return err;
    err = snd_pcm_sw_params(pcm, sw_params);
    if (err < 0)
	return err;
    return 0;
}

/****i* januae-sd-alsa/init_sound ******************************************
*
*   NAME
*       init_sound()
*
*   SYNOPSIS
*       int init_sound (void);
*
*   FUNCTION
*       Open and configure the ALSA PCM stream from current sound
*       preferences, select the sample handler and prepare the shared output
*       buffer.
*
*   RESULT
*       1 if playback initialization succeeds; otherwise 0.
*
*   NOTES
*       Supports signed 8-bit and 16-bit PCM, mono or stereo. Hardware may
*       choose a nearby rate, but obtainedfreq is assigned the requested
*       frequency. Some ALSA result codes are not checked.
*
****************************************************************************
*
*/
int init_sound (void)
{
    unsigned int rate;
    snd_pcm_format_t format;
    unsigned int channels;
    unsigned int dspbits;

    snd_pcm_hw_params_t *hw_params = 0;
    snd_pcm_sw_params_t *sw_params = 0;
    snd_pcm_uframes_t    buffer_frames;
    snd_pcm_uframes_t    period_frames;
    unsigned int         buffer_time;

    snd_output_t *alsa_out;

    int err;

    snd_output_stdio_attach (&alsa_out, stderr, 0);

    dspbits  = currprefs.sound_bits;
    channels = currprefs.sound_stereo ? 2 : 1;
    rate     = currprefs.sound_freq;

    have_sound = 0;
    alsa_playback_handle = 0;
    if ((err = open_sound()) < 0) {
	write_log ("Cannot open audio device: %s\n", snd_strerror (err));
	goto nosound;
    }

    buffer_time = currprefs.sound_latency * 1000;
    if (buffer_time < 1000 || buffer_time > 500000)
	buffer_time = 100000;

    if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) {
	write_log ("Cannot allocate hardware parameter structure: %s.\n", snd_strerror (err));
	goto nosound;
    }
    if ((err = snd_pcm_sw_params_malloc (&sw_params)) < 0) {
	write_log ("Cannot allocate software parameter structure: %s.\n", snd_strerror (err));
	goto nosound;
    }

    switch (dspbits) {
	case 8:
	    format = SND_PCM_FORMAT_S8;
	    break;
	case 16:
	    format = SND_PCM_FORMAT_S16;
	    break;
	default:
	    write_log ("%d-bit samples not supported by UAE.\n", dspbits);
	    goto nosound;
    }

    bytes_per_frame = dspbits / 8 * channels;

    if ((err = set_hw_params (alsa_playback_handle, hw_params, &rate, channels, format, &buffer_time, &buffer_frames, &period_frames)) < 0) {
	write_log ("Cannot set hw parameters: %s.\n", snd_strerror (err));
	goto nosound;
    }

    if ((err = set_sw_params (alsa_playback_handle, sw_params, buffer_frames, period_frames)) < 0) {
	write_log ("Cannot set sw parameters: %s.\n", snd_strerror (err));
	goto nosound;
    }

    sndbufsize = period_frames * bytes_per_frame;
    snd_pcm_hw_params_free (hw_params);
    snd_pcm_sw_params_free (sw_params);

    if ((err = snd_pcm_prepare (alsa_playback_handle)) < 0) {
	write_log ("Cannot prepare audio interface for use: %s.\n", snd_strerror (err));
	goto nosound;
    }

    obtainedfreq = currprefs.sound_freq;

    if (dspbits == 16) {
	init_sound_table16 ();
	sample_handler = currprefs.sound_stereo ? sample16s_handler : sample16_handler;
    } else {
	init_sound_table8 ();
	sample_handler = currprefs.sound_stereo ? sample8s_handler : sample8_handler;
    }
    have_sound = 1;
    sound_available = 1;

    write_log ("ALSA: Using device '%s'.\n", alsa_device);
    write_log ("ALSA: Sound configured for %d bits at %d Hz. Buffer length is %u us, period size %d bytes.\n",
	       dspbits, rate, buffer_time, period_frames * bytes_per_frame);

    if (alsa_verbose)
	snd_pcm_dump (alsa_playback_handle, alsa_out);

    sndbufpt = sndbuffer;

    return 1;

 nosound:
    have_sound = 0;
    if (hw_params)
	snd_pcm_hw_params_free (hw_params);
    if (sw_params)
	snd_pcm_sw_params_free (sw_params);

    close_sound ();
    return 0;
}

/****i* januae-sd-alsa/reset_sound *****************************************
*
*   NAME
*       reset_sound()
*
*   SYNOPSIS
*       void reset_sound (void);
*
*   FUNCTION
*       Perform no ALSA reset operation.
*
****************************************************************************
*
*/
void reset_sound (void)
{
}

/****i* januae-sd-alsa/pause_sound *****************************************
*
*   NAME
*       pause_sound()
*
*   SYNOPSIS
*       void pause_sound (void);
*
*   FUNCTION
*       Drop queued ALSA playback frames if a playback handle is open.
*
*   NOTES
*       The snd_pcm_drop() result is not inspected.
*
****************************************************************************
*
*/
void pause_sound (void)
{
    if (alsa_playback_handle)
	snd_pcm_drop (alsa_playback_handle);
}

/****i* januae-sd-alsa/resume_sound ****************************************
*
*   NAME
*       resume_sound()
*
*   SYNOPSIS
*       void resume_sound (void);
*
*   FUNCTION
*       Prepare the ALSA playback handle again if one is open.
*
*   NOTES
*       The snd_pcm_prepare() result is not inspected.
*
****************************************************************************
*
*/
void resume_sound (void)
{
    if (alsa_playback_handle)
	snd_pcm_prepare (alsa_playback_handle);
}

/****i* januae-sd-alsa/sound_volume ****************************************
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

/****i* januae-sd-alsa/audio_default_options *******************************
*
*   NAME
*       audio_default_options()
*
*   SYNOPSIS
*       void audio_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Set the ALSA device name to "default" and disable verbose ALSA
*       diagnostics.
*
*   INPUTS
*       p - Unused.
*
****************************************************************************
*
*/
void audio_default_options (struct uae_prefs *p)
{
    strncpy (alsa_device, "default", 256);
    alsa_verbose = 0;
}

/****i* januae-sd-alsa/audio_save_options **********************************
*
*   NAME
*       audio_save_options()
*
*   SYNOPSIS
*       void audio_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Write the ALSA device name and verbosity flag as configuration
*       options.
*
*   INPUTS
*       f - Destination configuration file.
*       p - Unused.
*
****************************************************************************
*
*/
void audio_save_options (FILE *f, const struct uae_prefs *p)
{
    cfgfile_write (f, "alsa.device=%s\n", alsa_device);
    cfgfile_write (f, "alsa.verbose=%s\n", alsa_verbose ? "true" : "false");
}

/****i* januae-sd-alsa/audio_parse_option **********************************
*
*   NAME
*       audio_parse_option()
*
*   SYNOPSIS
*       int audio_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Parse an ALSA device-name or verbosity option into backend global
*       settings.
*
*   INPUTS
*       p - Unused.
*       option - Backend option name.
*       value - Backend option value.
*
*   RESULT
*       Nonzero if either recognized option parser handles the option;
*       otherwise zero.
*
*   NOTES
*       The configured ALSA device-name storage is limited to 256 bytes.
*
****************************************************************************
*
*/
int audio_parse_option (struct uae_prefs *p, const char *option, const char *value)
{
    return (cfgfile_string (option, value, "device",   alsa_device, 256)
	 || cfgfile_yesno  (option, value, "verbose", &alsa_verbose));
}
