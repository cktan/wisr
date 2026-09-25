# DESIGN — rel, an interactive relational test driver for wisr

Andy Pavlo, Databaseology Division, CMU
Status: Design proposal for the `rel` test driver.

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
- Relational operators (scan, filter, project, join) operate
  directly over row streams.
- Statements map 1:1 to `include/sm.h` C API calls.
- Multi-session scripting enables deterministic concurrency,
  isolation anomaly reproduction, and crash-consistency tests.

---

## 2. Language Choice: Janet-Inspired Lisp (`rel-lisp`)

To manipulate rows directly and execute commands with minimal
syntactic baggage, `rel` adopts a Janet-inspired Lisp syntax.

Why a Janet-inspired Lisp over Python or Lua?

1. **First-Class Tuples:** Janet natively distinguishes array
   brackets `[...]` for data tuples and parentheses `(...)` for
   expressions and commands. A relational row is literally a
   tuple: `[1 "Andy Pavlo" 44 true]`.
2. **Keywords as Enums:** Types and options are native keywords
   prefixed with `:` (e.g., `:int4`, `:text`, `:serializable`,
   `:asc`, `:desc`, `:unique`).
3. **Simplicity First:** Parsing S-expressions requires only a
   tiny, zero-dependency reader (~300 lines of standard C/C++).
   We don't need to drag in a Python virtual machine or Lua
   runtime into `wisr`'s lean build.
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
| Comment | `; comment text` | `; insert a row` |

---

## 3. High-Level Relational Model (Without SQL)

`rel` provides a relational simulation layer on top of `sm.h`.
In this layer, tables and indexes are named relational entities
that manage the underlying `sm_` stores, sequences, heap rels,
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
| `:decimal` | 7 | `decimal` | Fixed-point decimal |
| `:text` | 8 | `text` | Variable-length UTF-8 string |
| `:varchar` | 9 | `varchar` | Variable-length varchar |
| `:bytea` | 10 | `bytea` | Binary byte array |
| `:date` | 11 | `date` | Date (days since 2000-01-01) |
| `:time` | 12 | `time` | Microseconds since midnight |
| `:timestamp` | 13 | `timestamp` | Microseconds since 2000-01-01 |
| `:timestamptz` | 14 | `timestamptz` | Timestamp in UTC |
| `:interval` | 15 | `interval` | Months, days, microseconds |
| `:uuid` | 16 | `uuid` | 16-byte UUID |

### 3.2 Defining Tables and Indexes

Tables define an ordered list of named attributes. Each attribute
specifies a type and optional flags (`:not-null`).

```lisp
;; Create table 'users'
(table-create users
  (columns
    (id   :int4 :not-null)
    (name :text)
    (age  :int4)))

;; Drop table 'users'
(table-drop users)
```

Indexes are created on existing tables, specifying indexed key
columns, sort order, and uniqueness.

```lisp
;; Create secondary unique index on 'users' by name ascending
(index-create idx_users_name
  (table users)
  (key (name :asc))
  :unique true)

;; Multi-column composite index with INCLUDE column
(index-create idx_users_age_name
  (table users)
  (key (age :desc :nulls-last) (name :asc))
  :unique false
  :include (id))

;; Drop index
(index-drop idx_users_name)
```

### 3.3 Direct Row Manipulation

Rows are manipulated directly as tuples without SQL
`INSERT INTO`, `UPDATE`, or `DELETE` syntax:

```lisp
;; 1. Insert a row directly: returns generated rowno (e.g. 1)
(insert users [1 "Andy Pavlo" 44])

;; 2. Read row by rowno: returns decoded tuple
(read users 1)
;; => [1 "Andy Pavlo" 44]

;; 3. Read latest committed version of row by rowno
;; Useful under READ COMMITTED to inspect newer commits
(read-latest users 1)
;; => [1 "Andy Pavlo" 44]

;; 4. Update row directly: replaces row at rowno, returns old tuple
(update users 1 [1 "Andy Pavlo" 45])
;; => [1 "Andy Pavlo" 44]

;; 5. Delete row directly: removes row at rowno, returns old tuple
(delete users 1)
;; => [1 "Andy Pavlo" 45]
```

### 3.4 Direct Relational Scans and Queries

Instead of SQL queries, `rel` uses direct table and index scans
with composable stream operators:

```lisp
;; Heap scan: scan all rows from rowno 1 to 100
(scan users :from 1 :to 100)
;; => ([1 [1 "Andy" 45]] [2 [2 "Lin" 30]])

;; Index scan with bounds: scan idx_users_name between "A" and "M"
(idx-scan idx_users_name :ge ["A"] :lt ["M"])
;; => ([1 false] [2 false]) ; yields (rowno, cut) entries

;; Functional filter and projection over row streams
(project
  (filter (scan users)
    (fn [r] (> (col r :age) 21)))
  [:name :age])
```

---

## 4. Statement Mapping to `include/sm.h`

Every statement in `rel` maps directly to one or more `sm_`
functions in `include/sm.h`. `rel` supports two levels of
statements:

1. **High-Level Relational Statements:** Coordinate heap rels and
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
map directly to `sm_options_t`:
- `:durability-sync` -> `opt.durability_sync`
- `:deadlock-ms` -> `opt.deadlock_ms`
- `:ts-lease` -> `opt.ts_lease`
- `:commit-cache-entries` -> `opt.commit_cache_entries`
- `:rocksdb-options` -> `opt.rocksdb_options`

### 4.2 Transaction Management and 2PC

| `rel` Statement | `include/sm.h` Function | Description |
| --- | --- | --- |
| `(begin [:rc \| :rr \| :ser])` | `sm_begin(s, level)` | Begins transaction |
| `(commit)` | `sm_commit(s)` | Commits transaction |
| `(abort)` | `sm_abort(s)` | Aborts transaction |
| `(stmt-begin)` | `sm_stmt_begin(s)` | Statement snapshot |
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

Under `wisr`'s pre-sm3 scaffolding, `sm_` manages storage through
store IDs and sequence names. `rel` maintains an in-memory
relational catalog mapping table names to their storage metadata.

#### `(table-create <name> (columns ...))`
1. Calls `sm_mkstore(s, &storeid)` to allocate a backing store.
2. Calls `sm_mkseq(s, seq_name, 1, 1)` for row number generation.
3. Constructs an array of `sm_attribute_t` from column schemas
   using `sm_type(code)`.
4. Calls `sm_rel_open(s, storeid, 0, schema, n, seq, &rel)` to
   open the heap relation.
5. Records the table in `rel`'s active session catalog.

#### `(table-drop <name>)`
1. Calls `sm_rel_close(rel)` on open table handle.
2. For each attached index, calls `sm_idx_close(idx)`.
3. Calls `sm_rmseq(s, seq_name)` to remove sequence.
4. Calls `sm_rmstore(s, storeid)` to drop the store and all rows.

#### `(index-create <idx_name> (table <table>) (key ...) [:unique bool] [:include ...])`
1. Looks up the table's `storeid`. Allocates a distinct prefix
   (e.g., prefix 1, 2, ...).
2. Converts key definitions into `sm_key_column_t` array:
   - Sets type via `sm_type(code)`.
   - Opens collation via `sm_collation_open(locale, ...)`.
   - Sets `desc` and `nulls_first` flags.
3. Converts INCLUDE definitions into `sm_attribute_t` array.
4. Calls `sm_idx_open(s, storeid, prefix, key, nkey,
   unique, inc, ninc, &idx)`.
5. If table already contains rows, scans heap via `sm_rel_scan`
   and populates index entries via `sm_idx_insert`.
6. Registers index under table in catalog.

### 4.4 High-Level Row DML Operations

#### `(insert <table> <row-tuple>)`
1. Validates that `<row-tuple>` matches column count and types.
2. Encodes tuple values into native format in `sm_datum_t[]`:
   - Fixed-width integers and floats written in little-endian.
   - Strings and bytea encoded as PostgreSQL varlenas with
     1-byte or 4-byte header (`sm2/VARLENA.md`).
   - Nulls marked with `null = true`.
3. Calls `sm_rel_insert(rel, datums, n, &rowno)`.
4. For each attached index on `<table>`:
   - Extracts key column datums and INCLUDE column datums.
   - Calls `sm_idx_insert(idx, kdatums, nk, rowno, idatums, ni)`.
   - If `sm_idx_insert` fails (e.g., `SM_ERR_DUPKEY`), the error
     is preserved and returned.
5. Returns `rowno`.

#### `(read <table> <rowno>)`
1. Calls `sm_rel_read(rel, rowno, &tuple, &len)`.
2. If return is 1, returns `nil` ("no row").
3. Calls `sm_rel_deform(rel, tuple, len, out_datums, ncols)`.
4. Decodes each `sm_datum_t` into a `rel-lisp` value:
   - Integers and floats to numbers.
   - Varlenas decoded to strings.
   - Null datums to `nil`.
5. Returns tuple vector `[v0 v1 ... vn]`.

#### `(read-latest <table> <rowno>)`
1. Calls `sm_rel_read_latest(rel, rowno, &tuple, &len)`.
2. Deforms and decodes tuple identically to `read`.
3. Returns newest committed tuple vector.

#### `(update <table> <rowno> <new-row-tuple>)`
1. Encodes `<new-row-tuple>` into `sm_datum_t[]`.
2. Calls `sm_rel_update(rel, rowno, datums, n, &old_tuple,
   &old_len)`.
3. Calls `sm_rel_deform` on `old_tuple` to recover old datums.
4. For each attached index on `<table>`:
   - Extracts old key datums from old row.
   - Calls `sm_idx_delete(idx, old_key, nk, rowno)`.
   - Extracts new key datums from new row.
   - Calls `sm_idx_insert(idx, new_key, nk, rowno, new_inc, ni)`.
5. Returns decoded old tuple vector.

#### `(delete <table> <rowno>)`
1. Calls `sm_rel_delete(rel, rowno, &old_tuple, &old_len)`.
2. If row found, calls `sm_rel_deform` on `old_tuple`.
3. For each attached index on `<table>`:
   - Calls `sm_idx_delete(idx, old_key, nk, rowno)`.
4. Returns decoded deleted tuple vector.

#### `(scan <table> [:from first] [:to last])`
1. Defaults: `first = 1`, `last = UINT64_MAX`.
2. Calls `sm_rel_scan(rel, first, last, &rscan)`.
3. Loops `sm_rel_scannxt(rscan, &rowno, &tuple, &len)` until 1.
4. Decodes each tuple via `sm_rel_deform`.
5. Calls `sm_rel_scanend(rscan)`.
6. Returns list of pairs:
   `([rowno_1 tuple_1] [rowno_2 tuple_2] ...)`.

#### `(idx-scan <index> [:ge lo] [:gt lo] [:le hi] [:lt hi])`
1. Constructs lower bound `sm_bound_t lo_b`:
   - Prefix datums encoded from `:ge` (inclusive) or
     `:gt` (exclusive).
2. Constructs upper bound `sm_bound_t hi_b`:
   - Prefix datums encoded from `:le` (inclusive) or
     `:lt` (exclusive).
3. Calls `sm_idx_scan(idx, &lo_b, &hi_b, &widened, &iscan)`.
4. Loops `sm_idx_scannxt(iscan, &rowno, &cut, &val, &len)`
   until 1.
5. If INCLUDE columns exist, decodes value via `sm_idx_deform`.
6. Calls `sm_idx_scanend(iscan)`.
7. Returns list of index entries `([rowno cut val] ...)`.

### 4.5 Low-Level Direct `sm_` Builtins

For testing exact `sm.h` C API boundaries and verifying error
conditions directly, `rel` exposes unadorned `sm:` primitives:

| Builtin | `sm.h` Equivalent |
| --- | --- |
| `(sm:mkstore)` | `sm_mkstore(s, &storeid)` |
| `(sm:rmstore storeid)` | `sm_rmstore(s, storeid)` |
| `(sm:lsstore)` | `sm_lsstore(s, &ids, &n)` |
| `(sm:mkseq name start inc)` | `sm_mkseq(s, name, start, inc)` |
| `(sm:rmseq name)` | `sm_rmseq(s, name)` |
| `(sm:rel-open ...)` | `sm_rel_open(...)` |
| `(sm:rel-close rel)` | `sm_rel_close(rel)` |
| `(sm:rel-insert rel datums)` | `sm_rel_insert(...)` |
| `(sm:rel-read rel rowno)` | `sm_rel_read(...)` |
| `(sm:rel-update rel rowno ...)`| `sm_rel_update(...)` |
| `(sm:rel-delete rel rowno)` | `sm_rel_delete(...)` |
| `(sm:idx-open ...)` | `sm_idx_open(...)` |
| `(sm:idx-insert idx ...)` | `sm_idx_insert(...)` |
| `(sm:idx-delete idx ...)` | `sm_idx_delete(...)` |
| `(sm:idx-scan idx ...)` | `sm_idx_scan(...)` |
| `(sm:errnum)` | `sm_errnum()` |
| `(sm:errname)` | `sm_errname()` |
| `(sm:errmsg)` | `sm_errmsg()` |
| `(sm:retryable)` | `sm_retryable()` |
| `(sm:reseterr)` | `sm_reseterr()` |

---

## 5. Multi-Session Concurrency and Test Harness

Concurrency anomalies and isolation verification are the entire
reason `wisr` implements MVCC, SSI, and Cahill pivot detection.
Testing these requires interleaving operations from multiple
concurrent sessions.

`rel` supports first-class session switching:

```lisp
;; Open two concurrent sessions on 'testdb'
(session-open s1 "testdb")
(session-open s2 "testdb")

;; Session 1 starts serializable transaction and inserts row 1
(session-use s1)
(begin :serializable)
(insert accounts [1 100])

;; Session 2 reads in its own transaction (does not see row 1)
(session-use s2)
(begin :serializable)
(assert-nil (read accounts 1))

;; Session 1 commits
(session-use s1)
(commit)

;; Session 2 attempts write that causes serialization conflict
(session-use s2)
(insert accounts [2 200])
(assert-err :err-serialization (commit))
```

### 5.1 Test Assertions

`rel` includes testing primitives that fail the test run with
detailed diagnostics when an invariant is violated:

- `(assert-eq <actual> <expected>)`: Value equality check.
- `(assert-nil <expr>)`: Verifies expression evaluates to `nil`.
- `(assert-err <err-keyword> <expr>)`: Verifies `<expr>` fails
  with the specified error code name (e.g. `:err-dupkey`,
  `:err-serialization`, `:err-conflict`).
- `(assert-count <n> <stream-or-list>)`: Verifies row count.

---

## 6. Architecture of the `rel` Interpreter

`rel` is implemented in standard C++20 and linked directly with
`build/libsm.a`. It has zero external dependencies.

```
+-------------------------------------------------------------+
|                     rel Test Driver                         |
|                                                             |
|  +-------------------+      +----------------------------+  |
|  | Lexer & Parser    | ---> | AST / S-Expression Evaluator| |
|  +-------------------+      +----------------------------+  |
|                                           |                 |
|                                           v                 |
|  +-------------------------------------------------------+  |
|  |              Relational Catalog & Schema              |  |
|  |    (Maps table names -> storeid, seq, rel_t, idx_t)   |  |
|  +-------------------------------------------------------+  |
|          |                                   |              |
|          v (row serialization)               v (index keys) |
|  +-------------------------------------------------------+  |
|  |                 include/sm.h C API                    |  |
|  +-------------------------------------------------------+  |
+-------------------------------------------------------------+
```

### 6.1 Component Structure

1. **Reader / Parser (`rel/reader.cc`):**
   - Tokenizes input stream into symbols, numbers, strings,
     keywords, lists `(...)`, and tuples `[...]`.
   - Emits an S-expression value tree.
2. **Value Model (`rel/value.h`):**
   - Represents atoms (`int64_t`, `double`, `string`, `bool`,
     `nil`, `keyword`) and compound forms (`vector`, `list`).
3. **Environment & Catalog (`rel/catalog.h`):**
   - Stores session handles (`sm_session_t*`).
   - Tracks table definitions (attribute types, store ID,
     sequence name, open `sm_rel_t*`, open `sm_idx_t*` list).
4. **Evaluator (`rel/eval.cc`):**
   - Dispatches builtins and commands.
   - Encodes input tuples to `sm_datum_t[]` buffers.
   - Invokes `sm_` functions.
   - Decodes output tuples to `rel` vectors.
5. **Driver Main (`rel/main.cc`):**
   - Supports interactive REPL mode when launched without args.
   - Executes script files when passed a path:
     `rel <script.rel>`.
   - Exits with 0 on test success, non-zero on assertion failure.

---

## 7. Example Script: Complete Test Walkthrough

Here is what an end-to-end test script in `rel` looks like:

```lisp
; test_isolation.rel -- verify repeatable-read snapshot isolation
(env-init "/tmp/wisr_rel_test")
(env-attach "/tmp/wisr_rel_test")
(db-create "testdb" {:durability-sync 0})

(session-open s1 "testdb")
(session-open s2 "testdb")

; Setup table
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

; Verify Repeatable Read Isolation
(session-use s1)
(begin :rr)
(assert-eq (read accounts 1) [1 1000])

; Session 2 updates row 1 concurrently
(session-use s2)
(begin :rc)
(update accounts 1 [1 1500])
(commit)

; Session 1 must still see old snapshot (1000)
(session-use s1)
(assert-eq (read accounts 1) [1 1000])

; But read-latest sees 1500
(assert-eq (read-latest accounts 1) [1 1500])
(commit)

; Cleanup
(session-close s1)
(session-close s2)
(db-drop "testdb")
(env-detach :force true)
```
