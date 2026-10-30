# BTREE — secondary indexes as B+trees

Status: proposed, for review. Nothing is built.

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

Proposed here:

- **Pages are keys of the table's store**, under a class byte
  of their own, so a store stays one column family
  (`sm1/SM1_DESIGN.md` §3.1). A change's pages are one
  RocksDB `WriteBatch`, in RocksDB's WAL: it is atomic and
  durable with nothing of ours to recover.
- **No buffer pool.** A shared pool needs a replacement
  policy, and one of sm's own would be a second cache beside
  RocksDB's block cache, competing with it for memory:
  - **Inner pages are kept in memory**, decoded: per index an
    immutable snapshot of them behind a
    `std::atomic<std::shared_ptr>`, in sm's environment and
    shared by every session, which a reader loads with no
    lock and a split replaces. They are about 1/200 of an
    index, 200 KB a million entries, so nothing is evicted
    while they fit; past a size sm chooses at a home's first
    attach, with no setting, the level above the leaves is
    read from RocksDB instead, and the levels over it stay.
  - **Leaves are read from RocksDB**, a `Get` of the page's
    key through the block cache, which keeps and evicts them,
    and are searched in place, in the pinned value (§3).
  - If decoded leaves are ever worth keeping, they go into
    the block cache as entries of their own
    (`rocksdb::Cache::Insert`, a charge and a deleter): its
    budget and its eviction, no policy of sm's.
- **Lehman and Yao's B-link tree**: each page has a right
  link and a high key, so a split never blocks a descent.
  **A reader takes no latch**: a `Put` replaces a page whole,
  so a `Get` sees it before or after a change, and the right
  link covers a split under way. A writer latches the page in
  a latch table by page id, as sm1 latches a key
  (`sm1/SM1_DESIGN.md` §5.1).

## 3. Layout

```
page key = 04 | relid (2B) | pageid (8B, big-endian)
value    = header | slots | entries
header   = level (1B, 0 a leaf) | flags (1B) | n (2B)
         | version (8B) | right (8B, 0 none)
         | high key (variable, none on the rightmost page of
           a level)
slots    = n offsets (2B each), of the entries in key order
entry    = leaf:   key bytes (the entry's sm2 key, as today:
                   encode(sk) ‖ row key, cut flag)
           inner:  separator key | child pageid
```

- **Class `0x04`** sorts after the rows (`0x00`) and a chained
  store's (`0x01`, `0x02`). Keys of the class compare
  bytewise, as `0x01` and `0x02` do; the GC filter keeps them
  as they are (§7).
- **The slots let a page be searched where it lies**: a
  binary search over them reads the value the `Get` pinned in
  the block cache, with no decode and no copy. The version
  counts the page's changes.
- **Page 0 is the index's meta page**: the root's pageid, the
  next pageid to give, the tree's height. A page id is never
  reused.
- **A leaf entry is its key alone.** The row's key ends it in
  a non-unique index, as today, and a unique one's carries it
  too: the row decides visibility, so equal keys of dead rows
  sit beside the live one (§6). INCLUDE columns are read from
  the row, which is read anyway: an entry newer than the
  reader's snapshot could not give them.
- **Pages of 8 KB**, PostgreSQL's, to begin with: about 200
  `c2` entries a leaf, three levels for a million. The size
  is measured against write amplification (§10).

## 4. Reads

- **A probe** descends the inner pages in memory, a binary
  search of each, to a leaf, which it reads with a `Get` and
  searches in place; it moves right while the key is above a
  page's high key. An equality's probe then reads entries
  while they match; a range's, until the upper bound.
- **An IN list's probes** descend the inner pages first, all
  of them, and read their distinct leaves in one `MultiGet`.
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
  the next insert into the parent finishes the post. The
  post makes the index's inner pages again, a new snapshot,
  swapped in for the old; a reader on the old one reaches the
  new page by the right link.
- **Order with the user's transaction**: the entry's change is
  in RocksDB's WAL before the row's write, so a row that
  commits always has its entry, and a crash that loses the
  row leaves only a dead entry.

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
| unique index | §6 |
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
| SSI | a scan registers the range of entry keys it reads as an SIREAD in sm1's tracker, under the index's relid, and an insert calls `on_write` for its entry's key: the keys need not be sm1's to be tracked (`sm1/SM1_DESIGN.md` §7.1) |
| open cursor, Halloween | an update's new entry ahead of the cursor is read, and its row, at the statement's snapshot, has the old key: skipped |
| GC watermark, CF:txn sweep | §7; the tree holds no xid, so the sweep is unaffected |
| CREATE INDEX pending | the backfill inserts entries as a writer does, each a top-level change; an entry already there is no change |
| DROP INDEX | `DeleteRange` of `04 \| relid` |
| TRUNCATE, ALTER rewrite | a new store: new trees, built as CREATE INDEX builds |
| temporary tables | a temporary store's pages, as its rows |
| keyless tables | the row key is the `_id`, as now |
| bulk load, its rollback | entries inserted as rows are; a rollback leaves them dead. A sorted build bottom-up is left for later |

## 9. What changes where

- **sm1**: class `0x04`, its comparison and the GC filter's
  pass-over; a top-level `WriteBatch` of pages; SIREAD and
  `on_write` on keys that are not stored.
- **sm2, `src/tree/`**, beside the index `rel` that uses it
  (`sm2/SM2_DESIGN.md` §5), each part a file of its own with
  unit tests of its own in `sm2/test`:
  - **the page**: its layout, the in-place search of its
    slots, an entry's insert and a split's halves, on bytes,
    with no RocksDB;
  - **the inner-page cache**: per index, the snapshot of its
    inner pages, shared and loaded with no lock, made again
    and swapped at a split, and the size rule of §2. It reads
    pages through a function it is given, so its tests give
    it pages from memory and drive splits from many threads,
    under TSan;
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

- **Q10**: the probe's 30k becomes a descent of the inner
  pages in memory, about 1k, and a leaf's `Get`, about 4k,
  less in the IN list's one `MultiGet`; Q10 about 1.3M
  instructions, **1.6 to 1.7 times** PostgreSQL. The rest is
  the row read by key, Q7's cost. A buffer pool of decoded
  leaves would save about 2k a probe more, 1.55 times.
- **Q9**: an entry about 200 instructions, not 2,820; but each
  row read by key costs about 8,400. Q9 about 1.1M, **2.9
  times**, until rows of sorted, close keys are read by one
  cursor (`NEXT.md` §1 item 3, step 2): then about 0.5M, 1.3
  times.
- **Writes**: an indexed insert writes an 8 KB page into
  RocksDB's WAL and memtable, against an entry of about 40
  bytes now. A write benchmark measures it before the design
  is final; the answer may be a smaller page, or logging the
  entry and writing the page later.
- **Index-only scans** get slower (§4).

## 11. Open questions

1. The size past which the level above the leaves is read
   from RocksDB, not kept (§2): a machine choice sm makes at a
   home's first attach, with no setting.
2. Page size: 8 KB, or smaller for writes (§10).
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
  | a probe of a non-unique key | about 30k (`Seek`) | about 5k |
  | a probe of a unique key | a `Get`, near PostgreSQL's | about 5k |
  | an index-only scan | yes: an entry is MVCC | reads the row |
  | an indexed insert, written | about 40 bytes | the page, about 8 KB |
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
