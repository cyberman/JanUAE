/**
  * UAE - The Un*x Amiga Emulator
  *
  * Hardfile emulation for *nix systems
  *
  * Copyright 2003-2006 Richard Drummond
  * Based on hardfile_win32.c
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "filesys.h"

//#define HDF_DEBUG
#ifdef  HDF_DEBUG
#define DEBUG_LOG write_log ( "%s: ", __func__); write_log
#else
#define DEBUG_LOG(...) do {} while(0)
#endif


/****i* januae-hardfile-unix/hdf_seek ****************************************
*
*   NAME
*       hdf_seek() -- Seek to a hardfile-relative block position.
*
*   SYNOPSIS
*       static int hdf_seek (struct hardfiledata *hfd, uae_u64 offset);
*
*   FUNCTION
*       Check the descriptor's size and offset sentinels, require an in-range,
*       block-aligned position, and seek the underlying host file.
*
*   INPUTS
*       hfd - Open hardfile descriptor with consistent size and offset fields.
*       offset - Byte offset relative to hfd->offset.
*
*   RESULT
*       Zero on successful lseek(); -1 on seek failure or when the
*       absolute position exceeds the 2 GiB limit with a narrow off_t.
*
*   NOTES
*       Invalid bounds, alignment or descriptor sentinels terminate the process.
*
****************************************************************************
*
*/

static int hdf_seek (struct hardfiledata *hfd, uae_u64 offset)
{
    off_t ret;

    DEBUG_LOG ("called with offset=0x%llx\n", offset);

    if (hfd->offset != hfd->offset2 || hfd->size != hfd->size2) {
	gui_message ("hd: memory corruption detected in seek");
	abort ();
    }

    if (offset >= hfd->size) {
	gui_message ("hd: tried to seek out of bounds! (0x%llx >= 0x%llx)\n", offset, hfd->size);
	abort ();
    }

    offset += hfd->offset;
    if (offset & (hfd->blocksize - 1)) {
	gui_message ("hd: poscheck failed, offset not aligned to blocksize! (0x%llx & 0x%04.4x = 0x%04.4x)\n", offset, hfd->blocksize, offset & (hfd->blocksize - 1));
	abort ();
    }

  if (offset >= 0x80000000 && sizeof (off_t) < sizeof (uae_u64)) {
	DEBUG_LOG ("Failed to seek passed 2GB limit (0x%llx)\n", offset);
	return -1;
    }

    ret = lseek (hfd->handle, offset, SEEK_SET);

    if (ret == -1) {
	DEBUG_LOG ("seek failed\n");
	return -1;
    }

    DEBUG_LOG ("seek okay\n");
    return 0;
}

/****i* januae-hardfile-unix/poscheck ****************************************
*
*   NAME
*       poscheck() -- Check the current hardfile position.
*
*   SYNOPSIS
*       static void poscheck (struct hardfiledata *hfd, int len);
*
*   FUNCTION
*       Read the host file position and check its relation to the configured
*       hardfile bounds and block alignment.
*
*   INPUTS
*       hfd - Open hardfile descriptor.
*       len - Requested transfer length, which must not be negative.
*
*   RESULT
*       None.
*
*   NOTES
*       Inconsistent size or offset fields, failed lseek(), invalid
*       position, negative length or misalignment terminate the process.
*       The current check does not establish that the full transfer fits.
*
****************************************************************************
*
*/

static void poscheck (struct hardfiledata *hfd, int len)
{
    off_t pos;

    if (hfd->offset != hfd->offset2 || hfd->size != hfd->size2) {
	gui_message ("hd: memory corruption detected in poscheck");
	abort ();
    }

    pos = lseek (hfd->handle, 0, SEEK_CUR);

    if (pos == -1 ) {
	gui_message ("hd: poscheck failed. seek failure, error %d", errno);
	abort ();
    }
    if (len < 0) {
	gui_message ("hd: poscheck failed, negative length! (%d)", len);
	abort ();
    }

    if ((uae_u64)pos < hfd->offset) {
	gui_message ("hd: poscheck failed, offset out of bounds! (0x%llx < 0x%llx)", pos, hfd->offset);
	abort ();
    }
    if ((uae_u64)pos >= hfd->offset + hfd->size || (uae_u64)pos >= hfd->offset + hfd->size + len) {
	gui_message ("hd: poscheck failed, offset out of bounds! (0x%llx >= 0x%llx, LEN=%d)", pos, hfd->offset + hfd->size, len);
	abort ();
    }
    if (pos & (hfd->blocksize - 1)) {
	gui_message ("hd: poscheck failed, offset not aligned to blocksize! (0x%llx & 0x%04.4x = 0x%04.4x\n", pos, hfd->blocksize, pos & hfd->blocksize);
	abort ();
    }
}

/****f* januae-hardfile-unix/hdf_open ****************************************
*
*   NAME
*       hdf_open() -- Open a host file as a hardfile.
*
*   SYNOPSIS
*       int hdf_open (struct hardfiledata *hfd, const char *name);
*
*   FUNCTION
*       Open the named file read-only or read/write according to hfd->readonly,
*       initialize its handle, cached state, identification strings, size and
*       512-byte block size.
*
*   INPUTS
*       hfd - Hardfile descriptor to initialize; readonly is used as input.
*       name - NUL-terminated host path.
*
*   RESULT
*       1 if open() succeeds; 0 if open() fails.
*
*   NOTES
*       The end-of-file lseek() result is not checked. Success therefore does
*       not independently establish a valid file size. The caller owns hfd and
*       must close the opened handle.
*
****************************************************************************
*
*/

int hdf_open (struct hardfiledata *hfd, const char *name)
{
    int handle;

    DEBUG_LOG ("called with name=%s\n",name);

   if ((handle = open (name, hfd->readonly ? O_RDONLY : O_RDWR)) != -1) {
	int i;

	hfd->handle = handle;
	hfd->cache = 0;

	i = strlen (name) - 1;
	while (i >= 0) {
	    if ((i > 0 && (name[i - 1] == '/' || name[i - 1] == '\\')) || i == 0) {
		strcpy (hfd->vendor_id, "UAE");
		strncpy (hfd->product_id, name + i, 15);
		strcpy (hfd->product_rev, "0.2");
		break;
	    }
	    i--;
	}

	hfd->size = hfd->size2 = lseek (handle, 0, SEEK_END);
	hfd->blocksize = 512;
	lseek (handle, 0, SEEK_SET);

	DEBUG_LOG ("okay\n");
	return 1;
    }

    DEBUG_LOG ("failed\n");
    return 0;
}

/****f* januae-hardfile-unix/hdf_dup *****************************************
*
*   NAME
*       hdf_dup() -- Duplicate a hardfile host handle.
*
*   SYNOPSIS
*       extern int hdf_dup (struct hardfiledata *dhfd, const struct hardfiledata *shfd);
*
*   FUNCTION
*       If the source handle is nonnegative, call dup() and assign its result
*       to the destination handle.
*
*   INPUTS
*       dhfd - Destination descriptor whose handle is assigned on that path.
*       shfd - Source descriptor supplying the handle.
*
*   RESULT
*       Always returns 0, including when dup() fails.
*
*   NOTES
*       This function does not copy any other descriptor fields. If the
*       source handle is negative, it leaves the destination handle unchanged.
*
****************************************************************************
*
*/

extern int hdf_dup (struct hardfiledata *dhfd, const struct hardfiledata *shfd)
{
    DEBUG_LOG ("called\n");

    if (shfd->handle >= 0) {
	dhfd->handle = dup (shfd->handle);
    }

    return 0;
}

/****f* januae-hardfile-unix/hdf_close ***************************************
*
*   NAME
*       hdf_close() -- Close a hardfile host handle.
*
*   SYNOPSIS
*       void hdf_close (struct hardfiledata *hfd);
*
*   FUNCTION
*       Call close() for the descriptor handle and set the stored handle
*       to -1.
*
*   INPUTS
*       hfd - Hardfile descriptor to close.
*
*   RESULT
*       None.
*
*   NOTES
*       The result of close() is not inspected.
*
****************************************************************************
*
*/

void hdf_close (struct hardfiledata *hfd)
{
    DEBUG_LOG ("called\n");

    close (hfd->handle);
    hfd->handle = -1;
}

/****f* januae-hardfile-unix/hdf_read ****************************************
*
*   NAME
*       hdf_read() -- Read bytes from a host-backed hardfile.
*
*   SYNOPSIS
*       int hdf_read (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len);
*
*   FUNCTION
*       Invalidate the hardfile cache, attempt to seek to the requested
*       relative offset, check the current position, then call read().
*
*   INPUTS
*       hfd - Open hardfile descriptor.
*       buffer - Writable destination buffer for len bytes.
*       offset - Byte offset relative to the configured hardfile origin.
*       len - Requested byte count.
*
*   RESULT
*       Number of bytes returned by read(), or -1 on its failure.
*
*   NOTES
*       The return value of hdf_seek() is ignored. Position-check failures
*       terminate the process; successful positioning and a complete transfer
*       are not guaranteed.
*
****************************************************************************
*
*/

int hdf_read (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len)
{
    int n;

    DEBUG_LOG ("called with offset=0x%llx len=%d\n", offset, len);

    hfd->cache_valid = 0;
    hdf_seek (hfd, offset);
    poscheck (hfd, len);
    n = read (hfd->handle, buffer, len);

    DEBUG_LOG ("read %d bytes\n", n);

    return n;
}

/****f* januae-hardfile-unix/hdf_write ***************************************
*
*   NAME
*       hdf_write() -- Write bytes to a host-backed hardfile.
*
*   SYNOPSIS
*       int hdf_write (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len);
*
*   FUNCTION
*       Invalidate the hardfile cache, attempt to seek to the requested
*       relative offset, check the current position, then call write().
*
*   INPUTS
*       hfd - Open writable hardfile descriptor.
*       buffer - Source buffer containing len bytes.
*       offset - Byte offset relative to the configured hardfile origin.
*       len - Requested byte count.
*
*   RESULT
*       Number of bytes returned by write(), or -1 on its failure.
*
*   NOTES
*       The return value of hdf_seek() is ignored. Position-check failures
*       terminate the process; a complete write is not guaranteed.
*
****************************************************************************
*
*/

int hdf_write (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len)
{
    int n;

    DEBUG_LOG ("called with offset=0x%llx len=%d\n", offset, len);

    hfd->cache_valid = 0;
    hdf_seek (hfd, offset);
    poscheck (hfd, len);
    n = write (hfd->handle, buffer, len);

    DEBUG_LOG ("Wrote %d bytes\n", n);

    return n;
}
