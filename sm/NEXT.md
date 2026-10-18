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
2. **digres's `xact.c`, until it is empty.** sm keeps the
   transaction; `xact.c` keeps only what a session needs
   around it (`../dg/PLAN.md` §7), and what it no longer needs
   is deleted as it is found. Nothing is moved out of it just
   so the file goes (`../dg/WORKNOTE.md` steps 232 to 236).
   It is 2,874 lines.

---

## 2. Loose ends

Owed, and blocking nothing above:

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

- **A scan pays for the whole row, whatever it reads.**
  `SELECT a FROM t` on a wide table costs as `SELECT *` does,
  three times over. Measure first: a scan of one column and
  of all of them, on a table of 50 columns, split between
  `memcpy`, `parse()` and `deform_each`.
  - *Deform lazily.* digres's slot is `TTSOpsVirtual`
    (`smtable_slot_callbacks()` in
    `../dg/src/backend/access/sm/smtable.c`), and
    `scan_row_into_slot()` in `smstore.c` deforms every
    column into it, so the projection's
    `slot_getsomeattrs(slot, 1)` finds them all done.
    PostgreSQL's heap slot deforms only as far as asked.
    digres should have slot ops of its own that hold sm's
    tuple and, in `getsomeattrs(slot, n)`, deform its first
    `n` columns only; sm2 needs a deform of a prefix, which
    `decode_view()` already shows how to find. The tuple's
    bytes are valid until the cursor's next call, as the
    virtual slot's are now, and a decimal of `slot_from_sm()`
    is made as its column is deformed.
  - *Check only what is read.* `parse()` in
    `sm2/src/tuple/tuple.cc` checks every slot and varlena
    before `deform_each` reads one. The lazy deform should
    check only what it reads, as `decode_view()` does;
    RocksDB's block checksums cover the rest.
  - *Lend the row, not a copy.* sm1's `scan_next` copies each
    row it hands out into the cursor
    (`sm1/src/access.cc`), because it steps past the key's
    older versions with `it->Next()` before it returns. It
    could return on the visible version and step past the
    rest at the next call, so the row is a view into
    RocksDB's block, valid until then. The SIREAD range
    (`last_examined`) and `skip_version()` move with it; or
    `ReadOptions::pin_data`, which pins every block the scan
    reads until it ends. Last of the three: one `memcpy`, and
    the change touches SSI.

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
