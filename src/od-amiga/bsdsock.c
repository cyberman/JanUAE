/************************************************************************ 
 *
 * bsdsocket.library emulation - AROS-dependent part
 *
 * Copyright 2009 Oliver Brunner - aros<at>oliver-brunner.de
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
 * $Id$
 *
 ************************************************************************/

//#define BSDSOCKET 1
#ifdef BSDSOCKET

#include <proto/bsdsocket.h>
#include <proto/exec.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>

#include "sysconfig.h"
#include "sysdeps.h"

#include <stddef.h>
#include <process.h>
#include <aros/debug.h>

#include <sys/ioctl.h>

#include "options.h"
#include "include/memory.h"
#include "custom.h"
#include "events.h"
#include "newcpu.h" 
#include "autoconf.h"
#include "traps.h"
#include "td-amigaos/thread.h"
#include "bsdsocket.h"

#include "threaddep/thread.h"
#include "native2amiga.h"

//#define BSDLOG_ENABLED 1
#if BSDLOG_ENABLED
#define BSDLOG(...)   do { kprintf("[%lx]%s:%d %s(): ",FindTask(NULL),__FILE__,__LINE__,__func__);kprintf(__VA_ARGS__); } while(0)
#else
#define BSDLOG(...)     do { ; } while(0)
#endif


#define WAITSIGNAL waitsig(context, sb)

#define SETSIGNAL addtosigqueue(sb,0)
#define CANCELSIGNAL cancelsig(sb)


#define SF_RAW_UDP 0x10000000
#define SF_RAW_RAW 0x20000000
#define SF_RAW_RUDP 0x08000000
#define SF_RAW_RICMP 0x04000000

static int read_blocking(struct socketbase *sb, int s);
static void set_blocking(struct socketbase *sb, int s, int value);
 


/*
 * copysockaddr_a2n
 *
 * Copy a sockaddr object from amiga space to native space
 *
 * (taken from bsdsocket-posix-new.c)
 */
/****i* januae-bsdsock/copysockaddr_a2n ************************************
*
*   NAME
*       copysockaddr_a2n() -- Decode a guest IPv4 address.
*
*   SYNOPSIS
*       static int copysockaddr_a2n (struct sockaddr_in *addr, uae_u32
*       a_addr, unsigned int len);
*
*   FUNCTION
*       Copy the guest family, port and IPv4 address fields into a host
*       sockaddr_in.
*
*   INPUTS
*       addr - Destination host structure. a_addr - Guest address. len -
*       Guest structure length.
*
*   RESULT
*       Zero for an accepted length; one for an invalid length.
*
*   NOTES
*       A zero guest address is accepted without copying fields.
*
****************************************************************************
*
*/

static int copysockaddr_a2n (struct sockaddr_in *addr, uae_u32 a_addr, unsigned int len) {

  if ((len > sizeof (struct sockaddr_in)) || (len < 8)) {
    return 1;
  }

  if (a_addr == 0) {
    return 0;
  }

  addr->sin_family      = get_byte (a_addr + 1);
  addr->sin_port        = htons (get_word (a_addr + 2));
  addr->sin_addr.s_addr = htonl (get_long (a_addr + 4));
  addr->sin_len         = sizeof(struct sockaddr_in); 

  if (len > 8) {
    memcpy (&addr->sin_zero, get_real_address (a_addr + 8), len - 8);   /* Pointless? */
  }

  return 0;
}


/*
 * Copy a sockaddr object from native space to amiga space
 */
/****i* januae-bsdsock/copysockaddr_n2a ************************************
*
*   NAME
*       copysockaddr_n2a() -- Encode an IPv4 address for the guest.
*
*   SYNOPSIS
*       static int copysockaddr_n2a (uae_u32 a_addr, const struct
*       sockaddr_in *addr, unsigned int len);
*
*   FUNCTION
*       Write the host IPv4 address fields into guest sockaddr storage.
*
*   INPUTS
*       a_addr - Guest destination. addr - Host address. len - Available
*       guest storage.
*
*   RESULT
*       Zero for an accepted length; one when fewer than eight bytes are
*       provided.
*
****************************************************************************
*
*/

static int copysockaddr_n2a (uae_u32 a_addr, const struct sockaddr_in *addr, unsigned int len) {

  if (len < 8) {
    return 1;
  }

  if (a_addr == 0) {
    return 0;
  }

  put_byte (a_addr, 0);                       /* Anyone use this field? */
  put_byte (a_addr + 1, addr->sin_family);
  put_word (a_addr + 2, ntohs (addr->sin_port));
  put_long (a_addr + 4, ntohl (addr->sin_addr.s_addr));
  
  if (len > 8) {
    memset (get_real_address (a_addr + 8), 0, len - 8);
  }

  return 0;
}

/****i* januae-bsdsock/get_errno *******************************************
*
*   NAME
*       get_errno() -- Read socket library error state.
*
*   SYNOPSIS
*       static LONG get_errno(struct socketbase *sb);
*
*   FUNCTION
*       Query the current socket-library error through the socketbase
*       associated with the guest context.
*
*   INPUTS
*       sb - Per-guest socket context.
*
*   RESULT
*       The socket-library error number, or ENOENT when the library or query
*       fails.
*
****************************************************************************
*
*/

static LONG get_errno(struct socketbase *sb) {

  struct Library *SocketBase;
  LONG connerr=-1;

  if(!(SocketBase=getsocketbase(sb))) {
    return ENOENT; /* should not happen.. */
  }

  if(SocketBaseTags(SBTM_GETREF(SBTC_ERRNO), &connerr, TAG_END)) {
    BSDLOG("ERROR on calling SocketBaseTags..\n");
    return ENOENT; 
  }

  //BSDLOG("SocketBaseTagList(SBTC_ERRNO): %d\n", connerr);

  return connerr;
}

/* not working ..? */
/****i* januae-bsdsock/get_errno_str ***************************************
*
*   NAME
*       get_errno_str() -- Resolve a socket error string.
*
*   SYNOPSIS
*       static char *get_errno_str(struct socketbase *sb, LONG error);
*
*   FUNCTION
*       Retrieve a textual description of an error through SocketBaseTags.
*
*   INPUTS
*       sb - Socket context. error - Error number.
*
*   RESULT
*       Socket-library string pointer; failure paths return an error number
*       cast to a pointer.
*
****************************************************************************
*
*/

static char *get_errno_str(struct socketbase *sb, LONG error) {

  struct Library *SocketBase;
  ULONG str;

  if(!(SocketBase=getsocketbase(sb))) {
    return (char *) ENOENT; /* should not happen.. */
  }

  str=error;

  if(SocketBaseTags(SBTM_GETREF(SBTC_ERRNOSTRPTR), &str, TAG_END)) {
    BSDLOG("ERROR on calling SocketBaseTags..\n");
    return (char *) ENOENT; 
  }

  return (char *) str;
}

/****i* januae-bsdsock/seterrno ********************************************
*
*   NAME
*       seterrno() -- Store a guest socket error.
*
*   SYNOPSIS
*       static void seterrno (struct socketbase *sb, int err);
*
*   FUNCTION
*       Assign the supplied error number to the socketbase error field.
*
*   INPUTS
*       sb - Socket context. err - Error number.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void seterrno (struct socketbase *sb, int err) {
  sb->sb_errno=err;
};

/****i* januae-bsdsock/getsocketbase ***************************************
*
*   NAME
*       getsocketbase() -- Resolve the socket library for a context.
*
*   SYNOPSIS
*       struct Library *getsocketbase(struct socketbase *sb);
*
*   FUNCTION
*       Return a cached bsdsocket.library base or open the library for this
*       socket context.
*
*   INPUTS
*       sb - Socket context.
*
*   RESULT
*       Socket library pointer, or NULL when the context or library is
*       unavailable.
*
*   NOTES
*       The opened base is retained in sb->bsdsocketbase.
*
****************************************************************************
*
*/

struct Library *getsocketbase(struct socketbase *sb) {

  struct Task *mytask;

  mytask=FindTask(NULL);

  BSDLOG("task: %lx\n", mytask);

  if(!sb) {
    BSDLOG("===================> ERROR ERROR ERROR: sb is NULL !?!?!?\n");
    return NULL;
  }

  if(sb->bsdsocketbase) {
    BSDLOG("return already opened SocketBase for sb %lx: %lx\n", sb, sb->bsdsocketbase);
    return sb->bsdsocketbase;
  }

  BSDLOG("OpenLibrary(\"bsdsocket.library\", 0) ..\n");
  sb->bsdsocketbase = OpenLibrary((STRPTR) "bsdsocket.library", 0);
  if(!sb->bsdsocketbase) {
    write_log("Unable to open bsdsocket.library.\n");
    BSDLOG("Unable to open bsdsocket.library!!\n");
  }

  BSDLOG("return NEW SocketBase %lx\n", sb->bsdsocketbase);

  //SocketBase=sb->bsdsocketbase;

  return sb->bsdsocketbase;
}

/****i* januae-bsdsock/clearsockabort **************************************
*
*   NAME
*       clearsockabort() -- Clear a socket abort indication.
*
*   SYNOPSIS
*       static void clearsockabort (SB);
*
*   FUNCTION
*       Emit a diagnostic; the previous abort-draining implementation is
*       disabled.
*
*   INPUTS
*       SB - Socket-context parameter macro.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void clearsockabort (SB) {

  BSDLOG("NOT YET DONE (needed for sockabort!!)\n");
#warning TODOTODOTODOTODOTODOTODOT!!!!!!!!!!!11

}




static int socket_layer_initialized = 0;

/****i* januae-bsdsock/init_socket_layer ***********************************
*
*   NAME
*       init_socket_layer() -- Initialize socket layer status.
*
*   SYNOPSIS
*       int init_socket_layer(void);
*
*   FUNCTION
*       Set socket_layer_initialized to a successful status without
*       performing the disabled stack startup sequence.
*
*   INPUTS
*       None.
*
*   RESULT
*       1.
*
*   NOTES
*       No socket library is opened by the active path.
*
****************************************************************************
*
*/

int init_socket_layer(void) {

  
  int result = 0;



/* DEBUG! */
  //BSDLOG("return faked socket_layer_initialized=TRUE!\n");
  result = 1;

  socket_layer_initialized = result;

  return result;
}


//#ifdef BSDSOCKET

/****i* januae-bsdsock/locksigqueue ****************************************
*
*   NAME
*       locksigqueue() -- Acquire signal queue protection.
*
*   SYNOPSIS
*       void locksigqueue(void);
*
*   FUNCTION
*       Emit a not-implemented diagnostic; the former critical-section
*       operation is disabled.
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

void locksigqueue(void)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
}

/****i* januae-bsdsock/unlocksigqueue **************************************
*
*   NAME
*       unlocksigqueue() -- Release signal queue protection.
*
*   SYNOPSIS
*       void unlocksigqueue(void);
*
*   FUNCTION
*       Emit a not-implemented diagnostic; the former critical-section
*       operation is disabled.
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

void unlocksigqueue(void)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
}


/****i* januae-bsdsock/host_sbinit *****************************************
*
*   NAME
*       host_sbinit() -- Initialize a socketbase abort signal.
*
*   SYNOPSIS
*       int host_sbinit(TrapContext *context, struct socketbase *sb);
*
*   FUNCTION
*       Allocate a signal bit for a guest socket context and retain it for
*       interruption requests.
*
*   INPUTS
*       context - Trap context. sb - Socket context.
*
*   RESULT
*       1 when a signal is allocated; otherwise 0.
*
*   NOTES
*       Socket-based abort setup is disabled.
*
****************************************************************************
*
*/

int host_sbinit(TrapContext *context, struct socketbase *sb) {

  BSDLOG("TEST: NOT YET TESTED!\n");
  sb->abort_signal=AllocSignal(-1);

  if(sb->abort_signal < 0) {
    write_log("ERROR: no free signal!!\n");
    BSDLOG("ERROR: no free signal!!\n");
    return 0;
  }


  /* this needs to be run in the real thread...!!*/
	
	return 1;
}

/****i* januae-bsdsock/host_closesocketquick *******************************
*
*   NAME
*       host_closesocketquick() -- Close a socket without waiting.
*
*   SYNOPSIS
*       void host_closesocketquick(int s);
*
*   FUNCTION
*       Emit a not-implemented diagnostic without closing the supplied
*       socket.
*
*   INPUTS
*       s - Native socket descriptor.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void host_closesocketquick(int s)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
}

/****i* januae-bsdsock/host_sbcleanup **************************************
*
*   NAME
*       host_sbcleanup() -- Clean up a guest socketbase.
*
*   SYNOPSIS
*       void host_sbcleanup(struct socketbase *sb);
*
*   FUNCTION
*       Emit a not-implemented diagnostic; the old descriptor and
*       asynchronous-event cleanup is disabled.
*
*   INPUTS
*       sb - Socket context.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void host_sbcleanup(struct socketbase *sb)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
}

/****i* januae-bsdsock/host_sbreset ****************************************
*
*   NAME
*       host_sbreset() -- Reset socket subsystem state.
*
*   SYNOPSIS
*       void host_sbreset(void);
*
*   FUNCTION
*       Emit a not-implemented diagnostic without resetting the disabled
*       asynchronous-event tables.
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

void host_sbreset(void)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
}


/****i* januae-bsdsock/sockabort *******************************************
*
*   NAME
*       sockabort() -- Request interruption of socket work.
*
*   SYNOPSIS
*       void sockabort(struct socketbase *sb);
*
*   FUNCTION
*       Signal the task recorded in the socket context using its allocated
*       abort signal bit.
*
*   INPUTS
*       sb - Socket context containing task and signal.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The earlier socket-based abort channel is disabled.
*
****************************************************************************
*
*/

void sockabort(struct socketbase *sb) {	

  ULONG sig;

  BSDLOG("Sock abort (sb->abort_signal %d (%lx), sb->abort_task %lx)!!\n", sb->abort_signal, 1L << sb->abort_signal,sb->aros_task);

  sig=1L << sb->abort_signal;

  Signal((struct Task *) sb->aros_task, sig);
}


// address cleaning
/****i* januae-bsdsock/prephostaddr ****************************************
*
*   NAME
*       prephostaddr() -- Normalize a host IPv4 address.
*
*   SYNOPSIS
*       static void prephostaddr(struct sockaddr_in *addr);
*
*   FUNCTION
*       Set the host sockaddr_in address family to AF_INET.
*
*   INPUTS
*       addr - IPv4 address structure to update.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void prephostaddr(struct sockaddr_in *addr)
{
    addr->sin_family = AF_INET;
}

/****i* januae-bsdsock/prepamigaaddr ***************************************
*
*   NAME
*       prepamigaaddr() -- Format a socket address for the guest.
*
*   SYNOPSIS
*       static void prepamigaaddr(struct sockaddr *realpt, int len);
*
*   FUNCTION
*       Rewrite the address family and length bytes into the guest sockaddr
*       layout.
*
*   INPUTS
*       realpt - Address storage. len - Reported structure length.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void prepamigaaddr(struct sockaddr *realpt, int len)
{
    // little endian address family value to the byte sin_family member
    ((char *)realpt)[1] = *((char *)realpt);
    
    // set size of address
    *((char *)realpt) = len;
}

/* debug */
/****i* januae-bsdsock/debug_socket ****************************************
*
*   NAME
*       debug_socket() -- Inspect a connected native socket.
*
*   SYNOPSIS
*       static void debug_socket(struct socketbase *sb, int s);
*
*   FUNCTION
*       Query and log the peer address of a socket when a socket library
*       base is available.
*
*   INPUTS
*       sb - Socket context. s - Native socket descriptor.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void debug_socket(struct socketbase *sb, int s) {
  struct Library *SocketBase;
  struct sockaddr_in debug_sockaddr;
  int debug_len;

  if(!(SocketBase=getsocketbase(sb))) {
    return;
  }

  debug_len=sizeof(struct sockaddr_in);

  if(getpeername(s, (struct sockaddr *) &debug_sockaddr, &debug_len)==-1) {
    BSDLOG("WARNING: getpeername on socket %d failed with errno %d ..\n", s, errno);
    
  }
  else {
     BSDLOG("socket %d Peer's IP address is: %s\n", s, inet_ntoa(debug_sockaddr.sin_addr));
     BSDLOG("socket %d Peer's port is: %d\n", s, (int) ntohs(debug_sockaddr.sin_port));
  }
}

/****************************************************************************
 * execute_in_task
 *
 * pack command an parameters in a JUAE_bsdsocket_Message and send the 
 * message to the port of the process, which handles our sb. The process has
 * opened its own native bsdsocket.library, so the right socket with
 * the right socketbase in the right task should be used.
 *
 * I am creating a fresh MsgPort for the answer each time, as I could
 * not get reusing it working at all. Might not be preformant..
 *
 ****************************************************************************/
/****i* januae-bsdsock/execute_in_task *************************************
*
*   NAME
*       execute_in_task() -- Dispatch a socket operation to its worker.
*
*   SYNOPSIS
*       static ULONG execute_in_task(struct socketbase *sb, LONG cmd, LONG
*       block, ULONG a, ULONG b, ULONG c, ULONG d, ULONG e, ULONG f, ULONG
*       g, ULONG h);
*
*   FUNCTION
*       Package an operation and up to eight arguments in a message, submit
*       it to the socket worker and await a reply.
*
*   INPUTS
*       sb - Socket context. cmd - Worker operation. block - Early-reply
*       flag. a through h - Operation arguments.
*
*   RESULT
*       The response message ret field.
*
*   NOTES
*       Creates and deletes a reply port for the operation. Early-reply
*       operations may finish after the waiting caller proceeds.
*
****************************************************************************
*
*/

static ULONG execute_in_task(struct socketbase *sb, 
                             LONG cmd, LONG block,
                             ULONG a, ULONG b, ULONG c, ULONG d, ULONG e, ULONG f, ULONG g, ULONG h) {

  struct JUAE_bsdsocket_Message msg;

  BSDLOG("block: %d\n", block);

  sb->reply_port=CreateMsgPort();
  msg.ExecMessage.mn_Node.ln_Type=NT_MESSAGE;
  msg.ExecMessage.mn_Length = sizeof(struct JUAE_bsdsocket_Message); 
  msg.ExecMessage.mn_ReplyPort = sb->reply_port; 

  msg.cmd  =cmd;
  msg.block=block;
  msg.a    =(ULONG) a;
  msg.b    =(ULONG) b;
  msg.c    =(ULONG) c;
  msg.d    =(ULONG) d;
  msg.e    =(ULONG) e;
  msg.f    =(ULONG) f;
  msg.g    =(ULONG) g;
  msg.h    =(ULONG) h;

  BSDLOG("PutMsg..\n");
  PutMsg(sb->aros_port, (struct Message *) &msg);
  BSDLOG("WaitPort..\n");
  WaitPort(sb->reply_port);
  BSDLOG("done!\n");
  DeleteMsgPort(sb->reply_port);
  sb->reply_port=NULL;

  return msg.ret;
}

/***********************************
 * host_dup2socket
 ***********************************
 *
 * WARNING: this has never been tested.
 *
 */
/****i* januae-bsdsock/host_dup2socket_real ********************************
*
*   NAME
*       host_dup2socket_real() -- Duplicate a native socket descriptor.
*
*   SYNOPSIS
*       int host_dup2socket_real(struct socketbase *sb, int fd1, int fd2);
*
*   FUNCTION
*       Resolve the source descriptor and perform the socket duplication
*       mapping in the worker context.
*
*   INPUTS
*       sb - Socket context. fd1 - Source descriptor. fd2 - Requested
*       destination.
*
*   RESULT
*       Mapped destination descriptor on success; a negative value on
*       failure.
*
*   NOTES
*       This implementation is marked untested in its source.
*
****************************************************************************
*
*/

int host_dup2socket_real(struct socketbase *sb, int fd1, int fd2) {

  struct Library *SocketBase;
  int s1,s2;

  BSDLOG("======== %s ========\n", __FILE__);
  BSDLOG("NOT YET TESTED!\n");

  if(!(SocketBase=getsocketbase(sb))) {
    return -1;
  }

  fd1++;

  s1 = getsock(sb, fd1);
  if (s1 != INVALID_SOCKET) {
    if (fd2 != -1) {
      if ((unsigned int) (fd2) >= (unsigned int) sb->dtablesize) {
        BSDLOG("Bad file descriptor (%d)\n", fd2);
        seterrno (sb, 9);	/* EBADF */
      }
      fd2++;
      s2 = getsock(sb,fd2);
      if (s2 != INVALID_SOCKET) {
        shutdown(s2,1);
        CloseSocket(s2);
      }
      setsd(sb,fd2,s1);
      BSDLOG("0\n");
      return 0;
    }
    else {
      fd2 = getsd(sb, 1);
      setsd(sb,fd2,s1);
      BSDLOG("%d\n",fd2);
      return (fd2 - 1);
    }
  }
  BSDLOG("-1\n");
  return -1;
}

/****i* januae-bsdsock/host_dup2socket *************************************
*
*   NAME
*       host_dup2socket() -- Dispatch socket descriptor duplication.
*
*   SYNOPSIS
*       int host_dup2socket(struct socketbase *sb, int fd1, int fd2);
*
*   FUNCTION
*       Submit a descriptor duplication request to the socket worker.
*
*   INPUTS
*       sb - Socket context. fd1 - Source descriptor. fd2 - Destination
*       descriptor.
*
*   RESULT
*       Worker result from execute_in_task().
*
****************************************************************************
*
*/

int host_dup2socket(struct socketbase *sb, int fd1, int fd2) {

  BSDLOG("======== %s ========\n",__func__);

  return execute_in_task(sb, BSD_dup2socket, 0, (ULONG) sb, (ULONG) fd1, (ULONG) fd2, 0,0,0,0,0);
}


/***********************************
 * socket
 ***********************************/

/****i* januae-bsdsock/host_socket_real ************************************
*
*   NAME
*       host_socket_real() -- Create and register a native socket.
*
*   SYNOPSIS
*       int host_socket_real(struct socketbase *sb, int af, int type, int
*       protocol);
*
*   FUNCTION
*       Open a native socket and install its descriptor in the guest socket
*       descriptor table.
*
*   INPUTS
*       sb - Socket context. af - Address family. type - Socket type.
*       protocol - Protocol.
*
*   RESULT
*       Guest descriptor on success; -1 on failure.
*
****************************************************************************
*
*/

int host_socket_real(struct socketbase *sb, int af, int type, int protocol) {
  struct Library *SocketBase;
  int s;
  int sd;
  int flags;
  unsigned long nonblocking = 1;

  BSDLOG("======== %s -> %s ========\n", __FILE__,__func__);

  BSDLOG("socket(%s,%s,%d) -> \n", af == AF_INET ? "AF_INET" : "AF_other", 
                                   type == SOCK_STREAM ? "SOCK_STREAM" : type == SOCK_DGRAM ? "SOCK_DGRAM " : "SOCK_RAW", 
                                   protocol);

  if(!(SocketBase=getsocketbase(sb))) {
    return -1;
  }

  {
    /* debug */
    struct protoent *p;
    if(protocol >0) {
      p=getprotobynumber(protocol);
      BSDLOG("protocol: %s\n", p->p_name);
    }
  }

  if ((s = socket(af, type, protocol)) == INVALID_SOCKET) {
    sb->sb_errno=errno;
    BSDLOG("ERROR: could not open socket: %d\n", sb->sb_errno);
    return -1;
  }

  BSDLOG("s: %lx\n", s);

  sd = getsd(sb, s);
  BSDLOG("sb: %lx\n",sb);
  BSDLOG("s:  %d\n",s);
  BSDLOG("sd: %d\n",sd);

  if(0) {
    /* the type argument may serve a second purpose: 
     * in addition to specifying a socket type, it may include 
     * the bitwise OR of any of the following values, to modify the behavior of socket():
     * SOCK_NONBLOCK   Set the O_NONBLOCK file status flag on the new open file description.  
     * SOCK_CLOEXEC    Set the close-on-exec (FD_CLOEXEC) flag on the new file descriptor.  
     *
     * But I am not sure, why WinUAE does this. Should not be necessary, as native 
     * socket connect should have cared already ..
     */
    BSDLOG("WARNING: we set port to nonblocking, NOT TESTED!\n");
    //IoctlSocket(s, FIONBIO, (char *) &nonblocking);
    flags=fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
  }

  /* our native socket is always nonblocking: */
  set_blocking(sb, s, FALSE);
 
	if (type == SOCK_RAW) {

		if (protocol==IPPROTO_UDP) {
      BSDLOG("=> IPPROTO_UDP\n");
	 		sb->ftable[sd-1] |= SF_RAW_UDP;
    }
		if (protocol==IPPROTO_ICMP) {
			struct sockaddr_in sin;

      BSDLOG("=> IPPROTO_ICMP\n");
      //BSDLOG("WARNING: oli forced socket to become  nonblocking!\n");
      //BSDLOG("IoctlSocket returns: %d\n", IoctlSocket(s, FIONBIO, (char *) &nonblocking));
      //BSDLOG("errno: %d\n", errno);

			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr = INADDR_ANY;
			bind(s,(struct sockaddr *)&sin,sizeof(sin)) ;
    }
 		if (protocol==IPPROTO_RAW) {
      BSDLOG("==> IPPROTO_RAW\n");
	 		sb->ftable[sd-1] |= SF_RAW_RAW;
    }
  }

  //set_blocking(sb, s, 1);

  /* "When you first create the socket descriptor with socket(), the kernel sets it to blocking." 
   * So we remember, that the 68k socket is blocking at the moment.
   */
  sb->ftable[sd-1] |= SF_BLOCKING;
  BSDLOG("sd: %d; sb->ftable[sd-1]: %lx\n", sd, sb->ftable[sd-1] );

  BSDLOG("return: %lx\n", sd-1);
	return sd-1;
}

/****i* januae-bsdsock/host_socket *****************************************
*
*   NAME
*       host_socket() -- Dispatch native socket creation.
*
*   SYNOPSIS
*       int host_socket(struct socketbase *sb, int af, int type, int
*       protocol);
*
*   FUNCTION
*       Submit a socket creation request to the socket worker.
*
*   INPUTS
*       sb - Socket context. af, type, protocol - Socket creation
*       parameters.
*
*   RESULT
*       Worker result from execute_in_task().
*
****************************************************************************
*
*/

int host_socket(struct socketbase *sb, int af, int type, int protocol) {

  BSDLOG("======== %s ========\n", __func__);

  return execute_in_task(sb, BSD_socket, 0,
                         (ULONG) sb, (ULONG) af, (ULONG) type, (ULONG) protocol, 0, 0, 0, 0);
}


/***********************************
 * bind
 ***********************************/

/****i* januae-bsdsock/host_bind_real **************************************
*
*   NAME
*       host_bind_real() -- Bind a native socket address.
*
*   SYNOPSIS
*       uae_u32 host_bind_real(struct socketbase *sb, uae_u32 sd, uae_u32
*       name, uae_u32 namelen);
*
*   FUNCTION
*       Resolve a guest descriptor, translate the guest socket address and
*       invoke the host bind operation.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. name - Guest address.
*       namelen - Address size.
*
*   RESULT
*       Socket operation status; failures update socket-context error state.
*
****************************************************************************
*
*/

uae_u32 host_bind_real(struct socketbase *sb, uae_u32 sd, uae_u32 name, uae_u32 namelen) {
  struct Library *SocketBase;
  int s;
  char buf[MAXADDRLEN];
  uae_u32 success = 0;

  BSDLOG("======== %s  ========\n", __func__);

  if(!(SocketBase=getsocketbase(sb))) {
    return -1;
  }

  sd++;
  BSDLOG("bind(%d,0x%lx,%d) -> \n",sd,name,namelen);
  s = getsock(sb, sd);

  if (s == INVALID_SOCKET) {
    return 0;
  }

	if (namelen > sizeof(buf)) {
    write_log("BSDSOCK: ERROR - Excessive namelen (%d) in bind()!\n",namelen);
    BSDLOG("BSDSOCK: ERROR - Excessive namelen (%d) in bind()!\n",namelen);
    return 0;
  }

  // some Amiga programs set this field to bogus values
  prephostaddr((struct sockaddr_in *)buf);

  if ((success = bind(s,(struct sockaddr *)buf,namelen)) != 0) {
    BSDLOG("ERROR: bind failed!\n");
    /* TODO: set errno right! */
    sb->sb_errno=errno;
  }

  return success;





}

/****i* januae-bsdsock/host_bind *******************************************
*
*   NAME
*       host_bind() -- Dispatch socket address binding.
*
*   SYNOPSIS
*       uae_u32 host_bind(struct socketbase *sb, uae_u32 sd, uae_u32 name,
*       uae_u32 namelen);
*
*   FUNCTION
*       Submit the guest address and descriptor for binding by the socket
*       worker.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. name - Guest address.
*       namelen - Address size.
*
*   RESULT
*       Worker result from execute_in_task().
*
****************************************************************************
*
*/

uae_u32 host_bind(struct socketbase *sb, uae_u32 sd, uae_u32 name, uae_u32 namelen) {

  BSDLOG("======== %s ========\n", __func__);

  return execute_in_task(sb, BSD_bind, 0,
                         (ULONG) sb, (ULONG) sd, (ULONG) name, (ULONG) namelen, 0, 0, 0, 0);
}

/****i* januae-bsdsock/host_listen *****************************************
*
*   NAME
*       host_listen() -- Handle a socket listen request.
*
*   SYNOPSIS
*       uae_u32 host_listen(struct socketbase *sb, uae_u32 sd, uae_u32
*       backlog);
*
*   FUNCTION
*       Report an unimplemented listen operation without invoking the native
*       listener.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. backlog - Requested
*       queue depth.
*
*   RESULT
*       -1.
*
****************************************************************************
*
*/

uae_u32 host_listen(struct socketbase *sb, uae_u32 sd, uae_u32 backlog)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
  SOCKET s;
  uae_u32 success = -1;
	
  return success;
}

/****i* januae-bsdsock/host_accept *****************************************
*
*   NAME
*       host_accept() -- Handle an incoming connection request.
*
*   SYNOPSIS
*       void host_accept(TrapContext *context, struct socketbase *sb,
*       uae_u32 sd, uae_u32 name, uae_u32 namelen);
*
*   FUNCTION
*       Emit a not-implemented diagnostic without accepting a native
*       connection.
*
*   INPUTS
*       context - Trap context. sb - Socket context. sd - Guest listener.
*       name, namelen - Guest peer-address output.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void host_accept(TrapContext *context, struct socketbase *sb, uae_u32 sd, uae_u32 name, uae_u32 namelen)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
	}


/***********************************
 * connect
 *
 * - we are now in the right socket thread
 * - we need to connect with a non-blocking socket, 
 *   independently from the fact, that
 *   our guest call was blocking or not.
 ***********************************/

/****i* januae-bsdsock/host_connect_real ***********************************
*
*   NAME
*       host_connect_real() -- Connect a native socket.
*
*   SYNOPSIS
*       void host_connect_real(TrapContext *context, struct socketbase *sb,
*       uae_u32 sd, uae_u32 addr68k, uae_u32 addrlen);
*
*   FUNCTION
*       Translate a guest peer address and execute the connection operation
*       in the socket worker.
*
*   INPUTS
*       context - Trap context. sb - Socket context. sd - Guest descriptor.
*       addr68k - Guest peer address. addrlen - Address size.
*
*   RESULT
*       No C value is returned.
*
*   NOTES
*       Operation status and errors are conveyed through the guest
*       trap/socket context.
*
****************************************************************************
*
*/

void host_connect_real(TrapContext *context, struct socketbase *sb, uae_u32 sd, uae_u32 addr68k, uae_u32 addrlen) {
  struct Library *SocketBase;
  int s;
  struct sockaddr_in sin;
#if OLD
  //int *addr;
#endif
  unsigned int reorder;
  int done=FALSE;
  LONG error;


  BSDLOG("======== %s -> %s ========\n", __FILE__,__func__);
  BSDLOG("TrapContext *%lx, struct socketbase *%lx, uae_u32 %lx, uae_u32 %lx, uae_u32 %lx\n", context, sb, sd, addr68k, addrlen);
  if(!(SocketBase=getsocketbase(sb))) {
		seterrno(sb,1); /*???*/
    return;
  }

  BSDLOG("addr68k: %lx\n", addr68k);
  BSDLOG("namelen: %d\n",  addrlen);

	sd++;
  s = getsock(sb,(int)sd);

  BSDLOG("s: %d\n", s);

  if (s == INVALID_SOCKET) {
    BSDLOG("s == INVALID_SOCKET!\n");
    sb->resultval = -1;
    bsdsocklib_seterrno (sb, EBADF);
    return;
  }

	if (addrlen > MAXADDRLEN) {
    write_log("BSDSOCK: WARNING - Excessive addrlen (%d) in connect()!\n",addrlen);
    BSDLOG("BSDSOCK: WARNING - Excessive addrlen (%d) in connect()!\n",addrlen);
    return;
  }

  copysockaddr_a2n (&sin, addr68k, sizeof (struct sockaddr_in));

  {
    /* 
     * check, if all arguments are in right byte order!
     */
    char *debug_ip=inet_ntoa(sin.sin_addr);
    BSDLOG("connecting to %s:%d ..\n", debug_ip, (int) ntohs(sin.sin_port));
  }
 
  /* native socket to nonblocking */
  set_blocking(sb, s, 0);

  sb->resultval=connect(s, (struct sockaddr *) &sin, sin.sin_len);

  error=get_errno(sb);

  BSDLOG("native connect returned: %d; errno %d (%s))\n", sb->resultval, error, get_errno_str(sb, error));

  BSDLOG("sd: %d; sb->ftable[sd-1]: %lx; SF_BLOCKING: 80000000; sb->ftable[sd-1] & SF_BLOCKING: %lx\n", sd, sb->ftable[sd-1], sb->ftable[sd-1] & SF_BLOCKING);

  if(! (sb->ftable[sd-1] & SF_BLOCKING) ) {
    /* non blocking, return result at once */
    BSDLOG("hosted is in non blocking mode -> return result at once.. errno is %d\n", errno);
    seterrno(sb, error);
    return;
  }

  /* blocking sux */

  while(!done) {
    sb->resultval=connect(s, (struct sockaddr *) &sin, sin.sin_len); /* try again */

    BSDLOG("sb->resultval: %d\n", sb->resultval);

    if(!sb->resultval) {
      /* success! */
      BSDLOG("connected!\n");
      seterrno(sb, 0);
      return;
    }

    error=get_errno(sb);

    BSDLOG("while (errno is %d)..\n", error);
    if ((error == EAGAIN) || (error == EWOULDBLOCK) || (error == EINPROGRESS) || (error == EALREADY)) {

      BSDLOG("EAGAIN, EWOULDBLOCK, EINPROGRESS..\n");

      fd_set readset, writeset, exceptset;
      //int maxfd = (sb->s > sb->sockAbort) ? sb->s : sb->sockAbort;
      int maxfd=s;
      int num;
      ULONG signals;

      BSDLOG("maxfd: %d\n", maxfd);

      FD_ZERO (&readset);
      FD_ZERO (&writeset);
      FD_ZERO (&exceptset);
      
      //if (sb->action == 3 || sb->action == 6)
          //FD_SET (sb->s, &readset);

      //if (sb->action == 2 || sb->action == 1 || sb->action == 4)
          FD_SET (sb->s, &writeset);

      //FD_SET (sb->sockAbort, &readset);

      BSDLOG("calling WaitSelect..\n");
      //num = select (maxfd + 1, &readset, &writeset, &exceptset, NULL);
      signals=1L <<  sb->abort_signal;
      num = WaitSelect (maxfd + 1, &readset, &writeset, &exceptset, NULL, &signals);
      BSDLOG("WaitSelect returned %d\n", num);
      if (num == -1) {
          BSDLOG ("Blocking WaitSelect() returns -1,errno is %d\n", errno); /* TODO: errno! */
          //TODO: fcntl (sb->s, F_SETFL, flags);
          BSDLOG("================>>>>>>>>>>> TODO!\n");
          return;
      }

      //if (FD_ISSET (sb->sockAbort, &readset) || FD_ISSET (sb->sockAbort, &writeset)) {
      if(num == 0) {
          /* reset sock abort pipe */
          /* read from the pipe to reset it */
          BSDLOG ("select aborted from signal\n");

          clearsockabort (sb);
          BSDLOG ("Done read\n");
          error = EINTR;
          done = 1;
      } else {
        done = 0;
      }
    } else if (error == EINTR) {
        done = 1;
    }
    else {
      /* real error, we are done! */
      sb->sb_errno=error;
      done=1;
    }

    //BSDLOG("next connect..\n");

    //sb->resultval=connect(s, (struct sockaddr *) &sin, sin.sin_len);
  }

CONNECTED:
  BSDLOG("connected!\n");

  BSDLOG("signal waiting task..\n");
  uae_Signal (sb->ownertask, sb->sigstosend | ((uae_u32) 1) << sb->signal); 
}

/****i* januae-bsdsock/host_connect ****************************************
*
*   NAME
*       host_connect() -- Dispatch a socket connection.
*
*   SYNOPSIS
*       void host_connect(TrapContext *context, struct socketbase *sb,
*       uae_u32 sd, uae_u32 addr68k, uae_u32 addrlen);
*
*   FUNCTION
*       Send a connect request to the socket worker using the guest
*       descriptor blocking flag.
*
*   INPUTS
*       context - Trap context. sb - Socket context. sd - Guest descriptor.
*       addr68k - Guest peer address. addrlen - Address size.
*
*   RESULT
*       No C value is returned.
*
****************************************************************************
*
*/

void host_connect(TrapContext *context, struct socketbase *sb, uae_u32 sd, uae_u32 addr68k, uae_u32 addrlen) {

  ULONG block=0;

  BSDLOG("======== %s ========\n",__func__);

  if(sb->ftable[sd] & SF_BLOCKING) {
    block=1;
  }

  BSDLOG("sd: %d;sb->ftable[sd]: %lx; SF_BLOCKING: %lx; sb->ftable[sd] & SF_BLOCKING: %lx => block is %d\n", sd, sb->ftable[sd], SF_BLOCKING, sb->ftable[sd] & SF_BLOCKING, block);

  execute_in_task(sb, BSD_connect, block,
                  (ULONG) context, (ULONG) sb, (ULONG) sd, (ULONG) addr68k, (ULONG) addrlen, 0, 0, 0);

  /* amiga side wants to wait for results.. */
  if(sb->ftable[sd] & SF_BLOCKING) {
    BSDLOG("calling WAITSIGNAL..\n");
    WAITSIGNAL;
  }

  BSDLOG("SIGNAL received => leave!\n");

  return;
}

/***********************************
 * sendto
 ***********************************/

/****i* januae-bsdsock/host_sendto_real ************************************
*
*   NAME
*       host_sendto_real() -- Send guest data through a socket.
*
*   SYNOPSIS
*       void host_sendto_real(TrapContext *context, struct socketbase *sb,
*       uae_u32 sd, uae_u32 msg, uae_u32 len, uae_u32 flags, uae_u32 to,
*       uae_u32 tolen);
*
*   FUNCTION
*       Translate guest send buffers and destination-address information and
*       perform native socket output.
*
*   INPUTS
*       context - Trap context. sb - Socket context. sd - Guest descriptor.
*       msg - Guest buffer. len - Byte count. flags - Flags. to, tolen -
*       Optional destination.
*
*   RESULT
*       No C value is returned.
*
*   NOTES
*       Result and errors are reported through the trap/socket context.
*
****************************************************************************
*
*/

void host_sendto_real(TrapContext *context, struct socketbase *sb, uae_u32 sd, uae_u32 msg, uae_u32 len, uae_u32 flags, uae_u32 to, uae_u32 tolen) {
  struct Library *SocketBase;
  int s; 
  char *realmsg;
  char *send_realmsg;
  int send_len;
  char buf[MAXADDRLEN];
  int iCut;

  BSDLOG("======== %s -> %s ========\n", __FILE__,__func__);

  if(!(SocketBase=getsocketbase(sb))) {
    seterrno(sb, 1);
    return; /* TODO!*/
  }

  if (to)
    BSDLOG("sendto(%d,msg 0x%lx, len %d, flags 0x%lx, to 0x%lx, tolen %d)\n",sd,msg,len,flags,to,tolen);
  else
    BSDLOG("send(%d,msg 0x%lx, len %d, flags %d)\n",sd,msg,len,flags);

	sd++;
  s = getsock(sb,sd);

  BSDLOG("s: %lx\n", s);

  if(s==INVALID_SOCKET) {
    BSDLOG("s==INVALID_SOCKET!!\n");
    sb->resultval=INVALID_SOCKET;
    return;
  }

  realmsg = (char *) get_real_address(msg);
  BSDLOG("realmsg: %lx\n", realmsg);
  if (to) {
    if (tolen > sizeof buf) {
      write_log("BSDSOCK: WARNING - Target address in sendto() too large (%d)!\n",tolen);
      BSDLOG("WARNING - Target address in sendto() too large (%d)!\n",tolen);
    }
    else {
      memcpy(buf,get_real_address(to),tolen);
      // some Amiga software sets this field to bogus values
      prephostaddr((struct sockaddr_in *)buf);
    }
  }


  BSDLOG("sd: %d; sb->ftable[sd-1]: 0x%lx\n", sd, sb->ftable[sd-1]);
  if (sb->ftable[sd-1] & SF_RAW_RAW) {
    BSDLOG("SF_RAW_RAW\n");

    if (*(realmsg+9) == 0x1) { // ICMP
      struct sockaddr_in sin;

      BSDLOG("ICMP (RAW 0x1\n)");
      shutdown(s,1);
      CloseSocket(s);
      s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

      sin.sin_family = AF_INET;
      sin.sin_addr.s_addr = INADDR_ANY;
      sin.sin_port = (unsigned short) (*(realmsg+21)&0xff)*256 + (unsigned short) (*(realmsg+20)&0xff);
      bind(s,(struct sockaddr *)&sin,sizeof(sin)) ;

      sb->dtable[sd-1] = s;
      sb->ftable[sd-1]&= ~SF_RAW_RAW;
      sb->ftable[sd-1]|= SF_RAW_RICMP;
    }

    if (*(realmsg+9) == 0x11) { // UDP
      struct sockaddr_in sin;

      BSDLOG("UDP (RAW 0x11)\n");
      shutdown(s,1);
      CloseSocket(s);
      s = socket(AF_INET,SOCK_RAW,IPPROTO_UDP);

      sin.sin_family = AF_INET;
      sin.sin_addr.s_addr = INADDR_ANY;
      sin.sin_port = (unsigned short) (*(realmsg+21)&0xff)*256 + (unsigned short) (*(realmsg+20)&0xff);
      bind(s,(struct sockaddr *)&sin,sizeof(sin)) ;

      sb->dtable[sd-1] = s;
      sb->ftable[sd-1]&= ~SF_RAW_RAW;
      sb->ftable[sd-1]|= SF_RAW_RUDP;
    }
  }

//  for(;;) {
    //send_realmsg=realmsg;
    //send_len=len;

    if (sb->ftable[sd - 1] & SF_RAW_RUDP) {
      BSDLOG("WARNING: sb->ftable[sd - 1] & SF_RAW_RUDP\n");
    }
    if (sb->ftable[sd - 1] & SF_RAW_RICMP) {
      BSDLOG("WARNING: sb->ftable[sd - 1] & SF_RAW_RICMP\n");
    }

    
    BSDLOG("calling sendto..\n");
    sb->resultval=sendto(s, realmsg, len, flags, (const struct sockaddr *) buf, tolen);

    BSDLOG("sent:\n-----------------\n%s\n-----------------\n", realmsg);



    if(sb->resultval == -1) {
      BSDLOG("SOCKET_ERROR!\n");
    }
    else {
      BSDLOG("sendto ok!\n");
    }
  //}

  BSDLOG("left: characters sent: %d\n", sb->resultval);

  return;

	
}

/****i* januae-bsdsock/host_sendto *****************************************
*
*   NAME
*       host_sendto() -- Dispatch socket output.
*
*   SYNOPSIS
*       void host_sendto(TrapContext *context, struct socketbase *sb,
*       uae_u32 sd, uae_u32 msg_in, uae_u32 len, uae_u32 flags, uae_u32 to,
*       uae_u32 tolen);
*
*   FUNCTION
*       Submit the guest send buffer, flags and optional destination to the
*       socket worker.
*
*   INPUTS
*       context - Trap context. sb - Socket context. sd - Guest descriptor.
*       msg_in - Guest data. len, flags, to, tolen - Send parameters.
*
*   RESULT
*       No C value is returned.
*
****************************************************************************
*
*/

void host_sendto(TrapContext *context, struct socketbase *sb, uae_u32 sd, uae_u32 msg_in, uae_u32 len, uae_u32 flags, uae_u32 to, uae_u32 tolen) {

  BSDLOG("======== %s ========\n",__func__);

  execute_in_task(sb, BSD_sendto, 0,
                  (ULONG) context, (ULONG) sb, (ULONG) sd, (ULONG) msg_in, (ULONG) len, 
                  (ULONG) flags, (ULONG) to, (ULONG) tolen);

  return;
}

/***********************************
 * recvfrom
 ***********************************
 *
 * If addr!=NULL, we have to fill in the results of the recv call there.
 * So len has to be the size of the struct sockaddr,
 * and addr needs to be filled with the source, where the data came
 * from!
 *
 * Can be blocking..
 */

/****i* januae-bsdsock/a ***************************************************
*
*   NAME
*       a() -- Display a readable byte for diagnostics.
*
*   SYNOPSIS
*       char a(char c);
*
*   FUNCTION
*       Return a printable ASCII character or a dot for a nonprintable byte.
*
*   INPUTS
*       c - Input byte.
*
*   RESULT
*       c when printable; otherwise a dot character.
*
****************************************************************************
*
*/

char a(char c) {
  if(c>31 && c<126) {
    return c;
  }
  return '.';
}
/****i* januae-bsdsock/host_recvfrom_real **********************************
*
*   NAME
*       host_recvfrom_real() -- Receive socket data for the guest.
*
*   SYNOPSIS
*       void host_recvfrom_real(TrapContext *context, struct socketbase *sb,
*       uae_u32 sd, uae_u32 msg, uae_u32 len, uae_u32 flags, uae_u32 addr,
*       uae_u32 addrlen);
*
*   FUNCTION
*       Receive native socket data and transfer the payload and optional
*       source address to guest memory.
*
*   INPUTS
*       context - Trap context. sb - Socket context. sd - Guest descriptor.
*       msg - Guest destination. len - Size. flags - Flags. addr, addrlen -
*       Source-address outputs.
*
*   RESULT
*       No C value is returned.
*
*   NOTES
*       Operation status is transmitted through the guest trap/socket
*       context.
*
****************************************************************************
*
*/

void host_recvfrom_real(TrapContext *context, struct socketbase *sb, uae_u32 sd, uae_u32 msg, uae_u32 len, uae_u32 flags, uae_u32 addr, uae_u32 addrlen) {
  struct Library *SocketBase;
  int s;
  struct sockaddr *rp_addr = NULL;
  uae_char *realpt;
  int hlen;
  int res;
  struct sockaddr result_addr;
  struct sockaddr_in host_addr;
  socklen_t fromlen;
  LONG error;
  ULONG done=0;
  int nonblock=1;


  BSDLOG("======== %s -> %s ========\n", __FILE__,__func__);

  if(!(SocketBase=getsocketbase(sb))) {
    seterrno(sb, ENETDOWN);
    sb->resultval = -1;
    return; 
  }

  if(sb->ftable[sd-1] & SF_BLOCKING) {
    nonblock=0;
  }

  if (addr)
    BSDLOG("recvfrom(%d,0x%lx,%d,0x%lx,0x%lx,%d) ->\n",sd,msg,len,flags,addr,get_long(addrlen));
  else
    BSDLOG("recv(%d,0x%lx,%d,0x%lx) ->\n",sd,msg,len,flags);

  sd++;
  s = getsock(sb,sd);

  BSDLOG("s: %lx\n", s);

  if (s == INVALID_SOCKET) {
    BSDLOG("INVALID_SOCKET!\n");
    sb->sb_errno =EBADF;
    sb->resultval=-1;
    return;
  }

  realpt = (uae_char*) get_real_address (msg);
  //flags &= ~0x40; /* ?? */

  /* native socket to nonblocking */
  set_blocking(sb, s, 0);

  while(!done) {
    BSDLOG("===> while(!done) <===\n");
    if(!addr) {
      /* same as recv */
      res=recvfrom(s, realpt, len, flags, NULL, 0);
      BSDLOG("recv returned %d \n", res);
    }
    else {
      socklen_t l;
      LONG i;
      l = sizeof (struct sockaddr_in);
      i = get_long (addrlen);
      copysockaddr_a2n (&host_addr, addr, i);

      res=recvfrom(s, realpt, len, flags/*|MSG_NOSIGNAL*/, (struct sockaddr *) &host_addr, (void *) &l);
      BSDLOG("recvfrom returned %d \n", res);
      if(res>0) {
        copysockaddr_n2a(addr, &host_addr, l);
        put_long(addrlen, l);
      }

      //prepamigaaddr(rp_addr,hlen);
      //put_long (addrlen,hlen);
    }
    error=get_errno(sb);
    BSDLOG("error: %d\n", error);

    if(!(res<0 /*&& !nonblock*/ )) {
      int i=0;
      BSDLOG("successfuly received %d bytes\n", res);
      //kprintf("GOT:\n%s\n", (char *) realpt);
      kprintf("GOT (%d): \n===========================================\n\n",res );
      /* dump first 160 chars */
      while(i+8<res && i<160) {
        kprintf("%02x %02x %02x %02x  %02x %02x %02x %02x   %c%c%c%c %c%c%c%c\n", realpt[i], realpt[i+1], realpt[i+2], realpt[i+3], realpt[i+4], realpt[i+5], realpt[i+6], realpt[i+7], a(realpt[i]), a(realpt[i+1]), a(realpt[i+2]), a(realpt[i+3]), a(realpt[i+4]), a(realpt[i+5]), a(realpt[i+6]), a(realpt[i+7]));

        i=i+8;
      }
      kprintf("\n... and so on ...\n");
      kprintf("\n===========================================\n\n",res );

      done=1;
    }
    else {

      BSDLOG("res<0 /* && !nonblock */\n");

      if ((error == EAGAIN) || (error == EWOULDBLOCK) || (error == EINPROGRESS) || (error == EALREADY)) {

        BSDLOG("EAGAIN, EWOULDBLOCK, EINPROGRESS..\n");

        fd_set readset, writeset, exceptset;
        //int maxfd = (sb->s > sb->sockAbort) ? sb->s : sb->sockAbort;
        ULONG signals;
        int maxfd = sb->s;
        int num;

        BSDLOG("maxfd: %d\n", maxfd);

        FD_ZERO (&readset);
        FD_ZERO (&writeset);
        FD_ZERO (&exceptset);

        FD_SET (sb->s, &readset);
        //FD_SET (sb->sockAbort, &readset);

        BSDLOG("calling WaitSelect(maxfd %d, readset %lx, writeset %lx, exceptset %lx, NULL, %lx) ..\n", maxfd + 1, &readset, &writeset, &exceptset, 1 << sb->abort_signal);
        signals=1L << sb->abort_signal;
        num = WaitSelect (maxfd + 1, &readset, &writeset, &exceptset, NULL, &signals);
        BSDLOG("select returned %d\n", num);
        if (num == -1) {
            BSDLOG ("Blocking select() returns -1,errno is %d\n", errno); /* errno TODO! */
            //TODO: fcntl (sb->s, F_SETFL, flags);
            BSDLOG("================>>>>>>>>>>> TODO!\n");
            sb->resultval=-1;
            return;
        }

#warning sockabort NEEDS TO BE DONE IN host_sbinit!!??
        //if (FD_ISSET (sb->sockAbort, &readset)) {
        if (num == 0) {
          /* reset sock abort pipe */
          /* read from the pipe to reset it */
          BSDLOG ("select aborted from signal\n");

          //TODO: clearsockabort (sb);
          BSDLOG ("Done read\n");
          error = EINTR;
          done = 1;
        } else {
          done = 0;
        }
      } else if (error == EINTR) {
        done = 1;
      }
      else {
        /* real error, we are done! */
        sb->sb_errno=error;
        done=1;
      }
    }

  }

  BSDLOG("bytes received with recvfrom: %d\n", res);

  sb->resultval = res;
  sb->sb_errno  = error;

RECEIVED:
  BSDLOG("received!\n");

  BSDLOG("signal waiting task..\n");
  uae_Signal (sb->ownertask, sb->sigstosend | ((uae_u32) 1) << sb->signal); 
}

/****i* januae-bsdsock/host_recvfrom ***************************************
*
*   NAME
*       host_recvfrom() -- Dispatch socket input.
*
*   SYNOPSIS
*       void host_recvfrom(TrapContext *context, struct socketbase *sb,
*       uae_u32 sd, uae_u32 msg_in, uae_u32 len, uae_u32 flags, uae_u32
*       addr, uae_u32 addrlen);
*
*   FUNCTION
*       Submit a socket receive operation to the worker, selecting the
*       early-reply path when the descriptor is marked blocking.
*
*   INPUTS
*       context - Trap context. sb - Socket context. sd - Guest descriptor.
*       msg_in - Guest destination. len, flags, addr, addrlen - Receive
*       parameters.
*
*   RESULT
*       No C value is returned.
*
****************************************************************************
*
*/

void host_recvfrom(TrapContext *context, struct socketbase *sb, uae_u32 sd, uae_u32 msg_in, uae_u32 len, uae_u32 flags, uae_u32 addr, uae_u32 addrlen) {

  ULONG block=0;

  BSDLOG("======== %s ========\n",__func__);

  if(sb->ftable[sd] & SF_BLOCKING) {
    block=1;
  }

  BSDLOG("sd: %d;sb->ftable[sd]: %lx; SF_BLOCKING: %lx; sb->ftable[sd] & SF_BLOCKING: %lx => block is %d\n", sd, sb->ftable[sd], SF_BLOCKING, sb->ftable[sd] & SF_BLOCKING, block);

  execute_in_task(sb, BSD_recvfrom, block,
                  (ULONG) context, (ULONG) sb, (ULONG) sd, (ULONG) msg_in, (ULONG) len, 
                  (ULONG) flags, (ULONG) addr, (ULONG) addrlen);

  /* amiga side wants to wait for results.. */
    BSDLOG("calling m68k WAITSIGNAL ..\n");
    WAITSIGNAL;
    BSDLOG("m68k Signal received => WAITSIGNAL left!\n");

  /* set errno's .. */

  if(sb->errnoptr) {
    BSDLOG("set guest errno %lx to %d\n", sb->errnoptr, sb->sb_errno);
    put_long(sb->errnoptr, sb->sb_errno);
  }

  return;
}



/****i* januae-bsdsock/host_shutdown ***************************************
*
*   NAME
*       host_shutdown() -- Handle socket shutdown.
*
*   SYNOPSIS
*       uae_u32 host_shutdown(struct socketbase *sb, uae_u32 sd, uae_u32
*       how);
*
*   FUNCTION
*       Report an unimplemented socket shutdown without invoking the native
*       operation.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. how - Shutdown
*       direction.
*
*   RESULT
*       -1.
*
****************************************************************************
*
*/

uae_u32 host_shutdown(struct socketbase *sb, uae_u32 sd, uae_u32 how)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
  return -1;
}

/***********************************
 * setsockopt
 ***********************************/

/****i* januae-bsdsock/host_setsockopt_real ********************************
*
*   NAME
*       host_setsockopt_real() -- Apply a socket option.
*
*   SYNOPSIS
*       void host_setsockopt_real(struct socketbase *sb, uae_u32 sd, uae_u32
*       level, uae_u32 optname, uae_u32 optval, uae_u32 len);
*
*   FUNCTION
*       Translate a guest socket option request and apply it to the resolved
*       native socket.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. level, optname - Option
*       identity. optval - Guest option value. len - Size.
*
*   RESULT
*       No C value is returned.
*
*   NOTES
*       Socket failures are reflected in the context error state.
*
****************************************************************************
*
*/

void host_setsockopt_real(struct socketbase *sb, uae_u32 sd, uae_u32 level, uae_u32 optname, uae_u32 optval, uae_u32 len) {

  struct Library *SocketBase;
  int s;
  char buf[MAXADDRLEN];
  BSDLOG("======== %s -> %s ========\n", __FILE__,__func__);

  BSDLOG("host_setsockopt(0x%lx,%d,%lx,0x%lx,0x%lx,%d)\n", sb, sd, level,optname,optval,len);
  if(level != SOL_SOCKET) {
    BSDLOG("Warning: strange level %lx != SOL_SOCKET !?\n", level);
  }
  else {
    BSDLOG("level: %lx (SOL_SOCKET)\n", level);
  }

  if(!(SocketBase=getsocketbase(sb))) {
    seterrno(sb, ENETDOWN);
    return; /* TODO!*/
  }

	sd++;
  s = getsock(sb,sd);

  BSDLOG("s: %lx\n", s);

  if (s == INVALID_SOCKET) {
    BSDLOG("INVALID_SOCKET (EBADF returned)!\n");
    seterrno(sb, EBADF); /* EBADF => The argument sockfd is not a valid descriptor. */
    return;
  }

	if (len > sizeof buf) {
	    write_log("BSDSOCK: WARNING - Excessive optlen in setsockopt() (%d)\n",len);
	    BSDLOG("BSDSOCK: WARNING - Excessive optlen in setsockopt() (%d)\n",len);
	    len = sizeof buf;
	}	
	
	if (level == IPPROTO_IP && optname == IP_HDRINCL) {
    /* The IPv4 layer generates an IP header when sending a packet unless 
     * the IP_HDRINCL socket option is enabled on the socket. 
     * When it is enabled, the packet must contain an IP header. 
     * For receiving the IP header is always included in the packet. 
     */
    BSDLOG("WARNING: IP_HDRINCL might NOT be implemented!\n");
  }

	if (level == SOL_SOCKET && optname == SO_LINGER) {
    BSDLOG("WARNING: SOL_SOCKET && SO_LINGER is TODO!\n");
	    //((LINGER *)buf)->l_onoff = get_long(optval);
	    //((LINGER *)buf)->l_linger = get_long(optval+4);
	}
	else {
    if (len == 4) {
      *(long *)buf = get_long(optval);
    }
    else {
      if (len == 2) {
        *(short *)buf = get_word(optval);
      }
      else {
        write_log("BSDSOCK: ERROR - Unknown optlen (%d) in setsockopt(%d,%d)\n",level,optname);
        BSDLOG("BSDSOCK: ERROR - Unknown optlen (%d) in setsockopt(%d,%d)\n",level,optname);
      }
    }
	}

  /* handle SO_EVENTMASK */
	if (level == 0xffff && optname == 0x2001) {
    BSDLOG("WARNING: SO_EVENTMASK is TODO!\n");
  }
 
  BSDLOG("call setsockopt(s %d, level %lx, optname %lx, buf 0x%lx, len %d)\n", s, level, optname, buf,len);
	sb->resultval = setsockopt(s, level, optname, buf, len);

  if(sb->resultval) {
    BSDLOG("ERROR!\n");
    sb->sb_errno=errno;
  }

  BSDLOG("result: %d\n", sb->resultval);
  return;
}

/****i* januae-bsdsock/host_setsockopt *************************************
*
*   NAME
*       host_setsockopt() -- Dispatch a socket option change.
*
*   SYNOPSIS
*       void host_setsockopt(struct socketbase *sb, uae_u32 sd, uae_u32
*       level, uae_u32 optname, uae_u32 optval, uae_u32 len);
*
*   FUNCTION
*       Submit a socket option update to the socket worker.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. level, optname, optval,
*       len - Option request.
*
*   RESULT
*       No C value is returned.
*
****************************************************************************
*
*/

void host_setsockopt(struct socketbase *sb, uae_u32 sd, uae_u32 level, uae_u32 optname, uae_u32 optval, uae_u32 len) {

  BSDLOG("======== %s ========\n",__func__);

  execute_in_task(sb, BSD_setsockopt, 0,
                  (ULONG) sb, (ULONG) sd, (ULONG) level, (ULONG) optname, (ULONG) optval, 
                  (ULONG) len, (ULONG) 0, (ULONG) 0);

  return;
}

/***********************************
 * getsockopt
 ***********************************/

/****i* januae-bsdsock/host_getsockopt *************************************
*
*   NAME
*       host_getsockopt() -- Handle a socket option query.
*
*   SYNOPSIS
*       uae_u32 host_getsockopt(struct socketbase *sb, uae_u32 sd, uae_u32
*       level, uae_u32 optname, uae_u32 optval, uae_u32 optlen);
*
*   FUNCTION
*       Report an unimplemented socket option query without populating the
*       supplied guest option buffer.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. level, optname - Option
*       identity. optval, optlen - Guest output parameters.
*
*   RESULT
*       -1.
*
****************************************************************************
*
*/

uae_u32 host_getsockopt(struct socketbase *sb, uae_u32 sd, uae_u32 level, uae_u32 optname, uae_u32 optval, uae_u32 optlen)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
	return -1;
}

/***********************************
 * getsockname
 ***********************************/

/****i* januae-bsdsock/host_getsockname_real *******************************
*
*   NAME
*       host_getsockname_real() -- Read a socket local address.
*
*   SYNOPSIS
*       uae_u32 host_getsockname_real(struct socketbase *sb, uae_u32 sd,
*       uae_u32 name, uae_u32 namelen);
*
*   FUNCTION
*       Query the native socket local address and copy it to guest storage.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. name - Guest address
*       output. namelen - Guest length location.
*
*   RESULT
*       Zero on success; -1 on error.
*
*   NOTES
*       Uses the translated guest socket descriptor.
*
****************************************************************************
*
*/

uae_u32 host_getsockname_real(struct socketbase *sb, uae_u32 sd, uae_u32 name, uae_u32 namelen) {

  struct Library *SocketBase;
  int s;
  int len;
	struct sockaddr *rp_name;

  BSDLOG("======== %s ========\n",__func__);
  BSDLOG("NOT YET TESTED!\n");

  if(!(SocketBase=getsocketbase(sb))) {
    seterrno(sb, ENETDOWN);
    return -1; /* TODO!*/
  }

	sd++;
	len = get_long(namelen);
  BSDLOG("len: %d\n", len);

	s = getsock(sb, sd);
  if(s==INVALID_SOCKET) {
    BSDLOG("ERROR: could not get socket!\n");
    return -1;
  }

  rp_name = (struct sockaddr *) get_real_address(name);

  if (getsockname(s, rp_name, &len)) {
    sb->sb_errno=errno;
    BSDLOG("ERROR: failed (%d)\n", sb->sb_errno);
    return -1;
  }
  
  BSDLOG("result len: %d\n", len);
  prepamigaaddr(rp_name, len);
  put_long(namelen, len);

  return 0;
}

/****i* januae-bsdsock/host_getsockname ************************************
*
*   NAME
*       host_getsockname() -- Dispatch a local socket address query.
*
*   SYNOPSIS
*       uae_u32 host_getsockname(struct socketbase *sb, uae_u32 sd, uae_u32
*       name, uae_u32 namelen);
*
*   FUNCTION
*       Submit the local-address arguments through the worker message
*       interface.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. name - Address
*       destination. namelen - Length location.
*
*   RESULT
*       Worker result from execute_in_task().
*
*   NOTES
*       The current implementation uses BSD_setsockopt as its command
*       identifier.
*
****************************************************************************
*
*/

uae_u32 host_getsockname(struct socketbase *sb, uae_u32 sd, uae_u32 name, uae_u32 namelen) {

  BSDLOG("======== %s ========\n",__func__);

  return execute_in_task(sb, BSD_setsockopt, 0,
                         (ULONG) sb, (ULONG) sd, (ULONG) name, (ULONG) namelen, (ULONG) 0, 
                         (ULONG) 0, (ULONG) 0, (ULONG) 0);
}

/***********************************
 * getpeername
 ***********************************/
/****i* januae-bsdsock/host_getpeername_real *******************************
*
*   NAME
*       host_getpeername_real() -- Read a socket peer address.
*
*   SYNOPSIS
*       uae_u32 host_getpeername_real(struct socketbase *sb, uae_u32 sd,
*       uae_u32 name, uae_u32 namelen);
*
*   FUNCTION
*       Query the native socket peer address and copy the resulting sockaddr
*       to guest storage.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. name - Guest address
*       output. namelen - Guest length location.
*
*   RESULT
*       Zero on success; -1 on error.
*
****************************************************************************
*
*/

uae_u32 host_getpeername_real(struct socketbase *sb, uae_u32 sd, uae_u32 name, uae_u32 namelen) {
  struct Library *SocketBase;
  int s;
  int len;
	struct sockaddr *rp_name;

  BSDLOG("======== %s ========\n",__func__);
  BSDLOG("NOT YET TESTED!\n");

  if(!(SocketBase=getsocketbase(sb))) {
    seterrno(sb, ENETDOWN);
    return -1; /* TODO!*/
  }

	sd++;
	len = get_long(namelen);
	
	BSDLOG("sd %d, name 0x%lx, len %d\n", sd, name, len);
	
	s = getsock(sb,sd);
	
	if (s == INVALID_SOCKET) {
    BSDLOG("ERROR: INVALID_SOCKET\n");
    return -1;
  }

  rp_name = (struct sockaddr *)get_real_address(name);
		
  if (getpeername(s,rp_name,&len)) {
			sb->sb_errno=errno;
			BSDLOG("ERROR: failed (%d)\n",sb->sb_errno);
      return -1;
  }
  
  BSDLOG("received len: %d\n",len);
  prepamigaaddr(rp_name,len);
  put_long(namelen,len);

  return 0;
}

/****i* januae-bsdsock/host_getpeername ************************************
*
*   NAME
*       host_getpeername() -- Dispatch a peer socket address query.
*
*   SYNOPSIS
*       uae_u32 host_getpeername(struct socketbase *sb, uae_u32 sd, uae_u32
*       name, uae_u32 namelen);
*
*   FUNCTION
*       Submit the peer-address query to the socket worker.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. name - Address
*       destination. namelen - Length location.
*
*   RESULT
*       Worker result from execute_in_task().
*
****************************************************************************
*
*/

uae_u32 host_getpeername(struct socketbase *sb, uae_u32 sd, uae_u32 name, uae_u32 namelen) {

  BSDLOG("======== %s ========\n",__func__);

  return execute_in_task(sb, BSD_getpeername, 0,
                         (ULONG) sb, (ULONG) sd, (ULONG) name, (ULONG) namelen, (ULONG) 0, 
                         (ULONG) 0, (ULONG) 0, (ULONG) 0);
}

/****i* januae-bsdsock/read_blocking ***************************************
*
*   NAME
*       read_blocking() -- Read a socket blocking flag.
*
*   SYNOPSIS
*       static int read_blocking(struct socketbase *sb, int s);
*
*   FUNCTION
*       Determine whether the guest descriptor corresponding to a native
*       socket has SF_BLOCKING set.
*
*   INPUTS
*       sb - Socket context. s - Native socket descriptor.
*
*   RESULT
*       1 when marked blocking; otherwise 0.
*
****************************************************************************
*
*/

static int read_blocking(struct socketbase *sb, int s) {
  struct Library *SocketBase;
  uae_u32 sd;

  BSDLOG("socket: %d\n", s);

  sd = getsd (sb, s);

  if(!(SocketBase=getsocketbase(sb))) {
    return 0; /* TODO!*/
  }

  if(sb->ftable[sd-1] & SF_BLOCKING) {
    return 1;
  }
  return 0;
}

/****i* januae-bsdsock/set_blocking ****************************************
*
*   NAME
*       set_blocking() -- Change native socket blocking mode.
*
*   SYNOPSIS
*       static void set_blocking(struct socketbase *sb, int s, int value);
*
*   FUNCTION
*       Set the native FIONBIO mode through IoctlSocket.
*
*   INPUTS
*       sb - Socket context. s - Native socket descriptor. value - Nonzero
*       for blocking mode, zero for nonblocking.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void set_blocking(struct socketbase *sb, int s, int value) {
  struct Library *SocketBase;
  int nonblocking;
  //uae_u32 sd;


  //sd = getsd (sb, s);

  if(!(SocketBase=getsocketbase(sb))) {
    return; /* TODO!*/
  }

  if (value) {
    BSDLOG("set native socket %d to BLOCKING\n", s);
    nonblocking=0;
    if( !IoctlSocket(s, FIONBIO, (char *) &nonblocking)) {
      //BSDLOG(("sb->ftable[sd-1]: blocking\n"));
      //sb->ftable[sd-1] |= SF_BLOCKING;
      return;
    }
  }
  else {
    nonblocking=1;
    BSDLOG("set native socket %d to NON_BLOCKING\n", s);
    if(!IoctlSocket(s, FIONBIO, (char *) &nonblocking)) {
      //sb->ftable[sd-1] &= ~SF_BLOCKING;
      //BSDLOG(("sb->ftable[sd-1]: nonblocking\n"));
      return;
    }
  }

  BSDLOG("ERROR on IoctlSocket call: %d\n", errno); /* errno TODO */
}

/****i* januae-bsdsock/host_IoctlSocket ************************************
*
*   NAME
*       host_IoctlSocket() -- Handle a guest socket control request.
*
*   SYNOPSIS
*       uae_u32 host_IoctlSocket(struct socketbase *sb, uae_u32 sd, uae_u32
*       request, uae_u32 arg);
*
*   FUNCTION
*       Report an unimplemented ioctl request; active code does not read or
*       write the supplied argument.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor. request - Control code.
*       arg - Guest argument.
*
*   RESULT
*       -1.
*
****************************************************************************
*
*/

uae_u32 host_IoctlSocket(struct socketbase *sb, uae_u32 sd, uae_u32 request, uae_u32 arg)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
  return -1;
}

/***********************************
 * CloseSocket
 ***********************************/

/****i* januae-bsdsock/host_CloseSocket ************************************
*
*   NAME
*       host_CloseSocket() -- Dispatch socket closure.
*
*   SYNOPSIS
*       int host_CloseSocket(struct socketbase *sb, int sd);
*
*   FUNCTION
*       Send a CloseSocket request to the socket worker.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor.
*
*   RESULT
*       Worker result from execute_in_task().
*
****************************************************************************
*
*/

int host_CloseSocket(struct socketbase *sb, int sd) {

  BSDLOG("======== %s ========\n",__func__);

  return execute_in_task(sb, BSD_CloseSocket, 0,
                         (ULONG) sb, (ULONG) sd, 0, 0, 0,
                         (ULONG) 0, (ULONG) 0, (ULONG) 0);
}

/****i* januae-bsdsock/host_CloseSocket_real *******************************
*
*   NAME
*       host_CloseSocket_real() -- Close a native socket.
*
*   SYNOPSIS
*       int host_CloseSocket_real(struct socketbase *sb, int sd);
*
*   FUNCTION
*       Resolve a guest descriptor and invoke the socket library CloseSocket
*       function.
*
*   INPUTS
*       sb - Socket context. sd - Guest descriptor.
*
*   RESULT
*       Native CloseSocket result; -1 when the socket library cannot be
*       obtained.
*
****************************************************************************
*
*/

int host_CloseSocket_real(struct socketbase *sb, int sd) {

  struct Library *SocketBase;
  int s;

  BSDLOG("======== %s -> %s ========\n", __FILE__,__func__);

  if(!(SocketBase=getsocketbase(sb))) {
    seterrno(sb, ENETDOWN);
    sb->sb_errno=EBADF;
    return -1; 
  }

	sd++;
  s = getsock(sb,sd);

  BSDLOG("call CloseSocket(%d)\n",s);

  return CloseSocket(s);
}

 

// For the sake of efficiency, we do not malloc() the fd_sets here.
// 64 sockets should be enough for everyone.

/****i* januae-bsdsock/host_WaitSelect *************************************
*
*   NAME
*       host_WaitSelect() -- Handle multiplexed socket waiting.
*
*   SYNOPSIS
*       void host_WaitSelect(TrapContext *context, struct socketbase *sb,
*       uae_u32 nfds, uae_u32 readfds, uae_u32 writefds, uae_u32 exceptfds,
*       uae_u32 timeout, uae_u32 sigmp);
*
*   FUNCTION
*       Emit a not-implemented diagnostic without performing a native select
*       or guest-signal wait.
*
*   INPUTS
*       context - Trap context. sb - Socket context. nfds - Descriptor
*       bound. readfds, writefds, exceptfds - Guest sets. timeout - Time
*       limit. sigmp - Guest signals.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void host_WaitSelect(TrapContext *context, struct socketbase *sb, uae_u32 nfds, uae_u32 readfds, uae_u32 writefds, uae_u32 exceptfds, uae_u32 timeout, uae_u32 sigmp)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
}

/***********************************
 * Inet_NtoA
 ***********************************/

/****i* januae-bsdsock/host_Inet_NtoA_real *********************************
*
*   NAME
*       host_Inet_NtoA_real() -- Convert an IPv4 address to text.
*
*   SYNOPSIS
*       uae_u32 host_Inet_NtoA_real(TrapContext *context, struct socketbase
*       *sb, uae_u32 in);
*
*   FUNCTION
*       Convert the supplied IPv4 value using the native library and copy
*       the result to guest scratch storage.
*
*   INPUTS
*       context - Trap context. sb - Socket context. in - IPv4 value.
*
*   RESULT
*       Guest scratch-buffer address on success; zero on failure.
*
****************************************************************************
*
*/

uae_u32 host_Inet_NtoA_real(TrapContext *context, struct socketbase *sb, uae_u32 in) {

  struct Library *SocketBase;
  char *addr;
	uae_u32 scratchbuf;
  uae_u32 hin;


  BSDLOG("======== %s -> %s ========\n", __FILE__,__func__);
	BSDLOG("Inet_NtoA(%lx)\n",in);

  if(!(SocketBase=getsocketbase(sb))) {
    seterrno(sb, ENETDOWN);
    return 0;
  }

	hin = htonl(in);

	if ((addr = Inet_NtoA(hin)) != 0) {

		scratchbuf = m68k_areg (&regs, 6)+offsetof(struct UAEBSDBase, scratchbuf);
		BSDLOG("addr: %s\n",addr);
		strncpyha(scratchbuf, addr, SCRATCHBUFSIZE);
		return scratchbuf;
  }
	sb->sb_errno=errno;
  return 0;
	
}


/****i* januae-bsdsock/host_Inet_NtoA **************************************
*
*   NAME
*       host_Inet_NtoA() -- Dispatch IPv4 address formatting.
*
*   SYNOPSIS
*       uae_u32 host_Inet_NtoA(TrapContext *context, struct socketbase *sb,
*       uae_u32 in);
*
*   FUNCTION
*       Request native IPv4 text formatting through the socket worker.
*
*   INPUTS
*       context - Trap context. sb - Socket context. in - IPv4 address.
*
*   RESULT
*       Worker result from execute_in_task().
*
****************************************************************************
*
*/

uae_u32 host_Inet_NtoA(TrapContext *context, struct socketbase *sb, uae_u32 in) {

  BSDLOG("======== %s ========\n",__func__);

  return execute_in_task(sb, BSD_Inet_NtoA, 0, (ULONG) context, (ULONG) sb, (ULONG) in, 0, 0, 0, 0, 0);
}

/***********************************
 * inet_addr
 ***********************************/

/****i* januae-bsdsock/host_inet_addr_real *********************************
*
*   NAME
*       host_inet_addr_real() -- Parse a guest IPv4 address string.
*
*   SYNOPSIS
*       uae_u32 host_inet_addr_real(TrapContext *context, struct socketbase
*       *sb, uae_u32 cp);
*
*   FUNCTION
*       Read a guest string, parse it as an IPv4 address and return the
*       converted address value.
*
*   INPUTS
*       context - Trap context. sb - Socket context. cp - Guest string
*       address.
*
*   RESULT
*       Converted IPv4 value, or the implementation failure sentinel.
*
****************************************************************************
*
*/

uae_u32 host_inet_addr_real(TrapContext *context, struct socketbase *sb, uae_u32 cp) {

  struct Library *SocketBase;
	uae_u32 addr;
	char *cp_rp;

  BSDLOG("======== %s ========\n", __func__);

  if(!(SocketBase=getsocketbase(sb))) {
    return 0;
  }

  if (!addr_valid ("host_inet_addr", cp, 4)) {
    return 0;
  }

  BSDLOG("cp: %lx\n", cp);
	cp_rp = (char *) get_real_address(cp);

  BSDLOG("cp_rp: %s\n", cp_rp);

	addr = inet_addr(cp_rp);
  if(addr == INADDR_NONE) {
    BSDLOG("ERROR: inet_addr(%s) returned -1 (INADDR_NONE)!\n", cp_rp);
    sb->sb_errno=errno;
    return -1;
  }
	addr = htonl(inet_addr(cp_rp));

	BSDLOG("inet_addr(%s) returns 0x%08lx\n", cp_rp,addr);

	return addr;
}

/****i* januae-bsdsock/host_inet_addr **************************************
*
*   NAME
*       host_inet_addr() -- Dispatch IPv4 string parsing.
*
*   SYNOPSIS
*       uae_u32 host_inet_addr(TrapContext *context, struct socketbase *sb,
*       uae_u32 cp);
*
*   FUNCTION
*       Submit a guest IPv4 string address for parsing by the socket worker.
*
*   INPUTS
*       context - Trap context. sb - Socket context. cp - Guest string
*       address.
*
*   RESULT
*       Worker result from execute_in_task().
*
****************************************************************************
*
*/

uae_u32 host_inet_addr(TrapContext *context, struct socketbase *sb, uae_u32 cp) {

  BSDLOG("======== %s ========\n",__func__);

  return execute_in_task(sb, BSD_inet_addr, 0, (ULONG) context, (ULONG) sb, (ULONG) cp, 0, 0, 0, 0, 0);
}


/***********************************
 * gethostbynameaddr
 ***********************************/

/****i* januae-bsdsock/host_gethostbynameaddr_real *************************
*
*   NAME
*       host_gethostbynameaddr_real() -- Resolve a host name or address.
*
*   SYNOPSIS
*       void host_gethostbynameaddr_real(TrapContext *context, struct
*       socketbase *sb, uae_u32 name68k, uae_u32 namelen, long addrtype);
*
*   FUNCTION
*       Query the native host database and encode the resulting hostent
*       information for the guest.
*
*   INPUTS
*       context - Trap context. sb - Socket context. name68k - Guest
*       name/address. namelen - Input length. addrtype - Query selector.
*
*   RESULT
*       No C value is returned.
*
*   NOTES
*       Guest result memory and socket-context status carry the lookup
*       result.
*
****************************************************************************
*
*/

void host_gethostbynameaddr_real(TrapContext *context, struct socketbase *sb, uae_u32 name68k, uae_u32 namelen, long addrtype) {

  struct Library *SocketBase;
  char *name="";
  long net;
  struct hostent *h;
	int size, numaliases = 0, numaddr = 0;
	uae_u32 aptr;
	int i;

  BSDLOG("======== %s -> %s ========\n", __FILE__,__func__);
  if(!(SocketBase=getsocketbase(sb))) {
		seterrno(sb,ENETDOWN); /*???*/
    return;
  }

  BSDLOG("addrtype: %lx\n", addrtype);

  if(addr_valid ("host_gethostbynameaddr", name68k, 1)) {
    name = (char *) get_real_address(name68k);
  }
  else {
    BSDLOG("ERROR: addr_valid is not valid!\n");
  }

  if(addrtype==-1) {
    /* gethostbyname */
    BSDLOG("gethostbyname(%s)\n", name);

    h=gethostbyname(name);
  }
  else {
    BSDLOG("gethostbyaddr(%lx)\n", (ULONG) name);
    h=gethostbyaddr(name, namelen, addrtype);
  }

  BSDLOG("h: %lx\n", h);
  if(!h) {
    BSDLOG("error: h is NULL\n");
    sb->sb_errno=errno;
    sb->resultval = -1;
    return;
  }

  // compute total size of hostent
  size = 28;
  if (h->h_name != NULL) size += strlen(h->h_name)+1;

  if (h->h_aliases != NULL)
    while (h->h_aliases[numaliases])  {
      BSDLOG("alias[%d]: %s\n", numaliases, h->h_aliases[numaliases]);
      size += strlen(h->h_aliases[numaliases++])+5;
    }

  if (h->h_addr_list != NULL) {
    while (h->h_addr_list[numaddr]) numaddr++;
    size += numaddr*(h->h_length+4);
  }

  if (sb->hostent) {
    uae_FreeMem(context, sb->hostent, sb->hostentsize );
  }

  sb->hostent = uae_AllocMem(context, size, 0 );

  if (!sb->hostent) {
    write_log("BSDSOCK: WARNING - gethostby%s() ran out of Amiga memory (couldn't allocate %ld bytes) while returning result of lookup for '%s'\n",addrtype == -1 ? "name" : "addr",size,(char *)name);
    seterrno(sb,12); // ENOMEM
    return;
  }
  
  sb->hostentsize = size;
  
  aptr = sb->hostent+28+numaliases*4+numaddr*4;

  // transfer hostent to Amiga memory
  put_long(sb->hostent+4,sb->hostent+20);
  put_long(sb->hostent+8,h->h_addrtype);
  put_long(sb->hostent+12,h->h_length);
  put_long(sb->hostent+16,sb->hostent+24+numaliases*4);
  
  for (i = 0; i < numaliases; i++) {
    put_long(sb->hostent+20+i*4,addstr(&aptr,h->h_aliases[i]));
  }

  put_long(sb->hostent+20+numaliases*4,0);

  for (i = 0; i < numaddr; i++) {
    put_long(sb->hostent+24+(numaliases+i)*4,addmem(&aptr,h->h_addr_list[i],h->h_length));
  }
  put_long(sb->hostent+24+numaliases*4+numaddr*4,0);
  put_long(sb->hostent,aptr);
  addstr(&aptr,h->h_name);

  BSDLOG("OK (%s)\n",h->h_name);
  seterrno(sb,0);


}

/* go to according task and call back _real .. */
/****i* januae-bsdsock/host_gethostbynameaddr ******************************
*
*   NAME
*       host_gethostbynameaddr() -- Dispatch host database lookup.
*
*   SYNOPSIS
*       void host_gethostbynameaddr(TrapContext *context, struct socketbase
*       *sb, uae_u32 name68k, uae_u32 namelen, long addrtype);
*
*   FUNCTION
*       Submit a host name/address query to the socket worker.
*
*   INPUTS
*       context - Trap context. sb - Socket context. name68k, namelen,
*       addrtype - Lookup parameters.
*
*   RESULT
*       No C value is returned.
*
****************************************************************************
*
*/

void host_gethostbynameaddr(TrapContext *context, struct socketbase *sb, uae_u32 name68k, uae_u32 namelen, long addrtype) {

  BSDLOG("======== %s ========\n",__func__);

  execute_in_task(sb, BSD_gethostbynameaddr, 0,
                  (ULONG) context, (ULONG) sb, (ULONG) name68k, (ULONG) namelen, (ULONG) addrtype, 0, 0, 0);
  return;
}

/***********************************
 * getprotobyname
 ***********************************/

/****i* januae-bsdsock/host_getprotobyname_real ****************************
*
*   NAME
*       host_getprotobyname_real() -- Resolve a protocol by name.
*
*   SYNOPSIS
*       void host_getprotobyname_real(TrapContext *context, struct
*       socketbase *sb, uae_u32 name);
*
*   FUNCTION
*       Query the native protocol database and encode the protoent record
*       for guest access.
*
*   INPUTS
*       context - Trap context. sb - Socket context. name - Guest protocol
*       name.
*
*   RESULT
*       No C value is returned.
*
****************************************************************************
*
*/

void host_getprotobyname_real(TrapContext *context, struct socketbase *sb, uae_u32 name) {
  struct Library *SocketBase;
	char *name_rp="";
  struct protoent *p;

	int size, numaliases = 0;
	uae_u32 aptr;
	int i;

  BSDLOG("======== %s -> %s ========\n", __FILE__,__func__);
  if(!(SocketBase=getsocketbase(sb))) {
		seterrno(sb,ENETDOWN); /*???*/
    return;
  }

  BSDLOG("entered(name %lx, sb %lx)\n", name, sb);

  if (addr_valid ("host_gethostbynameaddr", name, 1)) {
    name_rp = (char *) get_real_address(name);
  }

	BSDLOG("getprotobyname(%s)\n",name_rp);

  p=getprotobyname(name_rp); /* this returns a static buffer, no need to free it */

  if(!p) {
    BSDLOG("getprotobyname ERROR!\n");
    sb->sb_errno=errno;
    return;
  }

  // compute total size of protoent
  size = 16;
  if (p->p_name != NULL) size += strlen(p->p_name)+1;

  if (p->p_aliases != NULL) {
    while (p->p_aliases[numaliases]) {
      size += strlen(p->p_aliases[numaliases++])+5;
    }
  }

  if (sb->protoent) {
    uae_FreeMem(context, sb->protoent, sb->protoentsize); /* free last protoent, necessary? */
  }

  sb->protoent = uae_AllocMem(context, size, 0); /* get amiga memory */

  if (!sb->protoent) {
    write_log("BSDSOCK: WARNING - getprotobyname() ran out of Amiga memory (couldn't allocate %ld bytes) while returning result of lookup for '%s'\n",size,(char *)name);
    BSDLOG("BSDSOCK: WARNING - getprotobyname() ran out of Amiga memory (couldn't allocate %ld bytes) while returning result of lookup for '%s'\n",size,(char *)name);
    seterrno(sb,12); // ENOMEM
    return;
  }

  sb->protoentsize = size;
  
  aptr = sb->protoent+16+numaliases*4;

  // transfer protoent to Amiga memory
  put_long(sb->protoent+4,sb->protoent+12);
  put_long(sb->protoent+8,p->p_proto);
  
  for (i = 0; i < numaliases; i++) put_long(sb->protoent+12+i*4,addstr(&aptr,p->p_aliases[i]));
  put_long(sb->protoent+12+numaliases*4,0);
  put_long(sb->protoent,aptr);
  addstr(&aptr,p->p_name);
  BSDLOG("OK (%s, %lx)\n",p->p_name,p->p_proto);
  seterrno(sb,0);

  return;
}


/****i* januae-bsdsock/host_getprotobyname *********************************
*
*   NAME
*       host_getprotobyname() -- Dispatch protocol name lookup.
*
*   SYNOPSIS
*       void host_getprotobyname(TrapContext *context, struct socketbase
*       *sb, uae_u32 name);
*
*   FUNCTION
*       Submit a protocol-name query to the socket worker.
*
*   INPUTS
*       context - Trap context. sb - Socket context. name - Guest protocol
*       name.
*
*   RESULT
*       No C value is returned.
*
****************************************************************************
*
*/

void host_getprotobyname(TrapContext *context, struct socketbase *sb, uae_u32 name) {

  BSDLOG("======== %s ========\n",__func__);

  execute_in_task(sb, BSD_getprotobyname, 0,
                  (ULONG) context, (ULONG) sb, (ULONG) name, 0, 0, 0, 0, 0);
  return;
}


/****i* januae-bsdsock/host_getservbynameport ******************************
*
*   NAME
*       host_getservbynameport() -- Handle service database lookup.
*
*   SYNOPSIS
*       void host_getservbynameport(TrapContext *context, struct socketbase
*       *sb, uae_u32 nameport, uae_u32 proto, uae_u32 type);
*
*   FUNCTION
*       Emit a not-implemented diagnostic without resolving a service name
*       or port.
*
*   INPUTS
*       context - Trap context. sb - Socket context. nameport - Service name
*       or port. proto - Protocol. type - Query mode.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void host_getservbynameport(TrapContext *context, struct socketbase *sb, uae_u32 nameport, uae_u32 proto, uae_u32 type)
{
  BSDLOG("NOT YET IMPLEMENTED!\n");
}

/***********************************
 * gethostname
 ***********************************/

/****i* januae-bsdsock/host_gethostname_real *******************************
*
*   NAME
*       host_gethostname_real() -- Read the local host name.
*
*   SYNOPSIS
*       uae_u32 host_gethostname_real(struct socketbase *sb, uae_u32 name,
*       uae_u32 namelen);
*
*   FUNCTION
*       Copy the host system name into the supplied guest buffer through the
*       socket library.
*
*   INPUTS
*       sb - Socket context. name - Guest output buffer. namelen - Buffer
*       length.
*
*   RESULT
*       Native gethostname status.
*
****************************************************************************
*
*/

uae_u32 host_gethostname_real(struct socketbase *sb, uae_u32 name, uae_u32 namelen) {
  struct Library *SocketBase;
  int ret;

  BSDLOG("======== %s ========\n", __func__);
  BSDLOG("NOT YET TESTED!\n");

  if(!(SocketBase=getsocketbase(sb))) {
		seterrno(sb,ENETDOWN); /*???*/
    return -1;
  }

  ret=gethostname((char *) get_real_address(name),namelen);

  if(!ret) {
    sb->sb_errno=errno;
    BSDLOG("ERROR: %d\n", errno);
  }
  else {
    BSDLOG("name: %s\n", get_real_address(name));
  }

  return ret;
}

/****i* januae-bsdsock/host_gethostname ************************************
*
*   NAME
*       host_gethostname() -- Dispatch local host name lookup.
*
*   SYNOPSIS
*       uae_u32 host_gethostname(struct socketbase *sb, uae_u32 name,
*       uae_u32 namelen);
*
*   FUNCTION
*       Submit a hostname request to the socket worker.
*
*   INPUTS
*       sb - Socket context. name - Guest output buffer. namelen - Buffer
*       size.
*
*   RESULT
*       Worker result from execute_in_task().
*
****************************************************************************
*
*/

uae_u32 host_gethostname(struct socketbase *sb, uae_u32 name, uae_u32 namelen) {

  BSDLOG("======== %s ========\n",__func__);

  return execute_in_task(sb, BSD_gethostname, 0,
                         (ULONG) sb, (ULONG) name, (ULONG) namelen, 0, 0, 0, 0, 0);
}
  
/****i* januae-bsdsock/host_getprotobynumber *******************************
*
*   NAME
*       host_getprotobynumber() -- Handle protocol number lookup.
*
*   SYNOPSIS
*       void host_getprotobynumber (TrapContext *context, struct socketbase
*       *sb, uae_u32 any);
*
*   FUNCTION
*       Emit a not-implemented diagnostic without resolving the supplied
*       protocol number.
*
*   INPUTS
*       context - Trap context. sb - Socket context. any - Protocol number.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void host_getprotobynumber (TrapContext *context, struct socketbase *sb, uae_u32 any) {
  BSDLOG("NOT YET IMPLEMENTED!\n");
}

/****i* januae-bsdsock/aros_bsdsocket_kill_thread **************************
*
*   NAME
*       aros_bsdsocket_kill_thread() -- Request termination of a socket
*       worker.
*
*   SYNOPSIS
*       void aros_bsdsocket_kill_thread(struct socketbase *sb);
*
*   FUNCTION
*       Submit BSD_killme to the worker associated with the socket context.
*
*   INPUTS
*       sb - Socket context.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

void aros_bsdsocket_kill_thread(struct socketbase *sb) {

  BSDLOG("======== %s ========\n",__func__);

  execute_in_task(sb, BSD_killme, 0,
                  (ULONG) sb, 0, 0, 0, 0, 0, 0, 0);
  return;
}

/****i* januae-bsdsock/aros_bsdsocket_kill_thread_real *********************
*
*   NAME
*       aros_bsdsocket_kill_thread_real() -- Release socket worker
*       resources.
*
*   SYNOPSIS
*       void aros_bsdsocket_kill_thread_real(struct socketbase *sb);
*
*   FUNCTION
*       Delete the socket-context memory pool and close its cached
*       bsdsocket.library handle.
*
*   INPUTS
*       sb - Socket context.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       The worker processes this operation before terminating its message
*       loop.
*
****************************************************************************
*
*/

void aros_bsdsocket_kill_thread_real(struct socketbase *sb) {

  BSDLOG("======== %s ========\n",__func__);

  BSDLOG("sb: %lx\n", sb);

  if(sb->mempool) {
    BSDLOG("mempool: %lx\n", sb->mempool);
    DeletePool(sb->mempool);
    sb->mempool=NULL;
  }

  /* don't use getsocketbase here. It will open the library, if it was not opened before. */
  if( sb->bsdsocketbase ) {
    BSDLOG("close library..\n");
    CloseLibrary(sb->bsdsocketbase);
    sb->bsdsocketbase=NULL;
  }

  BSDLOG("SocketBase closed, mempool deleted\n");
}

#endif /* BSDSOCKET */
