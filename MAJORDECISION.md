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
  four calls on keys of class `0x04`: `pin_page` (through the
  block cache, pinned until `unpin_page`), `page_write` (a
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

## D6. A dropped tree's pages go after its drop commits

*Revised*: `sm1::page_drop` may be made outside a transaction, and
`DROP INDEX` of a tree drops its pages from an `sm1::on_commit`
function; a crash between the commit and the function leaves them,
as this entry first had it for every drop, until the store goes.
What follows is the first decision.

## D6, first. A dropped tree's pages stay until its relid or store is reused

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

## D8. sm3 hands a tree's scan out as rows, checked against their entries

- **Decided:** `tbl_idx_scan_rows`, a primary key's scan of
  rows, takes a tree too. For each entry sm3 reads the row at
  the transaction's snapshot into the cursor's pin, makes the
  row's entry for that index as a write would (the evaluator
  asked for an expression or a predicate), and hands the row
  out only if its key is the entry's. digres scans a tree as it
  scans a primary key, by that cursor, its index-only scans
  served from the row as a primary key's are.
- **Why:** the check needs the index's definition and the
  host's evaluator, which sm3 has and sm2 has not; digres would
  otherwise read each row twice, once for the check and once
  for the executor. A primary key's path in digres already
  takes rows from a cursor, so digres changes in three lines.
  An entry's key compared as bytes is exact: it holds the row's
  key, so a stale entry for another value is told apart even
  when that value still meets the query's bounds.
- **Cost:** each row is deformed and its key encoded again;
  measured in step 8. digres's read-ahead of rows, many a call,
  is not used for a tree yet.
- **Undo:** a batch read of the entries' rows in sm3, as
  `rel_read_batch`.

## D9. A unique index is not a tree yet (superseded: built)

- **Decided:** `CREATE UNIQUE INDEX ... WITH (tree = on)`, and a
  primary key as a tree, are refused (sm3 and digres). `BTREE.md`
  §6's check is designed, not built.
- **Why:** the check reads the newest version of every row an
  equal key names, waits for a running writer and checks again,
  under the leaf's latch held across leaves; it needs a wait
  for a writer from sm1 that no call gives yet. The non-unique
  tree answers Q9 and Q10, the queries this project is for.
- **Undo:** build §6 next; nothing on disk changes.

## D10. A tree's range is an SIREAD of its own, not narrowed

- **Decided:** sm1 gains `ssi_read_range` and `ssi_write_key`,
  on keys of a store no version holds. A serializable scan of a
  tree registers its whole requested range; an insert into a
  tree reports its entry's key.
- **Why:** a row read by key registers that key alone, so an
  insert into a range a serializable transaction scanned would
  go unseen: a write skew through a tree's range. sm1's
  tracker compares keys as bytes, and an entry key under the
  index's relid meets no row's key.
- **Cost:** the range is the scan's bound, not what it read:
  a scan stopped early, as by `LIMIT`, keeps the whole range,
  and may fail a writer it need not.
- **Undo:** narrow it at the cursor's end, as sm1's own scans
  do, with a call that takes the last key read.

## D11. An open tree keeps the inner pages it reads pinned

- **Decided:** a tree, as an index is opened for a statement,
  keeps the inner pages its descents read pinned in the block
  cache, up to 64, until it is closed; a descent reads them
  from there, with no cache `Lookup`. A descent that has to
  move right lets them go, and forgets the root, so the next
  reads afresh from the meta page. `BTREE.md` §2's index
  version and session pin are not built.
- **Why:** Q10's 100 probes each looked up three pages in the
  block cache, its shard mutex and hash, 5% of the statement.
  A held page may go stale as a split changes it, and a
  descent from it stays right: no page is ever freed, and a
  key moves only right, where the right links reach it. But a
  long statement, a million-row insert, routed through stale
  pages walked right a long way: the load went from 51 s to
  424 s until a move right let them go.
- **Measured:** Q10 2,394k to 2,255k, Q9 1,582k to 1,548k.
- **Undo:** none planned; held_max bounds the memory pinned.

## D12. A page's limit is 4 KB

- **Decided:** `page_limit` is 4 KB, not 8 KB. A static check
  keeps a keyed table's entries room for 256 bytes of key
  columns past the longest row key.
- **Why:** an insert writes its page whole. A million rows into
  `wide_mixed`: 51 s at 8 KB, 34 s at 4 KB, 27 s at 2 KB, 18 s
  with today's entries; Q9 and Q10 moved under 2% across the
  three. 2 KB leaves no room for a primary key of 1,024 bytes,
  `max_pk_len`, at a third of a page.
- **Undo:** the constant; a limit per index is a field of the
  meta page away.

## D13. A unique tree is built, an ON CONFLICT arbiter too

- **Decided:** a unique index may be a tree (D9 is undone).
  An insert latches the leaf that covers its key's prefix,
  judges every other entry of the prefix, on that leaf and the
  ones to its right, by its row's newest version past the
  snapshot (`sm1::read_newest`), and inserts while still holding
  that latch: a live row with the key, committed or this
  transaction's, is `SM_ERR_DUPKEY`; one another transaction is
  still writing is waited for (`sm1::wait_writer`, in the
  deadlock check, with every latch let go), and the key checked
  again. A primary key is never a tree.
- **An arbiter** (`INSERT ... ON CONFLICT`) looks a key up as
  `idx_find` and `idx_find_latest` do: each entry of the key,
  its row at the snapshot, or its newest version, a running
  writer waited for, then the row locked shared.
- **Why:** every insert of one prefix takes the same first
  latch, so two take turns; the entries to the right are read
  without a latch, as an insert of the prefix waits for the
  first and a split only moves them right. The arbiter's lookup
  locks the row where a unique index of sm1's entries locks the
  key; an insert of the key waits for the row's writer either
  way.
- **Undo:** none.
