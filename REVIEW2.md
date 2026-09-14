# REVIEW2 — wisr storage manager design review

Review of `DESIGN.md` against the goals, API, and architectural
decisions established in `OBJECTIVE.md` and `API.txt`.

---

## 1. Executive summary

`DESIGN.md` presents a thoughtful, high-performance architecture
for an MVCC storage manager on top of RocksDB. Its core
primitives — the commit sequencer, monotonic `visible_ts`,
custom `wisr.version.v1` comparator, zero-undo crash recovery,
and 2PC prepare durability — satisfy the architectural goals
laid out in `OBJECTIVE.md`.

However, this review identified several critical design flaws
and edge-case discrepancies that must be resolved prior to
implementation:
1. **Broken OLTP writes under SSI (§7.4):** Eager application
   of Ports & Grittner's safe-snapshot optimization causes
   standard read-then-write transactions to fail with
   `SM_ERR_SERIALIZATION` in an infinite retry loop on quiet
   databases.
2. **Scan SIREAD predicate bounds & phantom tracking (§7.1,
   §10.2):** Half-open vs. closed range discrepancies (`[lo, hi)`
   vs. `[skey, last_key_examined]`) risk leaving the boundary
   key unprotected against concurrent modifications, and
   skipped scan version handling is incomplete when no visible
   version exists.
3. **Catalog drop-and-recreate incompatibility (§11 vs. §5):**
   `sm_rmrel` writes a version with `dropped = true` rather than
   a tombstone; subsequent `sm_mkrel` calls fail with
   `SM_ERR_DUPKEY` (`SM_ERR_EXISTS`).
4. **Already-committed pivot handling (§7.3):** The eager SSI
   abort description assumes the pivot can always be doomed at
   its own commit, overlooking pivots that have already
   committed.

---

## 2. Alignment with OBJECTIVE.md

| Requirement / Decision | Status | Notes |
| --- | --- | --- |
| RocksDB as pure KV engine | Aligned | MVCC versions, headers, transaction table, and visibility logic reside entirely in the storage manager. |
| Full MVCC simulation | Aligned | Complemented `~xid` / `~seq` keys with custom comparator `wisr.version.v1` yield correct commit-order seeking. |
| Isolation levels (RC, RR, SSI) | Partially Aligned | RC and RR are sound. Serializable SSI contains a critical flaw in §7.4 that causes false abort loops for OLTP write transactions on idle systems. |
| Eager writes & status table | Aligned | Direct memtable writes, `CF:txn` commit status, fail-stop status write behavior, and zero-undo crash recovery are well specified. |
| True 2PC (`sm_prepare`) | Aligned | Durable `PREPARED` records persist writeset and relids, survive crash, restore locks, and serialize correctly across restarts. |
| Single-process multi-threading | Aligned | In-memory lock manager, SSI graph, process-wide open registry, and ref-counted CF handles match RocksDB constraints. |

---

## 3. Critical findings

### 3.1. Safe-snapshot optimization causes read-then-write serializable transactions to abort in quiet databases (§7.4, §12.5) [WILL FIX]

**Finding.** In §7.4:
"Its snapshot is safe, and its tracking can be dropped, only
once every read-write serializable transaction concurrent with
the snapshot has finished... If there are no such concurrent
transactions at `sm_begin`, the snapshot is safe at once...
The API has no read-only declaration, so 'read-only' means 'has
not written yet' (`T.xid == 0`), and the rule is applied only
while that holds. A transaction whose tracking was dropped and
which then writes has lost the read set it would need as a
pivot; its first write fails with `SM_ERR_SERIALIZATION`."
§12.5 includes a test for this behavior: "A transaction whose
snapshot became safe and then writes fails with
`SM_ERR_SERIALIZATION`."

**The bug.** In an OLTP workload (`OBJECTIVE.md`), transactions
regularly read rows before updating them:
```c
sm_begin(session, SM_XCT_SERIALIZABLE);
sm_read(session, "accounts", "acc_1", 5, &val, &vlen);
sm_update(session, "accounts", "acc_1", 5, new_val, nvlen);
sm_commit(session);
```
If this transaction begins on an idle or quiet database:
1. At `sm_begin`, there are zero concurrent read-write
   transactions.
2. The engine marks the snapshot "safe at once" and drops
   SIREAD tracking.
3. `sm_read` executes without recording a SIREAD lock.
4. `sm_update` is the first write (`T.xid == 0`). The engine
   sees that tracking was dropped and aborts the transaction
   with `SM_ERR_SERIALIZATION`.
5. Per `API.txt`, the application retries: `sm_begin`,
   `sm_read`, `sm_update`.
6. On retry, the database is still quiet. The transaction is
   again marked safe at once, drops tracking, and fails again on
   `sm_update`.

The transaction enters an infinite retry loop and can never
commit. In PostgreSQL (Ports & Grittner 2012), safe-snapshot
optimization applies exclusively to transactions explicitly
declared `READ ONLY`. In `wisr`, `sm_begin` has no read-only
flag; synthesizing "read-only" dynamically as `T.xid == 0`
during execution is unsound.

**Fix.** Do not drop SIREAD tracking while a transaction is
active simply because `T.xid == 0`. Retain SIREAD tracking for
all active serializable transactions until commit. If a
transaction commits with `T.xid == 0` and its snapshot was
safe, its SIREAD locks can be discarded immediately without
retaining them for future pivot detection.

**Note.** Confirmed, and not limited to quiet databases: the
snapshot also becomes safe once its concurrent read-write
transactions finish, so any serializable transaction that
reads, outlives them, and then writes fails every time. The
§12.5 test that asserts the failure must be inverted.

### 3.2. Scan SIREAD predicate upper bound and missed phantom edges (§7.1, §10.2) [WILL FIX]

**Finding.** §7.1 defines a scan's narrowed predicate as
`(relid, [lo, hi))`, which is half-open (exclusive of `hi`).
§10.2 specifies that `sm_scan_end` shrinks the range to
`[skey, last_key_examined] (through ekey if the scan reached
the end)`. `sm_scan_next` does not take point SIREAD locks on
yielded keys, relying entirely on the range predicate.
Additionally, §10.2's `sm_scan_next` steps past concurrent
versions with:
```python
if SERIALIZABLE:
    for each version u before v in the run:
        if concurrent(u, snap): ssi.on_skip(T, u.xid)
```

**The bugs.**
1. If the predicate is stored as half-open `[lo, hi)` where
   `hi = last_key_examined`, then `last_key_examined` itself is
   excluded from conflict checking in `ssi.on_write`. A
   concurrent write to `last_key_examined` will evaluate
   `key < hi` as false, missing the rw-edge.
2. If a key contains only versions written by concurrent
   transactions (in-flight or committed after `snap`), no visible
   version exists (`v == null`). Phrasing the loop as "before v"
   means an implementation skips it when `v` is absent, failing
   to register rw-edges for concurrent phantoms. This also
   breaks the §8.4 recovery rule for restored transactions.

**Fix.** Explicitly specify that the narrowed SIREAD predicate
is inclusive of `last_key_examined` (e.g. closed interval
`[skey, last_key_examined]` or `[skey, successor(last_key))]`).
In §10.2, specify that if no version in the key's run is
visible, all concurrent versions in that run must be passed to
`ssi.on_skip(T, u.xid)`.

**Note.** Both are wording gaps rather than design flaws: §10.1
already walks every version of a key, and §10.2 already says the
narrowed range is closed. The notation still needs to agree.
§10.2 also writes the requested range as `[skey, ekey]`,
ignoring `sm_scan`'s `skey_inclusive` and `ekey_inclusive`.

### 3.3. Recreating a dropped relation fails with SM_ERR_DUPKEY (§11 vs. §5) [WILL FIX]

**Finding.** §11 states that "sm_mkrel inserts a catalog row,
sm_rmrel writes a version with dropped = true, and both roll
back with the transaction." It explicitly notes that drop and
recreate of the same relation name is valid. However, §5 step 3
defines the write path for `sm_insert`:
```python
latest = newest committed version of (relid, k)
if latest is absent or tombstone: SM_ERR_NOTFOUND  # (insert: DUPKEY)
```

**The bug.** `sm_rmrel` writes `{ relid, created_ts, dropped:
true }` as a normal value, not a RocksDB tombstone (flags bit0
is 0, payload contains metadata). When `sm_mkrel` subsequently
inserts `"R" | relname`, `latest` is neither absent nor a
tombstone. Under §5 step 3, `sm_insert` returns `SM_ERR_DUPKEY`
(mapped to `SM_ERR_EXISTS`). Dropped relations can never be
recreated.

**Fix.** Specify in §11 that catalog row insertion diverges
from standard user-table `sm_insert`: when inserting into
`CF:rel.0`, if the newest committed version has `dropped ==
true`, the catalog treats the relation name as available,
succeeds, and writes a new version with `dropped = false` and a
freshly leased `relid`.

**Note.** Confirmed, but fixed differently: `sm_rmrel` deletes
the catalog row, so the name's newest version is a tombstone
and `sm_mkrel` stays an ordinary §5 insert with no catalog
special case. The tombstone carries no relid, so the
transaction tracks dropped relids in `T.dropped` (savepoint-
aware, and persisted in a `PREPARED` record like `created`),
and commit queues them for the background CF drop. The
`dropped` field goes away. Per `API.txt`, `sm_mkrel` on a live
name reports `SM_ERR_EXISTS`, not `SM_ERR_DUPKEY`.

### 3.4. Eager abort logic when the pivot has already committed (§7.3) [WILL FIX]

**Finding.** §7.3 specifies in pseudocode:
```python
if T has an out-edge to some T' that already committed
   and T' also has in+out conflicts:
    abort whichever is still running
```
The explanatory text in lines 560–565 states: "Concretely, the
check runs at prepare/commit time and, for the case where the
pivot is not the last to commit, eagerly when the edge is
created (mark the pivot as doomed; it fails at its own commit).
A prepared pivot cannot be doomed — it has already been told
'yes' — so when the pivot is prepared, the transaction whose new
edge completed the structure is doomed instead (§8.4)."

**The bug.** If `T'` has already committed, and `T`
subsequently reads past `T'`'s write (creating edge
`T ──rw──▶ T'`), `T'` is the pivot and already has an out-edge.
`T'` cannot be marked as doomed to fail at its own commit
because `T'` has already committed. The text notes the
exception for a prepared pivot, but fails to state that an
already-committed pivot also cannot be doomed, and the running
transaction (`T`) must be doomed instead.

**Fix.** Update §7.3 explanatory text to explicitly state that
when an incoming edge is formed to a pivot that is already
committed or prepared, the running transaction creating the
edge (`T`) is doomed immediately.

**Note.** Minor. The commit-time check in §7.3's pseudocode
already aborts the running transaction; only the sentence on
eager dooming is incomplete. The rule should cover any new edge
that completes the structure around a committed or prepared
pivot, not only an incoming one.

---

## 4. Medium & operational findings

### 4.1. Unbounded growth of in-memory commit_cache (§3.4, §8.2, §12.2) [WILL FIX]

**Finding.** §8.2 step 5 inserts every committing transaction
into `commit_cache`. Neither §8.2 nor the configuration knobs in
§12.2 define an eviction policy, maximum entry count, or memory
budget.

**Impact.** In an OLTP workload with sustained transaction
throughput, `commit_cache` will grow monotonically without
bound, eventually exhausting process memory.

**Fix.** Specify in §8.2 and §12.2 that `commit_cache` is a
fixed-size LRU cache. Because `CF:txn` durably retains records
until swept, evictions from `commit_cache` fall back safely to
step 3 (`CF:txn` lookup) in §3.4 without affecting correctness.

**Note.** Any size bound works; LRU is not required. Evicting
the oldest `commit_ts` first keeps the recent tail, which is
what stamping leaves for the cache to answer (§8.3).

### 4.2. Underspecified per-session catalog cache invalidation (§11) [WILL FIX]

**Finding.** §11 states: "A session caches `relname → relid`
and invalidates the cache entry when it observes a catalog
version newer than the cached one."

**Impact.** If a session checks its cache first, it never
accesses `CF:rel.0` on hit and will never observe a newer
catalog row. If it checks `CF:rel.0` on every lookup, the cache
provides no benefit.

**Fix.** Add a lightweight process-wide atomic counter
`catalog_epoch`, incremented whenever a DDL transaction commits.
Sessions store `(relid, epoch)` in their local cache. On
lookup, if `session.last_seen_epoch < catalog_epoch`, the
session revalidates against `CF:rel.0`; otherwise, cache hits
are served instantly.

**Note.** The gap is real, but the proposed epoch counter is
blind to snapshots, the same flaw for which REVIEW.md §3.1 was
declined:
1. A session with snapshot 10 finds `catalog_epoch` raised by
   a drop-and-recreate of `R` committed at 20.
2. It revalidates under snapshot 10, correctly gets the old
   relid, and records the new epoch.
3. Its next transaction, at snapshot 25, gets a cache hit and
   uses the dropped relid.

The fix compares timestamps instead: a process-wide
`last_ddl_ts` (the largest `commit_ts` of a committed catalog
write, raised before that commit is published), and a hit is
used only when `last_ddl_ts` is at or below both the entry's
snapshot and the lookup's. The transaction's own uncommitted
DDL bypasses the cache.

### 4.3. Read Committed transactions pin gc_watermark indefinitely (§8.3, §6.2) [WONT FIX]

**Finding.** §8.3 defines:
```python
gc_watermark = min(snapshot_ts of active sessions,
                   snapshot_ts of prepared transactions,
                   oldest open scan)
```
In §6.2, a Read Committed transaction takes `T.snapshot_ts` once
at `sm_begin` and never refreshes it.

**Impact.** If a Read Committed session remains idle between
operations, its `T.snapshot_ts` remains fixed at its begin
timestamp, pinning `gc_watermark` and halting RocksDB compaction
GC across all column families. However, an idle RC session
performs reads using statement-local snapshots (`visible_ts`)
and does not require historical versions between statements.

**Fix.** Clarify in §8.3 that active RC sessions only pin
`gc_watermark` during an active statement execution (or via an
open scan cursor), rather than continuously from `sm_begin`.

**Refutation.** A read-committed transaction's begin snapshot
is live between statements: §6.2 checks every write against
`T.snapshot_ts`. Unpinning it lets GC erase the evidence that
check needs, and a committed row is lost:
1. RC `T` begins at snapshot 10, writes `j`, and gets xid 11.
2. `D` (xid 12) deletes `k` and commits at 13.
3. `T` no longer pins the watermark, so it passes 13, and the
   tombstone purge issues `DeleteRange(k|~12…, k|FF×12)`.
4. `T` inserts `k`. No committed version is left, so the §5.2
   check passes; `T` writes `k|~11` and commits.
5. A repeated purge entry for `k` (§8.3 allows repeats, and a
   lower-level copy of the tombstone can requeue one) issues
   the same `DeleteRange` again. `k|~11` sorts after `k|~12`,
   so `T`'s committed row is deleted.

§8.3 calls repeats harmless because §4.1 guarantees a later
write sorts before the tombstone; step 4 broke that guarantee.
With the pin, the watermark stays at or below 10, the tombstone
survives, and step 4 fails with `SM_ERR_CONFLICT`. Holding GC
for an idle open transaction is the standard cost (PostgreSQL's
idle-in-transaction behaves the same way); the remedy is to
end the transaction, not to unpin it.

### 4.4. Lock tracking on savepoint rollback with repeated writes (§5, §6.5) [WILL FIX]

**Finding.** §6.5 tracks locks via `T.sp_locks = T.locks.size()`,
and on rollback executes `release locks acquired after the
mark`.

**Impact.** If `T` locks key `k` before the savepoint, and
updates key `k` again after the savepoint: if `lockmgr.acquire_x`
appends to `T.locks` on every write, truncating to `sp_locks`
and releasing locks acquired after the mark would release the
lock on `k`, even though `T` held it before the savepoint.

**Fix.** Clarify in §5.1 and §6.5 that `T.locks` records unique
keys. If a transaction writes to an already-locked key after the
savepoint, it does not record a new entry in `T.locks`,
ensuring pre-savepoint locks are never prematurely released.

**Note.** Deduplicating by key is not enough. §5.1 keys locks by
hash, so a key written after the savepoint can share an entry
with one locked before it, and releasing that entry drops both.
`T.locks` must record lock entries, appended only on first
acquisition.

---

## 5. Architectural strengths

1. **Monotonic commit sequencer & free snapshots (§4.3):**
   In-order publication via `visible_ts` provides O(1) snapshots
   without per-snapshot active transaction lists.
2. **Version layout & custom comparator (§3.1):** Complemented
   `~xid` / `~seq` with `wisr.version.v1` guarantees that
   userkeys sort lexicographically while internal versions sort
   newest-first, allowing point reads to locate the
   target version in a single seek.
3. **No-undo crash recovery (§3.4, §9.1):** Eager uncommitted
   writes from crashed transactions resolve to dead status
   automatically, enabling O(1) startup recovery relative to data
   size.
4. **Resilient 2PC design (§8.4):** Durable `PREPARED` records
   surviving restart, paired with the restored-transaction
   dooming rule, preserve SSI guarantees across crash boundaries
   without requiring durable SIREAD lock graphs.
