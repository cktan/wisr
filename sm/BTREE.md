# BTREE — secondary indexes as B+trees

The design of record for a tree: why it exists, what it is on
disk, in memory and under concurrency, and what is left. This is
the tree as built on `wip-btree`, off by default and asked for by
`CREATE INDEX ... WITH (tree = on)`: pages, cache, latches, the
tree, unique trees, `ON CONFLICT` arbiters, vacuum, and digres's
access. `../MAJORDECISION.md` (D1 to D14) says why each choice
was made; where one changed the first proposal, the text says so.
Paths are relative to `sm/`.

In short: a Lehman and Yao B-link tree whose pages are values in
RocksDB and entries in RocksDB's block cache. Pages are
immutable once written, and replaced whole. So there is no
buffer pool of our own, no dirty page, no page-level WAL, no
checkpoint, and a reader takes no latch. sm's own buffer pool
will change the first of those (§17, `BUFFERPOOL.md`).

**Nothing keeps compatibility before production**: a database
made before a change of a page's format is made again with
initdb.

---

---

## 1. Why

An index scan must cost digres no more than 1.5 times what it
costs PostgreSQL (2026-10-29). Reads by the primary key do,
through RocksDB; reads by a secondary index do not.
Instructions a statement, both servers pinned
(`../dg/WORKNOTE.md` steps 333 to 336, `NEXT.md` §1 item 3):

| Query | digres | PostgreSQL | Ratio |
| --- | --- | --- | --- |
| a row by its primary key | 48k | 50k | 0.96 |
| Q7, 100 rows by primary key | 976k | 802k | 1.22 |
| Q10, the same rows by `c2` | 2,946k | 802k | 3.67 |
| Q9, 100 rows of a `c2` range | 1,345k | 377k | 3.57 |
| 10 rows of a range of an uncorrelated column | 432k | 258k | 1.67 |

PostgreSQL pays the same for Q10 as for Q7: a descent of a
secondary btree costs what one of the primary key does.
digres pays about 30k instructions more a probe: a RocksDB
iterator made and destroyed for each, about 10k; its `Seek`
through every level and the index block's binary search, by
sm's comparator, about 12k; sm2 and sm3 about 8k. An entry of
a range costs 2,820 instructions against PostgreSQL's 695,
of which RocksDB's `Next` is 1,110: one key an entry cannot
reach PostgreSQL whatever is done above it.

**Decision: a secondary index, unique or not, may be a B+tree
of its own pages, beside the entries of today (§12); the
primary key stays the rows' RocksDB keys.** A read by key is
at PostgreSQL's cost already, and the rows' MVCC, locks and GC
stay sm1's.

---

## 2. The decisions

Made with the user (2026-10-29):

- **Any secondary index**, unique and non-unique, may be a
  tree; which is the index's kind, chosen when it is made
  (§12). A tree enforces uniqueness itself (§6).
- **The row decides visibility.** An entry carries no xid,
  as a PostgreSQL btree tuple carries none. A scan reads the
  row by its key at its snapshot and checks that the row
  still has the entry's key; an entry whose row does not is
  skipped, and removed later by vacuum (§7).
- **A change to the tree is a top-level transaction of its
  own**: an entry's insert, a split, a removal by vacuum
  commits when it is made, whatever the user's transaction
  then does. Its abort does not take the entry out; the row,
  which the abort does take out, makes the entry dead.
- **The page cache is read-only.** A page changes only by a
  put to RocksDB, so a cached page is never dirty: no
  write-back, no flush ordered against the WAL, no checkpoint,
  nothing to recover; any page may be dropped at any time, and
  a miss is a `Get`.

Proposed here:

- **Pages are keys of the table's store**, under a class byte
  of their own, so a store stays one column family
  (`sm1/SM1_DESIGN.md` §3.1). A change's pages are one
  RocksDB `WriteBatch`, in RocksDB's WAL: it is atomic and
  durable with nothing of ours to recover.
- **The page cache is RocksDB's block cache**, the one the
  environment has: a page's bytes, as RocksDB holds them, are
  an entry of its own there, by database, store and page id
  (`rocksdb::Cache::Insert`, its charge the page's length, a
  deleter), shared by every session. So there is one memory
  budget and one replacement policy, RocksDB's, and none of
  sm's. A writer writes through: under the page's latch it
  makes the new page, puts it in its change's `WriteBatch`,
  then replaces the page's entry. A page is allocated when it
  changes or misses, never when it is read.
- **A read writes nothing shared on the inner pages**, which
  every probe reads, so their cache lines do not bounce from
  core to core:
  - **Each index has a version**, which a change to an inner
    page moves. A session keeps its own pin on the index's
    inner pages and, at each probe, only loads the version,
    pinning them again when it has moved. An old pin goes when
    its session takes the new one. *Built otherwise*: an open
    index holds the inner pages it reads, and lets them go when
    a descent has to move right (`../MAJORDECISION.md` D11).
    Page 0 alone is now read from a session's copy, checked by
    its frame's version (§4.2).
  - **A leaf is pinned by its cache handle**, `Lookup` and
    `Release`: the count is the leaf's own, so it is contended
    only as the leaf is popular.
  - **Inner pages go in at high priority**
    (`Cache::Priority::HIGH`), so a leaf read once is evicted
    first. Leaves are cached too; one read again, a hit, moves
    to the high-priority pool beside them, as RocksDB's LRU
    cache does with any entry.
- **Lehman and Yao's B-link tree**: each page has a right link
  and a high key, so a split never blocks a descent. **A reader
  takes no latch**: a page is replaced whole, in RocksDB and in
  the cache, so a reader sees it before or after a change, and
  the right link covers a split under way. A writer latches the
  page in a latch table by page id, as sm1 latches a key
  (`sm1/SM1_DESIGN.md` §5.1).

### 2.1 Transactions

- **A change to a tree is its own transaction**: it commits when
  made, whatever the user's transaction then does. An abort
  does not take an entry out; the row, which the abort does take
  out, makes it dead.
- **The entry follows the row**, in the same statement, so it is
  in the WAL before the commit, and a committed row always has
  its entry; a crash before the commit leaves only a dead one.
  The WAL being one sequence, a commit's sync covers the entry
  (D5).
- **A unique insert** latches the leaf covering the key's prefix,
  judges every other entry of the prefix by its row's newest
  version (`sm1::read_newest`), and inserts holding that latch.
  A live row is `SM_ERR_DUPKEY`; a running writer is waited for
  with every latch let go, and the key checked again (D13).
- **Serializable**: a scan registers its whole requested range
  as an SIREAD of its own, and an insert reports its entry's key
  (D10). A scan stopped by `LIMIT` keeps the whole range, and may
  fail a writer it need not.

---

## 3. Layout

### 3.1 In RocksDB

A page is one key and one value in the table's column family,
so a store stays one column family (`sm1/SM1_DESIGN.md` §3.1).

```
key   = 0x04 | relid (2B) | pageid (8B)     big-endian
value = the page's bytes, at their own length
```

- **Class `0x04`** sorts after the rows (`0x00`) and a chained
  store's (`0x01`, `0x02`), compares bytewise, and is passed
  over by the GC filter and by every scan of versions.
- **Pages of a tree sort together**, by id. An id says nothing
  of key order: a split's right page gets the next id, wherever
  its keys fall.
- **Not versioned**: one key, the newest bytes. Older versions
  of a page are RocksDB's to drop at compaction.
- **Compressed by RocksDB**, LZ4 in its blocks of 16 KB
  (`store_block_size`), and cached uncompressed.

### 3.2 A page's bytes

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

Integers are little-endian. The key above is big-endian so that
it sorts; these are read, never compared.

`flags` has three bits:

| Bit | Name | Set when |
| --- | --- | --- |
| 0x01 | `page_has_high` | the page has a high key: every page but a level's rightmost |
| 0x02 | `page_incomplete` | the page has split, and its parent has no entry for the new right page yet |
| 0x04 | `page_all_visible` | a leaf vacuum found every entry of settled and live; an insert into it, or a delete or key change of an entry on it, clears it (§4.1) |

`page_incomplete` is set by a split's first write and cleared by
its second (§5). A reader ignores it, as `next` reaches the new
page. A writer that latches a flagged page finishes the split
before its own change, which is how a crash between the two
writes is repaired. Page 0 is never flagged.

**A page has a limit, not a size.** `page_limit` is 4 KB (D12):
past it an insert splits the page. A page is stored at its
length, with no free space, as every change writes it whole, and
a fixed size is for frames and disk blocks, which this design
has neither of.

**There is no gap between slots and entries, but the slots keep
room** (§17). A slotted page keeps free space to insert in place;
this one is replaced, so a new entry is spliced into a new copy
of the bytes (`page_insert_bytes()`, with no entry decoded): it
goes last, and its slot in key order. A page is stored at its
used length, which counts unused slots, so `entry_max` leaves
room for a half's 30. The pool's frames will hold the gap, and pages will
change in place (§17).

The slots let a page be searched where it lies: a binary search
over them reads the page's bytes in its cache entry, with no
decode and no copy.

The limit stays at or under the store's block size, 16 KB
(`sm1/SM1_DESIGN.md` §12.2), so a page's `Get` reads one block.
A page here is a RocksDB value, compressed in RocksDB's block,
and a cache entry charged its length, so padding would cost
writes and memory for nothing. A full leaf holds about 200 `c2`
entries: three levels for a million.

### 3.3 Entries

| Page | Key | Value |
| --- | --- | --- |
| leaf, level 0 | the entry's sm2 key: `encode(key columns)` then the row's key | its cut flag |
| inner, level 1 and up | its child's low key; empty on a level's leftmost page | the child's page id, 8B |

- **Keys are distinct byte strings, compared bytewise.** A leaf
  entry ends with its row's key, so equal column values do not
  need a tiebreak.
- **A leaf holds no MVCC.** No xid, no timestamp, no INCLUDE
  columns: the row decides what is visible (§2.1).
- **A page holds `[low, high)`.** Its high key is the least key
  it may not hold: its right sibling's first key, at the split
  that made it.
- **A descent takes the last inner entry at or below the key.**
- **No prefix compression, suffix truncation or deduplication
  yet.** A separator is a whole key. All three are wanted (§11).
- **INCLUDE columns are read from the row**, which is read
  anyway: an entry newer than the reader's snapshot could not
  give them. A unique index's entry carries the row's key too, as
  equal keys of dead rows sit beside the live one (§6).

### 3.4 Limits

| | Value |
| --- | --- |
| page limit | 4,096 bytes |
| header | 32 bytes |
| unused slots a half may carry | 30 bytes |
| largest entry, `entry_max()` | (4,096 - 32 - 30) / 3 = 1,344 bytes |
| longest key, as it must fit an inner entry too | 1,330 bytes |
| levels | 256 |
| page ids | 2^64, and never reused |

An entry is at most a third of what the header and a half's
unused slots leave, as PostgreSQL limits a btree tuple to a third
of a page: a half of a split holds a high key and two entries, so
a split always finds a point where both halves fit, with the
entry that caused it (D1). The split is made over the page's
entries and that one, as `_bt_findsplitloc()` does, at the point
nearest half the bytes where both fit. A longer key is cut and
flagged, and rechecked on the row (`sm2/SM2_DESIGN.md` §3.2); a
unique index's, never cut, is refused, where today's entries take
4,096 bytes (`sm1::max_keylen`).

**An open and a check.** Every read opens a page, checking its
header and slots alone. A writer also checks every entry before
it changes a page: they lie end to end from the slots to the
page's end, and each is named by one slot.

### 3.5 The shape of a tree

```
 page 0   [ ""→17 , m→23 ]                  level 2, the root
              |       |
 level 1   [17] <--> [23]                   next / prev links
           /  \       /  \
 level 0 [4]<>[9]<>[31]<>[12]               leaves, in key order
```

- **Page 0 is the root, for life.** Its key is `relid | 0`, so a
  descent starts with no lookup of where the root is.
- **Every level is a doubly linked list**, by `next` and
  `prev`. The links, not the parents, are what a reader relies
  on: B-link's reason for being.
- **Other page ids mean nothing**: they come from one sequence
  for the database, so a tree's are sparse. Holes cost nothing,
  as RocksDB stores only the keys that exist.
- **0 means "none" in a link.** The root is nobody's child or
  sibling, so no link names page 0.
- **Height is the root's level + 1.**
- **Only page 0 ever changes level.** Any other page keeps its
  level for life. No page is merged or freed, and no id reused.
  A tree made again under a relid first drops every page there.

### 3.6 Page ids

- **One sequence for the database**, not one a store or an index,
  starting at 1, so page 0 is never handed out. Holes are
  harmless, as ids need only be unique within a tree, and one a
  database has no table's lifetime to follow (D14).
- **Made in `cat_init()`** (`sm3/src/catalog/ddl.cc`), beside
  `sm3.oid`. Its name starts `sm3.`, so `reserved_seq()` keeps it
  from users. sm2 names it, a constant in `tree.h`, as sm2 is
  what draws from it.
- **Drawn by a `seqref_t`**, looked up at an open tree's first
  split. A root split draws two.
- **Why it is safe:** `sm1::nextseq` never hands a value out
  twice, across restarts too; a crash only skips values, and an
  abort gives none back. D4's objection was to a sequence made by
  `CREATE INDEX`, which an abort undoes; this one exists before
  any index.

### 3.7 A page holds a range

- **A page holds a half-open range of keys**, `[low, high)`: a
  reader moves right while its key is at or past the high key.
  Every key of a tree is distinct, as a leaf entry ends with its
  row's key, so no rule is needed for equal keys.
- **An inner entry is its child's low key and the child's page
  id**, the first entry of a level's leftmost page with an empty
  key, below every key.
- **`prev` exists for a reverse scan later**: nothing reads it
  yet but `tree_check()`. A split keeps it in its first batch, S
  latched under P's latch, left before right, and read anew.

---

## 4. Reads

**A descent** starts at page 0, binary-searches each page's slots
where it lies, moves right past a page whose high key its key is
at or past, and follows the child down. It latches nothing.

**A probe's page** comes from the cache, a miss read with a `Get`
and put there. An equality's probe then reads entries while they
match; a range's, until the upper bound.

**A range** copies the leaf's entries in range, releases the
leaf, and hands them out from the copy. The next batch starts by
reading that leaf again, past the last key, and moving right, so
a split meanwhile hides nothing. If that page is page 0 grown, a
leaf no longer, it descends again from the last key.

- **The leaf is not held** while rows are read: the scan copies a
  leaf's entries in range, one at a time in key order, and
  releases it.
- **A cursor** keeps its place as the last key it returned, and
  the leaf it copied from: no page is ever freed, so a split
  meanwhile hides nothing. The first proposal's check of the
  leaf's version was not built.

**An IN list** aims one cursor from probe to probe, keeping its
buffers (`cursor_rebound()`).

**Each entry's row** is read by its key at the transaction's
snapshot, into the cursor's pin, and handed out only if the row
still gives this entry's key, compared as bytes (D8). Rows of
sorted, close keys are read by one cursor (`BATCHREAD.md` §2.1).
*Built*: sm3 reads them, many a call, and makes each row's entry
as a write would, the evaluator asked for an expression or a
predicate. A cut key is rechecked against the row's columns.

**An IN list's misses** by one `MultiGet` are *not built*: digres
opens a probe a scan, each descending through the inner pages its
index holds (D11).

**Index-only scans read the row.** None is answered from the tree
alone; `count(*)` by an index gets slower. A leaf's all-visible
bit, proposed in §4.1, would answer it.

**Backward** (planned, after `prev`): a step left can land on a
page that has since split, so it must walk right until it meets
the page it came from, as nbtree's `_bt_walk_left()`.

### 4.1 An all-visible leaf

*Proposed 2026-11-02, and built the same day
(`../dg/WORKNOTE.md` step 365, `../MAJORDECISION.md` D22 to
D26).* It changes §2's "the row decides visibility" for one
case: a leaf vacuum has found settled answers an index-only
scan alone.

**As built**, where it differs from what follows:

- **Vacuum judges a leaf first without its latch**, as §7
  does, and latches it to judge whole only if every entry
  looked dead or settled: a leaf that cannot be set keeps
  its readers and writers from waiting on its rows (D22).
- **The planner is unchanged**: `allvisfrac` stays 1 (D23).
- **An index-only scan skips a row** only for a tree of key
  columns alone, whose keys give their columns back, and an
  entry not cut: one with INCLUDE columns reads them from
  the row (D24). sm3's tree cursor does it, asked by digres
  (`tbl_scan_entries_only`, `sm_rel_scan_entries`).
- **A unique insert that finds its key** clears its leaf, as
  any insert that finds its key does (D25).
- **A delete, or a key's change, reports its old entry's key
  to SSI** (`ssi_write_key`), as an insert reports its new
  one: a serializable index-only scan that read no row holds
  only its range's SIREAD, which a row's write alone does not
  meet (D27).

**Why.** `bench/` q2, `select count(*) from usertable where
state = 'CA'`, 183 rows of 10,000: a regular index counts its
entries, 0.18 ms; a tree reads each row, 0.39 to 0.59 ms. The
tree's count costs what the regular index's costs reading the
rows, 0.50 ms: the row reads are the difference. PostgreSQL
answers it from the index alone after `VACUUM`, its visibility
map's bits set, in 3 pages, 0.04 ms.

**The bit.** `page_all_visible`, `0x04` in a leaf's `flags`
(§3.2): every entry on the leaf is **settled and live**, its
row's newest version committed at or below the GC watermark
(`sm1/SM1_DESIGN.md` §8.3) with the entry's key, so every
snapshot there is or will be sees the entry.

**The algorithm.** `read_newest` is `sm1::read_newest`, a row's
newest version and whether it has committed; a row is read by
the key its entry ends with (§3.3).

```
INSERT an entry           (an insert, or an update's new key)
  latch leaf(key)
  add the entry if it is not there
  leaf.all_visible = false
  write leaf              -- an insert writes it already
  unlatch

DELETE a row, or UPDATE one changing an index's key or the
primary key
  write the row's new version              -- as today
  for each tree index:
    latch leaf(old key)
    if leaf.all_visible:
      leaf.all_visible = false
      write leaf          -- the only new write
    unlatch
  (an update then INSERTs its new entry)

VACUUM an index
  for each leaf, left to right:
    if leaf.all_visible: skip      -- it holds no dead entry
    latch leaf
    ok = true
    for each entry:
      r = read_newest(entry's row key)
      if r committed at or below the watermark:
        if r is gone or has another key: remove the entry
        -- else settled and live
      else:
        ok = false        -- not settled: kept, judged next time
      if the entry is cut: ok = false
    if ok: leaf.all_visible = true
    write leaf if anything changed
    unlatch

INDEX-ONLY SCAN
  take the snapshot
  for each leaf in the range:      -- no latch, as today
    if leaf.all_visible:
      hand out its entries in range, no row read
    else:
      for each entry in range:
        read its row at the snapshot
        hand it out if the row has the entry's key
```

**Why vacuum judges the leaf latched**, unlike §7's removal,
which judges first without the latch: a delete's row write
either comes before vacuum latches the leaf, so vacuum reads
a version not settled and leaves the bit unset, or after, so
the delete waits on the latch and finds the bit set, and
clears it. Judged without the latch, vacuum could read the old
row, and set the bit after the delete had looked. So a delete
reads the bit latched too; the latch costs no write.

**Why a scan needs no latch**: it reads a leaf after taking its
snapshot, and a writer clears the bit before its commit, as an
entry's insert is written (§5), in RocksDB's WAL before the
user's commit. A snapshot that includes the delete reads the
leaf with the bit clear.

**What it costs a writer.** A delete, or an update of a key,
latches the old key's leaf, descending to it through cached
inner pages, which it does not today, and writes the leaf only
if the bit is set: **once a leaf between two vacuums**, however
many of its entries change. An update keeping the index's key
and the row's key clears nothing: its entry is good at every
snapshot. An insert writes its leaf already.

**What it costs vacuum.** It skips a set leaf, as PostgreSQL's
vacuum skips an all-visible heap page: a second `VACUUM` with
no write between reads the leaves and no row. On a leaf not
set it reads the row of each entry, as §7 does today.

**The planner.** `estimate_rel_size()` gives an sm table
`allvisfrac` 1, true of today's entries, not of a tree: a
tree's would be its set leaves over its leaves, counted by
vacuum.

**Not taken:**

- **InnoDB's newest writer on a page.** Here an update or a
  delete does not touch its old entry's leaf, and an abort
  leaves its entry: a leaf whose newest writer is old can hold
  an entry whose row is gone.
- **An xmin and xmax on each entry**, as today's entries have:
  each update and delete writes its old entry's leaf, and an
  abort needs undo in the tree, which is the write cost the
  tree is for avoiding.
- **A bit on each entry, not the leaf**, until the tree writes
  back (§11, "Log the entry, write the page later"). A scan
  would skip each settled entry's row, and vacuum read only the
  changed entries' rows, but a delete would clear its own
  entry's bit: a leaf written at every delete, not once between
  vacuums. A classic B-tree changes its leaf at every delete
  too, but as a redo record on a dirty page, written once for
  many; here each change puts the whole page, and PostgreSQL's
  btree has no index write at a delete at all. With write-back,
  a clear is a small logged change, and the entry's bit is
  worth having. Short of it, vacuum could keep a bit on each
  entry for itself alone, a delete queueing its old key as a
  small key of its own rather than writing the leaf, and read
  only the unset entries' rows and the queued keys'.

**Cases:**

| Case | With the bit |
| --- | --- |
| TOAST, cut keys | a cut entry keeps its leaf unset; recheck on the row as today |
| non-unique, unique | set only when every entry is live: a unique key's dead equals are removed first |
| primary key | not a tree: the rows' keys |
| expression, partial | the entry has the key; no change |
| INCLUDE | not in the tree: the row is read, as today |
| update A → B → A | A → B clears A's leaf and, inserting, B's; back to A finds A's entry and clears its leaf |
| INCLUDE-only update, key kept | clears nothing: the entry is good at every snapshot |
| primary key change | the old entries' leaves cleared as a delete's; the new inserted |
| delete then reinsert | the delete clears; the reinsert finds the leaf unset |
| abort, savepoint rollback | the insert cleared its leaf; vacuum sets it again only once the dead entry is gone |
| undo-cap overflow | no undo in the tree: a clear is its own change |
| crash | a clear is in the WAL before the commit; one whose commit is lost leaves a leaf unset, which is safe |
| 2PC across restart | a prepared row is not settled: vacuum leaves its leaf unset |
| two writers, vacuum | the leaf's latch: vacuum judges a leaf with it held, and a writer reads the bit with it held |
| old RR snapshot, RC re-read | every snapshot is at or past the GC watermark: a set leaf's entries are its |
| SSI | the range's SIREAD, as today (D10); a delete or a key's change reports its old entry's key into it (D27) |
| open cursor, Halloween | an update's leaves are cleared before it returns: their rows are read |
| GC watermark, `CF:txn` sweep | a set bit needs neither again |
| CREATE INDEX pending, bulk load and its rollback | entries come in as inserts: leaves unset until vacuum |
| DROP INDEX, TRUNCATE, ALTER rewrite | pages dropped; a new store has a new tree |
| temp tables, keyless (`_id`) tables | the same, `_id` the row's key |

**Open:**

1. digres has no autovacuum (`../dg/UNSUPPORTED.md` §7): a bit
   is set only by `VACUUM`, which `bench/load.sql` would run on
   both systems.
2. Whether a delete's extra descent shows on the old bench's
   write-heavy Q11.

### 4.2 A session's copy of the root

*Built 2026-11-02 (`../dg/WORKNOTE.md` step 366,
`../MAJORDECISION.md` D28).* Every descent reads page 0, so its
pin, a count in its frame's state word, was a word every probe on
every core wrote. A session now keeps its own copy of page 0 and
descends from it with no pin while the page is as it was.

- **A frame's version** (`sm1/src/bufpool.cc`), a count moved by
  each change of the frame's page or bytes: an install, a
  discard, a purge, an eviction. All are under an exclusive pin.
  It sits in the descriptor's line, which still fits 64 bytes:
  the frame's bytes are found by its index, no longer a pointer.
- **The copy**: `sm1::keep_page_copy()`, with the page pinned,
  keeps its bytes, its frame and the frame's version, in the
  session; `sm1::page_copy()` hands the bytes back while the
  frame's version is unchanged, read with no pin: frames last the
  pool's life. A good copy ages its frame as a pin does, its
  age's top bit set (`bufpool_t::touch()`); without it, page 0,
  pinned by no one, ages out of the pool while it is read most,
  and every session's copy goes with it. A session keeps at most 32, by store and key, and
  drops them all for the 33rd. It lives in the sm1 session, not
  the tree: digres opens a table, and its trees, once a
  transaction.
- **`descend()`** uses the copy when page 0 is above the level it
  descends to. Otherwise, or with no good copy, it pins page 0 as
  before, and keeps a copy if the page is above that level. So a
  root that is a leaf, and a writer posting into the root's
  level, pin it as before, and an insert into a root leaf makes
  no copy.
- **Stale is safe**: the version is read, then the copy used; a
  write between is as a write just after a pinned read. A copy
  stale by any split is as good as page 0 read just before it:
  no page is ever freed, the children it names are still there at
  their level, and their right links lead on. Page 0's own split
  moves the version, so the next probe copies it again.
- **Writes** pin page 0 as before: only a descent reads the copy.

| Case | What happens |
| --- | --- |
| page 0 changed (a post, a root split) | install moves the version: the next descent pins and copies |
| DROP INDEX, TRUNCATE, `tree_create` again | `page_drop()`'s purge moves it; a store dropped fails `page_enter()` first |
| page 0 evicted | a good copy's check ages page 0 as a pin, so it is not while it is read; if it is, the copy is not good, and the next descent misses and reads it |
| crash, restart | the pool and the copies start empty |
| a root that is a leaf | pinned, as the leaf a descent wants |
| another database, temp tables | the copy is by store and key in one session, of one database |

Not done: copies of the inner pages below page 0.

---

## 5. Writes

**An insert** descends, latches the leaf, moving right with the
latch coupled, reads the page anew under it, and writes the page
whole: its new bytes in a batch. Finding the key already there
is no change, which an update A to B and back relies on.

**When**: when a row is inserted, or an update changes an index's
key, the new entry is inserted, in a top-level change, before the
row write returns. An update from A to B and back to A finds A's
entry still there.

**Nothing is removed by an update or a delete.** The old entry
stays, and its row, at the old key no longer, makes it dead until
vacuum. A primary key's change moves the row (`CLUSTER.md` §4):
its new entries name the new key, and the old ones name a key
that holds no row.

**Order with the user's transaction**: the entry is inserted
after the row's version is written, in the same statement, so
its change is in RocksDB's WAL before the commit's: a row that
commits always has its entry, and a crash before the commit
leaves only a dead entry. The entry first, as a first draft had
it, lets vacuum remove an entry an insert has just found there
(D5).

**A split of page P**, S being its old right sibling, R new:

```
first batch, atomic:
  R        the upper entries; R.prev = P, R.next = S
  P        the lower entries; P.next = R; flagged incomplete
  S        S.prev = R             (none if P was rightmost)
second batch, atomic:
  parent   a new entry for R
  P        unflagged
```

A reader in between reaches R by P's `next` link. A crash in
between leaves P flagged, and the next writer to latch it
finishes the post before its own change. A full parent splits
the same way, in the second batch.

**A split of page 0** has no in-between:

```
one batch, atomic:
  L, R     two new pages, the entries in halves, linked;
           L.prev = 0, R.next = 0
  page 0   entries for L and R, its level up one
```

So it is never flagged. This is what the fixed root costs and
buys: page 0's entries move to new pages, the one case of a key
moving other than right; and page 0's level changes, so a writer
who latches it, or a cursor who reads it again, checks the level
and descends again if it is not what it wanted. A reader
holding page 0's old bytes is still routed rightly.

**The cost of writing a page whole**: 4 KB through the WAL and
memtable where today's entries write about 40 bytes. See §10.

---

## 6. Uniqueness

As PostgreSQL's `_bt_check_unique()`: under the leaf's latch,
each entry with an equal key, its key columns not null, has
its row read at its newest version (`sm1::read_newest`):

- a row committed and live with that key: `SM_ERR_DUPKEY`;
- a row whose writer is still running, with that key: the
  latch is released, the writer waited for (sm1's lock on the
  row's key, `LOCK.md`), and the check made again;
- otherwise the entry is dead, and the next is looked at.

Then the new entry goes in, under the same latch. A key with
a null column is not checked, as now.

The check latches the leaf covering the key's prefix, and inserts
holding that latch (D13).

---

## 7. Vacuum, durability and space

An entry is removed when its row, at the GC watermark
(`sm1/SM1_DESIGN.md` §8.3), is gone or has another key, so no
snapshot can still read the row with it: `VACUUM` walks each
index's leaves, reads the rows of a leaf's entries, and
removes the dead ones in a top-level change, as PostgreSQL's
`btbulkdelete()`. An empty leaf stays linked, as PostgreSQL's
half-dead pages are, until a later design deletes them. The GC
filter does not touch class `0x04`.

- **Recovery has nothing of ours.** A batch is in RocksDB's WAL,
  unsynced, whole or not at all; replay restores it. A commit's
  sync makes it durable. The cache starts empty.
- **Half-done work is one state**: a page flagged incomplete,
  finished by the next writer to latch it.
- **A temporary table's pages** are in the temporary instance,
  with no WAL, as its rows are.
- **Vacuum** walks the leaves, and removes an entry only when its
  row is settled: its newest version committed at or below the GC
  watermark, and gone or of another key (D5). A leaf's entries
  are judged with no latch, and those to go are judged again with
  it latched, as an insert that finds its entry there moves no
  version: only its row, written first, shows it.
- **Pages are not merged.** An empty leaf stays linked, a tree
  never shrinks, and page ids are not reused.
- **`DROP INDEX`** drops the pages once it commits, by
  `sm1::page_drop` from an `on_commit` function, as a rollback
  must keep them. A crash first leaves them until the store goes
  (D6). `TRUNCATE` and a rewrite make a new store.

---

## 8. Cases

| Case | What happens |
| --- | --- |
| TOAST | an entry is built from the detoasted value, as now; no entry is out of line |
| non-unique index | §4, §5 |
| unique index | §6, built, an `ON CONFLICT` arbiter too (D13) |
| primary key | unchanged: the rows' keys |
| expression index | the key is the expression's value, as now; the recheck evaluates it on the row |
| partial index | an entry only where the predicate holds, as now; the recheck needs only the key |
| INCLUDE | not stored; read from the row (§3) |
| cut key | flagged and rechecked on the row, as now |
| update A → B → A | A's entry stays, B's is added, A's again is no change; B's is dead once the row says A |
| INCLUDE-only update | no index change |
| primary key change | the row moves; new entries name its new key, old ones are dead (§5) |
| delete then reinsert | the old entry, the same key and row key, is the new row's |
| abort, savepoint rollback | the entries stay, dead by the row |
| undo-cap overflow | the row's undo; the tree has none |
| crash | a top-level change is whole or not there (§2); a row lost leaves a dead entry |
| 2PC across restart | the entries committed before the prepare; the prepared row decides |
| two writers, one leaf | the leaf latch orders them; neither waits for the other's transaction |
| two writers, one unique key | §6: the second waits for the first's transaction |
| old RR snapshot | reads the row at its snapshot; an entry made since names a row it cannot see, or one with another key |
| RC re-read | the row at the statement's snapshot, as now |
| SSI | a scan registers the range of entry keys it reads as an SIREAD in sm1's tracker, under the index's relid (`sm1::ssi_read_range`), and an insert reports its entry's key (`sm1::ssi_write_key`): the keys need not be sm1's to be tracked (`sm1/SM1_DESIGN.md` §7.1, D10) |
| open cursor, Halloween | an update's new entry ahead of the cursor is read, and its row, at the statement's snapshot, has the old key: skipped |
| GC watermark, CF:txn sweep | §7; the tree holds no xid, so the sweep is unaffected |
| CREATE INDEX pending | the backfill inserts entries as a writer does, each a top-level change; an entry already there is no change |
| DROP INDEX | the pages go once the drop commits, by an `sm1::on_commit` function; a crash first leaves them until the store goes; a relid is never given again (D6) |
| TRUNCATE, ALTER rewrite | a new store: new trees, built as CREATE INDEX builds |
| temporary tables | a temporary store's pages, as its rows |
| keyless tables | the row key is the `_id`, as now |
| bulk load, its rollback | entries inserted as rows are; a rollback leaves them dead. A sorted build bottom-up is left for later |

---

## 9. Where it sits

```
 digres   btree access method, WITH (tree = on)
            |  scans rows through sm3's cursor (D8)
 sm3      index's kind, row check, unique check, DROP, vacuum
            |
 sm2      src/tree/   page.cc   bytes of one page, no I/O
                      tree.cc   descent, cursor, insert, split
            |  sm1::pin_page, page_write, page_drop, ...
 sm1      src/page.cc     a page's I/O
          src/bufpool.cc  frames, a pin that is a latch, the clock
            |
 RocksDB  block cache (row blocks), WAL, memtable, SSTs
```

- **sm1 owns a page's I/O**, as it is the only channel into
  RocksDB, and knows nothing of trees (D2).
- **sm2 owns the tree**, and holds no RocksDB handle.
- **A primary key is never a tree.** Its entries are the rows'
  RocksDB keys. A row stays in sm1's MVCC store; a tree holds
  only keys that lead to rows.

What each level holds:

- **sm1**: class `0x04`, its comparison and the GC filter's
  pass-over; a top-level `WriteBatch` of pages; SIREAD and
  `on_write` on keys that are not stored; the environment's
  block cache, for the page cache's entries.
- **sm2, `src/tree/`**, beside the index `rel` that uses it
  (`sm2/SM2_DESIGN.md` §5), each part a file of its own with
  unit tests of its own in `sm2/test`:
  - **the page**: its layout, the in-place search of its
    slots, an entry's insert and a split's halves, on bytes,
    with no RocksDB;
  - **the page cache**: pages as entries of the block cache,
    write-through, a miss's fill latched against writes. It is
    sm1's (`sm1::pin_page`, `sm1::page_write`), as sm1 is the
    only channel into RocksDB (`../MAJORDECISION.md` D2, D3);
    the index's version and a session's pin on its inner
    pages (§2) are not built: an open index holds the inner
    pages it reads instead (D11);
  - **the writers' latch table**, by page id;
  - **the tree**: descent, the IN list's batch, scans,
    inserts, splits and vacuum's walk, over the three and
    sm1.
- **sm2/sm3**: an index `rel` over a tree: `idx_insert`,
  `idx_scan`, `idx_scannxt`, the uniqueness check, vacuum's
  walk, and no `idx_delete`; an index's kind in its
  definition, and every index operation branching on it
  (§12).
- **dg**: none in the executor; the index access method's
  costs, as btree's, and `amcanreturn` false until §4's
  visibility map.

---

## 10. What it costs

### 10.1 Measured

Measured with the meta page, before this design.
Instructions are `perf stat`'s user instructions of the whole
server, per statement (`NEXT.md` §1, `../dg/WORKNOTE.md` step
354).

| | PostgreSQL 18 | today's entries | a tree |
| --- | --- | --- | --- |
| Q9, a range, 100 rows | 395k | 1,344k (3.4×) | 955k (2.4×) |
| Q10, 100 probes | 768k | 3,670k (4.8×) | 1,562k (2.0×) |
| Q11, 16 clients, tps | 33,512 | 12,250 | 19,721 |
| a write moving a key, 16 clients, tps | 26,904 | 10,874 | 18,196 |

| A million rows into `wide_mixed` | seconds |
| --- | --- |
| today's entries | 18 |
| a tree of 4 KB pages, spliced (step 351) | 27.7 |

Measured again with this design, against the commit before it,
on the same machine and build (`../dg/WORKNOTE.md` step 359):

| | before | this design | today's entries |
| --- | --- | --- | --- |
| Q9 | 958k | 957k | 1,324k |
| Q10 | 1,523k | 1,522k | 3,637k |
| Q11, 16 clients, tps | 20,019 | 22,889 | 11,108 |
| a write moving a key, 16 clients, tps | 15,356 | 15,184 | 9,835 |
| a million rows, seconds | 70 | 66 | 12 |

So the design costs nothing measurable. **A load is 5.6 times
today's entries here**, against the 1.5 above, on both builds,
and why is not known. Its profile is RocksDB's: LZ4 a tenth, and
compaction and the memtable.

The goal, 1.5× of PostgreSQL, is not met. What a tree's Q9
spends: malloc and free a fifth, the executor, each row's
recheck. Q10's: a descent a probe, and its row.

Measured again on a VM of 2 cores (2026-10-31, `BUFFERPOOL.md`
§1): a load is 2.04 times today's entries with the WAL's logs
zero-filled, 1.38 with that off, and 1.13 on tmpfs. Here the
extra is zeroing 64 MB a log on the write path, and stalls for
two immutable memtables, so new logs are now zero-filled only on
WSL (`sm1/WALPREALLOC.md` §2). That does not account for 5.6 times
on the machine above, which is still to be run both ways.

### 10.2 As estimated first

- **Q10**: the probe's 30k becomes a descent of pages in the
  cache, about 3k; Q10 about 1.25M instructions, **1.55
  times** PostgreSQL. The rest is the row read by key, Q7's
  cost.
- **Q9**: an entry about 200 instructions, not 2,820; but each
  row read by key costs about 8,400. Q9 about 1.1M, **2.9
  times**, until rows of sorted, close keys are read by one
  cursor (`NEXT.md` §1 item 3, step 2): then about 0.5M, 1.3
  times.
- **Writes**: an indexed insert writes its page, at its length,
  up to 4 KB, into RocksDB's WAL and memtable, against an entry
  of about 40 bytes now. Measured, a million rows loaded into
  `wide_mixed` with its `c2` index: 18 s with today's entries,
  51 s with a tree of 8 KB pages, 34 s of 4 KB, 27 s of 2 KB;
  Q9 and Q10 within 2% of each other at all three
  (`../MAJORDECISION.md` D12). An entry spliced into the page's
  bytes, none decoded, brought 4 KB to 27.7 s
  (`../dg/WORKNOTE.md` step 351). Logging the entry and
  writing the page later is what would close the rest.
- **Index-only scans** get slower (§4).

---

## 11. Open questions, and what is not planned

1. Whether pages share the block cache's budget with the
   rows' blocks as they are, or need a share of it set aside:
   a machine choice sm would make at a home's first attach,
   with no setting.
2. Answered: 4 KB (D12). Whether a unique index's key refused
   past a third of it (§3) is worth a larger limit is open.
3. Whether the row read of Q9's adjacent rows, by one cursor,
   comes first: without it the tree does not bring Q9 under
   1.5.
4. How a statement asks for a tree (§12): `USING smtree`, or
   `WITH (tree = on)`.

**Not shown to run**: `tree_prune()`'s check of page 0's level
has no test that reaches it.

**Not planned yet**, in the order I would take them. None is
needed by what is built.

| Work | What it buys | What it touches |
| --- | --- | --- |
| Reverse scan | `ORDER BY ... DESC` | `prev`, §4's walk right; SSI's range |
| Fix §15.3's two weaknesses | a fairer cache | measure first; `fill_cache`, a cursor that keeps the leaf's id |
| Log the entry, write the page later | closes 27.7 s to 18 s, the write gap | **dirty pages**, so write-back: this design's largest step |
| Prefix compression, suffix truncation of separators | more entries a page, a lower tree, far fewer page writes | §3.3, a split, the search of a page |
| Deduplicate equal keys | smaller leaves on low-cardinality columns | §3.3, the entry's format |
| Bottom-up sorted build | `CREATE INDEX` without a write per entry | sm3's build, `BULKLOAD.md` |
| Many leaves by one `MultiGet` | an IN list's misses in one read | §3.5 |
| Narrow an SIREAD at a cursor's end | fewer false failures | D10, a call with the last key read |
| Page deletion and merge | a tree that shrinks | **every "no page is freed" above**: held pages, cursors, links |
| Visibility data on a page | index-only scans from the tree | §2.1, a new write to every page |

---

## 12. Beside today's entries

A tree and today's entries can both index one table: each
index is of one kind, and keeps it for its life.

- **Their keys do not meet**: today's entries are class
  `0x00` keys under the index's relid, a tree's pages class
  `0x04` under its own. One store holds both, as it holds a
  table's rows and entries now.
- **The kind is in the index's definition** in sm3's catalog
  (`sm3/SM3_DESIGN.md` §2), and every call that touches an
  index branches on it: insert, scan, the uniqueness check,
  the build, vacuum, DROP. A table's indexes are written in
  one call (`sm3/SM3_DESIGN.md` §3), whichever their kinds.
- **digres has one index access method for both**; sm routes
  by the kind. Today's kind is the default. A tree is asked
  for when the index is made (§11, question 4); changing a
  kind is DROP and CREATE.
- **Where each is the better**, measured or estimated
  (instructions):

  | | Today's entries | A tree |
  | --- | --- | --- |
  | an entry of a range | 2,820 | about 200 |
  | a probe of a non-unique key | about 30k (`Seek`) | about 3k |
  | a probe of a unique key | a `Get`, near PostgreSQL's | about 3k |
  | a unique key, at most | 4,096 bytes | about 1.3 KB |
  | an index-only scan | yes: an entry is MVCC | reads the row |
  | an indexed insert, written | about 40 bytes | the page, up to 4 KB |
  | dead entries | none, GC drops versions | until vacuum |

- **Built behind the kind, off by default**, so each step is
  measured against today's entries on the same table and can
  be left off if it fails.
- **Kept both only if measured so**: the grid of index scans
  and a write benchmark, on each kind. If one kind wins every
  workload, the other goes; two kinds kept are two code paths
  in sm2 and sm3 and every test run on each.
- **Tests run on each kind**: slt and pgregress, the mvcc and
  rel cases each kind needs; a tree's cases are §8's.

**Whether today's entries stay** is decided by measuring both
kinds over that grid: a kind that loses every workload goes.

---

## 13. Order of work

A step at a time, each with its tests at every layer it
reaches, each measured where it changes a cost. Steps 1 to 3
need nothing decided above them; the rest settle what this
section proposes, each on review before it is built.

**What the steps settle**, proposed:

- **A page holds a half-open range of keys**, `[low, high)`:
  its high key is the least key it may not hold, none on the
  rightmost page of a level. A reader moves right while its
  key is at or past the high key. Every key of a tree is
  distinct, as a leaf entry ends with its row's key (§3), so
  no rule is needed for equal keys.
- **An inner entry is its child's low key and the child's page
  id**, the first entry of a level's leftmost page with an
  empty key, below every key. A descent takes the last entry
  whose key is at or below the key it looks for.
- **A split is two top-level changes**: the first writes the
  new right page, holding the upper half, and the left page
  with its high key the right page's first key, its right link
  the new page, and a flag, *incomplete split*; the second
  writes the parent's new entry and the left page without the
  flag. A crash between them leaves the flag, and a writer
  that meets it on its way down finishes the post before its
  own change, as PostgreSQL's `_bt_finish_split()`. A reader
  needs no flag: the right link takes it there.
- **A root split** is one change: both halves to new pages,
  and page 0 a level up, their parent, so it has no half to
  finish (`../MAJORDECISION.md` D14).
- **Page ids come from one sequence for the database**, made
  with its catalog: never handed out twice, and its gaps are
  harmless (D14).
- **A cached page is keyed by the database, the store, the
  index's relid and the page id**, as one block cache serves
  every database of the environment.
- **sm1 gains, for keys of class `0x04`** (`sm1/API.md`):
  - a top-level write, a `WriteBatch` of puts and deletes in
    the store's column family, applied at once, outside every
    transaction, in RocksDB's WAL; not synced, as a later
    commit's sync covers it, the WAL being one sequence;
  - a read of one page, and of many, through the block cache;
  - a range delete of an index's pages, for DROP INDEX;
  - an SIREAD range, and an `on_write`, on keys not stored,
    under the index's relid (§8, SSI);
  - a wait for the writer of a row's newest version, when it
    is another transaction still running, cut short as every
    wait is, for the uniqueness check (§6);
  - a read of a row's newest version only when no snapshot
    can still see an older one, at or below the GC watermark,
    for vacuum (§7).

**The steps:** all eight are built on `wip-btree`, with the
tests each names and more (`../dg/WORKNOTE.md` steps 345 to
350); where one went otherwise than written here,
`../MAJORDECISION.md` says how. sm1's calls are
`pin_page`, `page_write`, `page_drop`, `page_approxsize`,
`read_settled`, `read_newest`, `wait_writer`,
`ssi_read_range` and `ssi_write_key`; the mvcc scenarios step
4 names are unit tests of sm1 and sm3 instead, as the
scenario driver has no verb for a tree.

1. **The page** (`sm2/src/tree/page.cc`): the layout of §3,
   the search of its slots, an entry's insert, and a split's
   halves, on bytes alone. Tests: `sm2/test`, keys at random
   against a sorted list; a split at every size, its halves'
   keys the whole's and in order, each under the limit; an
   entry over a third of the limit refused; a page full of the
   longest entries split with one more at every position.
2. **The writers' latch table** (`sm2/src/tree/latch.cc`), by
   page id. Tests: `sm2/test`, many threads, one page and
   many, under TSan.
3. **The page cache**, sm1's (`sm1/src/page.cc`,
   `../MAJORDECISION.md` D2, D3): pages in the block cache,
   write-through, a miss's fill latched against writes. Tests:
   `sm1/test`, a page written outside the transaction and read
   back after a restart; a writer and four readers through a
   cache far smaller than the pages, the writer never reading
   back an older page than it wrote; a drop by prefix; pages
   apart from every version, GC and bulk load.
4. **sm1's calls** above. Tests: `sm1/test` for each; an mvcc
   scenario for the SIREAD on keys not stored, a write into a
   range a serializable reader holds, and one for the wait
   for a running writer, with the writer's commit and its
   abort.
5. **The tree** (`sm2/src/tree/tree.cc`): descent, scans, the
   IN list's batch, inserts, splits and their finish, over
   steps 1 to 4. Tests: `sm2/test`, a million entries against
   a sorted list; a crash between a split's two changes,
   finished by the next insert; inserts from many threads
   with scans running.
6. **An index `rel` over a tree** (`sm2/src/rel/index.cc`):
   the entry, the uniqueness check of §6, and vacuum's walk of
   §7. Tests: `sm2/test`'s index tests on both kinds; `rel`
   for a unique key's two writers and vacuum.
7. **The index's kind in sm3**: in its definition, and every
   index operation and the build branching on it
   (`sm3/SM3_DESIGN.md` §3). Tests: `rel`'s index tests on
   both kinds.
8. **digres's way to ask for one** (§11, question 4), and the
   index access method's costs. Tests: `dg/test/slt` on both
   kinds; the grid of index scans measured against today's
   entries and PostgreSQL (`NEXT.md` §1 item 3).

**After the eight steps**: the root moved to page 0 for life, the
meta page went for the sequence of §3.6, and every page gained
`prev` (D14). A pin's check was measured, and a snapshot older
than the last DDL now answers at its own snapshot (§14.3). The
next project is §17.

---

## 14. The page cache

**Replaced.** Built otherwise: sm's own pool, `BUFFERPOOL.md`.
This section, like §15 and §16, says how pages were held before
it.

**There was none of our own.** The page cache is RocksDB's block
cache, the one the environment has, shared by every database in
it, and by the rows' blocks. digres sizes it to `shared_buffers`
(`NBuffers * BLCKSZ`). One memory budget, one eviction policy.

### 14.1 Against PostgreSQL's

| | PostgreSQL | a tree |
| --- | --- | --- |
| frame | fixed 8 KB, an array | a `std::string`, the page's length |
| page table | shared hash, 128 partitions | the cache's shards |
| pin | an atomic in the buffer's descriptor | a handle's count, under its shard's mutex |
| content lock | LWLock, SH or EX, on every read | none to read; a writer's latch by page id |
| change | in place, in the frame | a new copy replaces the entry |
| dirty pages | yes | **never** |
| write-back | bgwriter, checkpointer | none |
| WAL | a record for the change | the whole page, in a write batch |
| recovery | redo into pages | RocksDB's WAL replay, nothing of ours |
| replacement | clock sweep | LRU with a high-priority pool (§15) |
| scan protection | ring buffers | none (§15.3) |

### 14.2 A cache entry

- **Key**: `'P' | dbid (8B) | storeid (8B) | page key`. The tag
  keeps it from any key RocksDB makes for a block.
- **Value**: a `std::string` of the page's bytes, uncompressed,
  charged its capacity, its object and its key.
- **A pin is the cache handle.** `sm1::pin_page` returns it, cast
  to an opaque `page_t`, so a hit allocates nothing. It holds
  the entry against eviction until `sm1::unpin_page`.
- **An entry's bytes never change.** A write replaces the entry;
  a pin on the old one keeps the old bytes, valid, until
  released. This is what lets a reader run without a latch, and
  why a pin has no SH or EX mode.

### 14.3 A read

```
pin_page(key, inner)
  Lookup                       a hit: take a reference, done
  miss: lock fill latch of key
    Lookup again               another reader filled it
    Get from RocksDB           memtable, then SSTs
    Insert at HIGH if inner, else LOW, and keep the handle
```

**What a pin checks first** (`enter_pages()`, on every pin and
page write, and D2's cost):

1. An active transaction, not prepared or doomed.
2. A store other than 0.
3. The store is live, at the transaction's view
   (`catalog_lookup()`): a hash lookup in the session's cache,
   and a load of the database's `last_ddl_ts`.
4. The store's handle, `session_store()`, twice over.

Then the page's key and its cache key are made. Measured on the
optimized build, a pin and unpin of a cached page, one thread:

| Transaction | ns |
| --- | --- |
| commonly | 65 |
| older than the database's last DDL | 65 |
| has made or dropped a store itself | 590 |

The last reads the catalog on every pin, and every row read does
the same: it must see its own uncommitted DDL, which the
session's cache does not stand for. `CREATE TABLE` and its load
in one transaction, as a restore does, pays it. It is left so for
now.

A pin has no mode today, as pages are immutable. With sm's own
pool it has two, SH and EX (§17).

### 14.4 A write

```
page_write(pages)              the batch is one change
  batch of Puts
  lock every page's fill latch, in stripe order
  RocksDB Write                WAL unsynced, then memtable
  each page: Insert, replacing its entry
  unlock
```

**Every page in the cache is clean**: it always equals RocksDB's.

- **To change a page:** take its writer's latch (§16), read it,
  make a changed copy, write the copy to RocksDB, put the copy in
  the cache in place of the old entry, release the latch. The
  old entry is never touched: anyone pinning it keeps reading it.
- **To add a page:** the same, with nothing to read: a new id,
  bytes laid out from nothing, written, put in the cache.
- **So nothing is ever flushed**, and any unpinned page can be
  dropped at any moment. The next reader's miss is a `Get`.

**The fill latch.** Reads do not take the writer's latch, so
without this a write could be lost:

1. A reader misses page P, and `Get`s its bytes.
2. A writer writes a new P to RocksDB and to the cache.
3. The reader, finishing, inserts the old P over the new one.
4. The next writer starts from the old P: step 2's change is
   gone.

RocksDB's cache has no compare-and-insert, so a miss holds a
latch across its `Get` and `Insert`, and a write across its
`Write` and its inserts, and steps 1 and 3 cannot straddle step
2 (D3). 256 stripes, by the cache key's hash, for the process.
A hit takes none.

### 14.5 Pages an open tree holds

Every page is in the one cache, shared by every transaction. A
tree keeps no copy of any. What it keeps is **pins**: having
pinned an inner page, it does not unpin it, and remembers the
pointer, so its next probe needs no `Lookup`.

This is here because a `Lookup` is not free: it takes the shard's
mutex and writes the entry's reference count, a shared cache line
on the hottest pages there are. A statement's 100 probes would
make 300 of them (D11). A pool whose pin is one atomic would need
little of this.

A tree is open as long as its table handle: in digres, to the
end of the transaction. It pins up to 64 inner pages
(`held_max`).

- **A held page may go stale**, and still routes rightly: its
  bytes are as they were, its children still exist, and a key
  moves only right, where the links reach it.
- **A descent that has to move right drops them all**, so a long
  statement does not keep routing through old pages. A million
  rows loaded took 424 s without this, 51 s with it (D11).
- **A held page 0 is not always safe to stop at**: a split can
  grow the tree past it, so a descent asking for a level above
  it drops the held pages and reads page 0 anew.

### 14.6 Memory

- **Pins outrun the budget, rather than fail.** The cache is not
  strict about its capacity, so a read never fails for want of
  room. What is pinned is at most `held_max` pages an open tree.
- **A cursor holds no page between calls.** A leaf's entries in
  range are copied, the page released, before any row is read.

---

## 15. Replacement

**Replaced.** Built otherwise: sm's own pool, `BUFFERPOOL.md`.
This section, like §14, says how pages were held before it.

This section is how pages are evicted today. sm's own pool
replaces it with a clock, leaves aging twice as fast as inner
pages (§17).

RocksDB's LRU cache, `NewLRUCache(capacity)`, its defaults
untouched: shards by the key's hash, as many as RocksDB chooses
for the capacity, each with its own mutex and lists; a high-
priority pool of half of each shard; no low-priority pool.

### 15.1 The rule

| Entry | Goes in at | Evicted |
| --- | --- | --- |
| inner page | high-priority list | last |
| leaf, read or written | low list | first |
| any entry with a hit | high-priority list, on release | last |
| pinned, any | on no list | never |

- **Eviction takes the low list's tail.** The high pool, when
  over half the shard, spills its oldest into the low list.
- **A hit promotes.** RocksDB's own rule: an entry with a hit
  goes to the highest list there is, whatever its priority. So a
  leaf read once goes first, and a leaf read twice stays with
  the inner pages. Midpoint insertion, as InnoDB's.
- **`inner` is a hint at a miss**, and a write's page carries its
  own, from its level. A root that is still a leaf is read as
  inner until its first write, as a descent cannot know before
  it reads: one page a tree.
- **There is no third score.** The cache has HIGH, LOW and
  BOTTOM, and a hit outranks all of them. Leaves are cached, and
  are not given a score of their own.

### 15.2 Rows' blocks share it

- **Row blocks go in at LOW**, as RocksDB's data blocks do, so a
  leaf and a row block are equals until a hit.
- **A hit on a row block promotes it too**, so hot row blocks
  and inner pages share the high pool.
- **Index and filter blocks are not in the cache**
  (`cache_index_and_filter_blocks` is off): they stay with the
  table readers.

### 15.3 Known weaknesses

- **Scans are not resisted.** A cursor reads a leaf again to step
  to the next (`fill()`), which is a hit, so each leaf of a long
  scan is promoted, and may push inner pages out of the high
  pool. Not measured.
- **A miss caches a page's bytes twice.** `pin_page`'s `Get` uses
  default `ReadOptions`, so `fill_cache` is on: the 16 KB block
  holding the page enters the cache, as well as the page's own
  entry. Not measured. `fill_cache = false` would stop it, at
  the cost of a block read for each neighbouring page's miss.
- **The share of the budget is unset.** Whether pages need a part
  set aside from rows' blocks is open (§7).
- **A hit writes shared memory**: a reference, under the shard's
  mutex. Every session's first descent of a transaction takes
  page 0's shard mutex. It is about 470 instructions a lookup,
  by D11's numbers; held pages spare the rest of a transaction.

---

## 16. Latches

**Replaced.** Built otherwise: sm's own pool, `BUFFERPOOL.md`.
This section, like §14, says how pages were held before it.

| Latch | Where | Protects | Held across |
| --- | --- | --- | --- |
| fill latch | sm1, 256 stripes | a miss's fill against a write | a `Get`, or a `Write` |
| cache shard mutex | RocksDB | the shard's lists and table | a lookup |
| writer's latch | sm2, one a page | one writer of a page | that page's change |
| none for readers | | | |

**A reader takes no page latch.** It sees a page before a write
or after it, never between, and `next` links cover a split under
way.

**A writer's latch is by page id, never a stripe.** A split holds
a page's and its parent's, and two pages sharing a stripe would
deadlock. It is a `std::mutex` in a table of 64 shards, made
when wanted and dropped when the last holder or waiter goes, so
a page can be latched though it is not cached.

**Order, so no two writers wait on each other:**

1. child before parent;
2. left before right, on a level.

A split's latch on its right sibling, for `prev`, is rule 2. A
latch is held for a page's change alone, never across a wait for
a transaction: a unique check lets every latch go first (D13).
Its wait is therefore not one `sm_wait()` cuts short.

---

## 17. sm's own buffer pool

Decided with the user (2026-10-31): sm will own a buffer pool for
a tree's pages, in place of RocksDB's block cache. It is a
project of its own, with its own design and plan
(`BUFFERPOOL.md`), built, and not yet measured. It will not close
the 1.5× gap; what it buys is eviction under memory pressure, and
cheaper pins at high client counts. It replaces §14 and §15, and §16's
fill latch and writers' latches, where they differ.

What is decided:

- **A pin is a latch, SH or EX.** There is no third mode. A
  reader pins SH, a writer EX. So a pin is held only as long as a
  latch is, never across calls, and §14.5's held pins go. This
  reverses the answer in `sm1.h`, that a pin has no mode.
- **To change a page P:**
  1. Pin P in EX.
  2. Copy P out, and change the copy.
  3. Write it to RocksDB.
  4. Copy it into the buffer pool.
  5. Unpin P.
- **To make a new page P:**
  1. Pin P in EX, with a flag not to read it.
  2. Make P, in a copy.
  3. Write it to RocksDB.
  4. Copy it into the buffer pool.
  5. Unpin P.
- **So a frame is never half changed.** Everything that can
  throw or fail is done on the copy, and the frame changes only
  by a `memcpy` that cannot. An exception or a failed write
  unpins a frame that still equals RocksDB's: pages stay clean,
  and there is no write-back.
- **A split** pins its pages in EX left to right, and writes them
  in one batch, then copies each into the pool.
- **Eviction is a clock.** Leaves age twice as fast as inner
  pages, as a scan of the leaves would otherwise page the inner
  pages out. Only dropping a tree purges its pages at once.
- **A page's slots keep room to grow.** A page's header has
  `max`, the slots it has, grown 16 at a time, and `top`, the
  slots in use, so `top <= max`. These replaced `n`. Slots past
  `top` are zeroed, but zero marks nothing: `top` alone says
  which are valid, as the first entry's offset is 0. Each slot
  holds its entry's offset from the end of the slot array, so
  growing the array moves the entries up with no slot changing.
  To insert between slots 8 and 9, with 48 slots of which 34 are
  used: slots 9 and up shift up one, and slot 9 takes the new
  entry, which goes at the end of the entries. Entries are in no
  order; only the slots are.

What it must settle:

- **Who waits under EX**: readers of P wait through the write to
  RocksDB, and through a unique check's row reads under the
  leaf's latch (D13), as today's readers do not. Measure it.
- **Memory**: one budget becomes two, the pool's and the block
  cache's for rows. How `shared_buffers` divides is open
  (§7).
- **RocksDB's copy**: a miss's `Get` should not fill the block
  cache too (§15.3).
- **Readers' cost**: SH writes a shared cache line on every page
  read, the root included. Today's readers write nothing but the
  cache's reference. Measure it against §10.
- **What goes**: the fill latch, as a miss loads the frame under
  its pin, the writers' latch table, and §14.5's held pins.
- **A cursor's copy of a leaf**: answered: `fill()` copies the
  entries in range, one at a time in key order.
- **`page_write`'s batch**: a split still writes several pages
  atomically, each under its EX pin.

---

## 18. Running its tests

- Build sm first, as a debug build: `DEBUG=1 make -C sm -j4`.
- One level's tree tests: `DEBUG=1 make -C sm/sm2 -j4
  build/test_sm2`, then, in `sm/sm2`, `./build/test_sm2
  -tc='tree:*'`. sm1's are `build/test_sm1`, the same way.
- `make tsan` in `sm/sm2` runs the tree's tests under TSan.
- The full suite, `DEBUG=1 make -C sm -j4 test`, takes over ten
  minutes: run it in the background, to a log. dg's takes longer.
  It passes when every doctest line reads `0 failed`, sm1's reads
  `scenarios: 122 scripts match, versioned and chained`, and the
  last is `All rel tests passed.`
- Do not edit `sm/` while dg's suite builds: it links sm as
  built.
