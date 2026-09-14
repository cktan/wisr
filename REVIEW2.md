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

This pass found a broken safe-snapshot rule under SSI, loose
scan predicate bounds, a catalog that could not recreate a
dropped relation, an incomplete rule for committed pivots, an
unbounded commit cache, an unspecified catalog cache check, and
ambiguous savepoint lock tracking. All confirmed findings have
been fixed in `DESIGN.md` and removed from this document. One
finding was declined and is recorded in §3 with its refutation.

---

## 2. Alignment with OBJECTIVE.md

| Requirement / Decision | Status | Notes |
| --- | --- | --- |
| RocksDB as pure KV engine | Aligned | MVCC versions, headers, transaction table, and visibility logic reside entirely in the storage manager. |
| Full MVCC simulation | Aligned | Complemented `~xid` / `~seq` keys with custom comparator `wisr.version.v1` yield correct commit-order seeking. |
| Isolation levels (RC, RR, SSI) | Aligned | Serializable transactions stay tracked while active; safe snapshots release a read-only commit's SIREAD locks early. Scan predicates follow `sm_scan`'s bounds. |
| Eager writes & status table | Aligned | Direct memtable writes, `CF:txn` commit status, bounded commit cache, fail-stop status write behavior, and zero-undo crash recovery are well specified. |
| True 2PC (`sm_prepare`) | Aligned | Durable `PREPARED` records persist writeset, created and dropped relids, survive crash, restore locks, and serialize correctly across restarts. |
| Single-process multi-threading | Aligned | In-memory lock manager, SSI graph, process-wide open registry, ref-counted CF handles, and a timestamp-checked catalog cache match RocksDB constraints. |

---

## 3. Declined findings

### 3.1. Read Committed transactions pin gc_watermark indefinitely (§8.3, §6.2) [WONT FIX]

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

---

## 4. Architectural strengths

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
