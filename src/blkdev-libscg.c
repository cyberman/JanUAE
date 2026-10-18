 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Block device access using libscg
  *
  * Copyright 2004-2005 Richard Drummond
  *
  * Heavily based on code:
  * Copyright 1995 Bernd Schmidt
  * Copyright 1999 Patrick Ohly
  *
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "threaddep/thread.h"
#include "blkdev.h"
#include "scsidev.h"
#include "sleep.h"
#include "gui.h"

//#define DEBUG_ME
#ifdef  DEBUG_ME
#define DEBUG_LOG    write_log
#else
#define DEBUG_LOG(...) do { ; } while (0);
#endif

typedef int BOOL;

#include "scg/scgcmd.h"
#include "scg/scsidefs.h"
#include "scg/scsireg.h"
#include "scg/scsitransp.h"

#define MAX_DRIVES	16

struct scsidevdata {
    int   bus, target, lun;
    int   isatapi;
    int   max_dma;
    SCSI *scgp;
};

static struct scsidevdata drives[MAX_DRIVES];
static int total_drives;

static const uae_u8 *execscsicmd_in (int unitnum, const uae_u8 *data, int len, int *outlen);

/*
 * scg_isatapi() is not implemented on all platforms. Therefore,
 * this little piece of magic from Toni Wilen is needed to detect
 * ATAPI devices.
 */
/****i* januae-blkdev-libscg/is_atapi_drive ********************************
*
*   NAME
*       is_atapi_drive() -- Identify an ATAPI-compatible optical drive.
*
*   SYNOPSIS
*       static int is_atapi_drive (int unitnum);
*
*   FUNCTION
*       Issue a six-byte SCSI INQUIRY through the selected drive and inspect
*       the returned peripheral type and response-format bits.
*
*   INPUTS
*       unitnum - Index of the discovered SCSI drive.
*
*   RESULT
*       One when at least two reply bytes identify a CD-ROM peripheral with
*       response-format bits zero; otherwise zero.
*
****************************************************************************
*
*/

static int is_atapi_drive (int unitnum)
{
     static const uae_u8 cmd[6] = {0x12, 0, 0, 0, 36, 0}; /* INQUIRY */
     uae_u8 out[36];
     int outlen = sizeof (out);
     const uae_u8 *p = execscsicmd_in (unitnum, cmd, sizeof (cmd), &outlen);
     if (!p)
	return 0;
     if (outlen >= 2 && (p[0] & 31) == 5 && (p[2] & 7) == 0)
	return 1;
     return 0;
}

/****i* januae-blkdev-libscg/add_drive *************************************
*
*   NAME
*       add_drive() -- Record a discovered SCSI device.
*
*   SYNOPSIS
*       static int add_drive (SCSI *scgp);
*
*   FUNCTION
*       Copy bus, target and logical-unit coordinates into the next drive
*       slot, determine whether the device uses ATAPI and advance the
*       discovered-drive count.
*
*   INPUTS
*       scgp - Active libscg transport context containing the selected
*       device address.
*
*   RESULT
*       One when the drive is added; zero when the 16-slot drive registry is
*       full.
*
*   NOTES
*       If the libscg ATAPI query does not identify the device, an
*       INQUIRY-based probe is attempted.
*
****************************************************************************
*
*/

static int add_drive (SCSI *scgp)
{
   int result = 0;

   if (total_drives < MAX_DRIVES) {
	int isatapi;

	drives [total_drives].bus     = scgp->addr.scsibus;
	drives [total_drives].target  = scgp->addr.target;
	drives [total_drives].lun     = scgp->addr.lun;

	isatapi = scg_isatapi (scgp);

	/* If scg_isatapi returned false, we need to double-check
	 * because it may not be implemented on the target platform
	 */
	if (!isatapi) {
	    drives [total_drives].scgp = scgp;
	    isatapi = is_atapi_drive (total_drives);
	    drives [total_drives].scgp = 0;
	}
	drives [total_drives].isatapi = isatapi;

	total_drives++;
	result = 1;
    }

    return result;
}

/* Global lock - this needs to be replaced with a per-device lock */
uae_sem_t scgp_sem;

/********** generic SCSI stuff stolen from cdrecord and scsitransp.c *********/

/****i* januae-blkdev-libscg/inquiry ***************************************
*
*   NAME
*       inquiry() -- Submit a SCSI INQUIRY command.
*
*   SYNOPSIS
*       static int inquiry (SCSI *scgp, void *bp, int cnt);
*
*   FUNCTION
*       Clear the supplied response buffer and construct a group-zero
*       INQUIRY request with data-in transfer, the selected LUN and a
*       requested response length.
*
*   INPUTS
*       scgp - Libscg transport context; bp - Output inquiry buffer; cnt -
*       Requested byte count.
*
*   RESULT
*       The status returned by scg_cmd.
*
****************************************************************************
*
*/

static int inquiry (SCSI *scgp, void *bp, int cnt)
{
    struct scg_cmd *scmd = scgp->scmd;
    int result;

    memset (bp, 0, cnt);
    memset ((void *)scmd, 0, sizeof(*scmd));
    scmd->addr      = bp;
    scmd->size      = cnt;
    scmd->flags     = SCG_RECV_DATA|SCG_DISRE_ENA;
    scmd->cdb_len   = SC_G0_CDBLEN;
    scmd->sense_len = CCS_SENSE_LEN;
    scmd->cdb.g0_cdb.cmd   = SC_INQUIRY;
    scmd->cdb.g0_cdb.lun   = scg_lun(scgp);
    scmd->cdb.g0_cdb.count = cnt;

    scgp->cmdname = "inquiry";

    result = scg_cmd(scgp);

    return result;
}

/****i* januae-blkdev-libscg/test_unit_ready *******************************
*
*   NAME
*       test_unit_ready() -- Submit a SCSI TEST UNIT READY command.
*
*   SYNOPSIS
*       static int test_unit_ready (SCSI *scgp);
*
*   FUNCTION
*       Initialize a zero-length group-zero command for the selected LUN and
*       submit it using the libscg transport.
*
*   INPUTS
*       scgp - Libscg transport context for the target device.
*
*   RESULT
*       The status returned by scg_cmd.
*
****************************************************************************
*
*/

static int test_unit_ready (SCSI *scgp)
{
    struct scg_cmd *scmd = scgp->scmd;
    int result;

    memset ((void *)scmd, 0, sizeof(*scmd));
    scmd->addr      = (caddr_t)0;
    scmd->size      = 0;
    scmd->flags     = SCG_DISRE_ENA | (scgp->silent ? SCG_SILENT:0);
    scmd->cdb_len   = SC_G0_CDBLEN;
    scmd->sense_len = CCS_SENSE_LEN;
    scmd->cdb.g0_cdb.cmd = SC_TEST_UNIT_READY;
    scmd->cdb.g0_cdb.lun = scg_lun(scgp);

    scgp->cmdname = "test unit ready";

    result = scg_cmd (scgp);

    return result;
}

/****i* januae-blkdev-libscg/unit_ready ************************************
*
*   NAME
*       unit_ready() -- Evaluate device readiness with limited retry.
*
*   SYNOPSIS
*       static int unit_ready (SCSI *scgp);
*
*   FUNCTION
*       Attempt TEST UNIT READY, retry after unit attention, and retry after
*       a busy status with a 500-millisecond delay. Interpret remaining SCSI
*       sense information for device readiness.
*
*   INPUTS
*       scgp - Libscg transport context for the target device.
*
*   RESULT
*       One when the device is considered ready; zero for fatal access or
*       not-ready conditions.
*
*   NOTES
*       The final decision is based on transport status and sense
*       information, not solely on a successful first command.
*
****************************************************************************
*
*/

static int unit_ready (SCSI *scgp)
{
    register struct scg_cmd *scmd = scgp->scmd;

    if (test_unit_ready(scgp) >= 0)             /* alles OK */
	return 1;
    else if (scmd->error >= SCG_FATAL)          /* nicht selektierbar */
	return 0;

    if (scg_sense_key(scgp) == SC_UNIT_ATTENTION) {
	if (test_unit_ready(scgp) >= 0)         /* alles OK */
	     return 1;
    }
    if ((scg_cmd_status(scgp) & ST_BUSY) != 0) {
	/* Busy/reservation_conflict */
	uae_msleep (500);

	if (test_unit_ready(scgp) >= 0)         /* alles OK */
	    return 1;
    }
    if (scg_sense_key(scgp) == -1) {            /* non extended Sense */
	if (scg_sense_code(scgp) == 4)          /* NOT_READY */
	    return 0;
	return 1;
    }
                                                /* FALSE wenn NOT_READY */
    return (scg_sense_key (scgp) != SC_NOT_READY);
}

/****i* januae-blkdev-libscg/print_product *********************************
*
*   NAME
*       print_product() -- Log SCSI inquiry identification.
*
*   SYNOPSIS
*       static void print_product (const struct scsi_inquiry *ip);
*
*   FUNCTION
*       Write the fixed-width vendor, product and revision fields to the
*       log; append the CD-ROM or disk category for recognized peripheral
*       types.
*
*   INPUTS
*       ip - SCSI INQUIRY data to format.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void print_product (const struct scsi_inquiry *ip)
{
    write_log ("'%.8s' ",  ip->vendor_info);
    write_log ("'%.16s' ", ip->prod_ident);
    write_log ("'%.4s' ",  ip->prod_revision);

    if (ip->type == INQ_ROMD)
	write_log ("CD-ROM");
    else if (ip->type == INQ_DASD)
	write_log ("Disk");
}

/* get integer value from env or return default value, if unset */
/****i* januae-blkdev-libscg/getenvint *************************************
*
*   NAME
*       getenvint() -- Read an integer-valued environment setting.
*
*   SYNOPSIS
*       static int getenvint (const char *varname, int def);
*
*   FUNCTION
*       Look up a named environment variable and convert its value with
*       atoi, falling back to the supplied default when it is absent.
*
*   INPUTS
*       varname - Environment variable name; def - Value to use when it is
*       unset.
*
*   RESULT
*       The parsed integer, or def when no environment value is present.
*
****************************************************************************
*
*/

static int getenvint (const char *varname, int def)
{
    const char *val = getenv (varname);
    return val ? atoi (val) : def;
}

/****i* januae-blkdev-libscg/openscsi **************************************
*
*   NAME
*       openscsi() -- Open a libscg target connection.
*
*   SYNOPSIS
*       static SCSI *openscsi (int scsibus, int target, int lun);
*
*   FUNCTION
*       Allocate a transport context, apply SCSI diagnostic settings from
*       environment variables, select the configured device path and target
*       coordinates, and open the backend connection.
*
*   INPUTS
*       scsibus - SCSI bus number; target - Target ID; lun - Logical unit
*       number.
*
*   RESULT
*       A live SCSI transport context, or NULL if allocation or opening
*       fails.
*
*   NOTES
*       UAE_SCSI_DEVICE overrides the configured SCSI device path when set
*       to a nonempty string.
*
****************************************************************************
*
*/

static SCSI *openscsi (int scsibus, int target, int lun)
{
    SCSI *scgp = scg_smalloc ();

    if (scgp) {
	char *device;

	scgp->debug   = getenvint ("UAE_SCSI_DEBUG", 0);
	scgp->kdebug  = getenvint ("UAE_SCSI_KDEBUG", 0);
	scgp->silent  = getenvint ("UAE_SCSI_SILENT", 1);
	scgp->verbose = getenvint ("UAE_SCSI_VERBOSE", 0);
	device        = getenv    ("UAE_SCSI_DEVICE");

	if (!device || (strlen(device) == 0))
	    device = currprefs.scsi_device;

	write_log ("SCSIDEV: Device '%s'\n", device);

	scg_settarget (scgp, scsibus, target, lun);
	scg_settimeout (scgp, 80*60);

	if (scg__open (scgp, device) <= 0) {
	    if (scgp->errstr)
		write_log ("SCSIDEV: Failed to open '%s': %s\n",
			   device, scgp->errstr);
	    scg_sfree (scgp);
	    scgp = 0;
       }
    }
    return scgp;
}

/****i* januae-blkdev-libscg/closescsi *************************************
*
*   NAME
*       closescsi() -- Close and release a libscg connection.
*
*   SYNOPSIS
*       static void closescsi (SCSI *scgp);
*
*   FUNCTION
*       Close the transport connection and free its libscg context.
*
*   INPUTS
*       scgp - Libscg context to close and release.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void closescsi (SCSI *scgp)
{
    scg__close (scgp);
    scg_sfree (scgp);
}

/********************* start of our own code ************************/

static uae_u8 scsibuf [DEVICE_SCSI_BUFSIZE];

/****i* januae-blkdev-libscg/execscsicmd ***********************************
*
*   NAME
*       execscsicmd() -- Submit a SCSI command using an optional shared
*       response buffer.
*
*   SYNOPSIS
*       static int execscsicmd (int unitnum, const uae_u8 *data, int len,
*       uae_u8 *inbuf, int inlen);
*
*   FUNCTION
*       Acquire the global SCSI semaphore, populate the command descriptor,
*       select its drive and target, optionally zero the response buffer,
*       and submit the command through libscg.
*
*   INPUTS
*       unitnum - Registered drive index; data - Command descriptor bytes;
*       len - Descriptor length; inbuf - Optional data-in buffer; inlen -
*       Its capacity.
*
*   RESULT
*       The transfer size reported in scmd->size after command submission.
*
*   NOTES
*       The return value does not separately encode the libscg command error
*       status.
*
****************************************************************************
*
*/

static int execscsicmd (int unitnum, const uae_u8 *data, int len, uae_u8 *inbuf,
			int inlen)
{
    int sactual = 0;
    struct scsidevdata *sdd = &drives[unitnum];
    SCSI *scgp              = sdd->scgp;
    struct scg_cmd *scmd;

    scmd = scgp->scmd;

    DEBUG_LOG ("SCSIDEV: execscicmd data=%08lx len=%d, inbuf=%08lx"\
	       " inlen=%d\n", data, len, inbuf, inlen);

    uae_sem_wait (&scgp_sem);
    memset (scmd, 0, sizeof (*scmd));

    scmd->timeout   = 80 * 60;
    if (inbuf) {
	scmd->addr      = (caddr_t) inbuf;
	scmd->size      = inlen;
	scmd->flags     = SCG_RECV_DATA;
	memset (inbuf, 0, inlen);
    } else {
	scmd->flags     = SCG_DISRE_ENA;
    }

    scmd->cdb_len   = len;
    memcpy (&scmd->cdb, data, len);
    scmd->target    = sdd->target;
    scmd->sense_len = -1;
    scmd->sense_count = 0;
    *(uae_u8 *)&scmd->scb = 0;

    scg_settarget (scgp, sdd->bus, sdd->target, sdd->lun);
    scgp->cmdname    = "???";
    scgp->curcmdname = "???";

    DEBUG_LOG ("SCSIDEV: sending command: 0x%2x\n", scmd->cdb.g0_cdb.cmd);

    gui_cd_led (1);

    scg_cmd (scgp);

    uae_sem_post (&scgp_sem);

    DEBUG_LOG ("SCSIDEV: result: %d %d\n", scmd->error, scmd->ux_errno);

    return scmd->size;
}

/****i* januae-blkdev-libscg/execscsicmd_direct ****************************
*
*   NAME
*       execscsicmd_direct() -- Execute a guest-provided direct SCSI
*       request.
*
*   SYNOPSIS
*       static int execscsicmd_direct (int unitnum, uaecptr acmd);
*
*   FUNCTION
*       Read the guest request fields, validate its data range, prepare the
*       libscg command and optional ATAPI adaptation, submit the command,
*       and write actual lengths, status and sense bytes back to guest
*       memory.
*
*   INPUTS
*       unitnum - Registered drive index; acmd - Guest address of the SCSI
*       command structure.
*
*   RESULT
*       A device I/O status value: zero on the accepted transfer path, minus
*       five for an invalid guest data range, or a nonzero error status for
*       reported command failures.
*
*   NOTES
*       Guest sense and transfer lengths are updated within the supplied
*       command structure.
*
****************************************************************************
*
*/

static int execscsicmd_direct (int unitnum, uaecptr acmd)
{
    int sactual = 0;
    struct scsidevdata *sdd = &drives[unitnum];
    SCSI *scgp              = sdd->scgp;
    struct scg_cmd *scmd    = scgp->scmd;

    uaecptr scsi_data      = get_long (acmd + 0);
    uae_u32 scsi_len       = get_long (acmd + 4);
    uaecptr scsi_cmd       = get_long (acmd + 12);
    int scsi_cmd_len       = get_word (acmd + 16);
    int scsi_cmd_len_orig  = scsi_cmd_len;
    uae_u8  scsi_flags     = get_byte (acmd + 20);
    uaecptr scsi_sense     = get_long (acmd + 22);
    uae_u16 scsi_sense_len = get_word (acmd + 26);
    int io_error           = 0;
    int parm;
    addrbank *bank_data    = &get_mem_bank (scsi_data);
    addrbank *bank_cmd	   = &get_mem_bank (scsi_cmd);
    uae_u8   *scsi_datap;
    uae_u8   *scsi_datap_org;

    DEBUG_LOG ("SCSIDEV: unit=%d: execscsicmd_direct\n", unitnum);

    /* do transfer directly to and from Amiga memory */
    if (!bank_data || !bank_data->check (scsi_data, scsi_len))
	return -5; /* IOERR_BADADDRESS */

    uae_sem_wait (&scgp_sem);

    memset (scmd, 0, sizeof (*scmd));
    /* the Amiga does not tell us how long the timeout shall be, so make it
     * _very_ long (specified in seconds) */
    scmd->timeout   = 80 * 60;
    scsi_datap      = scsi_datap_org = scsi_len
                      ? bank_data->xlateaddr (scsi_data) : 0;
    scmd->size      = scsi_len;
    scmd->flags     = (scsi_flags & 1) ? SCG_RECV_DATA : SCG_DISRE_ENA;
    memcpy (&scmd->cdb, bank_cmd->xlateaddr (scsi_cmd), scsi_cmd_len);
    scmd->target    = sdd->target;
    scmd->sense_len = (scsi_flags & 4) ? 4 : /* SCSIF_OLDAUTOSENSE */
		      (scsi_flags & 2) ? scsi_sense_len : /* SCSIF_AUTOSENSE */
		      -1;
    scmd->sense_count = 0;
    *(uae_u8 *)&scmd->scb = 0;
    if (sdd->isatapi)
	scsi_atapi_fixup_pre (scmd->cdb.cmd_cdb, &scsi_cmd_len, &scsi_datap,
			      &scsi_len, &parm);
    scmd->addr      = (caddr_t)scsi_datap;
    scmd->cdb_len   = scsi_cmd_len;

    scg_settarget (scgp, sdd->bus, sdd->target, sdd->lun);
    scgp->cmdname    = "???";
    scgp->curcmdname = "???";

    DEBUG_LOG ("SCSIDEV: sending command: 0x%2x\n", scmd->cdb.g0_cdb.cmd);

    scg_cmd (scgp);

    DEBUG_LOG ("SCSIDEV: result: %d %d %s\n", scmd->error, scmd->ux_errno,\
	       scgp->errstr);

    gui_cd_led (1);

    put_word (acmd + 18, scmd->error == SCG_FATAL
					? 0 : scsi_cmd_len); /* fake scsi_CmdActual */
    put_byte (acmd + 21, *(uae_u8 *)&scmd->scb);	     /* scsi_Status */

    if (*(uae_u8 *)&scmd->scb) {
	io_error = 45; /* HFERR_BadStatus */
	/* copy sense? */
	for (sactual = 0;
	     scsi_sense && sactual < scsi_sense_len && sactual < scmd->sense_count;
	     sactual++) {
	     put_byte (scsi_sense + sactual, scmd->u_sense.cmd_sense[sactual]);
	}
	put_long (acmd + 8, 0); /* scsi_Actual */
    } else {
	int i;
	for (i = 0; i < scsi_sense_len; i++)
	    put_byte (scsi_sense + i, 0);
	sactual = 0;
	if (scmd->error != SCG_NO_ERROR || scmd->ux_errno != 0) {
	    /* We might have been limited by the hosts DMA limits,
	       which is usually indicated by ENOMEM */
	    if (scsi_len > (unsigned int)sdd->max_dma && scmd->ux_errno == ENOMEM)
		io_error = (uae_u8)-4; /* IOERR_BADLENGTH */
	    else {
		io_error = 20; /* io_Error, but not specified */
		put_long (acmd + 8, 0); /* scsi_Actual */
	    }
        } else {
	    scsi_len = scmd->size;
	    if (sdd->isatapi)
		scsi_atapi_fixup_post (scmd->cdb.cmd_cdb, scsi_cmd_len,
				       scsi_datap_org, scsi_datap,
				       &scsi_len, parm);
	    io_error = 0;
            put_long (acmd + 8, scsi_len); /* scsi_Actual */
	}
    }
    put_word (acmd + 28, sactual);

    uae_sem_post (&scgp_sem);

    if (scsi_datap != scsi_datap_org)
	free (scsi_datap);

    return io_error;
}

/****i* januae-blkdev-libscg/execscsicmd_out *******************************
*
*   NAME
*       execscsicmd_out() -- Submit a SCSI command without a data-in buffer.
*
*   SYNOPSIS
*       static const uae_u8 *execscsicmd_out (int unitnum, const uae_u8
*       *data, int len);
*
*   FUNCTION
*       Forward a command descriptor to execscsicmd with no response buffer.
*
*   INPUTS
*       unitnum - Registered drive index; data - Command descriptor bytes;
*       len - Descriptor length.
*
*   RESULT
*       The original command buffer pointer when the reported transfer size
*       is nonnegative; otherwise NULL.
*
*   NOTES
*       A non-NULL return is not an independent test of the libscg command
*       error field.
*
****************************************************************************
*
*/

static const uae_u8 *execscsicmd_out (int unitnum, const uae_u8 *data, int len)
{
    DEBUG_LOG ("SCSIDEV: unit=%d: execscsicmd_out\n", unitnum);

    if (execscsicmd (unitnum, data, len, 0, 0) < 0)
	return 0;

    return data;
}

/****i* januae-blkdev-libscg/execscsicmd_in ********************************
*
*   NAME
*       execscsicmd_in() -- Read SCSI response data into the shared transfer
*       buffer.
*
*   SYNOPSIS
*       static const uae_u8 *execscsicmd_in (int unitnum, const uae_u8
*       *data, int len, int *outlen);
*
*   FUNCTION
*       Execute a command with the fixed-size SCSI response buffer and
*       optionally publish the reported transfer length.
*
*   INPUTS
*       unitnum - Registered drive index; data - Command descriptor bytes;
*       len - Descriptor length; outlen - Optional destination for the
*       reported byte count.
*
*   RESULT
*       A pointer to the shared response buffer when the reported size is
*       positive; otherwise NULL.
*
*   NOTES
*       The returned storage is shared across calls and is not caller-owned.
*
****************************************************************************
*
*/

static const uae_u8 *execscsicmd_in (int unitnum, const uae_u8 *data, int len, int *outlen)
{
    int v;

    DEBUG_LOG ("SCSIDEV: unit=%d: execscsicmd_in\n", unitnum);

    v = execscsicmd (unitnum, data, len, scsibuf, DEVICE_SCSI_BUFSIZE);

    if (v < 0)
	return 0;
    if (v == 0)
	return 0;
    if (outlen)
	*outlen = v;

    return scsibuf;
}

/*
 * Scan SCSI busses to detect any devices
 * that we want to supply to the Amgia.
 *
 * Based on code from cdrecord
 */
/****i* januae-blkdev-libscg/scanscsi **************************************
*
*   NAME
*       scanscsi() -- Discover CD/DVD devices on available SCSI buses.
*
*   SYNOPSIS
*       static int scanscsi (SCSI *scgp);
*
*   FUNCTION
*       Enumerate bus and target addresses at LUN zero, skip the initiator
*       ID, probe responsive devices, and register those whose INQUIRY type
*       identifies an optical drive.
*
*   INPUTS
*       scgp - Open scanning transport context.
*
*   RESULT
*       Zero after the scan completes.
*
*   NOTES
*       The scan considers up to 16 buses and 16 target IDs per bus;
*       discovered drives are retained in the bounded registry.
*
****************************************************************************
*
*/

static int scanscsi (SCSI *scgp)
{
    int bus;
    int tgt;
    int lun = 0;
    int initiator;
    int have_tgt;
    int n;

    scgp->silent++;
    for (bus = 0; bus < 16; bus++) {
	scg_settarget (scgp, bus, 0, 0);

	if (!scg_havebus (scgp, bus))
	    continue;

	initiator = scg_initiator_id (scgp);
	write_log ("scsibus%d:\n", bus);

	for (tgt = 0; tgt < 16; tgt++) {
	    n = bus * 100 + tgt;

	    scg_settarget (scgp, bus, tgt, lun);
	    have_tgt = unit_ready (scgp) || scgp->scmd->error != SCG_FATAL;

	    if (!have_tgt && tgt > 7) {
		if (scgp->scmd->ux_errno == EINVAL)
		    break;
		continue;
	    }

	    write_log ("  %d,%d,%d  %d ", bus, tgt, lun, n);

	    if (tgt == initiator) {
		write_log ("HOST ADAPTOR\n");
		continue;
	    }
	    if (!have_tgt) {
		/* Hack: fd -> -2 means no access */
		write_log( "%c\n", scgp->fd == -2 ? '?':'*');
		continue;
	    }

	    if ((scgp->scmd->error < SCG_FATAL)
		|| (scgp->scmd->scb.chk && scgp->scmd->sense_count > 0)) {
        	struct scsi_inquiry *inq = scgp->inq;

        	inquiry (scgp, inq, sizeof (*inq));
		print_product (inq);

		/* Just CD/DVD drives for now */
		if (inq->type == INQ_ROMD)
		    add_drive (scgp);
	    }

            write_log ("\n");
	}
    }
    scgp->silent--;
    return 0;
}

/****i* januae-blkdev-libscg/open_scsi_bus *********************************
*
*   NAME
*       open_scsi_bus() -- Initialize and scan the configured SCSI bus.
*
*   SYNOPSIS
*       static int open_scsi_bus (int flags);
*
*   FUNCTION
*       Initialize the global SCSI semaphore on first entry, open a libscg
*       scanning context with configured diagnostics, scan accessible
*       targets and release the scanning context.
*
*   INPUTS
*       flags - Bus opening flags; not consulted by this implementation.
*
*   RESULT
*       One when the scanning connection opens successfully; zero otherwise.
*
*   NOTES
*       A successful result indicates that the scanning bus was opened, not
*       that an optical drive was found.
*
****************************************************************************
*
*/

static int open_scsi_bus (int flags)
{
    int   result = 0;
    int   debug, verbose;
    SCSI *scgp_scan;
    char *device;
    char  errstr[128];
    static int init = 0;

    DEBUG_LOG ("SCSIDEV: open_scsi_bus\n");

    if (!init) {
	init = 1;
	uae_sem_init (&scgp_sem, 0, 1);
	/* TODO: replace global lock with per-device locks */
    }

    debug   = getenvint ("UAE_SCSI_DEBUG", 0);
    verbose = getenvint ("UAE_SCSI_VERBOSE", 0);
    device  = getenv    ("UAE_SCSI_DEVICE");

    if (!device || (strlen (device) == 0))
	device = currprefs.scsi_device;

    if ((scgp_scan = scg_open (device, errstr, sizeof (errstr),
					       debug, verbose)) != (SCSI *)0) {
	scanscsi (scgp_scan);
	result = 1;
	scg_close (scgp_scan);
    } else {
	write_log ("SCSIDEV: can't open bus: %s\n", errstr);
    }

    write_log ("SCSIDEV: %d devices found\n", total_drives);
    return result;
}

/****i* januae-blkdev-libscg/close_scsi_bus ********************************
*
*   NAME
*       close_scsi_bus() -- Close registered libscg drive connections.
*
*   SYNOPSIS
*       static void close_scsi_bus (void);
*
*   FUNCTION
*       Walk the discovered-drive registry and close each stored transport
*       context.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The routine does not reset the registry length or destroy the global
*       semaphore.
*
****************************************************************************
*
*/

static void close_scsi_bus (void)
{
    int i;

    DEBUG_LOG ("SCSIDEV: close_scsi_bus\n");

    for (i = 0; i < total_drives; i++) {
	closescsi (drives[i].scgp);
	drives[i].scgp = 0;
    }
}

/****i* januae-blkdev-libscg/open_scsi_device ******************************
*
*   NAME
*       open_scsi_device() -- Open a registered SCSI device for commands.
*
*   SYNOPSIS
*       static int open_scsi_device (int unitnum);
*
*   FUNCTION
*       Find the registry entry and open its bus, target and LUN through
*       libscg if no connection is already associated with it.
*
*   INPUTS
*       unitnum - Registered drive index.
*
*   RESULT
*       One when a connection is open or already present; zero when opening
*       is unsuccessful or the index is not less than the drive count.
*
****************************************************************************
*
*/

static int open_scsi_device (int unitnum)
{
    int result = 0;

    DEBUG_LOG ("SCSIDEV: unit=%d: open_scsi_device\n", unitnum);

    if (unitnum < total_drives) {
	struct scsidevdata *sdd = &drives[unitnum];

	if (!sdd->scgp) {
	    if ((sdd->scgp = openscsi (sdd->bus, sdd->target, sdd->lun)) != 0)
	        result = 1;
	} else
	    /* already open */
	    result = 1;
    }
    return result;
}

/****i* januae-blkdev-libscg/close_scsi_device *****************************
*
*   NAME
*       close_scsi_device() -- Retain the device connection until bus
*       closure.
*
*   SYNOPSIS
*       static void close_scsi_device (int unitnum);
*
*   FUNCTION
*       Leave the registered device connection open; the bus-close operation
*       owns its release.
*
*   INPUTS
*       unitnum - Registered drive index; unused.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void close_scsi_device (int unitnum)
{
    DEBUG_LOG ("SCSIDEV: unit=%d: close_scsi_device\n", unitnum);

    /* do nothing - leave devices open until the bus is closed */
}

/****i* januae-blkdev-libscg/media_check ***********************************
*
*   NAME
*       media_check() -- Query whether device media is ready.
*
*   SYNOPSIS
*       static int media_check (SCSI *scgp);
*
*   FUNCTION
*       Acquire the global SCSI semaphore, submit TEST UNIT READY and
*       release the semaphore.
*
*   INPUTS
*       scgp - Open libscg device context.
*
*   RESULT
*       One when TEST UNIT READY returns a nonnegative status; otherwise
*       zero.
*
****************************************************************************
*
*/

static int media_check (SCSI *scgp)
{
    int media = 0;

    uae_sem_wait (&scgp_sem);

    if (test_unit_ready (scgp) >= 0)
	media = 1;

    uae_sem_post (&scgp_sem);

    DEBUG_LOG ("SCSIDEV: media check :%d\n", media);

    return media;
}

/****i* januae-blkdev-libscg/info_device ***********************************
*
*   NAME
*       info_device() -- Populate the emulator-facing optical device
*       descriptor.
*
*   SYNOPSIS
*       static struct device_info *info_device (int unitnum, struct
*       device_info *di);
*
*   FUNCTION
*       For a registered drive, fill its logical unit index, media-presence
*       flag, read-only status, 2048-byte sector size, optical type,
*       identifier and formatted physical address label.
*
*   INPUTS
*       unitnum - Registered drive index; di - Caller-supplied device_info
*       structure.
*
*   RESULT
*       The supplied descriptor pointer for a registered drive, or NULL when
*       the index is not less than the registry count.
*
*   NOTES
*       Bus and LUN exposed to the emulator are zero; target is the registry
*       index, not the physical SCSI target.
*
****************************************************************************
*
*/

static struct device_info *info_device (int unitnum, struct device_info *di)
{
    DEBUG_LOG ("SCSIDEV: unit=%d: info_device\n", unitnum);

    if (unitnum < total_drives) {
	struct scsidevdata *sdd = &drives[unitnum];

	di->bus		    = 0;
	di->target	    = unitnum;
	di->lun		    = 0;
	di->media_inserted  = media_check (sdd->scgp);
	di->write_protected = 1;
	di->bytespersector  = 2048;
	di->cylinders	    = 1;
	di->type	    = INQ_ROMD; /* We only support CD/DVD drives for now */
	di->id		    = unitnum + 1;
	/* TODO: Create a more informative device label */
	sprintf (di->label, "(%d,%d,%d)", sdd->bus, sdd->target, sdd->lun);
    } else
	di = 0;

    return di;
}

/****i* januae-blkdev-libscg/check_isatapi *********************************
*
*   NAME
*       check_isatapi() -- Report the saved ATAPI classification.
*
*   SYNOPSIS
*       static int check_isatapi (int unitnum);
*
*   FUNCTION
*       Read the ATAPI flag recorded when the SCSI drive was discovered.
*
*   INPUTS
*       unitnum - Registered drive index.
*
*   RESULT
*       The stored ATAPI classification value.
*
****************************************************************************
*
*/

static int check_isatapi (int unitnum)
{
    return drives[unitnum].isatapi;
}

struct device_functions devicefunc_scsi_libscg = {
    open_scsi_bus,
    close_scsi_bus,
    open_scsi_device,
    close_scsi_device,
    info_device,
    execscsicmd_out,
    execscsicmd_in,
    execscsicmd_direct,
    0, 0, 0, 0, 0, 0, 0,
    check_isatapi,
    0, 0
};
