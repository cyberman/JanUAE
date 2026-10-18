
 /*
  * UAE - The Un*x Amiga Emulator
  *
  * scsi.device emulation
  *
  * Copyright 1995 Bernd Schmidt
  * Copyright 1999 Patrick Ohly
  * Copyright 2001 Brian King
  * Copyright 2002 Toni Wilen
  *
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "uae.h"
#include "threaddep/thread.h"
#include "options.h"
#include "memory.h"
#include "custom.h"
#include "events.h"
#include "newcpu.h"
#include "autoconf.h"
#include "traps.h"
#include "execlib.h"
#include "native2amiga.h"
#include "blkdev.h"
#include "scsidev.h"
#include "sleep.h"

#define CDDEV_COMMANDS

#define UAEDEV_SCSI "uaescsi.device"
#define UAEDEV_SCSI_ID 1
#define UAEDEV_DISK "uaedisk.device"
#define UAEDEV_DISK_ID 2

#define MAX_ASYNC_REQUESTS 20
#define MAX_OPEN_DEVICES 20

#define CMD_INVALID	0
#define CMD_RESET	1
#define CMD_READ	2
#define CMD_WRITE	3
#define CMD_UPDATE	4
#define CMD_CLEAR	5
#define CMD_STOP	6
#define CMD_START	7
#define CMD_FLUSH	8
#define CMD_MOTOR	9
#define CMD_SEEK	10
#define CMD_FORMAT	11
#define CMD_REMOVE	12
#define CMD_CHANGENUM	13
#define CMD_CHANGESTATE	14
#define CMD_PROTSTATUS	15
#define CMD_GETDRIVETYPE 18
#define CMD_GETNUMTRACKS 19
#define CMD_ADDCHANGEINT 20
#define CMD_REMCHANGEINT 21
#define CMD_GETGEOMETRY	22

#define CD_INFO		32
#define CD_CONFIG	33
#define CD_TOCMSF	34
#define CD_TOCLSN	35
#define CD_READXL	36
#define CD_PLAYTRACK	37
#define CD_PLAYMSF	38
#define CD_PLAYLSN	39
#define CD_PAUSE	40
#define CD_SEARCH	41
#define CD_QCODEMSF	42
#define CD_QCODELSN	43
#define CD_ATTENUATE	44
#define CD_ADDFRAMEINT	45
#define CD_REMFRAMEINT	46

#define ASYNC_REQUEST_NONE 0
#define ASYNC_REQUEST_TEMP 1
#define ASYNC_REQUEST_CHANGEINT 10
#define ASYNC_REQUEST_FRAMEINT 11
#define ASYNC_REQUEST_PLAY 12
#define ASYNC_REQUEST_READXL 13
#define ASYNC_REQUEST_FRAMECALL 14

struct devstruct {
    int unitnum, aunit;
    int opencnt;
    int changenum;
    int allow_scsi;
    int allow_ioctl;
    int drivetype;
    int iscd;
    volatile uaecptr d_request[MAX_ASYNC_REQUESTS];
    volatile int d_request_type[MAX_ASYNC_REQUESTS];
    volatile uae_u32 d_request_data[MAX_ASYNC_REQUESTS];
    struct device_info di;

    smp_comm_pipe requests;
    uae_thread_id tid;
    int thread_running;
    uae_sem_t sync_sem;
};

struct priv_devstruct {
    int inuse;
    int unit;
    int mode;
    int scsi;
    int ioctl;
    int noscsi;
    int type;
    int flags; /* OpenDevice() */
};

static struct devstruct devst[MAX_TOTAL_DEVICES];
static struct priv_devstruct pdevst[MAX_OPEN_DEVICES];

static uae_sem_t change_sem;

/****i* januae-scsiemul/devinfo ********************************************
*
*   NAME
*       devinfo() -- Query the host device descriptor.
*
*   SYNOPSIS
*       static struct device_info *devinfo (int mode, int unitnum, struct
*       device_info *di);
*
*   FUNCTION
*       Forward the selected access mode and unit number to the host
*       command-information interface.
*
*   INPUTS
*       mode - DF_SCSI or DF_IOCTL access mode; unitnum - Host device index;
*       di - Descriptor to populate.
*
*   RESULT
*       The descriptor pointer returned by sys_command_info.
*
****************************************************************************
*
*/

static struct device_info *devinfo (int mode, int unitnum, struct device_info *di)
{
    return sys_command_info (mode, unitnum, di);
}

/****i* januae-scsiemul/io_log *********************************************
*
*   NAME
*       io_log() -- Trace a guest device I/O request.
*
*   SYNOPSIS
*       static void io_log (const char *msg, uaecptr request);
*
*   FUNCTION
*       When SCSI logging is enabled, read the request command, data
*       address, lengths, offset and result fields from guest memory and
*       write one diagnostic record.
*
*   INPUTS
*       msg - Diagnostic operation label; request - Guest address of an I/O
*       request.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void io_log (const char *msg, uaecptr request)
{
    if (log_scsi)
	write_log ("%s: %08X %d %08.8X %d %d io_actual=%d io_error=%d\n",
	    msg, request,get_word(request + 28),get_long(request + 40),get_long(request + 36),get_long(request + 44),
	    get_long (request + 32), get_byte (request + 31));
}

/****i* januae-scsiemul/memcpyha *******************************************
*
*   NAME
*       memcpyha() -- Copy host bytes into guest memory.
*
*   SYNOPSIS
*       STATIC_INLINE void memcpyha (uae_u32 dst, const uae_u8 *src, int
*       size);
*
*   FUNCTION
*       Write each input byte through the emulated memory access function
*       while advancing the destination address.
*
*   INPUTS
*       dst - Guest destination address; src - Host source buffer; size -
*       Number of bytes to transfer.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

STATIC_INLINE void memcpyha (uae_u32 dst, const uae_u8 *src, int size)
{
    while (size--)
	put_byte (dst++, *src++);
}

/****i* januae-scsiemul/getdevstruct ***************************************
*
*   NAME
*       getdevstruct() -- Look up an emulated device unit.
*
*   SYNOPSIS
*       STATIC_INLINE struct devstruct *getdevstruct (int unit);
*
*   FUNCTION
*       Search the device table for a nonnegative logical unit number
*       matching the registered guest unit.
*
*   INPUTS
*       unit - Guest-visible unit number.
*
*   RESULT
*       A pointer to the matching device record, or NULL if none matches.
*
****************************************************************************
*
*/

STATIC_INLINE struct devstruct *getdevstruct (int unit)
{
    int i;
    for (i = 0; i < MAX_TOTAL_DEVICES; i++) {
	if (unit >= 0 && devst[i].aunit == unit) return &devst[i];
    }
    return 0;
}

/****i* januae-scsiemul/getpdevstruct **************************************
*
*   NAME
*       getpdevstruct() -- Resolve an opened guest request handle.
*
*   SYNOPSIS
*       static struct priv_devstruct *getpdevstruct (uaecptr request);
*
*   FUNCTION
*       Read the private handle index stored in the guest I/O request and
*       verify that its slot is in range and in use.
*
*   INPUTS
*       request - Guest address of an I/O request with a private handle
*       index at offset 24.
*
*   RESULT
*       The matching open-handle record, or NULL when its index or state is
*       invalid.
*
****************************************************************************
*
*/

static struct priv_devstruct *getpdevstruct (uaecptr request)
{
    int i = get_long (request + 24);
    if (i < 0 || i >= MAX_OPEN_DEVICES || pdevst[i].inuse == 0) {
	write_log ("uaescsi.device: corrupt iorequest %08.8X %d\n", request, i);
	return 0;
    }
    return &pdevst[i];
}

/****i* januae-scsiemul/getdevname *****************************************
*
*   NAME
*       getdevname() -- Select an emulated device name.
*
*   SYNOPSIS
*       static const char *getdevname (int type);
*
*   FUNCTION
*       Map the SCSI and disk device identifiers to their registered
*       device-name strings.
*
*   INPUTS
*       type - UAEDEV_SCSI_ID or UAEDEV_DISK_ID.
*
*   RESULT
*       The corresponding static name string for a recognized type.
*
*   NOTES
*       An unrecognized device type terminates execution through abort.
*
****************************************************************************
*
*/

static const char *getdevname (int type)
{
    switch (type) {
	case UAEDEV_SCSI_ID:
	return UAEDEV_SCSI;
	case UAEDEV_DISK_ID:
	return UAEDEV_DISK;
	default:
	abort ();
    }
}

static void *dev_thread (void *devs);
/****i* januae-scsiemul/start_thread ***************************************
*
*   NAME
*       start_thread() -- Start a device request worker.
*
*   SYNOPSIS
*       static int start_thread (struct devstruct *dev);
*
*   FUNCTION
*       Leave an already running worker unchanged; otherwise initialize its
*       message pipe and startup semaphore, start the worker and wait for
*       its startup acknowledgement.
*
*   INPUTS
*       dev - Device record owning the request pipe, semaphore and worker.
*
*   RESULT
*       One when the worker reports that it is running; otherwise zero.
*
****************************************************************************
*
*/

static int start_thread (struct devstruct *dev)
{
    if (dev->thread_running)
        return 1;
    init_comm_pipe (&dev->requests, 100, 1);
    uae_sem_init (&dev->sync_sem, 0, 0);
    uae_start_thread (dev_thread, dev, &dev->tid);
    uae_sem_wait (&dev->sync_sem);
    return dev->thread_running;
}

/****i* januae-scsiemul/dev_close_3 ****************************************
*
*   NAME
*       dev_close_3() -- Close the last reference to a device unit.
*
*   SYNOPSIS
*       static void dev_close_3 (struct devstruct *dev, struct
*       priv_devstruct *pdev);
*
*   FUNCTION
*       Decrement the unit open count; when it reaches zero, close the
*       selected host transports, release the private handle slot and queue
*       a worker shutdown request.
*
*   INPUTS
*       dev - Device record; pdev - Associated private open-handle record.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void dev_close_3 (struct devstruct *dev, struct priv_devstruct *pdev)
{
    if (!dev->opencnt) return;
    dev->opencnt--;
    if (!dev->opencnt) {
	if (pdev->scsi)
	    sys_command_close (DF_SCSI, dev->unitnum);
	if (pdev->ioctl)
	    sys_command_close (DF_IOCTL, dev->unitnum);
	pdev->inuse = 0;
        write_comm_pipe_u32 (&dev->requests, 0, 1);
    }
}

/****i* januae-scsiemul/dev_close_2 ****************************************
*
*   NAME
*       dev_close_2() -- Process a guest device close request.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 dev_close_2 (TrapContext *context);
*
*   FUNCTION
*       Resolve the private handle from guest register A1, close the unit,
*       clear the handle field and decrement the guest device-library open
*       count.
*
*   INPUTS
*       context - Trap context containing the I/O request in A1 and device
*       base in A6.
*
*   RESULT
*       Zero on every current path.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 dev_close_2 (TrapContext *context)
{
    uae_u32 request = m68k_areg (&context->regs, 1);
    struct priv_devstruct *pdev = getpdevstruct (request);
    struct devstruct *dev;

    if (!pdev)
	return 0;
    dev = getdevstruct (pdev->unit);
    if (log_scsi)
	write_log ("%s:%d close, req=%08.8X\n", getdevname (pdev->type), pdev->unit, request);
    if (!dev)
	return 0;
    dev_close_3 (dev, pdev);
    put_long (request + 24, 0);
    put_word (m68k_areg (&context->regs, 6) + 32, get_word (m68k_areg (&context->regs, 6) + 32) - 1);
    return 0;
}

/****i* januae-scsiemul/dev_close ******************************************
*
*   NAME
*       dev_close() -- Close a guest SCSI device.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 dev_close (TrapContext *context);
*
*   FUNCTION
*       Forward the SCSI device close trap to the common guest close
*       handler.
*
*   INPUTS
*       context - Device trap context.
*
*   RESULT
*       The status returned by dev_close_2.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 dev_close (TrapContext *context)
{
    return dev_close_2 (context);
}
/****i* januae-scsiemul/diskdev_close **************************************
*
*   NAME
*       diskdev_close() -- Close a guest disk device.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 diskdev_close (TrapContext *context);
*
*   FUNCTION
*       Forward the disk device close trap to the common guest close
*       handler.
*
*   INPUTS
*       context - Device trap context.
*
*   RESULT
*       The status returned by dev_close_2.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 diskdev_close (TrapContext *context)
{
    return dev_close_2 (context);
}

/****i* januae-scsiemul/openfail *******************************************
*
*   NAME
*       openfail() -- Report a failed guest device open.
*
*   SYNOPSIS
*       static int openfail (uaecptr ioreq, int error);
*
*   FUNCTION
*       Clear the guest request device slot by writing minus one at offset
*       20 and record the selected I/O error byte at offset 31.
*
*   INPUTS
*       ioreq - Guest address of the I/O request; error - Device error code.
*
*   RESULT
*       Minus one, represented in the function return type.
*
****************************************************************************
*
*/

static int openfail (uaecptr ioreq, int error)
{
    put_long (ioreq + 20, -1);
    put_byte (ioreq + 31, error);
    return (uae_u32)-1;
}

/****i* januae-scsiemul/dev_open_2 *****************************************
*
*   NAME
*       dev_open_2() -- Open a guest SCSI or disk device unit.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 dev_open_2 (TrapContext *context, int
*       type);
*
*   FUNCTION
*       Resolve the requested unit, select the appropriate host SCSI or
*       ioctl transport, associate an open-handle record, start the unit
*       worker and update guest request and device-library fields.
*
*   INPUTS
*       context - Trap registers holding guest request, unit, flags and
*       device base; type - Emulated SCSI or disk device selector.
*
*   RESULT
*       Zero when the guest request is opened; minus one through openfail
*       when a checked open condition fails.
*
*   NOTES
*       Subsequent opens of the same unit reuse its existing private handle
*       record.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 dev_open_2 (TrapContext *context, int type)
{
    uaecptr ioreq = m68k_areg (&context->regs, 1);
    uae_u32 unit = m68k_dreg (&context->regs, 0);
    uae_u32 flags = m68k_dreg (&context->regs, 1);
    struct devstruct *dev = getdevstruct (unit);
    struct priv_devstruct *pdev = 0;
    int i;

    if (log_scsi)
	write_log ("opening %s:%d ioreq=%08.8X\n", getdevname (type), unit, ioreq);
    if (!dev)
	return openfail (ioreq, 32); /* badunitnum */
    if (!dev->opencnt) {
        for (i = 0; i < MAX_OPEN_DEVICES; i++) {
	    pdev = &pdevst[i];
	    if (pdev->inuse == 0) break;
	}
	if (type == UAEDEV_SCSI_ID && sys_command_open (DF_SCSI, dev->unitnum)) {
	    pdev->scsi = 1;
	    pdev->mode = DF_SCSI;
	}
	if (type == UAEDEV_DISK_ID && sys_command_open (DF_IOCTL, dev->unitnum)) {
	    pdev->ioctl = 1;
	    pdev->mode = DF_IOCTL;
	}
	if (!pdev->scsi && !pdev->ioctl)
	    return openfail (ioreq, -1);
        pdev->type = type;
        pdev->unit = unit;
	pdev->flags = flags;
	pdev->inuse = 1;
        put_long (ioreq + 24, pdev - pdevst);
	start_thread (dev);
    } else {
        for (i = 0; i < MAX_OPEN_DEVICES; i++) {
	    pdev = &pdevst[i];
	    if (pdev->inuse && pdev->unit == (int)unit) break;
	}
	if (i == MAX_OPEN_DEVICES)
	    return openfail (ioreq, -1);
        put_long (ioreq + 24, pdev - pdevst);
    }
    dev->opencnt++;

    put_word (m68k_areg (&context->regs, 6) + 32, get_word (m68k_areg (&context->regs, 6) + 32) + 1);
    put_byte (ioreq + 31, 0);
    put_byte (ioreq + 8, 7);
    return 0;
}

/****i* januae-scsiemul/dev_open *******************************************
*
*   NAME
*       dev_open() -- Open a guest SCSI unit.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 dev_open (TrapContext *context);
*
*   FUNCTION
*       Dispatch the guest SCSI OpenDevice trap through the shared open
*       routine.
*
*   INPUTS
*       context - Device trap context.
*
*   RESULT
*       The status returned by dev_open_2.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 dev_open (TrapContext *context)
{
    return dev_open_2 (context, UAEDEV_SCSI_ID);
}
/****i* januae-scsiemul/diskdev_open ***************************************
*
*   NAME
*       diskdev_open() -- Open a guest disk unit.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 diskdev_open (TrapContext *context);
*
*   FUNCTION
*       Dispatch the guest disk OpenDevice trap through the shared open
*       routine.
*
*   INPUTS
*       context - Device trap context.
*
*   RESULT
*       The status returned by dev_open_2.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 diskdev_open (TrapContext *context)
{
    return dev_open_2 (context, UAEDEV_DISK_ID);
}

/****i* januae-scsiemul/dev_expunge ****************************************
*
*   NAME
*       dev_expunge() -- Handle a SCSI device expunge request.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 dev_expunge (TrapContext *context);
*
*   FUNCTION
*       Return without removing the emulated device or reclaiming its
*       resident structures.
*
*   INPUTS
*       context - Device trap context; unused.
*
*   RESULT
*       Zero.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 dev_expunge (TrapContext *context)
{
    return 0;
}
/****i* januae-scsiemul/diskdev_expunge ************************************
*
*   NAME
*       diskdev_expunge() -- Handle a disk device expunge request.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 diskdev_expunge (TrapContext *context);
*
*   FUNCTION
*       Return without removing the emulated disk device or reclaiming its
*       resident structures.
*
*   INPUTS
*       context - Device trap context; unused.
*
*   RESULT
*       Zero.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 diskdev_expunge (TrapContext *context)
{
    return 0;
}

/****i* januae-scsiemul/is_async_request ***********************************
*
*   NAME
*       is_async_request() -- Test whether an I/O request is registered.
*
*   SYNOPSIS
*       static int is_async_request (struct devstruct *dev, uaecptr
*       request);
*
*   FUNCTION
*       Search the device asynchronous-request slots for the supplied guest
*       request address.
*
*   INPUTS
*       dev - Device record; request - Guest I/O request address.
*
*   RESULT
*       One when a slot contains the request; otherwise zero.
*
****************************************************************************
*
*/

static int is_async_request (struct devstruct *dev, uaecptr request)
{
    int i = 0;
    while (i < MAX_ASYNC_REQUESTS) {
	if (dev->d_request[i] == request) return 1;
	i++;
    }
    return 0;
}

/****i* januae-scsiemul/scsi_do_disk_change ********************************
*
*   NAME
*       scsi_do_disk_change() -- Publish media-change notifications.
*
*   SYNOPSIS
*       void scsi_do_disk_change (int device_id, int insert);
*
*   FUNCTION
*       Under the shared change semaphore, increment the change counter for
*       matching device identifiers and signal registered change-interrupt
*       callbacks.
*
*   INPUTS
*       device_id - Host device identifier to match; insert - Media-change
*       indication, not consulted by this routine.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void scsi_do_disk_change (int device_id, int insert)
{
    int i, j;

    uae_sem_wait (&change_sem);
    for (i = 0; i < MAX_TOTAL_DEVICES; i++) {
	if (devst[i].di.id == device_id) {
	    devst[i].changenum++;
	    j = 0;
	    while (j < MAX_ASYNC_REQUESTS) {
		if (devst[i].d_request_type[j] == ASYNC_REQUEST_CHANGEINT) {
		    uae_Cause (devst[i].d_request_data[j]);
		}
		j++;
	    }
	}
    }
    uae_sem_post (&change_sem);
}

/****i* januae-scsiemul/add_async_request **********************************
*
*   NAME
*       add_async_request() -- Register an asynchronous I/O request.
*
*   SYNOPSIS
*       static int add_async_request (struct devstruct *dev, uaecptr
*       request, int type, uae_u32 data);
*
*   FUNCTION
*       Update an existing request slot or fill the first empty slot with a
*       request address, request category and associated callback data.
*
*   INPUTS
*       dev - Device record; request - Guest I/O request address; type -
*       Asynchronous request category; data - Associated request data.
*
*   RESULT
*       Zero when updated or inserted; minus one when all request slots are
*       occupied.
*
****************************************************************************
*
*/

static int add_async_request (struct devstruct *dev, uaecptr request, int type, uae_u32 data)
{
    int i;

    if (log_scsi)
	write_log ("async request %p (%d) added\n", request, type);
    i = 0;
    while (i < MAX_ASYNC_REQUESTS) {
	if (dev->d_request[i] == request) {
	    dev->d_request_type[i] = type;
	    dev->d_request_data[i] = data;
	    return 0;
	}
	i++;
    }
    i = 0;
    while (i < MAX_ASYNC_REQUESTS) {
	if (dev->d_request[i] == 0) {
	    dev->d_request[i] = request;
	    dev->d_request_type[i] = type;
	    dev->d_request_data[i] = data;
	    return 0;
	}
	i++;
    }
    return -1;
}

/****i* januae-scsiemul/release_async_request ******************************
*
*   NAME
*       release_async_request() -- Unregister an asynchronous I/O request.
*
*   SYNOPSIS
*       static int release_async_request (struct devstruct *dev, uaecptr
*       request);
*
*   FUNCTION
*       Find the request slot, clear its address, data and category fields,
*       and return the previous category.
*
*   INPUTS
*       dev - Device record; request - Guest I/O request address.
*
*   RESULT
*       The removed request category, or minus one when no slot matches.
*
****************************************************************************
*
*/

static int release_async_request (struct devstruct *dev, uaecptr request)
{
    int i = 0;

    if (log_scsi)
	write_log ("async request %p removed\n", request);
    while (i < MAX_ASYNC_REQUESTS) {
	if (dev->d_request[i] == request) {
	    int type = dev->d_request_type[i];
	    dev->d_request[i] = 0;
	    dev->d_request_data[i] = 0;
	    dev->d_request_type[i] = 0;
	    return type;
	}
	i++;
    }
    return -1;
}

/****i* januae-scsiemul/abort_async ****************************************
*
*   NAME
*       abort_async() -- Remove a pending asynchronous request.
*
*   SYNOPSIS
*       static void abort_async (struct devstruct *dev, uaecptr request, int
*       errcode, int type);
*
*   FUNCTION
*       Wait while a matching request is marked as processing, then remove
*       its registered request slot; optionally log the removal.
*
*   INPUTS
*       dev - Device record; request - Guest I/O request address; errcode -
*       Diagnostic error value; type - Unused request classification.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       This helper only changes the request registry; it does not directly
*       send a completion message.
*
****************************************************************************
*
*/

static void abort_async (struct devstruct *dev, uaecptr request, int errcode, int type)
{
    int i;
    i = 0;
    while (i < MAX_ASYNC_REQUESTS) {
	if (dev->d_request[i] == request && dev->d_request_type[i] == ASYNC_REQUEST_TEMP) {
	    /* ASYNC_REQUEST_TEMP = request is processing */
	    uae_msleep (10);
	    i = 0;
	    continue;
	}
	i++;
    }
    i = release_async_request (dev, request);
    if (i >= 0 && log_scsi)
	write_log ("asyncronous request=%08.8X aborted, error=%d\n", request, errcode);
}

/****i* januae-scsiemul/command_read ***************************************
*
*   NAME
*       command_read() -- Read sectors into guest memory.
*
*   SYNOPSIS
*       static int command_read (int mode, struct devstruct *dev, uaecptr
*       data, int offset, int length, uae_u32 *io_actual);
*
*   FUNCTION
*       Translate the byte offset into device sectors, request each sector
*       through the host command adapter, and copy the requested whole or
*       partial sectors into guest memory.
*
*   INPUTS
*       mode - Selected transport mode; dev - Device record; data - Guest
*       destination address; offset - Byte offset on media; length - Number
*       of requested bytes; io_actual - Output byte count.
*
*   RESULT
*       Zero after copying all requested bytes; error code 20 if the host
*       sector read returns no buffer.
*
*   NOTES
*       The output byte count is initialized to zero and accumulates bytes
*       copied before a failure.
*
****************************************************************************
*
*/

static int command_read (int mode, struct devstruct *dev, uaecptr data, int offset, int length, uae_u32 *io_actual)
{
    const uae_u8 *temp;
    int len, sector;

    int startoffset = offset % dev->di.bytespersector;
    offset -= startoffset;
    sector = offset / dev->di.bytespersector;
    *io_actual = 0;
    while (length > 0) {
	temp = sys_command_read (mode, dev->unitnum, sector);
	if (!temp) return 20;
	if (startoffset > 0) {
	    len = dev->di.bytespersector - startoffset;
	    if (len > length) len = length;
	    memcpyha (data, temp + startoffset, len);
	    length -= len;
	    data += len;
	    startoffset = 0;
	    *io_actual += len;
	} else if (length >= dev->di.bytespersector) {
	    len = dev->di.bytespersector;
	    memcpyha (data, temp, len);
	    length -= len;
	    data += len;
	    *io_actual += len;
	} else {
	    memcpyha (data, temp, length);
	    *io_actual += length;
	    length = 0;
	}
	sector++;
    }
    return 0;
}

/****i* januae-scsiemul/dev_do_io ******************************************
*
*   NAME
*       dev_do_io() -- Execute a guest SCSI or disk I/O command.
*
*   SYNOPSIS
*       static int dev_do_io (struct devstruct *dev, uaecptr request);
*
*   FUNCTION
*       Read command and transfer fields from the guest request, dispatch
*       supported read, status, media-change and direct-SCSI operations,
*       then write actual length and error code back to guest memory.
*
*   INPUTS
*       dev - Device record; request - Guest I/O request address.
*
*   RESULT
*       One for an accepted persistent change-interrupt request; zero for
*       synchronous completion and other paths.
*
*   NOTES
*       Write and format commands report write protection; unsupported
*       commands are assigned the current invalid-command error.
*
****************************************************************************
*
*/

static int dev_do_io (struct devstruct *dev, uaecptr request)
{
    uae_u32 command;
    uae_u32 io_data = get_long (request + 40); // 0x28
    uae_u32 io_length = get_long (request + 36); // 0x24
    uae_u32 io_actual = get_long (request + 32); // 0x20
    uae_u32 io_offset = get_long (request + 44); // 0x2c
    uae_u32 io_error = 0;
    int async = 0;
    struct device_info di;
    struct priv_devstruct *pdev = getpdevstruct (request);

    if (!pdev)
	return 0;
    command = get_word (request+28);

    switch (command)
    {
	case CMD_READ:
	io_error = command_read (pdev->mode, dev, io_data, io_offset, io_length, &io_actual);
	break;
	case CMD_WRITE:
	case CMD_FORMAT:
	io_error = 28; /* writeprotect */
	break;
	case CMD_UPDATE:
	case CMD_CLEAR:
	case CMD_FLUSH:
	case CMD_MOTOR:
	case CMD_SEEK:
	io_actual = 0;
	break;
	case CMD_REMOVE:
	io_actual = 0;
	break;
	case CMD_CHANGENUM:
	io_actual = dev->changenum;
	break;
	case CMD_CHANGESTATE:
	io_actual = devinfo(pdev->mode, dev->unitnum, &di)->media_inserted ? 0 : 1;
	break;
	case CMD_PROTSTATUS:
	io_actual = devinfo(pdev->mode, dev->unitnum, &di)->write_protected ? -1 : 0;
	break;
	case CMD_GETDRIVETYPE:
	io_actual = dev->drivetype;
	break;
	case CMD_GETNUMTRACKS:
	io_actual = dev->di.cylinders;
	break;
	case CMD_ADDCHANGEINT:
	io_error = add_async_request (dev, request, ASYNC_REQUEST_CHANGEINT, io_data);
	if (!io_error)
	    async = 1;
	break;
	case CMD_REMCHANGEINT:
	release_async_request (dev, request);
	break;
        case 28: /* HD_SCSICMD */
        if (dev->allow_scsi && pdev->scsi) {
	    uae_u32 sdd = get_long (request + 40);
	    io_error = sys_command_scsi_direct (dev->unitnum, sdd);
	    if (log_scsi)
		write_log ("scsidev: did io: sdd %p request %p error %d\n", sdd, request, get_byte (request + 31));
	}
	break;
	default:
	io_error = -3;
	break;
    }
    put_long (request + 32, io_actual);
    put_byte (request + 31, io_error);
    io_log ("dev_io",request);
    return async;
}

/****i* januae-scsiemul/dev_can_quick **************************************
*
*   NAME
*       dev_can_quick() -- Identify commands eligible for immediate
*       completion.
*
*   SYNOPSIS
*       static int dev_can_quick (uae_u32 command);
*
*   FUNCTION
*       Check whether the command is among the fixed reset, start, stop and
*       device-status operations marked for IO_QUICK.
*
*   INPUTS
*       command - I/O command identifier.
*
*   RESULT
*       One for a recognized immediate command; otherwise zero.
*
****************************************************************************
*
*/

static int dev_can_quick (uae_u32 command)
{
    switch (command)
    {
	case CMD_RESET:
	case CMD_STOP:
	case CMD_START:
	case CMD_CHANGESTATE:
	case CMD_PROTSTATUS:
	case CMD_GETDRIVETYPE:
	case CMD_GETNUMTRACKS:
	return 1;
    }
    return 0;
}

/****i* januae-scsiemul/dev_canquick ***************************************
*
*   NAME
*       dev_canquick() -- Check a guest request for immediate execution.
*
*   SYNOPSIS
*       static int dev_canquick (struct devstruct *dev, uaecptr request);
*
*   FUNCTION
*       Read the guest command identifier and forward it to the
*       quick-command classifier.
*
*   INPUTS
*       dev - Device record; unused; request - Guest I/O request address.
*
*   RESULT
*       The quick-command classification returned by dev_can_quick.
*
****************************************************************************
*
*/

static int dev_canquick (struct devstruct *dev, uaecptr request)
{
    uae_u32 command = get_word (request + 28);
    return dev_can_quick (command);
}

/****i* januae-scsiemul/dev_beginio ****************************************
*
*   NAME
*       dev_beginio() -- Dispatch a guest BeginIO request.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 dev_beginio (TrapContext *context);
*
*   FUNCTION
*       Read the request from guest register A1, perform eligible IO_QUICK
*       commands inline, or clear the quick flag and queue other commands on
*       the device worker pipe.
*
*   INPUTS
*       context - Trap context with the I/O request address in A1.
*
*   RESULT
*       The guest I/O error byte for immediate or invalid requests; zero
*       after an asynchronous request is queued.
*
*   NOTES
*       Worker completion is delivered by a reply message unless the command
*       registers an ongoing asynchronous operation.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 dev_beginio (TrapContext *context)
{
    uae_u32 request = m68k_areg (&context->regs, 1);
    uae_u8 flags = get_byte (request + 30);
    int command = get_word (request + 28);
    struct priv_devstruct *pdev = getpdevstruct (request);
    struct devstruct *dev = getdevstruct (pdev->unit);

    put_byte (request+8, NT_MESSAGE);
    if (!dev || !pdev) {
	put_byte (request + 31, 32);
	return get_byte (request + 31);
    }
    put_byte (request+31, 0);
    if ((flags & 1) && dev_canquick (dev, request)) {
	if (dev_do_io (dev, request))
	    write_log ("device %s command %d bug with IO_QUICK\n", getdevname (pdev->type), command);
	return get_byte (request + 31);
    } else {
        add_async_request (dev, request, ASYNC_REQUEST_TEMP, 0);
        put_byte (request+30, get_byte (request + 30) & ~1);
	write_comm_pipe_u32 (&dev->requests, request, 1);
	return 0;
    }
}

/****i* januae-scsiemul/dev_thread *****************************************
*
*   NAME
*       dev_thread() -- Process device requests in a worker thread.
*
*   SYNOPSIS
*       static void *dev_thread (void *devs);
*
*   FUNCTION
*       Open thread-local SCSI and ioctl adapters, acknowledge startup,
*       consume queued guest requests, execute their commands and reply to
*       completed requests until a zero-address shutdown token arrives.
*
*   INPUTS
*       devs - Pointer to the owning device record.
*
*   RESULT
*       Null pointer, represented as zero on worker termination.
*
*   NOTES
*       The shared change semaphore protects each queued request execution
*       path.
*
****************************************************************************
*
*/

static void *dev_thread (void *devs)
{
    struct devstruct *dev = devs;

    uae_set_thread_priority (2);
    dev->thread_running = 1;

    sys_command_open_thread (DF_SCSI, dev->unitnum);
    sys_command_open_thread (DF_IOCTL, dev->unitnum);

    uae_sem_post (&dev->sync_sem);
    for (;;) {
	uaecptr request = (uaecptr)read_comm_pipe_u32_blocking (&dev->requests);
	uae_sem_wait (&change_sem);
        if (!request) {
	    dev->thread_running = 0;
	    uae_sem_post (&dev->sync_sem);
	    uae_sem_post (&change_sem);
	    break;
	} else if (dev_do_io (dev, request) == 0) {
            put_byte (request + 30, get_byte (request + 30) & ~1);
	    release_async_request (dev, request);
            uae_ReplyMsg (request);
	} else {
	    if (log_scsi)
		write_log ("async request %08.8X\n", request);
	}
	uae_sem_post (&change_sem);
    }

    sys_command_close_thread (DF_SCSI, dev->unitnum);
    sys_command_close_thread (DF_IOCTL, dev->unitnum);

   return 0;
}

/****i* januae-scsiemul/dev_init_2 *****************************************
*
*   NAME
*       dev_init_2() -- Initialize a guest device-library base.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 dev_init_2 (TrapContext *context, int
*       type);
*
*   FUNCTION
*       Read the device base address from guest register D0 and optionally
*       log the selected device type.
*
*   INPUTS
*       context - Trap context containing D0; type - SCSI or disk device
*       selector.
*
*   RESULT
*       The original device base address from D0.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 dev_init_2 (TrapContext *context, int type)
{
    uae_u32 base = m68k_dreg (&context->regs,0);
    if (log_scsi)
	write_log ("%s init\n", getdevname (type));
    return base;
}

/****i* januae-scsiemul/dev_init *******************************************
*
*   NAME
*       dev_init() -- Initialize the SCSI device library.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 dev_init (TrapContext *context);
*
*   FUNCTION
*       Forward the SCSI device initialization trap to the shared
*       initialization routine.
*
*   INPUTS
*       context - Device initialization trap context.
*
*   RESULT
*       The guest device base address returned by dev_init_2.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 dev_init (TrapContext *context)
{
    return dev_init_2 (context, UAEDEV_SCSI_ID);
}
/****i* januae-scsiemul/diskdev_init ***************************************
*
*   NAME
*       diskdev_init() -- Initialize the disk device library.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 diskdev_init (TrapContext *context);
*
*   FUNCTION
*       Forward the disk device initialization trap to the shared
*       initialization routine.
*
*   INPUTS
*       context - Device initialization trap context.
*
*   RESULT
*       The guest device base address returned by dev_init_2.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 diskdev_init (TrapContext *context)
{
    return dev_init_2 (context, UAEDEV_DISK_ID);
}

/****i* januae-scsiemul/dev_abortio ****************************************
*
*   NAME
*       dev_abortio() -- Handle a guest AbortIO request.
*
*   SYNOPSIS
*       static uae_u32 REGPARAM2 dev_abortio (TrapContext *context);
*
*   FUNCTION
*       Resolve the guest request and unit, place the aborted I/O error
*       value in guest memory and remove the registered asynchronous
*       request.
*
*   INPUTS
*       context - Trap context containing the request address in A1.
*
*   RESULT
*       Zero for a resolved request; the guest I/O error byte when its
*       handle or unit is invalid.
*
****************************************************************************
*
*/

static uae_u32 REGPARAM2 dev_abortio (TrapContext *context)
{
    uae_u32 request = m68k_areg (&context->regs, 1);
    struct priv_devstruct *pdev = getpdevstruct (request);
    struct devstruct *dev;

    if (!pdev) {
	put_byte (request + 31, 32);
	return get_byte (request + 31);
    }
    dev = getdevstruct (pdev->unit);
    if (!dev) {
	put_byte (request + 31, 32);
	return get_byte (request + 31);
    }
    put_byte (request + 31, -2);
    if (log_scsi)
	write_log ("abortio %s unit=%d, request=%08.8X\n", getdevname (pdev->type), pdev->unit, request);
    abort_async (dev, request, -2, 0);
    return 0;
}

#define BTL2UNIT(bus, target, lun) (2 * (bus) + (target) / 8) * 100 + (lun) * 10 + (target % 8)

/*
 * Shut down SCSI layer
 */
/****i* januae-scsiemul/dev_exit *******************************************
*
*   NAME
*       dev_exit() -- Release registered device sessions and workers.
*
*   SYNOPSIS
*       static void dev_exit (void);
*
*   FUNCTION
*       For every device record, request shutdown of open workers, wait for
*       their stop acknowledgement, close host adapters and reinitialize the
*       record to an unused unit.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The shutdown loop also consults the pending asynchronous-request
*       slots before closing a unit.
*
****************************************************************************
*
*/

static void dev_exit (void)
{
    int i, j;
    struct devstruct *dev;

    for (i = 0; i < MAX_TOTAL_DEVICES; i++) {
	dev = &devst[i];
	if (dev->opencnt > 0) {
	    /* Abort any outstanding requests */
	    for (j = 0; j < MAX_ASYNC_REQUESTS; j++) {
	        uaecptr request;
		if ((request = dev->d_request[i]))
		    abort_async (dev, request, 0, 0);
	    }

	    /* Kill dev thread and wait for it to die */
	    write_comm_pipe_u32 (&dev->requests, 0, 1);
	    uae_sem_wait (&dev->sync_sem);

	    /* Close device */
	    dev->opencnt = 0;
	    sys_command_close (DF_SCSI, dev->unitnum);
	    sys_command_close (DF_IOCTL, dev->unitnum);
	}
	memset (dev, 0, sizeof (struct devstruct));
	dev->unitnum = dev->aunit = -1;
    }
}

/****i* januae-scsiemul/dev_reset ******************************************
*
*   NAME
*       dev_reset() -- Rebuild guest-visible SCSI unit registration.
*
*   SYNOPSIS
*       static void dev_reset (void);
*
*   FUNCTION
*       Initialize the host SCSI adapter, release prior sessions, clear open
*       handles and rescan host devices; assign guest unit numbers to
*       optical drives first and other discovered devices afterwards.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Unit identity is recalculated from the device descriptors returned
*       by the current host command adapter.
*
****************************************************************************
*
*/

static void dev_reset (void)
{
    int i, j;
    struct devstruct *dev;
    struct device_info *discsi, discsi2;
    int unitnum = 0;

    device_func_init (DEVICE_TYPE_SCSI);

    dev_exit();

    for (i = 0; i < MAX_OPEN_DEVICES; i++)
	memset (&pdevst[i], 0, sizeof (struct priv_devstruct));

    i = j = 0;
    while (i < MAX_TOTAL_DEVICES) {
	dev = &devst[i];
	discsi = 0;
	if (sys_command_open (DF_SCSI, j)) {
	    discsi = sys_command_info (DF_SCSI, j, &discsi2);
	    sys_command_close (DF_SCSI, j);
	}
        if (discsi) {
            dev->unitnum = j;
	    dev->allow_scsi = 1;
	    dev->drivetype = discsi->type;
	    memcpy (&dev->di, discsi, sizeof (struct device_info));
	    if (discsi->type == INQ_ROMD)
		dev->iscd = 1;
	}
	i++;
	j++;
    }
    unitnum = 0;
    for (i = 0; i < MAX_TOTAL_DEVICES; i++) {
        dev = &devst[i];
	if (dev->unitnum >= 0 && dev->iscd) {
	    dev->aunit = unitnum;
	    unitnum++;
	}
    }
    if (unitnum == 0)
	unitnum = 1;
    for (i = 0; i < MAX_TOTAL_DEVICES; i++) {
        dev = &devst[i];
	if (dev->unitnum >= 0) {
	    if (!dev->iscd) {
		dev->aunit = unitnum;
		unitnum++;
	    }
	    write_log ("%s = %s:%d\n", dev->di.label, UAEDEV_SCSI, dev->aunit);
	}
    }
}

static uaecptr ROM_scsidev_resname = 0,
    ROM_scsidev_resid = 0,
    ROM_scsidev_init = 0;

static uaecptr ROM_diskdev_resname = 0,
    ROM_diskdev_resid = 0,
    ROM_diskdev_init = 0;


/****i* januae-scsiemul/diskdev_startup ************************************
*
*   NAME
*       diskdev_startup() -- Write an emulated disk device Resident
*       structure.
*
*   SYNOPSIS
*       static uaecptr diskdev_startup (uaecptr resaddr);
*
*   FUNCTION
*       Write the guest Resident match word, pointers, flags, device node
*       fields and disk-device initialization address at the supplied guest
*       memory location.
*
*   INPUTS
*       resaddr - Guest destination address for the Resident structure.
*
*   RESULT
*       The first guest address after the 26-byte Resident structure.
*
****************************************************************************
*
*/

static uaecptr diskdev_startup (uaecptr resaddr)
{
    /* Build a struct Resident. This will set up and initialize
     * the cd.device */
    if (log_scsi)
	write_log ("diskdev_startup(0x%x)\n", resaddr);
    put_word(resaddr + 0x0, 0x4AFC);
    put_long(resaddr + 0x2, resaddr);
    put_long(resaddr + 0x6, resaddr + 0x1A); /* Continue scan here */
    put_word(resaddr + 0xA, 0x8101); /* RTF_AUTOINIT|RTF_COLDSTART; Version 1 */
    put_word(resaddr + 0xC, 0x0305); /* NT_DEVICE; pri 05 */
    put_long(resaddr + 0xE, ROM_diskdev_resname);
    put_long(resaddr + 0x12, ROM_diskdev_resid);
    put_long(resaddr + 0x16, ROM_diskdev_init);
    resaddr += 0x1A;
    return resaddr;
}

/****i* januae-scsiemul/scsidev_startup ************************************
*
*   NAME
*       scsidev_startup() -- Write an emulated SCSI device Resident
*       structure.
*
*   SYNOPSIS
*       uaecptr scsidev_startup (uaecptr resaddr);
*
*   FUNCTION
*       When SCSI support is enabled, write the guest SCSI device Resident
*       header and initialization pointers at the provided guest address.
*
*   INPUTS
*       resaddr - Guest destination address for the Resident structure.
*
*   RESULT
*       The address following the 26-byte Resident structure when SCSI is
*       enabled; otherwise the unchanged address.
*
****************************************************************************
*
*/

uaecptr scsidev_startup (uaecptr resaddr)
{
    if (!currprefs.scsi)
	return resaddr;
    if (log_scsi)
	write_log ("scsidev_startup(0x%x)\n", resaddr);
    /* Build a struct Resident. This will set up and initialize
     * the uaescsi.device */
    put_word(resaddr + 0x0, 0x4AFC);
    put_long(resaddr + 0x2, resaddr);
    put_long(resaddr + 0x6, resaddr + 0x1A); /* Continue scan here */
    put_word(resaddr + 0xA, 0x8101); /* RTF_AUTOINIT|RTF_COLDSTART; Version 1 */
    put_word(resaddr + 0xC, 0x0305); /* NT_DEVICE; pri 05 */
    put_long(resaddr + 0xE, ROM_scsidev_resname);
    put_long(resaddr + 0x12, ROM_scsidev_resid);
    put_long(resaddr + 0x16, ROM_scsidev_init); /* calls scsidev_init */
    resaddr += 0x1A;
    return resaddr;
    return diskdev_startup (resaddr);
}

/****i* januae-scsiemul/diskdev_install ************************************
*
*   NAME
*       diskdev_install() -- Install guest disk device trap vectors.
*
*   SYNOPSIS
*       static void diskdev_install (void);
*
*   FUNCTION
*       Write the emulated disk device name, identification, trap-call
*       stubs, function table, data table and AutoInit record into the guest
*       ROM area.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Returns without writing the device structures when SCSI support is
*       disabled.
*
****************************************************************************
*
*/

static void diskdev_install (void)
{
    uae_u32 functable, datatable;
    uae_u32 initcode, openfunc, closefunc, expungefunc;
    uae_u32 beginiofunc, abortiofunc;

    if (!currprefs.scsi)
	return;
    if (log_scsi)
	write_log ("diskdev_install(): 0x%x\n", here ());

    ROM_diskdev_resname = ds (UAEDEV_DISK);
    ROM_diskdev_resid = ds ("UAE disk.device 0.1");

    /* initcode */
    initcode = here ();
    calltrap (deftrap (diskdev_init)); dw (RTS);

    /* Open */
    openfunc = here ();
    calltrap (deftrap (diskdev_open)); dw (RTS);

    /* Close */
    closefunc = here ();
    calltrap (deftrap (diskdev_close)); dw (RTS);

    /* Expunge */
    expungefunc = here ();
    calltrap (deftrap (diskdev_expunge)); dw (RTS);

    /* BeginIO */
    beginiofunc = here ();
    calltrap (deftrap (dev_beginio));
    dw (RTS);

    /* AbortIO */
    abortiofunc = here ();
    calltrap (deftrap (dev_abortio)); dw (RTS);

    /* FuncTable */
    functable = here ();
    dl (openfunc); /* Open */
    dl (closefunc); /* Close */
    dl (expungefunc); /* Expunge */
    dl (EXPANSION_nullfunc); /* Null */
    dl (beginiofunc); /* BeginIO */
    dl (abortiofunc); /* AbortIO */
    dl (0xFFFFFFFFul); /* end of table */

    /* DataTable */
    datatable = here ();
    dw (0xE000); /* INITBYTE */
    dw (0x0008); /* LN_TYPE */
    dw (0x0300); /* NT_DEVICE */
    dw (0xC000); /* INITLONG */
    dw (0x000A); /* LN_NAME */
    dl (ROM_diskdev_resname);
    dw (0xE000); /* INITBYTE */
    dw (0x000E); /* LIB_FLAGS */
    dw (0x0600); /* LIBF_SUMUSED | LIBF_CHANGED */
    dw (0xD000); /* INITWORD */
    dw (0x0014); /* LIB_VERSION */
    dw (0x0004); /* 0.4 */
    dw (0xD000); /* INITWORD */
    dw (0x0016); /* LIB_REVISION */
    dw (0x0000); /* end of table already ??? */
    dw (0xC000); /* INITLONG */
    dw (0x0018); /* LIB_IDSTRING */
    dl (ROM_diskdev_resid);
    dw (0x0000); /* end of table */

    ROM_diskdev_init = here ();
    dl (0x00000100); /* size of device base */
    dl (functable);
    dl (datatable);
    dl (initcode);
}


/****i* januae-scsiemul/scsidev_install ************************************
*
*   NAME
*       scsidev_install() -- Install guest SCSI and disk device trap
*       vectors.
*
*   SYNOPSIS
*       void scsidev_install (void);
*
*   FUNCTION
*       Write the emulated SCSI device trap stubs and AutoInit tables into
*       the guest ROM area, then invoke diskdev_install for the disk-device
*       companion.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Returns without installation when SCSI support is disabled.
*
****************************************************************************
*
*/

void scsidev_install (void)
{
    uae_u32 functable, datatable;
    uae_u32 initcode, openfunc, closefunc, expungefunc;
    uae_u32 beginiofunc, abortiofunc;

    if (!currprefs.scsi)
	return;
    if (log_scsi)
	write_log ("scsidev_install(): 0x%x\n", here ());

    ROM_scsidev_resname = ds (UAEDEV_SCSI);
    ROM_scsidev_resid = ds ("UAE scsi.device 0.2");

    /* initcode */
    initcode = here ();
    calltrap (deftrap (dev_init)); dw (RTS);

    /* Open */
    openfunc = here ();
    calltrap (deftrap (dev_open)); dw (RTS);

    /* Close */
    closefunc = here ();
    calltrap (deftrap (dev_close)); dw (RTS);

    /* Expunge */
    expungefunc = here ();
    calltrap (deftrap (dev_expunge)); dw (RTS);

    /* BeginIO */
    beginiofunc = here ();
    calltrap (deftrap (dev_beginio));
    dw (RTS);

    /* AbortIO */
    abortiofunc = here ();
    calltrap (deftrap (dev_abortio)); dw (RTS);

    /* FuncTable */
    functable = here ();
    dl (openfunc); /* Open */
    dl (closefunc); /* Close */
    dl (expungefunc); /* Expunge */
    dl (EXPANSION_nullfunc); /* Null */
    dl (beginiofunc); /* BeginIO */
    dl (abortiofunc); /* AbortIO */
    dl (0xFFFFFFFFul); /* end of table */

    /* DataTable */
    datatable = here ();
    dw (0xE000); /* INITBYTE */
    dw (0x0008); /* LN_TYPE */
    dw (0x0300); /* NT_DEVICE */
    dw (0xC000); /* INITLONG */
    dw (0x000A); /* LN_NAME */
    dl (ROM_scsidev_resname);
    dw (0xE000); /* INITBYTE */
    dw (0x000E); /* LIB_FLAGS */
    dw (0x0600); /* LIBF_SUMUSED | LIBF_CHANGED */
    dw (0xD000); /* INITWORD */
    dw (0x0014); /* LIB_VERSION */
    dw (0x0004); /* 0.4 */
    dw (0xD000); /* INITWORD */
    dw (0x0016); /* LIB_REVISION */
    dw (0x0000); /* end of table already ??? */
    dw (0xC000); /* INITLONG */
    dw (0x0018); /* LIB_IDSTRING */
    dl (ROM_scsidev_resid);
    dw (0x0000); /* end of table */

    ROM_scsidev_init = here ();
    dl (0x00000100); /* size of device base */
    dl (functable);
    dl (datatable);
    dl (initcode);

    diskdev_install ();
}

/****i* januae-scsiemul/scsidev_start_threads ******************************
*
*   NAME
*       scsidev_start_threads() -- Initialize SCSI change-notification
*       synchronization.
*
*   SYNOPSIS
*       void scsidev_start_threads (void);
*
*   FUNCTION
*       Initialize the semaphore protecting shared SCSI change state when
*       SCSI support is enabled.
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

void scsidev_start_threads (void)
{
    if (!currprefs.scsi) /* quite useless.. */
	return;
    if (log_scsi)
	write_log ("scsidev_start_threads()\n");
    uae_sem_init (&change_sem, 0, 1);
}

/****i* januae-scsiemul/scsidev_exit ***************************************
*
*   NAME
*       scsidev_exit() -- Shut down emulated SCSI device state.
*
*   SYNOPSIS
*       void scsidev_exit (void);
*
*   FUNCTION
*       Release active SCSI device records and workers when SCSI support is
*       enabled.
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

void scsidev_exit (void)
{
    if (!currprefs.scsi)
	return;
    dev_exit ();
}

/****i* januae-scsiemul/scsidev_reset **************************************
*
*   NAME
*       scsidev_reset() -- Rescan and reset SCSI device state.
*
*   SYNOPSIS
*       void scsidev_reset (void);
*
*   FUNCTION
*       Rebuild SCSI device registration through dev_reset when SCSI support
*       is enabled.
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

void scsidev_reset (void)
{
    if (!currprefs.scsi)
	return;
    dev_reset ();
}
