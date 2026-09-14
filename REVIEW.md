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

Two review passes found concurrency bugs, `API.txt`
discrepancies, and storage engine edge cases. All confirmed
findings have been fixed in `DESIGN.md` and removed from this
document. Three findings were declined and are recorded in §3
with their refutations.

---

## 2. Alignment with OBJECTIVE.md

| Requirement / Decision | Status | Notes |
| --- | --- | --- |
| RocksDB as pure KV engine | Aligned | MVCC versions, headers, transaction table, and visibility are simulated in the storage manager. |
| Isolation levels (RC, RR, Serializable) | Aligned | Snapshot isolation at every level; SSI with SIREAD point and range predicates, read-side rw-edges for reads and scans, and safe snapshots for read-only transactions. |
| Eager writes & status table | Aligned | Disk-backed write capacity, O(1) abort, zero-undo crash recovery; stamped commit_ts survives the CF:txn sweep; failed status writes fail stop. |
| True 2PC (`sm_prepare`) | Aligned | Durable `PREPARED` records for writing and read-only transactions, restored in-doubt with row locks, serializable across restart. |
| Single-process multi-threading | Aligned | Process-wide open registry, shared column family map, and in-memory lock/SSI structures. |
| Complete `sm_*` API surface | Aligned | Reconciled with `API.txt`. |

---

## 3. Declined findings

### 3.1. Process-wide catalog invalidation (§11) [WONT FIX]

**Finding.** §11 says "A session caches relname → relid and
invalidates the cache entry when it observes a catalog version
newer than the cached one." Because all sessions run in one
process sharing one DB, use a process-shared catalog cache with
direct invalidation upon DDL commit.

**Refutation.** This is not a discrepancy with `API.txt`, which
says nothing about catalog caching. The proposed resolution is
also wrong. The catalog is an MVCC relation, so
`relname → relid` depends on the reader's snapshot. After a
committed `sm_rmrel` plus `sm_mkrel` of the same name, a
transaction with an older snapshot must still resolve the old
relid. `API.txt` says such snapshots "may still legitimately
read the relation". Invalidating a shared cache at DDL commit
would send that transaction to the new relation. How a session
detects a newer catalog version was later specified in §11: a
cache hit is checked against the commit timestamp of the latest
DDL, a fix from the `REVIEW2.md` pass.

### 3.2. Read-committed writes can break the version-chain invariant (§5, §6.2, §4.1) [WONT FIX]

§6.2 says that under read committed "a fresh
`snapshot_ts = visible_ts` is taken at the start of each
statement", and §5 step 3 rejects a write only when
`latest.commit_ts > T.snapshot_ts`. §4.1's invariant — for one
key, descending `xid` is descending `commit_ts` — rests on
`T.xid > T.snapshot_ts`, which a refreshed snapshot violates
once `T` has an xid.

**Finding.** If the write check uses the refreshed snapshot:
1. RC transaction `T2` writes key `j`; `T2.xid = 50`.
2. `T1` (`xid = 60`) updates key `k` and commits at 70.
3. `T2` updates `k`. Its statement snapshot is 75, and
   `70 <= 75`, so the check passes. It writes `k|~50`.
4. `T2` commits at 80.
5. `T1`'s version (`xid 60`) sorts before `T2`'s (`xid 50`).
   A reader at 90 seeks and returns `T1`'s value, and once
   `gc_watermark >= 70` the compaction filter treats `T2`'s
   committed version as superseded and drops it.

`T2`'s committed update is lost. §4.1's line
"`snapshot_ts <= visible_ts < xid`" is also false under read
committed after the first write (§10.1 already relies on
`snap > T.xid`).

**Fix.** The write check must compare against a bound below
`T.xid`: `bound = (T.xid != 0) ? min(snap, T.xid) : snap`, and
conflict when `latest.commit_ts > bound`. State in §6.2 which
snapshot writes use, and qualify §4.1 for read committed.

**Refutation.** The premise does not hold: `DESIGN.md` never
refreshes `T.snapshot_ts`. §4.1 and §6.1 take it once at
`sm_begin` at every level. §10.1 keeps the read-committed
statement snapshot in a local,
`snap = (level == READ_COMMITTED) ? visible_ts : T.snapshot_ts`,
a conditional that would be pointless if `T.snapshot_ts` were
overwritten. §6.2 lists `sm_read` and `sm_scan` as the
statements that take one. §5 step 3 therefore compares against
the begin snapshot, which is `< T.xid`. In the scenario, step 3
sees `70 > T2.snapshot_ts` and fails with `SM_ERR_CONFLICT`, so
the invariant holds. For the same reason §4.1's
`snapshot_ts <= visible_ts < xid` is true: it is about
`T.snapshot_ts`, not the statement snapshot. The proposed
`min(snap, T.xid)` bound would let read-committed writes
succeed more often. That is a behaviour change, not a fix.

### 3.3. §5.2 cross-reference to §7.4 [WONT FIX]

**Finding.** §5.2 refers to "the caveat noted in §7.4", which
§7.4 does not contain.

**Refutation.** §7.4 is where the non-serializable levels'
interaction with the write path is qualified. Their writes
still feed the SSI tracker, but they are never serialization
victims. That is the caveat on "applies at all three isolation
levels". The reference is loose, not dangling.

---

## 4. Architectural strengths

1. **Clean separation of concerns.** RocksDB handles block
   caching, WAL durability, SST compaction, and storage;
   `wisr` implements transaction tables, row locks, SIREAD
   locks, and version decoding.
2. **O(1) snapshot acquisition.** Monotonically advancing
   `global_ts` combined with in-order publication via
   `visible_ts` eliminates PostgreSQL-style transaction
   snapshot array allocations.
3. **No-undo crash recovery.** Eager uncommitted writes of a
   crashed transaction resolve as dead because it is absent
   from the active transaction table and has no status record,
   allowing crash recovery to complete in O(1) time relative
   to data size.
4. **Efficient custom comparator (`wisr.version.v1`).**
   Byte-reversed `~xid` and `~seq` ensure the newest version
   of any key is sought in a single `Seek()` without reading
   backwards.
5. **CF-per-relation architecture.** Enables instantaneous
   physical space reclamation on `sm_rmrel` via
   `DropColumnFamily` once snapshots pass the drop watermark.
