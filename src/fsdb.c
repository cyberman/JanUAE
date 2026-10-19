 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Library of functions to make emulated filesystem as independent as
  * possible of the host filesystem's capabilities.
  *
  * Copyright 1999 Bernd Schmidt
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "filesys.h"
#include "autoconf.h"
#include "fsusage.h"
#include "scsidev.h"
#include "fsdb.h"

#ifdef __AROS__
#include <dos/dos.h>
#include <proto/dos.h>
#endif
/* The on-disk format is as follows:
 * Offset 0, 1 byte, valid
 * Offset 1, 4 bytes, mode
 * Offset 5, 257 bytes, aname
 * Offset 263, 257 bytes, nname
 * Offset 519, 81 bytes, comment
 */

#define TRACING_ENABLED 0
#if TRACING_ENABLED
#define TRACE(x)	do { kprintf("FSDB %s: ",__func__);kprintf x; } while(0)
#else
#define TRACE(x)
#endif


/****i* januae-fsdb/nname_begin **********************************************
*
*   NAME
*       nname_begin() -- Return the final component of a native filesystem path.
*
*   SYNOPSIS
*       char *nname_begin (char *nname);
*
*   FUNCTION
*       Find the last FSDB_DIR_SEPARATOR and return the following character,
*       or the beginning of the string when no separator is present.
*
*   INPUTS
*       nname - Writable, zero-terminated native path.
*
*   RESULT
*       Pointer into nname; no new storage is allocated.
*
****************************************************************************
*
*/

char *nname_begin (char *nname)
{
    char *p = strrchr (nname, FSDB_DIR_SEPARATOR);
    if (p)
	return p + 1;
    return nname;
}

/****i* januae-fsdb/fsdb_search_dir ******************************************
*
*   NAME
*       fsdb_search_dir() -- Find a directory entry matching an Amiga filename.
*
*   SYNOPSIS
*       char *fsdb_search_dir (const char *dirname, char *rel);
*
*   FUNCTION
*       Return the first directory entry encountered whose name matches rel
*       exactly or without regard to case.
*
*   INPUTS
*       dirname - Native directory to search.
*       rel - Requested entry name.
*
*   RESULT
*       rel for an exact match, a newly allocated name for a
*       case-insensitive match, or NULL if none is found or the directory
*       cannot be opened.
*
*   NOTES
*       The caller must free only a newly allocated result. The AROS and
*       non-AROS builds use different native directory APIs.
*
****************************************************************************
*
*/

#ifndef __AROS__  //ndef _WIN32
char *fsdb_search_dir (const char *dirname, char *rel)
{
    char *p = 0;
    struct dirent *de;
    DIR *dir = opendir (dirname);

    /* This really shouldn't happen...  */
    if (! dir)
	return 0;

    while (p == 0 && (de = readdir (dir)) != 0) {
	if (strcmp (de->d_name, rel) == 0)
	    p = rel;
	else if (strcasecmp (de->d_name, rel) == 0)
	    p = my_strdup (de->d_name);
    }
    closedir (dir);
    return p;
}
#else

/* readdir has issues on my machine..? o1i */

char *fsdb_search_dir (const char *dirname, char *rel) {
  char   *p    = NULL;
  BPTR    lock = NULL;
  BOOL    done = FALSE;
  uae_u32 err;
  struct FileInfoBlock fib;

  TRACE(("(%s, %s)\n", dirname, rel));

  lock=Lock(dirname, ACCESS_READ);
  if(!lock) {
    TRACE(("Lock on %s failed\n",dirname));
    return NULL;
  }

  if(!Examine(lock, &fib)) {
    TRACE(("Examine failed!?\n"));
    UnLock(lock);
    return NULL;
  }

  while( !p && ExNext(lock, &fib)) {
    if (strcmp (fib.fib_FileName, rel) == 0)
      p = rel;
    else if (strcasecmp (fib.fib_FileName, rel) == 0)
      p = my_strdup (fib.fib_FileName);
  }

  UnLock(lock);

  TRACE(("return %s\n", p));
  return p;
}
#endif

/****i* januae-fsdb/get_fsdb *************************************************
*
*   NAME
*       get_fsdb() -- Open a directory metadata file.
*
*   SYNOPSIS
*       static FILE *get_fsdb (a_inode *dir, const char *mode);
*
*   FUNCTION
*       Build the native path to FSDB_FILE beneath dir and open it in the
*       requested stdio mode.
*
*   INPUTS
*       dir - Directory whose metadata is requested.
*       mode - Mode string passed to fopen().
*
*   RESULT
*       Open FILE pointer, or NULL on failure. The caller owns the stream.
*
****************************************************************************
*
*/

static FILE *get_fsdb (a_inode *dir, const char *mode)
{
    char *n;
    FILE *f;

    n = build_nname (dir->nname, FSDB_FILE);
    f = fopen (n, mode);
    free (n);
    return f;
}

/****i* januae-fsdb/kill_fsdb ************************************************
*
*   NAME
*       kill_fsdb() -- Remove a directory metadata file.
*
*   SYNOPSIS
*       static void kill_fsdb (a_inode *dir);
*
*   FUNCTION
*       Build the path to FSDB_FILE in dir, attempt to unlink it, and
*       release the temporary pathname.
*
*   INPUTS
*       dir - Directory whose metadata file is removed.
*
*   RESULT
*       None.
*
*   NOTES
*       The unlink result is not reported.
*
****************************************************************************
*
*/

static void kill_fsdb (a_inode *dir)
{
    char *n = build_nname (dir->nname, FSDB_FILE);
    unlink (n);
    free (n);
}

/****i* januae-fsdb/fsdb_fixup ***********************************************
*
*   NAME
*       fsdb_fixup() -- Invalidate a metadata entry whose native file is missing.
*
*   SYNOPSIS
*       static void fsdb_fixup (FILE *f, char *buf, int size, a_inode *base);
*
*   FUNCTION
*       For an active record, resolve its stored native basename below base
*       and clear its valid byte when the corresponding host pathname cannot
*       be found.
*
*   INPUTS
*       f - Unused stream argument.
*       buf - Writable fixed-size metadata record.
*       size - Unused record-size argument.
*       base - Directory used to resolve the stored filename.
*
*   RESULT
*       None.
*
*   NOTES
*       Only the in-memory valid byte is changed; the caller controls any
*       later file rewrite.
*
****************************************************************************
*
*/

static void fsdb_fixup (FILE *f, char *buf, int size, a_inode *base)
{
    char *nname;
    int ret;

    if (buf[0] == 0)
	return;
    nname = build_nname (base->nname, buf + 5 + 257);
    ret = fsdb_exists (nname);
    if (ret) {
	free (nname);
	return;
    }
    TRACE (("uaefsdb '%s' deleted\n", nname));
    /* someone deleted this file/dir outside of emulation.. */
    buf[0] = 0;
    free (nname);
}

/****i* januae-fsdb/fsdb_clean_dir *******************************************
*
*   NAME
*       fsdb_clean_dir() -- Discard metadata entries for missing native files.
*
*   SYNOPSIS
*       void fsdb_clean_dir (a_inode *dir);
*
*   FUNCTION
*       Read fixed-size FSDB records, retain records whose native files
*       exist, compact them toward the beginning of the metadata file, and
*       truncate the file to the retained length.
*
*   INPUTS
*       dir - Directory whose FSDB_FILE is cleaned.
*
*   RESULT
*       None.
*
*   NOTES
*       If the metadata file cannot be opened for update, the operation
*       returns without changing it. I/O errors are not returned.
*
****************************************************************************
*
*/

void fsdb_clean_dir (a_inode *dir)
{
    char buf[1 + 4 + 257 + 257 + 81];
    char *n = build_nname (dir->nname, FSDB_FILE);
    FILE *f = fopen (n, "r+b");
    off_t pos1 = 0, pos2;

    if (f == 0) {
	free (n);
	return;
    }
    for (;;) {
	pos2 = ftell (f);
	if (fread (buf, 1, sizeof buf, f) < sizeof buf)
	    break;
	fsdb_fixup (f, buf, sizeof buf, dir);
	if (buf[0] == 0)
	    continue;
	if (pos1 != pos2) {
	    fseek (f, pos1, SEEK_SET);
	    fwrite (buf, 1, sizeof buf, f);
	    fseek (f, pos2 + sizeof buf, SEEK_SET);
	}
	pos1 += sizeof buf;
    }
    fclose (f);
    truncate (n, pos1);
    free (n);
}

/****i* januae-fsdb/aino_from_buf ********************************************
*
*   NAME
*       aino_from_buf() -- Reconstruct an inode from a metadata record.
*
*   SYNOPSIS
*       static a_inode *aino_from_buf (a_inode *base, char *buf, long off);
*
*   FUNCTION
*       Allocate an a_inode, decode its stored Amiga protection mode, names
*       and optional comment, populate native filesystem attributes, and
*       bind it to the record offset.
*
*   INPUTS
*       base - Parent directory for the native pathname.
*       buf - Metadata record containing the mode, names and comment.
*       off - Byte offset of the record in FSDB_FILE.
*
*   RESULT
*       New a_inode with separately allocated name and optional comment
*       strings.
*
*   NOTES
*       The object remains marked as database-backed even if the native
*       attribute lookup fails.
*
****************************************************************************
*
*/

static a_inode *aino_from_buf (a_inode *base, char *buf, long off)
{
    uae_u32 mode;
    a_inode *aino = (a_inode *) xcalloc (sizeof (a_inode), 1);

    mode = do_get_mem_long ((uae_u32 *)(buf + 1));
    buf += 5;
    aino->aname = my_strdup (buf);
    buf += 257;
    aino->nname = build_nname (base->nname, buf);
    buf += 257;
    aino->comment = *buf != '\0' ? my_strdup (buf) : 0;
    fsdb_fill_file_attrs (base, aino);
    aino->amigaos_mode = mode;
    aino->has_dbentry = 1;
    aino->dirty = 0;
    aino->db_offset = off;
    return aino;
}

/****i* januae-fsdb/fsdb_lookup_aino_aname ***********************************
*
*   NAME
*       fsdb_lookup_aino_aname() -- Look up a recorded Amiga filename.
*
*   SYNOPSIS
*       a_inode *fsdb_lookup_aino_aname (a_inode *base, const char *aname);
*
*   FUNCTION
*       Scan valid directory metadata records for a case-insensitive
*       Amiga-name match and reconstruct the corresponding inode.
*
*   INPUTS
*       base - Directory containing the metadata file.
*       aname - Amiga filename to find.
*
*   RESULT
*       Newly allocated a_inode on a match; NULL if none is found or the
*       metadata file cannot be opened.
*
*   NOTES
*       The returned inode and its allocated fields are owned by the caller.
*
****************************************************************************
*
*/

a_inode *fsdb_lookup_aino_aname (a_inode *base, const char *aname)
{
    FILE *f = get_fsdb (base, "r+b");

    if (f == 0)
	return 0;

    for (;;) {
	char buf[1 + 4 + 257 + 257 + 81];
	if (fread (buf, 1, sizeof buf, f) < sizeof buf)
	    break;
	if (buf[0] != 0 && same_aname (buf + 5, aname)) {
	    long pos = ftell (f) - sizeof buf;
	    fclose (f);
	    return aino_from_buf (base, buf, pos);
	}
    }
    fclose (f);
    return 0;
}

/****i* januae-fsdb/fsdb_lookup_aino_nname ***********************************
*
*   NAME
*       fsdb_lookup_aino_nname() -- Look up a recorded native basename.
*
*   SYNOPSIS
*       a_inode *fsdb_lookup_aino_nname (a_inode *base, const char *nname);
*
*   FUNCTION
*       Scan valid directory metadata records for an exact match against the
*       stored native basename.
*
*   INPUTS
*       base - Directory containing the metadata file.
*       nname - Native basename to match, not a full path.
*
*   RESULT
*       Newly allocated a_inode on a match; NULL if none is found or the
*       metadata file cannot be opened.
*
*   NOTES
*       The returned inode and its allocated fields are owned by the caller.
*
****************************************************************************
*
*/

a_inode *fsdb_lookup_aino_nname (a_inode *base, const char *nname)
{
    FILE *f = get_fsdb (base, "r+b");

    if (f == 0)
	return 0;

    for (;;) {
	char buf[1 + 4 + 257 + 257 + 81];
	if (fread (buf, 1, sizeof buf, f) < sizeof buf)
	    break;
	if (buf[0] != 0 && strcmp (buf + 5 + 257, nname) == 0) {
	    long pos = ftell (f) - sizeof buf;
	    fclose (f);
	    return aino_from_buf (base, buf, pos);
	}
    }
    fclose (f);
    return 0;
}

/****i* januae-fsdb/fsdb_used_as_nname ***************************************
*
*   NAME
*       fsdb_used_as_nname() -- Check whether a native basename has a metadata entry.
*
*   SYNOPSIS
*       int fsdb_used_as_nname (a_inode *base, const char *nname);
*
*   FUNCTION
*       Scan active records in the directory metadata file for an exact
*       match of the native basename.
*
*   INPUTS
*       base - Directory containing the metadata file.
*       nname - Native basename to search for.
*
*   RESULT
*       One if an active record matches; zero otherwise, including when the
*       file cannot be opened.
*
****************************************************************************
*
*/

int fsdb_used_as_nname (a_inode *base, const char *nname)
{
    FILE *f = get_fsdb (base, "r+b");
    char buf[1 + 4 + 257 + 257 + 81];

    if (f == 0)
	return 0;
    for (;;) {
	if (fread (buf, 1, sizeof buf, f) < sizeof buf)
	    break;
	if (buf[0] == 0)
	    continue;
	if (strcmp (buf + 5 + 257, nname) == 0) {
	    fclose (f);
	    return 1;
	}
    }
    fclose (f);
    return 0;
}

/****i* januae-fsdb/needs_dbentry ********************************************
*
*   NAME
*       needs_dbentry() -- Determine whether an inode requires stored metadata.
*
*   SYNOPSIS
*       static int needs_dbentry (a_inode *aino);
*
*   FUNCTION
*       Request a record for a nondeleted inode when its protection mode is
*       not natively representable, it has a comment, or its native basename
*       differs from its Amiga name.
*
*   INPUTS
*       aino - Inode to examine.
*
*   RESULT
*       Nonzero if a metadata record is required; zero otherwise.
*
****************************************************************************
*
*/

static int needs_dbentry (a_inode *aino)
{
    const char *nn_begin;

    if (aino->deleted)
	return 0;

    if (! fsdb_mode_representable_p (aino) || aino->comment != 0)
	return 1;

    nn_begin = nname_begin (aino->nname);
    return strcmp (nn_begin, aino->aname) != 0;
}

/****i* januae-fsdb/write_aino ***********************************************
*
*   NAME
*       write_aino() -- Serialize an inode as a metadata record.
*
*   SYNOPSIS
*       static void write_aino (FILE *f, a_inode *aino);
*
*   FUNCTION
*       Write the active flag, encoded protection mode, bounded Amiga and
*       native names, and optional comment to the stream at its current
*       position.
*
*   INPUTS
*       f - Writable metadata stream positioned at the target record.
*       aino - Inode whose metadata is serialized.
*
*   RESULT
*       None.
*
*   NOTES
*       Sets db_offset and has_dbentry without checking the fwrite() result.
*
****************************************************************************
*
*/

static void write_aino (FILE *f, a_inode *aino)
{
    char buf[1 + 4 + 257 + 257 + 81];
    buf[0] = aino->needs_dbentry;
    do_put_mem_long ((uae_u32 *)(buf + 1), aino->amigaos_mode);
    strncpy (buf + 5, aino->aname, 256);
    buf[5 + 256] = '\0';
    strncpy (buf + 5 + 257, nname_begin (aino->nname), 256);
    buf[5 + 257 + 256] = '\0';
    strncpy (buf + 5 + 2*257, aino->comment ? aino->comment : "", 80);
    buf[5 + 2 * 257 + 80] = '\0';
    aino->db_offset = ftell (f);
    fwrite (buf, 1, sizeof buf, f);
    aino->has_dbentry = aino->needs_dbentry;
    TRACE (("%d '%s' '%s' written\n", aino->db_offset, aino->aname, aino->nname));
}

/****i* januae-fsdb/fsdb_dir_writeback ***************************************
*
*   NAME
*       fsdb_dir_writeback() -- Write pending directory metadata changes.
*
*   SYNOPSIS
*       void fsdb_dir_writeback (a_inode *dir);
*
*   FUNCTION
*       Determine which children need FSDB records, remove the metadata file
*       when no records are needed, and update or append records for dirty
*       children when a write is required.
*
*   INPUTS
*       dir - Directory whose child metadata is to be synchronized.
*
*   RESULT
*       None.
*
*   NOTES
*       A disabled FSDB preference suppresses creation when no metadata file
*       exists. Stream and allocation failures are not consistently reported
*       to callers.
*
****************************************************************************
*
*/

void fsdb_dir_writeback (a_inode *dir)
{
    FILE *f;
    int changes_needed = 0;
    int entries_needed = 0;
    a_inode *aino;
    char *tmpbuf;
    int size, i;

    TRACE (("fsdb writeback %s\n", dir->aname));
    /* First pass: clear dirty bits where unnecessary, and see if any work
     * needs to be done.  */
    for (aino = dir->child; aino; aino = aino->sibling) {
/*
	int old_needs_dbentry = aino->needs_dbentry || aino->has_dbentry;
	aino->needs_dbentry = needs_dbentry (aino);
	entries_needed |= aino->has_dbentry | aino->needs_dbentry;
*/
	int old_needs_dbentry = aino->has_dbentry;
	int need = needs_dbentry (aino);
	aino->needs_dbentry = need;
	entries_needed |= need;
	if (! aino->dirty)
	    continue;
	if (! aino->needs_dbentry && ! old_needs_dbentry)
	    aino->dirty = 0;
	else
	    changes_needed = 1;
    }
    if (! entries_needed) {
	kill_fsdb (dir);
	TRACE (("fsdb removed\n"));
	return;
    }

    if (! changes_needed) {
	TRACE (("not modified\n"));
	return;
    }

    f = get_fsdb (dir, "r+b");
    if (f == 0) {
	if (/*(currprefs.filesys_custom_uaefsdb  && (dir->volflags & MYVOLUMEINFO_STREAMS)) || */ currprefs.filesys_no_uaefsdb) {
	    for (aino = dir->child; aino; aino = aino->sibling) {
		aino->dirty = 0;
		aino->has_dbentry = 0;
		aino->needs_dbentry = 0;
	    }
	    return;
	}
	f = get_fsdb (dir, "w+b");
	if (f == 0) {
	    TRACE (("failed\n"));
	    /* This shouldn't happen... */
	    return;
	}
    }
    fseek (f, 0, SEEK_END);
    size = ftell (f);
    fseek (f, 0, SEEK_SET);
    tmpbuf = 0;
    if (size > 0) {
	tmpbuf = malloc (size);
	fread (tmpbuf, 1, size, f);
    }
    TRACE (("**** updating '%s' %d\n", dir->aname, size));

    for (aino = dir->child; aino; aino = aino->sibling) {
	if (! aino->dirty)
	    continue;
	aino->dirty = 0;

	i = 0;
	while (!aino->has_dbentry && i < size) {
	    if (!strcmp (tmpbuf + i + 5, aino->aname)) {
		aino->has_dbentry = 1;
		aino->db_offset = i;
	    }
	    i += 1 + 4 + 257 + 257 + 81;
	}

	if (! aino->has_dbentry) {
	    fseek (f, 0, SEEK_END);
	    aino->has_dbentry = 1;
	} else {
	    fseek (f, aino->db_offset, SEEK_SET);
	}
	write_aino (f, aino);
    }
    TRACE (("end\n"));
    fclose (f);
    free (tmpbuf);
}
