# CATALOG — sm3, the catalog

The design of sm3's catalog: where table, column, index and
collation definitions live, how they are kept in memory, and
how DDL changes them. It fills in `SM3_DESIGN.md` §2, whose
decisions it takes as given: the catalog is held in memory,
written through to storage, and changed under lock-based DDL,
as in PostgreSQL.

**Status: steps 1 to 4 of §4.3 are built** — sm1's store 0
under `"U"` and object ids from 1000, sm2's `catkv_*`, the
catalog's keys and blobs, and `cat_init`, `cat_open` and
`cat_create_table` in `src/catalog/`; the rest is design. §1 compares the candidates, §2 picks one, §3 lists the
primitives, §4 sketches the build.

---

## 1. Candidates

Every candidate stores the same facts, and every one is loaded
into memory at startup and changed by DDL in the DDL's own
transaction. They differ in how the facts are laid out on
disk, which is the one format sm3 writes that lasts.

### A. PostgreSQL-shaped system tables

A handful of sm2 heap rels in a catalog store — a `pg_class`,
`pg_attribute`, `pg_index` and `pg_collation` of sm3's own —
one row per object, each keyed by `rowno`, with sm2 unique
indexes for lookup by name and by oid.

- Familiar, and every row is small: a column is a row, as in
  `pg_attribute`, so no row nears sm2's 64 KB limit.
- Uniqueness of names comes from a unique index's
  `SM_ERR_DUPKEY`.
- But the schema lock (`SM3_DESIGN.md` §2) is an `sm1::lock`
  on a table's catalog row, and a heap row's sm1 key is sm2's
  business — its prefix and `rowno` layout. sm3 would lock
  keys it is not meant to know, or sm2 would grow a
  row-locking call for this one use.
- Every catalog change is a heap write plus index writes, and
  every lookup table needs its own hard-coded schema at
  bootstrap.

### B. One definition blob per table

A plain sm1 store, keyed by table oid, each value one encoded
definition: the table, its columns, its indexes, in a format
of sm3's own. CockroachDB's table descriptors are this shape.

- Simplest to load and to lock: one key per table, which is
  also its schema lock.
- A value is an sm1 value, up to 64 MB, so width is no issue.
- But the format is bespoke — a second record format beside
  sm2's tuple, to version and to test — and every DDL, `ADD
  COLUMN` included, rewrites the whole blob. Names need
  separate keys for uniqueness anyway.

### C. A keyed catalog: one row per object

One sm1 store whose keys sm3 lays out itself — a kind, then
the object's identifying numbers or name, built with sm2's key
encoder — and whose values are sm2 tuples. A table, a column,
an index, an index key column and a name are each one row.

- Transactional with the DDL, and sm3 owns the keys, so a
  table's row is its schema-lock key, and a name row is a
  unique key that refuses a duplicate with `SM_ERR_DUPKEY`.
- Every row is one object and an sm2 tuple, so the format is
  sm2's and no row nears 64 KB.
- But a table is many rows, loaded by prefix scans and
  reassembled, and sm3 reaches past sm2 to sm1 for the store.

### D. PostgreSQL's heap catalogs, kept

Rejected already (`SM3_DESIGN.md` §2): the fork replaces the
heap, and a hybrid puts DDL atomicity on the seam between two
storage systems.

### E. The root db, with a blob per entity

sm1's root db (`sm1/SM1_DESIGN.md` §3.5), which already maps
database names to directories, exposed as a key-value store,
holding one blob per catalog entity.

- Bootstrap is free: the root exists from `sm1::init`. It
  matches a catalog that memory makes authoritative: a
  synchronous store, used for persistence only.
- But the root is not transactional — no MVCC, no row locks —
  and it is a different RocksDB from the database's. DDL
  writes its stores in one and its blob in the other, so the
  catalog gets a second commit point: a table rewrite that
  commits its new store and crashes before the root write is
  left pointing at a store its own transaction dropped. Every
  DDL needs an ordering and a sweep, as `sm1::mkdb` has.
- sm1's locks cannot reach a root key, so each table still
  needs a schema row in its own store.
- Stamping each blob with the sm1 transaction that wrote it,
  and settling it at load by that transaction's status, would
  restore one commit point — at the price of two new sm1
  primitives, a root key-value API and a transaction-status
  query.

### F. A catalog key-value store in the database, through sm2

Each database keeps its catalog in sm1's own catalog store,
store 0, under a key prefix of its own, and sm2 exposes it to
a session as a small transactional key-value interface. sm3
keeps one blob per entity — a table with its columns and
indexes, a collation, a namespace — and a key per name.

- One commit point: the catalog is rows of an ordinary store
  in the same database, so DDL writes its blob and its table's
  stores in one sm1 transaction, and rollback, savepoints and
  crash recovery come with it.
- A blob's key is an sm1 row, so the schema lock is the table
  blob's key: shared for its users, exclusive by the DDL that
  rewrites the blob.
- A blob is an sm1 value, up to 64 MB, so no definition nears
  a limit; a table is one read and one write.
- A blob is a sequence of sm2 tuples, so its format is sm2's
  (§2.3).
- sm2's layer is thin, and knows where the catalog is, so sm3
  never handles a storeid or a prefix for it.

---

## 2. Decision

**Decision: F, a catalog key-value store in the database,
through sm2, with a blob per entity.** It keeps C's single
commit point and transactional locks and uniqueness, and
adds what E wanted: one key per entity, loaded whole, with no
row-size limit. It needs no new sm1 primitive, only a key
prefix in store 0 that sm1 lets callers use (§2.1); the new
code is a thin layer in sm2 and a blob framing made of sm2
tuples. Over
C it gives up only fine-grained writes — `ADD COLUMN` rewrites
its table's blob — which DDL's rate makes free.

### 2.1 The store

- **The catalog is `"U"` rows in store 0.** Store 0 is sm1's
  own catalog, already split by key prefix: `"R"` rows are its
  stores and `"S"` rows its sequences (`sm1/SM1_DESIGN.md`
  §11). `"U"`, for the level above, is a third kind beside
  them. Store 0 exists in every database from birth, so the
  catalog needs no search and no transaction can lose it.
- **sm1 ignores `"U"` rows.** `sm1::lsstore` scans `["R",
  "S")` and the startup sweep reads `"R"` rows, so neither sees
  the catalog, and under SERIALIZABLE a catalog write never
  conflicts with an `lsstore`. The one change the catalog asks
  of sm1 is that callers may use store 0 under `"U"`, and only
  there.
- **sm1's object ids start at 1000.** Stores and sequences
  share one counter in sm1, which today hands out 1 first. It
  starts at 1000 instead, and 2 to 999 are reserved for fixed
  objects sm1 may make with a database later.
- **Oids come from an sm1 sequence**, `sm3.oid`, created by
  `cat_init` to start at 1000. Oids 1 to 999 are for built-in
  objects with fixed oids: `pg_catalog` is 1 and `public` is 2.
  `nextseq` is not transactional, so an aborted DDL leaves a
  gap, as PostgreSQL's oid counter does.

### 2.2 Keys and entities

Every key starts with a kind (`int2`), then the kind's own
columns, encoded ascending by sm2's `encode_key`. The kinds'
values are on disk, so frozen: `META` 1, `NAME` 2, `NAMESPACE`
3, `TABLE` 4, `COLLATION` 5.

| Kind | Key after the kind | Value |
| --- | --- | --- |
| `META` | — | format version `int4` |
| `NAME` | namespace oid `int8`, name `text` | kind `int2`, oid `int8`, table oid `int8` or null |
| `NAMESPACE` | oid `int8` | blob: the namespace |
| `TABLE` | oid `int8` | blob: the table, its columns, its indexes |
| `COLLATION` | oid `int8` | blob: the collation |

- **A table is one entity.** Its columns and indexes are in its
  blob, so every DDL on a table — `ADD COLUMN`, `CREATE INDEX`,
  `RENAME` — rewrites that one value, and that write is what
  takes the table's schema lock exclusive.
- **Names are keys.** A table, an index, a namespace and a
  collation each own a `NAME` key, whose value says what it
  names; an index's names its table too. A namespace's own name
  is in namespace 0, which is no namespace. `CREATE` inserts the
  key, `RENAME` erases one and inserts another, `DROP` erases
  it, so sm1 refuses a duplicate and orders concurrent
  creators.
- **Type codes are sm2's frozen codes** (`sm2/TYPE.md` §3);
  code 7, the retired `decimal`, is never written. A declared
  `decimal(p, s)` is stored as the sm2 type it maps to
  (`SM3_DESIGN.md` §2.1).
- **A table's rowno sequence is `sm3.rowno.<oid>`**, made
  with its store.
- **Column positions are 0-based**, in an index's key and
  include parts as in memory.
- **The next index prefix is persisted** in the table's blob,
  so a prefix is never reused, across restarts too
  (`SM3_DESIGN.md` §4).

### 2.3 A blob

A blob is a sequence of sm2 tuples, each preceded by its
length as a varint, as `natts` is written (`sm2/TUPLE.md` §2).
Each tuple is laid out by a schema sm3 hard-codes for its
part:

```
table blob = table | column × natts | (index | key × nkey | include × ninclude) × nindex
```

| Part | Fields |
| --- | --- |
| table | namespace `int8`, name `text`, storeid `int8`, rowno sequence `text`, next index prefix `int2`, natts `int4`, nindex `int4` |
| column | name `text`, type code `int2`, typmod `int4`, not null `bool`, dropped `bool`, missing value `bytea` or null, collation oid `int8` or null |
| index | oid `int8`, name `text`, prefix `int2`, unique `bool`, nkey `int4`, ninclude `int4` |
| key | attnum `int4`, desc `bool`, nulls first `bool`, collation oid `int8` or null |
| include | attnum `int4` |
| namespace | name `text` |
| collation | name `text`, locale `text`, deterministic `bool`, ICU version `text` |

- **Each part evolves as a tuple does.** A field appended to a
  part's schema reads as its missing value in a blob written
  before it (`sm2/TUPLE.md` §2), so a blob needs no version of
  its own. `META`'s version changes only if a key's layout, or
  the framing, does.
- **A missing value is its bytes**, a varlena for a
  variable-width type, as sm2's `attribute_t` holds it.
- **A blob part is an sm2 tuple**, so each is held to 64 KB
  (`sm2/TUPLE.md` §1); the blob, a sequence of them, is not.

### 2.4 Locks

- **A table's `TABLE` key is its schema lock**
  (`SM3_DESIGN.md` §2). A transaction that uses the table
  takes it shared before reading the definition; DDL takes it
  exclusive by rewriting the blob.
- A namespace's and a collation's key serve the same way for
  DDL on them.

### 2.5 Memory, and privacy until commit

This settles the open question in `SM3_DESIGN.md` §2.

- **The shared catalog** is immutable definitions behind a
  reader-writer mutex: a map from oid to a
  `std::shared_ptr<const table_def_t>`, and from name to oid.
  A change replaces a definition; it never edits one in place.
- **A session's overlay** holds the definitions its open
  transaction has changed, each tagged with the savepoint depth
  it was made at. A lookup checks the overlay first, so the
  transaction sees its own DDL. A rollback to a savepoint drops
  the entries made past it; an abort drops them all. Nobody
  else sees the overlay.
- **Publishing at commit** closes the window between sm1
  releasing the schema lock and memory changing:
  1. the committing session takes the catalog mutex exclusive;
  2. it commits in sm1, which releases its locks;
  3. it moves its overlay into the shared catalog;
  4. it releases the mutex.

  A transaction that gets the schema lock between 2 and 3 then
  takes the mutex shared to read the definition, so it waits
  for 4 and sees the new one. DDL commits are serialized by the
  mutex; they are rare. A commit that fails at 2 discards the
  overlay instead.
- **DDL in a prepared transaction is refused** in v1: `PREPARE`
  after DDL fails, since the overlay would have to outlive the
  session into whoever commits the prepared transaction.

### 2.6 Temporary tables

A temporary table is a temporary store (`DESIGN.md` §5,
`sm1/SM1_DESIGN.md` §11.3). Its definition is kept only in the
session's own temporary catalog, a map beside the overlay,
never written to the catalog store and never published. It is
in the session's temporary namespace, as PostgreSQL's
`pg_temp_N`, so its name can shadow no one else's. Closing the
session drops its temporary stores; a crash leaves them for
sm1's startup sweep.

---

## 3. Primitives

### 3.1 Changed in sm1

- Store 0 accepts a caller's `read`, `insert`, `update`,
  `erase`, `scan` and `lock` for keys under `"U"`, and refuses
  every other key, so sm1's own `"R"` and `"S"` rows stay its
  own.
- The object id counter's first value is 1000; 2 to 999 are
  never handed out.

Nothing else changes in sm1, and no primitive is added.

### 3.2 New in sm2: the catalog key-value store

In `sm2/include/sm2.h`, in the stack's error convention
(`DESIGN.md` §5), each a thin call on sm1 over store 0, its
key prefixed with `"U"`, in the session's transaction.

| Call | Does | Over |
| --- | --- | --- |
| `catkv_open(session, catkv_t*&)` | open the catalog, store 0 under `"U"` | — |
| `catkv_close(catkv_t*)` | release the handle | — |
| `catkv_get(kv, key, value)` | 0 with value, 1 if none; a view valid to the next call | `sm1::read` |
| `catkv_insert(kv, key, value)` | a new key; `SM_ERR_DUPKEY` if taken | `sm1::insert` |
| `catkv_update(kv, key, value)` | replace a key's value | `sm1::update` |
| `catkv_erase(kv, key)` | remove a key | `sm1::erase` |
| `catkv_lock(kv, key, mode)` | a shared or exclusive row lock | `sm1::lock` |
| `catkv_scan(kv, prefix, catkv_scan_t*&)`, `catkv_scannxt`, `catkv_scanend` | keys under prefix, in order | `sm1::scan`, `scannxt`, `scanend` |

### 3.3 What sm3 uses

| Need | Primitive |
| --- | --- |
| the catalog | sm2's `catkv_*` |
| catalog keys | `sm2::encode_key`, `sm2::decode_key` |
| blob parts | `sm2::tup_encode`, `tup_deform`, one `tup_desc_t` per part |
| collations | `sm2::collation_open`, with the recorded ICU version |
| a table's store; its drop | `sm1::mkstore`, `sm1::rmstore` |
| oids; a table's `rowno` sequence | `sm1::mkseq`, `sm1::nextseq` |
| statement undo | `sm1::savepoint`, `rollback`, `forget` |

### 3.4 What sm3 offers

In `sm3/include/sm3.h`. Every call takes the session; DDL
requires a transaction.

- **Lifecycle.** `cat_init(session)` writes `META`, makes the
  `sm3.oid` sequence, and the `pg_catalog` and `public`
  namespaces with their fixed oids. `cat_open(session,
  catalog_t*&)` loads everything with one scan; it checks the
  format version and opens every collation.
- **Lookup.** `cat_table(session, namespace, name, def)` and
  `cat_table_by_oid(session, oid, def)` take the schema lock
  shared, then hand back the definition —
  `std::shared_ptr<const table_def_t>`, with its `tup_desc_t`
  and its indexes' `key_column_t`s built.
- **DDL.** `cat_create_table`, `cat_drop_table`,
  `cat_add_column`, `cat_drop_column`, `cat_rename`,
  `cat_create_index`, `cat_drop_index`, `cat_create_collation`.
  Each writes its blob and name keys, and records the new
  definition in the overlay.
- **Transaction.** `sm3::savepoint`, `rollback`, `commit` and
  `abort` wrap sm1's, so the overlay follows the transaction;
  `commit` runs §2.5's publish.

---

## 4. Implementation sketch

### 4.1 Layout

```
sm2/
  src/rel/catkv.cc       the catalog key-value store of §3.2
  test/test_catkv.cc
sm3/
  include/sm3.h          the API of §3.4
  src/catalog/key.cc     catalog keys: kind, then encode_key
  src/catalog/blob.cc    each part's schema; blob framing
  src/catalog/load.cc    cat_open: one scan into memory
  src/catalog/ddl.cc     the DDL calls
  src/catalog/overlay.cc the overlay and the publish protocol
  test/test_catalog.cc
```

### 4.2 In memory

```cpp
struct column_def_t {
    std::string name;
    const sm2::type_t* type;
    int32_t typmod;
    bool not_null, dropped;
    std::optional<std::string> missing;
    const sm2::collation_t* collation;
};

struct index_def_t {
    uint64_t oid;
    std::string name;
    uint16_t prefix;
    bool unique;
    std::vector<sm2::key_column_t> key;      /* built at load */
    std::vector<int32_t> key_attnums, include_attnums;
    std::vector<sm2::attribute_t> include;
};

struct table_def_t {
    uint64_t oid, nspoid, storeid;
    std::string name, rowno_seq;
    uint16_t next_prefix;
    bool temp;
    std::vector<column_def_t> columns;
    std::vector<sm2::attribute_t> schema;    /* for sm2 */
    sm2::tup_desc_t desc;                    /* over schema */
    std::vector<index_def_t> indexes;
};
```

A definition is built whole from its blob and never changed;
DDL builds a new one from the old and writes its blob.
`schema` and `desc` are built with it, so a reader never lays
out a row itself.

### 4.3 Steps

Each step builds and passes its tests before the next.

1. **sm1's store 0 under `"U"`, and object ids.** Verify: a
   caller reads and writes `"U"` keys in store 0, in its
   transaction; any other key in store 0 is `SM_ERR_INVALID`;
   `lsstore` and the startup sweep are unmoved by `"U"` rows;
   the first store or sequence made gets 1000, and no object id
   between 1 and 999 is used.
2. **sm2's catalog key-value store.** Verify: a write rolls
   back with its transaction and its savepoint; a taken key is
   `SM_ERR_DUPKEY`; two sessions inserting one key, one wins;
   a shared lock blocks an update until released; the catalog
   is there after a restart.
3. **Keys and blobs.** Each part's schema and `tup_desc_t`;
   `key(kind, ...)` over `encode_key`; blob framing. Verify:
   every entity round trips; a blob written before a field was
   appended to a part reads it as missing.
4. **Init and load.** `cat_init`, `cat_open`, and
   `cat_create_table` enough to have something to load.
   Verify: a table made, the database closed and reopened,
   loads to the same definition.
5. **DDL and the overlay.** The remaining DDL, the overlay,
   savepoints. Verify: DDL rolls back with its transaction and
   its savepoint, the in-memory copy included; a transaction
   sees its own DDL and no other's.
6. **Locks and publish.** The schema lock on lookup, §2.5's
   publish. Verify, with two sessions: a transaction holding
   the shared lock never sees a definition change; DDL waits
   for it; a transaction that waited sees the new definition,
   never the old; concurrent `CREATE` of one name — one wins,
   one fails.
7. **Temporary tables.** Verify: invisible to another session,
   gone at session close, swept after a crash.
8. **`rel` on sm3.** The `rel` driver's own catalog
   (`rel/src/rel_catalog.cc`) gives way to sm3's, so the
   `.rel` suites exercise it, and survive a restart they
   cannot today.

### 4.4 Open

- **Namespaces beyond two.** v1 makes `pg_catalog` and
  `public`; `CREATE SCHEMA` is a `NAMESPACE` blob and a `NAME`
  key, but search paths are the SQL layer's.
- **Dependencies.** PostgreSQL's `pg_depend` — a column's
  collation, an index's columns — is implicit in the blobs;
  `DROP COLLATION` in use must still be refused, by a scan of
  the table blobs, until it earns a key of its own.
- **Startup cost** for thousands of tables is one scan and one
  `tup_desc_t` per table and index; unmeasured.
