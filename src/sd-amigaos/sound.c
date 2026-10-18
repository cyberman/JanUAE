 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Support for Amiga audio.device sound
  *
  * Copyright 1996, 1997, 1998 Samuel Devulder, Holger Jakob (AHI).
  * Copyright 2004-2007 Richard Drummond
  *
  * $Id$
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "gensound.h"
#include "sounddep/sound.h"

#include <hardware/custom.h>
#include <hardware/cia.h>
#include <devices/audio.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <graphics/gfxbase.h>

#ifdef USE_AHIDEVICE
struct MsgPort    *AHImp    = NULL;
struct AHIRequest *AHIio[2] = {NULL, NULL};
struct AHIRequest *linkio   = NULL;
#endif

#if !defined __amigaos4__ && !defined __MORPHOS__ && !defined __AROS__
# define CIAAPRA 0xBFE001
# define CUSTOM  0xDFF000

static struct Custom *custom = (struct Custom*) CUSTOM;
static struct CIA    *cia    = (struct CIA *)   CIAAPRA;

static int oldledstate;
#endif

/*
 * Compared to Linux, AF_SOUND, and mac above, the AMIGA sound processing
 * with OS routines is awfull. (sam). But with AHI DOSDriver it is far more
 * easier (but it is still a mess here !).
 */

uae_u8 whichchannel[] = {1, 2, 4, 8};

struct IOAudio *AudioIO  = NULL;
struct MsgPort *AudioMP  = NULL;
struct Message *AudioMSG = NULL;

unsigned char *buffers[2];
uae_u16 *sndbuffer;
uae_u16 *sndbufpt;
int sndbufsize;
int bufidx, devopen, ahiopen;

int have_sound;
int clockval;
int period;



/****i* januae-sound/setup_sound *******************************************
*
*   NAME
*       setup_sound() -- Advertise sound-driver availability.
*
*   SYNOPSIS
*       int setup_sound (void);
*
*   FUNCTION
*       Mark the host sound backend available without opening an audio
*       device.
*
*   INPUTS
*       None.
*
*   RESULT
*       Always returns 1.
*
****************************************************************************
*
*/

int setup_sound (void)
{
    sound_available = 1;
    return 1;
}

/****i* januae-sound/open_AHI **********************************************
*
*   NAME
*       open_AHI() -- Open the optional AHI output device.
*
*   SYNOPSIS
*       static const char *open_AHI (void);
*
*   FUNCTION
*       Create a message port and the first AHI request, open ahi.device,
*       and allocate a copy of the request for double-buffered output when
*       AHI support is enabled.
*
*   INPUTS
*       None.
*
*   RESULT
*       The AHI device name on success; otherwise NULL.
*
*   NOTES
*       Without USE_AHIDEVICE, this function always returns NULL.
*
****************************************************************************
*
*/

static const char *open_AHI (void)
{
#ifdef USE_AHIDEVICE
    if ((AHImp = CreateMsgPort())) {
	if ((AHIio[0] = (struct AHIRequest *)
		CreateIORequest (AHImp, sizeof (struct AHIRequest)))) {
	    AHIio[0]->ahir_Version = 4;

	    if (!OpenDevice (AHINAME, 0, (struct IORequest *)AHIio[0], 0)) {
	        if ((AHIio[1] = malloc (sizeof(struct AHIRequest)))) {
		    memcpy (AHIio[1], AHIio[0], sizeof(struct AHIRequest));
		    //kprintf("AHI: AHINAME is >%s<\n", AHINAME);
		    return AHINAME;
		}
	    }
	}
    }
#endif
    return NULL;
}

/****i* januae-sound/close_AHI *********************************************
*
*   NAME
*       close_AHI() -- Release the AHI audio resources.
*
*   SYNOPSIS
*       static void close_AHI (void);
*
*   FUNCTION
*       Wait for outstanding AHI requests, close the device, free both
*       requests and delete the message port; reset the stored AHI request
*       pointers.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The resource-release sequence is compiled only with USE_AHIDEVICE.
*
****************************************************************************
*
*/

static void close_AHI (void)
{
#ifdef USE_AHIDEVICE
    if (!CheckIO ((struct IORequest *)AHIio[0]))
	WaitIO ((struct IORequest *)AHIio[0]);

    if (linkio) { /* Only if the second request was started */
	if (!CheckIO ((struct IORequest *) AHIio[1]))
	    WaitIO ((struct IORequest *) AHIio[1]);
    }
    CloseDevice ((struct IORequest *) AHIio[0]);
    DeleteIORequest ((void*) AHIio[0]);
    free (AHIio[1]);
    DeleteMsgPort ((void*)AHImp);
    AHIio[0] = NULL;
    AHIio[1] = NULL;
    linkio   = NULL;
#endif
}

/****i* januae-sound/get_clockval ******************************************
*
*   NAME
*       get_clockval() -- Determine the host audio reference clock.
*
*   SYNOPSIS
*       static int get_clockval (void);
*
*   FUNCTION
*       Read the graphics.library display flags and select the PAL or NTSC
*       reference frequency.
*
*   INPUTS
*       None.
*
*   RESULT
*       3546895 for PAL, 3579545 for NTSC, or 0 if graphics.library cannot
*       be opened.
*
****************************************************************************
*
*/

static int get_clockval (void)
{
    struct GfxBase *GB;
    int clk = 0;

    GB = (void*) OpenLibrary ("graphics.library", 0L);

    if (GB) {
	if (GB->DisplayFlags & PAL)
	    clk = 3546895;        /* PAL clock */
	else
	    clk = 3579545;        /* NTSC clock */
	CloseLibrary ((void *) GB);
    }
    return clk;
}

/****i* januae-sound/init_sound ********************************************
*
*   NAME
*       init_sound() -- Initialize Amiga audio output and mixing buffers.
*
*   SYNOPSIS
*       int init_sound (void);
*
*   FUNCTION
*       Derive the sample period from the host clock, prefer AHI when
*       available, otherwise open audio.device in eight-bit mono mode,
*       allocate two buffers and select the matching sample handler.
*
*   INPUTS
*       None.
*
*   RESULT
*       1 when device and buffers are initialized; otherwise 0.
*
*   NOTES
*       The buffer size has an 8192-byte minimum. Registers close_sound()
*       with atexit.
*
****************************************************************************
*
*/

int init_sound (void)
{
    /* too complex ? No it is only the allocation of a single channel ! */
    /* it would have been far less painfull if AmigaOS provided a */
    /* SOUND: device handler */
    int rate;
    const char *devname = NULL;

    atexit (close_sound); /* if only amiga os had resource tracking */

    /* determine the clock */
    clockval = get_clockval ();
    if (clockval == 0)
	goto fail;

    /* check freq */
    if (!currprefs.sound_freq)
	currprefs.sound_freq = 22000;
    if (clockval / currprefs.sound_freq < 80/*124*/ || clockval/currprefs.sound_freq > 65535) {
	write_log ("Can't use sound with desired frequency %d Hz\n", currprefs.sound_freq);
	changed_prefs.sound_freq = currprefs.sound_freq = 22000;
    }
    rate   = currprefs.sound_freq;
    period = (uae_u16)(clockval / rate);


    devname = open_AHI ();
    if (devname)
	ahiopen = 1;
    else
	ahiopen = 0;

    if (!ahiopen) {
	/* Use the plain old audio.device */

	/* We support only 8-bit mono sound */
	changed_prefs.sound_stereo = currprefs.sound_stereo = 0;
	changed_prefs.sound_bits = currprefs.sound_bits = 8;

	/* setup the stuff */
	AudioMP = CreateMsgPort ();
	if (!AudioMP)
	    goto fail;
	AudioIO = (struct IOAudio *) CreateIORequest (AudioMP,
						      sizeof(struct IOAudio));
	if (!AudioIO)
	    goto fail;

	AudioIO->ioa_Request.io_Message.mn_Node.ln_Pri /*pfew!!*/ = 85;
	AudioIO->ioa_Data     = whichchannel;
	AudioIO->ioa_Length   = sizeof(whichchannel);
	AudioIO->ioa_AllocKey = 0;

	if (OpenDevice (devname = AUDIONAME, 0, (void*)AudioIO, 0))
	    goto fail;
	devopen = 1;
    }

    /* calculate buffer size */
    sndbufsize = rate * currprefs.sound_latency * (currprefs.sound_bits / 8) * (currprefs.sound_stereo ? 2 : 1) / 1000;
    sndbufsize = (sndbufsize + 1) & ~1;

    /* added by o1i, Broken Wall on FreeMem otherwise ..? */
    if(sndbufsize < 8192) {
      sndbufsize = 8192; /* set to default */
    }

    /* get the buffers */
    if (ahiopen) {
	buffers[0] = (void*) AllocMem (sndbufsize,MEMF_PUBLIC | MEMF_CLEAR);
	buffers[1] = (void*) AllocMem (sndbufsize,MEMF_PUBLIC | MEMF_CLEAR);
	if (!buffers[0] || !buffers[1])
	    goto fail;
    } else {
	buffers[0] = (void*) AllocMem (sndbufsize, MEMF_CHIP | MEMF_CLEAR);
	buffers[1] = (void*) AllocMem (sndbufsize, MEMF_CHIP | MEMF_CLEAR);
	if (!buffers[0] || !buffers[1])
	    goto fail;
    }
   bufidx    = 0;
    sndbuffer = sndbufpt = (uae_u16*) buffers[bufidx];

#if !defined __amigaos4__ && !defined __MORPHOS__ && !defined __AROS__
    oldledstate  = cia->ciapra & (1 << CIAB_LED);
    cia->ciapra |= (1 << CIAB_LED);
#endif

    if (currprefs.sound_bits == 16) {
	init_sound_table16 ();
	sample_handler = currprefs.sound_stereo ? sample16s_handler : sample16_handler;
    } else {
	init_sound_table8 ();
	sample_handler = currprefs.sound_stereo ? sample8s_handler : sample8_handler;
    }

    have_sound = 1;
    obtainedfreq = rate;

    write_log ("Sound driver found and configured for %d bits %s "
	       "at %d Hz, buffer is %d bytes (%s)\n",
	       currprefs.sound_bits, currprefs.sound_stereo ? "stereo" : "mono",
	       rate, sndbufsize, devname);

    sound_available = 1;
    return 1;
fail:
    sound_available = 0;
    return 0;
}

/****i* januae-sound/close_sound *******************************************
*
*   NAME
*       close_sound() -- Release audio output and its buffers.
*
*   SYNOPSIS
*       void close_sound (void);
*
*   FUNCTION
*       Close the active AHI or audio.device connection, release requests
*       and message ports, free both sample buffers and clear sound
*       availability; restore the hardware LED state on applicable classic
*       hosts.
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

void close_sound (void)
{
#ifdef USE_AHIDEVICE
    if (ahiopen) {
	close_AHI ();
	ahiopen = 0;
    }
#endif
    if (devopen) {
	CloseDevice ((void*) AudioIO);
	devopen = 0;
    }
    if (AudioIO) {
	DeleteIORequest ((void*) AudioIO);
	AudioIO = NULL;
    }
    if (AudioMP) {
	DeleteMsgPort ((void*) AudioMP);
	AudioMP = NULL;
    }
    if (buffers[0]) {
	FreeMem ((APTR) buffers[0], sndbufsize);
	buffers[0] = 0;
    }
    if (buffers[1]) {
	FreeMem ((APTR) buffers[1], sndbufsize);
	buffers[1] = 0;
    }
    if (sound_available) {
#if !defined __amigaos4__ && !defined __MORPHOS__ && !defined __AROS__
	cia->ciapra = (cia->ciapra & ~(1 << CIAB_LED)) | oldledstate;
#endif
	sound_available = 0;
    }
}

/****i* januae-sound/pause_sound *******************************************
*
*   NAME
*       pause_sound() -- Provide the sound pause entry point.
*
*   SYNOPSIS
*       void pause_sound (void);
*
*   FUNCTION
*       Perform no operation. Pausing the host audio stream is not
*       implemented.
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

void pause_sound (void)
{
}

/****i* januae-sound/resume_sound ******************************************
*
*   NAME
*       resume_sound() -- Provide the sound resume entry point.
*
*   SYNOPSIS
*       void resume_sound (void);
*
*   FUNCTION
*       Perform no operation. Resuming the host audio stream is not
*       implemented.
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

void resume_sound (void)
{
}

/****i* januae-sound/reset_sound *******************************************
*
*   NAME
*       reset_sound() -- Provide the sound reset entry point.
*
*   SYNOPSIS
*       void reset_sound (void);
*
*   FUNCTION
*       Perform no operation. Resetting the host audio stream is not
*       implemented.
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

void reset_sound (void)
{
}

/****i* januae-sound/sound_volume ******************************************
*
*   NAME
*       sound_volume() -- Provide the sound-volume entry point.
*
*   SYNOPSIS
*       void sound_volume (int dir);
*
*   FUNCTION
*       Ignore the requested volume direction; volume adjustment is not
*       implemented.
*
*   INPUTS
*       dir - Requested adjustment direction; unused.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void sound_volume (int dir)
{
}

/*
 * Handle audio specific cfgfile options
 */
/****i* januae-sound/audio_default_options *********************************
*
*   NAME
*       audio_default_options() -- Provide the audio-preference default
*       hook.
*
*   SYNOPSIS
*       void audio_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Leave the supplied preferences unchanged.
*
*   INPUTS
*       p - Preferences structure; unused.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void audio_default_options (struct uae_prefs *p)
{
}

/****i* januae-sound/audio_save_options ************************************
*
*   NAME
*       audio_save_options() -- Provide the audio-preference save hook.
*
*   SYNOPSIS
*       void audio_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Write no audio-specific options.
*
*   INPUTS
*       f - Output file; unused. p - Preferences; unused.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void audio_save_options (FILE *f, const struct uae_prefs *p)
{
}

/****i* januae-sound/audio_parse_option ************************************
*
*   NAME
*       audio_parse_option() -- Provide the audio-preference parsing hook.
*
*   SYNOPSIS
*       int audio_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Accept no audio-specific option names and leave the preferences
*       unchanged.
*
*   INPUTS
*       p - Preferences; unused. option - Option name; unused. value -
*       Option value; unused.
*
*   RESULT
*       Always returns 0.
*
****************************************************************************
*
*/

int audio_parse_option (struct uae_prefs *p, const char *option, const char *value)
{
    return 0;
}
