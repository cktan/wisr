# DESIGN — rel, an interactive relational test driver for wisr

Andy Pavlo, Databaseology Division, CMU
Status: built; the design of record for the `rel` test driver.

---

## 1. What is rel?

`rel` is a dedicated, interactive relational test driver and
script interpreter for the `wisr` storage engine
(`include/sm.h`).

Why does a storage engine need a specialized driver without SQL?
Because SQL query engines are bloated beasts! When testing an
MVCC engine, tuple formats, serializable snapshot isolation
(SSI), Cahill pivot detection, index maintenance, savepoints,
and two-phase commit, you do not want a 50,000-line SQL parser,
planner, and cost-based optimizer standing between your test
and the storage manager. Every bug in the parser or planner
obfuscates whether the underlying database engine actually works!

In classical database systems (like IBM System R's RSS test
drivers or modern storage test harnesses), engineers manipulate
relations and rows directly. `rel` provides an interpreter that
simulates a full relational database without SQL:

- Tables have typed schemas and secondary indexes.
- Operations manipulate rows directly via first-class tuples.
- Scans return Janet arrays of rows, which Janet's own
  `filter` and `map` compose over.
- Statements map 1:1 to `include/sm.h` C API calls.
- Multi-session scripting enables deterministic concurrency,
  isolation anomaly reproduction, and crash-consistency tests.

---

## 2. Language Choice: Janet

To manipulate rows directly and execute commands with minimal
syntactic baggage, `rel` is Janet: it embeds the Janet
interpreter (`libjanet`) and registers its builtins in Janet's
core environment, so a script is a Janet program.

Why Janet over Python or Lua?

1. **First-Class Tuples:** Janet natively distinguishes array
   brackets `[...]` for data tuples and parentheses `(...)` for
   expressions and commands. A relational row is literally a
   tuple: `[1 "Andy Pavlo" 44 true]`.
2. **Keywords as Enums:** Types and options are native keywords
   prefixed with `:` (e.g., `:int4`, `:text`, `:serializable`,
   `:asc`, `:desc`, `:unique`).
3. **Simplicity First:** Janet is one small embeddable C
   library. We don't need to drag in a Python virtual machine
   or Lua runtime into `wisr`'s lean build.
4. **Scripting and Assertions:** Every operation is an expression
   that returns a value, making assertions and multi-statement
   transactions trivial to compose.

### 2.1 Lexical Elements and Literals

| Literal | Syntax | Example |
| --- | --- | --- |
| Symbol | `[a-zA-Z_][a-zA-Z0-9_-]*` | `users`, `table-create` |
| Keyword | `:[a-zA-Z0-9_-]+` | `:int4`, `:serializable` |
| Integer | `[+-]?[0-9]+` | `42`, `-10`, `1000000` |
| Float | `[+-]?[0-9]+\.[0-9]+` | `3.14159`, `-0.5` |
| String | `"..."` (standard C escapes) | `"Andy"`, `"hello\n"` |
| Boolean | `true`, `false` | `true`, `false` |
| Null | `nil` | `nil` |
| Row / Tuple | `[elem1 elem2 ...]` | `[101 "Andy" nil true]` |
| Expression | `(op arg1 arg2 ...)` | `(insert users [1 "Andy"])` |
| Comment | `# comment text` | `# insert a row` |

---

## 3. High-Level Relational Model (Without SQL)

`rel` provides a relational simulation layer on top of `sm.h`.
In this layer, tables and indexes are named relational entities
that manage the underlying `sm_` stores, sequences, row rels,
and index rels automatically.

### 3.1 Data Types

`rel` supports all frozen types defined in `sm2/TYPE.md` and
exposed via `sm_type(code)`:

| Keyword | Code | sm2 Type | Description |
| --- | --- | --- | --- |
| `:bool` | 1 | `bool` | 1-byte boolean (`true`/`false`) |
| `:int2` | 2 | `int2` | 16-bit signed integer |
| `:int4` | 3 | `int4` | 32-bit signed integer |
| `:int8` | 4 | `int8` | 64-bit signed integer |
| `:float4` | 5 | `float4` | 32-bit IEEE 754 float |
| `:float8` | 6 | `float8` | 64-bit IEEE 754 float |
| `:text` | 8 | `text` | Variable-length UTF-8 string |
| `:varchar` | 9 | `varchar` | Variable-length varchar |
| `:bytea` | 10 | `bytea` | Binary byte array |
| `:date` | 11 | `date` | Date (days since 2000-01-01) |
| `:time` | 12 | `time` | Microseconds since midnight |
| `:timestamp` | 13 | `timestamp` | Microseconds since 2000-01-01 |
| `:timestamptz` | 14 | `timestamptz` | Timestamp in UTC |
| `:interval` | 15 | `interval` | Months, days, microseconds |
| `:uuid` | 16 | `uuid` | 16-byte UUID |
| `:decimal64` | 17 | `decimal64` | Fixed-point decimal, p <= 17 |
| `:decimal128` | 18 | `decimal128` | Fixed-point decimal, p <= 36 |

### 3.2 Defining Tables and Indexes

Tables define an ordered list of named attributes. Each attribute
specifies a type and optional flags (`:not-null`).

```lisp
# Create table 'users'
(table-create users
  (columns
    (id   :int4 :not-null)
    (name :text)
    (age  :int4)))

# Drop table 'users'
(table-drop users)
```

Indexes are created on existing tables, specifying indexed key
columns, sort order, and uniqueness.

```lisp
# Create secondary unique index on 'users' by name ascending
(index-create idx_users_name
  (table users)
  (key (name :asc))
  :unique true)

# Multi-column composite index with INCLUDE column
(index-create idx_users_age_name
  (table users)
  (key (age :desc :nulls-last) (name :asc))
  :unique false
  :include (id))

# Drop index
(index-drop idx_users_name)
```

### 3.3 Direct Row Manipulation

Rows are manipulated directly as tuples without SQL
`INSERT INTO`, `UPDATE`, or `DELETE` syntax. A row of a table
without a primary key is named by its `_id`, as below; one of a
table with one (`table-create`'s `(primary-key ...)`, or
`add-primary-key`) by its key's values, a tuple:

```lisp
# 1. Insert a row directly: returns generated _id (e.g. 1)
(insert users [1 "Andy Pavlo" 44])

# 2. Read row by _id: returns decoded tuple
(read users 1)
# => [1 "Andy Pavlo" 44]

# 3. Read latest committed version of row by _id
# Useful under READ COMMITTED to inspect newer commits
(read-latest users 1)
# => [1 "Andy Pavlo" 44]

# 4. Update row directly: replaces row at _id, returns old tuple
(update users 1 [1 "Andy Pavlo" 45])
# => [1 "Andy Pavlo" 44]

# 5. Delete row directly: removes row at _id, returns old tuple
(delete users 1)
# => [1 "Andy Pavlo" 45]
```

### 3.4 Direct Relational Scans and Queries

Instead of SQL queries, `rel` uses direct table and index scans,
which return Janet arrays; Janet's own `filter` and `map`
compose over them, and `rel` adds no stream operators:

```lisp
# Table scan: scan all rows from _id 1 to 100
(scan users :from 1 :to 100)
# => ([1 [1 "Andy" 45]] [2 [2 "Lin" 30]])

# Index scan with bounds: scan idx_users_name between "A" and "M"
(idx-scan idx_users_name :ge ["A"] :lt ["M"])
# => ([1 false nil] [2 false nil]) ; (_id, cut, includes)

# Filter and project with Janet's own functions
(map (fn [[id r]] [(r 1) (r 2)])
     (filter (fn [[id r]] (> (r 2) 21)) (scan users)))
```

---

## 4. Statement Mapping to `include/sm.h`

Every statement in `rel` maps directly to one or more `sm_`
functions in `include/sm.h`. `rel` supports two levels of
statements:

1. **High-Level Relational Statements:** Coordinate row rels and
   attached secondary indexes automatically.
2. **Low-Level Direct `sm_` Statements:** Expose the exact C
   API calls for granular unit testing of individual API handles.

### 4.1 Environment and Database Lifecycle

| `rel` Statement | `include/sm.h` Function | Description |
| --- | --- | --- |
| `(env-init path)` | `sm_init(path)` | Initializes home dir |
| `(env-attach path)` | `sm_attach(path, &env)` | Attaches home dir |
| `(env-detach [:force true])` | `sm_detach(env, force)` | Detaches home dir |
| `(db-create name [opts])` | `sm_mkdb(env, name, opt)` | Creates database |
| `(db-drop name)` | `sm_rmdb(env, name)` | Drops database |
| `(db-list)` | `sm_lsdb(env, &names, &n)` | Lists databases |
| `(db-open name [opts])` | `sm_opendb(env, name, opt, &s)` | Opens DB session |
| `(db-close)` | `sm_closedb(session)` | Closes session |

Database options in `(db-create name {:durability-sync 0 ...})`
map directly to `sm_options_t`; the others keep their defaults:
- `:durability-sync` -> `opt.durability_sync`
- `:deadlock-ms` -> `opt.deadlock_ms`

### 4.2 Transaction Management and 2PC

| `rel` Statement | `include/sm.h` Function | Description |
| --- | --- | --- |
| `(begin [:rc \| :rr \| :ser])` | `sm_begin(s, level, 0)` | Begins transaction |
| `(commit)` | `sm_commit(s)` | Commits transaction |
| `(abort)` | `sm_abort(s)` | Aborts transaction |
| `(snapshot)` | `sm_stmt_begin(s)` | Statement snapshot |
| `(qa-nowait on)` | `sm_set_qa_nowait(s, on)` | A step that would wait fails with `ERR_BLOCKED` |
| `(savepoint)` | `sm_savepoint(s)` | Sets savepoint depth |
| `(rollback depth)` | `sm_rollback(s, depth)` | Undoes to savepoint |
| `(forget depth)` | `sm_forget(s, depth)` | Drops savepoint |
| `(prepare gid)` | `sm_prepare(s, gid)` | Prepares 2PC xct |
| `(prepared-list)` | `sm_list_prepared(s, ...)` | Lists prepared xcts |
| `(commit-prepared gid)` | `sm_commit_prepared(s, gid)` | Commits prepared |
| `(abort-prepared gid)` | `sm_abort_prepared(s, gid)` | Aborts prepared |

Isolation levels map directly to `sm_xctlevel_t`:
- `:rc` or `:read-committed` -> `SM_XCT_READ_COMMITTED`
- `:rr` or `:repeatable-read` -> `SM_XCT_REPEATABLE_READ`
- `:ser` or `:serializable` -> `SM_XCT_SERIALIZABLE`

### 4.3 High-Level Table and Index Operations

Tables and indexes live in sm3's catalog
(`sm3/CATALOG_INMEM.md`), through the C API's `sm_cat_*` calls,
in the `public` namespace. DDL is transactional -- undone by
abort and by rollback to a savepoint, private to its session
until commit -- and a table outlives a restart. `rel` keeps no
catalog of its own.

#### `(table-create <name> (columns ...))`
1. Builds an `sm_column_spec_t` per column: its name, type code,
   typmod and `:not-null`.
2. Calls `sm_cat_create_table(s, SM_NSP_PUBLIC, name, cols, n,
   &oid)`, which makes the table's store, `_id` sequence and
   catalog entry.

#### `(table-drop <name>)`
1. Calls `sm_cat_table_oid` for the table's oid.
2. Calls `sm_cat_drop_table(s, oid)`, which drops its store, its
   rows and index entries, and its `_id` sequence.

#### `(index-create <idx_name> (table <table>) (key ...) [:unique bool] [:include ...])`
1. Opens the table under its table-oid lock X, as the DDL takes
   it, to resolve the key and INCLUDE column names to their
   places.
2. Calls `sm_cat_create_index(s, table, name, keys, nkey,
   includes, ninclude, unique, &oid)`, which records the index
   under the table's next relid. On a table made in the same
   transaction the index is filled and valid at once. On any
   other it is recorded as pending, and filled in the
   background after the transaction commits
   (`sm3/SM3_DESIGN.md` §3).

A partial or an expression index is evaluated by the host, not
by the storage manager (`sm3/SM3_DESIGN.md` §3), and `rel` is
the host. It gives the storage manager its evaluator with
`sm_set_index_evaluator` when a home is attached. The
evaluator is C++ and touches nothing of Janet's, because the
index build calls it from worker threads, where the Janet
interpreter may not run. So it is a fixed one and not a script's
function, over two forms:
- `:where (col value)`: the row is in the index if `col`, as
  its type prints it, is `value`;
- `(lower col)` or `(upper col)` as a key column, of a text
  column.

`index-create` writes what it parsed into the index's blob,
which the storage manager keeps and hands back, with the
columns it reads. `describe-table` reads the blob to give both
forms back.

#### `(index-wait <idx_name>)`
1. Calls `sm_cat_index_oid` for the index and its table.
2. Calls `sm_cat_index_wait(s, table, index)`, outside a
   transaction, which blocks until the build has ended and
   fails with the build's own error if it failed.

#### `(index-state <idx_name>)`
1. Opens the index's table under its table-oid lock IS.
2. Returns `:pending`, `:building`, `:valid` or `:failed`
   from the `state` of its `sm_index_def_t`.

#### `(index-drop <idx_name>)`
1. Calls `sm_cat_index_oid` for the index and its table.
2. Calls `sm_cat_drop_index(s, table, index)`, which erases the
   index and its entries.

#### `(namespace-create <name>)`
1. Calls `sm_cat_create_namespace(s, name, &oid)`.

#### `(namespace-drop <name>)`
1. Calls `sm_cat_namespace_oid` for the namespace's oid.
2. Calls `sm_cat_drop_namespace(s, oid)`, which refuses one
   that is not empty and the two built in.

#### `(list-namespaces)`
1. Calls `sm_cat_list_namespaces(s, &names)`.
2. Returns the names, sorted, as an array of strings.

#### `(list-tables)`
1. Calls `sm_cat_list_tables(s, SM_NSP_PUBLIC, &names, &n)`,
   which takes no lock.
2. Returns the names, sorted, as an array of strings.

#### `(describe-table <name> [:include-indices])`
1. Opens the table under its table-oid lock IS, as a read does.
2. Renders each live column of its `sm_table_def_t` in
   `table-create`'s form, its typmod as the numbers after the
   type, and with the flag each index as a struct of
   `index-create`'s clauses, all as bracket tuples.

### 4.4 High-Level Row DML Operations

Each statement first opens its table by name: `sm_cat_table_oid`,
then `sm_tbl_open` -- `SM_LOCKMODE_IS` to read,
`SM_LOCKMODE_IX` to write -- which holds the table's
table-oid lock to the end of the transaction and opens its rows
and its indexes under it. `sm_tbl_def` gives its definition.
The handle is closed when the session's next statement begins.

A write is one call. sm3 writes the row and then every
index the write changes (`sm3/SM3_DESIGN.md` §3); `rel` names
no index and computes no key. The call sets no savepoint, so a
write that fails partway, on a duplicate in a unique index for
one, has left its earlier steps done. `rel` then aborts the
transaction, as PostgreSQL does on an error, and that undoes
them. `SM_ERR_UPDATED`, `SM_ERR_BLOCKED` and
`SM_ERR_SELFMODIFIED`, a row the statement already changed,
are outcomes, not errors, and leave the transaction active.
One limit follows:
a `SM_ERR_BLOCKED` that struck after the row's write, on a
later index, cannot be repeated safely, and a script that
meets one must abort.

The calls below are shown by `_id`. In a table with a primary
key, `read`, `read-latest`, `lock-row`, `update` and `delete`
take the key's values and call the same function's
`sm_row_key_t` overload; `scan`'s bounds are key values, and it
calls `sm_tbl_idx_scan_rows` on the primary key, giving
`[key row]` pairs; `(read-keys <table> [key ...])` reads many
rows in one `sm_tbl_read_keys` (`BATCHREAD.md`).

#### `(insert <table> <row-tuple>)`
1. Validates that `<row-tuple>` matches column count and types.
2. Converts tuple values into `sm_datum_t values[]` and
   `bool isnull[]`, as PostgreSQL's `Datum` and nulls:
   - By-value types (bool, integers, floats) held in the
     Datum itself.
   - Strings and bytea built as PostgreSQL varlenas with
     1-byte or 4-byte header (`sm2/VARLENA.md`), the Datum
     pointing at them.
   - Any other type parsed from its canonical text by
     `sm_type_in`.
3. Calls `sm_tbl_insert(tbl, values, isnull, n, &id)`.
4. If it fails (e.g., `SM_ERR_DUPKEY` from a unique index),
   aborts the transaction; the error is preserved and
   returned.
5. Returns the new row's `_id`.

#### `(read <table> <id>)`
1. Calls `sm_tbl_read(tbl, id, &tuple, &len)`.
2. If return is 1, returns `nil` ("no row").
3. Calls `sm_tbl_deform(tbl, tuple, len, values, isnull,
   ncols)`.
4. Decodes each value into a `rel-lisp` value:
   - Integers and floats to numbers.
   - Varlenas decoded to strings.
   - Any other type to its canonical text, via `sm_type_out`;
     on insert, that text is parsed by `sm_type_in`.
   - Null datums to `nil`.
5. Returns tuple vector `[v0 v1 ... vn]`.

#### `(read-latest <table> <id>)`
1. Calls `sm_tbl_read_latest(tbl, id, &tuple, &len)`.
2. Deforms and decodes tuple identically to `read`.
3. Returns newest committed tuple vector.

#### `(lock-row <table> <id> <mode>)`
1. Opens the table under `SM_LOCKMODE_IS` for
   `:shared`, `SM_LOCKMODE_IX` for `:exclusive`.
2. Calls `sm_tbl_lock(tbl, id, mode)`, which holds the row
   to the end of the transaction and writes nothing.

#### `(update <table> <id> <new-row-tuple>)`
1. Converts `<new-row-tuple>` into `values[]` and `isnull[]`.
2. Calls `sm_tbl_update(tbl, id, values, isnull, n,
   &old_tuple, &old_len)`.
3. If it fails, aborts the transaction, except on
   `SM_ERR_UPDATED`, which keeps the row lock for the
   `read-latest` that must come before the write is repeated,
   and on `SM_ERR_BLOCKED`.
4. Calls `sm_tbl_deform` on `old_tuple` to recover the old
   values.
5. Returns decoded old tuple vector.

#### `(delete <table> <id>)`
1. Calls `sm_tbl_delete(tbl, id, &old_tuple, &old_len)`,
   and on failure does as `update`.
2. Calls `sm_tbl_deform` on `old_tuple`.
3. Returns decoded deleted tuple vector.

#### `(scan <table> [:from first] [:to last])`
1. Defaults: `first = 1`, `last = UINT64_MAX`.
2. Calls `sm_tbl_scan(tbl, first, last, &rscan)`, the scan of
   a keyless table by `_id`.
3. Loops `sm_rel_scannxt(rscan, &id, &tuple, &len)` until 1.
4. Decodes each column of a tuple via `sm_tbl_decode`.
5. Calls `sm_rel_scanend(rscan)`.
6. Returns list of pairs:
   `([id_1 tuple_1] [id_2 tuple_2] ...)`.

#### `(idx-scan <index> [:ge lo] [:gt lo] [:le hi] [:lt hi])`
1. Constructs lower bound `sm_bound_t lo_b`:
   - Prefix values and null flags from `:ge` (inclusive) or
     `:gt` (exclusive).
2. Constructs upper bound `sm_bound_t hi_b`:
   - Prefix values and null flags from `:le` (inclusive) or
     `:lt` (exclusive).
3. Calls `sm_tbl_idx_scan(tbl, index, &lo_b, &hi_b,
   &widened, &iscan)`, the index named by its oid.
4. Loops `sm_idx_scannxt(iscan, &id, &cut, &val, &len)`
   until 1.
5. If INCLUDE columns exist, decodes value via
   `sm_tbl_idx_deform`.
6. Calls `sm_idx_scanend(iscan)`.
7. Returns list of index entries `([id cut includes] ...)`,
   `includes` being nil for an index without INCLUDE columns.

### 4.5 Low-Level Direct `sm_` Builtins

For verifying error conditions directly, `rel` exposes the
calling thread's error state as unadorned `sm:` primitives:

| Builtin | `sm.h` Equivalent |
| --- | --- |
| `(sm:errnum)` | `sm_errnum()` |
| `(sm:errname)` | `sm_errname()` |
| `(sm:errmsg)` | `sm_errmsg()` |
| `(sm:retryable)` | `sm_retryable()` |
| `(sm:reseterr)` | `sm_reseterr()` |

Sequences are reached through `(seq-create)`, `(seq-drop)` and
`(nextseq)`, over `sm_mkseq`, `sm_rmseq` and `sm_nextseq`.
Stores, row rels and indexes are reached only through tables.

---

## 5. Multi-Session Concurrency and Test Harness

`rel` runs one session at a time, on one thread, so a step
that would wait for another session's lock would hang the
script. `(qa-nowait true)` makes such a step fail with
`ERR_BLOCKED` instead, the transaction left active, and the
script repeats the step once the other session has let go
(`sm1/SM1_DESIGN.md` §5.1). That is how a script says "this
step blocks", with no thread and no sleep. A session polling
this way is never a deadlock victim, so deadlocks are tested
in C++ instead. `test/test_blocking.rel` and
`test/test_recheck.rel` are written this way; the second runs
the worked examples of `sm3/SM3_DESIGN.md` §3.1 step for step.


Concurrency anomalies and isolation verification are the entire
reason `wisr` implements MVCC, SSI, and Cahill pivot detection.
Testing these requires interleaving operations from multiple
concurrent sessions.

`rel` supports first-class session switching:

```lisp
# Open two concurrent sessions on 'testdb'
(session-open s1 "testdb")
(session-open s2 "testdb")

# Session 1 starts serializable transaction and inserts row 1
(session-use s1)
(begin :serializable)
(insert accounts [1 100])

# Session 2 reads in its own transaction (does not see row 1)
(session-use s2)
(begin :serializable)
(assert-nil (read accounts 1))

# Session 1 commits
(session-use s1)
(commit)

# Session 2 updates a row session 1 changed after it began:
# first-committer-wins fails it, retryably
(session-use s2)
(assert-eq (try (do (update accounts 1 [1 50]) false) ([err] true))
           true)
(assert-eq (sm:errname) "ERR_CONFLICT")
(assert-eq (sm:retryable) true)
(abort)
```

### 5.1 Test Assertions

`rel` includes testing primitives that fail the test run with
detailed diagnostics when an invariant is violated:

- `(assert-eq <actual> <expected>)`: Value equality check.
- `(assert-nil <expr>)`: Verifies expression evaluates to `nil`.
- `(assert-count <n> <list>)`: Verifies row count.
- `(assert-err <code> <expr>)`: Verifies `expr` fails with
  the error `code`, as `:err-dupkey`.

Any failure can also be caught with Janet's `try`, and its
code read with `(sm:errname)`, as in the example above.

---

## 6. Architecture of the `rel` Interpreter

`rel` is implemented in standard C++20, linked with
`build/libsm.a` and with `libjanet`, its one dependency beyond
the stack's own.

```
+-------------------------------------------------------------+
|                     rel Test Driver                         |
|                                                             |
|  +-------------------------------------------------------+  |
|  |          Janet interpreter (libjanet), with           |  |
|  |          rel's builtins in its core environment       |  |
|  +-------------------------------------------------------+  |
|                              |                              |
|                              v                              |
|  +-------------------------------------------------------+  |
|  |     Sessions, and each statement's tables, opened     |  |
|  |     from sm3's catalog (sm_cat_*) under their locks   |  |
|  +-------------------------------------------------------+  |
|          |                                   |              |
|          v (row serialization)               v (index keys) |
|  +-------------------------------------------------------+  |
|  |                 include/sm.h C API                    |  |
|  +-------------------------------------------------------+  |
+-------------------------------------------------------------+
```

### 6.1 Component Structure

Reading, parsing and evaluating are Janet's; `rel` adds its
builtins (`rel/src/`):

1. **Builtins (`rel_ops.cc`):**
   - Registers every builtin, and the macros that let a script
     name tables and indexes by bare symbol.
   - Invokes `sm_` functions, and turns an error into a Janet
     panic.
2. **Values (`rel_types.cc`):**
   - Converts input tuples to `sm_datum_t[]` values and
     `bool[]` null flags, and output tuples back to Janet
     values.
3. **Sessions and Tables (`rel_catalog.cc`):**
   - Stores session handles (`sm_session_t*`).
   - Opens a statement's tables from sm3's catalog, and keeps
     them, each with its `sm_tbl_t*`, until the session's
     next statement.
4. **Driver Main (`main.cc`):**
   - Supports interactive REPL mode when launched without args.
   - Executes script files when passed a path:
     `rel <script.rel>`, or one expression with `rel -e`.
   - Exits with 0 on test success, non-zero on assertion failure.

---

## 7. Example Script: Complete Test Walkthrough

Here is what an end-to-end test script in `rel` looks like:

```lisp
# test_isolation.rel -- verify repeatable-read snapshot isolation
(env-init "/tmp/wisr_rel_test")
(env-attach "/tmp/wisr_rel_test")
(db-create "testdb" {:durability-sync 0})

(session-open s1 "testdb")
(session-open s2 "testdb")

# Setup table
(session-use s1)
(begin :rc)
(table-create accounts
  (columns
    (id      :int4 :not-null)
    (balance :int4)))
(index-create accounts_pk
  (table accounts)
  (key (id :asc))
  :unique true)
(insert accounts [1 1000])
(insert accounts [2 2000])
(commit)

# Verify Repeatable Read Isolation
(session-use s1)
(begin :rr)
(assert-eq (read accounts 1) [1 1000])

# Session 2 updates row 1 concurrently
(session-use s2)
(begin :rc)
(update accounts 1 [1 1500])
(commit)

# Session 1 must still see old snapshot (1000)
(session-use s1)
(assert-eq (read accounts 1) [1 1000])
(commit)

# A new transaction sees 1500
(begin :rc)
(assert-eq (read accounts 1) [1 1500])
(commit)

# Cleanup
(session-close s1)
(session-close s2)
(db-drop "testdb")
(env-detach :force true)
```
