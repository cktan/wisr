# PIN — reading a row without copying it

A row read from sm is copied once or twice before digres's
executor sees it, where PostgreSQL copies it none: its slots
hold a pin on the buffer the row is in. The objective here is
to copy a row as few times as possible on its way from RocksDB
to the executor. This file is a digest of the discussion that
counted the copies, weighed a pin, and settled who owns one
and when it is released; §6 is the changes it suggests to sm
and to digres.

Paths are relative to `sm/`; digres's are under `../dg/`.

---

## 0. In brief

- **Every read by rowno copies the row twice**: an index
  scan's, a bitmap heap scan's, a TID scan's, and the fetch of
  an UPDATE's or a DELETE's target. PostgreSQL copies it none.
- **A pin removes both.** sm keeps the RocksDB iterator that
  found the row, and the row's bytes stay where they are
  until the pin is released.
- **The session owns every pin**, in a pool. A read hands the
  caller a pin ref, a plain value; the caller never holds sm's
  memory, and sm never touches the caller's.
- **A pin is released** by the next read into the same ref,
  by an unpin, by a release of all the session's pins, or at
  the end of the transaction. Never at a rollback to a
  savepoint, nor when its store is dropped.
- **A generation on each pin** makes a stale ref harmless, so
  digres needs no ResourceOwner and no memory context
  callback: a ref it loses on an error is left behind, and
  the pin goes at the end of the transaction.

---

## 1. Terms

| Term | Meaning | Owner |
|---|---|---|
| **row** | a tuple's bytes, as RocksDB holds them | RocksDB |
| **pin** | sm's object that keeps one row's bytes in memory: the iterator that found it, and a view of the row | the sm session, in a pool |
| **pin ref** | what a read hands the caller: which pin, and the pin's generation | the caller, a plain value |
| **generation** | a counter on each pin, increased each time it is released | the pin |

An index's entries are **entries**, not rows. A **slot** is
digres's, and appears only in §6.4.

---

## 2. Where a row is copied today

Counted from the bytes in RocksDB's block cache or memtable.
Reading a block into the cache on a miss is RocksDB's, as
reading a page into shared buffers is PostgreSQL's, and is not
counted. Nor are the copies a Sort, a Hash, an Agg or a
Material makes, which PostgreSQL makes too. Copying a `Datum`
pointer is not a copy of the row.

The copies:

- **A** — `scan_next` copies the visible version into the
  cursor, `c->data.assign` (`sm1/src/access.cc:741`): the
  whole row, though only its first `natts` columns are
  deformed.
- **A′** — the same step copies the userkey twice,
  `uk.ToString()` and `c->last_examined = uk`
  (`sm1/src/access.cc:701-702`): ten bytes for a row, the
  whole key for an entry.
- **B** — `access_read` copies the row into the session's
  `readbuf` (`sm1/src/access.cc:829`).
- **C** — `read_row()` materializes the slot, copying its
  by-reference columns, because B's bytes last only until
  sm's next call (`../dg/src/backend/access/sm/smstore.c`).
- **D** — `slot_from_sm()` materializes the slot, only on a
  table with a `decimal128` column.

Copying nothing: sm2's `tup_deform`, whose by-reference
values point into the row (`sm2/TUPLE.md` §8), and the C
wrapper's `view_out`.

| Scan | Entry | Row | Where | PostgreSQL 18 |
|---|---|---|---|---|
| Sequential | — | **1** (2 with a decimal) | A (+D) | 0 |
| Sample | — | **1**, of every row passed over too | A, by `smstore_scan_row()` | 0 |
| Index | 1, its key 2 | **2** | entry A, A′; row B, C | 0 |
| Bitmap heap | 1, its key 2, in the bitmap index scan | **2** | `smstore_fetch()`: B, C | 0 |
| TID | — | **2** | `smstore_fetch_row()`: B, C | 0 |
| Index-only, key decodable | value 1, key columns **4** | — | A′ twice; `decode_key_front` into a `std::string`, unescaping; `key_datum` into `make_varlena` (`sm2/src/rel/index.cc:443`); INCLUDE columns 1 (A); +D | 1 (btree's `currTuples`) |
| Index-only, key cut | 1, its key 2 | **2** | `entry_from_row()`: B, C | 0 |
| UPDATE, DELETE target | — | **2** | `smstore_fetch_row()`: B, C | 0 |
| DELETE's old row, for RETURNING | — | **3** | B; `tbl_delete` copies the old image (`sm3/src/table/table.cc:804`); `keep_row()` | 0 |
| Recheck under read committed | — | **2** | `read_latest` into `readbuf`; `keep_row()` | 0 |

---

## 3. A pin

- **Not `Get`.** A version's key carries `~wts`, so a point
  read is a `Seek` and a walk over the key's versions
  (`sm1/SM1_DESIGN.md` §10.1), not an exact lookup;
  RocksDB's `PinnableSlice` does not apply.
- **What it holds**: the iterator that found the visible
  version, the view of its value, and the store it read. The
  iterator does not move after it finds the version, so the
  view stays valid without `ReadOptions::pin_data`: a
  block's bytes through the block the iterator holds, a
  memtable's through the SuperVersion it refs.
- **What it costs**: nothing more than today. sm1 creates an
  iterator for every point read now and destroys it on
  return; a pin destroys it on release instead.
- **The read epoch is not held.** `db_read_leave` still runs
  before the read returns: the epoch guards resolving the
  versions' status, not the bytes. A pin left behind holds
  memory, never the CF:txn sweep.
- **What it holds back**: a pin's iterator holds its
  SuperVersion, so memtables and SST files that would be
  freed stay until the pin is released. PostgreSQL's pins are
  bounded by its buffer pool; these by how many rows a caller
  keeps pinned at once.
- **A pin covers the row only.** A value TOAST holds out of
  line is still fetched and assembled into memory of its own
  (`smstore_fetch_toast()`).

---

## 4. sm's contract

### 4.1 The calls

- `sm1::read(session, storeid, key, ref, data)` reads the
  row, takes a free pin from the session's pool for it, and
  sets `ref` to that pin and its generation; `data` views
  the row. If `ref` named a pin still held, that pin is
  released first, so a caller that reads into one ref holds
  one pin at most. `sm1::read_latest` is the same.
- `sm1::unpin(ref)` releases the pin if `ref`'s generation is
  the pin's; otherwise it does nothing.
- The old row an update or an erase hands back is read into
  a ref the same way.
- `sm1::release_all_pins(session)` releases every pin the
  session holds, as the end of the transaction does, and
  leaves the transaction open. It is for a caller that knows,
  at some point short of the transaction's end, that none of
  the rows it pinned is still wanted: digres at the end of a
  statement (§6.4). Every ref the caller still holds is then
  stale.
- A ref of zeros names no pin, and is what a caller starts
  with.

`data` is valid until the pin is released, which happens at
the first of:

1. the next read into the same ref;
2. `sm1::unpin(ref)`;
3. `sm1::release_all_pins(session)`;
4. the end of the session's transaction, commit, abort or
   prepare, or the session's close.

### 4.2 Releasing a pin

Releasing destroys the pin's iterator, increases its
generation, and returns it to the pool. Every ref to it is
then stale: an unpin through one does nothing, and a read
into one takes a fresh pin. A ref left in the caller's
memory after the transaction ends is stale the same way.

**Why each pin has a generation.** A released pin goes back
to the pool at once, so the next read, in the same transaction,
may take it for another row. A ref the caller still holds to
it must then be told apart from the ref the pin now belongs
to. Two refs, `r1` and `r2`, show what goes wrong without a
generation:

1. A read into `r1` takes pin P for row X.
2. `sm1::unpin(r1)` releases P; P goes back to the pool.
3. A read into `r2` takes P again, now for row Y.
4. The caller unpins `r1` a second time: a slot cleared twice,
   or one cleared after the transaction that filled it.

With only a flag on P saying "held" or "free", step 4 finds
P held, and releases it: `r2`'s row Y is gone while its caller
still reads it. With a generation, P's is 1 at step 1 and
`r1` records 1; step 2 makes it 2, and step 3 gives `r2` the
2. At step 4, `r1`'s 1 is not P's 2, so the unpin does
nothing, and Y stays pinned.

**Why not one generation per transaction.** The transaction's
number would tell a ref from an earlier transaction from a
current one, but not two refs of the same transaction. It
would work only if a pin, once taken, stayed with its ref
until the transaction ends, and was returned to the pool only
then. That fails a caller that makes a new ref for each read
and unpins it after: index maintenance reads the old row of
each row it changes, and a foreign key check reads each
referenced row, each with a ref of its own. An UPDATE of a
million rows would then hold a million pins at its commit,
every one already unpinned, none reusable until then.

**How many pins a session has.** A pin returns to the pool on
its release, so the pins in use at a moment are the rows
pinned at that moment: not the rows read so far, and not the
refs made so far. For example:

- An index scan that reads a million rows into one ref holds
  one pin at a time; each read releases the pin before it.
- Index maintenance that makes a ref per row and unpins it
  holds one pin at a time, and reuses the same pin from the
  pool each time.
- A join that keeps its outer row while it reads inner rows
  holds two: one per ref still pinned.

The pool grows to the most pins held at once, and keeps them
until the session closes. A released pin keeps nothing but
its generation: its iterator is destroyed. It cannot be freed
before the session closes, because a stale ref may still point
to it, and an unpin through that ref must find the pin there
to compare generations, not freed memory.

### 4.3 A savepoint releases nothing

sm1 releases the cursors opened after a savepoint's mark when
it is rolled back to (`sm1/src/xct.cc`): their snapshot would
admit the undone versions to the reads they make later. A pin
makes no later read. Its row was read before the rollback,
and its iterator still holds the bytes, so the rollback has no
reason to release it. Whether the row is still wanted only the
caller knows.

### 4.4 A dropped store keeps its pins

A rollback to a savepoint drops the column families created
after its mark (`sm1/src/xct.cc`), and the garbage collector
drops those of the stores whose drop has committed, in any
session's. Neither releases a pin. RocksDB keeps a column
family's data while an iterator holds it: the iterator's
SuperVersion holds a reference on it, which outlives both
`DropColumnFamily` and `DestroyColumnFamilyHandle`. So a
pinned row stays readable after its store is gone, until its
pin is released, and its files stay on disk until then.
Releasing first was considered, and dropped: a session could
release only its own pins, not those another session holds on
a store the collector drops. `test_pin.cc` reads a pinned row
after its store is dropped, under the address sanitizer too.

### 4.5 The lending form, and cursors

- The present `sm1::read(session, storeid, key, data)`, whose
  `data` lasts until the session's next call, stays: it reads
  into a ref the session keeps. Its callers lose the copy B
  and keep their contract.
- A cursor's row is not pinned. `scannxt`'s row lasts until
  the cursor's next call (§6.1 removes its copy), as a heap
  scan's tuple lasts in PostgreSQL until the scan moves.

### 4.6 The layout

The ref is two words; the pin is one cache line, 64 bytes,
and its iterator while it is held.

```cpp
namespace sm1 {

/* A pin ref: a plain value the caller holds. Zero names no pin. */
struct pin_ref_t {
  pin_t *pin = nullptr; /* the pin it was taken on */
  uint64_t gen = 0;     /* the pin's generation when taken */
};

} // namespace sm1

/* A pin: keeps one row's bytes in memory. The session owns it. */
struct pin_t {
  std::unique_ptr<rocksdb::Iterator> it; /* found the row; null when free */
  rocksdb::Slice row;                    /* the row's bytes, in a block it holds */
  uint64_t gen = 1;                      /* increased on each release */
  sm1::session_t *session = nullptr;     /* whose pool it is in */
  pin_t *prev = nullptr;                 /* in the session's held list, */
  pin_t *next = nullptr;                 /* or next in its free list */
};

/* In sm1::session_t: */
std::vector<std::unique_ptr<pin_t>> pins; /* every pin, freed at close */
pin_t *pins_free = nullptr;               /* released, ready to take */
pin_t *pins_held = nullptr;               /* taken, not yet released */
```

What each operation does to them:

- **Stale**: a ref is stale when `ref.pin` is null or
  `ref.gen != ref.pin->gen`. `sm1::unpin` and a read into a
  ref check it first.
- **Take**, a read into `ref`: if `ref` is not stale, its pin
  is released first. A pin is popped from `pins_free`, or
  made and appended to `pins` if none is free; its `it` and
  `row` are set, it is linked into `pins_held`, and `ref`
  becomes `{pin, pin->gen}`.
- **Release**: `it` is reset, `row` cleared and `gen`
  increased; the pin is unlinked from `pins_held` and pushed
  onto `pins_free`. Every ref to it is now stale.
- **`sm1::release_all_pins`, and the end of the
  transaction**, release every pin on `pins_held`, which then
  is empty, and every pin is on `pins_free`.
- **The session's close** frees every pin, through `pins`.
  Until then no pin is freed, so `ref.pin` always points to a
  pin, held or free.

Why it is laid out so:

- **No flag for held.** A free pin's `gen` is newer than that
  of every ref ever taken on it, so the generation alone tells
  a held pin from a free one.
- **`prev` and `next` serve both lists**, as a pin is on
  exactly one of them. The held list is doubly linked, so an
  unpin unlinks its pin in O(1).
- **`session` points back to the pool**, so `sm1::unpin(ref)`
  needs no session, and digres's slot can unpin with its ref
  alone. `sm1::unpin(session, ref)` would save the 8 bytes.
- **`gen` starts at 1**, so a ref whose `gen` is 0 never
  matches a pin.
- **The C wrapper's `sm_pin_ref_t`** is the same two words,
  `{void *pin; uint64_t gen;}`, opaque to digres.

### 4.7 No content lock

A PostgreSQL pin is only a count. Its shared and exclusive
modes are those of the buffer's content lock, taken beside the
pin, because a heap page changes in place: a writer sets a
tuple's xmax, hint bits and line pointers under an exclusive
lock, and a reader takes a shared one so as not to see them
half-written. Its cleanup lock, for pruning and VACUUM, waits
until it holds the page's only pin, as it moves tuples whose
addresses other pins hold; a cursor left on a page blocks it.

sm needs neither, as nothing changes a row's bytes once
written:

- **A write adds a version.** Every write is a new key put
  with `rdb->Put` (`sm1/src/access.cc`); sm1 has no write
  batch, so even a transaction's own rows are memtable
  entries already.
- **RocksDB changes nothing in place.** A memtable's entries
  are in an append-only arena, and an SST file's blocks are
  never written again. `inplace_update_support`, the one
  exception, is off: sm does not set it, and it defaults to
  false.
- **Nothing moves a row.** Compaction writes new files, and
  does not wait for a reader; the pinned iterator's
  SuperVersion keeps the old memtable and files, as it keeps a
  dropped store's (§4.4).
- **A reader sees only whole rows**, so it has nothing to
  wait on a writer for, nor a writer on it.

What a held pin costs is space, not a wait (§7):

- **Memory.** A memtable stays in memory after its flush,
  while a SuperVersion holds it. It does not hold writes up:
  RocksDB stops writes only when the column family has
  `max_write_buffer_number` memtables not yet flushed
  (`GetWriteStallConditionAndCause`, `db/column_family.cc`),
  and the pin holds a flushed one. Nor does a pin on the
  active memtable hold them up, as inserts go on beside its
  iterator. The exception would be a write buffer manager,
  a memory limit shared across column families, which sm does
  not set: sm1 opens with the default options
  (`sm1/src/db.cc`), so `db_write_buffer_size` is 0 and the
  manager is off. Were one set, as every store being a column
  family may one day call for, a held memtable's memory would
  count against it until `~MemTable` runs. That brings flushes
  early, and so more L0 files and maybe an L0 stall, or, with
  `allow_stall`, stalls writes outright. A long-held pin would
  then slow writes, and the release at the end of a statement
  (§8 step 8) would matter more.
- **Disk.** The files a compaction replaced are deleted only
  when the last iterator on them goes.
- **The block cache.** A block a pin holds is not evicted; as
  `strict_capacity_limit` is false by default, the cache grows
  past its capacity rather than failing.

---

## 5. Why not the other ways

- **Copying, as today**: two copies of every row read by
  rowno, §2.
- **A pin the caller owns, linked into the session**: sm
  would release it at the end of the transaction by writing
  into the caller's memory, which the caller may have freed
  already. digres frees it on every error: its `ereport`
  throws a C++ exception (`../dg/src/backend/utils/error/
  elog.c`), but a slot is palloc'd memory, and a memory
  context is freed with no destructor run.
- **A ResourceOwner, or a memory context callback, in
  digres**: either releases a lost pin early, but makes sm's
  correctness depend on its host's machinery. sm's own end of
  the transaction is enough.
- **Releasing at a savepoint the pins taken after its mark**,
  as cursors are: an outer cursor fetched inside the
  savepoint stays usable after the rollback, as PostgreSQL
  documents for `ROLLBACK TO SAVEPOINT`, and a nested loop or
  a merge join over it reads the row it held from one FETCH
  to the next.

---

## 6. Suggested changes

### 6.1 sm1

- **The pin, the pin ref and the pool** of §4: the session's
  free pins in a list, a ref two words, `sm1::unpin` and
  `sm1::release_all_pins`.
- **`read`, `read_latest`, `update` and `erase`** read into
  a ref; the lending form reads into the session's own.
  Removes B.
- **The end of the transaction** releases the pins still
  held, where `xct_end_scans()` closes the cursors: at
  commit, at abort and in `discard()`.
- **`scan_next` lends its row** (`NEXT.md`): it returns on
  the visible version and steps past the key's older ones at
  the next call, so the row is a view into the iterator's
  block until then. `last_examined` and `skip_version()` move
  with it, which touches SSI. Removes A.
- **The userkey is not copied twice**: `last_examined` takes
  it by swap or move, not by assignment. Removes half of A′,
  and the rest with the lent row.

### 6.2 sm2 and sm3

- **`rel_read`, `rel_read_latest`, `tbl_read`,
  `tbl_read_latest`** and the index's point reads take a ref
  and pass it down.
- **`tbl_delete` keeps the old row in a ref of its own**, not
  a copy (`sm3/src/table/table.cc:804`): the copy is there
  because index maintenance's reads overwrite the session's
  buffer, which a ref of its own is not.
- **An index-only scan decodes a key column once**:
  `decode_key_front` unescapes straight into the varlena
  `key_datum` hands out, with no `std::string` between them.

### 6.3 The C wrapper

- `sm_pin_ref_t`, a value: two words, zero for none. No
  create, no free.
- `sm_tbl_read()`, `sm_tbl_read_latest()`, `sm_tbl_update()`
  and `sm_tbl_delete()` take one and hand back a view.
- `sm_unpin()` and `sm_release_all_pins()`.

### 6.4 digres

- **A scan holds a pin ref.** An index scan, an index-only
  scan and a bitmap heap scan's fetch keep one `sm_pin_ref_t`
  in their `smstore_scan_t`, which each row they read by rowno
  is read into, and the scan's end unpins. Its slot's values
  point into the row until the scan reads its next: the
  lifetime a cursor's row has, which the executor already
  keeps to, as a scan's slot is overwritten at its next
  fetch. No new kind of slot. A slot that holds the ref, with
  a `TupleTableSlotOps` of its own whose clear unpins it, was
  considered: it would cover `smstore_fetch_row()` too, but
  every slot of an sm table would change kind.
- **Nothing on an error.** A scan freed by an error leaves its
  ref behind; the pin is released at the end of the
  transaction (§4.1). No ResourceOwner kind, no callback.
- **`read_row()` stops materializing** when its caller has a
  scan, and so a ref. Removes C from the index scan, the
  bitmap heap scan, and `entry_from_row()`'s. A TID scan, the
  fetch of a write's target and a recheck have no scan, and
  still read lent and copy once: B is gone from them, as the
  lent read is a pin of the session's.
- **`keep_row()` keeps a pin ref, not a `memcpy`**, for the
  statement: the deleted row for RETURNING and the newest
  version for a recheck.
- **A decimal does not materialize the slot.** The converted
  numerics go into memory the slot frees when it is cleared,
  and the other columns stay where they point. Removes D.
- **All pins released at the end of a statement**, by
  `sm_release_all_pins()`, when no row pinned is wanted after
  it. That shortens a pin left behind by an error from the
  transaction's life to the statement's. It is safe only when
  no portal outlives the statement with a slot that holds a
  ref:
  - *An open cursor* is such a portal. A FETCH leaves its
    plan's slots holding rows, and the next FETCH, in another
    statement, may read them: a nested loop's outer row, a
    merge join's marked one. With a cursor open, the call is
    skipped, and the pins wait for the transaction's end, or
    for the cursor's close and the next statement's end.
  - *A holdable cursor* has copied its rows out at its
    commit, so it holds no ref, and does not count.
  - *`keep_row()`'s ref* lasts for the statement, so it is
    released with the rest; the next statement starts with
    none kept.

  The statement is the top-level one, the client's. A
  statement a function runs inside it, through SPI, is not
  the end of anything the top-level statement's plan holds.

  digres has no procedural language, and no subtransaction
  of its own: a subtransaction is only a client's
  `SAVEPOINT`, and every rollback to one is a top-level
  statement. So a pin an error leaves behind is released at
  the end of that statement, unless a cursor is open (§7).

digres's commit is in the right order for it. In
`CommitTransaction` (`../dg/src/backend/access/transam/xact.c`)
`PreCommit_Portals()`, where a holdable cursor copies its rows
into its own store, runs before `smstore_commit()`, which
releases the pins. At abort a holdable cursor of the aborted
transaction is dropped, and those of earlier ones were copied
at their commit.

### 6.5 What is left

| Scan | Entry | Row |
|---|---|---|
| Sequential, sample | — | 0 |
| Index, bitmap heap | 0 | 0 |
| TID, a write's target, a recheck | — | 1, the slot's copy |
| Index-only | key columns 1, INCLUDE 0 | — |
| DELETE's old row | — | 0 |

Left are the index-only scan's unescaping of a key, which
PostgreSQL's btree matches with its copy into `currTuples`,
and the one copy of a read by rowno that has no scan to hold
its ref (§6.4).

---

## 7. Hazards

- **A failed statement's pins stay while a cursor is open.**
  Only an error leaves a pin behind, and the release at the
  end of a statement (§6.4) takes it, except while a cursor
  is open. A client that opens a cursor, then loops on
  `SAVEPOINT`, a statement that fails, and `ROLLBACK TO`,
  leaves the pins of each failed turn, an iterator each, a
  few KB and the blocks it holds, until the cursor is closed
  or the transaction ends.
- **A scripting language would make it worse.** One is
  planned, over SPI. A loop that catches an error each turn,
  as PL/pgSQL's `EXCEPTION` block does, runs within one
  top-level statement, and would leave a pin a turn until
  that statement ends, cursor or none. Either case, if seen,
  is met by a ref marked as the statement's, which a rollback
  to a savepoint releases: an addition, not a change to §4.
- **A cursor opened inside a savepoint is closed by a rollback
  to it**, and that is sm1's rule for cursors, not pins
  (`sm1/src/xct.cc`). An outer SQL cursor over a nested loop,
  fetched inside a savepoint, may rescan its inner index scan
  there, which opens a new sm cursor; after `ROLLBACK TO` its
  next FETCH fails with `SM_ERR_SCANCLOSED`, which digres
  reports as the scan's old view not being recreated, where
  PostgreSQL goes on: a known incompatibility
  (`../dg/README.md`). So a join's outer row read by rowno
  after a mark is never held across a rollback to it:
  `../dg/test/slt/point_read.test` fetches the first row
  before the savepoint for that reason.
- **A held pin holds a SuperVersion.** A cursor left open for
  hours keeps the memtables and files its rows are in (§4.7);
  PostgreSQL's pin keeps a page, which is cheaper, but blocks
  VACUUM on it. A session idle in a transaction with a cursor
  open is the case to watch; the release at the end of a
  statement (§8 step 8) limits the rest to the open scans.
- **The lent cursor row touches SSI.** `last_examined`
  narrows the SIREAD range at the cursor's close; returning
  before the older versions are stepped past moves where it
  is set. The isolation scenarios in `sm1/mvcc` are its test.

---

## 8. Order of work

1. **sm1's pin, pin ref and pool**, with the lending form
   over the session's ref; sm1's tests unchanged, and new ones
   for two rows pinned at once, a stale ref, a ref across a
   transaction's end, and a pin on a store a rollback to a
   savepoint drops. *Built.*
2. **sm2, sm3 and the wrapper** pass the ref through for a
   point read. *Built.*
3. **digres's scans** hold a ref; `read_row()` stops copying
   for them. The cursor and savepoint case of §5 as a
   regression test, in `../dg/test/slt/point_read.test`.
   *Built.*
4. **The old row of a write** in a ref: sm1's `update` and
   `erase`, sm2's `rel->old` copy, `tbl_update` and
   `tbl_delete`'s copies, and `keep_row()`.
5. **The decimal slot.**
6. **The index-only scan's key decoding.**
7. **`scan_next` lends its row**, with the SSI change.
8. **`sm_release_all_pins()` at the end of a statement**,
   with the open cursor check of §6.4.
