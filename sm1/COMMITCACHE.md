# COMMITCACHE — what the commit cache is, and how long it lives

A reader that meets a version must learn the fate of the
transaction that wrote it. The commit cache is the fast
answer for one case: a transaction that committed recently.
This file says what it holds, when an entry enters and
leaves, who asks it, and what happens after a restart.

It describes the code as it is, in `src/xct.cc` and
`src/db.h`. `SM1_DESIGN.md` §3.4 is the rule it serves.

---

## 0. In brief

- It is a map in memory from a transaction's `xid` to its
  `commit_ts`. Nothing else is in it.
- **It is in memory only.** It is never written to disk and
  never read from disk.
- An entry is added when a transaction commits, and when a
  lookup misses the cache and finds the transaction's commit
  record on disk.
- It holds a bounded number of entries, 65,536 by default,
  and evicts the entry that has been in it longest.
- **After a restart it is empty.** Nothing loads it at
  startup. It fills as transactions commit and as readers
  ask about older ones, one disk read for each transaction
  asked about.
- An entry stops mattering once compaction has stamped the
  commit timestamp into the transaction's versions, in the
  background (§5).
- It is never needed for a right answer. The durable status
  record, in `CF:txn`, says the same thing. A miss costs a
  RocksDB read and nothing else.

---

## 1. Why it exists

A version carries the `xid` of its writer, not the writer's
fate (`SM1_DESIGN.md` §3.1). To decide whether a version is
visible, a reader resolves that `xid`, in this order
(`SM1_DESIGN.md` §3.4):

| Step | Where | Answers |
| --- | --- | --- |
| 0 | the version's own header | committed, if compaction has stamped the commit timestamp into it |
| 1 | the active table, in memory | still running, or prepared |
| 2 | **the commit cache, in memory** | committed, at this `commit_ts` |
| 3 | `CF:txn`, on disk | committed, aborted or prepared, by its record |
| 4 | none of the above | never committed: dead |

Without step 2, every version of a transaction that has
committed and not yet been stamped would cost a read of
`CF:txn`. Stamping is done by compaction, which may not visit
a new version for a long time. The cache covers that gap for
the transactions most likely to be asked about: the newest.

---

## 2. What it is

Two maps in the database object, `db_t`, guarded by one
mutex, `txn_mu`:

```
cache        xid → commit_ts           the lookup
cache_order  xids, oldest entry first  the order of eviction
```

The same mutex guards the active table. That is deliberate: a
committing transaction leaves the active table and enters the
cache in one step, so a reader never finds it in neither
place and concludes it is dead.

Only committed transactions are in it. An aborted transaction
has no entry, and neither does a prepared one.

---

## 3. The life of an entry

### 3.1 It enters at commit

`sm1::commit` reaches its commit point when the `COMMITTED`
record is written to `CF:txn` (`SM1_DESIGN.md` §8.2). Then,
in this order:

1. The record is written. The transaction has committed.
2. **The `xid` leaves the active table and enters the cache**,
   under `txn_mu` (`enter_cache`).
3. The commit is published: `visible_ts` moves past its
   `commit_ts`, so that new snapshots include it.
4. The row locks are released.

The record comes first so that the cache never says
"committed" of something the disk does not. The cache comes
before publication so that no snapshot can include the commit
while a lookup would still miss it.

The commit of a prepared transaction takes the same path.

### 3.2 It enters when a lookup finds it on disk

A lookup that misses the cache reads the transaction's record
from `CF:txn`. If the record says `COMMITTED`, the lookup puts
the `xid` and its `commit_ts` in the cache before it answers.

So a transaction costs one disk read however many of its
versions are met, by this reader or the next. Without it, a
scan over a million unstamped rows of one transaction read
the same record a million times.

- **It is safe** because a commit is final: the record's
  `commit_ts` never changes, and an `xid` is never reused.
  An entry cannot become wrong.
- **Only `COMMITTED` is kept.** A prepared transaction may yet
  go either way. An aborted one, or one with no record, is
  not cached (§6).

### 3.3 It is read on every unstamped version of another transaction

One routine, `xct_status`, does steps 1 to 4 of §1. It is
called from three places:

| Caller | For |
| --- | --- |
| a read or a scan (`access.cc`) | whether a version is visible at the snapshot |
| a write (`access.cc`) | whether the newest version of the key committed, and when |
| the compaction filter (`gc.cc`) | whether to drop a version, keep it, or stamp it |

A hit returns the `commit_ts` under the mutex and reads
nothing from RocksDB.

A version the reader wrote itself is recognised by its `xid`
and never reaches the cache. A stamped version never does
either.

### 3.4 It leaves by eviction

After each insertion, while the cache holds more than
`commit_cache_entries`, the entry that came in first is
removed: first in, first out.

It is not by `commit_ts`, though it once was. An old commit
read back from disk has a small `commit_ts`. Evicting the
smallest would remove it the moment it entered a full cache,
and §3.2 would do nothing.

It is not by use either. Moving an entry to the front on
every hit would put a write on the read path, under the mutex
every begin and commit takes. A hot entry that is evicted
comes back with one disk read.

`commit_cache_entries` is an option given when the database
is opened. Its default is 65,536 and it must be at least 1.

Eviction cannot change an answer. The record was written
before the entry was made, so a lookup that misses the cache
goes on to `CF:txn` and finds the same `commit_ts` there.

### 3.5 Nothing else removes it

An entry is not removed when its transaction's versions are
all stamped, nor when the periodic sweep deletes its record
from `CF:txn` (`SM1_DESIGN.md` §8.3). It stays until newer
commits push it out. That is harmless: the entry is still
true, and once every version is stamped nothing asks for it.

### 3.6 It dies with the database object

The cache belongs to `db_t`, which exists from the first
`sm1::opendb` of a database in the process until the last
session on it closes. Every session on the database shares
it. Closing the last session frees it; opening the database
again starts with an empty one.

---

## 4. After a restart

The cache starts empty, and **nothing loads it at startup.**
Recovery (`SM1_DESIGN.md` §9.1) reads `CF:txn` only for
`PREPARED` records, to rebuild the transactions in doubt.

It fills on demand (§3.2). For what was committed before the
restart:

| Version | How it is resolved after a restart |
| --- | --- |
| stamped by compaction | by its header; nothing to look up |
| unstamped, committed before the restart | by a read of `CF:txn` the first time its transaction is asked about, and from the cache after |
| unstamped, of a transaction that was running at the crash | by a read of `CF:txn` that finds nothing, every time: dead |

Answers are right from the first read. What a restart costs
is one disk read for each committed transaction a reader
meets, not one for each version. It is the same cost an
evicted transaction pays at any time.

---

## 5. How a version stops needing the cache

The cache covers the time between a commit and the moment its
versions no longer need a lookup at all. Two background steps
end that time (`SM1_DESIGN.md` §8.3). Neither is prompt.

### 5.1 Compaction stamps the version

When RocksDB's background compaction rewrites a file, sm1's
compaction filter (`src/gc.cc`) looks at each version in it.

| The version's writer | The filter |
| --- | --- |
| aborted, or crashed | drops the version |
| still running, or prepared | keeps it as it is |
| committed after the GC watermark | keeps it as it is |
| committed at or below the watermark | replaces the `xid` in its header with the `commit_ts`, and sets a flag |

A stamped version is step 0 of §1: a reader takes the commit
timestamp from the header and asks neither the cache nor
`CF:txn`.

The filter runs only where compaction goes. A version in a
file that nothing rewrites stays unstamped. No task stamps
versions on a schedule of its own.

### 5.2 The GC worker deletes the status records

Each open database has a GC thread that wakes at least once a
second. One of its jobs is a sweep of `CF:txn`.

1. **Trigger.** RocksDB's estimate of the records in `CF:txn`
   passes one million (`txn_sweep_records`, `src/db.h`).
2. **Pass.** It notes the GC watermark, `W0`, and forces a
   full compaction of every store, down to the bottom level.
   Every committed version at or below `W0` is then stamped
   or dropped.
3. **Wait.** Readers that began before the pass may still
   hold unstamped copies. The sweep is left pending until
   they have finished.
4. **Delete.** It removes the `COMMITTED` records at or below
   `W0`, and every `ABORTED` record.

A record is removed, not marked, and only once no version
needs it. A cache entry for a transaction whose record has
gone is still true, and nothing asks for it (§3.5).

### 5.3 What follows

- **Records linger.** Below a million records nothing is
  swept. A database with modest traffic keeps every commit
  record, tens of megabytes at most, and that is what a cache
  miss falls through to.
- **Stamping is not prompt.** A new version starts at the top
  of the tree and is compacted soon, so for ordinary writes
  the gap is short. A version in a file at the bottom of the
  tree is reached only by the sweep's forced pass. That is
  where a bulk load's ingested files land (`BULKLOAD.md`
  §5.5), so their rows lean on the cache, and then on
  `CF:txn`, for a long time.
- **The sweep is expensive.** It rewrites every store to
  reclaim a small table, runs as an exclusive compaction, and
  holds up the GC thread while it does. `SM1_DESIGN.md` §8.3
  leaves this open, with a fix proposed and not built: record
  for each file whether it holds unstamped versions, and
  compact only those. That would also be the way to stamp
  sooner on purpose, after a bulk load or a restart.

---

## 6. What it does not do

- **It does not cache aborted transactions.** A version of an
  aborted or crashed transaction misses the active table and
  the cache, and is resolved by `CF:txn` each time: a record
  that says aborted, or no record. It stays that way until
  compaction drops the version.
- **It does not hold a transaction's whole status**, only
  `commit_ts`. A design that adds to a transaction's status,
  as `SAVEPOINT.md` does with dead ranges of `seq`, has to
  add it here too, or every version of such a transaction
  falls through to `CF:txn`.
- **It is not sized by bytes.** The bound is a count of
  entries. Each is two map nodes of two 8-byte numbers, a few
  megabytes in all at the default.

---

## 7. Observations

These follow from the code and are not decisions.

- **Dead transactions are looked up every time.** Caching
  "no record" would spare those reads too. It is sound, since
  a transaction absent from the active table with no record
  can never commit, and it is not built.
- **A scan can flush the cache.** A scan over the unstamped
  rows of more than 65,536 old transactions pushes every
  recent commit out, and each comes back with a disk read
  when next asked about. Eviction by use would resist that,
  at the price given in §3.4.
- **A large transaction is still evicted** by the next 65,536
  entries, however many of its rows are being read. It now
  costs one read to return, where it cost one a row.
- **One mutex.** Every resolution of an unstamped version
  takes `txn_mu`, which every begin, commit and abort takes
  too. No benchmark has measured it.

`test/test_xct.cc` opens a database with
`commit_cache_entries = 1` and checks that reads still see
what was committed. `SM1_DESIGN.md` §12.5 asks for the same
under a concurrent workload. Such tests are what hold the
cache to its one promise: it changes speed, never answers.
