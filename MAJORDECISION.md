# MAJORDECISION — the BTREE project, decided unattended

The user handed the whole of `sm/BTREE.md` over on 2026-10-29,
to be taken as far as it goes on the `wip-btree` branch, and
asked for each major decision to be logged here instead of
asked. Each entry is what was decided, why, and what it would
take to undo. Nothing here is merged into `main`.

## D1. A split takes the entry that caused it

- **Decided:** `page_split()` inserts the new entry and splits
  the page's entries and it together, at the point nearest
  half the bytes where both halves fit, as PostgreSQL's
  `_bt_findsplitloc()`. `entry_max()` is a third of what the
  page header leaves, `(limit - 22) / 3`, not `limit / 3`.
- **Why:** step 1 split without the new entry, and capped an
  entry at a third of the limit without counting the left
  half's new high key. A page of three 2,730-byte entries
  split so that the fourth fit on neither half, after any
  number of splits: an insert that could never be made.
  A half holds a high key, no longer than an entry, and two
  entries; with each at most `(limit - header) / 3` a split
  point where both fit always exists.
- **Undo:** none wanted; it is a bug fix.

## D2. sm1 owns a page's I/O and its cache entry

- **Decided:** the page cache's RocksDB half is in sm1, as
  four calls on keys of class `0x04`: `page_read` (through the
  block cache, pinned until `page_release`), `page_write` (a
  top-level `WriteBatch`, then each cache entry replaced), and
  `page_drop` (a prefix's pages and their cache entries). sm2's
  tree calls them; it holds no RocksDB handle. `BTREE.md` §9
  had `sm2/src/tree/cache.cc` holding a `rocksdb::Cache`.
- **Why:** `sm/CODE.md` §1: sm1's interface is the only
  channel into RocksDB, and no level above holds a RocksDB
  handle or type. The block cache is sm1's environment's.
- **Cost:** a page read pays a catalog lookup of the store
  (cached per session, `STORECACHE.md`) besides the cache's
  `Lookup`; measured in step 8.
- **Undo:** move the calls behind an sm1 handle per tree if the
  per-call check shows in the profile.

## D3. A miss's fill is latched against writes; writers' latches are per page

- **Decided:** two latches, kept apart.
  - **The fill latch**, in sm1, a stripe of 256 by the page's
    cache key: a read that misses holds it across its `Get`
    and its `Insert`; a write holds every stripe of its pages,
    in stripe order, across its `Write` and its inserts. A
    cache hit takes none.
  - **The writers' latch**, in sm2 (`src/tree/latch.cc`), one a
    page while it is held or waited for, in a sharded map by
    database, store, index and page id.
- **Why:** without the fill latch, a reader that missed and
  read a page before a write could insert it after the
  write's insert, and the cache would hold the old page; the
  next writer would change that and lose entries. The block
  cache has no compare-and-insert. The writers' latch cannot
  be a stripe: a split holds a page's latch and its parent's,
  and two pages on one stripe would deadlock it.
- **Undo:** none planned. The fill latch costs a miss a mutex;
  a miss costs a `Get` anyway.

## D4. Page ids from the meta page; descents from a remembered root

- **Decided:**
  - **A tree's next page id is in its meta page**, page 0,
    bumped under the meta page's latch in the same batch as
    the split's first change that uses it. No sm1 sequence.
  - **An open index remembers its root's page id**, and
    descends from it; it reads the meta page again only to
    post a separator, which descends from the true root.
  - **A post finds its parent by a descent**, from the meta
    page's root to the level above, by the separator: no stack
    of parents is kept.
  - **Making a tree first drops every page under its relid**,
    so pages left by a tree whose `CREATE INDEX` aborted, or
    by a dropped index whose relid is reused, are gone, and
    their cache entries with them.
- **Why:** `BTREE.md` §13 proposed a sequence of the index's
  own, made with it transactionally. Pages are written outside
  the transaction, so an aborted `CREATE INDEX` leaves pages
  the sequence no longer knows; the meta page goes with the
  pages, and a split's batch already writes three pages. A
  page is never deleted (§7: no merge), so a root that has
  split is still a page of the tree, the leftmost of its
  level, and a descent from it reaches every key by right
  links (Lehman and Yao). Splits are one insert in about 200,
  so a post's extra descent costs little, and a stack would be
  stale when the root moves anyway.
- **Undo:** keep a stack if the post's descent shows; the meta
  page's counter can become a sequence without a format
  change elsewhere.

## D5. The entry after the row; vacuum removes only what is settled

- **Decided:**
  - **An entry is inserted after its row's version is
    written**, in the same statement, so before the commit.
    `BTREE.md` §5 had it before.
  - **Vacuum removes an entry only when its row is settled**:
    its newest version committed at or below the GC watermark,
    so no snapshot can see another, and that version gone or
    of another key. It checks under the leaf's latch, which an
    insert holds to find an entry already there.
- **Why:** an insert that finds its entry already there (an
  update A to B and back, or a delete and reinsert) changes
  nothing. With the entry first, vacuum could find the row
  dead between the insert's "already there" and the row's
  write, remove the entry, and leave a live row with none.
  With the row first, vacuum under the latch sees the row's
  uncommitted version and keeps the entry; or it removes the
  entry first, and the insert then finds none and makes it.
  The WAL is one sequence, so the entry is in it before the
  commit record either way: a committed row always has its
  entry, and a crash before the commit leaves a dead one.
  Removing only settled rows' entries also covers versions
  between the watermark and the newest (A, then B committed
  after the watermark, then A again: a snapshot between still
  needs B's entry).
- **Undo:** none planned; it narrows what vacuum removes at
  once, and a later pass removes the rest.

## D6. A dropped tree's pages stay until its relid or store is reused

- **Decided:** `DROP INDEX` of a tree removes it from the
  catalog and leaves its pages. A tree later made under the
  same relid drops them first (D4); `DROP TABLE`, `TRUNCATE`
  and a rewrite drop the store, and the pages with it.
- **Why:** sm3 drops an index inside the transaction, so a
  rollback brings it back. A tree's pages are written outside
  every transaction: dropped at the `DROP`, a rollback would
  bring back an index with no pages. Dropped after the commit,
  by `sm1::on_commit`, there is no transaction left to name
  the store in, which every page call needs.
- **Cost:** a dropped tree's pages take disk until then.
- **Undo:** drop them at the next transaction's start, from a
  list sm3 keeps, or give sm1 a page drop that needs no
  transaction.

## D7. A tree is asked for by `WITH (tree = on)`

- **Decided:** `CREATE INDEX ... WITH (tree = on)` makes a
  tree; any other option is still refused, and `USING btree`,
  the default, stays the one access method. (`BTREE.md` §11,
  question 4.)
- **Why:** a tree is the same B-tree to the planner: the same
  operator classes, order and costs. `USING smtree` would need
  a new `pg_am` entry, its operator classes and the planner's
  knowledge of them, for no difference it can use. A storage
  parameter is how PostgreSQL asks for a variant of one access
  method (`fillfactor`, `deduplicate_items`).
- **Undo:** the flag is the catalog's; a second spelling is
  parser work only.
