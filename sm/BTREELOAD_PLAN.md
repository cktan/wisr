# BTREELOAD_PLAN: a bulk load builds a B-tree from the bottom up

**Status:** built, steps 1 to 5 below
([`BTREEINDEX.md`](BTREEINDEX.md#67-a-bulk-load-builds-the-tree-whole)
§6.7).
§6 has the measures.

This plan is for [`NEXT.md`](NEXT.md#1-next) §1 item 10. A load
into an empty
table sorts each tree's entries with the Sort
([`SORT.md`](SORT.md)), packs
them into leaves, builds the inner levels from the leaves, and
ingests the pages with the load's files. It writes no page twice
and holds no latch.

**Warning.**

- Every built entry has its in-doubt bit set, and no built leaf
  is all visible. §3.2 says why a clear bit would be wrong.
- Page 0 is written last, after the other pages are in RocksDB.
  A page 0 that names pages not yet there breaks the tree.

---

## 1. What a load does now

A load into an empty table
([`BULKLOAD.md`](BULKLOAD.md#51-two-cases-an-empty-table-and-one-with-rows)
§5.1) sends the rows
and each LSM Index's entries to files. A tree's entries do not
go to files:

- **A non-unique tree** inserts each entry by `idx_insert()`, as
  a writer does: one latch, one 4 KB page put and one queue
  entry a row. `idx_load()` in `sm2/src/rel/index.cc` does this.
- **A unique tree** makes the whole load insert its rows one at
  a time, rows and every index, as its check reads rows, and a
  file's rows are in no store yet. `tbl_load_open()` in
  `sm3/src/table/table.cc` does this.

So a `COPY` of 5 million rows into a table with one tree puts 5
million pages, about 20 GB, through RocksDB's WAL and memtable.

## 2. A worked example

```sql
CREATE TABLE t (id int8 PRIMARY KEY, email text);
CREATE UNIQUE INDEX t_email ON t USING btree (email);
COPY t FROM 'rows.csv';     -- 10 million rows
```

1. `tbl_load_open()` finds the table empty, and takes it X. It
   remakes `t_email` as one empty page 0, which also clears
   dead entries that aborted inserts left.
2. Each row goes to the heap's stream. Its entry, `email` then
   `id`, goes to the stream of `t_email`, which sorts it.
3. A duplicate `email` stops the sort with `SM_ERR_DUPKEY`, at
   the row whose run holds both, or at the merge.
4. At the close, the entries come back in key order. The
   builder fills a leaf to 90%, and starts the next one: about
   45,000 leaves of about 220 entries.
5. It keeps each leaf's first key and page id. From these it
   builds the level above, and so on, until one level fits one
   page.
6. Each page goes to a page stream of the load, in page id
   order. The load ingests the pages with the heap's files.
7. Then page 0 is written as the top level, by `page_write()`.
8. Each leaf goes on the btree reaper's queue. The btree reaper reads each
   row once, clears the bits, and sets the leaves all visible.

---

## 3. Decisions

### 3.1 How the pages get to RocksDB

| Option | For | Against |
| --- | --- | --- |
| **A. A page stream of sm1's loader** (recommended) | the pages are in a file at the bottom of the tree, as the rows are: no WAL, no memtable, no compaction; the buffer pool is not filled | a new kind of stream: keys and values as given, not versions; page 0 is a separate write |
| B. `page_write()` in batches of pages | no change to sm1's loader | each page goes through the WAL, the memtable and compaction; each needs a frame, pinned EX, and the build fills the pool |

Both remove the write per entry, which is most of the cost.
Option A is about 15 lines more in sm1 (`stream_key()` and
`stream_value()` take a raw stream as given). The loader writes
a small load as one batch
([`BULKLOAD.md`](BULKLOAD.md#57-a-small-load) §5.7), so a small
load's
pages go in that batch, in both options.

**Page ids ascend.** A file takes its keys in order, and a page's
key is `0x04 | relid | pageid`. The builder draws ids from
`sm3.tree_page` as it starts each page. It finishes a level
before it starts the next, so the ids of a level are above the
ids of the level below. Page 0's key is below them all, so page 0
cannot go in the file: it goes by `page_write()` after the
ingest.

**To check:** the drop at the open puts a range delete over the
tree's keys into the memtable. A file that overlaps it may make
RocksDB flush the memtable first. Measure this in the build step.

### 3.2 The in-doubt bit is set

Every built entry is in doubt, as an inserted entry is. A clear
bit, and a leaf set all visible, would be wrong in two cases:

1. **An abort after the close.** Tree pages are no versions, so
   the abort leaves them. The rows are dead, and an index-only
   scan would give them.
2. **An old snapshot after the commit.** The X lock goes at the
   commit. A repeatable-read transaction that began before the
   commit can then scan the tree. Its snapshot does not see the
   rows, but an all-visible leaf answers without reading them.

**The cost:** an index-only scan reads each row until the
btree reaper has read them once. PostgreSQL's `COPY` has the same
cost: its visibility map is clear until `VACUUM`.

**Recommended: queue each built leaf for the btree reaper.** `VACUUM`
reaps only the queue ([`BTREEINDEX.md`](BTREEINDEX.md#92-vacuum)
§9.2), so a leaf no one
queues stays in doubt after a `VACUUM`. The queue holds 262,144
pages, about 1 GB of tree. Leaves past that are dropped, and
their next reader queues them.

**Not in this plan:** a store made by this transaction (`CREATE
TABLE AS`, `TRUNCATE` then `COPY`) could have clear bits, if no
older snapshot can read the new store. That needs its own proof.

### 3.3 Where a tree's entries are sorted

| Option | For | Against |
| --- | --- | --- |
| **A. In the loader's stream** (recommended) | one memory budget for the load, as now; the stream is declared already | a new sm1 call, `load_drain()`, gives a stream's records in order to the caller, before `load_close()` |
| B. An `sm0::sort_t` of each tree, in sm3 | no change to sm1 | a budget of each tree's own on top of the load's, so a table with 8 trees takes 9 budgets |

`load_drain(loader, stream, key, value)` is an iterator: the
first call ends the stream's input, and each call gives the next
record, as `sort_next()` does. A drained stream writes no file.

### 3.4 A unique tree's check

A tree's entry key ends with the row's key, so two rows with an
equal `email` have unequal entry keys, and a unique sort would
not stop. A unique tree's stream sorts by what an LSM Index's
unique key holds: the key columns, with the row's key after them
only if a column is NULL (`entry_key()`). The row's key goes in
the record's value. The builder puts it back on each entry key.
So the Sort's unique check is the tree's check. It stops at the
first duplicate, and reads no row.

With this, `tbl_load_open()` no longer sends a table with a
unique tree to row-by-row inserts.

### 3.5 The shape of the built tree

- **Leaves fill to 90%** of `page_limit`, as PostgreSQL's
  default `fillfactor` for a btree does. The next inserts then
  split few leaves.
- **Inner pages fill to 70%**, as PostgreSQL's do.
- **A high key is the next page's first key**, whole, as a split
  sets it. No suffix is truncated
  ([`BTREEINDEX.md`](BTREEINDEX.md#43-entries) §4.3).
- **The links:** each page's `prev` and `next` name its
  neighbours on its level. The builder draws the next page's id
  before it writes a page, so a page goes out whole, once.
- **An inner entry is a child's first key and id.** The first
  entry of a level's leftmost page has an empty key, as
  [`BTREEINDEX.md`](BTREEINDEX.md#47-a-page-holds-a-range) §4.7
  needs.
- **The top level is page 0.** A level that fits one page is
  not written to the file. Its entries become page 0, at its
  level. An index with no entry keeps the empty page 0.
- **Memory:** a level's first keys and ids, one a page. For 100
  million rows of 30-byte keys, about 450,000 leaves: about 20
  MB.

### 3.6 When the build runs

- **At `tbl_load_close()`**, before `load_close()`.
- **At the switch to inserts.** A row that goes out of line
  closes the load, and the rows after it are inserted
  (`tbl_load_add()`). The trees are built first, so those
  inserts find them.
- **Not for `CREATE INDEX`.** Its build runs beside writers, and
  stays as it is.

---

## 4. Cases

| Case | What happens |
| --- | --- |
| TOAST | as now: a load takes no row out of line; the first that goes out of line switches to inserts, after the build |
| non-unique tree | sorted by entry key; built; bits set |
| unique tree | sorted by key columns, unique (§3.4); a duplicate is `SM_ERR_DUPKEY` naming the index |
| unique, a NULL key column | the record's key includes the row's key, so NULLs are not duplicates |
| primary key | not a tree: the heap's stream, as now |
| expression, partial index | the entry is made as now (`make_entry()`); a row out of a partial index gives none |
| INCLUDE | not in a tree's entry, as now |
| cut key | a non-unique entry is cut as now; a unique key too long fails, as now |
| abort before the close | the load's files and runs are deleted; the tree is the empty page 0 of the open |
| abort after the close | the pages stay; each entry is in doubt, its row dead; a reap removes them |
| crash before page 0's write | no commit, so an abort; the ingested pages are not reachable, and stay until the store goes. Page ids are not reused, so nothing finds them |
| savepoint rollback | as now: a load in a savepoint is refused or not as [`BULKLOAD.md`](BULKLOAD.md#35-savepoints) §3.5 says; nothing changes |
| 2PC | the loader must be closed before the prepare, as now |
| two writers | none: the table is X, or temporary |
| old RR snapshot | reads rows; no leaf is all visible (§3.2) |
| SSI | the heap and every full tree entry key report their writes; a committed reader can retain an SIREAD after releasing the table lock |
| the btree reaper, a page queued before the open | the drop removes the page; the btree reaper finds none, or the new page 0, judged again under the latch |
| other sessions' copies of page 0 | the drop's purge and page 0's write move the frame's version, so a copy is stale and read again ([`BTREEINDEX.md`](BTREEINDEX.md#55-a-sessions-copy-of-page-0) §5.5) |
| DROP INDEX, TRUNCATE | the table is X for the load |
| temporary table | built the same way, in its instance; not queued, so `VACUUM` in the session reaps it |
| keyless (`_id`) table | the row's key is the `_id` |
| a table with rows | not built: entries go as a writer's, as now |

---

## 5. Steps

1. **Measure today.** Add a btree option to `bench/sortload.sh`:
   one unique and one non-unique tree.
   → Check: digres's and PostgreSQL's instructions a row and
   time, recorded here.
2. **sm1: a raw stream and `load_drain()`.** A stream declared
   raw puts its keys and values as given. A drained stream gives
   its records back in order and writes nothing.
   → Check: an sm1 test loads rows and pages, and reads both
   after the close; a small load does too; a drained stream
   writes no file; an abort before the close leaves no file.
3. **sm2: the tree builder.** It takes entries in key order and
   gives pages in id order, and page 0's bytes.
   → Check: `tree_check()` passes on trees of 0, 1, one leaf's
   worth, and many entries, of cut and long keys; a scan gives
   every entry back; inserts after the build split as any tree.
4. **sm3: build at the close.** Remake the tree at the open,
   lift the unique-tree exclusion, build at the close and at the
   switch to inserts, write page 0, queue the leaves.
   → Check: a `rel` test of a load into a table with both
   trees: scans, a duplicate, a NULL key, abort before and after
   the close, the switch to inserts; dg's `COPY` tests; the
   isolation scenarios pass unchanged.
5. **Measure again.**
   → Check: a load into a table with trees costs about what one
   with LSM Indexes costs, and less than PostgreSQL's.

---

## 6. Measured

`bench/sortload.sh` with `KIND=btree`: a `COPY` of 5 million
rows in no key order into an empty table with a primary key, a
unique tree on `email` and a tree on `n`. Median of 3 rounds:

| System | Instructions a row | Time |
| --- | --- | --- |
| digres, rows inserted (before) | 494,733 | 825 s |
| digres, trees built | 20,819 | 12.0 s |
| PostgreSQL 18 | 43,534 | 59.4 s |

- The rounds agree to 0.1% in instructions.
- With LSM Indexes the same load costs 27,195 instructions a
  row. A built tree costs less: its pages go to the files whole.
- **Not measured:** whether the drop at the open makes RocksDB
  flush a memtable before the ingest (§3.1).
