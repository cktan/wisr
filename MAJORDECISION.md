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
  page header leaves, `(limit - 30) / 3`, not `limit / 3`.
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

*Superseded by D14*: there is no meta page, and the root does not
move. What follows is the first decision.

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
  move right lets them go, so the next reads afresh from page 0
  (D14). `BTREE.md` §2's index version and session pin are not
  built.
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

## D14. The root is page 0, and there is no meta page

Decided with the user (2026-10-31), revising D4.

- **Decided:**
  - **Page 0 is the root, for the tree's life.** The meta page's
    tag, root and height go: its header says what the tag did,
    its id is known, and its level + 1 is the height.
  - **A root's split is one write**: both halves to new pages,
    and page 0 a level up, their parent, as InnoDB's root stays
    put. It is never flagged incomplete, so `post()` loses the
    case of a root's split cut short.
  - **Page ids come from one sequence for the database**,
    `sm3.tree_page` (`sm2::tree_page_seq`), made by `cat_init()`,
    from 1. Holes cost nothing.
  - **Whoever reads a page again by its id checks its level**:
    `latch_covering()`, a cursor, `tree_prune()`. Only page 0 can
    differ, and each then descends again.
- **Why:** RocksDB's `WriteBatch` is atomic and a page has no
  fixed size, so a root can stay where it is. It saves a
  transaction's first descent a cache lookup. It ends a split's
  write of the meta page and the latch every split in a tree
  took on it, and `post()`'s `stale_root` path. It also answers
  the TODO of caching the root across transactions, which would
  now save one lookup, about 470 instructions, against the risk
  of a stale root when a tree is made again under its relid.
- **The sequence:** D4 refused one because a sequence made by
  `CREATE INDEX` is undone by its abort, and the pages are not.
  One made with the catalog exists before any index. `nextseq`
  never hands a value out twice, across a restart too, so a
  crash skips ids and cannot reuse one.
- **Cost:** page 0's level changes, and its entries move to new
  pages, the one case of a key moving other than right; a
  page's level is no longer fixed for life. `0` stays free to
  mean "none" in a link, as the root is nobody's child or
  sibling.
- **Undo:** a meta page again, and a format change.

## D15. Every page has a left link, for a reverse scan later

Decided with the user (2026-10-31).

- **Decided:** a page's header has `prev` beside `next`, which
  was `right`. A split's first write writes the page's old right
  sibling too, linked back to the new page, latched left before
  right. `tree_check()` checks it; nothing else reads it yet.
- **Why:** a reverse scan could instead descend once a leaf, with
  no write and no header bytes, at a descent's cost a leaf. The
  link is the cheaper read, and the user asked for it.
- **Cost:** 8 bytes of header; a split that has a right sibling
  reads and writes one more page, and waits for its latch.
  The rightmost page has none, so ascending loads pay nothing.
- **Undo:** the field, and that write.

## D16. The pool is built before its baseline is measured

- **Decided:** on 2026-10-31 the user asked for work to go on
  unattended on the buffer pool, page and B-tree, on a machine
  that may not run benchmarks and offers no instruction counter.
  So `BUFFERPOOL.md` steps 6 and 7 are built with step 1 not
  run, though §5 says to stop and ask, if it shows no contention,
  before the pool. Every verify a test can make is made; those
  that need a benchmark stay open: step 5's Q9 and Q10, and §10's
  grid.
- **Why:** the pool is the next code, and its design was settled.
  What it risks is measured cost, not correctness, and it is all
  on a branch.
- **Undo:** the commits are the branch's last; revert them. The
  baseline should still decide whether they merge.

## D17. A quarter of the budget goes to the pool, 64 frames at least

- **Decided:** `block_cache_bytes` stays one number: attach gives
  a quarter to the pool, in frames, and three quarters to the
  block cache, so `shared_buffers` keeps its meaning.
  `BUFFERPOOL.md` §4.5 leaves the split to the user, and names
  this as the start to measure. The floor is 64 frames, even past
  the budget.
- **Why:** two numbers would be a setting no one can yet choose.
- **Undo:** one line of `attach()`, or an option of its own.

## D18. The pool is a module of its own, named bufpool

- **Decided:** `sm1/src/bufpool.h`, `bufpool_t`, knows tags,
  bytes and pins, and reads a miss through its caller's loader,
  as the lock table is a module of its own. The user named it. A
  tag lies in relaxed atomics, not a hash of it, so a lookup
  compares the whole tag with no lock, and never mistakes one
  page for another; a pin makes the comparison sure.
- **Why:** so it is tested alone, every path of a pin reached on
  purpose, with no RocksDB.
- **Undo:** none planned.

## D19. A sleeping pin leaves new pins behind it, not a thread's own

- **Decided:** a pin that has not yet slept will not pass a frame
  with a sleeper, so readers do not starve a writer of a hot
  page; one that has slept, or a second shared pin that spun out,
  takes its turn. A pin never sleeps on a frame whose tag has
  changed: a lookup is lock-free, so it may hold a frame just
  taken for another page, whose holder may be waiting for a page
  this thread has pinned, a cycle no latch order forbids.
- **Why:** both are deadlocks or starvation that no test would
  reach by chance.
- **Undo:** none planned.

## D20. D2, D3 and D11 are replaced

- **Decided:** the pool replaces the page cache in the block
  cache (D2), the fill latch and the writers' latch table (D3),
  and an open tree's held pages (D11): `tree_close` goes, a tree
  holds nothing between calls, and `descend()` loses `afresh` and
  `moved`. `sm1::session_dbid` went with the table's names.
- **Why:** `BTREE.md` §17, decided with the user.
- **Undo:** with D16.

## D21. attach is told max_sessions, for the pool's floor

- **Decided:** `sm_env_options_t` gains `max_sessions`, 0 by
  default. The pool has at least six frames a session, past the
  budget if need be, which D17's floor alone did not give.
- **Why:** a session waiting for a frame holds up to five, so a
  floor too low can leave every session waiting on the others.
  `BUFFERPOOL.md` §3.5 called for a floor by sessions.
- **Undo:** the option goes, and with it the guarantee.


## D22. Vacuum latches a leaf to judge it whole only if it may be set

- **Decided:** `tree_prune()` judges a leaf's entries without
  its latch, as before. A leaf where every entry looked dead
  or settled is then judged again whole, latched from the
  first row read to the write, its dead entries removed and
  the bit set if every one left is settled still. Any other
  leaf is pruned as before, only its dead judged again. The
  judge answers 1 dead, 0 keep, 2 keep as settled and live; a
  judge that never says 2, as every one before this did, sets
  nothing. `BTREE.md` §4.1's algorithm latched every leaf not
  set before judging it.
- **Why:** the bit needs the whole judging under the latch, so
  a delete whose row was written first is seen. A leaf with an
  entry not yet settled cannot be set, and latching it only
  keeps its readers and writers waiting on vacuum's row reads.
- **Undo:** latch every leaf not set, as §4.1 wrote it.

## D23. The planner's allvisfrac is left at 1

- **Decided:** `estimate_rel_size()` still gives an sm table
  `allvisfrac` 1, which a tree's index-only scan is costed by.
  §4.1 proposed a tree's fraction of set leaves.
- **Why:** PostgreSQL keeps `allvisfrac` per table and applies
  it to every index in `cost_index()`, a path dg shares; a
  tree's fraction is per index. A tree's index-only scan was
  costed as reading no row before this change; it is now more
  often true. Changing the cost is its own step, measured.
- **Undo:** vacuum counts a tree's set leaves into its
  statistics, and dg's index cost hook scales a tree's
  index-only scan's row reads by the rest.

## D24. Only a tree of key columns alone skips rows, asked by digres

- **Decided:** sm3's tree cursor has an index-only mode
  (`tbl_scan_entries_only`, `sm_rel_scan_entries`): an entry
  on a leaf set, not cut, of an index whose keys give their
  columns back, is handed out with its row unread and no ref,
  `tbl_scan_cols` (`sm_rel_scan_cols`) giving its columns.
  digres turns it on in `smstore_index_getentry()`, for a tree
  with no INCLUDE columns.
- **Why:** a tree's entry holds no INCLUDE column (§3.3), so
  one with them must read the row anyway; a cut key, or a type
  whose key keeps no datum, gives no column back. The mode is
  the caller's to ask, as an index scan, not index-only, needs
  every row.
- **Undo:** none wanted; INCLUDE columns in a tree's entry
  would widen what skips.

## D25. A unique insert that finds its key clears its leaf

- **Decided:** `tree_insert_unique()` no longer returns at
  once when the key is there: it latches the key's leaf, as
  for an insert, and `insert_into()` finds the key and clears
  the leaf's bit, as `tree_insert()` does.
- **Why:** an update A to B and back to A finds A's entry
  there; the row changed, and the leaf set before it would
  hand out A's entry unread at a snapshot that should read
  the row.
- **Undo:** none wanted; it is the rule of §4.1's INSERT.

## D26. The bench vacuums after making its indexes

- **Decided:** `bench/mkregidx.sql` and `bench/mkbtidx.sql`
  end with `vacuum analyze usertable`, not `analyze`, on both
  systems.
- **Why:** the bit is set by vacuum alone, and digres has no
  autovacuum; PostgreSQL's visibility map is set by vacuum
  too, and a server running long would have had autovacuum.
  Without it q2 measured neither.
- **Undo:** `analyze` again, to measure an index never
  vacuumed.

## D27. A tree's delete reports its entry's key to SSI

- **Decided:** `idx_delete()` on a tree, after clearing the old
  key's leaf, calls `ssi_write_key` on that entry's key, as
  `idx_insert()` does on a new one. A delete, an update that
  changes the key, and a primary key's change all reach it.
- **Why:** a serializable reader registers a tree's range
  (D10), and the rows it reads; an index-only scan that skips
  a row on a set leaf registers no row. A delete of that row
  wrote only the row, so no rw-edge met the reader: two
  transactions could each count what the other changed and
  both commit. PostgreSQL's index-only scan predicate-locks
  the heap page it skips for the same reason. sm3's test
  "a delete fails a reader whose index-only scan read no row"
  showed both committing before this.
- **Cost:** a serializable delete or key change adds an
  SIREAD lookup per tree; below serializable, nothing.
- **Undo:** register each skipped row's key as an SIREAD at
  the reader instead, one a row, as PostgreSQL's page lock.
