/*-------------------------------------------------------------------------
 *
 * pmchild.c
 *	  Functions for keeping track of postmaster child processes.
 *
 * Postmaster keeps track of all child processes so that when a process exits,
 * it knows what kind of a process it was and can clean up accordingly.  Every
 * child process is allocated a PMChild struct from a fixed pool of structs.
 * The size of the pool is determined by various settings that configure how
 * many worker processes and backend connections are allowed, i.e.
 * max_wal_senders and max_connections.
 *
 * Dead-end backends are handled slightly differently.  There is no limit
 * on the number of dead-end backends, and they do not need unique IDs, so
 * their PMChild structs are allocated dynamically, not from a pool.
 *
 * The structures and functions in this file are private to the postmaster
 * process.  But note that there is an array in shared memory, managed by
 * pmsignal.c, that mirrors this.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/pmchild.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "utils/control_block.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"

/*
 * Freelists for different kinds of child processes.  We maintain separate
 * pools for each, so that for example launching a lot of regular backends
 * cannot prevent an aux process from launching.
 */
typedef struct PMChildPool {
  int size;            /* number of PMChild slots reserved for this
                        * kind of processes */
  int first_slotno;    /* first slot belonging to this pool */
  dlist_head freelist; /* currently unused PMChild entries */
} PMChildPool;

/*
 * The child slots: the main thread's, in the postmaster's thread control
 * block (postmaster_tcb->pmchild, utils/control_block.h), and read by no
 * other thread.
 */
struct pmchild_state_t {
  PMChildPool pmchild_pools[BACKEND_NUM_TYPES] = {};
  int num_pmchild_slots = 0;

  /*
   * List of active child processes.  This includes dead-end children.
   */
  dlist_head ActiveChildList = {};
};

/* The child slots, in the postmaster's thread control block. */
static inline pmchild_state_t &pcs(void) { return *postmaster_tcb->pmchild; }

/* Free the child slots' state, with its block. */
void pmchild_state_free_t::operator()(pmchild_state_t *state) const {
  delete state;
}

/* The list of active child processes, dead-end children among them. */
dlist_head *pmchild_active_list(void) { return &pcs().ActiveChildList; }

/*
 * MaxLivePostmasterChildren
 *
 * This reports the number of postmaster child processes that can be active.
 * It includes all children except for dead-end children.
 */
int MaxLivePostmasterChildren(void) {
  if (postmaster_tcb->pmchild == NULL || pcs().num_pmchild_slots == 0)
    elog(ERROR, "PM child array not initialized yet");
  return pcs().num_pmchild_slots;
}

/*
 * Initialize at postmaster startup
 *
 * Note: This is not called on crash restart.  We rely on PMChild entries to
 * remain valid through the restart process.
 */
void InitPostmasterChildSlots(void) {
  int slotno;
  PMChild *slots;

  /* The slots are the main thread's: the postmaster's, or a standalone's. */
  postmaster_tcb->pmchild.reset(new pmchild_state_t());

  /*
   * We allow more connections here than we can have backends because some
   * might still be authenticating; they might fail auth, or some existing
   * backend might exit before the auth cycle is completed.  The exact
   * MaxConnections limit is enforced when a new backend tries to join the
   * session registry.
   */
  pcs().pmchild_pools[B_BACKEND].size = 2 * pcb->MaxConnections;

  /*
   * There can be only one of each of these running at a time.  They each
   * get their own pool of just one entry.
   */

  /* The rest of the pmchild_pools are left at zero size */

  /* Count the total number of slots */
  pcs().num_pmchild_slots = 0;
  for (int i = 0; i < BACKEND_NUM_TYPES; i++)
    pcs().num_pmchild_slots += pcs().pmchild_pools[i].size;

  /* Initialize them */
  slots = (PMChild *)palloc(pcs().num_pmchild_slots * sizeof(PMChild));
  slotno = 0;
  for (int btype = 0; btype < BACKEND_NUM_TYPES; btype++) {
    pcs().pmchild_pools[btype].first_slotno = slotno + 1;
    dlist_init(&pcs().pmchild_pools[btype].freelist);

    for (int j = 0; j < pcs().pmchild_pools[btype].size; j++) {
      slots[slotno].pid = 0;
      slots[slotno].child_slot = slotno + 1;
      slots[slotno].bkend_type = B_INVALID;
      slots[slotno].thread = NULL;
      dlist_push_tail(&pcs().pmchild_pools[btype].freelist, &slots[slotno].elem);
      slotno++;
    }
  }
  Assert(slotno == pcs().num_pmchild_slots);

  /* Initialize other structures */
  dlist_init(&pcs().ActiveChildList);
}

/*
 * Allocate a PMChild entry for a postmaster child process of given type.
 *
 * The entry is taken from the right pool for the type.
 *
 * pmchild->child_slot in the returned struct is unique among all active child
 * processes.
 */
PMChild *AssignPostmasterChildSlot(BackendType btype) {
  dlist_head *freelist;
  PMChild *pmchild;

  if (pcs().pmchild_pools[btype].size == 0)
    elog(ERROR, "cannot allocate a PMChild slot for backend type %d", btype);

  freelist = &pcs().pmchild_pools[btype].freelist;
  if (dlist_is_empty(freelist))
    return NULL;

  pmchild = dlist_container(PMChild, elem, dlist_pop_head_node(freelist));
  pmchild->pid = 0;
  pmchild->bkend_type = btype;
  pmchild->thread = NULL;

  /*
   * pmchild->child_slot for each entry was initialized when the array of
   * slots was allocated.  Sanity check it.
   */
  if (!(pmchild->child_slot >= pcs().pmchild_pools[btype].first_slotno &&
        pmchild->child_slot <
            pcs().pmchild_pools[btype].first_slotno + pcs().pmchild_pools[btype].size)) {
    elog(ERROR, "pmchild freelist for backend type %d is corrupt",
         pmchild->bkend_type);
  }

  dlist_push_head(&pcs().ActiveChildList, &pmchild->elem);

  elog(DEBUG2, "assigned pm child slot %d for %s", pmchild->child_slot,
       PostmasterChildName(btype));

  return pmchild;
}

/*
 * Allocate a PMChild struct for a dead-end backend.  Dead-end children are
 * not assigned a child_slot number.  The struct is palloc'd; returns NULL if
 * out of memory.
 */
PMChild *AllocDeadEndChild(void) {
  PMChild *pmchild;

  elog(DEBUG2, "allocating dead-end child");

  pmchild = (PMChild *)palloc_extended(sizeof(PMChild), MCXT_ALLOC_NO_OOM);
  if (pmchild) {
    pmchild->pid = 0;
    pmchild->child_slot = 0;
    pmchild->bkend_type = B_DEAD_END_BACKEND;
    pmchild->thread = NULL;

    dlist_push_head(&pcs().ActiveChildList, &pmchild->elem);
  }

  return pmchild;
}

/* Release a PMChild slot, after the child process has exited. */
void ReleasePostmasterChildSlot(PMChild *pmchild) {
  dlist_delete(&pmchild->elem);
  if (pmchild->bkend_type == B_DEAD_END_BACKEND) {
    elog(DEBUG2, "releasing dead-end backend");
    pfree(pmchild);
  } else {
    PMChildPool *pool;

    elog(DEBUG2, "releasing pm child slot %d", pmchild->child_slot);

    pool = &pcs().pmchild_pools[pmchild->bkend_type];

    /* sanity check that we return the entry to the right pool */
    if (!(pmchild->child_slot >= pool->first_slotno &&
          pmchild->child_slot < pool->first_slotno + pool->size)) {
      elog(ERROR, "pmchild freelist for backend type %d is corrupt",
           pmchild->bkend_type);
    }

    dlist_push_head(&pool->freelist, &pmchild->elem);
  }
}

/*
 * Find the PMChild entry of a running child process by PID.
 */
PMChild *FindPostmasterChildByPid(int pid) {
  dlist_iter iter;

  dlist_foreach(iter, &pcs().ActiveChildList) {
    PMChild *bp = dlist_container(PMChild, elem, iter.cur);

    if (bp->pid == pid)
      return bp;
  }
  return NULL;
}
