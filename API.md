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
  functions have one: `sm_read` and `sm_scan_next` both
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
    SM_ERR_NOSAVEPOINT   = 5,   /* rollback with no savepoint set */

    /* data errors */
    SM_ERR_NOTFOUND      = 6,   /* relation or key does not exist */
    SM_ERR_DUPKEY        = 7,   /* insert onto a live key */
    SM_ERR_EXISTS        = 8,   /* home dir, database, or relation exists */
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
    SM_ERR_VERSION       = 18   /* home dir format not supported */
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

Byte strings passed *in* (`key`, `data`, `relname`,
`homedir`, `dbname`, `gid`) are read during the call and
never retained. The caller may free or reuse them immediately
afterwards.

Byte strings handed *out* by `sm_read` and `sm_scan_next`
point into engine-owned memory. They are **not**
NUL-terminated and must not be freed. Their validity
windows differ and are narrow:

| Producer | Buffer stays valid until |
| --- | --- |
| `sm_read` | the next `sm_*` call on that **session** |
| `sm_scan_next` | the next call on that **scan cursor** |

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
void        sm_strlist_free(sm_strlist_t* sl);
```

On success the out-param is never NULL; an empty result is
a list whose count is 0. `sm_strlist_at` returns a
NUL-terminated string owned by the list and valid until
`sm_strlist_free`, or NULL for an index past the end. The
caller releases the whole list with one call and frees no
element on its own. The count travels inside the handle, so
it cannot disagree with the array it describes.

A list holds no engine state — no lock, no snapshot, no
session reference — so one that is never freed leaks memory
and nothing else. An `sm_scan_t` left open is a different
matter; see `sm_scan_end`.

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

**2. Writes can fail with `SM_ERR_CONFLICT` at every
level, including READ COMMITTED.** If another transaction
committed a change to the same key after this
transaction's snapshot, the write is rejected. The engine
does not silently re-read the newer row and apply the write
to it; it reports the conflict and the caller retries. This
differs from engines that restart the statement.

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

DDL is transactional. `sm_mkrel` and `sm_rmrel` take effect
at commit and roll back with the transaction.

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
each exactly as `sm_close` would close it: cursors are
closed and an active transaction is aborted, but a
**prepared** transaction is left in doubt for its
coordinator, to be found again by `sm_list_prepared` after
the next `sm_open`. Their `sm_session_t` handles are
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
`sm_open` afterwards.

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
`sm_strlist_free`.

A database directory is named by number, so the names
exist only in the root db and this is the only way to
enumerate them; reading `homedir` will not tell a caller
what is there.

Errors: `SM_ERR_NOMEM`, `SM_ERR_IO`.

---

## Session API

### sm_session_t* sm_open(sm_env_t* env, const char* dbname, const sm_options_t* opt)

Open the database `dbname` and start a new session on it.
The database must already exist; `sm_open` never creates
one.

The first `sm_open` on a database in this process opens
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

### int sm_close(sm_session_t* session)

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
and visible. Releases all locks, discards the savepoint,
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
Releases all locks, discards the savepoint, and closes any
open scan cursors on this session.

Aborting a **prepared** transaction is legal and is the
coordinator's abort decision.

Must not be called after a retryable error, which already
aborted the transaction.

Errors: `SM_ERR_NOTRANS`.

### int sm_savepoint(sm_session_t* session)

Set a savepoint at the current point in the transaction.

A transaction may hold **one** savepoint. Calling
`sm_savepoint` again replaces the existing one; the earlier
mark is gone and cannot be returned to. The savepoint is
discarded at commit or abort.

Errors: `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`.

### int sm_rollback(sm_session_t* session)

Undo every write made since the savepoint. The savepoint
survives, so the same mark may be rolled back to more than
once. Row locks acquired since the mark are released; locks
held from before it are kept. The transaction remains
active.

**Scan cursors opened after the savepoint are closed**,
and their `sm_scan_t` handles must not be used again; a
cursor opened before the savepoint stays open and keeps
its position. A cursor's view is fixed when it is
created, so one opened after the mark would otherwise go
on returning rows this call just undid.

Requires a savepoint. To be able to roll a transaction back
to its start, call `sm_savepoint` immediately after
`sm_begin`.

**Rollback does not undo read tracking.** Under
`SM_XCT_SERIALIZABLE`, reads performed and then rolled back
still count towards serialization checking and can still
cause `SM_ERR_SERIALIZATION` at commit. This is
conservative and safe, never unsafe.

Errors: `SM_ERR_NOSAVEPOINT`, `SM_ERR_NOTRANS`,
`SM_ERR_PREPARED`.

---

## Two-phase commit API

`sm_prepare` makes a transaction durable but not visible.
Once prepared, a transaction survives a crash: it is
restored at the next `sm_open`, still holding its row
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
accepted. `sm_close` leaves it in-doubt rather than
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

A relation is an ordered map from key to value. Relation
names are NUL-terminated strings of at most `SM_MAX_RELNAME`
bytes.

### int sm_mkrel(sm_session_t* session, const char* relname)

Create a relation. Transactional: visible to others only at
commit, and undone by abort or by a rollback that spans it.

Errors: `SM_ERR_EXISTS`, `SM_ERR_NOTRANS`,
`SM_ERR_CONFLICT` (retryable — concurrently created),
`SM_ERR_INVALID`, `SM_ERR_PREPARED`.

### int sm_rmrel(sm_session_t* session, const char* relname)

Drop a relation and all of its rows. Transactional, as
`sm_mkrel`.

Space is not reclaimed at commit — transactions holding an
older snapshot may still legitimately read the relation.
Physical removal happens in the background once no snapshot
can see it.

Errors: `SM_ERR_NOTFOUND`, `SM_ERR_NOTRANS`,
`SM_ERR_CONFLICT` (retryable), `SM_ERR_PREPARED`.

---

## Update API

All three require an active, non-prepared transaction, and
all three can fail with `SM_ERR_CONFLICT` at every
isolation level.

Each blocks while another active transaction holds a write
lock on the same key, until that transaction ends. A
blocked call can return `SM_ERR_DEADLOCK` if it is selected
as the victim of a deadlock cycle.

```c
int sm_insert(sm_session_t* session, const char* relname,
              const char* key, size_t klen,
              const char* data, size_t datalen);

int sm_update(sm_session_t* session, const char* relname,
              const char* key, size_t klen,
              const char* data, size_t datalen);

int sm_delete(sm_session_t* session, const char* relname,
              const char* key, size_t klen);
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

Errors for all three: `SM_ERR_CONFLICT` /
`SM_ERR_DEADLOCK` (retryable), `SM_ERR_NOTFOUND` (relation
or, for update/delete, key), `SM_ERR_DUPKEY` (insert),
`SM_ERR_NOTRANS`, `SM_ERR_PREPARED`, `SM_ERR_INVALID`,
`SM_ERR_DISKFULL`, `SM_ERR_IO`.

---

## Read API

Reads never block and never take locks. They cannot return
`SM_ERR_CONFLICT` or `SM_ERR_DEADLOCK`. Under
`SM_XCT_SERIALIZABLE` a read is recorded, and may
contribute to an `SM_ERR_SERIALIZATION` failure later, at
commit.

### int sm_read(sm_session_t* session, const char* relname, const char* key, size_t klen, const char** data, size_t* datalen)

Read one key.

Returns **0** if the key exists — `*data` and `*datalen`
then describe the value, subject to the lifetime rules
above. Returns **1** if the key does not exist, leaving
`*data` NULL and `*datalen` 0; this is not an error and
does not set the error state. Returns **-1** on error.

Errors: `SM_ERR_NOTFOUND` (the *relation* does not exist —
a missing key returns 1, not an error), `SM_ERR_NOTRANS`,
`SM_ERR_PREPARED`, `SM_ERR_INVALID`, `SM_ERR_CORRUPT`,
`SM_ERR_IO`.

### sm_scan_t* sm_scan(sm_session_t* session, const char* relname, const char* skey, size_t sklen, int skey_inclusive, const char* ekey, size_t eklen, int ekey_inclusive)

Open a cursor over the keys of `relname` in ascending
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
end of the relation and discarding rows in the caller
records a far larger predicate and causes needless
serialization failures.

Scanning is **forward only**. Descending iteration is not
supported.

The cursor's view is fixed at this call (see "Isolation and
visibility semantics", point 3). Its snapshot also pins
storage reclamation, so a cursor left open indefinitely
prevents space from being reclaimed; close cursors
promptly.

The cursor is owned by the session, must be used only from
the thread using that session, and is invalidated by
`sm_commit`, `sm_abort`, and `sm_close`. Multiple cursors
may be open on one session at once.

Returns NULL on error: `SM_ERR_NOTFOUND` (no such
relation), `SM_ERR_NOTRANS`, `SM_ERR_PREPARED`,
`SM_ERR_INVALID`, `SM_ERR_NOMEM`, `SM_ERR_IO`.

### int sm_scan_next(sm_scan_t* scan, const char** key, size_t* klen, const char** data, size_t* datalen)

Advance to the next row.

Returns **0** if a row was produced — the four out-params
then describe it, valid until the next call on this cursor.
Returns **1** at end of range, leaving the out-params NULL
and 0; this is not an error and does not set the error
state. Returns **-1** on error.

Once it has returned 1 or -1, further calls on the cursor
return the same thing.

Errors: `SM_ERR_CORRUPT`, `SM_ERR_IO`.

### void sm_scan_end(sm_scan_t* scan)

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
3. **`sm_read`/`sm_scan_next` use a tri-state return**
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
8. **`sm_rollback` now requires a savepoint** instead of
   silently rolling back to `sm_begin` when none is set.
   The old behaviour makes one call mean either "undo the
   last few writes" or "undo everything" depending on
   invisible state. `sm_savepoint` right after `sm_begin`
   recovers the old capability explicitly. (D)
9. **`sm_scan` gained an end key** (required for
   serializable predicate tracking) and an inclusivity flag
   for it.
10. **`sm_scan_end` returns void.** A release call that can
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
    `sm_mkdb`, `sm_rmdb`, and `sm_open` take an env and a
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
    session means — `sm_close`'s rules apply unchanged, so
    a prepared transaction still survives.
