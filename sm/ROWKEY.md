# ROWKEY — a row named by its key

`CLUSTER.md` keys a table's rows by its primary key, and keeps
a hidden `_id` in every row so that PostgreSQL's executor can
go on naming a row by a 6-byte TID. This file takes the next
step (`CLUSTER.md` §10, question 6): a table with a primary
key has no `_id` and no `_id` index, and nothing in the system
is a TID. A row is named by its key, a tuple of the key
columns' values. Nothing of it is built yet; the work is on
the branch `wip-noctid`.

Paths are relative to `sm/`; digres's are under `../dg/`.

---

## 0. In brief

- **A row's name is its key**: the values of its primary key's
  columns, an array of (`Datum`, `isnull`), in the key's
  order. A table without a primary key is named by its `_id`,
  one `int8`. sm encodes a key; nothing decodes one.
- **A table with a primary key has no `_id`.** Its row is
  `relid ‖ encode(pk)` → `tuple`; it has no `_id` index and no
  `_id` sequence, and a secondary entry holds no `_id`.
- **A table without one keeps `_id`** as its hidden key,
  MyRocks' hidden primary key, and SQL reads it as the system
  column `_id`, `rowno` renamed.
- **There is no `ctid`**: no TID in a slot, in the table AM's
  calls, in ModifyTable, row marks, EvalPlanQual, triggers,
  `WHERE CURRENT OF` or `ON CONFLICT`. Where the executor
  carried the junk column `ctid`, it carries the key's columns.
- **A primary key change is a delete and an insert.** A writer
  at READ COMMITTED that finds the row moved by a concurrent
  key change fails with a serialization failure, as
  PostgreSQL's does for a row moved to another partition.
- **What it buys**: an insert writes one key less, and a
  secondary entry is 8 bytes shorter; nothing reads at random
  to resolve an `_id`; the executor's row name survives a sort
  or a hash, so no write reads a row twice.
- **What it costs**: a key change no longer follows the row;
  ANALYZE, TABLESAMPLE, the backfill and the rewrite must read
  in key order; a key wider than 6 bytes is carried in every
  row of a plan that updates, deletes or locks.

---

## 1. The key

- **The key is the primary key's columns** as the table's
  tuple holds them, not their encoding: `encode` is lossy for
  collated text, so a key is never decoded, and the values a
  read hands back come from the tuple. sm takes a key as
  `values`, `isnull` and a count, as it takes a row, and
  encodes it, which is CPU alone.
- **A keyless table's key is its `_id`**, one `int8`, encoded
  `be64(_id)` as now.
- **A key column is never NULL**: a primary key's columns are
  NOT NULL, and `_id` is drawn. `isnull` is in the signature
  for the one shape a row has.
- **A key change is a new name.** The executor names a row by
  its key as the scan read it; an UPDATE that changes it
  writes at the old name and the row ends at the new one.

---

## 2. sm

### 2.1 The row and its indexes

| | Key | Value |
| --- | --- | --- |
| row, with a primary key | `relid ‖ encode(pk)` | `tuple` |
| row, without one | `relid ‖ be64(_id)` | `tuple` |
| secondary, non-unique | `relid ‖ encode(sk) ‖ encode(pk)` | a cut flag, the INCLUDE columns, `encode(pk)` |
| secondary, unique | `relid ‖ encode(sk)` | a cut flag, the INCLUDE columns, `encode(pk)` |

- **The value is the tuple alone** in both kinds of table: a
  keyless row's `_id` is read from its key, so `CLUSTER.md`
  §10, question 2's copy in the value goes, and there is one
  value format.
- **The `_id` index is gone**, and with it relid `0xFFFE`'s
  rule, `rel_find`, `rel_find_latest` and `rel_lock_id`.
- **A secondary entry names its row by `encode(pk)`**, as now,
  less the `_id` it carried for the TID. `encode(pk)` is
  `be64(_id)` in a keyless table.
- **The `_id` sequence** is made only for a keyless table, and
  ADD and DROP PRIMARY KEY drop or make it (§2.4).

### 2.2 Calls by key

Every call that names a row takes its key (`values`,
`isnull`, `n`) where it took an `_id`: `tbl_read`,
`tbl_read_latest`, `tbl_update`, `tbl_delete`, `tbl_lock`,
`tbl_trylock`; and every call that handed one out hands out
the row, from which the caller takes the key's columns:

- **`tbl_insert`** returns nothing new: the caller holds the
  row it inserted. In a keyless table it returns the `_id`.
- **`tbl_insert_arbiter`** returns the conflicting row, read
  as it reads it now to find its `_id`.
- **A scan** hands out the row; `rel_scan(first, last)` by
  `_id` stays for a keyless table alone.
- **`sm_errdup`** reports the duplicate's key as the index
  entry holds it, not two `_id`s (§2.5).

The optional encoded `key` that `sm_tbl_update` and its kin
took from a scan (`CLUSTER.md` §6) goes: the key's values are
at hand, and their encoding costs no read.

### 2.3 A key change

- **The mover** erases the old key and inserts the new one,
  and rewrites every secondary entry, as now, less the `_id`
  entry. The erase is sm1's **moved tombstone**: a tombstone
  with a new flag, bit2, set (`sm1/SM1_DESIGN.md` §3). A
  reader sees it as any tombstone.
- **A writer that meets it** — an update, delete or lock at
  the old key whose newest version is a moved tombstone —
  fails with a new `SM_ERR_MOVED`, at every isolation level.
  At READ COMMITTED a plain tombstone is still
  `SM_ERR_NOTFOUND`, which the executor takes as a delete and
  skips. digres raises `SM_ERR_MOVED` as a serialization
  failure, PostgreSQL's "tuple to be locked was already moved
  to another partition due to concurrent update".
- **What goes**: sm3's `moved_to`, `write_followed`, the
  follow in `read_latest`, sm2's `set_id_entry`, and
  `LOCK.md`'s lock on an `_id` entry (row 2a). Its tests in
  `sm2/test/test_cluster.cc` and `sm3/test/test_cluster.cc`
  become tests of `SM_ERR_MOVED`.
- **The isolation scenarios** are untouched; scenarios of the
  moved tombstone are added.

### 2.4 Reading many rows

Nothing reads many rows of a table with a primary key in
`_id` order, since there is none:

- **ANALYZE** samples by key ranges: slots of 8 KB of the
  table's bytes, each the rows between the keys at its ends,
  found by bisecting the key range with sm1's size estimates
  (`sm2::rel_keys_at`), disk and memtables together, no flush.
  The row count is the rows read per byte the slots span,
  times the table's bytes: an estimate, close in sorted files,
  to an order of magnitude in memtables. Correlation is
  measured in key order. A keyless table is sampled as now.
  Built (`sm3/SM3_DESIGN.md` §6.4); `rowno_limit` stays in the
  statistics until a keyed table has no `_id` (step 4).
- **TABLESAMPLE** picks its blocks the same way: SYSTEM's are
  slots of 8 KB of the bytes, each read by a seek to its first
  key, the rest skipped; BERNOULLI's, which reads every row,
  256 rows of a scan in key order. Built
  (`../dg/WORKNOTE.md` step 290).
- **The index backfill** walks the rows in key order
  (`tbl_scan_rows`), and resumes from a key, not the next
  `_id`; the resume point is the builder's, in memory. Built.
- **The rewrite** (`CLUSTER.md` §8) copies the rows in key
  order, built. ADD PRIMARY KEY drops the `_id`s and their
  sequence; DROP PRIMARY KEY makes a sequence and draws an
  `_id` a row (step 4).
- **SET NOT NULL's check** walks the rows in key order. Built.

### 2.5 Duplicates and lock status

- **A duplicate key** is reported as the index and the key of
  a row that holds it, which digres reads by that key to say
  its "Key (…)=(…) is duplicated"; `sm0::dupkey_t` and the
  persisted index status carry the one key, not two `_id`s,
  and digres's hint that named the two rows goes. Built.
- **`pg_locks`** shows a row lock of a keyed table as now, its
  relation alone (`CLUSTER.md` §10, question 5); a keyless
  table's row lock keeps its page and tuple, from its `_id`.

---

## 3. digres

### 3.1 System columns

- **`ctid` is gone**: `SelfItemPointerAttributeNumber` has no
  attribute, no slot has a `tts_tid`, and `slot_getsysattr`
  answers `tableoid` and `_id` alone.
- **`_id`** is `rowno` renamed (`CLUSTER.md` §10, question 1),
  and a column of a keyless table alone: a table with a
  primary key has none, and naming it there is "column `_id`
  does not exist".

### 3.2 The row's name in a plan

- **The row identity columns are the key's columns.** Where
  the planner adds the junk column `ctid`
  (`add_row_identity_columns`, `appendinfo.c`) it adds a Var
  for each key column, named for the result relation. A keyless
  table adds `_id`. digres supports no table inheritance
  (`../dg/PLAN.md` §7), so a plan's rows are of one table's.
- **Row marks** (`preptlist.c`) do the same in place of
  `ctidN` when they lock, and `ExecAuxRowMark` keeps the
  attribute numbers of the key's columns where it kept
  `ctidAttNo`. One that locks nothing copies the row, a RECORD
  whole-row Var: PostgreSQL's re-fetch by reference fetched the
  same version again, which the copy is.
- **The scan reads the key's columns** because the plan asks
  for them, as any column; the scan's pruning
  (`scan_columns.test`) is untouched.

### 3.3 The executor

- **The table AM takes a key**: `tuple_update`,
  `tuple_delete`, `tuple_lock` and `tuple_fetch_row_version`
  take a `RowKey` — `values`, `isnull`, `n` — where they took
  an `ItemPointer`. `TM_FailureData`'s `ctid` goes; a moved
  row is `TM_Updated` with `SM_ERR_MOVED` raised as above.
- **ModifyTable** builds the `RowKey` from its junk columns
  and passes it to `ExecUpdate`, `ExecDelete` and their
  re-fetches, where it passed `tupleid`.
- **LockRows and EvalPlanQual** lock and re-fetch by the row
  mark's key; `ExecRowMark`'s `curCtid` becomes the current
  row's key, copied into the row mark's memory.
- **Triggers**: `GetTupleForTrigger` reads by key. AFTER
  events already keep their rows in a tuplestore, not by
  `ctid` (`trigger_tuples_stored`).
- **`ON CONFLICT DO UPDATE`** takes the key from the row the
  arbiter returned, and locks and updates by it.
- **`WHERE CURRENT OF`** (`execCurrent.c`) returns the
  cursor's current row's key, taken from the scan's slot; the
  TID scan is replaced by a **key scan** that reads one row by
  its key: the plan for CURRENT OF, and for `_id = n` or
  `_id = ANY (…)` in a keyless table. TID range scans go.
- **`smstore.c`'s `scan_at`** finds the scan whose current row
  has the key, and hands out its pinned row, so an UPDATE or
  DELETE of the row a scan just read reads nothing, as now.

### 3.4 What goes in digres

`ItemPointer` in the sm table AM, `smstore_rowno_to_tid`,
`smstore_tid_to_rowno`, `smstore_rowno_fits_tid`,
`nodeTidrangescan.c`, `tidpath.c`'s ctid quals, `currtid`'s
functions, `ItemPointerIndicatesMovedPartitions`' callers;
every heap path that is dead already is stripped where it
names a TID, not moved. The tests that print `rowno`, and the
pgregress files whose EXPLAIN shows `Output: ctid`, change
with them.

---

## 4. Costs

- **A concurrent key change fails the writer** at READ
  COMMITTED, where `CLUSTER.md` followed the row. OLTP keys
  rarely change.
- **A wide key is carried through the plan**: every row an
  UPDATE, DELETE or `FOR UPDATE` reads carries the key's
  columns, where it carried 6 bytes. They are columns the scan
  reads anyway; a sort or a hash below the write carries them.
- **ANALYZE and TABLESAMPLE estimate from the store's sizes**,
  which RocksDB approximates; their row count is rows per byte,
  not live `_id`s per `_id`.
- **The executor and the table AM part from PostgreSQL's**
  wherever a TID was: ModifyTable, LockRows, EvalPlanQual,
  triggers, `execCurrent.c`. A port of an upstream fix there
  is by hand.

---

## 5. Order of work

Each step leaves every suite passing.

1. **sm by key.** `tbl_read`, `tbl_read_latest`, `tbl_update`,
   `tbl_delete`, `tbl_lock`, `tbl_trylock` by key, through the
   C API (`sm_row_key_t`); sm1's moved tombstone
   (`sm1::erase_moved`) and `SM_ERR_MOVED`. Built. The `_id`
   calls stay for digres. Until step
   4 an update or delete by key of a keyed row reads the row
   for its `_id`.
2. **ANALYZE and TABLESAMPLE by key ranges** for a table with
   a primary key; the backfill, the rewrite and SET NOT NULL
   in key order; a duplicate reported by key.
3. **digres by key.** The arbiter returns its row; the row
   identity columns, the table AM, ModifyTable, LockRows,
   EvalPlanQual, triggers, `ON CONFLICT`, CURRENT OF and the
   key scan, built (`../dg/WORKNOTE.md` step 292); `ctid`
   gone, `rowno` renamed `_id` on a keyless table.
4. **No `_id` with a primary key.** The `_id` index, the
   follow, and the `_id` calls of a keyed table go; the value
   is the tuple alone; a key change writes the moved
   tombstone.
5. **The documents**: `CLUSTER.md` §0 and §2 to §11,
   `sm2/SM2_DESIGN.md` §5, `sm3/SM3_DESIGN.md` §3 and §6,
   `STATISTICS.md`, `LOCK.md`, `BULKLOAD.md`, `GLOSSARY.md`,
   `rel/REL.md`, each as its step lands, and this file marked
   built.
