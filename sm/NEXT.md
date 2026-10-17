# NEXT — what to build next, and in what order

This file holds only what is still to do, across levels. What
is built or settled is in the design documents: `DESIGN.md`
§3 has where each level stands, and `sm3/SM3_DESIGN.md` §8
has sm3's own order of work.

---

## 1. Next

Nothing is queued above low priority. The spike `PART4.md` §7
aimed the stack at is long past: digres runs on it
(`PART4.md` §10). Statistics for a planner are built, the
stack's part and digres's (`../dg/STATISTICS.md`,
`../dg/WORKNOTE.md` steps 74 to 78), and so are the catalog
tables psql reads (`../dg/SYSCATALOG.md` §6).

Low priority:

1. **Bulk load, the rest of it.** File mode into an empty
   table is built, and digres's `COPY` uses it (`BULKLOAD.md`
   §13). Not built: a load from files into a table that has
   rows, which inserts a row at a time today; rows stored out
   of line, which end a load's files; a `rel` verb; and batch
   mode and mini-batch, which matter less now that no write
   keeps a row lock.
2. **The public headers to `CODE.md` §2.**
   `sm_column_def_t::default_expr` is a
   `std::optional<std::string>` (`include/sm.h`), which the
   rule does not allow. A `bool has_default` beside a plain
   `std::string` replaces it; the change reaches dg.
3. **`rowno = n` as a TID scan.** A query names a row by its
   `rowno` (`../dg/WORKNOTE.md` step 200), and `WHERE rowno =
   n` reads the whole table with a filter: the planner knows a
   TID scan only as `ctid = tid` (optimizer `tidpath.c`).
   Taught `rowno`, it fetches the one row, as `UPDATE` and
   `DELETE` by a row's number want.
4. **A missing qualified column names the wrong error.** On a
   table in sm, `t.nosuch` fails with "relation "t" does not
   have a composite type", not `column t.nosuch does not
   exist`: the parser tries `nosuch(t)` as a function on the
   row, which a table in sm has no type for.
5. **Purge postmaster `make_legacy_dirs()`.** It makes eight
   directories nothing opens: `pg_dynshmem` (DSM, step 29),
   `pg_stat` and `pg_stat_tmp` (pgstat, step 28),
   `pg_replslot` and `pg_logical`'s three (replication, step
   2), and `pg_snapshots` (snapshot export, step 201). The
   list is then empty, and the function goes;
   `../dg/PLAN.md` §6 mentions it.
6. **Purge the replication catalogs.** `../dg/WORKNOTE.md`
   step 2's known leftovers: `pg_subscription`,
   `pg_subscription_rel`, `pg_publication*` and
   `pg_replication_origin`. They reach into generated catalog
   headers (`schemapg.h`, `system_fk_info.h`) and the
   relcache, so this cut is larger than item 5.

---

## 2. Loose ends

Owed, and blocking nothing above:

- **A rollback refused past the undo list's cap is untested in
  digres.** digres dooms the transaction when sm refuses a
  rollback to a savepoint (`../dg/WORKNOTE.md` step 205). Since
  a load whose end fails is ended too (step 206), only
  `savepoint_undo_bytes`, 64 MB of writes after a savepoint,
  leads there, and no test sets the cap low enough:
  `sm_attach()` takes no options from digres.

- **The commit cache's size** (`sm1/COMMITCACHE.md` §7).
  65,536 entries reaches back under a second at 100,000
  transactions a second, while a version needs a lookup until
  compaction stamps it. Size the cache by memory with a larger
  default, 16 MB for a million slots; and measure how
  long stamping takes under load, which is what the size
  should be set from.

- **Check `EvalPlanQual` against PostgreSQL's source.**
  `SM3_DESIGN.md` §3.1 says the re-check of a join uses the
  row of the other table that was already joined, not its
  newest version. That is from memory.
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
- **ADD COLUMN with a volatile default is refused.**
  `random()`, `clock_timestamp()`, a `serial` or an identity
  column give each existing row its own value, so PostgreSQL
  rewrites the table. sm3's `add_column` gives older rows one
  missing value and has no rewrite; digres says it is not
  supported yet. It needs a pass that writes the value into
  every row under the table's X lock, or a rewrite into a new
  store as `truncate_table` swaps one in.
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

- **A point read should pin its row, not lend it.** The bytes
  `sm1::read`, `sm1::read_latest` and the old row of a write
  hand out are valid only until the next call on the session
  (`sm1/API.md`), so a caller holding two rows read by rowno
  must copy one. PostgreSQL pins a page a row and holds any
  number at once. sm should give the same: a read that
  returns a handle the caller releases, over RocksDB's
  `PinnableSlice`, which keeps the block in memory and copies
  nothing. sm1's point read is an iterator's `Seek` today
  (`sm1/SM1_DESIGN.md` §10.1), so the handle would hold the
  iterator, or the read would move to `Get`. Until then
  digres copies: the row an index scan, a bitmap scan or an
  UPDATE fetches by its rowno has its columns passed by
  reference copied into its slot (`read_row()` in
  `../dg/src/backend/access/sm/smstore.c`, `../dg/WORKNOTE.md`
  step 161).

- **Views.** `CREATE VIEW` fails in digres: `a relation's
  storage cannot be made: digres has no catalog table`
  (`DefineView()` still makes a PostgreSQL relation). In the
  whole of SQLite's corpus, 5,675,180 records
  (`../dg/test/slt/README.md`), every one digres fails and
  PostgreSQL 18 passes, 48,608, uses a view. A view needs a
  place in sm3's catalog for its query, a tree digres
  serializes, as an index's predicate is kept
  (`sm_cat_create_index`'s `eval`), and digres's rewriter to
  expand it; `information_schema` waits for the same
  (`../dg/SYSCATALOG.md`).
- **What foreign keys left** (`FOREIGNKEY.md`,
  `../dg/WORKNOTE.md` step 177): a DEFERRABLE one is refused,
  as a row trigger on a table in sm is given its rows from a
  store that lasts only the statement; `pg_constraint` lists
  none, so `\d` does not show them.
- **What PostgreSQL's isolation specs found**
  (`../dg/test/isolation_specs.txt`, `../dg/WORKNOTE.md` step
  169), beyond the key-share modes above: sm1
  has no wait queue to reorder, so a soft deadlock PostgreSQL
  untangles ends a transaction (`deadlock-soft`).
- **A failed index build's detail is sm's.** `CREATE UNIQUE
  INDEX` over duplicates says `index build: two rows of u
  with one key in uk` where PostgreSQL says `Key (k)=(1) is
  duplicated.`; sm's error does not carry the key.

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
  for the dependency index of `sm3/CATALOG3.md` §0.5, and
  nothing is waiting for them.
- **The unmeasured startup cost for thousands of tables** —
  open in `sm3/CATALOG.md` §7.
- **The rest of the catalog's layout** (`sm3/CATALOG3.md`). A
  key for each column and each index is built. Still
  proposed: an index's own key, include and read columns as
  entries, which removes the last counted framing; the
  dependency index, which the checks above wait for; and keys
  as words with tagged values.
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
