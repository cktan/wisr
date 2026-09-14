# REVIEW — wisr storage manager design

Review of `DESIGN.md` against the goals, API, and architectural
decisions established in `OBJECTIVE.md` and `API.txt`.

---

## 1. Executive summary

`DESIGN.md` presents a thoughtful, high-performance architecture
for an MVCC storage manager on top of RocksDB. It adheres to the
core decisions in `OBJECTIVE.md`:
- Snapshot isolation everywhere with SSI (optimistic) and
  first-writer-wins row locks.
- Eager writes placed directly into RocksDB with status resolved
  via `CF:txn` and a commit sequencer.
- True 2PC with durable `PREPARED` records surviving restart and
  restoring row locks.
- Single-process deployment with shared in-memory structures.

However, a detailed review reveals 5 critical concurrency/logic
bugs, 4 specification discrepancies with `API.txt`, and 4 storage
engine edge cases around RocksDB compactions, tombstones, and
ring buffers that should be addressed before implementation.

---

## 2. Alignment with OBJECTIVE.md

| Requirement / Decision | Status | Notes |
| --- | --- | --- |
| RocksDB as pure KV engine | Aligned | MVCC versions, headers, transaction table, and visibility are simulated in the storage manager. |
| Isolation levels (RC, RR, Serializable) | Partially Aligned | Architecture is sound, but SSI predicate tracking and RC point reads contain critical flaws (§3.1, §3.2, §3.3). |
| Eager writes & status table | Aligned | Disk-backed write capacity, O(1) abort, zero-undo crash recovery. Status resolution rule has a gap for long runs (§4.2). |
| True 2PC (`sm_prepare`) | Partially Aligned | WAL durability is solid, but SSI serializability across crash recovery has a loophole (§3.5), and `prepared_at` is missing (§5.2). |
| Single-process multi-threading | Aligned | Process-wide open registry, shared column family map, and in-memory lock/SSI structures. |
| Complete `sm_*` API surface | Aligned | Reconciled with `API.txt`, with minor stale references in §6.5 and §8.2 (§5.1, §5.3). |

---

## 3. Critical correctness and concurrency findings

### 3.1. Read Committed point read skips newer committed versions (§10.1) [WILL FIX]

In `DESIGN.md` §10.1:
```python
snap = (level == READ_COMMITTED) ? visible_ts : T.snapshot_ts
hi   = (T.xid != 0) ? T.xid : snap
it.Seek(key | ~hi | 0x00000000)
```

**The bug.** Under `READ_COMMITTED`, `snap` is updated to
`visible_ts` on every statement. If transaction T writes early,
T.xid is assigned a low timestamp (e.g. 50). Later, another
transaction commits key K at commit_ts = 100 (xid = 80).
T then reads key K at `READ_COMMITTED`
(snap = visible_ts = 105). Because T.xid != 0, the formula
sets hi = T.xid = 50, instead of 105.

The iterator seeks to key | ~50 | 0x00000000.
Because 80 > 50 => ~80 < ~50, key K's committed version sorts
*before* the seek target in RocksDB comparator order. The seek
completely skips the newly committed version, violating Read
Committed visibility.

**Fix.** Calculate hi as max(T.xid, snap):
```c
hi = (T.xid > snap) ? T.xid : snap;
```

### 3.2. SSI predicate window vulnerability in `sm_scan` (§10.2, §7.1) [WILL FIX]

In `DESIGN.md` §10.2:
> "For SERIALIZABLE, `sm_scan_end` registers the SIREAD range
> actually covered, `[skey, last_key_examined]`, as the
> predicate (§7.1)."

**The bug.** If a transaction opens a scan on [a, z] and iterates
but has not yet called `sm_scan_end`:
1. A concurrent transaction T2 writes key b1 and calls
   `ssi.on_write(T2, relid, "b1")`.
2. Because the reader has not called `sm_scan_end` yet, the
   SIREAD lock is not registered in the SSI tracker.
3. T2 finds no SIREAD lock covering b1 and creates no rw-edge.
4. T2 commits.
5. The reader finishes iterating and calls `sm_scan_end`,
   registering [a, last].

The anti-dependency T_reader --rw--> T2 is missed. If T2 also
read a key the reader later writes, a phantom anomaly occurs
undetected under `SERIALIZABLE`.

**Fix.** The SIREAD range predicate must be registered in the
SSI tracker at `sm_scan` time over the requested range
[skey, ekey]. At `sm_scan_end`, the registered range can be
shrunk to [skey, last_key_examined].

### 3.3. Missing SSI tracking for non-existent keys in point read (§10.1, §7.2) [WILL FIX]

In `DESIGN.md` §10.1:
```python
while it.Valid() and userkey(it.key()) == key:
    v = decode(it)
    if visible(v, snap, T):
        if SERIALIZABLE: ssi.on_read(T, relid, key, v)
        return v.tombstone ? NOT_FOUND : v.payload
    it.Next()
return NOT_FOUND
```

**The bug.** If a key does not exist (or all versions in the DB
are invisible/tombstones), the loop terminates and returns
`NOT_FOUND` without calling `ssi.on_read`. In `SERIALIZABLE`,
reading a non-existent key is a predicate read ("assert key K
does not exist"). If another transaction subsequently inserts K
and commits, no SIREAD lock exists on K, so no conflict edge is
registered.

Furthermore, when `it.Next()` steps past invisible versions
from concurrent transactions, §7.2's requirement to add an
edge to the concurrent writer is ignored.

**Fix.** Call `ssi.on_read(T, relid, key, NULL)` on all return
paths of `sm_read`, including when no row is found, and record
skipped concurrent versions in the SSI graph.

**Note.** Two corrections. A *visible* tombstone already
calls `ssi.on_read` before returning `NOT_FOUND`; only the
loop-exhausted path misses it. And the gap is wider than
`it.Next()`: with `T.xid == 0` the seek target is `~snap`, so
versions with `xid > snap` sort before it and are never
visited. Those are exactly the concurrent writers §7.2 needs.
For SERIALIZABLE the fix must also examine versions above
`hi`, e.g. by seeking to `key | 00×12`.

### 3.4. Write path ignores own prior writes (§5) [WILL FIX]

In `DESIGN.md` §5:
```python
latest = newest committed version of (relid, k)
if latest.commit_ts > T.snapshot_ts: abort with SM_ERR_CONFLICT
if latest is absent or tombstone: SM_ERR_NOTFOUND # (insert: DUPKEY)
```

**The bug.** Step 3 checks only the newest **committed**
version:
- If T inserts key K and later calls `sm_update(rel, K, val2)`
  in the same transaction: because T is uncommitted, the
  newest committed version is absent. Step 3 returns
  `SM_ERR_NOTFOUND` on a key T just inserted.
- If T deletes key K and later calls `sm_insert(rel, K, val2)`:
  the newest committed version is the pre-existing row, so
  `sm_insert` erroneously fails with `SM_ERR_DUPKEY`.

**Fix.** Step 3 must check for T's own latest write to K
first. If present, that determines whether K currently exists
for T. Only if T has not written K does it inspect the
latest committed version.

### 3.5. SSI write-skew anomaly across crash recovery with prepared transactions (§8.4, §9.1) [WILL FIX]

§8.4 asserts:
> "Two prepared transactions were each live during the other's
> validation, so they were checked against each other — which
> is what makes the post-recovery state safe without replaying
> the SSI graph."

**The bug.** This holds between two transactions prepared before
the crash, but fails between a restored prepared transaction
and a new transaction started after restart:
1. Before crash: T_prep (SERIALIZABLE) reads key A (value a0)
   and writes key B (value b1). T_prep prepares. Its read of A
   is not stored in `CF:txn` (only `writeset = [B]`).
2. Crash and restart occur. T_prep is restored in-doubt and
   re-acquires its row lock on B. Its SIREAD lock on A is
   lost.
3. T_new (SERIALIZABLE) begins. T_new reads B. Because b1 is
   uncommitted/prepared, T_new reads b0, creating an edge
   T_new --rw--> T_prep.
4. T_new writes key A (value a1). Because T_prep's SIREAD
   lock on A was not restored, no edge T_prep --rw--> T_new
   is created.
5. T_new commits without conflict.
6. Coordinator calls `sm_commit_prepared(gid)`. T_prep commits.
7. History: T_prep read A=a0, wrote B=b1; T_new read B=b0,
   wrote A=a1. This is the canonical write-skew anomaly
   under SERIALIZABLE.

**Fix.** Either:
1. Store the SIREAD lock set in `CF:txn` on `sm_prepare` and
   restore them during recovery, OR
2. Follow PostgreSQL's SSI rule for 2PC: any transaction that
   incurs an rw-antidependency with a restored in-doubt
   transaction must be aborted at commit.

**Note.** Option 2 is not PostgreSQL's rule. PostgreSQL
persists predicate locks in the 2PC state file and, on
recovery, sets both summary conflict flags on the restored
transaction. Option 1 alone is also insufficient, because the
lost `in_conflict`/`out_conflict` flags matter too. Option 2
is sufficient on its own: post-restart transactions can only
reach a restored transaction via an rw-edge from reading its
writes, which is detectable. Aborting every such transaction
breaks every cycle through it. The rule must still apply
after the in-doubt transaction is resolved.

---

## 4. Storage engine, compaction, and GC mechanics

### 4.1. Tombstone leak in compaction filter (§8.3) [WILL FIX]

In `DESIGN.md` §8.3:
```python
filter(version_key, value):
    resolve xid
    if dead (aborted / crashed) → DROP
    if committed and commit_ts > gc_watermark → KEEP as-is
    if a newer version of the same userkey with commit_ts <= gc_watermark was already seen → DROP
    otherwise → KEEP, and stamp commit_ts into the header
```

**The issue.** When a key is deleted, a tombstone version is
written. When compaction runs, versions older than the
tombstone are dropped. But for the tombstone itself, there is
no newer version, so it hits `otherwise -> KEEP`. In
subsequent compactions, the tombstone is never dropped. Every
deleted key permanently retains a tombstone record in RocksDB,
leading to unbounded storage growth.

**Fix.** Check `CompactionFilter::Context::is_bottommost_level`.
If the version is a tombstone and `is_bottommost_level == true`
(and no live snapshot is older than its commit_ts), the
tombstone can be dropped safely without resurrecting older
versions.

**Note.** The leak is real, but the proposed fix does not
work. `CompactionFilter::Context` has no
`is_bottommost_level` field. Even at the bottommost level,
dropping is unsafe. RocksDB picks compaction inputs by its
own key, which is the full version key. A userkey's older
versions can therefore sit in an adjacent SST file, or
another subcompaction, that this filter never sees.
Dropping the tombstone would resurrect them. A safe
alternative is to queue the userkey and have a background
task issue `DeleteRange(tombstone_key, userkey | FF×12)`.
That covers the tombstone and every older version at all
levels, while newer versions sort before it and survive.

### 4.2. Status resolution rule gap for long-running processes (§3.4) [WILL FIX]

In `DESIGN.md` §3.4:
> "4. No record, and `xid < recovery_watermark` -> the writer
> died before committing. Dead."

`recovery_watermark` is set at startup to `next_ts` and never
advances. Any transaction started after startup has
xid > recovery_watermark. If an `ABORTED` transaction record
is purged from `CF:txn` during long uptime, its orphaned data
versions will have xid > recovery_watermark and no record in
`CF:txn`. Under Rule 4, its status is undefined.

**Fix.** In a single-process deployment, the active
transaction table is exhaustive. Rule 4 should state:
*"If xid is not in the active transaction table, not in the
commit cache, and has no COMMITTED record in `CF:txn`, the
transaction is dead."*

**Note.** The gap does not depend on purging. A commit whose
status write fails (§4.3: "the value simply goes unused"), or
an abort whose hint write fails, leaves an xid above the
watermark with no record. The fix is safe only if a
committer inserts into the commit cache before it leaves the
active transaction table.

### 4.3. Commit sequencer wrap-around and backpressure (§4.3) [WILL FIX]

The commit sequencer uses a ring of slots indexed by timestamp.
If slot K is delayed by a slow WAL sync or group commit batch,
while high-frequency writers allocate timestamps K+1 to K+N,
the ring buffer of size N will wrap around.

**Clarification needed.** Specify the ring buffer sizing and
backpressure mechanism: `sequencer.claim(commit_ts)` must
block if the distance between `commit_ts` and `visible_ts`
reaches the ring capacity.

**Note.** Xid allocation also consumes slots (§4.3), so
backpressure would have to block xid allocation as well, not
just `claim`. Alternatively, drop the ring: only in-progress
commits hold back `visible_ts`, and there are at most one per
session. A mutex-guarded ordered set gives
`visible_ts = min(in-progress) - 1`, or `global_ts - 1` when
the set is empty, with no sizing and no backpressure.

### 4.4. Compaction filter stamps in-flight versions (§8.3) [WILL FIX]

In `DESIGN.md` §8.3:
```python
filter(version_key, value):
    resolve xid
    if dead (aborted / crashed) → DROP
    if committed and commit_ts > gc_watermark → KEEP as-is
    if a newer version of the same userkey with commit_ts <= gc_watermark was already seen → DROP
    otherwise → KEEP, and stamp commit_ts into the header
```

**The bug.** The filter has no case for an xid that resolves
as in-flight (active or prepared). Such a version is not
dead, not committed, and is the newest version of its key,
so it falls through to `otherwise → KEEP, and stamp
commit_ts`. There is no `commit_ts` to stamp. Compaction runs
at any time, so this happens whenever a flush or compaction
covers a key that a running transaction has written:
1. T writes key K; the version is flushed to an SST file.
2. Compaction runs while T is still active.
3. The filter resolves T's xid as in-flight and stamps a
   header with bit1 set and an undefined `commit_ts`.
4. Readers trust the hint and skip status resolution (§8.3:
   "most versions answer visibility without a status
   lookup"). If the stamped value is at or below their
   snapshot, they see T's uncommitted write — a dirty read.
5. If T later aborts, the hint still says committed, so
   readers keep seeing the write and a later filter pass
   that trusts the hint never drops it.

**Fix.** Add an explicit case before `otherwise`:
`if in-flight (active / prepared) → KEEP as-is`. Stamp only
versions that resolve as COMMITTED with
`commit_ts <= gc_watermark`.

---

## 5. Discrepancies between DESIGN.md and API.txt

| Item | Location in `DESIGN.md` | Location in `API.txt` | Resolution |
| --- | --- | --- | --- |
| `sm_rollback` without Savepoint [WILL FIX] | §6.5 line 400: `m = (savepoint exists) ? T.sp_mark : 0 # else roll back to begin` | Divergence 8 & line 367: `sm_rollback` requires a savepoint, fails with `SM_ERR_NOSAVEPOINT` | Remove fallback to 0 in §6.5; return `SM_ERR_NOSAVEPOINT`. |
| `prepared_at` in `sm_prepared_t` [WILL FIX] | §3.2: `CF:txn` value schema omits `prepared_at` | Line 413: `sm_prepared_t` has `long long prepared_at` (epoch seconds) | Add `prepared_at` to the `PREPARED` record value in `CF:txn`. |
| Cursor Invalidation on Commit/Abort [WILL FIX] | §8.2 & §6.6 omit cursor cleanup | Lines 310 & 327: `sm_commit` and `sm_abort` close all open scan cursors on the session | Explicitly add closing open cursors and releasing their RocksDB snapshots in §8.2 and §6.6. |
| Process-Wide Catalog Invalidation [WONT FIX] | §11: "A session caches relname → relid and invalidates ... when it observes a catalog version newer..." | N/A (single process architecture) | Because all sessions run in one process sharing one DB, use a process-shared catalog cache with direct invalidation upon DDL commit. |

**Cursor invalidation note.** Under SERIALIZABLE, cursors
still open at `sm_commit` or `sm_prepare` must be closed, and
their ranges registered, *before* `ssi.validate`. Otherwise
their reads escape validation. This ordering is moot if §3.2
registers ranges at `sm_scan` time.

**Catalog invalidation refutation.** This is not a
discrepancy with `API.txt`; the row itself says N/A. The
proposed resolution is also wrong. The catalog is an MVCC
relation, so `relname → relid` depends on the reader's
snapshot. After a committed `sm_rmrel` plus `sm_mkrel` of the
same name, a transaction with an older snapshot must still
resolve the old relid. `API.txt` says such snapshots "may
still legitimately read the relation". Invalidating a shared
cache at DDL commit would send that transaction to the new
relation. A per-session cache is correct because a session's
snapshots only move forward. How a session detects a newer
catalog version is underspecified in §11, but that is a
separate clarification, not this change.

---

## 6. Architectural strengths

1. **Clean separation of concerns.** RocksDB handles block
   caching, WAL durability, SST compaction, and storage;
   `wisr` implements transaction tables, row locks, SIREAD
   locks, and version decoding.
2. **O(1) snapshot acquisition.** Monotonically advancing
   `global_ts` combined with in-order publication via
   `visible_ts` eliminates PostgreSQL-style transaction
   snapshot array allocations.
3. **No-undo crash recovery.** Eager uncommitted writes are
   marked dead post-crash because their xid falls below the
   persisted watermark, allowing crash recovery to complete
   in O(1) time relative to data size.
4. **Efficient custom comparator (`wisr.version.v1`).**
   Byte-reversed `~xid` and `~seq` ensure the newest version
   of any key is sought in a single `Seek()` without reading
   backwards.
5. **CF-per-relation architecture.** Enables instantaneous
   physical space reclamation on `sm_rmrel` via
   `DropColumnFamily` once snapshots pass the drop watermark.

---

## 7. Actionable recommendations for DESIGN.md

1. **Update §10.1:** Change `hi = (T.xid != 0) ? T.xid : snap`
   to `hi = (T.xid > snap) ? T.xid : snap`.
2. **Update §10.2:** Register the SIREAD predicate range at
   `sm_scan` cursor creation; tighten to
   `[skey, last_key_examined]` at `sm_scan_end`.
3. **Update §10.1:** Add `ssi.on_read` registration when
   returning `NOT_FOUND`, and track skipped concurrent
   versions for rw-edges.
4. **Update §5:** In step 3, check T's own uncommitted write
   set for key K before checking committed versions.
5. **Update §8.4 & §9.1:** Specify that post-restart
   serializable transactions that form an rw-dependency with
   an in-doubt transaction abort at commit.
6. **Update §8.3:** Add bottommost level check to drop
   tombstones during compaction.
7. **Update §6.5:** Remove the fallback to 0 on `sm_rollback`
   and specify returning `SM_ERR_NOSAVEPOINT`.
8. **Update §3.2:** Add `prepared_at` to the `CF:txn` value
   schema for `PREPARED` records.
