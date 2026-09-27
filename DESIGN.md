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
codes, and `sm_options.h`, sm1's database options. They are
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
| sm3 | being built: its catalog — keys, values, load, DDL, a session's overlay and schema locks — with its C++ interface in `sm3/include/sm3.h` and `sm3/CATALOG.md`; index maintenance, predicate scans and statistics are still design, in `sm3/SM3_DESIGN.md` and `sm3/PLAN.md` |
| top level | built: `include/sm.h` over sm1 and sm2, with `src/sm.cc`; the calls that open a table are scaffolding until the wrapper serves sm3's catalog (§1), and the `rel` driver keeps a catalog of its own until then |

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
writer, DDL against DML. sm2's testing (`sm2/SM2_DESIGN.md`
§8.3) and `sm3/PLAN.md` reuse this scheme rather than invent a
second, and defer only the question of
whether a level needs a driver of its own or can extend
sm1's — "a decision for the first scenario that needs it,
not before."

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
  | `KEYTOOLONG`, `ROWTOOLONG` | `54000` | `program_limit_exceeded` |
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
- **One sm1 store per table.** A table's rows, all its
  indexes and its TOAST data share one sm1 store,
  told apart by a 2-byte object prefix: `0x0000` for rows,
  `0x0001`–`0xFFFE` for indexes, `0xFFFF` for TOAST. sm3
  chooses the prefix; sm2 prepends and strips it and never
  interprets it (`sm2/SM2_DESIGN.md` §5), so sm2 never learns
  what a table is. `DROP TABLE` is one `sm1::rmstore`, sm1's O(1)
  column family drop; `DROP INDEX` is a range delete over its
  prefix, and index prefixes are never reused. The cost is
  that tables, partitions included, are bounded by the
  hundreds to low thousands of stores sm1 handles
  (`sm1/SM1_DESIGN.md` §13); accepted for v1 (`sm3/SM3_DESIGN.md`
  §4).
- **A temporary table is a temporary store.** Temporariness
  belongs to an sm1 store, and so to its RocksDB column family,
  not to any row, key or object prefix: every object in a
  temporary store is temporary with it. Because a table's heap,
  its indexes and its TOAST data share one store (above), a
  temporary table is one temporary store, and an index is
  temporary exactly when its table is. sm3 asks for a temporary
  store when it creates a temporary table; sm2 neither knows
  nor cares (`sm1/SM1_DESIGN.md` §11.3).
- **The key budget is shared.** `sm1::max_keylen` (4096) bounds
  prefix, index columns and `rowno` together. sm2's encoder
  enforces it: an over-long primary key refuses the row, and an
  over-long index key is truncated, flagged and rechecked
  against the tuple, never hashed (`sm2/SM2_DESIGN.md` §3.2). sm3
  owns the recheck and the unique check that follow from a cut
  key (`sm3/SM3_DESIGN.md` §3).
- **Unique checks are sm3's, and NULLs are distinct in them.**
  Two NULLs encode to the same bytes, so the encoding cannot
  say they differ; sm3's unique check skips any entry with a
  NULL key column, as SQL requires by default. `NULLS NOT
  DISTINCT` is refused at `CREATE INDEX` in v1; adding it
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
- **Constraints and labels are sm3's; sm2 stores what it is
  given.** The `varchar(n)` length limit is enforced by sm3,
  and an enum is `int4` to sm2 with its labels in sm3's
  catalog (`sm2/SM2_DESIGN.md` §2).
- **Batch-shaped API: a question for sm3, not sm2.** Sets of
  rows in and out was forced by an IPC boundary between a
  PostgreSQL backend process and sm1; the fork now moves
  PostgreSQL to threads (`PART4.md` §2), so the boundary is
  a function call and the shape is no longer forced. sm2 is
  tuple-at-a-time (`sm2/SM2_OBJECTIVE.md`); whether sm3 batches
  above it, for filter pushdown and fewer calls per scan, is
  open and belongs where the filters are
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
- **READ COMMITTED writes are resolved at sm3, not sm2.**
  `SM_ERR_UPDATED` is answered by re-checking the statement's
  filter against the newest image, and filters exist only at sm3.
  sm2 surfaces the error and offers `rel_read_latest`; sm3 does
  the re-check (`sm3/SM3_DESIGN.md` §3).
- **Open: who drives index maintenance.** PostgreSQL's kept
  executor already maintains indexes through its index access
  method (`ExecInsertIndexTuples`), evaluates expression and
  partial index keys, and re-checks a row under READ COMMITTED
  (`EvalPlanQual`) — the work `sm3/SM3_DESIGN.md` §3 plans for
  sm3. Expression and partial indexes favour the executor:
  sm3 cannot evaluate a PostgreSQL expression without calling
  back into C that may `longjmp` across its frames. The unique
  checks on NULL and cut keys, the online build and the
  index-heap invariant must stay below it. The C wrapper's row
  and entry calls have the shape either answer needs, so it is
  settled with sm3's design, not before.
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
