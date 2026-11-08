# CLUSTER: the Clustered Table

A Clustered Table keeps its rows in the order of its primary key.
The primary key is the row's key in the table's sm1 store, so a
range over the primary key is a forward scan of the rows, and the
primary key index has no entries of its own. A table with no
primary key is keyed by a hidden `_id`. digres names a row by its
key, and nothing in it is a TID ([`ROWSTORE.md`](ROWSTORE.md)).
The code is in
`sm2/src/rel/rel.cc` and `sm2/src/rel/index.cc` (rows, keys and
entries), `sm3/src/table/table.cc` (writes, the move and the
rewrite), `sm3/src/ddl/ddl.cc` (when to rewrite) and
`sm3/src/analyze/sample.cc` (sampling by key).

Paths are relative to `sm/`; digres's are under `../dg/`.

---

## 0. In brief

Terms: a **keyed** table has a primary key, and a **keyless**
table has none. `encode(x)` is sm2's key encoding of the columns
x. A **lookup** is one read of one key in the store. A **move**
is an update that changes a row's primary key.

- **A row's key is its primary key**: `relid ‖ encode(pk)`. A
  keyless table is keyed by `_id`, which makes it a heap.
- **A keyless row has an `_id`**, an `int8` from the table's
  sequence. It is the row's key, is never reused and never
  changes. SQL reads it as `_id`. A keyed row has no `_id`
  ([`ROWSTORE.md`](ROWSTORE.md#22-the-row-and-its-index-entries)
  §2.2).
- **There is no `_id` index.**
- **A secondary index maps to the primary key**:
  `encode(sk) ‖ encode(pk)`. A read through it is two lookups.
- **The primary key index is the table.** It has no entries of
  its own. A scan of it is a range scan of the rows, and its
  uniqueness is sm1's `SM_ERR_DUPKEY` on the row's key.
- **What it buys**: a range over the primary key reads one sorted
  run, a point read by it is one lookup, and an insert writes the
  row and one entry a secondary index.
- **What it costs**: a move fails a concurrent writer of the row
  and rewrites every one of its index entries. Adding or dropping
  a primary key rewrites a table that has rows.

```sql
CREATE TABLE t (id int4 PRIMARY KEY, v text, w int4);
CREATE INDEX t_v ON t (v);

INSERT INTO t VALUES (7, 'x', 1);
-- the row:   relid ‖ encode(7)   -> the tuple (7, 'x', 1)
-- t_v entry: t_v's relid ‖ encode('x') ‖ encode(7)
--                                -> not cut, encode(7)

UPDATE t SET w = 2 WHERE id = 7;  -- one write at the row's key
UPDATE t SET id = 8 WHERE id = 7; -- a move: the row gets a new
                                  -- key, and t_v a new entry
```

---

## 1. Why

A read by key starts at the top of the LSM: the file, the bloom
filter, the index block, the block cache, and a binary search of
the data block. A forward scan pays this once, then steps to the
next entry. So a range is cheap only if each row is the next
entry in the store. PostgreSQL's next row is on the same heap
page.

A heap is right for page storage, where a TID is an address. On
an LSM a heap's key is a key like any other, so a heap pays an
index's price twice. Every SQL system built on an LSM clusters on
the primary key: MyRocks, CockroachDB, TiDB, YugabyteDB. MyRocks
is the closest: a table keyed by its primary key, a secondary
entry of (key, primary key), and a hidden 8-byte key for a table
that declares none.

**Rejected: a heap** keyed by a row number, with the primary key
an index over it. Every read through an index is then two
lookups, the primary key's included, and a range over the primary
key pays a lookup a row. The arguments for a heap, answered:

- **The planner and executor assume a heap.** digres names a row
  by its key, which survives a sort or a hash, so the executor
  needs no TID (§6).
- **A table may have no primary key.** It is keyed by `_id`.
- **A primary key change must not rewrite every index.** It does,
  and this is accepted: OLTP keys rarely change.
- **An 8-byte suffix is smaller than an encoded key.** It is, and
  secondary entries grow by the key's width less 8 (§7).

---

## 2. The row

| | Key | Value |
| --- | --- | --- |
| keyed | `relid ‖ encode(pk)` | `tuple` |
| keyless | `relid ‖ be64(_id)` | `tuple` |

- **The relid is `sm3::heap_relid`, 1**, in every table. sm3
  makes every table's store chained, so a row is a chained key
  ([`ROWSTORE.md`](ROWSTORE.md#36-why-a-tables-rows-are-chained)
  §3.6).
- **`encode(pk)` is sm2's key encoding** of the primary key's
  columns, in their order, with their collations, as a unique
  index's key is ([`LSMINDEX.md`](LSMINDEX.md#2-key-encoding)
  §2). It is never cut: a key past
  `sm2::max_pk_len` refuses the row with `SM_ERR_KEYTOOLONG`. A
  NULL in it fails with `SM_ERR_INVALID`.
- **A key column may be out of line.** `encode_cols` fetches its
  value first, and encodes the value, not the pointer.
- **The tuple holds every column, the primary key's included.**
  `encode` may be lossy (collated text keeps a sort key). So the
  key is an ordering device, and the tuple is what a read
  returns.
- **The value is the tuple alone**
  ([`ROWSTORE.md`](ROWSTORE.md#22-the-row-and-its-index-entries)
  §2.2). A
  keyless row's `_id` is read from its key.
- **`_id` is drawn from the table's sequence**, by a keyless row
  alone. It only grows and may have gaps, so ANALYZE and
  TABLESAMPLE sample a keyless table by runs of it (§5). The
  sequence is made for every table. A keyed table draws nothing
  from it, unless a rewrite drops its primary key (§8).

---

## 3. Indexes

| Index | Key | Value |
| --- | --- | --- |
| secondary, non-unique | `relid ‖ encode(sk) ‖ encode(pk)` | a cut flag, the INCLUDE columns, `encode(pk)` |
| secondary, unique | `relid ‖ encode(sk)` | a cut flag, the INCLUDE columns, `encode(pk)` |
| B-tree Index, unique or not | `relid ‖ encode(sk) ‖ encode(pk)` | flags, and the columns' length |
| primary key | none: the rows are its entries | |

- **There is no `_id` index**
  ([`ROWSTORE.md`](ROWSTORE.md#22-the-row-and-its-index-entries)
  §2.2). Relid
  `0xFFFE` (`sm2::id_relid`) is reserved: no index takes it, and
  nothing is written under it.
- **An entry's value is a cut flag, the INCLUDE columns and
  `encode(pk)`**, in both kinds of table. digres reads an entry's
  INCLUDE columns from its second attribute on. `encode(pk)` ends
  the value, for the unique entry whose key does not carry it.
- **A secondary entry names its row by `encode(pk)`**, the row
  key less its relid, so the fetch is one lookup at
  `relid ‖ encode(pk)`. A unique entry with a NULL key column
  carries `encode(pk)` as its suffix, so that NULLs stay
  distinct. A keyless table has `be64(_id)` in each of these
  places.
- **A B-tree Index's entry always ends with `encode(pk)`**: the
  row decides whether the entry is live. Its value holds no
  INCLUDE column, as a scan reads them from the row
  ([`BTREEINDEX.md`](BTREEINDEX.md#4-page-layout) §4).
- **The key budget**: `encode(sk)` and `encode(pk)` together are
  within `sm1::max_keylen` less the relid. A non-unique entry
  cuts `encode(sk)` to fit and keeps `encode(pk)` whole. A unique
  one refuses the row with `SM_ERR_KEYTOOLONG`. A primary key's
  encoding is capped, so every secondary keeps room for its own
  columns (§10, question 3). A key is cut at the one length every
  cut key and cut bound share: 3070 bytes in a keyed table, 4086
  in a keyless one. A B-tree Index's budget is smaller, 304 and
  1320 bytes ([`BTREEINDEX.md`](BTREEINDEX.md#44-limits) §4.4).
- **The primary key index stays in PostgreSQL's catalog**, so the
  planner proves uniqueness with it and plans scans of it. digres
  turns a scan of it into a range scan of the rows (§6).
  `CLUSTER` warns and does nothing: every table is clustered.
- **The primary key index holds nothing.** Its relid is recorded
  and unused, and its size is 0. `idx_open_primary` opens it over
  the rel. A write to it fails (`need_entries`). Its INCLUDE
  columns are read from the row: `primary_value` makes an entry's
  value from each row a cursor reads.
- **`create_index` limits a primary key**: unique, on columns
  alone, every one NOT NULL, one a table, and never a B-tree
  Index, as its entries are the rows.

---

## 4. Writing

- **Insert** writes the row under its key, then each secondary:
  one write plus one per secondary. A duplicate primary key is
  sm1's `SM_ERR_DUPKEY`. A keyless row draws its `_id`, its key,
  first, and goes in by `sm1::insert_new`, with no check of the
  key.
- **An insert with arbiter indexes** (`tbl_insert_arbiter`)
  writes the arbiters' entries first and the row after them, as
  the row is the primary key's entry. A B-tree Index's entry goes
  in after its row, never before.
- **An update that leaves the primary key alone** writes the row
  at its key, and touches only the secondaries whose columns
  changed
  ([`LSMINDEX.md`](LSMINDEX.md#74-when-an-update-leaves-an-index-alone)
  §7.4). The primary key is unchanged if
  the new key's encoding is the old one's.
- **An update that changes the primary key moves the row**
  (`move_row`). It erases the old key, with a moved tombstone,
  and inserts the new one. Then it deletes and re-inserts every
  secondary entry, whose suffix changed. A duplicate of the new
  key is `SM_ERR_DUPKEY`, and the statement's savepoint undoes
  the half-done move, as it undoes any failed multi-key write.
- **A concurrent writer that finds a row moved fails.** The move
  erases the old key with sm1's moved tombstone
  (`sm1::erase_moved`,
  [`ROWSTORE.md`](ROWSTORE.md#25-a-key-change) §2.5):
  - A writer that began before the move committed fails as over
    any newer commit: `SM_ERR_UPDATED` at READ COMMITTED,
    `SM_ERR_CONFLICT` above it.
  - At READ COMMITTED the executor's re-check of the newest
    version (`tbl_read_latest`) then fails with `SM_ERR_MOVED`,
    and so does the write taken again.
  - A writer that began after the move committed fails with
    `SM_ERR_MOVED` at once.
  - digres raises `SM_ERR_MOVED` as a serialization failure.
- **Rejected: following the row.** Nothing leads from the old key
  to the new one, as a keyed row has no `_id` entry. PostgreSQL's
  cross-partition move is the precedent: it refuses to follow.
- **A row lock names the row's key**, `relid ‖ encode(pk)`. sm1
  hashes the key, so the lock holds a fixed-width id and no key
  bytes ([`LOCKMGR.md`](LOCKMGR.md#13-what-a-lock-names) §1.3). A
  move erases the old key, which is
  a write on it, so it waits for a lock on the old key as any
  write does. It inserts the new key, which it then holds as a
  writer. So a `FOR UPDATE` lock never loses its row to a move.
- **Delete** erases the row and each secondary entry.

---

## 5. Reading

- **By primary key**: a point read is one lookup. A range is a
  forward scan of the rows, one sorted run after VACUUM.
- **By `_id`**: a keyless table answers in one lookup, its key. A
  keyed table has no `_id` to read by
  ([`ROWSTORE.md`](ROWSTORE.md#24-calls-by-key) §2.4), and
  its calls by `_id` fail with `SM_ERR_INVALID`.
- **Through a secondary index**: the entry gives `encode(pk)`,
  and the row is a second lookup.
- **A sequential scan** (`tbl_scan_rows`) walks the rows in key
  order: the primary key's, or `_id`'s in a keyless table.
- **ANALYZE samples a keyed table by key ranges** (§10, question
  6; [`ROWSTORE.md`](ROWSTORE.md#26-reading-many-rows) §2.6,
  [`STATISTICS.md`](STATISTICS.md#94-the-sample) §9.4):
  1. Cut the table's bytes into slots of 8 KB. A slot's rows lie
     between the keys at its ends, which `rel_keys_at` finds from
     sm1's size estimates.
  2. Read a pilot of 30 slots, for the rows a slot.
  3. Read the whole table if the budget, a multiple of the rows
     wanted, covers it.
  4. Else read more slots, each round through one forward cursor,
     while the rows fall short and the budget lasts.
  5. Take the row count as the rows read per byte the slots span,
     times the table's bytes. With no byte spanned, take the rows
     a slot times the slots.

  The table's bytes are its newest versions' (`tbl_size` with
  `old` false), as a slot's span (`tbl_range_size`) counts no
  older version.
- **ANALYZE samples a keyless table by runs of `_id`s.** The row
  count is the live fraction times the limit.
- **Correlation is measured in the rows' own order**, the primary
  key's. So the key's leading column comes out at 1, and the
  planner costs a range over it as the sequential read it is.
- **TABLESAMPLE** of a keyed table picks slots of its bytes the
  same way. SYSTEM reads each slot by a seek to it. BERNOULLI
  reads blocks of 256 rows of a scan in key order. Of a keyless
  table, it picks blocks of 256 `_id`s.
- **The index build's backfill** walks the rows in key order
  (`tbl_scan_rows`), one sorted run. It reads each row, locks it
  at its key, and resumes from a key. A row moved since the scan
  needs nothing: its mover made its entry. A duplicate it finds
  is reported as the index and one row's key, which
  `sm0::dupkey_t` and the persisted index state carry
  ([`ROWSTORE.md`](ROWSTORE.md#27-duplicates-and-lock-status)
  §2.7).
- **Bulk load in file mode** sorts the row stream by key and
  checks it for duplicates, as it does a unique index's stream
  ([`BULKLOAD.md`](BULKLOAD.md#53-sorting-an-index) §5.3, §5.4).
  A keyless table's rows arrive in
  `_id` order. A table with a unique B-tree Index takes no file
  load: its rows are inserted (`tbl_load_open`).
- **TOAST is independent of the row's key**: a chunk's key is its
  own id and sequence number, not the row's
  ([`TOAST.md`](TOAST.md#2-design) §2). So a
  move leaves the row's chunks where they are.

---

## 6. digres

- **A row is named by its key**: its primary key's columns'
  values, or a keyless table's `_id`. Nothing is a TID
  ([`ROWSTORE.md`](ROWSTORE.md#10-digres) §10). The system column
  SQL reads is an `int8`,
  `_id` (§10, question 1), of a keyless table alone.
- **A scan of the primary key index is a scan of the rows.**
  `smstore.c`'s `open_probe()` sees the index is the primary key
  and opens a range scan of the table over the bounds
  (`sm_tbl_idx_scan_rows`), in place of an index cursor. An
  index-only scan of it reads the rows too.
- **A secondary index scan reads the row by `encode(pk)`**, from
  the entry (`sm_idx_scan_row`, `sm_tbl_read_key`).
- **A sequential scan reads the rows in key order**
  (`sm_tbl_scan_rows`), not in insertion order. So digres's tests
  compare unordered results as multisets
  (`../dg/test/rowset.py`).
- **A write names its row by its key**: `sm_tbl_update`,
  `sm_tbl_delete`, `sm_tbl_lock` and their kin take an
  `sm_row_key_t`, and sm3 writes at the key with no read
  ([`ROWSTORE.md`](ROWSTORE.md#24-calls-by-key) §2.4). The
  executor's fetch of a row a scan of
  the statement just handed out takes it from the scan's pin,
  with no read (`smstore.c`'s `scan_at()`). The scan is found by
  the key's encoding (`sm_tbl_row_key`, `sm_rel_scan_key`,
  `sm_idx_scan_row`). The key survives a sort or a hash.
- **No bitmap scan is planned** of a table in sm (§10, question
  6): an index in sm has no `amgetbitmap`, and `smtable.c` no
  `scan_bitmap_next_tuple`. A bitmap of `_id`s is no sorted read,
  as one of TIDs is in PostgreSQL: each `_id` is a lookup of its
  own. A secondary index scan reads its rows by key already. What
  is lost is combining indexes, BitmapAnd and BitmapOr, as InnoDB
  and CockroachDB do without. An OR over two indexes is a
  sequential scan.
- **A keyed table has no TID scan** but `WHERE CURRENT OF`'s,
  which reads by the cursor's row's key
  (`../dg/src/backend/optimizer/path/tidpath.c`): it has no `_id`
  to name ([`ROWSTORE.md`](ROWSTORE.md#101-system-columns)
  §10.1). A keyless table's TID scan reads
  by `_id`, its key: one lookup. digres has no `ctid`.
- **ADD PRIMARY KEY, DROP PRIMARY KEY**, and a change of a
  column's type that converts its values, rewrite a table that
  has rows (§8).
- **Foreign keys** that reference the primary key look the parent
  up by a point read of its rows, and lock its key.

---

## 7. Costs

- **A move rewrites all the row's index entries.** PostgreSQL's
  update of a key column is not HOT, so it adds an entry to every
  index too, but it erases none: VACUUM does that.
- **A statement that shifts primary keys can collide with
  itself.** `UPDATE t SET k = k + 1` reads the rows in key order
  and checks each new key at its write, as every unique index is
  checked. So the first row's new key is the second row's old
  one, which is still there: a duplicate. PostgreSQL checks at
  the write too, and passes only when its heap holds the rows in
  the other order. The SQL standard checks at the statement's
  end, which nothing in the stack does. This is accepted, as
  InnoDB and MyRocks accept it. Nine statements of pgregress's
  `foreign_key` are taken out for it.
- **Secondary entries carry `encode(pk)`**, wider than 8 bytes
  for most keys, so secondary indexes grow. A wide primary key
  leaves less of `max_keylen` to a secondary's columns.
- **Adding or dropping a primary key rewrites a table that has
  rows**, and its indexes, under the table-oid lock X. PostgreSQL
  builds only an index.
- **ANALYZE and TABLESAMPLE estimate from the store's sizes** in
  a keyed table, which RocksDB approximates
  ([`ROWSTORE.md`](ROWSTORE.md#28-costs) §2.8).
- **A row lock costs a hash of the row's key.** Two keys of a
  store that hash alike are one lock: a false conflict, never a
  missed one ([`LOCKMGR.md`](LOCKMGR.md#13-what-a-lock-names)
  §1.3).

---

## 8. A rewrite

`ADD PRIMARY KEY` and `DROP PRIMARY KEY` change what a row's key
is. `ALTER COLUMN … TYPE` with a conversion changes a column's
bytes, of any column, a key column's among them. So a table with
rows is copied into a new store (`rewrite_table`, `copy_rows`):

1. Take the table-oid lock X, as every DDL does.
2. Make a store, chained, and temporary if the table is.
3. Move the statement's mark past the store's making
   (`sm1::stmt_advance`), so that the copy may write it.
4. Make each B-tree Index, empty, in the new store.
5. Read the old rows in key order, one sorted run.
6. For each row, fetch its values out of line from the old store.
7. With a converter, put the column's new value into the row.
8. Insert the row under its new key, with its entry in every
   secondary index. Values out of line go into the new store
   under new pointers.
9. Put the table's record, which names the new store.
10. Drop the old store at commit (`sm1::rmstore`), as `TRUNCATE`
    swaps a store ([`CATALOG.md`](CATALOG.md#22-what-a-store-level-change-does) §2.2).

What the copy does to `_id`:

- A keyless row of a table that stays keyless keeps its `_id`.
- A row that gets a primary key loses its `_id`.
- A row of a table that loses its primary key draws an `_id`, in
  key order, from the table's sequence.

A duplicate key or a NULL in a key column fails the statement. A
duplicate gives the index and the row's key in the old store in
`sm1::errdup()`. A copy that fails leaves the table as it is: the
catalog gets the new definition only when the record is put.

Some changes need no rewrite:

- **An empty store** takes a new primary key or type by its
  record alone (`rekey`, `alter_column_type`). So `CREATE TABLE`
  with a primary key copies nothing.
- **A type change with no converter** puts the record alone.
  `retype_keeps_values` says which changes keep every stored
  value: the same type, or text and varchar to one as wide or
  wider.

---

## 9. What is where

- **sm1**: the moved tombstone, `sm1::erase_moved`, and
  `SM_ERR_MOVED` for a write or `read_latest` that meets it (§4,
  [`ROWSTORE.md`](ROWSTORE.md#25-a-key-change) §2.5); and
  `insert_new`, an insert that leaves
  out the check of its key (§10, question 6). Isolation scenario
  f11 covers a moved row.
- **sm2**: the rel is keyed by its clustering key, the primary
  key's columns or `_id`, given at its open. Its value is the
  tuple alone. `rel_insert` returns the key, and a keyless row's
  `_id`. Reads, updates, deletes and locks take the key. An
  update moves a row whose key changed. The index rel's entries
  carry `encode(pk)` (§3). The primary key opens as an index over
  the rows.
- **sm3**: the catalog's primary key is the clustering key;
  `tbl_*` by key, and by `_id` in a keyless table; the move (§4);
  the backfill, ANALYZE, TABLESAMPLE's slots and bulk load (§5);
  the rewrite (§8).
- **The C API** (`include/sm.h`):
  - Calls that read, scan, write and lock by key take an
    `sm_row_key_t` ([`ROWSTORE.md`](ROWSTORE.md#24-calls-by-key) §2.4).
  - The calls by `_id` are a keyless table's.
  - `sm_idx_scannxt` hands out a keyless row's `_id`, 0 for a
    keyed row's, and `sm_idx_scan_row` the row's key.
  - `sm_errdup` hands out the index and a row's key
    ([`ROWSTORE.md`](ROWSTORE.md#27-duplicates-and-lock-status) §2.7).
  - `sm_lock_status_t` names a row lock's table, and no row (§10,
    question 5; [`LOCKMGR.md`](LOCKMGR.md#13-what-a-lock-names) §1.3).
- **rel**: the verbs that take or return an `_id` are a keyless
  table's, and other verbs read by key.
- **digres**: §6.
- **Tests**: `sm2/test/test_cluster.cc`,
  `sm3/test/test_cluster.cc` and `sm3/test/test_altertype.cc`.

No database on disk carries over a change of this format: nothing
migrates.

---

## 10. Decided at review

Other documents cite these items as questions 1 to 6.

1. **The system column is named `_id`**, an `int8`: one name in
   SQL, in sm's calls and here. It exists on a keyless table
   alone ([`ROWSTORE.md`](ROWSTORE.md#101-system-columns) §10.1).
2. **A keyless row's value is the tuple alone**, its `_id` in its
   key. Both kinds of table have one row format and one deform
   path
   ([`ROWSTORE.md`](ROWSTORE.md#22-the-row-and-its-index-entries)
   §2.2). Rejected: a copy of `_id` in the
   value. It costs 8 bytes a row and buys nothing.
3. **A primary key's encoding is capped at 1024 bytes**
   (`sm2::max_pk_len`), which leaves every secondary at least 3
   KB of `max_keylen`. A longer key is refused at the write.
   PostgreSQL's B-tree caps an entry at about 2.7 KB.
4. **`DEFERRABLE` primary keys are refused**, at CREATE and at
   ALTER. Uniqueness is sm1's, checked at the write, as for any
   unique index, and a DEFERRABLE foreign key is refused too.
5. **`pg_locks` shows a row lock's relation alone**, its page and
   tuple NULL. A lock keeps a hash of its key, so sm cannot say
   which row it names
   ([`LOCKMGR.md`](LOCKMGR.md#13-what-a-lock-names) §1.3). A test
   of digres's
   locks a row of a keyed table (`../dg/test/server.sh`, lock
   status).
6. **A keyed row has no `_id`, and nothing reads many rows in
   `_id` order.** Sorted `_id`s are not a sequential read, as
   sorted TIDs are in PostgreSQL: each is a lookup of its own.
   - So no bitmap scan is planned (§6).
   - So ANALYZE and TABLESAMPLE sample a keyed table by ranges of
     the key, sized from the store's estimates, not by runs of
     `_id` (§5).
   - Rejected: a bitmap of sorted primary keys, fetched through
     one cursor. A secondary index scan reads its rows by key
     already, and InnoDB and CockroachDB do without a bitmap.
   - Rejected: an `_id` in every keyed row, with an index from
     `_id` to the primary key, to keep the 6-byte TID. A row
     fetched by it is two lookups, and each insert pays the
     entry's write and a check of its key.
   - Rejected: a TID as a slot in a statement's table of keys.
     digres names a row by its key, so it needs no such table
     ([`ROWSTORE.md`](ROWSTORE.md#21-a-rows-name) §2.1).
   - A keyless insert pays little for its `_id`.
     `sm1::insert_new` leaves out the check of the key, which no
     write can hold before the draw. The `_id` is drawn by a
     `seqref_t`, with no read of the sequence's name a row.

---

## 11. What is left

The Clustered Table is complete in sm. One item is left, in
digres. Other documents cite it as step 3 of this section.

**Step 3: digres's key-column ALTERs.** ADD and DROP PRIMARY KEY
([`../dg/PKDDL.md`](../dg/PKDDL.md)) and `ALTER COLUMN TYPE` of
any column, a key
column's among them ([`../dg/ALTERTYPE.md`](../dg/ALTERTYPE.md)),
run through the
rewrite (§8). A collation change does not arise: digres has one
collation, C. These are not built:

- **The dependents PostgreSQL makes again**
  ([`../dg/ALTERTYPE.md`](../dg/ALTERTYPE.md)
  §8). digres refuses a type change of a column one of these
  uses, and sm refuses it too (`find_column_use`):
  - an expression or a partial index that reads the column;
  - a foreign key on either side;
  - an identity column's sequence.
- **`ADD PRIMARY KEY USING INDEX`** is refused
  ([`../dg/PKDDL.md`](../dg/PKDDL.md)
  §3). sm has to mark an existing index a constraint's, or re-key
  by it.
