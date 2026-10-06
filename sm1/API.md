# Storage Manager — the `sm1::` C++ API

This is the interface contract for the MVCC storage manager
described in `SM1_DESIGN.md`. It is the specification of
`include/sm1.h`.

The interface is C++, in `namespace sm1`, as every level's is;
only the top level wraps the stack in a C API (`DESIGN.md` §5
at the top level). Callers qualify every name — `sm1::begin`,
`sm1::read` — and never write `using namespace sm1`, since
`sm1::abort` and `sm1::read` would then shadow the C library's
`abort` and POSIX `read`. No RocksDB type appears in the
interface, and the handles are opaque.

---

## Conventions

### Return values

- A call returns an `int`: **0 on success** and **-1 on
  error**, except where a positive return is documented for
  that specific call. Exactly two have one: `sm1::read` and
  `sm1::scannxt` both return 1 to mean "no row", which is not
  an error.
- A call that yields a handle — `sm1::attach`, `sm1::opendb`,
  `sm1::scan` — returns 0 or -1 like any other, and hands the
  handle back through its last parameter, set only on success.
- On any error return, the error code and message are
  available from `sm1::errnum()` / `sm1::errmsg()`.
- Results come back through out-parameters in one shape
  (`DESIGN.md` §5 at the top level): a required output is a
  reference, an optional one a pointer that may be `nullptr`,
  a list a `std::vector&`, and bytes in engine memory a
  `std::string_view&`.

### Errors

```cpp
/* include/sm_error.h, at the root */
typedef enum sm_errnum_t {
    SM_OK                = 0,

    /* caller/programming errors */
    SM_ERR_INVALID       = 1,   /* bad argument */
    SM_ERR_NOTRANS       = 2,   /* op requires an active transaction */
    SM_ERR_INTRANS       = 3,   /* op forbidden inside a transaction */
    SM_ERR_PREPARED      = 4,   /* op forbidden on a prepared transaction */
    SM_ERR_NOSAVEPOINT   = 5,   /* no savepoint at the given depth */

    /* data errors */
    SM_ERR_NOTFOUND      = 6,   /* store or key does not exist */
    SM_ERR_DUPKEY        = 7,   /* insert onto a live key */
    SM_ERR_EXISTS        = 8,   /* home dir, database, or store exists */
    SM_ERR_INUSE         = 9,   /* database or sessions still open */

    /* concurrency errors — RETRYABLE */
    SM_ERR_CONFLICT      = 10,  /* write-write conflict */
    SM_ERR_SERIALIZATION = 11,  /* SSI validation failure */
    SM_ERR_DEADLOCK      = 12,  /* chosen as deadlock victim */

    /* environment errors */
    SM_ERR_CORRUPT       = 13,
    SM_ERR_IO            = 14,
    SM_ERR_NOMEM         = 15,
    SM_ERR_DISKFULL      = 16,
    SM_ERR_PERMISSION    = 17,
    SM_ERR_VERSION       = 18,  /* home dir format not supported */

    /* NOT retryable, and not an abort */
    SM_ERR_UPDATED       = 19,  /* newer commit; sm1::read_latest, then retry */

    /* sm2's: types and tuples (sm2/SM2_DESIGN.md §3.2, §4, §5) */
    SM_ERR_KEYTOOLONG    = 20,  /* a key that may not be cut is too long */
    SM_ERR_ROWTOOLONG    = 22,  /* a tuple past sm2::max_tup_len */

    /* the C wrapper's: an exception reached its catch-all */
    SM_ERR_EXCEPTION     = 21,

    /* sm1's: NOT retryable, and not an abort */
    SM_ERR_BLOCKED       = 23,  /* would wait for a lock; repeat the call */

    /* sm3's: constraints */
    SM_ERR_NOTNULL       = 24,  /* a NULL in a NOT NULL column */

    /* sm1's: NOT retryable; the transaction is to be aborted */
    SM_ERR_UNDOLIMIT     = 25,  /* the savepoint can no longer be rolled back to */

    /* sm1's: NOT an abort; the caller skips the row */
    SM_ERR_SELFMODIFIED  = 26,  /* the statement already changed this row */

    /* The highest code, for code that walks them all. */
    SM_ERR_LAST          = SM_ERR_SELFMODIFIED
} sm_errnum_t;

/* sm1.h */
namespace sm1 {
using errnum_t = ::sm_errnum_t;
using enum ::sm_errnum_t;
}
```

The codes are one namespace for the whole stack: every
level's are defined in the root's `include/sm_error.h`, a C
header the C wrapper shares, and `sm1.h` brings them into
`namespace sm1`, so a caller outside it writes
`sm1::SM_ERR_UPDATED`. Values are explicit and
stable. New codes are appended, grouped by the level that
produces them; existing values never change.

```cpp
errnum_t    errnum();
const char* errmsg();
const char* errname();
int         seterr(errnum_t code, std::string_view msg);
void        reseterr();
```

`sm1::errnum()` and `sm1::errmsg()` report the last error
raised **on the calling thread**. They are only meaningful
immediately after a call that returned an error indication;
a successful call leaves them untouched, so do not use them
to test for success. `sm1::errmsg()` returns a NUL-terminated
string owned by the library, valid until the next failing
call on this thread. It is never NULL.

`sm1::errname()` names the current code without its `SM_`
prefix — `"ERR_UPDATED"` for `SM_ERR_UPDATED`, `"OK"` for
`SM_OK` — as a static string, never NULL.
`sm1::seterr(code, msg)` records an error for any level of the
stack, copying `msg` into the thread's buffer (cut short if it
does not fit), and returns -1, so a failing path is
`return sm1::seterr(...);`. `sm1::reseterr()` restores `SM_OK`
and "no error", the one way to start a sequence of calls from
a known state.

#### Retryable errors

`SM_ERR_CONFLICT`, `SM_ERR_SERIALIZATION`, and
`SM_ERR_DEADLOCK` mean the transaction cannot proceed but
the same work may succeed if attempted again. On any of
them the transaction is **already aborted by the engine**:
the caller must not call `sm1::abort`, and must not issue
further data operations. The correct response is to
`sm1::begin` a fresh transaction and redo the work.

```cpp
int retryable();   /* 1 if sm1::errnum() is retryable, else 0 */
```

`sm1::retryable` tests the last error on the calling thread,
so like `sm1::errnum` it is only meaningful immediately after
a call that returned an error indication. Callers should use
it rather than comparing against the three codes: a
retryable code added later is appended to the enum like any
other, so the retryable codes are not guaranteed to stay
contiguous.

Under `sm1::xctlevel_t::serializable` these are a normal
outcome, not an exceptional one. A caller that cannot retry
should not use that level.

All other errors leave the transaction active and
unchanged, and the caller decides whether to continue or
abort. There are two exceptions. `sm1::commit`: once it is
called on an active transaction, the transaction is no
longer active, whatever the result. `sm1::prepare`: on
`SM_ERR_IO` the transaction is no longer active (see there).

#### Running out of memory

`sm1::attach` installs a new-handler, unless the process
already has one, that makes running out of memory inside sm1
fatal: while a thread is in an sm1 call, or on sm1's GC thread,
a failing allocation reports `*** FATAL ***` on stderr and to
the log hook, then aborts, so no sm1 call ever throws
`std::bad_alloc`. A `std::bad_alloc` thrown from inside RocksDB
leaves it unreliable, and only a fresh open's WAL replay
recovers safely. Outside sm1 the handler throws
`std::bad_alloc`, as `operator new` would with no handler, for
the levels above to report as `SM_ERR_NOMEM`. A handler the
host installed first is kept, but one that throws is
unsupported: sm1 does not recover from a `std::bad_alloc`
inside it (`DESIGN.md` §5 at the top level).

### Memory and lifetimes

Byte strings passed *in* (`key`, `data`, `homedir`,
`dbname`, `gid`, a sequence `name`) are `std::string_view`s,
read during the call and never retained. The caller may free
or reuse what they view immediately afterwards.

Byte strings handed *out* by `sm1::read`, `sm1::read_latest`,
`sm1::scannxt`, and the old row of `sm1::update` and
`sm1::erase` are `std::string_view`s into engine-owned memory.
They are not NUL-terminated. Each is always assigned: the
value, or empty for "no row", at the end of a range, and on
failure. Their validity windows differ and are narrow:

| Producer | View stays valid until |
| --- | --- |
| `sm1::read`, `sm1::read_latest`, the old row | the next call on that **session** |
| `sm1::scannxt` | the next call on that **scan cursor** |

A caller that needs a value beyond that window must copy it.
Note in particular that an `sm1::read` result is invalidated
by a subsequent `sm1::read`, `sm1::insert`, `sm1::commit`, or
any other call on the same session — including one made on
behalf of an unrelated part of the program.

### Lists

A call that lists several things — `sm1::lsdb`,
`sm1::lsstore`, `sm1::list_prepared` — fills a `std::vector&`
the caller owns. On success the vector is replaced, and may
be empty; on failure it is untouched. A list holds no engine
state — no lock, no snapshot, no session reference. A
`sm1::scan_t` left open is a different matter; see
`sm1::scanend`.

### Threading

- An `sm1::env_t` may be used from any number of threads
  concurrently.
- An `sm1::session_t` and its `sm1::scan_t` cursors may be
  used by **one thread at a time**. Concurrent use of a single
  session from two threads is a caller error, not a
  supported mode, and is not diagnosed.
- Distinct sessions may be used concurrently from distinct
  threads without external synchronization.
- A `homedir` may be used by **at most one process**, and
  by at most one environment in it. The library does not
  enforce this: serializing processes is the caller's job,
  and violating it is a caller error. A second `sm1::attach`
  usually fails inside RocksDB, which locks each database it
  opens, but nothing guarantees it, and `sm1::attach`,
  `sm1::mkdb`, and `sm1::rmdb` assume they are alone in the
  home directory.
- All sessions opened on the same database share one
  underlying database instance.

### Keys, values and names

Keys and values are arbitrary byte strings, passed as
`std::string_view`, and may contain embedded NUL bytes. Keys
are ordered **bytewise** (`memcmp` order), and that ordering
is what `sm1::scan` exposes. A zero-length key is legal —
including a default `std::string_view`, whose `data()` is
null. A zero-length value is legal and is distinct from a
deleted key.

An individual key or value must not exceed
`sm1::max_keylen` / `sm1::max_datalen`; exceeding either fails
with `SM_ERR_INVALID`. A name — a database, sequence or gid —
must be 1 to its bound in bytes and must not hold a NUL byte,
since the engine keeps names as C strings; otherwise the call
fails with `SM_ERR_INVALID`.

`sm1::scan` alone keeps its bounds as a pointer and a
length, because a NULL bound means "no bound" and the empty
key is a valid bound: a `std::string_view` cannot say both.

### Types

```cpp
struct env_t;       /* opaque */
struct session_t;   /* opaque */
struct scan_t;      /* opaque */
```

### Isolation levels

```cpp
enum class xctlevel_t {
    read_committed  = 0,
    repeatable_read = 1,
    serializable    = 2
};
```

READ UNCOMMITTED is deliberately absent: under MVCC there
is no state in which a caller could observe an uncommitted
version, so the level has no meaning here.

### Options

```cpp
/* include/sm_options.h, at the root */
typedef struct sm_options_t {
    int         durability_sync;       /* default 1 */
    int         group_commit_us;       /* default 200; unused */
    int         deadlock_ms;           /* default 50 */
    int         ssi_max_entries;       /* default 1 << 20; unused */
    long long   ts_lease;              /* default 1000000 */
    const char* rocksdb_options;       /* default NULL: RocksDB's */
    int         commit_cache_entries;  /* default 65536 */
    long long   savepoint_undo_bytes;  /* default 64 MB */
} sm_options_t;

/* sm1.h */
struct options_t : ::sm_options_t {
    options_t() { sm_options_init(this); }
};
```

The fields and their defaults are the root's
`include/sm_options.h`, shared with the C wrapper, whose
`sm_options_init()` fills them; `sm1::options_t` adds only the
constructor that calls it.

A default-constructed `sm1::options_t` holds every default,
so a caller sets only the fields it cares about, and fields
added later keep theirs. Passing `nullptr` wherever a
`const sm1::options_t*` is accepted — the default — also means
"all defaults".

`durability_sync = 0` trades a bounded window of
committed-transaction loss on power failure for throughput.
The database remains crash-*consistent*; it is no longer
crash-*durable*. It does not weaken isolation.

`group_commit_us` is accepted but currently unused:
concurrent commits already share one synced WAL write
through RocksDB's write group, which has no knob for
waiting on followers.

`commit_cache_entries` bounds the in-memory map from
transaction id to commit timestamp that spares readers a
status-table lookup (SM1_DESIGN.md §3.4). It must be at least
1; a smaller cache costs lookups, never correctness.

`savepoint_undo_bytes` caps what one transaction keeps in
memory to roll back to its savepoints (SM1_DESIGN.md §6.5).
Past it the transaction keeps writing, a warning is logged,
and the savepoints then set can no longer be rolled back to.
It must be at least 1.

### Logging

```cpp
enum class loglevel_t {
    error = 0,   /* something failed and nobody was told */
    warn  = 1,   /* something needs an operator's attention */
    info  = 2    /* a notable event, such as recovery */
};

using log_fn = void (*)(void* arg, loglevel_t level, const char* msg);

void set_log(log_fn fn, void* arg);   /* nullptr removes it */
```

wisr never writes a log of its own. `sm1::set_log` installs a
process-wide hook, in the manner of SQLite's
`SQLITE_CONFIG_LOG`, so the host can route wisr's messages
into its own logging. With no hook installed, nothing is
formatted or reported.

The hook hears only what no caller would otherwise learn.
Errors a call returns are not logged: its caller already
has them.

| Level | Event |
| --- | --- |
| `error` | a background GC step failed: a tombstone purge, a flush, a column family drop, or a `CF:txn` sweep |
| `error` | a status record write failed, and the database stopped until reopened |
| `error` | the process ran out of memory and is aborting (`*** FATAL ***`) |
| `warn` | the compaction filter kept versions unstamped because it could not judge them; once per compaction |
| `warn` | a `CF:txn` sweep was abandoned rather than delete records still needed |
| `warn` | one snapshot has held back garbage collection for over 60 seconds; once per snapshot |
| `info` | `sm1::attach` destroyed an unnamed database directory |
| `info` | recovery restored a prepared transaction in doubt, or dropped an orphaned column family |
| `info` | a `CF:txn` sweep deleted status records |

The hook may be called from any thread, including wisr's own
GC thread, but never twice at once. `msg` is valid only for
the duration of the call. The hook must not call back into
wisr, because it runs with a library lock held.

RocksDB still writes its own `LOG` files in each database
directory; `rocksdb_options` controls those for a database.

---

## Isolation and visibility semantics

Three behaviours here are caller-visible and are not what a
reader coming from another engine will assume. They are
part of the contract.

**1. A statement's snapshot depends on the level.** Under
`sm1::xctlevel_t::read_committed`, every `sm1::read` takes a
fresh snapshot, and every `sm1::scan` takes one snapshot that the
cursor then holds for its whole life — so a single scan is
internally consistent even though two scans in the same
transaction may disagree. Under
`sm1::xctlevel_t::repeatable_read` and
`sm1::xctlevel_t::serializable`, one snapshot is taken at
`sm1::begin` and held to the end.

A read-committed caller whose statement spans several calls
brackets it with `sm1::stmt_begin`, which makes those calls
share one snapshot.

**2. Writes can fail if the row changed after the
transaction's snapshot.** If another transaction committed a
change to the same key after this transaction's snapshot, the
write is rejected. At `sm1::xctlevel_t::repeatable_read` and
`sm1::xctlevel_t::serializable` it fails with `SM_ERR_CONFLICT`,
the transaction is aborted, and the caller retries. At
`sm1::xctlevel_t::read_committed` an update or delete fails with
`SM_ERR_UPDATED`, which does not abort: the transaction stays
active and keeps the row lock. The caller calls
`sm1::read_latest`, re-evaluates the row, and writes again, as
PostgreSQL's executor does at its default level. The engine
never applies the write to the newer row on its own: a write
that skips the re-read is refused again. An insert at
READ COMMITTED needs none of this; it is judged against the
newest committed version.

**3. A scan does not observe writes made by its own
transaction after the cursor was opened.** The cursor's
view is fixed at `sm1::scan` time. This is deliberate — it is
Halloween protection, and it is what makes "scan a range and
update every row in it" safe. Writes made *before* the
cursor was opened are visible to it.

`sm1::xctlevel_t::repeatable_read` is snapshot isolation: no
dirty reads, no lost updates, no non-repeatable reads, no
phantoms — but write skew is possible. Callers that cannot
tolerate write skew must use `sm1::xctlevel_t::serializable`.

DDL is transactional. `sm1::mkstore`, `sm1::rmstore`,
`sm1::mkseq`, and `sm1::rmseq` take effect at commit and roll
back with the transaction. The values `sm1::nextseq` hands out do
not.

---

## Environment API

An environment is a process's handle on a home directory,
`homedir`, under which its databases live. Every database
and session call goes through one.

A home directory is laid out like a PostgreSQL data
directory: `global`, which holds the root db, and `base`,
which holds one directory per database. `sm1::init` builds
that structure once; `sm1::attach` and `sm1::detach` bracket
each process's use of it. The split is PostgreSQL's, between
`initdb` and starting a server.

### int sm1::init(std::string_view homedir)

Make `homedir` a wisr home directory. It must already
exist. Creates `global` and `base`, creates the root db in
`global`, and records the format version in it.

Call it once in a home directory's life, not once per
process. It takes no lock and opens nothing: two processes
racing on a fresh directory resolve themselves, because the
first `mkdir` wins and the second fails with
`SM_ERR_EXISTS`.

Errors: `SM_ERR_EXISTS` (already a wisr home directory),
`SM_ERR_NOTFOUND` (no such directory), `SM_ERR_PERMISSION`,
`SM_ERR_IO`, `SM_ERR_DISKFULL`, `SM_ERR_INVALID`.

### int sm1::attach(std::string_view homedir, sm1::env_t*& out)

Create an environment on a home directory `sm1::init` has
already prepared. No database is opened.

The caller must ensure that no other environment, in this
process or another, has `homedir` attached; the library
takes no lock of its own (see "Threading").

`sm1::attach` opens the root db, checks the recorded format
version, and destroys any directory under `base` the root
db does not name — what a crash during `sm1::mkdb` or
`sm1::rmdb` leaves behind. It creates nothing: a `homedir`
that was never initialized is an error, not a new home
directory, so a mistyped path fails instead of quietly
becoming an empty one.

A process may attach to different home directories, but
only once to a given `homedir`.

Unless the process already has one, `sm1::attach` installs a
new-handler (`std::set_new_handler`) that makes running out
of memory inside sm1 fatal: it reports `*** FATAL ***` on
stderr and to the log hook, then aborts. A `std::bad_alloc`
thrown from inside RocksDB leaves it unreliable, and only a
fresh open's WAL replay recovers safely (`DESIGN.md` §5 at the
top level). Outside sm1 the handler throws `std::bad_alloc`.
A process that installs its own handler first keeps it; one
that throws is unsupported.

Returns 0 with the environment in `out`, or -1 on error:
`SM_ERR_NOTFOUND` (no such directory, or not a wisr home
directory), `SM_ERR_VERSION` (written by an incompatible
version), `SM_ERR_CORRUPT`, `SM_ERR_PERMISSION`,
`SM_ERR_NOMEM`, `SM_ERR_INVALID`.

### int sm1::detach(sm1::env_t* env, int force)

Release an environment. The home directory is left intact;
this is the counterpart of `sm1::attach`, not of `sm1::init`.

With `force` **0**, fails with `SM_ERR_INUSE` if any
session opened through `env` is still open; `env` then
stays valid, and the caller closes the sessions and calls
`sm1::detach` again.

With `force` **non-zero**, those sessions are closed first,
each exactly as `sm1::closedb` would close it: cursors are
closed and an active transaction is aborted, but a
**prepared** transaction is left in doubt for its
coordinator, to be found again by `sm1::list_prepared` after
the next `sm1::opendb`. Their `sm1::session_t` handles are
invalid once `sm1::detach` returns, so a forced detach is a
shutdown path, not a way to reclaim an environment the
caller means to keep using.

`force` closes sessions; it does not preempt the threads
using them. A session being closed while another thread is
inside an `sm1::` call on it is the same caller error as any
other concurrent use of one session from two threads, and
is not diagnosed. Quiesce first, then force.

On success `env` is invalid, and another process may
attach `homedir`. Passing NULL is a no-op returning 0, whatever
`force` says.

Errors: `SM_ERR_INUSE` (only when `force` is 0).

---

## Database API

A database name `dbname` is a `std::string_view` of 1 to
`sm1::max_dbname` bytes, otherwise unrestricted: any byte
but NUL, in any position. `sm1::max_dbname` is 63, matching
PostgreSQL's `NAMEDATALEN - 1`, so every name PostgreSQL
accepts for a database is a name wisr accepts. A name
outside those bounds fails with `SM_ERR_INVALID`.

A `dbname` is never a path. It is a key in the
environment's root db, which maps it to the number naming
the directory the database actually lives in (`SM1_DESIGN.md`
§3.5). `sm1::mkdb(env, "a/../b")` creates a database called
`a/../b`, not a directory, and reaches nothing outside
`homedir`.

Two divergences from PostgreSQL, both deliberate:

- **Names are compared bytewise and never folded.**
  PostgreSQL lower-cases *unquoted* identifiers in its
  parser; wisr has no parser, so `sm1::mkdb(env, "Foo")` is
  `CREATE DATABASE "Foo"`, and `Foo` and `foo` are two
  databases. A SQL layer above wisr does its own folding.
- **An over-long name is an error, not a truncation.**
  PostgreSQL truncates at `NAMEDATALEN - 1` and emits a
  `NOTICE`. This API has no channel for a notice, and
  silent truncation would silently merge two distinct
  names into one database.

### int sm1::mkdb(sm1::env_t* env, std::string_view dbname, const sm1::options_t* opt = nullptr)

Create the database `dbname`. Its directory is created if
it does not exist. The database is not opened; call
`sm1::opendb` afterwards.

Errors: `SM_ERR_EXISTS` (a database already exists there),
`SM_ERR_PERMISSION`, `SM_ERR_DISKFULL`, `SM_ERR_IO`,
`SM_ERR_INVALID`.

### int sm1::rmdb(sm1::env_t* env, std::string_view dbname)

Delete the database `dbname`, including all its data.
**This call is destructive and cannot be undone.**

Fails if any session in this environment is open on the
database. No other process can be using it, provided the
caller keeps to one attached process per `homedir`.

Errors: `SM_ERR_INUSE`, `SM_ERR_NOTFOUND` (no database
there), `SM_ERR_PERMISSION`, `SM_ERR_IO`,
`SM_ERR_INVALID`.

### int sm1::lsdb(sm1::env_t* env, std::vector<std::string>& out)

List the names of the databases in `homedir`, in bytewise
order.

On success `out` is replaced by the names; on failure it is
untouched.

A database directory is named by number, so the names
exist only in the root db and this is the only way to
enumerate them; reading `homedir` will not tell a caller
what is there.

Errors: `SM_ERR_NOMEM`, `SM_ERR_IO`.

---

## Session API

### int sm1::opendb(sm1::env_t* env, std::string_view dbname, sm1::session_t*& out, const sm1::options_t* opt = nullptr)

Open the database `dbname` and start a new session on it.
The database must already exist; `sm1::opendb` never creates
one.

The first `sm1::opendb` on a database in this process opens
the database and its `opt` takes effect. Later sessions on
the same database join the open instance, and their `opt`
is ignored except for fields documented as per-session
(currently none). Recovery — restoring prepared
transactions and their locks — happens during that first
open.

A new session has no active transaction. It belongs to
`env`, which must not be released while the session is
open.

Returns 0 with the session in `out`, or -1 on error:
`SM_ERR_NOTFOUND` (no database there), `SM_ERR_CORRUPT`,
`SM_ERR_PERMISSION`, `SM_ERR_NOMEM`, `SM_ERR_IO`,
`SM_ERR_INVALID`.

### int sm1::closedb(sm1::session_t* session)

Close the session and release it. Any open scan cursors are
closed. An active transaction is forcibly aborted; a
transaction that has been **prepared** is not — it stays
in-doubt and awaits its coordinator (see the 2PC API).

`session` is invalid after this call, whether it succeeded
or not. Passing NULL is a no-op returning 0.

Closing the last session on a database closes the
underlying database.

---

## Transaction API

Exactly one transaction may be active on a session at a
time. There is no nesting.

### int sm1::begin(sm1::session_t* session, sm1::xctlevel_t level)

Begin a transaction at `level`, and take its snapshot if
the level calls for one.

Errors: `SM_ERR_INTRANS` (a transaction is already active),
`SM_ERR_INVALID` (unknown level).

### int sm1::commit(sm1::session_t* session)

Commit the active transaction, making its writes durable
and visible. Releases all locks, discards the savepoints,
and closes any open scan cursors on this session.

For a prepared transaction, this is the coordinator's
commit decision and always succeeds — validation already
happened at prepare time.

On `SM_ERR_SERIALIZATION` the transaction has been aborted
and its effects discarded; the caller retries.

On `SM_ERR_IO` the outcome is **unknown**: the commit record
may or may not have reached the log. **Do not retry
blindly** — the work may already be committed. The database
enters a failed state: further writes, commits, and
prepares fail with `SM_ERR_IO`, while reads on existing
snapshots continue. After the database is reopened, the
transaction is committed or not according to what reached
the log.

Errors: `SM_ERR_SERIALIZATION` (retryable),
`SM_ERR_NOTRANS`, `SM_ERR_IO` (outcome unknown).

### int sm1::abort(sm1::session_t* session)

Abort the active transaction, discarding all of its writes.
Releases all locks, discards the savepoints, and closes any
open scan cursors on this session.

Aborting a **prepared** transaction is legal and is the
coordinator's abort decision.

Must not be called after a retryable error, which already
aborted the transaction.

Errors: `SM_ERR_NOTRANS`.

### bool sm1::in_doubt(sm1::session_t* session)

Whether `session` prepared a transaction that no session has
resolved yet. While it is, `sm1::commit` and `sm1::abort` on
the session resolve that prepared transaction rather than an
active one, so a caller that must never undo a prepared
transaction asks first: the C wrapper's catch-all aborts the
session's transaction only when this is false (`DESIGN.md`
§5).

Only the session's own thread can prepare, so a false answer
stays false; a true one may turn false at any moment, when
another session resolves the gid. `NULL` is false. It needs
no transaction and sets no error.

### int sm1::stmt_begin(sm1::session_t* session)

Begin a statement. Under `sm1::xctlevel_t::read_committed` every
row the statement reads then comes from **one** snapshot,
instead of a fresh one per call.

Without it, each `sm1::read` and each `sm1::scan` takes its own
snapshot, so a caller whose statement spans several calls —
an index lookup is one scan plus N reads — sees N+1
committed states and can return a mixture of them that never
existed at any instant.

The snapshot is taken by the statement's first **read**, not
by this call and not by a write. A statement may therefore
lock, wait, or write first and still read everything
committed up to the moment it starts reading. The statement
runs until the next `sm1::stmt_begin`, which starts another, or
until the transaction ends.

Once called, the transaction reads this way for the rest of
its life; there is no returning to a snapshot per call. Under
`sm1::xctlevel_t::repeatable_read` and
`sm1::xctlevel_t::serializable` the whole transaction already
reads at one snapshot.

**A statement does not see its own writes**, at any level. Its
reads, point reads and cursors alike, see the transaction's
own writes made before it began, as PostgreSQL's command id
does (SM1_DESIGN.md §6.2).

Writes still check for a newer commit against the snapshot
taken by `sm1::begin`, so `SM_ERR_UPDATED` can still arrive on
a row this statement has just read; `sm1::read_latest` then
gives the newest version. An `sm1::update` or `sm1::erase` of a
key this statement has already written fails with
`SM_ERR_SELFMODIFIED` and writes nothing. It is not an abort:
the caller skips the row.

Errors: `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`, `SM_ERR_INVALID`.

### int sm1::stmt_begin(sm1::session_t* session, sm1::stmt_t& outer)

Begin a statement inside the running one, as a trigger's or a
function's commands run inside the statement that fired them.
The outer statement's snapshot and mark are saved in `outer`.
The inner statement sees the outer's writes so far.

### int sm1::stmt_end(sm1::session_t* session, const sm1::stmt_t& outer)

End a statement begun inside another. The outer one resumes
with its own snapshot and mark, and again sees neither its own
writes nor the inner one's.

Errors: `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`.

### int sm1::set_qa_nowait(sm1::session_t* session, bool on)

**For QA only.** It lets one thread play several sessions in a
test. `NOWAIT` and `SKIP LOCKED` are to be explicit flags on
the calls that can wait, not this.

Sets whether the session waits for locks. Off, the default, a
write or an `sm1::lock` blocks while another transaction holds
the lock in a conflicting mode. On, such a call fails at once
with `SM_ERR_BLOCKED`. That is not an abort and it is not
retryable: the transaction stays active and holds exactly what
it held before. The caller polls by repeating the call, which
fails the same way or goes through, and then runs the checks
it always ran, so `SM_ERR_UPDATED` and `SM_ERR_CONFLICT` still
arrive as before. The setting lasts until it is changed, across
transactions.

A session that polls is never a deadlock victim, and sessions
polling each other's locks poll for ever: breaking that cycle
is the caller's.

Errors: `SM_ERR_INVALID`.

### int sm1::lock(sm1::session_t* session, uint64_t storeid, std::string_view key, sm1::lockmode_t mode)

```cpp
enum class lockmode_t {
    shared                  = 0,  /* S */
    exclusive               = 1,  /* X */
    intent_shared           = 2,  /* IS */
    intent_exclusive        = 3,  /* IX */
    shared_intent_exclusive = 4   /* SIX */
};
```

Lock a row without writing it: SQL's `SELECT ... FOR SHARE`
and `FOR UPDATE`, and the intention locks of Gray's scheme for
locking at several granularities. The key need not exist.

The modes are Gray's. An intention mode locks a key that stands
for something larger — a table's catalog row, say — to announce
locks on what lies below it; sm1 keeps no hierarchy itself,
only this table on each key:

| held \ asked | IS | IX | S | SIX | X |
| --- | --- | --- | --- | --- | --- |
| IS | ok | ok | ok | ok | — |
| IX | ok | ok | — | — | — |
| S | ok | — | ok | — | — |
| SIX | ok | — | — | — | — |
| X | — | — | — | — | — |

A write takes the key it writes exclusive, so every mode stops
others writing the key.

The call **blocks** while another transaction holds the row
in a conflicting mode, exactly as a write does, and can fail
with `SM_ERR_DEADLOCK` for the same reason. A transaction
already holding the row and asking for a mode it does not
cover — including by writing it — **converts** to the weakest
mode covering both (IX and S make SIX; anything and X make X),
waiting for the other holders that conflict with it. Two
transactions converting on the same row deadlock, and the
younger is the victim.

Once the lock is held, the call runs the same check a write
runs: if another transaction committed a version of this key
after this transaction's snapshot, it fails with
`SM_ERR_CONFLICT`. A successful `sm1::lock` therefore means what
the caller reads next under that lock is current, which is
what makes "lock, then read, then act" safe. At
`sm1::xctlevel_t::read_committed` the call does not fail on a
newer commit; the caller reads the row with `sm1::read_latest`,
which returns the newest committed version.

The lock is held to the end of the transaction, or until an
`sm1::rollback` past the point it was taken. A conversion is
never undone by a rollback: the row stays locked in the
stronger mode until the transaction ends.

A transaction holding locks it took with `sm1::lock` may be
prepared. They survive in the prepared transaction, each in its
mode, and are restored after a crash, so nothing conflicting
may take those rows until a coordinator resolves it.

Errors: `SM_ERR_CONFLICT` (not at READ COMMITTED) /
`SM_ERR_DEADLOCK` (retryable), `SM_ERR_BLOCKED` (under
`sm1::set_qa_nowait`), `SM_ERR_NOTFOUND` (no such store),
`SM_ERR_NOTRANS`, `SM_ERR_PREPARED`, `SM_ERR_INVALID`.

### int sm1::savepoint(sm1::session_t* session)

Set a savepoint at the current point in the transaction
and return its **depth**: 1 for the first, 2 for one set
inside it, and so on.

Savepoints nest. A transaction may hold up to
`sm1::max_savepoints` (256) of them, and they are all
discarded at commit or abort.

**A depth is reused.** Once the mark holding a depth is
gone — rolled past, or released by `sm1::forget` — the next
`sm1::savepoint` hands that depth out again, and it then
names a different mark. A caller that keeps savepoints by
name must drop a name as soon as its mark goes, exactly as
SQL requires of `RELEASE` and of rollback to an outer
savepoint. The engine cannot tell a stale depth from a
current one.

Returns the depth (>= 1), or -1 on error.

Errors: `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`,
`SM_ERR_INVALID` (the stack is full).

### int sm1::rollback(sm1::session_t* session, int depth)

Undo every write made since the savepoint at `depth`. That
savepoint survives, so the same mark may be rolled back to
more than once; the savepoints set inside it are gone. Row
locks acquired since the mark are released; locks held from
before it are kept. The transaction remains active.

**Scan cursors opened after the savepoint are closed**,
and their `sm1::scan_t` handles must not be used again; a
cursor opened before the savepoint stays open and keeps
its position. A cursor's view is fixed when it is
created, so one opened after the mark would otherwise go
on returning rows this call just undid.

Requires a savepoint at that depth. To be able to roll a
transaction back to its start, call `sm1::savepoint`
immediately after `sm1::begin` and keep the depth it
returned.

**Rollback does not undo read tracking.** Under
`sm1::xctlevel_t::serializable`, reads performed and then rolled
back still count towards serialization checking and can still
cause `SM_ERR_SERIALIZATION` at commit. This is
conservative and safe, never unsafe.

**A savepoint can be lost.** What a rollback deletes is
remembered in memory, up to `savepoint_undo_bytes` for each
transaction. A transaction that writes more than that while a
savepoint is set keeps writing, and the savepoints then set
are lost: a rollback to one fails with `SM_ERR_UNDOLIMIT`,
changes nothing, and leaves the transaction to be aborted. A
savepoint set afterwards works as usual, and forgetting a
lost one is allowed.

Errors: `SM_ERR_NOSAVEPOINT` (no savepoint at that depth),
`SM_ERR_UNDOLIMIT`, `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`.

### int sm1::forget(sm1::session_t* session, int depth)

Forget the savepoint at `depth` and every savepoint set
inside it, so none of them can be rolled back to again.
This is SQL's `RELEASE SAVEPOINT`.

The writes made since the mark stay, and so do the row
locks taken since it: they now belong to the enclosing
savepoint, or to the transaction when there is none.

A layer that brackets each of its statements with a
savepoint releases it this way at the end of the statement,
so the stack does not grow with the transaction.

Errors: `SM_ERR_NOSAVEPOINT` (no savepoint at that depth),
`SM_ERR_NOTRANS`, `SM_ERR_PREPARED`.

---

## Two-phase commit API

`sm1::prepare` makes a transaction durable but not visible.
Once prepared, a transaction survives a crash: it is
restored at the next `sm1::opendb`, still holding its row
locks, and waits for a coordinator decision. It cannot be
told "no" by the engine.

An abandoned prepared transaction blocks writers on its
keys and prevents reclamation of space indefinitely. This
is inherent to 2PC and is exposed here rather than hidden;
`sm1::abort_prepared` is the operator escape hatch.

### int sm1::prepare(sm1::session_t* session, std::string_view gid)

Prepare the active transaction for commit under the global
transaction id `gid`, a `std::string_view` of 1 to
`sm1::max_gid` bytes holding no NUL, unique among unresolved
prepared transactions in this database.

Runs serialization validation, then durably records the
prepared state. On success the transaction is `PREPARED`:
data operations, savepoints, and rollback are rejected with
`SM_ERR_PREPARED`; only `sm1::commit` and `sm1::abort` are
accepted. `sm1::closedb` leaves it in-doubt rather than
aborting it.

From prepare on, the transaction belongs to the database,
not the session: any session may resolve it by `gid` with
`sm1::commit_prepared` or `sm1::abort_prepared`. While it is
unresolved, the preparing session's `sm1::begin` fails with
`SM_ERR_INTRANS`. Once any session has resolved it, the
preparing session has no transaction: its `sm1::commit` and
`sm1::abort` fail with `SM_ERR_NOTRANS`, and `sm1::begin`
succeeds.

On `SM_ERR_IO` the outcome is **unknown**: the prepared
record may or may not have reached the log. The transaction
is no longer active on the session, and the database enters
the same failed state as after `SM_ERR_IO` from `sm1::commit`.
After the database is reopened, the transaction is either
listed by `sm1::list_prepared` under `gid`, or it was never
prepared and is aborted. A coordinator that got `SM_ERR_IO`
checks the list; since the participant never voted yes,
`sm1::abort_prepared` is always a correct resolution.

Errors: `SM_ERR_SERIALIZATION` (retryable; the transaction
is aborted and *not* prepared), `SM_ERR_NOTRANS`,
`SM_ERR_EXISTS` (`gid` already in use), `SM_ERR_INVALID`,
`SM_ERR_IO` (outcome unknown).

### int sm1::list_prepared(sm1::session_t* session, std::vector<sm1::prepared_t>& out)

List the prepared transactions in this database that have
not yet been resolved, including those restored from before
a crash.

```cpp
struct prepared_t {
    std::string gid;
    long long   prepared_at;   /* wall-clock, seconds since epoch */
};
```

On success `out` is replaced by the list, which may be
empty; on failure it is untouched.

This call does not require an active transaction and does
not start one.

Errors: `SM_ERR_NOMEM`, `SM_ERR_IO`.

### int sm1::commit_prepared(sm1::session_t* session, std::string_view gid)
### int sm1::abort_prepared(sm1::session_t* session, std::string_view gid)

Resolve a prepared transaction by global id, committing or
discarding its writes and releasing its locks. Used by a
coordinator, typically on a session other than the one that
prepared it — in particular after a restart, when the
original session no longer exists.

Neither call requires an active transaction on `session`,
and neither disturbs one if present.

Errors: `SM_ERR_NOTFOUND` (no unresolved prepared
transaction with that `gid`), `SM_ERR_DISKFULL`,
`SM_ERR_IO`.

---

## Store API

A store is an ordered map from key to value, named by a
`uint64_t` storeid that the library allocates. Storeid 0 is the
catalog's own slot, never a store: `sm1::rmstore` given it
fails with `SM_ERR_INVALID`. The level above may keep its own
catalog there, under `sm1::upper_prefix` (`"U"`): `read`,
`read_latest`, `insert`, `update`, `erase` and `lock` take
store 0 for a key that begins with it, and `scan` for bounds
that stay inside it — a start key under it, and an end key
under it or `"V"`, exclusive. Any other key in store 0 fails
with `SM_ERR_INVALID`, so sm1's own rows stay its own.

### int sm1::mkstore(sm1::session_t* session, uint64_t& storeid)

Create a store and hand out its storeid in `storeid`.
Transactional: visible to others only at commit, and undone
by abort or by a rollback that spans it. A storeid from a
transaction that aborts is simply never used.

A storeid is never reused, so it never names two stores,
and one dropped by `sm1::rmstore` never comes back. Storeids rise
in creation order, so the lowest one `sm1::lsstore` lists is the
first store the database ever had. A layer above that needs a
catalog of its own keeps it in store 0 under
`sm1::upper_prefix`, which needs no finding.

Errors: `SM_ERR_NOTRANS`, `SM_ERR_CONFLICT` (retryable),
`SM_ERR_INVALID`, `SM_ERR_PREPARED`,
`SM_ERR_DISKFULL`, `SM_ERR_IO`.

### int sm1::rmstore(sm1::session_t* session, uint64_t storeid)

Drop a store and all of its rows. Transactional, as
`sm1::mkstore`.

Space is not reclaimed at commit — transactions holding an
older snapshot may still legitimately read the store.
Physical removal happens in the background once no snapshot
can see it.

Errors: `SM_ERR_NOTFOUND`, `SM_ERR_NOTRANS`,
`SM_ERR_CONFLICT` (retryable), `SM_ERR_INVALID` (storeid 0),
`SM_ERR_PREPARED`.

A store dropped and committed stays readable by a
transaction whose snapshot predates the drop, through the
same storeid.

### int sm1::lsstore(sm1::session_t* session, std::vector<uint64_t>& out)

List the storeids of the stores the transaction sees, in
ascending order. It reads the catalog as `sm1::scan` reads a
store: at the transaction's snapshot, including its own
uncommitted DDL. Under `sm1::xctlevel_t::serializable` the whole
list is recorded as read, so a concurrent `sm1::mkstore` or
`sm1::rmstore` is a read-write conflict.

On success `out` is replaced by the storeids, which may be
none; on failure it is untouched.

Errors: `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`,
`SM_ERR_INVALID`, `SM_ERR_NOMEM`, `SM_ERR_CORRUPT`,
`SM_ERR_IO`.

---

## Sequence API

A sequence name is a `std::string_view` of 1 to
`sm1::max_seqname` (63) bytes, any byte but NUL, like a
database name. A name outside those bounds fails with
`SM_ERR_INVALID`.

A sequence is a named counter belonging to a database. It
hands out `start`, `start + increment`, `start + 2 *
increment`, and so on, and it is the way a caller gets ids
of its own — row ids, index ids, anything that must be
unique across restarts.

```cpp
int mkseq(session_t* session, std::string_view name,
          uint64_t start, uint64_t increment);
int rmseq(session_t* session, std::string_view name);
int nextseq(session_t* session, std::string_view name,
            uint64_t& out);
```

`sm1::mkseq` and `sm1::rmseq` are transactional, like
`sm1::mkstore` and `sm1::rmstore`: they need an active
transaction, take effect at commit, and roll back with it.
`increment` must not be
0. A name used again after a drop is
a new sequence, starting at its own `start`.

The **values** are not transactional, like a PostgreSQL
sequence: `sm1::nextseq` needs no active transaction, never
blocks, and an abort does not give a value back. Values are
unique but not dense: besides aborts, a crash skips the rest
of a block reserved in advance (the `ts_lease` size),
leaving a gap. Distinct sessions may call it concurrently.

Inside a transaction, `sm1::nextseq` sees that transaction's
snapshot, so a sequence it created itself is usable before
it commits. When the next value would pass `UINT64_MAX`, the
sequence is exhausted and every further call fails with
`SM_ERR_INVALID`.

Errors for `sm1::mkseq`: `SM_ERR_EXISTS`, `SM_ERR_NOTRANS`,
`SM_ERR_CONFLICT` (retryable), `SM_ERR_INVALID`,
`SM_ERR_PREPARED`, `SM_ERR_DISKFULL`, `SM_ERR_IO`. For
`sm1::rmseq`: `SM_ERR_NOTFOUND` and the same otherwise. For
`sm1::nextseq`: `SM_ERR_NOTFOUND` (no such sequence),
`SM_ERR_INVALID` (NULL session, bad name, or exhausted),
`SM_ERR_PREPARED`, `SM_ERR_CORRUPT`, `SM_ERR_IO` (the
database has stopped, or persisting the next block failed),
`SM_ERR_DISKFULL`.

---

## Update API

All three require an active, non-prepared transaction. All
three can fail with `SM_ERR_CONFLICT` at REPEATABLE READ and
SERIALIZABLE, and `sm1::update` and `sm1::erase` with
`SM_ERR_UPDATED` at READ COMMITTED, when another transaction
committed a change to the same key after this transaction's
snapshot.

Each blocks while another active transaction holds a write
lock on the same key, until that transaction ends. A
blocked call can return `SM_ERR_DEADLOCK` if it is selected
as the victim of a deadlock cycle.

```cpp
int insert(session_t* session, uint64_t storeid,
           std::string_view key, std::string_view data);

int update(session_t* session, uint64_t storeid,
           std::string_view key, std::string_view data,
           std::string_view* old = nullptr);

int erase(session_t* session, uint64_t storeid,
          std::string_view key, std::string_view* old = nullptr);
```

- `sm1::insert` requires that the key not exist, and fails
  with `SM_ERR_DUPKEY` if it does.
- `sm1::update` requires that the key exist, and fails with
  `SM_ERR_NOTFOUND` if it does not. It replaces the value
  entirely; there is no partial update.
- `sm1::erase` requires that the key exist, and fails with
  `SM_ERR_NOTFOUND` if it does not.

There is no upsert. A caller wanting one attempts
`sm1::insert` and falls back to `sm1::update` on
`SM_ERR_DUPKEY`.

**The old row.** `sm1::update` and `sm1::erase` return the
row they replaced through `old`. A caller maintaining
secondary indexes needs it, to remove the entries the old
value produced; leave it `nullptr`, the default, to skip it.
The write already reads that row to check the key exists, so
asking for it costs a copy, not another seek.

`*old` views session-owned memory and is valid only until
the next call on this session — the same rule as
`sm1::read`, and the same buffer, so either call invalidates
what the other returned. Copy it to keep it. On failure
`*old` is empty.

Errors for all three: `SM_ERR_CONFLICT` /
`SM_ERR_DEADLOCK` (retryable), `SM_ERR_UPDATED` (READ
COMMITTED; not retryable, the transaction stays active; call
`sm1::read_latest`, then write again), `SM_ERR_NOTFOUND` (store
or, for update/delete, key), `SM_ERR_DUPKEY` (insert),
`SM_ERR_NOTRANS`, `SM_ERR_PREPARED`, `SM_ERR_INVALID`,
`SM_ERR_DISKFULL`, `SM_ERR_IO`.

---

## Read API

Reads never block and never take locks; `sm1::lock` is how a
caller takes one deliberately. They cannot return
`SM_ERR_CONFLICT` or `SM_ERR_DEADLOCK`. Under
`sm1::xctlevel_t::serializable` a read is recorded, and may
contribute to an `SM_ERR_SERIALIZATION` failure later, at
commit.

### int sm1::read(sm1::session_t* session, uint64_t storeid, std::string_view key, std::string_view& data)

Read one key.

Returns **0** if the key exists — `data` then views the
value, subject to the lifetime rules above. Returns **1** if
the key does not exist, leaving `data` empty; this is not an
error and does not set the error state. Returns **-1** on
error, also leaving `data` empty.

Errors: `SM_ERR_NOTFOUND` (the *store* does not exist —
a missing key returns 1, not an error), `SM_ERR_NOTRANS`,
`SM_ERR_PREPARED`, `SM_ERR_INVALID`, `SM_ERR_CORRUPT`,
`SM_ERR_IO`.

### int sm1::read_latest(sm1::session_t* session, uint64_t storeid, std::string_view key, std::string_view& data)

Read the newest committed version of one key, ignoring the
snapshot. Returns as `sm1::read` does: **0** found, **1** no
such key (or deleted), **-1** on error.

It exists for `sm1::xctlevel_t::read_committed`, and needs a row
lock the transaction already holds on the key: the one a write
that failed with `SM_ERR_UPDATED` left behind, or one taken with
`sm1::lock`. The lock is what keeps the version from changing
before commit. At any other level, or with no lock held, it
fails with `SM_ERR_INVALID`.

Under an exclusive lock the call also lets the next write of
this key go ahead, once. Under a shared lock it reads but
lets nothing through, since a shared holder cannot write
without upgrading. The write clears the permission, so a
second write after a savepoint rollback needs a fresh read.

Typical use, for `UPDATE ... WHERE`:

```cpp
rc = sm1::update(s, store, key, newval);
if (rc != 0 && sm1::errnum() == sm1::SM_ERR_UPDATED) {
    std::string_view data;
    sm1::read_latest(s, store, key, data);
    /* re-check the WHERE clause; recompute newval from data */
    rc = sm1::update(s, store, key, newval);
}
```

Errors: `SM_ERR_NOTFOUND` (the *store* does not exist),
`SM_ERR_NOTRANS`, `SM_ERR_PREPARED`, `SM_ERR_INVALID`
(not READ COMMITTED, or no lock held), `SM_ERR_CORRUPT`,
`SM_ERR_IO`.

### int sm1::scan(sm1::session_t* session, uint64_t storeid, const char* skey, size_t sklen, int skey_inclusive, const char* ekey, size_t eklen, int ekey_inclusive, sm1::scan_t*& out)

Open a cursor over the keys of `storeid` in ascending
bytewise order, from `skey` to `ekey`.

- `skey == NULL` starts at the first key; `sklen` and
  `skey_inclusive` are then ignored.
- `ekey == NULL` runs to the last key; `eklen` and
  `ekey_inclusive` are then ignored.
- The `*_inclusive` flags say whether a key exactly equal
  to the bound is returned.
- An empty range is legal and yields no rows.

Both bounds are required (rather than only a start key)
because under `sm1::xctlevel_t::serializable` the range is the
predicate that is tracked for conflicts. Scanning to the
end of the store and discarding rows in the caller
records a far larger predicate and causes needless
serialization failures.

Scanning is **forward only**. Descending iteration is not
supported, and is not a planned addition: no index serves a
backward scan (`SM1_OBJECTIVE.md`). A caller that needs
descending order keeps a second store with inverted key
bytes and scans that one forward.

The cursor's view is fixed at this call (see "Isolation and
visibility semantics", point 3). Its snapshot also pins
storage reclamation, so a cursor left open indefinitely
prevents space from being reclaimed; close cursors
promptly.

The cursor is owned by the session, must be used only from
the thread using that session, and is invalidated by
`sm1::commit`, `sm1::abort`, and `sm1::closedb`. Multiple cursors
may be open on one session at once.

Returns 0 with the cursor in `out`, or -1 on error:
`SM_ERR_NOTFOUND` (no such store), `SM_ERR_NOTRANS`,
`SM_ERR_PREPARED`, `SM_ERR_INVALID`, `SM_ERR_NOMEM`,
`SM_ERR_IO`.

### int sm1::scannxt(sm1::scan_t* scan, std::string_view& key, std::string_view& data)

Advance to the next row.

Returns **0** if a row was produced — `key` and `data` then
view it, valid until the next call on this cursor. Returns
**1** at end of range, leaving both empty; this is not an
error and does not set the error state. Returns **-1** on
error, also leaving both empty.

Once it has returned 1 or -1, further calls on the cursor
return the same thing.

Errors: `SM_ERR_CORRUPT`, `SM_ERR_IO`.

### int sm1::scan_seek(sm1::scan_t* scan, std::string_view key, int inclusive)

Move an open cursor to `key`, so the next `sm1::scannxt`
returns from there. This is how a caller probes a store
repeatedly — an index nested-loop join, an IN-list, a skip
scan — without closing and reopening a cursor per probe.

`inclusive` says whether a row at `key` itself is returned.

The cursor keeps its snapshot and its view, so a reposition
does **not** see writes made since `sm1::scan`, and it keeps
its bounds: it returns rows only from the range `sm1::scan`
was given.

Seeking moves **forward only**. A key before the last one
the cursor read fails with `SM_ERR_INVALID`, as does a key
below the scan's start key. A key past the scan's end key
is allowed and leaves the cursor at its end, where
`sm1::scannxt` returns 1.

Under `sm1::xctlevel_t::serializable` the keys skipped over still
count as read: the cursor's predicate covers its start key
through the furthest key it reached, gaps included. That is
an over-approximation, so it can only cause a serialization
failure that was not strictly necessary, never miss one.

Errors: `SM_ERR_INVALID` (a backward seek, a key below the
start key, or a cursor that already failed).

### void sm1::scanend(sm1::scan_t* scan)

Close a cursor and release its resources, including the
snapshot it was pinning. Passing NULL is a no-op.

Under `sm1::xctlevel_t::serializable`, this is where the range
the cursor actually traversed is registered for conflict
tracking. A cursor abandoned early registers the shorter
range it really read, which is both correct and tighter
than the requested range — so closing early is preferable
to draining a scan whose rows are not needed.

`scan` is invalid after this call. Closing a cursor never
fails, which is why it returns nothing: a caller has no
useful recovery from a failure to release resources.

---

## Divergences from the earlier sketch

These changes were made against the previous `API.txt`.
Each is a decision, not a detail, and `SM1_DESIGN.md` §10.3 and
§12.1 should be reconciled with the ones marked (D).

They were made while the API was C. The conversion to C++
that followed replaced some of their shapes — the string list,
`malloc`'d arrays, and pointer-and-length pairs — with those
under "Conventions" above; the names below are the current
ones.

1. **`sm1::errnum_t` became reachable, and both accessors are
   now functions.** (D) The old file exposed no way to read
   the code, only a message string, and callers cannot
   implement the retry contract without a machine-readable
   one. The names are unchanged, but `sm1::errnum` and
   `sm1::errmsg` are now calls rather than the old
   `extern thread_local int` / `char[200]` globals.
   Exported thread-local *data* is a poor fit for a C ABI:
   it needs `__declspec(dllimport)` on the consumer side on
   Windows, can fail to `dlopen` under the initial-exec TLS
   model, freezes the 200-byte buffer size into the ABI, is
   writable by the caller, and is unreachable through
   `dlsym` and other FFI. A call also lets the message be
   formatted only when someone asks for it, which matters
   because serialization failures here are a normal
   outcome rather than a rare one. glibc exposes `errno`
   as a function for the same reasons.
2. **Return convention repaired.** The old file promised
   0/-1 everywhere, then documented `sm1::rmdb` as returning
   true/false, and had two calls that need three outcomes.
3. **`sm1::read`/`sm1::scannxt` use a tri-state return**
   rather than `SM1_DESIGN.md`'s separate `int* found`. One
   idiom for "no row" across both calls. (D)
4. **The `schema` parameter is gone** from the read calls.
   No such concept exists anywhere else in the API.
5. **Lengths are `size_t`,** and every byte-string
   parameter has one. The old `sm1::read`/`sm1::scan` had none
   at all and could not carry binary keys or values. (D)
6. **2PC entry points added** — `gid` on `sm1::prepare`, plus
   `sm1::list_prepared` / `sm1::free_prepared` /
   `sm1::commit_prepared` / `sm1::abort_prepared`. Without
   them a prepared transaction that survives a restart is
   unreachable forever.
7. **`sm1::options_t` added.** `SM1_DESIGN.md` §12.2 specifies
   configuration knobs, including one (`durability_sync`)
   that changes the durability guarantee, and there was no
   surface to set any of them.
8. **`sm1::rollback` takes the depth of the savepoint to
   undo to**, and requires a savepoint there instead of
   silently rolling back to `sm1::begin` when none is set.
   The old behaviour makes one call mean either "undo the
   last few writes" or "undo everything" depending on
   invisible state. `sm1::savepoint` right after `sm1::begin`
   recovers the old capability explicitly. (D)
9. **`sm1::scan` gained an end key** (required for
   serializable predicate tracking) and an inclusivity flag
   for it.
10. **`sm1::scanend` returns void.** A release call that can
    fail gives the caller nothing to do about it. (D)
11. **Types are `sm1_`-prefixed and opaque** —
    `sm1::session_t`, `sm1::scan_t` — rather than occupying the
    global names `session_t` and `scan_t`.
12. **Preconditions, error sets, lifetimes, threading, key
    ordering, and the three surprising isolation
    behaviours are now stated.** They were decided in
    `SM1_DESIGN.md` but invisible to a caller reading only the
    API.
13. **A misspelling fixed.** The level was spelled
    `SM1_XCT_READ_COMMITED`; it became `SM1_XCT_READ_COMMITTED`,
    and is now `sm1::xctlevel_t::read_committed`.
14. **Databases are named within an environment.** (D)
    `sm1::attach(homedir)` returns an `sm1::env_t`, and
    `sm1::mkdb`, `sm1::rmdb`, and `sm1::opendb` take an env and a
    single-component `dbname` instead of a `dbpath`. The
    env is thread-safe and separate from the per-thread
    session, so concurrent sessions keep working as
    before. It gives the registry of open
    databases an owner, and a place for resources shared
    across databases. Process exclusion is the caller's:
    one process attaches a `homedir`, so `sm1::rmdb` can see
    every session on its database.
15. **A `dbname` is a name, not a path.** (D) The
    environment keeps a root db, `{homedir}/global/root`,
    mapping each name to the number that names its
    directory under `{homedir}/base`, so a `dbname` may be
    any 1..63 bytes —
    every name PostgreSQL allows. Restricting the
    spelling would have been simpler, but it would have
    made wisr reject database names a SQL layer above it
    must accept. Containment no longer rests on the
    spelling: no path is built from a `dbname` at all.
    `sm1::lsdb` comes with it, because the directory names
    stop being readable.
16. **A list of names is a `std::vector<std::string>`.** (D)
    `sm1::lsdb` fills one the caller owns, rather than a
    `char***` with a paired `size_t*`, and `sm1::free_dbs` is
    gone. The count travels with the list, so the two cannot
    disagree, and the list frees itself. An intermediate
    `sm1::strlist_t` handle did the same in C, and went with
    the move to C++.
17. **Preparing a home directory is separate from using
    one.** (D) `sm1::init` creates the structure once and
    returns an `int`; `sm1::attach` / `sm1::detach` bracket a
    process's use of it and replace `sm1::final`. The old
    `sm1::init` did both, which forced it to create `global`
    and `base` when absent — so it could not tell a
    directory that is not a wisr home from one that is
    brand new, and a mistyped path became an empty home
    rather than an error. The split is `initdb` against
    starting a server, it leaves `sm1::attach` with no
    creation path at all, and it gives the format version
    somewhere to be written and checked
    (`SM_ERR_VERSION`).
18. **`sm1::detach` takes a `force` flag.** (D) Shutdown
    otherwise has to walk the caller's own list of open
    sessions and close each one before the environment can
    go, which every caller writes identically and which a
    caller winding down after an error may not be able to
    do at all. `force` does not change what closing a
    session means — `sm1::closedb`'s rules apply unchanged, so
    a prepared transaction still survives.
19. **Savepoints nest, and `sm1::forget` releases one.** (D)
    The sketch gave a session one savepoint. A layer that
    wraps each of its own statements in a savepoint cannot
    then offer SQL savepoints to its caller, since the two
    uses collide on the single mark. `sm1::savepoint` now
    returns a depth, `sm1::rollback` takes one, and
    `sm1::forget` is `RELEASE SAVEPOINT`. `SM1_DESIGN.md` §6.5
    has the case that asked for it.

20. **`sm1::lock` added.** (D) Locking a row without writing it
    is how a layer does `SELECT ... FOR UPDATE`, and its
    shared mode is what lets a secondary-index builder wait
    out the writers that cannot yet know the index exists.
    The index build (`sm3/SM3_DESIGN.md` §3) is the case that
    asked for it; it was the only missing primitive whose
    absence made correct behaviour impossible rather than
    merely awkward.

21. **`sm1::scan_seek` added.** (D) Repositioning a cursor is
    what an index nested-loop join, an IN-list, and a skip
    scan all need; without it each probe closes and reopens
    a cursor, and under READ COMMITTED every reopen takes a
    new snapshot as well. `SM1_DESIGN.md` §10.2.

22. **`sm1::update` and `sm1::erase` return the row they
    replaced.** (D) The write already reads it to check the
    key exists, so a layer maintaining secondary indexes
    would otherwise pay a second seek and a copy for
    something the engine had in hand. `SM1_DESIGN.md` §5.3.
