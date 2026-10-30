# ROW — a row a client deforms only as far as it reads

A client that reads a row from sm deforms all of it today,
whatever it reads of it: digres's slots are virtual, so the
tuple is deformed into every column the plan might use before
the executor asks for any. PostgreSQL's heap slot deforms
lazily, only as far as the expression being evaluated needs.
The objective here is the same for every client of sm, digres
now and perhaps MySQL later: a row handed out as a handle the
client deforms column by column, as far as it reads, safely
for as long as the handle is valid, and with no copy.

The handle is an sm3 pin. It extends the pin of `PIN.md`, and
moves its pool from sm1 to sm3. This file is the design; §8 is
the order of work.

Paths are relative to `sm/`; digres's are under `../dg/`.

---

## 0. In brief

- **sm1 keeps a raw pin**, `sm1::rawpin_t`: the iterator that
  found a row and the row's bytes, owned by the caller. sm1
  has no pool, no generation and no refs any more.
- **sm3 owns the pins**, `sm3::pin_t`: a raw pin, the row's
  table definition, and how far the row is deformed. The pool,
  the generations and the refs of `PIN.md` §4 move here.
- **A scan's row is a pin too.** Each sm3 scan keeps one pin,
  and each `scannxt` moves it to the next row and makes every
  ref to the previous one stale.
- **A client deforms through a ref**: columns `from` to `n`,
  resuming where the last deform stopped, or one column by
  ordinal. A stale ref fails; it never reads freed memory.
- **A read checks only what it reads**: the header, the null
  bitmap, and the entry and varlena of each column deformed.
  Writes and `tup_check` keep the whole check.
- **digres's slot holds a ref.** It is no longer virtual, so
  the executor's expressions fetch only the columns they need,
  as with a heap slot.

---

## 1. Why

- **What a full deform costs.** `count(*)` over `wide_mixed`
  (`bench/`), 50 columns: sm2's `tup_deform` and `parse` are
  about 23% of digres's time, all of it columns nobody reads.
  PostgreSQL's planner gives a scan every column (its
  physical tlist, `use_physical_tlist`), and its heap slot
  deforms none of them unless asked.
- **Why the slot cannot just be lazy.** The executor compiles
  a fetch step (`EEOP_*_FETCHSOME`) before an expression's
  first column, and leaves it out for a virtual slot
  (`execExpr.c`), whose columns are taken to be filled. A
  lazy slot is one of a type of its own.
- **Why a lazy slot needs a handle.** It deforms after the
  read returned, so the bytes must still be there. A heap
  slot holds a buffer pin. A pointer to sm's bytes would read
  freed memory once the scan moved or the pin was released; a
  ref with a generation fails instead.
- **Why in sm, not in digres.** Every client needs it: a
  handle to a row, deformed only as far as it is read. What
  it needs to know, the table's layout and the tuple's
  format, is sm3's and sm2's.
- **Why sm3 owns the pool.** sm1 knows bytes, not tuples. The
  pin a client holds has to carry the table definition, so it
  is sm3's; sm1 only keeps the iterator that holds the bytes.
  The generations and the release rules are what clients see,
  and sm3 is the level they call.

---

## 2. sm1: the raw pin

```cpp
namespace sm1 {

/* A raw pin: keeps one row read or replaced in memory. Opaque. */
struct rawpin_t;

rawpin_t* rawpin_new();             /* empty, or NULL: SM_ERR_NOMEM */
void rawpin_free(rawpin_t* pin);    /* empties it first */
void unpin(rawpin_t* pin);          /* lets go of its row */

} // namespace sm1
```

- **Opaque, made by sm1.** A public header names no RocksDB
  type (`CODE.md` §1), so the pin is a handle: the iterator
  and the row's bytes are in `sm1/src/session.h`. A caller
  that keeps one per pin of its own, as sm3 does, makes it
  once and reuses it.
- **Owned by the caller.** A read or a write that keeps a
  row fills a `rawpin_t*` the caller gives it; a pin that
  held a row lets go of it first. sm1 never keeps a list of
  them.
- **The calls** are those that took a `pin_ref_t&`:
  `read`, `read_latest`, `update` and `erase`. A write fills
  a raw pin of its own first and moves it into the caller's
  only once it has succeeded, so the data written may view
  the row the caller's pin held; on failure the pin keeps
  what it held.
- **The lending form stays.** `sm1::read` with no pin, and a
  write's `old` with none, use one raw pin of the session's
  own, valid until its next call, as before; the end of the
  transaction and the session's close empty it.
- **No release at the end of a transaction.** sm1 no longer
  knows the caller's pins. A point read's iterator carries no
  RocksDB snapshot (`sm1/src/access.cc`): visibility is sm1's
  own, by timestamp, so a raw pin outliving its transaction
  holds memory, never a view. `PIN.md` §4.4 still holds: a
  dropped store keeps the pinned row readable.
- **A read into a pin seeks its iterator again.** A point
  read's row stays in the iterator that found it, and the next
  `read` into the same pin seeks that iterator rather than
  opening another, when it holds everything the read may see:
  the same session and store, the read epoch it was opened in,
  every commit the view sees, and no write by the session
  since; never under SERIALIZABLE (`sm1/SM1_DESIGN.md` §10.1).
  Opening an iterator was half of a point read's cost. A read
  that finds no row empties the pin, as before.
- **One rule for its callers**: every raw pin is emptied
  before the database closes, since RocksDB must not close a
  DB with an iterator open.
- **`release_all_pins` and `pin_ref_t` leave sm1**, and with
  them `sm1/src/pin.cc`'s lists and generations.
- **`scan_rolled_back(scan)`** is new: whether a rollback to a
  savepoint closed a cursor, which sm3 asks after a rollback
  (§4.5).

---

## 3. sm2: a tuple read column by column

sm2's rel calls that pin take a `sm1::rawpin_t&` in place of a
ref: `rel_read`, `rel_read_latest`, `rel_update` and
`rel_delete`. A client of sm2 alone gets pinned reads and
keeps each pin itself.

What sm2 adds is a reader: a tuple's header parsed once, then
its columns deformed in as many steps as the client takes.

```cpp
namespace sm2 {

/*
 * A tuple being read: its header and null bitmap, parsed once,
 * and where the next attribute's entry is. It views the tuple
 * and the schema, so it lasts as long as both.
 */
struct tup_reader_t {
  const tup_desc_t* desc = nullptr;
  std::string_view  tuple;
  const char*       nulls = nullptr; /* the bitmap, or none */
  size_t            natts = 0;       /* attributes the tuple has */
  size_t            next = 0;        /* the next attribute */
  size_t            pos = 0;         /* where its entry is */
};

/* Parse tuple's header under desc into r, at attribute 0. */
int tup_read_begin(const tup_desc_t& desc, std::string_view tuple,
                   tup_reader_t& r);

/*
 * Attributes r.next to n - 1 into datums and isnull at those
 * places, n at most the schema's size; r is then at n.
 */
int tup_read(tup_reader_t& r, size_t n, std::span<datum_t> datums,
             std::span<bool> isnull);

} // namespace sm2
```

- **It is `deform_each` cut in two.** `parse_head` is
  `tup_read_begin`; the loop over the entries, with `pos`
  carried from one call to the next, is `tup_read`. Each
  entry is checked as it is read, as `check_entry` does now.
- **Resuming is free.** An expression that needs column 3,
  then one that needs column 40, deforms 0 to 3, then 4 to
  40: each column once.
- **By ordinal, `tup_decode` stays.** A client that reads
  columns out of order, as MySQL's `read_set` does, decodes
  each one; it walks the null bitmap, not the entries before.
- **`tup_deform` stays as it is**, the whole tuple checked
  when every column is read (`sm2/TUPLE.md` §8), for its callers
  that hold bytes, not a pin.
- **A rel keeps a scratch raw pin** for a write of a row with
  datums out of line and no pin of the caller's: the old row
  stays pinned while its pointers are found. It is made on
  first use and freed by `rel_close`.

---

## 4. sm3: the pin

### 4.1 The pin and its ref

```cpp
/* sm3/src/table/pin.h */
struct sm3::pin_t {
  sm1::rawpin_t *raw;                     /* a read's row; made with the pin */
  std::string_view row;                   /* the row's bytes; empty when free */
  std::shared_ptr<const table_def_t> def; /* the row's layout */
  sm2::tup_reader_t reader;               /* how far deformed; begun lazily */
  uint64_t gen = 1;                       /* increased on each release */
  session_t *session;                     /* whose pool it is in */
  scan_t *scan;                           /* the cursor whose row it holds */
  pin_t *prev, *next;                     /* on its list */
};

/* sm3.h: a plain value the caller holds. Zero names no pin. */
struct pin_ref_t {
  pin_t* pin = nullptr;
  uint64_t gen = 0;
};
```

- **The pool is `PIN.md` §4.6's**, moved: every pin made, the
  free list, the held list, and the same take, release and
  staleness rules, in sm3's session. A third list holds the
  cursors' pins (§4.3). `sm3.h` declares `pin_t` only; its
  layout is sm3's.
- **`row` is apart from `raw`**, as a cursor's pin views the
  cursor's row and holds no iterator of its own.
- **`def` is set when the row is read**, and assigned only
  when it is a different definition: a scan of one table, or
  an index scan reading it row after row, copies the
  `shared_ptr` once. A definition is never changed, only
  replaced (`table_def_t`), so the pin's lasts as long as the
  row.
- **A released pin keeps its `def`** until it is taken again:
  memory only, a pool's worth at most.
- **The reader is begun on the first deform**, not on the
  read, so a row nobody deforms costs no parse: a `count(*)`,
  or an UPDATE's old row that no index needs.

### 4.2 Reading into a pin

`tbl_read`, `tbl_read_latest`, `tbl_update` and `tbl_delete`
take a `sm3::pin_ref_t&` where they took a `sm1::pin_ref_t&`,
with the same contract: a pin the ref still names is released
first, or for a write once all of it has succeeded, its
indexes fixed. The pin is taken from the session's pool and
its `raw` handed down. A read into a ref that still names a
held pin reads into that pin instead (`pin_reread`): its refs
go stale as on a release, but its raw pin keeps the iterator,
for sm1 to seek again (§2). An index scan reading row after row
into one ref opens one iterator, not one a row. A write with
no ref keeps the old row in a pin of the pool while its
indexes are fixed, then puts it back.

### 4.3 A scan's row

- **`tbl_scan` returns an sm3 cursor**, `sm3::scan_t`: the
  sm2 rel scan, and one pin taken from the pool when it
  opens. `tbl_scannxt`, `tbl_scan_seek` and `tbl_scanend`
  replace sm2's calls on it.
- **`tbl_scannxt` hands back a ref** to that pin, and the
  row's bytes. It increases the pin's generation, points
  `row` at the cursor's row, and resets the reader; a ref to
  the previous row is then stale, as is the last at the end.
  A form without the ref is for callers that want the bytes.
- **What it costs a row**: one increment and two stores. The
  bytes are the cursor's, valid until it moves, as now.
- **Its `scan` is set**, which the release rules read:
  - `unpin` through its ref does nothing; the scan owns it.
  - Releasing all pins, and the end of the transaction, make
    it stale and leave it with the scan. sm1 closes the
    cursor then too (`xct_end_scans`), so its row is gone.
  - Closing the scan releases it to the pool.

### 4.4 Deforming

```cpp
/*
 * Attributes from to n - 1 of the row ref pins, into datums and
 * isnull at those places. Resumes where the last deform stopped
 * when from is there; otherwise reads from the header again, and
 * may write attributes before from again, with the values they
 * had. A by-reference datum points into the row and lasts as
 * long as the pin. SM_ERR_INVALID if ref is stale.
 */
int pin_deform(pin_ref_t ref, size_t from, size_t n,
               std::span<datum_t> datums, std::span<bool> isnull);

/* One attribute of the row ref pins, by ordinal, as tup_decode. */
int pin_decode(pin_ref_t ref, size_t ordinal, datum_t& val, bool& isnull);

/* The row's bytes, to copy; SM_ERR_INVALID if ref is stale. */
int pin_tuple(pin_ref_t ref, std::string_view& tuple);
```

- **`from` is the client's**, so the pin keeps no values,
  only the reader: digres passes its slot's `tts_nvalid`, and
  its `tts_values` keep what earlier deforms filled.
- **A value stored out of line** comes back as its TOAST
  pointer, as now; `tbl_fetch` reads it.

### 4.5 When pins are released

- **At the end of the transaction**: `sm3::commit`, `abort`
  and `prepare` release every pin, as sm1 did, and make each
  cursor's pin stale: sm1 closes the cursors then. `prepare`
  releases them before it runs, whether or not it succeeds.
- **A rollback to a savepoint** releases no pin (`PIN.md`
  §4.3), but a cursor it closes has no row any more: sm3 asks
  each cursor's pin `scan_rolled_back` after the rollback,
  and makes the closed ones' stale.
- **When sm1 ends the transaction on its own**, as after a
  serialization failure, the pins stay until the client's
  `sm3::abort`. That holds memory only (§2).
- **At `session_close`**, before the client closes sm1's
  session, so no iterator outlives the database.
- **`release_all_pins(session)`** is sm3's, for a client that
  knows none of its rows is still wanted.

### 4.6 What a read checks

A deform checks what it reads, as `tup_decode` does: the
header and bitmap once, then each entry and varlena it
deforms. A malformed part of the row nobody reads is not
found. Writes check a tuple whole before it is stored, and
`tup_check` checks one on demand, so a tuple sm wrote is whole;
reading is checked against damage in what it reads, not
audited.

---

## 5. The C wrapper

- **`sm_pin_ref_t` keeps its two words**, now naming an
  sm3 pin; `sm_unpin` and `sm_release_all_pins` call sm3.
- **New calls:**
  - `sm_pin_deform(ref, from, n, values, isnull)`;
  - `sm_pin_decode(ref, ordinal, val, isnull)`;
  - `sm_pin_tuple(ref, tuple, len)`.
- **`sm_rel_scannxt` gains a form** that hands back the row's
  ref instead of its bytes.
- **`sm_tbl_deform` and `sm_tbl_decode` stay**, for bytes a
  client holds itself.
- **Order of close**: `sm3::session_close` before
  `sm1::closedb`, as `src/sm.cc` already does.

---

## 6. digres: a slot holding a ref

- **A slot type of its own**, `TTSOpsSmRow`, in
  `access/sm/smstore.c`: a virtual slot with the ref of the
  row stored into it. Not being virtual, it gets the
  executor's fetch steps. `smtable_slot_callbacks()` returns
  it, and the sequential, index and bitmap heap scans take it
  from `table_slot_callbacks()`, as PostgreSQL's do.
- **`getsomeattrs(slot, n)`** calls
  `sm_pin_deform(ref, tts_nvalid, n, …)`.
- **Stored with no ref**, as an INSERT's values, COPY's, a
  projection's, a lent copy's or an index-only scan's are, it
  is a virtual slot: its columns filled by whoever stored them.
- **`materialize`** deforms every column, drops the ref, and
  copies the by-reference values into the slot's memory, as a
  virtual slot materializes; `copyslot`, `copy_heap_tuple` and
  `copy_minimal_tuple` deform every column first, then do as a
  virtual slot does. A slot kept past the row's pin, by
  EvalPlanQual, a trigger, a sort or a cursor held across
  statements, is copied so already.
- **`clear`** drops the ref; it does not unpin. The pin is
  the reader's: the cursor's, or the index or fetch scan's ref.
- **System columns** need nothing: `slot_getsysattr` serves
  `tableoid` and a keyless table's `_id` from the slot itself;
  no row has a `ctid` (`ROWKEY.md`).
- **Who reads the values directly** asks the slot for them
  first: an index scan checks each row against its keys
  (`keys_met()`), so it deforms up to each key's column.
- **A slot of another type** given a ref is filled whole at
  once, as a virtual tuple (`ref_into_slot()`).
- **What goes**: the column count the sequential, index,
  bitmap heap and sample scans deformed to, and
  `smtable_scan_set_natts()`. A key scan, for `WHERE CURRENT
  OF` or `_id = n` only (`ROWKEY.md` §3.3), and a
  DELETE's RETURNING read a lent copy by the row's key and
  keep theirs (`smstore_scan_natts()`,
  `smstore_expr_natts()`).
- **A stale ref is an error**, not a wrong answer: a node
  that kept a slot after its scan moved, without copying it,
  is found by the test that runs it.

---

## 7. What it does not do

- **TOAST**: a value out of line is fetched and assembled as
  now (`PIN.md` §3, a pin covers the row only).
- **The block cache and compression**: the other part of the
  Q1 profile (`bench/`), a separate change.
- **A column read twice out of order** is decoded twice, by
  `pin_decode`; a client reading in order resumes instead.

---

## 8. Order of work

Each step builds and passes its level's tests before the
next.

1. **sm2: the reader.** `tup_reader_t`, `tup_read_begin`,
   `tup_read`; `deform_each` built on them. Tests: a tuple
   read in steps equals one deformed whole; a malformed entry
   is found only by the step that reads it.
2. **sm1: the raw pin.** `rawpin_t` and `unpin`; the reads
   and writes fill one; the pool, the refs and
   `release_all_pins` go. `test_pin.cc` keeps its cases on
   raw pins, the dropped store under the sanitizer among them.
3. **sm2: rel calls on raw pins.** `rel_read` and the writes;
   `test_rel.cc` and `test_toast.cc` follow.
4. **sm3: the pin.** The pool, refs and release rules moved
   from sm1; reads and writes into a ref; `scan_t` and its
   pin; `pin_deform`, `pin_decode`, `pin_tuple`. Tests: the
   generation cases of `PIN.md` §4.2; a scan's ref stale after
   `scannxt`; resuming; a stale ref refused.
5. **The C wrapper.** The new calls, and `sm_pin_ref_t` on
   sm3's pins.
6. **digres: `TTSOpsSmRow`.** Scans store refs; the slot's
   callbacks; `smstore_scan_natts()` goes. Tests: `SELECT *`
   with a selective filter on a wide table; a join's outer row
   kept while the inner scan runs; a cursor across statements;
   DELETE … RETURNING; EvalPlanQual. Then every suite.
7. **The benchmark again**, against `bench/results/`'s last
   run.

Steps 1 to 6 are built (`../dg/WORKNOTE.md` step 276); the
benchmark, step 7, has run since (`../bench/results/`).
`PIN.md` points here for what moved.
