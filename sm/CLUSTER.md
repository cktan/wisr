# CLUSTER — a table clustered on its primary key

A table today is a heap keyed by `rowno`, and its primary key
is an index over it (`sm2/SM2_DESIGN.md` §5). On RocksDB that
makes every read through an index two seeks, the primary
key's included, where PostgreSQL's heap fetch is a buffer
lookup. This file is the pivot away from it: a table is kept
in its primary key's order, and a hidden `_id` keeps the
6-byte TID PostgreSQL's executor runs on. Decided, not built.
The state before the pivot is on the branch
`prepivot-heap-with-recno`.

Paths are relative to `sm/`; digres's are under `../dg/`.

---

## 0. In brief

- **A row's key is its primary key**: `relid ‖ encode(pk)`. A
  table with no primary key is keyed by `_id`, which makes it
  the heap it is today.
- **Every row has an `_id`**, an `int8` from the table's
  sequence, stored in the row, never reused and never
  changed, not even when the primary key is. It is what the
  executor's TID holds, so `ctid`'s machinery stays: UPDATE,
  DELETE, row locks, EvalPlanQual, bitmap and TID scans.
- **`_id` has a unique index, `_id → pk`**, when the table has
  a primary key. It is sm3's, and no planner sees it.
- **A secondary index maps to the primary key**, not to `_id`:
  `encode(sk) ‖ encode(pk)`. A lookup through it is two seeks,
  as now.
- **The primary key index is the table.** It has no entries of
  its own: a scan of it is a range scan of the rows, and its
  uniqueness is sm1's `SM_ERR_DUPKEY` on the row's key.
- **What it buys**: a range over the primary key reads one
  sorted run, a point read by it is one seek, and an insert
  writes the row and the `_id` entry, as many keys as today.
- **What it costs**: reaching a row by its TID is two seeks;
  changing a row's primary key moves the row and rewrites
  every one of its index entries; adding or dropping a primary
  key rewrites the table.

---

## 1. Why

Q6 of the benchmark (`../bench/DESIGN.md` §5) reads 10,000
rows by a range of the primary key. Measured on
`wide_mixed` at `2eab187d`: PostgreSQL 2.3 ms, digres 9.0 ms,
about 230 ns a row against 900. A profile of digres puts 72%
of it in fetching the row by `rowno`, and 49% in that fetch's
`DBIter::Seek` alone: each row starts a seek from the top of
the LSM — the file, the bloom filter, the index block, the
block cache, a binary search of the data block — although the
next row is almost always the next entry in the store.
PostgreSQL's next row is on the same heap page.

A heap is right for page storage, where a TID is an address.
On an LSM `rowno` is a key like any other, so the heap pays an
index's price twice. Every SQL system built on an LSM clusters
on the primary key: MyRocks, CockroachDB, TiDB, YugabyteDB.
MyRocks is the closest: a table keyed by its primary key, a
secondary entry of (key, primary key), and a hidden 8-byte key
for a table that declares none.

The reasons `sm2/SM2_DESIGN.md` §5 gave for a heap, answered:

- **The planner and executor assume a heap.** `_id` keeps
  that assumption true where it matters: every row has a
  fixed-width identity that fits a TID.
- **A table may have no primary key.** It is keyed by `_id`.
- **A primary key change must not rewrite every index.** It
  does now, and is accepted: OLTP keys rarely change.
- **An 8-byte suffix is smaller than an encoded key.** It is,
  and secondary entries grow by the key's width less 8 (§7).

---

## 2. The row

| | Key | Value |
| --- | --- | --- |
| with a primary key | `relid ‖ encode(pk)` | `be64(_id) ‖ tuple` |
| without one | `relid ‖ be64(_id)` | `be64(_id) ‖ tuple` |

- **`encode(pk)` is sm2's key encoding** of the primary key's
  columns, in their order, with their collations, as a unique
  index's key is today (`sm2/SM2_DESIGN.md` §3). It is never
  cut: a key too long refuses the row with
  `SM_ERR_KEYTOOLONG`, as a unique index does now.
- **The tuple holds every column, the primary key's
  included.** `encode` stays allowed to be lossy (collated
  text keeps a sort key), so the key is an ordering device and
  the tuple is what a read returns, as now.
- **`_id` leads the value**, ahead of the tuple, so the tuple
  format and its deform are unchanged, and a scan of the rows
  hands the executor each row's TID without a lookup. A table
  without a primary key carries it twice, in the key and the
  value, for one code path (§10, question 2).
- **`_id` is drawn from the table's sequence**, the one
  `rowno` is drawn from now, renamed. It is dense and only
  grows, so it fits a TID while it is below 2^40
  (`smstore_rowno_fits_tid()`, `../dg/src/backend/access/sm/
  smstore.c`), and ANALYZE and TABLESAMPLE can keep sampling
  it (§5).

---

## 3. Indexes

| Index | Key | Value |
| --- | --- | --- |
| `_id`, with a primary key | `relid ‖ be64(_id)` | the row key's tail, `encode(pk)` |
| secondary, non-unique | `relid ‖ encode(sk) ‖ encode(pk)` | a cut flag, the INCLUDE columns |
| secondary, unique | `relid ‖ encode(sk)` | `encode(pk)`, a cut flag, the INCLUDE columns |
| primary key | none: the rows are its entries | |

- **The `_id` index is sm3's own.** It is created with the
  primary key, maintained as any index is, has a relid of its
  own, and is not in PostgreSQL's catalog. It is unique by
  construction, and its key is sm1's uniqueness unit as a
  unique index's is.
- **A secondary entry names its row by `encode(pk)`**, the row
  key less its relid, so the fetch is one seek to
  `relid ‖ encode(pk)`. A unique index's entry with a NULL key
  column carries `encode(pk)` as its suffix, as it carries
  `rowno` now, so that NULLs stay distinct. A table without a
  primary key has `be64(_id)` there: today's entry.
- **The key budget**: `encode(sk)` and `encode(pk)` together
  are within `sm1::max_keylen` less the relid. A non-unique
  entry cuts `encode(sk)` to fit and keeps `encode(pk)` whole;
  a unique one refuses the row, as now. A primary key's
  encoding is capped, so every secondary keeps room for its
  own columns (§10, question 3).
- **The primary key index stays in PostgreSQL's catalog**, so
  the planner proves uniqueness with it and plans scans of it.
  digres turns a scan of it into a range scan of the rows
  (§6). `CLUSTER` stays a no-op: every table is clustered.

---

## 4. Writing

- **Insert** draws an `_id`, writes the row under its key — a
  duplicate primary key is sm1's `SM_ERR_DUPKEY` — then the
  `_id` entry and each secondary. Two writes plus one per
  secondary, as today's row and primary key entry.
- **An update that leaves the primary key alone** writes the
  row at its key, and touches only the secondaries whose
  columns changed, as now (`sm3/SM3_DESIGN.md` §3).
- **An update that changes the primary key moves the row.**
  It erases the old key and inserts the new one, under the
  same `_id`; rewrites the `_id` entry; and deletes and
  re-inserts every secondary entry, whose suffix changed. A
  duplicate of the new key is `SM_ERR_DUPKEY`, and the
  statement's savepoint undoes the half-done move, as any
  failed multi-key write is undone now.
- **A concurrent writer that finds a row moved** follows it.
  It reaches the row by `_id`, through the `_id` index; if the
  key it read is gone and the newest committed `_id` entry
  names another, the row moved, which is an update, not a
  delete: at READ COMMITTED sm3 reports `SM_ERR_UPDATED` and
  the executor's re-check reads the row at its new key through
  the same `_id`. Above READ COMMITTED it is a serialization
  failure, as any concurrent update is. PostgreSQL's
  cross-partition move is the precedent, and is harsher: it
  refuses to follow.
- **Row locks name the row's key**, `relid ‖ encode(pk)`, so
  they are variable in width (`LOCK.md` §2). A move erases
  the old key, which is a write on it, so it waits for a lock
  on the old key as any write does; and it inserts the new
  key, which it then holds as a writer. A `FOR UPDATE` lock
  therefore never loses its row to a move.
- **Delete** erases the row, the `_id` entry and each
  secondary entry.

---

## 5. Reading

- **By primary key**: a point read is one seek; a range is a
  forward scan of the rows, one sorted run after VACUUM.
- **By `_id`** (the TID): the `_id` entry gives `encode(pk)`,
  and the row is a second seek. A table without a primary key
  answers in one, as now. digres remembers `encode(pk)` for
  the rows its scans hand out (§6), so the usual UPDATE or
  DELETE, whose TID came from the statement's own scan, pays
  one.
- **Through a secondary index**: the entry gives `encode(pk)`,
  the row a second seek. Its `_id` comes with it, so the TID
  costs nothing more.
- **A sequential scan** walks the rows in primary key order.
- **ANALYZE samples `_id`s as it samples `rownos` now**
  (`sm3/SM3_DESIGN.md` §6.3, §6.4): runs of consecutive
  `_id`s below the sequence's limit, so the estimator of the
  row count, live fraction times the limit, stands. With a
  primary key a run is a range scan of the `_id` index and a
  point read a row, not one forward cursor: about 30,000
  random reads a table, tens of milliseconds. Correlation is
  measured in the rows' own order, the primary key's, so the
  key's leading column comes out at 1 and the planner costs a
  range over it as the sequential read it is.
- **TABLESAMPLE** picks blocks of 256 `_id`s, as it picks
  `rowno`s now, read the same way.
- **The index build's backfill** walks the rows in primary key
  order, locks each by its key, and resumes from the last key
  it read. A duplicate it finds is reported as the two rows'
  `_id`s, so `sm0::dupkey_t` and the persisted index state
  keep their fixed width.
- **Bulk load in file mode** sorts the row stream by key and
  checks it for duplicates, as it does a unique index's stream
  now (`BULKLOAD.md` §5.3, §5.4); the `_id` index's stream is
  the one that arrives in order. A table without a primary key
  is loaded as now.
- **TOAST is untouched**: a chunk's key is its own id and
  sequence number, not the row's (`sm2/SM2_DESIGN.md` §4.2).

---

## 6. digres

- **The TID holds `_id`**, packed as `rowno` is now. The
  system column SQL reads stays an `int8`, renamed `_id`
  (§10, question 1).
- **A scan of the primary key index is a scan of the rows**:
  `smstore_index_getnext()` sees the index is the primary key
  and opens a range scan of the table over the bounds, instead
  of an index cursor. Q6 becomes a forward scan of 10,000
  rows. An index-only scan of it reads the rows too.
- **A secondary index scan reads the row by `encode(pk)`**,
  from the entry, in place of `rowno`.
- **Fetch by TID** goes through `_id`, with a small cache of
  `_id → encode(pk)` filled by the statement's scans, so an
  UPDATE or DELETE of a row just scanned is one read.
- **Bitmap scans are unchanged**: a bitmap of `_id`s, dense, in
  `_id` order, each row fetched by `_id`. Two seeks a row
  where a table has a primary key; the `_id` index is read in
  order, which a later step over it can exploit.
- **TID scans are unchanged**: `_id = …` reads by `_id`.
- **ADD PRIMARY KEY, DROP PRIMARY KEY**, and a change of a key
  column's type or collation, rewrite the table (§8).
- **Foreign keys** that reference the primary key look the
  parent up by a point read of its rows, and lock its key.

---

## 7. Costs

- **Reaching a row by its TID is two seeks** in a table with a
  primary key: an UPDATE, DELETE or lock whose TID did not
  come from the statement's own scan, EvalPlanQual's fetch,
  bitmap and TID scans.
- **Every insert into such a table writes the `_id` entry**,
  where a table clustered without `_id` would not.
- **A primary key change moves the row** and rewrites all its
  index entries; PostgreSQL rewrites only the changed indexes,
  unless HOT applies.
- **Secondary entries carry `encode(pk)`**, wider than 8 bytes
  for most keys, so secondary indexes grow, and a wide primary
  key leaves less of `max_keylen` to a secondary's columns.
- **Adding or dropping a primary key rewrites the table** and
  its indexes, under the table-oid lock X. PostgreSQL builds
  only an index.
- **ANALYZE and TABLESAMPLE read at random** in a table with a
  primary key, through the `_id` index.
- **Row locks are variable-width** names in the lock table.

---

## 8. A rewrite

`ADD PRIMARY KEY`, `DROP PRIMARY KEY`, and `ALTER COLUMN …
TYPE` or a collation change on a key column change what a
row's key is, so the table is copied into a new store:

1. Take the table-oid lock X, as every DDL does.
2. Make a store; copy each row under its new key, with its
   `_id` and its TOAST chunks, whose pointers name the store
   and are rewritten. A duplicate key or a NULL in a key column
   fails the statement.
3. Fill the `_id` index (or none, when the table has no
   primary key after), then every secondary index.
4. At commit the catalog names the new store, and the old one
   is dropped, as `TRUNCATE` swaps a store now
   (`sm3/CATALOG.md` §5.2).

A table created with a primary key is clustered from its
birth and is never rewritten.

---

## 9. What changes where

- **sm1**: nothing. The isolation scenarios are untouched.
- **sm2**: the rel is keyed by its clustering key, the primary
  key's columns or `_id`, named in its schema; its value is
  `_id ‖ tuple`; `rel_insert` returns `_id` and the key;
  reads, updates, deletes and locks take the key; the index
  rel's entries carry `encode(pk)` (§3).
- **sm3**: the catalog records the clustering key; the `_id`
  index; `tbl_*` by `_id` and by key; the move (§4); the
  backfill, ANALYZE, TABLESAMPLE and bulk load (§5); the
  rewrite (§8).
- **The C API** (`include/sm.h`): calls that take a `rowno`
  take an `_id` and keep their shape, which is what lets
  digres move in steps (§11); new calls read and scan by key;
  `sm_idx_scannxt` hands out the row's key, once digres reads
  by it (§11, step 2), and its `_id` until then, read from the
  row; `sm_errdup` hands out `_id`s; `sm_lock_status_t` names
  a row by its key.
- **rel**: the verbs that take or return a `rowno` take or
  return an `_id`, and new ones read by key.
- **digres**: §6.
- **Documents** rewritten as the code lands:
  `sm2/SM2_DESIGN.md` §5 and §7, `sm3/SM3_DESIGN.md` §3 and
  §6, `DESIGN.md` §5, `PART4.md` §3, `STATISTICS.md`,
  `BULKLOAD.md`, `LOCK.md`, `GLOSSARY.md`, `sm3/CATALOG.md`,
  `rel/REL.md`.

No database on disk is carried over: the format changes and
nothing migrates.

---

## 10. Decided at review

1. **The system column is renamed `_id`**, still the `int8`
   SQL reads: one name in SQL, in sm and here. digres's
   tests that print `rowno` change with it.
2. **A keyless table keeps `_id` in its value**, though its
   key holds it too: one row format and one deform path, for
   8 bytes a row.
3. **A primary key's encoding is capped at 1024 bytes**,
   which leaves every secondary at least 3 KB of
   `max_keylen`. A longer key is refused at the write.
   PostgreSQL's B-tree caps an entry at about 2.7 KB.
4. **`DEFERRABLE` primary keys are refused**, at CREATE and
   at ALTER. Uniqueness is sm1's, checked at the write, as
   for any unique index now, and a DEFERRABLE foreign key is
   refused already.
5. **`pg_locks` shows a row lock's relation alone**, its page
   and tuple NULL. sm3 does not read the row to resolve the
   key to its `_id`.

---

## 11. Order of work

Each step leaves every suite passing.

1. **sm2 and sm3, with the C API's shape kept.** Rows keyed by
   the primary key, the `_id` index, secondary entries by key,
   the move, the backfill, ANALYZE, bulk load, the rewrite;
   `rowno` renamed `_id` through sm, its calls unchanged in
   shape, so digres runs as it is, every TID resolved through
   `_id`. The `rel` suite proves it, and digres's suites pass
   unchanged.
2. **digres reads by key.** A primary key index scan becomes a
   range scan of the rows; a secondary index scan fetches by
   `encode(pk)`; the `_id → pk` cache; the system column
   renamed `_id` (§10, question 1). Q6 and Q7 measured.
3. **digres's DDL.** ADD and DROP PRIMARY KEY, and the
   key-column ALTERs, through the rewrite.
4. **The documents of §9**, each as its step lands, and this
   file marked built.
