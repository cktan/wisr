# GC: the Garbage Collector

The Garbage Collector reclaims what no snapshot can see: dead
versions, tombstones, the column families of dropped stores, and
status records no version needs. It rides on RocksDB compaction,
and one worker thread a database does what a compaction cannot.
Its code is `sm1/src/gc.cc`. Its queues and counters are in
`db_t` (`sm1/src/db.h`). `chain_report()` and `chain_repair()`
are in `sm1/src/chain.cc`, and `xct_drop_committed_records()` is
in `sm1/src/xct.cc`.

---

## 1. Design

### 1.1 What is garbage

| Garbage | Who finds it | Who removes it |
| --- | --- | --- |
| a version of an aborted or crashed transaction | the compaction filter | the filter (§4.1) |
| a committed version superseded by a newer one every live snapshot sees | the filter | the filter (§4.1) |
| a tombstone every live snapshot sees, and what it covers | the filter | the worker's SingleDeletes (§5) |
| a chained key's newest version, left by a dead transaction | the filter | the worker's repair (§4.2) |
| a chained key's older version no snapshot reaches | the filter | the filter (§4.2) |
| a dropped store's column family | `sm1::rmstore`'s commit | the worker's `DropColumnFamily` (§6) |
| a COMMITTED or ABORTED record no version needs | the worker, by the size of `CF:txn` | the sweep (§8) |

A **version** is one entry in a key's MVCC chain (ROWSTORE.md). A
**tombstone** is a version that records a delete. A **stamp** is
the `commit_ts` that the filter writes into a version's header in
place of its `xid`.

The filter is a RocksDB compaction filter. Compaction already
visits every key, so collection adds no pass of its own. There is
no vacuum thread for versions. The dead entries of a B-tree Index
are not this module's: BTREEINDEX.md's cleaner removes them.

### 1.2 Why a version carries a stamp

A stamped version answers visibility with no status lookup. So
over time most versions need neither the commit cache nor
`CF:txn`, and these are consulted mainly for the recent tail. The
stamp is also what lets the sweep delete a status record (§8).

### 1.3 What riding on compaction costs

- **Space is not reclaimed promptly.** A workload that writes and
  aborts heavily and triggers no compaction holds dead versions.
  The operational answer is `sm1::compact`, which is SQL's
  VACUUM.
- **Stamping is not prompt.** The filter runs only where
  compaction goes, and no task stamps on a schedule of its own. A
  new version starts at the top of the tree and is compacted
  soon, so for ordinary writes the gap is short. A version in a
  file at the bottom of the tree is reached only by the sweep's
  pass. A bulk load's ingested files land there, so their rows
  lean on the commit cache and `CF:txn` for a long time, and a
  load's dead versions wait for that pass
  ([`BULKLOAD.md`](BULKLOAD.md)).
- **Records linger.** Below a million records nothing is swept. A
  database with modest traffic keeps every commit record, tens of
  megabytes at most.
- **Collection inherits compaction's cost.** DBMGR.md assesses
  moving that work to another machine.

---

## 2. A worked example

A key `k` in a plain store has three committed versions. Reader R
holds snapshot 85.

```
T1: insert(k, "a")    commit_ts 70
T2: update(k, "b")    commit_ts 80
R:  begin             snapshot 85
T3: erase(k)          commit_ts 90
```

The watermark is 85. A compaction meets the versions newest
first:

| Version | Rule | Result |
| --- | --- | --- |
| tombstone, 90 | committed above the watermark | kept as it is |
| `"b"`, 80 | the newest at or below the watermark | kept, stamped 80 |
| `"a"`, 70 | older than that | dropped |

R ends, and no snapshot is left. The watermark is `visible_ts`,
say
95. A second compaction:

| Version | Rule | Result |
| --- | --- | --- |
| tombstone, 90 | the newest at or below the watermark, and a tombstone | kept, stamped 90, queued for purge |
| `"b"`, 80 | older than that | dropped |

The worker then writes a SingleDelete of each version from the
tombstone's key to `k | FF×12`. That removes the tombstone, and
every older version in a file the compaction never saw. The status records of
T1, T2 and T3 stay in `CF:txn` until a sweep.

---

## 3. The watermark

`gc_watermark` is the oldest registered snapshot, or `visible_ts`
if none is registered (`db_watermark()`). `sm1::gc_watermark`
gives it to a level above. A version committed at or below it is
seen by every snapshot.

A snapshot is registered in `db_t::snapshots`, a multiset, in
these cases:

- `sm1::begin`, at every isolation level (`db_take_snapshot()`);
- a restored prepared transaction, at open
  (`db_register_snapshot()`).

A prepared transaction keeps the registration its session made. A
transaction releases its snapshot when it ends. A cursor's
snapshot is never older than its transaction's begin snapshot, so
the begin snapshots suffice. A session set with
`sm1::set_no_transaction` has no snapshot and holds nothing back.

Rules:

- **No snapshot lands below a watermark in use.** Taking a
  snapshot and registering it happen under `snap_mu`, the mutex
  the watermark is read under.
- **The watermark never moves backwards.** A new snapshot is
  `visible_ts`, which is at or above every snapshot already
  counted.
- **A read-committed transaction counts at its begin snapshot for
  its whole life,** between statements too, because its writes
  are checked against that snapshot (ROWSTORE.md). If the
  watermark passed it, the purge could remove a delete committed
  after `sm1::begin`. A later write of that key would no longer
  meet a commit after its snapshot, and the check would pass
  without the re-read. So an idle open transaction holds back GC,
  as in any MVCC engine.

**The stuck-snapshot warning.** Each registered snapshot records
when it was taken. Each GC run warns once about a snapshot older
than `stuck_warn_secs`, 60 seconds. That is the operator's signal
for the classic MVCC trap: an idle transaction, a forgotten
cursor, or a transaction in doubt.

---

## 4. The compaction filter

Each store CF has its own factory, `gc_factory_t`, which knows
the CF's storeid for the queues. It makes one `gc_filter_t` for
each compaction, and RocksDB gives each compaction thread its own
instance, so the state for each key needs no lock. The filter
judges against the watermark at the compaction's start.

`Filter()` looks at the key's class byte (DBMGR.md):

- Class `0x04`, a page: keep it as it is. A page is no version,
  and holds no xid.
- Class `0x01` or `0x02`: go to `filter_chained()` (§4.2).
- Class `0x00`: apply §4.1.

### 4.1 Versioned keys

The filter sees a key's versions newest first. For each version:

1. If the value does not decode, keep it, and count a doubt
   (§4.3).
2. If it is unstamped, resolve its writer (`xct_status()`,
   DBMGR.md).
3. If the writer is dead, drop the version.
4. If the writer is in flight, keep the version as it is.
5. If the status lookup failed, keep it, and count a doubt.
6. If it committed above the watermark, keep it as it is. Some
   live snapshot may not see it yet.
7. If a newer version of the userkey at or below the watermark
   was already seen, drop it. Every live snapshot sees that newer
   one.
8. Note this version as the userkey's newest at or below the
   watermark.
9. If it is a tombstone, queue `(storeid, version key)` for
   purge, and wake the worker. Keep the tombstone.
10. If it is unstamped, stamp it: flags with `version_stamped`
    set, `commit_ts`, then the payload.

Past step 6, a version is committed at or below the watermark, so
only those are purged or stamped. A move's tombstone keeps its
`version_moved` flag when stamped, and is purged as any tombstone
(ROWSTORE.md).

The in-flight step must come before them. Compaction runs at any
time, over versions of running transactions too, and such a
version has no `commit_ts`. A stamp there would make readers skip
status resolution and see an uncommitted write, and it would
outlive an abort.

### 4.2 Chained keys

`filter_chained()` judges a chained store's keys (ROWSTORE.md). A
chained key's newest version is CURR, class `0x01`. Its older
ones are OLD, class `0x02`, reached through the chain. An OLD
entry's **displacer** is the transaction that replaced it. Each
entry's `displaced` field holds that xid, then the `commit_ts`
once stamped (`chain_displaced_stamped`). An **unlinked** entry
(`chain_unlinked`) is one no chain reaches any more. Its
`displaced` field holds the timestamp it was unlinked at.

For a CURR version:

1. If the value does not decode, keep it, and count a doubt.
2. Find the writer's fate, from the stamp or `xct_status()`.
3. If the writer is dead, keep the version, and report the key
   (`chain_report()`). The worker puts back the version before
   it.
4. If the writer is in flight, keep it.
5. If the status lookup failed, keep it, and count a doubt.
6. If it committed above the watermark, keep it.
7. If it is a tombstone, drop it. No purge is queued.
8. Stamp it.

Step 7 differs from §4.1 because a chained key has one RocksDB
key for its newest version. When a filter drops a key, RocksDB
writes a delete over it, so no older entry of that key comes
back. A versioned key's older versions are other RocksDB keys.
The chain behind a dropped CURR tombstone goes by the OLD rule.

For an OLD entry:

1. If the value does not decode, keep it, and count a doubt.
2. If it is unlinked, drop it once the watermark has reached its
   timestamp. A reader on its way to it is older than that
   timestamp.
3. Find the displacer's fate, from the stamp or `xct_status()`.
4. If the displacer is dead or in flight, keep the entry, and
   stamp its own writer if that committed. A dead displacer's
   version still points to the entry until the repair unlinks it.
5. If the displacer committed at or below the watermark, drop the
   entry. No snapshot needs it, nor anything older in its chain.
6. Else keep it. Stamp the displacer's `commit_ts`, and its own
   writer's if that committed, so the sweep may delete their
   records.

A failed status lookup in steps 3 to 6 keeps the entry, and
counts a doubt.

**The repair.** `chain_repair()` runs on the worker. For each
queued key:

1. Skip the key if its store is gone.
2. Take the key's write latch, so no write of it comes between.
3. Call `chain_restore()` with the dead xid (ROWSTORE.md). It
   puts back the newest version not written by that xid, and
   unlinks the old entries passed. A key queued twice finds
   nothing to do.

A failure is logged. The next compaction that meets the version
reports it again.

### 4.3 Doubt

A version the filter cannot judge is kept untouched: an
undecodable value, or a failed status lookup. Keeping a version
is always safe. The filter counts it in `gc_filter_errors`, which
forbids a sweep from trusting its pass (§8.2), and reports the
count once a compaction, when the filter is destroyed.

### 4.4 Before the database is open

Compactions can start inside `DB::Open`, before the handles and
the active table the filter reads exist. So until `gc_ready` is
set, the factory hands out `keep_all_filter_t`, which keeps
everything. `db_open()` sets `gc_ready` after recovery
(DBMGR.md).

### 4.5 Temporary stores

A temporary store's CF is in the temporary instance, and has this
same filter. It reads the watermark and commit cache of the
database that made it. Its purges and repairs write with no log,
as any of its writes does (`store_write_options()`). DBMGR.md
gives the order a database closes in, which waits out such a
compaction.

---

## 5. The tombstone purge

**Why the filter cannot drop a tombstone.** A tombstone every
live snapshot sees is never superseded, so §4.1 keeps it forever,
one record for each deleted key. The filter cannot drop it, not
even at the bottommost level. RocksDB picks compaction inputs by
its own key, the full version key. So older versions of the
userkey may sit in an adjacent file or another subcompaction this
filter never sees, and dropping the tombstone would resurrect
them.

**The purge.** The worker drains the queue (`purge_one()`):

1. Read the versions in `[tombstone, class | userkey | FF×12)`,
   with no snapshot: the tombstone and every older version of
   the key still there. A later write of the key has a larger
   `wts`, sorts before the tombstone, and survives.
2. Write a SingleDelete of each, in one batch, at low priority
   (`WriteOptions::low_pri`): when compaction is behind, RocksDB
   slows the purge, not the sessions' writes.

- A store already gone is skipped.
- A failed purge is logged. The tombstone stays, and the next
  compaction that meets it queues it again.
- A SingleDelete does not affect RocksDB snapshots taken before
  it: a pin's or a cursor's iterator keeps what it sees.

**Every deletion of a version key is a SingleDelete**
([`PURGE_PLAN.md`](PURGE_PLAN.md)). A version key is written once:
its `wts` is a new timestamp for each write. So a SingleDelete and
its Put go together at the first compaction that meets both, at
any level, where a Delete must reach the bottom level. RocksDB's
contract forbids a Delete and a SingleDelete of one key, and
enforces it (`enforce_single_del_contracts`): a compaction that
meets both fails. So:

- The filter removes a version key with `Decision::kPurge`,
  which RocksDB turns into a SingleDelete (`FilterV2()`). A
  chained key, CURR or OLD, is written more than once, and is
  removed with `kRemove`, a Delete.
- A savepoint's rollback erases its version by a SingleDelete.
- The purge erases by SingleDelete. A version the filter erases
  meanwhile gets two; RocksDB's compaction skips the first.

**The queue holds a key once** (`purge_ids`). Each compaction
that meets a tombstone before its purge queues it again; a mass
delete's level-0 files are met by many, and a queue that kept
every one held each key about 25 times. The worker empties the
queue and the set together, so a key met again after that is
queued again, and erased again, harmlessly.

**The queue is volatile.** An entry lost to a crash is queued
again by the next compaction that meets the tombstone.

**One key's versions a purge.** The range ends with the key's
oldest version. A range across keys would cover every version of
any key between them: live rows the filter never saw, as they may
sit in files outside the compaction.

**Range tombstones are not free.** Each is one that
`RangeDelAggregator` must consider on reads that overlap it, and
RocksDB warns that many in a memtable degrade reads. The purge
writes none. A loaded store's rollback and a tree's pages write
some, so a store CF's memtable is flushed once it holds
`store_max_range_deletions` (1,000) of them: RocksDB's
`memtable_max_range_deletions`, set on every store CF.

**Warning: a store CF must have no `row_cache`.** `DeleteRange`,
which a loaded store's rollback and a tree's pages use, returns
`Status::NotSupported()` with one.

---

## 6. Store drops

`sm1::rmstore` deletes the store's catalog row (DBMGR.md). The
tombstone carries no storeid. So commit queues the storeids in
`T.dropped` with its `commit_ts` (`gc_queue_drops()`). Once the
watermark reaches that commit, the worker calls
`db_drop_store_cf()`. An older snapshot reads the store through
the same storeid until then.

- **The queue is volatile.** An entry lost to a crash is caught
  by the orphan sweep at the next open (DBMGR.md).
- **A prepared drop** commits from its record's `dropped` list,
  after a restart too.
- **A temporary store** is dropped at commit, as no other session
  can read it. Its entry in the queue then finds it gone.
- **A store made and aborted** is dropped by the abort or the
  rollback, and never reaches this queue.

`db_drop_store_cf()` drops the CF and takes the store out of
`db_t::stores` either way. A failed drop is logged, and the
orphan sweep catches the CF.

**When the files go.** RocksDB keeps a dropped CF's files until
its last handle is destroyed, and the last copy of `store_t`
destroys it. A session's `store_handles` entry is a copy that
lasts until that session's transaction ends (DBMGR.md). So an
idle session delays the space, and nothing else. A cleaner's
session has no transaction end, and keeps a store's handle until
it closes, so it is opened for a short pass.

**Buffer pool frames.** A drop does not walk the frames. A
dropped store's frames age out. No frame can be mistaken for
another store's, as a storeid is never reused (BUFMGR.md).

---

## 7. The GC worker

One thread a database (`gc_main()`) runs `gc_run_once()` when
woken, and at least once a second. The filter's purge queue and
`gc_queue_drops()` wake it. `db_open()` starts it, and `~db_t`
stops it first of all.

A run does these steps, under `gc_run_mu`, one run at a time:

1. Take the watermark once, for the whole run.
2. Under `gc_mu`, take the queued purges and repairs, and the
   drops the watermark has passed.
3. Put back the chained keys compaction found dead (§4.2).
4. Purge the queued tombstones (§5).
5. Drop each store whose drop the watermark has passed (§6).
6. Free the SSI nodes the watermark has passed (`ssi_cleanup()`,
   XCTMGR.md).
7. Warn about stuck snapshots (§3).
8. Finish a pending `CF:txn` sweep, or start one past its
   threshold (§8).

Purges come before drops: a key queued from a store about to be
dropped is simply skipped once its CF is gone.

The worker does not flush after a batch of purges. It runs at
least once a second, so a delete-heavy workload would flush every
store it touched about once a second, however few range
tombstones it wrote. That is dozens of tiny level-0 files a
minute, which keep level-0 compaction busy and can stall writes.
The flush also waits, and it would hold the rest of the run
behind it.

Tests call `gc_run_once()` directly, to run GC synchronously.

---

## 8. The `CF:txn` sweep

A COMMITTED record can go once every surviving version of its
transaction carries a stamp. §6.6 of DBMGR.md, step 0, then
answers for them. The sweep guarantees that condition. An ABORTED
record exists only for a prepared transaction that aborted. It
can go at any time: step 4 of that rule gives the same answer
without it. The sweep deletes every one. It never deletes a
PREPARED record.

### 8.1 The argument

1. Record `W0`, the watermark. Then run a full compaction pass
   over every store CF, memtables first.
2. Every committed version with `commit_ts <= W0` was already in
   RocksDB when the pass began. The filter visited it with a
   watermark at or above `W0`. So it was dropped or stamped.
3. Once the pass completes, and every iterator created before it
   began has been released, drop the COMMITTED records with
   `commit_ts <= W0`. An older iterator can still read unstamped
   copies from before the pass.

**The watermark at the pass's start matters.** A version with
`W0 < commit_ts <= gc_watermark_now` may have been kept
unstamped. Dropping its record would make it resolve as dead.

### 8.2 The steps

`sweep_txn_locked()` runs with `gc_run_mu` held, and returns the
records deleted, 0 while pending, or -1:

1. With no sweep pending, return 0 unless asked to start.
2. Record `W0`, and the count of `gc_filter_errors`.
3. Take every store from `db_t::stores`. That includes temporary
   stores and chained stores.
4. Run `CompactRange` on each, with `kForce` and
   `exclusive_manual_compaction`.
5. If a compaction fails, log it, and return -1.
6. If `gc_filter_errors` moved, return -1. A version stayed
   unstamped and still needs its own record.
7. Keep `W0`, flip the read epoch (§8.3), and mark the sweep
   pending.
8. If the old epoch still has readers, return 0. A later run goes
   on from here.
9. End the sweep, and delete the records in one batch
   (`xct_drop_committed_records()`).

An abandoned sweep deletes nothing, and a later run may start
another. The sweep's state is volatile. A crash loses a pending
sweep, and nothing else, as the records stay.

### 8.3 Read epochs

Step 3 of §8.1 uses two read-epoch counters, `db_t::readers[2]`,
chosen by the low bit of `db_t::read_epoch`.

A reader enters before it opens an iterator (`db_read_enter()`):

1. Load the epoch.
2. Count itself in that epoch's slot.
3. Load the epoch again. If it moved, leave the slot, and go to
   step 1.

It leaves once it is done resolving what it read
(`db_read_leave()`). The readers are a point read, a write's
existence check, a chained or batch read, and a cursor for its
whole life.

After the pass, the sweep flips the epoch. The records go once
the old slot drains. A reader that enters after the flip opens
its iterator on the pass's output, and sees only stamped versions
at or below `W0`. Two slots suffice, as a second flip waits for
the sweep to end.

- **The wait is not inline.** The sweep stays pending, and the
  worker finishes it on a later run. So a long-open cursor delays
  the sweep without stalling purges or drops.
- **An iterator kept past its call** must not serve a read in a
  later epoch. A pin's iterator is reused only in the epoch it
  was opened in (`reusable()`, `rawpin_t::reuse_t`). A batch
  read's kept iterator is refreshed inside the call's epoch,
  which makes it as a new one.

### 8.4 Two RocksDB behaviours

Step 2 of §8.1 rests on these:

- **The pass must reach the bottommost level.** A cold SST that
  no ordinary compaction has touched in weeks still holds
  versions that need a stamp.
  `CompactRangeOptions::bottommost_level_compaction` defaults to
  `kIfHaveCompactionFilter`, which covers this only because a
  filter is installed. The sweep passes `kForce` explicitly, so a
  later change to the filter cannot silently invalidate it.
- **The filter must see every version.** Compaction filters
  ignore snapshots: `IgnoreSnapshots()` is fixed at `true`, and
  the filter does not override it. So an open reader cannot cause
  an entry to be skipped and left unstamped. The pass also runs
  as an exclusive manual compaction, so no automatic compaction
  that began under an older watermark writes unstamped output
  alongside it.

`sm1::compact` does not meet these rules: it uses
`kForceOptimized`, is not exclusive, and covers one store. It
reclaims space, and no record may go because of it.

### 8.5 The trigger

The pass rewrites L0 through L6 of every store CF, so its cost
grows with the number of stores and with data volume. So the
trigger is the size of `CF:txn`, the space the sweep reclaims,
and not a timer. A database that commits rarely never pays for
it. Each GC run reads RocksDB's estimate,
`rocksdb.estimate-num-keys`, and starts a sweep past
`db_t::txn_sweep_records`, one million records.

### 8.6 The pass under a production load: open

Nothing here is decided or built. As built, the pass has these
costs beyond its I/O:

- **It stalls writes.** `exclusive_manual_compaction` means no
  other compaction runs at the same time, across the whole
  instance. While the pass works through the stores, a busy store
  gets no automatic compaction. Its level-0 files pile up to
  RocksDB's slowdown and then its stop trigger, and writes stall.
- **It trades badly.** One million records are tens of megabytes.
  To reclaim them, the pass rewrites every store, which may be
  terabytes.
- **It blocks the worker.** `CompactRange` runs inside
  `gc_run_once()` and waits. Purges, drops and stuck-snapshot
  warnings wait behind the whole pass. Only the wait for old
  readers is off the worker.
- **It backs off by growth.** A pass that is abandoned, or an
  old snapshot that keeps `W0` low, leaves records behind. The
  pass sets `sweep_floor` to the records it left. The next pass
  starts only when `CF:txn` holds `txn_sweep_records` more than
  that.

The options:

- **(a) Trim the pass.** Trigger on `CF:txn`'s size relative to
  the stores' data, and run the pass off the worker. This is
  contained, but the exclusive stall and the full rewrite remain.
- **(b) Compact only the files that need it.** A RocksDB
  `TablePropertiesCollector` on every store CF records, for each
  SST, whether it holds unstamped versions and of which xids. It
  marks such a file for compaction (`NeedCompact()`), so
  RocksDB's own background compaction rewrites it and the filter
  stamps it. The sweep flushes the memtables and reads the live
  files' properties. It drops the records at or below `W0` once
  no live file holds an unstamped version of a transaction
  committed at or below `W0`. The read-epoch wait stays. The
  argument for soundness: the filter never removes a stamp, so a
  compaction whose inputs hold no such version outputs none, and
  files created after the check are made from files it saw. A
  stamped cold file is never rewritten, and nothing runs
  exclusively. It is also the way to stamp sooner on purpose,
  after a bulk load or a restart. Still to settle: what the
  property holds (a count, an xid range, or the xids), and the
  argument, written as carefully as §8.1 step 2.

(b) is the fix. (a) is what to do first if the stall bites before
(b) is built.

---

## 9. Implementation

State in `db_t`:

| Field | Guarded by | Use |
| --- | --- | --- |
| `snapshots`, `snap_since`, `snap_warned` | `snap_mu` | §3 |
| `gc_ready` | atomic | §4.4 |
| `gc_thread`, `gc_stopping`, `gc_cv` | `gc_mu` | §7 |
| `purges`, `repairs`, `drops` | `gc_mu` | the queues; `drops` is a multimap from `commit_ts` to storeid |
| `gc_run_mu` | itself | one GC run at a time; `gc_sweep_txn()`, the sweep for tests, takes it too |
| `read_epoch`, `readers[2]` | atomic | §8.3 |
| `sweep_pending`, `sweep_w0`, `sweep_epoch` | `gc_run_mu` | §8.2 |
| `gc_filter_errors` | atomic | §4.3 |

`stuck_warn_secs` and `txn_sweep_records` are fields with fixed
values, which tests lower. Nobody waits on the worker, so it logs
its failures (DBMGR.md).

---

## 10. Cases

| Case | What happens |
| --- | --- |
| TOAST, LSM Index entry | versioned keys: §4.1 and §5 apply |
| B-tree Index page | class `0x04`: kept; a tree holds no xid, so the sweep is unaffected |
| update A to B to A | three versions; §4.1 keeps the newest at or below the watermark |
| delete, then reinsert | the insert sorts before the tombstone: the purge spares it |
| abort | no record: dead. A versioned key's versions are dropped. A chained key is not repaired by the abort: readers pass the dead version by the chain, the next write overwrites it, and §4.2 repairs the rest |
| savepoint rollback | deletes its versions, and repairs a chained key's (XCTMGR.md): nothing is left for GC |
| crash | as an abort; the queues are lost and refill; a pending sweep is lost |
| 2PC across a restart | versions in flight, snapshot pinned, record kept; a commit after queues its drops |
| old RR snapshot | holds the watermark: versions stay, and no purge or drop passes it |
| RC re-read | the begin snapshot holds the watermark for the transaction's life |
| SSI | nodes stay until the watermark passes their end |
| open cursor | its transaction's snapshot holds the watermark; it holds a read epoch for its life |
| a commit whose WAL sync is not done | its xid is active: in flight to the filter |
| commit cache entry of a swept record | still true, and nothing asks for it |
| DROP, TRUNCATE, an ALTER's rewrite | the old store goes by §6 |
| temporary store | the filter applies; the pass covers it; a drop at commit is immediate |
| bulk load, committed | stamped by the sweep's pass; read by status until then |
| bulk load, rolled back | a store the transaction made goes whole; else the heap files' versions wait for the pass |
| keyless (`_id`) table | no difference |

---

## 11. Rejected

- **Truncating `CF:txn` on a timer.** It loses committed data in
  cold SST files: an unstamped version with no record resolves as
  dead. A record goes only after a full pass, and after every
  older iterator has gone, with the watermark from the pass's
  start (§8.1).
- **Dropping bottommost tombstones in the filter, by
  `CompactionFilter::Context::is_bottommost_level`.** No such
  field exists. The struct carries `is_full_compaction`,
  `is_manual_compaction`, `input_start_level`,
  `column_family_id`, `reason` and `input_table_properties`. The
  flag would not help: compaction inputs are chosen by full
  version key, so older versions can sit in an SST the filter
  never sees (§5).
- **Presuming a version with `xid < min_uncompacted_xid` and no
  entry in an aborted set is committed.** That is unsafe. It
  resurrects the versions of crashed transactions, which by
  construction leave no status record anywhere. That absence is
  the abort, and it is what makes recovery free of an undo pass.
- **Letting ordinary compaction do the stamping.** It never
  promises to visit every file, so no record could ever be
  dropped. Dropping one anyway is the timer's error above.
- **Compacting one key range at a time.** It can be made sound:
  fix `W0` once for the cycle, and make each range's compaction
  exclusive and bottommost. It shortens each exclusive window,
  but it is still a rewrite of every store, spread out.
- **Merging adjacent purged keys into one range.** It takes live
  rows (§5).
- **A flush after each batch of purges.** Tiny level-0 files and
  write stalls (§7).
- **Stamping an in-flight version.** Readers would see an
  uncommitted write (§4.1).

---

## 12. Tests

Paths are relative to `sm1/`.

- **`test/test_gc.cc`.** Versions stay in commit order. A
  read-committed write past a newer commit survives compaction. A
  long-running reader keeps what it can see, and space is
  reclaimed after it. A running transaction's versions survive a
  forced compaction, unstamped, and go after it aborts.
  Tombstones are purged for each key, sparing live neighbours and
  a re-insert. Range tombstones flush by count. An idle
  read-committed transaction holds back the purge. A dropped
  store's CF goes once no snapshot sees it. The sweep deletes
  records only once every version is stamped, leaves every
  committed row readable, keeps records a snapshot may need, and
  starts past its threshold. A cursor from before the pass holds
  it back, and a pin's iterator from before it is not sought
  again.
- **`test/test_chain.cc`.** Compaction reports a dead chained
  version, and GC puts it back. GC harvests what no snapshot of a
  chained store needs. GC drops unlinked OLD entries, and the
  sweep keeps chained reads right.
- **`test/test_2pc.cc`.** A prepared drop commits by gid after a
  restart, and its CF goes once the watermark passes.
- **`test/test_temp.cc`.** With a commit cache of one entry, the
  sweep stamps a temporary store's versions before it deletes
  their records.
- **`rel`.** `(compact table)` drives `sm1::compact`.
- **`mvcc`.** None: the scenario driver cannot force a
  compaction.

