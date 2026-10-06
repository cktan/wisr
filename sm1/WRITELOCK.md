# WRITELOCK — writes that keep no row lock

A write in sm1 takes its key's row lock, exclusive, and keeps it
to the end of the transaction. A transaction therefore holds a
lock entry for every key it writes. This file is the design of
writes that keep none, as PostgreSQL's do: the uncommitted
version itself stands for the lock, and a second writer of the
key waits for the transaction that wrote it.

**Status: built**, on the `writelock` branch. Every scenario in
`mvcc/` passes unchanged, with two added (`b33`, `b34`), and so
do the stress tests and every level's suite. Paths are relative
to `sm1/`.
`SM1_DESIGN.md` §5 and §5.1 are the write path and the lock
table as they stand. The scenarios in `mvcc/` define what the
concurrency control must do, and every one of them is to pass
unchanged (`CLAUDE.md`).

---

## 0. The proposal in brief

- **A short latch makes check-then-write one step.** A write
  takes a mutex chosen by hashing its store and key, checks the
  key, puts its version and lets the mutex go, all within the
  call. It never waits while holding it.
- **The uncommitted version is the lock.** A write that finds
  the newest version of its key written by another transaction
  still running lets the latch go, waits for that transaction,
  and starts again.
- **Explicit locks stay as they are.** `sm1::lock`, which sm3
  uses for schema locks and `tbl_lock`, keeps its entries in
  the lock table. A write still waits for a conflicting one,
  and an explicit lock waits for an uncommitted version.
- **A write adds nothing to the lock list or the lock table.**
  The one exception is READ COMMITTED's refused write, which
  keeps the key locked until the re-read (§3.5).
- **Nothing on disk changes.**

What it buys: a transaction's memory no longer grows with the
keys it writes. Loading a million rows with three indexes holds
four million lock entries today and none after. Mini-batch
needs no lock path of its own (`BULKLOAD.md` §4.5).

---

## 1. What a row lock does today

A write takes the key's lock exclusive before anything else and
holds it to commit (`SM1_DESIGN.md` §5, step 2). That one lock
does four jobs.

| Job | How the held lock does it |
| --- | --- |
| Check-then-write is one step | no one else can write the key between the write's check and its put |
| A second writer waits for the first to finish | it waits on the lock, which the first holds until it ends |
| A key's versions sort in commit order | a writer takes `wts` after the lock, and its `commit_ts` before releasing it (`SM1_DESIGN.md` §4.1) |
| A prepared transaction's keys stay protected across a restart | its locks, write keys among them, are saved in the `PREPARED` record and taken again by recovery (§9.1) |

The cost is the entry: one in the shared lock table and one in
the transaction's list, `T.locks`, for every key written, kept
to the end of the transaction. `SM1_DESIGN.md` §13 records it as
the reason a transaction writing tens of millions of keys costs
gigabytes.

---

## 2. How PostgreSQL holds none

An insert or an update in PostgreSQL takes a page latch for an
instant, writes its tuple, and lets the latch go. The tuple
carries the writer's transaction id; that is the lock. A second
writer of the same row or unique key finds the tuple, sees its
writer still running, waits for that transaction to end, and
checks again. Each transaction holds one lock on its own id, and
that is the only lock a write leaves behind.

`BULKLOAD.md` §6.1 goes through it step by step.

---

## 3. The design

### 3.1 The latch

A fixed array of mutexes in the database object, 4,096 of them.
A key's latch is the one its store and key hash to. Two writes
of one key always meet on it; two writes of different keys meet
on it only when their keys hash alike, which costs a moment's
wait and never a wrong answer.

A latch is held only while a write checks its key and puts its
version. It is never held while waiting for anything, and a
write holds one latch at a time, so latches cannot deadlock.

### 3.2 A write

```
sm1::insert / update / erase (storeid, k):
  1. take latch(storeid, k)
  2. if another transaction holds an explicit lock on k that
     conflicts with a write:
         let the latch go; wait for it (§3.4); go to 1
  3. find the newest version of k
     if it was written by another transaction still running:
         let the latch go; wait for that transaction (§3.4); go to 1
  4. the checks of SM1_DESIGN.md §5 step 3, unchanged:
     conflict, SM_ERR_UPDATED (§3.5), duplicate, not found,
     the statement mark
  5. wts = global_ts++; put the version
  6. let the latch go
```

Step 3 is the new rule, and the whole of what replaces the held
lock's second job. Telling a running writer from a dead one
needs the three-way status `xct_status` already gives: a
version whose writer aborted or crashed is skipped, as now.

Step 2 keeps a write behind an explicit lock: a `FOR SHARE` of
the row, a foreign key check on a parent row, an index build's
shared lock on the heap row it is backfilling. It takes the
lock table's mutex for a look-up, not an insert.

### 3.3 An explicit lock

`sm1::lock` keeps its entry in the lock table to the end of the
transaction, as now. It gains one check: under the key's latch,
if the newest version of the key was written by another
transaction still running, it waits for that transaction, as
`FOR UPDATE` in PostgreSQL waits for an uncommitted update.

The check costs a seek. sm3's schema locks are explicit locks
on keys that are never written, so the seek finds nothing, and
a bloom filter usually answers it. Skipping it for such keys,
with sm2 saying so in `catkv_lock`, was designed and is not
built: nothing showed the seek to matter.

### 3.4 Waiting for a transaction

A waiter on a transaction wakes when it commits or aborts, and
also when it rolls back to a savepoint, which deletes versions.
It then starts its write again from step 1, so a rollback that
removed the version lets it through without waiting for the
whole transaction.

- **Where it waits.** On the lock table's condition variable,
  which every release already wakes; commit, abort and a
  rollback to a savepoint wake it too.
- **Deadlock detection.** The waits-for graph is from session
  to session (`SM1_DESIGN.md` §5.1). A wait on a transaction is
  an edge to the session that runs it, found through a map
  from `xid` to session kept beside the active table. A
  prepared transaction is owned by the database and waits on
  no one, so no cycle passes through it.
- **`qa_nowait`.** A wait on a transaction fails with
  `SM_ERR_BLOCKED`, as a wait on a lock does.

### 3.5 READ COMMITTED's refused write keeps the lock

An update or delete at READ COMMITTED that meets a commit after
`sm1::begin` is refused with `SM_ERR_UPDATED`, and the caller
must read the newest version with `sm1::read_latest` before it
may write again (`SM1_DESIGN.md` §5.4). The rule rests on the
lock being held from the refusal to the repeated write, so the
newest committed version cannot change in between.

So that path keeps its lock: a write refused with
`SM_ERR_UPDATED` takes the key's explicit lock, exclusive,
before it returns, and the mark of §5.4 lives on that entry as
now. It is the one write that leaves a lock behind, and it is
the rare one.

### 3.6 Versions still sort in commit order

A second writer of a key takes its `wts` only once no other
running transaction has a version of the key. So the first
writer has ended: if it committed, its `commit_ts` was taken
before it left the active table, and the second writer's `wts`
comes later from the same counter. The argument of
`SM1_DESIGN.md` §4.1 holds with "acquired the lock after the
first released it" read as "found the first's version no
longer in flight".

### 3.7 A transaction's own versions

`SM1_DESIGN.md` §5 lets a write skip the conflict check when the
newest version of its key is the transaction's own, since the
held lock meant no one could have committed the key since.
That still holds: no one can commit the key while the
transaction's own version is in flight, because any other
writer waits for it.

### 3.8 Rollback to a savepoint

It deletes the versions written since the mark, as now, and
releases only the explicit locks taken since. There are no
write locks to release. It wakes the waiters, who find the
deleted versions gone.

### 3.9 Prepare and recovery

A prepared transaction's uncommitted versions are its write
locks. After a restart recovery puts it back in the active
table, in doubt (`SM1_DESIGN.md` §9.1), so its versions are in
flight again and a writer waits for it as before.

The `PREPARED` record need list only explicit locks. A record
written before this change also lists its write keys, as
exclusive locks; recovery takes them again, which protects
nothing more but harms nothing. **The database format does not
change.**

### 3.10 sm3's protocols still hold

Two of sm3's protocols rely on a writer holding its heap row to
commit:

- **The index build's backfill** takes a heap row's lock shared
  and adds the row's entry, relying on a writer of the row
  holding it exclusive to commit (`sm3/SM3_DESIGN.md` §3). The
  backfill's lock is explicit, so it now waits for the
  writer's uncommitted version (§3.3), and a writer waits for
  the backfill's lock (§3.2, step 2). Either way the two still
  never work on one row at once.
- **A foreign key check** locks the parent row shared. A
  writer of the parent waits for it, and it waits for an
  uncommitted write of the parent: the same pair.

---

## 4. What it costs

- **A seek in `sm1::lock`** on a key that may be written, to
  find an uncommitted version (§3.3).
- **False waits on a shared latch**, a moment each, when two
  keys hash alike.
- **A wait is a retry.** A writer that waited starts its write
  again, a second look-up of the key. Today it waits on the
  lock and then looks once.
- **Deadlocks are found the same way, over more edges.** A
  cycle through a wait on a transaction is found as one
  through a lock.
- **A change to the core.** The write path and the lock manager
  are where sm1's isolation is decided. That is why the
  `mvcc/` scenarios must all pass unchanged.

---

## 5. Alternatives considered

| Alternative | Why not |
| --- | --- |
| Keep the row locks | Memory grows with every key written, which is what this removes |
| Drop the lasting lock for inserts only | Updates and deletes would keep theirs, and a mixed rule is two write paths to reason about. The uncommitted version is the lock for all three as easily |
| PostgreSQL's in-tuple row locks for `FOR SHARE` (multixact) | Explicit locks are few next to writes, except a foreign key check per child row. Worth doing later if those turn out many; not needed for this |

---

## 6. What has to be built

| Where | What |
| --- | --- |
| `src/db.h` | the latch array; a map from `xid` to session beside the active table |
| `src/access.cc` | the write path of §3.2; `sm1::lock`'s check of §3.3 |
| `src/lockmgr.cc` | a wait on a transaction, with its edge in the waits-for graph; a look-up of conflicting explicit holders without taking the lock |
| `src/xct.cc` | commit, abort and rollback wake the waiters; prepare records explicit locks only |
| `SM1_DESIGN.md` | §4.1, §5, §5.1, §5.4, §6.5, §8.4, §9.1 and §13; `MVCC.md` §9 and §13 |

---

## 7. How it is tested

- **Every scenario in `mvcc/` passes unchanged**, and so do the
  stress tests `e02`, `e03` and `e06`. The existing scenarios
  already cover most of the new waits: a writer waiting on an
  uncommitted insert (`b11`, `b17`) and update (`b09`, `b10`),
  a waiter released by a rollback to a savepoint (`b25`),
  deadlocks through waits on a transaction (`b16` to `b21`).
- **Two scenarios added:** `b33`, an explicit lock waiting for
  an uncommitted write, and `b34`, one waiting for an
  uncommitted insert that aborts.
- **`test/test_2pc.cc`** already has an in-doubt transaction's
  write blocking a writer across a restart.
- **New tests in `test/test_lock.cc`:** a transaction writing a
  thousand keys holds no lock entries, and its writes still
  block another writer; a write refused with `SM_ERR_UPDATED`
  keeps one until the re-read.
- **One test changed:** `test_lock.cc` asserted that a write
  leaves its writer holding the row exclusive. It now asserts
  it holds nothing.
- **`sm1::read_latest`** accepted a lock held on the key; it
  now also accepts a write of the key still in effect, which
  every other writer waits for as it waited for the lock. A
  `rel` script reading a row back after inserting it needed
  this.
---

## 8. Order of work

1. Waiting on a transaction, with its deadlock edge and the
   wake-ups, tests first.
   → verify: the new scenarios for a wait and a deadlock.
2. The latch, and the write path of §3.2, with the
   `SM_ERR_UPDATED` path keeping its lock.
   → verify: all of `mvcc/` and `test/`, unchanged; no lock
   entries for writes.
3. `sm1::lock`'s check.
   → verify: `FOR UPDATE` waits for an uncommitted update; sm3's
   build and foreign-key tests pass unchanged.
4. Prepare and recovery.
   → verify: the in-doubt scenario across a restart.
5. The documents of §6.

Then batch mode and mini-batch (`BULKLOAD.md` §12).
