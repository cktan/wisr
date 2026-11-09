# BUFMGR: the Buffer Manager

The Buffer Manager holds the pages of B-tree Indexes in memory,
in a pool of 4 KB frames that sm owns. A pin of a page is also
its latch, so one atomic word answers both "is this page in use"
and "who may read it". Pages in the pool are always clean: a
change goes to RocksDB first, and only then into the frame. The
code is `sm1/src/bufpool.cc` and `sm1/src/bufpool.h`, whose type
is `bufpool_t`, and `sm1/src/page.cc`, which joins it to
sessions, stores and RocksDB. The API is the pages section of
`sm1/include/sm1.h`. Rows do not pass through it: they stay in
RocksDB's block cache.

**Warning.**

- A pin is a latch. Hold it only as long as a latch is held.
  Never hold it across calls of a tree, or across a wait for a
  transaction.
- A thread may pin a page shared again. No other second pin of a
  page is allowed: it deadlocks with the first.
- Writers keep to one pin order (§1.3). The pool cannot detect a
  break of it.
- The bytes of a pin are valid only until the unpin. Nothing
  changes a frame's bytes but `sm1::page_write`.

## 1. Design

### 1.1 A worked example

Table `t` has store 1001 and a B-tree Index. `INSERT INTO t
VALUES (42)` adds an entry to a leaf. Page 0 is the root.
`key(n)` is a page's 10-byte key: the index's relid, then its
page id.

```
find the leaf: a descent, one page at a time
  sm1::page_enter(s, 1001, store)           the checks, once
  sm1::page_copy(s, 1001, key(0), bytes)    page 0 with no pin
  sm1::pin_page(s, 1001, store, key(7), sh, page_inner, pin, bytes)
  ...read the child's id, then...
  sm1::unpin_page(s, pin)

change the leaf
  sm1::pin_page(s, 1001, key(31), ex, 0, pin, bytes)
  copy the bytes out, and change the copy
  sm1::page_write(s, 1001, {{pin, key(31), copy}})
    one RocksDB write, then the copy goes into the frame
  sm1::unpin_page(s, pin)
```

If the write fails, the frame still equals RocksDB's page. The
unpin needs no special case. Trees are
[`BTREEINDEX.md`](BTREEINDEX.md)'s; this
document says what they get from the pool.

### 1.2 Where it sits

The pool is sm1's, as sm1 owns a page's I/O and knows nothing of
trees. There is one in the environment (`env_t::bufpool`), beside
the block cache, shared by every database and the temporary
instance. `bufpool_t` itself knows tags, bytes and pins, and
nothing of sessions, stores or RocksDB: a miss reads through the
caller's loader. So it is tested alone.

| | PostgreSQL | Buffer Manager |
| --- | --- | --- |
| frame | fixed 8 KB, an array | fixed 4 KB, an array |
| page table | shared hash, 128 partitions | a chained hash, read with no lock |
| pin | an atomic in the buffer's descriptor | an atomic in the frame's descriptor |
| content lock | LWLock, SH or EX | the pin itself |
| change | in place, in the frame | on a copy, then copied in |
| dirty pages | yes | **never** |
| write-back | bgwriter, checkpointer | none of ours: RocksDB's memtable and flush |
| WAL | a record for the change | the whole page, in a write batch |
| recovery | redo into pages | RocksDB's WAL replay, nothing of ours |
| replacement | clock sweep | clock sweep, leaves aging twice as fast |
| scan protection | ring buffers | the leaves' faster aging |

Row blocks stay in RocksDB's block cache, in its LRU with its
defaults. Index and filter blocks are not in that cache
(`cache_index_and_filter_blocks` is off): they stay with the
table readers.

### 1.3 Pins are latches

A pin has two modes, SH and EX, and no third. A reader pins SH
and a writer pins EX. So a pin is held only as long as a latch
is, and no pin outlives a call of a tree. The bytes of a pin are
a view into the frame, and they do not change under it.

**Pin order.** No two writers wait on each other, as they keep to
these rules:

1. Child before parent.
2. Left before right, on a level.

A split's pin of its right sibling, for `prev`, is rule 2. A move
right is coupled: the right page is pinned before the left goes.

A reader never holds two pages. It pins SH, reads the child's id,
unpins, and pins the child. B-link needs no coupling, and a
reader holding a page while it waits for a child could wait on a
writer waiting for the page it holds. A descent from `post()`
holds a child's EX and takes SH above it. Those holders only ever
go up or right, so no cycle forms.

**A pin's wait.** A pin spins briefly, then waits by `sm_wait()`,
and fails with `SM_ERR_INTERRUPTED`
([`CODE.md`](CODE.md#7-threads-waits-and-logging) §7). A pin's
wait
can last most of a second through a stalled write (§1.11), so an
interrupt must be able to end it. A pin may therefore fail; every
caller already handles -1.

**What a pin never covers.** A pin is held for a page's change
alone, never across a wait for a transaction: a unique check lets
every pin go first. Row reads are not made under a pin, with two
exceptions, both under the leaf's EX:

- `tree_insert_unique()` reads rows to judge entries of the
  prefix on the leaf, and a miss is a disk read. Readers of that
  leaf wait. A unique check's entries of one prefix are few.
- `reap_leaf()` judges again only what is to go. The first
  judgement of a leaf runs with no pin, from a copy. So a clean
  leaf takes no row read under EX
  ([`BTREEINDEX.md`](BTREEINDEX.md)).

**The bound on pins.** A thread pins at most five pages. In a
split posting to a parent: the child, its parent, the parent's
sibling and a new page; in a unique insert, also the leaf it
began on. A root's split swaps the sibling for a second new page.

**One pin to change a page.** `pin_ex()` in
`sm2/src/tree/tree.cc` pins a page EX and views its bytes in one
call. A function that changes a page reads it through that pin,
as a second pin would deadlock with the first. Its callers are
`latch_covering()`, `write_split()`, `tree_insert_unique()`'s
walk and `reap_leaf()`.

### 1.4 The write protocol

To change page P:

1. Pin P in EX.
2. Copy P out, and change the copy.
3. Write the copy to RocksDB.
4. Copy it into the frame.
5. Unpin P.

To make a new page P:

1. Pin P in EX, with `page_no_read`.
2. Make P, in a copy.
3. Write it to RocksDB.
4. Copy it into the frame.
5. Unpin P.

**A frame is never half changed.** Everything that can throw or
fail is done on the copy. The frame changes only by a `memcpy`
that cannot fail. An exception or a failed write unpins a frame
that still equals RocksDB's.

**Every page is clean.** So nothing is ever flushed, there is no
write-back, and any unpinned page can leave at any moment. The
next reader's miss is a `Get`.

**A split** pins its pages in EX left to right, writes them in
one batch, then copies each into its frame. Several pages commit
atomically, each under its EX pin. An erase in the batch removes
the page's frame.

**A page is outside every transaction.** `sm1::page_write`
commits when it is made, whatever the caller's transaction then
does. It goes to RocksDB's WAL unsynced: a later commit's sync
covers it, the WAL being one sequence. The temporary instance
writes no WAL.

### 1.5 A miss

1. Take a frame by the clock (§1.6), with its tag set, pinned EX,
   in the hash.
2. Other pins of the page find it, and wait on its state word.
3. `Get` with `fill_cache = false`: the frame is the copy, so the
   block cache keeps none.
4. Copy the page in, and drop to the mode asked for.
5. On a failed `Get`, or a page that is not there, take the frame
   out of the hash and fail. It was never valid.

A write of the page waits on the same word, so no write passes a
miss. A stale read cannot be put back over a newer page.

With `page_no_read`, a miss is the same without step 3, and the
bytes are empty. Unless the page is written before the unpin, the
frame goes back unused.

### 1.6 The clock

**The hand** sweeps frames. It skips any pinned, takes an age of
0, and shifts any other right: by one for an inner page, by two
for a leaf. A scan of the leaves would otherwise page the inner
pages out.

**The age.** A use ORs in the top bit, 128, so after any use an
age is 128 to 255. The hand empties it in eight sweeps of an
inner page and four of a leaf, whatever its lower bits: they
shift out before the top bit does. So a use restarts a page's
life and promotes it no further, and a scan's re-read of a leaf
promotes nothing. A bit already set is not written, nor an
unchanged inner flag. So a hit on a hot page writes the state
word alone.

**`inner` is a hint.** sm1 does not read a page's level. The
caller gives it at the pin, and a frame keeps it from its last
pin. A write's `page_put_t` has none, as its page is already
pinned.

**The hand turns at most nine times** before it sleeps: eight
sweeps age an inner page to 0, and the ninth takes it. While
every frame is pinned a miss waits, and an interrupt can end that
wait.

**The floor.** Pins must find frames. The pool has at least 64
frames, and six a session if attach is told `max_sessions`, as
digres's `max_connections` tells it. A session waiting for a
frame holds at most five, so six a session leaves a frame over.

**Purge.** Only `sm1::page_drop` purges at once, as a page key
may be made again by a later tree under one prefix. A dropped
store's or database's frames age out: `storeid` and `dbid` are
never reused.

### 1.7 Page copies

Every descent reads page 0, so its pin is a word every probe on
every core writes. A **page copy** removes that write: a session
reads page 0 from an immutable copy, with no pin, while the page
is as it was. [`BTREEINDEX.md`](BTREEINDEX.md) says why a stale
copy is safe for a
descent. The pool gives three things.

- **A version** in each descriptor, a count each change of the
  frame's tag or bytes moves, under an EX pin: an install, a
  discard, a purge, an eviction. Read with no pin, it says
  whether the frame still holds the page as it was at the copy.
  Frames last the pool's life, so it may be read at any time.
- **A shared copy**, `bufpool_copy_t`: the page's bytes and the
  version, held by `shared_ptr`. The first reader at a version
  makes it, and the others take it (`share()`). The frame lets
  its copy go when the page changes or leaves. A holder keeps its
  own until its next refresh or its close, and the last holder
  frees it. So memory is about one copy a root, and one stale
  copy for each session idle since the root changed.
- **A touch** ages a frame as a pin does, with no pin. A good
  copy's check does it. Without it page 0, pinned by no one, ages
  out of the pool while it is read most, and every session's copy
  goes with it.

A copy is as fresh as a page pinned, read and unpinned. Its
reader does not wait through a write of the page. A session keeps
at most 32 copies, by store and key, and drops them all for the
33rd. They live in sm1's session, not in the tree, as digres
opens a table and its trees once a transaction.

### 1.8 Memory

One budget, `block_cache_bytes`, serves two users. digres sets it
to `shared_buffers` (`NBuffers * BLCKSZ`). attach gives a quarter
to the pool and the rest to the block cache, as the B-tree Index
benchmark's probes, the old bench's Q9 and Q10
(`../bench.old/btree.sh`), read more row blocks than pages.
The pool keeps its floor (§1.6), which can take it past the
quarter.

The pool's size is exact. A read waits for a frame rather than
outrun the budget. The block cache, which is not strict, no
longer holds the pages.

### 1.9 What a page must be

- **At most 4 KB.** `bufpool_frame_bytes`, `sm1::page_size`.
  sm2's `page_limit` must not exceed it: a `static_assert` in
  `sm2/src/tree/tree.h`. Its bytes in RocksDB are its used
  length, not its frame.
- **A key of at most 16 bytes**, `sm1::page_key_max`, as the tag
  holds it in the descriptor. A longer one is refused. sm2's are
  10.
- **Its layout is the caller's.** sm1 reads no page. A page's
  header and slots are [`BTREEINDEX.md`](BTREEINDEX.md)'s. They
  were chosen for
  the pool: `max` and `top` keep slots with room to grow, 16 at a
  time, and offsets count from the end of the slot array, so
  growing it moves no slot's value.

### 1.10 Reads of a tree

- **A descent** pins one page at a time, SH. It checks the store
  once (§2.9). It still checks page 0's level, as a split can
  grow the tree past a page read.
- **A cursor** pins SH, copies, and unpins. It copies the entries
  in range one at a time, in key order, and reads them from the
  copy. So it holds no page between calls, and a point look-up
  copies one entry, not 4 KB.

### 1.11 What the design rests on

These measures used an optimized build, RocksDB 8.9.1, and ext4
on a network disk. The machine was an EPYC 7B12 VM of 2 cores and
4 threads. A throwaway program drove `sm1` and a 60-line
prototype. RocksDB's cache is what pages used before the pool.

**A pin**, ns for a pin and its unpin on a hit. The prototype is
a CAS on one word a frame, found by a lock-free read of a chained
hash. It is a floor: no eviction, no real page name.

| Pages | What | 1 thread | 2 | 4 |
| --- | --- | --- | --- | --- |
| one, a root | RocksDB's cache | 40 | 782 | 682 |
| | prototype | 5 | 34 | 215 |
| 64 | RocksDB's cache | 56 | 264 | 399 |
| | prototype | 11 | 29 | 50 |
| 20,000 | RocksDB's cache | 129 | 266 | 463 |
| | prototype | 17 | 43 | 74 |

- The pool's pin is about 3 to 20 times cheaper than the cache's.
- SH still contends: a root's 5 ns is 215 at 4 threads, as it
  writes a shared line. So §1.7's copies.
- The transaction and store checks cost 40 to 230 ns more a pin,
  which a pool does not remove. So `sm1::page_enter`.

**A write held.** 1.5 million `page_write`s of 3 KB, one thread,
a new page each 150, as an ascending load:

| WAL zero-fill | total | p50 | p99.9 | over 100 ms | their time | longest |
| --- | --- | --- | --- | --- | --- | --- |
| on | 58.6 s | 4.9 µs | 33 µs | 68 | 50.5 s | 794 ms |
| off | 29.3 s | 6.7 µs | 88 µs | 66 | 15.0 s | 333 ms |

A write is 5 µs, or nearly a second. The pauses come one a
memtable switch: the log's zeros, then writes stopped for two
immutable memtables. A real load does work between writes, so it
stalls less: 7 to 17 stalls with zero-fill off, 22 to 43 on.
Under an EX pin across the write, every reader of that page waits
it out, and in an ascending load that page is the tail everyone
reads. **This is the pool's largest risk** (§6).

**A miss's two copies.** With the block cache, 40,000 pages of 3
KB were each missed once. A miss cost 7.9 µs, and the cache grew
248.8 MB for 120 MB of pages: 2.07 times.

**Eviction.** 60,000 leaves and 300 inner pages, by a synthetic
trace. RocksDB's cache was run. The clock was simulated in frames
of 4 KB with no row blocks, which flatters it. With probes alone
both miss 0.00% of inner pages and 0.8% of leaves. Among a scan
reading each leaf twice:

| Cache | LRU inner miss | clock | LRU leaf miss | clock |
| --- | --- | --- | --- | --- |
| 16 MB | 0.92% | 0.00% | 75.5% | 66.3% |
| 64 MB | 0.00% | 0.00% | 49.3% | 50.9% |

The clock's gain is real but narrow: only under a scan, in a
cache of about a tenth of the 180 MB of pages. Eviction alone
would not justify the pool.

| The pool buys | The pool costs |
| --- | --- |
| a pin 3 to 20 times cheaper | every descent pins every page, with no held pins |
| half a miss's memory back | readers wait through a write |
| held pins, a fill latch and a latch table gone | one budget becomes two |
| a clock, in a tight cache | a new page format |

It stands on its concurrency and its memory. It does not close
the 1.5× gap to PostgreSQL's indexes.

## 2. Implementation

### 2.1 Frames

`bufpool_t` makes three arrays at attach: descriptors
(`frame_t`), bytes (`m_mem`, 4 KB a frame), and copies
(`m_copies`). A frame's bytes and copy are found by its index, so
the descriptor fits one cache line: a `static_assert` holds it to
64 bytes.

| Field | What | Guard |
| --- | --- | --- |
| `state` | the pin (§2.2) | atomic |
| `next` | the next frame in its chain | written under its bucket's lock |
| `age`, `inner` | the clock's | relaxed; a hint |
| `valid`, `size` | the bytes hold the page, and its length | the pin |
| `dbid`, `storeid`, `k0`, `k1`, `len` | the tag; `len` is `no_page` while the frame holds none | relaxed atomics, written under EX |
| `copied` | its slot in `m_copies` holds a copy | a pin |
| `version` | §1.7 | moved under EX |

A tag lies in relaxed atomics, so a look-up reads it with no
lock. Only a pin makes it sure, as a tag changes only under EX.

### 2.2 The state word

`state` is 32 bits: bit 31 is EX, bit 30 is the wait bit, a
thread sleeps here, and the low bits count SH pins. `try_pin` is
one CAS loop. A pin yet to wait yields to the wait bit, so
readers do not starve a writer. One that has slept takes its turn
past it.

### 2.3 The look-up

Buckets are chains of frame indexes, in a power-of-two array at
least twice the frames. A bucket's hash is Murmur3's finalizer
over the tag's words. A chain changes under one of 256 bucket
locks and is read with none.

1. `find` walks the chain, comparing tags, for at most as many
   steps as there are frames.
2. It may miss. A reader on a frame that moves walks on into
   another chain. A frame that leaves keeps its `next`, for that
   reader.
3. It may find a frame that changes before it is pinned. So a pin
   compares the tag again, and looks again if it changed.
4. A miss looks again under the bucket's lock before it gives the
   page a frame. That look is the sure one: a page has one frame.

### 2.4 A pin

`bufpool_t::pin`:

1. Refuse a key over `bufpool_key_max` with `SM_ERR_INVALID`.
2. Look up the tag. On a hit, `acquire` the frame. If it holds
   another page by now, unpin and go to step 2.
3. On a miss, `take` a frame: `victim`, then the look under the
   bucket's lock. If the page has a frame by now, let the emptied
   frame go and go to step 2.
4. On a miss without `bufpool_no_read`, call the loader, with EX
   held. On failure, `discard` the frame and return the loader's
   result. Else set `size` and `valid`, and drop to SH if SH was
   asked, waking any sleepers.
5. `touch` the frame, and set `inner` if it differs.

`acquire` waits for a frame:

1. Try 32 times on the fast path, which yields to the wait bit.
2. Take the wait queue's mutex, held from here to the wait. 64
   queues serve the frames, by index.
3. Set the wait bit, so a release after it wakes this thread.
4. Try the pin, past the bit. On success, clear the bit if the
   queue's `sleepers` is 0. Else it outlasts the sleep it stood
   for, and sends the page's next pins to the slow path.
5. If the frame holds another page by now, return 1. Never sleep
   on another page's frame: its holder may wait for a page this
   thread holds, out of the pin order.
6. Sleep by `sm_wait()`, counted in `sleepers`, then go to step
   3.

A sleeper on another frame of the queue keeps the bit too: a
cost, never a lost wake-up.

`release` drops a pin. If the state is then the wait bit alone,
it calls `wake`:

1. Clear the wait bit.
2. Take the queue's mutex and let it go at once. A sleeper that
   set the bit is then in its wait. Without this step the notify
   can come between a sleeper's failed try and its wait, and be
   lost.
3. Notify every sleeper, with the mutex let go, so none wakes
   only to wait for it.

Each woken sleeper sets the bit again before it tries. The last
of them clears it.

### 2.5 The victim

`victim` takes a frame by the clock:

1. Move the hand, a shared atomic, to the next frame.
2. Skip the frame if it is pinned.
3. If it has an age, shift the age right, by one if inner and by
   two if a leaf, and go on.
4. Pin a frame of age 0 EX by CAS. If another thread pinned it
   first, go on.
5. Empty it: unlink its page from its chain, clear its tag, and
   let its shared copy go. Return it.
6. After nine turns of the hand, return `SM_ERR_INTERRUPTED` if a
   bit in the wait mask is pending. Else sleep 200 µs and go to
   step 1.

A pin is short and pins are bounded, so step 6 is rare. It sleeps
by `usleep`, to give the holders the processor.

### 2.6 Install, discard, unpin

- **`install`** copies the bytes in, sets `size` and `valid`,
  moves `version`, and lets the shared copy go. It cannot fail.
- **`discard`** takes a frame pinned EX out: unlink, clear the
  tag, let the copy go, set `valid` false and the age to 0. The
  caller still releases the pin.
- **`unpin`** reads the state word, as a holder of the EX bit is
  its only pin. An EX frame that is tagged but not valid is a new
  page never installed, so it is discarded first.

### 2.7 Purge

`purge(dbid, storeid, prefix)` walks every frame.

1. Compare the frame's tag with no pin, and skip it if it is not
   the prefix's.
2. Pin it EX, past the wait bit. This wait must not fail, or a
   dropped page would stay: it tries, and sleeps 200 µs between
   tries.
3. Compare the tag again, and discard the frame if it is still
   the prefix's.
4. Release the pin.

The page must be gone from what a loader reads first, so a pin
during the purge misses and finds no page. The caller holds no
pin of the prefix.

### 2.8 Version, share, touch

- **`version`** moves in `set_tag` and in `install`, each under
  EX. Eviction moves it, as `set_tag` clears the tag.
- **`share(frame, bytes)`** runs under the caller's pin. If the
  slot holds a copy at this version, it returns that. Else it
  makes one and publishes it by compare and swap. If another SH
  reader published first, it takes that one, or keeps its own if
  the slot moved on.
- **`drop_copy`** runs under EX, so no `share` of the frame runs
  meanwhile. `copied` spares a frame never copied the atomic
  store.
- **`touch`** sets the age's top bit if it is not set. Unpinned,
  it is a hint: the hand may age the frame over it, or the frame
  may hold another page by now.

### 2.9 Pages: `sm1/src/page.cc`

A page is a key of class `key_class_page` (0x04) in a store: no
scan, read or write of versions meets it. `sm1::page_t` is an
opaque `frame_t`.

**What a call checks first** (`enter_pages`):

1. An active transaction, not prepared or doomed, or a session
   set by `sm1::set_no_transaction`, a btree reaper's
   ([`BTREEINDEX.md`](BTREEINDEX.md)).
2. A store other than 0.
3. A store live at the view (`catalog_lookup`): a hash look-up in
   the session's cache, and a load of the database's
   `last_ddl_ts`.
4. The store's handle (`session_store`).

A transaction that has made or dropped a store itself reads the
catalog at step 3, as it must see its own uncommitted DDL. Before
the pool, that measured 590 ns a pin where the common case took
65 ns. `CREATE TABLE` and its load in one transaction pays it.

**`sm1::page_enter`** makes these checks once for many pins. The
handle it gives is good until the session's next call that is no
pin of a page. A descent makes no other call, so it checks once.
`pin_page` has a form that takes this handle, and one that takes
the storeid and checks.

**`load_page`** is the pool's loader: a `Get` with `fill_cache =
false`, 1 for no page, and `SM_ERR_CORRUPT` for a value over
`page_size`.

**`sm1::page_write`**:

1. Check the store, and refuse a page over `page_size`.
2. Make one `WriteBatch`: `Put` for each page, `Delete` for each
   erase.
3. Write it, with `disableWAL` for the temporary instance. On
   failure return: no frame changed.
4. For each page, `install`, or `discard` for an erase.

Each put's page is pinned EX by the caller. Its bytes may not lie
in a frame being written.

**`sm1::page_drop`** removes a prefix's pages with one
`DeleteRange`, then purges their frames. It is how a tree goes. A
rollback must keep a dropped tree's pages, so `DROP INDEX` calls
it at commit, from an `sm1::on_commit` function. So this one call
may run with no transaction active: the store is taken as the
database has it, and a store gone is no error.

**`sm1::keep_page_copy` and `sm1::page_copy`** hold a session's
copies. `page_copy` compares the copy's version with the frame's,
with no pin, and touches the frame if they agree.

`sm1::page_approxsize` gives RocksDB's estimate of a prefix's
bytes, on disk and in memtables. `sm1::page_cached` and
`sm1::pages_exclusive` are for tests.

## 3. Cases

| Case | What happens |
| --- | --- |
| a miss against a writer of the page | the miss holds EX through its `Get`, so the writer waits. The lost update a block cache had cannot happen: a reader cannot put old bytes over new |
| two misses of one page | the look under the bucket's lock gives one a frame; the other lets its emptied frame go and pins the first's |
| a failed `Get`, or no such page | the frame is discarded; its waiters find the tag changed, and look again |
| a failed `page_write` | every frame is as it was; the caller unpins |
| a new page whose write failed | `unpin` discards the frame |
| an exception on the copy | the frame is untouched; the unpin is plain |
| a reader through a stalled write | it waits, by `sm_wait()`, and an interrupt ends the wait. A reader of a page copy does not wait |
| every frame pinned | a miss sleeps and sweeps again; an interrupt ends it. The floor means a pin ends first |
| a pin on a frame being evicted | the tag has changed under the pin: it unpins and looks again |
| `DROP INDEX`, `tree_create` again | `sm1::page_drop`; a session's copy finds the version moved |
| `TRUNCATE`, `DROP TABLE`, `rmdb` | the old store's frames age out; a store dropped fails `sm1::page_enter` first |
| abort of the writing transaction | the page stays: it is outside every transaction. [`BTREEINDEX.md`](BTREEINDEX.md) says why that is safe |
| crash, restart | the pool and the copies start empty; RocksDB replays its WAL |
| a temporary store | its pages share the pool, with no WAL |
| page 0 evicted under a copy | a good copy's check ages it, so it is not while read; if it is, the copy fails its check and the next descent misses |
| another database | a tag has the `dbid`; a session's copy is of one database |

## 4. Rejected

| Alternative | Why not |
| --- | --- |
| RocksDB's block cache as the page cache | a pin was a handle's count under its shard's mutex, about 470 instructions a look-up, and a shared write on the hottest pages. A miss cached a page twice (§1.11) |
| Immutable entries, a write replacing the entry; a pin with no mode | readers took no latch, but it needed a fill latch, a latch table and held pins, and each write made a new entry |
| A fill latch, 256 stripes, held across a miss's `Get` and a write | the cache had no compare-and-insert. A miss now loads under its own pin |
| A table of writers' latches by page id | a pin is the latch now. A stripe would have deadlocked a split holding two pages |
| Held pins: a tree keeps inner pages pinned to the end of its transaction | they saved a costly look-up. A pin of one atomic needs no such thing, and a pin that is a latch cannot be held |
| LRU with a high-priority pool, a hit promoting | a scan's re-read of a leaf is a hit, so each leaf of a long scan is promoted and may push inner pages out |
| A third pin mode | nothing needs it: SH reads, EX writes |
| Dirty pages and write-back | RocksDB is the write-back. Clean pages leave a failed write with nothing to undo, and any page free to go |
| A change made in the frame | what can fail would then leave a frame half changed |
| Coupled pins in a descent | B-link needs none, and a reader holding a page can wait on a writer waiting for it |
| A page's `version` to prove nothing changed between `tree_prune()`'s two judgements | an insert meeting its key already there moves no version, so an entry judged dead, then removed after a row wrote that key again, would be lost. It judges again under the pin |

## 5. Tests

**`sm1/test/test_bufpool.cc`**, the pool alone: a miss reads
once; two stores and two databases stay apart; shared and
exclusive pins; no wait bit left after a sleep; a page's pins
wait out its miss; eviction leaves pinned pages; an inner page
outlasts a scan of leaves; the hand's shifts; a pin waits while
every frame is pinned; what fails leaves a frame as it was;
discard and purge; `version`, `touch`, `share`; interrupts;
writers and readers against a store.

**`sm1/test/test_page.cc`**, through the API: written outside the
transaction; a session with no transaction; a pin shuts out a
writer; a miss never puts back a page replaced; `sm1::page_drop`,
also after a commit; page copies; no version meets a page; an
inner page outlasts a leaf; a store's checks made once; a miss
leaves no second copy in the block cache.

**`sm1/test/test_env.cc`**: the pool's size at attach.

**sm2:** `sm2/test/test_tree.cc` and `sm2/test/test_page.cc`, for
what trees do with pins; `make tsan` in `sm2` runs them under
TSan.

**`rel` and dg** cover what a statement reaches, a load and a
vacuum above all ([`BTREEINDEX.md`](BTREEINDEX.md)).

**`sm1/mvcc` cannot express any of this.** A page is outside
every transaction, and its scenarios see only versions.

## 6. Open

- **The pool is built, not measured.** The baseline is still to
  run: on a machine with `perf` and `pgbench`, at 16 threads, the
  old B-tree Index benchmark's queries Q9 to Q12 and a load
  (`../bench.old/btree.sh`), with §1.11's pin and write measures
  again. If it shows no contention at 16 clients, the pool's case
  is memory alone.
- **Readers through a write**, §1.11's largest risk, has three
  steps. Zeroing the next log ahead is built, on WSL alone
  ([`DBMGR.md`](DBMGR.md#84-zero-filled-logs) §8.4): 794 ms goes
  to about 333 ms.
  `WriteOptions::no_slowdown` under EX is not built: a write that
  would stall returns `Incomplete`, and the caller unpins, waits
  with no pin held, and starts again. It keeps two modes, and the
  protocol already leaves the frame clean, but it does not cover
  a memtable switch itself. Whether a retried change costs less
  than the stall is not measured. A larger
  `max_write_buffer_number` is the last step, if stalls remain.
- **The memory split.** A quarter to the pool is a start, not a
  measure (§1.8). The alternative is a machine choice at the
  home's first attach.
- **Row reads under EX** in a unique insert are to be measured
  (§1.3).
- **A tree's load** costs 2.04 times a load of LSM Index entries
  with zero-fill on, and 1.38 times off, on the VM of §1.11. On
  WSL, 5.6 times is not yet accounted for.
- **Copies of the inner pages below page 0** are not done.
