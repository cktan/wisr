# NEXT — what to build next, and in what order

This file holds only what is still to do, across levels. What
is built or settled is in the design documents: `DESIGN.md`
§3 has where each level stands, and `sm3/SM3_DESIGN.md` §8
has sm3's own order of work.

---

## 1. Next

1. **A table clustered on its primary key** (`CLUSTER.md`).
   The heap keyed by `rowno` goes; a row is keyed by its
   primary key, or by `_id` when it has none, and `_id` keeps
   the executor's TID. In the steps of `CLUSTER.md` §11: the
   first, sm's, is built; next, digres reads by key.

The spike `PART4.md` §7 aimed the stack at is long past:
digres runs on it (`PART4.md` §10). Statistics for a planner
are built, the stack's part and digres's
(`../dg/STATISTICS.md`, `../dg/WORKNOTE.md` steps 74 to 78),
and so are the catalog tables psql reads
(`../dg/SYSCATALOG.md` §6).

Low priority:

1. **Bulk load, the rest of it.** File mode into an empty
   table is built, and digres's `COPY` uses it (`BULKLOAD.md`
   §13). Not built: `INSERT ... SELECT` into an empty table,
   which digres inserts a row at a time
   (`smtable_tuple_insert()` in
   `../dg/src/backend/access/sm/smtable.c`); a load from
   files into a table that has rows, which inserts a row at a
   time today; rows stored out of line, which end a load's
   files; a `rel` verb; and batch mode and mini-batch, which
   matter less now that no write keeps a row lock.
2. **digres's `xact.c`, until it is empty.** sm keeps the
   transaction; `xact.c` keeps only what a session needs
   around it (`../dg/PLAN.md` §7), and what it no longer needs
   is deleted as it is found. Nothing is moved out of it just
   so the file goes (`../dg/WORKNOTE.md` steps 232 to 236).
   It is 2,874 lines.

---

## 2. Loose ends

Owed, and blocking nothing above:

- **An upsert against a held row fails now and then.** dg's
  `test/server.sh` failed 2 runs in 35 with sm built `-O0 -g`
  (`DEBUG=1`), each time at a different check where one
  session holds a row and another upserts its key: "upsert of
  a row updated meanwhile", "upsert of a row deleted once
  found". Traced, the holder held its row within 0.13 s and
  the upsert was seen waiting within 0.2 s, far inside the
  test's 5 s and 10 s, so a timeout is unlikely; the upsert
  may at times not wait at all, a race in sm1. Not caught in
  a failing run, and not yet known to predate the fixes it
  was seen after. Next: repeat those four checks many times
  in one server's life, and read both sessions' output when
  one fails.

- **The commit cache's size** (`sm1/COMMITCACHE.md` §7).
  65,536 entries reaches back under a second at 100,000
  transactions a second, while a version needs a lookup until
  compaction stamps it. Size the cache by memory with a larger
  default, 16 MB for a million slots; and measure how
  long stamping takes under load, which is what the size
  should be set from.

- **A parent's other columns wait for its children.** sm1 has
  no pair of modes like PostgreSQL's `FOR KEY SHARE` and `FOR
  NO KEY UPDATE`, so an update of a parent row that leaves its
  key alone still waits for a child's transaction, which holds
  the row shared.
- **`lock_timeout`, and the rest of no-wait.** A lock that
  does not wait is built for a row (`sm1::trylock`,
  `sm_tbl_trylock`) and for a table's table-oid lock
  (`sm_tbl_tryopen`), and digres's NOWAIT, SKIP LOCKED and
  `LOCK TABLE ... NOWAIT` use them, and `lock_timeout` ends a
  wait in sm by a timer's cancel (`../dg/WORKNOTE.md` step
  168), counted from the call, as sm does not say when a call
  begins to wait. Not built: a write that does not wait, which
  nothing in SQL asks for. sm1's `set_qa_nowait` is still for
  QA only (`sm1/SM1_DESIGN.md` §5.1).
- **The evaluator's cost is unmeasured**: what a call a row
  adds to a write on a table with a partial or an expression
  index.
- **The index build is unmeasured.** Four workers is a guess:
  builds contend on sm1's lock table, one mutex, and on
  RocksDB's write path. Two indexes built on one table walk
  its heap twice where one pass could fill both.
- **Nothing reads a range at its newest version.** An
  over-long key in a unique index is refused instead
  (`SM3_DESIGN.md` §7). The primitive is built when a workload
  asks.

- **A point read pins its row** (`PIN.md`, `ROW.md`).
  `sm1::read` and `sm1::read_latest` read into a raw pin, and
  the lent forms into one the session keeps, so neither
  copies the row; sm3 keeps the pool of pins its clients hold
  refs to (`ROW.md` §4). digres's index, index-only and
  bitmap heap scans hold a pin ref each and copy nothing
  (`../dg/WORKNOTE.md` step 263), and so is the old row of an
  update or an erase, at every level (step 266).

- **A row is deformed as far as it is read** (`ROW.md`). A
  sequential, an index, a bitmap heap and a sample scan hand
  digres's slot a pin ref (`TTSOpsSmRow`), which the
  executor's fetch steps deform column by column, checking
  only what they read (`../dg/WORKNOTE.md` step 276), and an
  index-only scan whose key does not give its columns back
  decodes the index's columns alone (step 279). Not yet:
  - *A TID scan and a DELETE's RETURNING* read a lent copy by
    tid, their first columns up to the last the plan names
    (`smstore_scan_natts()`, `smstore_expr_natts()`); they
    would read into a pin of their own.

- **Views, the rest of them** (`VIEW.md` §10). CREATE, OR
  REPLACE, DROP and the cascades are built, sm3's and digres's
  (`../dg/WORKNOTE.md` step 238), and of SQLite's sample digres
  now fails nothing PostgreSQL 18 passes; psql's `\dv` and
  `\d+` and `pg_get_viewdef()` read them (step 239), and
  ALTER VIEW renames one and sets its options, `WITH CHECK
  OPTION` among them (step 240), and a temporary view is
  a session's (`VIEW.md` §12, step 245). Not built: ALTER
  VIEW's `OWNER TO`, `SET SCHEMA` and a column's DEFAULT; and
  `information_schema` (`../dg/SYSCATALOG.md` §7).
- **What foreign keys left** (`FOREIGNKEY.md`,
  `../dg/WORKNOTE.md` step 177): a DEFERRABLE one is refused,
  as a row trigger on a table in sm is given its rows from a
  store that lasts only the statement.
- **What PostgreSQL's isolation specs found**
  (`../dg/test/isolation_specs.txt`, `../dg/WORKNOTE.md` step
  169), beyond the key-share modes above: sm1
  has no wait queue to reorder, so a soft deadlock PostgreSQL
  untangles ends a transaction (`deadlock-soft`).
- **A bulk load's duplicate at its close is sm's.** A unique
  build and every write say PostgreSQL's key now
  (`../dg/WORKNOTE.md` steps 260 and 261). A COPY in file mode
  finds a duplicate when the load closes, in sm1's sort, which
  knows a stream, not the index, and has no row to read: it
  says `key already exists`.

---

## 3. Not blocking

- **Edges from digres in sm1's wait-for graph.** digres keeps
  PostgreSQL's advisory locks (`pg_advisory_lock()`,
  `pg_advisory_xact_lock()` and the rest) in a table of its
  own, `../dg/src/backend/storage/lmgr/advisory_lock.c`, with
  a deadlock detector for cycles among them (`../dg/PLAN.md`
  §7). A cycle through an advisory lock and an sm1 lock is
  seen by neither, and waits until `lock_timeout` or a cancel. sm1 could take
  an edge from its caller -- "session A waits for session B",
  set while A waits on the advisory lock and cleared after --
  and follow it in `find_cycle()` as its own, so the one
  detector finds every cycle.
- **A snapshot handed to another session.**
  `pg_export_snapshot()` and `SET TRANSACTION SNAPSHOT` share
  PostgreSQL's snapshot, which is not what decides what a
  session sees in digres, so digres refuses both
  (`../dg/WORKNOTE.md` step 149). `pg_dump -j` needs them: a
  session would take an sm1 snapshot another has, at its
  timestamp, for a transaction of its own.
- **What temporary tables left for later** (`TEMPTABLE.md`
  §9). They are built, in digres too. Not built: skipping the
  sync of a commit that wrote only temporary stores,
  statistics of a temporary table, `CREATE TEMPORARY
  SEQUENCE`, and a bulk load into one.
- **Dependency checks**, such as refusing `DROP COLLATION` in
  use and `DROP SCHEMA … CASCADE`: low priority. They wait
  for a dependency index (`sm3/CATALOG_INMEM.md` §4), and
  nothing is waiting for them.
- **The unmeasured startup cost for thousands of tables** —
  open in `sm3/CATALOG_INMEM.md` §4.
- **A table in a namespace other than `public`, from `rel`.**
  `rel` creates and drops namespaces but has no way to name
  one for a table.
- **Savepoints without an undo list** (`sm1/SAVEPOINT.md`):
  a rollback would mark the `seq` range it undid as dead and
  delete nothing. Designed, and not to be built for now: we
  lean towards the capped undo list, for the reasons at the
  top of that file.

---

## 4. What this file is not

Not a replacement for `sm3/SM3_DESIGN.md` §8, which holds
sm3's order of work and the reasoning behind it, nor for the
design documents, where a protocol is argued before it is
built and recorded once it is. A decision settled here moves
into one of those and leaves this file.
