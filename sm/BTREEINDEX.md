# BTREEINDEX: B-tree Index

The B-tree Index keeps a secondary index as a B+tree of its own
pages, so that a scan by the index costs near what a PostgreSQL
btree scan costs. An index is a tree when `CREATE INDEX ... USING
btree` makes it. The default kind stays `lsm` (LSMINDEX.md). The
page format and the tree are in `sm2/src/tree/`: `page.h`,
`page.cc`, `tree.h` and `tree.cc`. The index over a tree is in
the tree paths of `sm2/src/rel/index.cc`. The row check, the
unique check and `VACUUM` are in `sm3/src/table/table.cc`. The
cleaner is in `sm3/src/cleaner/`.

In short: a Lehman and Yao B-link tree. Its pages are values in
RocksDB, and frames in the Buffer Manager's pool (BUFMGR.md).
Every change writes a page whole, so there is no dirty page, no
page-level WAL and no checkpoint.

**Nothing keeps compatibility before production.** A database
made before a change of a page's format is made again with
initdb.

---

## 1. Why

An index scan must cost digres no more than 1.5 times what it
costs PostgreSQL. Reads by the primary key meet this, through
RocksDB. Reads by an LSM Index do not. Instructions a statement
on the old bench's queries (`../bench.old/DESIGN.md` §5), both
servers pinned (`../dg/WORKNOTE.md` steps 333 to 336):

| Query | digres | PostgreSQL | Ratio |
| --- | --- | --- | --- |
| a row by its primary key | 48k | 50k | 0.96 |
| Q7, 100 rows by primary key | 976k | 802k | 1.22 |
| Q10, the same rows by `c2` | 2,946k | 802k | 3.67 |
| Q9, 100 rows of a `c2` range | 1,345k | 377k | 3.57 |
| 10 rows of a range of an uncorrelated column | 432k | 258k | 1.67 |

PostgreSQL pays the same for Q10 as for Q7: a descent of a
secondary btree costs what a descent of the primary key costs. An
LSM Index pays about 30k instructions more a probe:

- a RocksDB iterator made and destroyed for each probe, about
  10k;
- its `Seek` through every level and the index block's binary
  search, by sm's comparator, about 12k;
- sm2 and sm3, about 8k.

An entry of a range costs 2,820 instructions against PostgreSQL's
695. RocksDB's `Next` alone is 1,110 of them. So one RocksDB key
an entry cannot reach PostgreSQL, whatever the levels above it
do.

**Decision.** A secondary index, unique or not, may be a B+tree
of its own pages, beside the LSM Index. The primary key stays the
rows' RocksDB keys. A read by key is at PostgreSQL's cost
already, and the rows' MVCC, locks and GC stay sm1's.

---

## 2. Decisions

### 2.1 Terms

- **A tree** is one index of kind btree. It is one rel of its
  table's store, named by the index's relid.
- **A latch** on a page is an EX pin of it in the Buffer
  Manager's pool. A pin is a latch, SH or EX, and lasts only as
  long as a latch does (BUFMGR.md).
- **A settled row**: the row's newest version committed at or
  below the GC watermark (GC.md), so every snapshot sees that
  version and no other. A row whose newest version is a delete is
  settled and gone.
- **A dead entry**: its row is settled, and is gone or does not
  have the entry's key. No snapshot can read the row by this
  entry.
- **A settled and live entry**: its row is settled and has the
  entry's key. Every snapshot sees the entry.
- **An entry in doubt**: its in-doubt bit is set, so a cleaning
  must read its row (§8.2).
- **A cleaning** judges the entries in doubt of one leaf (§9.1).
  `VACUUM` and the cleaner both do it.

### 2.2 The tree

- **Any secondary index may be a tree**, unique or not. The kind
  is chosen when the index is made, and kept for its life
  (LSMINDEX.md). A tree enforces uniqueness itself (§7).
- **A primary key is never a tree.** Its entries are the rows'
  RocksDB keys. A row stays in sm1's MVCC store, and a tree holds
  only keys that lead to rows.
- **The row decides visibility.** An entry carries no xid, as a
  PostgreSQL btree tuple carries none. A scan reads the row by
  its key at its snapshot, and checks that the row still has the
  entry's key. An entry whose row does not is skipped, and
  removed later by a cleaning (§9).
- **One exception:** a leaf with the all-visible flag answers an
  index-only scan with no row read (§5.4, §8).
- **A change to a tree is a top-level transaction of its own.**
  An entry's insert, a split, an in-doubt bit and a removal each
  commit when they are made, whatever the user's transaction then
  does. An abort does not take an entry out. The row, which the
  abort does take out, makes the entry dead.
- **The entry follows the row**, in the same statement (§6.3). So
  a committed row always has its entry, and a crash before the
  commit leaves only a dead entry.
- **Pages are keys of the table's store**, under a class byte of
  their own, so a store stays one column family. A change's pages
  are one RocksDB `WriteBatch`, in RocksDB's WAL: it is atomic
  and durable with nothing of ours to recover.
- **A page is written through.** A frame always equals RocksDB's
  page, so a frame is never dirty. sm has no write-back of its
  own, no flush ordered against the WAL, no checkpoint. RocksDB's
  memtable and WAL are the write-back. Any unpinned page may be
  dropped at any time, and a miss is a `Get`.
- **Lehman and Yao's B-link tree.** Each page has a right link
  and a high key, so a split never blocks a descent. A reader
  pins one page at a time, SH. A writer pins EX.
- **A read writes nothing shared on page 0**, which every probe
  reads: a session descends from its own copy (§5.5).
- **No page is ever freed**, and no page id is reused. Much of
  the design rests on this: a stale page id, a stale copy of page
  0 and a cursor's leaf all stay safe to read.

### 2.3 Serializable

A scan registers its whole requested range as an SIREAD of its
own, by `sm1::ssi_read_range`, under the index's relid. An insert
reports its entry's key by `sm1::ssi_write_key`. A delete, or a
key's change, reports its old entry's key the same way: a
serializable index-only scan that read no row holds only its
range's SIREAD, which a row's write alone does not meet. The keys
need not be sm1's to be tracked. A scan stopped by `LIMIT` keeps
the whole range, and may fail a writer it need not.

---

## 3. A worked example

Page 42 is an all-visible leaf of `t_status`. It holds `('open',
7)`, its in-doubt bit clear:

```sql
CREATE TABLE t (id int8 PRIMARY KEY, status text);
CREATE INDEX t_status ON t USING btree (status);
UPDATE t SET status = 'done' WHERE id = 7;
```

1. The `UPDATE` writes the row.
2. The `UPDATE` latches page 42, sets the entry's in-doubt bit,
   clears the leaf's all-visible flag, and writes the page once.
3. The `UPDATE` lets go of the latch, and queues page 42.
4. The `UPDATE` inserts `('done', 7)` on page 57, its in-doubt
   bit set, and queues page 57.
5. The `UPDATE` commits, and the GC watermark passes its commit.
6. The cleaner takes the queue.
7. The cleaner cleans page 42. It reads row 7 alone: only that
   entry has its in-doubt bit set. The row is settled, and its
   key is `'done'`, so the cleaner removes the entry. No in-doubt
   bit is left, so it sets the leaf all visible.
8. The cleaner cleans page 57. Row 7 is settled and has the key,
   so the cleaner clears that entry's in-doubt bit. The other 159
   rows of each leaf are not read.

---

## 4. Page layout

### 4.1 In RocksDB

A page is one key and one value in the table's column family, so
a store stays one column family.

```
key   = 0x04 | relid (2B) | pageid (8B)     big-endian
value = the page's bytes, at their own length
```

- **Class `0x04`** sorts after the rows (`0x00`) and a chained
  store's classes (`0x01`, `0x02`). It compares bytewise. The GC
  filter and every scan of versions pass over it.
- **Pages of a tree sort together**, by id. An id says nothing of
  key order: a split's right page gets the next id, wherever its
  keys fall.
- **Not versioned.** One key holds the newest bytes. Older
  versions of a page are RocksDB's to drop at compaction.
- **Compressed by RocksDB**, LZ4 in its blocks of 16 KB
  (`store_block_size`), and held uncompressed in a frame. A miss
  reads with `fill_cache` false, so the block cache keeps no
  second copy.

### 4.2 A page's bytes

```
+--------------------------------------------------------+
| header, 32 bytes                                       |
|   level   1B   0 a leaf                                |
|   flags   1B   see below                               |
|   max     2B   slots it has, a multiple of 16          |
|   top     2B   slots in use, the entries; top <= max   |
|   version 8B   this page's changes                     |
|   prev    8B   left sibling's page id, 0 none          |
|   next    8B   right sibling's page id, 0 none         |
|   hlen    2B   high key's length                       |
+--------------------------------------------------------+
| high key, hlen bytes; none on a level's rightmost page |
+--------------------------------------------------------+
| slots: max offsets, 2B each, from the array's end;     |
| the first top are in key order, the rest zero          |
+--------------------------------------------------------+
| entries, in no order, end to end to the page's end:    |
|   klen 2B | key | vlen 2B | value                      |
+--------------------------------------------------------+
```

Integers in the header are little-endian. The RocksDB key is
big-endian so that it sorts. These integers are read, never
compared. `version` counts the page's changes. Only tests read
it: no code branches on it.

`flags` has three bits:

| Bit | Name | Set when |
| --- | --- | --- |
| 0x01 | `page_has_high` | the page has a high key: every page but a level's rightmost |
| 0x02 | `page_incomplete` | the page has split, and its parent has no entry for the new right page yet |
| 0x04 | `page_all_visible` | a leaf has no entry in doubt (§8.3); never on an inner page |

`page_incomplete` is set by a split's first write and cleared by
its second (§6.4). A reader ignores it, as `next` reaches the new
page. A writer that latches a flagged page finishes the split
before its own change, which repairs a crash between the two
writes. Page 0 is never flagged.

**Why the flag exists.** A split writes page P twice: once
flagged, and once unflagged with its parent's new entry. The flag
marks the gap between the two writes. A split has two writes
because a full parent must split too, so one write would need a
pin on every level up to the root. A pin is a latch, and a
session may hold five. A root split has no parent to wait for, so
it is one write, with no flag. A parent with room needs no second
write either: see §12.1.

**A tree and a leaf, as a picture.** An index on `status`, with
three leaves and a root:

```
                 page 0, the root, level 1
         +----------------------------------------+
         | ""->17  |  "done"->23  |  "open"->31   |
         +----------------------------------------+
              |            |              |
              v            v              v
  level 0  page 17 <----> page 23 <----> page 31
           high "done"    high "open"    no high key

  page 23, a leaf:
  +-----------------------------------------------------+
  | header: level 0, top 3, prev 17, next 31            |
  | high key: "open"                                    |
  | slots, in key order:  [ 28 ][ 56 ][ 0 ]             |
  |                          |     |    |               |
  | entries, in no order:    |     |    v               |
  |   offset 0:  ("fail", 2) <----------+               |
  |   offset 28: ("done", 7) <---+                      |
  |   offset 56: ("done", 9) <---------+                |
  +-----------------------------------------------------+
```

An insert appends its entry and shifts only the slots. The
offsets here are for the picture: a real entry's length depends
on its key.

**A page has a limit, not a size.** `page_limit` is 4 KB: past it
an insert splits the page. RocksDB stores a page at its length,
with no free space, as every change writes it whole. A frame of
the pool is `sm1::page_size`, 4 KB, and `page_limit` may not pass
it (`tree.h` asserts this). The limit is under the store's block
size, 16 KB, so a page's `Get` reads one block. Padding a page
would cost writes and memory for nothing.

**There is no gap between slots and entries, but the slots keep
room.** A new entry is spliced into a new copy of the bytes by
`page_insert_bytes()`, with no entry decoded: it goes last, and
its slot goes in key order. Each slot holds its entry's offset
from the end of the slot array. So growing the array, 16 slots at
a time, moves the entries up with no slot changing. Slots past
`top` are zero, but zero marks nothing, as the first entry's
offset is 0: `top` alone says which slots are valid. A page's
stored length counts its unused slots, so `entry_max()` leaves
room for a half's 30 bytes of them. The slots keep this room so
that a page can change in place in a frame later. No code does so
yet: a change writes a new copy.

The slots let a page be searched where it lies: a binary search
reads the page's bytes in its frame, with no decode and no copy.
A full leaf holds about 200 `c2` entries: three levels for a
million rows.


### 4.3 Entries

**A cut key** is a key whose columns are too long for an entry.
The entry holds only their leading bytes, then the row's key, and
its cut flag is set. Two long values can then look equal in the
tree, so a scan compares the whole value on the row, and an
index-only scan cannot skip a cut entry's row. A unique index
never cuts: too long a key fails with `SM_ERR_KEYTOOLONG`. §4.4
gives the limits.

| Page | Key | Value |
| --- | --- | --- |
| leaf, level 0 | the entry's sm2 key: relid, `encode(key columns)`, then the row's key | 3 bytes: its flags, cut (`0x01`) and in doubt (`0x02`), then the key columns' length, 2B little-endian |
| inner, level 1 and up | its child's low key; empty on a level's leftmost page | the child's page id, 8B |

- **Keys are distinct byte strings, compared bytewise.** A leaf
  entry ends with its row's key, so equal column values need no
  tiebreak.
- **The key columns' length** tells the row's key from the
  columns, at the entry key's end.
- **A leaf holds no MVCC.** It holds no xid and no timestamp. The
  row decides what is visible, but for §8's flag.
- **A leaf holds no INCLUDE column.** They are read from the row,
  which is read anyway: an entry newer than the reader's snapshot
  could not give them.
- **A unique index's entry carries the row's key too**, as equal
  keys of dead rows sit beside the live one (§7).
- **A separator is a whole key.** There is no prefix compression,
  no suffix truncation and no deduplication yet. All three are
  wanted (§12.2).

### 4.4 Limits

| | Value |
| --- | --- |
| page limit | 4,096 bytes |
| header | 32 bytes |
| unused slots a half may carry | 30 bytes |
| largest entry, `entry_max()` | (4,096 - 32 - 30) / 3 = 1,344 bytes |
| longest key, as it must fit an inner entry too | 1,330 bytes |
| key columns before a cut, a table keyed by a primary key | 304 bytes |
| key columns before a cut, a keyless table | 1,320 bytes |
| levels | 256 |
| page ids | 2^64, and never reused |

An entry is at most a third of what the header and a half's
unused slots leave, as PostgreSQL limits a btree tuple to a third
of a page. A half of a split holds a high key and two entries, so
a split always finds a point where both halves fit, with the
entry that caused it. The split is made over the page's entries
and that one, as `_bt_findsplitloc()` does, at the point nearest
half the bytes where both fit.

The key columns' budget keeps room for the longest row key:
`budget()` in `sm2/src/rel/index.cc` takes off `sm2::max_pk_len`,
1,024 bytes, or `sm2::id_len`, 8 bytes, whatever the row's key
really takes. A longer key is cut and flagged
(`LSMINDEX.md` §2.4). A unique index's key is never cut,
and is refused with `SM_ERR_KEYTOOLONG`, where an LSM Index takes
4,096 bytes (`sm1::max_keylen`).

**An open and a check.** Every read opens a page, by
`page_view_t::open()`, checking its header and slots alone. A
writer also runs `check()` before it changes a page: the entries
lie end to end from the slots to the page's end, and each is
named by one slot.

### 4.5 The shape of a tree

```
 page 0   [ ""→17 , m→23 ]                  level 2, the root
              |       |
 level 1   [17] <--> [23]                   next / prev links
           /  \       /  \
 level 0 [4]<>[9]<>[31]<>[12]               leaves, in key order
```

- **Page 0 is the root, for life.** Its key is `relid | 0`, so a
  descent starts with no lookup of where the root is.
- **Every level is a doubly linked list**, by `next` and `prev`.
  The links, not the parents, are what a reader relies on: this
  is B-link's reason for being.
- **Other page ids mean nothing.** They come from one sequence
  for the database, so a tree's ids are sparse. Holes cost
  nothing, as RocksDB stores only the keys that exist.
- **0 means "none" in a link.** The root is nobody's child or
  sibling, so no link names page 0.
- **Height is the root's level + 1.**
- **Only page 0 ever changes level.** Any other page keeps its
  level for life. No page is merged or freed, and no id reused.
  `tree_create()` first drops every page under its relid, so a
  tree made again starts empty.

### 4.6 Page ids

- **One sequence for the database**, `sm3.tree_page`
  (`sm2::tree_page_seq`), starting at 1, so page 0 is never
  handed out. Ids need only be unique within a tree, and a
  sequence of the database has no table's lifetime to follow.
- **Made in `cat_init()`** (`sm3/src/catalog/ddl.cc`), beside
  `sm3.oid`. Its name starts `sm3.`, so `reserved_seq()` keeps it
  from users. sm2 names it, as sm2 draws from it.
- **Drawn by a `seqref_t`**, looked up at an open tree's first
  split. A root split draws two.
- **Why it is safe:** `sm1::nextseq` never hands a value out
  twice, across restarts too. A crash only skips values, and an
  abort gives none back.

### 4.7 A page holds a range

- **A page holds a half-open range of keys**, `[low, high)`. Its
  high key is the least key it may not hold: its right sibling's
  first key, at the split that made it. A reader moves right
  while its key is at or past the high key. Every key of a tree
  is distinct, so no rule is needed for equal keys.
- **An inner entry is its child's low key and the child's page
  id.** The first entry of a level's leftmost page has an empty
  key, below every key.
- **A descent takes the last inner entry at or below the key.**
- **`prev` exists for a reverse scan later.** Nothing reads it
  yet but `tree_check()`. A split keeps it right in its first
  write: the old right sibling is latched under the left page's
  latch, left before right, and read anew.

---

## 5. Reads

### 5.1 A descent

`descend()` finds the page at a level that covers a key, from
page 0, one page pinned at a time:

1. If page 0 is above the level, read it from the session's copy,
   with no pin, while the copy is good (§5.5).
2. Else pin the page SH. Let go of the page before it first: a
   reader holding a page while it waits for a child could wait on
   a writer that waits for the page it holds.
3. If the key is at or past the page's high key, move right by
   `next`, and go to step 2.
4. If the page is at the level, return it.
5. Else follow the child that covers the key, and go to step 2.

The store's checks run once a descent, by `sm1::page_enter()`,
not once a page. A page comes from the pool, and a miss is a
`Get`. A pin says whether the page is inner, as the pool ages
leaves faster (BUFMGR.md). The root is taken to be inner, and
below it the page above says.

### 5.2 A range and a cursor

A cursor (`cursor_t`) reads a tree in key order, forward only.

1. The first batch descends to the leaf of its lower bound.
2. `fill()` copies the leaf's entries in range end to end, one at
   a time in key order, and lets go of the leaf. The leaf is not
   held while rows are read.
3. If the leaf was not all visible, `cursor_next()` queues it for
   the cleaner (§9.5).
4. `cursor_next()` hands out entries from the copy. They stay
   valid until the copy is replaced: `cursor_held()` counts the
   entries left in it.
5. The next batch reads that leaf again, past the last key handed
   out, and moves right. A split meanwhile hides nothing, as no
   page is ever freed and a key only moves right.
6. If that leaf is page 0, a leaf no longer, the cursor descends
   again from the last key.

A leaf with nothing in range is passed to the right. A cursor
stops at the upper bound, or at a high key past it.

**An IN list** aims one cursor from probe to probe, keeping its
buffers (`cursor_rebound()`). digres opens a probe a scan, each
descending from the session's copy of page 0.

### 5.3 Each entry's row

sm3's tree cursor, which `tbl_idx_scan_rows` opens, reads an
entry's row by the entry's key, at the transaction's snapshot. It
hands the row out only if the row still gives this entry's key:

1. `tree_fill()` takes the next entries, in a batch that starts
   at 1 and doubles to `tree_batch_max`, 64. The batch views
   the cursor's copy of one leaf, with no copy of its own, so it
   also ends where that leaf's entries end
   (`sm2::idx_scan_held`).
2. It reads their rows in one call (`sm2::rel_read_batch`,
   `ROWSTORE.md` §2.1). A single row is read into the cursor's
   pin. Rows of sorted, close keys are read by one cursor.
   `tree_scattered()` says, from `ANALYZE`'s correlation, whether
   to skip that cursor.
3. `tree_next()` skips an entry whose row the snapshot does not
   show.
4. `still_has()` makes the row's entry as a write would, the
   evaluator asked for an expression or a predicate, and compares
   the bytes with the entry's key. Equal is a match. A row a
   partial index does not take has no entry, so it is no match.

A cut key is made cut, so `still_has()` compares it as any key. A
scan bound that was cut sets `widened`, and the caller rechecks
what it gets (`LSMINDEX.md` §2.4).

### 5.4 An index-only scan

**WARNING: a cut entry's row is always read, on any leaf.** A cut
key does not give its columns back.

An index-only scan skips an entry's row only when all of these
hold:

- digres asked for it: the index is a tree of key columns alone
  (`indnatts == indnkeyatts`), and it called
  `sm_rel_scan_entries()` (`tbl_scan_entries_only()`). An index
  with INCLUDE columns reads them from the row.
- the leaf was all visible when the batch was copied
  (`idx_scan_visible()`);
- the entry is not cut;
- the index's key columns decode from its keys
  (`idx_decodable()`).

`tree_fill()` tests these for each entry. An entry handed out
unread names no row, and `tbl_scan_cols()` decodes its columns
from its key. The algorithm:

1. Take the snapshot.
2. For each leaf in the range, with no latch, copy its entries,
   and note its flag.
3. On a leaf all visible, hand out its entries in range, with no
   row read.
4. On any other leaf, read each entry's row at the snapshot, and
   hand it out if the row has the entry's key.

**Why a scan needs no latch.** It reads a leaf after taking its
snapshot, and a writer clears the flag before its commit: an
in-doubt bit is written as an entry's insert is, in RocksDB's WAL
before the user's commit. So a snapshot that includes the delete
reads the leaf with the flag clear.

**The planner is unchanged.** `estimate_rel_size()` gives an sm
table `allvisfrac` 1, which is true of an LSM Index's entries,
not of a tree's (§12.1).

### 5.5 A session's copy of page 0

Every descent reads page 0, so its pin, a count in its frame's
state word, was a word every probe on every core wrote. A session
now holds a copy of page 0, shared with the other sessions that
read it. It descends from the copy with no pin while the page is
as it was.

- **A frame's version** (`sm1/src/bufpool.cc`) is a count moved
  by each change of the frame's page or bytes: an install, a
  discard, a purge, an eviction. All are under an exclusive pin.
  It sits in the descriptor's line, which still fits 64 bytes: a
  frame's bytes are found by its index, not a pointer.
- **The copy** is an immutable `bufpool_copy_t`: the page's bytes
  and the frame's version, held by `shared_ptr`.
  `sm1::keep_page_copy()`, with the page pinned, takes the copy
  the frame keeps for this version (`bufpool_t::share()`). If
  there is none, it makes one and publishes it in the frame's
  slot, a `std::atomic<std::shared_ptr>` beside the descriptors,
  for the next session. The session holds its `shared_ptr`, its
  frame, store and key. `sm1::page_copy()` hands the bytes back
  while the frame's version is unchanged, read with no pin:
  frames last the pool's life. A probe with a good copy changes
  no count.
- **The frame lets its copy go** when its page changes or leaves,
  under an exclusive pin (`drop_copy()`). A session holding the
  old copy keeps it until its next refresh or its close, and the
  last holder frees it. So memory is about one copy a root, and a
  stale one for each session idle since the root changed.
- **A good copy ages its frame** as a pin does, its age's top bit
  set (`bufpool_t::touch()`). Without this, page 0, pinned by no
  one, ages out of the pool while it is read most, and every
  session's copy goes with it.
- **A session holds at most 32** (`page_copies_max`), by store
  and key, and drops them all for the 33rd. They live in the sm1
  session, not the tree: digres opens a table, and its trees,
  once a transaction. The list is a cache, searched in order, so
  it keeps no record of use: clearing it needs no eviction
  policy. A cleared copy costs one pin at the next descent, and a
  session rarely reads 33 trees' roots.
- **`descend()`** uses the copy when page 0 is above the level it
  descends to. Otherwise, or with no good copy, it pins page 0,
  and keeps a copy if the page is above that level. So a root
  that is a leaf, and a writer posting into the root's level, pin
  it, and an insert into a root leaf makes no copy.
- **Stale is safe.** The version is read, then the copy used: a
  write between is as a write just after a pinned read. A copy
  stale by any split is as good as page 0 read just before it: no
  page is ever freed, the children it names are still at their
  level, and their right links lead on. Page 0's own split moves
  the version, so the next probe copies it again.
- **Writes** pin page 0: only a descent reads the copy.

| Case | What happens |
| --- | --- |
| page 0 changed (a post, a root split) | install moves the version: the next descent pins and copies |
| DROP INDEX, TRUNCATE, `tree_create` again | `page_drop()`'s purge moves it; a store dropped fails `page_enter()` first |
| page 0 evicted | a good copy's check ages page 0 as a pin, so it is not while it is read; if it is, the copy is not good, and the next descent misses and reads it |
| crash, restart | the pool and the copies start empty |
| a root that is a leaf | pinned, as the leaf a descent wants |
| another database, temp tables | a session's copy is by store and key, of one database; a frame's, by the frame and its version |
| two sessions refresh at once | each, pinned shared, finds the slot empty or stale; one publishes, the other takes it (compare and swap), or keeps its own if the slot moved on |

---

## 6. Writes

**WARNING: an entry goes in after its row, never before, and a
delete marks its entry after the row's write.** §6.3 and §8.6
rest on this order.

### 6.1 An insert

When a row is inserted, or an update changes an index's key, the
new entry is inserted in a top-level change, before the row write
returns.

1. `latch_leaf()` descends, and pins the leaf EX. It moves right
   with the pin coupled: the right page is pinned before the left
   page goes.
2. It checks every entry of the page. If the page is flagged
   incomplete, it finishes the split, and looks again.
3. `page_insert_bytes()` splices the entry into a new copy of the
   page, its in-doubt bit set, and clears the all-visible flag.
4. If the key is there already, `mark_in_doubt()` marks that
   entry in doubt instead (§8.2). An update A to B and back
   relies on this.
5. If there is no room, the page is decoded and split with the
   entry (§6.4).
6. `page_write()` writes the page whole, then the pin goes.
7. With no latch held, `insert_and_note()` queues the leaf for
   the cleaner (§9.5).

Then `idx_insert()` reports the key to SSI, whether the key was
there or not.

### 6.2 An update and a delete

**Nothing is removed by an update or a delete.** The old entry
stays, and its row, at the old key no longer, makes it dead until
a cleaning. A primary key's change moves the row (`CLUSTER.md`
§4): its new entries name the new key, and the old ones name a
key that holds no row.

- **A delete, or an update that changes the entry**, calls
  `idx_delete()` for the old entry after the row's write.
  `tree_mark_in_doubt()` descends to the old key's leaf through
  page 0's copy, latches it, and marks the entry in doubt. It
  writes the leaf only if the bit or the flag changes. It then
  queues the leaf, and `idx_delete()` reports the old key to SSI.
- **An entry not there**, a pending index's, leaves only the flag
  cleared.
- **An update that touches no column of the index** does nothing
  to the tree: not a key column, an INCLUDE column or a column
  the evaluator reads (`unchanged()`). For an expression index,
  an equal key with equal INCLUDE columns also does nothing.
- **An update of an INCLUDE column alone** takes the path of a
  key change, though the tree stores no INCLUDE column.
  `idx_delete()` marks the entry, and `idx_insert()` finds its
  key. The result is correct, and costs a page put if the entry
  was not in doubt.

### 6.3 Order with the user's transaction

The entry is inserted after the row's version is written, in the
same statement. So its change is in RocksDB's WAL before the
commit's, the WAL being one sequence, and a commit's sync covers
it. A row that commits always has its entry, and a crash before
the commit leaves only a dead entry.

A tree arbiter of `ON CONFLICT` follows the same order
(`tbl_insert_arbiter()`): its entry goes in last, under a
savepoint. If the key turns out another's, the row's write is
taken back to the savepoint.

A conflict lookup locks the candidate row shared, then reads
its newest version and checks its key and membership again.
An explicit lock can delay the lookup without an uncommitted
version to detect. A candidate changed while the lookup waited
starts the lookup again. The lookup rolls back and forgets its
internal savepoint before returning a successful result.

### 6.4 A split

A split of page P, S being its old right sibling and R new:

```
first write, atomic:
  R        the upper entries; R.prev = P, R.next = S
  P        the lower entries; P.next = R; flagged incomplete
  S        S.prev = R             (none if P was rightmost)
second write, atomic:
  parent   a new entry for R
  P        unflagged
```

A reader in between reaches R by P's `next` link. A crash in
between leaves P flagged, and the next writer to latch it
finishes the post before its own change. A full parent splits the
same way, and P's unflagging goes in the parent's first write.

`post()` makes the second write:

1. Clear the child's incomplete flag, in its decoded copy.
2. Descend to the level above, and latch the page there that
   covers the separator: the parent.
3. If page 0 split past this level meanwhile, go to step 2.
4. If the parent is flagged incomplete, finish its split, and go
   to step 2.
5. Insert the entry for R into the parent. If it is there
   already, a post a crash cut short made it: write the child
   alone.
6. If the parent has room, write the parent and the child in one
   write.
7. If the parent is full, split it with the entry, the child in
   the same write. If the parent is page 0, `split_root()` makes
   it a level up. Else post the parent's split a level up.

**The order of latches**, so that no two writers wait on each
other:

1. Child before parent.
2. Left before right, on a level.

A split's latch on its right sibling, for `prev`, is the second
rule. An SH pin held under an EX pin only goes up or right. A
latch is held for a page's change alone, never across a wait for
a transaction: a unique check lets every latch go first.

### 6.5 A split of page 0

A split of page 0 has no in-between:

```
one write, atomic:
  L, R     two new pages, the entries in halves, linked;
           L.prev = 0, R.next = 0
  page 0   entries for L and R, its level up one
```

So page 0 is never flagged. This is what the fixed root costs and
buys. Page 0's entries move to new pages, the one case of a key
moving other than right. Page 0's level changes, so a writer who
latches it, or a cursor who reads it again, checks the level, and
descends again if it is not what it wanted. A reader holding page
0's old bytes is still routed rightly.

### 6.6 Durability

- **Recovery has nothing of ours.** A write is in RocksDB's WAL,
  unsynced, whole or not at all, and replay restores it. A
  commit's sync makes it durable. The pool starts empty.
- **Half-done work is one state:** a page flagged incomplete,
  finished by the next writer to latch it.
- **A temporary table's pages** are in the temporary instance,
  with no WAL, as its rows are.

The cost of writing a page whole is in §10.1.

### 6.7 A bulk load builds the tree whole

A load into an empty table (`BULKLOAD.md` §5.1) does not insert
its entries. It builds each tree from the bottom up, with no
latch and one write a page (`BTREELOAD_PLAN.md`):

1. `idx_load_tree()` makes the tree empty, and declares two
   streams of the load: a drained stream for the entries and a
   page stream for the pages.
2. Each row's entry goes to the drained stream, which the
   Sort (`SORT.md`) sorts in the load's memory budget.
3. At the close, `idx_load_build()` takes the entries back in
   key order. `build_add()` fills each leaf to 90%, links it to
   the next, and gives it the next leaf's first key as its high
   key.
4. `build_end()` builds each level above from the first key and
   id of each page below, its pages filled to 70%, until a
   level fits one page.
5. Each page but page 0 goes to the page stream as it is, in
   page id order. `sm1::load_close()` puts the pages into the
   store with the rows.
6. `idx_load_root()` writes the top level as page 0, and queues
   each leaf for the cleaner.

- **Every entry is in doubt**, as an insert's is, and no leaf is
  all visible. The pages stay after an abort, and an old
  snapshot can scan the tree once the table's lock goes at the
  commit. The cleaner reads each row once.
- **Each entry's key is a write for SSI**, as an insert's is
  (§2.3). `idx_load()` reports the whole key before the Sort
  drops the row's key from a unique tree's record. A committed
  reader can keep an SIREAD after the table's lock ends.
  **Cost:** below SERIALIZABLE the call returns at once. Under
  SERIALIZABLE, each call builds a point key (one allocation),
  takes `ssi_mu`, the lock of the database, and looks the key
  up. The load already does this for each row, so each tree
  adds one more call a row. This cost is not measured. If it
  matters, reuse one key buffer, or take `ssi_mu` once for a
  batch of keys.
- **A page holds two entries at least**, so each level above
  has half the pages or fewer. A page whose high key does not
  fit gives its last entries to the next page.
- **A unique tree's entries are sorted by its key columns**, and
  the row's key only after a NULL. So the Sort's own check finds
  a duplicate, with no row read, and stops. The row's key rides
  in the record's value, and the build puts it back.
- **Page 0 is written last.** A crash before it leaves pages no
  page names, which no one reads: page ids are never reused.
- **A build that fails** cancels the load (`sm1::load_cancel`),
  and leaves the tree empty.

---

## 7. Uniqueness

### 7.1 The check

The check works as PostgreSQL's `_bt_check_unique()`. Every entry
ends with its row's key, so equal column values of dead rows can
lie on several leaves. The check therefore works on the key's
prefix, the relid and the key columns. A key with a null column
is not checked: `idx_insert_unique()` calls `tree_insert()`.

1. `tree_insert_unique()` latches the leaf that covers the
   prefix. Every insert of the prefix takes this latch first, so
   two inserts of one prefix take turns.
2. It judges every other entry of the prefix, on this leaf and to
   its right. Leaves to the right are pinned SH, the first leaf's
   latch held. An insert of the prefix waits for that latch, and
   a split only moves entries right.
3. It stops at an entry past the prefix, or at a high key that
   does not begin with the prefix.
4. `unique_tree_insert()` judges an entry by its row's newest
   version, past the snapshot (`sm1::read_newest`):
   - an entry of this row, an earlier key of it gone back to, is
     no conflict;
   - a row gone is no conflict;
   - a row written by another transaction still running: every
     latch is let go, the writer is waited for by
     `sm1::wait_writer`, and the check starts again from the
     first step. The row's key is not read: `read_newest` gives
     no row here;
   - a row committed, or this transaction's own, that has the
     key: `SM_ERR_DUPKEY`;
   - any other row: the entry is dead, and the next is judged.
5. The new entry goes in under the same latch. If its key lies on
   a leaf to the right, that leaf is latched left to right, the
   first still held.

A key there already is found by `insert_into()`, and marked in
doubt. Only an insert that passes the check queues its leaf: a
duplicate returns first.

`sm1::wait_writer` waits as a write waits for a row's uncommitted
version: it is cut short as every wait is, and is in the deadlock
check.

### 7.2 The row of a key

`idx_find` and `idx_find_latest` refuse a tree: its entries name
rows only its table reads. sm3's `tree_find()` serves `ON
CONFLICT` arbiters instead.

1. Return "no row" for a key with a null column.
2. Scan the key's entries, and read each row: as the snapshot
   shows it, or its newest version.
3. For a newest version, if a writer of the row is running, wait
   for it, and look the key up again.
4. Take the first row that still has the key.
5. For a newest version, lock the row SH, as `idx_find_latest`
   locks the key.

---

## 8. The in-doubt bit and the all-visible flag

Two marks say what a cleaning, and an index-only scan, can skip.
An entry's in-doubt bit says that its row must be read. A leaf's
all-visible flag says that no entry on the leaf is in doubt.

### 8.1 Why

- **The flag.** `bench/` q2, `select count(*) from usertable
  where state = 'CA'`, 183 rows of 10,000. An LSM Index counts
  its entries in 0.18 ms. A tree that reads each row takes 0.39
  to 0.59 ms. An LSM Index that reads the rows takes 0.50 ms: the
  row reads are the difference. PostgreSQL answers from the index
  alone after `VACUUM`, its visibility map's bits set, in 3 pages
  and 0.04 ms.
- **The bit.** With the flag alone, `VACUUM` reads the row of
  every entry on a leaf that is not all visible, in index order,
  so the reads are scattered over the rows. With the bit, a
  cleaning reads no other entry's row.

### 8.2 The in-doubt bit

The in-doubt bit is bit `0x02` of the entry's `value[0]`
(`entry_indoubt`). Bit `0x01` is the cut flag. So the entry does
not grow.

- **An insert** sets the bit, in `tree_value()`. The insert
  writes the page anyway, so the bit costs nothing. An aborted
  insert's entry therefore has its bit set, and a cleaning finds
  it.
- **A delete, and an update that changes a key,** set the bit on
  the old entry. If the bit is set already, nothing is written.
  So an entry no cleaning has judged since its insert costs no
  write.
- **An insert that finds its key** sets the bit, as its row just
  changed.
- **An update that keeps its keys and INCLUDE columns** sets
  nothing: its entry is good at every snapshot (§6.2).
- **Only a cleaning clears the bit,** under the leaf's latch,
  when the row is settled and still has the entry's key.

`page_mark_in_doubt()` changes the bit in the page's bytes, with
no entry decoded.

### 8.3 The rule for a leaf

**A leaf is all visible only if no entry on it has its in-doubt
bit set.** So every entry on an all-visible leaf is settled and
live, and every snapshot there is or will be sees it. A leaf
keeps a flag, not a count of such entries: a cleaning walks the
whole page, so a count would save nothing.

The flag is cleared by an insert into the leaf, and by a mark on
an entry of it. Only a cleaning and a split set it.

### 8.4 Why a leaf keeps the flag

The flag holds nothing the in-doubt bits do not: it is derived
from them. Its value is speed, not correctness:

- **A cleaning** skips a clean leaf by one header byte, and
  walks no entry. So does `VACUUM`'s walk of a temporary table's
  leaves (§9.2).
- **A reader** learns by one header byte whether a leaf needs
  cleaning, and tests no entry to queue it.
- **An index-only scan** decides for a whole leaf at once, and
  tests no entry.

Its cost is that it must agree with the bits:

- A split recomputes it for both halves.
- `tree_check()` reports a set leaf with an entry in doubt.
- A leaf with no entry in doubt, but its flag clear, costs a page
  put to set.

If the flag were removed, each reader above would test the
entries it holds. A scan would then skip rows by entry (§12.1),
and correctness would not change.

**A cut entry does not stop a leaf being all visible.** Its
in-doubt bit is cleared as any other's, and an index-only scan
reads a cut entry's row on any leaf (`tree_fill()` tests the cut
flag first).

### 8.5 A split recomputes the rule

`page_split()` does this for each half of a leaf split, in
`set_all_visible()`:

1. Read each entry's in-doubt bit. The page is decoded already,
   and the split walks its entries already.
2. Set the half all visible if no entry on it has the bit. That
   is the AND of each entry's "not in doubt", which is the
   inverse of the OR of its in-doubt bits.
3. Clear the half's flag otherwise.

The new entry has its in-doubt bit set, so its half is never set.
Only the other half can be. A split reads no row, and adds no
write: the flags go out in the split's first write. Setting the
flag here is safe. A writer whose row is written, and who has not
yet latched, sets its entry's bit and clears the leaf's flag when
it does, as against a cleaning (§8.6). A root split takes the
same path through `page_split()`. An inner page has no such flag.

### 8.6 Why the latch is enough

The latch is the leaf's EX pin in the pool. A writer writes its
row first, then latches the leaf to set the bit. A cleaning
judges an entry again under the latch before it clears the bit:

- If the row was written first, the cleaning reads a version not
  settled, and leaves the bit.
- If the cleaning holds the latch first, the writer waits, and
  then sets the entry's bit and clears the leaf's flag.

So a writer reads the bit latched too, though the latch costs no
write. Judged with no latch alone, a cleaning could read the old
row, and set the flag after the delete had looked. An insert that
finds its entry there moves no version of the entry. Only its
row, written first, shows the change, so only the judging under
the latch counts.

A scan needs no latch, for the reason in §5.4.

---

## 9. Cleaning: vacuum and the cleaner

A cleaning removes dead entries, clears the in-doubt bit of
settled and live entries, and sets a leaf all visible. `VACUUM`
cleans the leaves on the queue (§9.2). A cleaner thread cleans
the leaves that readers and writers queue: all of them when the
system is not busy, and about 100 pages a second when it is
(§9.9). digres has no autovacuum (`../dg/UNSUPPORTED.md`), so the
cleaner is what keeps a tree clean between `VACUUM`s. §8 defines
the bit and the flag that a cleaning works from.

### 9.1 Cleaning a leaf

`VACUUM` and the cleaner use the same procedure, `clean_page()`
and `clean_leaf()` in `tree.cc`:

1. Pin the leaf SH, copy it, and unpin it.
2. Skip a leaf set all visible.
3. With no pin held, read the row of each entry with its in-doubt
   bit set. Read no other row.
4. If any was dead or settled, or no entry is in doubt, pin the
   leaf EX, and judge those entries again.
5. Remove each dead entry.
6. Clear the in-doubt bit of each entry settled and live.
7. Leave the in-doubt bit of each entry not settled.
8. If no in-doubt bit is left, set the leaf all visible.
9. Write the leaf once, if anything changed, then unpin it.

The first judging is a filter: only the judging under the latch
counts (§8.6). It keeps a leaf that cannot change from making its
readers and writers wait on its rows.

An entry is not settled if its row was written by a transaction
still running, or committed past the GC watermark: an old
snapshot may still need the version before. An entry has no xid,
so only its row shows this.

`judge_entry()` in `table.cc` gives the verdict, by
`sm2::rel_read_settled` and `still_has()`: 1 dead, 2 settled and
live, 0 not settled. `judge_entry()` in `index.cc` takes the
row's key from the entry first. A cut entry is judged as any
other.

A leaf that splits while it is judged keeps its right half's
entries in doubt until the next cleaning.

### 9.2 `VACUUM`

`VACUUM` reaches `tbl_compact()`: it compacts the store, then
cleans each tree from the cleaner's queue alone. It does not walk
the leaves.

1. Take every page of the tree off the queue
   (`clean_queue_t::extract()`), the waiting ones too. Each
   page's filter bit is cleared as it goes, so a change after
   queues the page again.
2. Clean each page as the cleaner does (`idx_clean_leaf()`).
3. Put a page left with an entry in doubt back on the queue, to
   wait for the watermark.

A change to a leaf is a top-level change. A second `VACUUM` with
no write between reads no row.

**What `VACUUM` accepts.** The queue is a hint, and `VACUUM`
works from it alone. So it misses some leaves, and we accept
that:

- **A false positive of the filter.** A leaf whose filter bit
  another page set is not queued. It is queued by its next reader
  or writer, once that page is taken and the bit is clear.
- **A full queue.** A page that finds no room is dropped, and
  queued again by its next reader or writer.
- **A restart.** The queue starts empty. A leaf that no reader or
  writer has touched since is not cleaned by `VACUUM`. Its
  in-doubt bits are in the page, so nothing is lost: only the
  cleaning waits.
- **A leaf that changes while `VACUUM` runs.** Its page left the
  queue before it was read, so the change queues it again. The
  next `VACUUM` or the cleaner finds it.

**Why this is enough.** A leaf with an entry in doubt that
nobody reads or writes costs nothing: no scan meets its dead
entries, and it holds back no other work. A leaf in active use is
met by a reader or a writer, and that one queues it, as soon as
its page is off the queue. So a miss lasts only until the leaf is
used. A walk of every leaf would read each leaf's header, one pin
and one copy a leaf, to find a few. The queue gives the same
leaves for the cost of the queue.

**The exception.** A temporary table's leaves are never queued
(§9.5). `VACUUM` walks every leaf of its trees, left to right
from the leftmost, as PostgreSQL's `btbulkdelete()` does
(`tree_prune()`). A database opened with no cleaner does the same.

The GC filter does not touch class `0x04`, so GC never removes an
entry.

### 9.3 The buffer pool

Every change to a page goes through the pool, by the Buffer
Manager's protocol: a writer's in-doubt bit, a split's two
halves, and a cleaning's write. The cleaner is no exception for
having no transaction:

1. Pin the page EX. Other pins are waited for, and readers of the
   page wait from here on.
2. Change a copy of the page, not the frame.
3. Write the copy to RocksDB, by `page_write()`.
4. `page_write()` copies it into the frame, which cannot fail.
5. Unpin the page.

The frame changes after RocksDB does, not before. EX hides the
order from every reader, and a failed write then leaves the frame
equal to RocksDB's, with nothing to undo.

- **Row reads under EX.** The second judging reads rows under the
  EX pin, so the leaf's readers wait. Only entries that the first
  judging found dead or settled are read, and it has just read
  them, so they are usually in the block cache. A disk read under
  EX is still possible (§12.1).
- **Frames.** A cleaning pins one frame at a time, and takes no
  other pin while it holds it: row reads use no frame. So it can
  wait for a frame without deadlock, and the pool's floor by
  sessions needs no change.

### 9.4 The queue

The queue is a hint. The in-doubt bit is in the page, so a page
lost from the queue stays marked, and its next reader or writer
queues it again. A catalog (one database) has one queue,
`clean_queue_t`:

- **The queue** is a circular queue: a fixed array of 262,144
  slots, with a head and a tail, under a mutex. A page goes in at
  the tail and comes out at the head. The array is made when the
  catalog attaches, so a page in or out allocates nothing.
- **A slot** is `{store id, relid, page id, watermark}`, 32
  bytes, so the array is 8 MB. It holds the leaves of about 1 GB
  of tree. The watermark is 0 for a page that does not wait
  (§9.7).
- **The filter** is a 1-bit Bloom filter with one hash function,
  over the first three fields: 2^23 bits, 1 MB. A bit set means
  the page is on the queue, so the caller stops.
- **A false positive** leaves a page off the queue. Its next
  reader or writer tries again. With the queue full, about 3% of
  pages collide.
- **A cleaner clears a page's filter bit as it takes the page**,
  before it reads the page. So a change after that queues the
  page again. Two pages can share a bit, so this may queue a page
  twice, which costs one more look. One hash function is what
  makes this safe: with two or more, clearing a bit could hide
  other pages.

### 9.5 Queueing a page

Whoever finds a leaf not all visible queues it, reader or writer:
a cursor, an insert, a unique insert, a delete or a key change.
Each does this, by the tree's `leaf_note_fn`, with no latch held:

1. Hash the three fields, and load the filter's word. If the bit
   is set, stop. The load keeps the shared word clean on the read
   path.
2. Set the bit by an atomic `fetch_or`. If another caller set it
   first, stop.
3. Take the queue's mutex. If the queue is full, clear the bit,
   wake the cleaner, and stop. The page is dropped, and the
   oldest pages stay.
4. Put the page in at the tail.

A writer queues as well as a reader: an `UPDATE` by primary key
may leave a dead entry on a leaf that no cursor reads. An insert
that splits a leaf queues both halves, as the new entry may be in
the right one. A split of page 0 queues its two new pages. A
split made by a post to a parent queues no page, as an inner page
is no leaf.

A temporary table's pages are not queued: the cleaner cannot read
that session's instance. They keep their in-doubt bits for
`VACUUM`. `use_index()` gives a tree its `leaf_note_fn` only for
a table that is not temporary.

### 9.6 A pass

A pass handles the pages on the queue when it begins, one at a
time, up to its budget. Several workers can run passes, as each
takes its own pages. `run_pass()` opens a session with no
transaction, and reads the pass's watermark:
`sm1::gc_watermark()` plus 1, as a slot's 0 means a page that
does not wait. For each page:

1. Under the queue's mutex, take a page from the head.
2. If its slot's watermark equals the pass's watermark, the page
   waits: put it back at the tail, unread, and go to the next.
   This does not count against the budget.
3. Clear its filter bit.
4. Note `cat_publishes()`. Find the table's definition by
   `cat_get_table_by_store()`, and the index by its relid. If the
   store or the index has gone, drop the page.
5. Clean the leaf (`tbl_clean_leaf()`). A page that is no longer
   a leaf is dropped: page 0 can stop being a leaf.
6. If an in-doubt bit is left, put the page in at the tail, its
   slot holding the pass's watermark, and set its filter bit, so
   its readers do not queue it again. If the queue is full, drop
   the page, and clear the bit.

A failure drops the page: the queue is a hint, and the page's
bits are still there. `SM_ERR_NOTFOUND`, a store dropped
meanwhile, is no error to report. Any other failure is logged as
a warning.

Page ids are not reused and pages are never freed, so a stale
page id is safe to latch.

### 9.7 A page that waits

A page that waits is not read again until the watermark moves, so
a long transaction does not make the cleaner read the same rows
again and again. A page that waits costs a move from head to tail
under the mutex, and no read. The queue counts the slots that
wait.

### 9.8 No transaction

The cleaner starts no sm transaction, opens no table and takes no
lock on one. It does not need them:

- Judging an entry reads a row's newest version and compares it
  with the GC watermark. It needs no snapshot.
- A leaf's change is one `page_write()`, one RocksDB write batch,
  atomic by itself. A tree's change already commits when it is
  made (§2.2).
- So the cleaner holds no snapshot, and never holds back the
  watermark.

What a transaction and a table lock give a reader, the cleaner
gets another way:

| Need | How the cleaner meets it |
| --- | --- |
| the store stays open | its session keeps a copy of the store's handle until the pass ends: a handle goes with its last copy (`db_drop_store_cf()`). A call on a dropped store fails with `SM_ERR_NOTFOUND`, and the page is dropped |
| `CF:txn` records stay | `latest_version()` and `chain_latest()`, under every settled read, count it in the read epoch, as for every reader (`db_read_enter()`) |
| the table's definition | `table_def_t` is in memory, and is not changed once published. The cleaner notes `cat_publishes()` before it fetches the definition, and compares after each row read. If the count moved, the entry stays in doubt. A commit publishes under the catalog's mutex, and a later writer reads the definition under it, so a row written under a newer definition is read only after the count moved. A stale definition therefore judges no row |
| a key out of line | `toast_fetch()` reads by a cursor, which needs a transaction. `toast_fetch_settled()` reads the chunks by `sm1::read_settled_versioned()`, as a chunk is versioned in a chained store too. A settled row's chunks are settled |
| `DROP INDEX` | not kept out: a cleaning may write a leaf after the drop. The page stays under a relid never used again, until the store goes |

`tbl_clean_leaf()` makes a `table_t` from the definition, which
reads nothing, with `settled` true: `settled` sends
`fetch_cols()` to `toast_fetch_settled()`.
`sm1::set_no_transaction()` lets a session call `read_settled()`,
`read_settled_versioned()`, `pin_page()`, `page_enter()` and
`page_write()` with none. Every other call still needs a
transaction. A store is looked up at the newest state.

### 9.9 The workers

A database has one cleaner thread, and more when the queue
overflows. The first works as the Index Builder's worker does
(INDEXBUILD.md):

1. Sleep for a second, or until a caller finds the queue full, or
   until a detach stops it.
2. If the queue is empty, go to step 1.
3. If every slot waits, and no transaction has begun since the
   last look, go to step 1. A session here would keep the reaper
   from ever closing an idle database.
4. Set the pass's budget. On a busy system with room in the
   queue, it is `cleaner_busy_pages`, 100 pages, so about 100
   pages a second. Else it is the whole queue. A busy system is
   still cleaned: with no autovacuum, a system that is never
   quiet must not lose its cleaning.
5. Open a session with the function `start_builder()` gives the
   builder (`src/sm.cc`), and read the watermark.
6. Run a pass, close the session, and go to step 1.

`cleaner_budget()` holds the rule of the second to fourth steps.
A database has 4 workers, made when its catalog attaches. Three
sleep, and run a pass only while the queue is full. They are made
at attach, not at an overflow, as a thread that cannot start at
an overflow has no caller to tell. `sm_set_cleaner_workers()`
sets the count before a database is opened: `rel` sets 0, and
runs a pass by `(cleaner-pass)`, as its scripts count a tree's
entries.

No worker keeps a session between passes, so the reaper can still
close an idle database. A catalog's detach stops the workers and
joins them, as `builder_detach()` does. A detach ends a pass
after the leaf it is on.

### 9.10 Busy

Busy is the count of `sm3::begin()` calls on the catalog since
the first worker's last look: every transaction begins there. A
look comes a second after the last pass ends. The system is busy
if that count is above `cleaner_busy_limit`, 100. The catalog
keeps the count in an atomic counter, `begins`.

The budget also bounds a cycle on a hot leaf: an insert clears
the flag, a reader queues the page, a cleaning writes 4 KB, and
the next insert clears the flag again.

### 9.11 Space

- **Pages are not merged.** An empty leaf stays linked, as
  PostgreSQL's half-dead pages are, a tree never shrinks, and
  page ids are not reused.
- **`DROP INDEX`** drops the pages once it commits, by
  `sm1::page_drop` from an `on_commit` function, as a rollback
  must keep them. A crash first leaves them until the store goes.
- **`TRUNCATE` and a rewrite** make a new store.

---

## 10. What a tree costs

### 10.1 A page written whole

An indexed insert writes its page, at its length, up to 4 KB,
through RocksDB's WAL and memtable. An LSM Index writes an entry
of about 40 bytes. Measured, a million rows loaded into
`wide_mixed` with its `c2` index:

| Entries | Seconds |
| --- | --- |
| an LSM Index | 18 |
| a tree of 8 KB pages | 51 |
| a tree of 4 KB pages | 34 |
| a tree of 2 KB pages | 27 |
| a tree of 4 KB pages, the entry spliced into the bytes, none decoded | 27.7 |

The old bench's Q9 and Q10 stay within 2% of each other at all
three page sizes.
Logging the entry and writing the page later is what would close
the rest (§12.2).

### 10.2 Against PostgreSQL

`../bench/run.sh`'s q1 to q3 on `usertable`, 10,000 rows, one
client (`../PERF.md`). Latency over PostgreSQL's:

| Query | an LSM Index | a tree |
| --- | --- | --- |
| q1, 8 entries of `lastname`, a count | 1.41 | 1.19 |
| q2, 183 entries of `state`, a count | 2.96 | 1.29 |
| q3, 8 rows found by `lastname` | 2.40 | 1.74 |

A tree's instructions a statement are 1.05, 1.63 and 2.05 times
PostgreSQL's (`../PARITY.md` §3.2).

Writes and loads, from the old bench (`../bench.old/DESIGN.md`):
`wide_mixed`, a million rows, its `c2` index, the root at page 0
(`../dg/WORKNOTE.md` step 359):

| | a tree | an LSM Index |
| --- | --- | --- |
| the old bench's Q11, 16 clients, tps | 22,889 | 11,108 |
| a write moving a key, 16 clients, tps | 15,184 | 9,835 |
| a million rows, seconds | 66 | 12 |

- **The goal, 1.5× of PostgreSQL's instructions, is met by q1
  alone.** q2's gap is the cost of an entry, mostly the tree
  cursor's copies. q3's gap is a row's read, mostly RocksDB's
  `MultiGet` (`../PARITY.md` §4, `NEXT.md` §1).
- **A load is 5.6 times an LSM Index's on that machine**, against
  1.5 in §10.1, on both builds, and why is not known. Its profile
  is RocksDB's: LZ4 a tenth, then compaction and the memtable.
- **On a VM of 2 cores**, a load is 2.04 times an LSM Index's
  with the WAL's logs zero-filled, 1.38 with that off, and 1.13
  on tmpfs. There the extra is zeroing 64 MB a log on the write
  path, and stalls for two immutable memtables. So new logs are
  zero-filled only on WSL (`DBMGR.md` §2). That does
  not account for 5.6 times, which is still to be run both ways.

Estimated: a descent of cached pages costs about 3k instructions,
and an entry of a range about 200.

### 10.3 The in-doubt bit

For a leaf of 160 judged entries, 20 of them changed between two
cleanings:

| | A leaf flag alone | With the in-doubt bit |
| --- | --- | --- |
| Writer's page puts | 1 | 20 |
| Row reads to clean the leaf | 160 | 20 |

- A page put is not a page I/O. RocksDB is the write-back layer:
  the put is a WAL append and a memtable insert, and a flush
  writes only the page's newest version, unless a cursor's
  RocksDB snapshot holds an older one.
- §10.1 measured a put of 4 KB against an entry of 40 bytes: 27.7
  s against 18 s a million rows, so about 10 µs a put.
- A cleaning's row reads are scattered, and can each be a disk
  read. So 10 µs of sequential log traffic buys up to 7 random
  reads fewer in this example.
- The writer pays in the foreground, and the cleaner saves in the
  background. Only a bench against PostgreSQL shows whether that
  matters.
- A reader of a leaf not all visible takes one hash and one load.
  A new page adds one `fetch_or`, one mutex and an append.
- A delete, or a key's change, latches its old key's leaf, and
  descends to it through page 0's copy and the pool.
- `VACUUM` reads only the leaves on the queue (§9.2), and skips
  a set leaf among them, as PostgreSQL's vacuum skips an
  all-visible heap page.

### 10.4 Measured with the cleaner

Measured 2026-11-05, optimized builds, 200,000 rows, one index on
the changed column, one run of each, on a WSL2 machine. An update
changes the key of a random row. The first row counts the
session's own thread. The hot range is 200 rows, by 4 clients.
`VACUUM` follows an update of 1% of the rows. These runs
walked every leaf: `VACUUM` now reads the queue's leaves (§9.2),
and its time is not measured again.

| | PostgreSQL | digres, btree | digres, lsm |
| --- | --: | --: | --: |
| instructions an update | 127,854 | 187,467 | 202,617 |
| updates a second, 1 client | 668 | 569 | 585 |
| updates a second, hot range | 1,353 | 904 | 1,389 |
| `VACUUM`, seconds | 0.06 | 0.94 | 0.50 |

- A btree update costs 1.47 times PostgreSQL's instructions, and
  fewer than an lsm index's.
- **The hot range is the cost of the design:** 0.67 of
  PostgreSQL's rate, as each update puts a page under the leaf's
  EX pin.
- `VACUUM`'s time includes compacting the store, which PostgreSQL
  does not do.

---

## 11. Implementation

### 11.1 Where it sits

```
 digres   btree access method, USING btree (or lsm)
            |  scans rows through sm3's cursor
 sm3      index's kind, row check, unique check, DROP,
          VACUUM's queue pages, the cleaner
            |
 sm2      src/tree/   page.cc   bytes of one page, no I/O
                      tree.cc   descent, cursor, insert, split
          src/rel/index.cc      an index over a tree
            |  sm1::pin_page, page_write, page_drop, ...
 sm1      src/page.cc     a page's I/O
          src/bufpool.cc  frames, a pin that is a latch, the clock
            |
 RocksDB  block cache (row blocks), WAL, memtable, SSTs
```

- **sm1 owns a page's I/O**, as it is the only channel into
  RocksDB, and knows nothing of trees.
- **sm2 owns the tree**, and holds no RocksDB handle.
- **sm3 reads the rows**, in `still_has()` and `judge_entry()`.
  sm2 may not call sm3, so `tree_t` holds a `leaf_note_fn`, and
  the caller gives `dead` and `check` as functions.

### 11.2 What each level holds

| Level | What it holds |
| --- | --- |
| sm1 | class `0x04`, its comparison and the GC filter's pass-over; a page write outside every transaction; SIREAD and `on_write` on keys that are not stored; the pool; `gc_watermark()`; `set_no_transaction()`; `read_settled_versioned()` |
| sm2, `src/tree/page.*` | the page's layout, the search of its slots, an entry's insert and a split's halves, on bytes, with no RocksDB; `set_all_visible()`, for which the tree level reads a leaf entry's `value[0]`; `page_mark_in_doubt()` |
| sm2, `src/tree/tree.*` | descent, cursor, inserts, splits, `tree_mark_in_doubt()`, `clean_page()`, `clean_leaf()`, `tree_prune()` (a temporary table's walk), `tree_clean_leaf()`, `tree_check()`; `leaf_note_fn` |
| sm2, `src/rel/index.cc` | an index over a tree: the in-doubt bit in `tree_value()`, `idx_insert`, `idx_insert_unique`, `idx_delete`, `idx_scan`, `idx_scan_rebound`, `idx_scan_entry`, `idx_vacuum`, `idx_clean_leaf()`; `value[0]`'s readers test bit `0x01` for cut |
| sm2, `src/rel/` | `toast_fetch_settled()` |
| sm3, `src/table/table.cc` | `still_has()`, `tree_fill()`, `tree_next()`, `unique_tree_insert()`, `tree_find()`, `judge_entry()`, `tbl_clean_leaf()`, with no open table's lock, and `tbl_compact()`, which cleans the queue's pages; `use_index()` gives each tree its `leaf_note_fn`, but for a temporary table |
| sm3, `src/cleaner/` | the circular queue, the filter, the workers, `cleaner_budget()`, and `cleaner_run_once()` for tests |
| sm3, `src/catalog/` | `sm3::begin()` counts in `begins`; the index's kind in its definition, and every index operation branching on it (LSMINDEX.md) |
| sm, `src/sm.cc` | attaches and detaches the cleaner beside the builder; `sm_set_cleaner_workers()` and `sm_cleaner_run()`, for tests |
| dg | the index access method's costs; `sm_rel_scan_entries()` for an index-only scan. The cleaner needs no dg code: `smstore_index_eval()` adopts any sm thread by `session_thread_adopt()` |

### 11.3 What a tree asks of sm1

- A page write: a `WriteBatch` of puts in the store's column
  family, applied at once, outside every transaction, in
  RocksDB's WAL. It is not synced, as a later commit's sync
  covers it. `sm1::page_write`.
- A read of one page, through the pool: `sm1::pin_page`,
  `unpin_page`, `page_enter`, and the session's copy,
  `page_copy`, `keep_page_copy`.
- A range delete of an index's pages, for `DROP INDEX`:
  `sm1::page_drop`. An index's size: `sm1::page_approxsize`.
- An SIREAD range and a write report on keys not stored, under
  the index's relid: `sm1::ssi_read_range`, `ssi_write_key`.
- A wait for the writer of a row's newest version, when it is
  another transaction still running, for the unique check:
  `sm1::read_newest`, `wait_writer`.
- A read of a row's newest version only when no snapshot can
  still see an older one: `sm1::read_settled`, for a cleaning.

---

## 12. Open questions, and what is not planned

### 12.1 Open

1. **The limit and the rate.** `cleaner_busy_limit`, 100
   transactions a second, and `cleaner_busy_pages`, 100 pages a
   second, are guesses. A rate that keeps up with a given write
   load needs a bench.
2. **An index-only scan by entry.** A scan could skip the row of
   any entry with its in-doubt bit clear and not cut, not only on
   a set leaf. One changed entry would then cost one row read,
   not a leaf's. Proposed: a second step, after this is measured.
3. **A small write for the bit.** RocksDB's Merge could log a
   small change to a page's key, not 4 KB. sm uses no merge
   operator yet, and the frames and the GC filter need a design
   for it. Proposed: plain puts, and Merge only if a bench asks.
4. **Row reads under EX.** A cleaning's second judging can read a
   disk under the leaf's EX pin (§9.3). Measure it.
5. **A delete's descent.** Whether a delete's extra descent shows
   on the old bench's write-heavy Q11.
6. **The planner.** `allvisfrac` stays 1. A tree's could be its
   set leaves over its leaves, counted by a cleaning.
7. **A longer unique key.** Whether a unique index's key refused
   past a third of a page is worth a larger limit.
8. **A load's cost.** Why a load is 5.6 times an LSM Index's on
   one machine (§10.2).

How `shared_buffers` divides between the pool and the block cache
is the Buffer Manager's question (BUFMGR.md).
9. **One write for a split whose parent has room.** A split
   writes P twice, so a split costs one more 4 KB put than it
   needs, about one put in 100 inserts. A write batch is atomic
   across pages, so with the parent pinned first, R, P, S and the
   parent can go in one write, with no flag. That is four pins of
   the five a session may hold. A full parent still needs two
   writes and the flag, as one write would pin every level to the
   root. Proposed: build this if a bench shows the split's write.
10. **`VACUUM` from the queue.** How many leaves with an entry
    in doubt `VACUUM` leaves behind after a long run (§9.2), and
    its time against the old walk's.

**Not shown to run:** `tree_prune()`'s check of page 0's level
has no test that reaches it.
### 12.2 Not planned yet

These are in the order I would take them. None is needed by what
is built.

| Work | What it buys | What it touches |
| --- | --- | --- |
| Reverse scan | `ORDER BY ... DESC` | `prev`; SSI's range. A step left can land on a page that has since split, so it must walk right until it meets the page it came from, as nbtree's `_bt_walk_left()` |
| Scan resistance | a fairer pool | measure first; a cursor reads a leaf again to step to the next, which counts as a use (`fill()`) |
| Log the entry, write the page later | closes 27.7 s to 18 s, the write gap | RocksDB's memtable and WAL are the write-back, so no dirty pages of ours: a Merge on the page's key, with a design for the frames and the GC filter (§12.1) |
| Prefix compression, suffix truncation of separators | more entries a page, a lower tree, far fewer page writes | §4.3, a split, the search of a page |
| Deduplicate equal keys | smaller leaves on low-cardinality columns | §4.3, the entry's format |
| Bottom-up sorted build | `CREATE INDEX` without a write per entry | sm3's build, `BULKLOAD.md` |
| Many leaves by one `MultiGet` | an IN list's misses in one read | §5.1 |
| Copies of the inner pages below page 0 | no pin on them in a probe | §5.5 |
| Narrow an SIREAD at a cursor's end | fewer false failures | a call with the last key read |
| Page deletion and merge | a tree that shrinks | **every "no page is freed" above**: pins, cursors, links, the session's copy, the cleaner's stale page ids |

---

## 13. Cases

| Case | What happens |
| --- | --- |
| TOAST | an entry is built from the detoasted value; no entry is out of line. `still_has()` fetches a key's value, by a settled read in a cleaning; a row that is gone needs no fetch |
| non-unique index | §5, §6, and the example in §3 |
| unique index | §7, an `ON CONFLICT` arbiter too. `tree_insert_unique()` holds the leaf's latch through its check, and a cleaning judges again under the same latch: the bit does not change the check. A leaf is set only when every entry is live: a unique key's dead equals are removed first |
| primary key | not a tree: the rows' keys. No bit, nothing queued |
| expression index | the key is the expression's value; the recheck evaluates it on the row. `still_has()` runs the evaluator on the worker, adopted as a builder's worker is |
| partial index | an entry only where the predicate holds; `still_has()` evaluates it, and a row out of the index has no entry |
| INCLUDE | not stored; read from the row (§4.3). An index-only scan of such an index reads each row |
| cut key | flagged, and its row read once by a cleaning, then its bit is clear; its leaf may be set; a scan reads its row on any leaf |
| update A → B → A | A → B sets A's bit and inserts B. Back to A finds A's entry and sets its bit, and sets B's. A is judged live and its bit cleared; B is removed |
| INCLUDE-only update | a key change's path: the entry is marked in doubt, its leaf queued, and a cleaning judges it live (§6.2) |
| primary key change | the row moves; new entries name its new key. The old entries' bits are set as a delete's; their row key holds no row, so they are dead |
| delete then reinsert | the delete sets the bit; the reinsert finds the entry, of the same key and row key; judged live once settled |
| leaf split | each half recomputed from its entries; the half with the new entry is not set; a queued page id still names the left half |
| abort, savepoint rollback | a rolled-back insert's entry stays, its bit set, and a cleaning removes it. A rolled-back delete's entry is judged live, and its bit cleared |
| undo-cap overflow | the row's undo; the tree has none: a bit is its own change. As an abort |
| crash | a top-level change is whole or not there. A row lost leaves a dead entry. A bit is in the WAL before the commit, and a bit whose commit is lost costs one row read. The queue is lost: the next reader or writer of each leaf queues it again |
| 2PC across restart | the entries committed before the prepare; the prepared row decides. It is not settled, so the bit stays, across a restart too, and the page waits |
| two writers, one leaf | the leaf's latch orders them; neither waits for the other's transaction |
| two writers, one unique key | §7: the second waits for the first's transaction |
| two writers, a cleaning | the leaf's latch orders each bit against a cleaning; the filter's atomic bit, then the queue's mutex, order the queue |
| old RR snapshot | it reads the row at its snapshot; an entry made since names a row it cannot see, or one with another key. It holds the watermark, so entries are not settled: bits stay, and the page waits until the watermark moves. Every snapshot is at or past the watermark, so a set leaf's entries are its entries |
| RC re-read | the row at the statement's snapshot. An entry goes only when no snapshot needs it; the cleaner's own lack of a snapshot does not change that, as the watermark decides |
| SSI | §2.3; a removal or a bit is no write to SSI |
| open cursor, Halloween | a cursor holds no page, and reads its batch's rows at its snapshot. An update's new entry ahead of the cursor is read, and its row, at the statement's snapshot, has the old key: skipped. An update clears its leaves' flags before it returns, so an index-only scan reads those leaves' rows. A scan treats the in-doubt bit as data it does not read |
| GC watermark, `CF:txn` sweep | `rel_read_settled()` decides. The tree holds no xid, so the sweep is unaffected, and a set flag needs neither again. The GC filter does not touch class `0x04` |
| CREATE INDEX pending | the backfill inserts entries as a writer does, each a top-level change, bits set; an entry already there is marked in doubt. A delete of a row not reached finds no entry, and clears only the flag. The first cleaning reads each row once |
| DROP INDEX | the pages go once the drop commits, by an `sm1::on_commit` function; a crash first leaves them until the store goes; a relid is never given again. The cleaner takes no lock: the relid is gone, so its queued pages are dropped. A cleaning already running may leave one page under the dead relid, until the store goes |
| TRUNCATE, ALTER rewrite | a new store, with new trees built as CREATE INDEX builds. The old store id finds no table, so its pages are dropped; a write to the old store fails, and the page is dropped |
| temporary tables | a temporary store's pages, as its rows. Bits as any tree's; not queued, so `VACUUM` in the session |
| keyless (`_id`) tables | the row key is the `_id` |
| bulk load, its rollback | into an empty table, built whole (§6.7), bits set and leaves queued; a cleaning removes a rolled-back load's entries. Into a table with rows, entries inserted as rows are |

---

## 14. Rejected

- **A primary key as a tree.** A read by key is at PostgreSQL's
  cost already, and the rows' MVCC, locks and GC stay sm1's.
- **The entry before the row.** Vacuum could remove an entry an
  insert has just found there, and another's unique check would
  find it naming no row, and call it dead.
- **An xmin and xmax on each entry**, as an LSM Index's entries
  have. Each update and delete writes its old entry's leaf, and
  an abort needs undo in the tree, which is the write cost the
  tree is for avoiding.
- **InnoDB's newest writer on a page.** Here an abort leaves its
  entry: a leaf whose newest writer is old can hold an entry
  whose row is gone.
- **A leaf flag alone, with no bit on an entry.** It puts a leaf
  once between two cleanings, not once an entry, but a cleaning
  reads every entry's row on a leaf not set, and a scan or a
  cleaner cannot tell which entries changed (§10.3).
- **A queue of old keys beside the bit.** A lost key could set a
  leaf all visible over a dead entry.
- **`VACUUM` led by the store's OLD.** OLD ids rise, so the
  versions written since the last `VACUUM` are one range, and
  each holds the old row an update or a delete replaced. From
  it, `VACUUM` could find the old entries' leaves after a
  restart or a full queue, where the queue misses them (§9.2).
  It is not built:
  - **Not every entry in doubt has an OLD version.** An
    insert's entry is in doubt until its row is settled, and
    the insert writes CURR alone (`ROWSTORE.md` §4.2). Every
    dead entry has one, as only an update or a delete leaves
    one.
  - **The GC removes OLD versions first.** The compaction
    filter drops a version no snapshot reaches, and
    `tbl_compact()` compacts the store before it cleans a tree.
    Keeping the versions for `VACUUM` keeps garbage.
  - **The queue gives the same leaves for less.** OLD costs a
    read of each version, its index keys made again, and a
    descent to each key's leaf. A miss of the queue costs only
    until the leaf's next reader or writer queues it.
- **A count of entries in doubt on a leaf.** A cleaning walks the
  whole page, so a count would save nothing.
- **A cut entry keeping its leaf unset.** That would queue the
  leaf for ever.
- **Judging a whole leaf under its latch.** A leaf that cannot be
  set would keep its readers and writers waiting on its rows. A
  first judging with no latch filters it.
- **A cleaner that runs in a transaction.** Its snapshot would
  hold back the watermark, and it needs none (§9.8).
- **Two or more hash functions in the filter.** Clearing a bit
  could hide other pages.
- **Stopping cleaning on a busy system.** With no autovacuum, a
  system that is never quiet must not lose its cleaning.
- **Workers made at an overflow.** A thread that cannot start
  then has no caller to tell.
- **A worker that keeps a session between passes.** The reaper
  could never close an idle database.
- **RocksDB's block cache as the page cache.** A page was once an
  entry of the block cache, read with no latch. The Buffer
  Manager's pool replaced it (BUFMGR.md), so a pin is now a
  latch, and a reader pins SH.
- **A version on each index, and a session's pin on its inner
  pages.** A session now holds only a copy of page 0 (§5.5),
  checked by its frame's version.
- **A check of the leaf's version by a cursor.** No page is ever
  freed, so a split hides nothing.
- **A meta page naming the root.** Page 0 is the root for life,
  so a descent needs no lookup.
- **A page sequence made by `CREATE INDEX`.** An abort would undo
  it. The database's sequence exists before any index.
- **A page padded to a fixed size.** It would cost writes and
  memory for nothing: a page is a RocksDB value, compressed in
  RocksDB's block.

---

## 15. Tests

- **mvcc**: cannot express it: sm1 has no tree, and the scenario
  driver has no verb for one. sm1's and sm3's unit tests cover
  the SIREAD on keys not stored, and the wait for a running
  writer, with its commit and its abort.
- **sm1**: `sm1/test/test_page.cc`, for a page written outside
  the transaction and read back, and a session with no
  transaction.
- **sm2**: `test_page.cc`, for keys at random against a sorted
  list, a split at every size, the in-doubt bit and a split's two
  halves. `test_tree.cc`, for a tree against a sorted list, a
  split's post cut short, and writers with cursors running.
  `test_index.cc` and `test_tree_index.cc`, for an index over a
  tree.
- **sm3**: `test_cleaner.cc`, for the queue and the rule for a
  pass. `test_tree.cc` and `test_toast.cc`, for queueing, a pass,
  a page that waits, a tree gone, and a key out of line, through
  `cleaner_run_once()`. `make tsan` in sm3, for a pass beside
  readers and writers.
- **rel**: `test_tree.rel`, through `sm.h`, by `(cleaner-pass)`,
  and `test_tree_split.rel`.
- **aa, sq**: not reached.
- **dg**: `dg/test/slt/btree_tree*.test`. `server.sh`, the
  cleaner running: index scans agree with a sequential scan, an
  expression and a partial index among them. SQL cannot count a
  tree's dead entries, so `test_tree.rel` proves they go.
- **Not tested**: the three workers that run on a full queue, as
  a test cannot fill 262,144 slots quickly. The rule they follow
  is tested, in `cleaner_budget()`.

### 15.1 Running them

- Build sm first, as a debug build: `DEBUG=1 make -C sm -j4`.
- One level's tree tests: `DEBUG=1 make -C sm/sm2 -j4
  build/test_sm2`, then, in `sm/sm2`, `./build/test_sm2
  -tc='tree:*'`. sm1's are `build/test_sm1`, the same way.
- `make tsan` in `sm/sm2` runs the tree's tests under TSan.
- The full suite, `DEBUG=1 make -C sm -j4 test`, takes over ten
  minutes: run it in the background, to a log. dg's takes longer.
  It passes when every doctest line reads `0 failed`, sm1's
  `scenarios:` line reads `scripts match, versioned and chained`,
  and the last is `All rel tests passed.`
- Do not edit `sm/` while dg's suite builds: it links sm as
  built.
