 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Library of functions to make emulated filesystem as independent as
  * possible of the host filesystem's capabilities.
  * This is the Win32 version.
  *
  * Copyright 1997 Mathias Ortmann
  * Copyright 1999 Bernd Schmidt
  */

#ifdef FILESYS

#include "sysconfig.h"
#include "sysdeps.h"

#include "fsdb.h"
#include <windows.h>

/* these are deadly (but I think allowed on the Amiga): */
#define NUM_EVILCHARS 7
static const char evilchars[NUM_EVILCHARS] = { '\\', '*', '?', '\"', '<', '>', '|' };

/****i* januae-win32/fsdb_name_invalid *************************************
*
*   NAME
*       fsdb_name_invalid()
*
*   SYNOPSIS
*       int fsdb_name_invalid (const char *n);
*
*   FUNCTION
*       Check whether an Amiga filename is forbidden by the Win32 host
*       filesystem.
*
*   INPUTS
*       n - Filename to validate.
*
*   RESULT
*       Nonzero for rejected names; zero otherwise.
*
*   NOTES
*       Rejects reserved device names, trailing spaces or periods, seven
*       forbidden characters and FSDB_FILE. Requires a nonempty string; the
*       implementation examines n[strlen(n)-1].
*
****************************************************************************
*
*/
int fsdb_name_invalid (const char *n)
{
    int i;
    char a = n[0];
    char b = (a == '\0' ? a : n[1]);
    char c = (b == '\0' ? b : n[2]);
    char d = (c == '\0' ? c : n[3]);
    int l = strlen (n), ll;

    if (a >= 'a' && a <= 'z')
	a -= 32;
    if (b >= 'a' && b <= 'z')
	b -= 32;
    if (c >= 'a' && c <= 'z')
	c -= 32;

    /* reserved dos devices */
    ll = 0;
    if (a == 'A' && b == 'U' && c == 'X') ll = 3; /* AUX  */
    if (a == 'C' && b == 'O' && c == 'N') ll = 3; /* CON  */
    if (a == 'P' && b == 'R' && c == 'N') ll = 3; /* PRN  */
    if (a == 'N' && b == 'U' && c == 'L') ll = 3; /* NUL  */
    if (a == 'L' && b == 'P' && c == 'T'  && (d >= '0' && d <= '9')) ll = 4;  /* LPT# */
    if (a == 'C' && b == 'O' && c == 'M'  && (d >= '0' && d <= '9')) ll = 4; /* COM# */
    /* AUX.anything, CON.anything etc.. are also illegal names */
    if (ll && (l == ll || (l > ll && n[ll] == '.')))
	return 1;

    /* spaces and periods at the end are a no-no */
    i = l - 1;
    if (n[i] == '.' || n[i] == ' ')
	return 1;

    /* these characters are *never* allowed */
    for (i = 0; i < NUM_EVILCHARS; i++) {
	if (strchr (n, evilchars[i]) != 0)
	    return 1;
    }

    /* the reserved fsdb filename */
    if (strcmp (n, FSDB_FILE) == 0)
	return 1;
    return 0; /* the filename passed all checks, now it should be ok */
}

/****i* januae-win32/filesys_parse_mask ************************************
*
*   NAME
*       filesys_parse_mask()
*
*   SYNOPSIS
*       static uae_u32 filesys_parse_mask (uae_u32 mask);
*
*   FUNCTION
*       Convert between stored Amiga and host-facing protection-mask bits.
*
*   INPUTS
*       mask - Protection bit mask.
*
*   RESULT
*       mask with the low four bits inverted.
*
*   NOTES
*       XOR with 0xF is its own inverse; no other bits are modified.
*
****************************************************************************
*
*/
static uae_u32 filesys_parse_mask (uae_u32 mask)
{
    return mask ^ 0xf;
}

/****i* januae-win32/fsdb_exists *******************************************
*
*   NAME
*       fsdb_exists()
*
*   SYNOPSIS
*       int fsdb_exists (char *nname);
*
*   FUNCTION
*       Test whether Windows can obtain file attributes for a path.
*
*   INPUTS
*       nname - Native filesystem pathname.
*
*   RESULT
*       One if GetFileAttributes succeeds; zero otherwise.
*
*   NOTES
*       Does not distinguish a missing path from other attribute-query
*       failures.
*
****************************************************************************
*
*/
int fsdb_exists (char *nname)
{
    if (GetFileAttributes(nname) == 0xFFFFFFFF)
	return 0;
    return 1;
}

/****i* januae-win32/fsdb_fill_file_attrs **********************************
*
*   NAME
*       fsdb_fill_file_attrs()
*
*   SYNOPSIS
*       int fsdb_fill_file_attrs (a_inode *base, a_inode *aino);
*
*   FUNCTION
*       Populate an inode with Win32 file attribute information.
*
*   INPUTS
*       base - Unused parent inode argument.
*       aino - Inode with a valid native pathname to update.
*
*   RESULT
*       One when attributes are obtained; zero on failure.
*
*   NOTES
*       Sets directory status and maps archive, read-only, write and delete
*       access into Amiga protection bits. Logs attribute-query failures.
*
****************************************************************************
*
*/
int fsdb_fill_file_attrs (a_inode *base, a_inode *aino)
{
    DWORD mode;

    if ((mode = GetFileAttributes (aino->nname)) == 0xFFFFFFFF) {
	write_log ("GetFileAttributes('%s') failed! error=%d, aino=%p dir=%d\n", aino->nname,GetLastError(),aino,aino->dir);
	return 0;
    }

    aino->dir = (mode & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    aino->amigaos_mode = A_FIBF_EXECUTE | A_FIBF_READ;
    if (FILE_ATTRIBUTE_ARCHIVE & mode)
	aino->amigaos_mode |= A_FIBF_ARCHIVE;
    if (! (FILE_ATTRIBUTE_READONLY & mode))
	aino->amigaos_mode |= A_FIBF_WRITE | A_FIBF_DELETE;
    aino->amigaos_mode = filesys_parse_mask (aino->amigaos_mode);
    return 1;
}

/****i* januae-win32/fsdb_set_file_attrs ***********************************
*
*   NAME
*       fsdb_set_file_attrs()
*
*   SYNOPSIS
*       int fsdb_set_file_attrs (a_inode *aino);
*
*   FUNCTION
*       Translate stored Amiga protection bits to Win32 attributes.
*
*   INPUTS
*       aino - Inode with the target pathname and Amiga protection mask.
*
*   RESULT
*       ERROR_OBJECT_NOT_AROUND when stat fails; otherwise zero.
*
*   NOTES
*       Sets attributes only for nondirectories and marks the inode dirty.
*       Ignores the result of SetFileAttributes(), so zero does not
*       guarantee the attributes changed.
*
****************************************************************************
*
*/
int fsdb_set_file_attrs (a_inode *aino)
{
    struct stat statbuf;
    uae_u32 mode=0, tmpmask;

    tmpmask = filesys_parse_mask (aino->amigaos_mode);

    if (stat (aino->nname, &statbuf) == -1)
	return ERROR_OBJECT_NOT_AROUND;

    /* Unix dirs behave differently than AmigaOS ones.  */
    /* windows dirs go where no dir has gone before...  */
    if (! aino->dir) {
	if ((tmpmask & (A_FIBF_READ | A_FIBF_DELETE)) == 0)
	    mode |= FILE_ATTRIBUTE_READONLY;
	if (tmpmask & A_FIBF_ARCHIVE)
	    mode |= FILE_ATTRIBUTE_ARCHIVE;
	else
	    mode &= ~FILE_ATTRIBUTE_ARCHIVE;

	SetFileAttributes(aino->nname, mode);
    }

    aino->dirty = 1;
    return 0;
}

/****i* januae-win32/fsdb_mode_representable_p *****************************
*
*   NAME
*       fsdb_mode_representable_p()
*
*   SYNOPSIS
*       int fsdb_mode_representable_p (const a_inode *aino);
*
*   FUNCTION
*       Check whether the inode protection mask is representable with host
*       attributes.
*
*   INPUTS
*       aino - Inode whose Amiga protection mode is checked.
*
*   RESULT
*       One for accepted modes; zero otherwise.
*
*   NOTES
*       Directories are accepted only with a zero mask. For files, the
*       implementation rejects script/pure/execute/read bits and requires
*       write/delete bits to be both present or both absent.
*
****************************************************************************
*
*/
int fsdb_mode_representable_p (const a_inode *aino)
{
    int mask = aino->amigaos_mode;
    int m1;

    if (aino->dir)
	return aino->amigaos_mode == 0;

    /* P or S set, or E or R clear, means we can't handle it.  */
    if (mask & (A_FIBF_SCRIPT | A_FIBF_PURE | A_FIBF_EXECUTE | A_FIBF_READ))
	return 0;
    m1 = A_FIBF_DELETE | A_FIBF_WRITE;
    /* If it's rwed, we are OK... */
    if ((mask & m1) == 0)
	return 1;
    /* We can also represent r-e-, by setting the host's readonly flag.  */
    if ((mask & m1) == m1)
	return 1;
    return 0;
}

/****i* januae-win32/fsdb_create_unique_nname ******************************
*
*   NAME
*       fsdb_create_unique_nname()
*
*   SYNOPSIS
*       char *fsdb_create_unique_nname (a_inode *base, const char
*       *suggestion);
*
*   FUNCTION
*       Generate an available native pathname for an Amiga file.
*
*   INPUTS
*       base - Parent inode supplying the native directory.
*       suggestion - Requested filename component.
*
*   RESULT
*       Newly allocated native pathname.
*
*   NOTES
*       Prefixes __uae___, replaces forbidden characters, dots and spaces
*       with underscores, and retries random suffixes until access() reports
*       ENOENT. The caller owns the returned allocation; other access
*       failures do not terminate the retry loop.
*
****************************************************************************
*
*/
char *fsdb_create_unique_nname (a_inode *base, const char *suggestion)
{
    char *c;
    char tmp[256] = "__uae___";
    int i;

    strncat (tmp, suggestion, 240);

    /* replace the evil ones... */
    for (i=0; i < NUM_EVILCHARS; i++)
	while ((c = strchr (tmp, evilchars[i])) != 0)
	    *c = '_';

    while ((c = strchr (tmp, '.')) != 0)
	*c = '_';
    while ((c = strchr (tmp, ' ')) != 0)
	*c = '_';

    for (;;) {
	char *p = build_nname (base->nname, tmp);
	if (access (p, R_OK) < 0 && errno == ENOENT) {
	    write_log ("unique name: %s\n", p);
	    return p;
	}
	free (p);

	/* tmpnam isn't reentrant and I don't really want to hack configure
	 * right now to see whether tmpnam_r is available...  */
	for (i = 0; i < 8; i++) {
	    tmp[i+8] = "_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[rand () % 63];
	}
    }
}

/****i* januae-win32/dos_errno *********************************************
*
*   NAME
*       dos_errno()
*
*   SYNOPSIS
*       int dos_errno (void);
*
*   FUNCTION
*       Translate the last Win32 error to an AmigaDOS filesystem error.
*
*   RESULT
*       AmigaDOS error constant for the current GetLastError() value.
*
*   NOTES
*       Unmapped Windows codes are logged and converted to
*       ERROR_NOT_IMPLEMENTED.
*
****************************************************************************
*
*/
int dos_errno (void)
{
    DWORD e = GetLastError ();

    //write_log ("ec=%d\n", e);
    switch (e) {
     case ERROR_NOT_ENOUGH_MEMORY:
     case ERROR_OUTOFMEMORY:
	return ERROR_NO_FREE_STORE;

     case ERROR_FILE_EXISTS:
     case ERROR_ALREADY_EXISTS:
	return ERROR_OBJECT_EXISTS;

     case ERROR_WRITE_PROTECT:
     case ERROR_ACCESS_DENIED:
	return ERROR_WRITE_PROTECTED;

     case ERROR_FILE_NOT_FOUND:
     case ERROR_INVALID_DRIVE:
	return ERROR_OBJECT_NOT_AROUND;

     case ERROR_HANDLE_DISK_FULL:
	return ERROR_DISK_IS_FULL;

     case ERROR_SHARING_VIOLATION:
     case ERROR_BUSY:
	return ERROR_OBJECT_IN_USE;

     case ERROR_CURRENT_DIRECTORY:
	return ERROR_DIRECTORY_NOT_EMPTY;

     case ERROR_NEGATIVE_SEEK:
     case ERROR_SEEK_ON_DEVICE:
	return ERROR_SEEK_ERROR;

     default:
	write_log ("Unimplemented error %d\n", e);
	return ERROR_NOT_IMPLEMENTED;
    }
}
#endif
