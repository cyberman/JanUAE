 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Drive Click Emulation Support Functions
  *
  * Copyright 2004 James Bagg, Toni Wilen
  */

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef DRIVESOUND

#include "uae.h"
#include "options.h"
#include "sounddep/sound.h"
#include "zfile.h"
#include "events.h"

#include "driveclick.h"

#include "fsdb.h"

static struct drvsample drvs[4][DS_END];
static int freq = 44100;

static int drv_starting[4], drv_spinning[4], drv_has_spun[4], drv_has_disk[4];

static int click_initialized;
#define DS_SHIFT 10
static int sample_step;

static uae_s16 *clickbuffer;

/****i* januae-driveclick/decodewav ****************************************
*
*   NAME
*       decodewav() -- Decode a RIFF/WAVE drive sound sample.
*
*   SYNOPSIS
*       uae_s16 *decodewav (uae_u8 *s, int *lenp);
*
*   FUNCTION
*       Check the RIFF and WAVE identifiers, scan wave chunks for the data
*       chunk, copy its payload into allocated sample storage and replace
*       the supplied byte length with a count of 16-bit samples. A format
*       chunk updates the module sample-frequency setting.
*
*   INPUTS
*       s - Wave-file byte buffer; lenp - Pointer to its total byte length,
*       updated when sample data is found.
*
*   RESULT
*       An allocated sample buffer when a data chunk is found; otherwise
*       NULL.
*
*   NOTES
*       The decoder interprets the sample payload as signed 16-bit data
*       without performing channel or encoding conversion.
*
****************************************************************************
*
*/

uae_s16 *decodewav (uae_u8 *s, int *lenp)
{
    uae_s16 *dst;
    uae_u8 *src = s;
    int len;

    if (memcmp (s, "RIFF", 4))
	return 0;
    if (memcmp (s + 8, "WAVE", 4))
	return 0;
    s += 12;
    len = *lenp;
    while (s < src + len) {
	if (!memcmp (s, "fmt ", 4))
	    freq = s[8 + 4] | (s[8 + 5] << 8);
	if (!memcmp (s, "data", 4)) {
	    s += 4;
	    len = s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24);
	    dst = xmalloc (len);
	    memcpy (dst, s + 4, len);
	    *lenp = len / 2;
	    return dst;
	}
	s += 8 + (s[4] | (s[5] << 8) | (s[6] << 16) | (s[7] << 24));
    }
    return 0;
}

/****i* januae-driveclick/loadsample ***************************************
*
*   NAME
*       loadsample() -- Load a drive sound sample from a file.
*
*   SYNOPSIS
*       static int loadsample (const char *path, struct drvsample *ds);
*
*   FUNCTION
*       Read a file through the zfile interface, pass its contents to
*       decodewav and store the resulting sample pointer and sample count in
*       the drive sample descriptor.
*
*   INPUTS
*       path - Source filename; ds - Destination sample descriptor.
*
*   RESULT
*       Zero if the source file cannot be opened; one after attempting to
*       decode a readable file.
*
*   NOTES
*       A return value of one does not indicate whether decodewav found a
*       usable data chunk.
*
****************************************************************************
*
*/

static int loadsample (const char *path, struct drvsample *ds)
{
    struct zfile *f;
    uae_u8 *buf;
    int size;

    f = zfile_fopen (path, "rb");
    if (!f) {
	write_log ("driveclick: can't open '%s'\n", path);
	return 0;
    }
    zfile_fseek (f, 0, SEEK_END);
    size = zfile_ftell (f);
    buf = malloc (size);
    zfile_fseek (f, 0, SEEK_SET);
    zfile_fread (buf, size, 1, f);
    zfile_fclose (f);
    ds->len = size;
    ds->p = decodewav (buf, &ds->len);
    free (buf);
    return 1;
}

/****i* januae-driveclick/freesample ***************************************
*
*   NAME
*       freesample() -- Release the storage owned by a drive sample.
*
*   SYNOPSIS
*       static void freesample (struct drvsample *s);
*
*   FUNCTION
*       Free the sample data pointer and set it to NULL.
*
*   INPUTS
*       s - Drive sample descriptor whose data pointer is released.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void freesample (struct drvsample *s)
{
    free (s->p);
    s->p = 0;
}

extern char *start_path;

/****i* januae-driveclick/driveclick_init **********************************
*
*   NAME
*       driveclick_init() -- Initialize drive sound emulation from
*       preferences.
*
*   SYNOPSIS
*       void driveclick_init (void);
*
*   FUNCTION
*       Release previously owned sample buffers, then visit the four
*       configured floppy drives. Reset mixing state only when a positive
*       sample-selection total has been established.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The sample-loading branch is excluded by a compile-time false
*       condition in this implementation, so this routine does not activate
*       click mixing.
*
****************************************************************************
*
*/

void driveclick_init (void)
{
    int v, vv, i, j;
    char tmp[1000];

    driveclick_free ();
    vv = 0;
    for (i = 0; i < 4; i++) {
	if (currprefs.dfxclick[i]) {
	    /* TODO: Implement location of sample data */
	}
    }
    if (vv > 0) {
	driveclick_reset ();
	click_initialized = 1;
    }
}

/****i* januae-driveclick/driveclick_reset *********************************
*
*   NAME
*       driveclick_reset() -- Reallocate the drive sound mixing buffer.
*
*   SYNOPSIS
*       void driveclick_reset (void);
*
*   FUNCTION
*       Free the previous mixing buffer, allocate storage using the current
*       sound buffer size and derive the fixed-point sample step from the
*       drive sample frequency and configured output rate.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The sample step uses DS_SHIFT fractional bits.
*
****************************************************************************
*
*/

void driveclick_reset (void)
{
    free (clickbuffer);
    clickbuffer = xmalloc (sndbufsize);
    sample_step = (freq << DS_SHIFT) / currprefs.sound_freq;
}

/****i* januae-driveclick/driveclick_free **********************************
*
*   NAME
*       driveclick_free() -- Release drive sound samples and mixing state.
*
*   SYNOPSIS
*       void driveclick_free (void);
*
*   FUNCTION
*       Free sample buffers for all four drives, clear their descriptors,
*       release the mix buffer and mark click mixing uninitialized.
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

void driveclick_free (void)
{
    int i, j;

    for (i = 0; i < 4; i++) {
	for (j = 0; j < DS_END; j++)
	    freesample (&drvs[i][j]);
    }
    memset (drvs, 0, sizeof (drvs));
    free (clickbuffer);
    clickbuffer = 0;
    click_initialized = 0;
}

/****i* januae-driveclick/getsample ****************************************
*
*   NAME
*       getsample() -- Compute one emulated drive sound sample.
*
*   SYNOPSIS
*       STATIC_INLINE uae_s16 getsample (void);
*
*   FUNCTION
*       Inspect enabled drives, advance applicable startup, spin and click
*       sample positions, and combine the accumulated signed sample value
*       using the per-drive divisor.
*
*   INPUTS
*       None.
*
*   RESULT
*       A signed 16-bit output sample, or zero when no drive contributes to
*       the divisor.
*
*   NOTES
*       The active sample source depends on motor state, disk presence and
*       per-sample playback positions.
*
****************************************************************************
*
*/

STATIC_INLINE uae_s16 getsample (void)
{
    uae_s32 smp = 0;
    int div = 0, i;

    for (i = 0; i < 4; i++) {
	if (currprefs.dfxclick[i]) {
	    struct drvsample *ds_start = &drvs[i][DS_START];
	    struct drvsample *ds_spin = drv_has_disk[i] ? &drvs[i][DS_SPIN] : &drvs[i][DS_SPINND];
	    struct drvsample *ds_click = &drvs[i][DS_CLICK];
	    struct drvsample *ds_snatch = &drvs[i][DS_SNATCH];
	    div += 2;
	    if (drv_spinning[i] || drv_starting[i]) {
		if (drv_starting[i] && drv_has_spun[i]) {
		    if (ds_start->p && ds_start->pos < ds_start->len) {
			smp = ds_start->p[ds_start->pos >> DS_SHIFT];
			ds_start->pos += sample_step;
		    } else {
			drv_starting[i] = 0;
		    }
		} else if (drv_starting[i] && drv_has_spun[i] == 0) {
		    if (ds_snatch->p && ds_snatch->pos < ds_snatch->len) {
			smp = ds_snatch->p[ds_snatch->pos >> DS_SHIFT];
			ds_snatch->pos += sample_step;
		    } else {
			drv_starting[i] = 0;
			ds_start->pos = ds_start->len;
			drv_has_spun[i] = 1;
		    }
		}
		if (ds_spin->p && drv_starting[i] == 0) {
		    if (ds_spin->pos >= ds_spin->len)
			ds_spin->pos -= ds_spin->len;
		    smp = ds_spin->p[ds_spin->pos >> DS_SHIFT];
		    ds_spin->pos += sample_step;
		}
	    }
	    if (ds_click->p && ds_click->pos < ds_click->len) {
		smp += ds_click->p[ds_click->pos >> DS_SHIFT];
		ds_click->pos += sample_step;
	    }
	}
    }
    if (!div)
	return 0;
    return smp / div;
}

static int clickcnt;

/****i* januae-driveclick/mix **********************************************
*
*   NAME
*       mix() -- Accumulate drive audio for the current output buffer.
*
*   SYNOPSIS
*       static void mix (void);
*
*   FUNCTION
*       Determine the number of output frames already written, then fill the
*       drive click buffer up to that frame count by calling getsample.
*       Apply the configured click-volume attenuation when nonzero.
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

static void mix (void)
{
    int total = ((uae_u8*)sndbufpt - (uae_u8*)sndbuffer) / (currprefs.sound_stereo ? 4 : 2);

    if (currprefs.dfxclickvolume > 0) {
	while (clickcnt < total) {
	    clickbuffer[clickcnt++] = getsample() * (100 - currprefs.dfxclickvolume) / 100;
	}
    } else {
	while (clickcnt < total) {
	    clickbuffer[clickcnt++] = getsample();
	}
    }
}

/****i* januae-driveclick/limit ********************************************
*
*   NAME
*       limit() -- Clamp a mixed audio value to the 16-bit sample range.
*
*   SYNOPSIS
*       STATIC_INLINE uae_s16 limit (uae_s32 v);
*
*   FUNCTION
*       Saturate values below -32768 or above 32767 and return the resulting
*       signed 16-bit value.
*
*   INPUTS
*       v - Signed 32-bit mixed audio value.
*
*   RESULT
*       A signed 16-bit value within the inclusive range -32768 to 32767.
*
****************************************************************************
*
*/

STATIC_INLINE uae_s16 limit (uae_s32 v)
{
    if (v < -32768)
	v = -32768;
    if (v > 32767)
	v = 32767;
    return v;
}

/****i* januae-driveclick/driveclick_mix ***********************************
*
*   NAME
*       driveclick_mix() -- Mix generated drive sound into emulator audio.
*
*   SYNOPSIS
*       void driveclick_mix (uae_s16 *sndbuffer, int size);
*
*   FUNCTION
*       When initialized, fill the current drive sound buffer and combine
*       each drive sample with the existing output. Use the same drive
*       contribution for both stereo channels and saturate the result to
*       16-bit range.
*
*   INPUTS
*       sndbuffer - Destination emulator audio samples; size - Mono sample
*       count or total interleaved stereo sample count.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The combined sample is weighted by two thirds; when click mixing is
*       not initialized the output is left untouched.
*
****************************************************************************
*
*/

void driveclick_mix (uae_s16 *sndbuffer, int size)
{
    int i;

    if (!click_initialized)
	return;
    mix();
    clickcnt = 0;
    if (currprefs.sound_stereo) {
        for (i = 0; i < size / 2; i++) {
	    uae_s16 s = clickbuffer[i];
	    sndbuffer[0] = limit(((sndbuffer[0] + s) * 2) / 3);
	    sndbuffer[1] = limit(((sndbuffer[1] + s) * 2) / 3);
	    sndbuffer += 2;
        }
    } else {
        for (i = 0; i < size; i++) {
	    sndbuffer[0] = limit(((sndbuffer[0] + clickbuffer[i]) * 2) / 3);
	    sndbuffer++;
	}
    }
}

/****i* januae-driveclick/driveclick_click *********************************
*
*   NAME
*       driveclick_click() -- Start a floppy seek-click sample.
*
*   SYNOPSIS
*       void driveclick_click (int drive, int startOffset);
*
*   FUNCTION
*       If drive sounds are active for the selected drive, first preserve
*       audio already accumulated for the current buffer and then set the
*       click sample position from the supplied offset, capped at half the
*       sample length.
*
*   INPUTS
*       drive - Zero-based floppy-drive index; startOffset - Starting click
*       offset scaled to the fixed-point sample position.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void driveclick_click (int drive, int startOffset)
{
    if (!click_initialized)
	return;
    if (!currprefs.dfxclick[drive])
	return;
    mix ();
    drvs[drive][DS_CLICK].pos = (startOffset * 4) << DS_SHIFT;
    if (drvs[drive][DS_CLICK].pos > drvs[drive][DS_CLICK].len / 2)
	drvs[drive][DS_CLICK].pos = drvs[drive][DS_CLICK].len / 2;
}

/****i* januae-driveclick/driveclick_motor *********************************
*
*   NAME
*       driveclick_motor() -- Update the emulated drive motor sound state.
*
*   SYNOPSIS
*       void driveclick_motor (int drive, int running);
*
*   FUNCTION
*       Preserve mixed audio before changing the motor state, stop the motor
*       when requested, or start rotation and select the initial spin,
*       startup and disk-engagement sample positions.
*
*   INPUTS
*       drive - Zero-based floppy-drive index; running - Zero to stop, one
*       to run, or two to request startup-sample restart.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The function returns without modifying state when drive click mixing
*       is inactive or disabled for the specified drive.
*
****************************************************************************
*
*/

void driveclick_motor (int drive, int running)
{
    if (!click_initialized)
	return;
    if (!currprefs.dfxclick[drive])
	return;
    mix();
    if (running == 0) {
	drv_starting[drive] = 0;
	drv_spinning[drive] = 0;
    } else {
        if (drv_spinning[drive] == 0) {
	    drv_starting[drive] = 1;
	    drv_spinning[drive] = 1;
	    if (drv_has_disk[drive] && drv_has_spun[drive] == 0 && drvs[drive][DS_SNATCH].pos >= drvs[drive][DS_SNATCH].len)
		drvs[drive][DS_SNATCH].pos = 0;
	    if (running == 2)
		drvs[drive][DS_START].pos = 0;
	    drvs[drive][DS_SPIN].pos = 0;
	}
    }
}

/****i* januae-driveclick/driveclick_insert ********************************
*
*   NAME
*       driveclick_insert() -- Update disk-presence state for drive sounds.
*
*   SYNOPSIS
*       void driveclick_insert (int drive, int eject);
*
*   FUNCTION
*       Record whether the specified drive has an inserted disk and clear
*       its previous-spin flag when the disk is ejected.
*
*   INPUTS
*       drive - Zero-based floppy-drive index; eject - Nonzero for ejection,
*       zero for insertion.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Changes are ignored unless drive click mixing is initialized and
*       enabled for the specified drive.
*
****************************************************************************
*
*/

void driveclick_insert (int drive, int eject)
{
    if (!click_initialized)
	return;
    if (!currprefs.dfxclick[drive])
	return;
    if (eject)
	drv_has_spun[drive] = 0;
    drv_has_disk[drive] = !eject;
}

/****i* januae-driveclick/driveclick_check_prefs ***************************
*
*   NAME
*       driveclick_check_prefs() -- Apply updated floppy drive sound
*       preferences.
*
*   SYNOPSIS
*       void driveclick_check_prefs (void);
*
*   FUNCTION
*       Compare output volume, per-drive enable settings and external sample
*       identifiers with the pending preferences. Copy changed fields into
*       the current preferences and reinitialize drive sound state when any
*       tracked field differs.
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

void driveclick_check_prefs (void)
{
    int i;

    if (currprefs.dfxclickvolume != changed_prefs.dfxclickvolume ||
	currprefs.dfxclick[0] != changed_prefs.dfxclick[0] ||
	currprefs.dfxclick[1] != changed_prefs.dfxclick[1] ||
	currprefs.dfxclick[2] != changed_prefs.dfxclick[2] ||
	currprefs.dfxclick[3] != changed_prefs.dfxclick[3] ||
	strcmp (currprefs.dfxclickexternal[0], changed_prefs.dfxclickexternal[0]) ||
	strcmp (currprefs.dfxclickexternal[1], changed_prefs.dfxclickexternal[1]) ||
	strcmp (currprefs.dfxclickexternal[2], changed_prefs.dfxclickexternal[2]) ||
	strcmp (currprefs.dfxclickexternal[3], changed_prefs.dfxclickexternal[3]))
    {
	currprefs.dfxclickvolume = changed_prefs.dfxclickvolume;
	for (i = 0; i < 4; i++) {
	    currprefs.dfxclick[i] = changed_prefs.dfxclick[i];
	    strcpy (currprefs.dfxclickexternal[i], changed_prefs.dfxclickexternal[i]);
	}
	driveclick_init ();
    }
}

#endif
