# BTREE-todo — the root at page 0, and no meta page

The planned work on the `wip-btree` branch, from the review of
the TODOs in `934c5b7e`. Nothing below is built yet. It revises
`../MAJORDECISION.md` D4 and D11, and `BTREE.md` §2, §3 and §13.
Paths are relative to `sm/`.

---

## 0. Resuming

This file is enough to pick the work up in a new session, with
no memory of the one that wrote it.

**Where it stands** is §4. Confirm it first, with `git status`
and `git log`: the branch is `wip-btree`, and nothing of §2 is
built.

**Next:** §2, step 1. The user asked for this work; commit only
when asked (`CODE.md` §11), with `make format` first and both
suites passing.

**Read first:** `CODE.md`; `sm2/src/tree/tree.cc` and `tree.h`
whole; `../MAJORDECISION.md` D3, D4, D11, D13;
`sm2/src/rel/rel.cc` lines 80 to 95, for how a `seqref_t` is
kept and drawn; `sm3/src/catalog/ddl.cc`, `cat_init()`.

**Running things:**

- Build sm first, as a debug build: `DEBUG=1 make -C sm -j4`.
- One level's tree tests: `DEBUG=1 make -C sm/sm2 -j4
  build/test_sm2`, then, in `sm/sm2`, `./build/test_sm2
  -tc='tree:*'`. sm1's are `build/test_sm1`, the same way.
- The full suite, `DEBUG=1 make -C sm -j4 test`, takes over ten
  minutes, past a foreground command's limit: run it in the
  background, to a log. dg's takes longer. It passes when every
  doctest line reads `0 failed`, sm1's reads `scenarios: 122
  scripts match, versioned and chained`, and the last is `All
  rel tests passed.`
- Do not edit `sm/` while dg's suite builds: it links sm as
  built.

**Rules that bear on this work:** one issue at a time; a test
written first and seen to fail; no change to a scenario in
`sm1/mvcc` without the user's approval. `sm1/include/sm1.h` is
aligned by hand, and `make format` leaves it out.

---

## 1. Decided

- **Page 0 is the root, always.** A tree's first page, and its
  root for life: the root's key is `relid | 0`, with nothing to
  look up first.
- **There is no meta page.** Each of its fields goes:

  | Field | What replaces it |
  | --- | --- |
  | tag `BTM1` | nothing: page 0's header and `check()` |
  | `root` | page 0 |
  | `height` | the root's own level + 1 |
  | `next` | a sequence, §1.1 |

- **A root split is one atomic batch.** RocksDB's `WriteBatch`
  is atomic and a page has no fixed size, so one batch writes
  the left half and the right half to new pages, and page 0 as
  their parent, a level up, as InnoDB's root stays put. It is
  never left half done, and page 0 is never flagged incomplete.
- **Other splits stay two writes**, with the incomplete flag
  and `finish_split()`, as now.

### 1.1 Page ids

- **One sequence for the database**, not one a store or an
  index, starting at 1, so page 0 is never handed out. A
  sequence per store would work too; holes are harmless, as
  ids need only be unique within a tree. One a database has no
  table's lifetime to follow.
- **Made in `cat_init()`** (`sm3/src/catalog/ddl.cc`), beside
  `sm3.oid`. Its name starts `sm3.`, so `reserved_seq()` keeps
  it from users. sm2 names it, a constant in `tree.h`, as sm2
  is what draws from it.
- **Drawn by a `seqref_t`**, looked up at an open tree's first
  split, as `rel_t` keeps `id_ref` (`sm2/src/rel/rel.cc`). A
  root split draws two.
- **Why it is safe:** `sm1::nextseq` never hands a value out
  twice, across restarts too; a crash only skips values, and
  an abort gives none back. D4's objection was to a sequence
  made by `CREATE INDEX`, which an abort undoes; this one
  exists before any index.
- **What it removes:** the allocation latch, so splits in one
  tree no longer take turns, and a split's batch writes 2
  pages, not 3.

### 1.2 What changes in the tree's invariants

- **A page's level changes for page 0 alone.** Every other page
  keeps its level for life. Page 0 goes up a level at each root
  split, and its entries move to new pages.
- **`0` still means "none" in a link.** `right` and `child_of()`
  may keep it: the root is nobody's child or right sibling, so
  no link names page 0.
- **A held page 0 can go stale past the level asked for**
  (§2 step 3), where a remembered root used to be left behind
  for the meta page's. Its bytes never change, and it still
  routes rightly: the children it names stay where they are.

---

## 2. Order of work

Each of the steps 1, 5 and 6 ends with `DEBUG=1 make -C sm -j4
test` passing. Steps 2, 3 and 4 are one change: page 0 as the
root does not pass until all three are in, as `tree_check()`
stops before page 0 until step 4. Split them only to review
them.

1. **The sequence.**
   - sm2's constant, and `mkseq` in `cat_init()`.
   - `tree_t` keeps a `seqref_t`; a draw of a page id.
   - Test fixtures that skip `cat_init()` make it by hand, as
     they make `sm3.oid`: `sm2/test/db.h`,
     `sm3/test/test_catalog.cc`, `sm3/test/test_ddl.cc`.
2. **The root at page 0, in `sm2/src/tree/tree.cc`.**
   - `tree_create()` writes an empty leaf at page 0.
   - A root split: a new `split_root()`, called by
     `insert_into()` when the page is 0, and by `post()` when
     the full parent is 0, taking the child's unflagged write
     into its batch. It draws two ids, writes both halves and
     page 0, under page 0's latch.
   - `post()` loses `m.root == childid`: the child can no longer
     be the root. `write_split()` takes its id from the
     sequence, with no latch.
3. **Level checks where a page is read by id.**
   - `descend()`: with `v->level() < level`, drop the held pages
     and go again from page 0, once; lower still is corrupt.
     Page 0 read anew is sure to be above.
   - `latch_covering()`'s callers: after the latch, a page whose
     level is not the one wanted is page 0 grown under them.
     Descend again: `latch_leaf()` wants 0, `post()` its parent's
     level.
   - `fill()`: `c.leaf` read again may be page 0 grown. Not a
     leaf: descend again from `c.from`.
   - `tree_prune()`: as `latch_covering()`.
4. **Loops that stop at 0.** `tree_prune()` (`tree.cc:873`) and
   `tree_check()` (`tree.cc:935`) run `while (0 != pageid)` and
   may start at page 0: test after reading the page.
   `tree_height()` and `tree_check()` take the height from page
   0's level. `cursor_t::leaf` may now be 0; `started` already
   tells that from "not begun".
5. **Remove what is left unused.** `meta_t`, `meta_id`,
   `meta_tag`, `meta_size`, `meta_bytes()`, `read_meta()`,
   `tree_t::root`, `descend()`'s `from_meta`, and the
   `stale_root` path of `post()`. `latches_held()` loses its one
   caller outside `test_latch.cc`, so it and its test go too.
6. **Documents.**
   - `../MAJORDECISION.md`: D4 revised, D11's "forgets the root"
     revised, a new entry for §1 above.
   - `BTREE.md` §2, §3 ("Page 0 is the index's meta page"), §13;
     `sm2/src/tree/tree.h`'s header; `sm1/include/sm1.h`'s
     comment on an inner page, if it changes.
   - Delete the root-caching TODO in `sm1/include/sm1.h`: with
     no meta page, a root cached across transactions would save
     one cache lookup, about 470 instructions (D11's numbers),
     for the risk of a stale root across a relid's reuse.
   - `../dg/WORKNOTE.md`: a step.
7. **Measure.** `make -C ../dg release -j4`, then
   `../bench/btree.sh`: Q9, Q10 and the two 16-client mixes,
   recorded in `NEXT.md` §1 item 3.

---

## 3. Tests

| Layer | What | |
| --- | --- | --- |
| mvcc | none | the scenario driver has no verb for a tree |
| sm1 | none new | `nextseq` and pages are unchanged |
| sm2 | below | |
| rel | `test_tree.rel`, across a restart | ids go on past a restart |
| aa, sq | none | neither is touched |
| dg | `test/slt/btree_tree.test`, pgregress | a tree under SQL, unchanged |

New or changed in `sm2/test/test_tree.cc`:

- **A root split leaves no half**: the insert that splits a root
  with `detail::fail_posts` set still leaves the tree checking
  whole. Replaces "a root's split cut short is finished by its
  right page's", which can no longer happen.
- **A cursor across a root split**: a cursor stopped on a root
  leaf, the root split under it, reads on without a key lost or
  repeated.
- **A writer racing a root split**: threads inserting into a
  root leaf, each of them reaching page 0 before it grew;
  `tree_check()` passes, every key is there.
- **Stale page 0**: a held root, grown past it by a writer, and
  a `post()` that asks for a level above it.
- **Page ids go on across a restart**, and none is given twice.
- **`tree: a leaf a probe reads is evicted before the inner
  pages`**: it names page 1 as the leftmost leaf, and page 0 as
  the meta page; take the leftmost leaf from page 0's first
  entry.

In `sm3/test/test_tree.cc`, `has_meta` keeps working, as it looks
at `relid | 0`: only its name goes. Run the tree tests under TSan
(`sm/sm2`), as the concurrency in step 3 is new.

---

## 4. Where it stands

- **Done and pushed**, in `43e15ea9` on `wip-btree`: `page_read`
  and `page_release` renamed `pin_page` and `unpin_page`;
  `sm1::page_cached`, for tests; a leaf a descent misses cached
  as a leaf, not inner; the answers to the SH/EX and `page_drop`
  TODOs. On it, sm's full suite and dg's pass, but for
  pgregress, which skipped: no PostgreSQL 18 build in
  `~/p/postgres18` (set `PG18_SRC`).
- **Not started**: everything in §2. The root-caching TODO is
  still in `sm1/include/sm1.h`, until step 6.
- **A database made before this cannot be read**: its trees have
  a meta page at 0, and it has no sequence. Nothing keeps
  compatibility before production: run initdb again.
- **Unmeasured, separate from this plan:** a cursor reads each
  leaf again to move on (`fill()`), which counts as a hit, so a
  long scan moves its leaves to the high-priority pool, and may
  push inner pages out.
