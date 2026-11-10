# DESIGN — rel, an interactive relational test driver for wisr

Status: built; the design of record for the `rel` test driver.

---

## 1. What is rel?

`rel` is an interactive test driver and script interpreter for
the `wisr` storage engine (`include/sm.h`). It works with
relations and rows, without SQL.

**Why no SQL.** The tests target the storage engine: MVCC, tuple
formats, serializable snapshot isolation (SSI), Cahill's pivot
detection, index maintenance, savepoints and two-phase commit. A
SQL parser, planner and optimizer between the test and the
storage manager would add tens of thousands of lines of code. A
bug in that code would hide whether the storage engine works.

Classical systems test their storage the same way: IBM System
R's test drivers for its storage system (RSS), and modern storage
test harnesses, work on relations and rows directly. `rel` is an
interpreter that acts as a relational database without SQL:

- Tables have typed schemas and secondary indexes.
- Operations manipulate rows directly via first-class tuples.
- Scans return Janet arrays of rows, which Janet's own
  `filter` and `map` compose over.
- Statements map 1:1 to `include/sm.h` C API calls.
- Multi-session scripting enables deterministic concurrency,
  isolation anomaly reproduction, and crash-consistency tests.

---

## 2. Language Choice: Janet

`rel` is Janet. It embeds the Janet interpreter (`libjanet`),
and registers its builtins in Janet's core environment. So a
script is a Janet program, which works on rows directly with
little syntax.

Why Janet, and not Python or Lua?

1. **First-class tuples.** Janet has brackets `[...]` for data
   tuples, and parentheses `(...)` for expressions and commands.
   A row is a tuple: `[1 "Albert Einstein" 44 true]`.
2. **Keywords as enums.** Types and options are Janet keywords,
   which start with `:`. Examples: `:int4`, `:text`,
   `:serializable`, `:asc`, `:desc`, `:unique`.
3. **A small library.** Janet is one small C library to embed.
   The build needs no Python virtual machine and no Lua runtime.
4. **Expressions everywhere.** Every operation is an expression
   that returns a value. So assertions, and transactions of
   several statements, are easy to compose.

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

`rel` is a relational layer on top of `sm.h`. In this layer,
tables and indexes have names. A table or an index manages its
`sm_` stores, sequences, row rels and index rels for the script.

### 3.1 Data Types

`rel` supports every frozen type that
[`sm2/TYPE.md`](../sm2/TYPE.md) defines and `sm_type(code)`
exposes:

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

A table has an ordered list of named columns. Each column has a
type, and optional flags (`:not-null`).

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

An index is created on an existing table. It names its key
columns, their sort order, and whether it is unique.

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

A script works on rows directly, as tuples, with no SQL
`INSERT INTO`, `UPDATE` or `DELETE`. How a row is named depends
on its table:

- In a table without a primary key, a row is named by its `_id`,
  as in the examples below.
- In a table with a primary key, a row is named by its key's
  values, as a tuple. A table gets a primary key from
  `table-create`'s `(primary-key ...)`, or from
  `add-primary-key`.

```lisp
# 1. Insert a row directly: returns generated _id (e.g. 1)
(insert users [1 "Albert Einstein" 44])

# 2. Read row by _id: returns decoded tuple
(read users 1)
# => [1 "Albert Einstein" 44]

# 3. Read latest committed version of row by _id
# Useful under READ COMMITTED to inspect newer commits
(read-latest users 1)
# => [1 "Albert Einstein" 44]

# 4. Update row directly: replaces row at _id, returns old tuple
(update users 1 [1 "Albert Einstein" 45])
# => [1 "Albert Einstein" 44]

# 5. Delete row directly: removes row at _id, returns old tuple
(delete users 1)
# => [1 "Albert Einstein" 45]
```

### 3.4 Direct Relational Scans and Queries

Instead of SQL queries, `rel` has table scans and index scans.
They return Janet arrays. Janet's own `filter` and `map` work on
the arrays, so `rel` adds no stream operators:

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

Every statement in `rel` maps to one or more `sm_` functions in
`include/sm.h`. `rel` has two levels of statements:

1. **High-level relational statements** manage a table's row rel
   and its secondary indexes together.
2. **Low-level `sm_` statements** expose the exact C API calls,
   to unit test each API handle.

### 4.1 Environment and Database Lifecycle

| `rel` Statement | `include/sm.h` Function | Description |
| --- | --- | --- |
| `(env-init path)` | `sm_init(path)` | Initializes home dir |
| `(env-attach path)` | `sm_attach(path, &env)` | Attaches home dir |
| `(env-detach [:force true])` | `sm_detach(env, force)` | Detaches home dir |
| `(db-create name [opts])` | `sm_mkdb(env, name, opt)` | Creates database |
| `(db-drop name)` | `sm_rmdb(env, name)` | Drops database |
| `(db-list)` | `sm_lsdb(env, names)` | Lists databases |
| `(db-open name [opts])` | `sm_opendb(env, name, opt, &s)` | Opens DB session |
| `(db-close)` | `sm_closedb(session)` | Closes session |

The options in `(db-create name {:durability-sync 0 ...})` map
to fields of `sm_options_t`. Fields not given keep their
defaults:

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
| `(prepared-list)` | `sm_list_prepared(s, list)` | Lists prepared xcts |
| `(commit-prepared gid)` | `sm_commit_prepared(s, gid)` | Commits prepared |
| `(abort-prepared gid)` | `sm_abort_prepared(s, gid)` | Aborts prepared |

The isolation levels map to `sm_xctlevel_t`:

- `:rc` or `:read-committed` -> `SM_XCT_READ_COMMITTED`
- `:rr` or `:repeatable-read` -> `SM_XCT_REPEATABLE_READ`
- `:ser` or `:serializable` -> `SM_XCT_SERIALIZABLE`

### 4.3 High-Level Table and Index Operations

Tables and indexes live in sm3's catalog
([`../CATALOG.md`](../CATALOG.md)), in the `public` namespace.
`rel` reaches them through the C API's `sm_cat_*` calls, and
keeps no catalog of its own.

DDL is transactional:

- an abort, or a rollback to a savepoint, undoes it;
- it is private to its session until commit;
- a table outlives a restart.

#### `(table-create <name> (columns ...))`
1. Builds an `sm_column_spec_t` per column: its name, type code,
   typmod and `:not-null`.
2. Calls `sm_cat_create_table(s, SM_NSP_PUBLIC, name, cols,
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
2. Calls `sm_cat_create_index(s, table, name, keys, includes,
   unique, eval, reads, &oid)`, which records the index
   under the table's next relid.
   - On a table made in the same transaction, the index is
     filled, and valid at once.
   - On any other table, the index is recorded as pending. It
     is filled in the background after the transaction commits
     ([`../INDEXBUILD.md`](../INDEXBUILD.md#2-the-protocol) §2).

The host evaluates a partial or an expression index, not the
storage manager
([`../INDEXBUILD.md`](../INDEXBUILD.md#2-the-protocol) §2). Here,
`rel` is the host. When a home is attached, `rel` gives the
storage manager its evaluator with `sm_set_index_evaluator`.

The evaluator is C++, and touches nothing of Janet's. The index
build calls it from worker threads, where the Janet interpreter
may not run. So the evaluator is fixed, not a script's function.
It handles two forms:

- `:where (col value)`: the row is in the index if `col`, as its
  type prints it, is `value`;
- `(lower col)` or `(upper col)` as a key column, of a text
  column.

`index-create` writes what it parsed into the index's blob. The
storage manager keeps the blob, with the columns it reads, and
hands it back. `describe-table` reads the blob to give both forms
back.

#### `(index-wait <idx_name>)`
1. Calls `sm_cat_index_oid` for the index and its table.
2. Calls `sm_cat_index_wait(s, table, index)`, outside a
   transaction. The call blocks until the build has ended. If
   the build failed, the call fails with the build's own error.

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
1. Calls `sm_cat_list_namespaces(s, names)`.
2. Returns the names, sorted, as an array of strings.

#### `(list-tables)`
1. Calls `sm_cat_list_tables(s, SM_NSP_PUBLIC, names)`,
   which takes no lock.
2. Returns the names, sorted, as an array of strings.

#### `(describe-table <name> [:include-indices])`
1. Opens the table under its table-oid lock IS, as a read does.
2. Renders each live column of its `sm_table_def_t` in
   `table-create`'s form, with its typmod as the numbers after
   the type.
3. With `:include-indices`, also renders each index as a struct
   of `index-create`'s clauses.

All of it is rendered as bracket tuples.

### 4.4 High-Level Row DML Operations

Each statement first opens its table by name:

1. `sm_cat_table_oid` finds the table's oid.
2. `sm_tbl_open` opens the table: `SM_LOCKMODE_IS` to read,
   `SM_LOCKMODE_IX` to write. It holds the table-oid lock to the
   end of the transaction, and opens the table's rows and
   indexes under it.
3. `sm_tbl_def` gives the table's definition.

The handle is closed when the session's next statement begins.

**A write is one call.** sm3 writes the row, then every index the
write changes
([`../INDEXBUILD.md`](../INDEXBUILD.md#2-the-protocol) §2). `rel`
names no index and computes no key.

**A failed write aborts the transaction.** The call sets no
savepoint. So a write that fails partway, for example on a
duplicate in a unique index, leaves its earlier steps done.
`rel` then aborts the transaction, as PostgreSQL does on an
error, and the abort undoes them.

Three results are outcomes, not errors, and leave the
transaction active:

- `SM_ERR_UPDATED`;
- `SM_ERR_BLOCKED`;
- `SM_ERR_SELFMODIFIED`: a row that the statement already
  changed.

**Caution:** a `SM_ERR_BLOCKED` that came after the row's write,
on a later index, cannot be repeated safely. A script that meets
one must abort.

**Tables with a primary key.** The calls below are shown by
`_id`. In a table with a primary key:

- `read`, `read-latest`, `lock-row`, `update` and `delete` take
  the key's values, and call the `sm_row_key_t` overload of the
  same function;
- `scan`'s bounds are key values. It calls
  `sm_tbl_idx_scan_rows` on the primary key, and gives
  `[key row]` pairs;
- `(read-keys <table> [key ...])` reads many rows in one
  `sm_tbl_read_keys` ([`../ROWSTORE.md`](../ROWSTORE.md)).

#### `(insert <table> <row-tuple>)`
1. Validates that `<row-tuple>` matches column count and types.
2. Converts tuple values into `sm_datum_t values[]` and
   `bool isnull[]`, as PostgreSQL's `Datum` and nulls:
   - By-value types (bool, integers, floats) held in the
     Datum itself.
   - Strings and bytea are built as PostgreSQL varlenas, with a
     1-byte or 4-byte header
     ([`sm2/VARLENA.md`](../sm2/VARLENA.md)). The Datum points at
     them.
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
4. Decodes each value into a Janet value:
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
2. Calls `sm_tbl_update(tbl, id, values, isnull, n, &ref,
   &old_tuple, &old_len)`.
3. If it fails, aborts the transaction. There are two
   exceptions:
   - `SM_ERR_UPDATED` keeps the row lock. The script must call
     `read-latest` before it repeats the write.
   - `SM_ERR_BLOCKED` leaves the transaction active.
4. Calls `sm_tbl_deform` on `old_tuple` to recover the old
   values.
5. Returns decoded old tuple vector.

#### `(delete <table> <id>)`
1. Calls `sm_tbl_delete(tbl, id, &ref, &old_tuple, &old_len)`.
   On failure, it does as `update` does.
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

To check error conditions directly, `rel` exposes the calling
thread's error state as plain `sm:` primitives:

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

MVCC, SSI and Cahill's pivot detection exist to prevent
concurrency anomalies. To test them, a script interleaves the
operations of several concurrent sessions.

**Blocking without threads.** `rel` runs one session at a time,
on one thread. So a step that waits for another session's lock
would hang the script. Instead:

1. `(qa-nowait true)` makes such a step fail with `ERR_BLOCKED`,
   and leaves the transaction active
   ([`../LOCKMGR.md`](../LOCKMGR.md#32-the-lock-table) §3.2).
2. The script switches to the other session, which lets go of
   the lock.
3. The script repeats the step.

That is how a script says "this step blocks", with no thread and
no sleep. A session that polls this way is never a deadlock
victim, so deadlocks are tested in C++ instead.
`test/test_blocking.rel` and `test/test_recheck.rel` are written
this way. The second runs the worked examples of
[`../LSMINDEX.md`](../LSMINDEX.md#77-read-committed) §7.7, step
for step.

`rel` switches between sessions by name:

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

`rel` has test primitives. When an invariant does not hold, they
fail the test run with detailed diagnostics:

- `(assert-eq <actual> <expected>)`: the two values are equal.
- `(assert-nil <expr>)`: the expression evaluates to `nil`.
- `(assert-count <n> <list>)`: the list has `n` rows.
- `(assert-err <code> <expr>)`: `expr` fails with the error
  `code`, such as `:err-dupkey`.

Any failure can also be caught with Janet's `try`, and its
code read with `(sm:errname)`, as in the example above.

---

## 6. Architecture of the `rel` Interpreter

`rel` is written in standard C++20. It links `build/libsm.a`,
and `libjanet`, its one dependency outside the stack.

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

Janet reads, parses and evaluates the script. `rel` adds its
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
   - Without arguments, runs an interactive REPL.
   - With a path, runs a script file: `rel <script.rel>`. With
     `rel -e`, runs one expression.
   - Exits with 0 when the tests pass, and with a non-zero code
     when an assertion fails.

---

## 7. Example Script: Complete Test Walkthrough

This is a complete test script in `rel`. Its home is under
`bench/build`, not `/tmp`
([`../../CLAUDE.md`](../../CLAUDE.md)):

```lisp
# test_isolation.rel -- verify repeatable-read snapshot isolation
(env-init "bench/build/rel_test")
(env-attach "bench/build/rel_test")
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
