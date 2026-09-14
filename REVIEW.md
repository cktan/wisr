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

The concurrency bugs, `API.txt` discrepancies, and storage
engine edge cases found by this review have been fixed in
`DESIGN.md` and removed from this document. One finding was
declined and is recorded in §3 with its refutation.

---

## 2. Alignment with OBJECTIVE.md

| Requirement / Decision | Status | Notes |
| --- | --- | --- |
| RocksDB as pure KV engine | Aligned | MVCC versions, headers, transaction table, and visibility are simulated in the storage manager. |
| Isolation levels (RC, RR, Serializable) | Aligned | Snapshot isolation at every level; SSI with SIREAD point and range predicates for SERIALIZABLE. |
| Eager writes & status table | Aligned | Disk-backed write capacity, O(1) abort, zero-undo crash recovery. |
| True 2PC (`sm_prepare`) | Aligned | Durable `PREPARED` records, restored in-doubt with row locks, serializable across restart. |
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
would send that transaction to the new relation. A per-session
cache is correct because a session's snapshots only move
forward. How a session detects a newer catalog version is
underspecified in §11, but that is a separate clarification,
not this change.

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
