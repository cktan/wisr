# CATALOG_INMEM — sm3's catalog in memory

sm3's catalog in memory: the definitions of namespaces,
tables, columns, indexes, collations, sequences, foreign keys
and views, with indexes' states and tables' statistics, as
sessions read them, how a session's DDL changes them, and the
calls. `CATALOG_ONDISK.md` is where they are stored, and in
what bytes. The two fill in `SM3_DESIGN.md` §2, whose decisions
they follow: the catalog is held in memory, written through to
storage in the DDL's own transaction, and changed under
lock-based DDL, as in PostgreSQL.

**Status: built** — `cat_init`, `cat_open`, the DDL of §2, a
session's overlay, which keeps its transaction's DDL to itself
until commit, the table-oid locks, under which commit publishes
it, a session's temporary catalog, and the C API over them,
which digres and `rel` run on; the store, its keys and its
values are `CATALOG_ONDISK.md`'s. §4 is what is open.

---

## 1. In memory

`sm3/include/sm3.h` declares what `cat_open` builds:

- **`table_def_t`**: the table's oid, namespace, name, storeid
  and `_id` sequence; its `column_def_t`s, each with its sm2
  type, typmod, flags, missing value and collation; its
  `index_def_t`s, each with its `key_column_t`s and INCLUDE
  attributes built for sm2, and its entries' value laid out
  (`sm2::idx_layout_t`); and the table's sm2 `schema` with
  its `tup_desc_t`, built with it, so a reader never lays out a
  row itself. Every open of the table borrows these
  (`rel_open_desc`, `idx_open_layout`), held alive by the
  definition the handle holds: a generation's layouts go with
  it. A definition is built whole and never edited, and
  never copied, since its `desc` views its `schema`.
- **`catalog_t`**, opaque and shared by sessions: every committed
  table by oid and by namespace and name, as
  `std::shared_ptr<const table_def_t>`; every namespace; every
  collation, opened by `cat_open` at its recorded ICU version and
  closed by `cat_close`, and the namespace of each; each
  sequence's namespace and name, by its oid and its oid by them,
  its record and counter being read from the store in the
  transaction (`CATALOG_ONDISK.md` §2); every view, by oid and by
  namespace and name; the register of every session's temporary
  views (`../VIEW.md` §12.3); and each table's statistics, loaded
  on first use and kept (`SM3_DESIGN.md` §6). A reader-writer
  mutex guards it.
- **`session_t`**: an sm1 session, the catalog it reads, and its
  open transaction's **overlay** — the tables, names,
  collations, namespaces, index states, sequences, statistics
  and views its DDL has changed, each tagged with the
  savepoint depth it was made at (0 before any). A lookup
  through the session finds its newest change first, then the
  shared catalog, so the transaction sees its own DDL and no
  other session sees it.
- **The count of publishes.** The catalog counts the commits
  that published DDL, and remembers for an index the count at
  which it became valid. A session notes the count before it
  takes a snapshot, and may scan an index only if it was valid
  by then (`SM3_DESIGN.md` §3). Neither is on disk: no
  snapshot survives a restart.
- **A table's version** is from the same count: the catalog
  remembers for each table the count at which its definition,
  and its statistics, were last published, and
  `cat_table_version` gives the greatest of those and its
  indexes' counts of becoming valid, as `cat_view_version`
  gives a view's; `table_version_own` while the session's own
  transaction has changed the definition or the statistics. A
  host that caches what it learns of a table under the
  table-oid lock (`sm.h`'s `sm_table_def_t::version`) knows
  from it whether the cache is still good, with no word from
  the session that changed it (`../../dg/SHMEM.md` §5.1).
  **The namespaces' epoch**, `cat_namespace_epoch`, is the count
  at which a namespace was last made or dropped. A table not
  changed since the catalog was loaded is at 0, as is the epoch:
  a host's caches are as new as the process.
- **A definition's generations.** A table's definition is a
  generation, one `std::shared_ptr<const table_def_t>`, and
  what is built from it lives and dies with it. Take a
  transaction that runs `ALTER TABLE t ADD COLUMN`:
  - The ALTER takes t's table-oid lock X, held until the
    transaction ends, so no other transaction opens t
    meanwhile: an open's IS conflicts with it.
  - `build_table` builds generation N+1 then, at the ALTER,
    with its layouts (above), into the session's overlay,
    not the shared catalog. The transaction's own opens of t
    read N+1 there, at `table_version_own`.
  - `commit` takes the catalog's mutex exclusive; commits in
    sm1, which releases t's lock; publishes, replacing the
    catalog's pointer to t's definition with N+1 and dating
    t's version from this publish; and releases the mutex. A
    transaction waiting on t's lock gets it at sm1's commit,
    but reads the definition only once the mutex is free, so
    it sees N+1, never N.
  - N is not marked or swept: the catalog drops its reference,
    and N, with its layouts, is freed with the last holder's,
    a handle not yet closed or a pin.
  - A commit that fails, or an abort, drops the overlay, and
    N+1 with it; N stays.
  So nothing built from a definition needs invalidating; a
  host's cache keyed by the version (`../../dg/STATISTICS.md`
  §2.2) sees the new one at its next look.
- **Each index's state**, in a map by index oid beside the
  definitions and not in them. A definition is immutable and
  is replaced whole, under the table's lock X; a state changes
  under IX, while transactions hold the definition, and two
  indexes of one table may change at once. A session's
  overlay holds the states its transaction has set, and
  `commit` publishes them with everything else, under the
  catalog's mutex, so the map and the store never differ.
- The builder's queue and workers are not here: they are the
  process's, not a catalog's (`SM3_DESIGN.md` §3).
- **A session's temporary catalog** (`TEMPTABLE.md` at the top
  level, §4): the definitions of its temporary tables, their
  names, which are in no namespace, their indexes' states, and
  its temporary views (`../VIEW.md` §12.2). A change to a
  temporary table is made in the overlay like any other,
  marked as one; `commit` moves it
  here and not into the shared catalog, and takes the
  catalog's mutex only if something else is to be published.
  A lookup through the session finds the overlay first, then
  this, then the shared catalog. `cat_put_table`,
  `cat_del_table` and `cat_put_index_state` write nothing to
  the store for one and take no lock, and `cat_lock_table`
  takes none.

---

## 2. Calls

`cat_init` and `cat_open` need no session: each names its
database by environment and name, opens an sm1 session of its
own, works in a transaction of its own, and closes the
session. The rest take an sm3 session and work in its
transaction. Each reports failure as sm1 does (`DESIGN.md`
§5).

- **`cat_init(env, dbname, opt)`** makes a new database's
  catalog: `META`, the `sm3.oid` sequence, and the
  `pg_catalog` and `public` namespaces at their fixed oids,
  whole or not at all. `opt` is the options its session opens
  the database with, as `sm1::opendb` takes them. A database
  with a catalog fails with `SM_ERR_EXISTS`.
- **`cat_open(env, dbname, cat)`** reads `META` first — none is
  `SM_ERR_NOTFOUND`, another format `SM_ERR_VERSION` — then
  scans the names, for each collation's and sequence's
  namespace, and then namespaces, collations, tables,
  columns, indexes, constraints, index states and views, one
  kind each; puts each table's record together from its
  entries, with its foreign keys and those that reference it;
  and builds every definition. Statistics are not read: each
  table's are loaded when first asked for (`cat_get_stats`).
  A definition that contradicts itself — an unknown type or
  collation, an index on a column that is not there, a
  foreign key to no table — is `SM_ERR_CORRUPT`.
- **`cat_close(cat)`** frees the catalog and its collations. A
  catalog attached to the builder is detached first, by its
  caller.
- **`cat_get_table(cat, oid)`** and **`cat_get_table(cat, nspoid,
  name)`** find a committed table, or nullptr, and
  **`cat_list_tables(cat, nspoid)`** lists the names of a
  namespace's committed tables, sorted;
  **`cat_get_table_by_store(cat, storeid)`** finds the
  committed table whose rows are in a store, for a lock's
  table (`sm1::lock_status`).
  **`cat_get_namespace(cat, name, oid)`** and
  **`cat_list_namespaces(cat)`** are the same for committed
  namespaces. These need no session, and see nothing a
  transaction has not committed.
- **`session_open(cat, s, ss)`** opens a session over the sm1
  session `s`; **`session_close`** drops its overlay.
- **`begin`, `savepoint`, `rollback`, `forget`, `commit`,
  `abort`** wrap sm1's and keep the overlay in step.
  `rollback(d)` drops the changes tagged `d` or deeper, since the
  mark at `d` survives; `forget(d)` re-tags them `d - 1`, so a
  later mark that reuses the depth cannot drop them; `abort`
  drops them all;
  `commit` commits in sm1, then publishes them into the shared
  catalog, or drops them if the commit fails. With changes to
  publish, it holds the catalog's mutex exclusive across both:
  sm1's commit releases the table-oid locks, and a transaction
  that gets one then waits on the mutex to read the
  definition, so it sees the new one, never the old.
- **`cat_lock_table(ss, oid, mode, def)`** takes the table's
  oid lock — IS to read it, IX to write it — held to the end
  of the transaction, then sets `def` to its definition as the
  session sees it, which cannot change until then. A table
  dropped while the caller waited is `SM_ERR_NOTFOUND`.
  **`cat_trylock_table`** is the same, never waiting: 1, with
  nothing taken, if another transaction holds the lock in a
  mode that conflicts.
- **`cat_get_table(ss, oid)`** and **`cat_get_table(ss, nspoid,
  name)`** find a table as the session's transaction sees it,
  and **`cat_list_tables(ss, nspoid)`** lists the names of its
  tables, sorted;
  **`cat_get_index(ss, nspoid, name, table, index)`** finds an
  index and its table: in the definitions of the tables the
  transaction changed, then in a map of the committed indexes'
  names, by namespace and name, built at load and kept at
  each publish.
- **`cat_get_namespace(ss, name, oid)`** finds a namespace by
  name as the session's transaction sees it, by a look at
  each, since they are few and no map by name is kept;
  **`cat_get_namespace_name(ss, oid, name)`** finds one's name
  by its oid; and **`cat_list_namespaces(ss)`** lists their
  names, sorted.
- **`cat_get_sequence(ss, nspoid, name, oid)`** and
  **`cat_get_sequence_name(ss, oid, nspoid, name)`** find a
  sequence as a table is found by its name: the transaction's
  own creates and drops, then every committed one, from
  memory, not as of the snapshot;
  **`cat_list_sequences(ss, nspoid, names)`** lists a
  namespace's the same way, so it never names one they do
  not find, as a scan of the store in a statement would once
  the statement dropped it.
  **`cat_get_sequence_def(ss, oid, def)`** reads its record
  from the store, so it needs a transaction open.
- **`cat_lock_view(ss, oid, mode, def)`** takes a view's oid
  lock and gives its definition, as `cat_lock_table` does a
  table's; a temporary view takes none.
  **`cat_get_view(ss, oid)`** and **`cat_get_view(ss, nspoid,
  name, oid)`** find one with no lock,
  **`cat_list_views(ss, nspoid)`** lists a namespace's, and
  **`cat_view_readers(ss, oid)`** gives the views that read a
  relation, other sessions' temporary ones among them
  (`../VIEW.md` §12.3).
- **`cat_get_fkey(ss, oid, table, def)`** finds a foreign key
  and its table, the child, with no lock.
- **`cat_get_stats(ss, oid, out)`** gives a table's statistics
  as the session sees them: its transaction's own, else the
  committed ones, loaded in a transaction of their own on
  first use and then kept, so a plan takes no lock and reads
  nothing (`SM3_DESIGN.md` §6).
- **`prepare(ss, gid)`** prepares as sm1 does, but refuses a
  transaction that made DDL: its overlay would have to outlive
  the session.

### 2.1 The catalog's writes: put and del

The catalog is a repository. It stores the record it is given,
finds it again, and forgets it; it makes no store, no
sequence but a sequence object's own counter, and fills no
index. What it checks is only that its
own records agree: a name is taken once, a table's namespace
is there, a namespace that goes is empty. So its writes are
few, and named for what a repository does.

- **`cat_new_oid(ss, oid)`** takes the next oid from the
  sequence `sm3.oid`, and fails past `last_oid`.
- **`cat_get_table_rec(ss, table_oid, rec)`** reads a table's
  record in order to change it. A record, `table_rec_t`, is
  the table as the catalog keeps it, plain values with types
  and collations by code and oid; a definition, `table_def_t`,
  is the same table resolved for use, and carries the record
  it was built from. The call takes the table's oid lock
  X, so it waits out the table's readers and writers and they
  wait for it, and then takes the record from the catalog in
  memory, which has every commit by the time the lock is
  granted. It does not read the store: the transaction's
  snapshot may be older than a column another has just
  added, and DDL must change the table as it now is.
- **`cat_put_table(ss, table_oid, rec)`** stores a table's
  record. A table that is not there is added: its namespace
  must be (`SM_ERR_NOTFOUND`), and is locked shared against a
  drop. One that is there is replaced, under its table-oid lock
  X. Either way the names in the record — the table's and
  each index's — are made to be the names taken: the new ones
  are inserted first, so one another object has is
  `SM_ERR_DUPKEY` and leaves the table as it was, and then
  the ones no longer in the record are erased. That is all a
  rename is, and all that naming a new index is. Of the
  record itself it writes only the entries that differ from
  the one in memory: the table's own fields, a column, an
  index; and it erases the key of an index no longer there.
  Each is written under an X lock on its key, over its newest
  committed version: at READ COMMITTED a `catkv_get_latest`,
  which lets the write through; at REPEATABLE READ the lock
  fails on a version newer than the snapshot. A column cannot
  be taken out, only marked dropped, and the indexes stay in
  the order of their oids.
- **`cat_del_table(ss, table_oid)`** erases a table's record
  and the names it took. Its store and its sequence are the
  caller's to drop.
- **`cat_get_index_state(cat, index_oid)`** and
  **`cat_get_index_state(ss, index_oid)`** give an index's
  state, with the error and reason of a failed one: as
  committed, or as the session's transaction sees it. An
  index with none recorded is pending.
- **`cat_put_index_state(ss, table_oid, index_oid, status)`**
  stores an index's state. It takes the table's lock IX, as a
  writer of the table does, so it waits for no reader or
  writer and none waits for it; only DDL on the table is kept
  out, which is what stops the index being dropped under it.
  That is enough because a state seen late is seen as
  pending, and pending is always safe: the index is
  maintained and not scanned. A table opened before the
  change goes on with the state it saw (`tbl_index_state`),
  and a snapshot older than the index's last entries is kept
  off it by the count of publishes (§1), whether or not its
  transaction had opened the table. Because that rests on a
  state only ever leaving pending, a state is set once: an
  index that is valid or failed is `SM_ERR_INVALID` to set
  again, and of two transactions setting one index's state
  the second waits for the first and is then refused.
  Erasing an index, in `cat_put_table` or `cat_del_table`,
  erases its state.
- **`cat_put_namespace(ss, namespace_oid, name)`** stores a new
  namespace: its name — under namespace oid 0, so a
  namespace's name is unique in the database and a taken one
  is `SM_ERR_DUPKEY` — and its blob.
- **`cat_del_namespace(ss, namespace_oid)`** erases an empty
  namespace's name and blob. One that holds a table, a
  sequence, a view or a collation is `SM_ERR_INUSE`, and the
  two built in are
  `SM_ERR_INVALID`. A namespace has a lock, its oid's, as a
  table's oid lock is its table's: a put in it takes the
  lock shared and the del takes it exclusive, both to the end
  of the transaction, so the del waits out the puts and they
  it, while two puts do not wait for each other. Whether the
  namespace is there, and whether it is empty, are then read
  from the catalog in memory, which has every commit by the
  time the lock is granted; the transaction's own snapshot
  may be older than a table just made or a del just
  committed. For that the catalog keeps each collation's and
  each sequence's namespace in memory, found at `cat_open` by
  one scan of the names. There is no del of a collation, so a
  namespace that has held one stays.
- **`cat_put_collation(ss, collation_oid, namespace_oid, name,
  locale, deterministic)`** stores a new collation. The
  catalog opens it, to check the locale and record the ICU
  version it opens at, and keeps it open for definitions to
  point at.
- **`cat_put_sequence(ss, sequence_oid, namespace_oid, name,
  start, increment)`** stores a new sequence and makes its
  counter, the sm1 sequence `sm3.seq.<oid>`; its name is taken
  from the namespace's relations, so a taken one is
  `SM_ERR_DUPKEY`. **`cat_del_sequence(ss, sequence_oid)`**
  erases it, its name and its counter.
- **`cat_put_stats(ss, table_oid, stats)`** records a table's
  statistics, replacing any it had, under an X lock on its
  `STATS` entry, so two analyzes of one table queue. Each
  statistic must name a column or an index expression of the
  table, and its values be of that one's type
  (`SM_ERR_INVALID`).

A missing table, namespace or collation is `SM_ERR_NOTFOUND`.
Each call writes the catalog in the session's transaction and
records the change in its overlay; a collation it puts stays
open there, for the transaction's DDL to use at once.

### 2.2 DDL: what a CREATE, a DROP or an ALTER does

The DDL is a caller of the catalog, in `src/ddl/ddl.cc`. Each
call is one statement's work: it makes or drops what the
object needs outside the catalog, and puts the object's record
in the catalog or takes it out. Every one that changes a table
begins with `cat_get_table_rec` and ends with `cat_put_table`.

- **`create_namespace(session, name, oid)`** takes an oid and
  puts the namespace; **`drop_namespace(session, oid)`** dels
  it.
- **`create_table(session, nspoid, name, columns, oid)`**
  checks the name, the column names and each column's type
  code and typmod (`SM_ERR_INVALID`); takes an oid; makes the
  table's store and `_id` sequence; and puts its record. If
  the put fails — a taken name, no such namespace — it drops
  the store and the sequence again, so a transaction that
  goes on leaves neither behind.
- **`create_temp_table(session, name, columns, oid)`** is
  `create_table` in no namespace: a temporary store and `_id`
  sequence, and a definition in the session's temporary
  catalog alone (`../TEMPTABLE.md` §4). An identity column is
  `SM_ERR_INVALID`, as no sequence is temporary.
- **`drop_table(session, oid)`** dels the table's record and
  drops its store, which takes its rows and index entries
  with it, its `_id` sequence, and the sequences its columns
  own. One a view reads is `SM_ERR_INUSE`.
- **`rename_table(session, oid, name)`** puts the record back
  under the new name.
- **`truncate_table(session, oid)`** empties the table: it
  gets a new, empty store, and the old one is dropped, rows and
  index entries with it. The columns, the indexes and their
  states, and the `_id` sequence are kept, so new rows go on
  from the last `_id` handed out, and the cost is the same
  whatever the table holds.
- **`add_column(session, oid, col, missing)`** appends a
  column. A row written before it reads it as `missing` — its
  bytes, checked to be a datum of the column's type — or as
  null without one. So a `NOT NULL` column without one needs
  an empty table: the table-oid lock X is taken only if it is
  free now, and kept only if the store holds no version at
  all; a savepoint set before it lets the lock go otherwise.
- **`set_not_null(session, oid, name)`** makes a live column
  `NOT NULL`, opening the table exclusive and reading every
  row the transaction sees, `SM_ERR_NOTNULL` if one holds
  none. **`set_identity(session, oid, name, identity, seq)`**
  makes one an identity column whose values come from `seq`,
  which it then owns for good, as `create_table` makes one.
  `ADD COLUMN` with a volatile default or of an identity
  column is `add_column`, the rows given their values, then
  these.
- **`rename_column(session, oid, name, new_name)`** puts the
  record back with the live column renamed. An index names
  its columns by place, so it is untouched.
- **`drop_column(session, oid, name)`** marks the column
  dropped; it keeps its place and reads as null, and its name
  can be added again. One an index uses is refused.
- **`create_index(session, table, name, keys, includes,
  unique, oid)`** adds an index to the record — its key columns
  with their direction, null order and collation, a collation
  only on text or varchar — and takes the table's next relid,
  which is never handed out again. It does not fill the index
  from the table's rows: it records it as pending, for the
  build to fill and mark valid (`SM3_DESIGN.md` §3). On a
  table its own transaction made, which no other can see, it
  runs the build itself, over the rows the transaction has
  written, and records the index valid. With `primary` it marks
  the index the table's primary key, a mark sm3 keeps for the
  host and acts on no further: unique, on columns alone, each
  `NOT NULL`, and the table's only one. With `constraint` it
  marks the index one a `UNIQUE` or `PRIMARY KEY` constraint
  made, not `CREATE UNIQUE INDEX`, another mark for the host:
  unique, on columns alone. An index written before the mark
  was kept reads it false.
- **`create_index(…, eval, reads, oid)`** is the same for
  a partial or an expression index: `eval` its blob, `reads`
  the columns it reads. It needs the catalog to have an
  evaluator, **`cat_set_evaluator(cat, eval)`**, given once.
  **`cat_set_toast(cat, on)`**, given once too, says whether
  the host reads TOAST pointers, and so whether a table's long
  datums are stored out of line (`../sm2/SM2_DESIGN.md` §4.2).
- **`set_index_state(session, table, oid, state, error,
  reason, dup)`** puts an index's state, as the build does when
  it ends, with the error and its message for a failed one,
  and the key of a row that holds a duplicated key. It
  is `cat_put_index_state` and nothing more, so it does not
  wait for the table's users.
- **`drop_index(session, table, oid)`** erases the index's
  entries, one by one over its relid in the table's store,
  since sm1 has no range delete, and takes it out of the
  record.
- **`create_collation(session, nspoid, name, locale,
  deterministic, oid)`** takes an oid and puts the collation.
- **`create_fkey(session, table, def, oid)`**,
  **`drop_fkey`**, **`validate_fkey`**, **`rename_fkey`**: a
  foreign key of the table, its child, under its lock X and,
  to add or drop one, its parent's (`../FOREIGNKEY.md` §5). Its
  entry is the child's; the parent's `referenced_by` is derived,
  at load and by these, and written nowhere. `drop_table` of a
  parent another table references, `drop_index` of the parent
  index a foreign key uses and `drop_column` of a child's column
  in one are `SM_ERR_INUSE`.
- **`create_sequence(session, nspoid, name, start, increment,
  oid)`** takes an oid and puts the sequence;
  **`drop_sequence(session, oid)`** dels it, `SM_ERR_INUSE` if
  a column owns it; **`own_sequence(session, oid, table,
  attnum)`** makes a column own it, SQL's `OWNED BY`, or none
  with table 0; and **`nextval(session, oid, out)`** is sm1's
  `nextseq` on its counter, not transactional.
- **`create_view(session, nspoid, name, def, oid)`**,
  **`replace_view`**, **`rename_view`**, **`drop_view`**: a
  view is its record alone, with nothing outside the catalog
  to make or drop (`../VIEW.md` §5). Each relation it reads is
  locked IS to the end of the transaction; `nspoid`
  `nsp_temp` makes a temporary view, which alone may read a
  temporary relation. A view another view reads is
  `SM_ERR_INUSE` to drop, and a replace whose reads reach the
  view itself is `SM_ERR_INVALID`.

`create_index` holds the table's oid lock X to the end of
its transaction, as every DDL does; it is the build that is
concurrent (§2.3).

### 2.3 The builder

The builder is no part of the catalog either
(`SM3_DESIGN.md` §3). It runs a service and waits on it, so
its calls are its own, `builder_*`, in
`src/builder/builder.cc`. It meets the rest in three places:

- **`create_index`** hands an index it leaves pending to
  the builder's queue, `builder_enqueue`, in a function it
  gives `sm1::on_commit`: it runs once the transaction has
  committed, and not at all if the transaction aborts or
  rolls back past the index.
- **`enqueue_pending_indexes(cat)`**, in the DDL, queues every
  index that is pending, by a walk of the committed tables,
  namespace by namespace. It is called when a database is
  opened, for the indexes whose `on_commit` function a crash
  lost. The builder itself never looks for work: it is given
  a table and an index, checks the index is still pending,
  and fills it.
- **`set_index_state`**, above, marks an index valid or
  failed when its build ends.

---

## 3. Source

```
sm1/src/access.cc          store 0 under "U" for callers
sm2/src/rel/catkv.cc       the catalog key-value store
sm3/include/sm3.h          definitions and calls
sm3/src/catalog/catalog.h  keys, records and parts (internal)
sm3/src/catalog/internal.h catalog_t, session_t and the overlay
sm3/src/catalog/key.cc     keys
sm3/src/catalog/blob.cc    values: parts and their framing
sm3/src/catalog/load.cc    cat_open and the lookups
sm3/src/catalog/ddl.cc     cat_init, and the puts and dels
sm3/src/ddl/ddl.cc         the DDL: a caller of the catalog
sm3/src/catalog/session.cc the session, its transaction, publishing
sm3/src/builder/builder.cc the builder: a caller of the catalog
sm3/test/test_layout.cc    keys and values
sm3/test/test_catalog.cc   init, load and create
sm3/test/test_ddl.cc       the rest of the DDL
sm3/test/test_overlay.cc   the overlay: visibility, rollback, abort
sm3/test/test_lock.cc      table-oid locks, and publishing under them
sm3/test/test_temp.cc      the temporary catalog
sm3/test/test_fkey.cc      foreign keys
sm3/test/test_view.cc      views, temporary ones among them
sm3/test/test_stats.cc     statistics, recorded and read
src/sm.cc                  the C API: sm_cat_*, a catalog per database
rel/test/test_catalog.rel  DDL through the C API, end to end
```

The C API (`include/sm.h`) keeps each database's catalog in its
`sm_env_t`, made by `sm_mkdb` and loaded by the first
`sm_opendb`, and an sm3 session in each `sm_session_t`, so its
transaction calls keep the overlay in step. `sm_cat_*` are the
calls above as the host needs them: namespaces, tables and
their columns, indexes, sequences, foreign keys and views,
looked up, listed, locked, created, altered and dropped, with
definitions as plain C structs; a table's statistics are
`sm_tbl_stats`, on an open table. digres and `rel` keep no
catalog of their own.

---

## 4. Not built yet

Open: `DROP SCHEMA … CASCADE`, which waits for the
dependencies (search paths are the SQL layer's); dependencies
in general, such as refusing `DROP COLLATION` in use, by a
scan of the tables' records until it earns a key kind: what
is built is each kind's own, a view's reads and a foreign
key's parent, both derived in memory and stored nowhere;
`DROP COLLATION` itself, which there is none of; and startup
cost for thousands of tables, unmeasured, to which
`cat_open`'s scan of every name now adds.
