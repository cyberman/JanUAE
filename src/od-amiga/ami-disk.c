 /*
  * UAE - The Un*x Amiga Emulator
  *
  * ami-disk.c: Creates pseudo dev: handler. Copy tracks to rawfile
  * (used in zfile.c).
  *
  * 08/06/97: Modified to avoid a spilled registers error message
  *           when used with the new fd2inline convention.
  *
  * Copyright 1996 Samuel Devulder.
  */

#include "sysconfig.h"
#include "sysdeps.h"

/****************************************************************************/

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/devices.h>
#include <exec/io.h>

#include <dos/dos.h>

#include <devices/trackdisk.h>

#if defined(POWERUP)
#include <dos/dosextens.h>
#ifdef  USE_CLIB
#include <clib/exec_protos.h>
#include <clib/alib_protos.h>
#else
#include <powerup/ppcproto/exec.h>
#include <powerup/ppcproto/dos.h>
#endif
#else
/* This should cure the spilled-register problem. An other solution is to */
/* compile with -O2 instead of -O3 */
#define NO_INLINE_STDARGS
#define __NOINLINES__

#include <proto/exec.h>
#ifndef __SASC
#include <clib/alib_protos.h>
#endif
#include <proto/dos.h>
#endif

/****************************************************************************/

#include <ctype.h>
#include <signal.h>

/****************************************************************************/

const char *amiga_dev_path  = "DEV:";
const char *ixemul_dev_path = "/dev/";

int readdevice (char *name, char *dst);
void initpseudodevices (void);
void closepseudodevices (void);

static void myDoIO(struct IOStdReq *ioreq, LONG CMD, LONG FLAGS, LONG OFFSET,
                   LONG LENGTH, LONG DATA);

/****************************************************************************/

static const char *pseudo_dev_path = "T:DEV";

static char pseudo_dev_created  = 0;
static char pseudo_dev_assigned = 0;
static char dfx_done[4];

static int device_exists (const char *device_name, int device_unit);
static int dev_inhibit(char *dev,int on);
static void set_req(int ok);

/****************************************************************************/
/*
 * Creates peudo DEV:DFx files.
 */
/****i* januae-ami-disk/initpseudodevices **********************************
*
*   NAME
*       initpseudodevices() -- Provide pseudo floppy-device paths.
*
*   SYNOPSIS
*       void initpseudodevices(void);
*
*   FUNCTION
*       Initialize the pseudo-device state. If DEV: is absent, create T:DEV
*       and assign DEV: to that directory; then create DF0 through DF3
*       entries for available trackdisk.device units.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Only units 0 through 3 are examined. Directory and assignment
*       ownership is recorded for closepseudodevices().
*
****************************************************************************
*
*/

void initpseudodevices(void)
{
    ULONG lock;
    int i;

    pseudo_dev_created  = 0;
    pseudo_dev_assigned = 0;
    for(i=0;i<4;++i) dfx_done[i]=0;

    /* check if dev: already exists */
    set_req(0);lock = Lock(amiga_dev_path,SHARED_LOCK);set_req(1);
    if(!lock) {
        char name[80];
        set_req(0);lock = Lock(pseudo_dev_path,SHARED_LOCK);set_req(1);
        if(!lock) {
            /* create it */
            lock = CreateDir(pseudo_dev_path);
            if(!lock) goto fail;
            UnLock(lock);lock = Lock(pseudo_dev_path,SHARED_LOCK);
            pseudo_dev_created = 1;
        }
        strcpy(name,amiga_dev_path);
        if(*name && name[strlen(name)-1]==':') name[strlen(name)-1]='\0';
        if(!AssignLock(name,lock)) {UnLock(lock);goto fail;}
        /* the lock is the assign now */
        pseudo_dev_assigned = 1;
    } else UnLock(lock);

    /* Create the dev:DFi entry */
    for(i=0;i<4;++i) if(device_exists("trackdisk.device",i)) {
        ULONG fd;
        char name[80];

        sprintf(name,"%sDF%d",amiga_dev_path,i);
        fd = Open(name,MODE_NEWFILE);
        if(fd) {Close(fd);dfx_done[i]=1;}
    }

    return;
 fail:
    fprintf(stderr,"Failed to create pseudo dev: entry!\n");
}

/****************************************************************************/
/*
 * Cleanup pseudo DEV:DFx
 */
/****i* januae-ami-disk/closepseudodevices *********************************
*
*   NAME
*       closepseudodevices() -- Release pseudo floppy-device paths.
*
*   SYNOPSIS
*       void closepseudodevices(void);
*
*   FUNCTION
*       Remove device entries created by initpseudodevices(), withdraw its
*       DEV: assignment, and delete its temporary directory if it created
*       one.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Existing DEV: assignments and directories are left in place.
*
****************************************************************************
*
*/

void closepseudodevices(void)
{
    int i;
    for(i=0;i<4;++i) if(dfx_done[i]) {
        char name[80];
        sprintf(name,"%sDF%d",amiga_dev_path,i);
        DeleteFile(name);
        dfx_done[i] = 0;
    }

    if(pseudo_dev_assigned) {
        char name[80];
        strcpy(name,amiga_dev_path);
        if(*name && name[strlen(name)-1]==':') name[strlen(name)-1]='\0';
        AssignLock (name, (BPTR)NULL);
        pseudo_dev_assigned = 0;
    }

    if(pseudo_dev_created) {
        DeleteFile(pseudo_dev_path);
        pseudo_dev_created = 0;
    }
}

/****************************************************************************/
/*
 * Enable/Disable system requester
 */
/****i* januae-ami-disk/set_req ********************************************
*
*   NAME
*       set_req() -- Control DOS requester visibility.
*
*   SYNOPSIS
*       static void set_req(int ok);
*
*   FUNCTION
*       Replace the current process window pointer with the
*       disabled-requester sentinel, or restore the previously saved
*       pointer.
*
*   INPUTS
*       ok - Nonzero to restore the saved window pointer; zero to disable
*       requesters.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Only a task of type NT_PROCESS is modified.
*
****************************************************************************
*
*/

static void set_req(int ok)
{
    static ULONG wd = 0;
    struct Process *pr;

    pr = (void*)FindTask(NULL);

    if(pr->pr_Task.tc_Node.ln_Type != NT_PROCESS) return;

    if(ok)  {
        pr->pr_WindowPtr = (APTR)wd;
    }
    else    {
        wd = (ULONG)pr->pr_WindowPtr;
        pr->pr_WindowPtr = (APTR)-1;
    }
}

/****************************************************************************/
/*
 * checks if a device exists
 */
/****i* januae-ami-disk/device_exists **************************************
*
*   NAME
*       device_exists() -- Check for a native device unit.
*
*   SYNOPSIS
*       static int device_exists (const char *device_name, int device_unit);
*
*   FUNCTION
*       Open a named Exec device unit using a temporary message port and I/O
*       request, then release those temporary objects.
*
*   INPUTS
*       device_name - Exec device name. device_unit - Unit number.
*
*   RESULT
*       1 if OpenDevice succeeds; otherwise 0.
*
****************************************************************************
*
*/

static int device_exists (const char *device_name, int device_unit)
{
    struct IOStdReq *ioreq  = NULL;
    struct MsgPort  *port   = NULL;
    int ret = 0;

    port = CreatePort(0, 0);
    if(port) {
        ioreq = CreateStdIO(port);
        if(ioreq) {
            if(!OpenDevice(device_name,device_unit,(void*)ioreq,0)) {
                CloseDevice((void*)ioreq);
                ret = 1;
            }
            DeleteStdIO(ioreq);
        }
        DeletePort(port);
    }
    return ret;
}

/****************************************************************************/
/*
 * extract the device and unit form a filename.
 */
/****i* januae-ami-disk/extract_dev_unit ***********************************
*
*   NAME
*       extract_dev_unit() -- Parse a native disk identifier.
*
*   SYNOPSIS
*       static void extract_dev_unit(char *name, char **dev_name, int
*       *dev_unit);
*
*   FUNCTION
*       Interpret DF0 through DF3 as trackdisk.device units; otherwise split
*       a device path at its final slash or treat the name as unit zero.
*       Append .device when no extension exists.
*
*   INPUTS
*       name - Device identifier. dev_name - Output location for an
*       allocated device-name string. dev_unit - Output location for the
*       unit number.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The caller owns the allocated device-name string.
*
****************************************************************************
*
*/

static void extract_dev_unit(char *name, char **dev_name, int *dev_unit)
{
    char *s;
    if(tolower(name[0])=='d' && tolower(name[1])=='f' &&
       name[2]>='0' && name[2]<='3' && name[3]=='\0') {
        /* DF0 */
        *dev_unit = name[2]-'0';
        *dev_name = strdup("trackdisk.device");
    } else if((s = strrchr(name,'/'))) {
        /* trackdisk[.device]/0 */
        *dev_unit = atoi(s+1);
        *dev_name = malloc(1 + s-name);
        if(*dev_name) {
            strncpy(*dev_name, name, 1 + s-name);
            (*dev_name)[s-name]='\0';
        }
    } else {
        /* ?? STRANGEDISK0: ?? */
        *dev_unit = 0;
        *dev_name = strdup(name);
    }
    if(*dev_name) {
        char *s;
        if(!(s = strrchr(*dev_name,'.'))) {
            /* .device is missing */
            s = malloc(8+strlen(*dev_name));
            if(s) {
                sprintf(s,"%s.device",*dev_name);
                free(*dev_name);
                *dev_name = s;
            }
        }
    }
}

/****************************************************************************/
/*
 * copy a device to a FILE*.
 */
/****i* januae-ami-disk/raw_copy *******************************************
*
*   NAME
*       raw_copy() -- Export a device as raw tracks.
*
*   SYNOPSIS
*       static int raw_copy(char *dev_name, int dev_unit, FILE *dst);
*
*   FUNCTION
*       Open an Exec device, check media presence, read fixed-size tracks
*       into chip memory and write them to the destination stream. Stop the
*       drive motor and withdraw a temporary DOS inhibit when used.
*
*   INPUTS
*       dev_name - Exec device name. dev_unit - Device unit. dst - Open
*       binary output stream.
*
*   RESULT
*       1 when the export reaches its end without a reported setup, media,
*       or stream-write failure; otherwise 0.
*
*   NOTES
*       Uses 160 tracks of 11 sectors at 512 bytes per sector. Device read
*       errors are logged but do not themselves terminate the loop.
*
****************************************************************************
*
*/

static int raw_copy(char *dev_name, int dev_unit, FILE *dst)
{
    struct MsgPort  *port   = NULL;
    struct IOStdReq *ioreq  = NULL;
    int tracklen            = 512*11;
    int retstatus           = 1;
    int inhibited           = 0;
    char *buffer            = NULL;
    static char name[]      = {'D','F','0','\0'};

    if(!strcmp(dev_name, "trackdisk.device")) {
        inhibited = 1;
        name[2] = '0'+dev_unit;
    }

    /* allocate system stuff */
    if((port   = CreatePort(0, 0)))            {
    if((ioreq  = CreateStdIO(port)))           {
    if((buffer = AllocMem(tracklen, MEMF_CHIP))) {

    /* gain access to the device */
    if(!OpenDevice(dev_name, dev_unit, (struct IORequest*)ioreq, 0)) {

    /* check if a disk is present */
    myDoIO(ioreq, TD_CHANGESTATE, 0, -1, -1, -1);
    if(!ioreq->io_Error && ioreq->io_Actual) {
        fprintf(stderr,"No disk in %s unit %d !\n", dev_name, dev_unit);
        retstatus = 0;
    } else {
        int tr = 0;
        int write_protected = 0;
        /* check if disk is write-protected:
        myDoIO(ioreq, TD_PROTSTATUS, 0, -1, -1, -1);
        if(!ioreq->io_Error && ioreq->io_Actual) write_protected = 1;
        */

        /* inhibit device */
        if(inhibited) inhibited = dev_inhibit(name,1);

        /* read tracks */
        for(tr = 0; tr < 160; ++tr) {
            printf("Reading track %2d side %d of %s unit %d  \r",
                   tr/2, tr%2, dev_name, dev_unit);
            fflush(stdout);
            myDoIO(ioreq, CMD_READ, -1, tracklen*tr, tracklen, (LONG)buffer);
            if(ioreq->io_Error) printf("Err. on\n");
            if(fwrite(buffer, 1, tracklen, dst) != (unsigned int)tracklen) {
               retstatus = 0;
               break;
            }
        }

        /* ok everything done! */
        printf("                                                                        \r");
        fflush(stdout);

        /* stop motor */
        myDoIO(ioreq, TD_MOTOR, 0, -1, 0, -1);

        /* uninhibit */
        if(inhibited) dev_inhibit(name,0);
    }
    CloseDevice((struct IORequest*)ioreq);   } else retstatus = 0;
    FreeMem(buffer,tracklen);                } else retstatus = 0;
    DeleteStdIO(ioreq);                      } else retstatus = 0;
    DeletePort(port);                        } else retstatus = 0;
    return retstatus;
}

/****************************************************************************/
/*
 * Copy one raw disk to a file.
 */
/****i* januae-ami-disk/readdevice *****************************************
*
*   NAME
*       readdevice() -- Copy a native device to a host file.
*
*   SYNOPSIS
*       int readdevice(char *name, char *dst);
*
*   FUNCTION
*       Parse the device identifier and either probe the unit or open the
*       destination file and copy its raw tracks. Restore the prior SIGINT
*       action when supported.
*
*   INPUTS
*       name - Device identifier. dst - Destination filename, or NULL for a
*       device-existence probe.
*
*   RESULT
*       1 when the probe or raw-copy operation reports success; otherwise 0.
*
****************************************************************************
*
*/

int readdevice(char *name, char *dst)
{   /* erhm, I must admit this code is long and ugly! */
    FILE *f = NULL;
    char *device_name;
    int   device_unit;
    int   retstatus = 0;
#ifdef HAVE_SIGACTION
    struct sigaction oldsa;
    int oldsa_valid;

    /* disable break */
    oldsa_valid = (0==sigaction(SIGINT, NULL, &oldsa));
    signal(SIGINT, SIG_IGN); /* <--- gcc complains about something */
                             /* in there but I don't know why. */
#endif

    /* get device name & unit */
    extract_dev_unit(name, &device_name, &device_unit);
    if(device_name) {
        /* if no destination then just check if the device exists */
        if(dst == NULL)
           retstatus = device_exists(device_name, device_unit);
        else {
            /* open dest file */
            if((f = fopen(dst,"wb"))) {
                retstatus = raw_copy(device_name, device_unit, f);
                fclose(f);
            }
        }
        free(device_name);
    }

#ifdef HAVE_SIGACTION
    /* enable break */
    if(oldsa_valid) sigaction(SIGINT, &oldsa, NULL);
#endif

    return retstatus;
}

/****************************************************************************/

/****i* januae-ami-disk/myDoIO *********************************************
*
*   NAME
*       myDoIO() -- Submit a device I/O request.
*
*   SYNOPSIS
*       static void myDoIO(struct IOStdReq *ioreq, LONG CMD, LONG FLAGS,
*       LONG OFFSET, LONG LENGTH, LONG DATA);
*
*   FUNCTION
*       Update selected IOStdReq fields and submit the request synchronously
*       with DoIO.
*
*   INPUTS
*       ioreq - I/O request. CMD, FLAGS, OFFSET, LENGTH, DATA - Field
*       replacements; a negative value preserves each field.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Command completion status remains in the I/O request.
*
****************************************************************************
*
*/

static void myDoIO(struct IOStdReq *ioreq, LONG CMD, LONG FLAGS, LONG OFFSET,
                   LONG LENGTH, LONG DATA)
{
    if(CMD>=0)    ioreq->io_Command = CMD;
    if(FLAGS>=0)  ioreq->io_Flags   = FLAGS;
    if(OFFSET>=0) ioreq->io_Offset  = OFFSET;
    if(LENGTH>=0) ioreq->io_Length  = LENGTH;
    if(DATA>=0)   ioreq->io_Data    = (void*)DATA;
    DoIO((struct IORequest*)ioreq);
}

/****************************************************************************/
/*
 * Prevents DOS to access a DFx device.
 */
/****i* januae-ami-disk/dev_inhibit ****************************************
*
*   NAME
*       dev_inhibit() -- Toggle DOS device access.
*
*   SYNOPSIS
*       static int dev_inhibit (char *dev, int on);
*
*   FUNCTION
*       Resolve the DOS device process for a volume name and send
*       ACTION_INHIBIT with the requested state.
*
*   INPUTS
*       dev - Device name, with an optional trailing colon. on - Nonzero to
*       inhibit the device; zero to allow access.
*
*   RESULT
*       1 when an inhibit request is dispatched; otherwise 0.
*
*   NOTES
*       The uninhibit path sends its DOS packet but returns zero.
*
****************************************************************************
*
*/

static int dev_inhibit (char *dev, int on)
{
    char  buff[10];
    char *s;
    struct MsgPort *DevPort;

    if (!*dev)
	return 0;

    s = dev;
	while(*s++);

    if (s[-2] == ':')
	strcpy (buff, dev);
    else
	sprintf (buff, "%s:", dev);

    if ((DevPort = (struct MsgPort*) DeviceProc ((STRPTR)buff))) {
	if (on) {
	    DoPkt (DevPort, ACTION_INHIBIT, DOSTRUE, 0, 0, 0, 0);
	    return 1;
	}
	else
	    DoPkt (DevPort, ACTION_INHIBIT, DOSFALSE, 0, 0, 0, 0);
    }
    return 0;
}
