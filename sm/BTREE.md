# BTREE — secondary indexes as B+trees

Status: being built on the `wip-btree` branch, unattended;
the decisions taken there are in `../MAJORDECISION.md`, and
where one changed this design the text below says so. Built:
§13's steps 1 to 8 for a non-unique index: the page, the
latches, sm1's pages and cache, the tree, the index `rel` over
it with vacuum's walk, sm3's kind, and digres's
`WITH (tree = on)` (D7). Not built: a unique tree (§6, D9).

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
    its session takes the new one.
  - **A leaf is pinned by its cache handle**, `Lookup` and
    `Release`: the count is the leaf's own, so it is contended
    only as the leaf is popular.
  - **Inner pages go in at high priority**
    (`Cache::Priority::HIGH`), so the leaves are evicted first.
- **Lehman and Yao's B-link tree**: each page has a right link
  and a high key, so a split never blocks a descent. **A reader
  takes no latch**: a page is replaced whole, in RocksDB and in
  the cache, so a reader sees it before or after a change, and
  the right link covers a split under way. A writer latches the
  page in a latch table by page id, as sm1 latches a key
  (`sm1/SM1_DESIGN.md` §5.1).

## 3. Layout

```
page key = 04 | relid (2B) | pageid (8B, big-endian)
value    = header | slots | entries
header   = level (1B, 0 a leaf) | flags (1B) | n (2B)
         | version (8B) | right (8B, 0 none)
         | high key's length (2B) | high key (none on the
           rightmost page of a level)
slots    = n offsets (2B each), of the entries in key order
entry    = key's length (2B) | key | value's length (2B) | value
           leaf:   the entry's sm2 key, as today, encode(sk) ‖
                   row key; its value, the cut flag
           inner:  the child's low key; its value, the child's
                   page id
```

Integers are little-endian (`sm2/src/tree/page.cc`). The flags
are *has a high key* and *incomplete split* (§13).

- **Class `0x04`** sorts after the rows (`0x00`) and a chained
  store's (`0x01`, `0x02`). Keys of the class compare
  bytewise, as `0x01` and `0x02` do; the GC filter keeps them
  as they are (§7).
- **The slots let a page be searched where it lies**: a
  binary search over them reads the page's bytes in its cache
  entry, with no decode and no copy. The version counts the
  page's changes.
- **Page 0 is the index's meta page**: the root's pageid, the
  next pageid to give, the tree's height. A page id is never
  reused.
- **A leaf entry is its key alone.** The row's key ends it in
  a non-unique index, as today, and a unique one's carries it
  too: the row decides visibility, so equal keys of dead rows
  sit beside the live one (§6). INCLUDE columns are read from
  the row, which is read anyway: an entry newer than the
  reader's snapshot could not give them.
- **A page has a size limit, not a size.** It is stored at
  its length, and split when an insert would take it past the
  limit, 8 KB to begin with, leaves and inner pages alike.
  Fixed sizes are for disk blocks and buffer frames: a page
  here is a RocksDB value, compressed in RocksDB's block, and
  a cache entry charged its length, so padding would cost
  writes and memory for nothing; and every change writes the
  page again, compacted, so it keeps no free space. The limit
  stays at or under the store's block size, 16 KB
  (`sm1/SM1_DESIGN.md` §12.2), so a page's `Get` reads one
  block. A full leaf holds about 200 `c2` entries: three
  levels for a million.
- **An entry is at most a third of what the header leaves**,
  as PostgreSQL limits a btree tuple to a third of a page: a
  half of a split holds its high key, no longer than an entry,
  and two entries, so a split always finds a point where both
  halves fit with the entry that caused it. The split is made
  over the page's entries and that one, as
  `_bt_findsplitloc()` does, at the point nearest half the
  bytes where both fit. A tree's keys are cut there, about
  2.7 KB, flagged and rechecked as cut keys are now
  (`sm2/SM2_DESIGN.md` §3.2); a unique index's, never cut, are
  refused past it, where today's entries take 4,096 bytes
  (`sm1::max_keylen`).

## 4. Reads

- **A probe** descends from the root, each page from the
  cache, a miss read with a `Get` and put there, a binary
  search of each to a leaf; it moves right while the key is
  above a page's high key. An equality's probe then reads entries
  while they match; a range's, until the upper bound.
- **An IN list's probes** descend the inner pages first, all
  of them, and read the distinct leaves the cache does not
  hold in one `MultiGet`.
- **Each entry's row** is read by its key at the scan's
  snapshot, as `sm_tbl_read_keys()` reads them today, in
  batches, and kept only if its columns give the entry's key
  again. A cut key is rechecked against the row's columns, as
  now.
- **The leaf is not held** while rows are read: the scan
  copies a leaf's matching entries, and its high key, and
  releases it; the next leaf is the right link's.
- **Index-only scans read the row.** None is answered from
  the tree alone; `count(*)` by an index gets slower. A
  visibility map, or InnoDB's page-level newest writer, can
  be added later; it is no part of this design.
- **A cursor** keeps its position as the last key it returned
  and its leaf's version, and descends again if the leaf it
  reads next has another.

## 5. Writes

- **Insert**: when a row is inserted, or an update changes an
  index's key, the new entry is inserted, in a top-level
  change, before the row write returns. An entry already
  there, the same key and row key, is no change: an update
  from A to B and back to A finds A's entry still there.
- **Nothing is removed by an update or a delete.** The old
  entry stays, and its row, at the old key no longer, makes
  it dead. A primary key's change moves the row
  (`CLUSTER.md` §4): its new entries name the new key, and
  the old ones name a key that holds no row.
- **A split** latches the leaf, makes the new right page,
  links it, then posts the separator to the parent, each step
  a top-level change of its own, as Lehman and Yao order them:
  a crash between them leaves a tree a reader can walk, and
  the next insert into the parent finishes the post. Each
  page a step writes replaces its cache entry after its put,
  and the post moves the index's version; a reader on the old
  pages reaches the new one by the right link.
- **Order with the user's transaction**: the entry is inserted
  after the row's version is written, in the same statement,
  so its change is in RocksDB's WAL before the commit's: a row
  that commits always has its entry, and a crash before the
  commit leaves only a dead entry. The entry first, as this
  section once had it, lets vacuum remove an entry an insert
  has just found there (`../MAJORDECISION.md` D5).

## 6. Uniqueness

As PostgreSQL's `_bt_check_unique()`: under the leaf's latch,
each entry with an equal key, its key columns not null, has
its row read at its newest version (`sm1::read_latest`):

- a row committed and live with that key: `SM_ERR_DUPKEY`;
- a row whose writer is still running, with that key: the
  latch is released, the writer waited for (sm1's lock on the
  row's key, `LOCK.md`), and the check made again;
- otherwise the entry is dead, and the next is looked at.

Then the new entry goes in, under the same latch. A key with
a null column is not checked, as now.

## 7. Vacuum

An entry is removed when its row, at the GC watermark
(`sm1/SM1_DESIGN.md` §8.3), is gone or has another key, so no
snapshot can still read the row with it: `VACUUM` walks each
index's leaves, reads the rows of a leaf's entries, and
removes the dead ones in a top-level change, as PostgreSQL's
`btbulkdelete()`. Pages are not merged; an empty leaf stays
linked, as PostgreSQL's half-dead pages are, until a later
design deletes them. The GC filter does not touch class
`0x04`.

## 8. Cases

| Case | What happens |
| --- | --- |
| TOAST | an entry is built from the detoasted value, as now; no entry is out of line |
| non-unique index | §4, §5 |
| unique index | §6; refused as a tree until it is built (D9) |
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
| DROP INDEX | the pages stay until the store goes; a relid is never given again (D6) |
| TRUNCATE, ALTER rewrite | a new store: new trees, built as CREATE INDEX builds |
| temporary tables | a temporary store's pages, as its rows |
| keyless tables | the row key is the `_id`, as now |
| bulk load, its rollback | entries inserted as rows are; a rollback leaves them dead. A sorted build bottom-up is left for later |

## 9. What changes where

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
    sm1's (`sm1::page_read`, `sm1::page_write`), as sm1 is the
    only channel into RocksDB (`../MAJORDECISION.md` D2, D3);
    the index's version and a session's pin on its inner
    pages (§2) are not built: an open index remembers its root
    instead (D4);
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

## 10. What it buys, and what it does not

Estimates, to be measured as each part lands:

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
  up to 8 KB, into RocksDB's WAL and memtable, against an entry
  of about 40 bytes now. A write benchmark measures it before the
  design is final; the answer may be a smaller page, or logging
  the entry and writing the page later.
- **Index-only scans** get slower (§4).

## 11. Open questions

1. Whether pages share the block cache's budget with the
   rows' blocks as they are, or need a share of it set aside:
   a machine choice sm would make at a home's first attach,
   with no setting.
2. The page's limit: 8 KB, or smaller for writes (§10); and
   whether a unique index's key refused past a third of it
   (§3) is worth a 16 KB limit.
3. Whether the row read of Q9's adjacent rows, by one cursor,
   comes first: without it the tree does not bring Q9 under
   1.5.
4. How a statement asks for a tree (§12): `USING smtree`, or
   `WITH (tree = on)`.

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
  | a unique key, at most | 4,096 bytes | about 2.7 KB |
  | an index-only scan | yes: an entry is MVCC | reads the row |
  | an indexed insert, written | about 40 bytes | the page, up to 8 KB |
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
- **A root split** makes a new root of two entries, and writes
  the meta page with it, in the second change. The meta page,
  page 0, holds the root's page id and the tree's height.
- **Page ids come from an sm1 sequence** of the index's own
  (`sm1::mkseq`, made with the index, transactionally; its
  next value is not, as a page's is not): no page id is
  handed out twice, gaps are harmless, and the meta page is
  written only when the root changes.
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

**The steps:**

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
