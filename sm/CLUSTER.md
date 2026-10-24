# CLUSTER — a table clustered on its primary key

A table today is a heap keyed by `rowno`, and its primary key
is an index over it (`sm2/SM2_DESIGN.md` §5). On RocksDB that
makes every read through an index two seeks, the primary
key's included, where PostgreSQL's heap fetch is a buffer
lookup. This file is the pivot away from it: a table is kept
in its primary key's order, and a hidden `_id` kept the
6-byte TID PostgreSQL's executor ran on. Steps 1 and 2 of §11
are built. `ROWKEY.md` took the next step, built: digres names
a row by its key, nothing in it is a TID, and a table with a
primary key has no `_id` and no `_id` index.
The state before the pivot is on the branch
`prepivot-heap-with-recno`.

Paths are relative to `sm/`; digres's are under `../dg/`.

---

## 0. In brief

- **A row's key is its primary key**: `relid ‖ encode(pk)`. A
  table with no primary key is keyed by `_id`, which makes it
  the heap it is today.
- **A row of a table without a primary key has an `_id`**, an
  `int8` from the table's sequence, its key, never reused and
  never changed. SQL reads it as `_id`. It was what the
  executor's TID held, in every row; digres now names a row by
  its key, and a row of a table with a primary key has no
  `_id` (`ROWKEY.md` §2.1).
- **There is no `_id` index.** One, `_id → pk`, was sm2's
  rel's own in every table with a primary key, until the `_id`
  went from such a table.
- **A secondary index maps to the primary key**, not to `_id`:
  `encode(sk) ‖ encode(pk)`. A lookup through it is two seeks,
  as now.
- **The primary key index is the table.** It has no entries of
  its own: a scan of it is a range scan of the rows, and its
  uniqueness is sm1's `SM_ERR_DUPKEY` on the row's key.
- **What it buys**: a range over the primary key reads one
  sorted run, a point read by it is one seek, and an insert
  writes the row and one entry a secondary index.
- **What it costs**: changing a row's primary key moves the
  row, and fails a concurrent writer of it, and rewrites
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
  fixed-width identity that fits a TID. (`ROWKEY.md` later
  freed the executor of the TID.)
- **A table may have no primary key.** It is keyed by `_id`.
- **A primary key change must not rewrite every index.** It
  does now, and is accepted: OLTP keys rarely change.
- **An 8-byte suffix is smaller than an encoded key.** It is,
  and secondary entries grow by the key's width less 8 (§7).

---

## 2. The row

| | Key | Value |
| --- | --- | --- |
| with a primary key | `relid ‖ encode(pk)` | `tuple` |
| without one | `relid ‖ be64(_id)` | `tuple` |

- **`encode(pk)` is sm2's key encoding** of the primary key's
  columns, in their order, with their collations, as a unique
  index's key is today (`sm2/SM2_DESIGN.md` §3). It is never
  cut: a key too long refuses the row with
  `SM_ERR_KEYTOOLONG`, as a unique index does now.
- **The tuple holds every column, the primary key's
  included.** `encode` stays allowed to be lossy (collated
  text keeps a sort key), so the key is an ordering device and
  the tuple is what a read returns, as now.
- **The value is the tuple alone** (`ROWKEY.md` §2.1). `_id`
  led it once, ahead of the tuple; a keyless row's `_id` is
  read from its key, and a keyed row has none.
- **`_id` is drawn from the table's sequence**, the one
  `rowno` was drawn from, renamed, by a row of a table without
  a primary key alone. It is dense and only grows, so ANALYZE
  and TABLESAMPLE sample a keyless table by runs of it (§5).
  The sequence is made for every table; a keyed one draws
  nothing from it.

---

## 3. Indexes

| Index | Key | Value |
| --- | --- | --- |
| secondary, non-unique | `relid ‖ encode(sk) ‖ encode(pk)` | a cut flag, the INCLUDE columns, `encode(pk)` |
| secondary, unique | `relid ‖ encode(sk)` | a cut flag, the INCLUDE columns, `encode(pk)` |
| primary key | none: the rows are its entries | |

- **There is no `_id` index** (`ROWKEY.md` §2.1). It was the
  rel's own, under the fixed relid `0xFFFE`, in every table
  with a primary key; that relid is still one no index takes,
  and nothing is written under it.
- **An entry's value is a cut flag, the INCLUDE columns and
  `encode(pk)`**, in both kinds of table, with no `_id`; digres
  reads an entry's INCLUDE columns from its second attribute.
  `encode(pk)` ends the value, for the unique entry whose key
  does not carry it.
- **A secondary entry names its row by `encode(pk)`**, the row
  key less its relid, so the fetch is one seek to
  `relid ‖ encode(pk)`. A unique index's entry with a NULL key
  column carries `encode(pk)` as its suffix, as it carries
  `rowno` did, so that NULLs stay distinct. A table without a
  primary key has `be64(_id)` there: the entry as it was.
- **The key budget**: `encode(sk)` and `encode(pk)` together
  are within `sm1::max_keylen` less the relid. A non-unique
  entry cuts `encode(sk)` to fit and keeps `encode(pk)` whole;
  a unique one refuses the row, as before. A primary key's
  encoding is capped, so every secondary keeps room for its
  own columns (§10, question 3), and a key is cut at the one
  length every cut key shares: 3070 bytes in a table with a
  primary key, 4086 in one without.
- **The primary key index stays in PostgreSQL's catalog**, so
  the planner proves uniqueness with it and plans scans of it.
  digres turns a scan of it into a range scan of the rows
  (§6). `CLUSTER` stays a no-op: every table is clustered.

---

## 4. Writing

- **Insert** writes the row under its key — a duplicate
  primary key is sm1's `SM_ERR_DUPKEY` — then each secondary:
  one write plus one per secondary. A row of a table without a
  primary key draws its `_id`, its key, first.
- **An update that leaves the primary key alone** writes the
  row at its key, and touches only the secondaries whose
  columns changed, as now (`sm3/SM3_DESIGN.md` §3).
- **An update that changes the primary key moves the row.**
  It erases the old key, with a moved tombstone, and inserts
  the new one; and deletes and re-inserts every secondary
  entry, whose suffix changed. A
  duplicate of the new key is `SM_ERR_DUPKEY`, and the
  statement's savepoint undoes the half-done move, as any
  failed multi-key write is undone now.
- **A concurrent writer that finds a row moved fails.** The
  move erases the old key with sm1's moved tombstone
  (`sm1::erase_moved`), so a write at it meets
  `SM_ERR_UPDATED`, and the executor's re-check, the newest
  version there (`tbl_read_latest`), `SM_ERR_MOVED`, which
  digres raises as a serialization failure (`ROWKEY.md` §2.3).
  PostgreSQL's cross-partition move is the precedent: it
  refuses to follow. Above READ COMMITTED a moved row is a
  serialization failure, as any concurrent update is. sm3
  once followed the row through its `_id` entry, under a
  shared lock on it; that went when digres stopped writing by
  `_id` (`../dg/WORKNOTE.md` step 296), and the `_id` entry
  with the `_id` (step 298).
- **Row locks name the row's key**, `relid ‖ encode(pk)`, so
  they are variable in width (`LOCK.md` §2). A move erases
  the old key, which is a write on it, so it waits for a lock
  on the old key as any write does; and it inserts the new
  key, which it then holds as a writer. A `FOR UPDATE` lock
  therefore never loses its row to a move.
- **Delete** erases the row and each secondary entry.

---

## 5. Reading

- **By primary key**: a point read is one seek; a range is a
  forward scan of the rows, one sorted run after VACUUM.
- **By `_id`**: a table without a primary key answers in one
  seek, its key. A table with one has no `_id` to read by
  (`ROWKEY.md` §2.2).
- **Through a secondary index**: the entry gives `encode(pk)`,
  the row a second seek.
- **A sequential scan** walks the rows in primary key order.
- **ANALYZE samples a table with a primary key by key
  ranges** (§10, question 6; `ROWKEY.md` §2.4,
  `sm3/SM3_DESIGN.md` §6.4): slots of 8 KB of its bytes, each
  the rows between the keys at its ends, found from sm1's size
  estimates, read by one forward cursor; the row count is the
  rows read per byte the slots span, times the table's bytes.
  A keyless table is sampled by runs of `_id`s, as `rowno`s
  were, the row count the live fraction times the limit.
  Correlation is measured in the rows' own order, the primary
  key's, so the key's leading column comes out at 1 and the
  planner costs a range over it as the sequential read it is.
- **TABLESAMPLE** of a table with a primary key picks slots
  of its bytes the same way, SYSTEM's read by a seek to each,
  and BERNOULLI's 256 rows of a scan in key order; of a keyless
  table, blocks of 256 `_id`s, as it picked `rowno`s.
- **The index build's backfill** walks the rows in key order
  (`tbl_scan_rows`), one sorted run, reads each and locks it
  at its key, and resumes from a key. A duplicate it finds is
  reported as the index and one row's key, which
  `sm0::dupkey_t` and the persisted index state carry
  (`ROWKEY.md` §2.5).
- **Bulk load in file mode** sorts the row stream by key and
  checks it for duplicates, as it does a unique index's stream
  now (`BULKLOAD.md` §5.3, §5.4), with no `_id` and no `_id`
  stream. A table without a primary key is loaded as now, its
  rows arriving in `_id` order.
- **TOAST is untouched**: a chunk's key is its own id and
  sequence number, not the row's (`sm2/SM2_DESIGN.md` §4.2).

---

## 6. digres

- **A row is named by its key**, its primary key's columns'
  values, or a keyless table's `_id`; nothing is a TID
  (`ROWKEY.md` §3). The system column SQL reads is an `int8`,
  `_id`, `rowno` renamed (§10, question 1), of a keyless table
  alone.
- **A scan of the primary key index is a scan of the rows**:
  `smstore_index_getnext()` sees the index is the primary key
  and opens a range scan of the table over the bounds, instead
  of an index cursor. Q6 becomes a forward scan of 10,000
  rows. An index-only scan of it reads the rows too.
- **A secondary index scan reads the row by `encode(pk)`**,
  from the entry, in place of `rowno`.
- **A write names its row by its key**: `sm_tbl_update`,
  `sm_tbl_delete`, `sm_tbl_lock` and their kin take an
  `sm_row_key_t`, and sm3 writes at the key with no read
  (`ROWKEY.md` §2.2). The executor's fetch of a row a scan
  of the statement just handed out takes it from the scan's
  pin, with no read (`smstore.c`'s `scan_at()`), the scan found
  by the key's encoding (`sm_tbl_row_key`, `sm_rel_scan_key`,
  `sm_idx_scan_row`). The key survives a sort or a hash.
- **No bitmap scan is planned** of a table in sm (§10,
  question 6): an index in sm has no `amgetbitmap`. A bitmap
  of `_id`s was no sorted read, as one of TIDs is in
  PostgreSQL: each `_id` a seek of its own, and two in a
  table with a primary key. A secondary index scan already
  reads its rows by key; what is lost is combining indexes,
  BitmapAnd and BitmapOr, as InnoDB and CockroachDB do
  without.
- **No TID scan of a table with a primary key** but `WHERE
  CURRENT OF`'s, which reads by the cursor's row's key
  (`../dg/src/backend/optimizer/path/tidpath.c`): it has no
  `_id` to name (`ROWKEY.md` §3.1). A keyless table's TID scan
  reads by `_id`, its key: one seek. digres has no `ctid`.
- **ADD PRIMARY KEY, DROP PRIMARY KEY**, and a change of a key
  column's type or collation, rewrite the table (§8).
- **Foreign keys** that reference the primary key look the
  parent up by a point read of its rows, and lock its key.

---

## 7. Costs

- **A primary key change moves the row** and rewrites all its
  index entries; PostgreSQL rewrites only the changed indexes,
  unless HOT applies.
- **A statement that shifts primary keys can collide with
  itself.** `UPDATE t SET k = k + 1` reads the rows in key order
  and checks each new key at its write, as every unique index is
  checked, so the first row's new key is the second row's old
  one, which is still there: a duplicate. PostgreSQL checks at
  the write too, and passes only when its heap holds the rows in
  the other order; the SQL standard checks at the statement's
  end, which nothing in the stack does. Accepted, as InnoDB and
  MyRocks accept it; nine statements of pgregress's
  `foreign_key` are taken out for it.
- **Secondary entries carry `encode(pk)`**, wider than 8 bytes
  for most keys, so secondary indexes grow, and a wide primary
  key leaves less of `max_keylen` to a secondary's columns.
- **Adding or dropping a primary key rewrites the table** and
  its indexes, under the table-oid lock X. PostgreSQL builds
  only an index.
- **ANALYZE and TABLESAMPLE estimate from the store's sizes**
  in a table with a primary key, which RocksDB approximates
  (`ROWKEY.md` §4).
- **Row locks are variable-width** names in the lock table.

---

## 8. A rewrite

`ADD PRIMARY KEY`, `DROP PRIMARY KEY`, and `ALTER COLUMN …
TYPE` or a collation change on a key column change what a
row's key is, so the table is copied into a new store:

1. Take the table-oid lock X, as every DDL does.
2. Make a store; copy each row under its new key, with its
   TOAST chunks, whose pointers name the store and are
   rewritten: a row that had an `_id` loses it under a primary
   key, and a row of a table that loses its primary key draws
   one, in key order, from the table's sequence. A duplicate
   key or a NULL in a key column fails the statement.
3. Fill every secondary index.
4. At commit the catalog names the new store, and the old one
   is dropped, as `TRUNCATE` swaps a store now
   (`sm3/CATALOG.md` §5.2).

A table created with a primary key is clustered from its
birth and is never rewritten.

---

## 9. What changes where

- **sm1**: nothing. The isolation scenarios are untouched.
- **sm2**: the rel is keyed by its clustering key, the primary
  key's columns or `_id`, given at its open; its value is the
  tuple alone; `rel_insert` returns the key, and a keyless
  row's `_id`; reads, updates, deletes and locks take the key;
  an update moves a row whose key changed; the index rel's
  entries carry `encode(pk)` (§3); the primary key opens as an
  index over the rows. Built, the `_id` index and `rel_find`
  gone with a keyed row's `_id` (`ROWKEY.md` §2.1).
- **sm3**: the catalog's primary key is the clustering key;
  `tbl_*` by key, and by `_id` in a keyless table; the move
  (§4); the backfill, ANALYZE, TABLESAMPLE and bulk load (§5);
  the rewrite (§8). Built.
- **The C API** (`include/sm.h`): calls that take a `rowno`
  take an `_id` and keep their shape, which is what lets
  digres move in steps (§11); new calls read, scan, write and
  lock by key (`sm_row_key_t`, `ROWKEY.md` §2.2);
  `sm_idx_scannxt` hands out the row's key, and a keyless row's
  `_id`, 0 for a keyed row's; the calls by `_id` are a keyless
  table's; `sm_errdup` hands out the index and a row's key
  (`ROWKEY.md` §2.5); `sm_lock_status_t`
  names a row of a table with a primary key with no `_id`, 0
  (§10, question 5).
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
   tests that print `rowno` change with it. Built, on a keyless
   table alone (`ROWKEY.md` §3.1).
2. **A keyless table keeps `_id` in its value**, though its
   key holds it too: one row format and one deform path, for
   8 bytes a row. Undone: the value is the tuple alone in both
   kinds of table, the one format (`ROWKEY.md` §2.1).
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
   key to its `_id`. Built in step 1, as a test of digres's
   (`../dg/test/server.sh`, lock status) locks a row of a
   table with a primary key.
6. **`_id` stays in a table with a primary key, and nothing
   reads many rows in `_id` order.** Sorted `_id`s are not a
   sequential read, as sorted TIDs are in PostgreSQL: each is a
   seek of its own. So no bitmap scan is planned (§6; first
   decided as a bitmap of sorted primary keys, then dropped at
   review), and ANALYZE and TABLESAMPLE are to sample by
   ranges of the key, sized from the store's estimates, not by
   runs of `_id`. `_id` keeps the 6-byte TID: a row fetched by
   it is two seeks, which taking the key from the scan on the
   row spares the usual UPDATE and DELETE (§6). The `_id`
   entry's cost to an insert, measured: one `INSERT ... SELECT`
   of 1M rows took 9.8 s with it and 5.4 s with none, against
   PostgreSQL's 1.5 s; its write and a check of its key, which
   no row had, a seek, were 35% of the insert. sm1's
   `insert_new` leaves the check out, for the `_id` entry and a
   keyless row, and the `_id` is drawn by a `seqref_t`, with no
   read of the sequence's name a row: 6.8 s keyed, 3.0 s
   keyless (5.8 s before), PostgreSQL 1.5 s and 1.1 s. `_id`
   stays; dropping it where a primary key is, the TID then a
   slot in a statement's table of keys, is the way on if what
   is left of its cost is too dear. That way was taken next
   (`ROWKEY.md`): digres names a row by its key, with no TID,
   and a keyed table has no `_id` and no `_id` index, built.

---

## 11. Order of work

Each step leaves every suite passing.

1. **sm2 and sm3, with the C API's shape kept.** Rows keyed by
   the primary key, the `_id` index, secondary entries by key,
   the move, the backfill, ANALYZE, bulk load, the rewrite;
   `rowno` renamed `_id` through sm, its calls unchanged in
   shape, so digres runs as it is, every TID resolved through
   `_id`. The `rel` suite proves it, and digres's suites pass
   unchanged but for `pg_locks` (§10, question 5). Built.
   Until step 2 a table with a primary key is slower: a scan
   of it walks the `_id` index and reads each row by its key,
   which keeps the rows in the order digres's tests expect,
   and a read through an index is three seeks, the entry, the
   `_id` entry and the row.
2. **digres reads by key.** Built so far: a primary key index
   scan is a range scan of the rows (`sm_tbl_idx_scan_rows`), a
   secondary index fetches its row by the key in its entry
   (`sm_tbl_read_key`), and a sequential scan reads the rows in
   key order (`sm_tbl_scan_rows`), its unordered results
   compared as multisets by digres's tests
   (`../dg/test/rowset.py`). Q6 went from 9.0 to 4.2 ms. No
   bitmap scan is planned (§6), and no TID scan of a table
   with a primary key; a write of a row a scan is on takes its
   key from the scan. What the `_id` entry costs an insert is
   measured, and cut (§10, question 6). The backfill and
   ANALYZE's correlation in key order, and ANALYZE and
   TABLESAMPLE by key ranges, are built with `ROWKEY.md`'s
   step 2, and the system column renamed `_id` (§10, question
   1) with its step 3.
3. **digres's DDL.** ADD and DROP PRIMARY KEY, and the
   key-column ALTERs, through the rewrite.
4. **The documents of §9**, each as its step lands, and this
   file marked built. Brought up to `ROWKEY.md`'s step 4.
