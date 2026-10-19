 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Support for SDL sound
  *
  * Copyright 1997 Bernd Schmidt
  * Copyright 2003-2006 Richard Drummond
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "gensound.h"
#include "sounddep/sound.h"
#include "threaddep/thread.h"
#include <SDL_audio.h>

static int have_sound = 0;

uae_u16 sndbuffer[44100];
uae_u16 *sndbufpt;
int sndbufsize;
static SDL_AudioSpec spec;

static smp_comm_pipe to_sound_pipe;
static uae_sem_t data_available_sem, callback_done_sem, sound_init_sem;

static int in_callback, closing_sound;

/****i* januae-sd-sdl/clearbuffer ******************************************
*
*   NAME
*       clearbuffer()
*
*   SYNOPSIS
*       static void clearbuffer (void);
*
*   FUNCTION
*       Fill the fixed shared sound buffer with the format-dependent silence
*       value.
*
*   NOTES
*       The fill covers the complete sndbuffer array, not only sndbufsize.
*
****************************************************************************
*
*/
static void clearbuffer (void)
{
    memset (sndbuffer, (spec.format == AUDIO_U8) ? SOUND8_BASE_VAL : SOUND16_BASE_VAL, sizeof (sndbuffer));
}

/****i* januae-sd-sdl/dummy_callback ***************************************
*
*   NAME
*       dummy_callback()
*
*   SYNOPSIS
*       static void dummy_callback (void *userdata, Uint8 *stream, int len);
*
*   FUNCTION
*       Return without writing to an SDL audio callback buffer.
*
*   INPUTS
*       userdata - Unused.
*       stream - Unused.
*       len - Unused.
*
****************************************************************************
*
*/
static void dummy_callback (void *userdata, Uint8 *stream, int len)
{
  return;
}

/****i* januae-sd-sdl/sound_callback ***************************************
*
*   NAME
*       sound_callback()
*
*   SYNOPSIS
*       static void sound_callback (void *userdata, Uint8 *stream, int len);
*
*   FUNCTION
*       Wait for the producer to signal audio data, copy sndbufsize bytes
*       into the SDL stream and acknowledge consumption.
*
*   INPUTS
*       userdata - Unused.
*       stream - SDL-provided output buffer.
*       len - SDL callback buffer size; not used.
*
*   NOTES
*       When closing_sound is set, the callback returns without copying or
*       acknowledging. The function does not check that sndbufsize fits len.
*
****************************************************************************
*
*/
static void sound_callback (void *userdata, Uint8 *stream, int len)
{
    if (closing_sound)
	return;
    in_callback = 1;
    /* Wait for data to finish.  */
    uae_sem_wait (&data_available_sem);
    if (! closing_sound) {
	memcpy (stream, sndbuffer, sndbufsize);
	/* Notify writer that we're done.  */
	uae_sem_post (&callback_done_sem);
    }
    in_callback = 0;
}

/****i* januae-sd-sdl/finish_sound_buffer **********************************
*
*   NAME
*       finish_sound_buffer()
*
*   SYNOPSIS
*       void finish_sound_buffer (void);
*
*   FUNCTION
*       Signal that the shared sound buffer is ready, then wait for the SDL
*       callback acknowledgement.
*
*   NOTES
*       The caller blocks until callback_done_sem is posted; synchronization
*       requires a running callback.
*
****************************************************************************
*
*/
void finish_sound_buffer (void)
{
    uae_sem_post (&data_available_sem);
    uae_sem_wait (&callback_done_sem);
}

/****i* januae-sd-sdl/setup_sound ******************************************
*
*   NAME
*       setup_sound()
*
*   SYNOPSIS
*       int setup_sound (void);
*
*   FUNCTION
*       Probe SDL audio availability with the current frequency, sample
*       width, channel count and latency, then close a successful test
*       device.
*
*   RESULT
*       The updated sound_available flag: 1 if SDL_OpenAudio succeeds;
*       otherwise 0.
*
*   NOTES
*       A failed SDL audio subsystem initialization also returns 0. The test
*       does not leave playback open.
*
****************************************************************************
*
*/
int setup_sound (void)
{
    int success = 0;

    if (SDL_InitSubSystem (SDL_INIT_AUDIO) == 0) {
	spec.freq = currprefs.sound_freq;
	spec.format = currprefs.sound_bits == 8 ? AUDIO_U8 : AUDIO_S16SYS;
	spec.channels = currprefs.sound_stereo ? 2 : 1;
	spec.callback = dummy_callback;
	spec.samples  = spec.freq * currprefs.sound_latency / 1000;
	spec.callback = sound_callback;
	spec.userdata = 0;

	if (SDL_OpenAudio (&spec, 0) < 0) {
	    write_log ("Couldn't open audio: %s\n", SDL_GetError());
	    SDL_QuitSubSystem (SDL_INIT_AUDIO);
	} else {
	    success = 1;
	    SDL_CloseAudio ();
	}
    }

    sound_available = success;

    return sound_available;
}

/****i* januae-sd-sdl/open_sound *******************************************
*
*   NAME
*       open_sound()
*
*   SYNOPSIS
*       static int open_sound (void);
*
*   FUNCTION
*       Open SDL audio playback, choose the 8-bit or 16-bit sample handler
*       and initialize the shared output-buffer settings.
*
*   RESULT
*       1 if SDL_OpenAudio succeeds; otherwise 0.
*
*   NOTES
*       The callback length returned by SDL is not queried; the requested
*       format and number of samples are used.
*
****************************************************************************
*
*/
static int open_sound (void)
{
    spec.freq = currprefs.sound_freq;
    spec.format = currprefs.sound_bits == 8 ? AUDIO_U8 : AUDIO_S16SYS;
    spec.channels = currprefs.sound_stereo ? 2 : 1;
    spec.samples  = spec.freq * currprefs.sound_latency / 1000;
    spec.callback = sound_callback;
    spec.userdata = 0;

    clearbuffer();
    if (SDL_OpenAudio (&spec, NULL) < 0) {
	write_log ("Couldn't open audio: %s\n", SDL_GetError());
	return 0;
    }

    if (spec.format == AUDIO_S16SYS) {
	init_sound_table16 ();
	sample_handler = currprefs.sound_stereo ? sample16s_handler : sample16_handler;
    } else {
	init_sound_table8 ();
	sample_handler = currprefs.sound_stereo ? sample8s_handler : sample8_handler;
    }
    have_sound = 1;

    sound_available = 1;
    obtainedfreq = currprefs.sound_freq;
    sndbufsize = spec.samples * currprefs.sound_bits / 8 * spec.channels;
    write_log ("SDL sound driver found and configured for %d bits at %d Hz, buffer is %d ms (%d bytes).\n",
	currprefs.sound_bits, spec.freq, spec.samples * 1000 / spec.freq, sndbufsize);
   sndbufpt = sndbuffer;

    return 1;
}

/****i* januae-sd-sdl/sound_thread *****************************************
*
*   NAME
*       sound_thread()
*
*   SYNOPSIS
*       static void *sound_thread (void *dummy);
*
*   FUNCTION
*       Process blocking sound-thread commands to open SDL audio or
*       terminate the worker.
*
*   INPUTS
*       dummy - Unused.
*
*   RESULT
*       NULL when the termination command is received.
*
*   NOTES
*       Command 0 signals completion after open_sound(), even if that call
*       fails; command 1 signals termination.
*
****************************************************************************
*
*/
static void *sound_thread (void *dummy)
{
    for (;;) {
	int cmd = read_comm_pipe_int_blocking (&to_sound_pipe);
	int n;

	switch (cmd) {
	case 0:
	    open_sound ();
	    uae_sem_post (&sound_init_sem);
	    break;
	case 1:
	    uae_sem_post (&sound_init_sem);
	    return 0;
	}
    }
}

/****i* januae-sd-sdl/init_sound_thread ************************************
*
*   NAME
*       init_sound_thread()
*
*   SYNOPSIS
*       static void init_sound_thread (void);
*
*   FUNCTION
*       Initialize the sound command pipe and three semaphores, then start
*       the SDL sound worker.
*
*   NOTES
*       The startup and semaphore initialization results are not checked.
*
****************************************************************************
*
*/
static void init_sound_thread (void)
{
    uae_thread_id tid;

    init_comm_pipe (&to_sound_pipe, 20, 1);
    uae_sem_init (&data_available_sem, 0, 0);
    uae_sem_init (&callback_done_sem, 0, 0);
    uae_sem_init (&sound_init_sem, 0, 0);
    uae_start_thread (sound_thread, NULL, &tid);
}

/****i* januae-sd-sdl/close_sound ******************************************
*
*   NAME
*       close_sound()
*
*   SYNOPSIS
*       void close_sound (void);
*
*   FUNCTION
*       Pause SDL playback, signal the audio worker to terminate, close SDL
*       audio and destroy the associated semaphores.
*
*   NOTES
*       Does nothing when have_sound is zero. A pending callback is woken
*       through data_available_sem before shutdown.
*
****************************************************************************
*
*/
void close_sound (void)
{
    if (! have_sound)
	return;

    SDL_PauseAudio (1);
    clearbuffer();
    if (in_callback) {
	closing_sound = 1;
	uae_sem_post (&data_available_sem);
    }
    write_comm_pipe_int (&to_sound_pipe, 1, 1);
    uae_sem_wait (&sound_init_sem);
    SDL_CloseAudio ();
    uae_sem_destroy (&data_available_sem);
    uae_sem_destroy (&sound_init_sem);
    uae_sem_destroy (&callback_done_sem);
    have_sound = 0;
}

/****i* januae-sd-sdl/init_sound *******************************************
*
*   NAME
*       init_sound()
*
*   SYNOPSIS
*       int init_sound (void);
*
*   FUNCTION
*       Start the SDL audio worker, request opening a playback device and
*       unpause SDL audio after worker acknowledgement.
*
*   RESULT
*       The current have_sound flag: 1 if the worker opened playback;
*       otherwise 0.
*
*   NOTES
*       The unpause call occurs even if open_sound() failed.
*
****************************************************************************
*
*/
int init_sound (void)
{
  kprintf("init_sound entered\n");
    in_callback = 0;
    closing_sound = 0;

    init_sound_thread ();
    write_comm_pipe_int (&to_sound_pipe, 0, 1);
    uae_sem_wait (&sound_init_sem);
    SDL_PauseAudio (0);

  kprintf("init_sound left (have_sound: %d)\n",have_sound);

    return have_sound;
}

/****i* januae-sd-sdl/pause_sound ******************************************
*
*   NAME
*       pause_sound()
*
*   SYNOPSIS
*       void pause_sound (void);
*
*   FUNCTION
*       Pause SDL audio playback.
*
****************************************************************************
*
*/
void pause_sound (void)
{
    SDL_PauseAudio (1);
}

/****i* januae-sd-sdl/resume_sound *****************************************
*
*   NAME
*       resume_sound()
*
*   SYNOPSIS
*       void resume_sound (void);
*
*   FUNCTION
*       Clear the shared sound buffer and unpause SDL audio playback.
*
****************************************************************************
*
*/
void resume_sound (void)
{
    clearbuffer();
    SDL_PauseAudio (0);
}

/****i* januae-sd-sdl/reset_sound ******************************************
*
*   NAME
*       reset_sound()
*
*   SYNOPSIS
*       void reset_sound (void);
*
*   FUNCTION
*       Clear the shared sound buffer to its format-dependent silence value.
*
****************************************************************************
*
*/
void reset_sound (void)
{
   clearbuffer();
   return;
}

/****i* januae-sd-sdl/sound_volume *****************************************
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

/****i* januae-sd-sdl/audio_default_options ********************************
*
*   NAME
*       audio_default_options()
*
*   SYNOPSIS
*       void audio_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Perform no SDL audio default-option changes.
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

/****i* januae-sd-sdl/audio_save_options ***********************************
*
*   NAME
*       audio_save_options()
*
*   SYNOPSIS
*       void audio_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Write no SDL-specific configuration options.
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

/****i* januae-sd-sdl/audio_parse_option ***********************************
*
*   NAME
*       audio_parse_option()
*
*   SYNOPSIS
*       int audio_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Decline SDL-specific configuration options without changing
*       preferences.
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
