# PostgreSQL on this stack

What it would take to run PostgreSQL over this stack, and
what that target forces on sm2's and sm3's design.

The claims about PostgreSQL below were checked against the
PostgreSQL documentation and source, and against the public
repositories of the projects that have tried this. Section 8
lists what could not be verified, so it does not get repeated
as fact.

**The short answer.** The semantics line up remarkably well —
better than they have any right to. The mechanics do not, in
four specific places, and one of them rules out the supported
extension path entirely. This is a multi-year program in any
direction, and every comparable attempt in the last decade has
either forked PostgreSQL permanently or died.

---

## 1. The good news: the semantics already line up

sm1 was built watching PostgreSQL, and it shows. This is most
of the hard semantic work already done:

| sm1 | PostgreSQL |
| --- | --- |
| RC / RR / SERIALIZABLE, RR being snapshot isolation | identical; PG's RR is also SI |
| SSI with SIREAD predicate locks and pivot detection | the same Cahill algorithm PG uses (`sm1/SIREAD.md` tracks PG's refinement) |
| `sm1::stmt_begin` pinning the RC snapshot for a statement | PG takes a snapshot per statement at RC |
| nested savepoints as a depth stack | subtransactions |
| `sm1::prepare(gid)`, `sm1::max_gid` = 200 | 2PC, `GIDSIZE` |
| `sm1::max_dbname` = 63 | `NAMEDATALEN - 1` |
| deadlock found on a timeout, then a waits-for search, youngest victim | `deadlock_timeout`, same shape |
| home directory layout | modelled on PG's data directory |

The impedance is not semantic. It is mechanical.

---

## 2. The blocker: the process model

PostgreSQL forks a process per connection. sm1 keeps its
transaction table, lock manager and SSI graph in **process**
memory, and RocksDB permits one writer process.

Two backends are two processes. They cannot see each other's
in-process transaction table, and they cannot each open the
same RocksDB. So:

> **sm1 cannot be linked into PostgreSQL backends as a
> library.** This is structural, not a tuning problem.

The options, ranked:

1. **A separate server process, with IPC.** sm1, sm2 and sm3
   run as one multi-threaded server; each backend holds a
   session to it. This is exactly YugabyteDB's shape — a PG
   backend per session talking to a multi-threaded C++ storage
   tier through an RPC layer, with a shared-memory fast path
   added later. It preserves sm1's threading model untouched.
2. **Move sm1's shared state into PG shared memory.** Rewrites
   the transaction table, lock manager, SSI structures and
   deadlock detector as fixed-size shared-memory structures —
   and still founders on RocksDB's single-writer-process rule.
3. **Wait for a multi-threaded PostgreSQL.** Proposed in 2023
   with in-principle agreement; not in any released version as
   far as could be determined. Not a plannable option.

Option 1 is the only viable one, and it has a consequence that
reaches all the way back into the stack's topmost API: **every
call across that boundary is an RPC.** A per-row interface will
not survive it. This is why the API above was made batch-shaped
from the start — a decision that now belongs to sm3, the level
with the filters (`sm3/SM3_OBJECTIVE.md`) — and it is why the
batched read left unbuilt in `sm1/SM1_DESIGN.md` §10.1 stops
being an unproven optimization and becomes a requirement.

**Decision: the fork is threaded.** The project does not put
sm1 behind IPC (option 1). It changes PostgreSQL itself from
a process per connection to a thread per connection — option
3, done in the fork instead of waited for. sm1, sm2 and sm3
are then linked into the one server process, every backend thread
shares sm1's transaction table, lock manager and SSI graph as
sm1 already assumes, and RocksDB's one-writer-process rule is
met by construction.

What this changes above:

- The boundary between the executor and sm3 is a function
  call, not an RPC. A per-row interface is no longer
  unsalvageable, so the batch-shaped API is no longer forced
  by this section. Whether to keep it for other reasons —
  filter pushdown, fewer calls per scan — is open, and is
  sm3's to settle (`sm3/SM3_DESIGN.md` §5); sm2 is
  tuple-at-a-time either way.
- YugabyteDB stays the precedent for §4's replacement of the
  storage and transaction layers, but no longer for the
  process layout.
- The fork will be changed substantially: PostgreSQL's
  backend-local state, such as global variables, memory
  contexts, error handling and signals, has to become
  thread-safe. §4's effort figures do not include that, and
  it makes tracking upstream releases harder than the version
  lag §4 already counts as a liability.
- **The fork is compiled as C++.** `include/sm.h` passes
  `std::string` and `std::vector` across the boundary for
  owned data, and may pass `std::string_view` and
  `std::shared_ptr` (`DESIGN.md` §5); the fork calls it as it
  is, and there is no plain C header to keep beside it.
- **A row handed out is good until the next call.** A row or
  an index entry the stack returns points into its own
  memory and stays valid until the next `sm_` call on that
  session, so the executor can use it in place and copies
  only what it must keep longer.
- **Every wait can be interrupted.** A backend thread waiting
  inside the stack never reaches PostgreSQL's interrupt
  check, so the stack waits through `sm_sys.h`
  (`sm0/INTERRUPT.md`): an interrupt to the thread, a bit
  in its wait mask, fails the wait with `SM_ERR_INTERRUPTED`
  and leaves the transaction to the caller. The bit stays
  pending until the fork takes it. Query cancel,
  `statement_timeout` and `lock_timeout` are bits and `sm_sys.h`
  timers (`sm1/SM1_DESIGN.md` §5.1); the wait for an index
  build (`sm_cat_index_wait`) ends the same way.
- **The fork has no `longjmp`.** PostgreSQL raises an error by
  jumping out of wherever it is. That goes with the rest of
  its error handling, so the stack does not have to defend
  against a jump through one of its calls, or out of a
  callback it makes, such as the index evaluator's. Done:
  digres raises an error as a C++ exception
  (`../dg/WORKNOTE.md` step 7). An exception out of a callback
  the stack makes is therefore the thing to settle next:
  `sm.h` says no exception crosses it.

---

## 3. Why the table access method path does not work here

PostgreSQL has supported pluggable table storage since version
12, via `TableAmRoutine` — 41 callbacks then, 45 by PG 14, 44
in PG 18, with a rename in 16 and a signature change in 18.
The interface is not stable across major versions; anything
built on it is re-ported annually.

Three things rule it out for an engine like sm1.

**The interface is block-shaped, and says so.** Its own
documentation: *"if the AM wants to support modifications
and/or indexes, it is necessary for each tuple to have a tuple
identifier (TID) consisting of a block number and an item
number"*, and *"if bitmap scan support is desired … the block
number needs to provide locality"*. The header is blunter:
*"This obviously is primarily suited for block-based AMs. It's
not clear what a good interface for non block based AMs would
be, so there isn't one yet."*

A TID is six bytes, and the usable space is far smaller than
that suggests: the offset is effectively 11 bits, giving
roughly 2^43 rows per relation, and block locality is assumed
by bitmap scans, BRIN, `CLUSTER`, `ANALYZE`'s block sampling,
`relpages` in the cost model, and the user-visible `ctid`. sm1
has no TID. sm2 keys every row by its primary key, or in a
table without one by `_id`, a 64-bit number from an sm1
sequence, crash-safe and never reused (`ROWKEY.md`,
`sm2/SM2_DESIGN.md` §5); neither fits a 6-byte TID. A query
names a keyless table's row by `_id`, a system column of its
own, an `int8`; `ctid` is no relation's column, nor are `xmin`,
`xmax`, `cmin` and `cmax`, as sm keeps no transaction ids in a
row (decided 2026-10-16, `../dg/WORKNOTE.md` step 200). Inside,
the fork carries the key's columns where the executor carried
a TID (`ROWKEY.md` §3), rather than fitting a key into the
table AM's shape.

**PostgreSQL's MVCC would have to be the authority, which
deletes sm1's.** A table AM is handed PG's transaction ids and
snapshots and must answer in PG's terms:
`tuple_satisfies_snapshot` takes a PG `Snapshot`;
`index_delete_tuples` *returns* a `TransactionId` used to
generate recovery conflicts on standbys; the executor reads
`TM_FailureData.xmax` and calls
`TransactionIdIsCurrentTransactionId` and `XactLockTableWait`
on it, so the id returned must be a real PG xid that PG's lock
manager can block on; SSI is defined over relations, pages and
TIDs. There is no 2PC callback at
all.

Under that regime sm1's xids, `visible_ts` snapshots,
commit-status table, lock manager, SSI, savepoints, 2PC and
version GC are all dead weight — and worse than dead weight in
one case: **two deadlock detectors that cannot see each other's
wait-for edges produce global deadlocks that neither detects.**
That is a hang with no design around it.

**The one project that tried this had to patch the core
anyway.** OrioleDB is a table AM with its own MVCC. To make it
work it maintains a full PostgreSQL fork, and the patches are
not small: a new `RowRefType` enum added to core's
`primnodes.h`; an eighth system column (`RowIdAttributeNumber`)
for a variable-length row identity; a single "improvements to
table AM" commit touching **42 core files** across the planner,
the executor and the parser; `CSNSnapshotData` fields added
directly into core's `SnapshotData`, which has no room for an
extension's state; and a new transaction event,
`XACT_EVENT_PRE_PROC_ARRAY`, whose commit message describes
exactly the atomicity hole between two commit authorities —
an extension's commit callback runs *after* the transaction is
already visible, leaving a window where a snapshot can be taken
above a commit the extension still reports as in progress.

OrioleDB has been at this for about five years, with some of
the most senior PostgreSQL hackers alive, and is still beta and
still forking. zheap — which kept PG's xids, snapshots, clog,
WAL and buffer manager, and only changed the tuple format —
also concluded it needed core patches, and never shipped. The
one table AM that is production-shipping, Citus columnar,
bought that by **giving up UPDATE, DELETE, row locks and
serializable isolation** — precisely the parts sm1 is about.

---

## 4. The fork, and what it costs

That leaves the YugabyteDB shape: keep PostgreSQL's parser,
analyzer, rewriter, planner and executor — the valuable part,
and most of the user-visible surface — and replace the storage
and transaction layers underneath.

YugabyteDB is the right precedent because its architecture is
the same one: a RocksDB-derived store, index-organized tables,
its own MVCC, an out-of-process multi-threaded engine, one
backend per session. They started before the table AM existed,
they did not adopt it afterwards, and they replaced whole
executor nodes to push work down — sequential scan, bitmap
scan, batched nested loop, expression evaluation.

What must be replaced along with the storage: the transaction
manager, snapshot manager, clog and subtransactions, row
locking, SSI, WAL and recovery for user data, and vacuum.
What disappears entirely is worth counting as a win: **vacuum,
freezing, xid wraparound and autovacuum all go**, replaced by
sm1's GC.

What still has to be solved either way:

- **WAL, replication, PITR, logical decoding.** All three read
  PostgreSQL's WAL. With the data in RocksDB's WAL, physical
  replication, `pg_basebackup` and PITR stop working. How the
  stack replicates is open. RocksDB has no replication of its
  own, only pieces to build one from: `Checkpoint` for a base
  copy, `GetUpdatesSince` to tail its WAL. Logical decoding has
  a design of its own, not on PG's WAL (`LOGICALDECODE.md`,
  proposed). Replication at the storage tier is a subsystem in
  its own right; it is what YugabyteDB built, with Raft.
- **Catalogs move into sm3.** PostgreSQL hardwires catalogs
  and sequences to heap — the relation cache asserts it — so
  the fork rewrites that seam rather than living with a
  hybrid, where DDL atomicity would sit exactly on the seam.
  The catalog is held in memory, loaded at startup, with
  changes made in memory and written through to sm2 for
  persistence only. sm1's status record is then the one
  commit point: recovery is RocksDB's alone, DDL shares its
  transaction with the data, and sm1's sequences are used.
  Logical decoding can be fed from sm1 instead of a second
  copy in PostgreSQL's WAL, but sm1 has no commit-ordered
  change feed yet. The threaded fork (§2) makes one shared
  in-memory catalog coherent without cross-process
  invalidation. The cost is the code that reads the
  catalogs — the relation cache, syscache and system-table
  scans — and the open questions in `sm3/SM3_DESIGN.md` §2.
- **Statistics.** The planner is being kept, so it needs
  `reltuples`, `relpages`, histograms and correlations, and
  `ANALYZE` needs a sampling path.

**Effort.** Calibrated against those precedents, not measured:
a single-node prototype with basic DML, primary-key access and
one index type is on the order of **1.5–3 engineer-years**;
something an operator would run is **5–15**. YugabyteDB took
roughly six years to move its fork from PG 11 to PG 15, and
that version lag is a permanent, visible liability of the
approach.

---

## 5. The one conflict with a decision already made

`sm1/SM1_OBJECTIVE.md` settles that backward scans are never
supported. PostgreSQL does not let an engine opt out:

- `ExecSupportsBackwardScan` returns true **unconditionally**
  for a sequential scan, without consulting the table AM. That
  is what scrollable cursors rest on — `DECLARE … SCROLL`,
  `FETCH BACKWARD`, PL/pgSQL cursors, JDBC scrollable result
  sets.
- For indexes it is worse in a subtler way: the planner builds
  backward index paths guarded only by `amcanorder`, and
  consults `amcanbackward` nowhere in path generation. An
  ordered index that cannot scan backwards will be handed
  `BackwardScanDirection` at execution time. Declaring
  `amcanorder = false` to escape costs index-provided ordering,
  merge joins and `ORDER BY … LIMIT` via index — not a trade
  worth making.

The mirror-relation workaround — a second relation with
inverted key bytes — then applies to every table and every
ordered index, roughly doubling storage and write
amplification.

**Why the decision was made, and whether it can be revisited.**
The reason is in `sm1/SM1_DESIGN.md` §1, and it is not
"RocksDB cannot iterate backwards" — it can, with `Prev()`.
When it was made, sm1 stored every key's versions
newest-first (`key | ~wts | ~seq`), so a backward scan would
meet the *oldest* version of each key first and walk to the
start of that key's run before it could pick the visible one;
it would need `SeekForPrev` to cooperate with the prefix-seek
setup; and the SSI range bookkeeping, which narrows a scan's
predicate to the last key examined, inverts.

Since `VCHAIN.md`, most of that cost is gone for table rows. A
row's newest version is one CURR key, `01 ‖ rowkey`, and its
older versions are a chain in OLD, so a backward scan of rows
meets one key a row and walks the chain as a forward scan
does; and a scan of CURR keys runs in total order already,
where `SeekForPrev` works. Rows are keyed by the primary key,
so this covers backward primary-key and sequential scans. What
remains:

- **Secondary index entries, TOAST and the catalog** keep
  versioned keys (`VCHAIN.md` §9), and a backward scan of them
  still meets the oldest version first. `ORDER BY x DESC
  LIMIT n` on a secondary index is the case this section gives
  up.
- **The SSI bookkeeping** inverts whatever the key layout.

If secondary indexes move to B+trees (`BTREE.md`), their leaf
entries are unversioned, the row deciding visibility, and a
backward scan is a walk of leaves by a left link, as
PostgreSQL's nbtree does. The SSI inversion would then be the
cost left.

That is a real cost, but it is an implementation cost in sm1,
not a property of the storage below it. **If PostgreSQL is
genuinely the target, this decision has to be re-opened**, and
it is much cheaper to re-open now than after sm3 has built
mirror relations into its index design.

**Decision: the ban stands; the planner is made not to ask.**
sm1 stays forward-only, and there are no mirror relations. The
fork's planner is changed so that an index scan in the
backward direction costs infinity and is never chosen. A
forward path always exists, so this loses plans, not
correctness. What it means:

- `ORDER BY x DESC LIMIT n` on an ordered index becomes a
  top-N sort over a forward scan, not an early-terminating
  index scan. The same goes for a merge join that wants the
  descending order.
- PostgreSQL answers `MAX()` with a backward index scan, so it
  is expected to lose that plan too; `MIN()` keeps its
  forward one.
- The infinite cost is on the direction of the scan, not on
  how the index is declared. An index declared with DESC
  columns is still scanned forward, over inverted key bytes
  (`sm2/SM2_DESIGN.md` §3).
- Index paths are not the only place a backward scan is
  assumed. `ExecSupportsBackwardScan` returns true for a
  sequential scan unconditionally, so it is changed to return
  false always — every table is an sm3 table — which makes the
  planner put a Materialize node under a scrollable cursor.

---

## 5a. The second conflict: READ COMMITTED writes

`sm1` first settled that a write at READ COMMITTED fails with
`SM_ERR_CONFLICT` when the row was committed after the
transaction began. PostgreSQL's default level does not fail.
The second of two concurrent
`UPDATE t SET x = x + 1 WHERE id = 1` waits for the first,
re-reads the newest version of the row, re-checks the `WHERE`
clause and applies its change. Workloads written for
PostgreSQL rely on that — a shared counter, a hot account row
— so a fork that fails them is not PostgreSQL at its default
level.

Decided and built: at READ COMMITTED the write waits for the
row lock and then proceeds instead of failing. The change is to
sm1: a refused write, `SM_ERR_UPDATED`, is followed by
`sm1::read_latest` and then the write, and versions are keyed by
a write timestamp so their order stays commit order. The design
is `sm1/SM1_DESIGN.md` §5.4 and §6.2, and its tests are scenarios
`f01`–`f10` and `e06`.

---

## 6. What this forces on sm2 and sm3, now

These are the decisions constrained by the PG target. They are
reflected in each level's own `SMn_OBJECTIVE.md` and
`SMn_DESIGN.md`; the level column says which.

| Forced | Level | Why |
| --- | --- | --- |
| **Tuple format cheap to turn into a PG tuple** | sm2 | the executor works in tuple slots; a bespoke encoding pays a conversion per row, in both directions; the threaded fork removes the IPC boundary, not the conversion |
| **Row identity is the row's key, or `_id` without a primary key** | sm2 | tables may have no primary key, so a table without one is keyed by `_id`, a 64-bit number from an sm1 sequence, which a query reads as the system column `_id`; the executor carries the key's columns where it carried a TID, and there is no `ctid` (`ROWKEY.md`) |
| **Key encoder takes a collation** | sm2 | PG's text order is `strcoll`/ICU, not memcmp; the encoder needs `(type, collation, direction, nulls-order)` per column or half of SQL's `ORDER BY` is unrepresentable |
| **Collated keys store the sort key *and* the original** | sm2 | the collation transform is lossy and irreversible; this is what YugabyteDB does, and it costs storing the datum twice |
| **Batch-shaped API** | sm3 | was forced by §2's out-of-process engine; with a threaded fork it is no longer forced, and is open |
| **Collation version recorded with the schema** | sm3 | sort keys are not stable across collation library versions; PostgreSQL has had index corruption from exactly this, which is what `pg_collation.collversion` exists for |
| **Statistics are not optional** | sm3 | the planner is being kept |
| **Relations multiplexed onto few column families** | sm3 | sm1 targets hundreds to low thousands of relations; a schema with partitions and indexes passes that easily, and this limit will bind before most of the others |

And one that is not forced but is strongly advised: **sm2 and
sm3 must build and be testable without a PostgreSQL source
tree.** Everything above shapes their decisions; none of it
should become a dependency.

---

## 7. Recommended sequencing

1. **Build sm2, then sm3, standalone**, to
   `sm2/SM2_DESIGN.md` §8 and `sm3/SM3_DESIGN.md` §8.
2. **Backward scans are settled** (§5): the ban stands, and
   the planner is changed not to ask for one.
3. **Spike the fork early, and throw it away.** A prototype
   that runs `SELECT` and `INSERT` on one table through sm3,
   built to find which of §2's and §4's walls bites first —
   not to be kept. Its first target is narrower still: insert
   into, and scan, a table whose columns are all integers
   (§9.5). The formats of `sm2/SM2_DESIGN.md` §8.2,
   steps 2 and 3, are permanent once data exists; the spike is
   how to learn what they must carry *before* that happens.
4. Only then decide whether the full program is worth its
   multi-year cost.

Worth asking once, before step 4: if the goal is *SQL on this
stack* rather than *PostgreSQL specifically*, a purpose-built
SQL layer is a fraction of the cost. If the goal is
PostgreSQL's ecosystem, extensions and wire protocol — which
is a perfectly good reason — then it is a fork, and it should
be budgeted as one.

---

## 8. What is not verified

Stated so none of it is repeated as fact:

- **Multi-threaded PostgreSQL.** The 2023 proposal and its
  in-principle support were found; no merged implementation
  through PG 18 was found. Believed not landed, but no
  authoritative statement to that effect was located.
- **The exact size of OrioleDB's core patch set.** The named
  patches were read; the complete delta against upstream was
  not enumerated.
- ~~**RocksDB's approximate-statistics accuracy**~~ has since
  been measured: exact once flushed and compacted, as much as
  7x high for rows still unflushed (`STATISTICS.md` §6).
- **The cost of converting PostgreSQL to threads** (§2's
  decision). Not estimated, and not in §4's figures, which
  assume the process model stays.
- **The size of the catalog rewrite** (§4's decision). Not
  estimated.
- **What the planner change costs in practice** (§5's
  decision). The plans lost, such as `MAX()` through an
  index, are expected from how the planner works, but were
  not confirmed against its source or measured.
- **The READ COMMITTED write protocol under a race detector**
  (§5a). It is built and tested against GC and compaction, and
  `e06` loses no increment, but a `-fsanitize=thread` run was
  inconclusive (`sm1/SM1_DESIGN.md` §13).
- **The effort figures in §4** are calibrated judgment against
  the precedents, not measurements. Treat the ordering as
  sound and the absolute numbers as loose.

---

## 9. The stack's API against the executor's calls

Before the spike of §7, `include/sm.h` was read against what
PostgreSQL 18.6's executor calls to reach storage: the 44
callbacks of `TableAmRoutine` (`access/tableam.h`), the index
calls of `access/genam.h`, and the places `nodeSeqscan.c`,
`nodeIndexscan.c`, `nodeModifyTable.c` and `nodeLockRows.c`
make them. The question for each path: is there a call here
of the right shape, and what does the fork have to bridge.

This is a reading, not a run. The spike is what tests it.

### 9.1 Path by path

| Path | The executor calls | The stack's calls | Fit |
| --- | --- | --- | --- |
| Open a relation | `table_open` with a lock mode | `sm_tbl_open(session, oid, IS or IX)` | fits; the lock is the table-oid lock |
| Sequential scan | `table_beginscan`, `table_scan_getnextslot`, `table_rescan`, `table_endscan` | `sm_tbl_scan`, `sm_rel_scannxt`, `sm_rel_scanend`, then `sm_tbl_deform` into the slot | fits, forward only; a rescan is a new cursor |
| Index scan | `index_beginscan`, `index_rescan` with scan keys, `index_getnext_slot`: a TID from the index, then `table_index_fetch_tuple` | `sm_tbl_idx_scan` with a low and a high bound, `sm_idx_scannxt` for a rowno, then `sm_tbl_read` | fits for ranges on a key prefix; see gaps 2, 5, 6 |
| Insert | `table_tuple_insert`, then `ExecInsertIndexTuples` calling `index_insert` on each index | `sm_tbl_insert`, which writes the heap row and every index | fits, and simpler: the fork skips its own index maintenance |
| Update | `table_tuple_update`, returning a `TM_Result` and saying which indexes to update | `sm_tbl_update` | fits; results map as in §9.2 |
| Delete | `table_tuple_delete` | `sm_tbl_delete` | fits |
| Re-check after a concurrent update, READ COMMITTED | `table_tuple_lock` to fetch the newest version, then `EvalPlanQual` | `SM_ERR_UPDATED`, then `sm_tbl_read_latest`, then the write again | fits; built for this (§5a) |
| Row lock | `table_tuple_lock` in one of four modes, with a wait policy | `sm_tbl_lock` in two modes, always waiting | partly; gaps 3, 4 |
| Statement boundary | `CommandCounterIncrement`, a snapshot a statement | `sm_stmt_begin` | fits |
| Transaction | start, commit, abort; subtransactions; two-phase | `sm_begin`, `sm_commit`, `sm_abort`; `sm_savepoint`, `sm_rollback`, `sm_forget`; `sm_prepare` and the rest | fits |
| Cancel | `CHECK_FOR_INTERRUPTS` | `sm_interrupt` to the waiting thread; sm's waits end with `SM_ERR_INTERRUPTED` | fits |
| Create and drop | `heap_create_with_catalog`, `index_create`, and their drops | `sm_cat_create_table`, `sm_cat_create_index`, `sm_cat_drop_table`, `sm_cat_drop_index`, `sm_tbl_truncate` | fits for these; gaps 7, 8 |
| Sequence | `nextval`, `setval`, `currval` | `sm_cat_create_sequence`, `sm_cat_nextval`, `sm_cat_drop_sequence`: a sequence in a namespace, over an sm1 counter | `nextval`, and `currval` kept by the host; no `setval`, gap 9 |

Not needed by the stack, and so left out of the table: vacuum,
freezing, the visibility map, HOT, and `index_build_range_scan`
— the stack maintains and builds its own indexes.

### 9.2 Write results

| `TM_Result` | From the stack |
| --- | --- |
| `TM_Ok` | 0 |
| `TM_SelfModified` | `SM_ERR_SELFMODIFIED` |
| `TM_Updated` | `SM_ERR_UPDATED` at READ COMMITTED; `SM_ERR_CONFLICT`, which has already aborted, above it |
| `TM_Deleted` | `SM_ERR_UPDATED`, and `sm_tbl_read_latest` then finds no row |
| `TM_BeingModified`, `TM_WouldBlock` | nothing yet: gap 4 |
| `TM_Invisible` | does not arise: a rowno comes from a scan at the same snapshot |

One difference in kind: a retryable error from the stack has
already aborted the transaction, where PostgreSQL returns a
result and lets the executor raise the error. The fork raises
the same error either way, and must not abort twice.

### 9.3 Gaps

**In the way of the spike itself:**

1. ~~**A scanned row is promised for too short a time.**~~
   **Closed.** `sm.h` said a row is valid until the next call
   on the *session*. The executor keeps a scan's current row
   while it makes other calls on the same session: the inner
   side of a join, an index's heap fetch, the insert of
   `INSERT … SELECT`. By that promise every such row had to
   be copied. The stack in fact did better, and the header
   now promises it: a row from `sm_rel_scannxt` or
   `sm_idx_scannxt` lasts until the next call on that
   *cursor*, which is exactly what a scan slot needs, and a
   test holds it to that. A row from `sm_tbl_read` is the
   session's, and the fork copies that one or uses it at
   once.
2. ~~**The identity of a row is 64 bits and a TID is 48.**~~
   **Closed.** A row is named by its key, not a TID
   (`ROWKEY.md`).

**Not in the way of `SELECT` and `INSERT` on one table, but
the next things the fork would hit:**

3. **Two lock modes, where PostgreSQL has four.** `FOR KEY
   SHARE` and `FOR NO KEY UPDATE` have nothing to map to, so
   an update of a parent row that leaves its key alone waits
   for a child's foreign-key check (`NEXT.md`).
4. ~~**No wait policy.**~~ **Closed.** `sm_tbl_trylock` and
   `sm_tbl_tryopen` serve `NOWAIT` and `SKIP LOCKED`.
5. **An index entry gives back its key columns only where its
   key keeps them.** `sm_idx_scan_key` decodes them, but a
   key cut to fit, or a text key under a non-deterministic
   collation, keeps no datum: an index-only scan reads such an
   entry's row.
6. **No mark and restore on an index cursor.** A merge join
   backs its inner side up to a mark. A cursor only goes
   forward, so the fork puts a Materialize node there, as it
   does for a scrollable cursor (§5).
7. **A collation cannot be given to an index through
   `sm.h`.** sm3's index key takes a collation oid;
   `sm_index_key_spec_t` has no field for it, and there is no
   `sm_cat_create_collation`. Text keys made through the C
   API sort by bytes.
8. ~~**`ALTER TABLE` is not in `sm.h`.**~~ **Closed.**
   `sm_cat_add_column`, `sm_cat_drop_column`,
   `sm_cat_rename_column` and `sm_cat_rename_table` call
   sm3's; digres's ADD COLUMN, DROP COLUMN and RENAME use
   them.
9. **A sequence only goes forward.** No `setval`, no
   `ALTER SEQUENCE`, and none of a sequence's options: the
   stack's sequences are a name and a counter.
10. ~~**A row is at most 64 KB.**~~ **Closed.** TOAST is
    built: a row's long values are stored out of line
    (`include/sm.h`).
11. ~~**Nothing for the planner.**~~ **Closed.** Statistics
    are built (`STATISTICS.md`): `sm_tbl_size`,
    `sm_tbl_stats`.

**The fork's own, with nothing to change here:**

- **A table's oid.** The stack hands out a 64-bit oid of its
  own; PostgreSQL's is 32 bits and comes from its catalog.
  The spike keeps a map from one to the other. It goes away
  when the catalogs move into sm3 (§4).
- **Types.** A table of PostgreSQL type oids to sm2's type
  codes. `numeric` is the one that does not carry over:
  sm2's decimal is its own format, and PostgreSQL's numeric
  functions do not read it. The spike avoids it.
- **Scans the stack has no shape for**: bitmap scans, which
  want block locality; `TABLESAMPLE`; parallel scans. The
  fork's planner does not choose them.
- **`INSERT … ON CONFLICT`.** PostgreSQL inserts
  speculatively and then confirms. The stack reports a taken
  key as `SM_ERR_DUPKEY` after the heap row is written, so
  the fork needs a savepoint a row, or a call that says which
  row holds the key. Open.

### 9.4 What this says about readiness

For the spike — `SELECT` and `INSERT` on one table, one
backend — the stack needed one thing changed, gap 1, and that
was a promise to write down, not code. Everything else in the
way is the fork's: the slot, the oid map, the type map.

The open gaps among 3 to 11 are the order of work after the
spike, to be re-ranked by what it finds.

### 9.5 The first target: insert and scan, integers only

The spike starts narrower than §7 says: one table whose
columns are all `int2`, `int4` or `int8`, an `INSERT` into it
and a sequential scan of it, from one backend. Integers are
passed by value, so the target leaves out everything that is
about bytes owned by someone: varlenas, collations, TOAST,
`numeric`, and how long a pointer into a row is good for.
Indexes, updates and deletes come after it.

What the fork calls, start to finish:

| Step | Call |
| --- | --- |
| once a server | `sm_init`, `sm_attach`, `sm_mkdb` |
| once a backend | `sm_opendb` |
| `CREATE TABLE` | `sm_begin`, `sm_cat_create_table` with type codes 2, 3 or 4 (`sm2/TYPE.md` §3), `sm_commit`; the fork keeps the oid it gets back |
| `INSERT` | `sm_begin`, `sm_stmt_begin`, `sm_tbl_open` in IX, `sm_tbl_insert` with the slot's values and null flags, `sm_tbl_close`, `sm_commit` |
| `SELECT` | `sm_begin`, `sm_stmt_begin`, `sm_tbl_open` in IS, `sm_tbl_scan` over every rowno, then for each row `sm_rel_scannxt` and `sm_tbl_deform` into the slot, `sm_rel_scanend`, `sm_tbl_close`, `sm_commit` |

Every call in it exists. One thing the fork has to mind even
here, and one it no longer does:

- **A row's identity.** The slot's `tts_tid` is 48 bits and a
  `rowno` is 64 (gap 2). For this target the fork packs a
  small `rowno` into it.
- **An integer's unused bits are PostgreSQL's.** Both sides
  hold a by-value integer in an 8-byte datum. PostgreSQL's
  `Int32GetDatum` sign-extends, so -1 is all ones; the stack
  used to zero-extend, so that -1 as an `int4` had its upper
  half clear, and code that compares datums whole, as
  `datumIsEqual` does, would have seen two values for one
  negative number. sm2 now sign-extends too, where it builds
  a datum and where it deforms a row, a `float4` by its bits
  as `Float4GetDatum` does. A datum from the stack goes into
  a slot as it is.

Nothing in this target waits on the stack.

---

## 10. digres: how the fork is to be made

The fork is named **digres**, `dg` for short. These are the
decisions taken so far about how it is made; they override
§4 and §7 where they differ.

**Where it lives.** In this repository. Everything that is
here today moves under `sm/`, and the fork goes in `dg/`
beside it, so that a change to `sm.h` and to its caller is one
commit.

**It is a copy, not a branch of PostgreSQL's history.** The
core of PostgreSQL 18.6 (commit `724edf9`) is copied in, with
its `COPYRIGHT` notice. With threads, no `longjmp`, C++ and
what is cut below, an upstream change would not merge anyway;
the cost, accepted, is that every upstream fix wanted later
is ported by hand.

**PostgreSQL's catalog is cut completely**: its catalog
tables, their bootstrap (BKI), and the three caches over them
— the relation cache, the syscache and the catcache — with
the invalidation machinery that keeps those caches honest.
What replaces it:

- **A thin lookup layer.** The rest of the backend does not
  call `sm_*` at each of its three thousand catalog lookups.
  It goes on calling a narrow interface, in the manner of
  `lsyscache.h`'s helpers and a slim `Relation`, and that
  interface is implemented anew. Measured in 18.6: 1,141 raw
  `SearchSysCache*` calls in 152 files, about 1,500 calls to
  `lsyscache` helpers, 804 relation opens, 242 direct catalog
  scans and 350 catalog writes, over 65 catalogs.
- **Built-ins are compiled in, and immutable.** Types,
  functions, operators, casts, operator classes and
  aggregates are read-only tables generated from
  PostgreSQL's `src/include/catalog/*.dat`. There is no
  `CREATE TYPE`, `CREATE FUNCTION` or `CREATE OPERATOR`, so
  nothing about them is stored, locked or invalidated.
- **What a user creates is in sm3**: namespaces, tables,
  columns and their defaults, indexes, collations,
  sequences, foreign keys and views (`sm3/CATALOG_ONDISK.md` §2);
  what is open is in `sm3/CATALOG_INMEM.md` §4.

**An oid is 64 bits**, in digres as in sm3. For now digres keeps
PostgreSQL's 32-bit `Oid`, so sm3 hands out none past 2^32 - 1
(`sm3/CATALOG_ONDISK.md` §1); widening it is to come. The numbers
are PostgreSQL's where it has them: sm3 hands out oids from
16384, PostgreSQL's `FirstNormalObjectId`, and its two built-in
namespaces are `pg_catalog` at 11 and `public` at
2200. Below 16384 are the built-ins, compiled in with the
oids PostgreSQL gave them.

**Linux and UTF-8 only.** No other operating system's port,
and no other server encoding or its conversion procedures.

**Left out of the copy from the start**: PL/pgSQL and every
other procedural language, and BKI. More is to be cut; the
list is being drawn up.

**Order of work.** Cutting the catalog puts the spike of §7
behind its replacement, so the first things to run are:

1. `dg/` compiles.
2. `SELECT 1 + 1` runs, from the compiled-in tables alone:
   the parser, planner and executor with no storage.
3. `CREATE TABLE`, `INSERT` and a scan of a table of integer
   columns, through sm3 (§9.5).
