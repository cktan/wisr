# DESIGN — wisr

This document says how the stack in
[`OBJECTIVE.md`](OBJECTIVE.md) fits together: the levels, the
build, and the decisions that cross level boundaries. Each
level's own mechanics, such as sm1's MVCC, sm2's tuple format and
sm3's catalog, are argued in that level's `SMn_DESIGN.md`. This
document does not repeat them.

---

## 1. The stack

```
             digres (PostgreSQL compiled as C++)
                             │  include/ — the one C-style API
        ┌────────────────────┴─────────────────────────┐
        │  top level  the C wrapper                    │
        │  everything → sm3, but the rest of 2PC,      │
        │  stores and sequences → sm1                  │
        └────────────────────┬─────────────────────────┘
                             │  sm3::
        ┌────────────────────┴─────────────────────────┐
        │  sm3/  schema and index maintenance          │
        │  catalog · index maintenance ·               │
        │  scan-by-predicate · statistics              │
        └────────────────────┬─────────────────────────┘
                             │  sm2::  row and index relations
                             │         (sm3 also calls sm1::)
        ┌────────────────────┴─────────────────────────┐
        │  sm2/  types and tuples                      │
        │  type system · key encoding ·                │
        │  tuple format · row and index relations      │
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

**The store.** sm1's unit of storage is the *store*: an ordered
map from byte keys to byte values, named by a `uint64_t`
storeid.

- **A store is exactly one RocksDB column family.** A versioned
  store is `CF:store.<storeid>`, and a chained store is
  `CF:chain.<storeid>`
  ([`DBMGR.md`](DBMGR.md#62-key-classes) §6.2,
  [`ROWSTORE.md`](ROWSTORE.md#35-a-chained-key) §3.5).
  `sm1::mkstore` creates a store, and `sm1::rmstore` drops it.
- **sm3 makes every table's store chained.** A row's newest
  version is kept under its plain key, and the older versions in
  a chain behind it. The table's index entries and TOAST stay
  versioned.
- **What RocksDB offers for each column family, sm1 offers for
  each store**: an O(1) drop, and temporariness (§5). No level
  above sm1 sees a column family by that name.

**How the levels call each other.**

- **sm2 is a client of sm1, nothing more.** It holds no RocksDB
  handle of its own, and reaches storage only through sm1's
  interface.
- **sm3 is built on sm2.** It never encodes a key or decodes a
  tuple itself. It may also call sm1's interface directly, for
  anything that sm2 adds nothing to. Above all, that is
  `sm1::lock_oid`, which sm3's DDL and index-build protocols are
  built on ([`CATALOG.md`](CATALOG.md#1-design) §1).
- **Nothing calls upward.** sm2 calls only sm1.

This boundary is why each level's [`OBJECTIVE.md`](OBJECTIVE.md)
can promise that the level is "useful and testable on its own":
the whole stack below it is one dependency.

**The top level is not a level.** It adds no notion of its own,
only C-style signatures over the levels' C++ interfaces. It is
the one place where an exception is caught, and where a
PostgreSQL error path is safe to take
([`OBJECTIVE.md`](OBJECTIVE.md)). Like sm3, it may skip a level:

- A C session is an sm3 session over an sm1 session. A
  database's first session loads its sm3 catalog.
- Sessions, transactions, savepoints and prepare go through
  sm3, which keeps its overlay of DDL in step with them.
- The rest of 2PC, and stores and sequences, go to sm1 directly.

A caller uses a table in three steps:

1. Find the table through sm3's catalog (`sm_cat_*`).
2. Open it by its oid, with the lock it asks for
   (`sm_tbl_open`).
3. Read and write its rows through the open table (`sm_tbl_*`).
   sm3 serves these calls, and maintains the table's indexes
   with each write (§3).

**sm0, level 0, sits under every level.** It is outside the
diagram's chain of calls, because every level uses it. It holds:

- the calling thread's error state, and the log hook;
- general-purpose helpers that are about no storage: formatting
  into a fixed buffer, defer blocks, big-endian integers;
- the system services, `sm_sys.h`, which dg also includes
  directly.

sm0 includes nothing of the levels, and links no RocksDB. sm1
brings the error and log calls into namespace sm1. So the stack
and its callers name them `sm1::errnum()`, `sm1::set_log()` and
so on ([`sm0/README.md`](sm0/README.md)).

**Shared headers.** The root's `include/` also holds headers that
belong to no level, and that a level includes:

| Header | What it defines | Language |
| --- | --- | --- |
| `sm_error.h` | the stack's error codes; sm0 includes it too | C |
| `sm_lock.h` | the lock modes | C |
| `sm_options.h` | sm1's database options | C |
| `sm_stats.h` | a table's statistics; sm3 includes it | C++ |

These are shared definitions, not an upward call. A level and the
wrapper must agree on them bytewise, and one header is how. For
the statistics, it also means that the wrapper hands out sm3's
own statistics, as sm3 keeps them in memory, with no copy in a
form of its own ([`OBJECTIVE.md`](OBJECTIVE.md)).

---

## 2. Build

Each level has a real `Makefile`. The top-level `Makefile`
delegates to them. It also builds the one thing that belongs to
no level: the C wrapper, in the root's `include/` and `src/`.

The wrapper goes into `build/libsm.a`. An `ar` script bundles
every level's objects with the wrapper's into that one archive.
So PostgreSQL links one archive, plus RocksDB, ICU and the C++
runtime, and never learns that the stack has levels.

The test driver in `rel/` drives the C wrapper. It tests the
relational engine against `libsm.a`, with Janet test scripts.

```
make -j4            build every level
make -j4 test       build and test every level
make -C sm1 -j4     build level 1 alone
```

`LEVELS` in the top-level `Makefile` lists the levels that have a
Makefile to delegate to, in build order: `sm0 sm1 sm2 sm3`. To
add a level to the stack, add its name there, and nowhere else.

**Never compile with `-fno-exceptions`**, at any level or in the
C wrapper. An exception may unwind through sm1, sm2 and sm3 to
the wrapper's catch-all (§5). The flag would break this in two
ways:

- The wrapper needs `try` and `catch`, which the flag forbids.
- Each level needs the cleanup code that runs destructors during
  unwinding, which the flag omits. Without it, an RAII lock
  guard never releases its mutex, or the process terminates.

The flag also does not stop anything from being thrown: the
standard library throws from its own compiled code, whatever the
flag says. GCC enables exceptions for C++ by default. This rule
says that nothing turns them off.

---

## 3. Status

| Level | Status |
| --- | --- |
| sm0 | built and tested: the error state, the log hook, the general-purpose helpers and the system services (`sm0/include/sm_sys.h`, [`sm0/INTERRUPT.md`](sm0/INTERRUPT.md)). Its C++ interface is `sm0/include/sm0.h`. [`sm0/README.md`](sm0/README.md) describes it |
| sm1 | built and tested. Its C++ interface is `sm1/include/sm1.h`. The design of record is [`sm1/SM1_DESIGN.md`](sm1/SM1_DESIGN.md) |
| sm2 | built: its types, key encoding, tuple codec, row and index relations, and TOAST. Its C++ interface is `sm2/include/sm2.h`. The design of record is [`sm2/SM2_DESIGN.md`](sm2/SM2_DESIGN.md) |
| sm3 | being built. Built: its catalog ([`CATALOG.md`](CATALOG.md)), object mapping, index maintenance through table-level writes, and the concurrent index build with its background workers, including partial and expression indexes; statistics ([`STATISTICS.md`](STATISTICS.md#9-the-design) §9); TOAST (§3.4 there); and bulk load into an empty table ([`BULKLOAD.md`](BULKLOAD.md)). Constraints are settled: sm3 enforces `NOT NULL`, and the host the rest (§5). Its C++ interface is `sm3/include/sm3.h`. The order of work is in [`sm3/SM3_DESIGN.md`](sm3/SM3_DESIGN.md#4-structure) §4, and what is left in [`NEXT.md`](NEXT.md) |
| top level | built: `include/sm.h` over sm1, sm2 and sm3, with `src/sm.cc`. A row is read and written through an open table (`sm_tbl_*`), found by the table's oid from `sm_cat_*`. The heap-level and index-level calls that took a store id and a relid are gone. The `rel` driver runs on it |

**Why sm1 was built first, and alone.** sm1 is usable without
sm2: an MVCC key-value store is a complete product by itself.
Also, every sm2 decision needs sm1's real API and real behavior
to check against, not a sketch of them.

**Why sm2 and sm3 are split.** sm2 is two permanent on-disk
formats, and a codec over them. sm3 is protocols, which can be
rewritten. Keeping the permanent part small lets it get all the
scrutiny ([`sm2/SM2_DESIGN.md`](sm2/SM2_DESIGN.md#4-structure)
§4).

---

## 4. Testing across levels

Every level uses the same two-layer scheme, first set up in
[`sm1/mvcc/DRIVER.md`](sm1/mvcc/DRIVER.md):

- **Ordinary tests**, for the contract and for the mechanics of
  one session: C++ unit tests, in each level's `test/`.
- **A declarative scenario suite**, for any case where two
  sessions interleave. Isolation and concurrency questions are
  hard to read as imperative test code.

Most of sm2 needs the first kind: a codec is tested by its
properties, not by interleaving. Most of the second kind arrives
with sm3: an index build against a concurrent writer, or DDL
against DML.

**sm3 has no scenario driver of its own, and does not extend
sm1's.** sm1's driver has one verb for each sm1 call, and links
sm1 alone. Teaching it `create table` would make sm1's test
binary link the levels above sm1.

Instead, sm1 gained a session flag for QA only,
`sm1::set_qa_nowait`
([`LOCKMGR.md`](LOCKMGR.md#32-the-lock-table) §3.2):

1. A write or a lock that would wait fails with
   `SM_ERR_BLOCKED`, and leaves the transaction active.
2. The caller lets the other session go on.
3. The caller repeats the call.

So one thread can act as several sessions, and "this step
blocks" needs no thread and no sleep. sm3's cases with two
sessions are written this way in C++ (`sm3/test/test_table.cc`).
They are written again through the C API, as `rel` scripts
([`rel/REL_DESIGN.md`](rel/REL_DESIGN.md) §5).

The flag cannot reach two things, which are tested with real
threads instead:

- A session that polls is never a deadlock victim. So deadlocks
  are tested with real threads in sm1.
- The background index build is tested with real threads in
  sm3.

---

## 5. Cross-level decisions

These decisions constrain more than one level. They are
gathered here, so that a change to one level does not silently
break another level's design.

- **sm1's interface is the only way into sm1.** Nothing above
  sm1 may read RocksDB directly, or depend on sm1's internals. A
  missing primitive is added to sm1's interface, not worked
  around from outside it. The primitives that secondary indexes
  needed were added this way ([`DBMGR.md`](DBMGR.md#1-scope)
  §1).
- **Every level reports failure the way sm1 does.**
  - *The return value.* A call on a level's C++ interface
    returns a plain `int`: 0 on success, -1 on failure. The
    caller then reads the code and the message from the
    thread-local `sm1::errnum()` and `sm1::errmsg()`. A result
    comes back through an out-parameter.
  - *Where the calls come from.* Every error call is sm0's.
    `sm1.h` brings them into `namespace sm1`. The code accessor
    is `errnum`, not `errno`, because `errno` is a macro.
  - *No status type.* There is no status type and no
    `std::expected`, so the build stays at `-std=c++20`. This
    keeps sm1's message free of allocation, and its per-thread
    state. The C wrapper passes the convention through unchanged.
    The cost is that nothing stops a caller from ignoring a -1.
  - *One errno namespace for the whole stack.* The codes are
    `SM_ERR_*` and `SM_OK`, values of the enum `sm1::errnum_t`.
    They are the stack's codes, not sm1's alone, so their names
    carry no `1`. sm2 and sm3 report through the same
    thread-local state. Every code that any level uses is defined
    in the root's `include/sm_error.h`. This C header is included
    by sm1, and `sm1.h` brings it into `namespace sm1`. So the
    list holds codes that only an upper level produces, and the
    C wrapper passes one enum through unchanged.
  - *The enum is append only.* A number is never reused or
    renumbered, and a retired code stays, with a note. Codes are
    grouped by the level that produces them, under a comment that
    names the level, with no reserved numeric ranges. A number
    matters once PostgreSQL sees it through the C wrapper.
    [`sm2/TYPE.md`](sm2/TYPE.md) applies the same rule to type
    codes.
  - *Setting an error.* An upper level records an error with
    `int sm1::seterr(sm1::errnum_t code, std::string_view msg)`,
    which sm1 exports. It copies the message into the fixed
    thread-local buffer, cut short if it does not fit, and
    returns -1. So a failing path is `return sm1::seterr(...);`.
    The caller formats the message, which keeps templates and
    `<format>` out of sm1's public header. A report of running
    out of memory passes a literal.
  - *What an error holds.* An error holds a code and a message,
    nothing else. Whether it is retryable depends only on the
    code, and `sm1::retryable()` computes it. The message stays
    in the fixed thread-local buffer, with no allocation, so a
    report of running out of memory cannot itself fail. No
    SQLSTATE is stored: the fork's glue chooses it (below).
- **Two more calls sit beside `sm1::errnum()` and
  `sm1::errmsg()`.**
  - `void sm1::reseterr()` clears the calling thread's state back
    to what a thread starts with: `SM_OK`, and the message "no
    error". A successful call leaves the state as it was, so this
    is the one way to start a sequence of calls from a known
    state.
  - `const char* sm1::errname()` names the current code: the
    enumerator without its `SM_` prefix. So `SM_ERR_UPDATED`
    gives `"ERR_UPDATED"`, and `SM_OK` gives `"OK"`. The string
    is static, and never NULL. With one errno namespace, every
    level's codes are named here, so a new code is not finished
    until it has its name. The scenario driver keeps its own
    short names, so no transcript changes.
- **Only "no row" is a positive return; every other outcome is
  -1.** A read, and `sm1::scannxt`, return 1 for "no row".
  `DUPKEY` on insert, `NOTFOUND` on update or delete, and
  `UPDATED` are -1, with the code in `sm1::errnum()`, like any
  failure. A caller that treats one of them as an ordinary
  outcome reads the code. With an `int` and a thread-local,
  reading the code costs nothing, so there is no performance
  reason to split outcomes from errors. This also keeps
  [`sm1/API.md`](sm1/API.md), the tests and the error names in
  the scenario transcripts as they are.
- **Out-parameters have one shape at every level.**
  - A required output is a reference (`uint64_t& storeid`).
  - An optional output stays a pointer, where `nullptr` means
    "not wanted": for example, the old image from an update or a
    delete.
  - A list is a `std::vector<T>&`. This replaced sm1's C string
    list and `malloc`'d arrays.
  - Bytes that point into engine memory are a
    `std::string_view&`, valid until the next call on the
    session.
- **The top-level API is C++ in a C style.** Everything that
  includes `include/sm.h` compiles it as C++: the fork's glue is
  a C++ file, as `rel` is.
  - *C in shape.* The API has free functions named `sm_*`, opaque
    handles, an `int` result with the thread-local error, and
    callbacks that return a code and never throw.
  - *Callbacks.* The two callbacks, the log hook and the index
    evaluator, run where an error has nowhere to go. Their types
    are `noexcept`, so a callback that throws ends the process.
  - *Four C++ types cross the API, and no others*
    ([`CODE.md`](CODE.md#2-the-interface-to-sms-users) §2):
    - `std::string` and `std::vector` carry owned data only. A
      vector holds plain C types, `sm_datum_t` or strings. The
      data is a result that the caller keeps, or a list passed
      in, as an out-parameter or a const reference. It is filled
      inside the wrapper's catch-all.
    - `std::string_view` is bytes that the receiver does not own.
      They are read during the call, or point into session memory
      for as long as the call says. No call takes a
      `std::string_view` yet: a row or an index entry on a scan
      is still a pointer and a length into session memory.
    - `std::shared_ptr` is for an object whose life the stack and
      the host share. It points to a `const` object: a table's
      statistics, as sm3 keeps them (`sm_tbl_stats`).
  - *What is not allowed.* No `optional` or `function`, and no
    class of the stack's own. That line keeps the header from
    growing abstractions. A plain struct may hold the first two
    types, and a vector may hold such structs. A table's
    definition is one: a struct of its columns and its indexes.
  - *Waits.* The host ends a wait by an interrupt to its thread
    ([`sm0/INTERRUPT.md`](sm0/INTERRUPT.md)), not by a flag or a
    callback into the host.
  - *What this replaced.* To stay C, the header used to have
    lists as one `malloc`'d block freed by hand, owned results
    that lived "until the next call", and a C mirror of each
    table definition: seven parallel arrays that the wrapper
    built and kept.
  - *The costs.* No plain C program and no foreign-function
    binding can include the header. Both sides must be built with
    one C++ runtime, as they are while the stack is linked from
    source into one archive.

  It is still called the C wrapper and the C API, below and in
  other documents.
- **The C wrapper keeps the convention; the fork's glue calls
  `ereport`.** The wrapper's `sm_*` calls return 0 or -1, and
  leave the code and message in the same thread-local state.
  - *Why the glue reports.* Inside a PostgreSQL backend, a
    failure never travels back up as a return value. The access
    method callbacks return `void` or "got a row", and the
    executor has no error path. So the fork's glue turns a -1
    into `ereport(ERROR, ...)`. That aborts the transaction,
    releases what the statement held, and sends the client an
    `ErrorResponse`.
  - *Why the wrapper does not.* The wrapper never calls
    `ereport` itself. `sm.h` is a C-style API: no error leaves it
    except as a return code (below). So:
    1. The wrapper catches everything, and returns -1.
    2. Control comes back to the glue, with no wrapper frame on
       the stack.
    3. Only then does the glue report.
  - *Not every -1 is reported.* Under READ COMMITTED, `UPDATED`
    sends the executor to check the row again. After a retryable
    code, the engine has already aborted the transaction, so the
    wrapper must accept an abort of a transaction that is
    already gone. So the glue switches on the code first.
  - *The SQLSTATE.* The glue's switch chooses the SQLSTATE, from
    PostgreSQL's own `utils/errcodes.h`. No SQLSTATE lives in the
    stack. It is a SQL notion, and sm1 is meant to be useful
    without PostgreSQL. Also, some mappings depend on context
    that only the glue has. `DUPKEY` is `unique_violation` from a
    unique index or a primary key, but a bug from a fresh `_id`.

  The glue's mapping starts from:

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
- **Running out of memory is fatal to the process, because
  `std::bad_alloc` can be thrown from inside RocksDB.**

  *The failure.* RocksDB allocates as it works: a write builds a
  batch, joins a write group, appends to the WAL, and inserts
  into the memtable. When an allocation fails, `operator new`
  throws through RocksDB's own frames. RocksDB reports its
  errors through `Status`, and is not written to be unwound
  through. It keeps parts of its state consistent by explicit
  steps, not by RAII, and an exception skips those steps. One
  test failed each allocation inside the write of a status
  record, in turn. The outcome of the write was unknown: the
  record reached the WAL or not, by where the exception came
  from. Every later write to that database hung, and closing
  the database could crash.

  *The policy.* No caller can repair that state in the process.
  No caller can tell such a `bad_alloc` from a harmless one,
  either. An exception from inside RocksDB looks the same as
  one from sm1's own code, or from RocksDB before it changed
  anything. So every `bad_alloc` is fatal. Only the WAL replay
  of a new open recovers, and it recovers every committed
  transaction reliably.

  `sm1::attach` installs a new-handler, unless the host process
  already has one. The handler runs when an allocation fails,
  before anything is thrown. It reports `*** FATAL ***` on
  stderr and through the log hook. Then it aborts where the
  allocation failed, before any frame unwinds, so the core
  holds the stack of the failed allocation. It does this for
  every allocator: sm1, sm2, sm3, the wrapper, or the host. To
  tell them apart would need a guard on every sm1 entry point.
  That guard would give only a `NOMEM` from the levels above,
  which is almost never seen. RocksDB's background threads run
  under the same handler.

  This failure is very rare. On a default Linux system an
  allocation seldom fails, because the kernel's OOM killer ends
  the process first. It takes a hard memory limit to get here.

  *A host's own handler.* A host with its own new-handler keeps
  it. A handler that throws is not supported. sm does not
  recover from a `bad_alloc` inside it, so RocksDB can become
  unreliable and sm1's own state can be half changed.
- **sm never throws; an exception may pass through it, and only
  the top level catches.**

  *sm throws nothing of its own.* A failure is -1 and an error
  code, and there is no `throw` in sm's sources. An exception
  may still arise inside sm:
  - from the C++ library, such as a `std::system_error` when a
    thread cannot be started;
  - as a `std::bad_alloc`, before `sm1::attach` installs the
    handler (above);
  - from a callback that sm invokes, whose contract allows one.

  *It passes through the levels.* The exception passes through
  sm1, sm2 and sm3 unchanged: their entry points catch nothing.
  What a frame must release on the way is held by an RAII guard,
  such as sm0's `scope_guard_t`, never by a catch and rethrow.

  *The wrapper catches it.* The C wrapper is the one catch-all,
  because every call from a host crosses it. A host expects a
  return code there, not an exception: digres catches only its
  own `pg_error_t`. The wrapper catches everything:
  - `std::bad_alloc` becomes `NOMEM`. This is possible there only
    before the handler is installed, or under a host's own
    handler.
  - Anything else becomes `SM_ERR_EXCEPTION`, which means a bug.
    Its message is `what()`.
  - If the call involves a session, the wrapper aborts that
    session's transaction before it returns -1. The exception is
    a prepared transaction, which `sm1::in_doubt` reports. That
    transaction is the coordinator's to resolve, and aborting it
    would break the promise that `sm1::prepare` made.
  - A `NOMEM` returned from below is handled the same way. The
    wrapper aborts the session's transaction, because a data
    operation that ran out of memory midway may have written part
    of its change.

  *Why an abort is the right recovery.* sm1's abort works only in
  memory. It leaves the active table, which kills the
  transaction's versions without writing a status record. It
  also releases the transaction's cursors, SSI node, created
  stores and locks. So the abort scarcely allocates, and it
  returns whatever a half-done call touched to a known state.
  Every sm1 mutex is held by an RAII guard, so unwinding releases
  it.

  *Four conditions come with this design:*
  1. Nothing may throw after a commit point or a prepare point.
     Once the status record or the `PREPARED` record is written,
     an abort would kill, in memory, versions that the WAL says
     are committed. Nothing does throw there: that tail is
     `noexcept`. Running out of memory in it aborts the process,
     as anywhere, and a failed write or sync of the status record
     aborts the process.
  2. sm2 and sm3 must stay consistent when unwound mid-call. So
     the abort undoes what a half-done call changed, above all in
     sm3's in-memory catalog.
  3. After `NOMEM`, as after a retryable code, the engine has
     already aborted the transaction. So the glue does not
     continue it. `NOMEM` is not retryable.
  4. An exception on the GC thread, or inside a RocksDB callback,
     never reaches the wrapper. So those boundaries keep
     catch-alls of their own (next).

  Exceptions stay enabled in every build (§2).
- **The foreign boundaries report errors, and catch no
  exception.** The RocksDB callbacks (the compaction filter, the
  comparator, the prefix extractor) and the GC thread have no
  caller to return -1 to.
  - The compaction filter keeps any version it has a doubt
    about, such as an undecodable value or a failed status
    lookup. It reports how many through the `sm1::set_log` hook,
    once each compaction. So a failure can never delete data.
  - The GC thread logs a failed pass, and tries again on its
    next cycle.
  - The comparator and the prefix extractor are pure `memcmp`
    and byte slicing, and cannot fail.

  None of them catches. Running out of memory aborts the process
  (above), and sm throws nothing of its own. So an exception that
  reached one of them would be a bug, and it ends the process
  through `std::terminate`.
- **One encoder, one tuple format, two callers.** The key
  encoding and the tuple layout are sm2's alone
  ([`LSMINDEX.md`](LSMINDEX.md) §2,
  [`sm2/TUPLE.md`](sm2/TUPLE.md#10-why-the-codec-is-an-api-of-its-own)
  §10). sm3 builds its index entries, `encode(sk)` ending with
  the row's key, by calling sm2's encoder with the same arguments
  for each column. Two encoders that must agree bytewise would be
  a corruption bug waiting to be written, so there is only one.
- **sm3 may call sm1 as well as sm2.** §1 describes this: sm3
  never encodes a key or decodes a tuple itself, but calls sm1
  directly for what sm2 adds nothing to, above all
  `sm1::lock_oid` ([`CATALOG.md`](CATALOG.md#1-design) §1). sm2
  still calls only sm1, and nothing calls upward.
- **A table is keyed by its primary key, or by `_id` without
  one** ([`CLUSTER.md`](CLUSTER.md),
  [`ROWSTORE.md`](ROWSTORE.md)).
  - A row's key is the encoding of its primary key.
  - A table without a primary key is keyed by `_id`: an unsigned
    64-bit number from a sequence of the table's own in sm1. An
    `_id` is never reused, and `UPDATE` does not change it. SQL
    reads it as the system column `_id`.
  - A table with a primary key has no `_id`, and no `_id` index
    ([`ROWSTORE.md`](ROWSTORE.md#8-the-tuple-relation) §8).
  - Every other index is sm3's. Its entry names its row by the
    row's key ([`INDEXBUILD.md`](INDEXBUILD.md#2-the-protocol)
    §2).
  - There is no `ctid`. Wherever PostgreSQL's executor carried a
    TID, digres names a row by the values of its key's columns
    ([`ROWSTORE.md`](ROWSTORE.md#10-digres) §10).
  - A read by the primary key is one read.
  - A change of primary key is a delete at the old key, which
    leaves a moved tombstone, and an insert at the new key. It
    also rewrites the row's index entries
    ([`ROWSTORE.md`](ROWSTORE.md#25-a-key-change) §2.5).
- **One sm1 store for each table.** A table's rows, all its
  indexes and its TOAST data are rels in one sm1 store. A 2-byte
  relid tells them apart, with the values that
  [`CATALOG.md`](CATALOG.md#2-object-mapping) §2 fixes.
  - sm3 chooses an index's relid. sm2 adds and removes the relid,
    but never interprets it
    ([`ROWSTORE.md`](ROWSTORE.md#8-the-tuple-relation) §8). So
    sm2 never learns what a table is.
  - `DROP TABLE` is one `sm1::rmstore`: sm1's O(1) drop of a
    column family.
  - `DROP INDEX` erases the entries under the index's relid, one
    by one, because sm1 has no range delete
    ([`CATALOG.md`](CATALOG.md#8-ddl) §8). Index relids are never
    reused.
  - The cost: the number of tables, partitions included, is
    limited by what sm1 handles, hundreds to low thousands of
    stores ([`XCTMGR.md`](XCTMGR.md#114-trade-offs) §11.4). This
    is accepted for v1
    ([`CATALOG.md`](CATALOG.md#2-object-mapping) §2).
- **A temporary table is a temporary store.** Temporariness
  belongs to an sm1 store, and so to its RocksDB column family.
  It does not belong to any row, key or relid: every object in a
  temporary store is temporary with it. A table's rows, its
  indexes and its TOAST data share one store (above). So a
  temporary table is one temporary store, and an index is
  temporary exactly when its table is. sm3 asks for a temporary
  store when it creates a temporary table. sm2 does not know
  about it ([`DBMGR.md`](DBMGR.md#78-temporary-stores) §7.8).
- **The key budget is shared.** `sm1::max_keylen` (4096) limits
  the relid, the index columns and the row's key together. A
  primary key is held to 1024 bytes. sm2's encoder enforces the
  limit:
  - an over-long key in a unique index, including the primary
    key, refuses the row;
  - an over-long key in a non-unique index is cut, flagged and
    checked again against the tuple. It is never hashed
    ([`LSMINDEX.md`](LSMINDEX.md#24-cut-keys) §2.4).

  sm3 owns the check that a cut key needs
  ([`INDEXBUILD.md`](INDEXBUILD.md#2-the-protocol) §2).
- **Uniqueness is sm1's `SM_ERR_DUPKEY`, and NULLs are
  distinct.** Two NULLs encode to the same bytes, so the encoding
  cannot say they differ. Instead, an entry with a NULL key
  column carries its row's key, so it is never a duplicate. This
  is what SQL requires by default. No level makes a uniqueness
  check of its own. `NULLS NOT DISTINCT` is refused at
  `CREATE INDEX` in v1. Adding it later is a catalog flag, and
  changes no bytes on disk
  ([`INDEXBUILD.md`](INDEXBUILD.md#2-the-protocol) §2).
- **Collation versions are pinned, recorded and enforced across
  the stack.**
  - A specific ICU is linked into the server.
  - sm3 records each collation's version with the schema
    ([`CATALOG.md`](CATALOG.md#1-design) §1).
  - sm2 refuses to read or write an index built under another
    version, including the primary key, until an explicit online
    rebuild ([`LSMINDEX.md`](LSMINDEX.md#23-text-and-collations)
    §2.3).
  - Non-deterministic ICU collations are accepted. A string that
    is equal under one is equal at every level
    ([`LSMINDEX.md`](LSMINDEX.md#23-text-and-collations) §2.3).
- **`NOT NULL` and labels are sm3's; sm2 stores what it is
  given.**
  - sm3 refuses a NULL in a `NOT NULL` column with
    `SM_ERR_NOTNULL`, before the row is written. The check is
    exact, and costs one flag for each column. sm3 itself relies
    on it, because it assumes a primary key column is never
    NULL.
  - The `varchar(n)` limit is not sm3's, nor any level's. It is a
    coercion, not a constraint. PostgreSQL raises an error for a
    value that is too long, unless the excess is all spaces,
    which it trims. And it counts characters. That is
    evaluation, and evaluation is the host's (below). The stack
    keeps the typmod, and stores what it is given.
  - An enum is `int4` to sm2, with its labels in sm3's catalog
    ([`sm2/TYPE.md`](sm2/TYPE.md#5-why-the-types-are-so) §5).
- **Foreign keys are the host's.** PostgreSQL checks a foreign
  key from triggers that its executor runs. A trigger looks up
  the parent row and locks it. On a change to the parent, it
  looks for children. Those are queries, and the stack runs
  none.
  - *The stack's part is the lock*: a row locked without being
    written, in a mode that a reader of the key can share and a
    writer cannot, held to the end of the transaction. sm1 has
    it as `sm1::lock_row`, and sm2 has it on a table's row as
    `rel_lock`. sm3 offers it on an open table as `tbl_lock`,
    and the C API as `sm_tbl_lock`.
  - `SELECT … FOR SHARE` and `FOR UPDATE` take the same lock. It
    waits as a write does. `NOWAIT` and `SKIP LOCKED` take it
    without waiting (`sm_tbl_trylock`,
    [`NEXT.md`](NEXT.md#2-loose-ends) §2).
  - *The lock is coarser than PostgreSQL's.* sm1 has no mode
    that lets the parent's other columns be updated while a
    child holds its key, as `FOR KEY SHARE` does against
    `FOR NO KEY UPDATE`. So such an update waits for the child's
    transaction.
- **The API is one tuple at a time, at every level.** The
  exception is rows read by key: many in one call
  ([`ROWSTORE.md`](ROWSTORE.md)).
  - Sets of rows in and out were once forced by an IPC boundary
    between a PostgreSQL backend process and sm1. The fork has
    moved PostgreSQL to threads
    ([`PGFORK.md`](PGFORK.md#2-the-blocker-the-process-model)
    §2). So the boundary is a function call, and the shape is no
    longer forced.
  - The other reason for sets was filter pushdown, and sm3
    filters nothing (below).
  - What is left of the case for batches is fewer calls for each
    scan. The keyed read is the one batch a profile asked for
    ([`ROWSTORE.md`](ROWSTORE.md)). The others wait for a profile
    to ask ([`LSMINDEX.md`](LSMINDEX.md#5-scans) §5).
- **READ COMMITTED writes wait, then proceed.** PostgreSQL's
  default level does not fail a concurrent update of the same
  row. So sm1's first-committer-wins rule is relaxed at that
  level ([`XCTMGR.md`](XCTMGR.md#5-statements) §5). It needs a
  new version ordering in sm1, and a check of the row again in
  the executor.
- **One commit point.** The catalog lives in sm3, in memory, and
  is written through to sm2. So a DDL statement and the data it
  changes commit in one sm1 transaction, and recovery is
  RocksDB's alone
  ([`PGFORK.md`](PGFORK.md#4-the-fork-and-what-it-costs) §4,
  [`CATALOG.md`](CATALOG.md#1-design) §1).
- **A host's caches are kept fresh by a version, not by a
  message.** sm sends no notice of a DDL to the sessions that did
  not make it.
  - *A table's version.* A table definition carries a version
    (`sm_table_def_t::version`). It is sm3's count of the
    catalog's publishes, at the publish that last changed the
    definition or the table's statistics, or made one of its
    indexes valid. While the reader's own transaction has changed
    the definition, it is `SM_VERSION_OWN`
    ([`CATALOG.md`](CATALOG.md#4-in-memory) §4).
  - *How a host uses it.* A host that caches what it learns of a
    table uses the cache only under the table-oid lock, and
    compares the version there. Read under IS or IX, the version
    changes only by an analyze or an index made valid, because
    DDL takes X.
  - *Namespaces.* A namespace is looked up by name with no lock.
    It has an epoch instead (`sm_cat_namespace_epoch`).
  - *After a restart.* The counts are in memory, at 0 after a
    restart. No host's cache survives a restart either.

  digres uses these counts in place of PostgreSQL's shared
  invalidation
  ([`../dg/SHMEM.md`](../dg/SHMEM.md#51-invalidation-by-version-not-by-message)
  §5.1).
- **READ COMMITTED writes are resolved above the stack, and sm3
  fixes the indexes.** The answer to `SM_ERR_UPDATED` is to check
  the statement's filter again against the newest image.
  1. The filter is the executor's, and may span tables. So the
     executor reads the newest image.
  2. The executor checks the filter again (`EvalPlanQual`).
  3. The executor repeats the write, or skips the row.

  sm2 and sm3 return the error, and offer the read. sm3's part is
  to take the index entries to remove from the old image that the
  repeated write hands back
  ([`INDEXBUILD.md`](INDEXBUILD.md#2-the-protocol) §2).
- **sm3 drives index maintenance.** PostgreSQL's executor, which
  digres keeps, could maintain indexes itself through its index
  access method (`ExecInsertIndexTuples`). It does not. It writes
  a row through one table-level call that sm3 owns, `sm_tbl_*`.
  The C wrapper's heap-level and index-level calls are gone
  ([`INDEXBUILD.md`](INDEXBUILD.md#2-the-protocol) §2). Three
  things must stay below the executor: the rules of a unique
  index's key, the index build, and the invariant between an
  index and its rows. That invariant holds only if one layer
  writes both a row and its entries.
- **Evaluation is the host's, through a callback.** No level of
  the stack evaluates a predicate or an expression.
  - *The callback.* sm3 needs evaluation to maintain or build a
    partial or an expression index. For that, it calls a
    callback that the host registers. Given a row image, the
    callback says whether the row belongs in the index, and gives
    its key values.
  - *The callback's rules.* It must not `longjmp` out or throw
    through sm3's frames. It reports failure by a return code. It
    is deterministic. It may be called from sm3's build workers.
  - *Who keeps the rules.* The fork is threaded, and rewrites
    PostgreSQL's functions and operators for threads. So the
    fork keeps these rules as it ports them, and the results are
    PostgreSQL's by construction.
  - *Scans.* A scan takes key bounds and filters nothing. The
    planner derives the bounds from a `WHERE` clause, and the
    executor tests the rest of the predicate
    ([`INDEXBUILD.md`](INDEXBUILD.md#2-the-protocol) §2,
    [`LSMINDEX.md`](LSMINDEX.md#5-scans) §5).
- **The caller undoes a write that fails partway.** A table write
  is the row's write, then one or two writes for each index. sm3
  sets no savepoint inside it. So a write that fails on a later
  step, for example on a duplicate in a unique index, leaves the
  earlier steps done. The caller rolls back to a savepoint of its
  own, or aborts. PostgreSQL's errors abort the transaction or
  the subtransaction anyway. `rel` aborts. `SM_ERR_UPDATED` needs
  no undoing: it comes from the row's write, which is the first
  step ([`INDEXBUILD.md`](INDEXBUILD.md#2-the-protocol) §2).
- **An index is built in the background.**
  1. An index made on a table that has rows is recorded as
     pending.
  2. From then on, every writer maintains it.
  3. The process's builder fills it. The C wrapper attaches a
     database's catalog to the builder when it opens the
     database.

  A reader may not scan the index until it is valid, nor if the
  reader's snapshot is older than the build. `sm_cat_index_wait`
  is for a caller that wants the index at once. A failed build
  keeps its reason in the index's record
  ([`INDEXBUILD.md`](INDEXBUILD.md#2-the-protocol) §2).
- **The host can time storage lock waits.**
  `sm_set_lock_wait_hook()` reports the start and the end of
  waits for locks and for uncommitted versions. Neither callback
  holds a lock mutex. digres uses the hook to time waits inside
  a bulk load, without timing sorting or file writes.
- **No backward scans, at any level, permanently.** This is
  settled in [`sm1/SM1_OBJECTIVE.md`](sm1/SM1_OBJECTIVE.md),
  built into sm2's encoding, and inherited by every index that
  sm3 builds. The fork's planner is changed never to ask for a
  backward scan
  ([`PGFORK.md`](PGFORK.md#5-the-one-conflict-with-a-decision-already-made)
  §5). Descending order comes from inverted key bytes, scanned
  forward.

---

## 6. What this document is not

- It is not a substitute for
  [`sm1/SM1_DESIGN.md`](sm1/SM1_DESIGN.md),
  [`sm2/SM2_DESIGN.md`](sm2/SM2_DESIGN.md) or
  [`sm3/SM3_DESIGN.md`](sm3/SM3_DESIGN.md). Read those for how a
  level works.
- It is not a restatement of [`PGFORK.md`](PGFORK.md). Read that
  for the PostgreSQL target in full.

It exists so that a reader can see the whole stack, and the few
decisions that bind it together, without first reading any level
end to end.
