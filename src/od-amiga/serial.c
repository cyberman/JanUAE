/************************************************************************ 
 *
 * Copyright 2017 Oliver Brunner - aros<at>oliver-brunner.de
 *
 * This file is part of Janus-UAE.
 *
 * Janus-UAE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Janus-UAE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Janus-UAE. If not, see <http://www.gnu.org/licenses/>.
 *
 ************************************************************************
 *
 * serial port emulation
 *
 * just a quick hack, sorry
 *
 * enable serial output to file with "serial_filename=" in the used uaerc,
 * serial_filename defaults to "NIL:" otherwise and disables output
 * 
 ************************************************************************
 *
 * $Id$
 *
 ************************************************************************/

#include "j.h"
#include "od-amiga/serial.h"

//#define DebOut(...) do { kprintf("%s:%d  %s(): ",__FILE__,__LINE__,__func__);kprintf(__VA_ARGS__); } while(0)
#define DebOut(...) 

static BPTR serial_file;

/****i* januae-serial/checkserwrite ****************************************
*
*   NAME
*       checkserwrite() -- Report serial output availability.
*
*   SYNOPSIS
*       int checkserwrite(void);
*
*   FUNCTION
*       Check whether the serial output file handle is open.
*
*   INPUTS
*       None.
*
*   RESULT
*       1 when the handle is present; otherwise 0.
*
****************************************************************************
*
*/

int checkserwrite(void) {
  if(serial_file) {
    //DebOut("check ok\n");
    return 1;
  }
  DebOut("check failed\n");
  return 0;
}

/****i* januae-serial/writeser *********************************************
*
*   NAME
*       writeser() -- Write a serial value to the output file.
*
*   SYNOPSIS
*       void writeser(uae_u16 data);
*
*   FUNCTION
*       Write a character to the configured serial output file and flush it
*       immediately; do nothing when no file is open.
*
*   INPUTS
*       data - Serial value passed to FPutC.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void writeser(uae_u16 data) {
  DebOut("entered(data: %d)\n", data);
  if(serial_file) {
    FPutC(serial_file, data);
    Flush(serial_file);
  }
}

/****i* januae-serial/readseravail *****************************************
*
*   NAME
*       readseravail() -- Report serial input availability.
*
*   SYNOPSIS
*       int readseravail(void);
*
*   FUNCTION
*       Return the fixed no-input indication without checking an input
*       device.
*
*   INPUTS
*       None.
*
*   RESULT
*       0.
*
****************************************************************************
*
*/

int readseravail(void) {
  DebOut("entered\n");
  return 0;
}

/****i* januae-serial/readser **********************************************
*
*   NAME
*       readser() -- Read a serial input value.
*
*   SYNOPSIS
*       int readser(int *data);
*
*   FUNCTION
*       Return the fixed no-input indication without modifying the
*       destination.
*
*   INPUTS
*       data - Destination pointer; not written.
*
*   RESULT
*       0.
*
****************************************************************************
*
*/

int readser(int *data) {
  DebOut("entered\n");
  return 0;
}


/****i* januae-serial/setbaud **********************************************
*
*   NAME
*       setbaud() -- Request a serial baud-rate change.
*
*   SYNOPSIS
*       void setbaud(int rate);
*
*   FUNCTION
*       Return without changing transmission speed.
*
*   INPUTS
*       rate - Requested baud rate; ignored.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void setbaud(int rate) {
  DebOut("new rate: %d\n", rate);
}

/****i* januae-serial/setserstat *******************************************
*
*   NAME
*       setserstat() -- Request serial status changes.
*
*   SYNOPSIS
*       void setserstat(int state, int foo);
*
*   FUNCTION
*       Return without changing serial status.
*
*   INPUTS
*       state - Requested status; ignored. foo - Additional argument;
*       ignored.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void setserstat(int state, int foo) {
  DebOut("entered\n");
}

/****i* januae-serial/openser **********************************************
*
*   NAME
*       openser() -- Open the file-backed serial output.
*
*   SYNOPSIS
*       int openser(char *name);
*
*   FUNCTION
*       Close any previous serial file, open the named path for
*       existing-file access or create it, and seek to its end before
*       writing.
*
*   INPUTS
*       name - DOS path of the serial output file.
*
*   RESULT
*       1 when a file handle is open; otherwise 0.
*
*   NOTES
*       The target uses an output file rather than a hardware serial device.
*
****************************************************************************
*
*/

int openser(char *name) {

  DebOut("using serial output file: %s\n", name);
  write_log ("Serial output file: \"%s\"\n", name);
  
  if(serial_file) {
    closeser();
  }

  serial_file=Open(name, MODE_OLDFILE);
  if(serial_file == BNULL) {
    serial_file=Open(name, MODE_NEWFILE);
  }

  if(serial_file != BNULL) {
    /* append */
    Seek(serial_file, 0, OFFSET_END);
    DebOut("Serial file %s opened\n", name);
    return 1;
  }

  DebOut("ERROR: could not open %s\n", name);
  return 0;
}

/****i* januae-serial/closeser *********************************************
*
*   NAME
*       closeser() -- Close the serial output file.
*
*   SYNOPSIS
*       void closeser(void);
*
*   FUNCTION
*       Append two newline characters, flush and close the serial file when
*       open, then clear its handle.
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

void closeser(void) {
  DebOut("entered\n");
  if(serial_file) {
    FPutC(serial_file, '\n');
    FPutC(serial_file, '\n');
    Flush(serial_file);
    Close(serial_file);
    serial_file=NULL;
    DebOut("serial_file closed\n");
  }
}

/****i* januae-serial/getserstat *******************************************
*
*   NAME
*       getserstat() -- Read serial device status.
*
*   SYNOPSIS
*       void getserstat(int *status);
*
*   FUNCTION
*       Return without writing a status value.
*
*   INPUTS
*       status - Destination pointer; not written.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void getserstat(int *status) {
  DebOut("entered\n");
}

/****i* januae-serial/serialuartbreak **************************************
*
*   NAME
*       serialuartbreak() -- Request a serial break condition.
*
*   SYNOPSIS
*       void serialuartbreak(int v);
*
*   FUNCTION
*       Return without issuing a serial break.
*
*   INPUTS
*       v - Break request; ignored.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void serialuartbreak(int v) {
  DebOut("entered\n");
}





