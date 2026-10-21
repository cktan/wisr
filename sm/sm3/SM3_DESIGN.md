# DESIGN — sm3, schema and index maintenance

The level that turns sm2's tuples into a schema. sm2 gives it
typed rows in a heap keyed by `rowno`, and indexes whose
entries are keyed by encoded columns and point at a `rowno`.
sm3 gives those relations names and definitions, decides which
indexes a table has — the primary key's among them — and keeps
them in step with its heap, and adds statistics.
This document is the design of record; it follows the
decisions in `SM3_OBJECTIVE.md`:

| Decision | Choice |
| --- | --- |
| Catalog | in memory, written through to sm2, lock-based DDL |
| Index entry | `encode(index columns) ‖ rowno` → a tuple of the `rowno`, a cut flag and the INCLUDE columns; a unique index's key drops the `rowno` suffix unless a key column is NULL, and is never cut |
| Encoder | sm2's, called with the same per-column arguments |
| Relation mapping | one sm1 store per table; a store is a collection of rels -- its heap, one per index, one for TOAST -- told apart by sm2's 2-byte relid |
| API shape | tuple at a time; a scan takes key bounds and filters nothing, and whatever needs evaluating is the host's, by callback (§3, §5) |
| PostgreSQL | a target that shapes decisions, never a build dependency |

**Status: the catalog is implemented** — its keys, values,
load, DDL, a session's overlay and the table-oid locks of §2
(`CATALOG.md`), and §4's object mapping with them: a store
per table, index relids allocated and never reused, and
`DROP TABLE` as one `sm1::rmstore`. §3's index maintenance
is built, the C API's one way to write a row, and so is its
index build, for partial and expression indexes too; §6's
statistics are not. sm2, which it is built on, is, and its
TOAST (`sm2/SM2_DESIGN.md` §4.2) is used here once the host
says it reads TOAST pointers (§3.4).
This file exists so the protocols are argued before they are
written. Unlike sm2's two formats, almost nothing here is
permanent on disk — the catalog's own layout is the exception
— so the scrutiny goes to the concurrency instead.
§8 has the order of work. The PostgreSQL target
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
| sm2's 2-byte relid | a table's heap, indexes and TOAST data are rels in its one sm1 store (§4) |
| `rel_update` / `rel_delete` returning the old image as a tuple | the old index entries can be computed without a read of their own; the image is pinned into a ref, the caller's or one of the write's own, so the index calls leave it where it is and sm3 copies nothing (§3, `../PIN.md`) |
| sm2's codec over `datum_t` values and null flags, as PostgreSQL's `Datum` (`sm2/TUPLE.md` §9) | sm3 never handles a value's bytes: rows, index keys and bounds are `datum_t`s, and `encode_key` strips a varlena's header itself |
| `tup_desc_t`: a schema's widths and entry offsets, built once (`sm2/TUPLE.md` §6) | sm3 builds one per table and per index value when a definition loads or changes, so a one-column read costs a step per earlier null, not a pass over the row |
| a tuple of at most 64 KB, its long datums out of line with TOAST (`sm2/SM2_DESIGN.md` §4.2) | a row is as long as its datums once the host reads TOAST pointers (§3.4); without, or in an index entry, a longer one fails with `SM_ERR_ROWTOOLONG`, which sm3 passes up, as PostgreSQL's `program_limit_exceeded` |
| `SM_ERR_DUPKEY` from sm1's insert | a unique index keyed without `rowno` — the primary key's among them — needs no separate existence check (§3) |
| `SM_ERR_UPDATED` and `rel_read_latest` at READ COMMITTED | a filtered update or delete must re-check its predicate, and recompute its index entries, from the newest version (§3); the error does not abort and is not retryable |
| transactions at RC / RR / SERIALIZABLE | index maintenance is atomic with the base write for free, because both are in one transaction |
| `sm1::lock` in Gray's modes (IS, IX, S, SIX, X) | the DDL and online index build protocol (§2, §3): readers and writers of a table coexist on its table-oid lock, DDL waits them out, and an index build stops neither once its index is recorded |
| nested savepoints | a statement can be undone without losing the transaction |
| forward scans | probe loops cost one cursor, not one per probe |
| `sm1::max_keylen` 4096 | an index entry's key must fit 4094 bytes after the relid, `rowno` included where it carries one; an over-long one is truncated and flagged, and sm3 rechecks it (`sm2/SM2_DESIGN.md` §3.2) |
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
the same with an sm1 OID lock on the table's oid, the
**table-oid lock**, in Gray's modes, as PostgreSQL's table
locks are:

- every transaction that reads a table takes `sm1::lock` IS on
  that table's oid, and one that writes it IX, held to
  the end of the transaction, before it reads the definition —
  PostgreSQL's AccessShareLock and RowExclusiveLock, which
  coexist;
- DDL takes it X, which waits out the transactions
  in flight and blocks new ones until it commits. CREATE INDEX
  is DDL: `create_index` takes X only to record the index as
  pending, and the build that fills it then runs beside readers
  and writers, setting its state under IX (§3, §3.3);
- so once a transaction holds its IS or IX lock, the definition
  in memory cannot change under it. A catalog served from
  memory must take this lock, or DDL has nothing to wait on.
  The lock is an `sm1::LOCKTYPE_OID` lock on the table's
  oid: a name in no store, which nothing writes, so
  `sm1::lock` reads no store and checks no version and cannot
  fail on a newer commit at any level. The lock alone is what
  protects the reader, and the in-memory definition, not a
  read of a row, is what it uses. `cat_lock_table` takes it.

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
  `TEMPTABLE.md` at the top level is the design, and it is
  built: a table made by `create_temp_table`, in no
  namespace.

Settled for the layout, whatever its shape:

- **Type codes are sm2's frozen codes** (`sm2/TYPE.md` §3).
  Code 7, `decimal` before it was split, is retired; the
  catalog never writes it.
- **Each table's next index relid is persisted**, since a
  relid is never reused (§4) and must not be after a restart
  either.

### 2.1 Decimal columns

sm has no `decimal(p, s)`: a decimal column is sm2's
`decimal64` or `decimal128`, one per width
(`sm2/SM2_DESIGN.md` §2.2), with its `(p, s)` typmod, as the
host declares it.

**Decision: the host chooses the width, not sm3.** SQL's
`decimal(p, s)` and `numeric(p, s)`, `decimal(p)` and a bare
`numeric` are resolved above sm, to one of the two and a
declared `(p, s)`: digres's rules, and its default for a bare
`numeric`, are in `../../dg/DECIMAL.md` §2.3. sm3 takes the
type and typmod it is given, and supplies no default; a typmod
the type refuses, `-1` or a precision past its 17 or 36 digits
among them, fails `CREATE TABLE` with `SM_ERR_INVALID`.
Changing a column from `decimal64` to `decimal128` changes its
type and so rewrites the table.

---

## 3. Indexes

How indexes are defined, maintained and built. What sm3 needs
from sm1 is in sm1 already: `sm1::lock` (`sm1/SM1_DESIGN.md`
§5.1), `sm1::stmt_begin` (§6.2), nested savepoints (§6.5),
`sm1::scan_seek` (§10.2) and the old row a write returns
(§5.3). What sm1 relies on is already there too:

- **Unique enforcement.** An insert into the index's store
  fails with `SM_ERR_DUPKEY`, and a concurrent inserter of the
  same key waits for the first one's uncommitted version, then
  fails the same way if that commits (`LOCK.md` §4.1).
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
  table-oid lock ending with it.
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
- **A write refuses a NULL in a `NOT NULL` column**, with
  `SM_ERR_NOTNULL`, before the heap write, so nothing is half
  done and the transaction goes on. It is the one constraint
  sm3 enforces. The check is exact, it costs a flag a column,
  and sm3 leans on it itself: a primary key column is taken
  never to be NULL (above). The `varchar(n)` limit is not
  checked here. It is a coercion with PostgreSQL's own rules,
  trimming excess spaces and counting characters, so it is the
  host's, as all evaluation is (`DESIGN.md` §5).
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
  has a state, *pending*, *valid* or *failed*, kept by index
  and outside the table's definition (`CATALOG.md` §2, §4).
  A pending index a worker is filling is *building*, which
  only the builder knows and nothing records. The danger is a
  writer that does
  not know the index exists, changes the base relation and
  writes no entry. PostgreSQL's `CREATE INDEX CONCURRENTLY`
  meets it by entering the index as not ready and not valid,
  waiting out older transactions, scanning the table twice and
  waiting again. Here the table-oid lock does the waiting and the
  row lock stands in for the second scan:
  1. **Create.** `create_index` takes the table-oid lock X,
     as every DDL does, and records the index as pending. A
     writer holds that lock IX to the end of its transaction,
     so when this commits no writer is left that does not know
     the index, and the next one reads a definition that lists
     it (`CATALOG.md` §5).
  2. **Maintain.** From then on every writer maintains the
     index as it does any other, with one allowance: while the
     index is pending, an entry to remove that is not there
     is no error, since the backfill may not have reached the
     row. A row a writer touches is covered from then on.
  3. **Backfill.** A background task walks the heap in `rowno`
     order, in batches, each a short READ COMMITTED
     transaction that holds the table-oid lock IX. For each row
     it takes the row's lock shared, reads the row's newest
     committed version, and adds its entry if it is not there.
  4. **Validate.** When the walk reaches the end of the heap,
     the index is whole: there is nothing to catch up, since
     writers have maintained it all along. The task marks it
     valid in a transaction of its own, under the table-oid
     lock IX, as a batch holds it, so it waits for no one
     using the table.

  Step 3 is safe because of the order a writer works in. It
  changes the heap row before any index, and its uncommitted
  version of the row stands until it commits, which the
  task's row lock waits for; a writer in turn waits for the
  task's row lock. So the task and a writer never work on one
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
    index's state keeps the error's code and its message, and
    for a duplicate the rownos of the two rows that hold the
    key, across restarts, and `builder_wait` returns them to
    whoever waits, the rows in `sm1::errdup()`, so the host
    can show the key.
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
    guard covers every transaction, one that has opened the
    table included: marking the index valid waits for none of
    them.
  - **Marking it valid waits for no one.** An index's state is
    kept by index and set under the table-oid lock IX, for the
    reasons §3.3 gives.
  - **A table made in the same transaction** is seen by no one
    else, so `create_index` runs the backfill itself, over
    the rows the transaction has written, and records the
    index valid at once. It is the same routine, called from a
    second place, not a second build.
  - **Waiting.** `builder_wait` blocks until the index is
    valid, and fails if the build failed. It waits on the
    index's state, not on a thread, so any session may wait,
    for a build another session or an earlier run of the
    process began. It is called outside a transaction, so that
    the caller's next snapshot is new enough to scan the
    index. The caller may hand it a flag, a `std::atomic<int>`
    of its own, which ends the wait once it is not 0: the
    stack reads it and calls nothing of the caller's.
  - **Restart.** A build unfinished at a restart starts again
    from the first row. Adding only what is absent makes that
    safe, and no progress is kept on disk.
  - **The task is a queue and a few workers, and no part of
    the catalog.** The builder is the process's: one queue of
    pending indexes over every catalog attached to it, and at
    most N builds at once, N an option that defaults to 4. A
    worker is a thread, started when a build is queued and
    fewer than N are running, and it exits when it finds the
    queue empty, so a process with nothing to build holds no
    thread. It takes an index from the queue, which makes the
    index building, and fills it on a session of its own on
    that index's database, from whoever attached the catalog.
    One routine does a batch, and tests call it directly. Each
    batch is a transaction of its own, and so is marking the
    index valid: it waits for every user of the table, and
    must hold no row one of them may want. A batch that ends
    in a retryable error is run again, and one that ends in
    any other fails the build.
  - **The builder is a caller of the catalog.** It reads and
    changes one through the catalog's own calls
    (`CATALOG.md` §5), and the catalog knows nothing of it.
    The DDL knows its queue: `create_index` gives
    `sm1::on_commit` a function that puts the pending index on
    the queue, so it is queued once its transaction has
    committed and never if that aborts. The function runs
    before the commit publishes the new definition; a worker
    that takes the index at once waits for the publish when it
    opens the table.
    The builder does not look for work: it is given a table
    and an index, checks the index is still pending, and fills
    it. So after a restart, which loses those functions, the
    DDL's `enqueue_pending_indexes` walks the catalog and
    queues every index still pending, which is how a build
    survives one; nothing is queued twice. `builder_detach`
    drops what is queued for a catalog and waits for its
    builds to end, each after the batch it is in; it comes
    before `cat_close`, so no transaction on a table being
    built may be open then. A thread
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
  - a table's oid is a hot name: every reader and writer of the
    table locks it, so its lock-table mutex is contended. The
    table is one mutex, not sharded, so this is the first place
    a benchmark will point.

  The alternative was the build this section first described:
  the builder takes the table-oid lock S for the whole build,
  which shuts writers out and lets readers on, as PostgreSQL's
  plain `CREATE INDEX` does, and fills the index in one
  transaction. It is simpler, but it would have been a second
  build to keep beside this one, and its one transaction
  holds a lock and an undo entry in memory for every row
  (`sm1/SM1_DESIGN.md` §13). `sm1::lock` is used all the same:
  the table-oid lock, and the shared row lock of step 3.
- **Partial and expression indexes are evaluated by the host,
  through a callback.** Maintaining one means deciding whether
  a row belongs in the index and computing its key, from the
  index's predicate and its expressions. sm3 does neither
  itself: it has no evaluator and is to get none. The catalog
  keeps what the host gives it for an index, its predicate and
  its expressions, as bytes it does not read. The host
  registers one evaluator with the catalog. Given a table, an
  index and a row image as its values, it says whether the row
  belongs in the index and gives the row's key values, and sm3
  calls it
  wherever it computes an entry: on insert, on the old and the
  new image of an update and of a delete, and in the build.
  INCLUDE columns are plain columns and need none of this.

  The callback's rules:
  - **No `longjmp` out of it, and no C++ exception.** Either
    would unwind through sm3's frames and skip what they clean
    up.
  - **Failure is a return code, with the error set.** The
    write or the build fails with that error, and a failed
    build keeps it as its reason.
  - **It is deterministic.** The same row image gives the same
    answer every time, or a later delete computes another key
    and does not find the entry. PostgreSQL asks the same of
    an index's functions: they must be immutable.
  - **It may be called from any thread**, sm3's build workers
    among them, where no executor is running.
  - **It does not call the storage manager on the same
    session**, which is in the middle of a write.

  This is the host's to keep and not a wrapper's, because the
  fork is threaded and rewrites PostgreSQL's functions and
  operators for threads: how they report an error, and whether
  they need a backend around them, are choices made as they
  are ported. The results are PostgreSQL's by construction —
  NULLs, collations, coercions, every function — which no
  second implementation could promise.

  The alternative was an evaluator in sm3 over a subset safe to
  push down: comparisons of a column with a constant,
  conjunctions, `IS [NOT] NULL`. It would have refused any
  index outside the subset, an expression index with a
  function among them, and to widen it meant a function
  library that had to agree with PostgreSQL's row for row. A
  third course is for sm3 to link the fork's rewritten
  functions and call them without a callback. It ties the
  stack to PostgreSQL's function catalog, and it stays open if
  the callback ever shows in a profile.

  How it is built:
  - **What the callback computes** is only what sm3 cannot:
    whether the row belongs in the index, and the values of
    the key columns that are expressions. A plain key column
    sm3 reads from the row itself, as it does now, so a partial
    index over plain columns costs one yes or no an image. An
    expression column has no table column behind it, so the
    catalog records its type, typmod and collation, given when
    the index is created.
  - **The values it hands back are owned by whoever asked.**
    The evaluator fills a `std::vector<std::string>`, one
    string per expression column, each the value's bytes as
    sm2's `datum_bytes` gives them, a varlena for a
    variable-width type; a flag beside each says null. No rule
    says how long the host's memory must live, and sm3 holds
    an old image's values and a new one's together when it
    compares them. It costs a copy of each value a call.
    `std::string` and `std::vector` are the two C++ types that
    may cross `include/sm.h` with owned data (`DESIGN.md` §5).
  - **An update leaves such an index alone by its columns.**
    The host lists the columns the predicate and the
    expressions read, when the index is created, and the
    catalog records them. If none of them changed, by their
    bytes, and no plain key or INCLUDE column did (above), sm3
    does not call the evaluator at all. If one did, it
    evaluates both images: an entry is removed for the old
    one if it belonged, and added for the new one if it does,
    unless both belong with equal keys.
  - **The predicate and the expressions are one opaque blob**
    an index, serialized as the host likes, and handed back to
    the evaluator in the index's definition.
  - **The evaluator is registered once an environment, before
    a database is opened**, since opening one attaches it to
    the builder, which may at once resume a build a restart
    left.
    With none registered, an index that needs one cannot be
    created, and a write to its table or a build of it fails
    with an error that says so.

  The evaluator is shown a row as its values and null flags,
  one a column, and not as a tuple: a write has the new row
  that way already, sm3 deforms the old image to maintain any
  index, and a host needs no schema and no decoder to read
  it. The build shows it each row's newest image under the
  row's lock, as it reads any row. An evaluator that fails
  fails the write with its error, and fails a build, which
  keeps the error as its reason. It fails by returning, never
  by throwing: it runs inside a write and on the build's
  workers, where what it threw would have nowhere to go, so
  one that throws ends the process. The C API's evaluator is
  `noexcept` in its type; sm3's own is a `std::function`,
  whose type cannot say so, and is called from a function
  that is. In the C API the host gives
  its evaluator to an environment with
  `sm_set_index_evaluator`, and fails from it with
  `sm_seterr`. `rel` has one of its own, for a predicate that
  is a column equal to a value and for `lower` and `upper` of
  a text column (`rel/REL_DESIGN.md` §4.3). What a call a row
  costs on the write path is unmeasured.
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

### 3.1 Read committed, worked through

The split above is two jobs that need different knowledge:

| | Needs | Whose |
| --- | --- | --- |
| (a) decide whether the row still qualifies, then write or skip | the statement's qual | above sm3: the executor's |
| (b) remove the index entries that are actually there | the newest old image | sm3's |

`rel/test/test_recheck.rel` runs every example below as
written, the script playing the executor. T2 is the
transaction that lost the race, at READ COMMITTED.

**Why sm1 cannot do (a).** sm1 forces the re-read and can do
no more: it does not know the statement's predicate
(`sm1/SM1_DESIGN.md` §5.4). Two workers claim one job, on a
table with no index:

```sql
-- row 7: state='free', owner=NULL
UPDATE jobs SET state='taken', owner=:me WHERE state='free';
```

| Step | What happens |
| --- | --- |
| 1 | both statements take a snapshot in which row 7 is `free` |
| 2 | T1 writes row 7; its version is uncommitted |
| 3 | T2's write waits for T1 |
| 4 | T1 commits; T2's write is refused with `SM_ERR_UPDATED` |
| 5 | T2 reads the latest image: `state='taken'` |
| 6 | T2 re-evaluates `state='free'` against it, false, and writes nothing |

Without step 6 the job is claimed twice and nothing raises an
error. The line is not drawn by the index: this table has
none.

**Why (b) is sm3's.** With an index on `state`, T2 holds two
old images of the row: the one its scan read at its snapshot,
and the one the repeated write replaces. Computing the entry
to remove from the first is the bug. T1 claims the job as
above, and T2 runs a statement whose filter the row still
passes:

```sql
-- row 7: state='free', index entry ('free', 7)
UPDATE jobs SET state='taken', owner='w1' WHERE state='free'; -- T1
UPDATE jobs SET state='done' WHERE state <> 'done';           -- T2
```

Off the snapshot image, T2 would remove `('free', 7)`, which
T1 already replaced with `('taken', 7)`, and insert `('done',
7)`. The heap then says `done` and the index holds two entries
for one row. A scan for `state='done'` is right and a scan for
`state='taken'` returns a row that is not taken. This is what
happens instead:

| Step | Layer | What happens |
| --- | --- | --- |
| 1 | executor | T2's scan reads row 7 at its snapshot: `(7, 'free', NULL)`. It builds the new tuple `(7, 'done', NULL)`. |
| 2 | sm1 | T1 updates row 7 and commits. The heap holds `(7, 'taken', 'w1')` and the index holds `('taken', 7)`. |
| 3 | executor | T2 calls `tbl_update(jobs, 7, (7, 'done', NULL))`. |
| 3a | sm3 | sm2's `rel_update` is refused with `SM_ERR_UPDATED`. sm3 has touched no index and returns the status. |
| 4 | executor | It calls `tbl_read_latest(jobs, 7)`, which marks the row's lock and returns `(7, 'taken', 'w1')`. No index is involved. |
| 5 | executor | It evaluates T2's qual against that image, whose `state` is now `'taken'`. `'taken' <> 'done'` is true, so the row still qualifies. It rebuilds the new tuple from the image: `(7, 'done', 'w1')`. |
| 6 | executor | It calls `tbl_update(jobs, 7, (7, 'done', 'w1'))`. Steps 6a to 6c run inside that one call. |
| 6a | sm3 | `rel_update` again. sm1 accepts it and hands back the old image `(7, 'taken', 'w1')`. |
| 6b | sm3 | The old image is pinned, so the sm2 calls that follow on the session leave it where it is. |
| 6c | sm3 | For each index it takes the old key from the image and the new key from the new values. For the index on `state`, `'taken'` and `'done'` differ, so `idx_delete('taken', 7)` then `idx_insert('done', 7)`. |
| 7 | — | The heap holds `(7, 'done', 'w1')` and the index holds `('done', 7)` only. |

Steps 6a to 6c are the code of an ordinary update, and step
3a is that code stopping before it reaches an index. Step 6c
hides one more exchange with sm1: the entry `('taken', 7)` is
an sm1 key that T1 committed after T2 began, so its erase is
refused until its newest version is read. An entry has no qual
to re-check, and sm2's `idx_delete` does that read and repeats
the erase itself (`sm2/SM2_DESIGN.md` §5).

If T2 is a second worker running the claim statement, step 5
evaluates `state='free'` against `(7, 'taken', 'w1')`, false.
No write is issued, no index call is made, and T2 reports zero
rows.

**A qual that spans two tables** is why (a) can never descend.

```sql
-- orders:    row 42 = (id=42, cust_id=5, status='new')
-- customers: row 5  = (id=5, region='EU')
--            row 9  = (id=9, region='US')
-- an index on orders(status) holds ('new', 42)
UPDATE orders o SET status='hold' FROM customers c
 WHERE o.cust_id = c.id AND c.region = 'EU';
```

Only `orders` is written; `customers` is read at T2's snapshot
and never locked.

- **The order row changes under T2.** T1 sets `cust_id=9` and
  commits. T2's `tbl_update` is refused with `SM_ERR_UPDATED`
  and it reads the latest image, `orders(42, 9, 'new')`. It
  re-runs the join for this one row against the customer row
  it had already joined, `customers(5, 'EU')`: `9 = 5` is
  false, and it skips the row. The qual needed the customer
  row, and a table write carries a row of `orders` and nothing
  else.
- **Only the customer changes.** T1 sets customer 5's region
  to `'US'` and commits. Order 42 was not touched, so T2's
  write succeeds the first time, on the strength of `'EU'` as
  its snapshot saw it. No re-check happens: only a conflict on
  the row being written triggers one.
- **The order changes and still qualifies.** T1 sets
  `status='shipped'` and commits. T2 is refused, reads
  `orders(42, 5, 'shipped')`, finds the join still holds, and
  writes again. sm3's part is the single-table one: the old
  image handed back is the `'shipped'` one, so it removes
  `('shipped', 42)` and inserts `('hold', 42)`.

In all three sm3 sees one row of `orders` and the image sm1
hands back. That the re-check uses the customer row already
joined, and not the newest, is `EvalPlanQual`'s treatment of
non-target tables, checked against PostgreSQL 18's source:
the planner gives a table the statement reads and does not
lock `ROW_MARK_REFERENCE` (`select_rowmark_type()`,
`optimizer/plan/planner.c`), and the re-check fetches its row
by the ctid the scan joined, with `SnapshotAny`
(`EvalPlanQualFetchRowMark()`, `executor/execMain.c`): that
version, whatever has committed since. digres's fetch by ctid
reads the row as the statement sees it, which at READ
COMMITTED is the version the scan joined
(`smtable_fetch_row_version()`), so it agrees, and
`../dg/test/server.sh` checks it with a customer row moved
meanwhile. One case differs: a table joined to itself, whose
copy has the target's rowno. A digres ctid names a row, not a
version, so the copy's re-check reads the newest version the
write kept, where PostgreSQL reads the one it joined
(`../dg/README.md`, known incompatibilities).

### 3.2 How it is tested

- **`test/test_table.cc`** checks every index against a
  brute-force scan of the heap after each scenario: plain
  maintenance at all three isolation levels; the row race at
  READ COMMITTED, both the write and the skip, and above it,
  where the loser gets `SM_ERR_CONFLICT`; races on a unique
  key; the build's batches, a writer and a deleter racing
  them from either side, a duplicate found late, the inline
  build, the guard for older snapshots, and the background
  build with writers running. Partial and expression indexes
  go through the same cases with the tests' own evaluator: a
  row is checked against what that evaluator says of it.
- **Two sessions on one thread.** A step that would wait is
  scripted with sm1's `set_qa_nowait`, which makes it fail
  with `SM_ERR_BLOCKED` to be repeated later
  (`sm1/SM1_DESIGN.md` §5.1). sm3 has no scenario driver of
  its own: sm1's links sm1 alone, and a level may not depend
  on one above it. Deadlocks, which a polling session cannot
  be the victim of, stay in sm1's threaded tests.
- **`rel`** drives the same paths through the C API
  (`rel/test/test_recheck.rel`, `test_blocking.rel`,
  `test_build.rel`, `test_partial.rel`).

---

### 3.3 An index's state, and why setting it takes no one's turn

**The states.** An index is *pending*, *valid* or *failed*.

| State | A write to the table | A scan |
| --- | --- | --- |
| pending | maintains the index; an entry to remove that is not there is no error | may not use it |
| valid | maintains the index; a missing entry is an error | may use it, if its snapshot is new enough |
| failed | leaves the index alone | may not use it |

*Building* is not a fourth: it is a pending index a worker is
filling at this moment, which the builder knows and nothing
records.

**Who sets it.** `create_index` leaves an index pending, or
valid if it filled it itself on a table its own transaction
made. The builder sets valid or failed when a build ends.
Dropping the index, or its table, erases the state with it.

**There is nothing to catch up.** Some designs build an index
beside the table, log the writes made meanwhile, and at the
end lock the table exclusively to apply the log and switch
the index in. There the exclusive lock does real work, and
the state changes under it. This build has no such phase.
Every writer has maintained the index since `create_index`
committed, and the backfill and a writer never work on one
row at once (step 3 above). So when the walk reaches the end
of the heap the index is whole, and marking it valid changes
one fact about it and nothing in it.

It used to take the table-oid lock X all the same, only
because every catalog change did. That made the last step of
every build wait for the longest transaction using the
table. It now takes IX, as each batch of the build does.

**Seen late, a state does no harm.** In this an index's state
is like a statistic and unlike a definition. A stale
definition is corruption: a row decoded by last week's
columns. A stale state is not, because a state only ever
leaves pending:

| The index is | A transaction still sees | What it does | Harm |
| --- | --- | --- | --- |
| valid | pending | maintains it, tolerates a missing entry, does not scan it | none: a plan it could have had |
| failed | pending | goes on maintaining it | none: entries no one reads |

Pending is the reading that is always safe, and it is the
only stale reading there is. So no transaction has to be
waited out before a state is set.

A table handle makes this concrete. It notes each index's
state when the table is opened and goes by that until it is
closed (`tbl_index_state`); a state set meanwhile reaches it
at its next open.

**The other direction is the count's.** One reader could be
harmed: one whose snapshot is older than the index's last
entries, which would find the index partly filled. That is
not a stale state but a state newer than the snapshot, and
no lock was ever what stopped it. The count of publishes
does (above): an index may be scanned only by a snapshot
taken at or after the publish that made it valid. Under X
that guard was needed only for a transaction that had not
yet opened the table; now it stands for every one, and it
needed no change to do so.

**Why the state is kept by index.** A table's definition is
immutable and is replaced whole, by a transaction holding the
table's lock X. A state is set under IX, while other
transactions hold the definition, so it cannot be a field of
it. And two builds may end on two indexes of one table at
once: were the state part of the table's record, the second
to commit would write the first one's old state back. So:

- on disk an index's state has an entry of its own, under the
  table's oid and the index's, apart from the index's entry
  (`CATALOG.md` §2). A pending index has none: no entry is
  what pending means, so `create_index` writes nothing for
  it;
- in memory it is in a map by index oid, beside the
  definitions and not in them (`CATALOG.md` §4).

**Why IX, and not no lock at all.** IX admits every reader
and writer of the table and keeps out only DDL, which takes
X. That is what stops the index, or its table, being dropped
while its state is written, and an entry being left behind
for an index that is gone.

**Memory and the store never differ.** Statistics are
published after their commit returns, and may lag the store;
a state is not allowed to. The transaction that sets it
holds the change in its session's overlay, where it sees it
at once, and `commit` publishes it under the catalog's mutex
with the rest of the transaction's DDL. No reader of the
catalog can see the one without the other. If the process
dies between the two, the next `cat_open` reads the store.

**What X bought, and what has it now.**

| X at the end gave | Now given by |
| --- | --- |
| no transaction held a stale state | a stale state is pending, which is safe |
| no old snapshot scanned the new index | the count of publishes, for every transaction |
| the index was not dropped meanwhile | IX against DDL's X |
| two state changes did not overwrite each other | a state per index, not per table |

**A state is set once.** The argument rests on a state only
leaving pending, so `cat_put_index_state` enforces it: an
index that is valid or failed is `SM_ERR_INVALID` to set
again, whatever to. Otherwise a change from valid to failed,
or back to pending, would be seen late as *valid*, which is
not the safe reading. Two transactions that set one pending
index at once meet on its state's key: the second waits for
the first, finds a state there when that commits, and is
refused. An index whose build failed is dropped and made
again; it is not reset.

**Tested by** `test_lock.cc`: with a reader and a writer
holding the table, marking an index valid returns at once;
two sessions setting two indexes of one table in
transactions open together both take effect; a handle opened
before still has the index pending; a REPEATABLE READ
transaction that opens the table again sees it valid and
still may not scan it; and of two transactions setting one
index's state the second waits and is refused. `test_ddl.cc`
has every refused transition.

### 3.4 Datums out of line, and the indexes

sm2 moves a row's long datums out of its tuple (TOAST,
`sm2/SM2_DESIGN.md` §4.2), and a tuple then holds a pointer
where a datum was. Built in `src/table/table.cc`, with
`test/test_toast.cc`.

- **The host turns it on** (`cat_set_toast`), once for a
  catalog, to say it reads pointers. Off, the default, a
  table's heap is opened with no sequence for TOAST ids, so
  sm2 moves nothing: a host that reads a datum straight out of
  a tuple is given no pointer. digres turns it on, and reads a
  pointer as PostgreSQL reads its own (`../dg/WORKNOTE.md`
  step 125).
- **A table has a sequence for its ids**, `sm3.toast.<oid>`,
  made with it and dropped with it, temporary when the table
  is. It is named by the oid and not kept in the table's
  record. Every table has one, so opening a table names it and
  reads nothing: sm2 draws from it only when it moves a datum.
- **An index is made of values, never of a pointer.** What an
  index needs of a row -- its key columns, its INCLUDE columns,
  and the columns its evaluator reads -- is fetched before the
  entry is made, by every write and by the build. So the
  evaluator is never shown a pointer, an index-only scan never
  gives one, and an entry is held to 64 KB whatever the row is:
  a row whose INCLUDE column makes its entry longer is refused,
  as PostgreSQL refuses an index row too long for a page. The
  other course for INCLUDE, keeping the pointer in the entry,
  would have had a reader of the index fetch through the
  table.
- **An update tells an unchanged column by its pointer.** The
  same pointer before and after is one datum, and nothing is
  fetched for it; a pointer on one side and anything else on
  the other is taken as changed, and both sides are then
  fetched and the entries made. Comparing the bytes under the
  varlena header, as for a datum in line, would have called a
  pointer equal to the empty text.
- **The old row's datums outlast the write.** The old entries
  are made from the image the heap write hands back (§3), and
  by then sm2 would have erased what the old row held out of
  line. So `tbl_update` and `tbl_delete` write with hold, fix
  the indexes, and then release (`sm2::rel_release`).
- **`ANALYZE` fetches nothing for a column.** A datum out of
  line counts as PostgreSQL counts one past 1 KB: toward the
  column's width, at the length its pointer gives, and not
  among the values sorted for the common values and the
  histogram (§6.5). A row shown to an expression index's
  evaluator has what the evaluator reads fetched, as in a
  write.
- **`tbl_fetch` reads a datum** for the host, by its pointer.

## 4. Object mapping

**Decision: one sm1 store per table.** A store is a
collection of **rels**, and a table's heap, each of its
secondary indexes and its TOAST data is one of them. They are
told apart by sm2's 2-byte **relid**
(`sm2/SM2_DESIGN.md` §5):

| relid | Relation |
| --- | --- |
| `0x0000` | unused |
| `0x0001` | the table's heap, always |
| `0x0002`–`0xFFFE` | its indexes, one relid each |
| `0xFFFF` | its TOAST data |

This table is the only place these values are written down;
every other document points here for them.

- **The heap is always `0x0001`.** It is the constant
  `sm3::heap_relid` and is never drawn from the counter that
  hands out index relids, so a table's heap sits at the same
  relid in every database and nothing about it is recorded.
- **`0x0000` is left unused**, so a zeroed or unset relid is
  never a live rel.

A relid names a rel within its store and nowhere else; a rel
is reached as its store and its relid, and there is no wider
identity for one. sm3 chooses an index's relid and records it
in the catalog; the heap's and TOAST's are fixed. An sm3 table
or index is one `rel` over the
table's sm1 store with its relid; sm2 prepends and strips it
and otherwise ignores it. sm2 never learns what a table is,
and sm3 never encodes a key itself.

- **`DROP TABLE` is one `sm1::rmstore`**, sm1's O(1) column
  family drop, and takes the indexes and TOAST data with it.
- **`TRUNCATE` swaps the store.** `truncate_table` makes a
  new, empty store, points the table's record at it and drops
  the old one with `sm1::rmstore`, under the table-oid lock X.
  All three are transactional, so an abort, or a rollback to
  a savepoint before it, brings the rows back, and it costs
  the same whatever the table holds. The columns, the index
  definitions and their relids and states, and the rowno
  sequence carry over, so new rows go on from the last rowno.
  A transaction that began before it committed and first
  reads the table after finds it empty, as in PostgreSQL:
  the catalog is not versioned (§2). On an open table it is
  `tbl_truncate`, which needs the table opened exclusive and
  reopens the handle on the new store.
- **`DROP INDEX` is a range delete** over the index's keys.
- **Index relids are allocated in increasing order and never
  reused.** A dropped index's versions linger until sm1's
  garbage collection passes them; never reusing a relid means
  no later index can read them as its own, with no rule tied
  to the GC horizon. 65,533 index creations per table,
  rebuilds included, is the ceiling.
- **The cost: tables are bounded by sm1's store count.**
  sm1 targets hundreds to low thousands of stores, and
  every table, each partition included, takes one. Accepted
  for v1; raising that ceiling is sm1's problem if it bites.

The alternative was many tables per sm1 store, with a wider
object id as the relid, which makes the relation count a
tuning decision. It was rejected for v1 because `DROP TABLE`
becomes a range delete instead of sm1's O(1) drop.

---

## 5. Scans

A scan takes key bounds and filters nothing. The heap is
scanned by a range of `rowno`, and an index between two bounds
on its leading columns (`sm2/SM2_DESIGN.md` §5); `tbl_scan` and
`tbl_idx_scan` are those, on an open table. sm3 has no
predicate, and so no predicate scan.

- **Deriving the bounds from a `WHERE` clause is the
  planner's**, above: the columns of the index, in order,
  matched against the predicate's conjuncts, giving a start
  key, an end key, and whatever is left over as a residual.
  The kept planner does this already.
- **The residual is evaluated above too.** sm3 evaluates
  nothing (§3), so a filter pushed down would be a call back
  into the host for every row, and the rows that fail would
  cross the API all the same. The host filters what the scan
  returns.
- **A filter on one column of a wide row does not decode the
  row.** sm2 decodes a tuple per column
  (`sm2/SM2_DESIGN.md` §5), and the C API hands that through.
- **Tuple at a time.** Sets of rows in and out was forced by an
  IPC boundary that the threaded fork removes (§1), and its
  other reason was filter pushdown, which is gone. Fewer calls
  per scan is what is left of the case for batches, and it
  waits for a profile that asks, as sm1's batched read does
  (`sm1/SM1_DESIGN.md` §10.1).
- **Forward only**, at every level (§1). An `ORDER BY x DESC`
  that an index could have served backwards is a top-N sort
  over a forward scan instead (§7).

---

## 6. Statistics

The planner is kept, so statistics are not optional. The
strategy is `STATISTICS.md`, at the root of the stack, and
its measurements are done (§6 there). This section is the
design it asked for (§8 there, step 2): the choices it left
open, made here, and the format written out. Built: the
order of work is §6.9, and every step is done. The host's
part, the planner's use of these statistics, is built in
digres (`../../dg/STATISTICS.md`).

In brief: `tbl_analyze` samples a table by runs of rownos,
computes PostgreSQL's statistics from the sample with
PostgreSQL's estimators, and writes them to one catalog entry
of a new kind. A map beside the catalog holds them in memory
for the planner. Sizes, the rowno limit and the LSM's shape
are answered live and are not stored.

### 6.1 The stack analyzes

`STATISTICS.md` §7.1 is decided: **sm3 samples and estimates,
and the fork does not implement the table AM's analyze
callbacks** (`scan_analyze_next_block`,
`scan_analyze_next_tuple`).

- **Half the callback route is replaced anyway.** Its results
  go to `update_attstats`, which writes `pg_statistic`, which
  the fork does not have. The storage for statistics, §6.6, is
  needed on either route.
- **The sample's shape stays the stack's.** The run length and
  the sparse-table rule of §6.4 are what the measurements
  turned on, and through the callbacks they would be decided
  by PostgreSQL's block arithmetic, against
  `STATISTICS.md` principle 2.
- **sm3 is tested without a host** (principle 3). Every
  estimate is checked here against a full scan of generated
  data; on the callback route nothing could be until the fork
  links.
- **The cost is a port.** `compute_scalar_stats`,
  `compute_distinct_stats` and `compute_trivial_stats`, with
  the helpers they call, are copied and adapted into sm3
  (§6.5), and an upstream fix to them is ported by hand, as
  every upstream fix is (`PART4.md` §10).

### 6.2 Sizes, and the unflushed rows

**Decision: a size is two numbers, never one.** sm1 answers,
for a key range of a store:

| Part | What it is | From RocksDB |
| --- | --- | --- |
| `disk` | bytes in sorted files, compressed | `GetApproximateSizes`, files only |
| `mem` | bytes in the memtables, uncompressed | `GetApproximateMemTableStats` |

sm2 asks it of a relid's range, and sm3 of the heap and of
each index. Neither part is corrected, and the stack does not
add them.

- **Why not the sum.** It is what `STATISTICS.md` §6
  measured at 7.2x the flushed size for a table not yet
  flushed, and 6.6x mid-burst. `disk` alone answers 0 for the
  same table. Each part is true of what it measures, and only
  the sum is wrong, so the stack reports the parts (principle
  2) and the host decides what a memtable byte is worth.
- **What the host makes of them** (`STATISTICS.md` §3.7):
  bytes for the planner are `disk + mem × c`, where `c` is
  the table's compression as last recorded, its heap `disk`
  over live rows times average row bytes (§6.6), at most 1,
  and 1 when nothing was on disk at the last analyze. A table
  never flushed is then charged its uncompressed size, which
  is what it occupies, and a flushed one is charged its rows
  in memory at its own compression.
- **`mem` is an order of magnitude.** RocksDB samples its
  memtables. Over a whole store it was measured from 0.4 to
  1.05 times the bytes, but for one rel among others, however
  large, from 0 to 4 times (`sm1/API.md`). Accepted, not
  corrected: `mem` is bounded by the store's memtables, 64 MB
  each by default, so for a large table it is a small share of
  the total and its error moves little; it dominates only for
  a table that is mostly unflushed, which is small and cheap
  whichever plan is chosen (`STATISTICS.md` §5). If it bites,
  an exact count by reading the memtables alone belongs in
  `tbl_analyze`, not at plan time.
- **The rejected alternative** was to flush before measuring.
  It would make the number exact, and costs a flush per plan,
  or per analyze for a number that is stale by the next plan.

### 6.3 The rowno limit and the LSM's shape

- **`N`, the rowno limit**, is the last value the table's
  rowno sequence `sm3.rowno.<oid>` handed out, read without
  drawing one: a new sm1 call beside `nextseq`, 0 for a table
  that never had a row. Rownos start at 1.
  A value reserved in a block and lost to a crash, or drawn by
  a transaction that aborted, is a rowno with no row. That
  costs the row count nothing: §6.4 measures the live fraction
  over the same `1..N` it multiplies, gaps included.
- **Probes**, the LSM's shape, is the number of sorted runs a
  point lookup of the store may visit: its memtables, each
  level-0 file, and each non-empty level below. It is the
  store's, shared by the heap and the indexes, from RocksDB's
  `num-files-at-level<n>` and memtable properties. It is what
  `STATISTICS.md` §3.5 calls the level count, counted the way
  a lookup pays for it: level 0's files overlap and each is
  probed, a lower level is one probe.

All three, sizes, `N` and probes, are answered live on an open
table and never stored, so they need no analyze.

### 6.4 The sample

`tbl_analyze(table, target)` wants `300 × target` rows,
30,000 at PostgreSQL's default of 100. It reads them through
one forward cursor on the heap, in the caller's transaction,
at its snapshot.

1. **A pilot** of 300 single rownos drawn uniformly from
   `1..N`, through a cursor of its own. It gives a live
   fraction `f0` and an average row bytes `w0`. It costs some
   4 ms on ten million rownos (`STATISTICS.md` §6,
   experiment 4), and is not part of the sample.
2. **The run length** is `B = max(1, 8192 / w0)`: the rows of
   an 8 KB page at that width. That matches the clustering of
   PostgreSQL's own sampler, which reads whole blocks
   (`STATISTICS.md` §3.3), so a distinct count here is biased
   as PostgreSQL's is and no more. `w0` is the stored row as
   sm2 encodes it, uncompressed, as a heap page holds it.
3. **The run count** is `k = ⌈1.2 × 300 × target / (B ×
   f0)⌉`. Dividing by `f0` is the sparse-table rule
   `STATISTICS.md` §6 asked for: a table with nine rownos in
   ten dead gets ten times the runs, where a fixed count
   returned a tenth of the rows wanted. The 1.2 is margin for
   the pilot's error. A pilot that found nothing live counts
   as `f0 = 1 / 301`, with `B = 1`.
4. **The budget.** The runs cover at most `10 × 300 × target`
   rownos, 300,000 at the default target, whatever the
   table's size: `k` is capped at that over `B`. **Nothing
   reads the table whole**: a table emptied by deletes, or one
   so sparse that the rule above asks for more, gives fewer
   rows than wanted, and the estimators take any sample size.
   Only a table no larger than the budget is read whole, by
   the runs taking every slot, even when fewer runs would
   give the rows wanted: the reservoir then draws from every
   row, so a column that follows the rownos is not estimated
   from a few runs, as PostgreSQL reads a table under its
   sample whole. Its row count is then exact.
5. **The runs.** `1..N` is cut into `⌈N / B⌉` aligned slots of
   `B` rownos, and `k` of them are drawn without replacement
   by **Floyd's algorithm**, which draws `k` numbers and looks
   at no other slot, so the work is `O(k log k)` whatever `N`
   is. They are read in ascending order through one forward
   cursor, moved to each slot's first rowno with `scan_seek`;
   a row the cursor returns past a slot is held for the next,
   so every seek is forward (principle 6). The pilot's rownos
   are drawn the same way. If the runs come up short of the
   rows wanted, more slots are drawn among those not read, as
   many as the live fraction found so far says, while the
   budget lasts, each round through a cursor of its own.
6. **The row count** is the live rows found over the rownos
   the runs covered, times `N`.
7. **A reservoir** keeps a uniform `300 × target` of the rows
   found, each with its rowno, by Algorithm R: every row in a
   run is read anyway, so Vitter's algorithm Z, which saves
   random draws by skipping rows, would save nothing. The
   kept rows are sorted back into rowno order for the
   correlation of §6.5.

**The rows read are the caller's snapshot.** The cursor sees
what the caller's transaction sees, its own writes before
`tbl_analyze` among them, so a table loaded and analyzed in
one transaction is analyzed loaded. The pilot's cursor
too.

**Under SERIALIZABLE the runs leave SIREADs**, as any scan
does (`sm1/SM1_DESIGN.md` §7.1), and a writer into a run
makes an rw-edge to the analyze. PostgreSQL's `ANALYZE` takes
no predicate locks. Accepted for now: an analyze only the
host launches, and the host can run it at READ COMMITTED. If
an analyze inside a serializable transaction is wanted, the
fix is a cursor that records no SIREAD, a new sm1 flag,
justified because statistics are approximate (principle 4).

### 6.5 The estimators

**Ported from PostgreSQL 18.6's `analyze.c`**, not
reinvented, so the numbers mean what the selectivity code
assumes (`STATISTICS.md` §3.4). For each column, and for each
expression of an expression index, the routine is
`compute_scalar_stats`, which gives all six: null fraction,
width, distinct count, most common values, histogram and
correlation.

It is the only one ported. Every type sm2 has today has an
order, a key encoding, so `compute_distinct_stats`, for
equality without order, and `compute_trivial_stats`, for
neither, have no case yet; each comes with the first type that
needs it. The analyze routines for arrays, ranges and
tsvectors are not ported either, and the cost is
`STATISTICS.md` §3.4's.

**Equality and order are the column's key encoding**,
compared with `memcmp`. This corrects `STATISTICS.md` §3.4,
which says sm2's `cmp` under the column's collation: `cmp`
takes no collation. `key_encode` does, and its bytes order as
the type does under it and are equal exactly where PostgreSQL
calls the values equal, a non-deterministic collation
included (`sm2/TYPE.md` §3). So each sampled value is encoded
once, with the column's collation, and the port sorts and
groups the keys. A value kept for the catalog, a common value
or a histogram bound, is the datum, not its key, since a key
under a non-deterministic collation keeps no datum; within an
equal group the first datum seen is kept.

**What is carried over unchanged**: the distinct estimator,
Haas and Stokes' `Duj1`, and the rule that stores a distinct
count as a negated multiplier of total rows above 10% of
them; which values are common enough to list; histogram
bounds over the rest, target + 1 of them; correlation of each
column's order with the sample's order, which §6.4 made
rowno order; and `WIDTH_THRESHOLD`, a value over 1 KB counted
as distinct and kept out of the lists. A column's own
statistics target is not here: one target per call.

**Expressions and predicates.** For an index with an eval,
each sampled row is passed to the catalog's evaluator
(`index_eval_fn`), on the analyzing thread. It gives whether
the row belongs to the index and each expression's value. An
expression's values get the six statistics; for a partial
index, the expressions are over the rows that belong, and the
fraction of sampled rows that belong is recorded beside them.
An index's entries are then estimated, not counted:
live rows times that fraction.

### 6.6 What is recorded

**Catalog entries of kind `STATS`, value 10: one for the
table, and one for each statistic**, outside the `TABLE` entry
and its DDL (`STATISTICS.md` §3.6). The key after the kind is
(table oid, index oid, n): (t, 0, 0) is the table's own entry,
(t, 0, attnum + 1) a column's, and (t, index oid, place + 1) an
index expression's, so a table's entries are together, its own
first. Each value is parts, each an sm2 tuple under a fixed
schema with its length as a varint, as every catalog value is
(`CATALOG.md` §3):

```
the table's entry     = table | index × nindex
a statistic's entry   = statistic | mcv × nmcv | bound × nbound
```

- **Why an entry a statistic, not one a table.** An sm1 value
  is at most 64 MB (`sm1::max_datalen`). A statistic keeps up
  to `target` common values and `target + 1` bounds of up to
  1 KB each, some 200 KB a column at the default target and
  20 MB at the largest, 10,000: one entry a table would not
  hold a wide table, nor three columns at that target, and its
  analyze would fail. An entry a statistic holds the largest
  with room to spare, and lets one column be analyzed alone
  later.

| Part | Fields |
| --- | --- |
| table | format `int4`, target `int4`, run length `int4`, rows sampled `int8`, rowno limit `int8`, live rows `float8`, heap disk `int8`, heap mem `int8`, average row bytes `int4`, nindex `int4`, nstat `int4`, the statistics recorded |
| index | index oid `int8`, disk `int8`, mem `int8`, entries `float8`, fraction that belongs `float4` |
| statistic | index oid `int8` or null, position `int4`, null fraction `float4`, width `int4`, distinct `float4`, correlation `float4` or null, nmcv `int4`, nbound `int4` |
| mcv | value, frequency `float4` |
| bound | value |

- **A statistic is a column's or an expression's.** A null
  index oid makes `position` the column's attnum; an index oid
  makes it the expression's place among that index's key
  columns. The two share one schema because they carry the
  same six things.
- **A value has the type of what it describes**, the column's
  or the expression's, in sm2's encoding of it, so the tuple
  schema of an `mcv` or a `bound` part is built from the
  definition at read time. A value is read by the definition
  it was written under; that holds because a column's type
  never changes (sm3 adds and drops columns, nothing else), and
  an index's expressions are fixed when it is made.
- **Sizes are kept as recorded**, `disk` and `mem` apart, so
  the host has the compression of §6.2 and the growth since
  (`STATISTICS.md` §3.2) from one entry.
- **The format field** starts at 1. The kind's value is on
  disk and frozen with the rest; the parts' schemas are
  versioned by that field.

The format is declared in `include/sm3.h`, as `stats_t`, and
sm3 reads and writes it; unlike an index's eval, it is no blob
to sm3.

### 6.7 Writing, reading, and what DDL does to them

**Written in the caller's transaction**, so they commit, roll
back and survive a crash with it, with no undo of sm3's own.

- **Locks.** `tbl_analyze` runs on a table opened IS, the
  table-oid lock of a reader (§2). It blocks no reader, no
  writer and no index build, and DDL waits for it, so the
  definition does not change while it samples. Before
  sampling it takes `catkv_lock` X on its `STATS` key, so two
  analyzes of one table queue rather than both sampling and
  one losing at commit; at REPEATABLE READ and above the
  second then fails on the first's newer version, which is
  retryable, and the host's analyze runs at READ COMMITTED.
- **Published after commit.** The map from table oid to
  `std::shared_ptr<const stats_t>` (`STATISTICS.md` §3.6) is
  swapped when the commit returns, by the session that
  committed. Until then a planner reads the old ones, which
  is a stale read and allowed. A crash between the two loses
  nothing: the map is rebuilt from store 0.
- **Read lazily, never through sm1 at plan time.** The first
  plan to want a table's statistics loads its entry; later
  ones read the map. No SIREAD and no lock, for the reasons
  of `STATISTICS.md` §3.6.
- **A table never analyzed** has no entry. The host plans it
  as PostgreSQL plans `reltuples` -1.

**What DDL does**, with the rule that a stale statistic is a
worse plan and never a wrong answer:

| DDL | Its statistics |
| --- | --- |
| `DROP TABLE` | its `STATS` entry is deleted with the table's others, and its map entry dropped after commit |
| `DROP INDEX`, `DROP COLUMN` | the index's expressions' entries, or the column's, are erased with it; the table's own entry is left, and a reader skips its part for an index gone |
| `ADD COLUMN`, `CREATE INDEX` | none until the next analyze; the planner's defaults meanwhile |
| `TRUNCATE` | left, as PostgreSQL leaves `pg_statistic` |

Dropping a column or an index erases only that one's entries,
never the table's own, which `tbl_analyze` locks: DDL holds the
table-oid lock X, so no analyze is writing meanwhile. An
analyze writes every entry the definition allows, or erases it
if it has no statistic for it, reading each as of the newest
commit under its lock, so it needs no scan; `DROP TABLE` erases
them the same way. An attnum is never reused, nor an index oid
(`CATALOG.md` §1), so an index part left behind is never read
as another's.

**After a `TRUNCATE`**, `STATISTICS.md` §3.6 asks for a choice
of floor on rows. **Decision: a live heap of 0 bytes, `disk`
and `mem` both, whose sample reads no row, is never
analyzed.** The host plans it as PostgreSQL plans a truncated
table, `reltuples` -1 and its ten-page guess, not as empty.
Only a new store is 0 bytes: a table emptied by `DELETE`
keeps its tombstones until compaction, and is planned from
its statistics. The sizes are RocksDB's estimates, which may
read 0 for a heap with rows, mid-flush, so the sample has the
last word: rows read are recorded, whatever the bytes. This
needs nothing of the stack, since §6.2 already answers the
size, and nothing of the truncate.

### 6.8 The calls

| Level | Call | Gives |
| --- | --- | --- |
| sm1 | `approxsize(store, lo, hi, disk, mem)` | §6.2's two parts for a key range |
| sm1 | `lastseq(name, out)` | a sequence's last value, drawing none |
| sm1 | `probes(store, out)` | §6.3's probe count |
| sm2 | `relid_size(session, store, relid, disk, mem)`, the same by a rel's relid | §6.2 |
| sm3 | `tbl_size`, `tbl_idx_size`, `tbl_rowno_limit`, `tbl_probes` | live, on an open table |
| sm3 | `tbl_analyze(table, target)` | §6.4 to §6.7 |
| sm3 | `cat_put_stats(session, table oid, stats)` | records a `stats_t` in the session's transaction, published at its commit |
| sm3 | `cat_get_stats(session, table oid, out)` | the session's own, else the published `shared_ptr<const stats_t>`, loading it on first use in a READ COMMITTED transaction of the catalog's own; null if never analyzed |
| top | `sm_tbl_analyze`, `sm_tbl_stats`, and the four live calls | the same, in `include/sm.h`; `sm_tbl_stats` hands out `cat_get_stats`'s object itself, `stats_t` being the root's `sm_stats_t` (`include/sm_stats.h`) |
| `rel` | `analyze`, and a verb that prints a table's statistics | testing |

The calls of sm1, sm2 and the live four of sm3 are built
(§6.9, step 1), `cat_put_stats` and `cat_get_stats` (step
2), `tbl_analyze` (steps 4 and 5), and the top level's and
`rel`'s (step 5).
A statistic's `position` is a column's attnum 0-based, as
sm3's positions are everywhere.

### 6.9 Order of work

`STATISTICS.md` §8, steps 3 to 7, with what this section
settled:

1. ~~The sm1 calls of §6.8, and sm2's and sm3's over
   them.~~ **Built.**
   → verify: `disk` and `mem` follow a load before and after
   a flush; `N` is the sequence's last value; probes rise with
   level-0 files and fall after compaction.
2. ~~Kind 10, its format, and the map.~~ **Built.** The map
   is the catalog's, published at commit as DDL is; a table
   never analyzed is remembered as such once asked about.
   → verify: statistics survive a restart, an aborted analyze
   leaves the old ones, a planner holding one is unaffected by
   the next publish, and an entry is loaded on first use and
   not at startup.
3. ~~The pilot, the slots and the reservoir.~~ **Built**, in
   `src/analyze/sample.cc`, with the budget of §6.4.
   → verify: the row count against a full count, dense and
   sparse; a small table read whole by its runs; an emptied
   table read no further than the budget.
4. ~~The estimators, ported one routine at a time, tests
   first.~~ **Built**: `compute_scalar_stats` and
   `analyze_mcv_list` in `src/analyze/estimate.cc`, and
   `tbl_analyze` over the sample in `src/analyze/analyze.cc`.
   Sorting by key, then by place in the sample, puts equal
   values together, so PostgreSQL's `tupnoLink` is not needed.
   → verify: each statistic against a full scan of uniform,
   skewed, clustered and mostly-null data; text under a
   non-deterministic collation groups as it compares.
5. ~~Expressions, predicates, and the calls in
   `include/sm.h` and `rel`.~~ **Built**: `sm_tbl_analyze`,
   `sm_tbl_stats`, the four live calls and
   `sm_datum_of_bytes`; `rel`'s `analyze` and `stats`.
   → verify: a `rel` script analyzes a table with a partial
   and an expression index and prints what it expects.

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

---

## 8. Build order and testing

The order of work, and why it is this order; it was `PLAN.md`
until this section took it in. It starts where sm2's build
order (`sm2/SM2_DESIGN.md` §8) stops: sm2's types, encodings
and tuple relation exist before anything here does.

The principle: **the formats were sm2's problem; the
concurrency is this one's.** Almost nothing sm3 writes is
permanent on disk — the catalog's own layout is the exception
— so what gets the scrutiny is the protocols, and the tests
that matter most are the ones with two sessions in them.

### 8.1 The steps

| Step | What | State | Where |
| --- | --- | --- | --- |
| 1 | Catalog | built | §2, `CATALOG.md` |
| 2 | Object mapping | built | §4 |
| 3 | Indexes | built | §3 |
| 4 | Scans | settled, nothing to build | §5 |
| 5 | Statistics, constraints, bulk load | statistics built, constraints settled, bulk load in part | §6, `../BULKLOAD.md`, `NEXT.md` |

1. **Catalog** comes first because everything else needs a
   definition to work from. Tables, indexes, columns, types,
   collations, and each column's dropped mark and missing
   value, in memory, written through to sm2 and loaded at
   startup, under the table-oid locks of §2. A DDL change
   stays private until it commits in a session's overlay
   (`CATALOG.md` §4).
   Verified: DDL rolls back with its transaction, the
   in-memory copy included; a transaction holding the shared
   table-oid lock never sees a definition change under it; two
   sessions doing concurrent DDL behave as sm1's isolation
   says they must.
2. **Object mapping** is inside the catalog's DDL
   (`src/catalog/ddl.cc`).
   Verified: a table's rows and indexes share its store
   without meeting; `DROP TABLE` removes the store, indexes
   and all; a dropped index's relid is never handed out
   again, so its lingering versions are never read as another
   index's.
3. **Indexes**: maintenance through the table-level writes,
   the READ COMMITTED fix-up, the concurrent build, and
   partial and expression indexes by the host's callback.
   Verified: the index and the base relation agree with a
   brute-force scan, under concurrency, at every isolation
   level; the build does not miss a row written while it runs
   (§3.2).
4. **Scans** were to take a predicate, derive a key range and
   evaluate the residual below the API. Deriving the range is
   the kept planner's, and the residual is the executor's,
   since sm3 has no evaluator and gets none. A scan takes key
   bounds and filters nothing.
5. **Constraints** are settled: `NOT NULL` is built (§3), and
   the `varchar(n)` limit and foreign keys are the host's,
   with `tbl_lock` as the stack's share of a foreign key
   (`DESIGN.md` §5). **Statistics** is built (§6).
   **Bulk load** is built for files into an empty table
   (`../BULKLOAD.md`); the rest of it is not. `NEXT.md` has
   what is left and in what order.

### 8.2 Testing

sm1's two-layer arrangement is kept: C++ tests for contract
and single-session mechanics, and something declarative for
anything where two sessions interleave. sm3's concurrency
questions — an index build against a concurrent writer, DDL
against DML — are the second kind.

sm3 did not get a scenario driver of its own, nor extend
sm1's, which links sm1 alone. sm1's QA-only `set_qa_nowait`
lets one thread play several sessions: a step that would wait
fails with `SM_ERR_BLOCKED` and is repeated later. sm3's
two-session cases are written that way in `test/test_table.cc`
and as `rel` scripts; what needs real threads, deadlocks and
the background build, has them (§3.2, `DESIGN.md` §4).

### 8.3 What is deliberately not here

- No types, no key encoding, no tuple format. All of it is
  sm2's (`sm2/SM2_DESIGN.md` §8), and sm3 calls it rather than
  repeating it.
- No SQL, no planner, no executor.
- No PostgreSQL dependency. `PART4.md` explains what the PG
  target forces on the decisions above; none of it requires a
  PostgreSQL source tree to build or test sm3.
