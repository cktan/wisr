# BUFFERPOOL — sm's own buffer pool for a tree's pages

The design and plan of the project `BTREE.md` §17 decides. It
replaces that design's §14, §15, and §16's fill latch and
writers' latches. §17's decisions are taken as given, §2 here
restates them, and this file adds what they leave open. Of §5's
steps, 2 to 7 are built, the pool in `sm1/src/bufpool.h`. Step 1,
and with it every measure below, has not been run: §5's verifies
that need a benchmark are open (`../MAJORDECISION.md` D16).
Paths are relative to `sm/`. A bare §17 below is `BTREE.md`'s.

---

## 1. What was measured first

Measured 2026-10-31 at `bdb1f4a3`: optimized build, RocksDB
8.9.1, ext4 on a network disk, an EPYC 7B12 VM of 2 cores and 4
threads. A throwaway program, in no commit, drove `sm1` directly;
the prototype below is 60 lines, not a pool.

**Not measured here.** `perf` is refused on this VM, and
`pgbench` is not installed, so Q9 to Q12 have no new numbers; 4
threads cannot show what 16 clients do. Plan step 1 is for that.

### 1.1 A pin

ns for a pin and its unpin, a thread, on a hit. The prototype is
a CAS on one word a frame, found by a lock-free read of a chained
hash, its tag checked after the pin. Its tag is one integer, so
it is a floor: no eviction, no real page name.

| Pages | What | 1 thread | 2 | 4 |
| --- | --- | --- | --- | --- |
| one, a root | RocksDB's cache alone | 40 | 782 | 682 |
| | `sm1::pin_page` today | 83 | 1,181 | 1,163 |
| | prototype | 5 | 34 | 215 |
| 64 | RocksDB's cache alone | 56 | 264 | 399 |
| | `sm1::pin_page` today | 106 | 276 | 480 |
| | prototype | 11 | 29 | 50 |
| 20,000 | RocksDB's cache alone | 129 | 266 | 463 |
| | `sm1::pin_page` today | 363 | 548 | 939 |
| | prototype | 17 | 43 | 74 |

- **The pool's pin is about 3 to 20 times cheaper** than the
  cache's alone: the least on a root at 4 threads, the most on a
  root at 2.
- **SH still contends**: a root's 5 ns is 215 at 4 threads. It
  writes a shared line, which §17 expected.
- **`enter_pages()` and the key stay.** The difference between
  today's pin and the cache's is 40 to 230 ns, and a pool removes
  none of it. It is the larger share of a pin once the cache's
  goes (§4.6).

### 1.2 A write held

1.5 million `page_write`s of 3 KB, one thread, flat out, a new
page each 150, as an ascending load:

| WAL zero-fill | total | p50 | p99.9 | over 100 ms | their time | longest |
| --- | --- | --- | --- | --- | --- | --- |
| on | 58.6 s | 4.9 µs | 33 µs | 68 | 50.5 s | 794 ms |
| off | 29.3 s | 6.7 µs | 88 µs | 66 | 15.0 s | 333 ms |

- **A write is 5 µs, or nearly a second.** The pauses are one a
  memtable switch: the log's zeros, then writes stopped for two
  immutable memtables. A real load does work between writes, so
  it stalls less: 7 to 17 stalls with zero-fill off, 22 to 43 on.
- **Today a reader on a hit does not see this.** Under an EX pin
  across the write, every reader of that page waits it out, and
  in an ascending load that page is the tail everyone reads.
  **This is the pool's largest risk** (§4.1).

### 1.3 A miss's two copies

40,000 pages of 3 KB, each missed once in an empty cache: 7.9 µs
a miss, and the cache grew 248.8 MB for 120 MB of pages, **2.07
times**. `BTREE.md` §15.3's weakness is real.

### 1.4 Eviction

60,000 leaves and 300 inner pages in a small cache, by a
synthetic trace: probes alone, or probes among a scan reading
each leaf twice, as `fill()` does. RocksDB's cache was run; the
clock, leaves aging twice as fast, was simulated in frames of 4
KB, with no row blocks and no block copies, so it flatters the
clock.

| Cache | Trace | LRU inner miss | clock | LRU leaf miss | clock |
| --- | --- | --- | --- | --- | --- |
| 16 MB | probes | 0.00% | 0.00% | 0.8% | 0.8% |
| 16 MB | with a scan | 0.92% | 0.00% | 75.5% | 66.3% |
| 64 MB | probes | 0.00% | 0.00% | 0.8% | 0.8% |
| 64 MB | with a scan | 0.00% | 0.00% | 49.3% | 50.9% |

- **The clock's gain is real but narrow**: only under a scan, in
  a cache of about a tenth of the 180 MB of pages. Eviction alone
  would not justify this project.

### 1.5 What it says

| Buys | Costs |
| --- | --- |
| a pin 3 to 20 times cheaper | every descent pins every page, no held pins |
| half a miss's memory back | readers wait through a write |
| held pins, fill latch, latch table gone | one budget becomes two |
| a clock, in a tight cache | a new page format |

The project stands on its concurrency and its memory, if §4.1 is
solved. It will not close the 1.5× gap, as §17 said.

---

## 2. Decided

From `BTREE.md` §17:

- A pin is a latch, SH or EX, with no third mode, held only as
  long as a latch is.
- To change a page: pin EX, copy out, change the copy, write it
  to RocksDB, copy it in, unpin. A new page is the same, pinned
  with a flag not to read it.
- Nothing that can fail touches a frame. Pages are always clean.
- A split pins left to right and writes one batch.
- A clock, leaves aging twice as fast as inner pages. Only
  dropping a tree purges at once.
- Slots: a header's `max` and `top`, `top <= max`, grown 16 at a
  time; offsets from the end of the slot array; zero marks
  nothing.
- `BTREE.md` §14.5's held pins go.

---

## 3. The design

### 3.1 Where it sits

The pool is sm1's, as sm1 owns a page's I/O and knows nothing of
trees (D2). There is one in the environment, beside the block
cache, shared by every database and the temporary instance.

### 3.2 Frames

- **4 KB each**, an array made at attach, `sm1::page_size`. sm2's
  `page_limit` must not exceed it: a `static_assert`.
- **A descriptor, in its own cache line**: the state word, the
  clock's count, the tag.
- **The state word**: SH count, and an EX bit. One word, as the
  prototype's.
- **The tag**: database, store, and the page's key, at most 16
  bytes, kept in the descriptor. Longer is refused, where today
  `max_keylen` allows it: sm2's are 10.
- **Lookup**: a lock-free read of a chained hash, as prototyped;
  pin, then compare the tag again, as a frame may have been taken
  for another page between. A changed tag unpins and looks again.

### 3.3 The interface

```
pin_page(session, store, key, mode, flags, out, bytes)
  mode    sh or ex
  flags   inner: the clock's age
          no_read: a new page, its frame not read (ex alone)
unpin_page(session, page)
page_write(session, store, puts)   every put's page pinned ex
```

- **`page_write` copies each page into its frame** after RocksDB
  has it. A failed write touches no frame.
- **A pin is held in a view into the frame.** It must not outlive
  the pin, which a cursor already does not (§3.6).
- **`inner` stays a hint**, as sm1 does not read a page's level.
  It is given at the pin alone: a write's `page_put_t` has none,
  as its page is already pinned.

### 3.4 A miss

1. Take a frame (§3.5), its tag set, pinned EX, in the hash.
2. Other readers of the page find it and wait on the state word.
3. `Get` with `fill_cache = false`, as §1.3 calls for.
4. Copy in, and drop to the mode asked for.
5. A failed `Get`, or a page that is not there: take the frame
   out of the hash, and fail. It was never valid.

So the fill latch's lost update (`BTREE.md` §14.4) cannot
happen: a writer of the page waits on the same word.

`no_read` is a miss without step 3.

### 3.5 Eviction

- **The hand** sweeps frames, skipping any pinned, taking a count
  of 0.
- **The count**: 2 for an inner page, 1 for a leaf at a pin, not
  raised by a second pin of a leaf, so a scan's re-read of each
  leaf promotes nothing. A count at its value is not written, so
  a hit on a hot page writes the state word alone.
- **Pinned frames are bounded**: a thread pins at most five in a
  split: a child, its parent, the parent's sibling and two new
  pages. The floor is 64 frames, not set by sessions, as sm1 is
  not told how many there will be; while every frame is pinned a
  miss waits, and can be interrupted.
- **Purge**: `page_drop`, and `rmstore`, `TRUNCATE`, `rmdb` and a
  temporary store's end, walk the frames. Unpurged frames would
  only age out, as a store's id is never reused, so these may be
  lazy but for `page_drop`.

### 3.6 Reads

- **A descent pins one page at a time**: SH, read the child's id,
  unpin, pin the child. B-link needs no coupling, so a reader
  never holds two. The `afresh` and `moved` paths of `descend()`
  go, as there are no held pages. Page 0's level is still
  checked.
- **A cursor** pins SH, copies, unpins. Slots in no order ended
  `v.span()`, so it copies the entries in range one at a time, in
  key order: a point lookup copies one entry, not 4 KB, and
  `cursor_next()` is unchanged.
- **Row reads** are never made under a pin, but for §4.2's two.

### 3.7 Slots

As §2, `top` being a count, so an empty page has one. A page's
bytes in RocksDB are its used length, not its frame: the gap is
not written. They count unused slots, so a page fits its limit
and a frame of that size; `entry_max` gives up a half's 30 bytes
for it (1344 bytes at 4 KB, from 1355). Prefix compression's
format is not settled, and nothing here settles it:
`page_view_t` stays the only reader of the layout.

### 3.8 What goes

- `src/page.cc`'s fill latches and cache entries.
- `sm2/src/tree/latch.cc`, `latch.h`, and `page_latch_t`.
- `held_t`, `held_max`, `tree_close()`'s work.
- `BTREE.md` §14, §15; `sm1.h`'s "a pin has no mode".

---

## 4. What §17 left open

### 4.1 Readers through a write: the largest risk

§1.2's pauses happen under EX. Three steps, each its own measure:

1. **Zero the next log ahead** (`sm1/WALPREALLOC.md` §2). It
   takes the zeros off the write path, which is also what
   explains a tree's load (below). 794 ms goes to about 333 ms,
   on §1.2.
2. **`WriteOptions::no_slowdown` under EX.** A write that would
   stall returns `Incomplete`: unpin, wait with no pin held, and
   start the change again from pin EX. It keeps two modes, and
   §17's protocol already leaves the frame clean. Not tried: it
   does not cover a memtable switch itself, and whether a retried
   change costs less than the stall needs measuring.
3. **A larger `max_write_buffer_number`**, if the stalls remain.

A tree's load is 2.04 times today's entries with zero-fill on,
1.38 times off, on this machine, so step 1 is worth having with
or without a pool. On WSL, 5.6 times is not yet accounted for.

### 4.2 Row reads under EX

- **`tree_insert_unique()`**: `judge` reads rows under the first
  leaf's latch, and a miss is a disk read. Readers of the leaf
  wait, as §17 says. Left so at first: a unique check's entries
  of one prefix are few. Measure it.
- **`tree_prune()`, which §17 does not name**: `dead()` read a
  whole leaf's rows under the latch, up to about 150. It now
  judges with no latch, from a copy, then latches and judges
  again only what is to go. A page's `version` could not do this: an
  insert meeting its key already there changes nothing and moves
  no version, so an entry judged dead, then removed after a row
  wrote that key again, would be lost. Its row is written first,
  so asking again under the latch sees it. A leaf with dead
  entries still reads their rows under EX; a clean leaf takes
  none.

### 4.3 A latch and a read of one page

Four functions latch a page and then read it, in one call each,
`pin_ex()` in `sm2/src/tree/tree.cc`: `latch_covering()`,
`write_split()`, `tree_insert_unique()`'s walk, `prune_leaf()`.
With a pin that is a latch, a second pin would deadlock with the
first, so step 7 makes `pin_ex()` one EX pin. The latch
order stays: child before parent, left before right. A
`descend()` from `post()` holds a child's EX and takes SH above
it; those holders only ever go up or right, so no cycle forms.

### 4.4 A pin's wait

`BTREE.md` §16 holds that a latch's wait is short, so it is
not one `sm_wait()` cuts short. §1.2 ends that: a pin's wait can
be most of a second. **A pin waits by `sm_wait()`**, after a
short spin, and fails `SM_ERR_INTERRUPTED` (`CODE.md` §7). So a
pin may fail where a latch could not; every caller already
handles -1.

### 4.5 Memory

Open, for the user: how `shared_buffers` divides. §1.3 gives back
about half of what pages cost today. The start I would measure is
a quarter to the pool, three to the block cache, as Q9 and Q10
read more row blocks than pages; the alternative is a machine
choice at the home's first attach, `BTREE.md` §11's question 1.

### 4.6 `enter_pages()` on every pin

With no held pins, Q10's 100 probes make about 300 pins, each
checking its transaction and store. **Check once a descent**, and
pin with the store's handle. That needs a store to stay valid for
a descent, which a transaction's own open of it gives: to be
shown, not assumed. Without this a pool's pin saves little of
§1.1, as `enter_pages()` becomes its larger share.

---

## 5. Plan

One issue at a time, each test first, measured, and passing the
full suites before the next (`CLAUDE.md`).

1. **Baseline.** On a machine with `perf` and `pgbench`, at 16
   threads: `bench/btree.sh`, and §1.1 and §1.2 again. → verify:
   Q9, Q10, Q11, Q12 and a load, recorded here.
2. **Zero the next log ahead** (§4.1), which only WSL needs: off
   it no log is zeroed (`sm1/WALPREALLOC.md` §2). → verify, on
   WSL: §1.2's longest under 400 ms; commit latency at 1 and 16
   clients unchanged.
3. **`tree_prune()` judges from a copy** (§4.2), still on the
   block cache. → verify: a new test, an entry whose row returns
   between the two judgements stays, and no first judgement holds
   a latch; `rel`, dg.
4. **One call pins and latches** (§4.3): `pin_ex()`, in sm2, over
   today's cache and latch table, no behaviour changed; sm1's
   `pin_page` takes its mode at step 7. → verify: every tree
   test; TSan.
5. **Slots** (§3.7), still on the block cache: a format change,
   `initdb` again. → verify: sm2's splice tests; Q9 and Q10 no
   worse than 1%, against step 1.
6. **The pool, in sm1** (§3.2 to §3.5), with its tests: a miss
   against a writer, eviction under pins, a failed write leaving
   the frame clean, a purge. → verify: sm1's suite; TSan.
7. **The tree on the pool**, `no_read` and all; the fill latch,
   latch table and held pins go (§3.8). → verify: every suite;
   TSan on all tree tests.
8. **`enter_pages()` once a descent** (§4.6). → verify: Q10 no
   worse than step 1.
9. **`no_slowdown`** (§4.1), if readers' waits still show. →
   verify: a reader's longest wait during a load.
10. **Measure and decide**: step 1's grid, `BTREE.md` §17's
    questions, §4.5's split. The documents follow.

**Layers for tests** (`CLAUDE.md` §5): sm1 and sm2 for all of it;
`rel` and dg for what a statement reaches, a load and a vacuum
above all. `sm1/mvcc` cannot express any of it: a page is outside
every transaction, and its scenarios see only versions.

**If step 1 shows no contention** at 16 clients, and steps 2 and
3 are in, the pool's case is memory alone: stop and ask before
step 4.
