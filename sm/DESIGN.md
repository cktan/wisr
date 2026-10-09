# DESIGN — wisr

How the stack in `OBJECTIVE.md` fits together: the levels,
the build, and the decisions that cross level boundaries.
Each level's own mechanics — sm1's MVCC, sm2's tuple
format, sm3's catalog — are argued in that level's own
`SMn_DESIGN.md`; this document does not repeat them.

---

## 1. The stack

```
                      PostgreSQL (C)
                             │  include/ — the one C API
        ┌────────────────────┴─────────────────────────┐
        │  top level  the C wrapper                    │
        │  sessions · transactions · 2PC → sm1 ·       │
        │  rows and index entries → sm2, sm3 later     │
        └────────────────────┬─────────────────────────┘
                             │  sm3::
        ┌────────────────────┴─────────────────────────┐
        │  sm3/  schema and index maintenance          │
        │  catalog · index maintenance ·               │
        │  scan-by-predicate · statistics              │
        └────────────────────┬─────────────────────────┘
                             │  sm2::  heap and index relations
                             │         (sm3 also calls sm1::)
        ┌────────────────────┴─────────────────────────┐
        │  sm2/  types and tuples                      │
        │  type system · key encoding ·                │
        │  tuple format · heap and index relations     │
        └────────────────────┬─────────────────────────┘
                             │  sm1::
        ┌────────────────────┴─────────────────────────┐
        │  sm1/  MVCC storage manager                  │
        │  relations · transactions · isolation ·      │
        │  locks · 2PC · recovery · GC                 │
        └────────────────────┬─────────────────────────┘
                             │  RocksDB's own API
        ┌────────────────────┴─────────────────────────┐
        │  RocksDB — ordered, durable, crash-atomic    │
        │  key-value store                             │
        └──────────────────────────────────────────────┘
```

sm1's unit of storage is the **store**: an ordered map of byte
keys to byte values, named by a `uint64_t` storeid. **A store
is exactly one RocksDB column family**, `CF:store.<storeid>`
(`sm1/SM1_DESIGN.md` §3.1): `sm1::mkstore` creates it and
`sm1::rmstore` drops it. Whatever RocksDB offers per column
family is therefore offered per store — an O(1) drop, and
temporariness (§5) — and no level above sees a column family
by that name.

sm2 is a client of sm1, nothing more: it holds no RocksDB
handle of its own and reaches storage only through sm1's
interface. sm3 is built on sm2 — it never encodes a key or
decodes a tuple itself — and may also call sm1's interface
directly, for whatever sm2 has nothing to add to: above all
`sm1::lock`, which sm3's DDL and index-build protocols are
built on (`sm3/SM3_DESIGN.md` §2). That boundary is why each
level's `OBJECTIVE.md` can promise it is "useful and
testable on its own" — the whole lower stack is one
dependency.

The top level is not a level: it adds no notion of its own, only
a C signature over the levels' C++ interfaces, and it is the one
place an exception is caught and a PostgreSQL error path is safe
to take (`OBJECTIVE.md`). Like sm3, it may skip a level. A C
session is an sm3 session over an sm1 one, and each database's
sm3 catalog is loaded by its first session: sessions,
transactions, savepoints and prepare go through sm3, which keeps
its overlay of DDL in step with them, and the rest of 2PC,
stores and sequences go to sm1 directly. A caller finds and
locks a table through sm3's catalog (`sm_cat_*`), then opens it
with sm2's calls from the definition it is handed: those take a
store and schema, and are scaffolding, marked so in
`include/sm.h`, until sm3 opens tables and maintains their
indexes itself; the row and entry calls keep their C signatures
whichever level serves them. sm2 calls only sm1, and nothing
calls upward.

The root's `include/` also holds two C headers that belong to
no level and that sm1 includes: `sm_error.h`, the stack's error
codes, `sm_lock.h`, its lock modes, and `sm_options.h`, sm1's
database options. They are
shared definitions, not an upward call: sm1 and the wrapper
must agree on them bytewise, and one C header is how.

---

## 2. Build

Each level has a real `Makefile`; the top-level one
delegates to them, and builds the one thing that belongs to
no level — the C wrapper in the root's `include/` and `src/`
— into `build/libsm.a`. That archive bundles every level's
objects with the wrapper's, by an `ar` script, so PostgreSQL
links one archive, plus RocksDB, ICU and the C++ runtime, and
never learns the stack has levels. The test driver in `rel/`
drives the C wrapper and verifies the relational engine
against `libsm.a` via automated Janet test scripts.

```
make            build every level
make test       build and test every level
make -C sm1     build level 1 alone
```

`LEVELS` in the top-level `Makefile` lists which levels have
a Makefile to delegate to; `sm3` joins it when it has one.
Adding a level to the stack means adding its name there,
nowhere else.

**Never compile with `-fno-exceptions`,** at any level or in the
C wrapper. An exception may unwind through sm1, sm2 and sm3 to
the wrapper's catch-all (§5). The wrapper needs `try` and
`catch`, which the flag forbids. Each level needs the cleanup
code that runs destructors during unwinding, which the flag omits
— without it an RAII lock guard never releases its mutex, or the
process terminates. Nor does the flag stop anything being thrown:
the standard library throws from its own compiled code whatever
the flag says. GCC enables exceptions for C++ by default; this
rule is that nothing turns them off.

---

## 3. Status

| Level | Status |
| --- | --- |
| sm1 | built and tested, with its C++ interface in `sm1/include/sm1.h`; the design of record is `sm1/SM1_DESIGN.md` |
| sm2 | built but for TOAST: its types, key encoding, tuple codec and heap and index relations, with its C++ interface in `sm2/include/sm2.h`; `sm2/SM2_DESIGN.md` is the design of record, and its §8 has the build order |
| sm3 | being built: its catalog (`sm3/CATALOG.md`), object mapping, index maintenance through table-level writes, and the concurrent index build with its background workers, partial and expression indexes among them, with its C++ interface in `sm3/include/sm3.h`; constraints are settled, `NOT NULL` built in sm3 and the rest the host's (§5); statistics has a strategy, `STATISTICS.md`, and no design; the order of work is in `sm3/SM3_DESIGN.md` §8, and what is left in `NEXT.md` |
| top level | built: `include/sm.h` over sm1, sm2 and sm3, with `src/sm.cc`. A row is read and written through an open table, `sm_tbl_*`, by the table's oid from `sm_cat_*`; the heap-level and index-level calls that took a store id and a relid are gone. The `rel` driver runs on it |

sm1 was built first and alone, on purpose: it is usable
without sm2 (an MVCC key-value store is a complete product
by itself), and every sm2 decision needs sm1's real API and
real behavior to check itself against, not a sketch of one.
sm2 and sm3 are split for the same reason in a different
key: sm2 is two permanent on-disk formats and a codec over
them, sm3 is protocols that can be rewritten, and keeping
the permanent part small is what lets it get all the
scrutiny (`sm2/SM2_DESIGN.md` §8).

---

## 4. Testing across levels

Every level uses the same two-layer scheme, established in
`sm1/mvcc/DRIVER.md`:

- ordinary tests for contract and single-session mechanics —
  C++ unit tests in each level's `test/`.
- a **declarative scenario suite** for anything where two
  sessions interleave — the isolation and concurrency
  questions that read as unreadable imperative test code.

Most of sm2 is the first kind — a codec is tested by
property, not by interleaving — and most of the second kind
arrives with sm3: an index build against a concurrent
writer, DDL against DML.

sm3 did not get a driver of its own, nor extend sm1's.
sm1's driver is one verb per sm1 call and links sm1 alone, so
teaching it `create table` would make sm1's test binary link
the levels above it. Instead sm1 gained a QA-only session
flag, `sm1::set_qa_nowait`: a write or a lock that would wait
fails with `SM_ERR_BLOCKED`, the transaction left active, and
the caller repeats the call (`sm1/SM1_DESIGN.md` §5.1). One
thread can then play several sessions, and "this step blocks"
needs no thread and no sleep. sm3's two-session cases are
written this way in C++ (`sm3/test/test_table.cc`), and again
through the C API as `rel` scripts (`rel/REL_DESIGN.md` §5).
What the flag cannot reach stays where it was: a polling
session is never a deadlock victim, so deadlocks are tested
with real threads in sm1, and the background index build is
tested with real threads in sm3.

---

## 5. Cross-level decisions

Decisions that constrain more than one level, gathered here
so a change to one level does not silently violate another's
design:

- **sm1's interface is the only channel into sm1.** Nothing
  above it may read RocksDB directly or depend on sm1's
  internals. A missing primitive is added to sm1's surface
  instead of worked around from outside it, as the ones
  secondary indexes needed were (`sm1/SM1_DESIGN.md` §1).
- **Every level reports failure the way sm1 does.** A call on a
  level's C++ interface returns a plain `int` — 0 on success, -1
  on failure — and the code and message are read afterwards from
  the thread-local `sm1::errnum()` and `sm1::errmsg()`. Every
  error call lives in `namespace sm1`; the code accessor is
  `errnum`, not `errno`, because `errno` is a macro. The codes
  are `SM_ERR_*` and `SM_OK`, values of the enum `sm1::errnum_t`
  — the stack's codes, not sm1's alone, which is why they carry
  no `1`. There is no status type and no `std::expected`, so the
  build stays at `-std=c++20`. A result comes back through an
  out-parameter. This keeps sm1's allocation-free message and
  per-thread state, and the C wrapper passes the convention
  through unchanged; the price is that nothing stops a caller
  ignoring a -1. It is one errno namespace for the whole stack:
  sm2 and sm3 report through the same thread-local state, and
  every error code any level uses is defined in the root's
  `include/sm_error.h`, a C header that sm1 includes and that
  `sm1.h` brings into `namespace sm1`, so the list holds codes
  only an upper level produces and the C wrapper passes one
  enum through unchanged. The enum is
  append only: a number is never reused or renumbered, a retired
  code stays with a note, and codes are grouped by the level that
  produces them under a comment naming it, with no reserved
  numeric ranges — a number matters once PostgreSQL sees it
  through the C wrapper, and this is the rule `sm2/TYPE.md`
  already applies to type codes. An upper level records an error
  with
  `int sm1::seterr(sm1::errnum_t code, std::string_view msg)`,
  exported by sm1: it copies the message into the fixed
  thread-local buffer, cut short if it does not fit, and returns
  -1, so a failing path is `return sm1::seterr(...);`. The caller
  formats the message, which keeps a template and `<format>` out
  of sm1's public header; reporting out of memory passes a
  literal. An error carries a code and a message and nothing
  else: whether it is retryable is a function of the code, which
  `sm1::retryable()` computes; the message stays allocation-free,
  in the fixed thread-local buffer, so reporting out of memory
  cannot itself fail; and no SQLSTATE is stored, since the fork's
  glue chooses it (below).
- **Two more calls sit beside `sm1::errnum()` and
  `sm1::errmsg()`.** `void sm1::reseterr()` clears the calling
  thread's state back to what a thread starts with — `SM_OK` and
  the message "no error". A successful call leaves the state
  untouched, so this is the one way to start a sequence of calls
  from a known state. `const char* sm1::errname()` names the
  current code: the enumerator without its `SM_` prefix, so
  `SM_ERR_UPDATED` gives `"ERR_UPDATED"` and `SM_OK` gives
  `"OK"`. The string is static and never NULL. With one errno
  namespace every level's codes are named here, so a new code is
  not finished until it has its name. The scenario driver keeps
  its own short names, so no transcript changes.
- **Only "no row" is a positive return; every other outcome is
  -1.** A read and `sm1::scannxt` return 1 for "no row". `DUPKEY`
  on insert, `NOTFOUND` on update or delete, and `UPDATED` are -1
  with the code in `sm1::errnum()`, like any failure, and a
  caller that treats one of them as an ordinary outcome reads the
  code. Under an `int` and a thread-local, reading it costs
  nothing, so there is no performance reason to split outcomes
  from errors; it also keeps `sm1/API.md`, the tests and the
  scenario transcripts' error names as they are.
- **Out-parameters take one shape at every level.** A required
  output is a reference (`uint64_t& storeid`). An optional output
  stays a pointer, `nullptr` meaning "not wanted" — the old image
  from an update or a delete. A list is a `std::vector<T>&`,
  which replaced sm1's C string list and `malloc`'d arrays. Bytes
  that point into engine memory are a `std::string_view&`, valid
  until the next call on the session, as today.
- **The top-level API is C++ in a C style.** `include/sm.h` is
  compiled as C++ by everything that includes it: the fork's
  glue is a C++ file, as `rel` is. It stays C in shape: free
  functions named `sm_*`, opaque handles, an `int` result with
  the thread-local error, callbacks that return a code and
  never throw. The log hook, which returns nothing and runs
  where an error has nowhere to go, is `noexcept` in its type:
  one that throws ends the process. Four C++ types may cross
  it and no others.
  `std::string` and `std::vector`, of plain C types, of
  `sm_datum_t` or of strings, carry owned data only: a
  result the caller keeps, or a list passed in, as an
  out-parameter or a const reference, filled inside the
  wrapper's catch-all. `std::string_view` is bytes that are
  not the receiver's: read during the call, or pointing into
  session memory for as long as the call says. `std::atomic`
  is a value the host owns and the stack reads while another
  thread may change it, such as a flag that says give up; it
  is how the host is asked something without a callback into
  it: `sm_cat_index_wait` takes one, where it took a function
  to call. No call takes a `std::string_view` yet: a row or
  an index entry on a scan is still a pointer and a length
  into session memory. No `optional`, `function`
  or `shared_ptr`, and no class of the stack's own: that line
  is what keeps the header from growing abstractions. A plain
  struct may hold the first two types, and a vector may hold
  such structs: a table's definition is one, of its columns and
  its indexes. What it replaced is what the header did to stay C:
  lists as one malloc'd block freed by hand, owned results
  that lived "until the next call", and a C mirror of each
  table definition, seven parallel arrays built and kept by
  the wrapper. The costs are that
  no plain C program and no foreign-function binding can
  include the header, and that both sides must be built with
  one C++ runtime, as they are while the stack is linked from
  source into one archive. It is still called the C wrapper
  and the C API below and elsewhere.
- **The C wrapper keeps the convention; the fork's glue
  calls `ereport`.** The wrapper's `sm_*` calls return 0 or
  -1 and leave the code and message in the same thread-local
  state. Inside a PostgreSQL backend a failure never travels
  back up as a return value — the access method callbacks
  return `void` or "got a row", and the executor has no
  error path — so the fork's C glue turns a -1 into
  `ereport(ERROR, ...)`, which aborts the transaction,
  releases what the statement held and sends the client an
  `ErrorResponse`. The wrapper never calls `ereport` itself:
  it is C++, and the `longjmp` would skip its destructors.
  So the wrapper catches everything and returns -1, control
  comes back to the glue with no C++ frame on the stack, and
  only then does the glue report. Not every -1 is reported:
  `UPDATED` under read committed sends the executor to
  re-check the row, and after a retryable code the engine
  has already aborted, so the wrapper must accept an abort
  of a transaction already gone. The glue therefore switches
  on the code first, and that switch is where the SQLSTATE
  is chosen, from PostgreSQL's own `utils/errcodes.h`. No
  SQLSTATE lives in the stack: it is a SQL notion, sm1 is
  meant to be useful without PostgreSQL, and some mappings
  depend on context only the glue has — `DUPKEY` is
  `unique_violation` from a unique index but a bug from a
  fresh `rowno`. The glue's mapping starts from:

  | Code | SQLSTATE | PostgreSQL name |
  | --- | --- | --- |
  | `CONFLICT`, `SERIALIZATION` | `40001` | `serialization_failure` |
  | `DEADLOCK` | `40P01` | `deadlock_detected` |
  | `DUPKEY` (from a unique index) | `23505` | `unique_violation` |
  | `NOMEM` | `53200` | `out_of_memory` |
  | `DISKFULL` | `53100` | `disk_full` |
  | `IO`, `PERMISSION` | `58030` | `io_error` |
  | `CORRUPT` | `XX001` | `data_corrupted` |
  | `NOTNULL` | `23502` | `not_null_violation` |
  | `KEYTOOLONG`, `ROWTOOLONG`, `UNDOLIMIT` | `54000` | `program_limit_exceeded` |
  | `SELFMODIFIED` | none for `UPDATE` and `DELETE`, which skip the row; `21000` for `INSERT ... ON CONFLICT` | `cardinality_violation` |
  | `INVALID`, `NOTRANS`, `INTRANS` | `XX000` | `internal_error` (a bug in the glue) |
  | `EXCEPTION` | `XX000` | `internal_error` (a bug in the stack) |
- **Running out of memory inside sm1 is fatal to the process,
  because `std::bad_alloc` can be thrown from inside RocksDB;
  above sm1 it is an error, `NOMEM`.** RocksDB
  allocates as it works — a write builds a batch, joins a write
  group, appends to the WAL, inserts into the memtable — and when
  an allocation fails, `operator new` throws through RocksDB's
  own frames. RocksDB reports its errors through `Status` and is
  not written to be unwound through: it keeps parts of its state
  consistent by explicit steps, not RAII, and an exception skips
  them. In a test that failed each allocation inside the status
  record's write in turn, the write's outcome was unknown — the
  record had reached the WAL or not, depending on where the
  exception came from — every later write to that database hung,
  and closing it could crash. No caller can repair that in
  process, and none can tell such a `bad_alloc` from a harmless
  one: an exception from inside RocksDB looks the same as one
  from sm1's own code, or from inside RocksDB before it touched
  anything. So every `bad_alloc` inside sm1 is treated as fatal,
  and only a fresh open's WAL replay recovers — reliably, and
  with every committed transaction. Above sm1 nothing touches
  RocksDB, so sm2 and sm3 can unwind and report instead.
  `sm1::attach` installs a new-handler, unless the host process
  already has one, that runs when an allocation fails, before
  anything is thrown, and asks whether the thread is inside sm1:
  every sm1 entry point, the GC thread and the compaction filter
  mark their thread as inside for as long as they run, with an
  RAII guard over a thread-local flag. Inside sm1 the handler
  reports `*** FATAL ***` on stderr and through the log hook,
  then aborts where the allocation failed, before any frame
  unwinds, so the core holds the failing allocation's own stack.
  Outside sm1 — in sm2, sm3, the wrapper or the host's own code
  — it throws `std::bad_alloc`, as `operator new` would with no
  handler, and sm2 and sm3 report it as `NOMEM` (next). RocksDB's
  own background threads run no sm1 entry point: an allocation
  failing there throws, and the exception ends the process
  through `std::terminate`, without the report. All of it should
  be very rare: on a default Linux system an allocation seldom
  fails, since the kernel's OOM killer ends the process first;
  it takes a hard memory limit to get here. A host with its own
  handler keeps it, but one that throws is unsupported: sm1 does
  not recover from a `bad_alloc` inside it, which can leave
  RocksDB unreliable and sm1's own state half-changed.
- **`std::bad_alloc` stops at sm2's and sm3's entry points;
  other exceptions cross the levels, and only the top level
  catches them.** Under sm1's handler (above), a `bad_alloc` is
  thrown only outside sm1. Every sm2 and sm3 entry point catches
  it and returns -1 with `NOMEM`; sm1's error state formats into
  a thread-local buffer and allocates nothing, so recording the
  error cannot fail in turn. Any other exception may pass from
  sm1 through sm2 and sm3 unchanged; sm1's entry points catch
  nothing. The C wrapper is the one catch-all, since it is the
  only place a C caller can be reached. It catches everything:
  `std::bad_alloc` becomes `NOMEM` — possible there only from
  the wrapper's own code — anything else `SM_ERR_EXCEPTION`,
  which means a bug, with `what()` as its message, and if the
  call involves a session the wrapper aborts that session's
  transaction before returning -1 — unless the transaction is
  prepared, which `sm1::in_doubt` tells it: that one is the
  coordinator's to resolve, and aborting it would break the
  promise `sm1::prepare` made. A `NOMEM` returned
  from below is handled the same way: the wrapper aborts the
  session's transaction, since a data operation that ran out of
  memory midway may have written part of its change. Aborting
  is the right recovery because sm1's abort works only in memory
  — it leaves the active table, which kills the transaction's
  versions without writing a status record, and releases its
  cursors, SSI node, created stores and locks — so it scarcely
  allocates, and it returns whatever a half-done call touched to
  a known state; every sm1 mutex is held by an RAII guard, so
  unwinding releases it. Four conditions come with it. Nothing
  may throw after a commit or prepare point: once the status or
  `PREPARED` record is written, an abort would kill in memory
  versions the WAL says are committed. Nothing does: that tail
  is `noexcept`, running out of memory in it aborts the process
  as anywhere inside sm1, and a failed write goes fail-stop.
  sm2 and sm3 must stay consistent when unwound mid-call, so
  the abort undoes what a
  half-done call changed — above all in sm3's in-memory catalog.
  After `NOMEM`, as after a retryable code, the engine has
  already aborted, so the glue does not continue the transaction;
  `NOMEM` is not retryable. And an exception on the GC thread or
  inside a RocksDB callback never reaches the wrapper, so those
  boundaries keep catch-alls of their own (next). Exceptions stay
  enabled in every build (§2).
- **The foreign boundaries catch for themselves.** The
  RocksDB callbacks — the compaction filter, the comparator,
  the prefix extractor — and the GC thread have no caller to
  return -1 to. Every callback and thread entry is
  `noexcept`, with a catch-all that reports through the
  `sm1::set_log` hook. The compaction filter keeps the version
  on error, so a failure can never delete data; the GC thread
  logs and tries again on its next cycle. The comparator and
  the prefix extractor are pure `memcmp` and byte slicing:
  they are marked `noexcept` to say so, and need no
  catch-all.

- **One encoder, one tuple format, two callers.** The key
  encoding and the tuple layout are sm2's alone
  (`sm2/SM2_DESIGN.md` §3, §4), and sm3 builds its index entries
  — `encode(sk) ‖ rowno` — by calling sm2's encoder
  with the same per-column arguments. Two encoders that have
  to agree bytewise is a corruption bug waiting to be
  written, so there is only one.
- **sm3 may call sm1 as well as sm2.** sm3 is built on sm2's
  tuple relation and never encodes a key or decodes a tuple
  itself, but it may call sm1's interface directly for
  anything sm2 has nothing to add to — `sm1::lock` above all,
  which sm3's DDL and index-build protocols rest on
  (`sm3/SM3_DESIGN.md` §2). sm2 still calls only sm1, and
  nothing calls upward.
- **A table is a heap keyed by `rowno`, and the primary key
  is an index.** A row's key is an unsigned 64-bit `rowno`
  from a per-table sm1 sequence, never reused and unchanged by
  `UPDATE` (`sm2/SM2_DESIGN.md` §5). Every index, the primary
  key's included, is sm3's and points at `rowno`
  (`sm3/SM3_DESIGN.md` §3). The fork redefines `ctid` as a
  `uint64_t`, so `rowno` is `ctid` — a core patch to
  PostgreSQL's 6-byte `ItemPointerData` (`PART4.md` §3). This
  matches the heap PostgreSQL's planner and executor assume,
  serves tables without a primary key, and keeps a primary
  key change from rewriting every index; a primary key lookup
  costs two reads.
- **One sm1 store per table.** A table's heap, all its
  indexes and its TOAST data are rels in one sm1 store, told
  apart by a 2-byte relid whose values `sm3/SM3_DESIGN.md` §4
  fixes. sm3 chooses an index's relid; sm2 prepends and strips
  it and never interprets it (`sm2/SM2_DESIGN.md` §5), so sm2
  never learns what a table is. `DROP TABLE` is one
  `sm1::rmstore`, sm1's O(1) column family drop; `DROP INDEX`
  is a range delete over the index's relid, and index relids
  are never reused. The cost is
  that tables, partitions included, are bounded by the
  hundreds to low thousands of stores sm1 handles
  (`sm1/SM1_DESIGN.md` §13); accepted for v1 (`sm3/SM3_DESIGN.md`
  §4).
- **A temporary table is a temporary store.** Temporariness
  belongs to an sm1 store, and so to its RocksDB column family,
  not to any row, key or relid: every object in a
  temporary store is temporary with it. Because a table's heap,
  its indexes and its TOAST data share one store (above), a
  temporary table is one temporary store, and an index is
  temporary exactly when its table is. sm3 asks for a temporary
  store when it creates a temporary table; sm2 neither knows
  nor cares (`sm1/SM1_DESIGN.md` §11.3).
- **The key budget is shared.** `sm1::max_keylen` (4096) bounds
  relid, index columns and `rowno` together. sm2's encoder
  enforces it: an over-long key in a unique index, the primary
  key's among them, refuses the row, and an over-long key in a
  non-unique index is truncated, flagged and rechecked against
  the tuple, never hashed (`sm2/SM2_DESIGN.md` §3.2). sm3 owns
  the recheck that follows from a cut key
  (`sm3/SM3_DESIGN.md` §3).
- **Uniqueness is sm1's `SM_ERR_DUPKEY`, and NULLs are
  distinct.** Two NULLs encode to the same bytes, so the
  encoding cannot say they differ; an entry with a NULL key
  column carries the `rowno` suffix and is never a duplicate,
  as SQL requires by default. No level makes a uniqueness
  check of its own. `NULLS NOT DISTINCT` is refused at
  `CREATE INDEX` in v1; adding it
  later is a catalog flag and changes no bytes on disk
  (`sm3/SM3_DESIGN.md` §3).
- **Collation versions are pinned, recorded and enforced across
  the stack.** A specific ICU is linked into the server; sm3
  records each collation's version with the schema
  (`sm3/SM3_DESIGN.md` §2); sm2 refuses to read or write an index
  built under another version, the primary key's included, until
  an explicit online rebuild (`sm2/SM2_DESIGN.md` §3.3).
  Non-deterministic ICU collations are accepted, and a string
  equal under one is equal at every level
  (`sm2/SM2_DESIGN.md` §3.3).
- **`NOT NULL` and labels are sm3's; sm2 stores what it is
  given.** sm3 refuses a NULL in a `NOT NULL` column with
  `SM_ERR_NOTNULL`, before the row is written: the check is
  exact and costs a flag a column, and sm3 itself leans on it,
  since a primary key column is taken never to be NULL. The
  `varchar(n)` limit is not sm3's, nor any level's. It is a
  coercion and not a constraint: PostgreSQL raises an error
  for a value too long unless the excess is all spaces, which
  it trims, and it counts characters. That is evaluation, and
  evaluation is the host's (below); the stack keeps the typmod
  and stores what it is given. An enum is `int4` to sm2 with
  its labels in sm3's catalog (`sm2/SM2_DESIGN.md` §2).
- **Foreign keys are the host's.** PostgreSQL checks one from
  triggers its executor runs: it looks the parent row up and
  locks it, and on a change to the parent looks for children.
  Those are queries, and the stack runs none. Its share is the
  lock: a row locked without being written, in a mode a
  reader of the key can share and a writer cannot, held to
  the end of the transaction. sm1 has it, `sm1::lock`; sm2
  has it on a heap row, `rel_lock`; sm3 hands it out on an
  open table, `tbl_lock`, and the C API as `sm_tbl_lock`. It
  is what `SELECT … FOR SHARE` and `FOR UPDATE` take too. It
  waits as a write does; `NOWAIT` and `SKIP LOCKED` are not
  on it yet. The lock is coarser than PostgreSQL's:
  sm1 has no mode that admits an update of the parent's other
  columns while a child holds its key, as `FOR KEY SHARE`
  does against `FOR NO KEY UPDATE`, so such an update waits
  for the child's transaction.
- **The API is tuple at a time, at every level.** Sets of
  rows in and out was forced by an IPC boundary between a
  PostgreSQL backend process and sm1; the fork now moves
  PostgreSQL to threads (`PART4.md` §2), so the boundary is
  a function call and the shape is no longer forced. Its other
  reason was filter pushdown, and sm3 filters nothing (above).
  Fewer calls per scan is what is left of the case for
  batches, and it waits for a profile that asks
  (`sm3/SM3_DESIGN.md` §5).
- **READ COMMITTED writes wait, then proceed.** PostgreSQL's
  default level does not fail a concurrent update of the same
  row, so sm1's first-committer-wins rule is relaxed there
  (`sm1/SM1_DESIGN.md` §6.2). It needs a new version ordering in
  sm1 and a row re-check in the executor.
- **One commit point.** The catalog lives in sm3, in memory
  and written through to sm2, so a DDL statement and the data
  it changes commit in one sm1 transaction, and recovery is
  RocksDB's alone (`PART4.md` §4, `sm3/SM3_DESIGN.md` §2).
- **READ COMMITTED writes are resolved above the stack, and
  sm3 fixes the indexes.** `SM_ERR_UPDATED` is answered by
  re-checking the statement's filter against the newest image.
  The filter is the executor's and may span tables, so the
  executor reads that image, re-checks (`EvalPlanQual`), and
  repeats the write or skips the row. sm2 and sm3 surface the
  error and offer the read. sm3's share is to take the index
  entries to remove from the old image the repeated write
  hands back (`sm3/SM3_DESIGN.md` §3).
- **sm3 drives index maintenance.** PostgreSQL's kept executor
  could maintain indexes itself through its index access
  method (`ExecInsertIndexTuples`). It does not: it writes a
  row through one table-level call that sm3 owns, `sm_tbl_*`,
  and the C wrapper's heap-level and index-level calls are
  gone (`sm3/SM3_DESIGN.md` §3). The rules of a unique index's
  key, the index build and the index-heap invariant must
  stay below the executor, and the invariant holds only if one
  layer writes both a row and its entries.
- **Evaluation is the host's, by callback.** No level of the
  stack evaluates a predicate or an expression. Where sm3
  needs one evaluated, to maintain or build a partial or an
  expression index, it calls a callback the host registers:
  given a row image, whether the row belongs in the index, and
  its key values. The callback must not `longjmp` out or throw
  through sm3's frames, reports failure by return code, is
  deterministic, and may be called from sm3's build workers.
  The fork is threaded and rewrites PostgreSQL's functions and
  operators for threads, so those rules are its to keep as it
  ports them, and the results are PostgreSQL's by
  construction. A scan takes key bounds and filters nothing:
  deriving bounds from a `WHERE` clause and testing the
  residual are the planner's and the executor's
  (`sm3/SM3_DESIGN.md` §3, §5).
- **A write that fails partway is the caller's to undo.** A
  table write is the heap write and then a write or two per
  index, and sm3 sets no savepoint inside it. One that fails
  on a later step, a duplicate in a unique index for one, has
  left the earlier steps done. The caller rolls back to a
  savepoint of its own or aborts, as PostgreSQL's errors abort
  the transaction or the subtransaction anyway; `rel` aborts.
  `SM_ERR_UPDATED` needs no undoing: it comes from the heap
  write, the first step (`sm3/SM3_DESIGN.md` §3).
- **An index is built in the background.** An index made on a
  table that has rows is recorded as pending, maintained by
  every writer from then on, and filled by the process's
  builder, which the C wrapper attaches a database's catalog
  to when it opens the database.
  A reader may not scan it until it is valid, nor if its
  snapshot is older than the build; `sm_cat_index_wait` is for
  a caller that wants it at once. A failed build keeps its
  reason in the index's record (`sm3/SM3_DESIGN.md` §3).
- **No backward scans, at any level, ever.** Settled in
  `sm1/SM1_OBJECTIVE.md`, built into sm2's encoding and
  inherited by every index sm3 builds. The fork's planner is
  changed never to ask for a backward scan (`PART4.md` §5);
  descending order comes from inverted key bytes, scanned
  forward.

---

## 6. What this document is not

It is not a substitute for `sm1/SM1_DESIGN.md`,
`sm2/SM2_DESIGN.md` or `sm3/SM3_DESIGN.md` — read those for how a
level works. It is
not a restatement of `PART4.md` — read that for the
PostgreSQL target in full. It exists so a reader can see the
whole stack, and the handful of decisions that bind it
together, without first reading either level end to end.
