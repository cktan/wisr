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
engine edge cases found by the first pass have been fixed in
`DESIGN.md` and removed from this document. One finding was
declined and is recorded in §4 with its refutation.

A second pass over the revised `DESIGN.md` (§3) found 6
confirmed correctness issues — status resolution after GC,
scan conflict tracking, savepoint visibility, the read-only
SSI optimization, read-only commit/prepare, and failed commit
writes — plus 4 confirmed minor issues. Two observations were
refuted: read-committed writes (§3.1) and the §5.2
cross-reference (§3.8).

---

## 2. Alignment with OBJECTIVE.md

| Requirement / Decision | Status | Notes |
| --- | --- | --- |
| RocksDB as pure KV engine | Aligned | MVCC versions, headers, transaction table, and visibility are simulated in the storage manager. |
| Isolation levels (RC, RR, Serializable) | Partially Aligned | Scans and the read-only optimization miss rw-edges (§3.3, §3.5); savepoint visibility leaks (§3.4). |
| Eager writes & status table | Partially Aligned | Status resolution ignores the commit_ts hint, so GC can drop committed data (§3.2). |
| True 2PC (`sm_prepare`) | Partially Aligned | Read-only prepare has no xid to key its record (§3.6). |
| Single-process multi-threading | Aligned | Process-wide open registry, shared column family map, and in-memory lock/SSI structures. |
| Complete `sm_*` API surface | Aligned | Reconciled with `API.txt`. |

---

## 3. Findings (second pass)

### 3.1. Read-committed writes can break the version-chain invariant (§5, §6.2, §4.1) [WONT FIX]

§6.2 says that under read committed "a fresh
`snapshot_ts = visible_ts` is taken at the start of each
statement", and §5 step 3 rejects a write only when
`latest.commit_ts > T.snapshot_ts`. §4.1's invariant — for one
key, descending `xid` is descending `commit_ts` — rests on
`T.xid > T.snapshot_ts`, which a refreshed snapshot violates
once `T` has an xid.

**The bug.** If the write check uses the refreshed snapshot:
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

### 3.2. Status resolution ignores the commit_ts hint, so GC drops committed versions (§3.4, §4.4, §8.3) [WILL FIX]

§3.1 calls the header `commit_ts` "a hint, see §8.3", and §8.3
says stamped versions "answer visibility without a status
lookup". But neither §3.4 nor §4.4 consults the hint, and the
filter itself begins with `resolve xid (§3.4)`.

**The bug.** §8.3 drops `CF:txn` records with
`commit_ts < gc_watermark` "after a full compaction pass".
1. Key `k`'s newest version `V` has `commit_ts = 100`.
2. A full compaction pass runs while `gc_watermark = 90`;
   `V` hits `commit_ts > gc_watermark → KEEP as-is` and is
   not stamped.
3. Later `gc_watermark = 200`; the sweep drops record 100,
   and the commit cache has evicted it.
4. The next compaction resolves `V.xid`: not active, not
   cached, no record → rule 3.4/4 → dead → `DROP`.

A committed row disappears. Even when `V` was stamped, the
rules as written ignore the stamp and reach the same result.

**Fix.** Add a step 0 to §3.4 and §4.4: a stamped header is
authoritative (committed at that `commit_ts`). Make the sweep
drop only records with `commit_ts <= ` the `gc_watermark` in
force when the completed full pass *started*, so every
surviving version was stamped. Also define the sweep for
`ABORTED` records (no `commit_ts`; droppable any time under
rule 4) and for `PREPARED` writeset chunk keys `xid | chunkno`
when the record flips.

### 3.3. Scans do not record rw-edges for skipped versions (§10.2, §7.2, §8.4) [WILL FIX]

§10.2's `sm_scan_next` takes the "first visible version in the
run" and never calls into the SSI tracker, unlike §10.1.

**The bug.** Under SERIALIZABLE:
1. `T_r` begins (snapshot 100).
2. `T_w` inserts `b` and commits at 110.
3. `T_r` calls `sm_scan([a, z])`. The range SIREAD is
   registered now, after `T_w`'s `ssi.on_write`, so no edge
   was created on the write side.
4. `T_r` steps past `b` (committed after its snapshot) without
   recording an edge.

The edge `T_r ──rw──▶ T_w` is lost, so a phantom-based
dangerous structure goes undetected. This also breaks §8.4's
post-restart rule, which depends on readers seeing every
version they skip that was written by a restored transaction —
including one found by a scan.

**Fix.** In `sm_scan_next`, call `ssi.on_skip(T, v.xid)` for
every skipped version with `concurrent(v, snap)`, as §10.1
does. Also require that `sm_scan` registers the range SIREAD
*before* taking the RocksDB snapshot, for the same reason
§10.1 takes its lock before creating the iterator.

**Note.** §7.2's read rule is stated generally, so a careful
implementer could apply it to scans. But §10.2 is the only
place scan behaviour is specified, and §10.1 was made explicit
for exactly this reason. The ordering of SIREAD registration
against the RocksDB snapshot is stated nowhere, and without it
a write can land in the gap even when the edge rule is
applied.

### 3.4. `seq_hwm` is never advanced on write, and rolled-back versions reappear (§4.4, §5, §6.5) [WILL FIX]

§4.4 makes an own version visible iff `V.seq <= T.seq_hwm`.
§5 step 5 writes `~T.seq++` but never updates `seq_hwm`; only
§6.5 assigns it, on rollback.

**The bug.** Read literally, a transaction cannot see its own
writes until it rolls back. Read as "advance `seq_hwm` on each
write", rolled-back versions come back:
1. `sm_savepoint` (mark 0); write `K1` (`seq 1`).
2. `sm_scan` opens; its RocksDB snapshot contains `K1@1`.
3. `sm_rollback`: `Delete(K1@1)` (invisible to the cursor's
   snapshot) and `seq_hwm = 0`.
4. Write `K2` (`seq 2`), so `seq_hwm = 2`.
5. `sm_scan_next` reaches `K1@1`: own version, `1 <= 2` →
   returns a row that was rolled back.

Before step 4 the cursor hides `K1`; after it, `K1` is back —
an unrelated write changes what the cursor returns. §6.5's
claim that `seq_hwm` "means a rollback is correct even before
the deletes are physically applied" holds only until the next
write. Also, if `T.seq` starts at 0, `seq_hwm = 0` after
rolling back to mark 0 still admits `seq 0`.

**Fix.** Set `T.seq_hwm = seq` in §5 step 5 and start `seq` at 1.
On rollback, rewind `T.seq` to `seq_hwm + 1`, so `seq_hwm` stays
an exact prefix of live writes. Have a cursor capture `seq_hwm`
at `sm_scan`, which matches `API.txt`'s "view is fixed at this
call".

**Note.** The cursor scenario is confirmed. The `seq 0`
sub-point is weaker than stated: §6.5's
`(m > 0) ? T.undo[m-1].seq : 0` already implies that `seq`
starts at 1. The fix should still say so explicitly.

### 3.5. The read-only safe-snapshot optimization is unsound as stated (§7.4) [WILL FIX]

§7.4: "if no concurrent transaction has an out-conflict, the
read-only transaction can never be a pivot and its tracking is
dropped."

**The bug.** A read-only transaction can never be a pivot
anyway — it has no in-edges, because it writes nothing — so
the stated reason says nothing. Its role in a dangerous
structure is `T_in`: `T_ro ──rw──▶ T_pivot ──rw──▶ T_out`,
Fekete's read-only anomaly. If its SIREAD locks are dropped
while a concurrent read-write transaction can still gain an
out-conflict, the in-edge to the pivot is never created and
the anomaly commits. The text does not say *when* the
condition is checked; checked at `sm_begin`, it is unsafe.

**Fix.** Use Ports & Grittner's safe-snapshot rule: a
read-only snapshot is safe only once every concurrent
read-write serializable transaction has finished, and none
committed with an out-conflict to a transaction that
committed before the snapshot. Keep tracking until then.

### 3.6. Read-only commit and prepare write `CF:txn[0]` (§8.2, §8.4, §3.2) [WILL FIX]

§3.2 says only writing transactions get a `CF:txn` entry, but
§8.2 and §8.4 write `CF:txn[T.xid]` unconditionally, and a
read-only transaction has `T.xid == 0`.

**The bug.** Read-only commits allocate a `commit_ts` and
write a meaningless `CF:txn[0]` record. Worse, two read-only
transactions prepared under different gids overwrite each
other's `PREPARED` record at key 0. After restart only one is
restored, and `sm_list_prepared` loses the other gid, though
`API.txt` promises the prepared state is durable.

**Fix.** In §8.2, skip steps 2–5 when `T.xid == 0` (validate
and release only). In §8.4, allocate an xid at prepare if the
transaction has none, so the record has a unique key.

### 3.7. A failed status write is indeterminate, not dead (§3.4, §4.3) [WILL FIX]

§3.4 says a transaction "whose commit status write failed
(§4.3) ... leaves the same trace" as a crashed one, i.e. dead.

**The bug.** A RocksDB `Write` can return an I/O error after
the WAL record reached the disk, for example when the sync
fails. The engine publishes past that `commit_ts` and treats
the versions as dead, but after restart WAL replay finds the
`COMMITTED` record and the transaction is committed. The
caller was told it failed.

**Fix.** Treat a failed status write as an ambiguous outcome:
put the database into a failed state (RocksDB already enters
read-only mode on such errors) so no reader or compaction acts
on "dead" before reopen, and document that `SM_ERR_IO` from
`sm_commit` means the outcome is unknown until restart.

### 3.8. Minor

- **Prepared pivots in §7.3.** [WILL FIX] §7.3 dooms the pivot
  "eagerly when the edge is created". A prepared pivot cannot
  fail; §8.4 says the partner aborts instead. §7.3 should state
  that exception.
- **Catalog `"N"` row (§11).** [WILL FIX] If `sm_mkrel` updates
  the `"N"` next-relid row transactionally, every concurrent
  `sm_mkrel` contends on that row lock and fails with
  `SM_ERR_CONFLICT` even for different names. Relids already come
  from the `CF:meta` lease (§4.2); drop the row or never write it
  per `sm_mkrel`. *Note:* §11 does not say whether `sm_mkrel`
  writes the row. Either it does and serializes all DDL, or it
  doesn't and the row is a stale duplicate of `CF:meta`. Both
  justify removing it.
- **Cursors at prepare (§8.4).** [WILL FIX] `sm_prepare` does not
  close open cursors, and commit of a prepared transaction starts
  at §8.2 step 2, skipping the cursor close. A cursor left open
  across prepare pins `gc_watermark` while the transaction is in
  doubt.
- **ABORTED flip (§3.2).** [WILL FIX] §3.2 says `writeset`,
  `created`, and `prepared_at` are dropped when the record flips
  to COMMITTED; they are also dropped on flip to ABORTED.
- **§5.2 cross-reference.** [WONT FIX] §5.2 refers to "the caveat
  noted in §7.4", which §7.4 does not contain. *Refutation:* §7.4
  is where the non-serializable levels' interaction with the
  write path is qualified. Their writes still feed the SSI
  tracker, but they are never serialization victims. That is the
  caveat on "applies at all three isolation levels". The
  reference is loose, not dangling.

---

## 4. Declined findings

### 4.1. Process-wide catalog invalidation (§11) [WONT FIX]

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

## 5. Architectural strengths

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
