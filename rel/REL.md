# Relational Driver Reference (`rel`)

The `rel` driver provides a Janet-based scripting interface
and interactive environment for driving the storage engine
via [`include/sm.h`](file:///home/cktan/p/wisr/include/sm.h).
Source bindings are implemented in
[`rel_ops.cc`](file:///home/cktan/p/wisr/rel/src/rel_ops.cc).

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

### `(env-attach path)`
Attaches to an existing storage home directory at `path` and
initializes the environment handle for the process.

```janet
(env-attach "/tmp/wisr_home")
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

### `(db-open name)`
Opens a database and binds it to the current default session.

```janet
(db-open "mydb")
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

### `(begin [level])`
Begins a new transaction in the active session. The isolation
level defaults to `:rc` (`read-committed`). Supported levels:
- `:rc` / `:read-committed`
- `:rr` / `:repeatable-read`
- `:ser` / `:serializable`

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
Pins or advances a statement-level read snapshot inside a
read-committed transaction. Subsequent reads within the
statement see a consistent snapshot. `(stmt-begin)` is
supported as an alias.

```janet
(snapshot)
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
disassociated from the transaction.

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

Define and manage heap tables and secondary indexes.

### `(table-create name (columns ...))`
Defines and creates a heap table backed by a storage store and a
dedicated row sequence (`<name>_rowno`).

Supported column types:
- Integer: `:int2`, `:int4`, `:int8`
- Floating: `:float4`, `:float8`
- Numeric: `:decimal64`, `:decimal128`
- Boolean: `:bool`
- Text: `:text`, `:varchar`
- UUID: `:uuid`
- Temporal: `:date`, `:time`, `:timetz`, `:timestamp`,
  `:timestamptz`, `:interval`

Column qualifiers:
- `:not-null` -- requires non-null value on insert/update.
- `:dropped` -- marks column as dropped.
- `:missing val` -- default missing value for older tuples.

```janet
(table-create users
  (columns
    (id :int4 :not-null)
    (name :text :not-null)
    (email :text)
    (active :bool)))
```

### `(table-drop name)`
Drops the heap table `name`, its underlying store, and its
associated row sequence.

```janet
(table-drop users)
```

### `(index-create name (table tbl) ...)`
Creates a secondary index on table `tbl`.

Clauses:
- `(table tbl)` -- target table name.
- `(key (col [:asc|:desc] [:nulls-first|:nulls-last])...)` -- key
  columns with ordering and null placement.
- `:unique bool` -- enforce unique constraint on key columns.
- `:include (col...)` -- non-key payload columns embedded
  in index tuples.

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

---

## 6. Data Manipulation (DML) & Scans

CRUD operations and ordered scans on tables and indexes.
All DML operations require an active transaction.

### `(insert table [val1 val2 ...])`
Encodes and inserts a new row into `table` and all attached
secondary indexes. Returns the allocated 64-bit `rowno`. If a
unique index violation occurs, raises `SM_ERR_DUPKEY`.

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
   insert sm_idx_insert failed: [SM_ERR_DUPKEY] duplicate key
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

4. **Testing Expected Errors with `assert-err`:**
   Test scripts can verify that an expression fails with a
   specific error code name:
   ```janet
   (assert-err :err-dupkey (insert users [1 "Duplicate"]))
   ```

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
Returns the uppercase string name of the last error code (e.g.
`"SM_ERR_DUPKEY"`, `"SM_ERR_UPDATED"`, `"SM_ERR_CONFLICT"`).

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

### `(assert-err expected-keyword-or-string expr)`
Evaluates `expr` and expects it to fail. Panics if `expr`
succeeds or if the thrown error does not match `expected`.

```janet
(assert-err :err-dupkey (insert users [1 "Bob" "alice@example.com" true]))
```
