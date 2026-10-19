
/*
 *     xDMS  v1.3  -  Portable DMS archive unpacker  -  Public Domain
 *     Written by     Andre Rodrigues de la Rocha  <adlroc@usa.net>
 *
 *     Decruncher reinitialization
 *
 */

#include <string.h>

#include "cdata.h"
#include "u_init.h"
#include "u_quick.h"
#include "u_medium.h"
#include "u_deep.h"
#include "u_heavy.h"


/****i* januae-dms/Init_Decrunchers ****************************************
*
*   NAME
*       Init_Decrunchers()
*
*   SYNOPSIS
*       void Init_Decrunchers(void);
*
*   FUNCTION
*       Reset Quick, Medium, Deep and Heavy dictionary positions and clear
*       the shared DMS text dictionary.
*
*   NOTES
*       Marks the Deep Huffman tables for reinitialization on the next Deep
*       decode. Requires the shared text buffer to have been allocated.
*
****************************************************************************
*
*/
void Init_Decrunchers(void){
	quick_text_loc = 251;
	medium_text_loc = 0x3fbe;
	heavy_text_loc = 0;
	deep_text_loc = 0x3fc4;
	init_deep_tabs = 1;
	memset(text,0,0x3fc8);
}

