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
procsignal barriers (66); `barrier.c` (10); shared invalidation,
`sinval.c` and `sinvaladt.c` (146).

What is left is built at start by `CreateOrAttachShmemStructs()`
(storage/ipc `ipci.c`): the init list of LWLocks, `varsup`, the
lock manager, the PGPROCs, the proc array, PMSignal and
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
  that sleep short. Taken still: ProcArray, the lock manager's
  partitions and each PGPROC's
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

- **Shared invalidation is gone** (`WORKNOTE.md` step 146): its
  queue, `sinval.c` and `sinvaladt.c`, the catchup interrupt and
  the two SInval LWLocks. A session processes its own messages
  (utils/cache `inval.c`), and learns of another's DDL from sm's
  versions and epoch (§5.1). A virtual xid's local part comes
  from one counter for the process (`pcb_t::next_lxid`), where
  it came from the queue's per-slot state.
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
- **ProcSignal** (storage/ipc `procsignal.c`). One reason has a
  sender, `pg_log_backend_memory_contexts()`, the sinval catchup
  gone with sinval (`WORKNOTE.md` step 146). It also keeps the
  cancel keys. **To replace**: an interrupt bit for the one
  reason left, the cancel key in the session record.
- **PMSignalState** (storage/ipc `pmsignal.c`, postmaster
  `pmchild.c`). It tells an abnormal end of a session thread
  from a clean one, and keeps the quit reason; the walsender
  flags are dead. **To replace**: an atomic state in the
  thread's launch record, and an atomic quit reason in the pcb.

### 5.1 Invalidation by version, not by message

**Built** (`WORKNOTE.md` steps 141 to 146): no session sends or
reads a message, and the queue is gone.

sinval, PostgreSQL's shared invalidation, tells every session
what another's DDL changed, so it drops what it has cached of
it. A session's caches are its own (utils/cache `relcache.c`,
`catcache.c`: `thread_local`), and what is cached of a
compiled-in row never goes stale; what can is what sm keeps: a
table's or an index's descriptor, and a schema. digres asks
instead of being told.

- **What sends a message now** (access/sm `smstore.c`): CREATE
  and DROP SCHEMA flush `pg_namespace`; DROP TABLE, ALTER TABLE
  (RENAME too, through `alter_table_rel()`), ANALYZE, CREATE
  INDEX and DROP INDEX invalidate the table's relcache entry.
- **A relation has a version, in sm.** sm gives each table a
  definition version, which each of those but the schema's
  changes in the DDL's own transaction: sm3's count of the
  catalog's publishes at which the table's definition or its
  statistics were last published, or an index of it made valid
  (`sm_table_def_t::version`, `../sm/sm3/CATALOG.md` §4;
  `WORKNOTE.md` step 143). A version is read as the transaction
  sees it: `SM_VERSION_OWN` while its own DDL or ANALYZE of the
  table is uncommitted, else the newest committed, never one at
  its snapshot, since the catalog is not versioned
  (`SM3_DESIGN.md` §2). A DDL that aborts leaves it as it was.
  An index has no version of its own: it takes no lock in sm
  (`WORKNOTE.md` step 111), so its entry is checked under its
  table's lock, by its table's version.
- **It is checked under the lock.** A statement takes sm's
  table-oid lock before it reads a definition (step 111): IS to
  read, IX to write, held to the end of the transaction. After
  the lock is granted, storage/lmgr `lmgr.c`'s
  `LockRelationOid()` family calls
  `AcceptInvalidationMessages()`; it is to compare the relcache
  entry's version with sm's instead. No lock is added: the
  parser locks what a query names, the plancache locks again
  before it reuses a plan (`AcquirePlannerLocks()` in
  `RevalidateCachedQuery()`, `AcquireExecutorLocks()` in
  `CheckCachedPlan()`), and the executor opens each relation
  with `NoLock` under those (executor `execUtils.c`,
  `ExecGetRangeTableRelation()`).
- **Why the check cannot go stale.** DROP, ALTER, CREATE INDEX
  and DROP INDEX take X in sm (sm3 `cat_get_table_rec()`), which
  waits for every IS and IX and is let go only after the DDL
  commits. So a version checked under IS or IX is the table's
  until the transaction ends, and one a DDL changed under X is
  committed by the time the lock is granted. Two changes are
  made without X. ANALYZE opens the table IS and takes only its
  statistics' own lock (`cat_lock_stats()`), and an index's
  build marks it valid under IX (`../sm/sm3/SM3_DESIGN.md`
  §3.3): a session holding the table can pass its check and
  then miss either until its next lock, planning with the old
  statistics, or without the new index. sm maintains every
  index and enforces every unique one itself, so this costs a
  plan, never a row.
- **A mismatch is the message that was not sent.** The entry's
  invalidation is processed locally, as utils/cache `inval.c`'s
  `LocalExecuteInvalidationMessage()` processes a session's
  own: the relcache entry is rebuilt, and the callbacks run as
  they do now (`PlanCacheRelCallback()`,
  `TypeCacheRelCallback()`), so a cached plan is planned again.
- **What no lock guards has an epoch.** A schema is looked up
  by name with no lock, so CREATE and DROP SCHEMA raise a
  process-wide epoch, sm3's count at which a namespace was last
  made or dropped (`sm_cat_namespace_epoch()`). A session
  compares it with the epoch it last saw wherever the queue is
  drained now: at a transaction's start (`AtStart_Cache()`,
  access/transam `xact.c`) and after each lock. On a change it
  processes the catalog flush of `pg_namespace` locally, which
  drops its search path too. The check is one atomic load. A
  user's function, type, cast, operator or view, if digres ever
  has them, raises the same epoch: such DDL is rare, and a flush
  of every user object a session has cached is affordable then.
- **The holes to close first**, each a definition read with no
  lock that conflicts with DDL's X:
  - `ConditionalLockRelationOid()` took no lock in sm. It takes
    sm's lock if it is free now (`smstore_trylock_relation()`,
    `WORKNOTE.md` step 138), so LOCK TABLE ... NOWAIT fails at
    once where it waited.
  - A descriptor used with no lock held. One built takes IS
    itself (access/sm `smstore_table_desc()`), so the hole is
    a cached one; an audit (`WORKNOTE.md` step 141) logged each
    use of a cached descriptor of sm's with no sm lock held,
    over digres's tests. Four call sites read only what never
    changes for an oid, or what the DDL then reads again under
    X: ALTER TABLE's and RENAME COLUMN's lookups, and the
    permission callbacks of CREATE INDEX and LOCK TABLE. One
    did not: `pg_get_indexdef()` opened an index before its
    table, and a lock on an index took nothing in sm, so it
    printed the definition of an index dropped while it
    waited. A lock on an index now locks its table, and DROP
    INDEX invalidates the index's own descriptor too.
  - **The rule is checked.** `CheckRelationLockedByMe()`
    (storage/lmgr `lmgr.c`) answers for a relation of sm's
    whether this transaction holds sm's lock on it
    (`smstore_relation_held()`), where it said yes always, so
    `relation_open()`'s assertion that a relation opened with
    `NoLock` is locked holds for sm's (`WORKNOTE.md` step 142).
    ALTER TABLE's and RENAME COLUMN's lookups, which may take no
    lock, have their descriptor without it.
  - Not closed: lsyscache's `get_rel_name()` and the rest read
    a cached descriptor through `RelationGetClassForm()` with
    no lock; of what they serve only the name changes, so a
    message may name a table by its old name.
- **The blocking is the lock's, not this design's.** DDL on a
  table waits for every transaction that holds it, and a
  waiting X may hold back later readers; PostgreSQL's
  AccessExclusiveLock does the same, with sinval. `lock_timeout`
  is not built (`../sm/NEXT.md` §2).
- **Why PostgreSQL sends messages.** Its catalogs are rows any
  SQL can change, most looked up with no lock (functions,
  types, operators), so a version would be checked at every
  cache hit, and the common case would pay for the rare one. Its
  invalidations are also replayed on a hot standby from the
  WAL. In digres only sm's objects change, and each but a
  schema is behind a lock.

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
2. **Invalidation by version** (§5.1): the holes closed and
   checked first; then a relation's version and the schema
   epoch in sm, checked where the queue was drained; then
   sinval, the catchup interrupt and the SInval LWLocks gone.
   Done (`WORKNOTE.md` steps 141 to 146). Next, ProcSignal and
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
