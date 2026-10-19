 /*
  * UAE - The Un*x Amiga Emulator
  *
  * lowlevel device glue
  *
  */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"

#include "blkdev.h"

static struct device_functions *device_func[2];
static int have_ioctl;

/****i* januae-blockdevice/install_driver *************************************
*
*   NAME
*       install_driver() -- Select the compiled host-device backend.
*
*   SYNOPSIS
*       static void install_driver (int flags);
*
*   FUNCTION
*       Bind device_func to the platform-selected SCSI implementation and,
*       where available, an IOCTL implementation.
*
*   INPUTS
*       flags - Unused backend-selection argument.
*
*   RESULT
*       None.
*
*   NOTES
*       The Win32, AmigaOS, Linux IOCTL and libscg definitions are mutually
*       exclusive at build time.
*
****************************************************************************
*
*/

#ifdef WIN32

#include "od-win32/win32.h"

extern struct device_functions devicefunc_win32_aspi;
extern struct device_functions devicefunc_win32_spti;
extern struct device_functions devicefunc_win32_ioctl;

static void install_driver (int flags)
{
    device_func[DF_SCSI] = &devicefunc_win32_aspi;
#ifdef WINDDK
    if (os_winnt && os_winnt_admin) {
	device_func[DF_IOCTL] = &devicefunc_win32_ioctl;
	device_func[DF_SCSI]  = &devicefunc_win32_spti;
    }
    if (currprefs.win32_aspi) {
	device_func[DF_SCSI]  = &devicefunc_win32_aspi;
	device_func[DF_IOCTL] = 0;
    }
#endif
}
#else
# ifdef TARGET_AMIGAOS

extern struct device_functions devicefunc_scsi_amiga;

static void install_driver (int flags)
{
    device_func[DF_SCSI]  = &devicefunc_scsi_amiga;
    device_func[DF_IOCTL] = 0;
}
# else
#   ifdef SCSIEMU_LINUX_IOCTL

extern struct device_functions devicefunc_scsi_linux_ioctl;
    
static void install_driver (int flags)
{
    device_func[DF_SCSI] = &devicefunc_scsi_linux_ioctl;
    device_func[DF_IOCTL] = 0;
}

#   else

extern struct device_functions devicefunc_scsi_libscg;

static void install_driver (int flags)
{
    device_func[DF_SCSI]  = &devicefunc_scsi_libscg;
    device_func[DF_IOCTL] = 0;
}
#  endif
# endif
#endif

/****i* januae-blockdevice/sys_command_open ***********************************
*
*   NAME
*       sys_command_open() -- Open a host device unit.
*
*   SYNOPSIS
*       int sys_command_open (int mode, int unitnum);
*
*   FUNCTION
*       Invoke the selected backend opendev callback, using the SCSI backend
*       when mode is DF_SCSI or no IOCTL backend is available.
*
*   INPUTS
*       mode - Backend selection; DF_SCSI requests SCSI.
*       unitnum - Host device unit number.
*
*   RESULT
*       Return value supplied by the selected opendev callback.
*
****************************************************************************
*
*/

int sys_command_open (int mode, int unitnum)
{
    if (mode == DF_SCSI || !have_ioctl)
	return device_func[DF_SCSI]->opendev (unitnum);
    else
	return device_func[DF_IOCTL]->opendev (unitnum);
}

/****i* januae-blockdevice/sys_command_close **********************************
*
*   NAME
*       sys_command_close() -- Close a host device unit.
*
*   SYNOPSIS
*       void sys_command_close (int mode, int unitnum);
*
*   FUNCTION
*       Dispatch to the selected backend closedev callback.
*
*   INPUTS
*       mode - Backend selection.
*       unitnum - Host device unit number.
*
*   RESULT
*       None.
*
****************************************************************************
*
*/

void sys_command_close (int mode, int unitnum)
{
    if (mode == DF_SCSI || !have_ioctl)
	device_func[DF_SCSI]->closedev (unitnum);
    else
	device_func[DF_IOCTL]->closedev (unitnum);
}

/****i* januae-blockdevice/sys_command_open_thread ****************************
*
*   NAME
*       sys_command_open_thread() -- Initialize per-thread device state when supported.
*
*   SYNOPSIS
*       int sys_command_open_thread (int mode, int unitnum);
*
*   FUNCTION
*       Call the selected backend opendevthread callback when the callback is
*       installed.
*
*   INPUTS
*       mode - Backend selection.
*       unitnum - Host device unit number.
*
*   RESULT
*       Backend callback result, or zero when no per-thread open callback
*       exists.
*
****************************************************************************
*
*/

int sys_command_open_thread (int mode, int unitnum)
{
    int result = 0;

    if (mode == DF_SCSI || !have_ioctl) {
	if (device_func[DF_SCSI]->opendevthread)
	    result = device_func[DF_SCSI]->opendevthread (unitnum);
    } else {
	if (device_func[DF_IOCTL]->opendevthread)
	    result= device_func[DF_IOCTL]->opendevthread (unitnum);
    }
    return result;
}

/****i* januae-blockdevice/sys_command_close_thread ***************************
*
*   NAME
*       sys_command_close_thread() -- Release optional per-thread device state.
*
*   SYNOPSIS
*       void sys_command_close_thread (int mode, int unitnum);
*
*   FUNCTION
*       Call the selected backend closedevthread callback if available.
*
*   INPUTS
*       mode - Backend selection.
*       unitnum - Host device unit number.
*
*   RESULT
*       None.
*
****************************************************************************
*
*/

void sys_command_close_thread (int mode, int unitnum)
{
    if (mode == DF_SCSI || !have_ioctl) {
	if (device_func[DF_SCSI]->closedevthread)
	    device_func[DF_SCSI]->closedevthread (unitnum);
    } else {
	if (device_func[DF_IOCTL]->closedevthread)
	    device_func[DF_IOCTL]->closedevthread (unitnum);
    }
}

/****i* januae-blockdevice/device_func_init ***********************************
*
*   NAME
*       device_func_init() -- Initialize host device backends.
*
*   SYNOPSIS
*       int device_func_init (int flags);
*
*   FUNCTION
*       Choose the compiled backends, open their buses, and report which
*       bus-open calls succeeded.
*
*   INPUTS
*       flags - DEVICE_TYPE_SCSI or other device-type selection.
*
*   RESULT
*       On first invocation, a mask of successful DF_SCSI and DF_IOCTL bus
*       openings. On subsequent calls, one.
*
*   NOTES
*       Initialization is cached even when no bus opens successfully; later
*       calls do not retry or repeat the capability mask.
*
****************************************************************************
*
*/

int device_func_init (int flags)
{
    static int initialized;
    int support_scsi = 0, support_ioctl = 0;
    int oflags = (flags & DEVICE_TYPE_SCSI) ? 0 : (1 << INQ_ROMD);

    if (initialized)
	return initialized;
    install_driver (flags);
    if (device_func[DF_IOCTL])
	have_ioctl = 1;
    else
	have_ioctl = 0;
    support_scsi = device_func[DF_SCSI]->openbus (oflags) ? 1 : 0;
    if (have_ioctl)
	support_ioctl = device_func[DF_IOCTL]->openbus (1 << INQ_ROMD) ? 1 : 0;
    initialized = 1;
    write_log ("support_scsi = %d support_ioctl = %d\n", support_scsi, support_ioctl);
    return (support_scsi ? (1 << DF_SCSI) : 0) | (support_ioctl ? (1 << DF_IOCTL) : 0);
}

/****i* januae-blockdevice/audiostatus ****************************************
*
*   NAME
*       audiostatus() -- Read the SCSI CD audio status byte.
*
*   SYNOPSIS
*       static int audiostatus (int unitnum);
*
*   FUNCTION
*       Issue a READ SUB-CHANNEL command through the SCSI backend and inspect
*       the returned status.
*
*   INPUTS
*       unitnum - SCSI device unit.
*
*   RESULT
*       Byte one of the returned buffer, or zero when the command returns no
*       buffer.
*
****************************************************************************
*
*/

static int audiostatus (int unitnum)
{
    static const uae_u8 cmd[10] = {
	0x42, 2, 0x40, 1, 0, 0, 0, DEVICE_SCSI_BUFSIZE >> 8, DEVICE_SCSI_BUFSIZE & 0xff, 0
    };
    const uae_u8 *p = device_func[DF_SCSI]->exec_in (unitnum, cmd, sizeof (cmd), 0);
    if (!p)
	return 0;
    return p[1];
}

/****i* januae-blockdevice/sys_command_pause **********************************
*
*   NAME
*       sys_command_pause() -- Request a CD audio pause or resume.
*
*   SYNOPSIS
*       void sys_command_pause (int mode, int unitnum, int paused);
*
*   FUNCTION
*       Forward the request to the IOCTL backend when selected. The current
*       SCSI path reads the audio status but never submits a pause command.
*
*   INPUTS
*       mode - Backend selection.
*       unitnum - CD device unit.
*       paused - Nonzero to request pause; zero to request resume.
*
*   RESULT
*       None.
*
*   NOTES
*       The SCSI condition requires paused and !paused simultaneously, so no
*       SCSI pause or resume command is emitted.
*
****************************************************************************
*
*/

void sys_command_pause (int mode, int unitnum, int paused)
{
    if (mode == DF_SCSI || !have_ioctl) {
	int as = audiostatus (unitnum);
	if ((paused && as == 0x11) && (!paused && as == 0x12)) {
	    uae_u8 cmd[10] = {
		0x4b, 0, 0, 0, 0, 0, 0, 0, paused ? 0 :1, 0
	    };
	    device_func[DF_SCSI]->exec_out (unitnum, cmd, sizeof (cmd));
	}
	return;
    }
    device_func[DF_IOCTL]->pause (unitnum, paused);
}

/****i* januae-blockdevice/sys_command_stop ***********************************
*
*   NAME
*       sys_command_stop() -- Stop CD audio playback.
*
*   SYNOPSIS
*       void sys_command_stop (int mode, int unitnum);
*
*   FUNCTION
*       Use the selected IOCTL stop callback or send a SCSI STOP PLAY/SCAN
*       command when the SCSI audio status is 0x11.
*
*   INPUTS
*       mode - Backend selection.
*       unitnum - CD device unit.
*
*   RESULT
*       None.
*
*   NOTES
*       The SCSI path does nothing for other audio status values.
*
****************************************************************************
*
*/

void sys_command_stop (int mode, int unitnum)
{
    if (mode == DF_SCSI || !have_ioctl) {
	int as = audiostatus (unitnum);
	if (as == 0x11) {
	    static const uae_u8 cmd[6] = {
		0x4e, 0, 0, 0, 0, 0
	    };
	    device_func[DF_SCSI]->exec_out (unitnum, cmd, sizeof (cmd));
	}
	return;
    }
    device_func[DF_IOCTL]->stop (unitnum);
}

/****i* januae-blockdevice/sys_command_play ***********************************
*
*   NAME
*       sys_command_play() -- Start CD audio playback over an MSF range.
*
*   SYNOPSIS
*       int sys_command_play (int mode, int unitnum, uae_u32 startmsf, uae_u32 endmsf, int scan);
*
*   FUNCTION
*       For SCSI, issue PLAY AUDIO MSF using the two packed MSF addresses.
*       Otherwise call the IOCTL play callback.
*
*   INPUTS
*       mode - Backend selection.
*       unitnum - CD device unit.
*       startmsf - Packed starting minute, second and frame.
*       endmsf - Packed ending minute, second and frame.
*       scan - Scan selection passed to IOCTL.
*
*   RESULT
*       On SCSI, zero when exec_out returns zero and one otherwise; on IOCTL,
*       the callback result.
*
*   NOTES
*       The current SCSI code ignores scan; the alternative scan command is
*       disabled.
*
****************************************************************************
*
*/

int sys_command_play (int mode, int unitnum, uae_u32 startmsf, uae_u32 endmsf, int scan)
{
    if (mode == DF_SCSI || !have_ioctl) {
	uae_u8 cmd[12] = {
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
#if 0
	if (scan) {
	    cmd[0] = 0xba;
	    cmd[1] = scan < 0 ? 0x10 : 0x0;
	    cmd[3] = (uae_u8)(startmsf >> 16);
	    cmd[4] = (uae_u8)(startmsf >> 8);
	    cmd[5] = (uae_u8)(startmsf >> 0);
	    cmd[9] = 0x40;
	} else {
#endif
	    cmd[0] = 0x47;
	    cmd[3] = (uae_u8)(startmsf >> 16);
	    cmd[4] = (uae_u8)(startmsf >> 8);
	    cmd[5] = (uae_u8)(startmsf >> 0);
	    cmd[6] = (uae_u8)(endmsf >> 16);
	    cmd[7] = (uae_u8)(endmsf >> 8);
	    cmd[8] = (uae_u8)(endmsf >> 0);
#if 0
	}
#endif
        return device_func[DF_SCSI]->exec_out (unitnum, cmd, sizeof (cmd)) == 0 ? 0 : 1;
    }
    return device_func[DF_IOCTL]->play (unitnum, startmsf, endmsf, scan);
}

/****i* januae-blockdevice/sys_command_qcode **********************************
*
*   NAME
*       sys_command_qcode() -- Read CD subchannel information.
*
*   SYNOPSIS
*       const uae_u8 *sys_command_qcode (int mode, int unitnum);
*
*   FUNCTION
*       Issue a READ SUB-CHANNEL request through SCSI or use the selected
*       IOCTL qcode callback.
*
*   INPUTS
*       mode - Backend selection.
*       unitnum - CD device unit.
*
*   RESULT
*       Pointer returned by the selected backend, potentially NULL.
*
*   NOTES
*       The backend owns the returned data; this function does not allocate a
*       caller-owned copy.
*
****************************************************************************
*
*/

const uae_u8 *sys_command_qcode (int mode, int unitnum)
{
    if (mode == DF_SCSI || !have_ioctl) {
	static const uae_u8 cmd[10] = {
	    0x42, 2, 0x40, 1, 0, 0, 0, DEVICE_SCSI_BUFSIZE >> 8, DEVICE_SCSI_BUFSIZE & 0xff, 0
	};
	return device_func[DF_SCSI]->exec_in (unitnum, cmd, sizeof (cmd), 0);
    }
    return device_func[DF_IOCTL]->qcode (unitnum);
};

/****i* januae-blockdevice/sys_command_toc ************************************
*
*   NAME
*       sys_command_toc() -- Read a CD table of contents.
*
*   SYNOPSIS
*       const uae_u8 *sys_command_toc (int mode, int unitnum);
*
*   FUNCTION
*       Issue READ TOC through SCSI or delegate to the selected IOCTL toc
*       callback.
*
*   INPUTS
*       mode - Backend selection.
*       unitnum - CD device unit.
*
*   RESULT
*       Pointer returned by the selected backend, potentially NULL.
*
*   NOTES
*       The backend owns the returned data; this function does not allocate a
*       caller-owned copy.
*
****************************************************************************
*
*/

const uae_u8 *sys_command_toc (int mode, int unitnum)
{
    if (mode == DF_SCSI || !have_ioctl) {
	static const uae_u8 cmd[10] = {
	    0x43, 0, 2, 0, 0, 0, 1, DEVICE_SCSI_BUFSIZE >> 8, DEVICE_SCSI_BUFSIZE & 0xFF, 0
	};
        return device_func[DF_SCSI]->exec_in (unitnum, cmd, sizeof(cmd), 0);
    }
    return device_func[DF_IOCTL]->toc (unitnum);
}

/****i* januae-blockdevice/sys_command_read ***********************************
*
*   NAME
*       sys_command_read() -- Read one CD sector.
*
*   SYNOPSIS
*       const uae_u8 *sys_command_read (int mode, int unitnum, int offset);
*
*   FUNCTION
*       Issue READ CD for one logical block with the SCSI backend, or
*       delegate to the IOCTL read callback.
*
*   INPUTS
*       mode - Backend selection.
*       unitnum - CD device unit.
*       offset - Logical sector address passed to the backend.
*
*   RESULT
*       Pointer returned by the selected backend, potentially NULL.
*
*   NOTES
*       The returned buffer is backend-owned. The SCSI command encodes only
*       the low 24 bits of offset.
*
****************************************************************************
*
*/

const uae_u8 *sys_command_read (int mode, int unitnum, int offset)
{
    if (mode == DF_SCSI || !have_ioctl) {
	uae_u8 cmd[12] = {
	    0xbe, 0, 0, 0, 0, 0, 0, 0, 1, 0x10, 0, 0
	};
	cmd[3] = (uae_u8)(offset >> 16);
	cmd[4] = (uae_u8)(offset >> 8);
	cmd[5] = (uae_u8)(offset >> 0);
        return device_func[DF_SCSI]->exec_in (unitnum, cmd, sizeof (cmd), 0);
    }
    return device_func[DF_IOCTL]->read (unitnum, offset);
}

/****i* januae-blockdevice/sys_command_info ***********************************
*
*   NAME
*       sys_command_info() -- Query information about a host device.
*
*   SYNOPSIS
*       struct device_info *sys_command_info (int mode, int unitnum, struct device_info *di);
*
*   FUNCTION
*       Pass the supplied device_info structure to the selected backend info
*       callback.
*
*   INPUTS
*       mode - Backend selection.
*       unitnum - Host device unit.
*       di - Device information storage supplied by the caller.
*
*   RESULT
*       Pointer returned by the selected backend.
*
****************************************************************************
*
*/

struct device_info *sys_command_info (int mode, int unitnum, struct device_info *di)
{
    if (mode == DF_SCSI || !have_ioctl)
	return device_func[DF_SCSI]->info (unitnum, di);
    else
	return device_func[DF_IOCTL]->info (unitnum, di);
}

#define MODE_SELECT_6 0x15
#define MODE_SENSE_6 0x1a
#define MODE_SELECT_10 0x55
#define MODE_SENSE_10 0x5a

/****i* januae-blockdevice/scsi_atapi_fixup_pre *******************************
*
*   NAME
*       scsi_atapi_fixup_pre() -- Convert six-byte MODE commands for ATAPI.
*
*   SYNOPSIS
*       void scsi_atapi_fixup_pre (uae_u8 *scsi_cmd, int *len, uae_u8 **datap, unsigned int *datalenp, int *parm);
*
*   FUNCTION
*       Leave unrelated commands unchanged. Convert MODE SELECT(6) or MODE
*       SENSE(6) to the corresponding ten-byte command and allocate a
*       replacement transfer buffer.
*
*   INPUTS
*       scsi_cmd - Writable command buffer with capacity for a ten-byte
*       command.
*       len - In/out command length.
*       datap - In/out data buffer pointer.
*       datalenp - In/out transfer length.
*       parm - Output conversion indicator; zero if no conversion occurs.
*
*   RESULT
*       None.
*
*   NOTES
*       The caller must free a replacement buffer after dispatch. MODE SELECT
*       conversion reads the first four input bytes without validating the
*       input length.
*
****************************************************************************
*
*/

void scsi_atapi_fixup_pre (uae_u8 *scsi_cmd, int *len, uae_u8 **datap, unsigned int *datalenp, int *parm)
{
    uae_u8 cmd, *p, *data = *datap;
    unsigned int l, datalen = *datalenp;

    *parm = 0;
    cmd = scsi_cmd[0];
    if (cmd != MODE_SELECT_6 && cmd != MODE_SENSE_6)
	return;
    l = scsi_cmd[4];
    if (l > 4)
	l += 4;
    scsi_cmd[7] = l >> 8;
    scsi_cmd[8] = l;
    if (cmd == MODE_SELECT_6) {
	scsi_cmd[0] = MODE_SELECT_10;
	scsi_cmd[9] = scsi_cmd[5];
	scsi_cmd[2] = scsi_cmd[3] = scsi_cmd[4] = scsi_cmd[5] = scsi_cmd[6] = 0;
	*len = 10;
	p = xmalloc (8 + datalen + 4);
	if (datalen > 4)
	    memcpy (p + 8, data + 4, datalen - 4);
	p[0] = 0;
	p[1] = data[0];
	p[2] = data[1];
	p[3] = data[2];
	p[4] = p[5] = p[6] = 0;
	p[7] = data[3];
	if (l > 8)
	    datalen += 4;
	*parm = MODE_SELECT_10;
	*datap = p;
    } else {
	scsi_cmd[0] = MODE_SENSE_10;
	scsi_cmd[9] = scsi_cmd[5];
	scsi_cmd[3] = scsi_cmd[4] = scsi_cmd[5] = scsi_cmd[6] = 0;
	if (l > 8)
	    datalen += 4;
	*datap = xmalloc (datalen);
	*len = 10;
	*parm = MODE_SENSE_10;
    }
    *datalenp = datalen;
}

/****i* januae-blockdevice/scsi_atapi_fixup_post ******************************
*
*   NAME
*       scsi_atapi_fixup_post() -- Restore MODE SENSE results after ATAPI dispatch.
*
*   SYNOPSIS
*       void scsi_atapi_fixup_post (uae_u8 *scsi_cmd, int len, uae_u8 *olddata, uae_u8 *data, unsigned int *datalenp, int parm);
*
*   FUNCTION
*       For a MODE SENSE(10) conversion with nonempty data, copy the returned
*       header and mode pages into the original six-byte-layout buffer and
*       reduce the reported transfer length.
*
*   INPUTS
*       scsi_cmd - Unused command buffer.
*       len - Unused command length.
*       olddata - Original transfer buffer for the converted result.
*       data - Returned ATAPI transfer buffer.
*       datalenp - In/out byte count.
*       parm - Conversion indicator from scsi_atapi_fixup_pre().
*
*   RESULT
*       None.
*
*   NOTES
*       The copy assumes a complete converted header. No conversion is
*       applied for other indicators or an empty returned buffer.
*
****************************************************************************
*
*/

void scsi_atapi_fixup_post (uae_u8 *scsi_cmd, int len, uae_u8 *olddata, uae_u8 *data, unsigned int *datalenp, int parm)
{
    unsigned int datalen = *datalenp;
    if (!data || !datalen)
	return;
    if (parm == MODE_SENSE_10) {
	olddata[0] = data[1];
	olddata[1] = data[2];
	olddata[2] = data[3];
	olddata[3] = data[7];
        datalen -= 4;
	if (datalen > 4)
	    memcpy (olddata + 4, data + 8, datalen - 4);
	*datalenp = datalen;
    }
}

/****i* januae-blockdevice/scsi_atapi_fixup_inquiry ***************************
*
*   NAME
*       scsi_atapi_fixup_inquiry() -- Adjust ATAPI CD-ROM inquiry version reporting.
*
*   SYNOPSIS
*       static void scsi_atapi_fixup_inquiry (uaecptr req);
*
*   FUNCTION
*       For an INQUIRY response describing a CD-ROM with zero ANSI version,
*       set the response ANSI version bits to two.
*
*   INPUTS
*       req - Guest-memory address of a direct SCSI request.
*
*   RESULT
*       None.
*
*   NOTES
*       The adjustment requires a nonzero guest data address and a reported
*       response length greater than two.
*
****************************************************************************
*
*/

static void scsi_atapi_fixup_inquiry (uaecptr req)
{
    uaecptr scsi_data = get_long (req + 0);
    uae_u32 scsi_len = get_long (req + 4);
    uaecptr scsi_cmd = get_long (req + 12);
    uae_u8 cmd;

    cmd = get_byte (scsi_cmd);
    /* CDROM INQUIRY: most Amiga programs expect ANSI version == 2
     * (ATAPI normally responds with zero)
     */
    if (cmd == 0x12 && scsi_len > 2 && scsi_data) {
	uae_u8 per = get_byte (scsi_data + 0);
	uae_u8 b = get_byte (scsi_data + 2);
	/* CDROM and ANSI version == 0 ? */
	if ((per & 31) == 5 && (b & 7) == 0) {
	    b |= 2;
	    put_byte (scsi_data + 2, b);
	}
    }
}

/****i* januae-blockdevice/sys_command_scsi_direct ****************************
*
*   NAME
*       sys_command_scsi_direct() -- Submit a direct SCSI command.
*
*   SYNOPSIS
*       int sys_command_scsi_direct (int unitnum, uaecptr request);
*
*   FUNCTION
*       Call the SCSI backend exec_direct callback. For a successful ATAPI
*       command, apply the CD-ROM inquiry adjustment to the guest response.
*
*   INPUTS
*       unitnum - SCSI device unit.
*       request - Guest-memory address of the direct SCSI request.
*
*   RESULT
*       Result returned by the exec_direct callback.
*
****************************************************************************
*
*/

int sys_command_scsi_direct (int unitnum, uaecptr request)
{
    int ret = device_func[DF_SCSI]->exec_direct (unitnum, request);
    if (!ret && device_func[DF_SCSI]->isatapi(unitnum))
        scsi_atapi_fixup_inquiry (request);
    return ret;
}

/****i* januae-blockdevice/scsi_log_before ************************************
*
*   NAME
*       scsi_log_before() -- Log an outgoing SCSI command.
*
*   SYNOPSIS
*       void scsi_log_before (const uae_u8 *cdb, int cdblen, const uae_u8 *data, int datalen);
*
*   FUNCTION
*       Write the command bytes to the emulator log and, if data is supplied,
*       log its length and up to 100 outgoing bytes.
*
*   INPUTS
*       cdb - Command descriptor block.
*       cdblen - Command length in bytes.
*       data - Optional outgoing data.
*       datalen - Outgoing data length.
*
*   RESULT
*       None.
*
****************************************************************************
*
*/

void scsi_log_before (const uae_u8 *cdb, int cdblen, const uae_u8 *data, int datalen)
{
    int i;
    for (i = 0; i < cdblen; i++) {
        write_log("%s%02.2X", i > 0 ? "." : "", cdb[i]);
    }
    write_log("\n");
    if (data) {
	write_log ("DATAOUT: %d\n", datalen);
        for (i = 0; i < datalen && i < 100; i++)
	    write_log("%s%02.2X", i > 0 ? "." : "", data[i]);
        if (datalen > 0)
	    write_log("\n");
    }
}

/****i* januae-blockdevice/scsi_log_after *************************************
*
*   NAME
*       scsi_log_after() -- Log incoming SCSI data and sense bytes.
*
*   SYNOPSIS
*       void scsi_log_after (const uae_u8 *data, int datalen, const uae_u8 *sense, int senselen);
*
*   FUNCTION
*       Write the incoming data length and up to 100 bytes when data is
*       present; log up to 32 sense bytes when the sense length is positive.
*
*   INPUTS
*       data - Optional returned data.
*       datalen - Returned data length.
*       sense - Sense data buffer if senselen is positive.
*       senselen - Sense data length.
*
*   RESULT
*       None.
*
****************************************************************************
*
*/

void scsi_log_after (const uae_u8 *data, int datalen, const uae_u8 *sense, int senselen)
{
    int i;
    if (data) {
	write_log ("DATAIN: %d\n", datalen);
	for (i = 0; i < datalen && i < 100; i++)
	    write_log("%s%02.2X", i > 0 ? "." : "", data[i]);
	if (datalen > 0)
	    write_log("\n");
    }
    if (senselen > 0) {
        write_log("SENSE: ");
        for (i = 0; i < senselen && i < 32; i++) {
	    write_log("%s%02.2X", i > 0 ? "." : "", sense[i]);
	}
	write_log("\n");
    }
}
