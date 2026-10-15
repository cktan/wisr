# SHMEM — PostgreSQL's shared memory, in one process

What is left in digres of PostgreSQL's cross-process machinery,
and what is to replace it. digres is one process; a session is a
thread of it, and every allocation is shared but what is
`thread_local` (`CODE.md` §3–4). PostgreSQL's shared memory,
locks and signals were built for processes that share nothing
else: here they are an indirect, often unsafe, way of doing
what C++ does plainly. This file is the survey and the order of
work; a step done is recorded in `WORKNOTE.md` and leaves this
file.

Paths are under `src/`. A function is named with its module.

---

## 1. Where digres stands

Already gone (`WORKNOTE.md`): the DSM family, `dsa`, `dshash`,
`shm_mq` and `shm_toc` (step 29); the shared-memory size
arithmetic of `CalculateShmemSize` and the private mapping
(step 26); SLRUs, clog, subtrans, commit timestamps, multixacts
(steps 19, 46, 67, 68); two-phase commit (47); `NOTIFY` (43);
predicate locks (68); the buffer manager (65); pgstat (28);
procsignal barriers (66); `barrier.c` (10).

What is left is built at start by `CreateOrAttachShmemStructs()`
(storage/ipc `ipci.c`): the init list of LWLocks, `varsup`, the
lock manager, the PGPROCs, the proc array, sinval, PMSignal and
ProcSignal.

## 2. Allocation: a registry of named blocks

storage/ipc `shmem.c` is a name registry now: `shmem_index_t`,
a `std::mutex` and an `unordered_map` of name to block, held in
`pcb->shmem_index`. `ShmemAlloc()` is `calloc()`, never freed;
`ShmemInitStruct()` finds a block by name or makes it;
`ShmemInitHash()` makes a dynahash in shared mode, with a
directory fixed in size ahead of time.

| Block | Made in | Holds | Guard |
| --- | --- | --- | --- |
| TransamVariables | access/transam `varsup.c` | the snapshot's constants | set before threads start |
| LOCK, PROCLOCK hashes | storage/lmgr `lock.c:422,439` | heavyweight locks | partition LWLocks |
| Fast Path Strong Relation Lock Data | storage/lmgr `lock.c:446` | counts per partition | a spinlock |
| Proc Header, PGPROC structures, Fast-Path Lock Array | storage/lmgr `proc.c:172,194,234` | a PGPROC a session, free lists | ProcStructLock |
| Proc Array | storage/ipc `procarray.c:308` | pgprocnos[MaxBackends] | ProcArrayLock |
| (none: `pcb->sinval`, `WORKNOTE.md` step 130) | storage/ipc `sinvaladt.c` | the SI ring, procState[] | SInvalRead/Write, atomics |
| PMSignalState | storage/ipc `pmsignal.c:143` | child flags, quit reason | relaxed atomics |
| ProcSignal | storage/ipc `procsignal.c:95` | a slot a session, cancel keys | pss_mutex |

Beside the registry: the main LWLock array
(`pcb->main_lwlock_array`) and the semaphores
(`pcb->shared_semas`), step 27.

- **Each thread attaches again.**
  `AttachSharedMemoryStructs()` (storage/ipc `ipci.c`) runs the
  init list in every session thread, which finds each block by
  name and sets its pointer. The pointers are SHARED, copied
  from the postmaster already, so this does nothing but for
  the lock manager's two hashes, whose `HTAB` handle each thread
  makes for itself over the shared table.
- **Sized ahead of time**: `MaxBackends`, `NLOCKENTS`, the
  fixed hash directories, `PROCARRAY_MAXPROCS`,
  `num_os_semaphores`.
- **To replace**: each block a typed field of `pcb_t`, made by
  the postmaster before the first thread starts (`CODE.md` §3).
  Then `AttachSharedMemoryStructs()`, the registry, the
  `*ShmemSize()` functions and dynahash's shared mode go. A
  PGPROC must not move: pointers to it are held everywhere, so
  its container is allocated once or keeps addresses stable.

## 3. Locks

- **LWLocks** (storage/lmgr `lwlock.c`). Short critical
  sections, held with interrupts held off. A waiter sleeps on its
  PGPROC's POSIX semaphore (port `posix_sema.c`); nothing cuts
  that sleep short. Taken still: ProcArray, SInvalRead and
  SInvalWrite, the lock manager's partitions and each PGPROC's
  `fpInfoLock`; and InjectionPoint, a build option's. **To
  replace**: a `std::mutex` or
  `std::shared_mutex` held by `held_lock_t`
  (`storage/held_lock.h`), which holds interrupts off as an
  LWLock does and lets go as an error unwinds it; AutoFile is
  one (`WORKNOTE.md` step 132).
- **XidGen, OidGen, XactTruncation, RelationMapping and
  RelCacheInit are gone**, with what they guarded
  (`WORKNOTE.md` step 137): PostgreSQL's xid and OID counters,
  the relation mapper and the relcache's init files. An oid
  digres stores is sm3's: tcop `utility.c` sends CREATE TABLE
  and CREATE INDEX to access/sm `smstore.c`, whose
  `sm_cat_create_*` calls take it from sm3's `cat_new_oid()`,
  sm1's sequence `oid_seq`, 16384 to `UINT32_MAX`. What would
  draw one of PostgreSQL's for a catalog row (CREATE TYPE,
  DOMAIN, CAST and the rest) is refused as the write would be.
- **Spinlocks** (storage/lmgr `s_lock.c`): none is taken.
  Each is a `std::mutex`, or an atomic where it guarded one word
  (`WORKNOTE.md` steps 132, 133). Left: port `atomics.c`'s
  emulation of 64-bit atomics, compiled out on both platforms.
- **`pg_atomic_*` and the barriers**: in `lwlock.c`, `proc.c`,
  `procarray.c`, `procsignal.c`, postmaster `backend_thread.c`.
  **To replace**: `std::atomic` with explicit orders.
- **The heavyweight lock manager** (storage/lmgr `lock.c`,
  `lmgr.c`, `deadlock.c`, `proc.c`'s `ProcSleep()`) is live.
  A user's table is locked by sm (`locked_by_sm()`, `lmgr.c`),
  but the lock manager still takes catalog relations' locks,
  advisory locks, and the virtual xid's at every transaction's
  start; its waits are a latch's, cut short by an interrupt.
  **To replace**: advisory locks into sm, catalog relations'
  locks gone (their rows are compiled in and never change), and
  then the lock manager, the deadlock detector, the partition
  LWLocks and the fast-path arrays go.
- **A wait that must be cut short** is `sm_wait()` (`sm_sys.h`,
  `../sm/sm0/INTERRUPT.md`), then `CHECK_FOR_INTERRUPTS()`. A
  short mutex needs none, but is never held across a wait.

## 4. Latches: an interrupt bit

Done (`WORKNOTE.md` step 136). A latch was a flag in shared
memory that other processes set to wake its one owner. In one
process a latch is always a thread's, and `sm_sys.h` already
wakes a thread without losing the wake: so a latch is the
owning thread's `INTERRUPT_LATCH` bit. `Latch` keeps only the
owner, for others to find the thread by. What is left: the
`Latch` handle itself, and the local and shared latches a thread
switches between (`SwitchToSharedLatch()`), which are one bit.

## 5. Shared structures

- **Shared invalidation** (storage/ipc `sinval.c`,
  `sinvaladt.c`; utils/cache `inval.c`). Live: it carries the
  relcache invalidations of sm's tables and indexes (access/sm
  `smstore.c`: DROP, ALTER, ANALYZE, CREATE and DROP INDEX) and
  the catalog flush of `pg_namespace` (CREATE and DROP SCHEMA).
  Every catalog write is refused, so the rest of its message
  kinds are dead. Its segment is `pcb->sinval`, and its flags
  and `maxMsgNum` atomics (`WORKNOTE.md` step 130). **To
  replace**: an `inval_bus_t` in the pcb: a mutex, a queue of
  messages under 64-bit sequence numbers, `published` an atomic,
  and a reader's `next` and `reset` for each session. A reader
  is caught up when `next == published`; one too far behind is
  reset. That removes the wraparound arithmetic, the catchup
  interrupt and the two SInval LWLocks.
- **PGPROC** (storage/lmgr `proc.c`). One a session: its latch,
  its LWLock and lock waits, its semaphore, its fast-path slots,
  the target of `InterruptProc()`. **To replace**: a session
  record owned by its thread's tcb, listed in a registry in the
  pcb (a mutex and a vector), once the lock manager and LWLocks
  are gone.
- **The proc array** (storage/ipc `procarray.c`). Mostly dead:
  no xid is ever assigned (step 45), yet `GetSnapshotData()`
  scans the array under `ProcArrayLock` for every snapshot
  (utils/time `snapmgr.c`). Its live users are DROP DATABASE
  FORCE, the signal functions and `ProcNumberGetProc()`. **To
  replace**: a snapshot that is a constant, taken without a
  lock; the lookups by pid move to the session registry.
- **TransamVariables** (access/transam `varsup.c`). Its
  counters are gone (`WORKNOTE.md` step 137); left are
  `oldestXid`, `latestCompletedXid` and `xactCompletionCount`,
  which `GetSnapshotData()` reads and which never change. **To
  remove** with the constant snapshot (§7, step 3).
- **ProcSignal** (storage/ipc `procsignal.c`). Two reasons have
  senders: the sinval catchup and
  `pg_log_backend_memory_contexts()`. It also keeps the cancel
  keys. **To replace**: an interrupt bit for the one
  reason left, the cancel key in the session record.
- **PMSignalState** (storage/ipc `pmsignal.c`, postmaster
  `pmchild.c`). It tells an abnormal end of a session thread
  from a clean one, and keeps the quit reason; the walsender
  flags are dead. **To replace**: an atomic state in the
  thread's launch record, and an atomic quit reason in the pcb.

## 6. What is left of modules removed

The dead code is gone (`WORKNOTE.md` step 131). What is left
has a caller still, and goes with the step that removes it:

- **With the constant snapshot** (§7, step 3): the proc array's
  group XID clearing (`ProcArrayGroupClearXid()`), the
  horizons `GetSnapshotData()` and `GlobalVisTestFor()` compute,
  the replication slots' xmin fields they read, the
  `XidCache`.
- **With the lock manager** (§7, step 4): lock groups
  (`lockGroupLeader`, `lockGroupMembers`), which `lock.c` and
  `deadlock.c` test throughout though no proc ever joins one;
  and the tuple, page, extension and speculative lock tags,
  which the executor still takes.
- **Kept, for extensions**: `shmem_request_hook`,
  `shmem_startup_hook` and `RequestNamedLWLockTranche()`, the
  API a library in `shared_preload_libraries` uses for state
  and locks of its own. Nothing in the tree uses them, but
  digres still loads libraries (utils/fmgr `dfmgr.c`); they go,
  or become a pcb registry, with §2's registry.

## 7. Order of work

1. **PostgreSQL's xid and OID counters, the relation mapper's
   files and the relcache's init files**, removed, with
   XidGen, OidGen, RelationMapping and RelCacheInit (§3).
   Done (`WORKNOTE.md` step 137).
2. **The invalidation bus** (§5), then ProcSignal and
   PMSignalState.
3. **The constant snapshot**: the proc array becomes the
   session registry.
4. **The lock manager**: advisory locks into sm, catalog locks
   gone; then the lock manager, the remaining LWLocks,
   `posix_sema.c` and PGPROC.
5. **The registry** of §2, once each block is a pcb field.

Each step is checked by the full suite and by `make tsan`;
asserts are on in every build (`dg.mk`), so a broken invariant
fails a test. Steps 2 to 4 change what sessions see of each
other, which `test/server.sh` checks only in part: DDL seen by
another session, `pg_cancel_backend()`, a session that ends
abnormally.
