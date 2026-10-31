# SHMEM — PostgreSQL's shared memory, in one process

What is left in digres of PostgreSQL's cross-process machinery,
and what is to replace it. digres is one process; a session is a
thread of it, and every allocation is shared but what is
`thread_local` (`CODE.md` §3–4). PostgreSQL's shared memory,
locks and signals were built for processes that share nothing
else: here they are an indirect, often unsafe, way of doing
what C++ does plainly. This file was the survey and the order of
work. Every step of §7 is done, recorded in `WORKNOTE.md`, so
it is now a record of how the machinery went.

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

What is left is built at start by
`CreateSharedMemoryAndSemaphores()` (storage/ipc `ipci.c`): the
session registry, the advisory lock table and, with the build
option, the injection points' (§2). The lock manager is gone
(step 156), the LWLocks and semaphores (step 157), and PGPROC
(step 159).

## 2. Allocation: shmem's fields

**Done** (`WORKNOTE.md` steps 173 and 211). A structure the
threads share is a field of `shmem_t`
(`utils/control_block.h`), made by the postmaster, or a
standalone backend, before the first thread starts:
`CreateSharedMemoryAndSemaphores()` (storage/ipc `ipci.c`)
makes the session registry, the advisory lock table and, with
the build option, the injection points'. A session thread
finds them in shmem and attaches to nothing. What the main
thread fixes before then and no thread writes after is
`pcb_t`'s, read through a const pointer: the program's paths,
the data directory, `MaxBackends` and the other limits, the
file modes, and modules' own state (`utils/control_block.h`).

- **Gone**: storage/ipc `shmem.c`'s registry of named blocks,
  `ShmemInitStruct()`, `ShmemInitHash()`, `ShmemAlloc()` and
  `pcb->shmem_index`; `AttachSharedMemoryStructs()` and the
  init list a session thread ran; `TransamVariables`, whose
  constants `GetSnapshotData()` now names; and
  `shmem_request_hook` and `shmem_startup_hook`, which a
  library had no registry to use with. `shmem.c` kept
  `pg_numa_available()`, until it went too (`WORKNOTE.md` step
  241).
- **Sized ahead of time** still: `MaxBackends`, the session
  registry's limit. A snapshot's xid arrays, sized by it, are
  not allocated (`WORKNOTE.md` step 191): a snapshot holds no
  xid.
- **dynahash's shared mode is gone too** (`WORKNOTE.md` step
  174): its partitioned free lists and their mutexes, a header
  in shared memory a thread attached to, a fixed directory and
  a fixed size. A hash table is a backend's own.

## 3. Locks

- **LWLocks are gone** (`WORKNOTE.md` step 157), with
  storage/lmgr `lwlock.c`, the semaphores their waiters slept on
  (port `posix_sema.c`), and the LWLock wait events. A short
  critical section is a `std::mutex` or `std::shared_mutex`
  held by `held_lock_t` (`storage/held_lock.h`), which holds
  interrupts off as an LWLock did and lets go as an error
  unwinds it: AutoFile (step 132), InjectionPoint, a build
  option's (157).
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
- **Spinlocks are gone** (`WORKNOTE.md` step 182), with
  storage/lmgr `s_lock.c`: each was made a `std::mutex`, or an
  atomic where it guarded one word (steps 132, 133).
- **`pg_atomic_*` and the barriers are gone** (`WORKNOTE.md`
  step 181), with port `atomics.h` and `atomics.c`: an atomic
  is a `std::atomic`, its order explicit.
- **The heavyweight lock manager is gone** (`WORKNOTE.md`
  step 156): `lock.c`, `deadlock.c`, `proc.c`'s `ProcSleep()`,
  the partition LWLocks and the fast-path arrays. A user's
  table is locked by sm (`locked_by_sm()`, storage/lmgr
  `lmgr.c`), a catalog by nothing, as its rows are compiled in
  and never change (step 154), and what waited on a lock waits
  for nothing (step 155). `lmgr.c` keeps the entry points that
  take sm's lock or the invalidations waiting; its no-ops are
  gone with their calls (step 176). `pg_lock_status()` lists
  sm1's locks, held and waited for, and the advisory table's,
  in sm's modes (step 193), and `pg_blocking_pids()` names an
  advisory lock's holders and whom a session waits for in sm
  (step 166).
- **Advisory locks** are in a table of digres's own
  (storage/lmgr `advisory_lock.c`, `WORKNOTE.md` step 153), not
  sm's: they guard nothing stored (`DESIGN.md` §4). A mutex, the
  holders of each key, and a condition variable its waiters
  sleep on in `sm_wait()`; a deadlock among advisory waits is
  found as a session starts to wait. One through an sm lock as
  well is not.
- **A wait that must be cut short** is `sm_wait()` (`sm_sys.h`,
  `../sm/sm0/INTERRUPT.md`), then `CHECK_FOR_INTERRUPTS()`. A
  short mutex needs none, but is never held across a wait.

## 4. Latches: an interrupt bit

Done (`WORKNOTE.md` step 136). A latch was a flag in shared
memory that other processes set to wake its one owner. In one
process a latch is always a thread's, and `sm_sys.h` already
wakes a thread without losing the wake: so a latch is the
owning thread's `INTERRUPT_LATCH` bit. The shared latch a
thread switched to (`SwitchToSharedLatch()`) went with PGPROC's
latch (`WORKNOTE.md` step 158), and the owner a `Latch` kept
with step 192: a `Latch` is an empty token, `MyLatch`, that
names the calling thread's bit to PostgreSQL's API, and another
thread raises the bit through the thread's handle
(`sm_interrupt()`).

## 5. Shared structures

- **Shared invalidation is gone** (`WORKNOTE.md` step 146): its
  queue, `sinval.c` and `sinvaladt.c`, the catchup interrupt and
  the two SInval LWLocks. A session processes its own messages
  (utils/cache `inval.c`), and learns of another's DDL from sm's
  versions and epoch (§5.1). A virtual xid's local part comes
  from one counter for the process (`shmem_t::next_lxid`), where
  it came from the queue's per-slot state.
- **PGPROC is gone** (`WORKNOTE.md` step 159), with
  `ProcGlobal` and its free list. Its lock waits, semaphore and
  fast-path slots went with the lock manager and the LWLocks
  (steps 156, 157), its latch and cancel key with step 158: a
  session is interrupted, and a cancel request matched, through
  its record (§5.2). What was left is the session's own: its
  number is its thread's id (`MyProcNumber`, `MyProcPid`), and
  its transaction's local id and xmin are its tcb's.
  `max_connections` is the registry's limit.
- **The proc array is gone** (storage/ipc `procarray.c`,
  `WORKNOTE.md` step 194). No xid is ever assigned (step 45),
  so a snapshot is a constant, made with no lock and no scan
  (utils/time `snapmgr.c`'s `GetSnapshotData()`), and
  `TransactionIdIsInProgress()` asks only of the session's own
  (access/transam `transam.c`, step 150). A session is found by
  its record in the session registry (§5.2, step 152), as DROP
  DATABASE FORCE, the signal functions and `pg_blocking_pids()`
  find one.
- **TransamVariables is gone** (`WORKNOTE.md` step 173): its
  counters went with step 137, and what was left never changed;
  `GetSnapshotData()` names the constants.
- **ProcSignal is gone** (`WORKNOTE.md` step 147). Its one
  reason left, `pg_log_backend_memory_contexts()`, is an
  interrupt bit of its own, `INTERRUPT_LOG_MEMORY`; a session's
  cancel key is in its record, under `shmem->cancel_key_mutex`
  (step 158). storage/ipc
  `procsignal.c` keeps the interrupt bits' calls and the cancel
  request.
- **PMSignalState is gone** (`WORKNOTE.md` step 148). Its quit
  reason is `shmem->quit_reason`, an atomic. Its child flags,
  which told the postmaster of a child that ended without
  cleaning up after itself, went with nothing in their place:
  in one process whatever skips a session's exit callbacks
  ends the process, so the postmaster tells a crash by exit
  status alone.
- **The postmaster-death watch is gone** (`WORKNOTE.md` step
  184): a session cannot outlive the postmaster, its own
  process.

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
  (`sm_table_def_t::version`, `../sm/sm3/CATALOG_INMEM.md` §1;
  `WORKNOTE.md` step 143). A version is read as the transaction
  sees it: `SM_VERSION_OWN` while its own DDL or ANALYZE of the
  table is uncommitted, else the newest committed, never one at
  its snapshot, since the catalog is not versioned
  (`../sm/sm3/SM3_DESIGN.md` §2). A DDL that aborts leaves it
  as it was. An index has no version of its own: it takes no
  lock in sm (`WORKNOTE.md` step 111), so its entry is checked
  under its table's lock, by its table's version.
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
    INDEX invalidates the index's own descriptor too. The audit
    missed five, which no test cached a descriptor before:
    DROP INDEX, DISCARD TEMP and ON COMMIT DROP's drop, ON
    COMMIT DELETE ROWS, OWNED BY and `pg_stats()` read one
    before sm's lock, and stopped the server on the rule below;
    each takes its lock first now (`WORKNOTE.md` step 188).
  - **The rule is checked.** `CheckRelationLockedByMe()`
    (storage/lmgr `lmgr.c`) answers for a relation of sm's
    whether this transaction holds sm's lock on it
    (`smstore_relation_held()`), where it said yes always, so
    `relation_open()`'s assertion that a relation opened with
    `NoLock` is locked holds for sm's (`WORKNOTE.md` step 142).
    ALTER TABLE's and RENAME COLUMN's lookups, which may take no
    lock, have their descriptor without it.
  - lsyscache's `get_rel_name()` and the rest read a cached
    descriptor through `RelationGetClassForm()` with no lock,
    so they gave a renamed table's old name, and a dropped
    one's. They ask sm's version with no lock
    (`sm_cat_table_version_oid()`), and when it has moved take
    the name and schema from sm's definition, or nothing for a
    table gone (`smstore_relation_name()`, `WORKNOTE.md` step
    175).
- **The blocking is the lock's, not this design's.** DDL on a
  table waits for every transaction that holds it, and a
  waiting X may hold back later readers; PostgreSQL's
  AccessExclusiveLock does the same, with sinval.
  `lock_timeout` ends such a wait (`WORKNOTE.md` step 168).
- **Why PostgreSQL sends messages.** Its catalogs are rows any
  SQL can change, most looked up with no lock (functions,
  types, operators), so a version would be checked at every
  cache hit, and the common case would pay for the rare one. Its
  invalidations are also replayed on a hot standby from the
  WAL. In digres only sm's objects change, and each but a
  schema is behind a lock.

### 5.2 The session registry

**Built** (`WORKNOTE.md` step 152).

PostgreSQL's processes share a PGPROC slot and a pid, and nothing
else, so one session reads another's slot under a lock that
keeps the slot from being given to the next: `ProcArrayLock`, or
the lock manager's partition locks. Both a slot and a pid are
reused, so a reader rechecks that the slot is still the
session's it looked up. In one process a session can be named by
an object of its own instead.

- **A session has a record**, made when it registers and owned
  by `std::shared_ptr`: an id from a counter for the process,
  never reused, as a pid is; its pid; its thread; its database
  and role; its cancel key (step 158); and `ended`, an atomic
  set as it leaves the registry. What one session reads of
  another is in the record, set before it registers and fixed
  after, or atomic, or under a mutex.
- **The registry is shmem's**: a mutex, and the records of
  the sessions registered, by pid. A session registers where it
  joined the proc array (`InitProcessPhase2()`) and leaves where
  it left it (`RemoveProcFromArray()`). A lookup copies the
  record's `shared_ptr` under the mutex and lets the mutex go:
  the record is the caller's for as long as it keeps it, and is
  never another session's. A pid names whichever session is
  registered with it then, which PostgreSQL also accepts.
- **The lookups by pid use it**: the signal functions,
  `pg_log_backend_memory_contexts()`, the wait functions, and
  DROP DATABASE FORCE, which signals each other session of the
  database once the mutex is let go. `ProcArrayLock` goes, and
  with it the array of PGPROC numbers and the dense arrays
  nothing reads, `ProcGlobal->xids`, `subxidStates` and
  `statusFlags`, each PGPROC's offset into them, and the
  `XidCache`.
- **`pg_blocking_pids()`** names the holders of an advisory lock
  a session waits for, from digres's own table (steps 153,
  156), and whom it waits for in sm, a row's writer or a table
  lock's holder (step 166): each sm session is numbered with its
  pid, its thread's id (`sm_session_tag`, step 172), and sm1's
  waits-for edges give the pids (`sm_waits_for`).

## 6. What is left of modules removed

The dead code is gone (`WORKNOTE.md` step 131). What is left
has a caller still, and goes with the step that removes it:

- **Since the snapshot is a constant** (§7, step 3,
  `WORKNOTE.md` step 150), the proc array's group XID clearing,
  the horizons `ComputeXidHorizons()` and `GlobalVisTestFor()`
  computed and the replication slots' xmin fields they read are
  gone (step 151), and the `XidCache` and the dense arrays
  `ProcGlobal->xids`, `subxidStates` and `statusFlags` with
  the session registry (step 152).
- **Gone with the lock manager** (`WORKNOTE.md` step 156): lock
  groups, and the page, extension and apply-transaction locks.
- **Gone, the API for extensions' shared state**:
  `shmem_request_hook` and `shmem_startup_hook` with §2's
  registry (step 173), and `RequestNamedLWLockTranche()` with
  the LWLocks (step 157). Nothing in the tree used them; a
  library in `shared_preload_libraries` keeps state of its own,
  under a `std::mutex`.

## 7. Order of work

1. **PostgreSQL's xid and OID counters, the relation mapper's
   files and the relcache's init files**, removed, with
   XidGen, OidGen, RelationMapping and RelCacheInit (§3).
   Done (`WORKNOTE.md` step 137).
2. **Invalidation by version** (§5.1): the holes closed and
   checked first; then a relation's version and the schema
   epoch in sm, checked where the queue was drained; then
   sinval, the catchup interrupt and the SInval LWLocks gone.
   Done (`WORKNOTE.md` steps 141 to 146), and ProcSignal and
   PMSignalState with it (147, 148).
3. **The constant snapshot**: done (`WORKNOTE.md` step 150),
   the code it fed gone (step 151), and the proc array the
   session registry (§5.2, step 152).
4. **The lock manager**: advisory locks into a table of
   digres's own (`WORKNOTE.md` step 153), catalog locks
   gone (step 154), nothing left to take a lock (155), the
   lock manager gone (156), the LWLocks and `posix_sema.c`
   (157), interrupts and cancels through the session record
   (158), PGPROC gone (159), `pg_blocking_pids()` asking sm1's
   waits-for graph (166), and `pg_lock_status()` listing sm1's
   locks and the advisory table's (193).
5. **The registry** of §2: done (`WORKNOTE.md` step 173),
   each block a field of the pcb, now shmem (211), and
   dynahash's shared mode (174).

Each step is checked by the full suite and by `make tsan`;
asserts are on in the debug build (`dg.mk`), so a broken
invariant fails a test. Steps 2 to 4 change what sessions see
of each other, which `test/server.sh` checks only in part: DDL
seen by another session, `pg_cancel_backend()`, a session that
ends abnormally.
