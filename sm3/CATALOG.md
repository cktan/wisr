# CATALOG — sm3, the catalog

sm3's catalog: where table, column, index, namespace and
collation definitions are kept, how they are laid out, and how
they come into memory. It fills in `SM3_DESIGN.md` §2, whose
decisions it follows: the catalog is held in memory, written
through to storage in the DDL's own transaction, and changed
under lock-based DDL, as in PostgreSQL.

**Status: built** — the store, its keys and values, `cat_init`,
`cat_open` and `cat_create_table`. §7 is what is settled but
not built yet.

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
| `catkv_insert` | a new key; `SM_ERR_DUPKEY` if taken | `sm1::insert` |
| `catkv_update`, `catkv_erase` | replace or remove a key; `SM_ERR_NOTFOUND` if missing | `sm1::update`, `sm1::erase` |
| `catkv_lock` | a shared or exclusive row lock | `sm1::lock` |
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

- **A table is one entry.** Its columns and indexes are in its
  blob, so every DDL on a table rewrites that one value.
- **Names are keys.** Every named object owns a `NAME` key, so a
  name taken in its namespace fails with sm1's `SM_ERR_DUPKEY`,
  and two sessions taking it at once meet in sm1's write-write
  conflict. A namespace's own name is in namespace 0, which is
  no namespace.
- **A table's rowno sequence is `sm3.rowno.<oid>`**, made with
  its store.

---

## 3. Values

A value is a sequence of **parts**, each an sm2 tuple under a
schema sm3 fixes for it, preceded by its length as a varint
(`src/catalog/blob.cc`):

```
table blob = table | column × natts | (index | key × nkey | include × ninclude) × nindex
```

| Part | Fields |
| --- | --- |
| meta | format version `int4` |
| name | kind `int2`, oid `int8`, table oid `int8` or null |
| table | namespace `int8`, name `text`, storeid `int8`, rowno sequence `text`, next index prefix `int2`, natts `int4`, nindex `int4` |
| column | name `text`, type code `int2`, typmod `int4`, not null `bool`, dropped `bool`, missing value `bytea` or null, collation oid `int8` or null |
| index | oid `int8`, name `text`, prefix `int2`, unique `bool`, nkey `int4`, ninclude `int4` |
| key | attnum `int4`, desc `bool`, nulls first `bool`, collation oid `int8` or null |
| include | attnum `int4` |
| namespace | name `text` |
| collation | name `text`, locale `text`, deterministic `bool`, ICU version `text` |

- **A part evolves as a tuple does.** A field appended to a
  part's schema reads as its missing value in a value written
  before it (`sm2/TUPLE.md` §2), so a value needs no version of
  its own. `META`'s version changes only if a key's layout, or
  the framing, does.
- **Type codes are sm2's frozen codes** (`sm2/TYPE.md` §3); code
  7, the retired `decimal`, is never written. A declared
  `decimal(p, s)` is stored as the sm2 type it maps to
  (`SM3_DESIGN.md` §2.1).
- **Column positions are 0-based**, in key and include parts as
  in memory.
- **A missing value is its bytes**, a varlena for a
  variable-width type, as sm2's `attribute_t` holds it.
- **The next index prefix is kept** in the table part, so a
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
- **`catalog_t`**, opaque: every table by oid and by namespace
  and name, as `std::shared_ptr<const table_def_t>`; every
  namespace; every collation, opened by `cat_open` at its
  recorded ICU version and closed by `cat_close`.

---

## 5. Calls

Every call takes the session and works in its transaction; each
reports failure as sm1 does (`DESIGN.md` §5).

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
  name)`** find a table, or nullptr.
- **`cat_create_table(session, nspoid, name, columns, oid)`**
  checks the name, the column names and each column's type
  code and typmod (`SM_ERR_INVALID`) and the namespace
  (`SM_ERR_NOTFOUND`); then takes an oid, inserts the name —
  first, so a taken one (`SM_ERR_DUPKEY`) fails before a store
  is made — makes the table's store and rowno sequence, and
  writes its blob. It writes the catalog only: a `catalog_t`
  already loaded does not see the table until the overlay of §7
  is built.

---

## 6. Source

```
sm1/src/access.cc          store 0 under "U" for callers
sm2/src/rel/catkv.cc       the catalog key-value store
sm3/include/sm3.h          definitions and calls
sm3/src/catalog/catalog.h  keys, records and parts (internal)
sm3/src/catalog/key.cc     keys
sm3/src/catalog/blob.cc    values: parts and their framing
sm3/src/catalog/load.cc    cat_open and the lookups
sm3/src/catalog/ddl.cc     cat_init and cat_create_table
sm3/test/test_layout.cc    keys and values
sm3/test/test_catalog.cc   init, load and create
```

---

## 7. Not built yet

Settled, and next in this order:

1. **The rest of the DDL** — drop table, add and drop column,
   rename, create and drop index, create collation — each
   rewriting its entries as `cat_create_table` writes them.
2. **The overlay.** A session's open transaction keeps the
   definitions it has changed, each tagged with the savepoint
   depth it was made at; its lookups see them first, a rollback
   to a savepoint drops those made past it, an abort drops all,
   and nobody else sees them. `sm3::savepoint`, `rollback`,
   `commit` and `abort` wrap sm1's to keep it in step.
3. **Schema locks and publishing.** A table's `TABLE` key is its
   schema lock (`SM3_DESIGN.md` §2): a user takes it shared
   before reading the definition, DDL takes it exclusive by
   rewriting the blob. At commit the session takes the shared
   catalog's mutex exclusive, commits in sm1 — which releases
   its locks — moves its overlay into the shared catalog, and
   releases the mutex; a transaction that got the lock in
   between waits on the mutex, so it sees the new definition,
   never the old. DDL in a prepared transaction is refused, since
   the overlay would have to outlive its session.
4. **Temporary tables.** A temporary table is a temporary store
   (`DESIGN.md` §5, `sm1/SM1_DESIGN.md` §11.3); its definition
   lives only in the session's own temporary catalog, in the
   session's temporary namespace, never written to store 0.
5. **`rel` on sm3.** The `rel` driver's own catalog
   (`rel/src/rel_catalog.cc`) gives way to this one.

Open: namespaces beyond the two built in (`CREATE SCHEMA` is a
`NAMESPACE` blob and a `NAME` key; search paths are the SQL
layer's); dependencies, such as refusing `DROP COLLATION` in
use, by a scan of the table blobs until it earns a key kind; and
startup cost for thousands of tables, unmeasured.
