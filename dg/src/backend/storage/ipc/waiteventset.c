/*-------------------------------------------------------------------------
 *
 * waiteventset.c
 *	  ppoll()/pselect() like abstraction
 *
 * WaitEvents are an abstraction for waiting for one or more events at a time.
 * The waiting can be done in a race free fashion, similar ppoll() or
 * pselect() (as opposed to plain poll()/select()).
 *
 * You can wait for:
 * - a latch being set by another thread, or by the thread itself
 *   (WL_LATCH_SET)
 * - data to become readable or writeable on a socket (WL_SOCKET_*)
 * - timeout (WL_TIMEOUT)
 *
 * Implementation
 * --------------
 *
 * digres runs on Linux alone (../sm/PART4.md section 10), and waits with
 * epoll().  A latch is the thread's wake fd from sm_sys.h, an eventfd that
 * a latch's setter writes through sm_interrupt(), or the thread itself
 * through sm_interrupt_self(), so a set latch is never missed: no signal is
 * sent.  PostgreSQL's poll(), kqueue() and Windows implementations, and the
 * self-pipe, are gone (WORKNOTE.md step 238).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/waiteventset.c
 *
 *-------------------------------------------------------------------------
 */
/* sm's header first: it includes the C++ library, which must be read
 * before PostgreSQL's macros. */
#include "sm.h"

#include "postgres.h"

#include <limits.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procsignal.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

/* typedef in waiteventset.h */
struct WaitEventSet {
  ResourceOwner owner;

  int nevents;       /* number of registered events */
  int nevents_space; /* maximum number of events in this set */

  /*
   * Array, of nevents_space length, storing the definition of events this
   * set is waiting for.
   */
  WaitEvent *events;

  /*
   * If WL_LATCH_SET is specified in any wait event, latch is a pointer to
   * said latch, and latch_pos the offset in the ->events array. This is
   * useful because we check the state of the latch before performing doing
   * syscalls related to waiting.
   */
  Latch *latch;
  int latch_pos;

  int epoll_fd;
  /* epoll_wait returns events in a user provided arrays, allocate once */
  struct epoll_event *epoll_ret_events;
};

/* The thread's wake fd (sm_sys.h), which sm0 closes as the thread ends. */
static thread_local /*LOCAL*/ int wake_fd = -1;

static void drain(void);

static void WaitEventAdjustEpoll(WaitEventSet *set, WaitEvent *event,
                                 int action);

static inline int WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
                                        WaitEvent *occurred_events,
                                        int nevents);

/* ResourceOwner support to hold WaitEventSets */
static void ResOwnerReleaseWaitEventSet(Datum res);

static const ResourceOwnerDesc wait_event_set_resowner_desc = {
    .name = "WaitEventSet",
    .release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
    .release_priority = RELEASE_PRIO_WAITEVENTSETS,
    .ReleaseResource = ResOwnerReleaseWaitEventSet,
    .DebugPrint = NULL};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void ResourceOwnerRememberWaitEventSet(ResourceOwner owner,
                                                     WaitEventSet *set) {
  ResourceOwnerRemember(owner, PointerGetDatum(set),
                        &wait_event_set_resowner_desc);
}
static inline void ResourceOwnerForgetWaitEventSet(ResourceOwner owner,
                                                   WaitEventSet *set) {
  ResourceOwnerForget(owner, PointerGetDatum(set),
                      &wait_event_set_resowner_desc);
}

/*
 * Close what InitializeWaitEventSupport() opened: a session thread, as it
 * ends, which no process exit does for it.
 */
void ShutdownWaitEventSupport(void) {
  /* sm_sys.h closes the fd itself; only fd.c's count of it goes. */
  if (wake_fd != -1) {
    wake_fd = -1;
    ReleaseExternalFD();
  }
}

/*
 * Initialize the process-local wait event infrastructure.
 *
 * This must be called once during startup of any process that can wait on
 * latches, before it issues any InitLatch() call.
 */
void InitializeWaitEventSupport(void) {

  /*
   * The thread's wake fd, made now: before the thread's first SetLatch()
   * writes it (sm_interrupt_self() cannot make it).
   */
  wake_fd = sm_wake_fd();
  if (wake_fd < 0)
    elog(FATAL, "could not make the wake fd: %s", sm_errmsg());
  ReserveExternalFD();
}

/*
 * Create a WaitEventSet with space for nevents different events to wait for.
 *
 * These events can then be efficiently waited upon together, using
 * WaitEventSetWait().
 *
 * The WaitEventSet is tracked by the given 'resowner'.  Use NULL for session
 * lifetime.
 */
WaitEventSet *CreateWaitEventSet(ResourceOwner resowner, int nevents) {
  WaitEventSet *set;
  char *data;
  Size sz = 0;

  /*
   * Use MAXALIGN size/alignment to guarantee that later uses of memory are
   * aligned correctly. E.g. epoll_event might need 8 byte alignment on some
   * platforms, but earlier allocations like WaitEventSet and WaitEvent
   * might not be sized to guarantee that when purely using sizeof().
   */
  sz += MAXALIGN(sizeof(WaitEventSet));
  sz += MAXALIGN(sizeof(WaitEvent) * nevents);

  sz += MAXALIGN(sizeof(struct epoll_event) * nevents);

  if (resowner != NULL)
    ResourceOwnerEnlarge(resowner);

  data = (char *)MemoryContextAllocZero(TopMemoryContext, sz);

  set = (WaitEventSet *)data;
  data += MAXALIGN(sizeof(WaitEventSet));

  set->events = (WaitEvent *)data;
  data += MAXALIGN(sizeof(WaitEvent) * nevents);

  set->epoll_ret_events = (struct epoll_event *)data;
  data += MAXALIGN(sizeof(struct epoll_event) * nevents);

  set->latch = NULL;
  set->nevents_space = nevents;

  if (resowner != NULL) {
    ResourceOwnerRememberWaitEventSet(resowner, set);
    set->owner = resowner;
  }

  if (!AcquireExternalFD())
    elog(ERROR, "AcquireExternalFD, for epoll_create1, failed: %m");
  set->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (set->epoll_fd < 0) {
    ReleaseExternalFD();
    elog(ERROR, "epoll_create1 failed: %m");
  }

  return set;
}

/*
 * Free a previously created WaitEventSet.
 *
 * Note: preferably, this shouldn't have to free any resources that could be
 * inherited across an exec().  If it did, we'd likely leak those resources in
 * many scenarios.  For the epoll case, we ensure that by setting EPOLL_CLOEXEC
 * when the FD is created.
 */
void FreeWaitEventSet(WaitEventSet *set) {
  if (set->owner) {
    ResourceOwnerForgetWaitEventSet(set->owner, set);
    set->owner = NULL;
  }

  close(set->epoll_fd);
  ReleaseExternalFD();

  pfree(set);
}

/* ---
 * Add an event to the set. Possible events are:
 * - WL_LATCH_SET: Wait for the latch to be set
 * - WL_SOCKET_READABLE: Wait for socket to become readable,
 *	 can be combined in one event with other WL_SOCKET_* events
 * - WL_SOCKET_WRITEABLE: Wait for socket to become writeable,
 *	 can be combined with other WL_SOCKET_* events
 * - WL_SOCKET_CONNECTED: Wait for socket connection to be established,
 *	 the same as WL_SOCKET_WRITEABLE
 * - WL_SOCKET_ACCEPT: Wait for new connection to a server socket,
 *	 the same as WL_SOCKET_READABLE
 * - WL_SOCKET_CLOSED: Wait for socket to be closed by remote peer.
 *
 * Returns the offset in WaitEventSet->events (starting from 0), which can be
 * used to modify previously added wait events using ModifyWaitEvent().
 *
 * In the WL_LATCH_SET case the latch must be owned by the current thread,
 * i.e. it must be a latch it initialized with InitLatch.
 *
 * In the WL_SOCKET_READABLE/WRITEABLE/CONNECTED/ACCEPT cases, EOF and error
 * conditions cause the socket to be reported as readable/writable/connected,
 * so that the caller can deal with the condition.
 *
 * The user_data pointer specified here will be set for the events returned
 * by WaitEventSetWait(), allowing to easily associate additional data with
 * events.
 */
int AddWaitEventToSet(WaitEventSet *set, uint32 events, pgsocket fd,
                      Latch *latch, void *user_data) {
  WaitEvent *event;

  /* not enough space */
  Assert(set->nevents < set->nevents_space);

  if (latch) {
    if (!LatchIsMine(latch))
      elog(ERROR, "cannot wait on a latch owned by another thread");
    if (set->latch)
      elog(ERROR, "cannot wait on more than one latch");
    if ((events & WL_LATCH_SET) != WL_LATCH_SET)
      elog(ERROR, "latch events only support being set");
  } else {
    if (events & WL_LATCH_SET)
      elog(ERROR, "cannot wait on latch without a specified latch");
  }

  /* waiting for socket readiness without a socket indicates a bug */
  if (fd == PGINVALID_SOCKET && (events & WL_SOCKET_MASK))
    elog(ERROR, "cannot wait on socket event without a socket");

  event = &set->events[set->nevents];
  event->pos = set->nevents++;
  event->fd = fd;
  event->events = events;
  event->user_data = user_data;

  if (events == WL_LATCH_SET) {
    set->latch = latch;
    set->latch_pos = event->pos;
    event->fd = wake_fd;
  }

  /* perform wait primitive specific initialization, if needed */
  WaitEventAdjustEpoll(set, event, EPOLL_CTL_ADD);

  return event->pos;
}

/*
 * Change the event mask and, in the WL_LATCH_SET case, the latch associated
 * with the WaitEvent.  The latch may be changed to NULL to disable the latch
 * temporarily, and then set back to a latch later.
 *
 * 'pos' is the id returned by AddWaitEventToSet.
 */
void ModifyWaitEvent(WaitEventSet *set, int pos, uint32 events, Latch *latch) {
  WaitEvent *event;

  Assert(pos < set->nevents);

  event = &set->events[pos];

  /*
   * If neither the event mask nor the associated latch changes, return
   * early. That's an important optimization for some sockets, where
   * ModifyWaitEvent is frequently used to switch from waiting for reads to
   * waiting on writes.
   */
  if (events == event->events &&
      (!(event->events & WL_LATCH_SET) || set->latch == latch))
    return;

  if (event->events & WL_LATCH_SET && events != event->events)
    elog(ERROR, "cannot modify latch event");

  /* FIXME: validate event mask */
  event->events = events;

  if (events == WL_LATCH_SET) {
    if (latch && !LatchIsMine(latch))
      elog(ERROR, "cannot wait on a latch owned by another thread");
    set->latch = latch;

    /*
     * The kernel object need not change: the wake fd is the thread's, the
     * same for any latch it waits on.
     */
    return;
  }

  WaitEventAdjustEpoll(set, event, EPOLL_CTL_MOD);
}

/*
 * action can be one of EPOLL_CTL_ADD | EPOLL_CTL_MOD | EPOLL_CTL_DEL
 */
static void WaitEventAdjustEpoll(WaitEventSet *set, WaitEvent *event,
                                 int action) {
  struct epoll_event epoll_ev;
  int rc;

  /* pointer to our event, returned by epoll_wait */
  epoll_ev.data.ptr = event;
  /* always wait for errors */
  epoll_ev.events = EPOLLERR | EPOLLHUP;

  /* prepare pollfd entry once */
  if (event->events == WL_LATCH_SET) {
    Assert(set->latch != NULL);
    epoll_ev.events |= EPOLLIN;
  } else {
    Assert(event->fd != PGINVALID_SOCKET);
    Assert(event->events &
           (WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE | WL_SOCKET_CLOSED));

    if (event->events & WL_SOCKET_READABLE)
      epoll_ev.events |= EPOLLIN;
    if (event->events & WL_SOCKET_WRITEABLE)
      epoll_ev.events |= EPOLLOUT;
    if (event->events & WL_SOCKET_CLOSED)
      epoll_ev.events |= EPOLLRDHUP;
  }

  /*
   * Even though unused, we also pass epoll_ev as the data argument if
   * EPOLL_CTL_DEL is passed as action.  There used to be an epoll bug
   * requiring that, and actually it makes the code simpler...
   */
  rc = epoll_ctl(set->epoll_fd, action, event->fd, &epoll_ev);

  if (rc < 0)
    ereport(ERROR, (errcode_for_socket_access(),
                    errmsg("%s() failed: %m", "epoll_ctl")));
}

/*
 * Wait for events added to the set to happen, or until the timeout is
 * reached.  At most nevents occurred events are returned.
 *
 * If timeout = -1, block until an event occurs; if 0, check sockets for
 * readiness, but don't block; if > 0, block for at most timeout milliseconds.
 *
 * Returns the number of events occurred, or 0 if the timeout was reached.
 *
 * Returned events will have the fd, pos, user_data fields set to the
 * values associated with the registered event.
 */
int WaitEventSetWait(WaitEventSet *set, long timeout,
                     WaitEvent *occurred_events, int nevents,
                     uint32 wait_event_info) {
  int returned_events = 0;
  instr_time start_time;
  instr_time cur_time;
  long cur_timeout = -1;

  Assert(nevents > 0);

  /*
   * Initialize timeout if requested.  We must record the current time so
   * that we can determine the remaining timeout if interrupted.
   */
  if (timeout >= 0) {
    INSTR_TIME_SET_CURRENT(start_time);
    Assert(timeout >= 0 && timeout <= INT_MAX);
    cur_timeout = timeout;
  } else
    INSTR_TIME_SET_ZERO(start_time);

  while (returned_events == 0) {
    int rc;

    /*
     * An interrupt woke us: run its handler, before looking at the latch
     * the handler may set.
     */
    DeliverInterrupts();

    /*
     * Check if the latch is set already first.  If so, we either exit
     * immediately or ask the kernel for further events available right
     * now without waiting, depending on how many events the caller wants.
     *
     * The latch is the thread's INTERRUPT_LATCH bit: a set after this
     * look raises it, then writes the wake fd, so the wait below returns.
     */
    if (set->latch && LatchIsSet(set->latch)) {
      occurred_events->fd = PGINVALID_SOCKET;
      occurred_events->pos = set->latch_pos;
      occurred_events->user_data = set->events[set->latch_pos].user_data;
      occurred_events->events = WL_LATCH_SET;
      occurred_events++;
      returned_events++;

      if (returned_events == nevents)
        break; /* output buffer full already */

      /*
       * Even though we already have an event, we'll poll just once with
       * zero timeout to see what non-latch events we can fit into the
       * output buffer at the same time.
       */
      cur_timeout = 0;
      timeout = 0;
    }

    /*
     * Wait for events using the readiness primitive chosen at the top of
     * this file. If -1 is returned, a timeout has occurred, if 0 we have
     * to retry, everything >= 1 is the number of returned events.
     */
    rc = WaitEventSetWaitBlock(set, cur_timeout, occurred_events,
                               nevents - returned_events);


    if (rc == -1)
      break; /* timeout occurred */
    else
      returned_events += rc;

    /* If we're not done, update cur_timeout for next iteration */
    if (returned_events == 0 && timeout >= 0) {
      INSTR_TIME_SET_CURRENT(cur_time);
      INSTR_TIME_SUBTRACT(cur_time, start_time);
      cur_timeout = timeout - (long)INSTR_TIME_GET_MILLISEC(cur_time);
      if (cur_timeout <= 0)
        break;
    }
  }
  return returned_events;
}

/*
 * Wait using linux's epoll_wait(2).
 *
 * This is the preferable wait method, as several readiness notifications are
 * delivered, without having to iterate through all of set->events. The return
 * epoll_event struct contain a pointer to our events, making association
 * easy.
 */
static inline int WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
                                        WaitEvent *occurred_events,
                                        int nevents) {
  int returned_events = 0;
  int rc;
  WaitEvent *cur_event;
  struct epoll_event *cur_epoll_event;

  /* Sleep */
  rc = epoll_wait(set->epoll_fd, set->epoll_ret_events,
                  Min(nevents, set->nevents_space), cur_timeout);

  /* Check return code */
  if (rc < 0) {
    /* EINTR is okay, otherwise complain */
    if (errno != EINTR) {
      ereport(ERROR, (errcode_for_socket_access(),
                      errmsg("%s() failed: %m", "epoll_wait")));
    }
    return 0;
  } else if (rc == 0) {
    /* timeout exceeded */
    return -1;
  }

  /*
   * At least one event occurred, iterate over the returned epoll events
   * until they're either all processed, or we've returned all the events
   * the caller desired.
   */
  for (cur_epoll_event = set->epoll_ret_events;
       cur_epoll_event < (set->epoll_ret_events + rc) &&
       returned_events < nevents;
       cur_epoll_event++) {
    /* epoll's data pointer is set to the associated WaitEvent */
    cur_event = (WaitEvent *)cur_epoll_event->data.ptr;

    occurred_events->pos = cur_event->pos;
    occurred_events->user_data = cur_event->user_data;
    occurred_events->events = 0;

    if (cur_event->events == WL_LATCH_SET &&
        cur_epoll_event->events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
      /* Drain the wake fd. */
      drain();

      if (set->latch && LatchIsSet(set->latch)) {
        occurred_events->fd = PGINVALID_SOCKET;
        occurred_events->events = WL_LATCH_SET;
        occurred_events++;
        returned_events++;
      }
    } else if (cur_event->events &
               (WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE | WL_SOCKET_CLOSED)) {
      Assert(cur_event->fd != PGINVALID_SOCKET);

      if ((cur_event->events & WL_SOCKET_READABLE) &&
          (cur_epoll_event->events & (EPOLLIN | EPOLLERR | EPOLLHUP))) {
        /* data available in socket, or EOF */
        occurred_events->events |= WL_SOCKET_READABLE;
      }

      if ((cur_event->events & WL_SOCKET_WRITEABLE) &&
          (cur_epoll_event->events & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
        /* writable, or EOF */
        occurred_events->events |= WL_SOCKET_WRITEABLE;
      }

      if ((cur_event->events & WL_SOCKET_CLOSED) &&
          (cur_epoll_event->events & (EPOLLRDHUP | EPOLLERR | EPOLLHUP))) {
        /* remote peer shut down, or error */
        occurred_events->events |= WL_SOCKET_CLOSED;
      }

      if (occurred_events->events != 0) {
        occurred_events->fd = cur_event->fd;
        occurred_events++;
        returned_events++;
      }
    }
  }

  return returned_events;
}

/*
 * Return whether the current build options can report WL_SOCKET_CLOSED.
 */
bool WaitEventSetCanReportClosed(void) {
  /* epoll reports a closed peer (EPOLLRDHUP). */
  return true;
}

/*
 * Get the number of wait events registered in a given WaitEventSet.
 */
int GetNumRegisteredWaitEvents(WaitEventSet *set) { return set->nevents; }

/* Read all that the thread's wake fd holds (sm_sys.h). */
static void drain(void) { sm_clear_wake_fd(); }

static void ResOwnerReleaseWaitEventSet(Datum res) {
  WaitEventSet *set = (WaitEventSet *)DatumGetPointer(res);

  Assert(set->owner != NULL);
  set->owner = NULL;
  FreeWaitEventSet(set);
}

