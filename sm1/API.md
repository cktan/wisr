# Storage Manager — the `sm_*` C API

This is the interface contract for the MVCC storage manager
described in `DESIGN.md`. It is the specification of
`include/sm.h`.

Everything here is a C ABI. The implementation is C++, but no
C++ type appears in the interface.

---

## Conventions

### Return values

- Functions returning `int` return **0 on success** and
  **-1 on error**, except where a positive return is
  documented for that specific function. Exactly two
  functions have one: `sm_read` and `sm_scannxt` both
  return 1 to mean "no row", which is not an error.
- Functions returning a pointer return **NULL on error**.
- On any error return, the error code and message are
  available from `sm_errno()` / `sm_errmsg()`.

### Errors

```c
typedef enum {
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
    SM_ERR_UPDATED       = 19   /* newer commit; sm_read_latest, then retry */
} sm_errno_t;
```

Values are explicit and stable. New codes are appended;
existing values never change.

```c
sm_errno_t  sm_errno(void);
const char* sm_errmsg(void);
```

Both report the last error raised **on the calling thread**.
They are only meaningful immediately after a call that
returned an error indication; a successful call leaves them
untouched, so do not use them to test for success.
`sm_errmsg()` returns a NUL-terminated string owned by the
library, valid until the next failing `sm_*` call on this
thread. It is never NULL.

#### Retryable errors

`SM_ERR_CONFLICT`, `SM_ERR_SERIALIZATION`, and
`SM_ERR_DEADLOCK` mean the transaction cannot proceed but
the same work may succeed if attempted again. On any of
them the transaction is **already aborted by the engine**:
the caller must not call `sm_abort`, and must not issue
further data operations. The correct response is to
`sm_begin` a fresh transaction and redo the work.

```c
int sm_retryable(void);   /* 1 if sm_errno() is retryable, else 0 */
```

`sm_retryable` tests the last error on the calling thread,
so like `sm_errno` it is only meaningful immediately after a
call that returned an error indication. Callers should use
it rather than comparing against the three codes: a
retryable code added later is appended to the enum like any
other, so the retryable codes are not guaranteed to stay
contiguous.

Under `SM_XCT_SERIALIZABLE` these are a normal outcome, not
an exceptional one. A caller that cannot retry should not
use that level.

All other errors leave the transaction active and
unchanged, and the caller decides whether to continue or
abort. There are two exceptions. `sm_commit`: once it is
called on an active transaction, the transaction is no
longer active, whatever the result. `sm_prepare`: on
`SM_ERR_IO` the transaction is no longer active (see there).

### Memory and lifetimes

Byte strings passed *in* (`key`, `data`, `homedir`,
`dbname`, `gid`, a sequence `name`) are read during the call and
never retained. The caller may free or reuse them immediately
afterwards.

Byte strings handed *out* by `sm_read` and `sm_scannxt`
point into engine-owned memory. They are **not**
NUL-terminated and must not be freed. Their validity
windows differ and are narrow:

| Producer | Buffer stays valid until |
| --- | --- |
| `sm_read` | the next `sm_*` call on that **session** |
| `sm_scannxt` | the next call on that **scan cursor** |

A caller that needs a value beyond that window must copy it.
Note in particular that an `sm_read` result is invalidated
by a subsequent `sm_read`, `sm_insert`, `sm_commit`, or any
other call on the same session — including one made on
behalf of an unrelated part of the program.

### String lists

A call that returns several names returns an
`sm_strlist_t`: a counted, immutable list.

```c
size_t      sm_strlist_count(const sm_strlist_t* sl);
const char* sm_strlist_at(const sm_strlist_t* sl, size_t i);
```

On success the out-param is never NULL; an empty result is a list
whose count is 0. `sm_strlist_at` returns a NUL-terminated string
owned by the list and valid until the list is freed, or NULL for
an index past the end. The list is one `malloc` block: the caller
releases the whole list with one `free()` and frees no element on
its own. The count travels inside the handle, so it cannot
disagree with the array it describes.

A list holds no engine state — no lock, no snapshot, no
session reference — so one that is never freed leaks memory
and nothing else. An `sm_scan_t` left open is a different
matter; see `sm_scanend`.

A list carries names, never keys or values: its elements
are NUL-terminated, and a key or value may contain NUL.

### Threading

- An `sm_env_t` may be used from any number of threads
  concurrently.
- A `sm_session_t` and its `sm_scan_t` cursors may be used
  by **one thread at a time**. Concurrent use of a single
  session from two threads is a caller error, not a
  supported mode, and is not diagnosed.
- Distinct sessions may be used concurrently from distinct
  threads without external synchronization.
- A `homedir` may be used by **at most one process**, and
  by at most one environment in it. The library does not
  enforce this: serializing processes is the caller's job,
  and violating it is a caller error. A second `sm_attach`
  usually fails inside RocksDB, which locks each database it
  opens, but nothing guarantees it, and `sm_attach`,
  `sm_mkdb`, and `sm_rmdb` assume they are alone in the home
  directory.
- All sessions opened on the same database share one
  underlying database instance.

### Keys and values

Keys and values are arbitrary byte strings and may contain
embedded NUL bytes; every call that takes one also takes an
explicit length. Keys are ordered **bytewise** (`memcmp`
order), and that ordering is what `sm_scan` exposes. A
zero-length key is legal. A zero-length value is legal and
is distinct from a deleted key.

Lengths are `size_t`. An individual key or value must not
exceed `SM_MAX_KEYLEN` / `SM_MAX_DATALEN`; exceeding either
fails with `SM_ERR_INVALID`.

### Types

```c
typedef struct sm_env_t     sm_env_t;       /* opaque */
typedef struct sm_session_t sm_session_t;   /* opaque */
typedef struct sm_scan_t    sm_scan_t;      /* opaque */
typedef struct sm_strlist_t sm_strlist_t;   /* opaque */
```

### Isolation levels

```c
typedef enum {
    SM_XCT_READ_COMMITTED  = 0,
    SM_XCT_REPEATABLE_READ = 1,
    SM_XCT_SERIALIZABLE    = 2
} sm_xctlevel_t;
```

READ UNCOMMITTED is deliberately absent: under MVCC there
is no state in which a caller could observe an uncommitted
version, so the level has no meaning here.

### Options

```c
typedef struct sm_options_t sm_options_t;
struct sm_options_t {
    int         durability_sync;       /* default 1  */
    int         group_commit_us;       /* default 200, unused */
    int         deadlock_ms;           /* default 50 */
    int         ssi_max_entries;       /* default 1<<20, unused */
    long long   ts_lease;              /* default 1000000 */
    const char* rocksdb_options;       /* option string, NULL = defaults */
    int         commit_cache_entries;  /* default 65536 */
};

void sm_options_init(sm_options_t* opt);   /* fill with defaults */
```

`sm_options_init` must be called before setting any field,
so that fields added in later versions keep their defaults.

Passing NULL wherever an `sm_options_t*` is accepted means
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
status-table lookup (DESIGN.md §3.4). It must be at least
1; a smaller cache costs lookups, never correctness.

### Logging

```c
typedef enum {
    SM_LOG_ERROR = 0,   /* something failed and nobody was told */
    SM_LOG_WARN  = 1,   /* something needs an operator's attention */
    SM_LOG_INFO  = 2    /* a notable event, such as recovery */
} sm_loglevel_t;

typedef void (*sm_log_fn)(void* arg, sm_loglevel_t level, const char* msg);

void sm_set_log(sm_log_fn fn, void* arg);   /* NULL removes it */
```

wisr never writes a log of its own. `sm_set_log` installs a
process-wide hook, in the manner of SQLite's
`SQLITE_CONFIG_LOG`, so the host can route wisr's messages
into its own logging. With no hook installed, nothing is
formatted or reported.

The hook hears only what no caller would otherwise learn.
Errors a call returns are not logged: its caller already
has them.

| Level | Event |
| --- | --- |
| `SM_LOG_ERROR` | a background GC step failed: a tombstone purge, a flush, a column family drop, or a `CF:txn` sweep |
| `SM_LOG_ERROR` | a status record write failed, and the database stopped until reopened |
| `SM_LOG_WARN` | the compaction filter kept versions unstamped because it could not judge them; once per compaction |
| `SM_LOG_WARN` | a `CF:txn` sweep was abandoned rather than delete records still needed |
| `SM_LOG_WARN` | one snapshot has held back garbage collection for over 60 seconds; once per snapshot |
| `SM_LOG_INFO` | `sm_attach` destroyed an unnamed database directory |
| `SM_LOG_INFO` | recovery restored a prepared transaction in doubt, or dropped an orphaned column family |
| `SM_LOG_INFO` | a `CF:txn` sweep deleted status records |

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
`SM_XCT_READ_COMMITTED`, every `sm_read` takes a fresh
snapshot, and every `sm_scan` takes one snapshot that the
cursor then holds for its whole life — so a single scan is
internally consistent even though two scans in the same
transaction may disagree. Under `SM_XCT_REPEATABLE_READ`
and `SM_XCT_SERIALIZABLE`, one snapshot is taken at
`sm_begin` and held to the end.

A read-committed caller whose statement spans several calls
brackets it with `sm_stmt_begin`, which makes those calls
share one snapshot.

**2. Writes can fail if the row changed after the
transaction's snapshot.** If another transaction committed a
change to the same key after this transaction's snapshot, the
write is rejected. At `SM_XCT_REPEATABLE_READ` and
`SM_XCT_SERIALIZABLE` it fails with `SM_ERR_CONFLICT`, the
transaction is aborted, and the caller retries. At
`SM_XCT_READ_COMMITTED` an update or delete fails with
`SM_ERR_UPDATED`, which does not abort: the transaction stays
active and keeps the row lock. The caller calls
`sm_read_latest`, re-evaluates the row, and writes again, as
PostgreSQL's executor does at its default level. The engine
never applies the write to the newer row on its own: a write
that skips the re-read is refused again. An insert at
READ COMMITTED needs none of this; it is judged against the
newest committed version.

**3. A scan does not observe writes made by its own
transaction after the cursor was opened.** The cursor's
view is fixed at `sm_scan` time. This is deliberate — it is
Halloween protection, and it is what makes "scan a range and
update every row in it" safe. Writes made *before* the
cursor was opened are visible to it.

`SM_XCT_REPEATABLE_READ` is snapshot isolation: no dirty
reads, no lost updates, no non-repeatable reads, no
phantoms — but write skew is possible. Callers that cannot
tolerate write skew must use `SM_XCT_SERIALIZABLE`.

DDL is transactional. `sm_mkstore`, `sm_rmstore`, `sm_mkseq`,
and `sm_rmseq` take effect at commit and roll back with the
transaction. The values `sm_nextseq` hands out do not.

---

## Environment API

An environment is a process's handle on a home directory,
`homedir`, under which its databases live. Every database
and session call goes through one.

A home directory is laid out like a PostgreSQL data
directory: `global`, which holds the root db, and `base`,
which holds one directory per database. `sm_init` builds
that structure once; `sm_attach` and `sm_detach` bracket
each process's use of it. The split is PostgreSQL's, between
`initdb` and starting a server.

### int sm_init(const char* homedir)

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

### sm_env_t* sm_attach(const char* homedir)

Create an environment on a home directory `sm_init` has
already prepared. No database is opened.

The caller must ensure that no other environment, in this
process or another, has `homedir` attached; the library
takes no lock of its own (see "Threading").

`sm_attach` opens the root db, checks the recorded format
version, and destroys any directory under `base` the root
db does not name — what a crash during `sm_mkdb` or
`sm_rmdb` leaves behind. It creates nothing: a `homedir`
that was never initialized is an error, not a new home
directory, so a mistyped path fails instead of quietly
becoming an empty one.

A process may attach to different home directories, but
only once to a given `homedir`.

Returns NULL on error: `SM_ERR_NOTFOUND` (no such directory,
or not a wisr home directory), `SM_ERR_VERSION` (written by
an incompatible version), `SM_ERR_CORRUPT`,
`SM_ERR_PERMISSION`, `SM_ERR_NOMEM`, `SM_ERR_INVALID`.

### int sm_detach(sm_env_t* env, int force)

Release an environment. The home directory is left intact;
this is the counterpart of `sm_attach`, not of `sm_init`.

With `force` **0**, fails with `SM_ERR_INUSE` if any
session opened through `env` is still open; `env` then
stays valid, and the caller closes the sessions and calls
`sm_detach` again.

With `force` **non-zero**, those sessions are closed first,
each exactly as `sm_closedb` would close it: cursors are
closed and an active transaction is aborted, but a
**prepared** transaction is left in doubt for its
coordinator, to be found again by `sm_list_prepared` after
the next `sm_opendb`. Their `sm_session_t` handles are
invalid once `sm_detach` returns, so a forced detach is a
shutdown path, not a way to reclaim an environment the
caller means to keep using.

`force` closes sessions; it does not preempt the threads
using them. A session being closed while another thread is
inside an `sm_*` call on it is the same caller error as any
other concurrent use of one session from two threads, and
is not diagnosed. Quiesce first, then force.

On success `env` is invalid, and another process may
attach `homedir`. Passing NULL is a no-op returning 0, whatever
`force` says.

Errors: `SM_ERR_INUSE` (only when `force` is 0).

---

## Database API

A database name `dbname` is a NUL-terminated string of 1
to `SM_MAX_DBNAME` bytes, otherwise unrestricted: any byte
but NUL, in any position. `SM_MAX_DBNAME` is 63, matching
PostgreSQL's `NAMEDATALEN - 1`, so every name PostgreSQL
accepts for a database is a name wisr accepts. A name
outside those bounds fails with `SM_ERR_INVALID`.

A `dbname` is never a path. It is a key in the
environment's root db, which maps it to the number naming
the directory the database actually lives in (`DESIGN.md`
§3.5). `sm_mkdb(env, "a/../b")` creates a database called
`a/../b`, not a directory, and reaches nothing outside
`homedir`.

Two divergences from PostgreSQL, both deliberate:

- **Names are compared bytewise and never folded.**
  PostgreSQL lower-cases *unquoted* identifiers in its
  parser; wisr has no parser, so `sm_mkdb(env, "Foo")` is
  `CREATE DATABASE "Foo"`, and `Foo` and `foo` are two
  databases. A SQL layer above wisr does its own folding.
- **An over-long name is an error, not a truncation.**
  PostgreSQL truncates at `NAMEDATALEN - 1` and emits a
  `NOTICE`. A C API has no channel for a notice, and
  silent truncation would silently merge two distinct
  names into one database.

### int sm_mkdb(sm_env_t* env, const char* dbname, const sm_options_t* opt)

Create the database `dbname`. Its directory is created if
it does not exist. The database is not opened; call
`sm_opendb` afterwards.

Errors: `SM_ERR_EXISTS` (a database already exists there),
`SM_ERR_PERMISSION`, `SM_ERR_DISKFULL`, `SM_ERR_IO`,
`SM_ERR_INVALID`.

### int sm_rmdb(sm_env_t* env, const char* dbname)

Delete the database `dbname`, including all its data.
**This call is destructive and cannot be undone.**

Fails if any session in this environment is open on the
database. No other process can be using it, provided the
caller keeps to one attached process per `homedir`.

Errors: `SM_ERR_INUSE`, `SM_ERR_NOTFOUND` (no database
there), `SM_ERR_PERMISSION`, `SM_ERR_IO`,
`SM_ERR_INVALID`.

### int sm_lsdb(sm_env_t* env, sm_strlist_t** out)

List the names of the databases in `homedir`, in bytewise
order.

On success `*out` is a list of names; release it with
`free()`.

A database directory is named by number, so the names
exist only in the root db and this is the only way to
enumerate them; reading `homedir` will not tell a caller
what is there.

Errors: `SM_ERR_NOMEM`, `SM_ERR_IO`.

---

## Session API

### sm_session_t* sm_opendb(sm_env_t* env, const char* dbname, const sm_options_t* opt)

Open the database `dbname` and start a new session on it.
The database must already exist; `sm_opendb` never creates
one.

The first `sm_opendb` on a database in this process opens
the database and its `opt` takes effect. Later sessions on
the same database join the open instance, and their `opt`
is ignored except for fields documented as per-session
(currently none). Recovery — restoring prepared
transactions and their locks — happens during that first
open.

A new session has no active transaction. It belongs to
`env`, which must not be released while the session is
open.

Returns NULL on error: `SM_ERR_NOTFOUND` (no database
there), `SM_ERR_CORRUPT`, `SM_ERR_PERMISSION`,
`SM_ERR_NOMEM`,
`SM_ERR_IO`, `SM_ERR_INVALID`.

### int sm_closedb(sm_session_t* session)

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

### int sm_begin(sm_session_t* session, sm_xctlevel_t level)

Begin a transaction at `level`, and take its snapshot if
the level calls for one.

Errors: `SM_ERR_INTRANS` (a transaction is already active),
`SM_ERR_INVALID` (unknown level).

### int sm_commit(sm_session_t* session)

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

### int sm_abort(sm_session_t* session)

Abort the active transaction, discarding all of its writes.
Releases all locks, discards the savepoints, and closes any
open scan cursors on this session.

Aborting a **prepared** transaction is legal and is the
coordinator's abort decision.

Must not be called after a retryable error, which already
aborted the transaction.

Errors: `SM_ERR_NOTRANS`.

### int sm_stmt_begin(sm_session_t* session)

Begin a statement. Under `SM_XCT_READ_COMMITTED` every row
the statement reads then comes from **one** snapshot,
instead of a fresh one per call.

Without it, each `sm_read` and each `sm_scan` takes its own
snapshot, so a caller whose statement spans several calls —
an index lookup is one scan plus N reads — sees N+1
committed states and can return a mixture of them that never
existed at any instant.

The snapshot is taken by the statement's first **read**, not
by this call and not by a write. A statement may therefore
lock, wait, or write first and still read everything
committed up to the moment it starts reading. The statement
runs until the next `sm_stmt_begin`, which starts another, or
until the transaction ends.

Once called, the transaction reads this way for the rest of
its life; there is no returning to a snapshot per call. Under
`SM_XCT_REPEATABLE_READ` and `SM_XCT_SERIALIZABLE` the call
is accepted and does nothing, since the whole transaction
already reads at one snapshot.

Writes are unaffected: they still check for a newer commit
against the snapshot taken by `sm_begin`, so
`SM_ERR_UPDATED` can still arrive on a row this statement
has just read; `sm_read_latest` then gives the newest version.

Errors: `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`, `SM_ERR_INVALID`.

### int sm_lock(sm_session_t* session, uint64_t storeid, const char* key, size_t klen, sm_lockmode_t mode)

```c
typedef enum {
    SM_LOCK_SHARED    = 0,
    SM_LOCK_EXCLUSIVE = 1
} sm_lockmode_t;
```

Lock a row without writing it: SQL's `SELECT ... FOR SHARE`
and `FOR UPDATE`. The key need not exist.

`SM_LOCK_SHARED` lets other transactions hold the same row
shared but stops any of them writing it.
`SM_LOCK_EXCLUSIVE` excludes every other holder.

The call **blocks** while another transaction holds the row
in a conflicting mode, exactly as a write does, and can fail
with `SM_ERR_DEADLOCK` for the same reason. A transaction
holding the row shared and asking for it exclusive — including
by writing it — **upgrades**, waiting for the other shared
holders to finish. Two transactions upgrading the same row
deadlock, and the younger is the victim.

Once the lock is held, the call runs the same check a write
runs: if another transaction committed a version of this key
after this transaction's snapshot, it fails with
`SM_ERR_CONFLICT`. A successful `sm_lock` therefore means what
the caller reads next under that lock is current, which is
what makes "lock, then read, then act" safe. At
`SM_XCT_READ_COMMITTED` the call does not fail on a newer
commit; the caller reads the row with `sm_read_latest`, which
returns the newest committed version.

The lock is held to the end of the transaction, or until an
`sm_rollback` past the point it was taken. An upgrade is never
undone by a rollback: the row stays locked in the stronger
mode until the transaction ends.

A transaction holding shared locks may be prepared. They
survive in the prepared transaction and are restored after a
crash, so nothing may write those rows until a coordinator
resolves it.

Errors: `SM_ERR_CONFLICT` (not at READ COMMITTED) /
`SM_ERR_DEADLOCK` (retryable), `SM_ERR_NOTFOUND` (no such
store), `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`,
`SM_ERR_INVALID`.

### int sm_savepoint(sm_session_t* session)

Set a savepoint at the current point in the transaction
and return its **depth**: 1 for the first, 2 for one set
inside it, and so on.

Savepoints nest. A transaction may hold up to
`SM_MAX_SAVEPOINTS` (256) of them, and they are all
discarded at commit or abort.

**A depth is reused.** Once the mark holding a depth is
gone — rolled past, or released by `sm_forget` — the next
`sm_savepoint` hands that depth out again, and it then
names a different mark. A caller that keeps savepoints by
name must drop a name as soon as its mark goes, exactly as
SQL requires of `RELEASE` and of rollback to an outer
savepoint. The engine cannot tell a stale depth from a
current one.

Returns the depth (>= 1), or -1 on error.

Errors: `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`,
`SM_ERR_INVALID` (the stack is full).

### int sm_rollback(sm_session_t* session, int depth)

Undo every write made since the savepoint at `depth`. That
savepoint survives, so the same mark may be rolled back to
more than once; the savepoints set inside it are gone. Row
locks acquired since the mark are released; locks held from
before it are kept. The transaction remains active.

**Scan cursors opened after the savepoint are closed**,
and their `sm_scan_t` handles must not be used again; a
cursor opened before the savepoint stays open and keeps
its position. A cursor's view is fixed when it is
created, so one opened after the mark would otherwise go
on returning rows this call just undid.

Requires a savepoint at that depth. To be able to roll a
transaction back to its start, call `sm_savepoint`
immediately after `sm_begin` and keep the depth it
returned.

**Rollback does not undo read tracking.** Under
`SM_XCT_SERIALIZABLE`, reads performed and then rolled back
still count towards serialization checking and can still
cause `SM_ERR_SERIALIZATION` at commit. This is
conservative and safe, never unsafe.

Errors: `SM_ERR_NOSAVEPOINT` (no savepoint at that depth),
`SM_ERR_NOTRANS`, `SM_ERR_PREPARED`.

### int sm_forget(sm_session_t* session, int depth)

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

`sm_prepare` makes a transaction durable but not visible.
Once prepared, a transaction survives a crash: it is
restored at the next `sm_opendb`, still holding its row
locks, and waits for a coordinator decision. It cannot be
told "no" by the engine.

An abandoned prepared transaction blocks writers on its
keys and prevents reclamation of space indefinitely. This
is inherent to 2PC and is exposed here rather than hidden;
`sm_abort_prepared` is the operator escape hatch.

### int sm_prepare(sm_session_t* session, const char* gid)

Prepare the active transaction for commit under the global
transaction id `gid`, a NUL-terminated string of at most
`SM_MAX_GID` bytes, unique among unresolved prepared
transactions in this database.

Runs serialization validation, then durably records the
prepared state. On success the transaction is `PREPARED`:
data operations, savepoints, and rollback are rejected with
`SM_ERR_PREPARED`; only `sm_commit` and `sm_abort` are
accepted. `sm_closedb` leaves it in-doubt rather than
aborting it.

From prepare on, the transaction belongs to the database,
not the session: any session may resolve it by `gid` with
`sm_commit_prepared` or `sm_abort_prepared`. While it is
unresolved, the preparing session's `sm_begin` fails with
`SM_ERR_INTRANS`. Once any session has resolved it, the
preparing session has no transaction: its `sm_commit` and
`sm_abort` fail with `SM_ERR_NOTRANS`, and `sm_begin`
succeeds.

On `SM_ERR_IO` the outcome is **unknown**: the prepared
record may or may not have reached the log. The transaction
is no longer active on the session, and the database enters
the same failed state as after `SM_ERR_IO` from `sm_commit`.
After the database is reopened, the transaction is either
listed by `sm_list_prepared` under `gid`, or it was never
prepared and is aborted. A coordinator that got `SM_ERR_IO`
checks the list; since the participant never voted yes,
`sm_abort_prepared` is always a correct resolution.

Errors: `SM_ERR_SERIALIZATION` (retryable; the transaction
is aborted and *not* prepared), `SM_ERR_NOTRANS`,
`SM_ERR_EXISTS` (`gid` already in use), `SM_ERR_INVALID`,
`SM_ERR_IO` (outcome unknown).

### int sm_list_prepared(sm_session_t* session, sm_prepared_t** out, size_t* n)

List the prepared transactions in this database that have
not yet been resolved, including those restored from before
a crash.

```c
typedef struct {
    const char* gid;
    long long   prepared_at;   /* wall-clock, seconds since epoch */
} sm_prepared_t;
```

On success `*out` points to `*n` entries allocated by the
library; release them with `sm_free_prepared`. `*n` may be
0, in which case `*out` is NULL.

This call does not require an active transaction and does
not start one.

Errors: `SM_ERR_NOMEM`, `SM_ERR_IO`.

### void sm_free_prepared(sm_prepared_t* list, size_t n)

Release a list returned by `sm_list_prepared`. Passing NULL
is a no-op.

### int sm_commit_prepared(sm_session_t* session, const char* gid)
### int sm_abort_prepared(sm_session_t* session, const char* gid)

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

## Rel API

A store is an ordered map from key to value, named by a
`uint64_t` storeid that the library allocates. Relid 0 is the
catalog's own slot, never a store: every call given it
fails with `SM_ERR_INVALID`.

### int sm_mkstore(sm_session_t* session, uint64_t* storeid)

Create a store and hand out its storeid in `*storeid`.
Transactional: visible to others only at commit, and undone
by abort or by a rollback that spans it. A storeid from a
transaction that aborts is simply never used.

A storeid is never reused, so it never names two stores,
and one dropped by `sm_rmstore` never comes back. Storeids rise
in creation order, so the lowest one `sm_lsstore` lists is the
first store the database ever had — which is how a
program finds its own catalog store again after a
restart.

Errors: `SM_ERR_NOTRANS`, `SM_ERR_CONFLICT` (retryable),
`SM_ERR_INVALID` (NULL `storeid`), `SM_ERR_PREPARED`,
`SM_ERR_DISKFULL`, `SM_ERR_IO`.

### int sm_rmstore(sm_session_t* session, uint64_t storeid)

Drop a store and all of its rows. Transactional, as
`sm_mkstore`.

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

### int sm_lsstore(sm_session_t* session, uint64_t** out, size_t* n)

List the storeids of the stores the transaction sees, in
ascending order. It reads the catalog as `sm_scan` reads a
store: at the transaction's snapshot, including its own
uncommitted DDL. Under `SM_XCT_SERIALIZABLE` the whole list
is recorded as read, so a concurrent `sm_mkstore` or
`sm_rmstore` is a read-write conflict.

On success `*out` points to `*n` storeids in one `malloc`
block; release it with `free()`. `*n` may be 0, in which case
`*out` is NULL.

Errors: `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`,
`SM_ERR_INVALID`, `SM_ERR_NOMEM`, `SM_ERR_CORRUPT`,
`SM_ERR_IO`.

---

## Sequence API

A sequence name is a NUL-terminated string of 1 to
`SM_MAX_SEQNAME` (63) bytes, otherwise unrestricted, like a
database name. A name outside those bounds fails with
`SM_ERR_INVALID`.

A sequence is a named counter belonging to a database. It
hands out `start`, `start + increment`, `start + 2 *
increment`, and so on, and it is the way a caller gets ids
of its own — row ids, index ids, anything that must be
unique across restarts.

```c
int sm_mkseq(sm_session_t* session, const char* name,
             uint64_t start, uint64_t increment);
int sm_rmseq(sm_session_t* session, const char* name);
int sm_nextseq(sm_session_t* session, const char* name,
               uint64_t* out);
```

`sm_mkseq` and `sm_rmseq` are transactional, like `sm_mkstore`
and `sm_rmstore`: they need an active transaction, take effect
at commit, and roll back with it. The name is a
NUL-terminated string of at most `SM_MAX_SEQNAME` bytes, and
`increment` must not be 0. A name used again after a drop is
a new sequence, starting at its own `start`.

The **values** are not transactional, like a PostgreSQL
sequence: `sm_nextseq` needs no active transaction, never
blocks, and an abort does not give a value back. Values are
unique but not dense: besides aborts, a crash skips the rest
of a block reserved in advance (the `ts_lease` size),
leaving a gap. Distinct sessions may call it concurrently.

Inside a transaction, `sm_nextseq` sees that transaction's
snapshot, so a sequence it created itself is usable before
it commits. When the next value would pass `UINT64_MAX`, the
sequence is exhausted and every further call fails with
`SM_ERR_INVALID`.

Errors for `sm_mkseq`: `SM_ERR_EXISTS`, `SM_ERR_NOTRANS`,
`SM_ERR_CONFLICT` (retryable), `SM_ERR_INVALID`,
`SM_ERR_PREPARED`, `SM_ERR_DISKFULL`, `SM_ERR_IO`. For
`sm_rmseq`: `SM_ERR_NOTFOUND` and the same otherwise. For
`sm_nextseq`: `SM_ERR_NOTFOUND` (no such sequence),
`SM_ERR_INVALID` (NULL argument, bad name, or exhausted),
`SM_ERR_PREPARED`, `SM_ERR_CORRUPT`, `SM_ERR_IO` (the
database has stopped, or persisting the next block failed),
`SM_ERR_DISKFULL`.

---

## Update API

All three require an active, non-prepared transaction. All
three can fail with `SM_ERR_CONFLICT` at REPEATABLE READ and
SERIALIZABLE, and `sm_update` and `sm_delete` with
`SM_ERR_UPDATED` at READ COMMITTED, when another transaction
committed a change to the same key after this transaction's
snapshot.

Each blocks while another active transaction holds a write
lock on the same key, until that transaction ends. A
blocked call can return `SM_ERR_DEADLOCK` if it is selected
as the victim of a deadlock cycle.

```c
int sm_insert(sm_session_t* session, uint64_t storeid,
              const char* key, size_t klen,
              const char* data, size_t datalen);

int sm_update(sm_session_t* session, uint64_t storeid,
              const char* key, size_t klen,
              const char* data, size_t datalen,
              const char** old, size_t* oldlen);

int sm_delete(sm_session_t* session, uint64_t storeid,
              const char* key, size_t klen,
              const char** old, size_t* oldlen);
```

- `sm_insert` requires that the key not exist, and fails
  with `SM_ERR_DUPKEY` if it does.
- `sm_update` requires that the key exist, and fails with
  `SM_ERR_NOTFOUND` if it does not. It replaces the value
  entirely; there is no partial update.
- `sm_delete` requires that the key exist, and fails with
  `SM_ERR_NOTFOUND` if it does not.

There is no upsert. A caller wanting one attempts
`sm_insert` and falls back to `sm_update` on
`SM_ERR_DUPKEY`.

**The old row.** `sm_update` and `sm_delete` return the row
they replaced through `old` / `oldlen`. A caller maintaining
secondary indexes needs it, to remove the entries the old
value produced; pass NULL for both to skip it. They are a
pair, and one without the other is `SM_ERR_INVALID`. The
write already reads that row to check the key exists, so
asking for it costs a copy, not another seek.

`*old` points into session-owned memory and is valid only
until the next `sm_*` call on this session — the same rule
as `sm_read`, and the same buffer, so either call
invalidates what the other returned. Copy it to keep it. On
failure, and for a zero-length old value, `*old` may be
NULL.

Errors for all three: `SM_ERR_CONFLICT` /
`SM_ERR_DEADLOCK` (retryable), `SM_ERR_UPDATED` (READ
COMMITTED; not retryable, the transaction stays active; call
`sm_read_latest`, then write again), `SM_ERR_NOTFOUND` (store
or, for update/delete, key), `SM_ERR_DUPKEY` (insert),
`SM_ERR_NOTRANS`, `SM_ERR_PREPARED`, `SM_ERR_INVALID`,
`SM_ERR_DISKFULL`, `SM_ERR_IO`.

---

## Read API

Reads never block and never take locks; `sm_lock` is how a
caller takes one deliberately. They cannot return
`SM_ERR_CONFLICT` or `SM_ERR_DEADLOCK`. Under
`SM_XCT_SERIALIZABLE` a read is recorded, and may
contribute to an `SM_ERR_SERIALIZATION` failure later, at
commit.

### int sm_read(sm_session_t* session, uint64_t storeid, const char* key, size_t klen, const char** data, size_t* datalen)

Read one key.

Returns **0** if the key exists — `*data` and `*datalen`
then describe the value, subject to the lifetime rules
above. Returns **1** if the key does not exist, leaving
`*data` NULL and `*datalen` 0; this is not an error and
does not set the error state. Returns **-1** on error.

Errors: `SM_ERR_NOTFOUND` (the *store* does not exist —
a missing key returns 1, not an error), `SM_ERR_NOTRANS`,
`SM_ERR_PREPARED`, `SM_ERR_INVALID`, `SM_ERR_CORRUPT`,
`SM_ERR_IO`.

### int sm_read_latest(sm_session_t* session, uint64_t storeid, const char* key, size_t klen, const char** data, size_t* datalen)

Read the newest committed version of one key, ignoring the
snapshot. Returns as `sm_read` does: **0** found, **1** no
such key (or deleted), **-1** on error.

It exists for `SM_XCT_READ_COMMITTED`, and needs a row lock
the transaction already holds on the key: the one a write that
failed with `SM_ERR_UPDATED` left behind, or one taken with
`sm_lock`. The lock is what keeps the version from changing
before commit. At any other level, or with no lock held, it
fails with `SM_ERR_INVALID`.

Under an exclusive lock the call also lets the next write of
this key go ahead, once. Under a shared lock it reads but
lets nothing through, since a shared holder cannot write
without upgrading. The write clears the permission, so a
second write after a savepoint rollback needs a fresh read.

Typical use, for `UPDATE ... WHERE`:

```c
rc = sm_update(s, store, key, klen, newval, len, NULL, NULL);
if (rc != 0 && sm_errno() == SM_ERR_UPDATED) {
    sm_read_latest(s, store, key, klen, &data, &n);
    /* re-check the WHERE clause; recompute newval from data */
    rc = sm_update(s, store, key, klen, newval, len, NULL, NULL);
}
```

Errors: `SM_ERR_NOTFOUND` (the *store* does not exist),
`SM_ERR_NOTRANS`, `SM_ERR_PREPARED`, `SM_ERR_INVALID`
(not READ COMMITTED, or no lock held), `SM_ERR_CORRUPT`,
`SM_ERR_IO`.

### sm_scan_t* sm_scan(sm_session_t* session, uint64_t storeid, const char* skey, size_t sklen, int skey_inclusive, const char* ekey, size_t eklen, int ekey_inclusive)

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
because under `SM_XCT_SERIALIZABLE` the range is the
predicate that is tracked for conflicts. Scanning to the
end of the store and discarding rows in the caller
records a far larger predicate and causes needless
serialization failures.

Scanning is **forward only**. Descending iteration is not
supported, and is not a planned addition: no index serves a
backward scan (`OBJECTIVE.md`). A caller that needs
descending order keeps a second store with inverted key
bytes and scans that one forward.

The cursor's view is fixed at this call (see "Isolation and
visibility semantics", point 3). Its snapshot also pins
storage reclamation, so a cursor left open indefinitely
prevents space from being reclaimed; close cursors
promptly.

The cursor is owned by the session, must be used only from
the thread using that session, and is invalidated by
`sm_commit`, `sm_abort`, and `sm_closedb`. Multiple cursors
may be open on one session at once.

Returns NULL on error: `SM_ERR_NOTFOUND` (no such
store), `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`,
`SM_ERR_INVALID`, `SM_ERR_NOMEM`, `SM_ERR_IO`.

### int sm_scannxt(sm_scan_t* scan, const char** key, size_t* klen, const char** data, size_t* datalen)

Advance to the next row.

Returns **0** if a row was produced — the four out-params
then describe it, valid until the next call on this cursor.
Returns **1** at end of range, leaving the out-params NULL
and 0; this is not an error and does not set the error
state. Returns **-1** on error.

Once it has returned 1 or -1, further calls on the cursor
return the same thing.

Errors: `SM_ERR_CORRUPT`, `SM_ERR_IO`.

### int sm_scan_seek(sm_scan_t* scan, const char* key, size_t klen, int inclusive)

Move an open cursor to `key`, so the next `sm_scannxt`
returns from there. This is how a caller probes a store
repeatedly — an index nested-loop join, an IN-list, a skip
scan — without closing and reopening a cursor per probe.

`inclusive` says whether a row at `key` itself is returned.

The cursor keeps its snapshot and its view, so a reposition
does **not** see writes made since `sm_scan`, and it keeps
its bounds: it returns rows only from the range `sm_scan`
was given.

Seeking moves **forward only**. A key before the last one
the cursor read fails with `SM_ERR_INVALID`, as does a key
below the scan's start key. A key past the scan's end key
is allowed and leaves the cursor at its end, where
`sm_scannxt` returns 1.

Under `SM_XCT_SERIALIZABLE` the keys skipped over still
count as read: the cursor's predicate covers its start key
through the furthest key it reached, gaps included. That is
an over-approximation, so it can only cause a serialization
failure that was not strictly necessary, never miss one.

Errors: `SM_ERR_INVALID` (a backward seek, a key below the
start key, or a cursor that already failed).

### void sm_scanend(sm_scan_t* scan)

Close a cursor and release its resources, including the
snapshot it was pinning. Passing NULL is a no-op.

Under `SM_XCT_SERIALIZABLE`, this is where the range the
cursor actually traversed is registered for conflict
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
Each is a decision, not a detail, and `DESIGN.md` §10.3 and
§12.1 should be reconciled with the ones marked (D).

1. **`sm_errno_t` became reachable, and both accessors are
   now functions.** (D) The old file exposed no way to read
   the code, only a message string, and callers cannot
   implement the retry contract without a machine-readable
   one. The names are unchanged, but `sm_errno` and
   `sm_errmsg` are now calls rather than the old
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
   0/-1 everywhere, then documented `sm_rmdb` as returning
   true/false, and had two calls that need three outcomes.
3. **`sm_read`/`sm_scannxt` use a tri-state return**
   rather than `DESIGN.md`'s separate `int* found`. One
   idiom for "no row" across both calls. (D)
4. **The `schema` parameter is gone** from the read calls.
   No such concept exists anywhere else in the API.
5. **Lengths are `size_t`,** and every byte-string
   parameter has one. The old `sm_read`/`sm_scan` had none
   at all and could not carry binary keys or values. (D)
6. **2PC entry points added** — `gid` on `sm_prepare`, plus
   `sm_list_prepared` / `sm_free_prepared` /
   `sm_commit_prepared` / `sm_abort_prepared`. Without
   them a prepared transaction that survives a restart is
   unreachable forever.
7. **`sm_options_t` added.** `DESIGN.md` §12.2 specifies
   configuration knobs, including one (`durability_sync`)
   that changes the durability guarantee, and there was no
   surface to set any of them.
8. **`sm_rollback` takes the depth of the savepoint to
   undo to**, and requires a savepoint there instead of
   silently rolling back to `sm_begin` when none is set.
   The old behaviour makes one call mean either "undo the
   last few writes" or "undo everything" depending on
   invisible state. `sm_savepoint` right after `sm_begin`
   recovers the old capability explicitly. (D)
9. **`sm_scan` gained an end key** (required for
   serializable predicate tracking) and an inclusivity flag
   for it.
10. **`sm_scanend` returns void.** A release call that can
    fail gives the caller nothing to do about it. (D)
11. **Types are `sm_`-prefixed and opaque** —
    `sm_session_t`, `sm_scan_t` — rather than occupying the
    global names `session_t` and `scan_t`.
12. **Preconditions, error sets, lifetimes, threading, key
    ordering, and the three surprising isolation
    behaviours are now stated.** They were decided in
    `DESIGN.md` but invisible to a caller reading only the
    API.
13. `SM_XCT_READ_COMMITED` → `SM_XCT_READ_COMMITTED`.
14. **Databases are named within an environment.** (D)
    `sm_attach(homedir)` returns an `sm_env_t`, and
    `sm_mkdb`, `sm_rmdb`, and `sm_opendb` take an env and a
    single-component `dbname` instead of a `dbpath`. The
    env is thread-safe and separate from the per-thread
    session, so concurrent sessions keep working as
    before. It gives the registry of open
    databases an owner, and a place for resources shared
    across databases. Process exclusion is the caller's:
    one process attaches a `homedir`, so `sm_rmdb` can see
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
    `sm_lsdb` comes with it, because the directory names
    stop being readable.
16. **A list of names is an `sm_strlist_t`.** (D) `sm_lsdb`
    returns one counted, immutable handle rather than a
    `char***` with a paired `size_t*`, and `sm_free_dbs` is
    gone. The count travels with the array, so the two
    cannot disagree; an index past the end returns NULL;
    and one call releases the whole list. Elements are
    NUL-terminated, so a list carries names only — keys and
    values may contain NUL, and a call that returns those
    will need a counted-buffer type instead.
17. **Preparing a home directory is separate from using
    one.** (D) `sm_init` creates the structure once and
    returns an `int`; `sm_attach` / `sm_detach` bracket a
    process's use of it and replace `sm_final`. The old
    `sm_init` did both, which forced it to create `global`
    and `base` when absent — so it could not tell a
    directory that is not a wisr home from one that is
    brand new, and a mistyped path became an empty home
    rather than an error. The split is `initdb` against
    starting a server, it leaves `sm_attach` with no
    creation path at all, and it gives the format version
    somewhere to be written and checked
    (`SM_ERR_VERSION`).
18. **`sm_detach` takes a `force` flag.** (D) Shutdown
    otherwise has to walk the caller's own list of open
    sessions and close each one before the environment can
    go, which every caller writes identically and which a
    caller winding down after an error may not be able to
    do at all. `force` does not change what closing a
    session means — `sm_closedb`'s rules apply unchanged, so
    a prepared transaction still survives.
19. **Savepoints nest, and `sm_forget` releases one.** (D)
    The sketch gave a session one savepoint. A layer that
    wraps each of its own statements in a savepoint cannot
    then offer SQL savepoints to its caller, since the two
    uses collide on the single mark. `sm_savepoint` now
    returns a depth, `sm_rollback` takes one, and
    `sm_forget` is `RELEASE SAVEPOINT`. `SECINDEX.md` gap 2
    is the case that asked for it.

20. **`sm_lock` added.** (D) Locking a row without writing it
    is how a layer does `SELECT ... FOR UPDATE`, and its
    shared mode is what lets a secondary-index builder wait
    out the writers that cannot yet know the index exists.
    `SECINDEX.md` gap 1 is the case that asked for it; it was
    the only gap there that made correct behaviour impossible
    rather than merely awkward.

21. **`sm_scan_seek` added.** (D) Repositioning a cursor is
    what an index nested-loop join, an IN-list, and a skip
    scan all need; without it each probe closes and reopens
    a cursor, and under READ COMMITTED every reopen takes a
    new snapshot as well. `SECINDEX.md` gap 5.

22. **`sm_update` and `sm_delete` return the row they
    replaced.** (D) The write already reads it to check the
    key exists, so a layer maintaining secondary indexes
    would otherwise pay a second seek and a copy for
    something the engine had in hand. `SECINDEX.md` gap 6.
