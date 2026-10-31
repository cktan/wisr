# BTREEDESIGN — the B+tree of a secondary index

The design of record for a tree: what it is on disk, in memory
and under concurrency. It describes the tree as it will be once
`BTREE-todo.md` is done, and §1 lists how that differs from the
code today. `BTREE.md` holds why the tree exists and how it was
proposed, `../MAJORDECISION.md` (D1 to D13) why each choice was
made. Paths are relative to `sm/`.

In short: a Lehman and Yao B-link tree whose pages are values in
RocksDB and entries in RocksDB's block cache. Pages are
immutable once written, and replaced whole. So there is no
buffer pool of our own, no dirty page, no page-level WAL, no
checkpoint, and a reader takes no latch.

---

## 1. Status

**Built**, on `wip-btree`, off by default, asked for by `CREATE
INDEX ... WITH (tree = on)`: pages, cache, latches, the tree,
unique trees, `ON CONFLICT` arbiters, vacuum, digres's access.

**Since `43e15ea9`** (`BTREE-todo.md`), and written below:

| Below | Before |
| --- | --- |
| page 0 is the root, always | page 0 is a meta page; the root moves |
| no meta page | meta page: tag, root, next page id, height |
| page ids from one sequence for the database | `next` in the meta page, under its latch |
| a root split is one atomic batch | two writes, like any split |
| `prev` and `next`, the links, on every page | `right` only |
| header 30 bytes | header 22 bytes |

**Not planned yet**: §13.

---

## 2. Where it sits

```
 digres   btree access method, WITH (tree = on)
            |  scans rows through sm3's cursor (D8)
 sm3      index's kind, row check, unique check, DROP, vacuum
            |
 sm2      src/tree/   page.cc   bytes of one page, no I/O
                      tree.cc   descent, cursor, insert, split
                      latch.cc  writers' latches
            |  sm1::pin_page, page_write, page_drop, ...
 sm1      src/page.cc  a page's I/O, its cache entry, fill latch
            |
 RocksDB  block cache (pages + row blocks), WAL, memtable, SSTs
```

- **sm1 owns a page's I/O**, as it is the only channel into
  RocksDB, and knows nothing of trees (D2).
- **sm2 owns the tree**, and holds no RocksDB handle.
- **A primary key is never a tree.** Its entries are the rows'
  RocksDB keys. A row stays in sm1's MVCC store; a tree holds
  only keys that lead to rows.

---

## 3. Page format

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
| header, 30 bytes                                       |
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

`flags` has two bits:

| Bit | Name | Set when |
| --- | --- | --- |
| 0x01 | `page_has_high` | the page has a high key: every page but a level's rightmost |
| 0x02 | `page_incomplete` | the page has split, and its parent has no entry for the new right page yet |

`page_incomplete` is set by a split's first write and cleared by
its second (§9). A reader ignores it, as `next` reaches the new
page. A writer that latches a flagged page finishes the split
before its own change, which is how a crash between the two
writes is repaired. Page 0 is never flagged.

**A page has a limit, not a size.** `page_limit` is 4 KB (D12):
past it an insert splits the page. A page is stored at its
length, with no free space, as every change writes it whole, and
a fixed size is for frames and disk blocks, which this design
has neither of.

**There is no gap between slots and entries, but the slots keep
room** (§14). A slotted page keeps free space to insert in place;
this one is replaced, so a new entry is spliced into a new copy of
the bytes (`page_insert_bytes()`, with no entry decoded): it goes
last, and its slot in key order. A page is stored at its used
length, which counts unused slots, so `entry_max` leaves room for
a half's 30. The pool's frames will hold the gap, and pages will
change in place (§14).

### 3.3 Entries

| Page | Key | Value |
| --- | --- | --- |
| leaf, level 0 | the entry's sm2 key: `encode(key columns)` then the row's key | its cut flag |
| inner, level 1 and up | its child's low key; empty on a level's leftmost page | the child's page id, 8B |

- **Keys are distinct byte strings, compared bytewise.** A leaf
  entry ends with its row's key, so equal column values do not
  need a tiebreak.
- **A leaf holds no MVCC.** No xid, no timestamp, no INCLUDE
  columns: the row decides what is visible (§10).
- **A page holds `[low, high)`.** Its high key is the least key
  it may not hold: its right sibling's first key, at the split
  that made it.
- **A descent takes the last inner entry at or below the key.**
- **No prefix compression, suffix truncation or deduplication
  yet.** A separator is a whole key. All three are wanted (§13).

### 3.4 Limits

| | Value |
| --- | --- |
| page limit | 4,096 bytes |
| largest entry, `entry_max()` | (4,096 - 30) / 3 = 1,355 bytes |
| longest key, as it must fit an inner entry too | 1,341 bytes |
| levels | 256 |
| page ids | 2^64, and never reused |

An entry is at most a third of what the header leaves, as a half
of a split holds a high key and two entries, so a split always
finds a point where both halves fit, with the entry that caused
it (D1). A longer key is cut and flagged, and rechecked on the
row; a unique index's is refused.

**An open and a check.** Every read opens a page, checking its
header and slots alone. A writer also checks every entry before
it changes a page.

---

## 4. The shape of a tree

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

---

## 5. The buffer pool

**There is none of our own.** The page cache is RocksDB's block
cache, the one the environment has, shared by every database in
it, and by the rows' blocks. digres sizes it to `shared_buffers`
(`NBuffers * BLCKSZ`). One memory budget, one eviction policy.

### 5.1 Against PostgreSQL's

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
| replacement | clock sweep | LRU with a high-priority pool (§6) |
| scan protection | ring buffers | none (§6.3) |

### 5.2 A cache entry

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

### 5.3 A read

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
pool it has two, SH and EX (§14).

### 5.4 A write

```
page_write(pages)              the batch is one change
  batch of Puts
  lock every page's fill latch, in stripe order
  RocksDB Write                WAL unsynced, then memtable
  each page: Insert, replacing its entry
  unlock
```

**Every page in the cache is clean**: it always equals RocksDB's.

- **To change a page:** take its writer's latch (§7), read it,
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

### 5.5 Pages an open tree holds

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

### 5.6 Memory

- **Pins outrun the budget, rather than fail.** The cache is not
  strict about its capacity, so a read never fails for want of
  room. What is pinned is at most `held_max` pages an open tree.
- **A cursor holds no page between calls.** A leaf's entries in
  range are copied, the page released, before any row is read.

---

## 6. Replacement

This section is how pages are evicted today. sm's own pool
replaces it with a clock, leaves aging twice as fast as inner
pages (§14).

RocksDB's LRU cache, `NewLRUCache(capacity)`, its defaults
untouched: shards by the key's hash, as many as RocksDB chooses
for the capacity, each with its own mutex and lists; a high-
priority pool of half of each shard; no low-priority pool.

### 6.1 The rule

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

### 6.2 Rows' blocks share it

- **Row blocks go in at LOW**, as RocksDB's data blocks do, so a
  leaf and a row block are equals until a hit.
- **A hit on a row block promotes it too**, so hot row blocks
  and inner pages share the high pool.
- **Index and filter blocks are not in the cache**
  (`cache_index_and_filter_blocks` is off): they stay with the
  table readers.

### 6.3 Known weaknesses

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
  set aside from rows' blocks is open (`BTREE.md` §11).
- **A hit writes shared memory**: a reference, under the shard's
  mutex. Every session's first descent of a transaction takes
  page 0's shard mutex. It is about 470 instructions a lookup,
  by D11's numbers; held pages spare the rest of a transaction.

---

## 7. Latches

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

## 8. Reads

**A descent** starts at page 0, binary-searches each page's slots
where it lies, moves right past a page whose high key its key is
at or past, and follows the child down. It latches nothing.

**A range** copies the leaf's entries in range, releases the
leaf, and hands them out from the copy. The next batch starts by
reading that leaf again, past the last key, and moving right, so
a split meanwhile hides nothing. If that page is page 0 grown, a
leaf no longer, it descends again from the last key.

**An IN list** aims one cursor from probe to probe, keeping its
buffers (`cursor_rebound()`).

**Each entry's row** is read by its key at the transaction's
snapshot, into the cursor's pin, and handed out only if the row
still gives this entry's key, compared as bytes (D8). Rows of
sorted, close keys are read by one cursor (`BATCHREAD.md` §2.1).

**Backward** (planned, after `prev`): a step left can land on a
page that has since split, so it must walk right until it meets
the page it came from, as nbtree's `_bt_walk_left()`.

---

## 9. Writes

**An insert** descends, latches the leaf, moving right with the
latch coupled, reads the page anew under it, and writes the page
whole: its new bytes in a batch. Finding the key already there
is no change, which an update A to B and back relies on.

**Nothing is removed by an update or a delete.** The old entry
stays, dead by its row, until vacuum.

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
memtable where today's entries write about 40 bytes. See §12.

---

## 10. Transactions

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

## 11. Durability and space

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

## 12. What it costs today

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

---

## 13. Not planned yet

In the order I would take them. None is needed by §1's work.

| Work | What it buys | What it touches |
| --- | --- | --- |
| Reverse scan | `ORDER BY ... DESC` | `prev`, §8's walk right; SSI's range |
| Fix §6.3's two weaknesses | a fairer cache | measure first; `fill_cache`, a cursor that keeps the leaf's id |
| Log the entry, write the page later | closes 27.7 s to 18 s, the write gap | **dirty pages**, so write-back: this design's largest step |
| Prefix compression, suffix truncation of separators | more entries a page, a lower tree, far fewer page writes | §3.3, a split, the search of a page |
| Deduplicate equal keys | smaller leaves on low-cardinality columns | §3.3, the entry's format |
| Bottom-up sorted build | `CREATE INDEX` without a write per entry | sm3's build, `BULKLOAD.md` |
| Many leaves by one `MultiGet` | an IN list's misses in one read | `BTREE.md` §4 |
| Narrow an SIREAD at a cursor's end | fewer false failures | D10, a call with the last key read |
| Page deletion and merge | a tree that shrinks | **every "no page is freed" above**: held pages, cursors, links |
| Visibility data on a page | index-only scans from the tree | §10, a new write to every page |

**Whether today's entries stay** is decided by measuring both
kinds over the grid (`BTREE.md` §12): a kind that loses every
workload goes.

---

## 14. sm's own buffer pool

Decided with the user (2026-10-31): sm will own a buffer pool for
a tree's pages, in place of RocksDB's block cache. It is a
project of its own, with its own design and plan
(`BUFFERPOOL.md`), after `BTREE-todo.md`, and measured first. It
will not close the 1.5×
gap; what it buys is eviction under memory pressure, and cheaper
pins at high client counts. It replaces §5 and §6, and §7's fill
latch and writers' latches, where they differ.

What is decided:

- **A pin is a latch, SH or EX.** There is no third mode. A
  reader pins SH, a writer EX. So a pin is held only as long as a
  latch is, never across calls, and §5.5's held pins go. This
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
  (`BTREE.md` §11).
- **RocksDB's copy**: a miss's `Get` should not fill the block
  cache too (§6.3).
- **Readers' cost**: SH writes a shared cache line on every page
  read, the root included. Today's readers write nothing but the
  cache's reference. Measure it against §12.
- **What goes**: the fill latch, as a miss loads the frame under
  its pin, the writers' latch table, and §5.5's held pins.
- **A cursor's copy of a leaf**: answered: `fill()` copies the
  entries in range, one at a time in key order.
- **`page_write`'s batch**: a split still writes several pages
  atomically, each under its EX pin.
