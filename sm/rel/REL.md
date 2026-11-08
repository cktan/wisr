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

### `(env-attach path [:toast bool] [:cache-bytes n])`
Attaches to an existing storage home directory at `path` and
initializes the environment handle for the process. With
`:toast true`, a row's long values are stored out of line
(`sm_set_toast`): a row may then be longer than 64 KB. A value
read or scanned comes back whole. The old row `update` and
`delete` return has the keyword `:toast` where a value was out
of line, since that write erased it. `:cache-bytes` sizes the
one block cache every database reads through
(`sm_env_options_t`), 128 MB if not given; a size under 1
fails with `ERR_INVALID`.

```janet
(env-attach "/tmp/wisr_home")
(env-attach "/tmp/wisr_home" :toast true)
(env-attach "/tmp/wisr_home" :cache-bytes (* 2 1024 1024 1024))
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

### `(db-create name [opts])`
Creates a new database named `name` in the attached environment.
`opts`, a struct or table, sets `:durability-sync` and
`:deadlock-ms` of its `sm_options_t`; the others keep their
defaults.

```janet
(db-create "mydb")
(db-create "fastdb" {:durability-sync 0 :deadlock-ms 100})
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

### `(stmt-save)`, `(stmt-resume v)` and `(stmt-free v)`
`(stmt-save)` saves the running statement's view, its snapshot
and mark, and returns a handle to it (`sm_stmt_save`).
`(stmt-resume v)` runs view `v`'s statement inside the running
one, as a cursor's FETCH reads at its DECLARE's view: it sees
the transaction's writes made before the save, at its snapshot,
and none since. `(stmt-end)` ends it, as it ends `(stmt-inner)`.
A view saved before its statement read takes its snapshot at
its first resumed read, and keeps it. `(stmt-free v)` frees it.

```janet
(def v (stmt-save))
(stmt-begin)
(delete t id)
(stmt-resume v)
(read t id)        # the row, as v saw it
(stmt-end)
(stmt-free v)
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

Define and manage tables, their primary keys, and secondary
indexes. Tables and indexes live in sm3's catalog, in the
`public` namespace. Every DDL statement runs in the active
transaction: it is undone by `(abort)` or `(rollback sp)`, seen
by other sessions only once committed, and kept across a
restart.

### `(table-create name (columns ...) [(primary-key c...)])`
Defines and creates a table, backed by a store and an `_id`
sequence of its own. Without `(primary-key ...)` its rows are
named by `_id`. With it, the columns named are the table's
primary key, in their order: each is made NOT NULL, as SQL
makes it, the key is the unique index `<name>_pkey`, and the
table's rows are keyed by it ([`../CLUSTER.md`](../CLUSTER.md)),
named by their
key's values (§6). A second `(primary-key ...)` clause, one of
no column, or one of a column not there, panics.

Supported column types:
- Integer: `:int2`, `:int4`, `:int8`
- Floating: `:float4`, `:float8`
- Numeric: `:decimal64`, `:decimal128`
- Boolean: `:bool`
- Text: `:text`, `:varchar`
- Binary: `:bytea`
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

Column qualifiers:
- `:not-null` -- requires non-null value on insert/update.
- `:on-update "text"` -- the column's update default, the
  host's text, kept in the catalog and never run: what an
  update that does not assign the column writes
  ([`../../dg/ONUPDATE.md`](../../dg/ONUPDATE.md)). rel writes
  nothing from it.

```janet
(table-create users
  (columns
    (id :int4 :not-null)
    (name :text :not-null)
    (email :text)
    (active :bool)))

(table-create accounts
  (columns (id :int8) (owner :text))
  (primary-key id))
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

### `(add-primary-key table c...)`
Makes the columns `c...`, in their order, the primary key of
`table`, as `ALTER TABLE ... ADD PRIMARY KEY`: the unique
index `<table>_pkey`, keying the table's rows from then on.
sm rewrites a table that has rows into a new store, keyed by
it, its secondary indexes filled anew
([`../CLUSTER.md`](../CLUSTER.md#8-a-rewrite) §8);
an abort, or a rollback to a savepoint before it, leaves the
table as it was. It takes the table's lock exclusive. A
nullable column, or a table with a primary key already, is
`ERR_INVALID`; two rows of one key are `ERR_DUPKEY`.

```janet
(add-primary-key users id)
```

### `(alter-column-type table column type mods... [:keep])`
Changes `column`'s type to `type`, with its modifiers, as
`ALTER TABLE ... ALTER COLUMN ... TYPE`
([`../../dg/ALTERTYPE.md`](../../dg/ALTERTYPE.md)). Each row's
value is printed as its
old type's text and parsed as the new type, as a cast through
text is. sm rewrites a table that has rows into a new store,
every index on the column filled under the new type, a key
column's rows keyed anew. With `:keep` no value is converted
and only the record changes, which sm takes only when every
value stays one of the new type: the same type and modifiers,
or `text` and `varchar`, the new one no narrower. A value
the new type does not parse, or `:keep` where the values
change, is `ERR_INVALID`; two rows of one key `ERR_DUPKEY`; a
column a foreign key, a view, or an expression or partial
index uses `ERR_INUSE`.

```janet
(alter-column-type users age :int8)
(alter-column-type users name :text :keep)
```

### `(table-drop name)`
Drops the table `name`, its underlying store, and its
associated `_id` sequence.

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

### `(cat-epoch)`
Returns the catalog's epoch as the active session sees it
(`sm_cat_epoch`): a number that moves whenever a DDL, an
analyze or an index made valid is published, or the session's
temporary tables change; `:own` while the session's transaction
has DDL of its own
([`../../dg/SCHEMACACHE.md`](../../dg/SCHEMACACHE.md#3-the-epoch)
§3).

```janet
(cat-epoch)
# => 12
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
type, the numbers after the type, `:not-null`, and
`:on-update` with its text. A decimal
always shows
both numbers: `(price :decimal64 10)` comes back as
`[price :decimal64 10 0]`. With `:include-indices`, `:indexes`
holds each index as a struct of its `index-create` clauses,
every key column's order and null placement spelled out, and
`:primary true` in the primary key's.

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
- `:tree bool` -- a B+tree of the index's own pages
  ([`../BTREEINDEX.md`](../BTREEINDEX.md)), not sm1's entries.
  Its entries carry no
  visibility: a scan of it is `(idx-rows)`, which reads each
  entry's row and gives it only while the row has the entry's
  key, and its dead entries stay until `(compact)` removes
  them (`(idx-entries)` counts them). A unique tree checks a key
  against the rows its equal entries name, waiting for a
  running writer
  ([`../BTREEINDEX.md`](../BTREEINDEX.md#7-uniqueness) §7).

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
built in the background
([`../INDEXBUILD.md`](../INDEXBUILD.md#2-the-protocol) §2):
writers and
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
([`../FOREIGNKEY.md`](../FOREIGNKEY.md)). sm keeps it and checks
no row: that is
the host's. Without parent columns it takes the parent's
primary key's, and a parent without one fails with `table
'<parent>' has no primary key`: name the parent's columns. Its
index is the parent's unique index whose keys are the
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

A row's name, the `id` below, is its `_id`, an integer, in a
table without a primary key. In a table with one it is the
row's key: a tuple of the key columns' values, in the key's
order, or one value alone for a key of one column. A key of
another width panics. Where a verb gives back an `_id`, it
gives back the key, as a tuple, in such a table.

```janet
(read accounts [7])
(read accounts 7)
```

### `(insert table [val1 val2 ...])`
Encodes and inserts a new row into `table` and all attached
secondary indexes. Returns the new row's 64-bit `_id`, or its
key in a table with a primary key. If a
unique index violation occurs, raises `SM_ERR_DUPKEY`, and the
transaction is aborted (§7).

```janet
(def r1 (insert users [1 "Alice" "alice@example.com" true]))
```

### `(read table id)`
Reads the row tuple with `_id` `id` under the transaction's
snapshot.
Returns a tuple of values `[val1 val2 ...]` or `nil` if absent.

```janet
(def u (read users r1))
```

### `(read-keys table keys)`
Reads the rows of a table with a primary key named by `keys`,
an array or tuple of key tuples, in one `sm_tbl_read_keys`
call ([`../ROWSTORE.md`](../ROWSTORE.md)). Returns an array of
the rows, each
as `read` gives it, `nil` for a key with none, in the order of
`keys`. A table without a primary key panics.

```janet
(def rows (read-keys accounts [[1] [2] [7]]))
```

### `(read-latest table id)`
Reads the latest committed version of row `id`. Valid only under
Read Committed isolation.

```janet
(def u (read-latest users r1))
```

### `(lock-row table id mode)`
Locks the row `id` without writing it, to the end of the
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
rows go on from the last `_id` handed out. An abort, or a
rollback to a savepoint taken before it, brings the rows back.

```janet
(truncate jobs)
```

### `(load table rows)`
A bulk load of `rows`, an array of rows, into the table
(`sm_tbl_load_begin`, `sm_tbl_load_add`, `sm_tbl_load_end`):
into an empty table, as sorted files past 4 MB or as one batch,
under the table's lock taken exclusive to the end of the
transaction; into one that has rows, as inserts. Returns the
rows' `_id`s, or their keys. Until the transaction ends it may
not set a savepoint.

```janet
(load jobs [[1 "free"] [2 "busy"]])
```

### `(compact table)`
Compacts the table's store through GC (`sm_tbl_compact`), as
`VACUUM`: versions no reader needs are dropped, and a newest
version an aborted writer left is queued for GC's worker, which
puts the version before it back within a second or so
([`../GC.md`](../GC.md#42-chained-keys) §4.2). It takes the
table's lock as a reader. A
tree's entries whose rows are settled without them go too
([`../BTREEINDEX.md`](../BTREEINDEX.md#9-cleaning-vacuum-and-the-cleaner)
§9).

```janet
(compact jobs)
```

### `(cleaner-pass)`
Runs one pass of the btree cleaner of the session's database, on
this thread (`sm_cleaner_run`,
[`../BTREEINDEX.md`](../BTREEINDEX.md#9-cleaning-vacuum-and-the-cleaner)
§9). A script
gets no cleaner thread, so a tree's entries change only by this
and by `(compact)`. A pass cleans the leaves that readers and
writers queued, with no transaction of its own.

```janet
(cleaner-pass)
```

### `(table-size table)`
The bytes of the table's rows, in sorted files and in memtables,
summed (`sm_tbl_size`): a chained table's older versions are
counted too.

```janet
(table-size jobs)
```

### `(table-version table)`
The version in the table's definition (`sm_tbl_def`), as of this
open of the table.

```janet
(table-version jobs)
```

### `(tables-parked)`
The closed handles the session keeps for its next opens of a
table (`sm_tbl_parked`).

```janet
(tables-parked)
```

### `(analyze table [target])`
Analyzes the table in the current transaction
([`../STATISTICS.md`](../STATISTICS.md#9-the-design) §9): samples
up to 300 × `target` rows,
100 if none is given, reading no more than ten times that in
`_id`s whatever the table's size, and records each
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

### `(update table id [val1 val2 ...])`
Updates the row `id` and synchronizes secondary indexes.
Returns the previous tuple values `[old1 old2 ...]`. New values
of a primary key's columns move the row to its new key; a key
another row has is `ERR_DUPKEY`.

```janet
(def old (update users r1 [1 "Alice C." "alice@example.com" true]))
```

### `(delete table id)`
Deletes the row `id` from the table and secondary indexes.
Returns the previous tuple values `[old1 old2 ...]`.

```janet
(def old (delete users r1))
```

### `(scan table [:from first] [:to last])`
Scans the table, keyless, in `_id` order over the `_id`s
`[first, last]`. Returns an array of `[id [val1 val2 ...]]`
pairs.

In a table with a primary key it scans the rows in key order,
the bounds keys or their first columns, both inclusive: the
rows themselves over the range (`sm_tbl_idx_scan_rows`), as
digres scans its primary key, or every row with no bound
(`sm_tbl_scan_rows`), as its sequential scan. Each pair is
`[key [val1 val2 ...]]`.

```janet
(def all-rows (scan users))
(def slice (scan users :from 2 :to 10))
(def some (scan accounts :from [100] :to [199]))
```

### `(scan-count table [:from first] [:to last])`
The number of rows `scan` gives over the same bounds, counted
with none decoded: each is stepped to in the cursor's pin, as
digres's scans step. What a scan costs in sm and RocksDB alone
(`../../bench/relq6`).

```janet
(scan-count users :from 2 :to 10)
# => 9
```

### `(idx-scan index [:ge lo] [:gt lo] [:le hi] [:lt hi])`
Scans a secondary index across key prefix bounds.
Bounds are tuples matching key prefix columns.
Returns an array of `[id cut include-tuple]` entries, `id` the
row's `_id`, or its key, read from the row, in a table with a
primary key: `cut`
is true when sm2 truncated an over-long key, so the caller must
recheck the row; `include-tuple` is nil for an index without
INCLUDE columns. The key itself is not returned: sm2 can decode
an entry's key columns (`sm_idx_scan_key`), but `rel` does not
expose them.

```janet
(def hits (idx-scan idx_users_email :ge ["alice@"] :le ["alice@~"]))
```

### `(idx-rows index [:ge lo] [:gt lo] [:le hi] [:lt hi])`
The rows a scan of the index's rows gives between the bounds,
as `(idx-scan)` takes them, in the index's order: a tree's,
each entry's row as the transaction sees it while it still has
the entry's key
([`../INDEXBUILD.md`](../INDEXBUILD.md#2-the-protocol) §2), or a
primary
key's, its rows. Returns `[id row]` pairs as `(scan)` does,
`[key row]` in a table with a primary key.

```janet
(index-create jobs_state (table jobs) (key (state :asc)) :tree true)
(idx-rows jobs_state :ge ["b"] :le ["b"])
# => @[(2 (2 "b" "Bob")) (3 (3 "b" "Cy"))]
```

### `(idx-entries index [:ge lo] [:gt lo] [:le hi] [:lt hi])`
How many entries the index holds between the bounds, none read
nor checked: a tree's dead entries among them, until
`(compact)` removes the ones no snapshot needs.

```janet
(idx-entries jobs_state)
# => 8
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
   changed the row and some indexes before it failed, and the
   abort is what undoes that. A script that catches the panic
   calls `(abort)`, which is harmless, and `(begin)` again.
   Three outcomes are not errors and leave the transaction
   active: `ERR_UPDATED`, answered with `(read-latest ...)`,
   `ERR_BLOCKED` under `(qa-nowait true)`, answered by repeating
   the step, and `ERR_SELFMODIFIED`, a row the statement already
   changed, which the script skips. A row that `rel` itself
   refuses, before any call on the storage manager, aborts
   nothing.

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
