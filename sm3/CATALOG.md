# CATALOG — sm3, the catalog

sm3's catalog: where table, column, index, namespace and
collation definitions are kept, how they are laid out, and how
they come into memory. It fills in `SM3_DESIGN.md` §2, whose
decisions it follows: the catalog is held in memory, written
through to storage in the DDL's own transaction, and changed
under lock-based DDL, as in PostgreSQL.

**Status: built** — the store, its keys and values, `cat_init`,
`cat_open`, the DDL of §5, a session's overlay, which keeps its
transaction's DDL to itself until commit, the schema locks,
under which commit publishes it, and the C API over them, which
`rel` runs on. §7 is what is settled but not built yet.

---

## 1. Where it lives

The catalog is a key-value map in each database, kept in sm1's
own catalog store, store 0, under the key prefix `"U"`.

- **One commit point.** Store 0 is an ordinary MVCC store in the
  same database as the tables, so DDL writes its catalog entries
  and its tables' stores in one sm1 transaction: they commit,
  roll back and survive a crash together.
- **Beside sm1's own rows.** Store 0 is split by key prefix:
  `"R"` rows are sm1's stores and `"S"` rows its sequences
  (`sm1/SM1_DESIGN.md` §11). sm1 never reads `"U"` rows —
  `sm1::lsstore` scans `["R", "S")`, the startup sweep reads
  `"R"` rows — and lets a caller use store 0 under `"U"` only
  (`sm1::upper_prefix`), so neither can touch the other's rows.
- **Always there.** Store 0 exists in every database from its
  birth, so the catalog needs no search at startup and no
  transaction can lose it.
- **Object ids.** sm1's stores and sequences share one counter,
  which starts at 1000; 1 to 999 are reserved. sm3's own oids
  come from the sequence `sm3.oid`, which `cat_init` makes to
  start at 1000; oids below it are for built-in objects:
  `pg_catalog` is 1, `public` is 2. `nextseq` is not
  transactional, so an aborted DDL leaves a gap, as PostgreSQL's
  oid counter does.

sm3 reaches the catalog through sm2's catalog key-value store,
`catkv_*` in `sm2/include/sm2.h`: each call is one sm1 call on
store 0, in the session's transaction, with `"U"` added to the
key going in and stripped coming out.

| Call | Does | Over |
| --- | --- | --- |
| `catkv_open`, `catkv_close` | a handle on the session's catalog | — |
| `catkv_get` | 0 with the value, 1 if none; a view valid to the next call | `sm1::read` |
| `catkv_get_latest` | as `catkv_get`, the newest committed version, under a lock held | `sm1::read_latest` |
| `catkv_insert` | a new key; `SM_ERR_DUPKEY` if taken | `sm1::insert` |
| `catkv_update`, `catkv_erase` | replace or remove a key; `SM_ERR_NOTFOUND` if missing | `sm1::update`, `sm1::erase` |
| `catkv_lock` | a row lock in any of sm1's modes (IS, IX, S, SIX, X) | `sm1::lock` |
| `catkv_scan`, `catkv_scannxt`, `catkv_scanend` | the keys under a prefix, in order | `sm1::scan` |

---

## 2. Keys

Every key is a kind (`int2`), then the kind's own columns,
encoded ascending by sm2's `encode_key` (`src/catalog/key.cc`).
A kind's keys are contiguous, and ordered by their columns. The
kinds' values are on disk, so frozen.

| Kind | Value | Key after the kind | Holds |
| --- | --- | --- | --- |
| `META` | 1 | — | the format version, `int4` |
| `NAME` | 2 | namespace oid `int8`, name `text` | what the name names: kind `int2`, oid `int8`, and an index's table oid `int8` |
| `NAMESPACE` | 3 | oid `int8` | the namespace's blob |
| `TABLE` | 4 | oid `int8` | the table's blob: the table, its columns, its indexes |
| `COLLATION` | 5 | oid `int8` | the collation's blob |
| `INDEX` | 6 | — | no key of its own: an index is named by a `NAME` entry and kept in its table's blob |
| `LOCK` | 7 | table oid `int8` | nothing: the table's schema lock, locked and never written |

- **A table is one entry.** Its columns and indexes are in its
  blob, so every DDL on a table rewrites that one value.
- **Names are keys.** Every named object owns a `NAME` key, so a
  name taken in its namespace fails with sm1's `SM_ERR_DUPKEY`,
  and two sessions taking it at once meet in sm1's write-write
  conflict. A namespace's own name is in namespace 0, which is
  no namespace.
- **A table's rowno sequence is `sm3.rowno.<oid>`**, made with
  its store.
- **A table's schema lock is its `LOCK` key**, not its `TABLE`
  key (`SM3_DESIGN.md` §2). Every DDL rewrites the `TABLE`
  entry, and a lock on a key that was written fails at
  REPEATABLE READ for a version newer than the snapshot. A key
  no one writes never does, so a reader that waited out DDL
  gets the new definition, not a serialization failure.

---

## 3. Values

A value is a sequence of **parts**, each an sm2 tuple under a
schema sm3 fixes for it, preceded by its length as a varint
(`src/catalog/blob.cc`):

```
table blob = table | column × natts | (index | key × nkey | include × ninclude | read × nread) × nindex
```

| Part | Fields |
| --- | --- |
| meta | format version `int4` |
| name | kind `int2`, oid `int8`, table oid `int8` or null |
| table | namespace `int8`, name `text`, storeid `int8`, rowno sequence `text`, next index relid `int2`, natts `int4`, nindex `int4` |
| column | name `text`, type code `int2`, typmod `int4`, not null `bool`, dropped `bool`, missing value `bytea` or null, collation oid `int8` or null |
| index | oid `int8`, name `text`, prefix `int2`, unique `bool`, nkey `int4`, ninclude `int4`, state `int2`, error `int2` or null, reason `text` or null, eval `bytea` or null, nread `int4` or null |
| key | attnum `int4`, desc `bool`, nulls first `bool`, collation oid `int8` or null, type code `int2` or null, typmod `int4` or null |
| include | attnum `int4` |
| read | attnum `int4` |
| namespace | name `text` |
| collation | name `text`, locale `text`, deterministic `bool`, ICU version `text` |

- **A part evolves as a tuple does.** A field appended to a
  part's schema reads as its missing value in a value written
  before it (`sm2/TUPLE.md` §2), so a value needs no version of
  its own. `META`'s version changes only if a key's layout, or
  the framing, does.
- **An index's state** is 0 building, 1 valid or 2 failed
  (`SM3_DESIGN.md` §3). It was appended to the index part, so
  an index written before it has none, and reads as valid.
- **A partial or an expression index keeps a blob.** Its eval
  is its predicate and its expressions as the host serialized
  them, bytes sm3 does not read, and its read parts are the
  columns they read (`SM3_DESIGN.md` §3). A plain index has
  neither. A key that is an expression has attnum -1 and gives
  its own type code and typmod, which a column's key leaves
  null.
- **A failed index keeps why.** Its error is the code its build
  failed with and its reason that error's message, both null
  in any other state.
- **Type codes are sm2's frozen codes** (`sm2/TYPE.md` §3); code
  7, the retired `decimal`, is never written. A declared
  `decimal(p, s)` is stored as the sm2 type it maps to
  (`SM3_DESIGN.md` §2.1).
- **Column positions are 0-based**, in key and include parts as
  in memory.
- **A missing value is its bytes**, a varlena for a
  variable-width type, as sm2's `attribute_t` holds it.
- **The next index relid is kept** in the table part, so a
  prefix is never reused, across restarts too (`SM3_DESIGN.md`
  §4).
- **Each part is held to 64 KB** (`sm2/TUPLE.md` §1); a value, a
  sequence of them, is not, and is an sm1 value of up to 64 MB.
- A value cut short, with bytes past its last part, or with a
  required field null, is `SM_ERR_CORRUPT`.

---

## 4. In memory

`sm3/include/sm3.h` declares what `cat_open` builds:

- **`table_def_t`**: the table's oid, namespace, name, storeid
  and rowno sequence; its `column_def_t`s, each with its sm2
  type, typmod, flags, missing value and collation; its
  `index_def_t`s, each with its `key_column_t`s and INCLUDE
  attributes built for sm2; and the table's sm2 `schema` with
  its `tup_desc_t`, built with it, so a reader never lays out a
  row itself. A definition is built whole and never edited, and
  never copied, since its `desc` views its `schema`.
- **`catalog_t`**, opaque and shared by sessions: every
  committed table by oid and by namespace and name, as
  `std::shared_ptr<const table_def_t>`; every namespace; every
  collation, opened by `cat_open` at its recorded ICU version
  and closed by `cat_close`. A reader-writer mutex guards it.
- **`session_t`**: an sm1 session, the catalog it reads, and its
  open transaction's **overlay** — the table definitions, table
  names and collations its DDL has changed, each tagged with the
  savepoint depth it was made at (0 before any). A lookup through
  the session finds its newest change first, then the shared
  catalog, so the transaction sees its own DDL and no other
  session sees it.
- **The count of publishes.** The catalog counts the commits
  that published DDL, and remembers for an index the count at
  which it became valid. A session notes the count before it
  takes a snapshot, and may scan an index only if it was valid
  by then (`SM3_DESIGN.md` §3). Neither is on disk: no
  snapshot survives a restart.
- **The builder**: the queue of indexes still building and
  the workers filling them (`SM3_DESIGN.md` §3,
  `src/catalog/build.cc`).

---

## 5. Calls

`cat_init` and `cat_open` take an sm1 session; the rest take an
sm3 session and work in its transaction. Each reports failure
as sm1 does (`DESIGN.md` §5).

- **`cat_init(session)`** makes a new database's catalog:
  `META`, the `sm3.oid` sequence, and the `pg_catalog` and
  `public` namespaces at their fixed oids. A database with a
  catalog fails with `SM_ERR_EXISTS`.
- **`cat_open(session, cat)`** reads `META` first — none is
  `SM_ERR_NOTFOUND`, another format `SM_ERR_VERSION` — then
  scans namespaces, collations and tables, one kind each, and
  builds every definition. A definition that contradicts itself
  — an unknown type or collation, an index on a column that is
  not there — is `SM_ERR_CORRUPT`.
- **`cat_close(cat)`** frees the catalog and its collations.
- **`cat_table(cat, oid)`** and **`cat_table(cat, nspoid,
  name)`** find a committed table, or nullptr.
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
  sm1's commit releases the schema locks, and a transaction
  that gets one then waits on the mutex to read the
  definition, so it sees the new one, never the old.
- **`cat_lock_table(ss, oid, mode, def)`** takes the table's
  schema lock — IS to read it, IX to write it — held to the end
  of the transaction, then sets `def` to its definition as the
  session sees it, which cannot change until then. A table
  dropped while the caller waited is `SM_ERR_NOTFOUND`.
- **`cat_table(ss, oid)`** and **`cat_table(ss, nspoid, name)`**
  find a table as the session's transaction sees it, and
  **`cat_tables(ss, nspoid)`** lists the names of its tables,
  sorted;
  **`cat_index(ss, nspoid, name, table, index)`** finds an index
  and its table, by a scan of the tables' indexes, since no map
  of index names is kept.
- **`prepare(ss, gid)`** prepares as sm1 does, but refuses a
  transaction that made DDL: its overlay would have to outlive
  the session.
- **`cat_create_table(session, nspoid, name, columns, oid)`**
  checks the name, the column names and each column's type
  code and typmod (`SM_ERR_INVALID`) and the namespace
  (`SM_ERR_NOTFOUND`); then takes an oid, inserts the name —
  first, so a taken one (`SM_ERR_DUPKEY`) fails before a store
  is made — makes the table's store and rowno sequence, and
  writes its blob.
- **`cat_drop_table(session, oid)`** erases the table's name,
  its indexes' names and its blob, and drops its store, which
  takes its rows and index entries with it, and its rowno
  sequence.
- **`cat_rename_table(session, oid, name)`** inserts the new
  name before erasing the old, so a taken one leaves the table
  as it was.
- **`cat_add_column(session, oid, col, missing)`** appends a
  column. A row written before it reads it as `missing` — its
  bytes, checked to be a datum of the column's type — or as
  null without one, so a `NOT NULL` column needs one.
- **`cat_drop_column(session, oid, name)`** marks the column
  dropped; it keeps its place and reads as null, and its name
  can be added again. One an index uses is refused.
- **`cat_create_index(session, table, name, keys, includes,
  unique, oid)`** records an index — its key columns with their
  direction, null order and collation, a collation only on
  text or varchar — and takes the table's next relid, which is
  never handed out again. It does not fill the index from the
  table's rows: it records it as building, for the build to
  fill and mark valid (`SM3_DESIGN.md` §3). On a table its own
  transaction made, which no other can see, it runs the build
  itself, over the rows the transaction has written, and
  records the index valid.
- **`cat_create_index(…, eval, reads, oid)`** is the same for
  a partial or an expression index: `eval` its blob, `reads`
  the columns it reads. It needs the catalog to have an
  evaluator, **`cat_set_evaluator(cat, eval)`**, given once.
- **`cat_set_index_state(session, table, oid, state, error,
  reason)`** rewrites an index's state, as the build does when
  it ends, with the error and its message for a failed one.
- **`cat_drop_index(session, table, oid)`** erases the index's
  name, its place in the blob, and its entries, one by one over
  its prefix in the table's store, since sm1 has no range
  delete.
- **`cat_create_collation(session, nspoid, name, locale,
  deterministic, oid)`** opens the collation, to check the
  locale and record the ICU version it opens at.

Every DDL on an existing table takes its schema lock X first,
so it waits out the table's readers and writers and they wait
for it, and then reads the table's entry under an X lock on its
`TABLE` key: at READ COMMITTED with `catkv_get_latest`, which
lets it overwrite a version newer than its snapshot; at
REPEATABLE READ the lock fails on one. `cat_create_index` takes
X too, and keeps it: the build is concurrent (§7).

A missing table, index, namespace or collation is
`SM_ERR_NOTFOUND`; a name taken in its namespace is
`SM_ERR_DUPKEY`. Each call writes the catalog in the session's
transaction and records the new definition in its overlay; a
collation it creates stays open there, for the transaction's
DDL to use at once.

---

## 6. Source

```
sm1/src/access.cc          store 0 under "U" for callers
sm2/src/rel/catkv.cc       the catalog key-value store
sm3/include/sm3.h          definitions and calls
sm3/src/catalog/catalog.h  keys, records and parts (internal)
sm3/src/catalog/internal.h catalog_t, session_t and the overlay
sm3/src/catalog/key.cc     keys
sm3/src/catalog/blob.cc    values: parts and their framing
sm3/src/catalog/load.cc    cat_open and the lookups
sm3/src/catalog/ddl.cc     cat_init and the DDL
sm3/src/catalog/session.cc the session, its transaction, publishing
sm3/test/test_layout.cc    keys and values
sm3/test/test_catalog.cc   init, load and create
sm3/test/test_ddl.cc       the rest of the DDL
sm3/test/test_overlay.cc   the overlay: visibility, rollback, abort
sm3/test/test_lock.cc      schema locks, and publishing under them
src/sm.cc                  the C API: sm_cat_*, a catalog per database
rel/test/test_catalog.rel  DDL through the C API, end to end
```

The C API (`include/sm.h`) keeps each database's catalog in its
`sm_env_t`, made by `sm_mkdb` and loaded by the first
`sm_opendb`, and an sm3 session in each `sm_session_t`, so its
transaction calls keep the overlay in step. `sm_cat_*` are the
calls above that `rel` needs: list the tables, look up a table
or index, lock a table and read its definition as plain C
structs, and create and drop tables and indexes. `rel` keeps no
catalog of its own.

---

## 7. Not built yet

Settled, and next:

- **Temporary tables.** A temporary table is a temporary store
  (`DESIGN.md` §5, `sm1/SM1_DESIGN.md` §11.3); its definition
  lives only in the session's own temporary catalog, in the
  session's temporary namespace, never written to store 0.

Open: namespaces beyond the two built in (`CREATE SCHEMA` is a
`NAMESPACE` blob and a `NAME` key; search paths are the SQL
layer's); dependencies, such as refusing `DROP COLLATION` in
use, by a scan of the table blobs until it earns a key kind; and
startup cost for thousands of tables, unmeasured.
