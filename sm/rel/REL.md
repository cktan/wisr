# Relational Driver Reference (`rel`)

The `rel` driver provides a Janet-based scripting interface
and interactive environment for driving the storage engine
via [`include/sm.h`](../include/sm.h).
Source bindings are implemented in
[`rel_ops.cc`](src/rel_ops.cc).

Primitives are grouped below by entity type.

---

## 1. Environment & Filesystem

Manipulate storage home directories and filesystem paths.

### `(rm-rf path)`
Recursively removes the file or directory tree at `path`.

```janet
(rm-rf "/tmp/wisr_home")
```

### `(env-init path)`
Formats a fresh wisr storage home directory at `path`.

```janet
(env-init "/tmp/wisr_home")
```

### `(env-attach path [:toast bool])`
Attaches to an existing storage home directory at `path` and
initializes the environment handle for the process. With
`:toast true`, a row's long values are stored out of line
(`sm_set_toast`): a row may then be longer than 64 KB. A value
read or scanned comes back whole. The old row `update` and
`delete` return has the keyword `:toast` where a value was out
of line, since that write erased it.

```janet
(env-attach "/tmp/wisr_home")
(env-attach "/tmp/wisr_home" :toast true)
```

### `(env-detach [:force bool])`
Detaches from the attached storage home environment. If `:force`
is `true`, flushes and tears down active session handles.

```janet
(env-detach :force true)
```

---

## 2. Databases & Sessions

Manage database instances and concurrent client sessions.

### `(db-create name)`
Creates a new database named `name` in the attached environment.

```janet
(db-create "mydb")
```

### `(db-drop name)`
Drops the database named `name` from the attached environment.

```janet
(db-drop "mydb")
```

### `(db-list)`
Returns an array of strings naming all databases in the
environment.

```janet
(def dbs (db-list))
```

### `(db-open name [opts])`
Opens a database and binds it to the current default session.
`opts` may hold `:savepoint-undo-bytes`, the cap on what one
transaction keeps to roll back to its savepoints
(`sm_options_t`). It takes effect if this is the database's
first session.

```janet
(db-open "mydb")
(db-open "mydb" {:savepoint-undo-bytes 4096})
```

### `(db-close)`
Closes the active default session.

```janet
(db-close)
```

### `(session-open name db-name)`
Opens an additional named session connected to `db-name`.
Useful for multi-client concurrency and isolation tests.

```janet
(session-open s2 "mydb")
```

### `(session-use name)`
Switches the active session context to `name`. The special name
`default` refers to the primary session.

```janet
(session-use s2)
(session-use default)
```

### `(session-close name)`
Closes the named session.

```janet
(session-close s2)
```

---

## 3. Transactions & Two-Phase Commit (2PC)

Control transaction lifecycles, statement snapshots, savepoints,
and distributed 2PC resolution.

### `(begin [level [:read-only]])`
Begins a new transaction in the active session. The isolation
level defaults to `:rc` (`read-committed`). Supported levels:
- `:rc` / `:read-committed`
- `:rr` / `:repeatable-read`
- `:ser` / `:serializable`

`:read-only` after the level declares the transaction READ
ONLY: a write fails, except to a temporary table.

```janet
(begin :rc)
(begin :rr)
(begin :ser)
```

### `(commit)`
Commits the active transaction, making its changes durable.

```janet
(commit)
```

### `(abort)`
Aborts the active transaction, rolling back all uncommitted
modifications.

```janet
(abort)
```

### `(snapshot)`
Begins a statement. Inside a read-committed transaction its
reads see one snapshot. At every level its reads see the
transaction's own writes made before it began, not its own:
an `update` or `delete` of a row the statement already changed
fails with `ERR_SELFMODIFIED`, which leaves the transaction
active. `(stmt-begin)` is supported as an alias.

```janet
(snapshot)
```

### `(stmt-inner)` and `(stmt-end)`
`(stmt-inner)` begins a statement inside the running one, as a
trigger's commands run inside the statement that fired them:
it sees the outer's writes so far. `(stmt-end)` ends it, and the
outer statement resumes with its own view.

```janet
(stmt-inner)
(insert audit [1 "fired"])
(stmt-end)
```

### `(stmt-advance)`
Advances the running statement to now, as PostgreSQL's
`CommandCounterIncrement` does: its later reads see the
transaction's writes so far, its own included, at the same
snapshot. A sequence or a table the statement made is usable
after it.

```janet
(stmt-begin)
(table-create t (columns (id :int4)))
(stmt-advance)
(insert t [1])
```

### `(qa-nowait on)`
Sets whether the active session waits for locks. With `true`,
an operation that would wait for another session's lock fails
with `ERR_BLOCKED` instead. The transaction stays active, and
the script polls by repeating the operation once the other
session has let go. `rel` runs one session at a time, so this
is how a script says "this step blocks". The setting lasts
until it is changed, across transactions; `false` is the
default, and a blocked step then hangs the script.

```janet
(qa-nowait true)
(assert-err :err-blocked (update accounts 1 [1 400]))
```

### `(savepoint)`
Creates a transaction savepoint and returns its integer depth.

```janet
(def sp (savepoint))
```

### `(rollback depth)`
Rolls back the active transaction to the savepoint at `depth`.

```janet
(rollback sp)
```

### `(forget depth)`
Releases and forgets the savepoint at `depth` without rolling
back changes made since it was taken.

```janet
(forget sp)
```

### `(prepare gid)`
Prepares the active transaction under global transaction ID `gid`
in a two-phase commit protocol. The session becomes idle and
disassociated from the transaction. A transaction that made DDL
cannot be prepared: it fails with `ERR_INVALID`.

```janet
(prepare "tx-coord-001")
```

### `(prepared-list)`
Returns an array of structs `{:gid string :prepared-at integer}`
describing all in-doubt prepared transactions.

```janet
(def txs (prepared-list))
```

### `(commit-prepared gid)`
Commits an in-doubt prepared transaction identified by `gid`.

```janet
(commit-prepared "tx-coord-001")
```

### `(abort-prepared gid)`
Aborts an in-doubt prepared transaction identified by `gid`.

```janet
(abort-prepared "tx-coord-001")
```

---

## 4. Sequences

Named atomic monotonically increasing counters. Sequence
advancements survive transaction aborts and rollbacks.

### `(seq-create name [start [increment]])`
Creates a sequence named `name`. Defaults to `start=1` and
`increment=1`. Sequence definition writes participate in DDL
transactions.

```janet
(seq-create s1 100 5)
```

### `(seq-drop name)`
Drops the named sequence.

```janet
(seq-drop s1)
```

### `(nextseq name)`
Atomically advances and returns the next sequence number as a
number. Operates outside MVCC undo; values are strictly monotonic
and never recycled on abort.

```janet
(def val (nextseq s1))
```

---

## 5. Schema: Tables & Indexes (DDL)

Define and manage heap tables and secondary indexes. Tables and
indexes live in sm3's catalog, in the `public` namespace. Every
DDL statement runs in the active transaction: it is undone by
`(abort)` or `(rollback sp)`, seen by other sessions only once
committed, and kept across a restart.

### `(table-create name (columns ...))`
Defines and creates a heap table, backed by a store and a row
sequence of its own.

Supported column types:
- Integer: `:int2`, `:int4`, `:int8`
- Floating: `:float4`, `:float8`
- Numeric: `:decimal64`, `:decimal128`
- Boolean: `:bool`
- Text: `:text`, `:varchar`
- UUID: `:uuid`
- Temporal: `:date`, `:time`, `:timestamp`, `:timestamptz`,
  `:interval`

Numbers after the type are its modifier, as in SQL:
`(price :decimal64 10 2)` is `decimal(10, 2)`,
`(at :timestamp 3)` keeps 3 fraction digits, and
`(code :varchar 20)` is `varchar(20)`. A decimal must be given
one.

Values are Janet numbers for the integers and floats, booleans
for `:bool`, and strings for the text types. Every other type is
written and read as PostgreSQL's canonical text for it --
`"2026-09-26"`, `"12.30"` under `decimal(10, 2)`,
`"P1Y2M3DT4H5M6S"` -- which is the only text accepted.

Column qualifier:
- `:not-null` -- requires non-null value on insert/update.

```janet
(table-create users
  (columns
    (id :int4 :not-null)
    (name :text :not-null)
    (email :text)
    (active :bool)))
```

### `(temp-table-create name (columns ...))`
Creates a temporary table, as `table-create` does: the
session's alone, seen by no other session, and gone when the
session closes. Every primitive that takes a table or index
name looks among the session's temporary tables first and
then in `public`, so a temporary table hides a table of the
same name.

```janet
(temp-table-create scratch
  (columns
    (id :int4 :not-null)))
```

### `(table-drop name)`
Drops the heap table `name`, its underlying store, and its
associated row sequence.

```janet
(table-drop users)
```

### `(namespace-create name)`
Creates the namespace `name`, SQL's schema, in the active
transaction. A name another namespace has is refused.

```janet
(namespace-create app)
```

Tables are still made in `public`: `rel` has no way to name
another namespace for one.

### `(namespace-drop name)`
Drops the namespace `name`. One that holds a table or a
collation is refused, and so are `public` and `pg_catalog`.

```janet
(namespace-drop app)
```

### `(list-namespaces)`
Returns an array of the names of the namespaces, sorted, as
the active transaction sees them.

```janet
(list-namespaces)
# => @["app" "pg_catalog" "public"]
```

### `(list-tables)`
Returns an array of the names of the tables in `public`, sorted,
as the active transaction sees them: its own uncommitted DDL
included, another session's not. It takes no lock.

```janet
(list-tables)
# => @["products" "users"]
```

### `(describe-table name [:include-indices])`
Returns a struct describing table `name`, under its table-oid
lock intent-shared, as a read takes it. `:columns` holds each
live column in the form `table-create` takes it: its name,
type, the numbers after the type, and `:not-null`. A decimal
always shows
both numbers: `(price :decimal64 10)` comes back as
`[price :decimal64 10 0]`. With `:include-indices`, `:indexes`
holds each index as a struct of its `index-create` clauses,
every key column's order and null placement spelled out.

The forms are bracket tuples, so compare against a quoted
literal: a quoted `[...]` keeps its brackets, which `=` tells
apart from an evaluated one.

```janet
(describe-table users :include-indices)
# => {:name "users"
#     :columns [[id :int4 :not-null] [name :text]]
#     :indexes [{:name "idx_users_name" :unique false
#                :key [[name :asc :nulls-last]] :include [id]}]}
(assert-eq ((describe-table users) :columns)
           '[[id :int4 :not-null] [name :text]])
```

### `(index-create name (table tbl) ...)`
Creates a secondary index on table `tbl`. On a table made in
the same transaction it is filled and valid at once. On a table
that already holds committed rows it is built in the
background once the transaction commits; `(index-wait name)`
waits for that.

Clauses:
- `(table tbl)` -- target table name.
- `(key (col [:asc|:desc] [:nulls-first|:nulls-last])...)` -- key
  columns with ordering and null placement. In a column's
  place, `(lower col)` or `(upper col)` makes the key column an
  expression of a text column.
- `:unique bool` -- enforce unique constraint on key columns.
- `:include (col...)` -- non-key payload columns embedded
  in index tuples.
- `:where (col value)` -- a partial index: only the rows whose
  `col` prints as `value` are in it. A null equals nothing.

The storage manager evaluates neither form. It calls `rel`'s
own evaluator, as it would call PostgreSQL's in the fork.

```janet
(index-create idx_users_email
  (table users)
  (key (email :asc))
  :unique true)

(index-create idx_users_name
  (table users)
  (key (name :asc))
  :unique false
  :include (id active))
```

### `(index-drop name)`
Drops the secondary index named `name`.

```janet
(index-drop idx_users_email)
```

### `(index-wait name)`
Waits until the build of the index named `name` has ended. An
index created on a table that already holds committed rows is
built in the background (`sm3/SM3_DESIGN.md` §3): writers and
readers go on meanwhile, but the index may not be scanned
until it is valid. One created in the same transaction as its
table is valid at once and needs no wait. Call it outside a
transaction. If the build failed, on two rows with one key in
a unique index for one, it raises the build's own error.

```janet
(index-create idx_users_email (table users) (key (email :asc))
  :unique true)
(commit)
(index-wait idx_users_email)
```

### `(index-state name)`
Returns `:pending`, `:building`, `:valid` or `:failed` for the
index named `name`, inside a transaction. A pending index waits
for a worker, and a building one has a worker filling it; neither
may be scanned yet. A failed index is not maintained
and cannot be scanned; it can only be dropped.

```janet
(assert-eq (index-state idx_users_email) :valid)
```

### `(fkey-create name (table child) (columns c...) (references parent p...) ...)`
Adds the foreign key `name` to the table `child`
(`../FOREIGNKEY.md`). sm keeps it and checks no row: that is
the host's. Without parent columns it takes the parent's primary
key. Its index is the parent's unique index whose keys are the
referenced columns, in any order, unless `:index` names one.

Options:
- `:index idx` -- the parent's index it uses.
- `:on-delete` and `:on-update` -- `:no-action` (the default),
  `:restrict`, `:cascade`, `:set-null` or `:set-default`; kept,
  not acted on.
- `:match :full` -- MATCH FULL; `:simple` is the default.
- `:not-valid true` -- record the rows there as not checked.

```janet
(fkey-create orders_user (table orders) (columns user_id)
  (references users id) :on-delete :cascade)
```

### `(fkey-drop table name)`, `(fkey-validate table name)`, `(fkey-rename table name new-name)`
Drop the foreign key `name` of `table`; mark its rows checked;
rename it. A taken name is `ERR_DUPKEY`.

### `(fkeys table)`
The table's foreign keys, each a struct of its `:name`,
`:columns`, the table it `:references` and its `:ref-columns`,
`:on-delete`, `:on-update`, `:match` and `:validated`.

```janet
(fkeys orders)
# => [{:name "orders_user" :columns [user_id] :references "users"
#      :ref-columns [id] :on-delete :cascade :on-update :no-action
#      :match :simple :validated true}]
```

### `(referenced-by table)`
The foreign keys that reference `table`, its own among them, each
`[child-table fkey-name]`, sorted. While there is one,
`table-drop` of the table and `index-drop` of the index it uses
fail with `ERR_INUSE`.

---

## 6. Data Manipulation (DML) & Scans

CRUD operations and ordered scans on tables and indexes.
All DML operations require an active transaction. Each one
first takes its table's oid lock -- intent-shared to read,
intent-exclusive to write -- held to the end of the transaction,
so DDL on the table waits for it and it for DDL. A table that
does not exist, or that another session has created but not
committed, fails with `<op>: table '<name>' not found`.

### `(insert table [val1 val2 ...])`
Encodes and inserts a new row into `table` and all attached
secondary indexes. Returns the allocated 64-bit `rowno`. If a
unique index violation occurs, raises `SM_ERR_DUPKEY`, and the
transaction is aborted (§7).

```janet
(def r1 (insert users [1 "Alice" "alice@example.com" true]))
```

### `(read table rowno)`
Reads the row tuple at `rowno` under the transaction's snapshot.
Returns a tuple of values `[val1 val2 ...]` or `nil` if absent.

```janet
(def u (read users r1))
```

### `(read-latest table rowno)`
Reads the latest committed version of `rowno`. Valid only under
Read Committed isolation.

```janet
(def u (read-latest users r1))
```

### `(lock-row table rowno mode)`
Locks the row at `rowno` without writing it, to the end of the
transaction. `mode` is `:shared`, which other shared lockers
join and a writer of the row waits for, or `:exclusive`, which
everyone waits for. It is what a host takes on the parent row
to check a foreign key, and what `SELECT ... FOR SHARE` and
`FOR UPDATE` take. Under `(qa-nowait true)` a lock that would
wait fails with `ERR_BLOCKED`.

```janet
(lock-row owners r1 :shared)
```

### `(truncate table)`
Empties the table and every index on it, in the current
transaction: `TRUNCATE`. It takes the table's lock
exclusive, so it waits for every other reader and writer of
the table; under `(qa-nowait true)` it fails with
`ERR_BLOCKED` instead. Columns and indexes are kept, and new
rows go on from the last row number handed out. An abort, or a
rollback to a savepoint taken before it, brings the rows back.

```janet
(truncate jobs)
```

### `(analyze table [target])`
Analyzes the table in the current transaction
(`sm3/SM3_DESIGN.md` §6): samples up to 300 × `target` rows,
100 if none is given, reading no more than ten times that in
row numbers whatever the table's size, and records each
column's statistics, each partial or expression index's from
rel's evaluator, and the sizes. They commit and roll back with
the transaction. It takes the table's lock as a reader.

```janet
(analyze jobs 10)
```

### `(stats table)`
The table's statistics, `nil` if it was never analyzed: a
struct of `:live-rows`, `:rows-sampled`, `:rowno-limit`,
`:run-length`, `:avg-row-bytes`, `:heap-disk`, `:heap-mem`,
`:columns`, by name, and `:indexes`, by name, each with
`:belongs`, `:entries`, its sizes, and `:exprs`, its
expressions' by place. A column's or an expression's has
`:null-frac`, `:width`, `:distinct`, `:correlation`, `:mcv` and
`:freqs`, the common values and their frequencies, and
`:bounds`, the histogram.

```janet
(((stats jobs) :columns) "state")
```

### `(update table rowno [val1 val2 ...])`
Updates the row at `rowno` and synchronizes secondary indexes.
Returns the previous tuple values `[old1 old2 ...]`.

```janet
(def old (update users r1 [1 "Alice C." "alice@example.com" true]))
```

### `(delete table rowno)`
Deletes the row at `rowno` from the table and secondary indexes.
Returns the previous tuple values `[old1 old2 ...]`.

```janet
(def old (delete users r1))
```

### `(scan table [:from first] [:to last])`
Performs an ordered heap scan over `rowno` range `[first, last]`.
Returns an array of `[rowno [val1 val2 ...]]` pairs.

```janet
(def all-rows (scan users))
(def slice (scan users :from 2 :to 10))
```

### `(idx-scan index [:ge lo] [:gt lo] [:le hi] [:lt hi])`
Scans a secondary index across key prefix bounds.
Bounds are tuples matching key prefix columns.
Returns an array of `[rowno cut include-tuple]` entries: `cut`
is true when sm2 truncated an over-long key, so the caller must
recheck the row; `include-tuple` is nil for an index without
INCLUDE columns. The key itself is not returned, since the index
holds it only in its sort-key encoding.

```janet
(def hits (idx-scan idx_users_email :ge ["alice@"] :le ["alice@~"]))
```

### `(idx-open index [:ge lo] [:gt lo] [:le hi] [:lt hi])`
Opens a cursor over the index, with `idx-scan`'s bounds, and
returns its handle, an integer. The cursor stays open across
calls until `cursor-close` ends it, or the transaction's end
does: `commit`, `abort` and `prepare` end the cursors still
open, as do `session-close` and `env-detach`. It reads at the
view it opened with, so rows written after it are not among
its entries.

### `(cursor-next c)`
The cursor's next entry, as `idx-scan` gives each, or nil at
its end. A cursor that a rollback to a savepoint set before it
opened has closed fails with `ERR_SCANCLOSED`, on every call,
as its old view cannot be recreated. A handle that names no
open cursor panics.

### `(cursor-close c)`
Ends the cursor; its handle names none after.

```janet
(def c (idx-open idx_users_email :ge ["alice@"]))
(def first (cursor-next c))
(cursor-close c)
```

---

## 7. Diagnostics & Error Handling

Inspect engine errors, catch panics, and check retryability.

### Error Propagation Model

When a storage manager operation fails (returns `-1`), `rel`
translates the error into a **Janet panic** via
`janet_panicf`.

1. **Panic Format:**
   Panics are formatted as:
   ```text
   <action> failed: [<ERROR_NAME>] <message>
   ```
   For example, a unique constraint violation on an insert
   or index creation produces:
   ```text
   insert sm_tbl_insert failed: [ERR_DUPKEY] key already exists
   ```

2. **Unhandled Panics:**
   If a script does not catch a panic, execution terminates
   immediately with a non-zero exit code and prints a Janet stack
   trace with the source file, line number, and error message.

3. **Catching Panics with `try`:**
   Janet scripts can intercept panics using standard `try`:
   ```janet
   (try
     (insert users [1 "Alice"])
     ([err]
      (print "Caught error: " err)))
   ```

4. **A Failed Write Aborts the Transaction:**
   When `insert`, `update` or `delete` fails inside the storage
   manager, `rel` aborts the session's transaction before it
   panics, as PostgreSQL does on an error. The write may have
   changed the heap and some indexes before it failed, and the
   abort is what undoes that. A script that catches the panic
   calls `(abort)`, which is harmless, and `(begin)` again. Two
   outcomes are not errors and leave the transaction active:
   `ERR_UPDATED`, answered with `(read-latest ...)`, and
   `ERR_BLOCKED` under `(qa-nowait true)`, answered by repeating
   the step. A row that `rel` itself refuses, before any call
   on the storage manager, aborts nothing.

5. **Diagnostic State:**
   Even after a panic is caught, the underlying storage manager
   error state remains preserved on the calling thread until the
   next operation or until cleared via `(sm:reseterr)`.

### `(sm:errnum)`
Returns the integer error code of the last error.

```janet
(def code (sm:errnum))
```

### `(sm:errname)`
Returns the uppercase name of the last error code, without its
`SM_` prefix (e.g. `"ERR_DUPKEY"`, `"ERR_UPDATED"`,
`"ERR_CONFLICT"`).

```janet
(def name (sm:errname))
```

### `(sm:errmsg)`
Returns the descriptive error message string.

```janet
(def msg (sm:errmsg))
```

### `(sm:retryable)`
Returns `true` if the last error is retryable under transaction
conflict rules (such as `SM_ERR_UPDATED` or `SM_ERR_CONFLICT`).

```janet
(if (sm:retryable) (do ...))
```

### `(sm:reseterr)`
Resets the calling thread's last error code and message to
`SM_OK`.

```janet
(sm:reseterr)
```

---

## 8. Test Assertions

Convenience assertions for script-driven test suites.

### `(assert-eq actual expected [msg])`
Panics if `actual` does not equal `expected`.

```janet
(assert-eq (read users 1) [1 "Alice" "alice@example.com" true])
```

### `(assert-nil val [msg])`
Panics if `val` is not `nil`.

```janet
(assert-nil (read users 999))
```

### `(assert-count n list [msg])`
Panics if the length/count of `list` is not `n`.

```janet
(assert-count 5 (scan users))
```

### `(assert-err code expr)`
Panics unless `expr` fails with the error `code`, a keyword
whose name, upper-cased with `-` as `_`, is found in the error:
`:err-dupkey` matches `ERR_DUPKEY`.

```janet
(assert-err :err-dupkey (insert users [1 "Bob"]))
```

To expect any error, catch it with `try`:

```janet
(assert-eq (try (do (insert users [1 "Bob"]) false) ([err] true)) true)
```
