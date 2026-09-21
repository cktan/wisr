# PostgreSQL on this stack

What it would take to run PostgreSQL over sm1 and sm2, and
what that target forces on sm2's design.

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
| `sm_stmt_begin` pinning the RC snapshot for a statement | PG takes a snapshot per statement at RC |
| nested savepoints as a depth stack | subtransactions |
| `sm_prepare(gid)`, `SM_MAX_GID` = 200 | 2PC, `GIDSIZE` |
| `SM_MAX_DBNAME` = 63 | `NAMEDATALEN - 1` |
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

1. **A separate server process, with IPC.** sm1 and sm2 run as
   one multi-threaded server; each backend holds a session to
   it. This is exactly YugabyteDB's shape — a PG backend per
   session talking to a multi-threaded C++ storage tier through
   an RPC layer, with a shared-memory fast path added later.
   It preserves sm1's threading model untouched.
2. **Move sm1's shared state into PG shared memory.** Rewrites
   the transaction table, lock manager, SSI structures and
   deadlock detector as fixed-size shared-memory structures —
   and still founders on RocksDB's single-writer-process rule.
3. **Wait for a multi-threaded PostgreSQL.** Proposed in 2023
   with in-principle agreement; not in any released version as
   far as could be determined. Not a plannable option.

Option 1 is the only viable one, and it has a consequence that
reaches all the way back into sm2's API: **every call across
that boundary is an RPC.** A per-row interface will not
survive it. This is why `sm2/OBJECTIVE.md` makes the API
batch-shaped from the start, and it is why the batched read
left unbuilt in `sm1/SECINDEX.md` gap 6 stops being an
unproven optimization and becomes a requirement.

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
rows are identified by a variable-length primary key. There is
no TID, and inventing a dense 48-bit surrogate means a
crash-safe never-reusing allocator plus a surrogate→key map —
an extra index hop on every fetch.

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
  PostgreSQL's WAL. With the data in RocksDB's WAL, crash
  recovery becomes two independent replays with no shared LSN
  — catalog and data can diverge at a crash — and physical
  replication, `pg_basebackup` and PITR stop working. Logical
  decoding is possible only through a custom WAL resource
  manager, which means writing full row images into PG's WAL as
  well, paying for durability twice. Rebuilding replication at
  the storage tier is a subsystem in its own right; it is what
  YugabyteDB did, with Raft.
- **Catalogs stay on heap.** PostgreSQL hardwires it: the
  relation cache asserts that catalog relations use heap, and
  sequences are hardwired to heap too — so sm1's sequences
  would go unused. A PG-on-sm2 system is a hybrid, with
  catalogs on heap under PG's MVCC and user data in sm2, and
  the DDL-atomicity problem lives exactly on that seam.
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

`sm1/OBJECTIVE.md` settles that backward scans are never
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
The reason is in `sm1/SECINDEX.md` gap 4, and it is not
"RocksDB cannot iterate backwards" — it can, with `Prev()`.
It is that sm1 stores a key's versions newest-first
(`key | ~xid | ~seq`), so a backward scan meets the *oldest*
version of each key first and must walk to the start of that
key's run before it can pick the visible one; it needs
`SeekForPrev` to cooperate with the prefix-seek setup; and the
SSI range bookkeeping, which narrows a scan's predicate to the
last key examined, inverts.

That is a real cost, but it is an implementation cost in sm1,
not a property of the storage below it. **If PostgreSQL is
genuinely the target, this decision has to be re-opened**, and
it is much cheaper to re-open now than after sm2 has built
mirror relations into its index design.

---

## 6. What this forces on sm2, now

These are the decisions constrained by the PG target. They are
reflected in `sm2/OBJECTIVE.md` and `sm2/DESIGN.md`.

| Forced | Why |
| --- | --- |
| **Record format cheap to turn into a PG tuple** | the executor works in tuple slots; a bespoke encoding pays a conversion per row, in both directions, across an IPC boundary |
| **Row identity is the primary key** | already chosen; it is `ybctid` by another name, and it is the only identity that fits an index-organized store without a surrogate allocator |
| **Batch-shaped API** | §2: the engine must be out of process, so a per-row interface is unsalvageable |
| **Key encoder takes a collation** | PG's text order is `strcoll`/ICU, not memcmp; the encoder needs `(type, collation, direction, nulls-order)` per column or half of SQL's `ORDER BY` is unrepresentable |
| **Collated keys store the sort key *and* the original** | the collation transform is lossy and irreversible; this is what YugabyteDB does, and it costs storing the value twice |
| **Collation version recorded per relation** | sort keys are not stable across collation library versions; PostgreSQL has had index corruption from exactly this, which is what `pg_collation.collversion` exists for |
| **Statistics are not optional** | the planner is being kept |
| **Relations multiplexed onto few column families** | sm1 targets hundreds to low thousands of relations; a schema with partitions and indexes passes that easily, and this limit will bind before most of the others |

And one that is not forced but is strongly advised: **sm2 must
build and be testable without a PostgreSQL source tree.**
Everything above shapes sm2's decisions; none of it should
become a dependency.

---

## 7. Recommended sequencing

1. **Build sm2 standalone**, to `sm2/PLAN.md`.
2. **Re-open the backward scan decision** (§5) before sm2's
   index design hardens around mirror relations.
3. **Spike the fork early, and throw it away.** A prototype
   that runs `SELECT` and `INSERT` on one table through sm2,
   built to find which of §2's and §4's walls bites first —
   not to be kept. The formats in `sm2/PLAN.md` steps 1 and 2
   are permanent once data exists; the spike is how to learn
   what they must carry *before* that happens.
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
- **RocksDB's approximate-statistics accuracy** for planner
  purposes. Not measured, and §6's statistics row depends on
  it.
- **The effort figures in §4** are calibrated judgment against
  the precedents, not measurements. Treat the ordering as
  sound and the absolute numbers as loose.
