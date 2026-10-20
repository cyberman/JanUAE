/* unzip.c -- IO on .zip files using zlib 
   Version 0.15 beta, Mar 19th, 1998,

   Read unzip.h for more info
*/

#include "sysconfig.h"
#include "sysdeps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"
#include "zfile.h"
#include "unzip.h"

#ifdef STDC
#  include <stddef.h>
#  include <string.h>
#  include <stdlib.h>
#endif
#ifdef NO_ERRNO_H
    extern int errno;
#else
#   include <errno.h>
#endif


#ifndef local
#  define local static
#endif
/* compile with -Dlocal if your debugger can't find static symbols */



#if !defined(unix) && !defined(CASESENSITIVITYDEFAULT_YES) && \
                      !defined(CASESENSITIVITYDEFAULT_NO)
#define CASESENSITIVITYDEFAULT_NO
#endif


#ifndef UNZ_BUFSIZE
#define UNZ_BUFSIZE (16384)
#endif

#ifndef UNZ_MAXFILENAMEINZIP
#define UNZ_MAXFILENAMEINZIP (256)
#endif

#ifndef ALLOC
# define ALLOC(size) (malloc(size))
#endif
#ifndef TRYFREE
# define TRYFREE(p) {if (p) free(p);}
#endif

#define SIZECENTRALDIRITEM (0x2e)
#define SIZEZIPLOCALHEADER (0x1e)


/* I've found an old Unix (a SunOS 4.1.3_U1) without all SEEK_* defined.... */

#ifndef SEEK_CUR
#define SEEK_CUR    1
#endif

#ifndef SEEK_END
#define SEEK_END    2
#endif

#ifndef SEEK_SET
#define SEEK_SET    0
#endif

const char unz_copyright[] =
   " unzip 0.15 Copyright 1998 Gilles Vollant ";

/* unz_file_info_interntal contain internal info about a file in zipfile*/
typedef struct unz_file_info_internal_s
{
    uLong offset_curfile;/* relative offset of local header 4 bytes */
} unz_file_info_internal;


/* file_in_zip_read_info_s contain internal information about a file in zipfile,
    when reading and decompress it */
typedef struct
{
	char  *read_buffer;         /* internal buffer for compressed data */
	z_stream stream;            /* zLib stream structure for inflate */

	uLong pos_in_zipfile;       /* position in byte on the zipfile, for fseek*/
	uLong stream_initialised;   /* flag set if stream structure is initialised*/

	uLong offset_local_extrafield;/* offset of the local extra field */
	uInt  size_local_extrafield;/* size of the local extra field */
	uLong pos_local_extrafield;   /* position in the local extra field in read*/

	uLong crc32;                /* crc32 of all data uncompressed */
	uLong crc32_wait;           /* crc32 we must obtain after decompress all */
	uLong rest_read_compressed; /* number of byte to be decompressed */
	uLong rest_read_uncompressed;/*number of byte to be obtained after decomp*/
	struct zfile *file;                 /* io structore of the zipfile */
	uLong compression_method;   /* compression method (0==store) */
	uLong byte_before_the_zipfile;/* byte before the zipfile, (>0 for sfx)*/
} file_in_zip_read_info_s;


/* unz_s contain internal information about the zipfile
*/
typedef struct
{
	struct zfile* file;                 /* io structore of the zipfile */
	unz_global_info gi;       /* public global information */
	uLong byte_before_the_zipfile;/* byte before the zipfile, (>0 for sfx)*/
	uLong num_file;             /* number of the current file in the zipfile*/
	uLong pos_in_central_dir;   /* pos of the current file in the central dir*/
	uLong current_file_ok;      /* flag about the usability of the current file*/
	uLong central_pos;          /* position of the beginning of the central dir*/

	uLong size_central_dir;     /* size of the central directory  */
	uLong offset_central_dir;   /* offset of start of central directory with
								   respect to the starting disk number */

	unz_file_info cur_file_info; /* public info about the current file in zip*/
	unz_file_info_internal cur_file_info_internal; /* private info about it*/
    file_in_zip_read_info_s* pfile_in_zip_read; /* structure about the current
	                                    file if we are decompressing it */
} unz_s;


/****i* januae-zip/unzlocal_getByte *********************************************
*
*   NAME
*       unzlocal_getByte()
*
*   SYNOPSIS
*       static int unzlocal_getByte(struct zfile *fin, int *pi);
*
*   FUNCTION
*       Read one byte from a zfile and store its unsigned value in *pi.
*
*   RESULT
*       UNZ_OK after one byte; UNZ_EOF when no byte is read.
*
*   NOTES
*       The output is not assigned on failure.
*
*************************************************************************
*
*/
local int unzlocal_getByte(fin,pi)
	struct zfile *fin;
	int *pi;
{
    unsigned char c;
	int err = zfile_fread(&c, 1, 1, fin);
    if (err==1)
    {
        *pi = (int)c;
        return UNZ_OK;
    }
    else
    {
		return UNZ_EOF;
    }
}


/****i* januae-zip/unzlocal_getShort ********************************************
*
*   NAME
*       unzlocal_getShort()
*
*   SYNOPSIS
*       static int unzlocal_getShort(struct zfile *fin, uLong *pX);
*
*   FUNCTION
*       Read a two-byte little-endian value from the archive stream.
*
*   RESULT
*       UNZ_OK on a complete read; otherwise the underlying byte-read status.
*
*   NOTES
*       Stores zero on an incomplete read.
*
*************************************************************************
*
*/
local int unzlocal_getShort (fin,pX)
	struct zfile* fin;
    uLong *pX;
{
    uLong x ;
    int i;
    int err;

    err = unzlocal_getByte(fin,&i);
    x = (uLong)i;
    
    if (err==UNZ_OK)
        err = unzlocal_getByte(fin,&i);
    x += ((uLong)i)<<8;
   
    if (err==UNZ_OK)
        *pX = x;
    else
        *pX = 0;
    return err;
}

/****i* januae-zip/unzlocal_getLong *********************************************
*
*   NAME
*       unzlocal_getLong()
*
*   SYNOPSIS
*       static int unzlocal_getLong(struct zfile *fin, uLong *pX);
*
*   FUNCTION
*       Read a four-byte little-endian value from the archive stream.
*
*   RESULT
*       UNZ_OK on a complete read; otherwise the underlying byte-read status.
*
*   NOTES
*       Stores zero on an incomplete read.
*
*************************************************************************
*
*/
local int unzlocal_getLong (fin,pX)
	struct zfile* fin;
    uLong *pX;
{
    uLong x ;
    int i;
    int err;

    err = unzlocal_getByte(fin,&i);
    x = (uLong)i;
    
    if (err==UNZ_OK)
        err = unzlocal_getByte(fin,&i);
    x += ((uLong)i)<<8;

    if (err==UNZ_OK)
        err = unzlocal_getByte(fin,&i);
    x += ((uLong)i)<<16;

    if (err==UNZ_OK)
        err = unzlocal_getByte(fin,&i);
    x += ((uLong)i)<<24;
   
    if (err==UNZ_OK)
        *pX = x;
    else
        *pX = 0;
    return err;
}


/****i* januae-zip/strcmpcasenosensitive_internal *******************************
*
*   NAME
*       strcmpcasenosensitive_internal()
*
*   SYNOPSIS
*       static int strcmpcasenosensitive_internal(const char *fileName1,
*       const char *fileName2);
*
*   FUNCTION
*       Compare two NUL-terminated filenames after folding ASCII lowercase
*       letters to uppercase.
*
*   RESULT
*       A negative, zero, or positive value according to the comparison.
*
*   NOTES
*       Only ASCII a-z are folded; this is not locale-aware.
*
*************************************************************************
*
*/
local int strcmpcasenosensitive_internal (fileName1,fileName2)
	const char* fileName1;
	const char* fileName2;
{
	for (;;)
	{
		char c1=*(fileName1++);
		char c2=*(fileName2++);
		if ((c1>='a') && (c1<='z'))
			c1 -= 0x20;
		if ((c2>='a') && (c2<='z'))
			c2 -= 0x20;
		if (c1=='\0')
			return ((c2=='\0') ? 0 : -1);
		if (c2=='\0')
			return 1;
		if (c1<c2)
			return -1;
		if (c1>c2)
			return 1;
	}
}


#ifdef  CASESENSITIVITYDEFAULT_NO
#define CASESENSITIVITYDEFAULTVALUE 2
#else
#define CASESENSITIVITYDEFAULTVALUE 1
#endif

#ifndef STRCMPCASENOSENTIVEFUNCTION
#define STRCMPCASENOSENTIVEFUNCTION strcmpcasenosensitive_internal
#endif

/****i* januae-zip/unzStringFileNameCompare *************************************
*
*   NAME
*       unzStringFileNameCompare()
*
*   SYNOPSIS
*       int ZEXPORT unzStringFileNameCompare(const char *fileName1, const
*       char *fileName2, int iCaseSensitivity);
*
*   FUNCTION
*       Compare ZIP entry names using case-sensitive or ASCII case-insensitive
*       comparison.
*
*   RESULT
*       The comparison result from the selected string comparison.
*
*   NOTES
*       Mode 1 is case-sensitive; mode 2 is case-insensitive. Mode 0 selects
*       the compile-time default.
*
*************************************************************************
*
*/
extern int ZEXPORT unzStringFileNameCompare (fileName1,fileName2,iCaseSensitivity)
	const char* fileName1;
	const char* fileName2;
	int iCaseSensitivity;
{
	if (iCaseSensitivity==0)
		iCaseSensitivity=CASESENSITIVITYDEFAULTVALUE;

	if (iCaseSensitivity==1)
		return strcmp(fileName1,fileName2);

	return STRCMPCASENOSENTIVEFUNCTION(fileName1,fileName2);
} 

#define BUFREADCOMMENT (0x400)

/****i* januae-zip/unzlocal_SearchCentralDir ************************************
*
*   NAME
*       unzlocal_SearchCentralDir()
*
*   SYNOPSIS
*       static uLong unzlocal_SearchCentralDir(struct zfile *fin);
*
*   FUNCTION
*       Search backward near the end of the archive for the ZIP
*       end-of-central-directory signature.
*
*   RESULT
*       The located file offset, or zero when no signature is found or a
*       read/allocation fails.
*
*   NOTES
*       Searches at most 0xffff bytes back; offset zero also acts as the
*       not-found sentinel.
*
*************************************************************************
*
*/
local uLong unzlocal_SearchCentralDir(fin)
	struct zfile *fin;
{
	unsigned char* buf;
	uLong uSizeFile;
	uLong uBackRead;
	uLong uMaxBack=0xffff; /* maximum size of global comment */
	uLong uPosFound=0;
	
	if (zfile_fseek(fin,0,SEEK_END) != 0)
		return 0;


	uSizeFile = zfile_ftell( fin );
	
	if (uMaxBack>uSizeFile)
		uMaxBack = uSizeFile;

	buf = (unsigned char*)ALLOC(BUFREADCOMMENT+4);
	if (buf==NULL)
		return 0;

	uBackRead = 4;
	while (uBackRead<uMaxBack)
	{
		uLong uReadSize,uReadPos ;
		int i;
		if (uBackRead+BUFREADCOMMENT>uMaxBack) 
			uBackRead = uMaxBack;
		else
			uBackRead+=BUFREADCOMMENT;
		uReadPos = uSizeFile-uBackRead ;
		
		uReadSize = ((BUFREADCOMMENT+4) < (uSizeFile-uReadPos)) ? 
                     (BUFREADCOMMENT+4) : (uSizeFile-uReadPos);
		if (zfile_fseek(fin,uReadPos,SEEK_SET)!=0)
			break;

		if (zfile_fread(buf,(uInt)uReadSize,1,fin)!=1)
			break;

                for (i=(int)uReadSize-3; (i--)>0;)
			if (((*(buf+i))==0x50) && ((*(buf+i+1))==0x4b) && 
				((*(buf+i+2))==0x05) && ((*(buf+i+3))==0x06))
			{
				uPosFound = uReadPos+i;
				break;
			}

		if (uPosFound!=0)
			break;
	}
	TRYFREE(buf);
	return uPosFound;
}

/****i* januae-zip/unzOpen ******************************************************
*
*   NAME
*       unzOpen()
*
*   SYNOPSIS
*       unzFile ZEXPORT unzOpen(struct zfile *fin);
*
*   FUNCTION
*       Parse the ZIP end record and allocate an archive cursor for the
*       supplied zfile.
*
*   RESULT
*       An allocated ZIP handle or NULL if the end record cannot be validated.
*
*   NOTES
*       On success, unzClose() closes the supplied zfile. On failure, this
*       function does not close fin. Multi-disk archives are rejected.
*
*************************************************************************
*
*/
extern unzFile ZEXPORT unzOpen (fin)
	struct zfile *fin;
{
	unz_s us;
	unz_s *s;
	uLong central_pos,uL;

	uLong number_disk;          /* number of the current dist, used for 
								   spaning ZIP, unsupported, always 0*/
	uLong number_disk_with_CD;  /* number the the disk with central dir, used
								   for spaning ZIP, unsupported, always 0*/
	uLong number_entry_CD;      /* total number of entries in
	                               the central dir 
	                               (same than number_entry on nospan) */

	int err=UNZ_OK;

    if (unz_copyright[0]!=' ')
        return NULL;

	central_pos = unzlocal_SearchCentralDir(fin);
	if (central_pos==0)
		err=UNZ_ERRNO;

	if (zfile_fseek(fin,central_pos,SEEK_SET)!=0)
		err=UNZ_ERRNO;

	/* the signature, already checked */
	if (unzlocal_getLong(fin,&uL)!=UNZ_OK)
		err=UNZ_ERRNO;

	/* number of this disk */
	if (unzlocal_getShort(fin,&number_disk)!=UNZ_OK)
		err=UNZ_ERRNO;

	/* number of the disk with the start of the central directory */
	if (unzlocal_getShort(fin,&number_disk_with_CD)!=UNZ_OK)
		err=UNZ_ERRNO;

	/* total number of entries in the central dir on this disk */
	if (unzlocal_getShort(fin,&us.gi.number_entry)!=UNZ_OK)
		err=UNZ_ERRNO;

	/* total number of entries in the central dir */
	if (unzlocal_getShort(fin,&number_entry_CD)!=UNZ_OK)
		err=UNZ_ERRNO;

	if ((number_entry_CD!=us.gi.number_entry) ||
		(number_disk_with_CD!=0) ||
		(number_disk!=0))
		err=UNZ_BADZIPFILE;

	/* size of the central directory */
	if (unzlocal_getLong(fin,&us.size_central_dir)!=UNZ_OK)
		err=UNZ_ERRNO;

	/* offset of start of central directory with respect to the 
	      starting disk number */
	if (unzlocal_getLong(fin,&us.offset_central_dir)!=UNZ_OK)
		err=UNZ_ERRNO;

	/* zipfile comment length */
	if (unzlocal_getShort(fin,&us.gi.size_comment)!=UNZ_OK)
		err=UNZ_ERRNO;

	if ((central_pos<us.offset_central_dir+us.size_central_dir) && 
		(err==UNZ_OK))
		err=UNZ_BADZIPFILE;

	if (err!=UNZ_OK)
		return NULL;

	us.file=fin;
	us.byte_before_the_zipfile = central_pos -
		                    (us.offset_central_dir+us.size_central_dir);
	us.central_pos = central_pos;
    us.pfile_in_zip_read = NULL;
	

	s=(unz_s*)ALLOC(sizeof(unz_s));
	*s=us;
	unzGoToFirstFile((unzFile)s);	
	return (unzFile)s;	
}


/****i* januae-zip/unzClose *****************************************************
*
*   NAME
*       unzClose()
*
*   SYNOPSIS
*       int ZEXPORT unzClose(unzFile file);
*
*   FUNCTION
*       Release the archive cursor, its current entry stream if any, and the
*       underlying zfile.
*
*   RESULT
*       UNZ_OK on a non-NULL handle; UNZ_PARAMERROR for NULL.
*
*   NOTES
*       Errors from closing the entry stream or underlying zfile are not
*       propagated.
*
*************************************************************************
*
*/
extern int ZEXPORT unzClose (file)
	unzFile file;
{
	unz_s* s;
	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;

    if (s->pfile_in_zip_read!=NULL)
        unzCloseCurrentFile(file);

	zfile_fclose(s->file);
	TRYFREE(s);
	return UNZ_OK;
}


/****i* januae-zip/unzGetGlobalInfo *********************************************
*
*   NAME
*       unzGetGlobalInfo()
*
*   SYNOPSIS
*       int ZEXPORT unzGetGlobalInfo(unzFile file, unz_global_info
*       *pglobal_info);
*
*   FUNCTION
*       Copy the archive entry count and global comment length to the caller.
*
*   RESULT
*       UNZ_OK or UNZ_PARAMERROR for a NULL archive handle.
*
*   NOTES
*       pglobal_info is dereferenced without a NULL check.
*
*************************************************************************
*
*/
extern int ZEXPORT unzGetGlobalInfo (file,pglobal_info)
	unzFile file;
	unz_global_info *pglobal_info;
{
	unz_s* s;
	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;
	*pglobal_info=s->gi;
	return UNZ_OK;
}


/****i* januae-zip/unzlocal_DosDateToTmuDate ************************************
*
*   NAME
*       unzlocal_DosDateToTmuDate()
*
*   SYNOPSIS
*       static void unzlocal_DosDateToTmuDate(uLong ulDosDate, tm_unz *ptm);
*
*   FUNCTION
*       Decode a ZIP DOS timestamp into date and time fields.
*
*   NOTES
*       The date is interpreted in DOS year/month/day and two-second
*       granularity; the output pointer is not validated.
*
*************************************************************************
*
*/
local void unzlocal_DosDateToTmuDate (ulDosDate, ptm)
    uLong ulDosDate;
    tm_unz* ptm;
{
    uLong uDate;
    uDate = (uLong)(ulDosDate>>16);
    ptm->tm_mday = (uInt)(uDate&0x1f) ;
    ptm->tm_mon =  (uInt)((((uDate)&0x1E0)/0x20)-1) ;
    ptm->tm_year = (uInt)(((uDate&0x0FE00)/0x0200)+1980) ;

    ptm->tm_hour = (uInt) ((ulDosDate &0xF800)/0x800);
    ptm->tm_min =  (uInt) ((ulDosDate&0x7E0)/0x20) ;
    ptm->tm_sec =  (uInt) (2*(ulDosDate&0x1f)) ;
}

/*
  Get Info about the current file in the zipfile, with internal only info
*/
local int unzlocal_GetCurrentFileInfoInternal OF((unzFile file,
                                                  unz_file_info *pfile_info,
                                                  unz_file_info_internal 
                                                  *pfile_info_internal,
                                                  char *szFileName,
												  uLong fileNameBufferSize,
                                                  void *extraField,
												  uLong extraFieldBufferSize,
                                                  char *szComment,
												  uLong commentBufferSize));

/****i* januae-zip/unzlocal_GetCurrentFileInfoInternal **************************
*
*   NAME
*       unzlocal_GetCurrentFileInfoInternal()
*
*   SYNOPSIS
*       static int unzlocal_GetCurrentFileInfoInternal(unzFile file,
*       unz_file_info *pfile_info, unz_file_info_internal
*       *pfile_info_internal, char *szFileName, uLong fileNameBufferSize,
*       void *extraField, uLong extraFieldBufferSize, char *szComment,
*       uLong commentBufferSize);
*
*   FUNCTION
*       Read the current ZIP central-directory header and optionally copy its
*       metadata, filename, extra field and comment.
*
*   RESULT
*       UNZ_OK, UNZ_PARAMERROR, UNZ_BADZIPFILE or UNZ_ERRNO.
*
*   NOTES
*       Optional output buffers are bounded by their supplied lengths. A
*       full-length filename/comment copy is not necessarily NUL-terminated.
*
*************************************************************************
*
*/
local int unzlocal_GetCurrentFileInfoInternal (file,
                                              pfile_info,
                                              pfile_info_internal,
                                              szFileName, fileNameBufferSize,
                                              extraField, extraFieldBufferSize,
                                              szComment,  commentBufferSize)
	unzFile file;
	unz_file_info *pfile_info;
	unz_file_info_internal *pfile_info_internal;
	char *szFileName;
	uLong fileNameBufferSize;
	void *extraField;
	uLong extraFieldBufferSize;
	char *szComment;
	uLong commentBufferSize;
{
	unz_s* s;
	unz_file_info file_info;
	unz_file_info_internal file_info_internal;
	int err=UNZ_OK;
	uLong uMagic;
	long lSeek=0;

	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;
	if (zfile_fseek(s->file,s->pos_in_central_dir+s->byte_before_the_zipfile,SEEK_SET)!=0)
		err=UNZ_ERRNO;


	/* we check the magic */
	if (err==UNZ_OK)
		if (unzlocal_getLong(s->file,&uMagic) != UNZ_OK)
			err=UNZ_ERRNO;
		else if (uMagic!=0x02014b50)
			err=UNZ_BADZIPFILE;

	if (unzlocal_getShort(s->file,&file_info.version) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getShort(s->file,&file_info.version_needed) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getShort(s->file,&file_info.flag) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getShort(s->file,&file_info.compression_method) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getLong(s->file,&file_info.dosDate) != UNZ_OK)
		err=UNZ_ERRNO;

    unzlocal_DosDateToTmuDate(file_info.dosDate,&file_info.tmu_date);

	if (unzlocal_getLong(s->file,&file_info.crc) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getLong(s->file,&file_info.compressed_size) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getLong(s->file,&file_info.uncompressed_size) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getShort(s->file,&file_info.size_filename) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getShort(s->file,&file_info.size_file_extra) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getShort(s->file,&file_info.size_file_comment) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getShort(s->file,&file_info.disk_num_start) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getShort(s->file,&file_info.internal_fa) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getLong(s->file,&file_info.external_fa) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getLong(s->file,&file_info_internal.offset_curfile) != UNZ_OK)
		err=UNZ_ERRNO;

	lSeek+=file_info.size_filename;
	if ((err==UNZ_OK) && (szFileName!=NULL))
	{
		uLong uSizeRead ;
		if (file_info.size_filename<fileNameBufferSize)
		{
			*(szFileName+file_info.size_filename)='\0';
			uSizeRead = file_info.size_filename;
		}
		else
			uSizeRead = fileNameBufferSize;

		if ((file_info.size_filename>0) && (fileNameBufferSize>0))
			if (zfile_fread(szFileName,(uInt)uSizeRead,1,s->file)!=1)
				err=UNZ_ERRNO;
		lSeek -= uSizeRead;
	}

	
	if ((err==UNZ_OK) && (extraField!=NULL))
	{
		uLong uSizeRead ;
		if (file_info.size_file_extra<extraFieldBufferSize)
			uSizeRead = file_info.size_file_extra;
		else
			uSizeRead = extraFieldBufferSize;

		if (lSeek!=0)
			if (zfile_fseek(s->file,lSeek,SEEK_CUR)==0)
				lSeek=0;
			else
				err=UNZ_ERRNO;
		if ((file_info.size_file_extra>0) && (extraFieldBufferSize>0))
			if (zfile_fread(extraField,(uInt)uSizeRead,1,s->file)!=1)
				err=UNZ_ERRNO;
		lSeek += file_info.size_file_extra - uSizeRead;
	}
	else
		lSeek+=file_info.size_file_extra; 

	
	if ((err==UNZ_OK) && (szComment!=NULL))
	{
		uLong uSizeRead ;
		if (file_info.size_file_comment<commentBufferSize)
		{
			*(szComment+file_info.size_file_comment)='\0';
			uSizeRead = file_info.size_file_comment;
		}
		else
			uSizeRead = commentBufferSize;

		if (lSeek!=0)
			if (zfile_fseek(s->file,lSeek,SEEK_CUR)==0)
				lSeek=0;
			else
				err=UNZ_ERRNO;
		if ((file_info.size_file_comment>0) && (commentBufferSize>0))
			if (zfile_fread(szComment,(uInt)uSizeRead,1,s->file)!=1)
				err=UNZ_ERRNO;
		lSeek+=file_info.size_file_comment - uSizeRead;
	}
	else
		lSeek+=file_info.size_file_comment;

	if ((err==UNZ_OK) && (pfile_info!=NULL))
		*pfile_info=file_info;

	if ((err==UNZ_OK) && (pfile_info_internal!=NULL))
		*pfile_info_internal=file_info_internal;

	return err;
}



/****i* januae-zip/unzGetCurrentFileInfo ****************************************
*
*   NAME
*       unzGetCurrentFileInfo()
*
*   SYNOPSIS
*       int ZEXPORT unzGetCurrentFileInfo(unzFile file, unz_file_info
*       *pfile_info, char *szFileName, uLong fileNameBufferSize, void
*       *extraField, uLong extraFieldBufferSize, char *szComment, uLong
*       commentBufferSize);
*
*   FUNCTION
*       Retrieve public metadata and optional central-directory fields for the
*       selected entry.
*
*   RESULT
*       The status returned by the internal central-directory reader.
*
*   NOTES
*       A NULL optional data pointer suppresses the corresponding copy.
*
*************************************************************************
*
*/
extern int ZEXPORT unzGetCurrentFileInfo (file,
                                                  pfile_info,
                                                  szFileName, fileNameBufferSize,
                                                  extraField, extraFieldBufferSize,
                                                  szComment,  commentBufferSize)
	unzFile file;
	unz_file_info *pfile_info;
	char *szFileName;
	uLong fileNameBufferSize;
	void *extraField;
	uLong extraFieldBufferSize;
	char *szComment;
	uLong commentBufferSize;
{
	return unzlocal_GetCurrentFileInfoInternal(file,pfile_info,NULL,
												szFileName,fileNameBufferSize,
												extraField,extraFieldBufferSize,
												szComment,commentBufferSize);
}

/****i* januae-zip/unzGoToFirstFile *********************************************
*
*   NAME
*       unzGoToFirstFile()
*
*   SYNOPSIS
*       int ZEXPORT unzGoToFirstFile(unzFile file);
*
*   FUNCTION
*       Select the first entry in the ZIP central directory and update its
*       metadata.
*
*   RESULT
*       UNZ_OK, UNZ_PARAMERROR or an entry-header read error.
*
*   NOTES
*       Selection does not open the entry data stream.
*
*************************************************************************
*
*/
extern int ZEXPORT unzGoToFirstFile (file)
	unzFile file;
{
	int err=UNZ_OK;
	unz_s* s;
	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;
	s->pos_in_central_dir=s->offset_central_dir;
	s->num_file=0;
	err=unzlocal_GetCurrentFileInfoInternal(file,&s->cur_file_info,
											 &s->cur_file_info_internal,
											 NULL,0,NULL,0,NULL,0);
	s->current_file_ok = (err == UNZ_OK);
	return err;
}


/****i* januae-zip/unzGoToNextFile **********************************************
*
*   NAME
*       unzGoToNextFile()
*
*   SYNOPSIS
*       int ZEXPORT unzGoToNextFile(unzFile file);
*
*   FUNCTION
*       Advance the central-directory cursor to the next entry.
*
*   RESULT
*       UNZ_OK, UNZ_END_OF_LIST_OF_FILE, UNZ_PARAMERROR or a header read
*       error.
*
*   NOTES
*       Requires a current entry and does not open its data stream.
*
*************************************************************************
*
*/
extern int ZEXPORT unzGoToNextFile (file)
	unzFile file;
{
	unz_s* s;	
	int err;

	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;
	if (!s->current_file_ok)
		return UNZ_END_OF_LIST_OF_FILE;
	if (s->num_file+1==s->gi.number_entry)
		return UNZ_END_OF_LIST_OF_FILE;

	s->pos_in_central_dir += SIZECENTRALDIRITEM + s->cur_file_info.size_filename +
			s->cur_file_info.size_file_extra + s->cur_file_info.size_file_comment ;
	s->num_file++;
	err = unzlocal_GetCurrentFileInfoInternal(file,&s->cur_file_info,
											   &s->cur_file_info_internal,
											   NULL,0,NULL,0,NULL,0);
	s->current_file_ok = (err == UNZ_OK);
	return err;
}


/****i* januae-zip/unzLocateFile ************************************************
*
*   NAME
*       unzLocateFile()
*
*   SYNOPSIS
*       int ZEXPORT unzLocateFile(unzFile file, const char *szFileName, int
*       iCaseSensitivity);
*
*   FUNCTION
*       Search directory entries by name with the specified case-sensitivity
*       mode.
*
*   RESULT
*       UNZ_OK if found; UNZ_END_OF_LIST_OF_FILE if absent; otherwise a
*       cursor/header error.
*
*   NOTES
*       Restores the prior current-entry selection when the name is not found.
*
*************************************************************************
*
*/
extern int ZEXPORT unzLocateFile (file, szFileName, iCaseSensitivity)
	unzFile file;
	const char *szFileName;
	int iCaseSensitivity;
{
	unz_s* s;	
	int err;

	
	uLong num_fileSaved;
	uLong pos_in_central_dirSaved;


	if (file==NULL)
		return UNZ_PARAMERROR;

    if (strlen(szFileName)>=UNZ_MAXFILENAMEINZIP)
        return UNZ_PARAMERROR;

	s=(unz_s*)file;
	if (!s->current_file_ok)
		return UNZ_END_OF_LIST_OF_FILE;

	num_fileSaved = s->num_file;
	pos_in_central_dirSaved = s->pos_in_central_dir;

	err = unzGoToFirstFile(file);

	while (err == UNZ_OK)
	{
		char szCurrentFileName[UNZ_MAXFILENAMEINZIP+1];
		unzGetCurrentFileInfo(file,NULL,
								szCurrentFileName,sizeof(szCurrentFileName)-1,
								NULL,0,NULL,0);
		if (unzStringFileNameCompare(szCurrentFileName,
										szFileName,iCaseSensitivity)==0)
			return UNZ_OK;
		err = unzGoToNextFile(file);
	}

	s->num_file = num_fileSaved ;
	s->pos_in_central_dir = pos_in_central_dirSaved ;
	return err;
}


/****i* januae-zip/unzlocal_CheckCurrentFileCoherencyHeader *********************
*
*   NAME
*       unzlocal_CheckCurrentFileCoherencyHeader()
*
*   SYNOPSIS
*       static int unzlocal_CheckCurrentFileCoherencyHeader(unz_s *s, uInt
*       *piSizeVar, uLong *poffset_local_extrafield, uInt
*       *psize_local_extrafield);
*
*   FUNCTION
*       Validate the local ZIP entry header against the selected
*       central-directory entry and determine its data and extra-field
*       offsets.
*
*   RESULT
*       UNZ_OK, UNZ_BADZIPFILE or UNZ_ERRNO.
*
*   NOTES
*       Checks supported compression flags, method and sizes before the entry
*       data stream is opened.
*
*************************************************************************
*
*/
local int unzlocal_CheckCurrentFileCoherencyHeader (s,piSizeVar,
													poffset_local_extrafield,
													psize_local_extrafield)
	unz_s* s;
	uInt* piSizeVar;
	uLong *poffset_local_extrafield;
	uInt  *psize_local_extrafield;
{
	uLong uMagic,uData,uFlags;
	uLong size_filename;
	uLong size_extra_field;
	int err=UNZ_OK;

	*piSizeVar = 0;
	*poffset_local_extrafield = 0;
	*psize_local_extrafield = 0;

	if (zfile_fseek(s->file,s->cur_file_info_internal.offset_curfile +
								s->byte_before_the_zipfile,SEEK_SET)!=0)
		return UNZ_ERRNO;


	if (err==UNZ_OK)
		if (unzlocal_getLong(s->file,&uMagic) != UNZ_OK)
			err=UNZ_ERRNO;
		else if (uMagic!=0x04034b50)
			err=UNZ_BADZIPFILE;

	if (unzlocal_getShort(s->file,&uData) != UNZ_OK)
		err=UNZ_ERRNO;
/*
	else if ((err==UNZ_OK) && (uData!=s->cur_file_info.wVersion))
		err=UNZ_BADZIPFILE;
*/
	if (unzlocal_getShort(s->file,&uFlags) != UNZ_OK)
		err=UNZ_ERRNO;

	if (unzlocal_getShort(s->file,&uData) != UNZ_OK)
		err=UNZ_ERRNO;
	else if ((err==UNZ_OK) && (uData!=s->cur_file_info.compression_method))
		err=UNZ_BADZIPFILE;

    if ((err==UNZ_OK) && (s->cur_file_info.compression_method!=0) &&
                         (s->cur_file_info.compression_method!=Z_DEFLATED))
        err=UNZ_BADZIPFILE;

	if (unzlocal_getLong(s->file,&uData) != UNZ_OK) /* date/time */
		err=UNZ_ERRNO;

	if (unzlocal_getLong(s->file,&uData) != UNZ_OK) /* crc */
		err=UNZ_ERRNO;
	else if ((err==UNZ_OK) && (uData!=s->cur_file_info.crc) &&
		                      ((uFlags & 8)==0))
		err=UNZ_BADZIPFILE;

	if (unzlocal_getLong(s->file,&uData) != UNZ_OK) /* size compr */
		err=UNZ_ERRNO;
/*	else if ((err==UNZ_OK) && (uData!=s->cur_file_info.compressed_size) &&
							  ((uFlags & 8)==0))
		err=UNZ_BADZIPFILE;
*/
	if (unzlocal_getLong(s->file,&uData) != UNZ_OK) /* size uncompr */
		err=UNZ_ERRNO;
/*	else if ((err==UNZ_OK) && (uData!=s->cur_file_info.uncompressed_size) && 
							  ((uFlags & 8)==0))
		err=UNZ_BADZIPFILE;
*/

	if (unzlocal_getShort(s->file,&size_filename) != UNZ_OK)
		err=UNZ_ERRNO;
	else if ((err==UNZ_OK) && (size_filename!=s->cur_file_info.size_filename))
		err=UNZ_BADZIPFILE;

	*piSizeVar += (uInt)size_filename;

	if (unzlocal_getShort(s->file,&size_extra_field) != UNZ_OK)
		err=UNZ_ERRNO;
	*poffset_local_extrafield= s->cur_file_info_internal.offset_curfile +
									SIZEZIPLOCALHEADER + size_filename;
	*psize_local_extrafield = (uInt)size_extra_field;

	*piSizeVar += (uInt)size_extra_field;

	return err;
}
												
/****i* januae-zip/unzOpenCurrentFile *******************************************
*
*   NAME
*       unzOpenCurrentFile()
*
*   SYNOPSIS
*       int ZEXPORT unzOpenCurrentFile(unzFile file);
*
*   FUNCTION
*       Open the selected entry for stored or raw-DEFLATE data access,
*       allocating decompression state and a read buffer.
*
*   RESULT
*       UNZ_OK, UNZ_PARAMERROR, UNZ_BADZIPFILE or UNZ_INTERNALERROR.
*
*   NOTES
*       With GTKMUI defined, unconditionally returns UNZ_PARAMERROR. The
*       normal path replaces any already-open entry stream.
*
*************************************************************************
*
*/
extern int ZEXPORT unzOpenCurrentFile (file)
	unzFile file;
{
#if defined GTKMUI
    return UNZ_PARAMERROR;
#else
	int err=UNZ_OK;
	int Store;
	uInt iSizeVar;
	unz_s* s;
	file_in_zip_read_info_s* pfile_in_zip_read_info;
	uLong offset_local_extrafield;  /* offset of the local extra field */
	uInt  size_local_extrafield;    /* size of the local extra field */

	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;
	if (!s->current_file_ok)
		return UNZ_PARAMERROR;

    if (s->pfile_in_zip_read != NULL)
        unzCloseCurrentFile(file);

	if (unzlocal_CheckCurrentFileCoherencyHeader(s,&iSizeVar,
				&offset_local_extrafield,&size_local_extrafield)!=UNZ_OK)
		return UNZ_BADZIPFILE;

	pfile_in_zip_read_info = (file_in_zip_read_info_s*)
									    ALLOC(sizeof(file_in_zip_read_info_s));
	if (pfile_in_zip_read_info==NULL)
		return UNZ_INTERNALERROR;

	pfile_in_zip_read_info->read_buffer=(char*)ALLOC(UNZ_BUFSIZE);
	pfile_in_zip_read_info->offset_local_extrafield = offset_local_extrafield;
	pfile_in_zip_read_info->size_local_extrafield = size_local_extrafield;
	pfile_in_zip_read_info->pos_local_extrafield=0;

	if (pfile_in_zip_read_info->read_buffer==NULL)
	{
		TRYFREE(pfile_in_zip_read_info);
		return UNZ_INTERNALERROR;
	}

	pfile_in_zip_read_info->stream_initialised=0;
	
	if ((s->cur_file_info.compression_method!=0) &&
        (s->cur_file_info.compression_method!=Z_DEFLATED))
		err=UNZ_BADZIPFILE;
	Store = s->cur_file_info.compression_method==0;

	pfile_in_zip_read_info->crc32_wait=s->cur_file_info.crc;
	pfile_in_zip_read_info->crc32=0;
	pfile_in_zip_read_info->compression_method =
            s->cur_file_info.compression_method;
	pfile_in_zip_read_info->file=s->file;
	pfile_in_zip_read_info->byte_before_the_zipfile=s->byte_before_the_zipfile;

    pfile_in_zip_read_info->stream.total_out = 0;

	if (!Store)
	{
	  pfile_in_zip_read_info->stream.zalloc = (alloc_func)0;
	  pfile_in_zip_read_info->stream.zfree = (free_func)0;
	  pfile_in_zip_read_info->stream.opaque = (voidpf)0; 
      
	  err=inflateInit2(&pfile_in_zip_read_info->stream, -MAX_WBITS);
	  if (err == Z_OK)
	    pfile_in_zip_read_info->stream_initialised=1;
        /* windowBits is passed < 0 to tell that there is no zlib header.
         * Note that in this case inflate *requires* an extra "dummy" byte
         * after the compressed stream in order to complete decompression and
         * return Z_STREAM_END. 
         * In unzip, i don't wait absolutely Z_STREAM_END because I known the 
         * size of both compressed and uncompressed data
         */
	}
	pfile_in_zip_read_info->rest_read_compressed = 
            s->cur_file_info.compressed_size ;
	pfile_in_zip_read_info->rest_read_uncompressed = 
            s->cur_file_info.uncompressed_size ;

	
	pfile_in_zip_read_info->pos_in_zipfile = 
            s->cur_file_info_internal.offset_curfile + SIZEZIPLOCALHEADER + 
			  iSizeVar;
	
	pfile_in_zip_read_info->stream.avail_in = (uInt)0;


	s->pfile_in_zip_read = pfile_in_zip_read_info;
    return UNZ_OK;
#endif
}


/****i* januae-zip/unzReadCurrentFile *******************************************
*
*   NAME
*       unzReadCurrentFile()
*
*   SYNOPSIS
*       int ZEXPORT unzReadCurrentFile(unzFile file, voidp buf, unsigned len);
*
*   FUNCTION
*       Copy up to len uncompressed entry bytes into buf, using stored-data
*       reads or raw DEFLATE inflation.
*
*   RESULT
*       Positive byte count, zero at end of entry, or a negative ZIP/zlib
*       error code.
*
*   NOTES
*       With GTKMUI defined, returns UNZ_PARAMERROR. The archive cursor must
*       have an opened entry.
*
*************************************************************************
*
*/
extern int ZEXPORT unzReadCurrentFile  (file, buf, len)
	unzFile file;
	voidp buf;
	unsigned len;
{
#if defined GTKMUI
  return UNZ_PARAMERROR;
#else
	int err=UNZ_OK;
	uInt iRead = 0;
	unz_s* s;
	file_in_zip_read_info_s* pfile_in_zip_read_info;
	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;
    pfile_in_zip_read_info=s->pfile_in_zip_read;

	if (pfile_in_zip_read_info==NULL)
		return UNZ_PARAMERROR;


	if ((pfile_in_zip_read_info->read_buffer == NULL))
		return UNZ_END_OF_LIST_OF_FILE;
	if (len==0)
		return 0;

	pfile_in_zip_read_info->stream.next_out = (Bytef*)buf;

	pfile_in_zip_read_info->stream.avail_out = (uInt)len;
	
	if (len>pfile_in_zip_read_info->rest_read_uncompressed)
		pfile_in_zip_read_info->stream.avail_out = 
		  (uInt)pfile_in_zip_read_info->rest_read_uncompressed;

	while (pfile_in_zip_read_info->stream.avail_out>0)
	{
		if ((pfile_in_zip_read_info->stream.avail_in==0) &&
            (pfile_in_zip_read_info->rest_read_compressed>0))
		{
			uInt uReadThis = UNZ_BUFSIZE;
			if (pfile_in_zip_read_info->rest_read_compressed<uReadThis)
				uReadThis = (uInt)pfile_in_zip_read_info->rest_read_compressed;
			if (uReadThis == 0)
				return UNZ_EOF;
			if (zfile_fseek(pfile_in_zip_read_info->file,
                      pfile_in_zip_read_info->pos_in_zipfile + 
                         pfile_in_zip_read_info->byte_before_the_zipfile,SEEK_SET)!=0)
				return UNZ_ERRNO;
			if (zfile_fread(pfile_in_zip_read_info->read_buffer,uReadThis,1,
                         pfile_in_zip_read_info->file)!=1)
				return UNZ_ERRNO;
			pfile_in_zip_read_info->pos_in_zipfile += uReadThis;

			pfile_in_zip_read_info->rest_read_compressed-=uReadThis;
			
			pfile_in_zip_read_info->stream.next_in = 
                (Bytef*)pfile_in_zip_read_info->read_buffer;
			pfile_in_zip_read_info->stream.avail_in = (uInt)uReadThis;
		}

		if (pfile_in_zip_read_info->compression_method==0)
		{
			uInt uDoCopy,i ;
			if (pfile_in_zip_read_info->stream.avail_out < 
                            pfile_in_zip_read_info->stream.avail_in)
				uDoCopy = pfile_in_zip_read_info->stream.avail_out ;
			else
				uDoCopy = pfile_in_zip_read_info->stream.avail_in ;
				
			for (i=0;i<uDoCopy;i++)
				*(pfile_in_zip_read_info->stream.next_out+i) =
                        *(pfile_in_zip_read_info->stream.next_in+i);
					
			pfile_in_zip_read_info->crc32 = crc32(pfile_in_zip_read_info->crc32,
								pfile_in_zip_read_info->stream.next_out,
								uDoCopy);
			pfile_in_zip_read_info->rest_read_uncompressed-=uDoCopy;
			pfile_in_zip_read_info->stream.avail_in -= uDoCopy;
			pfile_in_zip_read_info->stream.avail_out -= uDoCopy;
			pfile_in_zip_read_info->stream.next_out += uDoCopy;
			pfile_in_zip_read_info->stream.next_in += uDoCopy;
            pfile_in_zip_read_info->stream.total_out += uDoCopy;
			iRead += uDoCopy;
		}
		else
		{
			uLong uTotalOutBefore,uTotalOutAfter;
			const Bytef *bufBefore;
			uLong uOutThis;
			int flush=Z_SYNC_FLUSH;

			uTotalOutBefore = pfile_in_zip_read_info->stream.total_out;
			bufBefore = pfile_in_zip_read_info->stream.next_out;

			/*
			if ((pfile_in_zip_read_info->rest_read_uncompressed ==
			         pfile_in_zip_read_info->stream.avail_out) &&
				(pfile_in_zip_read_info->rest_read_compressed == 0))
				flush = Z_FINISH;
			*/
			err=inflate(&pfile_in_zip_read_info->stream,flush);

			uTotalOutAfter = pfile_in_zip_read_info->stream.total_out;
			uOutThis = uTotalOutAfter-uTotalOutBefore;
			
			pfile_in_zip_read_info->crc32 = 
                crc32(pfile_in_zip_read_info->crc32,bufBefore,
                        (uInt)(uOutThis));

			pfile_in_zip_read_info->rest_read_uncompressed -=
                uOutThis;

			iRead += (uInt)(uTotalOutAfter - uTotalOutBefore);
            
			if (err==Z_STREAM_END)
				return (iRead==0) ? UNZ_EOF : iRead;
			if (err!=Z_OK) 
				break;
		}
	}

	if (err==Z_OK)
		return iRead;
	return err;
#endif
}


/****i* januae-zip/unztell ******************************************************
*
*   NAME
*       unztell()
*
*   SYNOPSIS
*       z_off_t ZEXPORT unztell(unzFile file);
*
*   FUNCTION
*       Report the total uncompressed bytes already delivered from the current
*       entry.
*
*   RESULT
*       Current uncompressed offset; UNZ_PARAMERROR for an absent archive or
*       unopened entry.
*
*   NOTES
*       The error code is returned through z_off_t.
*
*************************************************************************
*
*/
extern z_off_t ZEXPORT unztell (file)
	unzFile file;
{
	unz_s* s;
	file_in_zip_read_info_s* pfile_in_zip_read_info;
	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;
    pfile_in_zip_read_info=s->pfile_in_zip_read;

	if (pfile_in_zip_read_info==NULL)
		return UNZ_PARAMERROR;

	return (z_off_t)pfile_in_zip_read_info->stream.total_out;
}


/****i* januae-zip/unzeof *******************************************************
*
*   NAME
*       unzeof()
*
*   SYNOPSIS
*       int ZEXPORT unzeof(unzFile file);
*
*   FUNCTION
*       Test whether the current entry has no uncompressed bytes remaining.
*
*   RESULT
*       1 at end, 0 otherwise; UNZ_PARAMERROR for a missing handle or entry
*       stream.
*
*   NOTES
*       The test uses the stored remaining-byte count, not the underlying
*       archive EOF.
*
*************************************************************************
*
*/
extern int ZEXPORT unzeof (file)
	unzFile file;
{
	unz_s* s;
	file_in_zip_read_info_s* pfile_in_zip_read_info;
	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;
    pfile_in_zip_read_info=s->pfile_in_zip_read;

	if (pfile_in_zip_read_info==NULL)
		return UNZ_PARAMERROR;
	
	if (pfile_in_zip_read_info->rest_read_uncompressed == 0)
		return 1;
	else
		return 0;
}



/****i* januae-zip/unzGetLocalExtrafield ****************************************
*
*   NAME
*       unzGetLocalExtrafield()
*
*   SYNOPSIS
*       int ZEXPORT unzGetLocalExtrafield(unzFile file, voidp buf, unsigned
*       len);
*
*   FUNCTION
*       Inspect the local-header extra field of the current entry, optionally
*       copying bytes to buf.
*
*   RESULT
*       Remaining extra-field length when buf is NULL; bytes requested or a
*       negative error otherwise.
*
*   NOTES
*       len does not bound the underlying read: buf must hold the entire
*       remaining local extra field. The stored extra-field position is not
*       advanced.
*
*************************************************************************
*
*/
extern int ZEXPORT unzGetLocalExtrafield (file,buf,len)
	unzFile file;
	voidp buf;
	unsigned len;
{
	unz_s* s;
	file_in_zip_read_info_s* pfile_in_zip_read_info;
	uInt read_now;
	uLong size_to_read;

	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;
    pfile_in_zip_read_info=s->pfile_in_zip_read;

	if (pfile_in_zip_read_info==NULL)
		return UNZ_PARAMERROR;

	size_to_read = (pfile_in_zip_read_info->size_local_extrafield - 
				pfile_in_zip_read_info->pos_local_extrafield);

	if (buf==NULL)
		return (int)size_to_read;
	
	if (len>size_to_read)
		read_now = (uInt)size_to_read;
	else
		read_now = (uInt)len ;

	if (read_now==0)
		return 0;
	
	if (zfile_fseek(pfile_in_zip_read_info->file,
              pfile_in_zip_read_info->offset_local_extrafield + 
			  pfile_in_zip_read_info->pos_local_extrafield,SEEK_SET)!=0)
		return UNZ_ERRNO;

	if (zfile_fread(buf,(uInt)size_to_read,1,pfile_in_zip_read_info->file)!=1)
		return UNZ_ERRNO;

	return (int)read_now;
}

/****i* januae-zip/unzCloseCurrentFile ******************************************
*
*   NAME
*       unzCloseCurrentFile()
*
*   SYNOPSIS
*       int ZEXPORT unzCloseCurrentFile(unzFile file);
*
*   FUNCTION
*       Close the current entry stream, release its inflater and read buffer,
*       and verify CRC if completely consumed.
*
*   RESULT
*       UNZ_OK or UNZ_CRCERROR after a complete read; UNZ_PARAMERROR for
*       invalid state.
*
*   NOTES
*       With GTKMUI defined, returns UNZ_PARAMERROR. CRC is not checked after
*       a partial read.
*
*************************************************************************
*
*/
extern int ZEXPORT unzCloseCurrentFile (file)
	unzFile file;
{
#if defined GTKMUI
    return UNZ_PARAMERROR;
#else
	int err=UNZ_OK;

	unz_s* s;
	file_in_zip_read_info_s* pfile_in_zip_read_info;
	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;
    pfile_in_zip_read_info=s->pfile_in_zip_read;

	if (pfile_in_zip_read_info==NULL)
		return UNZ_PARAMERROR;


	if (pfile_in_zip_read_info->rest_read_uncompressed == 0)
	{
		if (pfile_in_zip_read_info->crc32 != pfile_in_zip_read_info->crc32_wait)
			err=UNZ_CRCERROR;
	}


	TRYFREE(pfile_in_zip_read_info->read_buffer);
	pfile_in_zip_read_info->read_buffer = NULL;
	if (pfile_in_zip_read_info->stream_initialised)
		inflateEnd(&pfile_in_zip_read_info->stream);

	pfile_in_zip_read_info->stream_initialised = 0;
	TRYFREE(pfile_in_zip_read_info);

    s->pfile_in_zip_read=NULL;

	return err;
#endif
}


/****i* januae-zip/unzGetGlobalComment ******************************************
*
*   NAME
*       unzGetGlobalComment()
*
*   SYNOPSIS
*       int ZEXPORT unzGetGlobalComment(unzFile file, char *szComment,
*       uLong uSizeBuf);
*
*   FUNCTION
*       Read up to uSizeBuf bytes of the ZIP archive global comment.
*
*   RESULT
*       Bytes read; UNZ_PARAMERROR for a NULL handle or UNZ_ERRNO on I/O
*       failure.
*
*   NOTES
*       szComment must be writable when a nonzero length is requested. A NUL
*       terminator is added only when the buffer exceeds the comment length.
*
*************************************************************************
*
*/
extern int ZEXPORT unzGetGlobalComment (file, szComment, uSizeBuf)
	unzFile file;
	char *szComment;
	uLong uSizeBuf;
{
	int err=UNZ_OK;
	unz_s* s;
	uLong uReadThis ;
	if (file==NULL)
		return UNZ_PARAMERROR;
	s=(unz_s*)file;

	uReadThis = uSizeBuf;
	if (uReadThis>s->gi.size_comment)
		uReadThis = s->gi.size_comment;

	if (zfile_fseek(s->file,s->central_pos+22,SEEK_SET)!=0)
		return UNZ_ERRNO;

	if (uReadThis>0)
    {
      *szComment='\0';
	  if (zfile_fread(szComment,(uInt)uReadThis,1,s->file)!=1)
		return UNZ_ERRNO;
    }

	if ((szComment != NULL) && (uSizeBuf > s->gi.size_comment))
		*(szComment+s->gi.size_comment)='\0';
	return (int)uReadThis;
}
