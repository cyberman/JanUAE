/************************************************************************ 
 *
 * Copyright 2003-2005 Richard Drummond
 * Copyright 2009 Oliver Brunner - aros<at>oliver-brunner.de
 *
 * Thread and semaphore support using AmigaOS processes
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

#include "sysconfig.h"
#include "sysdeps.h"

#include <exec/exec.h>
#include <exec/ports.h>
#include <exec/lists.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <clib/alib_protos.h>

#include "threaddep/thread.h"

/*
 * Simple emulation of POSIX semaphores using message-passing
 * with a proxy task.
 *
 */

//#define THREAD_DEBUG 1
#ifdef  THREAD_DEBUG
#define DEBUG_LOG(...) do { kprintf("%s:%d %s(): ",__FILE__,__LINE__,__func__);kprintf(__VA_ARGS__); } while(0)
#else
#define DEBUG_LOG(...) do ; while(0)
#endif

struct Process *proxy_thread;
static struct MsgPort *proxy_msgport;

static int sem_count;

/*
 * Message packet which is sent to the proxy thread
 * to effect blocking on a semaphore
 */

/*
 * Message types
 */
typedef enum {
    PROXY_MSG_BLOCK,
    PROXY_MSG_UNBLOCK,
    PROXY_MSG_DESTROY,
} ProxyMsgType;

struct ProxyMsg {
    struct Message     msg;
    struct PSemaphore *sem;
    struct Task       *reply_task;
    ProxyMsgType       type;
};

/* Proxy replies to message sender with this signal */
#define PROXY_MSG_REPLY_SIG		SIGBREAKF_CTRL_F

/****i* januae-thread/init_proxy_msg ***************************************
*
*   NAME
*       init_proxy_msg() -- Initialize a semaphore-proxy message.
*
*   SYNOPSIS
*       STATIC_INLINE void init_proxy_msg (struct ProxyMsg *msg,
*       ProxyMsgType type, struct PSemaphore *sem);
*
*   FUNCTION
*       Clear a proxy message and populate its length, command, target
*       semaphore and originating task.
*
*   INPUTS
*       msg - Message to initialize. type - Proxy command. sem - Target
*       semaphore.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

STATIC_INLINE void init_proxy_msg (struct ProxyMsg *msg, ProxyMsgType type, struct PSemaphore *sem)
{
    memset (msg, 0, sizeof (struct ProxyMsg));

    msg->msg.mn_Length = sizeof (struct ProxyMsg);
    msg->sem           = sem;
    msg->type          = type;
    msg->reply_task    = FindTask (NULL);
}

/****i* januae-thread/put_proxy_msg ****************************************
*
*   NAME
*       put_proxy_msg() -- Send a message to the semaphore proxy.
*
*   SYNOPSIS
*       STATIC_INLINE void put_proxy_msg (struct ProxyMsg *msg);
*
*   FUNCTION
*       Queue the supplied message on the shared proxy message port.
*
*   INPUTS
*       msg - Initialized proxy message.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

STATIC_INLINE void put_proxy_msg (struct ProxyMsg *msg)
{
    PutMsg (proxy_msgport, (struct Message *)msg);
}

/****i* januae-thread/wait_proxy_reply *************************************
*
*   NAME
*       wait_proxy_reply() -- Wait for a semaphore-proxy reply signal.
*
*   SYNOPSIS
*       STATIC_INLINE BOOL wait_proxy_reply (void);
*
*   FUNCTION
*       Wait for SIGBREAKF_CTRL_F and test whether the reply signal was
*       received.
*
*   INPUTS
*       None.
*
*   RESULT
*       TRUE if the reply signal is present; otherwise FALSE.
*
****************************************************************************
*
*/

STATIC_INLINE BOOL wait_proxy_reply (void)
{
    BOOL  got_reply = FALSE;
    ULONG sigs;

    sigs = Wait (PROXY_MSG_REPLY_SIG);

    if (sigs & PROXY_MSG_REPLY_SIG)
	got_reply = TRUE;

    return got_reply;
}

/*
 * Do proxy's reply to a semaphore message
 */
/****i* januae-thread/do_proxy_reply ***************************************
*
*   NAME
*       do_proxy_reply() -- Reply to a pending proxy message.
*
*   SYNOPSIS
*       STATIC_INLINE void do_proxy_reply (struct ProxyMsg *msg);
*
*   FUNCTION
*       Reply to the Exec message and signal its originating task with
*       SIGBREAKF_CTRL_F.
*
*   INPUTS
*       msg - Message whose sender is to be awakened.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

STATIC_INLINE void do_proxy_reply (struct ProxyMsg *msg)
{
   ReplyMsg ((struct Message *)msg);

   Signal (msg->reply_task, PROXY_MSG_REPLY_SIG);
}



/*
 * Block current process on semaphore
 */
/****i* januae-thread/BlockMe **********************************************
*
*   NAME
*       BlockMe() -- Block the current task on a semaphore.
*
*   SYNOPSIS
*       static int BlockMe (struct PSemaphore *psem);
*
*   FUNCTION
*       Send a block request to the proxy and wait for its reply signal; if
*       the wait does not report the reply signal, remove the pending
*       request from its queue.
*
*   INPUTS
*       psem - Semaphore on which to block.
*
*   RESULT
*       0 on receipt of a proxy reply; otherwise -1.
*
****************************************************************************
*
*/

static int BlockMe (struct PSemaphore *psem)
{
    int result = -1;
    struct ProxyMsg msg;

    init_proxy_msg (&msg, PROXY_MSG_BLOCK, psem);

    Forbid ();

    put_proxy_msg (&msg);
    if (wait_proxy_reply ())
	result = 0;
    else {
	/* Hack - remove block request from semaphore's waiting list
	 * When the block is interrupted, the proxy won't do it */
	Remove ((struct Node *)&msg);
	msg.msg.mn_Node.ln_Succ = msg.msg.mn_Node.ln_Pred = NULL;
    }

    Permit ();

    return result;
}

/*
 * Unblock one process blocked on semaphore
 */
/****i* januae-thread/UnblockOne *******************************************
*
*   NAME
*       UnblockOne() -- Request release of one blocked task.
*
*   SYNOPSIS
*       static void UnblockOne (struct PSemaphore *psem);
*
*   FUNCTION
*       Send an unblock command for the semaphore and wait for the proxy
*       acknowledgement.
*
*   INPUTS
*       psem - Semaphore whose waiting queue is to be serviced.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void UnblockOne (struct PSemaphore *psem)
{
    struct ProxyMsg msg;

    init_proxy_msg (&msg, PROXY_MSG_UNBLOCK, psem);

    put_proxy_msg (&msg);
    wait_proxy_reply ();
}

/*
 * Destroy the semaphore block
 */
/****i* januae-thread/Destroy **********************************************
*
*   NAME
*       Destroy() -- Request cancellation of semaphore waiters.
*
*   SYNOPSIS
*       static void Destroy (struct PSemaphore *psem);
*
*   FUNCTION
*       Send the semaphore destruction command to the proxy and wait for its
*       acknowledgement.
*
*   INPUTS
*       psem - Semaphore whose blocked tasks are to be signalled.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void Destroy (struct PSemaphore *psem)
{
    struct ProxyMsg msg;

    init_proxy_msg (&msg, PROXY_MSG_DESTROY, psem);

    put_proxy_msg (&msg);
    wait_proxy_reply ();
}



/**
 ** Proxy thread
 **/

/****i* januae-thread/handle_startup_msg ***********************************
*
*   NAME
*       handle_startup_msg() -- Acknowledge the proxy process startup
*       message.
*
*   SYNOPSIS
*       static void handle_startup_msg (void);
*
*   FUNCTION
*       Wait for a startup message on the current process port and reply to
*       it.
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

static void handle_startup_msg (void)
{
    struct Process *self = (struct Process*) FindTask (NULL);
    WaitPort (&self->pr_MsgPort);
    ReplyMsg (GetMsg (&self->pr_MsgPort));
}

/****i* januae-thread/proxy_thread_main ************************************
*
*   NAME
*       proxy_thread_main() -- Run the semaphore message dispatcher.
*
*   SYNOPSIS
*       static void proxy_thread_main (void);
*
*   FUNCTION
*       Create the proxy port; queue block requests, release one waiter for
*       unblock requests, signal all waiters for destruction requests, and
*       exit on SIGBREAKF_CTRL_C.
*
*   INPUTS
*       None.
*
*   RESULT
*       No value is returned.
*
*   NOTES
*       Unanswered messages are replied to during shutdown, and the shared
*       proxy-task and port pointers are cleared.
*
****************************************************************************
*
*/

static void proxy_thread_main (void) {

  ULONG sigmask;
  int dont_exit = 1;
  struct ProxyMsg *msg = NULL;

  proxy_msgport = CreateMsgPort ();
  sigmask = SIGBREAKF_CTRL_C | (1 << proxy_msgport->mp_SigBit);

  /* Wait for and reply to startup msg */
  handle_startup_msg ();

  while (dont_exit) {
    DEBUG_LOG("wait...\n");
    ULONG sigs = Wait (sigmask);

    /*
      * Handle any semaphore message sent us
      */
    while ((msg = (struct ProxyMsg *) GetMsg (proxy_msgport))) {
      struct PSemaphore *psem = msg->sem;

      switch (msg->type) {
        case PROXY_MSG_BLOCK:
          /* Block requesting task on semaphore.
            *
            * We effect the block by not replying to this message now. Just
            * put the request in the waiting list and reply to it when we
            * get an unblock request for this semaphore. The process requesting
            * the block will sleep until we do (unless it is interrupted). */
          AddTail ((struct List*)&psem->wait_queue, (struct Node *)msg);
          break;

        case PROXY_MSG_UNBLOCK:
          /* A request to unblock a task on semaphore. */
          {
            struct ProxyMsg *wait_msg;

            Forbid ();

            /* If there's a task blocking on the semaphore, remove
              * it from the waiting list */
            wait_msg = (struct ProxyMsg *) RemHead ((struct List *)&psem->wait_queue);
            
            /* Reply to the unblock request */
            do_proxy_reply (msg);

            /* Reply to the block request - this will wake up the
              * blocked task. */
            if (wait_msg)
                do_proxy_reply (wait_msg);

            Permit ();
          }
          break;

        case PROXY_MSG_DESTROY:
          Forbid ();
          {
            struct ProxyMsg *wait_msg;
            
            while ((wait_msg = (struct ProxyMsg *) RemHead ((struct List *)&psem->wait_queue))) {
              /* Break the block for all tasks blocked on the semaphore */
              Signal (wait_msg->reply_task, SIGBREAKF_CTRL_C);
            }
          }
          do_proxy_reply (msg);
          Permit ();
          break;
        } /* switch (msg->type) */
      } /* while */

    /*
     * Check for break signal and exit if received
     */
    if ((sigs & SIGBREAKF_CTRL_C) != 0) {
      dont_exit = FALSE;
      DEBUG_LOG("dont_exit=FALSE\n");
    }

  } /* while (dont_exit) */

   /*
    * Clean up and exit
    */
  DEBUG_LOG("semaphore thread cleanup\n");

  Forbid ();
  {
    struct ProxyMsg *rem_msg;

    /* Reply to any unanswered requests */
    while ((rem_msg = (struct ProxyMsg *) GetMsg (proxy_msgport)) != NULL)
      do_proxy_reply (rem_msg);

    /* reply to the quit msg */
    if (msg != NULL)
      do_proxy_reply (msg);

    DeleteMsgPort (proxy_msgport);
    proxy_msgport = 0;
    proxy_thread  = 0;
  }
  Permit ();

  DEBUG_LOG("semaphore thread exit\n");
}

/****i* januae-thread/stop_proxy_thread ************************************
*
*   NAME
*       stop_proxy_thread() -- Request proxy-process termination.
*
*   SYNOPSIS
*       void stop_proxy_thread (void);
*
*   FUNCTION
*       Signal the active semaphore proxy process with SIGBREAKF_CTRL_C when
*       it exists.
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

void stop_proxy_thread (void) {
  if (proxy_thread)
    Signal ((struct Task *)proxy_thread, SIGBREAKF_CTRL_C);
}

/*
 * Start proxy thread
 */
/****i* januae-thread/start_proxy_thread ***********************************
*
*   NAME
*       start_proxy_thread() -- Create and synchronize the semaphore proxy.
*
*   SYNOPSIS
*       static int start_proxy_thread (void);
*
*   FUNCTION
*       Start an AmigaOS process for proxy_thread_main(), exchange its
*       startup acknowledgement, save its task pointer and register a
*       process-exit stop request.
*
*   INPUTS
*       None.
*
*   RESULT
*       0 when the proxy process starts; otherwise -1.
*
****************************************************************************
*
*/

static int start_proxy_thread (void) {
  int result = -1;
  struct MsgPort *replyport = CreateMsgPort();
  struct Process *p;

  if (replyport) {
    p = myCreateNewProcTags (NP_Name,	(ULONG) "j-uae semaphore proxy",
				 NP_Priority,		10,
				 NP_StackSize,		2048,
				 NP_Entry,	(ULONG) proxy_thread_main,
				 TAG_DONE);
    if (p) {
      /* Send startup message */
      struct Message msg;
      msg.mn_ReplyPort = replyport;
      msg.mn_Length    = sizeof msg;
      PutMsg (&p->pr_MsgPort, (struct Message*)&msg);
      WaitPort (replyport);
      
      proxy_thread = p;
      
      atexit (stop_proxy_thread);
      
      result = 0;
    }
    DeleteMsgPort (replyport);
  }
  return result;
}

/**
 ** Emulation of POSIX semaphore API
 ** (or, at least, UAE's version thereof).
 **/

/****i* januae-thread/uae_sem_init *****************************************
*
*   NAME
*       uae_sem_init() -- Initialize a UAE semaphore.
*
*   SYNOPSIS
*       int uae_sem_init (uae_sem_t *sem, int pshared, unsigned int value);
*
*   FUNCTION
*       Initialize the mutex and waiting list, set the initial count and
*       live flag, and start the shared proxy when creating the first
*       semaphore.
*
*   INPUTS
*       sem - Semaphore to initialize. pshared - Unused. value - Initial
*       semaphore count.
*
*   RESULT
*       0 on successful initialization; -1 if starting the first proxy
*       fails.
*
****************************************************************************
*
*/

int uae_sem_init (uae_sem_t *sem, int pshared, unsigned int value)
{
    int result = 0;

    InitSemaphore (&sem->mutex);
    NewList ((struct List *)&sem->wait_queue);
    sem->value = value;
    sem->live  = 1;

    if (sem_count == 0)
	result = start_proxy_thread ();

    if (result != -1)
	sem_count++;

    return result;
}

/****i* januae-thread/uae_sem_destroy **************************************
*
*   NAME
*       uae_sem_destroy() -- Deactivate a UAE semaphore.
*
*   SYNOPSIS
*       int uae_sem_destroy (uae_sem_t *sem);
*
*   FUNCTION
*       Mark the semaphore inactive, request cancellation of its waiters,
*       decrement the active-semaphore count and signal the proxy to stop
*       when the final semaphore is destroyed.
*
*   INPUTS
*       sem - Semaphore to deactivate.
*
*   RESULT
*       Always returns 0.
*
****************************************************************************
*
*/

int uae_sem_destroy (uae_sem_t *sem)
{
  DEBUG_LOG("uae_sem_destroy(%lx)\n", sem);
  ObtainSemaphore (&sem->mutex);

  sem->live = 0;
  Destroy (sem);

  /* If this is the last semaphore to die,
    * kill the proxy thread */
  if (--sem_count == 0) {
    stop_proxy_thread ();
  }

  DEBUG_LOG("uae_sem_destroy: %d sems left\n", sem_count);
  ReleaseSemaphore (&sem->mutex);

  return 0;
}

/****i* januae-thread/uae_sem_wait *****************************************
*
*   NAME
*       uae_sem_wait() -- Wait for a semaphore value.
*
*   SYNOPSIS
*       int uae_sem_wait (uae_sem_t *sem);
*
*   FUNCTION
*       Under the semaphore mutex, consume an available count or ask the
*       proxy to block the current task; on successful wake-up decrement the
*       count.
*
*   INPUTS
*       sem - Initialized semaphore.
*
*   RESULT
*       0 if the count was acquired; otherwise -1.
*
****************************************************************************
*
*/

int uae_sem_wait (uae_sem_t *sem)
{
    int result = -1;

    ObtainSemaphore (&sem->mutex);

    if (sem->live) {
	if (sem->value > 0) {
	    --sem->value;
	    result = 0;
        } else {
	    ReleaseSemaphore (&sem->mutex);

	    /* Block on this semaphore by waiting for
	     * the proxy thread to reply to our lock request
	     */
	    result = BlockMe (sem);

	    ObtainSemaphore (&sem->mutex);

	    if (result != -1)
		--sem->value;
	}
    }
    ReleaseSemaphore (&sem->mutex);
    return result;
}

/****i* januae-thread/uae_sem_trywait **************************************
*
*   NAME
*       uae_sem_trywait() -- Acquire a semaphore without blocking.
*
*   SYNOPSIS
*       int uae_sem_trywait (uae_sem_t *sem);
*
*   FUNCTION
*       Decrement the count only if the semaphore is live and its value is
*       positive.
*
*   INPUTS
*       sem - Initialized semaphore.
*
*   RESULT
*       0 if the count was acquired; otherwise -1.
*
****************************************************************************
*
*/

int uae_sem_trywait (uae_sem_t *sem)
{
    int result = -1;

    ObtainSemaphore (&sem->mutex);

    if (sem->live) {
	if (sem->value > 0) {
	    --sem->value;
	    result = 0;
        }
    }
    ReleaseSemaphore (&sem->mutex);
    return result;
}

/****i* januae-thread/uae_sem_get_value ************************************
*
*   NAME
*       uae_sem_get_value() -- Read the stored semaphore count.
*
*   SYNOPSIS
*       int uae_sem_get_value (uae_sem_t *sem);
*
*   FUNCTION
*       Read the value while holding the semaphore mutex.
*
*   INPUTS
*       sem - Initialized semaphore.
*
*   RESULT
*       The current stored count.
*
****************************************************************************
*
*/

int uae_sem_get_value (uae_sem_t *sem)
{
    int value;

    ObtainSemaphore (&sem->mutex);
    value = sem->value;
    ReleaseSemaphore (&sem->mutex);

    return value;
}

/****i* januae-thread/uae_sem_post *****************************************
*
*   NAME
*       uae_sem_post() -- Increment a semaphore and wake a waiter.
*
*   SYNOPSIS
*       int uae_sem_post (uae_sem_t *sem);
*
*   FUNCTION
*       Increase the count of a live semaphore and request one proxy wake-up
*       when the value changes from zero to one.
*
*   INPUTS
*       sem - Initialized semaphore.
*
*   RESULT
*       0 if posted to a live semaphore; otherwise -1.
*
****************************************************************************
*
*/

int uae_sem_post (uae_sem_t *sem)
{
    int result = -1;

    ObtainSemaphore (&sem->mutex);

    if (sem->live) {
	sem->value++;

	if (sem->value == 1) {
	    /* Ask the proxy to wake up a task blocked
	     * on this semaphore */
	    UnblockOne (sem);
	}
	result = 0;
    }
    ReleaseSemaphore (&sem->mutex);

    return result;
}


/**
 ** Thread support
 **/

struct startupmsg
{
    struct Message msg;
    void           *(*func) (void *);
    void           *arg;
};

/****i* januae-thread/do_thread ********************************************
*
*   NAME
*       do_thread() -- Run the entry point of a new UAE process.
*
*   SYNOPSIS
*       static void do_thread (void);
*
*   FUNCTION
*       Receive the startup message, save its function and argument,
*       acknowledge the sender and invoke the supplied function.
*
*   INPUTS
*       None; the function and argument arrive in the startup message.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

static void do_thread (void)
{
   struct Process *pr = (struct Process *) FindTask (NULL);
   struct startupmsg *msg;
   void *(*func) (void *);
   void *arg;

   WaitPort (&pr->pr_MsgPort);
   msg = (struct startupmsg *) GetMsg(&pr->pr_MsgPort);
   func = msg->func;
   arg  = msg->arg;
   ReplyMsg ((struct Message*)msg);

   func (arg);
}

/****i* januae-thread/uae_start_thread *************************************
*
*   NAME
*       uae_start_thread() -- Create an AmigaOS process for a UAE thread.
*
*   SYNOPSIS
*       int uae_start_thread (void *(*f) (void *), void *arg, uae_thread_id
*       *foo, char *name);
*
*   FUNCTION
*       Create a process running do_thread(), pass the supplied function and
*       argument through a startup message and wait for its acknowledgement.
*
*   INPUTS
*       f - Thread entry point. arg - Entry argument. foo - Storage for the
*       task identifier. name - Optional process name.
*
*   RESULT
*       Nonzero if the resulting task identifier is nonzero; otherwise zero.
*
*   NOTES
*       If the reply port cannot be created, this function does not
*       initialize the supplied task-identifier storage.
*
****************************************************************************
*
*/

int uae_start_thread (void *(*f) (void *), void *arg, uae_thread_id *foo, char *name) {
  struct MsgPort *replyport = CreateMsgPort();
  char default_name[]="J-UAE thread";

  DEBUG_LOG("start thread \"%s\" ..\n", name);

  if(!name) {
    name=default_name;
  }

  if (replyport) {
    *foo = (struct Task *)myCreateNewProcTags (//NP_Output,		   Output (),
             //NP_Input,		   Input (),
             NP_Name,	   (ULONG) name,
             //NP_CloseOutput,	   FALSE,
             //NP_CloseInput,	   FALSE,
             NP_StackSize,	   16384*4,
             NP_Entry,	   (ULONG) do_thread,
             TAG_DONE);

    if(*foo) {
      struct startupmsg msg;
      
      msg.msg.mn_ReplyPort = replyport;
      msg.msg.mn_Length    = sizeof msg;
      msg.func             = f;
      msg.arg              = arg;
      PutMsg (&((struct Process*)*foo)->pr_MsgPort, (struct Message*)&msg);
      WaitPort (replyport);
    }
    DeleteMsgPort (replyport);
  }

  DEBUG_LOG("thread \"%s\" started with id %lx\n", name, *foo);

  return *foo!=0;
}

/****i* januae-thread/uae_set_thread_priority ******************************
*
*   NAME
*       uae_set_thread_priority() -- Set the current task priority.
*
*   SYNOPSIS
*       extern void uae_set_thread_priority (int pri);
*
*   FUNCTION
*       Apply the requested priority to the calling AmigaOS task.
*
*   INPUTS
*       pri - New priority.
*
*   RESULT
*       No value is returned.
*
****************************************************************************
*
*/

extern void uae_set_thread_priority (int pri)
{
    SetTaskPri (FindTask (NULL), pri);
}
