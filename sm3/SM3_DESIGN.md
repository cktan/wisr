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
| Index entry | `encode(index columns) ‖ rowno` → a tuple of the `rowno`, a cut flag and the INCLUDE columns; a unique index's key drops the `rowno` suffix unless a key column is NULL, and is never cut |
| Encoder | sm2's, called with the same per-column arguments |
| Relation mapping | one sm1 store per table, holding its rows, indexes and TOAST data, told apart by sm2's 2-byte object prefix |
| API shape | open: batch-oriented (sets of rows, filters pushed down) was forced by an IPC boundary, which the threaded fork removes (§1) |
| PostgreSQL | a target that shapes decisions, never a build dependency |

**Status: the catalog is implemented** — its keys, values,
load, DDL, a session's overlay and the schema locks of §2
(`CATALOG.md`), and §4's object mapping with them: a store
per table, index prefixes allocated and never reused, and
`DROP TABLE` as one `sm1::rmstore`. §3's index maintenance
is built, the C API's one way to write a row, and so is its
index build; §5's predicate scans and §6's statistics are
not. sm2, which it is built on, is, but for TOAST
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
  is keyed this way. One kind of entry in a unique index still
  carries the suffix: one with a NULL key column (next). It
  needs no check either, so sm3 makes no uniqueness check of
  its own in any case.
- **NULLs are distinct in a unique index.** Two NULLs encode to
  the same bytes, so an entry with a NULL key column carries the
  `rowno` suffix and skips the uniqueness check, as SQL requires
  by default. A primary key column is never NULL, so the primary
  key index never has such an entry. `NULLS NOT DISTINCT` is
  refused at `CREATE INDEX` in v1; adding it later is a catalog
  flag and changes no bytes on disk.
- **A truncated key is rechecked, and a unique index has
  none.** In a non-unique index sm2 cuts an over-long key to
  fit and flags the entry (`sm2/SM2_DESIGN.md` §3.2), and a
  scan rechecks flagged entries against the base row. In a
  unique index an over-long key refuses the write with
  `SM_ERR_KEYTOOLONG`, whether or not a key column is NULL,
  as PostgreSQL refuses an oversize B-tree entry. Two
  different originals can share a cut key's bytes, so a cut
  key would need a check that compares originals over the
  prefix range, and at READ COMMITTED that check would have
  to read a range at its newest committed version, which
  nothing in the stack does (§7).
- **Equal means the collation's equal.** Under a
  non-deterministic collation sm2 gives strings ICU calls
  equal one key, so uniqueness and equality bounds follow it
  with no work here (`sm2/SM2_DESIGN.md` §3.3). Everything sm3
  compares itself must follow it too: a recheck, and hashing
  and grouping, which hash the sort key as PostgreSQL's
  `hashtext` does, not the string's bytes.
- **Maintenance:** an update keeps the row's `rowno`, so only
  the indexes whose columns changed are touched — a primary key
  change included. An index is left alone when the row's key
  columns compare equal before and after, by their types'
  `cmp`, and its INCLUDE columns hold the same bytes. The two
  tests differ on purpose. The key folds values the type calls
  equal, a float's `-0` and `+0`, so `cmp` decides whether the
  key moved. An INCLUDE column is stored as written, so an
  entry skipped on `cmp` alone would keep `-0` beside a heap
  row that says `+0`. On update and delete, the old image from
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
  of the heap and of an index stay. In sm3 the calls are
  `tbl_open`, on a table's oid and in the mode of its schema
  lock, then `tbl_insert`, `tbl_update` and `tbl_delete`, with
  `tbl_read`, `tbl_read_latest` and the two scans beside them
  (`include/sm3.h`). A handle is good for one transaction, the
  schema lock ending with it.
- **A write sets no savepoint.** It is the heap write and then
  a write or two per index, and one that fails after its first
  step — a duplicate in a unique index, an over-long key —
  leaves the earlier steps done: the heap and the indexes
  disagree inside the transaction. The caller rolls back to a
  savepoint of its own, or aborts, before going on, as sm1
  expects of the layer that makes one row write into several
  (`sm1/SM1_DESIGN.md` §6.5). A mark per row was the
  alternative; it costs a lock-table mutex acquisition per
  row, and PostgreSQL's errors abort the transaction or the
  subtransaction anyway. `SM_ERR_UPDATED` needs no rollback:
  it comes from the heap write, the first step.
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
    are actually there. It needs no predicate for this. Those
    entries were committed after the transaction began too,
    and sm2's `idx_delete` removes them all the same
    (`sm2/SM2_DESIGN.md` §5).

  An insert has no filter and needs none of this. sm1 refuses
  a write repeated without the read, so an executor cannot
  forget it.
- **Building an index on a populated relation.** There is one
  build and it is concurrent: neither writers nor readers are
  shut out for longer than a catalog change takes. An index
  has a state, *building*, *valid* or *failed*, kept in its
  record (`CATALOG.md` §3). The danger is a writer that does
  not know the index exists, changes the base relation and
  writes no entry. PostgreSQL's `CREATE INDEX CONCURRENTLY`
  meets it by entering the index as not ready and not valid,
  waiting out older transactions, scanning the table twice and
  waiting again. Here the schema lock does the waiting and the
  row lock stands in for the second scan:
  1. **Create.** `cat_create_index` takes the schema lock X,
     as every DDL does, and records the index as building. A
     writer holds that lock IX to the end of its transaction,
     so when this commits no writer is left that does not know
     the index, and the next one reads a definition that lists
     it (`CATALOG.md` §5).
  2. **Maintain.** From then on every writer maintains the
     index as it does any other, with one allowance: while the
     index is building, an entry to remove that is not there
     is no error, since the backfill may not have reached the
     row. A row a writer touches is covered from then on.
  3. **Backfill.** A background task walks the heap in `rowno`
     order, in batches, each a short READ COMMITTED
     transaction that holds the schema lock IX. For each row
     it takes the row's lock shared, reads the row's newest
     committed version, and adds its entry if it is not there.
  4. **Validate.** When the walk reaches the end of the heap,
     the task marks the index valid, a catalog change like any
     other.

  Step 3 is safe because of the order a writer works in. It
  changes the heap row before any index and holds that row X
  to its commit, so the task and a writer never work on one
  row at once. Either the task waits for the writer and then
  finds the entry already made, or the writer waits for the
  task and then finds the entry to remove. No second scan is
  needed, and no lock stronger than a row's.

  The walk is in `rowno` order, not key order. A B-tree gains
  from sorted input; RocksDB sorts what it is given when it
  flushes, the heap is read sequentially by `rowno`, and each
  row is read again under its lock whatever the order.

  What follows from it:
  - **A unique index finds some duplicates late.** A duplicate
    of a row not yet reached is found only when the backfill
    reaches that row and its entry meets another row's. The
    build then fails and marks the index failed, as PostgreSQL
    leaves an invalid index behind. A failed index is not
    maintained and cannot be scanned; it can only be dropped.
  - **A failed build keeps its reason.** Any error from a
    batch that is not retryable fails the build: a duplicate,
    a key too long for a unique index, or anything else. The
    index's record keeps the error's code and its message,
    across restarts, and `cat_index_wait` returns them to
    whoever waits.
  - **A reader with an older snapshot is kept off the index.**
    Entries are versioned as rows are, so a snapshot older
    than the last batch sees the index partly filled.
    PostgreSQL waits such transactions out. Here the catalog
    counts its publishes; a session notes the count before it
    takes a snapshot, at `begin` and at each statement of a
    READ COMMITTED transaction; and it may scan an index only
    if the index became valid at or before that count. An
    index made valid in its own transaction it may scan at
    once. Writers maintain an index whatever their snapshot.
    A READ COMMITTED transaction that begins no statements
    reads each call at a fresh snapshot and may scan the index
    as soon as it is valid. The count is kept in memory only:
    no snapshot survives a restart. `tbl_index_usable` answers
    for a planner, and `tbl_idx_scan` refuses an index that is
    not usable. A session's statements begin through sm3,
    `stmt_begin`, since that is where the count is noted. The
    guard is for a transaction that has not yet touched the
    table: marking the index valid takes the schema lock X,
    which waits out every one that has.
  - **A table made in the same transaction** is seen by no one
    else, so `cat_create_index` runs the backfill itself, over
    the rows the transaction has written, and records the
    index valid at once. It is the same routine, called from a
    second place, not a second build.
  - **Waiting.** `cat_index_wait` blocks until the index is
    valid, and fails if the build failed. It waits on the
    index's state, not on a thread, so any session may wait,
    for a build another session or an earlier run of the
    process began. It is called outside a transaction, so that
    the caller's next snapshot is new enough to scan the
    index.
  - **Restart.** A build unfinished at a restart starts again
    from the first row. Adding only what is absent makes that
    safe, and no progress is kept on disk.
  - **The task is a queue and a few workers.** The catalog
    keeps a queue of builds and runs at most N of them at
    once, N an option that defaults to 4. A worker is a thread
    with a session of its own, from whoever opened the
    catalog. It is started when a build is queued and fewer
    than N are running, and it exits when it finds the queue
    empty, so a database with nothing to build holds no
    thread. Opening a catalog queues every index still marked
    building, which is how a build survives a restart. One
    routine does a batch, and tests call it directly. Each
    batch is a transaction of its own, and so is marking the
    index valid: it waits for every user of the table, and
    must hold no row one of them may want. A batch that ends
    in a retryable error is run again, and one that ends in
    any other fails the build. `cat_builder_start`
    starts all this, and `cat_close` stops the workers and
    waits for them, so no transaction on a table being built
    may be open then. A thread
    per build was the alternative: less code, but a restore
    that creates hundreds of indexes would start hundreds of
    threads, and nothing would restart a build after a crash.

  Its costs:
  - a writer above READ COMMITTED that updates a row
    backfilled after its snapshot gets `SM_ERR_CONFLICT` from
    the index entry, though the heap row had not changed, and
    retries;
  - uniqueness is not enforced against a row the backfill has
    not reached, so a build can fail late, over a row
    committed while it ran;
  - the state is in the index's record, which changes the
    catalog's format;
  - four workers are not four times the speed: builds contend
    on sm1's lock table, one mutex, and on RocksDB's write
    path, and the default is unmeasured;
  - two indexes built on one table walk its heap twice, where
    one pass could fill both;
  - the schema row is a hot key: every reader and writer of the
    table locks it, so its lock-table mutex is contended. The
    table is one mutex, not sharded, so this is the first place
    a benchmark will point.

  The alternative was the build this section first described:
  the builder takes the schema lock S for the whole build,
  which shuts writers out and lets readers on, as PostgreSQL's
  plain `CREATE INDEX` does, and fills the index in one
  transaction. It is simpler, but it would have been a second
  build to keep beside this one, and its one transaction
  holds a lock and an undo entry in memory for every row
  (`sm1/SM1_DESIGN.md` §13). `sm1::lock` is used all the same:
  the schema lock, and the shared row lock of step 3.
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
  not. The rules of a unique index's key, the online
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
- **An over-long key in a unique index is refused.** A write
  whose unique-index key exceeds the key budget fails with
  `SM_ERR_KEYTOOLONG`, where a non-unique index would cut the
  key (§3). Accepting it needs a uniqueness check over a cut
  key's prefix range at the newest committed version, a new
  concurrency primitive in sm1 with a locking rule of its
  own, since `sm1::read_latest` reads one key under a lock
  already held and a range has no single key to lock. That is
  too much for the rarest path in index maintenance. The
  primitive is built when a workload asks for it.
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
