
/*
 *     xDMS  v1.3  -  Portable DMS archive unpacker  -  Public Domain
 *     Written by     Andre Rodrigues de la Rocha  <adlroc@usa.net>
 *
 *     Makes decoding table for Heavy LZH decompression
 *     From  UNIX LHA made by Masaru Oki
 *
 */


#include "cdata.h"
#include "maketbl.h"


static SHORT c;
static USHORT n, tblsiz, len, depth, maxdepth, avail;
static USHORT codeword, bit, *tbl, TabErr;
static UCHAR *blen;


static USHORT mktbl(void);



/****i* januae-dms/make_table **********************************************
*
*   NAME
*       make_table()
*
*   SYNOPSIS
*       USHORT make_table(USHORT nchar, UCHAR bitlen[],USHORT tablebits,
*       USHORT table[]);
*
*   FUNCTION
*       Build the primary lookup table and overflow tree for a canonical
*       Huffman code.
*
*   INPUTS
*       nchar - Number of symbols.
*       bitlen - Code length of each symbol.
*       tablebits - Number of bits in the primary lookup.
*       table - Destination lookup table.
*
*   RESULT
*       Zero on success; nonzero (1 through 5) for a malformed or
*       unrepresentable tree.
*
*   NOTES
*       Uses shared tree arrays and file-static builder state; a call
*       overwrites the preceding table-building state.
*
****************************************************************************
*
*/
USHORT make_table(USHORT nchar, UCHAR bitlen[],USHORT tablebits, USHORT table[]){
	n = avail = nchar;
	blen = bitlen;
	tbl = table;
	tblsiz = (USHORT) (1U << tablebits);
	bit = (USHORT) (tblsiz / 2);
	maxdepth = (USHORT)(tablebits + 1);
	depth = len = 1;
	c = -1;
	codeword = 0;
	TabErr = 0;
	mktbl();	/* left subtree */
	if (TabErr) return TabErr;
	mktbl();	/* right subtree */
	if (TabErr) return TabErr;
	if (codeword != tblsiz) return 5;
	return 0;
}



/****i* januae-dms/mktbl ***************************************************
*
*   NAME
*       mktbl()
*
*   SYNOPSIS
*       static USHORT mktbl(void);
*
*   FUNCTION
*       Build one subtree or lookup entry while advancing the shared Huffman
*       table builder.
*
*   RESULT
*       Leaf symbol or internal node index; zero when construction is
*       stopped by an error.
*
*   NOTES
*       Called recursively by make_table(); errors are recorded in the
*       file-static TabErr state.
*
****************************************************************************
*
*/
static USHORT mktbl(void){
	USHORT i=0;

	if (TabErr) return 0;

	if (len == depth) {
		while (++c < n)
			if (blen[c] == len) {
				i = codeword;
				codeword += bit;
				if (codeword > tblsiz) {
					TabErr=1;
					return 0;
				}
				while (i < codeword) tbl[i++] = (USHORT)c;
				return (USHORT)c;
			}
		c = -1;
		len++;
		bit >>= 1;
	}
	depth++;
	if (depth < maxdepth) {
		mktbl();
		mktbl();
	} else if (depth > 32) {
		TabErr = 2;
		return 0;
	} else {
		if ((i = avail++) >= 2 * n - 1) {
			TabErr = 3;
			return 0;
		}
		left[i] = mktbl();
		right[i] = mktbl();
		if (codeword >= tblsiz) {
			TabErr = 4;
			return 0;
		}
		if (depth == maxdepth) tbl[codeword++] = i;
	}
	depth--;
	return i;
}


