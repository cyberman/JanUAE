 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Unix file system handler for AmigaDOS
  *
  * Copyright 1996 Ed Hanway
  * Copyright 1996, 1997 Bernd Schmidt
  * Copyright 2009, 2010 Oliver Brunner
  *
  * Version 0.4: 970308
  *
  * Based on example code (c) 1988 The Software Distillery
  * and published in Transactor for the Amiga, Volume 2, Issues 2-5.
  * (May - August 1989)
  *
  * Known limitations:
  * Does not support ACTION_INHIBIT (big deal).
  * Does not support several 2.0+ packet types.
  * Does not support removable volumes.
  * May not return the correct error code in some cases.
  * Does not check for sane values passed by AmigaDOS.  May crash the emulation
  * if passed garbage values.
  * Could do tighter checks on malloc return values.
  * Will probably fail spectacularly in some cases if the filesystem is
  * modified at the same time by another process while UAE is running.
  *
  * $Id$
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "threaddep/thread.h"
#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "events.h"
#include "newcpu.h"
#include "filesys.h"
#include "autoconf.h"
#include "traps.h"
#include "fsusage.h"
#include "native2amiga.h"
#include "scsidev.h"
#include "fsdb.h"
#include "zfile.h"
#include "gui.h"
#include "savestate.h"
#include "filesys_internal.h"

#if defined TARGET_AMIGAOS || defined __AROS__
# include <dos/dos.h>
# include <proto/dos.h>
#endif

#define TRACING_ENABLED 0
#if TRACING_ENABLED
#define TRACE(x)        do { kprintf("FS %s: ",__func__);kprintf x; } while(0)
#define DUMPLOCK(u,x)   dumplock(u,x)
#else
#define TRACE(x)
#define DUMPLOCK(u,x)
#endif

/* try to get some snoopdos functionality */
//#define SNOOP_ENABLED 1
#if SNOOP_ENABLED
#define SNOOP(...)      kprintf("FSNOOP %s: ",__PRETTY_FUNCTION__);kprintf(__VA_ARGS__)
#else
#define SNOOP(...)
#endif



/****i* januae-filesys-internal/aino_test
 *
 *   NAME
 *       aino_test()
 *
 *   SYNOPSIS
 *       static void aino_test (a_inode *aino)
 *
 *   FUNCTION
 *       Inspect inode recycling links when inode diagnostics are enabled.
 *
 *   INPUTS
 *       aino -- Inode at which to begin checking the linked list.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       When AINO_DEBUG is not defined, this function does nothing. The diagnostic path reports an invalid checksum or broken backward link; it does not repair either condition.
 *
 ****************************************************************************/

static void aino_test (a_inode *aino)
{
#ifdef AINO_DEBUG
    a_inode *aino2 = aino, *aino3;
    for (;;) {
        if (!aino || !aino->next)
            return;
        if ((aino->checksum1 ^ aino->checksum2) != 0xaaaa5555) {
            write_log ("PANIC: corrupted or freed but used aino detected!", aino);
        }
        aino3 = aino;
        aino = aino->next;
        if (aino->prev != aino3) {
            write_log ("PANIC: corrupted aino linking!\n");
            break;
        }
        if (aino == aino2) break;
    }
#endif
}

/****i* januae-filesys-internal/aino_test_init
 *
 *   NAME
 *       aino_test_init()
 *
 *   SYNOPSIS
 *       static void aino_test_init (a_inode *aino)
 *
 *   FUNCTION
 *       Initialize the diagnostic checksum fields of an inode.
 *
 *   INPUTS
 *       aino -- Inode whose diagnostic fields are initialized.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       The fields are written only when AINO_DEBUG is defined; otherwise the function has no effect.
 *
 ****************************************************************************/

static void aino_test_init (a_inode *aino)
{
#ifdef AINO_DEBUG
    aino->checksum1 = (uae_u32)aino;
    aino->checksum2 = aino->checksum1 ^ 0xaaaa5555;
#endif
}

/*
 * This _should_ be no issue for us, but let's rather use a guaranteed
 * thread safe function if we have one.
 * This used to be broken in glibc versions <= 2.0.1 (I think). I hope
 * no one is using this these days.
 * Michael Krause says it's also broken in libc5. ARRRGHHHHHH!!!!
 */
#if 0 && defined HAVE_READDIR_R

static struct dirent *my_readdir (DIR *dirstream, struct dirent *space)
{
    struct dirent *loc;
    if (readdir_r (dirstream, space, &loc) == 0) {
        /* Success */
        return loc;
    }
    return 0;
}

#else
#define my_readdir(dirstream, space) readdir(dirstream)
#endif

uaecptr filesys_initcode;
static uae_u32 fsdevname, filesys_configdev;

#define FS_STARTUP 0
#define FS_GO_DOWN 1


struct uaedev_mount_info current_mountinfo;
struct uaedev_mount_info options_mountinfo;

/****i* januae-filesys-mount/nr_units
 *
 *   NAME
 *       nr_units()
 *
 *   SYNOPSIS
 *       int nr_units (struct uaedev_mount_info *mountinfo)
 *
 *   FUNCTION
 *       Return the number of configured filesystem units.
 *
 *   INPUTS
 *       mountinfo -- mount configuration whose unit count is queried.
 *
 *   RESULT
 *       The current value of mountinfo->num_units.
 *
 *   NOTES
 *       The caller must supply a valid mount configuration.
 *
 ****************************************************************************/

int nr_units (struct uaedev_mount_info *mountinfo)
{
    return mountinfo->num_units;
}

/****i* januae-filesys-mount/is_hardfile
 *
 *   NAME
 *       is_hardfile()
 *
 *   SYNOPSIS
 *       int is_hardfile (struct uaedev_mount_info *mountinfo, int unit_no)
 *
 *   FUNCTION
 *       Classify a configured filesystem or hardfile unit.
 *
 *   INPUTS
 *       mountinfo -- mount configuration, or NULL to use current_mountinfo.
 *       unit_no -- index of the unit to classify.
 *
 *   RESULT
 *       FILESYS_VIRTUAL, FILESYS_HARDDRIVE, FILESYS_HARDFILE_RDB, or FILESYS_HARDFILE.
 *
 *   NOTES
 *       This function does not validate unit_no. The caller must supply an existing unit index.
 *
 ****************************************************************************/

int is_hardfile (struct uaedev_mount_info *mountinfo, int unit_no)
{
    if (!mountinfo)
        mountinfo = &current_mountinfo;
    if (mountinfo->ui[unit_no].volname)
        return FILESYS_VIRTUAL;
    if (mountinfo->ui[unit_no].hf.secspertrack == 0) {
        if (mountinfo->ui[unit_no].hf.flags & 1)
            return FILESYS_HARDDRIVE;
        return FILESYS_HARDFILE_RDB;
    }
    return FILESYS_HARDFILE;
}

/****i* januae-filesys-mount/close_filesys_unit
 *
 *   NAME
 *       close_filesys_unit()
 *
 *   SYNOPSIS
 *       static void close_filesys_unit (UnitInfo *uip)
 *
 *   FUNCTION
 *       Close an opened hardfile and release the owned unit resources.
 *
 *   INPUTS
 *       uip -- unit whose owned resources are released.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Closes the hardfile handle when present, frees the unit name and path strings and both pipes, and clears their fields.
 *
 ****************************************************************************/

static void close_filesys_unit (UnitInfo *uip)
{
    if (uip->hf.handle != 0)
        hdf_close (&uip->hf);
    if (uip->volname != 0)
        xfree (uip->volname);
    if (uip->devname != 0)
        xfree (uip->devname);
    if (uip->rootdir != 0)
        xfree (uip->rootdir);
    if (uip->unit_pipe)
        xfree (uip->unit_pipe);
    if (uip->back_pipe)
        xfree (uip->back_pipe);

    uip->unit_pipe = 0;
    uip->back_pipe = 0;

    uip->hf.handle = 0;
    uip->volname = 0;
    uip->devname = 0;
    uip->rootdir = 0;
}

/****i* januae-filesys-internal/get_filesys_unit
 *
 *   NAME
 *       get_filesys_unit()
 *
 *   SYNOPSIS
 *       const char *get_filesys_unit (struct uaedev_mount_info *mountinfo, int nr,
 *       char **devname, char **volname, char **rootdir, int *readonly,
 *       int *secspertrack, int *surfaces, int *reserved,
 *       int *cylinders, uae_u64 *size, int *blocksize, int *bootpri,
 *       char **filesysdir, int *flags)
 *
 *   FUNCTION
 *       Copy the configuration of a selected mount unit into caller-provided output fields.
 *
 *   INPUTS
 *       mountinfo -- Mount configuration to inspect.
 *       nr -- Unit index.
 *       devname, volname, rootdir -- Receivers for allocated strings or null pointers.
 *       readonly, secspertrack, surfaces, reserved, cylinders, size, blocksize, bootpri -- Receivers for the stored unit properties.
 *       filesysdir -- Optional receiver for the allocated filesystem directory string.
 *       flags -- Optional receiver, set to zero.
 *
 *   RESULT
 *       Null on success; an error string when the index is greater than or equal to the number of units.
 *
 *   NOTES
 *       The caller owns the duplicated strings. Negative unit indices are not rejected. There are no corresponding output writes on the reported index-error path.
 *
 ****************************************************************************/

const char *get_filesys_unit (struct uaedev_mount_info *mountinfo, int nr,
                              char **devname, char **volname, char **rootdir, int *readonly,
                              int *secspertrack, int *surfaces, int *reserved,
                              int *cylinders, uae_u64 *size, int *blocksize, int *bootpri,
                              char **filesysdir, int *flags)
{
    UnitInfo *uip = mountinfo->ui + nr;

    if (nr >= mountinfo->num_units)
        return "No slot allocated for this unit";

    *volname = uip->volname ? my_strdup (uip->volname) : 0;
    if (uip->devname == 0 || strlen(uip->devname) == 0) {
        *devname = xmalloc (10);
        sprintf (*devname, "DH%d", nr);
    } else {
        *devname = my_strdup (uip->devname);
    }
    *rootdir = uip->rootdir ? my_strdup (uip->rootdir) : 0;
    *readonly = uip->readonly;
    *secspertrack = uip->hf.secspertrack;
    *surfaces = uip->hf.surfaces;
    *reserved = uip->hf.reservedblocks;
    *cylinders = uip->hf.nrcyls;
    *blocksize = uip->hf.blocksize;
    *size = uip->hf.size;
    *bootpri = uip->bootpri;
    if (flags)
        *flags = 0;
    if (filesysdir)
        *filesysdir = uip->filesysdir ? my_strdup (uip->filesysdir) : 0;
    return 0;
}

/****i* januae-filesys-internal/set_filesys_unit_1
 *
 *   NAME
 *       set_filesys_unit_1()
 *
 *   SYNOPSIS
 *       static const char *set_filesys_unit_1 (struct uaedev_mount_info *mountinfo, int nr,
 *       const char *devname, const char *volname, const char *rootdir,
 *       int readonly, int secspertrack, int surfaces, int reserved,
 *       int blocksize, int bootpri, const char *filesysdir, int flags)
 *
 *   FUNCTION
 *       Configure an allocated mount slot for a host directory or hardfile.
 *
 *   INPUTS
 *       mountinfo -- Mount configuration to modify.
 *       nr -- Existing unit index.
 *       devname, volname, rootdir -- Device, volume, and host path names.
 *       readonly -- Requested read-only mode.
 *       secspertrack, surfaces, reserved, blocksize -- Hardfile geometry.
 *       bootpri -- Boot priority.
 *       filesysdir -- Optional filesystem directory.
 *       flags -- Configuration flags.
 *
 *   RESULT
 *       Null on success; a literal or static error string when validation or opening fails.
 *
 *   NOTES
 *       A null volume name selects hardfile handling. A failed writable hardfile open may be retried read-only. The boot priority is limited to -128 through 127. On failure, the function may already have changed the selected slot; the caller must handle rollback.
 *
 ****************************************************************************/

static const char *set_filesys_unit_1 (struct uaedev_mount_info *mountinfo, int nr,
                                       const char *devname, const char *volname, const char *rootdir,
                                       int readonly, int secspertrack, int surfaces, int reserved,
                                       int blocksize, int bootpri, const char *filesysdir, int flags)
{
    UnitInfo *ui = mountinfo->ui + nr;
    static char errmsg[1024];
    int i;

    if (nr >= mountinfo->num_units)
        return "No slot allocated for this unit";

    for (i = 0; i < mountinfo->num_units; i++) {
        if (nr == i)
            continue;
        if (!strcasecmp (mountinfo->ui[i].rootdir, rootdir)) {
            sprintf (errmsg, "directory/hardfile '%s' already added", rootdir);
            return errmsg;
        }
    }

    TRACE(("devname:    %s\n", devname));
    TRACE(("volname:    %s\n", volname));
    TRACE(("rootdir:    %s\n", rootdir));
    TRACE(("filesysdir: %s\n", filesysdir));

    ui->devname = 0;
    ui->volname = 0;
    ui->rootdir = 0;
    ui->unit_pipe = 0;
    ui->back_pipe = 0;
    ui->hf.handle = 0;
    ui->bootpri = 0;
    ui->filesysdir = 0;

    if (volname != 0) {
        struct stat statbuf;
        memset (&statbuf, 0, sizeof (statbuf));
        ui->volname = my_strdup (volname);
#if defined(WIN32) && !defined(__MINGW32__)
        v = isspecialdrive (rootdir);
        if (v < 0) {
            sprintf (errmsg, "invalid drive '%s'", rootdir);
            return errmsg;
        }
        if (v == 0) {
#endif
            if (stat (rootdir, &statbuf) < 0) {
                sprintf (errmsg, "directory '%s' not found", rootdir);
                return errmsg;
            }
#ifdef WIN32
            if (!(statbuf.st_mode & FILEFLAG_WRITE)) {
                write_log ("'%s' set to read-only\n", rootdir);
                readonly = 1;
            }
#else
            /* Check if the filesystem which contains rootdir is read-only */
            if (filesys_is_readonly (rootdir) && !readonly) {
                write_log ("Mounting '%s' as read-only\n", rootdir);
                readonly = 1;
        }
#endif
#if defined(WIN32) && !defined(__MINGW32__)
        }
#endif
    } else {
        ui->hf.secspertrack = secspertrack;
        ui->hf.surfaces = surfaces;
        ui->hf.reservedblocks = reserved;
        ui->hf.blocksize = blocksize;
        ui->volname = 0;
        ui->hf.readonly = readonly;
        if (!hdf_open (&ui->hf, rootdir) && !readonly) {
            write_log ("Failed to open hardfile with write permission. Trying as read-only\n");
            ui->hf.readonly = readonly = 1;
            hdf_open (&ui->hf, rootdir);
        }
        ui->hf.readonly = readonly;
        if (ui->hf.handle == 0)
            return "Hardfile not found";
        if ((ui->hf.blocksize & (ui->hf.blocksize - 1)) != 0 || ui->hf.blocksize == 0)
            return "Bad blocksize";
        if ((ui->hf.secspertrack || ui->hf.surfaces || ui->hf.reservedblocks) &&
            (ui->hf.secspertrack < 1 || ui->hf.surfaces < 1 || ui->hf.surfaces > 1023 ||
            ui->hf.reservedblocks < 0 || ui->hf.reservedblocks > 1023) != 0)
            return "Bad hardfile geometry";
        if (ui->hf.blocksize > ui->hf.size || ui->hf.size == 0)
            return "Hardfile too small";
        ui->hf.nrcyls = (int)(ui->hf.secspertrack * ui->hf.surfaces ? (ui->hf.size / ui->hf.blocksize) / (ui->hf.secspertrack * ui->hf.surfaces) : 0);
    }
    ui->self = 0;
    ui->reset_state = FS_STARTUP;
    ui->rootdir = my_strdup (rootdir);
    if (devname != 0 && strlen (devname) != 0)
        ui->devname = my_strdup (devname);
    if (filesysdir)
        ui->filesysdir = my_strdup (filesysdir);
    ui->readonly = readonly;
    if (bootpri < -128) bootpri = -128;
    if (bootpri > 127) bootpri = 127;
    ui->bootpri = bootpri;

    return 0;
}

/****i* januae-filesys-internal/set_filesys_unit
 *
 *   NAME
 *       set_filesys_unit()
 *
 *   SYNOPSIS
 *       const char *set_filesys_unit (struct uaedev_mount_info *mountinfo, int nr,
 *       const char *devname, const char *volname, const char *rootdir,
 *       int readonly, int secspertrack, int surfaces, int reserved,
 *       int blocksize, int bootpri, const char *filesysdir, int flags)
 *
 *   FUNCTION
 *       Replace a mount unit configuration while retaining the previous slot contents on an error.
 *
 *   INPUTS
 *       mountinfo -- Mount configuration to modify.
 *       nr -- Index of the unit to replace.
 *       devname, volname, rootdir -- Device, volume, and host path names.
 *       readonly -- Requested read-only mode.
 *       secspertrack, surfaces, reserved, blocksize -- Hardfile geometry.
 *       bootpri -- Boot priority.
 *       filesysdir -- Optional filesystem directory.
 *       flags -- Configuration flags.
 *
 *   RESULT
 *       Null on success; an error string returned by set_filesys_unit_1() otherwise.
 *
 *   NOTES
 *       The previous hardfile handle is closed before the replacement attempt. On failure the saved unit structure is restored; restoration does not reopen that handle. This function does not validate nr before indexing the unit array.
 *
 ****************************************************************************/

const char *set_filesys_unit (struct uaedev_mount_info *mountinfo, int nr,
                              const char *devname, const char *volname, const char *rootdir,
                              int readonly, int secspertrack, int surfaces, int reserved,
                              int blocksize, int bootpri, const char *filesysdir, int flags)
{
    const char *result;
    UnitInfo ui = mountinfo->ui[nr];

    hdf_close (&ui.hf);
    result = set_filesys_unit_1 (mountinfo, nr, devname, volname, rootdir, readonly,
        secspertrack, surfaces, reserved, blocksize, bootpri, filesysdir, flags);
    if (result)
        mountinfo->ui[nr] = ui;
    else
        close_filesys_unit (&ui);

    return result;
}

/****i* januae-filesys-internal/add_filesys_unit
 *
 *   NAME
 *       add_filesys_unit()
 *
 *   SYNOPSIS
 *       const char *add_filesys_unit (struct uaedev_mount_info *mountinfo,
 *       const char *devname, const char *volname, const char *rootdir,
 *       int readonly, int secspertrack, int surfaces, int reserved,
 *       int blocksize, int bootpri, const char *filesysdir, int flags)
 *
 *   FUNCTION
 *       Append a directory or hardfile configuration to the mount-unit table.
 *
 *   INPUTS
 *       mountinfo -- Mount configuration to extend.
 *       devname, volname, rootdir -- Device, volume, and host path names.
 *       readonly -- Requested read-only mode.
 *       secspertrack, surfaces, reserved, blocksize -- Hardfile geometry.
 *       bootpri -- Boot priority.
 *       filesysdir -- Optional filesystem directory.
 *       flags -- Configuration flags.
 *
 *   RESULT
 *       Null on success; an error string if the table is full or unit configuration fails.
 *
 *   NOTES
 *       The unit count is incremented before configuration and decremented if configuration fails. The function does not otherwise discard partial slot changes on failure.
 *
 ****************************************************************************/

const char *add_filesys_unit (struct uaedev_mount_info *mountinfo,
                              const char *devname, const char *volname, const char *rootdir,
                              int readonly, int secspertrack, int surfaces, int reserved,
                              int blocksize, int bootpri, const char *filesysdir, int flags)
{
    const char *retval;
    int nr = mountinfo->num_units;
    UnitInfo *uip = mountinfo->ui + nr;

    if (nr >= MAX_FILESYSTEM_UNITS)
        return "Maximum number of file systems mounted";

    mountinfo->num_units++;
    retval = set_filesys_unit_1 (mountinfo, nr, devname, volname, rootdir, readonly,
                                 secspertrack, surfaces, reserved, blocksize, bootpri, filesysdir, flags);
    if (retval)
        mountinfo->num_units--;
    return retval;
}

/****i* januae-filesys-mount/kill_filesys_unit
 *
 *   NAME
 *       kill_filesys_unit()
 *
 *   SYNOPSIS
 *       int kill_filesys_unit (struct uaedev_mount_info *mountinfo, int nr)
 *
 *   FUNCTION
 *       Remove a configured unit and close its owned resources.
 *
 *   INPUTS
 *       mountinfo -- mount configuration to modify.
 *       nr -- index of the unit to remove.
 *
 *   RESULT
 *       Zero on success; -1 for an index outside the configured unit range.
 *
 *   NOTES
 *       Subsequent units shift down one position. The caller supplies a valid mountinfo pointer.
 *
 ****************************************************************************/

int kill_filesys_unit (struct uaedev_mount_info *mountinfo, int nr)
{
    UnitInfo *uip = mountinfo->ui;
    if (nr >= mountinfo->num_units || nr < 0)
        return -1;

    close_filesys_unit (mountinfo->ui + nr);

    mountinfo->num_units--;
    for (; nr < mountinfo->num_units; nr++) {
        uip[nr] = uip[nr+1];
    }
    return 0;
}

/****i* januae-filesys-mount/move_filesys_unit
 *
 *   NAME
 *       move_filesys_unit()
 *
 *   SYNOPSIS
 *       int move_filesys_unit (struct uaedev_mount_info *mountinfo, int nr, int to)
 *
 *   FUNCTION
 *       Reorder one configured unit without closing its resources.
 *
 *   INPUTS
 *       mountinfo -- mount configuration to modify.
 *       nr -- existing source index.
 *       to -- destination index.
 *
 *   RESULT
 *       Zero on success; -1 for invalid indices or identical source and destination.
 *
 *   NOTES
 *       Intermediate entries are shifted to make room for the moved unit.
 *
 ****************************************************************************/

int move_filesys_unit (struct uaedev_mount_info *mountinfo, int nr, int to)
{
    UnitInfo tmpui;
    UnitInfo *uip = mountinfo->ui;

    if (nr >= mountinfo->num_units || nr < 0
        || to >= mountinfo->num_units || to < 0
        || to == nr)
        return -1;
    tmpui = uip[nr];
    if (to > nr) {
        int i;
        for (i = nr; i < to; i++)
            uip[i] = uip[i + 1];
    } else {
        int i;
        for (i = nr; i > to; i--)
            uip[i] = uip[i - 1];
    }
    uip[to] = tmpui;
    return 0;
}

/****i* januae-filesys-mount/sprintf_filesys_unit
 *
 *   NAME
 *       sprintf_filesys_unit()
 *
 *   SYNOPSIS
 *       int sprintf_filesys_unit (const struct uaedev_mount_info *mountinfo, char *buffer, int num)
 *
 *   FUNCTION
 *       Format a short description of a configured filesystem or hardfile.
 *
 *   INPUTS
 *       mountinfo -- mount configuration.
 *       buffer -- caller-provided output buffer.
 *       num -- index to describe.
 *
 *   RESULT
 *       Zero on success; -1 if num is greater than or equal to the unit count.
 *
 *   NOTES
 *       The function uses sprintf without a buffer-size argument and does not reject negative indices. The caller must provide adequate storage and a valid index.
 *
 ****************************************************************************/

int sprintf_filesys_unit (const struct uaedev_mount_info *mountinfo, char *buffer, int num)
{
    const UnitInfo *uip = mountinfo->ui;
    if (num >= mountinfo->num_units)
        return -1;

    if (uip[num].volname != 0)
        sprintf (buffer, "(DH%d:) Filesystem, %s: %s %s", num, uip[num].volname,
                 uip[num].rootdir, uip[num].readonly ? "ro" : "");
    else
        sprintf (buffer, "(DH%d:) Hardfile, \"%s\", size %d Mbytes", num,
        uip[num].rootdir, uip[num].hf.size / (1024 * 1024));
    return 0;
}

/****i* januae-filesys-mount/write_filesys_config
 *
 *   NAME
 *       write_filesys_config()
 *
 *   SYNOPSIS
 *       void write_filesys_config (const struct uaedev_mount_info *mountinfo, const char *unexpanded, const char *default_path, FILE *f)
 *
 *   FUNCTION
 *       Write configuration entries for all mounted filesystem and hardfile units.
 *
 *   INPUTS
 *       mountinfo -- mount configuration to serialize.
 *       unexpanded -- substitution token for default-path expansion.
 *       default_path -- path used by cfgfile_subst_path.
 *       f -- open output stream.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Writes the current and legacy filesystem or hardfile entry forms for each unit. The function does not report stream write errors.
 *
 ****************************************************************************/

void write_filesys_config (const struct uaedev_mount_info *mountinfo,
                           const char *unexpanded, const char *default_path, FILE *f)
{
    const UnitInfo *uip = mountinfo->ui;
    int i;
    char tmp[MAX_DPATH];

    for (i = 0; i < mountinfo->num_units; i++) {
        char *str;
        str = cfgfile_subst_path (default_path, unexpanded, uip[i].rootdir);
        if (uip[i].volname != 0) {
            fprintf (f, "filesystem2=%s,%s:%s:%s,%d\n", uip[i].readonly ? "ro" : "rw",
                uip[i].devname ? uip[i].devname : "", uip[i].volname, str, uip[i].bootpri);
            fprintf (f, "filesystem=%s,%s:%s\n", uip[i].readonly ? "ro" : "rw",
                uip[i].volname, str);
        } else {
            fprintf (f, "hardfile2=%s,%s:%s,%d,%d,%d,%d,%d,%s\n",
                     uip[i].readonly ? "ro" : "rw",
                     uip[i].devname ? uip[i].devname : "", str,
                     uip[i].hf.secspertrack, uip[i].hf.surfaces, uip[i].hf.reservedblocks, uip[i].hf.blocksize,
                     uip[i].bootpri,uip[i].filesysdir ? uip[i].filesysdir : "");
            fprintf (f, "hardfile=%s,%d,%d,%d,%d,%s\n",
                     uip[i].readonly ? "ro" : "rw", uip[i].hf.secspertrack,
                     uip[i].hf.surfaces, uip[i].hf.reservedblocks, uip[i].hf.blocksize, str);
        }
        xfree (str);
    }
}

/****i* januae-filesys-mount/dup_mountinfo
 *
 *   NAME
 *       dup_mountinfo()
 *
 *   SYNOPSIS
 *       static void dup_mountinfo (const struct uaedev_mount_info *mip, struct uaedev_mount_info *mip_d)
 *
 *   FUNCTION
 *       Copy mount configuration and duplicate resources owned by its units.
 *
 *   INPUTS
 *       mip -- source mount configuration.
 *       mip_d -- destination mount configuration.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Copies the structure before duplicating the configured volume, device, and root-directory names and nonzero hardfile handles.
 *
 ****************************************************************************/

static void dup_mountinfo (const struct uaedev_mount_info *mip, struct uaedev_mount_info *mip_d)
{
    int i;
    struct uaedev_mount_info *i2 = mip_d;

    memcpy (i2, mip, sizeof *i2);

    for (i = 0; i < i2->num_units; i++) {
        UnitInfo *uip = i2->ui + i;
        if (uip->volname)
            uip->volname = my_strdup (uip->volname);
        if (uip->devname)
            uip->devname = my_strdup (uip->devname);
        if (uip->rootdir)
            uip->rootdir = my_strdup (uip->rootdir);
        if (uip->hf.handle)
            hdf_dup (&uip->hf, &uip->hf);
    }
}

/****i* januae-filesys-mount/free_mountinfo
 *
 *   NAME
 *       free_mountinfo()
 *
 *   SYNOPSIS
 *       void free_mountinfo (struct uaedev_mount_info *mip)
 *
 *   FUNCTION
 *       Release all units in a mount configuration.
 *
 *   INPUTS
 *       mip -- mount configuration, or NULL for no operation.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Closes each configured unit and resets the unit count to zero. The mountinfo structure itself is not freed.
 *
 ****************************************************************************/

void free_mountinfo (struct uaedev_mount_info *mip)
{
    int i;
    if (!mip)
        return;
    for (i = 0; i < mip->num_units; i++)
        close_filesys_unit (mip->ui + i);
    mip->num_units = 0;
}

/****i* januae-filesys-internal/get_hardfile_data
 *
 *   NAME
 *       get_hardfile_data()
 *
 *   SYNOPSIS
 *       struct hardfiledata *get_hardfile_data (int nr)
 *
 *   FUNCTION
 *       Return the hardfile descriptor associated with a configured mount unit.
 *
 *   INPUTS
 *       nr -- Index in current_mountinfo.
 *
 *   RESULT
 *       Pointer to the stored hardfiledata structure, or null for an out-of-range index or a directory-backed unit.
 *
 *   NOTES
 *       The returned pointer is borrowed from current_mountinfo and is invalidated if the owning mount configuration changes or is released.
 *
 ****************************************************************************/

struct hardfiledata *get_hardfile_data (int nr)
{
    UnitInfo *uip = current_mountinfo.ui;
    if (nr < 0 || nr >= current_mountinfo.num_units || uip[nr].volname != 0)
        return 0;
    return &uip[nr].hf;
}

/* minimal AmigaDOS definitions */

/* field offsets in DosPacket */
#define dp_Type 8
#define dp_Res1 12
#define dp_Res2 16
#define dp_Arg1 20
#define dp_Arg2 24
#define dp_Arg3 28
#define dp_Arg4 32

/* result codes */
#define DOS_TRUE ((unsigned int)-1L)
#define DOS_FALSE (0L)

/* on amigaoid hosts, this is already defined */
#ifndef DOS_DOS_H

/* Passed as type to Lock() */
#define SHARED_LOCK         -2     /* File is readable by others */
#define ACCESS_READ         -2     /* Synonym */
#define EXCLUSIVE_LOCK      -1     /* No other access allowed    */
#define ACCESS_WRITE        -1     /* Synonym */

#endif

/* packet types */
#define ACTION_CURRENT_VOLUME   7
#define ACTION_LOCATE_OBJECT    8
#define ACTION_RENAME_DISK      9
#define ACTION_FREE_LOCK        15
#define ACTION_DELETE_OBJECT    16
#define ACTION_RENAME_OBJECT    17
#define ACTION_COPY_DIR         19
#define ACTION_SET_PROTECT      21
#define ACTION_CREATE_DIR       22
#define ACTION_EXAMINE_OBJECT   23
#define ACTION_EXAMINE_NEXT     24
#define ACTION_DISK_INFO        25
#define ACTION_INFO             26
#define ACTION_FLUSH            27
#define ACTION_SET_COMMENT      28
#define ACTION_PARENT           29
#define ACTION_SET_DATE         34
#define ACTION_FIND_WRITE       1004
#define ACTION_FIND_INPUT       1005
#define ACTION_FIND_OUTPUT      1006
#define ACTION_END              1007
#define ACTION_SEEK             1008
#define ACTION_IS_FILESYSTEM    1027
#define ACTION_READ             'R'
#define ACTION_WRITE            'W'

/* 2.0+ packet types */
#define ACTION_INHIBIT          31
#define ACTION_SET_FILE_SIZE    1022
#define ACTION_LOCK_RECORD      2008
#define ACTION_FREE_RECORD      2009
#define ACTION_SAME_LOCK        40
#define ACTION_CHANGE_MODE      1028
#define ACTION_FH_FROM_LOCK     1026
#define ACTION_COPY_DIR_FH      1030
#define ACTION_PARENT_FH        1031
#define ACTION_EXAMINE_FH       1034
#define ACTION_EXAMINE_ALL      1033
#define ACTION_MAKE_LINK        1021
#define ACTION_READ_LINK        1024
#define ACTION_FORMAT           1020
#define ACTION_IS_FILESYSTEM    1027
#define ACTION_ADD_NOTIFY       4097
#define ACTION_REMOVE_NOTIFY    4098

#define DISK_TYPE               0x444f5301 /* DOS\1 */

typedef struct {
    uae_u32 uniq;
    /* The directory we're going through.  */
    a_inode *aino;
    /* The file we're going to look up next.  */
    a_inode *curr_file;
} ExamineKey;

typedef struct key {
    struct key *next;
    a_inode *aino;
    uae_u32 uniq;
    int fd;
    off_t file_pos;
    int dosmode;
    int createmode;
    int notifyactive;
} Key;

typedef struct notify {
    struct notify *next;
    uaecptr notifyrequest;
    char *fullname;
    char *partname;
} Notify;

/* Since ACTION_EXAMINE_NEXT is so braindamaged, we have to keep
 * some of these around
 */

#define EXKEYS 100
#define MAX_AINO_HASH 128
#define NOTIFY_HASH_SIZE 127

/* handler state info */

typedef struct _unit {
    struct _unit *next;

    /* Amiga stuff */
    uaecptr dosbase;
    uaecptr volume;
    uaecptr port;       /* Our port */
    uaecptr locklist;

    /* Native stuff */
    uae_s32 unit;       /* unit number */
    UnitInfo ui;        /* unit startup info */
    char tmpbuf3[256];

    /* Dummy message processing */
    uaecptr dummy_message;
    volatile unsigned int cmds_sent;
    volatile unsigned int cmds_complete;
    volatile unsigned int cmds_acked;

    /* ExKeys */
    ExamineKey examine_keys[EXKEYS];
    int next_exkey;
    unsigned long total_locked_ainos;

    /* Keys */
    struct key *keys;
    uae_u32 key_uniq;
    uae_u32 a_uniq;

    a_inode rootnode;
    unsigned long aino_cache_size;
    a_inode *aino_hash[MAX_AINO_HASH];
    unsigned long nr_cache_hits;
    unsigned long nr_cache_lookups;

    struct notify *notifyhash[NOTIFY_HASH_SIZE];

} Unit;

typedef uae_u8 *dpacket;
#define PUT_PCK_RES1(p,v) do { do_put_mem_long ((uae_u32 *)((p) + dp_Res1), (v)); } while (0)
#define PUT_PCK_RES2(p,v) do { do_put_mem_long ((uae_u32 *)((p) + dp_Res2), (v)); } while (0)
#define GET_PCK_TYPE(p) ((uae_s32)(do_get_mem_long ((uae_u32 *)((p) + dp_Type))))
#define GET_PCK_RES1(p) ((uae_s32)(do_get_mem_long ((uae_u32 *)((p) + dp_Res1))))
#define GET_PCK_RES2(p) ((uae_s32)(do_get_mem_long ((uae_u32 *)((p) + dp_Res2))))
#define GET_PCK_ARG1(p) ((uae_s32)(do_get_mem_long ((uae_u32 *)((p) + dp_Arg1))))
#define GET_PCK_ARG2(p) ((uae_s32)(do_get_mem_long ((uae_u32 *)((p) + dp_Arg2))))
#define GET_PCK_ARG3(p) ((uae_s32)(do_get_mem_long ((uae_u32 *)((p) + dp_Arg3))))
#define GET_PCK_ARG4(p) ((uae_s32)(do_get_mem_long ((uae_u32 *)((p) + dp_Arg4))))

/****i* januae-filesys-internal/char1
 *
 *   NAME
 *       char1()
 *
 *   SYNOPSIS
 *       static char *char1 (uaecptr addr)
 *
 *   FUNCTION
 *       Read a null-terminated guest string into a shared scratch buffer.
 *
 *   INPUTS
 *       addr -- Guest address of the first character.
 *
 *   RESULT
 *       A pointer to an internal 1024-byte buffer, overwritten by a subsequent call.
 *
 *   NOTES
 *       The routine reads guest memory until a zero byte or the buffer limit. A string filling the buffer may not be terminated.
 *
 ****************************************************************************/

static char *char1 (uaecptr addr)
{
    static char buf[1024];
    unsigned int i = 0;
    do {
      buf[i] = get_byte(addr);
      addr++;
    } while (buf[i++] && i < sizeof(buf));
    return buf;
}

/****i* januae-filesys-internal/bstr1
 *
 *   NAME
 *       bstr1()
 *
 *   SYNOPSIS
 *       static char *bstr1 (uaecptr addr)
 *
 *   FUNCTION
 *       Convert a length-prefixed guest string into a shared C string.
 *
 *   INPUTS
 *       addr -- Guest address of the length byte.
 *
 *   RESULT
 *       A pointer to an internal 256-byte buffer, overwritten by a subsequent call.
 *
 *   NOTES
 *       The leading byte supplies the string length. The result is null-terminated.
 *
 ****************************************************************************/

static char *bstr1 (uaecptr addr)
{
    static char buf[256];
    int i;
    int n = get_byte(addr);
    addr++;

    for (i = 0; i < n; i++, addr++)
      buf[i] = get_byte(addr);
    buf[i] = 0;
    return buf;
}

/****i* januae-filesys-internal/bstr
 *
 *   NAME
 *       bstr()
 *
 *   SYNOPSIS
 *       static char *bstr (Unit *unit, uaecptr addr)
 *
 *   FUNCTION
 *       Copy a length-prefixed guest string into the unit scratch buffer.
 *
 *   INPUTS
 *       unit -- Unit whose temporary buffer receives the string.
 *       addr -- Guest address of the length byte.
 *
 *   RESULT
 *       A pointer to unit->tmpbuf3, overwritten by later uses of that buffer.
 *
 *   NOTES
 *       The leading byte supplies the string length; the result is null-terminated. The caller must provide a valid unit.
 *
 ****************************************************************************/

static char *bstr (Unit *unit, uaecptr addr)
{
    int i;
    int n = get_byte(addr);

    addr++;
    for (i = 0; i < n; i++, addr++)
        unit->tmpbuf3[i] = get_byte(addr);
    unit->tmpbuf3[i] = 0;
    return unit->tmpbuf3;
}

/****i* januae-filesys-internal/bstr_cut
 *
 *   NAME
 *       bstr_cut()
 *
 *   SYNOPSIS
 *       static char *bstr_cut (Unit *unit, uaecptr addr)
 *
 *   FUNCTION
 *       Copy a length-prefixed guest string and return its final path component.
 *
 *   INPUTS
 *       unit -- Unit whose temporary buffer receives the string.
 *       addr -- Guest address of the length byte.
 *
 *   RESULT
 *       A pointer into unit->tmpbuf3, after the last slash or first colon if present.
 *
 *   NOTES
 *       The copied string is null-terminated. The returned pointer is invalidated when the unit scratch buffer is reused.
 *
 ****************************************************************************/

static char *bstr_cut (Unit *unit, uaecptr addr)
{
    char *p = unit->tmpbuf3;
    int i, colon_seen = 0;
    int n = get_byte (addr);

    addr++;
    for (i = 0; i < n; i++, addr++) {
        uae_u8 c = get_byte(addr);
        unit->tmpbuf3[i] = c;
        if (c == '/' || (c == ':' && colon_seen++ == 0))
            p = unit->tmpbuf3 + i + 1;
    }
    unit->tmpbuf3[i] = 0;
    return p;
}

static Unit *units = 0;
static int unit_num = 0;

static Unit*
/****i* januae-filesys-internal/find_unit
 *
 *   NAME
 *       find_unit()
 *
 *   SYNOPSIS
 *       find_unit (uaecptr port)
 *
 *   FUNCTION
 *       Look up a filesystem unit by its guest message-port address.
 *
 *   INPUTS
 *       port -- Guest address of the unit message port.
 *
 *   RESULT
 *       The matching unit, or NULL if none is registered.
 *
 *   NOTES
 *       Searches the current linked list of units.
 *
 ****************************************************************************/

find_unit (uaecptr port)
{
    Unit* u;
    for (u = units; u; u = u->next)
        if (u->port == port)
            break;

    return u;
}

/****i* januae-filesys-internal/prepare_for_open
 *
 *   NAME
 *       prepare_for_open()
 *
 *   SYNOPSIS
 *       static void prepare_for_open (char *name)
 *
 *   FUNCTION
 *       Accept a host path at the file-opening hook.
 *
 *   INPUTS
 *       name -- Path supplied by the caller.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       The current implementation is empty and does not inspect or modify name.
 *
 ****************************************************************************/

static void prepare_for_open (char *name)
{
}

/****i* januae-filesys-internal/de_recycle_aino
 *
 *   NAME
 *       de_recycle_aino()
 *
 *   SYNOPSIS
 *       static void de_recycle_aino (Unit *unit, a_inode *aino)
 *
 *   FUNCTION
 *       Remove an inode from the unit recycling chain.
 *
 *   INPUTS
 *       unit -- Unit that owns the recycling chain.
 *       aino -- Inode to unlink.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Leaves the root node and any inode not linked into the recycling chain unchanged. Decrements the cache count when unlinking.
 *
 ****************************************************************************/

static void de_recycle_aino (Unit *unit, a_inode *aino)
{
    aino_test (aino);
    if (aino->next == 0 || aino == &unit->rootnode)
        return;
    aino->next->prev = aino->prev;
    aino->prev->next = aino->next;
    aino->next = aino->prev = 0;
    unit->aino_cache_size--;
}

/****i* januae-filesys-internal/dispose_aino
 *
 *   NAME
 *       dispose_aino()
 *
 *   SYNOPSIS
 *       static void dispose_aino (Unit *unit, a_inode **aip, a_inode *aino)
 *
 *   FUNCTION
 *       Remove an inode from its sibling list and free its allocated fields.
 *
 *   INPUTS
 *       unit -- Unit containing the inode hash table.
 *       aip -- Address of the sibling-list link referencing the inode.
 *       aino -- Inode to dispose.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Clears the matching inode hash entry, writes back dirty parent directory metadata when needed, advances the sibling link, and releases the inode and its owned strings. The inode must no longer be in use.
 *
 ****************************************************************************/

static void dispose_aino (Unit *unit, a_inode **aip, a_inode *aino)
{
    int hash = aino->uniq % MAX_AINO_HASH;
    if (unit->aino_hash[hash] == aino)
        unit->aino_hash[hash] = 0;

    if (aino->dirty && aino->parent)
        fsdb_dir_writeback (aino->parent);

    *aip = aino->sibling;
    xfree (aino->aname);
    if (aino->comment)
        xfree (aino->comment);
    xfree (aino->nname);
    xfree (aino);
}

/****i* januae-filesys-internal/recycle_aino
 *
 *   NAME
 *       recycle_aino()
 *
 *   SYNOPSIS
 *       static void recycle_aino (Unit *unit, a_inode *new_aino)
 *
 *   FUNCTION
 *       Add an unused inode to the unit recycling list.
 *
 *   INPUTS
 *       unit -- Filesystem unit that owns the inode cache.
 *       new_aino -- Inode eligible for recycling.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Directory inodes, locked inodes, and the unit root are left untouched. If the cache exceeds its threshold, the routine first discards eligible cached inodes. The inode must not already be in the recycling list.
 *
 ****************************************************************************/

static void recycle_aino (Unit *unit, a_inode *new_aino)
{
    aino_test (new_aino);
    if (new_aino->dir || new_aino->shlock > 0
        || new_aino->elock || new_aino == &unit->rootnode)
        /* Still in use */
        return;

    TRACE (("Recycling; cache size %d, total_locked %d\n",
            unit->aino_cache_size, unit->total_locked_ainos));
    if (unit->aino_cache_size > 5000 + unit->total_locked_ainos) {
        /* Reap a few. */
        int i = 0;
        while (i < 50) {
            a_inode *parent = unit->rootnode.prev->parent;
            a_inode **aip;
            aip = &parent->child;

            aino_test (parent);
            if (! parent->locked_children) {
                for (;;) {
                    a_inode *aino = *aip;
                    aino_test (aino);
                    if (aino == 0)
                        break;
                    /* Not recyclable if next == 0 (i.e., not chained into
                       recyclable list), or if parent directory is being
                       ExNext()ed.  */
                    if (aino->next == 0) {
                        aip = &aino->sibling;
                    } else {
                        if (aino->shlock > 0 || aino->elock)
                            write_log ("panic: freeing locked a_inode!\n");

                        de_recycle_aino (unit, aino);
                        dispose_aino (unit, aip, aino);
                        i++;
                    }
                }
            }
            /* In the previous loop, we went through all children of one
               parent.  Re-arrange the recycled list so that we'll find a
               different parent the next time around.  */
            do {
                unit->rootnode.next->prev = unit->rootnode.prev;
                unit->rootnode.prev->next = unit->rootnode.next;
                unit->rootnode.next = unit->rootnode.prev;
                unit->rootnode.prev = unit->rootnode.prev->prev;
                unit->rootnode.prev->next = unit->rootnode.next->prev = &unit->rootnode;
            } while (unit->rootnode.prev->parent == parent);
        }
    }

    aino_test (new_aino);
    /* Chain it into circular list. */
    new_aino->next = unit->rootnode.next;
    new_aino->prev = &unit->rootnode;
    new_aino->prev->next = new_aino;
    new_aino->next->prev = new_aino;
    aino_test (new_aino->next);
    aino_test (new_aino->prev);

    unit->aino_cache_size++;
}

/****i* januae-filesys-internal/update_child_names
 *
 *   NAME
 *       update_child_names()
 *
 *   SYNOPSIS
 *       static void update_child_names (Unit *unit, a_inode *a, a_inode *parent)
 *
 *   FUNCTION
 *       Rebuild host pathnames for a sibling chain and its descendants.
 *
 *   INPUTS
 *       unit -- Filesystem unit.
 *       a -- First inode in the sibling chain, or NULL.
 *       parent -- New parent inode.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Updates each inode parent pointer and replaces its allocated host pathname. Descendant directories are processed recursively. Existing pathnames must contain the filesystem directory separator.
 *
 ****************************************************************************/

static void update_child_names (Unit *unit, a_inode *a, a_inode *parent)
{
    int l0 = strlen (parent->nname) + 2;

    while (a != 0) {
        char *name_start;
        char *new_name;
        char dirsep[2] = { FSDB_DIR_SEPARATOR, '\0' };

        a->parent = parent;
#warning: care for ':' separator??
        name_start = strrchr (a->nname, FSDB_DIR_SEPARATOR);
        if (name_start == 0) {
            write_log ("malformed file name");
        }
        name_start++;
        new_name = (char *)xmalloc (strlen (name_start) + l0);
        strcpy (new_name, parent->nname);
        strcat (new_name, dirsep);
        strcat (new_name, name_start);
        xfree (a->nname);
        a->nname = new_name;
        if (a->child)
            update_child_names (unit, a->child, a);
        a = a->sibling;
    }
}

/****i* januae-filesys-internal/move_aino_children
 *
 *   NAME
 *       move_aino_children()
 *
 *   SYNOPSIS
 *       static void move_aino_children (Unit *unit, a_inode *from, a_inode *to)
 *
 *   FUNCTION
 *       Transfer a directory inode's children to another inode.
 *
 *   INPUTS
 *       unit -- Filesystem unit.
 *       from -- Source inode.
 *       to -- Destination inode.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Transfers the child-list pointer, clears the source child list, and updates the transferred nodes' parent pointers and host pathnames.
 *
 ****************************************************************************/

static void move_aino_children (Unit *unit, a_inode *from, a_inode *to)
{
    aino_test (from);
    aino_test (to);
    to->child = from->child;
    from->child = 0;
    update_child_names (unit, to->child, to);
}

/****i* januae-filesys-internal/delete_aino
 *
 *   NAME
 *       delete_aino()
 *
 *   SYNOPSIS
 *       static void delete_aino (Unit *unit, a_inode *aino)
 *
 *   FUNCTION
 *       Remove an inode from its parent directory cache.
 *
 *   INPUTS
 *       unit -- Filesystem unit.
 *       aino -- Inode to remove.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Marks the inode dirty and deleted, removes it from recycling, and advances matching directory enumeration cursors. If the inode is found in its parent sibling chain, its storage is released. Otherwise the routine logs an error and leaves the marked inode allocated.
 *
 ****************************************************************************/

static void delete_aino (Unit *unit, a_inode *aino)
{
    a_inode **aip;

    TRACE(("deleting aino %x\n", aino->uniq));

    aino_test (aino);
    aino->dirty = 1;
    aino->deleted = 1;
    de_recycle_aino (unit, aino);

    /* If any ExKeys are currently pointing at us, advance them.  */
    if (aino->parent->exnext_count > 0) {
        int i;
        TRACE(("entering exkey validation\n"));
        for (i = 0; i < EXKEYS; i++) {
            ExamineKey *k = unit->examine_keys + i;
            if (k->uniq == 0)
                continue;
            if (k->aino == aino->parent) {
                TRACE(("Same parent found for %d\n", i));
                if (k->curr_file == aino) {
                    k->curr_file = aino->sibling;
                    TRACE(("Advancing curr_file\n"));
                }
            }
        }
    }

    aip = &aino->parent->child;
    while (*aip != aino && *aip != 0)
        aip = &(*aip)->sibling;
    if (*aip != aino) {
        write_log ("Couldn't delete aino.\n");
        return;
    }
    dispose_aino (unit, aip, aino);
}

/****i* januae-filesys-internal/lookup_sub
 *
 *   NAME
 *       lookup_sub()
 *
 *   SYNOPSIS
 *       static a_inode *lookup_sub (a_inode *dir, uae_u32 uniq)
 *
 *   FUNCTION
 *       Search a directory subtree for an inode identifier.
 *
 *   INPUTS
 *       dir -- Root directory inode of the search.
 *       uniq -- Inode identifier.
 *
 *   RESULT
 *       The matching inode, or NULL if no match exists.
 *
 *   NOTES
 *       Searches child lists recursively through directory nodes. Moves a matched child to the front of its sibling list unless directory enumeration has locked the child order.
 *
 ****************************************************************************/

static a_inode *lookup_sub (a_inode *dir, uae_u32 uniq)
{
    a_inode **cp = &dir->child;
    a_inode *c, *retval;

    for (;;) {
        c = *cp;
        if (c == 0)
            return 0;

        if (c->uniq == uniq) {
            retval = c;
            break;
        }
        if (c->dir) {
            a_inode *a = lookup_sub (c, uniq);
            if (a != 0) {
                retval = a;
                break;
            }
        }
        cp = &c->sibling;
    }
    if (! dir->locked_children) {
        /* Move to the front to speed up repeated lookups.  Don't do this if
           an ExNext is going on in this directory, or we'll terminally
           confuse it.  */
        *cp = c->sibling;
        c->sibling = dir->child;
        TRACE(("c->sibling = dir->child: %lx\n", dir->child));
        dir->child = c;
    }
    return retval;
}

/****i* januae-filesys-internal/lookup_aino
 *
 *   NAME
 *       lookup_aino()
 *
 *   SYNOPSIS
 *       static a_inode *lookup_aino (Unit *unit, uae_u32 uniq)
 *
 *   FUNCTION
 *       Resolve an inode identifier through the unit cache and directory tree.
 *
 *   INPUTS
 *       unit -- Filesystem unit.
 *       uniq -- Inode identifier; zero denotes the unit root.
 *
 *   RESULT
 *       The matching inode, or NULL when the identifier cannot be resolved.
 *
 *   NOTES
 *       For nonzero identifiers, checks the hash slot before searching from the root. Updates lookup statistics and caches the result, including a missing result. The zero identifier returns the root immediately.
 *
 ****************************************************************************/

static a_inode *lookup_aino (Unit *unit, uae_u32 uniq)
{
    a_inode *a;
    int hash = uniq % MAX_AINO_HASH;

    if (uniq == 0)
        return &unit->rootnode;
    a = unit->aino_hash[hash];
    if (a == 0 || a->uniq != uniq)
        a = lookup_sub (&unit->rootnode, uniq);
    else
        unit->nr_cache_hits++;
    unit->nr_cache_lookups++;
    unit->aino_hash[hash] = a;
    aino_test (a);
    return a;
}

/****i* januae-filesys-internal/build_nname
 *
 *   NAME
 *       build_nname()
 *
 *   SYNOPSIS
 *       char *build_nname (const char *d, const char *n)
 *
 *   FUNCTION
 *       Construct a native path from a directory and entry name.
 *
 *   INPUTS
 *       d -- Native directory path.
 *       n -- Entry name to append.
 *
 *   RESULT
 *       A newly allocated path string; the caller releases it.
 *
 *   NOTES
 *       Adds a directory separator before the entry name, subject to the TARGET_AMIGAOS separator guard. No explicit allocation failure handling is performed.
 *
 ****************************************************************************/

char *build_nname (const char *d, const char *n)
{
    unsigned int   d_len = strlen (d);
    char *p = xmalloc (d_len + strlen (n) + 2);

    strcpy (p, d);

#ifdef TARGET_AMIGAOS
    if (d_len && p[d_len-1] != ':' && p[d_len-1] != FSDB_DIR_SEPARATOR)
#endif
        p[d_len++] = FSDB_DIR_SEPARATOR;

    strcpy (&p[d_len], n);

    return p;
}


/* This gets called to translate an Amiga name that some program used to
 * a name that we can use on the native filesystem.  */
/****i* januae-filesys-internal/get_nname
 *
 *   NAME
 *       get_nname()
 *
 *   SYNOPSIS
 *       static char *get_nname (Unit *unit, a_inode *base, char *rel,
 *                               char **modified_rel)
 *
 *   FUNCTION
 *       Resolve a guest entry name to an existing native filesystem path.
 *
 *   INPUTS
 *       unit -- Filesystem unit (unused by this function).
 *       base -- Parent inode providing the native directory.
 *       rel -- Guest entry name to resolve.
 *       modified_rel -- Receives an alternate native entry name when the directory search returns one.
 *
 *   RESULT
 *       A newly allocated native path, or NULL if the guest name is invalid, reserved by a mapping, or not found.
 *
 *   NOTES
 *       Clears *modified_rel before lookup. A non-NULL alternate name returned by fsdb_search_dir is passed to the caller through *modified_rel; its ownership follows the filesystem database contract.
 *
 ****************************************************************************/

static char *get_nname (Unit *unit, a_inode *base, char *rel,
                        char **modified_rel)
{
    char *found;
    char *result;

    TRACE(("(a_inode %lx, rel %s)\n", base, rel));

    aino_test (base);

    *modified_rel = 0;

    /* If we have a mapping of some other aname to "rel", we must pretend
     * it does not exist.
     * This can happen for example if an Amiga program creates a
     * file called ".".  We can't represent this in our filesystem,
     * so we create a special file "uae_xxx" and record the mapping
     * aname "." -> nname "uae_xxx" in the database.  Then, the Amiga
     * program looks up "uae_xxx" (yes, it's contrived).  The filesystem
     * should not make the uae_xxx file visible to the Amiga side.  */
    if (fsdb_used_as_nname (base, rel))
        return NULL;
    /* A file called "." (or whatever else is invalid on this filesystem)
        * does not exist, as far as the Amiga side is concerned.  */
    if (fsdb_name_invalid (rel))
        return NULL;

    TRACE(("search..\n"));
    /* See if we have a file that has the same name as the aname we are
     * looking for.  */
    found = fsdb_search_dir (base->nname, rel);
    if (found == 0) {
      TRACE(("found == 0\n"));
      return NULL;
    }

    if (found == rel) {
      result=build_nname (base->nname, rel);
      TRACE(("return1: %s\n",result));
      return result;
    }

    *modified_rel = found;
    result=build_nname (base->nname, found);
    TRACE(("return2: %s\n",result));
    return result;
}

/****i* januae-filesys-internal/create_nname
 *
 *   NAME
 *       create_nname()
 *
 *   SYNOPSIS
 *       static char *create_nname (Unit *unit, a_inode *base, char *rel)
 *
 *   FUNCTION
 *       Choose a native path for a new guest filesystem entry.
 *
 *   INPUTS
 *       unit -- Filesystem unit (unused by this function).
 *       base -- Parent inode whose directory contains the entry.
 *       rel -- Guest entry name to create.
 *
 *   RESULT
 *       A native path string obtained from the filesystem database or build_nname().
 *
 *   NOTES
 *       Requests a unique native name when the guest name cannot be represented directly or collides with an existing native-name mapping.
 *
 ****************************************************************************/

static char *create_nname (Unit *unit, a_inode *base, char *rel)
{
    char *p;

    aino_test (base);
    /* We are trying to create a file called REL.  */

    /* If the name is used otherwise in the directory (or globally), we
     * need a new unique nname.  */
    if (fsdb_name_invalid (rel) || fsdb_used_as_nname (base, rel)) {
        p = fsdb_create_unique_nname (base, rel);
        return p;
    }
    p = build_nname (base->nname, rel);
    return p;
}

/*
 * This gets called if an ACTION_EXAMINE_NEXT happens and we hit an object
 * for which we know the name on the native filesystem, but no corresponding
 * Amiga filesystem name.
 * @@@ For DOS filesystems, it might make sense to declare the new name
 * "weak", so that it can get overriden by a subsequent call to get_nname().
 * That way, if someone does "dir :" and there is a file "foobar.inf", and
 * someone else tries to open "foobar.info", get_nname() could maybe made to
 * figure out that this is supposed to be the file "foobar.inf".
 * DOS sucks...
 */
/****i* januae-filesys-internal/get_aname
 *
 *   NAME
 *       get_aname()
 *
 *   SYNOPSIS
 *       static char *get_aname (Unit *unit, a_inode *base, char *rel)
 *
 *   FUNCTION
 *       Copy a native entry name for use as a guest entry name.
 *
 *   INPUTS
 *       unit -- Filesystem unit (unused by this function).
 *       base -- Parent inode (unused by this function).
 *       rel -- Native entry name to copy.
 *
 *   RESULT
 *       A duplicate of rel, as returned by my_strdup().
 *
 *   NOTES
 *       This helper performs no name translation or validation.
 *
 ****************************************************************************/

static char *get_aname (Unit *unit, a_inode *base, char *rel)
{
    return my_strdup (rel);
}

/* not called for subsequent exnext calls to same directory */
/****i* januae-filesys-internal/init_child_aino
 *
 *   NAME
 *       init_child_aino()
 *
 *   SYNOPSIS
 *       static void init_child_aino (Unit *unit, a_inode *base, a_inode *aino)
 *
 *   FUNCTION
 *       Initialize an inode and link it as the first child of a parent.
 *
 *   INPUTS
 *       unit -- Filesystem unit supplying the inode sequence and lock counters.
 *       base -- Parent inode.
 *       aino -- New or recovered inode to initialize.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Assigns an incremented inode identifier, clears lock and deletion state, and inserts the inode at the head of the parent child list. Updates locked-inode accounting when the parent is being enumerated. Identifier exhaustion is logged but not prevented.
 *
 ****************************************************************************/

static void init_child_aino (Unit *unit, a_inode *base, a_inode *aino)
{
    aino->uniq = ++unit->a_uniq;
    if (unit->a_uniq == 0xFFFFFFFF) {
        write_log ("Running out of a_inodes (prepare for big trouble)!\n");
    }
    aino->shlock = 0;
    aino->elock = 0;

    aino->dirty = 0;
    aino->deleted = 0;

    /* For directories - this one isn't being ExNext()ed yet.  */
    aino->locked_children = 0;
    aino->exnext_count = 0;
    /* But the parent might be.  */
    if (base->exnext_count) {
        unit->total_locked_ainos++;
        base->locked_children++;
    }
    /* Update tree structure */
    aino->parent = base;
    aino->child = 0;
    aino->sibling = base->child;
    TRACE(("aino->sibling = base->child: %lx\n", base->child));
    base->child = aino;
    aino->next = aino->prev = 0;

    aino_test_init (aino);
    aino_test (aino);
}

/****i* januae-filesys-internal/new_child_aino
 *
 *   NAME
 *       new_child_aino()
 *
 *   SYNOPSIS
 *       static a_inode *new_child_aino (Unit *unit, a_inode *base, char *rel)
 *
 *   FUNCTION
 *       Find or create an inode representing an existing guest-visible entry.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the inode cache.
 *       base -- Parent inode.
 *       rel -- Guest entry name to resolve.
 *
 *   RESULT
 *       The initialized child inode, or NULL when path resolution, allocation, or attribute loading fails.
 *
 *   NOTES
 *       Reuses an inode from the filesystem database when available. Otherwise resolves the native path, allocates an inode, and reads its attributes. Initializes the parent-child link and places the inode in the recycle cache. The failure path after fsdb_fill_file_attrs() frees the inode structure without releasing its previously assigned name strings.
 *
 ****************************************************************************/

static a_inode *new_child_aino (Unit *unit, a_inode *base, char *rel)
{
    char *modified_rel;
    char *nn;
    a_inode *aino;

    TRACE(("new_child_aino(base->aname: %s, rel: %s)\n", base->aname, rel));

    aino = fsdb_lookup_aino_aname (base, rel);
    TRACE(("aino: %lx\n",aino));

    if (aino == 0) {
        nn = get_nname (unit, base, rel, &modified_rel);
        if (nn == 0) {
          TRACE(("nn==0\n"));
          return 0;
        }

        aino = (a_inode *) xcalloc (sizeof (a_inode), 1);
        if (aino == 0) {
          TRACE(("no memory!!\n"));
          return 0;
        }
        aino->aname = modified_rel ? modified_rel : my_strdup (rel);
        aino->nname = nn;

        aino->comment = 0;
        aino->has_dbentry = 0;

        if (!fsdb_fill_file_attrs (base, aino)) {
          TRACE(("!fsdb_fill_file_attrs\n"));
          xfree (aino);
          return 0;
        }
        if (aino->dir)
            fsdb_clean_dir (aino);
    }
    init_child_aino (unit, base, aino);

    recycle_aino (unit, aino);
    TRACE(("created aino %x, lookup, amigaos_mode %d\n", aino->uniq, aino->amigaos_mode));
    return aino;
}

/****i* januae-filesys-internal/create_child_aino
 *
 *   NAME
 *       create_child_aino()
 *
 *   SYNOPSIS
 *       static a_inode *create_child_aino (Unit *unit, a_inode *base, char *rel, int isdir)
 *
 *   FUNCTION
 *       Create an inode record for a guest-requested new entry.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the inode cache.
 *       base -- Parent inode.
 *       rel -- Guest entry name.
 *       isdir -- Directory flag stored in the new inode.
 *
 *   RESULT
 *       The newly initialized inode, or NULL on inode or native-name allocation failure.
 *
 *   NOTES
 *       Selects a native path, duplicates the guest name, marks the inode dirty, links it beneath the parent, and adds it to the recycle cache. The corresponding native filesystem object is not created here.
 *
 ****************************************************************************/

static a_inode *create_child_aino (Unit *unit, a_inode *base, char *rel, int isdir)
{
    a_inode *aino = (a_inode *) xcalloc (sizeof (a_inode), 1);
    if (aino == 0)
        return 0;

    aino->nname = create_nname (unit, base, rel);
    if (!aino->nname) {
        free (aino);
        return 0;
    }
    aino->aname = my_strdup (rel);

    init_child_aino (unit, base, aino);
    aino->amigaos_mode = 0;
    aino->dir = isdir;

    aino->comment = 0;
    aino->has_dbentry = 0;
    aino->dirty = 1;

    recycle_aino (unit, aino);
    TRACE(("created aino %x, create\n", aino->uniq));
    return aino;
}

/****i* januae-filesys-internal/lookup_child_aino
 *
 *   NAME
 *       lookup_child_aino()
 *
 *   SYNOPSIS
 *       static a_inode *lookup_child_aino (Unit *unit, a_inode *base, char *rel, uae_u32 *err)
 *
 *   FUNCTION
 *       Resolve a guest-named child from the inode cache or native filesystem.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the inode cache.
 *       base -- Parent inode to search.
 *       rel -- Guest entry name.
 *       err -- Receives an error code when resolution fails.
 *
 *   RESULT
 *       The matching child inode, or NULL when the parent is not a directory or the child cannot be resolved.
 *
 *   NOTES
 *       Checks that the parent is a directory. Matches cached names case-insensitively using same_aname(), then calls new_child_aino() if no cached child matches. Sets ERROR_OBJECT_WRONG_TYPE or ERROR_OBJECT_NOT_AROUND on the corresponding failure; it does not clear *err on success.
 *
 ****************************************************************************/

static a_inode *lookup_child_aino (Unit *unit, a_inode *base, char *rel, uae_u32 *err)
{
    a_inode *c = base->child;
    int l0 = strlen (rel);

    aino_test (base);
    aino_test (c);

    if (base->dir == 0) {
        *err = ERROR_OBJECT_WRONG_TYPE;
        return 0;
    }

    while (c != 0) {
        int l1 = strlen (c->aname);
        if (l0 <= l1 && same_aname (rel, c->aname + l1 - l0)
            && (l0 == l1 || c->aname[l1-l0-1] == '/'))
            break;
        c = c->sibling;
    }
    if (c != 0)
        return c;
    c = new_child_aino (unit, base, rel);
    if (c == 0)
        *err = ERROR_OBJECT_NOT_AROUND;
    return c;
}

/* Different version because for this one, REL is an nname.  */
/****i* januae-filesys-internal/lookup_child_aino_for_exnext
 *
 *   NAME
 *       lookup_child_aino_for_exnext()
 *
 *   SYNOPSIS
 *       static a_inode *lookup_child_aino_for_exnext (Unit *unit, a_inode *base, char *rel, uae_u32 *err)
 *
 *   FUNCTION
 *       Resolve a directory-enumeration entry by its native name.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the inode cache.
 *       base -- Parent directory inode.
 *       rel -- Native entry name returned during directory enumeration.
 *       err -- Receives zero initially or an allocation/attribute error on failure.
 *
 *   RESULT
 *       The matching or newly initialized inode, or NULL on allocation or attribute-loading failure.
 *
 *   NOTES
 *       Searches cached native path suffixes with strcmp(), then consults fsdb_lookup_aino_nname(). If no inode exists, allocates one, builds its names, and reads native attributes. Newly resolved entries are linked under the parent and entered into the recycle cache. The attribute failure path releases the inode structure but not its allocated names.
 *
 ****************************************************************************/

static a_inode *lookup_child_aino_for_exnext (Unit *unit, a_inode *base, char *rel, uae_u32 *err)
{
    a_inode *c = base->child;
    int l0 = strlen (rel);

    TRACE(("(base %lx, rel %s)\n", base, rel));

    aino_test (base);
    aino_test (c);

    *err = 0;
    while (c != 0) {
        int l1 = strlen (c->nname);
        TRACE(("c->nname: %s\n", c->nname));
        /* Note: using strcmp here.  */
        if ( (l0 <= l1 && strcmp (rel, c->nname + l1 - l0) == 0) &&
#ifndef TARGET_AMIGAOS
             (l0 == l1 || c->nname[l1-l0-1] == FSDB_DIR_SEPARATOR)
#else
             /* AmigaOS has *two dir separators. If ':' is missing, you will get
              * duplicate entries in dir listings, if emulated volume is mapped
              * to a host path containing a ':'
              */
             (l0 == l1 || c->nname[l1-l0-1] == FSDB_DIR_SEPARATOR || c->nname[l1-l0-1] == ':' )
#endif
             )
            break;
        c = c->sibling;
    }
    if (c != 0) {
        TRACE(("return found aino: %x\n", c->uniq));
        return c;
    }
    c = fsdb_lookup_aino_nname (base, rel);
    if (c == 0) {
        c = xcalloc (sizeof (a_inode), 1);
        if (c == 0) {
            *err = ERROR_NO_FREE_STORE;
            return 0;
        }

        c->nname = build_nname (base->nname, rel);
        c->aname = get_aname (unit, base, rel);
        c->comment = 0;
        c->has_dbentry = 0;
        if (!fsdb_fill_file_attrs (base, c)) {
            xfree (c);
            *err = ERROR_NO_FREE_STORE;
            return 0;
        }
        if (c->dir)
            fsdb_clean_dir (c);
    }
    init_child_aino (unit, base, c);

    recycle_aino (unit, c);
    TRACE(("created aino %x, exnext\n", c->uniq));

    return c;
}

/****i* januae-filesys-internal/get_aino
 *
 *   NAME
 *       get_aino()
 *
 *   SYNOPSIS
 *       static a_inode *get_aino (Unit *unit, a_inode *base, const char *rel, uae_u32 *err)
 *
 *   FUNCTION
 *       Resolve a guest path starting from an inode.
 *
 *   INPUTS
 *       unit -- Filesystem unit used for child lookup.
 *       base -- Starting inode.
 *       rel -- Guest path containing optional volume prefix and slash-separated components.
 *       err -- Receives the result of child lookup or a type error.
 *
 *   RESULT
 *       The resolved inode, NULL on an unresolved intermediate component or other lookup failure, or the parent inode when only the final component is absent.
 *
 *   NOTES
 *       Discards a prefix ending with colon before traversal. A leading slash in a component moves to the parent when one exists. Copies the input path for destructive component splitting, then frees the copy. The final-missing-component case retains ERROR_OBJECT_NOT_AROUND in *err.
 *
 ****************************************************************************/

static a_inode *get_aino (Unit *unit, a_inode *base, const char *rel, uae_u32 *err)
{
    char *tmp;
    char *p;
    a_inode *curr;
    int i;

    aino_test (base);

    *err = 0;
    TRACE(("get_aino(%s,%s)\n", base->aname, rel));

    /* root-relative path? */
    for (i = 0; rel[i] && rel[i] != '/' && rel[i] != ':'; i++)
        ;
    if (':' == rel[i])
        rel += i+1;

    tmp = my_strdup (rel);
    p = tmp;
    curr = base;

    while (*p) {
        /* start with a slash? go up a level. */
        if (*p == '/') {
            if (curr->parent != 0)
                curr = curr->parent;
            p++;
        } else {
            a_inode *next;

            char *component_end;
            component_end = strchr (p, '/');
            if (component_end != 0)
                *component_end = '\0';
            next = lookup_child_aino (unit, curr, p, err);
            if (next == 0) {
                /* if only last component not found, return parent dir. */
                if (*err != ERROR_OBJECT_NOT_AROUND || component_end != 0)
                    curr = 0;
                /* ? what error is appropriate? */
                break;
            }
            curr = next;
            if (component_end)
                p = component_end+1;
            else
                break;

        }
    }
    xfree (tmp);
    TRACE(("get_aino: aino: %lx\n", curr));
    return curr;
}

/****i* januae-filesys-internal/startup_update_unit
 *
 *   NAME
 *       startup_update_unit()
 *
 *   SYNOPSIS
 *       static void startup_update_unit (Unit *unit, UnitInfo *uinfo)
 *
 *   FUNCTION
 *       Update the active unit configuration from a mount description.
 *
 *   INPUTS
 *       unit -- Unit to update; NULL is ignored.
 *       uinfo -- Mount description providing the replacement fields.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Copies device, root path, read-only state, and pipe references; frees the previous volume name and duplicates the replacement. Other pointer fields are assigned without duplicating their storage.
 *
 ****************************************************************************/

static void startup_update_unit (Unit *unit, UnitInfo *uinfo)
{
    if (!unit)
        return;
    unit->ui.devname = uinfo->devname;
    xfree (unit->ui.volname);
    unit->ui.volname = my_strdup (uinfo->volname); /* might free later for rename */
    unit->ui.rootdir = uinfo->rootdir;
    unit->ui.readonly = uinfo->readonly;
    unit->ui.unit_pipe = uinfo->unit_pipe;
    unit->ui.back_pipe = uinfo->back_pipe;
}

/****i* januae-filesys-internal/startup_create_unit
 *
 *   NAME
 *       startup_create_unit()
 *
 *   SYNOPSIS
 *       static Unit *startup_create_unit (UnitInfo *uinfo, uae_u32 port)
 *
 *   FUNCTION
 *       Allocate and initialize a filesystem unit for a startup packet.
 *
 *   INPUTS
 *       uinfo -- Mount description associated with the new unit.
 *       port -- Guest message port associated with the unit.
 *
 *   RESULT
 *       The newly initialized unit pointer.
 *
 *   NOTES
 *       Prepends the unit to the global units list, stores the back-reference in uinfo, initializes the root inode, key state, hash buckets, and examine slots. The allocation result is used without an explicit NULL check; allocation failure is not handled locally.
 *
 ****************************************************************************/

static Unit *startup_create_unit (UnitInfo *uinfo, uae_u32 port)
{
    int i;
    Unit *unit;

    unit = (Unit *) xcalloc (sizeof (Unit), 1);
    unit->next = units;
    units = unit;
    uinfo->self = unit;

    unit->volume = 0;
    unit->port = port;
    unit->unit = unit_num++;

    startup_update_unit (unit, uinfo);

    unit->cmds_complete = 0;
    unit->cmds_sent = 0;
    unit->cmds_acked = 0;
    for (i = 0; i < EXKEYS; i++) {
        unit->examine_keys[i].aino = 0;
        unit->examine_keys[i].curr_file = 0;
        unit->examine_keys[i].uniq = 0;
    }
    unit->total_locked_ainos = 0;
    unit->next_exkey = 1;
    unit->keys = 0;
    unit->a_uniq = unit->key_uniq = 0;

    unit->rootnode.aname = uinfo->volname;
    unit->rootnode.nname = uinfo->rootdir;
    unit->rootnode.sibling = 0;

    TRACE(("unit->rootnode.sibling = 0\n"));
    unit->rootnode.next = unit->rootnode.prev = &unit->rootnode;
    unit->rootnode.uniq = 0;
    unit->rootnode.parent = 0;
    unit->rootnode.child = 0;
    unit->rootnode.dir = 1;
    unit->rootnode.amigaos_mode = 0;
    unit->rootnode.shlock = 0;
    unit->rootnode.elock = 0;
    unit->rootnode.comment = 0;
    unit->rootnode.has_dbentry = 0;
    aino_test_init (&unit->rootnode);
    unit->aino_cache_size = 0;
    for (i = 0; i < MAX_AINO_HASH; i++)
        unit->aino_hash[i] = 0;
    return unit;
}

static uae_u32 REGPARAM2 startup_handler (TrapContext *context)
{
    /* Just got the startup packet. It's in A4. DosBase is in A2,
     * our allocated volume structure is in D6, A5 is a pointer to
     * our port. */
    uaecptr rootnode = get_long (m68k_areg (&context->regs, 2) + 34);
    uaecptr dos_info = get_long (rootnode + 24) << 2;
    uaecptr pkt = m68k_dreg (&context->regs, 3);
    uaecptr arg2 = get_long (pkt + dp_Arg2);
    uaecptr port = m68k_areg (&context->regs, 5);
    int i, namelen;
    char* devname = bstr1 (get_long (pkt + dp_Arg1) << 2);
    char* s;
    Unit *unit;
    UnitInfo *uinfo;

    /* find UnitInfo with correct device name */
    s = strchr (devname, ':');
    if (s)
        *s = '\0';

    for (i = 0; i < current_mountinfo.num_units; i++) {
        /* Hardfile volume name? */
        if (current_mountinfo.ui[i].volname == 0)
            continue;

        if (current_mountinfo.ui[i].startup == arg2)
            break;
    }

    if (i == current_mountinfo.num_units
        || access (current_mountinfo.ui[i].rootdir, R_OK) != 0)
    {
        write_log ("Failed attempt to mount device\n", devname);
        put_long (pkt + dp_Res1, DOS_FALSE);
        put_long (pkt + dp_Res2, ERROR_DEVICE_NOT_MOUNTED);
        return 1;
    }
    uinfo = current_mountinfo.ui + i;
    unit = startup_create_unit (uinfo, port);

/*    write_comm_pipe_int (unit->ui.unit_pipe, -1, 1);*/

    write_log ("FS: %s starting..\n", unit->ui.volname);

    /* fill in our process in the device node */
    put_long ((get_long (pkt + dp_Arg3) << 2) + 8, unit->port);
    unit->dosbase = m68k_areg (&context->regs, 2);

    /* make new volume */
    unit->volume = m68k_areg (&context->regs, 3) + 32;
#ifdef UAE_FILESYS_THREADS
    unit->locklist = m68k_areg (&context->regs, 3) + 8;
#else
    unit->locklist = m68k_areg (&context->regs, 3);
#endif
    unit->dummy_message = m68k_areg (&context->regs, 3) + 12;

    put_long (unit->dummy_message + 10, 0);

    put_long (unit->volume + 4, 2); /* Type = dt_volume */
    put_long (unit->volume + 12, 0); /* Lock */
    put_long (unit->volume + 16, 3800); /* Creation Date */
    put_long (unit->volume + 20, 0);
    put_long (unit->volume + 24, 0);
    put_long (unit->volume + 28, 0); /* lock list */
    put_long (unit->volume + 40, (unit->volume + 44) >> 2); /* Name */
    namelen = strlen (unit->ui.volname);
    put_byte (unit->volume + 44, namelen);
    for (i = 0; i < namelen; i++)
        put_byte (unit->volume + 45 + i, unit->ui.volname[i]);

    /* link into DOS list */
    put_long (unit->volume, get_long (dos_info + 4));
    put_long (dos_info + 4, unit->volume >> 2);

    put_long (unit->volume + 8, unit->port);
    put_long (unit->volume + 32, DISK_TYPE);

    put_long (pkt + dp_Res1, DOS_TRUE);

    fsdb_clean_dir (&unit->rootnode);

    return 0;
}

static void
do_info (Unit *unit, dpacket packet, uaecptr info)
{
    struct fs_usage fsu;

    if (get_fs_usage (unit->ui.rootdir, 0, &fsu) != 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, dos_errno ());
        return;
    }

    fsu.fsu_blocks >>= 1;
    fsu.fsu_bavail >>= 1;
    put_long (info, 0); /* errors */
    put_long (info + 4, unit->unit); /* unit number */
    put_long (info + 8, unit->ui.readonly ? 80 : 82); /* state  */
    put_long (info + 12, fsu.fsu_blocks ); /* numblocks */
    put_long (info + 16, fsu.fsu_blocks - fsu.fsu_bavail); /* inuse */
    put_long (info + 20, 1024); /* bytesperblock */
    put_long (info + 24, DISK_TYPE); /* disk type */
    put_long (info + 28, unit->volume >> 2); /* volume node */
    put_long (info + 32, 0); /* inuse */
    PUT_PCK_RES1 (packet, DOS_TRUE);
}

static void
action_disk_info (Unit *unit, dpacket packet)
{
    TRACE(("ACTION_DISK_INFO\n"));
    do_info(unit, packet, GET_PCK_ARG1 (packet) << 2);
}

static void
action_info (Unit *unit, dpacket packet)
{
    TRACE(("ACTION_INFO\n"));
    do_info(unit, packet, GET_PCK_ARG2 (packet) << 2);
}

/****i* januae-filesys-internal/free_key
 *
 *   NAME
 *       free_key()
 *
 *   SYNOPSIS
 *       static void free_key (Unit *unit, Key *k)
 *
 *   FUNCTION
 *       Release an open-file key and its host descriptor.
 *
 *   INPUTS
 *       unit -- Unit whose open-key list is searched.
 *       k -- Key to remove and release.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Unlinks the key if present in the unit list. Closes its host descriptor when nonnegative, then frees the key even if it was not found in the list.
 *
 ****************************************************************************/

static void free_key (Unit *unit, Key *k)
{
    Key *k1;
    Key *prev = 0;
    for (k1 = unit->keys; k1; k1 = k1->next) {
        if (k == k1) {
            if (prev)
                prev->next = k->next;
            else
                unit->keys = k->next;
            break;
        }
        prev = k1;
    }

    if (k->fd >= 0)
        close (k->fd);

    xfree(k);
}

/****i* januae-filesys-internal/lookup_key
 *
 *   NAME
 *       lookup_key()
 *
 *   SYNOPSIS
 *       static Key *lookup_key (Unit *unit, uae_u32 uniq)
 *
 *   FUNCTION
 *       Look up a file key by its unit-local identifier.
 *
 *   INPUTS
 *       unit -- Filesystem unit whose key list is searched.
 *       uniq -- Identifier assigned when the key was created.
 *
 *   RESULT
 *       The matching key, or NULL if the identifier is absent.
 *
 *   NOTES
 *       Traverses the unit's linked key list without changing it. Logs two diagnostics when no key matches.
 *
 ****************************************************************************/

static Key *lookup_key (Unit *unit, uae_u32 uniq)
{
    Key *k;
    /* It's hardly worthwhile to optimize this - most of the time there are
     * only one or zero keys. */
    for (k = unit->keys; k; k = k->next) {
        if (uniq == k->uniq)
            return k;
    }
    write_log ("Error: couldn't find key!\n");
    /* There isn't much hope we will recover. Unix would kill the process,
     * AmigaOS gets killed by it. */
    write_log ("Better reset that Amiga - the system is messed up.\n");
    return 0;
}

/****i* januae-filesys-internal/new_key
 *
 *   NAME
 *       new_key()
 *
 *   SYNOPSIS
 *       static Key *new_key (Unit *unit)
 *
 *   FUNCTION
 *       Allocate and link a file key to a filesystem unit.
 *
 *   INPUTS
 *       unit -- Filesystem unit that owns the new key.
 *
 *   RESULT
 *       The newly allocated key.
 *
 *   NOTES
 *       Allocates a Key with xmalloc(), assigns the next unit-local identifier, initializes the file descriptor to -1 and the file position to zero, and prepends the key to unit->keys. No allocation-failure branch is present in this function.
 *
 ****************************************************************************/

static Key *new_key (Unit *unit)
{
    Key *k = (Key *) xmalloc(sizeof(Key));
    k->uniq = ++unit->key_uniq;
    k->fd = -1;
    k->file_pos = 0;
    k->next = unit->keys;
    unit->keys = k;

    return k;
}

/****i* januae-filesys-internal/dumplock
 *
 *   NAME
 *       dumplock()
 *
 *   SYNOPSIS
 *       static void dumplock (Unit *unit, uaecptr lock)
 *
 *   FUNCTION
 *       Trace a guest lock and its associated inode name.
 *
 *   INPUTS
 *       unit -- Unit used to resolve the inode.
 *       lock -- Guest lock address, or zero.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Trace output depends on TRACE configuration. A zero lock is reported without dereferencing it; an unknown inode is traced as not found.
 *
 ****************************************************************************/

static void dumplock (Unit *unit, uaecptr lock) {
  a_inode *a;

  if (!lock) {
    TRACE(("lock is NULL !!\n"));
    return;
  }
  else {
    TRACE(("LOCK: 0x%lx\n", lock));
  }

  TRACE(("{\n"));
  TRACE(("  next=0x%lx, mode=%ld, handler=0x%lx, volume=0x%lx, aino %lx \n",
         get_long (lock) << 2, get_long (lock+8),
         get_long (lock+12), get_long (lock+16),
         get_long (lock + 4)));
  a = lookup_aino (unit, get_long (lock + 4));
  if (a == 0) {
    TRACE(("  not found!\n"));
  } else {
    TRACE(("  %s\n", a->nname));
  }
  TRACE((" }\n"));

}

/****i* januae-filesys-internal/find_aino
 *
 *   NAME
 *       find_aino()
 *
 *   SYNOPSIS
 *       static a_inode *find_aino (Unit *unit, uaecptr lock, const char *name, uae_u32 *err)
 *
 *   FUNCTION
 *       Resolve a guest path relative to a lock or the unit root.
 *
 *   INPUTS
 *       unit -- Filesystem unit to search.
 *       lock -- Guest lock address, or zero to start at the root.
 *       name -- Guest path to resolve.
 *       err -- Error result passed through to get_aino().
 *
 *   RESULT
 *       The inode returned by get_aino(), which may be NULL.
 *
 *   NOTES
 *       When the lock refers to an unknown inode, resolution falls back to the unit root. Calls aino_test() on the returned inode.
 *
 ****************************************************************************/

static a_inode *find_aino (Unit *unit, uaecptr lock, const char *name, uae_u32 *err)
{
    a_inode *a;

    if (lock) {
        a_inode *olda = lookup_aino (unit, get_long (lock + 4));
        if (olda == 0) {
            /* That's the best we can hope to do. */
            a = get_aino (unit, &unit->rootnode, name, err);
        } else {
            TRACE(("aino: 0x%08lx", (unsigned long int)olda->uniq));
            TRACE((" \"%s\"\n", olda->nname));
            a = get_aino (unit, olda, name, err);
        }
    } else {
        a = get_aino (unit, &unit->rootnode, name, err);
    }
    if (a) {
        TRACE(("aino=\"%s\"\n", a->nname));
    }
    else {
        TRACE(("aino=NULL\n"));
    }
    aino_test (a);

    return a;
}

/****i* januae-filesys-internal/make_lock
 *
 *   NAME
 *       make_lock()
 *
 *   SYNOPSIS
 *       static uaecptr make_lock (Unit *unit, uae_u32 uniq, long mode)
 *
 *   FUNCTION
 *       Allocate a guest lock and link it to the volume's lock chain.
 *
 *   INPUTS
 *       unit -- Filesystem unit that owns the guest lock pool and volume.
 *       uniq -- Inode identifier stored in the new lock.
 *       mode -- Lock mode written to the guest lock.
 *
 *   RESULT
 *       Guest address of the newly initialized lock.
 *
 *   NOTES
 *       Removes the first entry from the guest lock free list, fills its inode, mode, handler port and volume fields, and prepends it to the volume lock chain. The free list is assumed to contain an available entry; this function does not check for exhaustion.
 *
 ****************************************************************************/

static uaecptr make_lock (Unit *unit, uae_u32 uniq, long mode)
{
    /* allocate lock from the list kept by the assembly code */
    uaecptr lock;

    TRACE(("(..)\n"));

    lock = get_long (unit->locklist);
    put_long (unit->locklist, get_long (lock));
    lock += 4;

    put_long (lock + 4, uniq);
    put_long (lock + 8, mode);
    put_long (lock + 12, unit->port);
    put_long (lock + 16, unit->volume >> 2);

    /* prepend to lock chain */
    put_long (lock, get_long (unit->volume + 28));
    put_long (unit->volume + 28, lock >> 2);

    DUMPLOCK(unit, lock);
    return lock;
}

/****i* januae-filesys-internal/notifyhash
 *
 *   NAME
 *       notifyhash()
 *
 *   SYNOPSIS
 *       static uae_u32 notifyhash (char *s)
 *
 *   FUNCTION
 *       Compute the notification table bucket for a name.
 *
 *   INPUTS
 *       s -- NUL-terminated name to hash.
 *
 *   RESULT
 *       Bucket index from zero to NOTIFY_HASH_SIZE - 1.
 *
 *   NOTES
 *       Accumulates each character with a five-bit shift before taking the table modulus. The caller supplies a valid string.
 *
 ****************************************************************************/

static uae_u32 notifyhash (char *s)
{
    uae_u32 hash = 0;
    while (*s) hash = (hash << 5) + *s++;
    return hash % NOTIFY_HASH_SIZE;
}

/****i* januae-filesys-internal/new_notify
 *
 *   NAME
 *       new_notify()
 *
 *   SYNOPSIS
 *       static Notify *new_notify (Unit *unit, char *name)
 *
 *   FUNCTION
 *       Allocate a notification record and link it into the unit hash table.
 *
 *   INPUTS
 *       unit -- Unit that owns the notification table.
 *       name -- Path string stored in the notification record.
 *
 *   RESULT
 *       Pointer to the newly allocated notification record.
 *
 *   NOTES
 *       The record takes the name pointer without duplicating it. The caller must ensure that the name remains allocated for the lifetime expected by free_notify().
 *
 ****************************************************************************/

static Notify *new_notify (Unit *unit, char *name)
{
    Notify *n = xmalloc(sizeof(Notify));
    int hash = notifyhash (name);
    n->next = unit->notifyhash[hash];
    unit->notifyhash[hash] = n;
    n->partname = name;
    return n;
}

/****i* januae-filesys-internal/free_notify
 *
 *   NAME
 *       free_notify()
 *
 *   SYNOPSIS
 *       static void free_notify (Unit *unit, int hash, Notify *n)
 *
 *   FUNCTION
 *       Unlink and release a notification record.
 *
 *   INPUTS
 *       unit -- Unit containing the notification table.
 *       hash -- Notification table bucket to search.
 *       n -- Notification record to release.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Searches the selected bucket and unlinks the matching record when present. Frees the supplied record regardless of whether it was found. The caller remains responsible for its separately allocated name fields.
 *
 ****************************************************************************/

static void free_notify (Unit *unit, int hash, Notify *n)
{
    Notify *n1, *prev = 0;
    for (n1 = unit->notifyhash[hash]; n1; n1 = n1->next) {
        if (n == n1) {
            if (prev)
                prev->next = n->next;
            else
                unit->notifyhash[hash] = n->next;
            break;
        }
        prev = n1;
    }
    xfree(n);
}

#define NOTIFY_CLASS    0x40000000
#define NOTIFY_CODE     0x1234

#ifndef TARGET_AMIGAOS
# define NRF_SEND_MESSAGE 1
# define NRF_SEND_SIGNAL 2
# define NRF_WAIT_REPLY 8
# define NRF_NOTIFY_INITIAL 16
# define NRF_MAGIC (1 << 31)
#endif

/****i* januae-filesys-internal/notify_send
 *
 *   NAME
 *       notify_send()
 *
 *   SYNOPSIS
 *       static void notify_send (Unit *unit, Notify *n)
 *
 *   FUNCTION
 *       Dispatch a filesystem change notification to the guest.
 *
 *   INPUTS
 *       unit -- Unit whose message port is used for notification.
 *       n -- Notification record containing the guest request address.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Reads the guest request flags. Message notifications use uae_NotificationHack or mark a reply-pending request; signal notifications use uae_Signal. Message delivery takes precedence when both flag types are set.
 *
 ****************************************************************************/

static void notify_send (Unit *unit, Notify *n)
{
    uaecptr nr = n->notifyrequest;
    int flags = get_long (nr + 12);

    if (flags & NRF_SEND_MESSAGE) {
        if (!(flags & NRF_WAIT_REPLY) || ((flags & NRF_WAIT_REPLY) && !(flags & NRF_MAGIC))) {
            uae_NotificationHack (unit->port, nr);
        } else if (flags & NRF_WAIT_REPLY) {
            put_long (nr + 12, get_long (nr + 12) | NRF_MAGIC);
        }
    } else if (flags & NRF_SEND_SIGNAL) {
        uae_Signal (get_long (nr + 16), 1 << get_byte (nr + 20));
    }
}

/****i* januae-filesys-internal/notify_check
 *
 *   NAME
 *       notify_check()
 *
 *   SYNOPSIS
 *       static void notify_check (Unit *unit, a_inode *a)
 *
 *   FUNCTION
 *       Check notification registrations for an inode and its parent.
 *
 *   INPUTS
 *       unit -- Unit containing registered notifications.
 *       a -- Inode whose name is checked.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Checks matching name buckets and resolves full paths before dispatch. A registration is notified only when its resolved inode is identical to the changed inode or its parent.
 *
 ****************************************************************************/

static void notify_check (Unit *unit, a_inode *a)
{
    Notify *n;
    int hash = notifyhash (a->aname);
    for (n = unit->notifyhash[hash]; n; n = n->next) {
        if (same_aname(n->partname, a->aname)) {
            uae_u32 err;
            a_inode *a2 = find_aino (unit, 0, n->fullname, &err);
            if (err == 0 && a == a2)
                notify_send (unit, n);
        }
    }
    if (a->parent) {
        hash = notifyhash (a->parent->aname);
        for (n = unit->notifyhash[hash]; n; n = n->next) {
            if (same_aname(n->partname, a->parent->aname)) {
                uae_u32 err;
                a_inode *a2 = find_aino (unit, 0, n->fullname, &err);
                if (err == 0 && a->parent == a2)
                    notify_send (unit, n);
            }
        }
    }
}

/****i* januae-filesys-internal/action_add_notify
 *
 *   NAME
 *       action_add_notify()
 *
 *   SYNOPSIS
 *       static void action_add_notify (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Register a filesystem change notification request.
 *
 *   INPUTS
 *       unit -- Filesystem unit that owns the notification table.
 *       packet -- Guest packet containing a NotifyRequest address.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Rejects requests without NRF_SEND_MESSAGE or NRF_SEND_SIGNAL with DOS_FALSE and ERROR_BAD_NUMBER. Extracts a basename from the requested path, registers the notification, and sends an initial notification when NRF_NOTIFY_INITIAL is set and the path resolves. Reports DOS_TRUE after registration.
 *
 ****************************************************************************/

static void
action_add_notify (Unit *unit, dpacket packet)
{
    uaecptr nr = GET_PCK_ARG1 (packet);
    int flags;
    Notify *n;
    char *name, *p, *partname;

    TRACE(("ACTION_ADD_NOTIFY\n"));

    name = my_strdup (char1 (get_long (nr + 4)));
    flags = get_long (nr + 12);

    if (!(flags & (NRF_SEND_MESSAGE | NRF_SEND_SIGNAL))) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_BAD_NUMBER);
        return;
    }


    p = name + strlen (name) - 1;
    if (p[0] == ':') p--;
    while (p > name && p[0] != ':' && p[0] != '/') p--;
    if (p[0] == ':' || p[0] == '/') p++;
    partname = my_strdup (p);
    n = new_notify (unit, partname);
    n->notifyrequest = nr;
    n->fullname = name;
    if (flags & NRF_NOTIFY_INITIAL) {
        uae_u32 err;
        a_inode *a = find_aino (unit, 0, n->fullname, &err);
        if (err == 0)
            notify_send (unit, n);
    }
    PUT_PCK_RES1 (packet, DOS_TRUE);
}
/****i* januae-filesys-internal/action_remove_notify
 *
 *   NAME
 *       action_remove_notify()
 *
 *   SYNOPSIS
 *       static void action_remove_notify (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Unregister a filesystem change notification request.
 *
 *   INPUTS
 *       unit -- Filesystem unit that owns the notification table.
 *       packet -- Guest packet identifying the NotifyRequest.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Searches all notification buckets by guest NotifyRequest address. Frees the stored names and removes the first matching entry. Reports DOS_TRUE even when no matching request exists.
 *
 ****************************************************************************/

static void
action_remove_notify (Unit *unit, dpacket packet)
{
    uaecptr nr = GET_PCK_ARG1 (packet);
    Notify *n;
    int hash;

    TRACE(("ACTION_REMOVE_NOTIFY\n"));
    for (hash = 0; hash < NOTIFY_HASH_SIZE; hash++) {
        for (n = unit->notifyhash[hash]; n; n = n->next) {
            if (n->notifyrequest == nr) {
                //write_log ("NotifyRequest %08.8X freed\n", n->notifyrequest);
                xfree (n->fullname);
                xfree (n->partname);
                free_notify (unit, hash, n);
                PUT_PCK_RES1 (packet, DOS_TRUE);
                return;
            }
        }
    }
    //write_log ("Tried to free non-existing NotifyRequest %08.8X\n", nr);
    PUT_PCK_RES1 (packet, DOS_TRUE);
}

/****i* januae-filesys-internal/free_lock
 *
 *   NAME
 *       free_lock()
 *
 *   SYNOPSIS
 *       static void free_lock (Unit *unit, uaecptr lock)
 *
 *   FUNCTION
 *       Return a guest lock to the unit's lock free list.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the volume lock chain and free list.
 *       lock -- Guest lock address; zero is ignored.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Unlinks the lock from the volume lock chain and pushes its backing slot onto unit->locklist. The non-head unlink path assumes the lock is present in the chain; no missing-lock check is performed.
 *
 ****************************************************************************/

static void free_lock (Unit *unit, uaecptr lock)
{
    if (! lock)
        return;

    if (lock == get_long (unit->volume + 28) << 2) {
        put_long (unit->volume + 28, get_long (lock));
    } else {
        uaecptr current = get_long (unit->volume + 28);
        uaecptr next = 0;
        while (current) {
            next = get_long (current << 2);
            if (lock == next << 2)
                break;
            current = next;
        }
        put_long (current << 2, get_long (lock));
    }
    lock -= 4;
    put_long (lock, get_long (unit->locklist));
    put_long (unit->locklist, lock);
}

/****i* januae-filesys-internal/action_lock
 *
 *   NAME
 *       action_lock()
 *
 *   SYNOPSIS
 *       static void action_lock (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Resolve an object name and acquire a guest filesystem lock.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the inode and guest-lock state.
 *       packet -- Guest packet carrying the base lock, pathname, and lock mode.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Normalizes an unsupported mode to SHARED_LOCK. Rejects conflicting shared or exclusive locks with ERROR_OBJECT_IN_USE; other lookup errors are propagated through the packet. On success updates inode lock bookkeeping and writes the new BPTR lock to the primary packet result. Does not perform access checks.
 *
 ****************************************************************************/

static void
action_lock (Unit *unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG1 (packet) << 2;
    uaecptr name = GET_PCK_ARG2 (packet) << 2;
    long mode = GET_PCK_ARG3 (packet);
    a_inode *a;
    uae_u32 err;

    if (mode != SHARED_LOCK && mode != EXCLUSIVE_LOCK) {
        TRACE(("Bad mode %d (should be %d or %d).\n", mode, SHARED_LOCK, EXCLUSIVE_LOCK));
        mode = SHARED_LOCK;
    }

    TRACE(("ACTION_LOCK(0x%lx, \"%s\", %d)\n", lock, bstr (unit, name), mode));
    DUMPLOCK(unit, lock);

    a = find_aino (unit, lock, bstr (unit, name), &err);
    if (err == 0 && (a->elock || (mode != SHARED_LOCK && a->shlock > 0))) {
      TRACE(("ERROR_OBJECT_IN_USE\n"));
      err = ERROR_OBJECT_IN_USE;
      SNOOP("lock(%s): FAILED (ERROR_OBJECT_IN_USE)\n", bstr (unit, name));
    }
    /* Lock() doesn't do access checks. */
    if (err != 0) {
        SNOOP("lock(%s): FAILED (%d)\n", bstr (unit, name), err);
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err);
        return;
    }
    if (mode == SHARED_LOCK)
        a->shlock++;
    else
        a->elock = 1;
    de_recycle_aino (unit, a);
    
    SNOOP("lock(%s): ok\n", bstr (unit, name));
    PUT_PCK_RES1 (packet, make_lock (unit, a->uniq, mode) >> 2);
}

/****i* januae-filesys-internal/action_free_lock
 *
 *   NAME
 *       action_free_lock()
 *
 *   SYNOPSIS
 *       static void action_free_lock (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Release an existing guest filesystem lock.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing inode and lock state.
 *       packet -- Guest packet carrying the lock BPTR.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Looks up the inode identifier stored in the lock. Reports ERROR_OBJECT_NOT_AROUND when that inode is absent. Otherwise clears its exclusive-lock flag or decrements its shared-lock count, updates recycling state, returns the lock slot to the free list, and reports DOS_TRUE.
 *
 ****************************************************************************/

static void action_free_lock (Unit *unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG1 (packet) << 2;
    a_inode *a;
    TRACE(("ACTION_FREE_LOCK(0x%lx)\n", lock));
    DUMPLOCK(unit, lock);

    a = lookup_aino (unit, get_long (lock + 4));
    if (a == 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_OBJECT_NOT_AROUND);
        return;
    }

    SNOOP("free_lock(%s)\n",a->nname);

    if (a->elock)
        a->elock = 0;
    else
        a->shlock--;

    recycle_aino (unit, a);
    free_lock(unit, lock);

    PUT_PCK_RES1 (packet, DOS_TRUE);
}

/****i* januae-filesys-internal/action_dup_lock_2
 *
 *   NAME
 *       action_dup_lock_2()
 *
 *   SYNOPSIS
 *       static uaecptr action_dup_lock_2 (Unit *unit, dpacket packet, uaecptr lock)
 *
 *   FUNCTION
 *       Duplicate a shared guest filesystem lock.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing inode and lock state.
 *       packet -- Guest packet receiving the duplicated lock or error.
 *       lock -- Guest address of the lock to duplicate.
 *
 *   RESULT
 *       The duplicated lock BPTR, or zero when no lock is supplied or duplication fails.
 *
 *   NOTES
 *       A zero input lock produces a zero primary packet result. Missing inodes report ERROR_OBJECT_NOT_AROUND; inodes with an exclusive lock report ERROR_OBJECT_IN_USE. On success increments the shared-lock count and creates another guest lock.
 *
 ****************************************************************************/

static uaecptr
action_dup_lock_2 (Unit *unit, dpacket packet, uaecptr lock)
{
    uaecptr out;
    a_inode *a;
    TRACE (("ACTION_DUP_LOCK(0x%lx)\n", lock));
    DUMPLOCK (unit, lock);

    if (!lock) {
        PUT_PCK_RES1 (packet, 0);
        return 0;
    }
    a = lookup_aino (unit, get_long (lock + 4));
    if (a == 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_OBJECT_NOT_AROUND);
        return 0;
    }
    /* DupLock()ing exclusive locks isn't possible, says the Autodoc, but
     * at least the RAM-Handler seems to allow it. Let's see what happens
     * if we don't. */
    if (a->elock) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_OBJECT_IN_USE);
        return 0;
    }
    a->shlock++;
    de_recycle_aino (unit, a);
    out = make_lock (unit, a->uniq, -2) >> 2;
    PUT_PCK_RES1 (packet, out);
    return out;
}

/****i* januae-filesys-internal/action_dup_lock
 *
 *   NAME
 *       action_dup_lock()
 *
 *   SYNOPSIS
 *       static void action_dup_lock (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Handle a guest request to duplicate a filesystem lock.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing inode and lock state.
 *       packet -- Guest packet carrying the lock BPTR.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Converts the packet lock BPTR to a guest address and delegates to action_dup_lock_2(), which writes the result and error fields.
 *
 ****************************************************************************/

static void
action_dup_lock (Unit *unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG1 (packet) << 2;
    action_dup_lock_2 (unit, packet, lock);
}


#if !defined TARGET_AMIGAOS || !defined WORDS_BIGENDIAN
/*
 * The difference between the start of the Unix epoch and the start
 * of the Amiga epoch
 */
const int secs_per_day = 24 * 60 * 60;
const int diff = (8 * 365 + 2) * (24 * 60 * 60);

/*
 * Convert host time (seconds since the start of the Unix Epoch) to
 * Amiga date-stamp
 */
static void
get_time (time_t t, long* days, long* mins, long* ticks)
{
    /* t is secs since 1-1-1970 */
    /* days since 1-1-1978 */
    /* mins since midnight */
    /* ticks past minute @ 50Hz */

# if !defined _WIN32 && !defined TARGET_AMIGAOS
    /*
     * On Unix-like systems, t is in UTC. The Amiga
     * requires local time, so we have to take account of
     * this difference if we can. This ain't easy to do in
     * a portable, thread-safe way.
     */
#  if defined HAVE_LOCALTIME_R && defined HAVE_TIMEGM 
    struct tm tm;

    /* Convert t to local time */
    localtime_r (&t, &tm);

    /* Calculate local time in seconds since the Unix Epoch */
    t = timegm (&tm);
#  endif
# endif

    /* Adjust for difference between Unix and Amiga epochs */
    t -= diff;

    /* Calculate Amiga date-stamp */
    *days = t / secs_per_day;
    t -= *days * secs_per_day;
    *mins = t / 60;
    t -= *mins * 60;
    *ticks = t * 50;
}

/*
 * Convert Amiga date-stamp to host time in seconds since the start
 * of the Unix epoch
 */
static time_t
put_time (long days, long mins, long ticks)
{
    time_t t;

    /* Calculate time in seconds since Amiga epoch */
    t  = ticks / 50;
    t += mins * 60;
    t += days * secs_per_day;

    /* Adjust for difference between Unix and Amiga epochs */
    t += diff;

# if !defined _WIN32
    /*
     * t is still in local time zone. For Unix-like systems
     * we need a time in UTC, so we have to take account of
     * the difference if we can. This ain't easy to do in
     * a portable, thread-safe way.
     *     *
     * New AROS includes have gmtime_r defined (old ones had them commented out),
     * so configure now finds them, even if they are not present?
     */
#  if defined HAVE_GMTIME_R && defined HAVE_LOCALTIME_R && !defined __AROS__
    {
        struct tm tm;
        struct tm now_tm;
        time_t now_t;

        gmtime_r (&t, &tm);

        /*
         * tm now contains the desired time in local time zone, not taking account
         * of DST. To fix this, we determine if DST is in effect now and stuff that
         * into tm.
         */
        now_t = time (0);
        localtime_r (&now_t, &now_tm);
        tm.tm_isdst = now_tm.tm_isdst;

        /* Convert time to UTC in seconds since the Unix epoch */
        t = mktime (&tm);
    }
#  endif
# endif
    return t;
}
#endif

/****i* januae-filesys-internal/free_exkey
 *
 *   NAME
 *       free_exkey()
 *
 *   SYNOPSIS
 *       static void free_exkey (Unit *unit, ExamineKey *ek)
 *
 *   FUNCTION
 *       Release an examine cursor reference to its inode.
 *
 *   INPUTS
 *       unit -- Unit tracking locked inode counts.
 *       ek -- Examine cursor to clear.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Decrements the inode examination count. When it reaches zero, subtracts the inode locked-child count from the unit total and clears that count. Sets the cursor inode and identifier to zero.
 *
 ****************************************************************************/

static void free_exkey (Unit *unit, ExamineKey *ek)
{
    if (--ek->aino->exnext_count == 0) {
        TRACE (("Freeing ExKey and reducing total_locked from %d by %d\n",
                unit->total_locked_ainos, ek->aino->locked_children));
        unit->total_locked_ainos -= ek->aino->locked_children;
        ek->aino->locked_children = 0;
    }
    ek->aino = 0;
    ek->uniq = 0;
}

/****i* januae-filesys-internal/lookup_exkey
 *
 *   NAME
 *       lookup_exkey()
 *
 *   SYNOPSIS
 *       static ExamineKey *lookup_exkey (Unit *unit, uae_u32 uniq)
 *
 *   FUNCTION
 *       Find a directory examination key by its identifier.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the examination-key array.
 *       uniq -- Examination-key identifier to find.
 *
 *   RESULT
 *       The matching examination key, or NULL if none matches.
 *
 *   NOTES
 *       Searches all EXKEYS slots by identifier, including slots that may have no current inode. Writes a diagnostic if no identifier matches.
 *
 ****************************************************************************/

static ExamineKey *lookup_exkey (Unit *unit, uae_u32 uniq)
{
    ExamineKey *ek;
    int i;

    ek = unit->examine_keys;
    for (i = 0; i < EXKEYS; i++, ek++) {
        /* Did we find a free one? */
        if (ek->uniq == uniq)
            return ek;
    }
    write_log ("Houston, we have a BIG problem.\n");
    return 0;
}

/* This is so sick... who invented ACTION_EXAMINE_NEXT? What did he THINK??? */
/* o1i: always the same (unit,aino) for one directoy */
/****i* januae-filesys-internal/new_exkey
 *
 *   NAME
 *       new_exkey()
 *
 *   SYNOPSIS
 *       static ExamineKey *new_exkey (Unit *unit, a_inode *aino)
 *
 *   FUNCTION
 *       Allocate a directory examination key for an inode.
 *
 *   INPUTS
 *       unit -- Filesystem unit owning the examination keys.
 *       aino -- Inode to associate with the key.
 *
 *   RESULT
 *       The initialized examination-key slot.
 *
 *   NOTES
 *       Uses an unused slot if one is available. Otherwise releases the key with the smallest identifier before reusing its slot. Assigns the unit sequence number, wrapping it to 1 near its upper limit, and resets the current-file cursor. Does not increment the inode examination count here.
 *
 ****************************************************************************/

static ExamineKey *new_exkey (Unit *unit, a_inode *aino)
{
    uae_u32 uniq;
    uae_u32 oldest = 0xFFFFFFFE;
    ExamineKey *ek, *oldest_ek = 0;
    int i;


    ek = unit->examine_keys;
    for (i = 0; i < EXKEYS; i++, ek++) {
        /* Did we find a free one? */
        if (ek->aino == 0)
            continue;
        if (ek->uniq < oldest)
            oldest = (oldest_ek = ek)->uniq;
    }
    ek = unit->examine_keys;
    for (i = 0; i < EXKEYS; i++, ek++) {
        /* Did we find a free one? */
        if (ek->aino == 0)
            goto found;
    }
    /* This message should usually be harmless. */
    write_log ("Houston, we have a problem (%s).\n", aino->nname);
    free_exkey (unit, oldest_ek);
    ek = oldest_ek;
    found:

    uniq = unit->next_exkey;
    if (uniq >= 0xFFFFFFFE) {
        /* Things will probably go wrong, but most likely the Amiga will crash
         * before this happens because of something else. */
        uniq = 1;
    }
    unit->next_exkey = uniq+1;
    ek->aino = aino;
    ek->curr_file = 0;
    ek->uniq = uniq;

    TRACE(("new_exkey(0x%lx, 0x%lx): %0lx (uniq %d)\n", unit, aino, ek, uniq));
    return ek;
}

/****i* januae-filesys-internal/move_exkeys
 *
 *   NAME
 *       move_exkeys()
 *
 *   SYNOPSIS
 *       static void move_exkeys (Unit *unit, a_inode *from, a_inode *to)
 *
 *   FUNCTION
 *       Transfer examination-key references and bookkeeping to another inode.
 *
 *   INPUTS
 *       unit -- Filesystem unit whose examination keys are scanned.
 *       from -- Inode whose examination references are transferred.
 *       to -- Inode receiving those references.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Repoints every occupied examination-key slot that refers to from. Copies examination and locked-child counts to to, clears both counts in from, and logs a discrepancy if the number of reassigned slots differs from the recorded examination count.
 *
 ****************************************************************************/

static void move_exkeys (Unit *unit, a_inode *from, a_inode *to)
{
    int i;
    unsigned long tmp = 0;
    for (i = 0; i < EXKEYS; i++) {
        ExamineKey *k = unit->examine_keys + i;
        if (k->uniq == 0)
            continue;
        if (k->aino == from) {
            k->aino = to;
            tmp++;
        }
    }
    if (tmp != from->exnext_count)
        write_log ("filesys.c: Bug in ExNext bookkeeping.  BAD.\n");
    to->exnext_count = from->exnext_count;
    to->locked_children = from->locked_children;
    from->exnext_count = 0;
    from->locked_children = 0;
}

/****i* januae-filesys-internal/get_fileinfo
 *
 *   NAME
 *       get_fileinfo()
 *
 *   SYNOPSIS
 *       static void get_fileinfo (Unit *unit, dpacket packet, uaecptr info, a_inode *aino)
 *
 *   FUNCTION
 *       Write an inode's information into a guest FileInfoBlock.
 *
 *   INPUTS
 *       unit -- Filesystem unit providing the volume name.
 *       packet -- Filesystem packet whose result field is updated.
 *       info -- Guest address of the FileInfoBlock.
 *       aino -- Inode whose details are written.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Writes the entry type, name, protection flags, host file size, block count, timestamps, and comment into guest memory. Names are limited to 106 characters and comments to 78. The host metadata paths do not validate all acquisition failures before using the result. Sets the packet primary result to DOS_TRUE.
 *
 ****************************************************************************/

static void
get_fileinfo (Unit *unit, dpacket packet, uaecptr info, a_inode *aino)
{
    int i, n, entrytype;
    const char *x;

    TRACE(("aino->parent: %d\n", aino->parent));
    if (aino->parent == 0) {
        /* Guru book says ST_ROOT = 1 (root directory, not currently used)
         * and some programs really expect 2 from root dir..
         */
        entrytype = 2;
        x = unit->ui.volname;
    } else {
        entrytype = aino->dir ? 2 : -3;
        x = aino->aname;
    }
    TRACE(("entrytype=%d\n", entrytype));
    put_long (info + 4, entrytype);
    /* AmigaOS docs say these have to contain the same value. */
    put_long (info + 120, entrytype);
    TRACE(("name=\"%s\"\n", x));
    n = strlen (x);
    /* cut to 106 chars */
    if (n > 106)
        n = 106;
    i = 8;
    /* write length */
    put_byte (info + i, n); i++;
    /* copy filename */
    while (n--)
        put_byte (info + i, *x), i++, x++;
    /* fill remainig space with (char) 0 */
    while (i < 108)
        put_byte (info + i, 0), i++;

    put_long (info + 116, aino->amigaos_mode);
   
#if defined TARGET_AMIGAOS 
   //&& defined WORDS_BIGENDIAN
     {
         BPTR lock;
        struct FileInfoBlock fib __attribute__((aligned(4)));
        TRACE(("Lock(%s, SHARED_LOCK)\n", aino->nname));
   
         if ((lock = Lock (aino->nname, SHARED_LOCK))) {
             Examine (lock, &fib);
             UnLock (lock);
         }
         put_long (info + 124, fib.fib_Size);
         put_long (info + 128, fib.fib_NumBlocks);
         put_long (info + 132, fib.fib_Date.ds_Days);
         put_long (info + 136, fib.fib_Date.ds_Minute);
         put_long (info + 140, fib.fib_Date.ds_Tick);
    }              
#else
    {
        struct stat statbuf;
        long days, mins, ticks;
       
        /* No error checks - this had better work. */
        stat (aino->nname, &statbuf);       
   
        put_long (info + 124, statbuf.st_size);
# ifdef HAVE_ST_BLOCKS
        put_long (info + 128, statbuf.st_blocks);
# else
        put_long (info + 128, statbuf.st_size / 512 + 1);
# endif
        get_time (statbuf.st_mtime, &days, &mins, &ticks);
        put_long (info + 132, days);
        put_long (info + 136, mins);
        put_long (info + 140, ticks);
    }   
#endif
    if (aino->comment == 0)
        put_long (info + 144, 0);
    else {
        TRACE(("comment=\"%s\"\n", aino->comment));
        i = 144;
        x = aino->comment;
        if (! x)
            x = "";
        n = strlen (x);
        if (n > 78)
            n = 78;
        put_byte (info + i, n); i++;
        while (n--)
            put_byte (info + i, *x), i++, x++;
        while (i < 224)
            put_byte (info + i, 0), i++;
    }
    PUT_PCK_RES1 (packet, DOS_TRUE);
}

/****i* januae-filesys-internal/action_examine_object
 *
 *   NAME
 *       action_examine_object()
 *
 *   SYNOPSIS
 *       static void action_examine_object (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Handle examination of a filesystem object.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the inode table and root inode.
 *       packet -- Guest filesystem packet containing the lock and information-buffer addresses.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Resolves the inode associated with the supplied lock, falling back to the root inode for a zero or unresolved lock. Writes its file information, then sets the disk-key field to 0xFFFFFFFF for directories or zero for other objects.
 *
 ****************************************************************************/

static void action_examine_object (Unit *unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG1 (packet) << 2;
    uaecptr info = GET_PCK_ARG2 (packet) << 2;
    a_inode *aino = 0;

    TRACE(("ACTION_EXAMINE_OBJECT(0x%lx,0x%lx)\n", lock, info));
    DUMPLOCK(unit, lock);

    if (lock != 0)
        aino = lookup_aino (unit, get_long (lock + 4));
    if (aino == 0)
        aino = &unit->rootnode;

    get_fileinfo (unit, packet, info, aino);
    if (aino->dir) {
        put_long (info, 0xFFFFFFFF);
    } else
        put_long (info, 0);
}

/* Read a directory's contents, create a_inodes for each file, and
   mark them as locked in memory so that recycle_aino will not reap
   them.
   We do this to avoid problems with the host OS: we don't want to
   leave the directory open on the host side until all ExNext()s have
   finished - they may never finish!  */

#ifndef __AROS__
static void populate_directory (Unit *unit, a_inode *base)
{
    DIR *d = opendir (base->nname);
    a_inode *aino;

    if (!d) {
      TRACE(("opendir(%s) failed\n",base->nname));
      return;
    }

    for (aino = base->child; aino; aino = aino->sibling) {
      base->locked_children++;
      unit->total_locked_ainos++;
    }

    TRACE(("Populating directory %s, child %p, locked_children %d\n",
            base->nname, base->child, base->locked_children));
    for (;;) {
        struct dirent *de;
        uae_u32 err;

        TRACE(("  for(;;)\n"));

        /* Find next file that belongs to the Amiga fs (skipping things
           like "..", "." etc.  */
        do {
            de = my_readdir (d, &de_space);
            if(de) {
              TRACE(("    de=%lx (%s)\n", de, de->d_name));
            }
            else {
              TRACE(("    de=NULL\n"));
            }
        } while (de && fsdb_name_invalid (de->d_name));
        if (! de)
            break;
        /* This calls init_child_aino, which will notice that the parent is
           being ExNext()ed, and it will increment the locked counts.  */
        aino = lookup_child_aino_for_exnext (unit, base, de->d_name, &err);
    }
    
    closedir (d);
}

#else

/* AROS own version 
 *
 * for some (unknown) reason, on one of my machines, the de->d_name above was rather
 * weird and never valid. So I rewrote it to use the AROS API. o1i.
 */
static void populate_directory (Unit *unit, a_inode *base) {

  BPTR    lock =NULL;
  BOOL    done =FALSE;
  uae_u32 err;
  struct FileInfoBlock fib;

    //DIR *d = opendir (base->nname);
  a_inode *aino;

  TRACE(("populate_directory(%s) AROS version\n",base->nname));

  lock=Lock(base->nname, ACCESS_READ);

  if (!lock) {
    TRACE(("Lock(%s) failed\n",base->nname));
    return;
  }
  TRACE(("locked %lx\n",lock));

  if(!Examine(lock, &fib)) {
    TRACE(("Examine failed!?\n"));
    UnLock(lock);
    return;
  }
  TRACE(("fib: %lx\n", fib));

  for (aino = base->child; aino; aino = aino->sibling) {
    base->locked_children++;
    unit->total_locked_ainos++;
  }

  TRACE(("Populating directory %s, child %p, locked_children %d\n",
            base->nname, base->child, base->locked_children));

  while(!done) {
    /* fib.fib_DirEntryType>0 => directory: puae only cares for directories here,
       but in juae this leads to all files missing 
     */
    if(ExNext(lock, &fib) /*&& (fib.fib_DirEntryType >0)*/) {
      /* process found fib */
      /* This calls init_child_aino, which will notice that the parent is
          being ExNext()ed, and it will increment the locked counts.  */
      TRACE(("process %s\n",fib.fib_FileName));
      aino = lookup_child_aino_for_exnext (unit, base, fib.fib_FileName, &err);
    }
    else {
      done=TRUE;
      if(IoErr() != ERROR_NO_MORE_ENTRIES) {
        TRACE(("ERROR: %lx\n",IoErr()));
      }
    }
  }

  UnLock(lock);
}
#endif



/****i* januae-filesys-internal/do_examine
 *
 *   NAME
 *       do_examine()
 *
 *   SYNOPSIS
 *       static void do_examine (Unit *unit, dpacket packet, ExamineKey *ek, uaecptr info)
 *
 *   FUNCTION
 *       Return the current directory entry and advance an examination cursor.
 *
 *   INPUTS
 *       unit -- Unit containing the examination key.
 *       packet -- Guest packet receiving the examination result.
 *       ek -- Examination cursor.
 *       info -- Guest FileInfoBlock address.
 *
 *   RESULT
 *       None. On end of enumeration, the packet reports DOS_FALSE and ERROR_NO_MORE_ENTRIES.
 *
 *   NOTES
 *       For an available entry, get_fileinfo() writes its information and the cursor advances to the next sibling. At end of enumeration, the examination key is released.
 *
 ****************************************************************************/

static void do_examine (Unit *unit, dpacket packet, ExamineKey *ek, uaecptr info)
{
    if (ek->curr_file == 0)
        goto no_more_entries;

    get_fileinfo (unit, packet, info, ek->curr_file);
    ek->curr_file = ek->curr_file->sibling;
    TRACE (("curr_file set to %p %s\n", ek->curr_file,
            ek->curr_file ? ek->curr_file->aname : "NULL"));
    return;

  no_more_entries:
    TRACE(("no more entries\n"));
    free_exkey (unit, ek);
    PUT_PCK_RES1 (packet, DOS_FALSE);
    PUT_PCK_RES2 (packet, ERROR_NO_MORE_ENTRIES);
}

static void action_examine_next (Unit *unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG1 (packet) << 2;
    uaecptr info = GET_PCK_ARG2 (packet) << 2;
    a_inode *aino = 0;
    ExamineKey *ek;
    uae_u32 uniq;

    TRACE(("ACTION_EXAMINE_NEXT(0x%lx,0x%lx)\n", lock, info));
    gui_hd_led (1);
    DUMPLOCK(unit, lock);

    if (lock != 0)
        aino = lookup_aino (unit, get_long (lock + 4));
    if (aino == 0)
        aino = &unit->rootnode;

    uniq = get_long (info);
    TRACE(("uniq: 0x%lx\n", uniq));
    if (uniq == 0) {
        write_log ("ExNext called for a file! (Houston?)\n");
        goto no_more_entries;
    } else if (uniq == 0xFFFFFFFE) {
        goto no_more_entries;
    }
    else if (uniq == 0xFFFFFFFF) {
        TRACE(("Creating new ExKey\n"));
        ek = new_exkey (unit, aino);
        if (ek) {
            if (aino->exnext_count++ == 0) {
                TRACE(("call populate_directory\n"));
                populate_directory (unit, aino);
            }
        }
        ek->curr_file = aino->child;
        TRACE(("Initial curr_file: %p %s\n", ek->curr_file,
               ek->curr_file ? ek->curr_file->aname : "NULL"));
    } else {
        TRACE(("Looking up ExKey\n"));
        ek = lookup_exkey (unit, get_long (info));
    }
    if (ek == 0) {
        write_log ("Couldn't find a matching ExKey. Prepare for trouble.\n");
        goto no_more_entries;
    }
    else {
        TRACE(("curr_file: %p %s\n", ek->curr_file, ek->curr_file ? ek->curr_file->aname : "NULL"));
    }
    put_long (info, ek->uniq);
    do_examine (unit, packet, ek, info);
    return;

  no_more_entries:
    TRACE(("no_more_entries!\n"));
    PUT_PCK_RES1 (packet, DOS_FALSE);
    PUT_PCK_RES2 (packet, ERROR_NO_MORE_ENTRIES);
}

/****i* januae-filesys-internal/do_find
 *
 *   NAME
 *       do_find()
 *
 *   SYNOPSIS
 *       static void do_find (Unit *unit, dpacket packet, int mode, int create, int fallback)
 *
 *   FUNCTION
 *       Open a host file for a guest filesystem handle.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the inode and key tables.
 *       packet -- Guest packet containing the filehandle, base lock, and pathname.
 *       mode -- Requested Amiga file protection/access flags.
 *       create -- Zero to open an existing object, one to allow creation, or two to create and truncate.
 *       fallback -- Allow a read-only fallback when requested write access is unavailable.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Resolves the pathname, rejects directories and conflicting locks, and checks applicable protection and read-only restrictions. Creates an inode when permitted and missing, opens the host file, and registers a file key. On success stores its key identifier in the guest filehandle at offset 36 and sets DOS_TRUE. On failure writes DOS_FALSE and the applicable error to the packet; a newly created inode is discarded if the host open fails.
 *
 ****************************************************************************/

static void do_find (Unit *unit, dpacket packet, int mode, int create, int fallback)
{
    uaecptr fh = GET_PCK_ARG1 (packet) << 2;
    uaecptr lock = GET_PCK_ARG2 (packet) << 2;
    uaecptr name = GET_PCK_ARG3 (packet) << 2;
    a_inode *aino;
    Key *k;
    int fd;
    uae_u32 err;
    mode_t openmode;
    int aino_created = 0;

    TRACE(("ACTION_FIND_*(0x%lx,0x%lx,\"%s\",%d,%d)\n", fh, lock, bstr (unit, name), mode, create));
    DUMPLOCK(unit, lock);

    aino = find_aino (unit, lock, bstr (unit, name), &err);

    if (aino == 0 || (err != 0 && err != ERROR_OBJECT_NOT_AROUND)) {
        /* Whatever it is, we can't handle it. */
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err);
        SNOOP("action_find(%s) FAILED (%d)\n",bstr (unit, name), err);
        return;
    }
    if (err == 0) {
        /* Object exists. */
        if (aino->dir) {
            PUT_PCK_RES1 (packet, DOS_FALSE);
            PUT_PCK_RES2 (packet, ERROR_OBJECT_WRONG_TYPE);
            SNOOP("action_find(%s) FAILED (ERROR_OBJECT_WRONG_TYPE)\n",bstr (unit, name));
            return;
        }
        if (aino->elock || (create == 2 && aino->shlock > 0)) {
            PUT_PCK_RES1 (packet, DOS_FALSE);
            PUT_PCK_RES2 (packet, ERROR_OBJECT_IN_USE);
            SNOOP("action_find(%s) FAILED (ERROR_OBJECT_IN_USE)\n",bstr (unit, name));
            return;
        }
        if (create == 2 && (aino->amigaos_mode & A_FIBF_DELETE) != 0) {
            PUT_PCK_RES1 (packet, DOS_FALSE);
            SNOOP("action_find(%s) FAILED (ERROR_DELETE_PROTECTED)\n",bstr (unit, name));
            PUT_PCK_RES2 (packet, ERROR_DELETE_PROTECTED);
            return;
        }
        if (create != 2) {
            if ((((mode & aino->amigaos_mode) & A_FIBF_WRITE) != 0 || unit->ui.readonly)
                && fallback)
            {
                mode &= ~A_FIBF_WRITE;
            }
            /* Kick 1.3 doesn't check read and write access bits - maybe it would be
             * simpler just not to do that either. */
            if ((mode & A_FIBF_WRITE) != 0 && unit->ui.readonly) {
                PUT_PCK_RES1 (packet, DOS_FALSE);
                PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
                SNOOP("action_find(%s) FAILED (ERROR_DISK_WRITE_PROTECTED)\n",bstr (unit, name));
                return;
            }
            if (((mode & aino->amigaos_mode) & A_FIBF_WRITE) != 0
                || mode == 0)
            {
                PUT_PCK_RES1 (packet, DOS_FALSE);
                PUT_PCK_RES2 (packet, ERROR_WRITE_PROTECTED);
                SNOOP("action_find(%s) FAILED (ERROR_WRITE_PROTECTED)\n",bstr (unit, name));
                return;
            }
            if (((mode & aino->amigaos_mode) & A_FIBF_READ) != 0) {
                PUT_PCK_RES1 (packet, DOS_FALSE);
                PUT_PCK_RES2 (packet, ERROR_READ_PROTECTED);
                SNOOP("action_find(%s) FAILED (ERROR_READ_PROTECTED)\n",bstr (unit, name));
                return;
            }
        }
    } else if (create == 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err);
        SNOOP("action_find(%s) FAILED b (%d)\n",bstr (unit, name), err);
        return;
    } else {
        /* Object does not exist. aino points to containing directory. */
        aino = create_child_aino (unit, aino, my_strdup (bstr_cut (unit, name)), 0);
        if (aino == 0) {
            PUT_PCK_RES1 (packet, DOS_FALSE);
            PUT_PCK_RES2 (packet, ERROR_DISK_IS_FULL); /* best we can do */
            SNOOP("action_find(%s) FAILED (ERROR_DISK_IS_FULL)\n",bstr (unit, name));
            return;
        }
        aino_created = 1;
    }

    prepare_for_open (aino->nname);

    openmode = (((mode & A_FIBF_READ) == 0 ? O_WRONLY
                 : (mode & A_FIBF_WRITE) == 0 ? O_RDONLY
                 : O_RDWR)
                | (create ? O_CREAT : 0)
                | (create == 2 ? O_TRUNC : 0));

    fd = open (aino->nname, openmode | O_BINARY, 0777);

    if (fd < 0) {
        if (aino_created)
            delete_aino (unit, aino);
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, dos_errno ());
        SNOOP("action_find(%s) FAILED c (%d)\n",bstr (unit, name), dos_errno());
        return;
    }

    k = new_key (unit);
    k->fd = fd;
    k->aino = aino;
    k->dosmode = mode;
    k->createmode = create;
    k->notifyactive = create ? 1 : 0;

    if (create)
        fsdb_set_file_attrs (aino);

    put_long (fh+36, k->uniq);
    if (create == 2)
        aino->elock = 1;
    else
        aino->shlock++;
    de_recycle_aino (unit, aino);
    SNOOP("action_find(%s) ok\n",bstr (unit, name));
    PUT_PCK_RES1 (packet, DOS_TRUE);
}

/****i* januae-filesys-internal/action_lock_from_fh
 *
 *   NAME
 *       action_lock_from_fh()
 *
 *   SYNOPSIS
 *       static void action_lock_from_fh (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Request a filesystem lock from an existing guest filehandle.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the open file keys.
 *       packet -- Guest packet identifying the file key.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Looks up the file key and reports DOS_FALSE if it is absent. Otherwise constructs a lock for the associated inode and passes it to action_dup_lock_2(), which supplies the packet result. The intermediate lock is not explicitly released here.
 *
 ****************************************************************************/

static void
action_lock_from_fh (Unit *unit, dpacket packet)
{
    Key *k = lookup_key (unit, GET_PCK_ARG1 (packet));
    uaecptr lock;
   
    TRACE (("ACTION_LOCK_FROM_FH(%p)\n", k));

    if (k == 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        return;
    }
    lock = action_dup_lock_2 (unit, packet, make_lock (unit, k->aino->uniq, -2));
    TRACE (("=%08x\n", lock));
}

/****i* januae-filesys-internal/action_fh_from_lock
 *
 *   NAME
 *       action_fh_from_lock()
 *
 *   SYNOPSIS
 *       static void action_fh_from_lock (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Open a guest filehandle for a filesystem object identified by a lock.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the lock and inode tables.
 *       packet -- Guest packet carrying the destination filehandle and source lock.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       A zero lock produces DOS_FALSE and a zero secondary result. An unresolved inode identifier selects the root inode. Opens the host path using the inode protection flags, or read-only access for a read-only unit. A successful open registers a file key in the guest filehandle and reports DOS_TRUE; open failures report the host-mapped DOS error. This routine does not adjust shared-lock counts or free the supplied lock.
 *
 ****************************************************************************/

static void
action_fh_from_lock (Unit *unit, dpacket packet)
{
    uaecptr fh = GET_PCK_ARG1 (packet) << 2;
    uaecptr lock = GET_PCK_ARG2 (packet) << 2;
    a_inode *aino;
    Key *k;
    int fd;
    mode_t openmode;
    int mode;

    TRACE(("ACTION_FH_FROM_LOCK(0x%lx,0x%lx)\n",fh,lock));
    DUMPLOCK(unit,lock);

    if (!lock) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, 0);
        return;
    }

    aino = lookup_aino (unit, get_long (lock + 4));
    if (aino == 0)
        aino = &unit->rootnode;
    mode = aino->amigaos_mode; /* Use same mode for opened filehandle as existing Lock() */

    prepare_for_open (aino->nname);

    TRACE (("  mode is %d\n", mode));
    openmode = (((mode & A_FIBF_READ) ? O_WRONLY
                 : (mode & A_FIBF_WRITE) ? O_RDONLY
                 : O_RDWR));

   /* the files on CD really can have the write-bit set.  */
    if (unit->ui.readonly)
        openmode = O_RDONLY;

    fd = open (aino->nname, openmode | O_BINARY, 0777);

    if (fd < 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, dos_errno());
        return;
    }
    k = new_key (unit);
    k->fd = fd;
    k->aino = aino;
    k->dosmode = mode;
    k->createmode = (mode != O_RDONLY ? 1 : 0);
    k->notifyactive = k->createmode != 0;

    put_long (fh + 36, k->uniq);
    /* I don't think I need to play with shlock count here, because I'm
       opening from an existing lock ??? */

    /* Is this right?  I don't completely understand how this works.  Do I
       also need to free_lock() my lock, since nobody else is going to? */
    de_recycle_aino (unit, aino);
    PUT_PCK_RES1 (packet, DOS_TRUE);
    /* PUT_PCK_RES2 (packet, k->uniq); - this shouldn't be necessary, try without it */
}

/****i* januae-filesys-internal/action_find_input
 *
 *   NAME
 *       action_find_input()
 *
 *   SYNOPSIS
 *       static void action_find_input (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Open an existing file for guest input.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the file.
 *       packet -- Guest packet containing the filehandle, base lock, and pathname.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Delegates to do_find() with read/write mode flags, no file creation, and read-only fallback enabled. The helper writes the packet results.
 *
 ****************************************************************************/

static void
action_find_input (Unit *unit, dpacket packet)
{
    do_find(unit, packet, A_FIBF_READ|A_FIBF_WRITE, 0, 1);
}

/****i* januae-filesys-internal/action_find_output
 *
 *   NAME
 *       action_find_output()
 *
 *   SYNOPSIS
 *       static void action_find_output (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Create or truncate a file for guest output.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the file.
 *       packet -- Guest packet containing the filehandle, base lock, and pathname.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Rejects read-only units with DOS_FALSE and ERROR_DISK_WRITE_PROTECTED. Otherwise delegates to do_find() with create-and-truncate mode and no read-only fallback.
 *
 ****************************************************************************/

static void
action_find_output (Unit *unit, dpacket packet)
{
    if (unit->ui.readonly) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
        return;
    }
    do_find(unit, packet, A_FIBF_READ|A_FIBF_WRITE, 2, 0);
}

/****i* januae-filesys-internal/action_find_write
 *
 *   NAME
 *       action_find_write()
 *
 *   SYNOPSIS
 *       static void action_find_write (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Open or create a file for guest writing.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the file.
 *       packet -- Guest packet containing the filehandle, base lock, and pathname.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Rejects read-only units with DOS_FALSE and ERROR_DISK_WRITE_PROTECTED. Otherwise delegates to do_find() with creation enabled but without truncation or read-only fallback.
 *
 ****************************************************************************/

static void
action_find_write (Unit *unit, dpacket packet)
{
    if (unit->ui.readonly) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
        return;
    }
    do_find(unit, packet, A_FIBF_READ|A_FIBF_WRITE, 1, 0);
}

/* change file/dir's parent dir modification time */
/****i* januae-filesys-internal/updatedirtime
 *
 *   NAME
 *       updatedirtime()
 *
 *   SYNOPSIS
 *       static void updatedirtime (a_inode *a1, int now)
 *
 *   FUNCTION
 *       Update the host modification time of an inode's parent directory.
 *
 *   INPUTS
 *       a1 -- Inode whose parent directory is to be updated.
 *       now -- Nonzero to use the current time, or zero to copy the inode timestamp.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       If there is no parent, the function returns without updating anything. For now == 0, a failed stat() call also leaves the parent unchanged. On the AmigaOS big-endian build selected by the conditional compilation guard, this function has no body effect.
 *
 ****************************************************************************/

static void updatedirtime (a_inode *a1, int now)
{
#if !defined TARGET_AMIGAOS || !defined WORDS_BIGENDIAN
    struct stat statbuf;
    struct utimbuf ut;
    long days, mins, ticks;

    if (!a1->parent)
        return;
    if (!now) {
        if (stat (a1->nname, &statbuf) == -1)
            return;
        get_time (statbuf.st_mtime, &days, &mins, &ticks);
        ut.actime = ut.modtime = put_time(days, mins, ticks);
        utime (a1->parent->nname, &ut);
    } else {
        utime (a1->parent->nname, NULL);
    }
#endif   
}

/****i* januae-filesys-internal/action_end
 *
 *   NAME
 *       action_end()
 *
 *   SYNOPSIS
 *       static void action_end (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Close a guest file key and release its inode lock state.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the file keys and inodes.
 *       packet -- Guest packet whose first argument identifies the file key.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       If the key exists, checks pending notifications, updates the parent timestamp when required, decrements the shared-lock count or clears the exclusive lock, recycles the inode, and releases the key. An unknown key is ignored. In either case, the packet receives DOS_TRUE and a zero secondary result.
 *
 ****************************************************************************/

static void
action_end (Unit *unit, dpacket packet)
{
    Key *k;
    TRACE(("ACTION_END(0x%lx)\n", GET_PCK_ARG1 (packet)));

    k = lookup_key (unit, GET_PCK_ARG1 (packet));
    if (k != 0) {
        if (k->notifyactive) {
            notify_check (unit, k->aino);
            updatedirtime (k->aino, 1);
        }
        if (k->aino->elock)
            k->aino->elock = 0;
        else
            k->aino->shlock--;
        recycle_aino (unit, k->aino);
        free_key (unit, k);
    }
    PUT_PCK_RES1 (packet, DOS_TRUE);
    PUT_PCK_RES2 (packet, 0);
}

/*
 * GET_PCK_ARG1 (packet): opened FileHandle
 * GET_PCK_ARG2 (packet): target buffer
 * GET_PCK_ARG3 (packet): number of bytes to read
 */
/****i* januae-filesys-internal/action_read
 *
 *   NAME
 *       action_read()
 *
 *   SYNOPSIS
 *       static void action_read (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Read bytes from an open host file into guest memory.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the open file key.
 *       packet -- Guest packet providing the key identifier, target address, and byte count.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       An unknown key sets DOS_FALSE without explicitly setting the secondary result. For directly addressable guest memory, reads into its mapped buffer; otherwise uses a temporary allocation and writes the returned bytes through put_byte(). Reports the byte count or an applicable read/allocation error in the packet. Advances the tracked position by the number of bytes read. The indirect path limits the requested count against the host file size, rather than the number of bytes remaining at the current offset.
 *
 ****************************************************************************/

static void action_read (Unit *unit, dpacket packet) {

  Key *k = lookup_key (unit, GET_PCK_ARG1 (packet));
  uaecptr addr = GET_PCK_ARG2 (packet);
  long size = (uae_s32)GET_PCK_ARG3 (packet);
  int actual;

  if (k == 0) {
    PUT_PCK_RES1 (packet, DOS_FALSE);
    /* PUT_PCK_RES2 (packet, EINVAL); */
    return;
  }
  TRACE(("ACTION_READ(%s,0x%lx,%ld)\n",k->aino->nname,addr,size));
  gui_hd_led (1);
#ifdef RELY_ON_LOADSEG_DETECTION
  /* HACK HACK HACK HACK
   * Try to detect a LoadSeg() */
  if (k->file_pos == 0 && size >= 4) {
    unsigned char buf[4];
    off_t currpos = lseek(k->fd, 0, SEEK_CUR);
    read(k->fd, buf, 4);
    lseek(k->fd, currpos, SEEK_SET);
    if (buf[0] == 0 && buf[1] == 0 && buf[2] == 3 && buf[3] == 0xF3)
      possible_loadseg();
    }
#endif
  if (valid_address (addr, size)) {
    uae_u8 *realpt;
    realpt = get_real_address (addr);
    actual = read(k->fd, realpt, size);

    if (actual == 0) {
        PUT_PCK_RES1 (packet, 0);
        PUT_PCK_RES2 (packet, 0);
    } else if (actual < 0) {
        PUT_PCK_RES1 (packet, 0);
        PUT_PCK_RES2 (packet, dos_errno());
    } else {
        PUT_PCK_RES1 (packet, actual);
        k->file_pos += actual;
    }
  } 
  else {
    char *buf;
    off_t old, filesize;

    write_log ("unixfs warning: Bad pointer passed for read: %08x, size %d\n", addr, size);
    /* ugh this is inefficient but easy */

    old = lseek (k->fd, 0, SEEK_CUR);
    filesize = lseek (k->fd, 0, SEEK_END);
    lseek (k->fd, old, SEEK_SET);
    if (size > filesize)
        size = filesize;

    buf = (char *)malloc(size);
    if (!buf) {
        PUT_PCK_RES1 (packet, -1);
        PUT_PCK_RES2 (packet, ERROR_NO_FREE_STORE);
        return;
    }
    actual = read(k->fd, buf, size);

    if (actual < 0) {
        PUT_PCK_RES1 (packet, 0);
        PUT_PCK_RES2 (packet, dos_errno());
    } else {
        int i;
        PUT_PCK_RES1 (packet, actual);
        for (i = 0; i < actual; i++)
          put_byte(addr + i, buf[i]);
        k->file_pos += actual;
    }
    xfree (buf);
  }
  TRACE(("=%d\n", actual));
}

/****i* januae-filesys-internal/action_write
 *
 *   NAME
 *       action_write()
 *
 *   SYNOPSIS
 *       static void action_write (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Write bytes from guest memory to an open host file.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the open file key.
 *       packet -- Guest packet providing the key identifier, source address, and byte count.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Rejects unknown keys and read-only units. Allocates a temporary buffer, copies the guest bytes through get_byte(), and writes them to the host descriptor. Stores the host write result in the packet and reports dos_errno() when the number of written bytes differs from the request. Advances the tracked file position for nonnegative writes and marks the key for notification checks. An unknown key does not explicitly set the packet secondary result.
 *
 ****************************************************************************/

static void
action_write (Unit *unit, dpacket packet)
{
    Key *k = lookup_key (unit, GET_PCK_ARG1 (packet));
    uaecptr addr = GET_PCK_ARG2 (packet);
    long size = GET_PCK_ARG3 (packet);
    long actual;
    char *buf;
    int i;

    if (k == 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        /* PUT_PCK_RES2 (packet, EINVAL); */
        return;
    }

    gui_hd_led (2);
    TRACE(("ACTION_WRITE(%s,0x%lx,%ld)\n",k->aino->nname,addr,size));

    if (unit->ui.readonly) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
        return;
    }

    /* ugh this is inefficient but easy */
    buf = (char *)malloc(size);
    if (!buf) {
        PUT_PCK_RES1 (packet, -1);
        PUT_PCK_RES2 (packet, ERROR_NO_FREE_STORE);
        return;
    }

    for (i = 0; i < size; i++)
        buf[i] = get_byte(addr + i);

    actual = write (k->fd, buf, size);
    TRACE(("=%d\n", actual));
    PUT_PCK_RES1 (packet, actual);
    if (actual != size)
        PUT_PCK_RES2 (packet, dos_errno ());
    if (actual >= 0)
        k->file_pos += actual;

    k->notifyactive = 1;
    xfree (buf);
}

/****i* januae-filesys-internal/action_seek
 *
 *   NAME
 *       action_seek()
 *
 *   SYNOPSIS
 *       static void action_seek (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Reposition the host file descriptor for a guest file key.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the open file key.
 *       packet -- Guest packet providing the key identifier, signed offset, and seek mode.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Negative, zero, and positive modes select SEEK_SET, SEEK_CUR, and SEEK_END, respectively. Rejects unknown keys and destinations beyond the current file size with packet errors. On success returns the previous offset in the packet; on host seek failure reports ERROR_SEEK_ERROR. Updates the tracked position with the host seek result, including a failed result.
 *
 ****************************************************************************/

static void
action_seek (Unit *unit, dpacket packet)
{
    Key *k = lookup_key (unit, GET_PCK_ARG1 (packet));
    long pos = (uae_s32)GET_PCK_ARG2 (packet);
    long mode = (uae_s32)GET_PCK_ARG3 (packet);
    off_t res;
    long old;
    int whence = SEEK_CUR;

    if (k == 0) {
        PUT_PCK_RES1 (packet, -1);
        PUT_PCK_RES2 (packet, ERROR_INVALID_LOCK);
        return;
    }

    if (mode > 0) whence = SEEK_END;
    if (mode < 0) whence = SEEK_SET;

    TRACE(("ACTION_SEEK(%s,%d,%d)\n", k->aino->nname, pos, mode));
    gui_hd_led (1);

    old = lseek (k->fd, 0, SEEK_CUR);
    {
        uae_s32 temppos;
        long filesize = lseek (k->fd, 0, SEEK_END);
        lseek (k->fd, old, SEEK_SET);

        temppos = pos;
        if (whence == SEEK_CUR) temppos += old;
        if (whence == SEEK_END) temppos += filesize;
        if (filesize < temppos) {
            res = -1;
            PUT_PCK_RES1 (packet,res);
            PUT_PCK_RES2 (packet, ERROR_SEEK_ERROR);
            return;
        }
    }
    res = lseek (k->fd, pos, whence);

    if (-1 == res) {
        PUT_PCK_RES1 (packet, res);
        PUT_PCK_RES2 (packet, ERROR_SEEK_ERROR);
    } else
        PUT_PCK_RES1 (packet, old);
    k->file_pos = res;
}

/****i* januae-filesys-internal/action_set_protect
 *
 *   NAME
 *       action_set_protect()
 *
 *   SYNOPSIS
 *       static void action_set_protect (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Set the Amiga protection mask for a filesystem inode.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the requested object.
 *       packet -- Guest packet providing the base lock, pathname, and protection mask.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Rejects read-only units or unresolved paths and reports the error in the packet. Otherwise assigns the inode mask and calls fsdb_set_file_attrs(); the packet reports whether that operation succeeded. Notification checks and disk activity reporting follow even when setting host attributes fails.
 *
 ****************************************************************************/

static void
action_set_protect (Unit *unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG2 (packet) << 2;
    uaecptr name = GET_PCK_ARG3 (packet) << 2;
    uae_u32 mask = GET_PCK_ARG4 (packet);
    a_inode *a;
    uae_u32 err;

    TRACE(("ACTION_SET_PROTECT(0x%lx,\"%s\",0x%lx)\n", lock, bstr (unit, name), mask));

    if (unit->ui.readonly) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
        return;
    }

    a = find_aino (unit, lock, bstr (unit, name), &err);
    if (err != 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err);
        return;
    }

    a->amigaos_mode = mask;
    err = fsdb_set_file_attrs (a);
    if (err != 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err);
    } else {
        PUT_PCK_RES1 (packet, DOS_TRUE);
    }
    notify_check (unit, a);
    gui_hd_led (2);
}

/****i* januae-filesys-internal/action_set_comment
 *
 *   NAME
 *       action_set_comment()
 *
 *   SYNOPSIS
 *       static void action_set_comment (Unit * unit, dpacket packet)
 *
 *   FUNCTION
 *       Set or clear the comment associated with a filesystem inode.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the requested object.
 *       packet -- Guest packet providing the base lock, pathname, and comment string.
 *
 *   RESULT
 *       None.
 *
 *   NOTES
 *       Rejects read-only units or unresolved paths. A zero-length comment clears the stored comment. If the new comment matches the existing one, returns success without rewriting attributes. Otherwise replaces the owned comment string, invokes fsdb_set_file_attrs(), and notifies listeners. The return value of fsdb_set_file_attrs() is not checked here; the packet already reports success.
 *
 ****************************************************************************/

static void action_set_comment (Unit * unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG2 (packet) << 2;
    uaecptr name = GET_PCK_ARG3 (packet) << 2;
    uaecptr comment = GET_PCK_ARG4 (packet) << 2;
    char *commented;
    a_inode *a;
    uae_u32 err;

    if (unit->ui.readonly) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
        return;
    }

    commented = bstr (unit, comment);
    commented = strlen (commented) > 0 ? my_strdup (commented) : NULL;
    TRACE (("ACTION_SET_COMMENT(0x%lx,\"%s\")\n", lock, commented));

    a = find_aino (unit, lock, bstr (unit, name), &err);
    if (err != 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err);

        maybe_free_and_out:
        if (commented)
            xfree (commented);
        return;
    }
    PUT_PCK_RES1 (packet, DOS_TRUE);
    PUT_PCK_RES2 (packet, 0);
    if (a->comment == 0 && commented == 0)
        goto maybe_free_and_out;
    if (a->comment != 0 && commented != 0 && strcmp (a->comment, commented) == 0)
        goto maybe_free_and_out;
    if (a->comment)
        xfree (a->comment);
    a->comment = commented;
    fsdb_set_file_attrs (a);
    notify_check (unit, a);
    gui_hd_led (2);
}

/****i* januae-filesys-internal/action_same_lock
 *
 *   NAME
 *       action_same_lock()
 *
 *   SYNOPSIS
 *       static void action_same_lock (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Compare the inode identifiers held by two guest locks.
 *
 *   INPUTS
 *       unit -- Filesystem unit used for lock diagnostics.
 *       packet -- Guest packet containing the two lock values.
 *
 *   RESULT
 *       None. The packet primary result is DOS_TRUE when both locks identify the same inode or when both locks are null.
 *
 *   NOTES
 *       A null lock and a nonnull lock compare unequal. The function does not validate the addresses of nonnull guest locks and does not explicitly update the secondary packet result.
 *
 ****************************************************************************/

static void
action_same_lock (Unit *unit, dpacket packet)
{
    uaecptr lock1 = GET_PCK_ARG1 (packet) << 2;
    uaecptr lock2 = GET_PCK_ARG2 (packet) << 2;

    TRACE(("ACTION_SAME_LOCK(0x%lx,0x%lx)\n",lock1,lock2));
    DUMPLOCK(unit, lock1); DUMPLOCK(unit, lock2);

    if (!lock1 || !lock2) {
        PUT_PCK_RES1 (packet, lock1 == lock2 ? DOS_TRUE : DOS_FALSE);
    } else {
        PUT_PCK_RES1 (packet, get_long (lock1 + 4) == get_long (lock2 + 4) ? DOS_TRUE : DOS_FALSE);
    }
}

/****i* januae-filesys-internal/action_change_mode
 *
 *   NAME
 *       action_change_mode()
 *
 *   SYNOPSIS
 *       static void action_change_mode (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Change the recorded shared or exclusive access mode of a lock or file handle.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the inode and file-key state.
 *       packet -- Guest packet specifying object kind, object reference, and requested mode.
 *
 *   RESULT
 *       None. The packet primary result reports success or failure.
 *
 *   NOTES
 *       Rejects null objects, unknown object kinds, absent file keys, or missing inodes. File-handle modes are translated into shared or exclusive inode modes. The conversion checks the shared-lock count before granting exclusive access, but does not perform a general compatibility check against all extant handles. On success it does not explicitly clear the packet secondary result.
 *
 ****************************************************************************/

static void
action_change_mode (Unit *unit, dpacket packet)
{
#define CHANGE_LOCK 0
#define CHANGE_FH 1
    /* will be CHANGE_FH or CHANGE_LOCK value */
    long type = GET_PCK_ARG1 (packet);
    /* either a file-handle or lock */
    uaecptr object = GET_PCK_ARG2 (packet) << 2;
    /* will be EXCLUSIVE_LOCK/SHARED_LOCK if CHANGE_LOCK,
     * or MODE_OLDFILE/MODE_NEWFILE/MODE_READWRITE if CHANGE_FH */
    long mode = GET_PCK_ARG3 (packet);
    unsigned long uniq;
    a_inode *a = NULL, *olda = NULL;
    uae_u32 err = 0;
    TRACE(("ACTION_CHANGE_MODE(0x%lx,%d,%d)\n",object,type,mode));

    if (! object
        || (type != CHANGE_FH && type != CHANGE_LOCK))
    {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_INVALID_LOCK);
        return;
    }

    /* @@@ Brian: shouldn't this be good enough to support
       CHANGE_FH?  */
    if (type == CHANGE_FH)
        mode = (mode == 1006 ? -1 : -2);

    if (type == CHANGE_LOCK)
        uniq = get_long (object + 4);
    else {
        Key *k = lookup_key (unit, object);
        if (!k) {
            PUT_PCK_RES1 (packet, DOS_FALSE);
            PUT_PCK_RES2 (packet, ERROR_OBJECT_NOT_AROUND);
            return;
        }
        uniq = k->aino->uniq;
    }
    a = lookup_aino (unit, uniq);

    if (! a)
        err = ERROR_INVALID_LOCK;
    else {
        if (mode == -1) {
            if (a->shlock > 1)
                err = ERROR_OBJECT_IN_USE;
            else {
                a->shlock = 0;
                a->elock = 1;
            }
        } else { /* Must be SHARED_LOCK == -2 */
            a->elock = 0;
            a->shlock++;
        }
    }

    if (err) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err);
        return;
    } else {
        de_recycle_aino (unit, a);
        PUT_PCK_RES1 (packet, DOS_TRUE);
    }
}

/****i* januae-filesys-internal/action_parent_common
 *
 *   NAME
 *       action_parent_common()
 *
 *   SYNOPSIS
 *       static void action_parent_common (Unit *unit, dpacket packet, unsigned long uniq)
 *
 *   FUNCTION
 *       Acquire a shared guest lock for the parent of an inode.
 *
 *   INPUTS
 *       unit -- Filesystem unit owning the inode and lock records.
 *       packet -- Guest packet receiving the resulting parent lock or error.
 *       uniq -- Identifier of the inode whose parent is requested.
 *
 *   RESULT
 *       None. The packet receives a parent lock, zero for a root object, or DOS_FALSE on error.
 *
 *   NOTES
 *       Rejects unknown inode identifiers and parents with an exclusive lock. A successful parent lookup increments the parent shared-lock count and allocates a guest lock. The success path does not explicitly set the secondary packet result.
 *
 ****************************************************************************/

static void
action_parent_common (Unit *unit, dpacket packet, unsigned long uniq)
{
    a_inode *olda = lookup_aino (unit, uniq);
    if (olda == 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_INVALID_LOCK);
        return;
    }

    if (olda->parent == 0) {
        PUT_PCK_RES1 (packet, 0);
        PUT_PCK_RES2 (packet, 0);
        return;
    }
    if (olda->parent->elock) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_OBJECT_IN_USE);
        return;
    }
    olda->parent->shlock++;
    de_recycle_aino (unit, olda->parent);
    PUT_PCK_RES1 (packet, make_lock (unit, olda->parent->uniq, -2) >> 2);
}

/****i* januae-filesys-internal/action_parent_fh
 *
 *   NAME
 *       action_parent_fh()
 *
 *   SYNOPSIS
 *       static void action_parent_fh (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Resolve an open file key and request a lock for its parent directory.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the file key.
 *       packet -- Guest packet whose first argument is the file-key identifier.
 *
 *   RESULT
 *       None. Results are stored in the packet.
 *
 *   NOTES
 *       An absent file key reports ERROR_OBJECT_NOT_AROUND. For an existing key, delegates the parent lookup and lock creation to action_parent_common().
 *
 ****************************************************************************/

static void
action_parent_fh (Unit *unit, dpacket packet)
{
    Key *k = lookup_key (unit, GET_PCK_ARG1 (packet));
    if (!k) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_OBJECT_NOT_AROUND);
        return;
    }
    action_parent_common (unit, packet, k->aino->uniq);
}

/****i* januae-filesys-internal/action_parent
 *
 *   NAME
 *       action_parent()
 *
 *   SYNOPSIS
 *       static void action_parent (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Request the parent lock of a guest lock.
 *
 *   INPUTS
 *       unit -- Filesystem unit owning the lock records.
 *       packet -- Guest packet whose first argument is a guest lock.
 *
 *   RESULT
 *       None. Results are stored in the packet.
 *
 *   NOTES
 *       A null lock returns zero for both packet results. Otherwise reads the inode identifier from the guest lock and delegates to action_parent_common(). The input lock address is not independently validated.
 *
 ****************************************************************************/

static void
action_parent (Unit *unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG1 (packet) << 2;

    TRACE(("ACTION_PARENT(0x%lx)\n",lock));

    if (!lock) {
        PUT_PCK_RES1 (packet, 0);
        PUT_PCK_RES2 (packet, 0);
    } else {
        action_parent_common (unit, packet, get_long (lock + 4));
    }
    TRACE(("=%x %d\n", GET_PCK_RES1 (packet), GET_PCK_RES2 (packet)));
}

/****i* januae-filesys-internal/action_create_dir
 *
 *   NAME
 *       action_create_dir()
 *
 *   SYNOPSIS
 *       static void action_create_dir (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Create a directory on the host filesystem and return its guest lock.
 *
 *   INPUTS
 *       unit -- Filesystem unit owning the destination path and inode records.
 *       packet -- Guest packet specifying the base lock and directory name.
 *
 *   RESULT
 *       None. The packet receives a new lock or an error result.
 *
 *   NOTES
 *       Rejects read-only units, unresolved parent paths, and existing objects. Allocates an inode before invoking mkdir() with mode 0777. Reports ERROR_DISK_IS_FULL if inode creation fails and dos_errno() on mkdir() failure. The newly allocated inode is not explicitly removed when mkdir() fails. On success updates attributes, notifications, and parent time and returns a shared lock.
 *
 ****************************************************************************/

static void
action_create_dir (Unit *unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG1 (packet) << 2;
    uaecptr name = GET_PCK_ARG2 (packet) << 2;
    a_inode *aino;
    uae_u32 err;

    TRACE(("ACTION_CREATE_DIR(0x%lx,\"%s\")\n", lock, bstr (unit, name)));

    if (unit->ui.readonly) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
        return;
    }

    aino = find_aino (unit, lock, bstr (unit, name), &err);
    if (aino == 0 || (err != 0 && err != ERROR_OBJECT_NOT_AROUND)) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err);
        return;
    }
    if (err == 0) {
        /* Object exists. */
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_OBJECT_EXISTS);
        return;
    }
    /* Object does not exist. aino points to containing directory. */
    aino = create_child_aino (unit, aino, my_strdup (bstr_cut (unit, name)), 1);
    if (aino == 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_IS_FULL); /* best we can do */
        return;
    }

    if (mkdir (aino->nname, 0777) == -1) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, dos_errno());
        return;
    }
    aino->shlock = 1;
    fsdb_set_file_attrs (aino);
    de_recycle_aino (unit, aino);
    notify_check (unit, aino);
    updatedirtime (aino, 0);
    PUT_PCK_RES1 (packet, make_lock (unit, aino->uniq, -2) >> 2);
    gui_hd_led (2);
}

/****i* januae-filesys-internal/action_examine_fh
 *
 *   NAME
 *       action_examine_fh()
 *
 *   SYNOPSIS
 *       static void action_examine_fh (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Fill a guest FileInfoBlock from the inode associated with a file key.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the file keys and root inode.
 *       *       packet -- Guest packet with the file-key identifier in argument 1 and the FileInfoBlock BPTR in argument 2.
 *
 *   RESULT
 *       None. get_fileinfo() writes the packet result and the guest information buffer.
 *
 *   NOTES
 *       An unknown file key selects the root inode. The FileInfoBlock disk-key field is then set to 0xFFFFFFFF for directories and zero for other objects.
 *
 ****************************************************************************/

static void
action_examine_fh (Unit *unit, dpacket packet)
{
    Key *k;
    a_inode *aino = 0;
    uaecptr info = GET_PCK_ARG2 (packet) << 2;

    TRACE(("ACTION_EXAMINE_FH(0x%lx,0x%lx)\n",
           GET_PCK_ARG1 (packet), GET_PCK_ARG2 (packet) ));

    k = lookup_key (unit, GET_PCK_ARG1 (packet));
    if (k != 0)
        aino = k->aino;
    if (aino == 0)
        aino = &unit->rootnode;

    get_fileinfo (unit, packet, info, aino);
    if (aino->dir)
        put_long (info, 0xFFFFFFFF);
    else
        put_long (info, 0);
}

/* For a nice example of just how contradictory documentation can be, see the
 * Autodoc for DOS:SetFileSize and the Packets.txt description of this packet...
 * This implementation tries to mimic the behaviour of the Kick 3.1 ramdisk
 * (which seems to match the Autodoc description). */
/****i* januae-filesys-internal/action_set_file_size
 *
 *   NAME
 *       action_set_file_size()
 *
 *   SYNOPSIS
 *       static void action_set_file_size (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Resize an open host file using the guest file-size packet.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the open file keys.
 *       *       packet -- Guest packet with a file-key identifier, signed offset, and seek mode.
 *
 *   RESULT
 *       None. The packet primary result receives the computed offset on the successful truncate path, or DOS_FALSE on a truncate failure.
 *
 *   NOTES
 *       Positive, negative, and zero modes select SEEK_END, SEEK_SET, and SEEK_CUR. The implementation raises the requested offset to accommodate other open file positions before seeking, writing one byte, and truncating. A missing file key sets DOS_TRUE with ERROR_OBJECT_NOT_AROUND; seek and write results are not checked.
 *
 ****************************************************************************/

static void
action_set_file_size (Unit *unit, dpacket packet)
{
    Key *k, *k1;
    off_t offset = GET_PCK_ARG2 (packet);
    long mode = (uae_s32)GET_PCK_ARG3 (packet);
    int whence = SEEK_CUR;

    if (mode > 0) whence = SEEK_END;
    if (mode < 0) whence = SEEK_SET;

    TRACE(("ACTION_SET_FILE_SIZE(0x%lx, %d, 0x%x)\n", GET_PCK_ARG1 (packet), offset, mode));

    k = lookup_key (unit, GET_PCK_ARG1 (packet));
    if (k == 0) {
        PUT_PCK_RES1 (packet, DOS_TRUE);
        PUT_PCK_RES2 (packet, ERROR_OBJECT_NOT_AROUND);
        return;
    }

    gui_hd_led (1);
    k->notifyactive = 1;
    /* If any open files have file pointers beyond this size, truncate only
     * so far that these pointers do not become invalid.  */
    for (k1 = unit->keys; k1; k1 = k1->next) {
        if (k != k1 && k->aino == k1->aino) {
            if (k1->file_pos > offset)
                offset = k1->file_pos;
        }
    }

    /* Write one then truncate: that should give the right size in all cases.  */
    offset = lseek (k->fd, offset, whence);
    write (k->fd, /* whatever */(char *)&k1, 1);
    if (k->file_pos > offset)
        k->file_pos = offset;
    lseek (k->fd, k->file_pos, SEEK_SET);

    /* Brian: no bug here; the file _must_ be one byte too large after writing
       The write is supposed to guarantee that the file can't be smaller than
       the requested size, the truncate guarantees that it can't be larger.
       If we were to write one byte earlier we'd clobber file data.  */
    if (truncate (k->aino->nname, offset) == -1) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, dos_errno ());
        return;
    }

    PUT_PCK_RES1 (packet, offset);
    PUT_PCK_RES2 (packet, 0);
}

/****i* januae-filesys-internal/relock_do
 *
 *   NAME
 *       relock_do()
 *
 *   SYNOPSIS
 *       static int relock_do (Unit *unit, a_inode *a1)
 *
 *   FUNCTION
 *       Close open host descriptors associated with an inode before retrying a rename.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the file keys.
 *       *       a1 -- Inode whose open descriptors are selected.
 *
 *   RESULT
 *       Number of selected keys whose descriptors were passed to close().
 *
 *   NOTES
 *       The descriptors remain recorded in the keys until relock_re() processes them. The return value of close() is not checked.
 *
 ****************************************************************************/

static int relock_do(Unit *unit, a_inode *a1)
{
    Key *k1, *knext;
    int wehavekeys = 0;

    for (k1 = unit->keys; k1; k1 = knext) {
        knext = k1->next;
        if (k1->aino == a1 && k1->fd >= 0) {
            wehavekeys++;
            close (k1->fd);
            write_log ("handle %p freed\n", k1->fd);
        }
    }
    return wehavekeys;
}

/****i* januae-filesys-internal/relock_re
 *
 *   NAME
 *       relock_re()
 *
 *   SYNOPSIS
 *       static void relock_re (Unit *unit, a_inode *a1, a_inode *a2, int failed)
 *
 *   FUNCTION
 *       Reopen file descriptors closed during a rename retry.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing the file keys.
 *       *       a1 -- Inode matched by the original file keys.
 *       *       a2 -- Destination inode when the rename succeeded.
 *       *       failed -- Nonzero when the rename failed and the old filename is to be reopened.
 *
 *   RESULT
 *       None. Matching file keys may receive reopened descriptors, be reassigned to a2, or be freed.
 *
 *   NOTES
 *       The open mode is derived from each key's DOS access flags. Failed opens cause free_key() to be called; successful opens seek to the recorded file position without checking that seek result. A successful rename with no destination inode does not reopen its matching descriptors.
 *
 ****************************************************************************/

static void relock_re(Unit *unit, a_inode *a1, a_inode *a2, int failed)
{
    Key *k1, *knext;

    for (k1 = unit->keys; k1; k1 = knext) {
        knext = k1->next;
        if (k1->aino == a1 && k1->fd >= 0) {
            int mode = (k1->dosmode & A_FIBF_READ) == 0 ? O_WRONLY : (k1->dosmode & A_FIBF_WRITE) == 0 ? O_RDONLY : O_RDWR;
            mode |= O_BINARY;
            if (failed) {
                /* rename still failed, restore fd */
                k1->fd = open (a1->nname, mode, 0777);
                write_log ("restoring old handle '%s' %d\n", a1->nname, k1->dosmode);
            } else {
                /* transfer fd to new name */
                if (a2) {
                    k1->aino = a2;
                    k1->fd = open (a2->nname, mode, 0777);
                    write_log ("restoring new handle '%s' %d\n", a2->nname, k1->dosmode);
                } else {
                    write_log ("no new handle, deleting old lock(s).\n");
                }
            }
            if (k1->fd < 0) {
                write_log ("relocking failed '%s' -> '%s'\n", a1->nname, a2->nname);
                free_key (unit, k1);
            } else {
                lseek (k1->fd, k1->file_pos, SEEK_SET);
            }
        }
    }
}

/****i* januae-filesys-internal/action_delete_object
 *
 *   NAME
 *       action_delete_object()
 *
 *   SYNOPSIS
 *       static void action_delete_object (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Remove a guest filesystem object from the host filesystem.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing mount and inode state.
 *       *       packet -- Guest packet with a parent lock and object name.
 *
 *   RESULT
 *       None. The packet primary result is DOS_TRUE after removal, or DOS_FALSE on the checked failure paths.
 *
 *   NOTES
 *       Rejects write-protected units, unresolved objects, delete-protected objects, and objects with active inode locks. Directories are removed with rmdir() after metadata writeback; other objects use unlink(). On success the inode is deleted or marked deleted if children remain, notifications are checked, and the parent time is updated.
 *
 ****************************************************************************/

static void
action_delete_object (Unit *unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG1 (packet) << 2;
    uaecptr name = GET_PCK_ARG2 (packet) << 2;
    a_inode *a;
    uae_u32 err;

    TRACE(("ACTION_DELETE_OBJECT(0x%lx,\"%s\")\n", lock, bstr (unit, name)));

    if (unit->ui.readonly) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
        return;
    }

    a = find_aino (unit, lock, bstr (unit, name), &err);

    if (err != 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err);
        return;
    }
    if (a->amigaos_mode & A_FIBF_DELETE) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DELETE_PROTECTED);
        return;
    }
    if (a->shlock > 0 || a->elock) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_OBJECT_IN_USE);
        return;
    }
    if (a->dir) {
        /* This should take care of removing the fsdb if no files remain.  */
        fsdb_dir_writeback (a);
        if (rmdir (a->nname) == -1) {
            PUT_PCK_RES1 (packet, DOS_FALSE);
            PUT_PCK_RES2 (packet, dos_errno());
            return;
        }
    } else {
        if (unlink (a->nname) == -1) {
            PUT_PCK_RES1 (packet, DOS_FALSE);
            PUT_PCK_RES2 (packet, dos_errno());
            return;
        }
    }
    notify_check (unit, a);
    updatedirtime (a, 1);
    if (a->child != 0) {
        write_log ("Serious error in action_delete_object.\n");
        a->deleted = 1;
    } else {
        delete_aino (unit, a);
    }
    PUT_PCK_RES1 (packet, DOS_TRUE);
    gui_hd_led (2);
}

/****i* januae-filesys-internal/action_set_date
 *
 *   NAME
 *       action_set_date()
 *
 *   SYNOPSIS
 *       static void action_set_date (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Set the date of a named guest filesystem object.
 *
 *   INPUTS
 *       unit -- Filesystem unit containing mount and inode state.
 *       *       packet -- Guest packet containing the parent lock, object name, and guest date pointer.
 *
 *   RESULT
 *       None. The packet primary result is DOS_TRUE on success or DOS_FALSE when a checked operation fails.
 *
 *   NOTES
 *       Rejects read-only units. TARGET_AMIGAOS with WORDS_BIGENDIAN uses SetFileDate(); other builds convert the guest timestamp and call utime(). Notification checking and activity indication run after the date operation, including its failure path.
 *
 ****************************************************************************/

static void
action_set_date (Unit *unit, dpacket packet)
{
    uaecptr lock = GET_PCK_ARG2 (packet) << 2;
    uaecptr name = GET_PCK_ARG3 (packet) << 2;
    uaecptr date = GET_PCK_ARG4 (packet);
    a_inode *a;
    struct utimbuf ut;
    uae_u32 err;

    TRACE(("ACTION_SET_DATE(0x%lx,\"%s\")\n", lock, bstr (unit, name)));

    if (unit->ui.readonly) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
        return;
    }

    a = find_aino (unit, lock, bstr (unit, name), &err);   
#if defined TARGET_AMIGAOS && defined WORDS_BIGENDIAN
    if (err == 0 && SetFileDate (a->nname, (struct DateStamp *) date) == DOSFALSE)
        err = IoErr ();
#else   
    ut.actime = ut.modtime = put_time(get_long (date), get_long (date + 4),
                                      get_long (date + 8));
    if (err == 0 && utime (a->nname, &ut) == -1)
        err = dos_errno ();
#endif
    if (err != 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err);
    } else
        PUT_PCK_RES1 (packet, DOS_TRUE);
    notify_check (unit, a);
    gui_hd_led (2);
}

/****i* januae-filesys-internal/action_rename_object
 *
 *   NAME
 *       action_rename_object()
 *
 *   SYNOPSIS
 *       static void action_rename_object (Unit *unit, dpacket packet)
 *
 *   FUNCTION
 *       Rename or move a guest filesystem object and update its inode references.
 *
 *   INPUTS
 *       unit -- Filesystem unit holding the source and destination directories.
 *       *       packet -- Guest packet containing source lock/name and destination lock/name.
 *
 *   RESULT
 *       None. The packet primary result is DOS_TRUE after a successful rename or an exact-name no-op, or DOS_FALSE on checked failure paths.
 *
 *   NOTES
 *       Rejects read-only units and source files opened in creation mode 2. A failed host rename triggers a retry after relock_do(), followed by relock_re(). On success the function transfers inode metadata and child, key, and examine references to the new inode and updates notifications and parent time. Reopen and seek failures are not propagated as a separate packet result.
 *
 ****************************************************************************/

static void
action_rename_object (Unit *unit, dpacket packet)
{
    uaecptr lock1 = GET_PCK_ARG1 (packet) << 2;
    uaecptr name1 = GET_PCK_ARG2 (packet) << 2;
    uaecptr lock2 = GET_PCK_ARG3 (packet) << 2;
    uaecptr name2 = GET_PCK_ARG4 (packet) << 2;
    a_inode *a1, *a2;
    uae_u32 err1, err2;
    Key *k1, *knext;
    int wehavekeys = 0;

    TRACE(("ACTION_RENAME_OBJECT(0x%lx,\"%s\",", lock1, bstr (unit, name1)));
    TRACE(("0x%lx,\"%s\")\n", lock2, bstr (unit, name2)));

    if (unit->ui.readonly) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
        return;
    }

    a1 = find_aino (unit, lock1, bstr (unit, name1), &err1);
    if (err1 != 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err1);
        return;
    }

    /* rename always fails if file is open for writing */
    for (k1 = unit->keys; k1; k1 = knext) {
        knext = k1->next;
        if (k1->aino == a1 && k1->fd >= 0 && k1->createmode == 2) {
            PUT_PCK_RES1 (packet, DOS_FALSE);
            PUT_PCK_RES2 (packet, ERROR_OBJECT_IN_USE);
            return;
        }
    }

    /* See whether the other name already exists in the filesystem.  */
    a2 = find_aino (unit, lock2, bstr (unit, name2), &err2);

    if (a2 == a1) {
        /* Renaming to the same name, but possibly different case.  */
        if (strcmp (a1->aname, bstr_cut (unit, name2)) == 0) {
            /* Exact match -> do nothing.  */
            notify_check (unit, a1);
            updatedirtime (a1, 1);
            PUT_PCK_RES1 (packet, DOS_TRUE);
            return;
        }
        a2 = a2->parent;
    } else if (a2 == 0 || err2 != ERROR_OBJECT_NOT_AROUND) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, err2 == 0 ? ERROR_OBJECT_EXISTS : err2);
        return;
    }

    a2 = create_child_aino (unit, a2, bstr_cut (unit, name2), a1->dir);
    if (a2 == 0) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_IS_FULL); /* best we can do */
        return;
    }

    if (-1 == rename (a1->nname, a2->nname)) {
        int ret = -1;
        /* maybe we have open file handles that caused failure? */
        write_log ("rename '%s' -> '%s' failed, trying relocking..\n", a1->nname, a2->nname);
        wehavekeys = relock_do (unit, a1);
        /* try again... */
        ret = rename (a1->nname, a2->nname);
        /* restore locks */
        relock_re (unit, a1, a2, ret == -1 ? 1 : 0);
        if (ret == -1) {
            delete_aino (unit, a2);
            PUT_PCK_RES1 (packet, DOS_FALSE);
            PUT_PCK_RES2 (packet, dos_errno ());
            return;
        }
    }

    notify_check (unit, a1);
    notify_check (unit, a2);
    a2->comment = a1->comment;
    a1->comment = 0;
    a2->amigaos_mode = a1->amigaos_mode;
    a2->uniq = a1->uniq;
    a2->elock = a1->elock;
    a2->shlock = a1->shlock;
    a2->has_dbentry = a1->has_dbentry;
    a2->db_offset = a1->db_offset;
    a2->dirty = 0;
    /* Ugly fix: update any keys to point to new aino */
    for (k1 = unit->keys; k1; k1 = knext) {
        knext = k1->next;
        if (k1->aino == a1) k1->aino=a2;
    }
    move_exkeys (unit, a1, a2);
    move_aino_children (unit, a1, a2);
    delete_aino (unit, a1);
    a2->dirty = 1;
    if (a2->parent)
        fsdb_dir_writeback (a2->parent);
    updatedirtime (a2, 1);
    fsdb_set_file_attrs (a2);
    if (a2->elock > 0 || a2->shlock > 0 || wehavekeys > 0)
        de_recycle_aino (unit, a2);
    PUT_PCK_RES1 (packet, DOS_TRUE);
    gui_hd_led (2);
}

static void
action_current_volume (Unit *unit, dpacket packet)
{
    PUT_PCK_RES1 (packet, unit->volume >> 2);
}

static void
action_rename_disk (Unit *unit, dpacket packet)
{
    uaecptr name = GET_PCK_ARG1 (packet) << 2;
    int i;
    int namelen;

    TRACE(("ACTION_RENAME_DISK(\"%s\")\n", bstr (unit, name)));

    if (unit->ui.readonly) {
        PUT_PCK_RES1 (packet, DOS_FALSE);
        PUT_PCK_RES2 (packet, ERROR_DISK_WRITE_PROTECTED);
        return;
    }

    /* get volume name */
    namelen = get_byte (name); name++;
    xfree (unit->ui.volname);
    unit->ui.volname = (char *) xmalloc (namelen + 1);
    for (i = 0; i < namelen; i++, name++)
        unit->ui.volname[i] = get_byte (name);
    unit->ui.volname[i] = 0;

    put_byte (unit->volume + 44, namelen);
    for (i = 0; i < namelen; i++)
        put_byte (unit->volume + 45 + i, unit->ui.volname[i]);

    PUT_PCK_RES1 (packet, DOS_TRUE);
}

static void
action_is_filesystem (Unit *unit, dpacket packet)
{
    PUT_PCK_RES1 (packet, DOS_TRUE);
}

static void
action_flush (Unit *unit, dpacket packet)
{
    /* sync(); */ /* pretty drastic, eh */
    PUT_PCK_RES1 (packet, DOS_TRUE);
}

/* We don't want multiple interrupts to be active at the same time. I don't
 * know whether AmigaOS takes care of that, but this does. */
static uae_sem_t singlethread_int_sem;

extern void call_68k(TrapContext *context, 
                     ULONG arg1,
                     ULONG arg2,
                     ULONG arg3,
                     ULONG arg4,
                     ULONG arg5,
                     ULONG arg6,
                     ULONG arg7,
                     ULONG arg8); /* for ami-win.c*/

static uae_u32 REGPARAM2 exter_int_helper (TrapContext *context)
{
    UnitInfo *uip = current_mountinfo.ui;
    uaecptr port;
    static int unit_no;
    int o1i=0;

    switch (m68k_dreg (&context->regs, 0)) {
     case 0:
        /* Determine whether a given EXTER interrupt is for us. */
        if (uae_int_requested) {
            if (uae_sem_trywait (&singlethread_int_sem) != 0)
                /* Pretend it isn't for us. We might get it again later. */
                return 0;
            /* Clear the interrupt flag _before_ we do any processing.
             * That way, we can get too many interrupts, but never not
             * enough. */
            uae_int_requested = 0;
            unit_no = 0;
            return 1;
        }
        return 0;
     case 1:
        /* Release a message_lock. This is called as soon as the message is
         * received by the assembly code. We use the opportunity to check
         * whether we have some locks that we can give back to the assembler
         * code.
         * Note that this is called from the main loop, unlike the other cases
         * in this switch statement which are called from the interrupt handler.
         */
#ifdef UAE_FILESYS_THREADS
        {
            Unit *unit = find_unit (m68k_areg (&context->regs, 5));
            uaecptr msg = m68k_areg (&context->regs, 4);
            unit->cmds_complete = unit->cmds_acked;
            while (comm_pipe_has_data (unit->ui.back_pipe)) {
                uaecptr locks, lockend;
                int cnt = 0;
                locks = read_comm_pipe_int_blocking (unit->ui.back_pipe);
                lockend = locks;
                while (get_long (lockend) != 0) {
                    if (get_long (lockend) == lockend) {
                        write_log ("filesystem lock queue corrupted!\n");
                        break;
                    }
                    lockend = get_long (lockend);
                    cnt++;
                }
                TRACE(("%d %x %x %x\n", cnt, locks, lockend, m68k_areg (&context->regs, 3)));
                put_long (lockend, get_long (m68k_areg (&context->regs, 3)));
                put_long (m68k_areg (&context->regs, 3), locks);
            }
        }
#else
        write_log ("exter_int_helper should not be called with arg 1!\n");
#endif
        break;
     case 2:
        /* Find work that needs to be done:
         * return d0 = 0: none
         *        d0 = 1: PutMsg(), port in a0, message in a1
         *        d0 = 2: Signal(), task in a1, signal set in d1
         *        d0 = 3: ReplyMsg(), message in a1
         *        d0 = 4: Cause(), interrupt in a1
         *        d0 = 5: AllocMem(), size in d0, flags in d1, pointer to address at a0
         *        d0 = 6: FreeMem(), memory in a1, size in d0
         */

#ifdef SUPPORT_THREADS
        /* First, check signals/messages */
        while (comm_pipe_has_data (&native2amiga_pending)) {
            int cmd = read_comm_pipe_int_blocking (&native2amiga_pending);
            switch (cmd) {
             case 0: /* Signal() */
                m68k_areg (&context->regs, 1) = read_comm_pipe_u32_blocking (&native2amiga_pending);
                m68k_dreg (&context->regs, 1) = read_comm_pipe_u32_blocking (&native2amiga_pending);
                return 2;

             case 1: /* PutMsg() */
                m68k_areg (&context->regs, 0) = read_comm_pipe_u32_blocking (&native2amiga_pending);
                m68k_areg (&context->regs, 1) = read_comm_pipe_u32_blocking (&native2amiga_pending);
                return 1;

             case 2: /* ReplyMsg() */
                m68k_areg (&context->regs, 1) = read_comm_pipe_u32_blocking (&native2amiga_pending);
                return 3;

             case 3: /* Cause() */
                m68k_areg (&context->regs, 1) = read_comm_pipe_u32_blocking (&native2amiga_pending);
                return 4;

             case 4: /* NotifyHack() */
                m68k_areg (&context->regs, 0) = read_comm_pipe_u32_blocking (&native2amiga_pending);
                m68k_areg (&context->regs, 1) = read_comm_pipe_u32_blocking (&native2amiga_pending);
                return 5;

             default:
                write_log ("exter_int_helper: unknown native action %d\n", cmd);
                break;
            }
        }
#endif

        if(!o1i) {
        /* Find some unit that needs a message sent, and return its port,
         * or zero if all are done.
         * Take care not to dereference self for units that didn't have their
         * startup packet sent. */
        for (;;) {
            if (unit_no >= current_mountinfo.num_units)
                return 0;

            if (uip[unit_no].self != 0
                && uip[unit_no].self->cmds_acked == uip[unit_no].self->cmds_complete
                && uip[unit_no].self->cmds_acked != uip[unit_no].self->cmds_sent)
                break;
            unit_no++;
        }
        uip[unit_no].self->cmds_acked = uip[unit_no].self->cmds_sent;
        port = uip[unit_no].self->port;
        if (port) {
            m68k_areg (&context->regs, 0) = port;
            m68k_areg (&context->regs, 1) = find_unit (port)->dummy_message;
            unit_no++;
            return 1;
        }
        }
        break;
     case 4:
        /* Exit the interrupt, and release the single-threading lock. */
        uae_sem_post (&singlethread_int_sem);
        break;


     default:
        write_log ("Shouldn't happen in exter_int_helper.\n");
        break;
    }
    return 0;
}

static int handle_packet (Unit *unit, dpacket pck)
{
    uae_s32 type = GET_PCK_TYPE (pck);
    PUT_PCK_RES2 (pck, 0);
    switch (type) {
     case ACTION_LOCATE_OBJECT: action_lock (unit, pck); break;
     case ACTION_FREE_LOCK: action_free_lock (unit, pck); break;
     case ACTION_COPY_DIR: action_dup_lock (unit, pck); break;
     case ACTION_DISK_INFO: action_disk_info (unit, pck); break;
     case ACTION_INFO: action_info (unit, pck); break;
     case ACTION_EXAMINE_OBJECT: action_examine_object (unit, pck); break;
     case ACTION_EXAMINE_NEXT: action_examine_next (unit, pck); break;
     case ACTION_FIND_INPUT: action_find_input (unit, pck); break;
     case ACTION_FIND_WRITE: action_find_write (unit, pck); break;
     case ACTION_FIND_OUTPUT: action_find_output (unit, pck); break;
     case ACTION_END: action_end (unit, pck); break;
     case ACTION_READ: action_read (unit, pck); break;
     case ACTION_WRITE: action_write (unit, pck); break;
     case ACTION_SEEK: action_seek (unit, pck); break;
     case ACTION_SET_PROTECT: action_set_protect (unit, pck); break;
     case ACTION_SET_COMMENT: action_set_comment (unit, pck); break;
     case ACTION_SAME_LOCK: action_same_lock (unit, pck); break;
     case ACTION_PARENT: action_parent (unit, pck); break;
     case ACTION_CREATE_DIR: action_create_dir (unit, pck); break;
     case ACTION_DELETE_OBJECT: action_delete_object (unit, pck); break;
     case ACTION_RENAME_OBJECT: action_rename_object (unit, pck); break;
     case ACTION_SET_DATE: action_set_date (unit, pck); break;
     case ACTION_CURRENT_VOLUME: action_current_volume (unit, pck); break;
     case ACTION_RENAME_DISK: action_rename_disk (unit, pck); break;
     case ACTION_IS_FILESYSTEM: action_is_filesystem (unit, pck); break;
     case ACTION_FLUSH: action_flush (unit, pck); break;

     /* 2.0+ packet types */
     case ACTION_SET_FILE_SIZE: action_set_file_size (unit, pck); break;
     case ACTION_EXAMINE_FH: action_examine_fh (unit, pck); break;
     case ACTION_FH_FROM_LOCK: action_fh_from_lock (unit, pck); break;
     case ACTION_COPY_DIR_FH: action_lock_from_fh (unit, pck); break;
     case ACTION_CHANGE_MODE: action_change_mode (unit, pck); break;
     case ACTION_PARENT_FH: action_parent_fh (unit, pck); break;
     case ACTION_ADD_NOTIFY: action_add_notify (unit, pck); break;
     case ACTION_REMOVE_NOTIFY: action_remove_notify (unit, pck); break;

     /* unsupported packets */
     case ACTION_LOCK_RECORD:
     case ACTION_FREE_RECORD:
     case ACTION_EXAMINE_ALL:
     case ACTION_MAKE_LINK:
     case ACTION_READ_LINK:
     case ACTION_FORMAT:
         return 0;
     default:
        write_log ("FILESYS: UNKNOWN PACKET %x\n", type);
        return 0;
    }

    if(GET_PCK_RES1(pck) != DOSFALSE) {
      TRACE(("result: DOS TRUE (ok)\n"));
    }
    else {
      TRACE(("result: ERROR %d -- DOSFALSE\n", GET_PCK_RES2(pck)));
    }
    return 1;
}

#ifdef UAE_FILESYS_THREADS
static void *filesys_thread (void *unit_v)
{
    UnitInfo *ui = (UnitInfo *)unit_v;

    uae_set_thread_priority (2);
    for (;;) {
        uae_u8 *pck;
        uae_u8 *msg;
        uae_u32 morelocks;

        pck = (uae_u8 *)read_comm_pipe_pvoid_blocking (ui->unit_pipe);
        msg = (uae_u8 *)read_comm_pipe_pvoid_blocking (ui->unit_pipe);
        morelocks = (uae_u32)read_comm_pipe_int_blocking (ui->unit_pipe);

        if (ui->reset_state == FS_GO_DOWN) {
            if (pck != 0)
                continue;
            /* Death message received. */
            uae_sem_post ((uae_sem_t *)&ui->reset_sync_sem);
            /* Die.  */
            return 0;
        }

        put_long (get_long (morelocks), get_long (ui->self->locklist));
        put_long (ui->self->locklist, morelocks);
        if (! handle_packet (ui->self, pck)) {
            PUT_PCK_RES1 (pck, DOS_FALSE);
            PUT_PCK_RES2 (pck, ERROR_ACTION_NOT_KNOWN);
        }
        /* Mark the packet as processed for the list scan in the assembly code. */
        do_put_mem_long ((uae_u32 *)(msg + 4), -1);
        /* Acquire the message lock, so that we know we can safely send the
         * message. */
        ui->self->cmds_sent++;
        /* The message is sent by our interrupt handler, so make sure an interrupt
         * happens. */
        do_uae_int_requested ();
        /* Send back the locks. */
        if (get_long (ui->self->locklist) != 0)
            write_comm_pipe_int (ui->back_pipe, (int)(get_long (ui->self->locklist)), 0);
        put_long (ui->self->locklist, 0);
    }
    return 0;
}
#endif

/* Talk about spaghetti code... */
static uae_u32 REGPARAM2 filesys_handler (TrapContext *context)
{
    Unit *unit = find_unit (m68k_areg (&context->regs, 5));
    uaecptr packet_addr = m68k_dreg (&context->regs, 3);
    uaecptr message_addr = m68k_areg (&context->regs, 4);
    uae_u8 *pck;
    uae_u8 *msg;

    TRACE(("entered\n"));

    if (! valid_address (packet_addr, 36) || ! valid_address (message_addr, 14)) {
        write_log ("Bad address passed for packet.\n");
        goto error2;
    }
    pck = get_real_address (packet_addr);
    msg = get_real_address (message_addr);


    do_put_mem_long ((uae_u32 *)(msg + 4), -1);
    if (!unit || !unit->volume) {
      write_log ("Filesystem was not initialized.\n");
      goto error;
    }
#ifdef UAE_FILESYS_THREADS
    {
        uae_u32 morelocks;
        if (!unit->ui.unit_pipe)
            goto error;
        /* Get two more locks and hand them over to the other thread. */
        morelocks = get_long (m68k_areg (&context->regs, 3));
        put_long (m68k_areg (&context->regs, 3), get_long (get_long (morelocks)));
        put_long (get_long (morelocks), 0);

        /* The packet wasn't processed yet. */
        do_put_mem_long ((uae_u32 *)(msg + 4), 0);
        write_comm_pipe_pvoid (unit->ui.unit_pipe, (void *)pck, 0);
        write_comm_pipe_pvoid (unit->ui.unit_pipe, (void *)msg, 0);
        write_comm_pipe_int (unit->ui.unit_pipe, (int)morelocks, 1);
        /* Don't reply yet. */
        return 1;
    }
#endif

    if (! handle_packet (unit, pck)) {
        error:
        TRACE(("ERROR: DOS_FALSE - ERROR_ACTION_NOT_KNOWN\n"));
        PUT_PCK_RES1 (pck, DOS_FALSE);
        PUT_PCK_RES2 (pck, ERROR_ACTION_NOT_KNOWN);
    }
    TRACE(("reply: get_pck_res1 %8lx, get_pck_res2 %ld\n", GET_PCK_RES1 (pck), GET_PCK_RES2 (pck)));

    error2:

    return 0;
}

static void init_filesys_diagentry (void)
{
    do_put_mem_long ((uae_u32 *)(filesysory + 0x2100), EXPANSION_explibname);
    do_put_mem_long ((uae_u32 *)(filesysory + 0x2104), filesys_configdev);
    do_put_mem_long ((uae_u32 *)(filesysory + 0x2108), EXPANSION_doslibname);
    do_put_mem_long ((uae_u32 *)(filesysory + 0x210c), current_mountinfo.num_units);
    native2amiga_startup ();
}

void filesys_start_threads (void)
{
    UnitInfo *uip;
    int i;

    free_mountinfo (&current_mountinfo);
    dup_mountinfo (&options_mountinfo, &current_mountinfo);
    uip = current_mountinfo.ui;
    for (i = 0; i < current_mountinfo.num_units; i++) {
        UnitInfo *ui = &uip[i];
        ui->unit_pipe = 0;
        ui->back_pipe = 0;
        ui->reset_state = FS_STARTUP;
        if (savestate_state != STATE_RESTORE) {
            ui->startup = 0;
            ui->self = 0;
        }
#ifdef UAE_FILESYS_THREADS
        if (is_hardfile (&current_mountinfo, i) == FILESYS_VIRTUAL) {
            ui->unit_pipe = (smp_comm_pipe *)xmalloc (sizeof (smp_comm_pipe));
            ui->back_pipe = (smp_comm_pipe *)xmalloc (sizeof (smp_comm_pipe));
            init_comm_pipe (uip[i].unit_pipe, 100, 3);
            init_comm_pipe (uip[i].back_pipe, 100, 1);
            uae_start_thread (filesys_thread, (void *)(uip + i), &uip[i].tid);
        }
#endif
        if (savestate_state == STATE_RESTORE)
            startup_update_unit (uip->self, uip);
    }
}

void filesys_cleanup (void)
{
    free_mountinfo (&current_mountinfo);
}

void filesys_reset (void)
{
    Unit *u, *u1;

    /* We get called once from customreset at the beginning of the program
     * before filesys_start_threads has been called. Survive that.  */
    if (savestate_state == STATE_RESTORE)
        return;

    for (u = units; u; u = u1) {
        u1 = u->next;
        xfree (u);
    }
    unit_num = 0;
    units = 0;
}

static void free_all_ainos (Unit *u, a_inode *parent)
{
    a_inode *a;
    while ((a = parent->child)) {
        free_all_ainos (u, a);
        dispose_aino (u, &parent->child, a);
    }
}

void filesys_flush_cache (void)
{
}

void filesys_prepare_reset (void)
{
    UnitInfo *uip;
    Unit *u;
    int i;

    if (savestate_state == STATE_RESTORE)
        return;
    uip = current_mountinfo.ui;
#ifdef UAE_FILESYS_THREADS
    for (i = 0; i < current_mountinfo.num_units; i++) {
        if (uip[i].unit_pipe != 0) {
            uae_sem_init ((uae_sem_t *)&uip[i].reset_sync_sem, 0, 0);
            uip[i].reset_state = FS_GO_DOWN;
            /* send death message */
            write_comm_pipe_int (uip[i].unit_pipe, 0, 0);
            write_comm_pipe_int (uip[i].unit_pipe, 0, 0);
            write_comm_pipe_int (uip[i].unit_pipe, 0, 1);
            uae_sem_wait ((uae_sem_t *)&uip[i].reset_sync_sem);
        }
    }
#endif
    u = units;
    while (u != 0) {
        free_all_ainos (u, &u->rootnode);
        u->rootnode.next = u->rootnode.prev = &u->rootnode;
        u->aino_cache_size = 0;
        u = u->next;
    }
}

static uae_u32 REGPARAM2 filesys_diagentry (TrapContext *context)
{
    uaecptr resaddr = m68k_areg (&context->regs, 2) + 0x10;
    uaecptr start = resaddr;
    uaecptr residents, tmp;

    TRACE (("filesystem: diagentry called\n"));

    filesys_configdev = m68k_areg (&context->regs, 3);
    init_filesys_diagentry ();

    uae_sem_init (&singlethread_int_sem, 0, 1);
    if (ROM_hardfile_resid != 0) {
        /* Build a struct Resident. This will set up and initialize
         * the uae.device */
        put_word (resaddr + 0x0, 0x4AFC);
        put_long (resaddr + 0x2, resaddr);
        put_long (resaddr + 0x6, resaddr + 0x1A); /* Continue scan here */
        put_word (resaddr + 0xA, 0x8101); /* RTF_AUTOINIT|RTF_COLDSTART; Version 1 */
        put_word (resaddr + 0xC, 0x0305); /* NT_DEVICE; pri 05 */
        put_long (resaddr + 0xE, ROM_hardfile_resname);
        put_long (resaddr + 0x12, ROM_hardfile_resid);
        put_long (resaddr + 0x16, ROM_hardfile_init); /* calls filesys_init */
    }
    resaddr += 0x1A;
    tmp = resaddr;

    /* The good thing about this function is that it always gets called
     * when we boot. So we could put all sorts of stuff that wants to be done
     * here.
     * We can simply add more Resident structures here. Although the Amiga OS
     * only knows about the one at address DiagArea + 0x10, we scan for other
     * Resident structures and call InitResident() for them at the end of the
     * diag entry. */

    resaddr = scsidev_startup(resaddr);

    /* scan for Residents and return pointer to array of them */
    residents = resaddr;
    while (tmp < residents && tmp > start) {
        if (get_word (tmp) == 0x4AFC &&
            get_long (tmp + 0x2) == tmp) {
            put_word (resaddr, 0x227C);         /* movea.l #tmp,a1 */
            put_long (resaddr + 2, tmp);
            put_word (resaddr + 6, 0x7200);     /* moveq.l #0,d1 */
            put_long (resaddr + 8, 0x4EAEFF9A); /* jsr -$66(a6) ; InitResident */
            resaddr += 12;
            tmp = get_long (tmp + 0x6);
        } else {
            tmp++;
        }
    }
    /* call setup_exter */
    put_word (resaddr +  0, 0x2079);
    put_long (resaddr +  2, RTAREA_BASE + 28 + 4); /* move.l RTAREA_BASE+32,a0 */
    put_word (resaddr +  6, 0xd1fc);
    put_long (resaddr +  8, RTAREA_BASE + 8 + 4); /* add.l #RTAREA_BASE+12,a0 */
    put_word (resaddr + 12, 0x4e90); /* jsr (a0) */
    put_word (resaddr + 14, 0x7001); /* moveq.l #1,d0 */
    put_word (resaddr + 16, RTS);

    m68k_areg (&context->regs, 0) = residents;
    return 1;
}

/* don't forget filesys.asm! */
#define PP_MAXSIZE 4 * 96
#define PP_FSSIZE 400
#define PP_FSPTR 404
#define PP_FSRES 408
#define PP_EXPLIB 412
#define PP_FSHDSTART 416

static uae_u32 REGPARAM2 filesys_dev_bootfilesys (TrapContext *context)
{
    uaecptr devicenode = m68k_areg (&context->regs, 3);
    uaecptr parmpacket = m68k_areg (&context->regs, 1);
    uaecptr fsres = get_long (parmpacket + PP_FSRES);
    uaecptr fsnode;
    uae_u32 dostype, dostype2;
    UnitInfo *uip = current_mountinfo.ui;
    uae_u32 no = m68k_dreg (&context->regs, 6);
    int unit_no = no & 65535;
    int type = is_hardfile (&current_mountinfo, unit_no);

    if (type == FILESYS_VIRTUAL)
        return 0;
    dostype = get_long (parmpacket + 80);
    fsnode = get_long (fsres + 18);
    while (get_long (fsnode)) {
        dostype2 = get_long (fsnode + 14);
        if (dostype2 == dostype) {
            if (get_long (fsnode + 22) & (1 << 7)) {
                put_long (devicenode + 32, get_long (fsnode + 54)); /* dn_SegList */
                put_long (devicenode + 36, -1); /* dn_GlobalVec */
            }
            return 1;
        }
        fsnode = get_long (fsnode);
    }
    return 0;
}

/* Remember a pointer AmigaOS gave us so we can later use it to identify
 * which unit a given startup message belongs to.  */
static uae_u32 REGPARAM2 filesys_dev_remember (TrapContext *context)
{
    uae_u32 no = m68k_dreg (&context->regs, 6);
    int unit_no = no & 65535;
    int sub_no = no >> 16;
    UnitInfo *uip = &current_mountinfo.ui[unit_no];
    int i;
    uaecptr devicenode = m68k_areg (&context->regs, 3);
    uaecptr parmpacket = m68k_areg (&context->regs, 1);

    /* copy filesystem loaded from RDB */
    if (get_long (parmpacket + PP_FSPTR)) {
        for (i = 0; i < uip->rdb_filesyssize; i++)
            put_byte (get_long (parmpacket + PP_FSPTR) + i, uip->rdb_filesysstore[i]);
        xfree (uip->rdb_filesysstore);
        uip->rdb_filesysstore = 0;
        uip->rdb_filesyssize = 0;
    }
    if ( ((uae_s32)m68k_dreg (&context->regs, 3)) >= 0)
        uip->startup = get_long (devicenode + 28);
    return devicenode;
}

static int legalrdbblock (UnitInfo *uip, unsigned int block)
{
    if (block == 0)
        return 0;
    if (block >= uip->hf.size / uip->hf.blocksize)
        return 0;
    return 1;
}

STATIC_INLINE unsigned int rl (uae_u8 *p)
{
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | (p[3]);
}

static int rdb_checksum (const char *id, uae_u8 *p, int block)
{
    uae_u32 sum = 0;
    int i, blocksize;

    if (memcmp (id, p, 4))
        return 0;
    blocksize = rl (p + 4);
    if (blocksize < 1 || blocksize * 4 > FILESYS_MAX_BLOCKSIZE)
        return 0;
    for (i = 0; i < blocksize; i++)
        sum += rl (p + i * 4);
    sum = -sum;
    if (sum) {
        write_log ("RDB: block %d ('%s') checksum error\n", block, id);
        return 1;
    }
    return 1;
}

static char *device_dupfix (uaecptr expbase, const char *devname)
{
    uaecptr bnode, dnode, name;
    int len, i, modified;
    char dname[256], newname[256];

    strcpy (newname, devname);
    modified = 1;
    while (modified) {
        modified = 0;
        bnode = get_long (expbase + 74); /* expansion.library bootnode list */
        while (get_long (bnode)) {
            dnode = get_long (bnode + 16); /* device node */
            name = get_long (dnode + 40) << 2; /* device name BSTR */
            len = get_byte(name);
            for (i = 0; i < len; i++)
                dname[i] = get_byte (name + 1 + i);
            dname[len] = 0;
            for (;;) {
                if (!strcasecmp (newname, dname)) {
                    if (strlen (newname) > 2 && newname[strlen (newname) - 2] == '_') {
                        newname[strlen (newname) - 1]++;
                    } else {
                        strcat (newname, "_0");
                    }
                    modified = 1;
                } else {
                    break;
                }
            }
            bnode = get_long (bnode);
        }
    }
    return strdup (newname);
}

static void dump_partinfo (const char *name, int num, uaecptr pp)
{
    uae_u32 dostype = get_long (pp + 80);
    write_log ("RDB: '%s' dostype=%08.8X\n", name, dostype);
    write_log ("BlockSize: %d, Surfaces: %d, SectorsPerBlock %d\n",
               get_long (pp + 20) * 4, get_long (pp + 28), get_long (pp + 32));
    write_log ("SectorsPerTrack: %d, Reserved: %d, LowCyl %d, HighCyl %d\n",
               get_long (pp + 36), get_long (pp + 40), get_long (pp + 52), get_long (pp + 56));
    write_log ("Buffers: %d, BufMemType: %08.8x, MaxTransfer: %08.8x, BootPri: %d\n",
               get_long (pp + 60), get_long (pp + 64), get_long (pp + 68), get_long (pp + 76));
}

#define rdbmnt write_log ("Mounting uaehf.device %d (%d) (size=%I64u):\n", unit_no, partnum, hfd->size);

static int rdb_mount (UnitInfo *uip, int unit_no, unsigned int partnum, uaecptr parmpacket)
{
    unsigned int lastblock = 63, blocksize, readblocksize;
    int driveinitblock, badblock;
    uae_u8 bufrdb[FILESYS_MAX_BLOCKSIZE], *buf = 0;
    uae_u8 *fsmem = 0;
    unsigned int rdblock, partblock, fileblock, i;
    int lsegblock;
    uae_u32 flags;
    struct hardfiledata *hfd = &uip->hf;
    uae_u32 dostype;
    uaecptr fsres, fsnode;
    int err = 0;
    int oldversion, oldrevision;
    int newversion, newrevision;

    if (hfd->blocksize == 0) {
        rdbmnt
        write_log ("failed, blocksize == 0\n");
        return -1;
    }
    if (lastblock * hfd->blocksize > hfd->size) {
        rdbmnt
        write_log ("failed, too small (%d*%d > %I64u)\n", lastblock, hfd->blocksize, hfd->size);
        return -2;
    }
    for (rdblock = 0; rdblock < lastblock; rdblock++) {
        hdf_read (hfd, bufrdb, rdblock * hfd->blocksize, hfd->blocksize);
        if (rdb_checksum ("RDSK", bufrdb, rdblock))
            break;
        hdf_read (hfd, bufrdb, rdblock * hfd->blocksize, hfd->blocksize);
        if (!memcmp ("RDSK", bufrdb, 4)) {
            bufrdb[0xdc] = 0;
            bufrdb[0xdd] = 0;
            bufrdb[0xde] = 0;
            bufrdb[0xdf] = 0;
            if (rdb_checksum ("RDSK", bufrdb, rdblock)) {
                write_log ("Windows trashed RDB detected, fixing..\n");
                hdf_write (hfd, bufrdb, rdblock * hfd->blocksize, hfd->blocksize);
                break;
            }
        }
    }
    if (rdblock == lastblock) {
        rdbmnt
        write_log ("failed, no RDB detected\n");
        return -2;
    }
    blocksize = rl (bufrdb + 16);
    readblocksize = blocksize > hfd->blocksize ? blocksize : hfd->blocksize;
    badblock = rl (bufrdb + 24);
    if (badblock != -1) {
        rdbmnt
        write_log ("RDB: badblock list is not yet supported. Contact the author.\n");
        return -2;
    }
    driveinitblock = rl (bufrdb + 36);
    if (driveinitblock != -1) {
        rdbmnt
        write_log ("RDB: driveinit is not yet supported. Contact the author.\n");
        return -2;
    }
    hfd->cylinders = rl (bufrdb + 64);
    hfd->sectors = rl (bufrdb + 68);
    hfd->heads = rl (bufrdb + 72);
    fileblock = rl (bufrdb + 32);
    partblock = rl (bufrdb + 28);
    buf = xmalloc (readblocksize);
    for (i = 0; i <= partnum; i++) {
        if (!legalrdbblock (uip, partblock)) {
            err = -2;
            goto error;
        }
        hdf_read (hfd, buf, partblock * hfd->blocksize, readblocksize);
        if (!rdb_checksum ("PART", buf, partblock)) {
            err = -2;
            goto error;
        }
        partblock = rl (buf + 4 * 4);
    }

    rdbmnt
    flags = rl (buf + 20);
    if (flags & 2) { /* do not mount */
        err = -1;
        write_log ("Automount disabled, not mounting\n");
        goto error;
    }

    if (!(flags & 1)) /* not bootable */
        m68k_dreg (&regs, 7) = 0;

    buf[37 + buf[36]] = 0; /* zero terminate BSTR */
    uip->rdb_devname_amiga[partnum] = ds (device_dupfix (get_long (parmpacket + PP_EXPLIB), (char *)(buf + 37)));
    put_long (parmpacket, uip->rdb_devname_amiga[partnum]); /* name */
    put_long (parmpacket + 4, ROM_hardfile_resname);
    put_long (parmpacket + 8, uip->devno);
    put_long (parmpacket + 12, 0); /* Device flags */
    for (i = 0; i < PP_MAXSIZE; i++)
        put_byte (parmpacket + 16 + i, buf[128 + i]);
    dump_partinfo ((char *)(buf + 37), uip->devno, parmpacket);
    dostype = get_long (parmpacket + 80);

    if (dostype == 0) {
        write_log ("failed, dostype==0\n");
        err = -1;
        goto error;
    }

    if (hfd->cylinders * hfd->sectors * hfd->heads * blocksize > hfd->size)
        write_log ("WARNING: end of partition > size of disk!\n");

    err = 2;

    /* load custom filesystems if needed */
    if (!legalrdbblock (uip, fileblock))
        goto error;
    fsres = get_long (parmpacket + PP_FSRES);
    if (!fsres) {
        write_log ("FileSystem.resource not found, this shouldn't happen!\n");
        goto error;
    }

    for (;;) {
        if (!legalrdbblock (uip, fileblock)) {
            err = -1;
            goto error;
        }
        hdf_read (hfd, buf, fileblock * hfd->blocksize, readblocksize);
        if (!rdb_checksum ("FSHD", buf, fileblock)) {
            err = -1;
            goto error;
        }
        fileblock = rl (buf + 16);
        if ((dostype >> 8) == (rl (buf + 32) >> 8))
            break;
    }

    fsnode = get_long (fsres + 18);
    while (get_long (fsnode)) {
        if (get_long (fsnode + 14) == dostype)
            break;
        fsnode = get_long (fsnode);
    }

    oldversion = oldrevision = -1;
    newversion = (buf[36] << 8) | buf[37];
    newrevision = (buf[38] << 8) | buf[39];
    write_log ("RDB: RDB filesystem %08.8X version %d.%d\n", dostype, newversion, newrevision);
    if (get_long (fsnode)) {
        oldversion = get_word (fsnode + 18);
        oldrevision = get_word (fsnode + 20);
        write_log ("RDB: %08.8X in FileSystem.resouce version %d.%d\n", dostype, oldversion, oldrevision);
    }
    if (newversion * 65536 + newrevision <= oldversion * 65536 + oldrevision && oldversion >= 0) {
        write_log ("RDB: fs in FileSystem.resource is newer or same, ignoring RDB filesystem\n");
        goto error;
    }

    for (i = 0; i < 140; i++)
        put_byte (parmpacket + PP_FSHDSTART + i, buf[32 + i]);
    put_long (parmpacket + PP_FSHDSTART, dostype);
    /* we found required FSHD block */
    fsmem = xmalloc (262144);
    lsegblock = rl (buf + 72);
    i = 0;
    for (;;) {
        if (!legalrdbblock (uip, lsegblock))
            goto error;
        hdf_read (hfd, buf, lsegblock * hfd->blocksize, readblocksize);
        if (!rdb_checksum ("LSEG", buf, lsegblock))
            goto error;
        lsegblock = rl (buf + 16);
        memcpy (fsmem + i * (blocksize - 20), buf + 20, blocksize - 20);
        i++;
        if (lsegblock == -1)
            break;
    }
    write_log ("RDB: Filesystem loaded, %d bytes\n", i * (blocksize - 20));
    put_long (parmpacket + PP_FSSIZE, i * (blocksize - 20)); /* RDB filesystem size hack */
    uip->rdb_filesysstore = fsmem;
    uip->rdb_filesyssize = i * (blocksize - 20);
    xfree (buf);
    return 2;
error:
    xfree (buf);
    xfree (fsmem);
    return err;
}

static void addfakefilesys (uaecptr parmpacket, uae_u32 dostype)
{
    int i;

    for (i = 0; i < 140; i++)
        put_byte (parmpacket + PP_FSHDSTART + i, 0);
    put_long (parmpacket + 80, dostype);
    put_long (parmpacket + PP_FSHDSTART, dostype);
    put_long (parmpacket + PP_FSHDSTART + 8, 0x100 | (dostype == 0x444f5300 ? 0x0 : 0x80));
    put_long (parmpacket + PP_FSHDSTART + 44, 0xffffffff);
}

/* o1i: merged from winuae */
static int dofakefilesys (UnitInfo *uip, uaecptr parmpacket) {

    int i, size;
    char tmp[1024];
    struct zfile *zf;
    uae_u32 dostype, fsres, fsnode;

    write_log("RDB dofakefilesys entered\n");

    hdf_read (&uip->hf, tmp, 0, 4);
    dostype = (tmp[0] << 24) | (tmp[1] << 16) |(tmp[2] << 8) | tmp[3];
    if (dostype == 0)
        return FILESYS_HARDFILE;
    fsres = get_long (parmpacket + PP_FSRES);
    fsnode = get_long (fsres + 18);
    while (get_long (fsnode)) {
        if (get_long (fsnode + 14) == dostype) {
            if (kickstart_version < 36) {
                addfakefilesys (parmpacket, dostype);
            } else if ((dostype & 0xffffff00) != 0x444f5300) {
                addfakefilesys (parmpacket, dostype);
            }
            return FILESYS_HARDFILE;
        }
        fsnode = get_long (fsnode);
    }

    tmp[0] = 0;
    if (uip->filesysdir && strlen(uip->filesysdir) > 0) {
        strcpy (tmp, uip->filesysdir);
    } else if ((dostype & 0xffffff00) == 0x444f5300) {
        strcpy (tmp, currprefs.romfile);
        i = strlen (tmp);
        while (i > 0 && tmp[i - 1] != '/' && tmp[i - 1] != '\\')
            i--;
        strcpy (tmp + i, "FastFileSystem");
    }
    if (tmp[0] == 0) {
        write_log ("RDB: no filesystem for dostype 0x%08X\n", dostype);
        if ((dostype & 0xffffff00) == 0x444f5300)
            return FILESYS_HARDFILE;
        return -1;
    }
    write_log ("RDB: fakefilesys, trying to load '%s', dostype 0x%08X\n", tmp, dostype);
    zf = zfile_fopen (tmp, "rb");
    if (!zf) {
        write_log ("RDB: filesys not found\n");
        if ((dostype & 0xffffff00) == 0x444f5300)
            return FILESYS_HARDFILE;
        return -1;
    }

    zfile_fseek (zf, 0, SEEK_END);
    size = zfile_ftell (zf);
    if (size > 0) {
        zfile_fseek (zf, 0, SEEK_SET);
        uip->rdb_filesysstore = (uae_u8*)xmalloc (size);
        zfile_fread (uip->rdb_filesysstore, size, 1, zf);
    }
    zfile_fclose (zf);
    uip->rdb_filesyssize = size;
    put_long (parmpacket + PP_FSSIZE, uip->rdb_filesyssize);
    addfakefilesys (parmpacket, dostype);
    write_log ("HDF: faked RDB filesystem %08X loaded\n", dostype);
    return FILESYS_HARDFILE;
}


static void get_new_device (int type, uaecptr parmpacket, char **devname, uaecptr *devname_amiga, int unit_no)
{
    char buffer[80];

    if (*devname == 0 || strlen(*devname) == 0) {
        sprintf (buffer, "DH%d", unit_no);
    } else {
        strcpy (buffer, *devname);
    }
    *devname_amiga = ds (device_dupfix (get_long (parmpacket + PP_EXPLIB), buffer));
    if (type == FILESYS_VIRTUAL)
        write_log ("FS: mounted virtual unit %s (%s)\n", buffer, current_mountinfo.ui[unit_no].rootdir);
    else
        write_log ("FS: mounted HDF unit %s (%04.4x-%08.8x, %s)\n", buffer,
            (uae_u32)(current_mountinfo.ui[unit_no].hf.size >> 32),
            (uae_u32)(current_mountinfo.ui[unit_no].hf.size),
            current_mountinfo.ui[unit_no].rootdir);
}

/* Fill in per-unit fields of a parampacket */
static uae_u32 REGPARAM2 filesys_dev_storeinfo (TrapContext *context)
{
    UnitInfo *uip = current_mountinfo.ui;
    uae_u32 no = m68k_dreg (&context->regs, 6);
    int unit_no = no & 65535;
    int sub_no = no >> 16;
    int type = is_hardfile (&current_mountinfo, unit_no);
    uaecptr parmpacket = m68k_areg (&context->regs, 0);

    if (type == FILESYS_HARDFILE_RDB || type == FILESYS_HARDDRIVE) {
        /* RDB hardfile */
        uip[unit_no].devno = unit_no;
        return rdb_mount (&uip[unit_no], unit_no, sub_no, parmpacket);
    }
    if (sub_no)
        return -2;

    write_log ("Mounting uaehf.device %d (%d):\n", unit_no, sub_no);

    get_new_device (type, parmpacket, &uip[unit_no].devname, &uip[unit_no].devname_amiga, unit_no);
    uip[unit_no].devno = unit_no;
    put_long (parmpacket, uip[unit_no].devname_amiga);
    put_long (parmpacket + 4, type != FILESYS_VIRTUAL ? ROM_hardfile_resname : fsdevname);
    put_long (parmpacket + 8, uip[unit_no].devno);
    put_long (parmpacket + 12, 0); /* Device flags */
    put_long (parmpacket + 16, 16); /* Env. size */
    put_long (parmpacket + 20, uip[unit_no].hf.blocksize >> 2); /* longwords per block */
    put_long (parmpacket + 24, 0); /* unused */
    put_long (parmpacket + 28, uip[unit_no].hf.surfaces); /* heads */
    put_long (parmpacket + 32, 1); /* sectors per block */
    put_long (parmpacket + 36, uip[unit_no].hf.secspertrack); /* sectors per track */
    put_long (parmpacket + 40, uip[unit_no].hf.reservedblocks); /* reserved blocks */
    put_long (parmpacket + 44, 0); /* unused */
    put_long (parmpacket + 48, 0); /* interleave */
    put_long (parmpacket + 52, 0); /* lowCyl */
    put_long (parmpacket + 56, uip[unit_no].hf.nrcyls - 1); /* hiCyl */
    put_long (parmpacket + 60, 50); /* Number of buffers */
    put_long (parmpacket + 64, 0); /* Buffer mem type */
    put_long (parmpacket + 68, 0x7FFFFFFF); /* largest transfer */
    put_long (parmpacket + 72, ~1); /* addMask (?) */
    put_long (parmpacket + 76, uip[unit_no].bootpri); /* bootPri */
    put_long (parmpacket + 80, 0x444f5300); /* DOS\0 */
    if (type == FILESYS_HARDFILE)
        dofakefilesys (&uip[unit_no], parmpacket);
    return type;
}

void filesys_install (void)
{
    uaecptr loop;

    TRACE (("Installing filesystem\n"));

    ROM_filesys_resname = ds("UAEunixfs.resource");
    ROM_filesys_resid = ds("UAE unixfs 0.4");

    fsdevname = ds ("uae.device"); /* does not really exist */

    ROM_filesys_diagentry = here();
    calltrap (deftrap2(filesys_diagentry, 0, "filesys_diagentry"));
    dw(0x4ED0); /* JMP (a0) - jump to code that inits Residents */

    loop = here ();

    org (RTAREA_BASE + 0xFF18);
    calltrap (deftrap2 (filesys_dev_bootfilesys, 0, "filesys_dev_bootfilesys"));
    dw (RTS);

    /* Special trap for the assembly make_dev routine */
    org (RTAREA_BASE + 0xFF20);
    calltrap (deftrap2 (filesys_dev_remember, 0, "filesys_dev_remember"));
    dw (RTS);

    org (RTAREA_BASE + 0xFF28);
    calltrap (deftrap2 (filesys_dev_storeinfo, 0, "filesys_dev_storeinfo"));
    dw (RTS);

    org (RTAREA_BASE + 0xFF30);
    calltrap (deftrap2 (filesys_handler, 0, "filesys_handler"));
    dw (RTS);

    org (RTAREA_BASE + 0xFF40);
    calltrap (deftrap2 (startup_handler, 0, "startup_handler"));
    dw (RTS);

    org (RTAREA_BASE + 0xFF50);
    calltrap (deftrap2 (exter_int_helper, 0, "exter_int_helper"));
    dw (RTS);

    org (loop);
}

void filesys_install_code (void)
{
    align(4);

    /* The last offset comes from the code itself, look for it near the top. */
    EXPANSION_bootcode = here () + 8 + 0x1c + 4;
    /* Ouch. Make sure this is _always_ a multiple of two bytes. */
    filesys_initcode = here () + 8 + 0x30 + 4;

    #include "filesys_bootrom.c"
}

#if defined(WIN32) && !defined(__MINGW32__)
# include "od-win32/win32_filesys.c"
#endif

