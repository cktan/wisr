# DESIGN — sm3, schema and index maintenance

The level that turns sm2's tuples into a schema. sm2 gives it
typed rows in a heap keyed by `rowno`, and indexes whose
entries are keyed by encoded columns and point at a `rowno`.
sm3 gives those relations names and definitions, decides which
indexes a table has — the primary key's among them — and keeps
them in step with its heap, and adds predicates and statistics.
This document is the design of record; it follows the
decisions in `SM3_OBJECTIVE.md`:

| Decision | Choice |
| --- | --- |
| Catalog | in memory, written through to sm2, lock-based DDL |
| Index entry | `encode(index columns) ‖ rowno` → a tuple of the `rowno`, a cut flag and the INCLUDE columns; a unique index's key drops the `rowno` suffix unless a key column is NULL or the key was cut |
| Encoder | sm2's, called with the same per-column arguments |
| Relation mapping | one sm1 store per table, holding its rows, indexes and TOAST data, told apart by sm2's 2-byte object prefix |
| API shape | open: batch-oriented (sets of rows, filters pushed down) was forced by an IPC boundary, which the threaded fork removes (§1) |
| PostgreSQL | a target that shapes decisions, never a build dependency |

**Status: the catalog is implemented** — its keys, values,
load, DDL, a session's overlay and the schema locks of §2
(`CATALOG.md`), and §4's object mapping with them: a store
per table, index prefixes allocated and never reused, and
`DROP TABLE` as one `sm1::rmstore`. §3's index maintenance,
§5's predicate scans and §6's statistics are not built. sm2,
which it is built on, is, but for TOAST
(`sm2/SM2_DESIGN.md` §8.2).
This file exists so the protocols are argued before they are
written. Unlike sm2's two formats, almost nothing here is
permanent on disk — the catalog's own layout is the exception
— so the scrutiny goes to the concurrency instead.
`PLAN.md` has the order of work. The PostgreSQL target
(`PART4.md`) shapes these decisions but is never a build
dependency: sm3 must build and be tested without a PostgreSQL
source tree.

---

## 1. What sm2 and sm1 provide

sm3 is written against sm2's tuple relation, and against
`sm1/include/sm1.h` for the one thing sm2 has nothing to add
to: locking.

**Deployment: one process, one thread per session.** The
PostgreSQL fork is threaded (`PART4.md` §2), not process per
connection, so sm1, sm2 and sm3 are linked into the one server
process and every backend thread shares sm1's transaction
table, lock manager and SSI graph. A session is used by one
thread at a time (`sm1/API.md`). The boundary between the
executor and sm3 is a function call, not an RPC, so a per-row
interface is no longer ruled out. Whether sm3 keeps a
batch-shaped API anyway, for filter pushdown and fewer calls
per scan, is open (§5), and nothing below depends on the
answer.

| the stack gives sm3 | and the constraint it brings |
| --- | --- |
| sm2's tuples in a heap keyed by `rowno` | a row's identity is its `rowno`, which never changes; every index, the primary key's included, points at it |
| sm2's index `rel`: entries keyed by encoded columns, each pointing at a `rowno` | sm2 lays out an entry's bytes and scan bounds; which indexes exist, and writing their entries on every heap write, are sm3's (§3) |
| sm2's key encoder | every index key, the primary key's included, sorts by the same rules, because it is the same code (`sm2/SM2_DESIGN.md` §3) |
| sm2's 2-byte object prefix | a table's rows, indexes and TOAST data share its one sm1 store (§4) |
| `rel_update` / `rel_delete` returning the old image as a tuple | the old index entries can be computed without a read of their own; the image views sm1 session memory, valid until the next call, so sm3 copies it before any index call (§3) |
| sm2's codec over `datum_t` values and null flags, as PostgreSQL's `Datum` (`sm2/TUPLE.md` §9) | sm3 never handles a value's bytes: rows, index keys and bounds are `datum_t`s, and `encode_key` strips a varlena's header itself |
| `tup_desc_t`: a schema's widths and entry offsets, built once (`sm2/TUPLE.md` §6) | sm3 builds one per table and per index value when a definition loads or changes, so a one-column read costs a step per earlier null, not a pass over the row |
| a tuple of at most 64 KB, TOAST postponed (`sm2/TUPLE.md` §1) | a longer row fails with `SM_ERR_ROWTOOLONG`, which sm3 passes up, as PostgreSQL's `program_limit_exceeded`, until TOAST exists |
| `SM_ERR_DUPKEY` from sm1's insert | a unique index keyed without `rowno` — the primary key's among them — needs no separate existence check (§3) |
| `SM_ERR_UPDATED` and `rel_read_latest` at READ COMMITTED | a filtered update or delete must re-check its predicate, and recompute its index entries, from the newest version (§3); the error does not abort and is not retryable |
| transactions at RC / RR / SERIALIZABLE | index maintenance is atomic with the base write for free, because both are in one transaction |
| `sm1::lock` in Gray's modes (IS, IX, S, SIX, X) | the DDL and online index build protocol (§2, §3): readers and writers of a table coexist on its schema row, and an index build stops writers without stopping readers |
| nested savepoints | a statement can be undone without losing the transaction |
| forward scans | probe loops cost one cursor, not one per probe |
| `sm1::max_keylen` 4096 | an index entry's key must fit 4094 bytes after the prefix, `rowno` included where it carries one; an over-long one is truncated and flagged, and sm3 rechecks it (`sm2/SM2_DESIGN.md` §3.2) |
| no backward scan, ever | a descending column is stored with inverted bytes and scanned forward, in the same index; there are no mirror relations, and the fork's planner never asks for a backward scan (§5, §7) |
| one column family per store, hundreds to low thousands | tables, partitions included, are bounded by it (§4) |

---

## 2. Catalog

*To be written.* Tables, indexes, columns, types and
collations, stored in sm3's own sm2 relations and
written in the same sm1 transaction as the DDL that changes
them — which is what makes DDL transactional for free. It is
also where a collation's recorded version lives
(`sm2/SM2_DESIGN.md` §3), and where each column's dropped
mark and missing value live: a table's columns are only ever
appended, and a row too old to hold a column reads it as that
value, as in PostgreSQL (`sm2/SM2_DESIGN.md` §4).

**Decision: the catalog is held in memory.** It is loaded at
startup; a change is made in memory and written through to
sm2, which is used for persistence only. It replaces
PostgreSQL's heap catalogs (`PART4.md` §4), so sm1's status
tuple is the one commit point.

**Decision: schema changes are lock-based, as in PostgreSQL.**
The catalog is not versioned, and an old snapshot does not see
an old definition. PostgreSQL's catalogs are MVCC heap tables,
but it does not give a transaction the old schema either: it
keeps DDL and users of a table apart with table locks, so a
transaction that first touches a table after a DDL commits sees
the new definition, alongside its old data snapshot. sm3 does
the same with sm1's row locks, in Gray's modes, as PostgreSQL's
table locks are:

- every transaction that reads a table takes `sm1::lock` IS on
  that table's schema row, and one that writes it IX, held to
  the end of the transaction, before it reads the definition —
  PostgreSQL's AccessShareLock and RowExclusiveLock, which
  coexist;
- DDL takes the schema row X, which waits out the transactions
  in flight and blocks new ones until it commits; a plain index
  build takes S, which waits out writers and blocks new ones but
  lets readers through, as PostgreSQL's ShareLock does (§3);
- so once a transaction holds its IS or IX lock, the definition
  in memory cannot change under it. A catalog served from
  memory must take this lock, or DDL has nothing to wait on.
  The schema row is the table's `LOCK` key, which no one
  writes (`CATALOG.md` §2), so `sm1::lock` on it never fails
  on a newer commit, at any level: the lock alone is what
  protects the reader, and the in-memory definition, not a
  read of the row, is what it uses. `cat_lock_table` takes it.

Deadlocks between DDL and DML are found by sm1's detector like
any other.

PostgreSQL hardwires its catalogs and sequences to heap, so the
fork rewrites that seam instead of living with a hybrid, where
DDL atomicity would sit exactly on the seam. sm1's sequences are
used. Because the fork is threaded, one shared in-memory catalog
is coherent without cross-process invalidation. What it costs is
the code that reads the catalogs: the relation cache, syscache
and system-table scans. Logical decoding could be fed from sm1
instead of a second copy in PostgreSQL's WAL, but sm1 has no
commit-ordered change feed yet, and physical replication and PITR
over PostgreSQL's WAL do not apply to data held in RocksDB
(`PART4.md` §4).

Making memory authoritative moves the hard part; it does not
remove it. A definition that outlives the transaction that
changed it is the usual place such a design goes wrong.
`CATALOG.md` documents the catalog as built, which settles
bootstrap, row size and privacy until commit; its §7 settles
the others, not built yet:

- **Privacy until commit.** A DDL change must be invisible to
  other transactions until it commits, and undone on abort and
  on savepoint rollback. The shape expected is a
  per-transaction overlay, written to sm2 inside the DDL's own
  transaction and published to the shared catalog only after
  the commit, in the order sm1 uses for its commit cache
  (`sm1/SM1_DESIGN.md` §3.4).
- **Size and startup.** Load time and memory for a schema
  with many partitions.
- **Bootstrap.** Which sm1 store holds the catalog's own
  relations, how their schemas are known before the catalog
  is loaded — PostgreSQL hard-codes its bootstrap catalogs —
  and how the catalog's layout, the one format sm3 writes that
  lasts, is versioned.
- **Rows within 64 KB.** A catalog row is an sm2 tuple, so it
  is held to 64 KB with no TOAST behind it (`sm2/TUPLE.md`
  §1). A table's definition in one row, its columns' missing
  values included, could pass that; a row per column, as
  PostgreSQL's `pg_attribute` has, keeps each row small.
- **Temporary tables.** A temporary table is a temporary
  store, which never commits a catalog row (`DESIGN.md` §5,
  `sm1/SM1_DESIGN.md` §11.3). Its definition is the one the
  catalog holds in the session's memory only, never written
  through to sm2, and visible to no other session.

Settled for the layout, whatever its shape:

- **Type codes are sm2's frozen codes** (`sm2/TYPE.md` §3).
  Code 7, `decimal` before it was split, is retired; the
  catalog never writes it.
- **Each table's next index prefix is persisted**, since a
  prefix is never reused (§4) and must not be after a restart
  either.

### 2.1 Decimal columns

sm2 has no `decimal(p, s)`: it has `decimal64` and
`decimal128`, one per width (`sm2/SM2_DESIGN.md` §2.2). The
catalog maps a declared column to one of them.

**Decision: the narrowest that holds the precision.**

| Declared | sm2 type | typmod |
| --- | --- | --- |
| `decimal(p, s)`, `numeric(p, s)`, `p <= 18` | `decimal64` | `(p, s)` |
| `decimal(p, s)`, `numeric(p, s)`, `19 <= p <= 38` | `decimal128` | `(p, s)` |
| `decimal(p)` | as `decimal(p, 0)` | |
| bare `decimal` or `numeric` | `decimal64` | `(18, 3)`, DuckDB's default |

A precision past 38 is refused at `CREATE TABLE`. A bare
`decimal` rounds a datum past 3 fractional digits, as PostgreSQL
rounds for a declared typmod, and refuses one whose integer part
needs more than 15 digits; refusing a bare `decimal` was the
alternative. Widening a column from `decimal64` to `decimal128`,
as `ALTER COLUMN TYPE decimal(30, 2)` on a `decimal(10, 2)`
does, changes its type and so rewrites the table.

---

## 3. Indexes

How indexes are defined, maintained and built. What sm3 needs
from sm1 is in sm1 already: `sm1::lock` (`sm1/SM1_DESIGN.md`
§5.1), `sm1::stmt_begin` (§6.2), nested savepoints (§6.5),
`sm1::scan_seek` (§10.2) and the old row a write returns
(§5.3). What sm1 relies on is already there too:

- **Unique enforcement.** An insert into the index's store
  fails with `SM_ERR_DUPKEY`, and a concurrent inserter of the
  same key blocks on the row lock, then fails with a conflict.
- **Halloween protection.** A cursor's view is fixed at open,
  so updating an indexed column while scanning the index does
  not revisit the moved entries.
- **Scan the index, then fetch the base.** A read does not
  invalidate a cursor's current row, which lives until the
  next call on that cursor.
- **A statement's partial writes undo** to its savepoint, and
  a failed write leaves the transaction active and unchanged.

- **The primary key is an index.** The table is a heap keyed
  by `rowno` (`sm2/SM2_DESIGN.md` §5), and the primary key is a
  unique index over it, built and maintained like any other.
  A table may have none.
- **Entry:** `encode(sk) ‖ rowno` → a tuple of the `rowno`, a
  flag saying the key was cut, and the INCLUDE columns — sm2's
  index `rel` (`sm2/SM2_DESIGN.md` §5), built with sm2's encoder
  (`sm2/SM2_DESIGN.md` §3). The
  `rowno` suffix makes every entry unique, so a non-unique
  index needs no further disambiguation, and it is 8 bytes
  whatever the primary key is.
- **Unique indexes are keyed without `rowno`:**
  `encode(sk)` → the same tuple. The KV key
  is then the uniqueness unit, so sm1's insert refuses a
  duplicate with `SM_ERR_DUPKEY` and its write-write conflict
  handles two sessions inserting the same key at once; no
  separate existence check is needed. The primary key index
  is keyed this way. Two kinds of entry in a unique index
  still carry the suffix, and still need the existence check
  under `sm1::lock(EXCLUSIVE)`: one with a NULL key column, and
  one whose key was cut (below). At READ COMMITTED that check
  reads the newest version, not the snapshot: a statement
  snapshot can predate a duplicate that committed while the
  lock was awaited, and the lock is what keeps the newest
  version stable. **Open: nothing reads a range at its newest
  version.** The check is a scan over the key's prefix, but
  sm1's `read_latest` reads one key, and sm2's `idx_scan`
  reads at the transaction's snapshot. It needs a primitive
  that does not exist yet — a scan of the newest committed
  versions, in sm1 and through sm2 — or a check shaped
  differently.
- **NULLs are distinct in a unique index.** Two NULLs encode to
  the same bytes, so an entry with a NULL key column carries the
  `rowno` suffix and skips the uniqueness check, as SQL requires
  by default. A primary key column is never NULL, so the primary
  key index never has such an entry. `NULLS NOT DISTINCT` is
  refused at `CREATE INDEX` in v1; adding it later is a catalog
  flag and changes no bytes on disk.
- **A truncated key is rechecked.** sm2 cuts an over-long
  index key to fit and flags the entry (`sm2/SM2_DESIGN.md`
  §3.2). A scan rechecks flagged entries against the base row,
  and a unique check on a cut key compares originals over the
  prefix range, with NULLs distinct as above.
- **Equal means the collation's equal.** Under a
  non-deterministic collation sm2 gives strings ICU calls
  equal one key, so uniqueness and equality bounds follow it
  with no work here (`sm2/SM2_DESIGN.md` §3.3). Everything sm3
  compares itself must follow it too: a recheck, the unique
  check on a cut key, and hashing and grouping, which hash
  the sort key as PostgreSQL's `hashtext` does, not the
  string's bytes.
- **Maintenance:** an update keeps the row's `rowno`, so only
  the indexes whose columns changed are touched — a primary key
  change included. On update and delete, the old image from
  `rel_update` / `rel_delete` gives the old index entries to
  remove — as a tuple, so sm3 reads the indexed columns out
  of it without decoding bytes itself. The image views sm1
  session memory, valid only until the next call on the
  session, and the index calls that use it are calls on the
  session: sm3 copies it first, or a later index's old key is
  read from memory an earlier call reused.
- **One call writes a row and its indexes.** The executor
  writes a table through a table-level call that sm3 owns. It
  changes the heap row and then every index the table's
  definition lists. That call is the only way to write a row
  through the C API. The heap-level writes and the index
  entry writes in `include/sm.h` are scaffolding from before
  sm3 opened tables, and they leave it, so no caller can
  change a row and leave its entries behind. Reads and scans
  of the heap and of an index stay. The call's name and its
  handle are settled when maintenance is written (`PLAN.md`
  step 3).
- **Read committed writes.** An update or delete that meets a
  row committed after the transaction began fails with
  `SM_ERR_UPDATED` (`sm1/SM1_DESIGN.md` §5.4). It does not abort
  and it is not retryable. Resolving it needs the statement's
  filter, which neither sm2 (`sm2/SM2_DESIGN.md` §5) nor sm3
  has, so the work divides in two:
  - **The qual check is the executor's.** When the heap write
    is refused, sm3 has touched no index, and it returns the
    status to the caller as an outcome, not as a failure. The
    executor reads the newest committed image of the row,
    re-checks its filter against it, and repeats the write
    only if the row still qualifies, or skips it. A filter may
    span tables, so no image of this row alone could decide
    it.
  - **The index fix-up is sm3's.** The old image the repeated
    write hands back is the newest version's, not the one the
    statement's scan read. sm3 takes the index entries to
    remove from it, so the entries removed are the ones that
    are actually there. It needs no predicate for this.

  An insert has no filter and needs none of this. sm1 refuses
  a write repeated without the read, so an executor cannot
  forget it.
- **Online build on a populated relation**, the protocol
  `sm1::lock` exists for. The danger: a writer whose snapshot
  predates the build's commit does not know the index exists,
  so it changes the base relation and writes no entry.
  PostgreSQL prevents it with a ShareLock on the table, or with
  `CREATE INDEX CONCURRENTLY`, which waits out older
  transactions. Before `sm1::lock` neither could be expressed —
  locks were exclusive only, so a schema row every writer wrote
  would serialize them all, and SSI catches it only if every
  writer is SERIALIZABLE — which made correct behaviour
  impossible, not merely slow. The protocol:
  1. every writer takes `sm1::lock` IX on the table's schema
     row and reads it, so what it maintains is current;
  2. the builder takes the schema row **S**, which waits out the
     writers in flight and blocks new ones, while readers, who
     hold IS, go on;
  3. the builder scans the base relation — its snapshot is taken
     after the lock, so it sees every committed row — fills the
     index, and commits;
  4. blocked writers then get their IX lock and read the new
     schema, which commit published before they could read it
     (`CATALOG.md` §5). The schema row is never written, so
     their `sm1::lock` does not fail at any level: it is the
     lock that keeps them out while the build runs, not a
     conflict check.
  `sm1/test/test_lock.cc` has its shape end to end, in the
  shared and exclusive modes that came before Gray's. Its costs:
  - writers are blocked for the whole build, as by
    PostgreSQL's plain `CREATE INDEX`; readers are not;
  - recording the index rewrites the table's catalog entry, so
    that write must not take the schema lock X, or readers are
    blocked after all: the lock key and the entry's key are
    separate (`CATALOG.md` §2);
  - the schema row is a hot key: every reader and writer of the
    table locks it, so its lock-table mutex is contended. The
    table is one mutex, not sharded, so this is the first place
    a benchmark will point.

  The alternative was a primitive that waits for every
  transaction begun before now, with a CIC-style multi-phase
  build here. It needs no shared locks, but it admits stale
  entries when a writer's `sm1::erase` of an entry not yet
  built gets `NOTFOUND`: harmless for a non-unique index whose
  readers recheck the base row, false duplicates for a unique
  one. `sm1::lock` won because it also gives lock-without-write
  — `SELECT … FOR UPDATE` through an index had to write a dummy
  version to lock the base row. The alternative waits until
  blocking builds hurt.
- **Partial and expression indexes wait for an evaluator.**
  Maintaining one means evaluating the index's own predicate
  or expression against a row image. That is sm3's to do, and
  it needs no help from the executor per write. The evaluator
  arrives with §5, and `CREATE INDEX` refuses both kinds until
  then: `cat_create_index` takes key columns and INCLUDE
  columns only, so the catalog cannot record a predicate or
  an expression. INCLUDE columns need no evaluator and are
  maintained with the key.
- **sm3 drives maintenance; the executor owns the qual.** The
  kept executor could write index entries itself through its
  index access method, as PostgreSQL does, reaching sm2's
  index `rel` through the C wrapper (`DESIGN.md` §5). It does
  not. The unique checks on NULL and cut keys, the online
  build, and the index and the heap never disagreeing are
  sm3's whoever writes the entries, and the last holds only
  if one layer writes both the row and its entries. What
  stays above is `EvalPlanQual`: re-checking a row under READ
  COMMITTED needs the statement's filter.

---

## 4. Object mapping

**Decision: one sm1 store per table.** A table's rows, all
its secondary indexes and its TOAST data live in one
sm1 store, told apart by sm2's 2-byte object prefix
(`sm2/SM2_DESIGN.md` §5):

| Prefix | Object |
| --- | --- |
| `0x0000` | the table's rows |
| `0x0001`–`0xFFFE` | its indexes, one prefix each |
| `0xFFFF` | its TOAST data |

sm3 chooses the prefixes and records them in the catalog. An
sm3 table or index is one `rel` over the table's sm1 store
with its prefix; sm2 prepends and strips it and otherwise
ignores it. sm2 never learns what a table is, and sm3 never
encodes a key itself.

- **`DROP TABLE` is one `sm1::rmstore`**, sm1's O(1) column
  family drop, and takes the indexes and TOAST data with it.
- **`DROP INDEX` is a range delete** over the index's prefix.
- **Index prefixes are allocated in increasing order and never
  reused.** A dropped index's versions linger until sm1's
  garbage collection passes them; never reusing a prefix means
  no later index can read them as its own, with no rule tied
  to the GC horizon. 65,534 index creations per table,
  rebuilds included, is the ceiling.
- **The cost: tables are bounded by sm1's store count.**
  sm1 targets hundreds to low thousands of stores, and
  every table, each partition included, takes one. Accepted
  for v1; raising that ceiling is sm1's problem if it bites.

The alternative was many tables per sm1 store, with a wider
object id as the prefix, which makes the relation count a
tuning decision. It was rejected for v1 because `DROP TABLE`
becomes a range delete instead of sm1's O(1) drop.

---

## 5. Scan by predicate

*To be written.* A predicate goes in, a key range comes out of
it, and the residual filter is evaluated below the API instead
of returning rows for a caller to discard.

- Deriving the range from the predicate is the whole point:
  the columns of the index, in order, matched against the
  predicate's conjuncts, giving a start key, an end key, and
  whatever is left over as a residual.
- The residual is evaluated against the tuple, per column
  (`sm2/SM2_DESIGN.md` §5), so a filter on one column of a wide
  row does not decode the row.
- Forward only, at every level (§1). An `ORDER BY x DESC` that
  an index could have served backwards is a top-N sort over a
  forward scan instead (§7).

Open: **the API shape.** Sets of rows in and out was forced by
an IPC boundary that the threaded fork removes (§1). Filter
pushdown and fewer calls per scan are reasons to keep it
anyway, and the choice belongs here, at the level that has the
filters — sm2 is tuple-at-a-time either way.

---

## 6. Statistics

*To be written.* Statistics are not optional, because the planner
is kept. It needs `reltuples`, `relpages`, histograms and
correlations, and `ANALYZE` needs a sampling path: row counts,
sizes, and a sampling primitive from sm1. For a heap keyed by
`rowno`, correlation is measured as in PostgreSQL: `rowno` is
roughly insertion order, so a column that grows with insertion
correlates, and the primary key usually does. How accurate
RocksDB's approximate statistics are for this purpose has not
been measured, and this section depends on it.

---

## 7. Known trade-offs

*To be filled in as the design settles, in the manner of
`sm1/SM1_DESIGN.md` §13 — the costs this design accepts, written
down where the next reader will find them.*

Accepted so far, from the PostgreSQL target (`PART4.md`):

- **Plans lost to the backward-scan ban.** An index-ordered
  `ORDER BY x DESC LIMIT n` and a merge join that wants
  descending order become a top-N sort over a forward scan, not
  an early-terminating index scan. `MAX()`, which PostgreSQL
  answers with a backward index scan, is expected to lose that
  plan; `MIN()` keeps its forward one. Scrollable cursors get a
  Materialize node, because `ExecSupportsBackwardScan` is
  changed to return false for every table. These lose plans, not
  correctness, and the size of the loss is expected from how the
  planner works but has not been measured.
- **Tables are bounded by sm1's store count.** One sm1
  store per table makes `DROP TABLE` sm1's O(1) drop, and
  caps tables, partitions included, at the hundreds to low
  thousands of stores sm1 handles (§4). `DROP INDEX` is a
  range delete inside the table's relation.
- **The catalog is not versioned.** A transaction gets the
  newest definition and its own data snapshot, which is what
  PostgreSQL does, and the price is that DDL blocks DML on the
  same table rather than passing it (§2).
- **The version lag is permanent.** The fork replaces the
  storage and transaction layers and makes PostgreSQL thread
  safe, so tracking upstream releases is harder than the version
  lag it already counts as a liability.
- **Not unblocked by sm3:** replication, PITR and logical
  decoding over PostgreSQL's WAL (§2).
