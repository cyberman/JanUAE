 /*
  * UAE - The Un*x Amiga Emulator
  *
  * routines to handle compressed file automatically
  *
  * (c) 1996 Samuel Devulder, Tim Gunn
  *     2002-2004 Toni Wilen
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "zfile.h"
#include "unzip.h"
#include "disk.h"
#include "dms/cdata.h"
#include "dms/pfile.h"
#include "gui.h"
#include "crc32.h"

#include <zlib.h>

struct zfile {
    char *name;
    char *zipname;
    FILE *f;
    uae_u8 *data;
    int size;
    int seek;
    int deleteafterclose;
    struct zfile *next;
};

static struct zfile *zlist = 0;
int is_zlib;

/****i* januae-zfile/zlib_test *********************************************
*
*   NAME
*       zlib_test() -- Check availability of the compression runtime.
*
*   SYNOPSIS
*       static int zlib_test (void);
*
*   FUNCTION
*       Return whether compression support is available. The Windows build
*       without MinGW uses the runtime availability flag and reports a
*       missing zlib DLL once.
*
*   INPUTS
*       None.
*
*   RESULT
*       One when the compression runtime is considered available; zero
*       otherwise.
*
****************************************************************************
*
*/

static int zlib_test (void)
{
#if defined WIN32 && !defined __MINGW32__
    static int zlibmsg;
    if (is_zlib)
	return 1;
    if (zlibmsg)
	return 0;
    zlibmsg = 1;
    gui_message("zip and gzip support disabled because zlib1.dll is missing");
    return 0;
#else
    /* On non-Windows platforms, we can safely assume (I think) that if we got this
     * far zlib is present - Rich */
    return 1;
#endif
}

/****i* januae-zfile/zfile_create ******************************************
*
*   NAME
*       zfile_create() -- Create a tracked zfile handle.
*
*   SYNOPSIS
*       static struct zfile *zfile_create (void);
*
*   FUNCTION
*       Allocate a zero-initialized zfile and link it at the head of the
*       active zfile list.
*
*   INPUTS
*       None.
*
*   RESULT
*       A new zfile handle, or NULL when allocation fails.
*
****************************************************************************
*
*/

static struct zfile *zfile_create (void)
{
    struct zfile *z;

    z = malloc (sizeof *z);
    if (!z)
	return 0;
    memset (z, 0, sizeof *z);
    z->next = zlist;
    zlist = z;
    return z;
}

/****i* januae-zfile/zfile_free ********************************************
*
*   NAME
*       zfile_free() -- Release the resources owned by a zfile.
*
*   SYNOPSIS
*       static void zfile_free (struct zfile *f);
*
*   FUNCTION
*       Close an open standard file, remove an owned temporary file when
*       requested, and release the stored filename, memory buffer and
*       handle.
*
*   INPUTS
*       f - zfile handle whose resources are to be released.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void zfile_free (struct zfile *f)
{
    if (f->f)
	fclose (f->f);
    if (f->deleteafterclose) {
	unlink (f->name);
	write_log ("deleted temporary file '%s'\n", f->name);
    }
    free (f->name);
    free (f->data);
    free (f);
}

/****i* januae-zfile/zfile_exit ********************************************
*
*   NAME
*       zfile_exit() -- Close every tracked zfile.
*
*   SYNOPSIS
*       void zfile_exit (void);
*
*   FUNCTION
*       Walk the active zfile list and release each entry, leaving the list
*       empty.
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

void zfile_exit (void)
{
    struct zfile *l;
    while ((l = zlist)) {
	zlist = l->next;
	zfile_free (l);
    }
}

/****i* januae-zfile/zfile_fclose ******************************************
*
*   NAME
*       zfile_fclose() -- Close and unlink a tracked zfile.
*
*   SYNOPSIS
*       void zfile_fclose (struct zfile *f);
*
*   FUNCTION
*       Find the supplied handle in the active list, release its owned
*       resources and remove it from the list.
*
*   INPUTS
*       f - Tracked handle to close; NULL is ignored.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       An unknown non-NULL handle is reported and left unchanged.
*
****************************************************************************
*
*/

void zfile_fclose (struct zfile *f)
{
    struct zfile *pl = NULL;
    struct zfile *l  = zlist;
    struct zfile *nxt = NULL;

    if (!f)
	return;
    while (l != f) {
	if (l == 0) {
	    write_log ("zfile: tried to free already freed filehandle!\n");
	    return;
	}
	pl = l;
	l = l->next;
    }
    if (l) nxt = l->next;
    zfile_free (f);
    if (l == 0)
	return;
    if(!pl)
	zlist = nxt;
    else
	pl->next = nxt;
}

static uae_u8 exeheader[]={0x00,0x00,0x03,0xf3,0x00,0x00,0x00,0x00};
/****i* januae-zfile/zfile_gettype *****************************************
*
*   NAME
*       zfile_gettype() -- Identify the stored file category.
*
*   SYNOPSIS
*       int zfile_gettype (struct zfile *z);
*
*   FUNCTION
*       Classify recognized filename extensions as disk images, ROMs, saved
*       states, keys, nonvolatile data or configurations; otherwise inspect
*       the initial executable header for a disk-image signature.
*
*   INPUTS
*       z - Open zfile to classify.
*
*   RESULT
*       A ZFILE_* type constant, or ZFILE_UNKNOWN for an unrecognized or
*       NULL handle.
*
****************************************************************************
*
*/

int zfile_gettype (struct zfile *z)
{
    uae_u8 buf[8];
    char *ext;

    if (!z)
	return ZFILE_UNKNOWN;
    ext = strrchr (z->name, '.');
    if (ext != NULL) {
	ext++;
	if (strcasecmp (ext, "adf") == 0)
	    return ZFILE_DISKIMAGE;
	if (strcasecmp (ext, "adz") == 0)
	    return ZFILE_DISKIMAGE;
	if (strcasecmp (ext, "roz") == 0)
	    return ZFILE_ROM;
	if (strcasecmp (ext, "ipf") == 0)
	    return ZFILE_DISKIMAGE;
	if (strcasecmp (ext, "fdi") == 0)
	    return ZFILE_DISKIMAGE;
	if (strcasecmp (ext, "uss") == 0)
	    return ZFILE_STATEFILE;
	if (strcasecmp (ext, "dms") == 0)
	    return ZFILE_DISKIMAGE;
	if (strcasecmp (ext, "rom") == 0)
	    return ZFILE_ROM;
	if (strcasecmp (ext, "key") == 0)
	    return ZFILE_KEY;
	if (strcasecmp (ext, "nvr") == 0)
	    return ZFILE_NVR;
	if (strcasecmp (ext, "uae") == 0)
	    return ZFILE_CONFIGURATION;
    }
    memset (buf, 0, sizeof (buf));
    zfile_fread (buf, 8, 1, z);
    zfile_fseek (z, -8, SEEK_CUR);
    if (!memcmp (buf, exeheader, sizeof(buf)))
	return ZFILE_DISKIMAGE;
    return ZFILE_UNKNOWN;
}


static struct zfile *zuncompress (struct zfile *z);

/****i* januae-zfile/gunzip ************************************************
*
*   NAME
*       gunzip() -- Attempt gzip stream expansion.
*
*   SYNOPSIS
*       static struct zfile *gunzip (struct zfile *z);
*
*   FUNCTION
*       Check compression support, interpret gzip header fields and inflate
*       a raw deflate payload into an in-memory zfile when the non-GTKMUI
*       implementation is enabled.
*
*   INPUTS
*       z - Source zfile containing the candidate gzip data.
*
*   RESULT
*       An expanded zfile after successful inflation; otherwise the original
*       handle.
*
*   NOTES
*       With GTKMUI defined, this routine returns the original handle
*       without expansion.
*
****************************************************************************
*
*/

static struct zfile *gunzip (struct zfile *z)
{
    uae_u8 header[2 + 1 + 1 + 4 + 1 + 1];
    z_stream zs;
    int i, size, ret, first;
    uae_u8 flags;
    long offset;
    char name[MAX_DPATH];
    uae_u8 buffer[8192];
    struct zfile *z2;
    uae_u8 b;

    if (!zlib_test ())
	return z;

#if defined GTKMUI
    return z;
#else
    strcpy (name, z->name);
    memset (&zs, 0, sizeof (zs));
    memset (header, 0, sizeof (header));
    zfile_fread (header, sizeof (header), 1, z);
    flags = header[3];
    if (header[0] != 0x1f && header[1] != 0x8b)
	return z;
    if (flags & 2) /* multipart not supported */
	return z;
    if (flags & 32) /* encryption not supported */
	return z;
    if (flags & 4) { /* skip extra field */
	zfile_fread (&b, 1, 1, z);
	size = b;
	zfile_fread (&b, 1, 1, z);
	size |= b << 8;
	zfile_fseek (z, size + 2, SEEK_CUR);
    }
    if (flags & 8) { /* get original file name */
	i = 0;
	do {
	    zfile_fread (name + i, 1, 1, z);
	} while (name[i++]);
    }
    if (flags & 16) { /* skip comment */
	i = 0;
	do {
	    zfile_fread (&b, 1, 1, z);
	} while (b);
    }
    offset = zfile_ftell (z);
    zfile_fseek (z, -4, SEEK_END);
    zfile_fread (&b, 1, 1, z);
    size = b;
    zfile_fread (&b, 1, 1, z);
    size |= b << 8;
    zfile_fread (&b, 1, 1, z);
    size |= b << 16;
    zfile_fread (&b, 1, 1, z);
    size |= b << 24;
    if (size < 8 || size > 10000000) /* safety check */
	return z;
    zfile_fseek (z, offset, SEEK_SET);
    z2 = zfile_fopen_empty (name, size);
    if (!z2)
	return z;
    zs.next_out = z2->data;
    zs.avail_out = size;
    first = 1;
    do {
	zs.next_in = buffer;
	zs.avail_in = sizeof (buffer);
	zfile_fread (buffer, sizeof (buffer), 1, z);
	if (first) {
	    if (inflateInit2 (&zs, -MAX_WBITS) != Z_OK)
		break;
	    first = 0;
	}
	ret = inflate (&zs, 0);
    } while (ret == Z_OK);
    inflateEnd (&zs);
    if (ret != Z_STREAM_END || first != 0) {
	zfile_fclose (z2);
	return z;
    }
    zfile_fclose (z);
    return z2;
#endif
}


/****i* januae-zfile/bunzip ************************************************
*
*   NAME
*       bunzip() -- Pass through the requested bzip2 stream.
*
*   SYNOPSIS
*       static struct zfile *bunzip (const char *decompress, struct zfile
*       *z);
*
*   FUNCTION
*       Return the supplied zfile without performing decompression.
*
*   INPUTS
*       decompress - Requested decompression command; z - Source zfile.
*
*   RESULT
*       The original zfile handle.
*
****************************************************************************
*
*/

static struct zfile *bunzip (const char *decompress, struct zfile *z)
{
    return z;
}

/****i* januae-zfile/lha ***************************************************
*
*   NAME
*       lha() -- Pass through an LHA archive.
*
*   SYNOPSIS
*       static struct zfile *lha (struct zfile *z);
*
*   FUNCTION
*       Return the supplied archive handle without extracting its contents.
*
*   INPUTS
*       z - Source zfile.
*
*   RESULT
*       The original zfile handle.
*
****************************************************************************
*
*/

static struct zfile *lha (struct zfile *z)
{
    return z;
}

/****i* januae-zfile/dms ***************************************************
*
*   NAME
*       dms() -- Expand a DMS disk image into a memory-backed disk.
*
*   SYNOPSIS
*       static struct zfile *dms (struct zfile *z);
*
*   FUNCTION
*       Allocate a disk-image-sized memory zfile and invoke DMS_Process_File
*       to unpack the input stream. On a successful DMS status, close the
*       input handle and return the output.
*
*   INPUTS
*       z - DMS input zfile.
*
*   RESULT
*       The expanded image on a successful unpack; otherwise the original
*       input handle.
*
****************************************************************************
*
*/

static struct zfile *dms (struct zfile *z)
{
    int ret;
    struct zfile *zo;

    zo = zfile_fopen_empty ("zipped.dms", 1760 * 512);
    if (!zo) return z;
    ret = DMS_Process_File (z, zo, CMD_UNPACK, OPT_VERBOSE, 0, 0);
    if (ret == NO_PROBLEM || ret == DMS_FILE_END) {
	zfile_fclose (z);
	return zo;
    }
    return z;
}


static const char *ignoreextensions[] =
    { ".gif", ".jpg", ".png", ".xml", ".pdf", ".txt", 0 };
static const char *diskimageextensions[] =
    { ".adf", ".adz", ".ipf", ".fdi", 0 };

/****i* januae-zfile/isdiskimage *******************************************
*
*   NAME
*       isdiskimage() -- Recognize supported disk-image filename suffixes.
*
*   SYNOPSIS
*       static int isdiskimage (char *name);
*
*   FUNCTION
*       Compare the final four characters of the filename against the
*       configured ADF, ADZ, IPF and FDI extensions without case
*       sensitivity.
*
*   INPUTS
*       name - Filename to examine.
*
*   RESULT
*       One for a recognized image suffix; zero otherwise.
*
****************************************************************************
*
*/

static int isdiskimage (char *name)
{
    int i;

    i = 0;
    while (diskimageextensions[i]) {
	if (strlen (name) > 3 && !strcasecmp (name + strlen (name) - 4, diskimageextensions[i]))
	    return 1;
	i++;
    }
    return 0;
}

/****i* januae-zfile/unzip *************************************************
*
*   NAME
*       unzip() -- Select and extract an archive member.
*
*   SYNOPSIS
*       static struct zfile *unzip (struct zfile *z);
*
*   FUNCTION
*       Enumerate ZIP members, ignore configured auxiliary extensions, and
*       extract a selected member into an in-memory zfile. The selection
*       considers an explicit member name, a numbered selector or the first
*       recognized file type.
*
*   INPUTS
*       z - Input archive handle; its optional zipname selects a member.
*
*   RESULT
*       The selected extracted zfile when available; otherwise the original
*       archive handle.
*
*   NOTES
*       Disk-image member names may be sent to the disk history list.
*
****************************************************************************
*
*/

static struct zfile *unzip (struct zfile *z)
{
    unzFile uz;
    unz_file_info file_info;
    char filename_inzip[2048];
    struct zfile *zf;
    unsigned int err, zipcnt, i, we_have_file = 0;
    int select;
    char tmphist[MAX_DPATH];
    int first = 1;

    if (!zlib_test ())
	return z;
    zf = 0;
    uz = unzOpen (z);
    if (!uz)
	return z;
    if (unzGoToFirstFile (uz) != UNZ_OK)
	return z;
    zipcnt = 1;
    tmphist[0] = 0;
    for (;;) {
	err = unzGetCurrentFileInfo(uz,&file_info,filename_inzip,sizeof(filename_inzip),NULL,0,NULL,0);
	if (err != UNZ_OK)
	    return z;
	if (file_info.uncompressed_size > 0) {
	    i = 0;
	    while (ignoreextensions[i]) {
		if (strlen(filename_inzip) > strlen (ignoreextensions[i]) &&
		    !strcasecmp (ignoreextensions[i], filename_inzip + strlen (filename_inzip) - strlen (ignoreextensions[i])))
		    break;
		i++;
	    }
	    if (!ignoreextensions[i]) {
		if (tmphist[0]) {
		    DISK_history_add (tmphist, -1);
		    tmphist[0] = 0;
		    first = 0;
		}
		if (first) {
		    if (isdiskimage (filename_inzip))
			sprintf (tmphist,"%s/%s", z->name, filename_inzip);
		} else {
		    sprintf (tmphist,"%s/%s", z->name, filename_inzip);
		    DISK_history_add (tmphist, -1);
		    tmphist[0] = 0;
		}
		select = 0;
		if (!z->zipname)
		    select = 1;
		if (z->zipname && !strcasecmp (z->zipname, filename_inzip))
		    select = -1;
		if (z->zipname && z->zipname[0] == '#' && atol (z->zipname + 1) == (int)zipcnt)
		    select = -1;
		if (select && !we_have_file) {
		    unsigned int err = unzOpenCurrentFile (uz);
		    if (err == UNZ_OK) {
			zf = zfile_fopen_empty (filename_inzip, file_info.uncompressed_size);
			if (zf) {
			    err = unzReadCurrentFile  (uz, zf->data, file_info.uncompressed_size);
			    unzCloseCurrentFile (uz);
			    if (err == 0 || err == file_info.uncompressed_size) {
				zf = zuncompress (zf);
				if (select < 0 || zfile_gettype (zf)) {
				    we_have_file = 1;
				}
			    }
			}
			if (!we_have_file) {
			    zfile_fclose (zf);
			    zf = 0;
			}
		    }
		}
	    }
	}
	zipcnt++;
	err = unzGoToNextFile (uz);
	if (err != UNZ_OK)
	    break;
    }
    if (zf) {
	zfile_fclose (z);
	z = zf;
    }
    return z;
}

/****i* januae-zfile/zuncompress *******************************************
*
*   NAME
*       zuncompress() -- Dispatch automatic compressed-file handling.
*
*   SYNOPSIS
*       static struct zfile *zuncompress (struct zfile *z);
*
*   FUNCTION
*       Inspect recognized filename extensions and, for other extensions,
*       initial magic bytes to select ZIP, gzip, DMS or LHA processing.
*
*   INPUTS
*       z - Source zfile opened for inspection.
*
*   RESULT
*       The resulting zfile handle; handlers may return the original handle
*       unchanged.
*
****************************************************************************
*
*/

static struct zfile *zuncompress (struct zfile *z)
{
    char *name = z->name;
    char *ext = strrchr (name, '.');
    uae_u8 header[4];

    if (ext != NULL) {
	ext++;
	if (strcasecmp (ext, "zip") == 0 && zlib_test ())
	     return unzip (z);
	if (strcasecmp (ext, "gz") == 0)
	     return gunzip (z);
	if (strcasecmp (ext, "adz") == 0)
	     return gunzip (z);
	if (strcasecmp (ext, "roz") == 0)
	     return gunzip (z);
	if (strcasecmp (ext, "dms") == 0)
	     return dms (z);
	if (strcasecmp (ext, "lha") == 0
	    || strcasecmp (ext, "lzh") == 0)
	    return lha (z);
	memset (header, 0, sizeof (header));
	zfile_fseek (z, 0, SEEK_SET);
	zfile_fread (header, sizeof (header), 1, z);
	zfile_fseek (z, 0, SEEK_SET);
	if (header[0] == 0x1f && header[1] == 0x8b)
	    return gunzip (z);
	if (header[0] == 'P' && header[1] == 'K')
	    return unzip (z);
	if (header[0] == 'D' && header[1] == 'M' && header[2] == 'S' && header[3] == '!')
	    return dms (z);
    }
    return z;
}

/****i* januae-zfile/openzip ***********************************************
*
*   NAME
*       openzip() -- Open a ZIP container named within a path.
*
*   SYNOPSIS
*       static FILE *openzip (char *name, char *zippath);
*
*   FUNCTION
*       Inspect path separators for a preceding .zip component and open that
*       container for reading when found.
*
*   INPUTS
*       name - Mutable path inspected for an embedded ZIP filename; zippath
*       - Optional output buffer for the path within the archive.
*
*   RESULT
*       An open standard FILE handle for the ZIP container, or NULL.
*
*   NOTES
*       The routine temporarily terminates the candidate container prefix
*       while probing.
*
****************************************************************************
*
*/

static FILE *openzip (char *name, char *zippath)
{
    int i;
    char v;

    i = strlen (name) - 2;
    if (zippath)
	zippath[0] = 0;
    while (i > 0) {
	if ((name[i] == '/' || name[i] == '\\') && i > 4) {
	    v = name[i];
	    name[i] = 0;
	    if (!strcasecmp (name + i - 4, ".zip")) {
		FILE *f = fopen (name, "rb");
		if (f) {
		    if (zippath)
			strcpy (zippath, name + i + 1);
		    return f;
		}
	    }
	    name[i] = v;
	}
	i--;
    }
    return 0;
}

#ifdef SINGLEFILE
extern uae_u8 singlefile_data[];

/****i* januae-zfile/zfile_opensinglefile **********************************
*
*   NAME
*       zfile_opensinglefile() -- Resolve an embedded single-file payload.
*
*   SYNOPSIS
*       static struct zfile *zfile_opensinglefile (struct zfile *l);
*
*   FUNCTION
*       Search the compiled-in singlefile_data index for the requested
*       basename and bind the matching payload to the supplied memory-backed
*       zfile.
*
*   INPUTS
*       l - zfile whose name identifies the embedded payload.
*
*   RESULT
*       The supplied zfile when found; otherwise NULL.
*
*   NOTES
*       Compiled only when SINGLEFILE is defined.
*
****************************************************************************
*
*/

static struct zfile *zfile_opensinglefile (struct zfile *l)
{
    uae_u8 *p = singlefile_data;
    int size, offset;
    char tmp[256], *s;

    strcpy (tmp, l->name);
    s = tmp + strlen (tmp) - 1;
    while (*s != 0 && *s != '/' && *s != '\\') s--;
    if (s > tmp)
	s++;
    write_log("loading from singlefile: '%s'\n", tmp);
    while (*p++);
    offset = (p[0] << 24)|(p[1] << 16)|(p[2] << 8)|(p[3] << 0);
    p += 4;
    for (;;) {
	size = (p[0] << 24)|(p[1] << 16)|(p[2] << 8)|(p[3] << 0);
	if (!size)
	    break;
	if (!strcmpi (tmp, p + 4)) {
	    l->data = singlefile_data + offset;
	    l->size = size;
	    write_log ("found, size %d\n", size);
	    return l;
	}
	offset += size;
	p += 4;
	p += strlen (p) + 1;
    }
    write_log ("not found\n");
    return 0;
}
#endif

/*
 * fopen() for a compressed file
 */
/****i* januae-zfile/zfile_fopen *******************************************
*
*   NAME
*       zfile_fopen() -- Open a file with optional archive-member expansion.
*
*   SYNOPSIS
*       struct zfile *zfile_fopen (const char *name, const char *mode);
*
*   FUNCTION
*       Allocate a tracked zfile, attempt to open an embedded ZIP container
*       or ordinary file, and pass the opened handle to the automatic
*       decompression dispatcher.
*
*   INPUTS
*       name - File path, optionally naming an archive member; mode -
*       Standard file opening mode.
*
*   RESULT
*       An open zfile handle, or NULL when opening fails.
*
*   NOTES
*       Embedded ZIP members are accepted only when mode is rb.
*
****************************************************************************
*
*/

struct zfile *zfile_fopen (const char *name, const char *mode)
{
    struct zfile *l;
    FILE *f;
    char zipname[1000];

    if( *name == '\0' )
	return NULL;
    l = zfile_create ();
    l->name = strdup (name);
#ifdef SINGLEFILE
    if (zfile_opensinglefile (l))
	return l;
#endif
    f = openzip (l->name, zipname);
    if (f) {
	if (strcasecmp (mode, "rb")) {
	    zfile_fclose (l);
	    fclose (f);
	    return 0;
	}
	l->zipname = strdup (zipname);
    }
    if (!f) {
	f = fopen (name, mode);
	if (!f) {
	    zfile_fclose (l);
	    return 0;
	}
    }
    l->f = f;
    l = zuncompress (l);
    return l;
}

/****i* januae-zfile/zfile_exists ******************************************
*
*   NAME
*       zfile_exists() -- Test file or ZIP-container path availability.
*
*   SYNOPSIS
*       int zfile_exists (const char *name);
*
*   FUNCTION
*       Try to open a named ZIP container prefix or the ordinary path in
*       read-binary mode, then close the probe handle.
*
*   INPUTS
*       name - Path to test.
*
*   RESULT
*       One if a container or ordinary file can be opened; zero otherwise.
*
****************************************************************************
*
*/

int zfile_exists (const char *name)
{
    char fname[2000];
    FILE *f;

    strcpy (fname, name);
    f = openzip (fname, 0);
    if (!f)
	f = fopen(name,"rb");
    if (!f)
	return 0;
    fclose (f);
    return 1;
}

/****i* januae-zfile/zfile_iscompressed ************************************
*
*   NAME
*       zfile_iscompressed() -- Identify a memory-backed zfile.
*
*   SYNOPSIS
*       int zfile_iscompressed (struct zfile *z);
*
*   FUNCTION
*       Check whether the handle stores file contents in its data buffer
*       rather than through a standard FILE stream.
*
*   INPUTS
*       z - Open zfile to inspect.
*
*   RESULT
*       One for an in-memory handle; zero for a FILE-backed handle.
*
****************************************************************************
*
*/

int zfile_iscompressed (struct zfile *z)
{
    return z->data ? 1 : 0;
}

/****i* januae-zfile/zfile_fopen_empty *************************************
*
*   NAME
*       zfile_fopen_empty() -- Allocate a zero-filled in-memory zfile.
*
*   SYNOPSIS
*       struct zfile *zfile_fopen_empty (const char *name, int size);
*
*   FUNCTION
*       Create a tracked zfile with a copy of the supplied name and a
*       zero-initialized memory buffer of the requested size.
*
*   INPUTS
*       name - Logical name for the buffer; size - Requested buffer size in
*       bytes.
*
*   RESULT
*       A newly created zfile handle with memory-backed storage.
*
****************************************************************************
*
*/

struct zfile *zfile_fopen_empty (const char *name, int size)
{
    struct zfile *l;
    l = zfile_create ();
    l->name = strdup (name);
    l->data = malloc (size);
    l->size = size;
    memset (l->data, 0, size);
    return l;
}

/****i* januae-zfile/zfile_ftell *******************************************
*
*   NAME
*       zfile_ftell() -- Report the current zfile position.
*
*   SYNOPSIS
*       long zfile_ftell (struct zfile *z);
*
*   FUNCTION
*       Return the internal offset for a memory-backed zfile or delegate to
*       ftell for an ordinary file.
*
*   INPUTS
*       z - Open zfile.
*
*   RESULT
*       Current memory offset, or the result of ftell on the backing file.
*
****************************************************************************
*
*/

long zfile_ftell (struct zfile *z)
{
    if (z->data)
	return z->seek;
    return ftell (z->f);
}

/****i* januae-zfile/zfile_fseek *******************************************
*
*   NAME
*       zfile_fseek() -- Move the current zfile position.
*
*   SYNOPSIS
*       int zfile_fseek (struct zfile *z, long offset, int mode);
*
*   FUNCTION
*       Update and clamp the memory offset for SEEK_SET, SEEK_CUR or
*       SEEK_END; for a file-backed handle, delegate to fseek.
*
*   INPUTS
*       z - Open zfile; offset - Requested displacement; mode - SEEK_*
*       origin selector.
*
*   RESULT
*       For an in-memory handle, the prior offset; for a file-backed handle,
*       the fseek status.
*
*   NOTES
*       The in-memory SEEK_END calculation subtracts offset from the buffer
*       size.
*
****************************************************************************
*
*/

int zfile_fseek (struct zfile *z, long offset, int mode)
{
    if (z->data) {
	int old = z->seek;
	switch (mode) {
	    case SEEK_SET:
		z->seek = offset;
		break;
	    case SEEK_CUR:
		z->seek += offset;
		break;
	    case SEEK_END:
		z->seek = z->size - offset;
		break;
	}
	if (z->seek < 0) z->seek = 0;
	if (z->seek > z->size) z->seek = z->size;
	return old;
    }
    return fseek (z->f, offset, mode);
}

/****i* januae-zfile/zfile_fread *******************************************
*
*   NAME
*       zfile_fread() -- Read bytes or records from a zfile.
*
*   SYNOPSIS
*       size_t zfile_fread (void *b, size_t l1, size_t l2, struct zfile *z);
*
*   FUNCTION
*       Copy up to the remaining in-memory bytes and advance the memory
*       offset, or delegate the read to fread for a standard file.
*
*   INPUTS
*       b - Destination buffer; l1 - Record size; l2 - Record count; z -
*       Source zfile.
*
*   RESULT
*       For memory storage, the number of bytes copied; for file storage,
*       the number of complete records read.
*
****************************************************************************
*
*/

size_t zfile_fread  (void *b, size_t l1, size_t l2, struct zfile *z)
{
    long len = l1 * l2;
    if (z->data) {
	if (z->seek + len > z->size)
	    len = z->size - z->seek;
	memcpy (b, z->data + z->seek, len);
	z->seek += len;
	return len;
    }
    return fread (b, l1, l2, z->f);
}

/****i* januae-zfile/zfile_fwrite ******************************************
*
*   NAME
*       zfile_fwrite() -- Write bytes or records to a zfile.
*
*   SYNOPSIS
*       size_t zfile_fwrite (const void *b, size_t l1, size_t l2, struct
*       zfile *z);
*
*   FUNCTION
*       Copy up to the remaining in-memory capacity and advance the offset,
*       or delegate to fwrite for a standard file.
*
*   INPUTS
*       b - Source buffer; l1 - Record size; l2 - Record count; z -
*       Destination zfile.
*
*   RESULT
*       For memory storage, the number of bytes copied; for file storage,
*       the number of complete records written.
*
****************************************************************************
*
*/

size_t zfile_fwrite  (const void *b, size_t l1, size_t l2, struct zfile *z)
{
    long len = l1 * l2;
    if (z->data) {
	if (z->seek + len > z->size)
	    len = z->size - z->seek;
	memcpy (z->data + z->seek, b, len);
	z->seek += len;
	return len;
    }
    return fwrite (b, l1, l2, z->f);
}

/****i* januae-zfile/zfile_zuncompress *************************************
*
*   NAME
*       zfile_zuncompress() -- Attempt zlib stream expansion into caller
*       memory.
*
*   SYNOPSIS
*       int zfile_zuncompress (void *dst, int dstsize, struct zfile *src,
*       int srcsize);
*
*   FUNCTION
*       When the non-GTKMUI zlib path is enabled, consume at most the
*       requested compressed input span and inflate into the supplied output
*       buffer.
*
*   INPUTS
*       dst - Output buffer; dstsize - Output capacity; src - Compressed
*       source zfile; srcsize - Compressed input span.
*
*   RESULT
*       Zero in every compiled branch, including after the inflation
*       attempt.
*
*   NOTES
*       GTKMUI builds do not perform the inflation loop.
*
****************************************************************************
*
*/

int zfile_zuncompress (void *dst, int dstsize, struct zfile *src, int srcsize)
{
    z_stream zs;
    int v;
    uae_u8 inbuf[4096];
    int incnt;

    if (!zlib_test ())
	return 0;

#if !defined GTKMUI
    memset (&zs, 0, sizeof(zs));
    if (inflateInit (&zs) != Z_OK)
	return 0;
    zs.next_out = dst;
    zs.avail_out = dstsize;
    incnt = 0;
    v = Z_OK;
    while (v == Z_OK && zs.avail_out > 0) {
	if (zs.avail_in == 0) {
	    int left = srcsize - incnt;
	    if (left == 0)
		break;
	    if (left > (int)sizeof (inbuf)) left = sizeof (inbuf);
	    zs.next_in = inbuf;
	    zs.avail_in = zfile_fread (inbuf, 1, left, src);
	    incnt += left;
	}
	v = inflate (&zs, 0);
    }
    inflateEnd (&zs);
#endif
    return 0;
}

/****i* januae-zfile/zfile_zcompress ***************************************
*
*   NAME
*       zfile_zcompress() -- Write a zlib-compressed memory region.
*
*   SYNOPSIS
*       int zfile_zcompress (struct zfile *f, void *src, int size);
*
*   FUNCTION
*       When compression is enabled, deflate the supplied buffer in blocks
*       and write resulting bytes to the target zfile.
*
*   INPUTS
*       f - Destination zfile; src - Source memory region; size - Source
*       length in bytes.
*
*   RESULT
*       Number of compressed bytes emitted on the active deflate path, or
*       zero when unavailable or not initialized.
*
*   NOTES
*       GTKMUI builds return zero without compressing.
*
****************************************************************************
*
*/

int zfile_zcompress (struct zfile *f, void *src, int size)
{
    int v;
    z_stream zs;
    uae_u8 outbuf[4096];

#ifdef WIN32
    if (!is_zlib)
	return 0;
#endif

#if defined GTKMUI
    return 0;
#else
    memset (&zs, 0, sizeof (zs));
    if (deflateInit (&zs, Z_DEFAULT_COMPRESSION) != Z_OK)
	return 0;
    zs.next_in = src;
    zs.avail_in = size;
    v = Z_OK;
    while (v == Z_OK) {
	zs.next_out = outbuf;
        zs.avail_out = sizeof (outbuf);
	v = deflate(&zs, Z_NO_FLUSH | Z_FINISH);
	if (sizeof(outbuf) - zs.avail_out > 0)
	    zfile_fwrite (outbuf, 1, sizeof (outbuf) - zs.avail_out, f);
    }
    deflateEnd (&zs);
    return zs.total_out;
#endif
}

/****i* januae-zfile/zfile_crc32 *******************************************
*
*   NAME
*       zfile_crc32() -- Compute a zfile checksum.
*
*   SYNOPSIS
*       uae_u32 zfile_crc32 (struct zfile *f);
*
*   FUNCTION
*       Run get_crc32 over the in-memory payload, or read the entire backing
*       file into a temporary buffer, restore its prior cursor position and
*       compute the checksum.
*
*   INPUTS
*       f - zfile to checksum; NULL is accepted.
*
*   RESULT
*       CRC-32 of the stored data; zero for NULL input or a failed
*       temporary-buffer allocation.
*
****************************************************************************
*
*/

uae_u32 zfile_crc32 (struct zfile *f)
{
    uae_u8 *p;
    int pos, size;
    uae_u32 crc;

    if (!f)
	return 0;
    if (f->data)
	return get_crc32 (f->data, f->size);
    pos = zfile_ftell (f);
    zfile_fseek (f, 0, SEEK_END);
    size = zfile_ftell (f);
    p = xmalloc (size);
    if (!p)
	return 0;
    memset (p, 0, size);
    zfile_fseek (f, 0, SEEK_SET);
    zfile_fread (p, 1, size, f);
    zfile_fseek (f, pos, SEEK_SET);
    crc = get_crc32 (p, size);
    free (p);
    return crc;
}
