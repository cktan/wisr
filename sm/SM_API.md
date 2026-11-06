# SM_API: the sm.h interface

`sm.h` is the one entry into the storage stack. It is C in style
and compiled as C++. digres calls it, and the `rel` test driver
calls it too. The declarations are in `include/sm.h` and the code
is in `src/sm.cc`. `include/sm_error.h`, `sm_lock.h`,
`sm_options.h` and `sm_stats.h` hold its error codes, lock modes,
options and statistics. Each call converts its arguments, calls
the sm1, sm2 or sm3 call of the same name, and converts the
results back. The wrapper adds no concept of its own: it keeps
each database's sm3 catalog and each session's sm3 session.
`sm1/API.md` is the contract of the level below.

## Conventions

These rules hold for every entry. An entry states only what
differs.

### Words

- **host**: the caller of `sm.h`, which is digres or `rel`.
- **tuple**: the bytes of a row, as sm2 lays them out.
- **keyless table**: a table without a primary key.
- **`_id`**: the number that names a row of a keyless table. A
  row of a table with a primary key has no `_id`, and a call that
  hands one out gives 0.
- **row key**: the encoded key of a row in its table, past the
  2-byte relid (`ROWSTORE.md` §2.1).
- **LSM index**: an index whose entries are sm1 keys
  (`LSMINDEX.md`).
- **tree**: a B-tree index, an index of its own pages
  (`BTREEINDEX.md`).
- **table-oid lock**: the lock on a table's oid. `SM_LOCKMODE_EX`
  on it is "X".

### Return values

- A call that returns `int` returns 0 on success and -1 on error.
  An entry that says nothing about its return value returns
  these.
- A positive return is not an error. It is 1, and the entry says
  what 1 means: no such row or object, the end of a cursor, a try
  call that would have waited, or a key that does not give its
  columns back.
- `sm_savepoint` returns a depth, 1 or more, on success.
- A call that yields a handle hands it back through an
  out-parameter, set only on success. It is the last parameter of
  every such call but `sm_attach`.
- A call that fills a `std::vector` replaces its contents on
  success.
- A call that returns no `int` cannot report an error, but for
  `sm_tbl_def`.

### The error state

The error state is per thread. A failing call sets it, and a
successful call leaves it as it was. Read it only after a call
that reports an error.

Entries list the errors a caller can act on. They leave out three
groups that apply throughout:

- Any call that reads or writes storage can fail with
  `SM_ERR_IO`, `SM_ERR_CORRUPT`, `SM_ERR_DISKFULL`,
  `SM_ERR_PERMISSION` or `SM_ERR_NOFDS`.
- A call that needs a transaction can fail with `SM_ERR_NOTRANS`,
  `SM_ERR_PREPARED` or `SM_ERR_DOOMED`.
- A call that can wait for a lock or a writer can fail with
  `SM_ERR_DEADLOCK`, `SM_ERR_BLOCKED` or `SM_ERR_INTERRUPTED`.

What an error leaves behind:

| Code | Meaning | The transaction |
| --- | --- | --- |
| `SM_ERR_CONFLICT`, `SM_ERR_SERIALIZATION`, `SM_ERR_DEADLOCK` | retryable: `sm_retryable` returns 1 | the engine has aborted it; begin a new one and redo the work |
| `SM_ERR_UPDATED` | a newer commit, at READ COMMITTED | active; read the row with `sm_tbl_read_latest`, then write again |
| `SM_ERR_SELFMODIFIED` | the statement already changed this row | active; nothing is written; skip the row |
| `SM_ERR_BLOCKED` | would wait, under `sm_set_qa_nowait` or in a call that does not wait | active; repeat the call |
| `SM_ERR_READONLY` | a write in a transaction begun read-only | active; nothing is written |
| `SM_ERR_INTERRUPTED` | an interrupt to the thread ended a wait | active; it holds what it held |
| `SM_ERR_SCANCLOSED` | a rollback closed the cursor | active; end the cursor |
| `SM_ERR_MOVED` | the newest version of the key is a move's tombstone | active; the row lives under another key |
| `SM_ERR_UNDOLIMIT`, `SM_ERR_DOOMED` | a rollback to a savepoint failed | only `sm_abort` works |
| `SM_ERR_NOMEM`, `SM_ERR_EXCEPTION` | the catch-all, below | aborted, if the call gave the catch-all its session |
| any other | a refused call | active and unchanged |

`sm_commit` and `sm_prepare` have rules of their own, in their
entries.

**The catch-all.** Every call that returns `int`, and
`sm_tbl_def`, runs inside one catch-all:

1. `std::bad_alloc` becomes `SM_ERR_NOMEM`.
2. Any other exception becomes `SM_ERR_EXCEPTION`, which means a
   bug.
3. The call returns -1, and no exception crosses the interface.
4. After either code, from an exception or from below, the
   catch-all aborts the session's transaction if it has a
   session. It leaves a prepared transaction in doubt. The error
   stays as it was.

Some calls give the catch-all no session, because they write
nothing. Their transaction stays active after either code. These
calls are the cursor calls (`sm_rel_*`, `sm_idx_*`),
`sm_pin_deform`, `sm_pin_decode`, `sm_pin_tuple`,
`sm_tbl_deform`, `sm_tbl_decode`, `sm_tbl_idx_deform`,
`sm_session_tag`, `sm_set_deadlock_ms`, `sm_cat_index_wait`, and
every call on an environment. A call that returns no `int` has no
catch-all, but `sm_tbl_def`.

`sm_attach` installs a new-handler unless the process has one.
After it, a failed allocation ends the process.

### Handles and their lifetimes

| Handle | Made by | Ended by | Good for |
| --- | --- | --- | --- |
| `sm_env_t` | `sm_attach` | `sm_detach` | the attached home directory, and each database's catalog |
| `sm_session_t` | `sm_opendb` | `sm_closedb`, or a forced `sm_detach` | one session on one database |
| `sm_tbl_t` | `sm_tbl_open`, `sm_tbl_tryopen` | `sm_tbl_close` | one transaction; close it before or after that transaction ends |
| `sm_rel_scan_t` | `sm_tbl_scan`, `sm_tbl_scan_rows`, `sm_tbl_idx_scan_rows` | `sm_rel_scanend` | one transaction; end it before that transaction ends |
| `sm_idx_scan_t` | `sm_tbl_idx_scan` | `sm_idx_scanend` | one transaction; end it before that transaction ends |
| `sm_stmt_t` | `sm_stmt_begin_inner`, `sm_stmt_resume`, `sm_stmt_save` | `sm_stmt_end`, or `sm_stmt_free` for a saved view | one transaction |
| `sm_collation_t` | `sm_collation_open` | `sm_collation_close` | no session or environment |

The end of a transaction closes its cursors and frees sm1's
cursor under each handle. End each cursor first.

A handle given to a call must not be NULL. Some calls check and
fail with `SM_ERR_INVALID`, and the others read through it. These
calls take NULL and do nothing: `sm_detach`, `sm_closedb`,
`sm_unpin`, `sm_release_all_pins`, `sm_stmt_free`,
`sm_collation_close`, `sm_rel_scanend`, `sm_idx_scanend` and
`sm_tbl_close`.

### Memory

Names are NUL-terminated strings, and data is a pointer and a
length. The stack reads both during the call and does not keep
them. A NULL name is the empty name, which most calls refuse with
`SM_ERR_INVALID`. The stack keeps only the `arg` of `sm_set_log`
and the `ctx` of `sm_set_index_evaluator`.

A row or an index entry handed out points into engine memory. It
has no NUL terminator, and the caller does not free it. How long
it stays valid depends on what handed it out, so a caller can use
it in place and copy only what it keeps:

| Handed out by | Stays valid until |
| --- | --- |
| a cursor: `sm_rel_scannxt`, `sm_idx_scannxt` | the next call on that cursor, its close, or the end of its transaction; for `sm_rel_scannxt`, also `sm_release_all_pins` |
| a point read: `sm_tbl_read`, `sm_tbl_read_latest` without a ref | the next call on the same session |
| a read into a pin ref, or the old row of a write | the pin's release: the next read into the same ref, `sm_unpin`, `sm_release_all_pins`, `sm_prepare`, or the end of the transaction |
| a cursor into a pin ref: `sm_rel_scannxt` with a ref | as the cursor's row: the ref goes stale with it |
| `sm_tbl_deform`, `sm_tbl_decode`, `sm_tbl_idx_deform`, `sm_pin_deform`, `sm_pin_decode` | the life of the row given to them |
| `sm_rel_scan_cols`, `sm_idx_scan_key` | the next call on the cursor |

Other calls on the session, on its tables and on its other
cursors leave a cursor's row alone. A cursor's current row
outlives the inner side of a join, a heap fetch and an insert.

A `std::vector` or a `std::string` a call fills is the caller's
own. The caller frees nothing by hand but the handles above.

### Threading

An environment may be used from any number of threads. A session,
its tables and its cursors may be used by one thread at a time.
`sm_waits_for` and `sm_lock_status` may run on any thread, for
any session of the environment. The log hook and the evaluator
may run on the stack's own threads.

### What may cross the interface

- The interface is C-like functions. Their parameters may be C++
  references, and some have default arguments and overloads.
- The standard library types in `sm.h` are `std::string`,
  `std::vector` and `std::shared_ptr`. `std::shared_ptr` appears
  in `sm_tbl_stats` alone. `sm/CODE.md` §2 also allows
  `std::string_view`, which no call in `sm.h` takes.
- `std::string` and `std::vector` carry owned data.
- A callback the host gives is `noexcept`: `sm_log_fn`,
  `sm_index_eval_fn`, `sm_column_conv_fn`. The compiler refuses
  one not declared so, and one that throws all the same ends the
  process.
- No C++ exception crosses the interface.

## Build

### `sm_build_flags`

```cpp
const char* sm_build_flags(void);
```

Returns the compiler flags sm was built with. It returns one
string if sm0, sm1, sm2, sm3 and the wrapper agree, such as `-O3
-DNDEBUG`. Otherwise it returns each one's flags, as `sm0 -O3;
...; wrapper -O1 -g`. The string is static.

## Errors

The error state is level 0's, thread-local (`sm0/README.md`,
`DESIGN.md` §5). `include/sm_error.h` holds the codes.

### `sm_errnum`

```cpp
sm_errnum_t sm_errnum(void);
```

Returns the last error raised on the calling thread.

### `sm_errmsg`

```cpp
const char* sm_errmsg(void);
```

Returns the last error's message. It is never NULL, and stays
valid until the thread's next error.

### `sm_errname`

```cpp
const char* sm_errname(void);
```

Returns the last error's code as a static string, without the
`SM_` prefix: `ERR_UPDATED`, or `OK`.

### `sm_retryable`

```cpp
int sm_retryable(void);
```

Returns 1 if the last error is `SM_ERR_CONFLICT`,
`SM_ERR_SERIALIZATION` or `SM_ERR_DEADLOCK`, else 0.

### `sm_reseterr`

```cpp
void sm_reseterr(void);
```

Clears the thread's error to `SM_OK` and "no error", and clears
what `sm_errdup` reports.

### `sm_seterr`

```cpp
int sm_seterr(sm_errnum_t code, const char* msg);
```

Records `code` and a copy of `msg` as the thread's error, and
returns -1. A long message is cut to fit. A callback of the host
fails with `return sm_seterr(...);`.

### `sm_errdup`

```cpp
int sm_errdup(uint64_t* index, std::string* row = nullptr);
```

Says where the thread's last error, `SM_ERR_DUPKEY`, met its key
(`ROWSTORE.md` §2.7).

- `*index` gets the unique index's oid, or 0 if the stack does
  not know it. `index` must not be NULL.
- `*row`, if `row` is not NULL, gets the row key of a row that
  holds the key, in the form `sm_tbl_read_key` takes, or an empty
  string.

Returns 1 with a row key, else 0. After any other error, `*index`
is 0 and the row key is empty.

## The log hook

The hook is sm1's (`sm1/API.md`, "Logging").

```cpp
enum sm_loglevel_t {
    SM_LOG_ERROR = 0,   /* something failed and nobody was told */
    SM_LOG_WARN  = 1,   /* something needs an operator's attention */
    SM_LOG_INFO  = 2    /* a notable event, such as recovery */
};

typedef void (*sm_log_fn)(void* arg, sm_loglevel_t level,
                          const char* msg) noexcept;
```

`sm_log_fn` receives the `arg` given to `sm_set_log`, the level,
and a NUL-terminated message valid only during the call. It may
run on sm1's own threads and inside engine calls.

### `sm_set_log`

```cpp
int sm_set_log(sm_log_fn fn, void* arg);
```

Installs `fn` as the process-wide log hook. NULL removes the
hook. Each hook installed stays allocated for the life of the
process, because a thread may still be calling the one it
replaces.

## Home directories and databases

The Database Manager holds the design (`DBMGR.md`). The first
session on a database also loads its catalog (`CATALOG.md`).

### `sm_init`

```cpp
int sm_init(const char* homedir);
```

Makes `homedir`, which must exist, a wisr home directory. Call it
once in a home directory's life, not once in each process.

Errors: `SM_ERR_EXISTS` (already a home directory),
`SM_ERR_NOTFOUND` (no such directory).

### `sm_attach`

```cpp
int sm_attach(const char* homedir, sm_env_t** out,
              const sm_env_options_t* opt = nullptr);
```

Attaches `homedir` and hands back the environment `sm_env_t`. It
recovers every database, then closes each one again. NULL `opt`
means the defaults. No other environment, in any process, may
have `homedir` attached: the stack takes no lock for that.

`sm_env_options_t` (`include/sm_options.h`):

| Field | Default | What it is |
| --- | --- | --- |
| `block_cache_bytes` | 128 MB | the one block cache of the environment, at least 1; a quarter goes to the buffer pool of a tree's pages (`BUFMGR.md`) |
| `max_sessions` | 0 | if above 0, the most sessions at once, so the buffer pool has frames enough for each; below 1, the pool has its floor of 64 frames |

Errors: `SM_ERR_NOTFOUND` (not a home directory),
`SM_ERR_VERSION`, `SM_ERR_CORRUPT`, `SM_ERR_INVALID`.

### `sm_env_options_init`

```cpp
static inline void sm_env_options_init(sm_env_options_t* opt);
```

Declared in `include/sm_options.h`. Fills `opt` with the
defaults.

### `sm_detach`

```cpp
int sm_detach(sm_env_t* env, int force);
```

Detaches `env`. With `force` 0 and a session open, it fails with
`SM_ERR_INUSE`, and `env` stays as it was. Otherwise:

1. Abort the active transaction of each open session. A prepared
   transaction stays in doubt.
2. Stop each catalog's Index Builder and btree cleaner.
3. Detach sm1, which closes the sessions and the databases.
4. Free the sessions, the catalogs and `env`.

If sm1's detach fails, the call restarts the workers and returns
-1, and `env` stays valid. After a forced detach, every
`sm_session_t` of `env` is invalid. Force closes sessions, but it
does not stop threads that use them: quiesce those first.

### `sm_mkdb`

```cpp
int sm_mkdb(sm_env_t* env, const char* dbname, const sm_options_t* opt);
```

Creates the database `dbname` and its catalog (`sm3::cat_init`).
A name is 1 to 63 bytes, any byte but NUL, compared bytewise.
NULL `opt` means the defaults. If the catalog cannot be made, the
call removes the database. The database is not opened.

`sm_options_t` (`include/sm_options.h`), also taken by the first
`sm_opendb`:

| Field | Default | What it is |
| --- | --- | --- |
| `durability_sync` | 1 | 0 trades a bounded window of committed transactions on power failure for throughput; the database stays crash-consistent |
| `group_commit_us` | 200 | unused |
| `deadlock_ms` | 50 | how long a wait lasts before a deadlock check |
| `ssi_max_entries` | 1 << 20 | unused |
| `ts_lease` | 1000000 | the size of each lease, of timestamps and of a sequence's values (`XCTMGR.md`) |
| `rocksdb_options` | NULL | a RocksDB `DBOptions` string; NULL means RocksDB's defaults |
| `commit_cache_entries` | 65536 | slots in the commit cache, at least 1; a slot is 16 bytes |
| `savepoint_undo_bytes` | 64 MB | the undo a transaction keeps for its savepoints, at least 1 |
| `idle_close_ms` | 60000 | how long a database stays open after its last session closes; 0 closes it at once |
| `load_small_bytes` | 4 MB | a bulk load below this size is one batch, not files |
| `load_sort_bytes` | 256 MB | the memory a bulk load sorts in |
| `load_file_bytes` | 256 MB | the size at which a load closes a sorted file |

Errors: `SM_ERR_EXISTS`, `SM_ERR_INVALID`.

### `sm_options_init`

```cpp
static inline void sm_options_init(sm_options_t* opt);
```

Declared in `include/sm_options.h`. Fills `opt` with the
defaults.

### `sm_rmdb`

```cpp
int sm_rmdb(sm_env_t* env, const char* dbname);
```

Deletes the database `dbname` with all its data, and closes its
catalog in memory. The call cannot be undone. It stops the
catalog's workers first, and restarts them if the delete fails.

Errors: `SM_ERR_INUSE` (a session is open on it),
`SM_ERR_NOTFOUND`.

### `sm_lsdb`

```cpp
int sm_lsdb(sm_env_t* env, std::vector<std::string>& names);
```

Lists the databases into `names`, in bytewise order.

### `sm_dbid`

```cpp
int sm_dbid(sm_env_t* env, const char* dbname, uint64_t* out);
```

Sets `*out` to the id `sm_mkdb` gave `dbname`. No other database
of this home directory ever has it. A SQL layer can use it as the
database's oid.

Errors: `SM_ERR_NOTFOUND`.

### `sm_opendb`

```cpp
int sm_opendb(sm_env_t* env, const char* dbname, const sm_options_t* opt,
              sm_session_t** out);
```

Opens a session `sm_session_t` on `dbname`. NULL `opt` means the
defaults. The session holds an sm3 session over an sm1 session,
so the transaction calls keep its overlay of DDL in step.

The first session on a database does more:

1. Open the database, with `opt`. A later session's `opt` is
   ignored.
2. Load the catalog (`sm3::cat_open`), in a transaction of its
   own.
3. Give the catalog the environment's evaluator and TOAST
   setting.
4. Start the Index Builder, and queue the indexes a restart left
   pending.
5. Start the btree cleaner.

Errors: `SM_ERR_NOTFOUND`.

### `sm_closedb`

```cpp
int sm_closedb(sm_session_t* session);
```

Closes a session. It drops the session's pins, its overlay and
its temporary tables and views, closes its cursors, and aborts an
active transaction. A prepared transaction stays in doubt.
`session` is invalid after the call, whether it succeeds or not.

## Transactions

The Transaction Manager holds the design (`XCTMGR.md`), and the
Lock Manager the locks (`LOCKMGR.md`). Pins are the Row Store's
(`ROWSTORE.md` §6.4).

### `sm_begin`

```cpp
enum sm_xctlevel_t {
    SM_XCT_READ_COMMITTED  = 0,
    SM_XCT_REPEATABLE_READ = 1,
    SM_XCT_SERIALIZABLE    = 2
};

int sm_begin(sm_session_t* session, sm_xctlevel_t level, int read_only);
```

Begins a transaction at `level`. A nonzero `read_only` declares
it READ ONLY: a write fails with `SM_ERR_READONLY`, and at
SERIALIZABLE it costs SSI less (`XCTMGR.md` §7.5).

Errors: `SM_ERR_INTRANS` (a transaction is active, or this
session's prepared transaction is unresolved), `SM_ERR_INVALID`
(an unknown level).

### `sm_commit`

```cpp
int sm_commit(sm_session_t* session);
```

Commits the active transaction, and publishes its DDL to the
catalog. The call first releases the session's pins and ends the
session's overlay. On a transaction this session prepared, it is
the coordinator's commit decision.

After a failure the transaction is over, but for two errors. With
a bulk load open, the call fails with `SM_ERR_INVALID`. On a
doomed transaction, it fails with `SM_ERR_DOOMED`. In both, the
transaction stays active without its pins or its overlay, so call
`sm_abort`.

Errors: `SM_ERR_SERIALIZATION` (retryable), `SM_ERR_NOTRANS`,
`SM_ERR_IO` (it did not commit), `SM_ERR_INVALID`,
`SM_ERR_DOOMED`.

### `sm_abort`

```cpp
int sm_abort(sm_session_t* session);
```

Aborts the active transaction, its writes and its overlay, and
releases its pins. On a transaction this session prepared, it is
the coordinator's abort decision. With no active transaction, as
after a retryable error or `SM_ERR_NOMEM`, the call returns 0.
The error state then holds `SM_ERR_NOTRANS`.

### `sm_stmt_begin`

```cpp
int sm_stmt_begin(sm_session_t* session);
```

Begins a statement, a SQL command (`XCTMGR.md` §5). It runs until
the next `sm_stmt_begin` or the end of the transaction.

- Its reads see the transaction's writes made before it began,
  not its own, as PostgreSQL's command id does.
- Under READ COMMITTED its reads use one snapshot, which its
  first read takes. Once called, the transaction reads this way
  for the rest of its life.
- An update or a delete of a row the statement has already
  changed fails with `SM_ERR_SELFMODIFIED` and writes nothing.
  The caller skips the row, or raises an error as INSERT ... ON
  CONFLICT does.

A transaction that never calls it sees all its own writes, call
by call.

### `sm_unpin`

```cpp
typedef struct sm_pin_ref_t {
    void*    pin;
    uint64_t gen;
} sm_pin_ref_t;

void sm_unpin(sm_pin_ref_t* ref);
```

`sm_pin_ref_t` is what a read into a pin, a write's old row or a
cursor's row hands back. It names the pin that keeps the row and
the pin's generation. It is a plain value, zero for none, that
the caller keeps where it likes and never frees. Once its pin is
released it is stale and names nothing.

`sm_unpin` releases the pin `*ref` names, if the ref is live, and
always zeroes `*ref`. A cursor's pin stays the cursor's: the call
zeroes the ref and leaves the pin.

### `sm_release_all_pins`

```cpp
void sm_release_all_pins(sm_session_t* session);
```

Releases every pin the session holds, and leaves the transaction
open. Every ref to them goes stale. The current row of each
`sm_rel_scan_t` of the session goes too, and its refs go stale,
but the cursor keeps its pin.

### `sm_view_latest`

```cpp
int sm_view_latest(sm_session_t* session, bool on, bool* previous);
```

Enables current reads for a DDL check, at every isolation level.
The caller holds table locks that prevent concurrent writes to
all tables it reads, or knows that no write can break what it
decides: a foreign key check holds only IS, and the key, not
valid, is enforced on every write. Writes retain their normal
conflict checks.
If `previous` is non-NULL, the call stores the old setting there.
Restore that setting after the check, including on error. The
transaction's end turns current reads off.

### `sm_stmt_begin_inner`

```cpp
typedef struct sm_stmt_t sm_stmt_t;

int sm_stmt_begin_inner(sm_session_t* session, sm_stmt_t** outer);
```

Begins a statement inside the running one, as a trigger's or a
function's commands run inside the statement that fired them. The
inner statement sees the outer's writes so far. `*outer` saves
the outer statement. `sm_stmt_end` takes it back and frees it.
`sm_stmt_t` is opaque.

### `sm_stmt_end`

```cpp
int sm_stmt_end(sm_session_t* session, sm_stmt_t* outer);
```

Ends a statement begun by `sm_stmt_begin_inner` or
`sm_stmt_resume`. The outer statement resumes with its own
snapshot, and again sees neither its own writes nor the inner
one's. The call frees `outer`, whether it succeeds or not. If a
resumed view had no snapshot, the view keeps the one its run
took.

### `sm_stmt_save`

```cpp
int sm_stmt_save(sm_session_t* session, sm_stmt_t** view);
```

Saves the running statement's view, its snapshot and mark, in
`*view`, to read at again with `sm_stmt_resume`, as a cursor
reads at its DECLARE's view on each FETCH. The running statement
goes on unchanged. A view is good for one transaction. Free it
with `sm_stmt_free`, before or after that transaction ends.

### `sm_stmt_resume`

```cpp
int sm_stmt_resume(sm_session_t* session, sm_stmt_t* view,
                   sm_stmt_t** outer);
```

Runs `view`'s statement inside the running one, and saves the
running one in `*outer` for `sm_stmt_end`. Reads then see the
transaction's writes made before `view` was saved, at its
snapshot, and none since. A view saved before its statement took
a snapshot takes one at its first resumed read, and keeps it for
each later resume.

### `sm_stmt_free`

```cpp
void sm_stmt_free(sm_stmt_t* view);
```

Frees a view `sm_stmt_save` made.

### `sm_stmt_advance`

```cpp
int sm_stmt_advance(sm_session_t* session);
```

Advances the running statement to now, as PostgreSQL's
`CommandCounterIncrement` does. Its later reads see the
transaction's writes so far, its own included, at the same
snapshot. A table the statement made can then be written in it,
as CREATE TABLE AS writes its new table. With no statement begun,
the call does nothing.

### `sm_set_qa_nowait`

```cpp
int sm_set_qa_nowait(sm_session_t* session, bool on);
```

For QA only: `rel`'s scripts use it. It is not how NOWAIT or SKIP
LOCKED is asked for. With `on`, a call that would wait for
another transaction's lock fails with `SM_ERR_BLOCKED`, and the
transaction stays active. The caller polls by repeating the call.
Off is the default. The setting lasts across transactions.

### `sm_session_tag`

```cpp
int sm_session_tag(sm_session_t* session, uint64_t tag);
```

Gives the session the host's number for it, by which
`sm_waits_for` and `sm_lock_status` name it. 0, a session's
number until this call, is none. The host keeps the numbers
distinct among its open sessions: the stack does not check.

### `sm_set_deadlock_ms`

```cpp
int sm_set_deadlock_ms(sm_session_t* session, int ms);
```

Sets how long the session waits for a lock or a writer before it
looks for a cycle of waits: `ms` above 0, or 0 for the database's
`deadlock_ms`, the default. It is PostgreSQL's
`deadlock_timeout`. A session with a shorter wait finds a cycle
first. The transaction ended is still the cycle's youngest.

Errors: `SM_ERR_INVALID` (a negative `ms`).

### `sm_waits_for`

```cpp
int sm_waits_for(sm_env_t* env, uint64_t tag, std::vector<uint64_t>* out);
```

Fills `*out`, cleared first, with the numbers of the sessions
that the open session numbered `tag` waits for now. They are the
holders of the lock it waits on, or the transaction whose
uncommitted write it waits behind. It is what PostgreSQL's
`pg_blocking_pids()` reports.

`*out` is empty if it waits for none, or no session has the
number. A session it waits for that has no number is left out,
and so is a prepared transaction.

### `sm_lock_status`

```cpp
typedef enum sm_lockname_t {
    SM_LOCKNAME_OBJECT = 0, /* an object by its oid: a table, an
                               index or a namespace */
    SM_LOCKNAME_ROW    = 1, /* a row of a table: FOR SHARE, FOR
                               UPDATE, an index build */
    SM_LOCKNAME_KEY    = 2  /* any other key */
} sm_lockname_t;

typedef struct sm_lock_status_t {
    uint64_t      dbid;    /* its database's id (sm_dbid) */
    sm_lockname_t name;    /* what it names */
    uint64_t      oid;     /* see below */
    sm_lockmode_t mode;    /* held, or asked */
    uint64_t      tag;     /* the session's number, 0 for none or a
                              prepared transaction */
    bool          granted; /* held; false if waited for */
} sm_lock_status_t;

int sm_lock_status(sm_env_t* env, std::vector<sm_lock_status_t>& out);
```

Fills `out`, cleared first, with every lock of `env`'s open
databases: one element for each holder of each lock, and one for
each session waiting for one. It is what PostgreSQL's `pg_locks`
lists. A writer waiting behind an uncommitted write holds and
asks no lock, and is not listed: `sm_waits_for` names whom it
waits for.

`name` is `SM_LOCKNAME_OBJECT` for an oid's lock, and
`SM_LOCKNAME_ROW` for a lock on a table's row. Anything else is
`SM_LOCKNAME_KEY`: a key of the catalog, or an index entry.

`oid` is the object's oid for `SM_LOCKNAME_OBJECT`. For
`SM_LOCKNAME_ROW` it is the table's, or 0 if no committed table
has the row's store. It does not say which row: sm keeps a lock's
id, not its key. For `SM_LOCKNAME_KEY` it is 0.

### `sm_savepoint`

```cpp
int sm_savepoint(sm_session_t* session);
```

Sets a savepoint and returns its depth, 1 or more, or -1. A
transaction holds up to 256. A depth is used again once its mark
is gone.

Errors: `SM_ERR_INVALID` (the stack is full, or a bulk load is
open).

### `sm_rollback`

```cpp
int sm_rollback(sm_session_t* session, int depth);
```

Undoes every write since the savepoint at `depth`. That savepoint
survives, and those set inside it go. A cursor opened after the
savepoint is closed: each read on it fails with
`SM_ERR_SCANCLOSED` until the caller ends it. Reads stay counted
for SSI.

Errors: `SM_ERR_NOSAVEPOINT`, `SM_ERR_UNDOLIMIT` (the undo passed
`savepoint_undo_bytes`; only an abort is left), `SM_ERR_INVALID`
(a bulk load is open).

### `sm_forget`

```cpp
int sm_forget(sm_session_t* session, int depth);
```

Forgets the savepoint at `depth` and those set inside it, as
SQL's RELEASE SAVEPOINT does. The writes and the locks stay.

Errors: `SM_ERR_NOSAVEPOINT`.

## Two-phase commit

The Transaction Manager holds the design (`XCTMGR.md` §10).

### `sm_prepare`

```cpp
int sm_prepare(sm_session_t* session, const char* gid);
```

Prepares the active transaction under the global id `gid`, 1 to
200 bytes. The transaction then belongs to the database: any
session may resolve it. This session's `sm_begin` fails with
`SM_ERR_INTRANS` until one does.

A transaction that made DDL is refused with `SM_ERR_INVALID` and
stays as it was. Any other call releases the session's pins,
whether it succeeds or not. A transaction that ran a bulk load
into an empty table, or made, changed or wrote a temporary table,
is refused with `SM_ERR_INVALID` too.

Errors: `SM_ERR_SERIALIZATION` (retryable; not prepared),
`SM_ERR_EXISTS` (`gid` is in use), `SM_ERR_INVALID`, `SM_ERR_IO`
(the transaction is no longer active).

### `sm_list_prepared`

```cpp
struct sm_prepared_t {
    std::string gid;
    long long   prepared_at;   /* wall-clock, seconds since epoch */
};

int sm_list_prepared(sm_session_t* session, std::vector<sm_prepared_t>& out);
```

Lists the database's unresolved prepared transactions into `out`.
It needs no active transaction.

### `sm_commit_prepared`

```cpp
int sm_commit_prepared(sm_session_t* session, const char* gid);
```

Commits the prepared transaction `gid`. It needs no active
transaction on `session`, and disturbs none.

Errors: `SM_ERR_NOTFOUND`.

### `sm_abort_prepared`

```cpp
int sm_abort_prepared(sm_session_t* session, const char* gid);
```

Aborts the prepared transaction `gid`, as `sm_commit_prepared`
commits one.

Errors: `SM_ERR_NOTFOUND`.

## Sequences, below the catalog

Sequence holds the design (`SEQUENCE.md`). These are counters of
the caller's own, with a name and no namespace. SQL's CREATE
SEQUENCE is `sm_cat_create_sequence`. A name is 1 to 63 bytes.
Names that begin `sm3.` are sm3's own, and each call here refuses
one with `SM_ERR_INVALID`.

### `sm_mkseq`

```cpp
int sm_mkseq(sm_session_t* session, const char* name, uint64_t start,
             uint64_t increment);
```

Creates the sequence `name`, which hands out `start`, `start +
increment`, and so on. The creation is transactional. `increment`
must not be 0.

Errors: `SM_ERR_EXISTS`, `SM_ERR_INVALID`.

### `sm_rmseq`

```cpp
int sm_rmseq(sm_session_t* session, const char* name);
```

Drops the sequence `name`, in the session's transaction.

Errors: `SM_ERR_NOTFOUND`.

### `sm_nextseq`

```cpp
int sm_nextseq(sm_session_t* session, const char* name, uint64_t* out);
```

Hands out the sequence's next value in `*out`. It needs no
transaction, and an abort does not give the value back.

Errors: `SM_ERR_NOTFOUND`, `SM_ERR_INVALID` (`out` is NULL, or
the sequence is past `UINT64_MAX`).

## Types and collations

These are sm2's (`sm2/TYPE.md`, and `LSMINDEX.md` §2.3 for
collations). `sm_type_t` and `sm_collation_t` are opaque.

`sm_datum_t` is a `uintptr_t`, as PostgreSQL's 8-byte Datum. A
by-value type's value fits in the datum: int2, int4, int8, bool,
float4, float8, date, time, timestamp, timestamptz. A
by-reference type's datum points at its bytes or varlena.

The macros `SM_DATUM_FROM_INT16`, `_INT32`, `_INT64`, `_BOOL` and
`_PTR`, and `SM_DATUM_TO_INT16`, `_INT32`, `_INT64`, `_BOOL` and
`_PTR`, convert a value. A value narrower than the datum is
sign-extended, as PostgreSQL's `Int16GetDatum` and
`Int32GetDatum` extend theirs, so -1 is all ones.

### `sm_type`

```cpp
const sm_type_t* sm_type(uint8_t code);
```

Returns the type with this frozen code (`sm2/TYPE.md` §3), or
NULL.

### `sm_type_byval_width`

```cpp
size_t sm_type_byval_width(const sm_type_t* type);
```

Returns a by-value type's width in bytes, or 0 for a by-reference
type. `type` must not be NULL.

### `sm_collation_open`

```cpp
int sm_collation_open(const char* locale, bool deterministic,
                      const char* expected_version, sm_collation_t** out);
```

Opens the collation for `locale`, a BCP 47 tag, or `C` or `POSIX`
for byte order. With `expected_version` non-empty, the call
refuses an ICU version that differs. NULL means the empty string.

Errors: `SM_ERR_INVALID`: an unknown tag or language, a version
that differs, or byte order with `deterministic` false or an
`expected_version`.

### `sm_collation_close`

```cpp
void sm_collation_close(sm_collation_t* collation);
```

Closes a collation.

### `sm_collation_version`

```cpp
const char* sm_collation_version(const sm_collation_t* collation);
```

Returns the collation's ICU version, for the catalog, or an empty
string for byte order. The string lasts as long as the collation.

### `sm_datum_of_bytes`

```cpp
sm_datum_t sm_datum_of_bytes(const sm_type_t* type, const std::string& bytes);
```

Returns the datum of `type` whose bytes are `bytes`, as a
statistic keeps a value (`sm_stat_t`). A by-value datum is built
from them. A by-reference datum points into `bytes`, which must
outlive it.

### `sm_datum_from_float`

```cpp
static inline sm_datum_t sm_datum_from_float(float f);
```

Returns a float's bits as a datum, sign-extended from 32 bits.

### `sm_datum_from_double`

```cpp
static inline sm_datum_t sm_datum_from_double(double f);
```

Returns a double's bits as a datum.

### `sm_datum_to_float`

```cpp
static inline float sm_datum_to_float(sm_datum_t d);
```

Returns the float in a datum.

### `sm_datum_to_double`

```cpp
static inline double sm_datum_to_double(sm_datum_t d);
```

Returns the double in a datum.

### `sm_type_in`

```cpp
int sm_type_in(const sm_type_t* type, int32_t typmod, const char* text,
               size_t len, sm_datum_t* val, std::string& buf);
```

Parses PostgreSQL's canonical text for a value of `type` under
`typmod`, as the type's typinput does (`sm2/TYPE.md` §3). A
by-value result is `*val` itself, and `buf` is left empty. A
by-reference result, fixed-width bytes or a varlena, goes in
`buf`, and `*val` points at it. `buf` must outlive `*val` and
must not change meanwhile.

Errors: `SM_ERR_INVALID` (a NULL argument, or text that is not
canonical).

### `sm_type_out`

```cpp
int sm_type_out(const sm_type_t* type, int32_t typmod, sm_datum_t val,
                std::string& text);
```

Prints a non-null value of `type` under `typmod` as PostgreSQL's
canonical text into `text`, as the type's typoutput does.

## Cursors over rows and index entries

The Row Store and the index modules hold the design
(`ROWSTORE.md`, `LSMINDEX.md`, `BTREEINDEX.md`, `CLUSTER.md` §5).
A caller reads and writes a row or an index entry through an open
table, never through a heap or an index of its own. So no caller
can change a row and leave its index entries behind
(`INDEXBUILD.md` §2).

`sm_rel_scan_t` is a cursor over a table's rows, in their key's
order: `_id` order in a keyless table, the primary key's in a
table with one. A tree's rows come in the tree's order.
`sm_idx_scan_t` is a cursor over an index's entries, in key
order. `sm_tbl_scan`, `sm_tbl_scan_rows`, `sm_tbl_idx_scan_rows`
and `sm_tbl_idx_scan`, under Tables, open them.

### `sm_rel_scannxt`

```cpp
int sm_rel_scannxt(sm_rel_scan_t* scan, uint64_t* id, const void** tuple,
                   size_t* len);
int sm_rel_scannxt(sm_rel_scan_t* scan, uint64_t* id, sm_pin_ref_t* ref);
```

Moves to the next row. Returns 0 with it, 1 at the end, or -1.
`*id` is the row's `_id`.

- The first form sets `*tuple` and `*len`, only on 0.
- The second form keeps the row in the cursor's pin, and `*ref`
  names it, for `sm_pin_deform`. `*ref` is zero on 1 or -1. The
  cursor's next call, its close, `sm_release_all_pins` and the
  end of its transaction make the ref stale.
- On a cursor set by `sm_rel_scan_entries`, an entry with its row
  unread leaves `*ref` zero on 0.

Errors: `SM_ERR_SCANCLOSED`.

### `sm_rel_scan_seek`

```cpp
int sm_rel_scan_seek(sm_rel_scan_t* scan, uint64_t id);
```

Moves a cursor over a keyless table's rows forward to `_id` `id`.

Errors: `SM_ERR_INVALID` (a backward move, a cursor in key order,
or a tree's cursor).

### `sm_rel_scan_entries`

```cpp
int sm_rel_scan_entries(sm_rel_scan_t* scan);
```

Makes a cursor of a tree's rows an index-only scan's
(`BTREEINDEX.md` §8). An entry on a leaf vacuum found all
visible, whose key gives its columns back, then comes with its
row unread. `sm_rel_scannxt`'s `*ref` names no row for such an
entry, and `sm_rel_scan_cols` gives its columns.

Errors: `SM_ERR_INVALID` (any other cursor).

### `sm_rel_scan_cols`

```cpp
int sm_rel_scan_cols(sm_rel_scan_t* scan, sm_datum_t* values, bool* isnull,
                     size_t n);
```

Gives the key columns of the entry of a tree's rows that
`sm_rel_scannxt` last gave, as `sm_idx_scan_key` does. `n` is the
index's key column count. Returns 0, or 1 if the key does not
give them back and the row, which the cursor read, has them.

Errors: `SM_ERR_INVALID` (not a tree's cursor, no entry yet, or
the wrong `n`).

### `sm_rel_scan_rebound`

```cpp
struct sm_bound_t {
    const sm_datum_t* prefix;
    const bool*           isnull;
    size_t                n;
    bool                  inclusive;
};

int sm_rel_scan_rebound(sm_rel_scan_t* scan, const sm_bound_t* lo,
                        const sm_bound_t* hi);
```

`sm_bound_t` is a scan bound: the values of the index's first `n`
columns, with a null flag for each. `inclusive` says whether an
entry equal to them is inside the bound. Bounds are in the
index's own order, after direction and null ordering.

`sm_rel_scan_rebound` aims a cursor of a tree's rows at the rows
between `lo` and `hi`, as if opened anew: an IN list's next
probe, with no new cursor. Either bound may be NULL for none.

Errors: `SM_ERR_INVALID` (any other cursor, or a bound with `n`
and no arrays).

### `sm_rel_scan_key`

```cpp
int sm_rel_scan_key(sm_rel_scan_t* scan, const void** key, size_t* len);
```

Gives the row key of the row `sm_rel_scannxt` last gave, as
`sm_tbl_row_key` encodes a row's name. Call it only after
`sm_rel_scannxt` returned a row. The key is valid as the row is.

### `sm_rel_scan_seek_key`

```cpp
int sm_rel_scan_seek_key(sm_rel_scan_t* scan, const void* key, size_t len);
```

Moves a cursor in key order forward to `key`, a row key as
`sm_tbl_keys_at` gives one. The next row is the first at or after
it.

Errors: `SM_ERR_INVALID` (a backward move, or a tree's cursor).

### `sm_rel_scanend`

```cpp
void sm_rel_scanend(sm_rel_scan_t* scan);
```

Closes a cursor and releases its pin.

### `sm_tup_encode`

```cpp
struct sm_attribute_t {
    const sm_type_t* type;
    int32_t          typmod;
    bool             dropped;
    bool             has_missing;
    std::string      missing;
};

int sm_tup_encode(const std::vector<sm_attribute_t>& schema,
                  const sm_datum_t* values, const bool* isnull,
                  std::string& tuple);
```

`sm_attribute_t` is one attribute of a table's schema: its type,
its typmod and whether it is dropped. A row older than the
attribute reads it as null, or as `missing` when `has_missing` is
set.

`sm_tup_encode` encodes `values` and `isnull`, one per attribute
of `schema`, into `tuple`. A dropped attribute must be null.

Errors: `SM_ERR_INVALID`, `SM_ERR_ROWTOOLONG`. On either, `tuple`
stays as it was.

### `sm_idx_scannxt`

```cpp
int sm_idx_scannxt(sm_idx_scan_t* scan, uint64_t* id, bool* cut,
                   const void** value, size_t* len);
```

Moves to the next entry. Returns 0 with it, 1 at the end, or -1.
`*id` is its row's `_id`. `*cut` is true if the key was cut to
fit (`LSMINDEX.md` §2.4), so that only the row holds the whole
value. `*value` and `*len`, set only on 0, are the entry's value,
for `sm_tbl_idx_deform`.

An LSM index's cursor returns entries the transaction sees. A
tree's entries carry no version, so its cursor returns each entry
in range, and the row decides what the transaction sees
(`BTREEINDEX.md` §2). A host reads a tree through
`sm_tbl_idx_scan_rows`.

### `sm_idx_scan_seek`

```cpp
int sm_idx_scan_seek(sm_idx_scan_t* scan, const sm_bound_t* lo);
```

Moves the cursor forward to the first entry at or after `lo`,
which must not be NULL. On an LSM index a backward move fails
with `SM_ERR_INVALID`. A tree's cursor does not check, and goes
to `lo`.

### `sm_idx_scan_key`

```cpp
int sm_idx_scan_key(sm_idx_scan_t* scan, sm_datum_t* values, bool* isnull,
                    size_t n);
```

Gives the key columns of the entry `sm_idx_scannxt` last
returned, into `values` and `isnull`, one per key column, as a
bound gives them. Returns 0, or 1 if the key does not give them
back, so the row must. That is so if the key was cut, or a text
column is under a non-deterministic collation.

Errors: `SM_ERR_INVALID` (`n` is not the key column count).

### `sm_idx_scan_row`

```cpp
int sm_idx_scan_row(sm_idx_scan_t* scan, const void** key, size_t* len);
```

Gives the row key of the row of the entry `sm_idx_scannxt` last
returned, for `sm_tbl_read_key`. The key is valid until the next
call on the cursor.

### `sm_idx_scanend`

```cpp
void sm_idx_scanend(sm_idx_scan_t* scan);
```

Closes a cursor.

## The catalog

The Catalog holds the design (`CATALOG.md`), with Namespace
(`NAMESPACE.md`) and Sequence (`SEQUENCE.md`). These calls are
sm3's. A lookup takes no lock unless its entry says so.

```cpp
#define SM_NSP_PG_CATALOG 11
#define SM_NSP_PUBLIC     2200
#define SM_NSP_TEMP       0
```

`SM_NSP_TEMP` is the namespace of a session's temporary tables,
which is none. The lookups by name take it to mean the session's
own temporary tables and their indexes. A temporary table's
definition has it for its namespace.

### `sm_cat_namespace_epoch`

```cpp
uint64_t sm_cat_namespace_epoch(sm_session_t* session);
```

Returns the count of the catalog's publish that last made or
dropped a namespace (`NAMESPACE.md` §5). A namespace's lookup by
name takes no lock, so a host that caches namespaces, or a search
path over them, drops them when the epoch moves. It returns 0 for
a NULL session.

### `sm_cat_epoch`

```cpp
uint64_t sm_cat_epoch(sm_session_t* session);
```

Returns the catalog's epoch as the session sees it. It moves when
a DDL, an analyze or an index made valid is published to what the
session looks up, the session's temporary tables' changes among
them. It is `UINT64_MAX` while the session's transaction has DDL
of its own. A host that caches what it looks up reads it before
each lookup, and keeps an answer while it reads the same
(`../dg/SCHEMACACHE.md`). It returns 0 for a NULL session.

### `sm_cat_publishes`

```cpp
uint64_t sm_cat_publishes(sm_session_t* session);
```

Returns the count of the catalog's publishes, whatever the
session's own transaction has done. It is one load, for a host
that asks at every lock and makes its own DDL known to itself. A
publish that moves `sm_cat_namespace_epoch` moves this after, so
a reader that sees this moved sees that moved too. It returns 0
for a NULL session.

### `sm_cat_create_namespace`

```cpp
int sm_cat_create_namespace(sm_session_t* session, const char* name,
                            uint64_t* oid);
```

Creates the namespace `name`, SQL's schema, in the session's
transaction. The transaction sees it at once, and others at
commit. `*oid` gets its oid.

Errors: `SM_ERR_DUPKEY` (the name is taken), `SM_ERR_INVALID` (no
name).

### `sm_cat_drop_namespace`

```cpp
int sm_cat_drop_namespace(sm_session_t* session, uint64_t oid);
```

Drops the namespace `oid`.

Errors: `SM_ERR_INUSE` (it holds a table, a collation, a sequence
or a view), `SM_ERR_INVALID` (`SM_NSP_PG_CATALOG` or
`SM_NSP_PUBLIC`), `SM_ERR_NOTFOUND`.

### `sm_cat_namespace_oid`

```cpp
int sm_cat_namespace_oid(sm_session_t* session, const char* name,
                         uint64_t* oid);
```

Sets `*oid` to the oid of the namespace `name`, as the session's
transaction sees it. Returns 0, or 1 if there is none.

### `sm_cat_namespace_name`

```cpp
int sm_cat_namespace_name(sm_session_t* session, uint64_t oid,
                          std::string& name);
```

Sets `name` to the name of the namespace `oid`, as the session's
transaction sees it. Returns 0, or 1 if there is none.

### `sm_cat_list_namespaces`

```cpp
int sm_cat_list_namespaces(sm_session_t* session,
                           std::vector<std::string>& names);
```

Lists the namespaces' names, sorted, as the session's transaction
sees them.

### `sm_cat_table_oid`

```cpp
int sm_cat_table_oid(sm_session_t* session, uint64_t nspoid,
                     const char* name, uint64_t* oid);
```

Sets `*oid` to the oid of the table `name` in `nspoid`, as the
session's transaction sees it. Returns 0, or 1 if there is none.
`sm_tbl_open` then finds a table dropped since.

### `sm_cat_list_tables`

```cpp
int sm_cat_list_tables(sm_session_t* session, uint64_t nspoid,
                       std::vector<std::string>& names);
```

Lists the names of the tables in `nspoid`, sorted, as the
session's transaction sees them.

### `sm_cat_table_def`

```cpp
int sm_cat_table_def(sm_session_t* session, uint64_t nspoid,
                     const char* name, sm_table_def_t& out);
```

Copies the definition of the table `name` in `nspoid` into `out`,
as the session's transaction sees it. Returns 0, or 1 if there is
none. It does not wait for a DDL that commits meanwhile. It is
for listing tables, as a catalog query does. `sm_tbl_open` is for
using one.

The definition and its parts:

```cpp
struct sm_table_def_t {
    uint64_t                     oid;
    uint64_t                     nspoid;
    std::string                  name;
    std::vector<sm_column_def_t> columns;
    std::vector<sm_attribute_t>  schema;
    std::vector<sm_index_def_t>  indexes;
    uint64_t                     version = 0;
    std::vector<sm_fkey_def_t>   fkeys;         /* its own */
    std::vector<sm_fkey_ref_t>   referenced_by; /* those that reference it */
};

struct sm_column_def_t {
    std::string name;
    uint8_t     type_code;
    int32_t     typmod;
    bool        not_null;
    bool        dropped;
    bool        has_default;
    std::string default_expr;   /* the host's, opaque; "" if none */
    char        identity;       /* 'a', 'd' or 0 */
    uint64_t    identity_seq;   /* an identity column's sequence */
    bool        has_on_update;
    std::string on_update_expr; /* the host's update default; "" if none */
};

enum sm_index_state_t {
    SM_INDEX_PENDING  = 0,  /* maintained, not yet filled; not scanned */
    SM_INDEX_VALID    = 1,
    SM_INDEX_FAILED   = 2,  /* its build failed; it can only be dropped */
    SM_INDEX_BUILDING = 3   /* pending, and being filled now */
};

struct sm_key_column_t {
    const sm_type_t*      type;
    int32_t               typmod;
    const sm_collation_t* collation;   /* text, varchar, bpchar; NULL: bytes */
    bool                  desc;        /* descending */
    bool                  nulls_first; /* nulls before every value */
};

struct sm_index_def_t {
    uint64_t                     oid;
    std::string                  name;
    bool                         unique;
    std::vector<int32_t>         key_attnums;     /* -1: an expression */
    std::vector<sm_key_column_t> key;
    std::vector<int32_t>         include_attnums;
    std::vector<sm_attribute_t>  include;
    sm_index_state_t             state;
    int                          error;   /* SM_OK unless failed */
    std::string                  reason;  /* that error's message */
    std::string                  eval;    /* predicate and expressions */
    std::vector<int32_t>         reads;   /* the columns they read */
    bool                         primary = false;    /* the primary key */
    bool                         constraint = false; /* of a constraint */
    bool                         tree = false;       /* a tree */
};

struct sm_fkey_ref_t {
    uint64_t table;  /* the child */
    uint64_t fkey;   /* the foreign key's oid */
};
```

`schema` holds the columns' attributes, as sm2 lays a row out by
them. `sm_fkey_def_t` is under `sm_cat_create_fkey`.

`version` tells a host when what it caches of the table is stale.
It is the count of the catalog's publish that last changed the
definition or the table's statistics, or made one of its indexes
valid. It is `SM_VERSION_OWN`, `UINT64_MAX`, while the
transaction reading it has changed the definition or the
statistics and not committed. Read under the table-oid lock IS or
IX, it changes only by an analyze or an index made valid, which
no DDL's X keeps out.

### `sm_cat_table_def_oid`

```cpp
int sm_cat_table_def_oid(sm_session_t* session, uint64_t oid,
                         sm_table_def_t& out);
```

As `sm_cat_table_def`, of the table `oid`. Returns 0, or 1 if
there is none.

### `sm_cat_table_version_oid`

```cpp
int sm_cat_table_version_oid(sm_session_t* session, uint64_t oid,
                             uint64_t* version);
```

Sets `*version` to the version of the table `oid`, as the
session's transaction sees it. Returns 0, or 1 if there is none.
It copies no definition: a host that caches the table asks this
whether its copy is current.

### `sm_cat_index_oid`

```cpp
int sm_cat_index_oid(sm_session_t* session, uint64_t nspoid,
                     const char* name, uint64_t* table, uint64_t* index);
```

Sets `*table` and `*index` to the oids of the index `name` in
`nspoid` and its table. Returns 0, or 1 if there is none.

### `sm_cat_create_sequence`

```cpp
int sm_cat_create_sequence(sm_session_t* session, uint64_t nspoid,
                           const char* name, uint64_t start,
                           uint64_t increment, uint64_t* oid);
```

Creates the sequence `name` in `nspoid`, SQL's CREATE SEQUENCE,
in the session's transaction. It hands out `start`, `start +
increment`, and so on. `*oid` gets its oid. Its name is a
relation's.

Errors: `SM_ERR_DUPKEY` (a table or an index of `nspoid` has the
name), `SM_ERR_INVALID` (an increment of 0, or no name).

### `sm_cat_drop_sequence`

```cpp
int sm_cat_drop_sequence(sm_session_t* session, uint64_t oid);
```

Drops the sequence `oid`, in the session's transaction.

Errors: `SM_ERR_INUSE` (a column owns it), `SM_ERR_NOTFOUND`.

### `sm_cat_own_sequence`

```cpp
int sm_cat_own_sequence(sm_session_t* session, uint64_t oid, uint64_t table,
                        int32_t attnum);
```

Makes the column `attnum` (0-based) of `table` own the sequence
`oid`, SQL's ALTER SEQUENCE ... OWNED BY, in the session's
transaction. Dropping the table or the column then drops the
sequence. `table` 0 is OWNED BY NONE. The call locks `table` IS.

Errors: `SM_ERR_INVALID` (a table in another namespace, or a
column it does not have), `SM_ERR_INUSE` (an identity column's
sequence, which is its for good).

### `sm_cat_sequence_oid`

```cpp
int sm_cat_sequence_oid(sm_session_t* session, uint64_t nspoid,
                        const char* name, uint64_t* oid);
```

Sets `*oid` to the oid of the sequence `name` in `nspoid`, as a
table's oid is found. The call looks at the transaction's own
creates and drops, then at every committed sequence, not as of
the transaction's snapshot. Returns 0, or 1 if there is none. It
reads no store.

### `sm_cat_sequence_name`

```cpp
int sm_cat_sequence_name(sm_session_t* session, uint64_t oid,
                         uint64_t* nspoid, std::string& name);
```

Gives the namespace and the name of the sequence `oid`, found as
`sm_cat_sequence_oid` finds one. Returns 0, or 1 if there is no
such sequence, as for a table's or an index's oid. It reads no
store.

### `sm_cat_sequence_def`

```cpp
struct sm_sequence_def_t {
    uint64_t    nspoid;
    std::string name;
    uint64_t    start;
    uint64_t    increment;
    uint64_t    owner;        /* the table whose column owns it, or 0 */
    int32_t     owner_attnum; /* that column, 0-based, or -1 */
};

int sm_cat_sequence_def(sm_session_t* session, uint64_t oid,
                        sm_sequence_def_t* out);
```

Copies the sequence `oid` into `*out`, as the session's
transaction sees it. Returns 0, or 1 if there is no such
sequence. The transaction must be open.

### `sm_cat_list_sequences`

```cpp
int sm_cat_list_sequences(sm_session_t* session, uint64_t nspoid,
                          std::vector<std::string>& names);
```

Lists the names of the sequences in `nspoid`, sorted, as
`sm_cat_sequence_oid` finds them: every committed sequence, then
the transaction's own creates and drops. It needs no open
transaction.

### `sm_cat_nextval`

```cpp
int sm_cat_nextval(sm_session_t* session, uint64_t oid, uint64_t* out);
```

Hands out the next value of the sequence `oid` in `*out`. It is
not transactional: a rollback does not give the value back.

Errors: `SM_ERR_NOTFOUND` (not there, as the session's
transaction sees it), `SM_ERR_INVALID` (past `UINT64_MAX`).

## Views

View holds the design (`VIEW.md`). A view is a name in a
namespace, a relation's, with the host's query behind it. sm
keeps the query, never reads it, and keeps the view true to the
relations it reads. A view has a table's oid lock: a query takes
it `SM_LOCKMODE_IS`, and DDL on the view `SM_LOCKMODE_EX`, so
each waits for the other.

### `sm_cat_create_view`

```cpp
struct sm_view_read_t {
    uint64_t relation;
    int32_t  attnum;   /* a table's column, 0-based, or -1 for all */
};

struct sm_view_def_t {
    uint64_t                    nspoid = 0;
    std::string                 name;
    std::string                 query;    /* the host's, opaque */
    std::string                 options;  /* the host's, or empty */
    std::vector<sm_view_read_t> reads;
    uint64_t                    version = 0;
};

int sm_cat_create_view(sm_session_t* session, uint64_t nspoid,
                       const char* name, const sm_view_def_t& def,
                       uint64_t* oid);
```

Creates the view `name` in `nspoid`, in the session's
transaction. `*oid` gets its oid. The call keeps `def`'s query,
options and reads, and ignores its name, namespace and version.
Each relation read is locked `SM_LOCKMODE_IS` to the end of the
transaction. It must be a table, with a live column where one is
named, or a view, with none named.

`SM_NSP_TEMP` makes a temporary view, the session's (`VIEW.md`
§12). A temporary view alone may read a temporary table or view.

Errors: `SM_ERR_NOTFOUND` (a relation read is not there),
`SM_ERR_INVALID` (a read that is not allowed, or no name),
`SM_ERR_DUPKEY` (a relation of `nspoid` has the name, or for a
temporary view, one of the session's temporary relations).

### `sm_cat_replace_view`

```cpp
int sm_cat_replace_view(sm_session_t* session, uint64_t oid,
                        const sm_view_def_t& def);
```

Replaces the query, options and reads of the view `oid` with
`def`'s, SQL's CREATE OR REPLACE VIEW, under its lock
`SM_LOCKMODE_EX`. The call checks the reads as
`sm_cat_create_view` does.

Errors: as `sm_cat_create_view`, and `SM_ERR_INVALID` (a read
that reaches the view itself, through other views or directly).

### `sm_cat_rename_view`

```cpp
int sm_cat_rename_view(sm_session_t* session, uint64_t oid, const char* name);
```

Renames the view `oid`, under its lock `SM_LOCKMODE_EX`.

Errors: `SM_ERR_DUPKEY`, `SM_ERR_INVALID` (no name).

### `sm_cat_drop_view`

```cpp
int sm_cat_drop_view(sm_session_t* session, uint64_t oid);
```

Drops the view `oid`, under its lock `SM_LOCKMODE_EX`. For
CASCADE, the host drops the views that read it first.
`sm_cat_drop_table`, `sm_cat_drop_column` and
`sm_cat_drop_namespace` also fail with `SM_ERR_INUSE` on what a
view reads or holds.

Another session's temporary view, a reader `sm_cat_view_readers`
gives as `other`, takes no lock. Once this transaction commits,
its session no longer sees it, nor its own temporary views that
read it, and drops them at its next transaction (`VIEW.md`
§12.4).

Errors: `SM_ERR_INUSE` (another view reads it).

### `sm_cat_lock_view`

```cpp
int sm_cat_lock_view(sm_session_t* session, uint64_t oid, sm_lockmode_t mode,
                     uint64_t* version);
```

Takes the view `oid`'s lock in `mode`, held to the end of the
transaction, and sets `*version` to its version under the lock,
as a table's in `sm_table_def_t`. Returns 0, or 1 if there is no
such view, as for one dropped while the caller waited.

### `sm_cat_view_oid`

```cpp
int sm_cat_view_oid(sm_session_t* session, uint64_t nspoid, const char* name,
                    uint64_t* oid);
```

Sets `*oid` to the oid of the view `name` in `nspoid`, as the
session's transaction sees it. Returns 0, or 1 if there is none.

### `sm_cat_view_def`

```cpp
int sm_cat_view_def(sm_session_t* session, uint64_t oid, sm_view_def_t& out);
```

Copies the view `oid` into `out`, as the session's transaction
sees it. Returns 0, or 1 if there is none.

### `sm_cat_list_views`

```cpp
int sm_cat_list_views(sm_session_t* session, uint64_t nspoid,
                      std::vector<std::string>& names);
```

Lists the names of the views in `nspoid`, sorted, as the
session's transaction sees them.

### `sm_cat_view_readers`

```cpp
struct sm_view_reader_t {
    uint64_t                    oid;
    std::string                 name;
    std::vector<sm_view_read_t> reads;
    bool                        other = false;
};

int sm_cat_view_readers(sm_session_t* session, uint64_t oid,
                        std::vector<sm_view_reader_t>& views);
```

Fills `views`, cleared first, with the views that read the
relation `oid`, a table or a view, directly, sorted by oid, as
the session's transaction sees them. It adds other sessions'
temporary views that read it, with `other` set, which
`sm_cat_view_def` does not find (`VIEW.md` §12.3). It leaves out
those the transaction has dropped.

## Index builds, TOAST and the btree cleaner

The Index Builder (`INDEXBUILD.md`), TOAST (`TOAST.md` §2) and
the B-tree Index (`BTREEINDEX.md` §9) hold the designs. The calls
that set the environment run once, before the first `sm_opendb`.

TOAST stores a row's long values out of line. With TOAST off, the
default, each value is in its row's tuple, and a row is at most
65535 bytes. A host turns it on to say it reads TOAST pointers. A
tuple may then hold, where a variable-width value was, a pointer
to it: 22 bytes whose first is 0x01. sm moves a row's longest
values out once its tuple passes 2000 bytes.

- A write takes values. `sm_tbl_update` keeps a row's own
  pointer, given back for a column it leaves alone, with no
  write. `sm_tbl_insert` given a pointer stores a copy of its
  value.
- An index holds values always. It fetches its key and INCLUDE
  columns, so an entry never holds a pointer, and is held to
  65535 bytes itself.
- `sm_tbl_analyze` counts a value out of line as too wide to
  sort, and fetches none.

### `sm_cat_index_wait`

```cpp
int sm_cat_index_wait(sm_session_t* session, uint64_t table, uint64_t index);
```

Waits until the build of the table's index has ended, for a
caller that wants the index at once. Returns 0 once it is valid.
An interrupt to the calling thread, a bit in its wait mask
(`sm_sys.h`), ends the wait, and the bit stays pending for the
caller to take.

Errors:

- the build's own error, code and message, if the build failed;
  `SM_ERR_DUPKEY` also fills `sm_errdup`
- `SM_ERR_INTRANS` (inside a transaction)
- `SM_ERR_NOTFOUND` (no such table or index)
- `SM_ERR_INVALID` (the index is pending and no build of it runs,
  or the build failed with no error kept)
- `SM_ERR_INTERRUPTED`

### `sm_set_index_evaluator`

```cpp
typedef int (*sm_index_eval_fn)(void* ctx, uint64_t table, uint64_t index,
                                const std::string& eval,
                                const sm_datum_t* values, const bool* isnull,
                                size_t n, bool* belongs,
                                std::vector<std::string>& out,
                                bool* out_isnull) noexcept;

int sm_set_index_evaluator(sm_env_t* env, sm_index_eval_fn fn, void* ctx);
```

`sm_index_eval_fn` is the host's evaluator of partial and
expression indexes (`INDEXBUILD.md` §2). The stack evaluates
nothing itself. Where it must know whether a row belongs in such
an index, and what its expression key columns are, it calls this.

- It receives `ctx`, the table's and the index's oids, the
  index's `eval` as `sm_cat_create_index` took it, and a row as
  `n` values and null flags, one per column.
- A by-reference value points at memory good only for the call.
- It sets `*belongs`. For each key column that is an expression,
  in key order, it puts the value's bytes in `out`, or sets the
  flag in `out_isnull`. Both come sized to the count of
  expression columns. A value is a by-value type's bytes, a
  fixed-width type's, or a varlena.
- It returns 0, or -1 with the error set (`sm_seterr`).
- It must give the same answer for the same row every time.
- It may run on any thread, the Index Builder's workers among
  them, and must not call `sm.h` on the session that is writing.

`sm_set_index_evaluator` gives `env` its evaluator, once, before
any database is opened, as opening one may at once resume such an
index's build. Without one, `sm_cat_create_index` refuses a
partial or expression index, and a write to a table that has one
fails.

Errors: `SM_ERR_INVALID` (NULL `env` or `fn`, one is set already,
or a database is open).

### `sm_set_toast`

```cpp
int sm_set_toast(sm_env_t* env, int on);
```

Turns TOAST on or off for `env`, before any database is opened.
Do not turn it off over tables written with it on.

Errors: `SM_ERR_INVALID` (a database has been opened).

### `sm_set_cleaner_workers`

```cpp
int sm_set_cleaner_workers(sm_env_t* env, int workers);
```

Sets the btree cleaner's threads a database gets, 4 by default,
before any database is opened (`BTREEINDEX.md` §9). 0 runs none,
for a test that runs a pass itself with `sm_cleaner_run`. The
call does not check for a negative `workers`.

Errors: `SM_ERR_INVALID` (a database has been opened).

### `sm_cleaner_run`

```cpp
int sm_cleaner_run(sm_env_t* env, const char* dbname);
```

Runs one pass of `dbname`'s btree cleaner on this thread, for
tests.

Errors: `SM_ERR_NOTFOUND` (no session has opened the database).

### `sm_datum_is_toast`

```cpp
static inline bool sm_datum_is_toast(sm_datum_t val);
```

Returns whether `val`, a non-null variable-width value, is a
TOAST pointer.

## Tables

An open table `sm_tbl_t` is a table's definition under the
table-oid lock, its heap and its indexes. A write through it
keeps every index of the table in step with the heap
(`INDEXBUILD.md` §2). The Row Store (`ROWSTORE.md`), Clustered
Table (`CLUSTER.md`), Bulk Load (`BULKLOAD.md`) and TOAST
(`TOAST.md`) hold the designs.

**The writes.** A write is the heap write, then a write or two to
each index it changes, and it sets no savepoint. One that fails
after its first step leaves the earlier steps done, so the caller
rolls back to a savepoint of its own, or aborts. These rules hold
for each write:

- `n` must be the table's column count, dropped columns included,
  or the call fails with `SM_ERR_INVALID`.
- A table not opened to write fails with `SM_ERR_INVALID`, and a
  NULL in a NOT NULL column with `SM_ERR_NOTNULL`. Both come
  before anything is written.
- `SM_ERR_UPDATED` comes from the heap write, the first step, and
  needs no rollback.
- A key a unique index holds already is `SM_ERR_DUPKEY`, with the
  index's oid in `sm_errdup`.
- A row or a key too long is `SM_ERR_ROWTOOLONG` or
  `SM_ERR_KEYTOOLONG`.

**Calls by `_id` and by key.** A call that takes an `_id` works
on a keyless table. On a table with a primary key it fails with
`SM_ERR_INVALID`. The overload that takes an `sm_row_key_t` works
on either kind of table.

### `sm_tbl_open`

```cpp
int sm_tbl_open(sm_session_t* session, uint64_t oid, sm_lockmode_t mode,
                sm_tbl_t** out);
```

Opens the table `oid` in the session's transaction, and takes its
table-oid lock in `mode`, held to the end of the transaction.
`SM_LOCKMODE_IS` reads its rows. `SM_LOCKMODE_IX`,
`SM_LOCKMODE_SIX` or `SM_LOCKMODE_EX` writes them. The definition
cannot change until then.

Errors: `SM_ERR_NOTFOUND` (no such table, or one dropped while
the caller waited).

### `sm_tbl_tryopen`

```cpp
int sm_tbl_tryopen(sm_session_t* session, uint64_t oid, sm_lockmode_t mode,
                   sm_tbl_t** out);
```

As `sm_tbl_open`, but never waits for the table-oid lock. Returns
0 with the table open, or 1, with nothing taken and `*out`
untouched, if another transaction holds the lock in a mode that
conflicts. SQL's LOCK TABLE ... NOWAIT fails on 1.

### `sm_tbl_close`

```cpp
void sm_tbl_close(sm_tbl_t* tbl);
```

Closes a table, and frees the handle of a bulk load not ended.
The table-oid lock stays to the end of the transaction.

### `sm_tbl_def`

```cpp
const sm_table_def_t* sm_tbl_def(const sm_tbl_t* tbl);
```

Returns the table's definition, valid until `sm_tbl_close`, or
NULL with the error set if the call cannot build it. The first
call builds it, so its index states and version are as of that
call. The columns and indexes are the open's. After
`sm_tbl_truncate`, a definition already built keeps its contents,
with a new `version`.

### `sm_tbl_version`

```cpp
uint64_t sm_tbl_version(const sm_tbl_t* tbl);
```

Returns the table's `version` as its session sees it now. A DDL,
an ANALYZE or an index made valid changes it. It is `UINT64_MAX`
while the session's transaction has changed the table or its
statistics and not committed. It copies nothing: a host that only
checks what it caches asks this, not `sm_tbl_def`.

### `sm_tbl_insert`

```cpp
int sm_tbl_insert(sm_tbl_t* tbl, const sm_datum_t* values,
                  const bool* isnull, size_t n, uint64_t* id);
```

Inserts a row of `n` values and null flags. `*id` gets its `_id`.
`id` must not be NULL.

### `sm_tbl_insert_arbiter`

```cpp
int sm_tbl_insert_arbiter(sm_tbl_t* tbl, const sm_datum_t* values,
                          const bool* isnull, size_t n,
                          const uint64_t* arbiters, size_t narbiters,
                          uint64_t* id, std::string* row = nullptr);
```

Inserts a row, unless a row has its key in an arbiter. `arbiters`
names `narbiters` unique indexes by oid. With `narbiters` 0, each
unique index of the table is an arbiter. It is INSERT ... ON
CONFLICT.

Returns 0 with the new row's `_id` in `*id`. Returns 1, with
nothing written, if a row has the key. `*id` then gets that row's
`_id`, and `*row`, if `row` is not NULL, gets its tuple. The
caller takes that row's key from its tuple.

The call finds a conflict before the row is written, and leaves
nothing locked by it. It waits for a transaction that has
inserted the key and not ended. Above READ COMMITTED, a key taken
since the snapshot fails the call and the transaction. The row
found is not locked: for DO UPDATE the caller locks it with
`sm_tbl_lock`, and calls this again if it is gone by then.

Errors: `SM_ERR_INVALID` (an arbiter that is not a unique index
of the table), `SM_ERR_DUPKEY` (a duplicate in a unique index
that is not an arbiter).

### `sm_tbl_load_begin`

```cpp
int sm_tbl_load_begin(sm_tbl_t* tbl);
```

Begins a bulk load of many rows into a table opened to write, for
COPY, a restore or CREATE TABLE AS (`BULKLOAD.md` §5). A load has
three steps:

1. Call `sm_tbl_load_begin`.
2. Call `sm_tbl_load_add` for each row.
3. Call `sm_tbl_load_end`.

The rows are the transaction's own, as inserted ones are. No
other transaction sees them before commit, and an abort leaves
none.

Into an empty table, one whose store holds no version of a row,
the load writes sorted files. The store takes them whole when the
load ends, past its log and its compactions. For that:

- The call takes the table's lock X to the end of the
  transaction, if it is free now. A temporary table needs none.
  The load never waits for the lock: with another transaction
  using the table, even to read, the rows are inserted instead.
- A table with a unique tree is never loaded from files: its rows
  are inserted, because a unique tree checks a key against rows,
  and a file's rows are in no store yet.
- A duplicate key among the files' rows is found when the load
  ends, not at its row.
- The first row too long, or to be stored out of line, ends the
  files. What was handed over goes into the table, and that row
  and the rest are inserted.

Into a table that has rows, each row is inserted as
`sm_tbl_insert` would, and no lock is taken but the table's own.
An inserted row finds a duplicate key at its own row.

While a load into files is under way, the transaction cannot
commit, set a savepoint or roll back to one, and the table's
other writes fail with `SM_ERR_INVALID`. After one, the
transaction cannot be prepared. After a failed call, abort the
transaction. A load not ended ends with its transaction, and
`sm_tbl_close` frees its handle.

Errors: `SM_ERR_INVALID` (not opened to write, or a load is begun
already).

### `sm_tbl_load_add`

```cpp
int sm_tbl_load_add(sm_tbl_t* tbl, const sm_datum_t* values,
                    const bool* isnull, size_t n, uint64_t* id);
```

Hands over one row of `n` values and null flags. `*id` is as
`sm_tbl_insert`'s.

Errors: `SM_ERR_INVALID` (no load is begun), `SM_ERR_NOTNULL`.

### `sm_tbl_load_end`

```cpp
int sm_tbl_load_end(sm_tbl_t* tbl);
```

Ends the load: each row handed over is in the table. The call
frees the load, whatever it returns.

Errors: `SM_ERR_INVALID` (no load is begun), `SM_ERR_DUPKEY`.

### `sm_tbl_update`

```cpp
typedef struct sm_row_key_t {
    const sm_datum_t* values;
    const bool*       isnull;
    size_t            n;
} sm_row_key_t;

int sm_tbl_update(sm_tbl_t* tbl, uint64_t id, const sm_datum_t* values,
                  const bool* isnull, size_t n, sm_pin_ref_t* ref = nullptr,
                  const void** old = nullptr, size_t* len = nullptr);
int sm_tbl_update(sm_tbl_t* tbl, const sm_row_key_t& key,
                  const sm_datum_t* values, const bool* isnull, size_t n,
                  sm_pin_ref_t* ref = nullptr, const void** old = nullptr,
                  size_t* len = nullptr);
```

`sm_row_key_t` is a row's name (`ROWSTORE.md` §2.1): `n` values
and null flags, the values of the primary key's columns in the
key's order. In a keyless table it is the `_id` alone, an int8. A
null, or an `n` that is not the key's, fails with
`SM_ERR_INVALID`. A write does not follow a row that moved: it
fails with `SM_ERR_MOVED`.

`sm_tbl_update` replaces the row with `_id` `id`, or the row
`key` names.

With `ref`, `old` and `len` must be given too. The call pins the
tuple it replaced into `*ref`, and `*old` and `*len` view it
until the pin's release. At READ COMMITTED that tuple is the
newest, not the one the caller read. The row `*ref` held before
goes only when the write succeeds, so `values` may point into it.
On failure `*old` is NULL and `*ref` keeps what it held.

Errors: `SM_ERR_UPDATED`, `SM_ERR_SELFMODIFIED`, `SM_ERR_MOVED`,
`SM_ERR_INVALID`, and the rules for each write.

### `sm_tbl_delete`

```cpp
int sm_tbl_delete(sm_tbl_t* tbl, uint64_t id, sm_pin_ref_t* ref = nullptr,
                  const void** old = nullptr, size_t* len = nullptr);
int sm_tbl_delete(sm_tbl_t* tbl, const sm_row_key_t& key,
                  sm_pin_ref_t* ref = nullptr, const void** old = nullptr,
                  size_t* len = nullptr);
```

Deletes the row with `_id` `id`, or the row `key` names. `ref`,
`old` and `len` are as `sm_tbl_update`'s.

Errors: as `sm_tbl_update`.

### `sm_tbl_read`

```cpp
int sm_tbl_read(sm_tbl_t* tbl, uint64_t id, const void** tuple,
                size_t* len);
int sm_tbl_read(sm_tbl_t* tbl, uint64_t id, sm_pin_ref_t* ref,
                const void** tuple, size_t* len);
int sm_tbl_read(sm_tbl_t* tbl, const sm_row_key_t& key, const void** tuple,
                size_t* len);
int sm_tbl_read(sm_tbl_t* tbl, const sm_row_key_t& key, sm_pin_ref_t* ref,
                const void** tuple, size_t* len);
```

Reads the row with `_id` `id`, or the row `key` names, at the
transaction's view. Returns 0 with its tuple, or 1 if there is
none. `*tuple` and `*len` are set only on 0.

- Without a ref, the tuple is valid until the next call on this
  session.
- With a ref, the call reads into a pin with no copy, and the
  tuple is valid until the pin's release. It releases a pin
  `*ref` names first, and `*ref` is zero on 1 or -1. `ref` must
  not be NULL by `_id`. By key, NULL means no pin.

### `sm_tbl_read_latest`

```cpp
int sm_tbl_read_latest(sm_tbl_t* tbl, uint64_t id, const void** tuple,
                       size_t* len);
int sm_tbl_read_latest(sm_tbl_t* tbl, uint64_t id, sm_pin_ref_t* ref,
                       const void** tuple, size_t* len);
int sm_tbl_read_latest(sm_tbl_t* tbl, const sm_row_key_t& key,
                       const void** tuple, size_t* len);
int sm_tbl_read_latest(sm_tbl_t* tbl, const sm_row_key_t& key,
                       sm_pin_ref_t* ref, const void** tuple, size_t* len);
```

Reads the newest committed version of the row, as `sm_tbl_read`
reads one. A READ COMMITTED writer does this after
`SM_ERR_UPDATED`, before it writes again.

Errors: `SM_ERR_MOVED`.

### `sm_tbl_read_keys`

```cpp
int sm_tbl_read_keys(sm_tbl_t* tbl, const void* const* keys,
                     const size_t* lens, size_t n, sm_pin_ref_t* refs,
                     const void** tuples, size_t* tuple_lens, int* rc);
```

Reads the rows at `n` row keys in one call, each into a pin as
`sm_tbl_read_key` does (`ROWSTORE.md` §2.6). For key `i`, the row
is in `refs[i]`, `tuples[i]` and `tuple_lens[i]` with `rc[i]` 0.
If there is none, `rc[i]` is 1, `refs[i]` is zero and `tuples[i]`
is NULL. On -1, each ref is zero and each tuple NULL.

### `sm_tbl_read_key`

```cpp
int sm_tbl_read_key(sm_tbl_t* tbl, const void* key, size_t len,
                    sm_pin_ref_t* ref, const void** tuple, size_t* tlen);
```

Reads the row at `key`, a row key as an index entry names it
(`sm_idx_scan_row`), into a pin as `sm_tbl_read` does. `ref` must
not be NULL. Returns 0, or 1 if there is none.

### `sm_tbl_lock`

```cpp
int sm_tbl_lock(sm_tbl_t* tbl, uint64_t id, sm_lockmode_t mode);
int sm_tbl_lock(sm_tbl_t* tbl, const sm_row_key_t& key, sm_lockmode_t mode);
```

Locks the row with `_id` `id`, or the row `key` names, in `mode`,
without writing it, to the end of the transaction. The row need
not exist. The call waits as a write does.

- `SM_LOCKMODE_SH`: as a foreign key check locks the parent row,
  and SELECT ... FOR SHARE a row it read. Other readers of the
  key may share it, and a writer waits for it.
- `SM_LOCKMODE_EX`: as SELECT ... FOR UPDATE. Any mode but
  `SM_LOCKMODE_SH` needs the table opened to write.

Errors: `SM_ERR_SELFMODIFIED` (the transaction changed the row
after the running statement began; a FOR UPDATE cursor passes
over it), `SM_ERR_INVALID`.

### `sm_tbl_trylock`

```cpp
int sm_tbl_trylock(sm_tbl_t* tbl, uint64_t id, sm_lockmode_t mode);
int sm_tbl_trylock(sm_tbl_t* tbl, const sm_row_key_t& key,
                   sm_lockmode_t mode);
```

As `sm_tbl_lock`, but never waits. Returns 0 with the row locked,
or 1, with nothing taken, if another transaction holds the row in
a mode that conflicts, or has written it and not ended. SELECT
... FOR UPDATE NOWAIT fails on 1, and SKIP LOCKED passes over the
row.

### `sm_tbl_row_key`

```cpp
int sm_tbl_row_key(sm_tbl_t* tbl, const sm_row_key_t& key, std::string& out);
```

Sets `out` to the row key of the row `key` names, past the
table's relid: what `sm_rel_scan_key` gives of a row a cursor is
on, to tell two rows apart.

### `sm_tbl_truncate`

```cpp
int sm_tbl_truncate(sm_tbl_t* tbl);
```

Empties the table, as SQL's TRUNCATE. Its rows and each index's
entries go, in the table's transaction, at the same cost however
many there are. Columns, indexes and the row numbering stay, so
new rows go on from the last `_id` handed out. The handle stays
open on the emptied table. A transaction that began before the
truncate committed, and first reads the table after, finds it
empty, as in PostgreSQL.

Errors: `SM_ERR_INVALID` (the handle does not hold the table
`SM_LOCKMODE_EX`, from its open or a bulk load).

### `sm_tbl_scan`

```cpp
int sm_tbl_scan(sm_tbl_t* tbl, uint64_t first, uint64_t last,
                sm_rel_scan_t** out);
```

Opens a cursor over the rows with `_id` `first` to `last`, both
included, of a keyless table.

Errors: `SM_ERR_INVALID` (a table with a primary key).

### `sm_tbl_deform`

```cpp
int sm_tbl_deform(sm_tbl_t* tbl, const void* tuple, size_t len,
                  sm_datum_t* values, bool* isnull, size_t n);
```

Reads the first `n` attributes of a row's tuple into `values` and
`isnull`, one per column. Reading every column checks the whole
tuple. Reading fewer checks only what is read. A by-reference
value points into the tuple.

Errors: `SM_ERR_INVALID` (`n` is past the column count).

### `sm_pin_deform`

```cpp
int sm_pin_deform(sm_pin_ref_t ref, size_t from, size_t n,
                  sm_datum_t* values, bool* isnull);
```

Reads attributes `from` to `n - 1` of the row `ref` pins, into
`values` and `isnull` at those places. Each array is indexed by
attribute and is at least `n` long. The call resumes where the
last deform of the row stopped. If `from` is before that point,
it starts again at attribute 0 and fills those places too. It
checks only what it reads. A by-reference value points into the
row.

Errors: `SM_ERR_INVALID` (a stale ref).

### `sm_pin_decode`

```cpp
int sm_pin_decode(sm_pin_ref_t ref, size_t ordinal, sm_datum_t* val,
                  bool* isnull);
```

Reads one attribute of the row `ref` pins, as `sm_tbl_decode`
does.

Errors: `SM_ERR_INVALID` (a stale ref).

### `sm_pin_tuple`

```cpp
int sm_pin_tuple(sm_pin_ref_t ref, const void** tuple, size_t* len);
```

Gives the bytes of the row `ref` pins. On a stale ref, `*tuple`
is NULL and `*len` is 0.

Errors: `SM_ERR_INVALID` (a stale ref).

### `sm_tbl_decode`

```cpp
int sm_tbl_decode(sm_tbl_t* tbl, const void* tuple, size_t len,
                  size_t ordinal, sm_datum_t* val, bool* isnull);
```

Reads one attribute of a row's tuple, by `ordinal`, into `*val`
and `*isnull`. A null or dropped attribute sets `*isnull`.

### `sm_tbl_fetch`

```cpp
int sm_tbl_fetch(sm_tbl_t* tbl, sm_datum_t val, std::string* out);
```

Reads the value the TOAST pointer `val` names, one
`sm_tbl_deform` or `sm_tbl_decode` gave of this table, into
`*out`: its bytes, with no varlena header. It reads as the
session's transaction sees the table, so fetch a pointer in the
statement that read it. The old tuple `sm_tbl_update` and
`sm_tbl_delete` hand back names values that write erased. The
statement that made the write still sees them, and no later one
does.

Errors: `SM_ERR_INVALID` (not a pointer of this table's store),
`SM_ERR_NOTFOUND` (the value is gone).

### `sm_toast_fetch`

```cpp
int sm_toast_fetch(sm_session_t* session, sm_datum_t val, std::string* out);
```

Reads the value the TOAST pointer `val` names as `sm_tbl_fetch`
does, with no table: the pointer names the store. The table it
was read from must be open in the session's transaction, which
keeps the store from being dropped.

Errors: `SM_ERR_INVALID` (not a pointer), `SM_ERR_NOTFOUND` (the
value is gone).

### `sm_tbl_idx_scan`

```cpp
int sm_tbl_idx_scan(sm_tbl_t* tbl, uint64_t index, const sm_bound_t* lo,
                    const sm_bound_t* hi, bool* widened,
                    sm_idx_scan_t** out);
```

Opens a cursor over the entries of the table's `index`, by oid,
between `lo` and `hi`, either NULL for no bound. A bound too long
to encode widens the scan, and `*widened`, if `widened` is not
NULL, says so.

Errors: `SM_ERR_NOTFOUND` (the table has no such index),
`SM_ERR_INVALID` (`sm_tbl_index_usable` is false).

### `sm_tbl_scan_rows`

```cpp
int sm_tbl_scan_rows(sm_tbl_t* tbl, sm_rel_scan_t** out);
```

Opens a cursor over each row of the table, in its key's order:
its primary key's, or with none its `_id`'s (`CLUSTER.md` §5). A
sequential scan reads this.

### `sm_tbl_idx_scan_rows`

```cpp
int sm_tbl_idx_scan_rows(sm_tbl_t* tbl, uint64_t index, const sm_bound_t* lo,
                         const sm_bound_t* hi, bool* widened,
                         sm_rel_scan_t** out);
```

Opens a cursor over the rows of `index`, between `lo` and `hi` as
`sm_tbl_idx_scan` takes them, in key order.

- On the table's primary key, it gives the rows themselves, one
  sorted run (`CLUSTER.md` §5). It seeks no `_id`.
- On a tree (`BTREEINDEX.md` §5), it gives each entry's row as
  the transaction sees it, while the row still has the entry's
  key, in the tree's order. A tree's cursor seeks nothing.

Errors: `SM_ERR_NOTFOUND` (no such index), `SM_ERR_INVALID`
(neither of the two, or `sm_tbl_index_usable` is false).

### `sm_tbl_index_usable`

```cpp
bool sm_tbl_index_usable(const sm_tbl_t* tbl, uint64_t index);
```

Returns whether the session may scan the table's index now. It
may not while the index is pending, once it has failed, or in a
statement or transaction whose snapshot is older than the index's
build.

### `sm_tbl_index_tree`

```cpp
bool sm_tbl_index_tree(const sm_tbl_t* tbl, uint64_t index);
```

Returns whether the table's index is a tree (`BTREEINDEX.md`),
whose rows a scan reads through `sm_tbl_idx_scan_rows`. It
returns false for no such index.

### `sm_tbl_idx_ncols`

```cpp
size_t sm_tbl_idx_ncols(const sm_tbl_t* tbl, uint64_t index);
```

Returns the number of attributes in an entry's value in that
index, or 0 for no such index. The attributes are the cut flag,
the INCLUDE columns, then the row key (`ROWSTORE.md` §2.2). A
tree's entry gives each INCLUDE column as null.

### `sm_tbl_idx_deform`

```cpp
int sm_tbl_idx_deform(sm_tbl_t* tbl, uint64_t index, const void* value,
                      size_t len, sm_datum_t* values, bool* isnull,
                      size_t n);
```

Reads the first `n` attributes of an entry's value in that index,
as `sm_tbl_deform` does.

Errors: `SM_ERR_NOTFOUND` (no such index), `SM_ERR_INVALID` (`n`
is past the attribute count).

## Statistics

Statistics holds the design (`STATISTICS.md` §9). The size calls
read the table's LSM live, and nothing stores the answer. Each
size is apart: bytes in sorted files (`disk`) and in memtables
(`mem`), never summed. `include/sm_stats.h` holds the types.

### `sm_tbl_size`

```cpp
int sm_tbl_size(sm_tbl_t* tbl, uint64_t* disk, uint64_t* mem);
```

Gives the bytes of the table's rows, their older versions
included.

### `sm_tbl_keys_at`

```cpp
int sm_tbl_keys_at(sm_tbl_t* tbl, const double* fs, size_t n,
                   std::vector<std::string>& keys);
```

Fills `keys` with one row key for each of the fractions
`fs[0..n)` of the bytes of the table's rows. A key is a position
in key order below which about that share of the bytes lie, by
sm's estimates, to a data block or so (`ROWSTORE.md` §2.6). The
call reads no row, and no row need have the key. The key at
fraction 0 is empty. A sample scan of a table with a primary key
cuts it into blocks by these.

Errors: `SM_ERR_INVALID` (a fraction outside [0, 1), or not
ascending).

### `sm_tbl_idx_size`

```cpp
int sm_tbl_idx_size(sm_tbl_t* tbl, uint64_t index, uint64_t* disk,
                    uint64_t* mem);
```

Gives the bytes of one index. A primary key's are 0, because its
entries are the rows. A tree's are its pages.

Errors: `SM_ERR_NOTFOUND` (no such index).

### `sm_tbl_rowno_limit`

```cpp
int sm_tbl_rowno_limit(sm_tbl_t* tbl, uint64_t* n);
```

Sets `*n` to the last `_id` handed out, or 0 if none, as in a
table with a primary key.

### `sm_tbl_probes`

```cpp
int sm_tbl_probes(sm_tbl_t* tbl, uint64_t* n);
```

Sets `*n` to the sorted runs a lookup in the table's store may
visit.

### `sm_tbl_compact`

```cpp
int sm_tbl_compact(sm_tbl_t* tbl);
```

Compacts the table's store, SQL's VACUUM:

1. Flush the store's memtables, and merge each level into the
   bottom one, so a lookup visits one sorted run and old versions
   no reader needs go.
2. Remove each tree's dead entries, and set a leaf with no entry
   in doubt all visible (`BTREEINDEX.md` §9).

The call waits for the compaction, seconds for a table of
hundreds of megabytes, and cannot be cut short. The handle's lock
keeps DDL out meanwhile.

### `sm_tbl_analyze`

```cpp
int sm_tbl_analyze(sm_tbl_t* tbl, int32_t target);
```

Analyzes the table in its transaction. It samples up to 300 x
`target` of its rows, and reads no more than ten times that in
`_id`s, whatever the table's size. It records each column's
statistics, each partial or expression index's from the
evaluator, and the sizes. They commit and roll back with the
transaction. A second analyze of the table waits. A table whose
heap holds no bytes and whose sample reads no row, as a new or
truncated one, is not analyzed, and the call returns 0.

Errors: `SM_ERR_INVALID` (a `target` below 1, a temporary table,
or an index to evaluate and no evaluator).

### `sm_tbl_tryanalyze`

```cpp
int sm_tbl_tryanalyze(sm_tbl_t* tbl, int32_t target);
```

As `sm_tbl_analyze`, but never waits for another analyze of the
table. Returns 0 analyzed, or 1, having recorded nothing, if
another transaction's analyze of it has not ended. SQL's ANALYZE
(SKIP_LOCKED) passes over the table on 1.

### `sm_tbl_stats`

```cpp
int sm_tbl_stats(sm_session_t* session, uint64_t oid,
                 std::shared_ptr<const sm_stats_t>& out);
```

Gives the statistics of the table `oid`, as the session sees
them. Returns 0 with them in `out`, or 1 with `out` empty if it
was never analyzed, is temporary, or is not there.

The statistics are sm3's, shared and read-only among all callers
in the process. sm3 holds them in memory and loads them once on
first use. The call copies nothing and takes no lock. A holder
keeps the ones it was given, whatever is analyzed since.

`sm_stats_t` (`include/sm_stats.h`) holds `format`,
`sm_stats_format`; `target`; `run_length`; `rows_sampled`;
`rowno_limit`, N when analyzed; `live_rows`; `heap_disk`;
`heap_mem`; `avg_row_bytes`; `indexes`; and `stats`.

- `sm_stats_index_t` is an index's `oid`, `disk`, `mem`,
  `entries` and `belongs`, the fraction of rows in it.
- `sm_stat_t` is one column's statistics, or one expression's of
  an index: PostgreSQL's six. `has_index` and `index` name the
  index, and `position` the column or the expression's place.
  `null_frac`, `width`, `distinct`, `correlation` (if
  `has_correlation`), `mcv` with `mcv_freq`, and `bounds` hold
  the values. A `distinct` below 0 is minus a fraction of the
  rows.
- A value in `mcv` or `bounds` is its bytes as a datum of its
  type, a varlena for a variable-width one. `sm_datum_of_bytes`
  reads one.

## Table and index DDL

The Catalog holds the design (`CATALOG.md` §8), with Temporary
Table (`TEMPTABLE.md`), Index Builder (`INDEXBUILD.md`) and
Foreign Key (`FOREIGNKEY.md`). Each call works in the session's
transaction. A call on an existing table takes its table-oid lock
X to the end of the transaction, unless the table is temporary or
the entry says otherwise. A table not there is `SM_ERR_NOTFOUND`.

### `sm_cat_create_table`

```cpp
struct sm_column_spec_t {
    const char* name;
    uint8_t     type_code;  /* sm2/TYPE.md section 3 */
    int32_t     typmod;
    bool        not_null;
    const char* default_expr = nullptr; /* the host's default, opaque */
    char        identity = 0;     /* 'a', 'd' or 0 */
    uint64_t    identity_seq = 0; /* an identity column's sequence */
    const char* on_update_expr = nullptr; /* the host's update default */
};

int sm_cat_create_table(sm_session_t* session, uint64_t nspoid,
                        const char* name,
                        const std::vector<sm_column_spec_t>& columns,
                        uint64_t* oid);
```

Creates the table `name` in `nspoid` with these columns: its
store, its `_id` sequence and its definition. The session's
transaction sees it at once, and others at commit. An identity
column owns its sequence from then on.

Errors:

- `SM_ERR_DUPKEY` (the name is taken)
- `SM_ERR_NOTFOUND` (no such namespace, `SM_NSP_TEMP` among them)
- `SM_ERR_INVALID`: no name; a column with no name, or two with
  one; a type code sm2 lacks, or a typmod its type refuses; an
  identity column that is not a NOT NULL smallint, integer or
  bigint with a sequence of this namespace, owned by none, and no
  default

### `sm_cat_create_temp_table`

```cpp
int sm_cat_create_temp_table(sm_session_t* session, const char* name,
                             const std::vector<sm_column_spec_t>& columns,
                             uint64_t* oid);
```

Creates the temporary table `name` with these columns
(`TEMPTABLE.md`). It is the session's alone, seen by no other,
and gone when the session closes. It is in no namespace, and is
found by name with `SM_NSP_TEMP`. Its rows are never logged and
take no locks, but follow the session's transactions as any
table's do. The other calls take its oid as they take any
table's. `sm_tbl_analyze` fails with `SM_ERR_INVALID`, and so
does preparing a transaction that made, changed or wrote one.

Errors: `SM_ERR_DUPKEY` (another of the session's temporary
tables or their indexes has the name), `SM_ERR_INVALID` (as
`sm_cat_create_table`, or an identity column).

### `sm_cat_drop_table`

```cpp
int sm_cat_drop_table(sm_session_t* session, uint64_t oid);
```

Drops the table `oid`: its store, rows, indexes, its own foreign
keys, and the sequences its columns own.

Errors: `SM_ERR_INUSE` (a view reads it, or another table's
foreign key references it).

### `sm_cat_rename_table`

```cpp
int sm_cat_rename_table(sm_session_t* session, uint64_t oid,
                        const char* name);
```

Renames the table `oid` to `name`, in its namespace.

Errors: `SM_ERR_DUPKEY`, `SM_ERR_INVALID` (no name).

### `sm_cat_add_column`

```cpp
int sm_cat_add_column(sm_session_t* session, uint64_t oid,
                      const sm_column_spec_t& col, const sm_datum_t* missing);
```

Appends `col` to the table `oid`. A row written before it reads
it as the datum `*missing` of `col`'s type, or as null when
`missing` is NULL. A NOT NULL column with no `missing` needs an
empty table.

Errors:

- `SM_ERR_BLOCKED` (NOT NULL with no `missing`, and the table is
  in use: the call does not wait)
- `SM_ERR_NOTNULL` (NOT NULL with no `missing`, and the table has
  rows)
- `SM_ERR_DUPKEY` (a live column has the name)
- `SM_ERR_INVALID` (as a column of `sm_cat_create_table`, a
  `missing` that is not a datum of the type, or an identity
  column)

### `sm_cat_set_not_null`

```cpp
int sm_cat_set_not_null(sm_session_t* session, uint64_t oid,
                        const char* name);
```

Makes the column `name` of the table `oid` NOT NULL. The call
scans the table, and a row older than the column counts by its
missing value. A column already NOT NULL returns 0 with no scan.

Errors: `SM_ERR_NOTNULL` (a row holds a null in it),
`SM_ERR_NOTFOUND` (no such column).

### `sm_cat_alter_column_type`

```cpp
struct sm_column_type_t {
    uint8_t     type_code;  /* sm2/TYPE.md section 3 */
    int32_t     typmod;
    const char* default_expr = nullptr;   /* the host's, opaque */
    const char* on_update_expr = nullptr; /* the host's update default */
};

typedef int (*sm_column_conv_fn)(void* ctx, const sm_datum_t* values,
                                 const bool* isnull, size_t n,
                                 std::string& out, bool* out_isnull) noexcept;

int sm_cat_alter_column_type(sm_session_t* session, uint64_t oid,
                             const char* name, const sm_column_type_t& to,
                             sm_column_conv_fn conv, void* ctx);
```

`sm_column_type_t` is a column's new type: sm2's code and typmod,
and the host's default and update default for the new type, which
replace the column's. NULL is none.

`sm_column_conv_fn` is the host's conversion of one row's value
of the column to the new type. It receives `ctx` and the row as
`n` values and null flags, one per column before the change. It
puts the new value's bytes in `out`, a by-value type's bytes, a
fixed-width type's, or a varlena, or it sets `*out_isnull`. A
by-reference value points at memory good only for the call. It
returns 0, or -1 with the error set (`sm_seterr`). It runs on the
thread of the session that made this call, and must not call
`sm.h` on that session.

`sm_cat_alter_column_type` changes the column `name` of the table
`oid` to the type `to`, under the table-oid lock X.

- With `conv` and a table with rows, the call rewrites the table
  into a new store. Each row's value of the column is `conv`'s,
  and each index on the column is made under the new type, a key
  column's rows keyed anew.
- With `conv` and an empty table, or with `conv` NULL, only the
  record changes.
- With `conv` NULL, every value must stay as it is, as
  `sm_retype_keeps_values` says.

The column's statistics go.

Errors:

- `SM_ERR_NOTFOUND` (no such column)
- `SM_ERR_INUSE` (an expression or a partial index, a foreign key
  on either side, or a view reads the column)
- `SM_ERR_INVALID` (an identity column, a change without `conv`
  whose values would change, or a type code or typmod sm2
  refuses)
- `SM_ERR_DUPKEY` (two rows with one key, a row's key in
  `sm_errdup`)
- `SM_ERR_NOTNULL` (`conv`'s null in a NOT NULL column)
- `conv`'s own error

### `sm_cat_column_use`

```cpp
typedef enum sm_column_use_t {
    SM_COLUMN_USE_NONE,
    SM_COLUMN_USE_INDEX,      /* an expression or a partial index reads it */
    SM_COLUMN_USE_FKEY,       /* a foreign key of the table names it */
    SM_COLUMN_USE_REFERENCED, /* a foreign key referencing it names it */
    SM_COLUMN_USE_VIEW,       /* a view reads it */
    SM_COLUMN_USE_IDENTITY    /* it is an identity column */
} sm_column_use_t;

int sm_cat_column_use(sm_session_t* session, uint64_t oid, int32_t at,
                      sm_column_use_t* use, std::string& by);
```

Sets `*use` to what keeps the column `at`, 0-based, of the table
`oid` from changing type, and `by` to its name, or empty. This is
what `sm_cat_alter_column_type` refuses, asked first for a host's
own words. A view comes first, then the others in the order
`sm_column_use_t` lists them. The call takes the table-oid lock
X, as any DDL does, so a session that does not hold it waits.

Errors: `SM_ERR_NOTFOUND` (no such table), `SM_ERR_INVALID` (no
such column).

### `sm_retype_keeps_values`

```cpp
bool sm_retype_keeps_values(uint8_t from_code, int32_t from_typmod,
                            uint8_t to_code, int32_t to_typmod);
```

Returns whether each stored value and key of a column stays a
value of the new type, so that `sm_cat_alter_column_type` takes
the change with no converter. It is true for the same code and
typmod. It is also true between text and varchar, if `to_typmod`
is negative, or `from_typmod` is not negative and `to_typmod` is
at least `from_typmod`.

### `sm_cat_set_identity`

```cpp
int sm_cat_set_identity(sm_session_t* session, uint64_t oid,
                        const char* name, char identity, uint64_t seq);
```

Makes the column `name` of the table `oid` an identity column,
`'a'` or `'d'`, that owns the sequence `seq`.

Errors: `SM_ERR_NOTFOUND` (no such column or sequence),
`SM_ERR_INVALID` (a column that is not a NOT NULL smallint,
integer or bigint with no default and no identity; a temporary
table; a sequence in another namespace, or owned).

### `sm_cat_rename_column`

```cpp
int sm_cat_rename_column(sm_session_t* session, uint64_t oid,
                         const char* name, const char* new_name);
```

Renames the column `name` of the table `oid` to `new_name`.

Errors: `SM_ERR_NOTFOUND` (no such column), `SM_ERR_DUPKEY` (a
live column has the name), `SM_ERR_INVALID` (no name).

### `sm_cat_drop_column`

```cpp
int sm_cat_drop_column(sm_session_t* session, uint64_t oid,
                       const char* name);
```

Drops the column `name` of the table `oid`. It reads as null from
then on, and the sequences it owns go with it.

Errors: `SM_ERR_NOTFOUND` (no such column), `SM_ERR_INUSE` (a
view reads it, or a foreign key of the table uses it),
`SM_ERR_INVALID` (an index uses it).

### `sm_cat_create_index`

```cpp
struct sm_index_key_spec_t {
    int32_t attnum;         /* the column's 0-based place; -1: expression */
    bool    desc;
    bool    nulls_first;
    uint8_t type_code;      /* an expression's (sm2/TYPE.md section 3) */
    int32_t typmod;         /* an expression's */
};

int sm_cat_create_index(sm_session_t* session, uint64_t table,
                        const char* name,
                        const std::vector<sm_index_key_spec_t>& keys,
                        const std::vector<int32_t>& includes, bool unique,
                        const std::string& eval,
                        const std::vector<int32_t>& reads, uint64_t* oid,
                        bool primary = false, bool constraint = false,
                        bool tree = false);
```

`sm_index_key_spec_t` is a key column as an index is created with
it, with no collation. With `attnum` -1 it is an expression,
which has no column behind it and gives its own type code and
typmod.

`sm_cat_create_index` creates the index `name` on `table`, keyed
by `keys`, with the INCLUDE columns `includes` by 0-based place.

- On a table this transaction made, the index is filled and valid
  at once.
- On any other table, the index is recorded as pending, and
  filled in the background once the transaction commits.
  `sm_cat_index_wait` waits for that.
- A primary index has no entries to fill. The call keys the rows
  anew under the table-oid lock X, rewriting a table that has
  rows, and the index is valid at once.

A partial or an expression index gives `eval`, its predicate and
its expressions serialized as the host likes, and `reads`, the
columns they read. The stack keeps `eval` and hands it to the
evaluator. An update that changes none of `reads` leaves the
index alone. A plain index gives both empty.

A `primary` index is the table's primary key: unique, on columns
alone, each NOT NULL, and the table's only one. A `constraint`
index is one a UNIQUE or PRIMARY KEY constraint made, not CREATE
UNIQUE INDEX: unique and on columns alone, a mark the host reads.
A `tree` index is a B-tree, and cannot be primary.

Errors: `SM_ERR_DUPKEY` (the name is taken), `SM_ERR_INVALID` (a
rule above is broken, no name or no key, a key or an INCLUDE that
names no live column, `reads` with no `eval`, or `eval` with no
evaluator).

### `sm_cat_drop_index`

```cpp
int sm_cat_drop_index(sm_session_t* session, uint64_t table, uint64_t index);
```

Drops the index `index` of `table`, and its entries. Dropping a
primary key keys the rows anew. A tree's pages go once the drop
has committed.

Errors: `SM_ERR_NOTFOUND` (no such index), `SM_ERR_INUSE` (a
foreign key uses it).

### `sm_cat_create_fkey`

```cpp
struct sm_fkey_def_t {
    uint64_t             oid = 0;
    std::string          name;
    uint64_t             ref_table = 0;
    uint64_t             ref_index = 0;
    std::vector<int32_t> columns;
    std::vector<int32_t> ref_columns;
    char                 on_update = 'a';
    char                 on_delete = 'a';
    char                 match = 's';
    bool                 deferrable = false;
    bool                 deferred = false;
    bool                 validated = true; /* the rows there were checked */
    std::string          host;
};

int sm_cat_create_fkey(sm_session_t* session, uint64_t table,
                       const sm_fkey_def_t& def, uint64_t* oid);
```

`sm_fkey_def_t` is a foreign key of a table, its child
(`FOREIGNKEY.md`): the parent `ref_table`, its unique index
`ref_index` on `ref_columns`, and the column pairs by 0-based
place. The actions are `'a'` no action, `'r'` restrict, `'c'`
cascade, `'n'` set null and `'d'` set default. The match types
are `'s'` simple and `'f'` full. These, `deferrable`, `deferred`
and `host` are the host's to act on: sm only keeps them.

`sm_cat_create_fkey` adds `def` to `table` (`FOREIGNKEY.md` §5),
under the child's table-oid lock X and then the parent's. `*oid`
gets its oid, and `def.oid` is ignored. The call reads no row:
`def.validated` is the caller's word, after it has checked them.
Once the foreign key is there, dropping the parent, its index, or
a column of the child it uses is `SM_ERR_INUSE`.

Errors: `SM_ERR_DUPKEY` (the child has a foreign key of the
name), `SM_ERR_INVALID`, unless each of these holds:

- it has a name, a known action and a known match type
- it has 1 to 32 column pairs, each column live
- `def.ref_index` is a valid unique index of the parent with no
  predicate or expression, whose keys are `def.ref_columns` in
  any order
- both tables are temporary, or neither is

### `sm_cat_drop_fkey`

```cpp
int sm_cat_drop_fkey(sm_session_t* session, uint64_t table, uint64_t fkey);
```

Drops the foreign key `fkey` of `table`, under both tables' locks
X.

Errors: `SM_ERR_NOTFOUND`.

### `sm_cat_validate_fkey`

```cpp
int sm_cat_validate_fkey(sm_session_t* session, uint64_t table,
                         uint64_t fkey);
```

Marks the foreign key `fkey` of `table` validated, its rows
checked.

Errors: `SM_ERR_NOTFOUND`.

### `sm_cat_rename_fkey`

```cpp
int sm_cat_rename_fkey(sm_session_t* session, uint64_t table, uint64_t fkey,
                       const char* name);
```

Renames the foreign key `fkey` of `table`.

Errors: `SM_ERR_DUPKEY`, `SM_ERR_NOTFOUND`, `SM_ERR_INVALID` (no
name).

### `sm_cat_fkey`

```cpp
int sm_cat_fkey(sm_session_t* session, uint64_t fkey, uint64_t* table,
                sm_fkey_def_t* out);
```

Gives the foreign key with the oid `fkey` and its table, the
child, as the session's transaction sees them. Returns 0, or 1 if
there is none. It takes no lock: the host holds the child's.
