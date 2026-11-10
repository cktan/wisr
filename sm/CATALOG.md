# CATALOG: Catalog

The Catalog holds sm3's definitions: namespaces, tables, columns,
indexes, collations, sequences, foreign keys and views, with
index states and table statistics. It maps each table onto sm1
and sm2. DDL changes it, and every reader of a table depends on
it. The code is in `sm3/src/catalog/` (keys, values, the load,
the puts and dels, a session and its overlay) and
`sm3/src/ddl/ddl.cc` (what a CREATE, a DROP or an ALTER does).
`sm3/include/sm3.h` declares the calls, and `include/sm.h` gives
them to the host as `sm_cat_*`. sm1's own catalog of stores is in
[`DBMGR.md`](DBMGR.md). [`NAMESPACE.md`](NAMESPACE.md) and
[`SEQUENCE.md`](SEQUENCE.md) cover the two
objects of those names.

---

## 1. Design

### 1.1 A worked example

A **definition** is a `table_def_t`: a table resolved for use,
with its layouts. A definition is built whole and never edited.
Each one is a **generation** of its table. Take this transaction
on a table `t`, whose definition is generation N:

```sql
BEGIN;
ALTER TABLE t ADD COLUMN c int;
SELECT * FROM t;
COMMIT;
```

1. `add_column` calls `cat_get_table_rec`, which takes t's
   table-oid lock X until the transaction ends. An open's IS
   conflicts with X, so no other transaction opens t.
2. `cat_get_table_rec` reads t's record from memory, not from the
   store.
3. `cat_put_table` writes one `COLUMN` key in store 0, in the
   transaction.
4. `build_table` builds generation N+1 with its layouts, into the
   session's overlay. The shared catalog does not change.
5. The `SELECT` opens t and finds N+1 in the overlay.
   `cat_table_version` gives `table_version_own`.
6. `commit` commits in sm1 and publishes under the Catalog's
   mutex (§5.2). The pointer to t now names N+1, and t's
   version dates from this publish.
7. A transaction that waited on t's lock reads the definition
   only once the mutex is free, so it sees N+1, never N.
8. Nothing marks or sweeps N. The Catalog drops its reference. N
   and its layouts go with their last holder, a handle not yet
   closed or a pin.

A commit that fails, or an abort, drops the overlay and N+1 with
it. N stays. So nothing built from a definition needs
invalidating. A host cache keyed by the version sees the new
version at its next look.

### 1.2 Decisions

- **The Catalog is held in memory.** `cat_open` loads it at
  startup. A change is made in memory and written through to
  store 0, which is used for persistence only. It replaces
  PostgreSQL's heap catalogs ([`PART4.md`](PART4.md)).
- **There is one commit point.** Store 0 is an MVCC store in the
  same database as the tables. DDL writes its catalog entries and
  its tables' stores in one sm1 transaction. They commit, roll
  back and survive a crash together, so sm1's status tuple is the
  one commit point. DDL is transactional for free.
- **Schema changes are lock-based, as in PostgreSQL.** The
  Catalog is not versioned, and an old snapshot does not see an
  old definition. PostgreSQL's catalogs are MVCC heap tables, but
  PostgreSQL does not give a transaction the old schema either.
  It keeps DDL and the users of a table apart with table locks. A
  transaction that first touches a table after a DDL commits sees
  the new definition, with its old data snapshot. sm3 does the
  same (§1.3).
- **One shared catalog needs no invalidation.** The fork is
  threaded, so one shared in-memory catalog is coherent with no
  cross-process messages. The cost is the code that reads
  catalogs: the relation cache, the syscache and system-table
  scans.
- **The fork rewrites the seam.** PostgreSQL hardwires its
  catalogs and sequences to heap. The fork replaces them, and
  uses sm1's sequences. Physical replication and PITR over
  PostgreSQL's WAL do not apply to data held in RocksDB
  ([`PART4.md`](PART4.md)).
- **The Catalog is a repository.** It stores the record it is
  given, finds it again, and forgets it. It makes no store and
  fills no index. It makes one sequence only, a sequence object's
  own counter. It checks only that its own records agree: a name
  is taken once, a table's namespace is there, a namespace that
  goes is empty.
- **DDL is a caller of the Catalog.** Each DDL call makes or
  drops what the object needs outside the Catalog, and puts or
  takes out the object's record (§8).
- **A catalog row stays small.** A catalog row is an sm2 tuple,
  held to 64 KB with no TOAST behind it
  ([`sm2/TUPLE.md`](sm2/TUPLE.md)). A table's definition in one
  row could pass that, with its columns' missing values. A key
  for each column, as PostgreSQL's `pg_attribute` has a row,
  keeps each row small.
- **The bootstrap needs no search.** Store 0 exists in every
  database from its birth, so no transaction can lose the
  Catalog. sm3 fixes the schema of each value part in code
  (§3.5), and `META` holds the layout's format version.
- **Type codes are sm2's frozen codes**
  ([`sm2/TYPE.md`](sm2/TYPE.md)). Code 7, the retired `decimal`,
  is never written.

### 1.3 The table-oid lock

The **table-oid lock** is an sm1 OID lock (`sm1::LOCKTYPE_OID`)
on the table's oid, in Gray's modes. It is what makes an
in-memory catalog safe.

- A transaction that reads a table takes `sm1::lock_oid` IS. One
  that writes it takes IX. PostgreSQL's AccessShareLock and
  RowExclusiveLock coexist in the same way.
- The lock is taken before the definition is read, and is held to
  the end of the transaction.
- DDL takes X. X waits out the transactions in flight and blocks
  new ones until the DDL commits.
- `create_index` is DDL. It takes X only to record the index as
  pending. The build then runs beside readers and writers, and
  sets the index's state under IX
  ([`INDEXBUILD.md`](INDEXBUILD.md)).
- Once a transaction holds IS or IX, the definition in memory
  cannot change under it. A Catalog served from memory must take
  this lock, or DDL has nothing to wait on.
- The lock names no store and nothing writes the name. So
  `sm1::lock_oid` reads no store, checks no version, and cannot
  fail on a newer commit at any isolation level. The lock alone
  protects the reader, and the reader uses the in-memory
  definition, not a read of a row.
- `cat_lock_table` takes the lock. sm1's detector finds deadlocks
  between DDL and DML like any others.

A view has the same lock on its oid ([`VIEW.md`](VIEW.md)), and a
namespace has one on its oid ([`NAMESPACE.md`](NAMESPACE.md)).
Every object draws its oid from one sequence, so one oid names
one object, with no kind byte ([`LOCKMGR.md`](LOCKMGR.md)).

### 1.4 The overlay

The **overlay** is a session's open transaction's own DDL: the
tables, names, collations, namespaces, index states, sequences,
statistics and views it changed. Each change is tagged with the
savepoint depth it was made at, 0 before any. A lookup through
the session finds its newest change first, then the shared
catalog. So the transaction sees its own DDL and no other session
sees it. **Publishing** moves the overlay into the shared catalog
when sm1 has committed (§5).

---

## 2. Object mapping

### 2.1 One store per table

**Decision: one sm1 store per table.** A store is a collection of
**rels**. A table's rows, each of its secondary indexes and its
TOAST data are one rel each. sm2's 2-byte **relid** tells them
apart.

| relid | Relation |
| --- | --- |
| `0x0000` | unused |
| `0x0001` | the table's rows, always |
| `0x0002`–`0xFFFD` | its indexes, one relid each; a primary key's is recorded and holds nothing |
| `0xFFFE` | reserved, unused: once the `_id` index's |
| `0xFFFF` | its TOAST data |

This table is the only place these values are written down. Every
other document points here for them.

- **The rows are always `0x0001`.** It is the constant
  `sm3::heap_relid`. The counter of index relids never hands it
  out. So a table's rows sit at the same relid in every database,
  and nothing about it is recorded.
- **`0xFFFE`, `sm2::id_relid`, is reserved.** There is no `_id`
  index: a keyed row has no `_id`, and a keyless row is keyed by
  it ([`ROWSTORE.md`](ROWSTORE.md)). No index takes the relid,
  and nothing is written under it.
- **`0x0000` is unused**, so a zeroed or unset relid is never a
  live rel.
- **A relid names a rel within its store and nowhere else.** A
  rel is reached as its store and its relid, and has no wider
  identity. sm3 chooses an index's relid and records it in the
  Catalog. The rows' and TOAST's are fixed.
- **sm2 prepends the relid and otherwise ignores it.** An sm3
  table or index is one `rel` over the table's store with its
  relid. sm2 never learns what a table is, and sm3 never encodes
  a key itself.
- **Index relids go up and are never reused.** The next relid is
  kept in the table part, across restarts too. A dropped index's
  versions linger until sm1's garbage collection passes them.
  Because no relid is reused, no later index can read them as its
  own, and no rule depends on the GC horizon. A table has 65,532
  index creations, rebuilds included: `0x0002` to `0xFFFD`.
- **Every table's store is chained.** `sm1::mkstore` is told so,
  for a temporary table too. Its one column family is
  `chain.<storeid>`. A row's newest version is under its plain
  key, with older ones in a chain behind it. Index entries and
  TOAST data, in the same store, stay versioned
  ([`ROWSTORE.md`](ROWSTORE.md)).
- **The cost: sm1's store count bounds the tables.** sm1 targets
  hundreds to low thousands of stores, and every table takes one,
  each partition included. This is accepted for v1. Raising the
  ceiling is sm1's problem if it bites.

### 2.2 What a store-level change does

- **`DROP TABLE` is one `sm1::rmstore`**, sm1's O(1) column
  family drop. It takes the indexes and TOAST data with it.
- **`TRUNCATE` swaps the store.** `truncate_table` makes a new,
  empty store, points the table's record at it, makes each tree
  index empty in it, and drops the old store with `sm1::rmstore`,
  under the table-oid lock X. All of it is transactional, so an
  abort, or a rollback to a savepoint before it, brings the rows
  back. The cost is the same whatever the table holds. The
  columns, the index definitions with their relids and states,
  and the `_id` sequence carry over, so new rows go on from the
  last `_id`. A transaction that began before the TRUNCATE
  committed and first reads the table after finds it empty, as in
  PostgreSQL: the Catalog is not versioned. On an open table the
  call is `tbl_truncate`, which needs the table opened exclusive
  and reopens the handle on the new store.
- **`DROP INDEX` erases the index's keys** one by one, as sm1 has
  no range delete. A tree index's pages go after the commit
  instead ([`BTREEINDEX.md`](BTREEINDEX.md)). Dropping a primary
  key keys the rows anew (below).
- **A rewrite** (`rewrite_table`) copies a table into a new
  store, under the table-oid lock X. It reopens every index in
  the new store, fills each one, puts the record, and drops the
  old store at commit, as `TRUNCATE` does
  ([`ROWSTORE.md`](ROWSTORE.md)).
- **Adding a primary key** to a table with rows rewrites it: each
  row is copied under its new key, losing its `_id`. An empty
  store only records the key.
- **Dropping a primary key** rewrites a table with rows into a
  store keyed by `_id`. The rows draw theirs from the table's
  sequence in key order.
- **Changing a column's type** (`alter_column_type`,
  [`../dg/ALTERTYPE.md`](../dg/ALTERTYPE.md)) rewrites a table
  with rows in the same way. The host's converter gives each
  row's value. Every index on the column is filled under the new
  type, and a key column's rows are keyed anew. A change whose
  values stay, the same type code or `text` and `varchar`,
  changes the record alone. A column that an expression or
  partial index, a foreign key or a view reads keeps its type.

### 2.3 A key in RocksDB

Each level puts its part in front of the one above. sm3 picks a
rel's relid and what its key encodes. sm2 puts the relid first
(`row_key()`). sm1 sees a storeid and a userkey, picks the
store's column family, puts its class byte first and, for a
versioned key, the version after (`sm1/src/version.h`,
`sm1/src/chain.h`):

```
RocksDB key  =  class (1B, sm1) ‖ userkey [‖ ~wts (8B) ‖ ~seq (4B), versioned]
userkey      =  relid (2B BE, sm2) ‖ the rel's key (sm3)
```

| What | Class | Key in RocksDB | Value |
| --- | --- | --- | --- |
| row's newest version, with a primary key | `0x01` current | `0x01 ‖ 0x0001 ‖ encode(pk)` | flags, xid or commit_ts, wts, seq, prev ‖ tuple |
| row's newest version, without one | `0x01` current | `0x01 ‖ 0x0001 ‖ be64(_id)` | as above |
| an older version of a row | `0x02` old | `0x02 ‖ be64(old id)`, no relid | the header ‖ displaced ‖ tuple |
| secondary index entry, not unique | `0x00` versioned | `0x00 ‖ relid ‖ encode(sk) ‖ encode(pk) ‖ ~wts ‖ ~seq` | flags, xid or commit_ts ‖ cut flag, INCLUDE columns, `encode(pk)` |
| secondary index entry, unique | `0x00` versioned | `0x00 ‖ relid ‖ encode(sk) ‖ ~wts ‖ ~seq` | as above |
| TOAST chunk | `0x00` versioned | `0x00 ‖ 0xFFFF ‖ id ‖ seq ‖ ~wts ‖ ~seq` | flags, xid or commit_ts ‖ the chunk |

A tree index's pages are not in this table:
[`BTREEINDEX.md`](BTREEINDEX.md) has their keys.

- **The class byte orders the column family.** The store's
  comparator, `wisr.store.v2`, sorts versioned keys by userkey
  then newest version first, and current and old keys bytewise.
  The three classes lie in three ranges, and a scan of rows
  bounds itself to one. [`ROWSTORE.md`](ROWSTORE.md) says why sm1
  keeps a byte of its own and does not read the relid.
- **A row's newest version is under its plain key**, so a scan of
  the newest rows reads one key a row. Index entries and TOAST
  keep the version in the key.
- **The bytes sort as the SQL key.** `encode()` is sm2's
  order-preserving encoding, `_id` is big-endian, and
  `~wts ‖ ~seq` puts the newest version first.
- **The Catalog is a store of its own**, versioned, outside every
  table's.

---

## 3. On disk

### 3.1 Where it lives

The Catalog is a key-value map in each database. It is in sm1's
own catalog store, store 0, under the key prefix `"U"`.

- **It sits beside sm1's own rows.** Store 0 is split by key
  prefix: `"R"` rows are sm1's stores and `"S"` rows its
  sequences ([`DBMGR.md`](DBMGR.md)). sm1 never reads `"U"` rows:
  `sm1::lsstore` scans `["R", "S")`, and the startup sweep reads
  `"R"` rows. sm1 lets a caller use store 0 under `"U"` only
  (`sm1::upper_prefix`), so neither can touch the other's rows.
- **Object ids.** sm1's stores and sequences share one counter,
  which starts at 1000; 1 to 999 are reserved. sm3's own oids
  come from the sequence `sm3.oid`, which `cat_init` makes to
  start at 16384 (`first_oid`), PostgreSQL's
  `FirstNormalObjectId`.
- **Oids below 16384 are built-in objects.** They are
  PostgreSQL's own numbers, since digres compiles its built-ins
  in with them ([`PART4.md`](PART4.md)): `pg_catalog` is 11 and
  `public` is 2200.
- **An aborted DDL leaves a gap**, as PostgreSQL's oid counter
  does: `nextseq` is not transactional.
- **Oids stop at `last_oid`, 2^32 - 1.** digres's oid is 64 bits,
  but for now it keeps PostgreSQL's 32-bit `Oid`, so a larger one
  would wrap onto a built-in one. Past the cap, `cat_new_oid`
  fails with `SM_ERR_INVALID`. The cap goes when digres widens
  `Oid`.

### 3.2 The catalog key-value store

sm3 reaches the Catalog through sm2's catalog key-value store,
`catkv_*` in `sm2/include/sm2.h` and `sm2/src/rel/catkv.cc`. Each
call is one sm1 call on store 0, in the session's transaction.
`"U"` is added to the key going in and stripped coming out.

| Call | Does | Over |
| --- | --- | --- |
| `catkv_open`, `catkv_close` | a handle on the session's catalog | — |
| `catkv_get` | 0 with the value, 1 if none; a view valid to the next call | `sm1::read` |
| `catkv_get_latest` | as `catkv_get`, the newest committed version, under a lock held | `sm1::read_latest` |
| `catkv_insert` | a new key; `SM_ERR_DUPKEY` if taken | `sm1::insert` |
| `catkv_update`, `catkv_erase` | replace or remove a key; `SM_ERR_NOTFOUND` if missing | `sm1::update`, `sm1::erase` |
| `catkv_lock` | a row lock in any of sm1's modes (IS, IX, S, SIX, X) | `sm1::lock_row` |
| `catkv_trylock` | as `catkv_lock`, never waiting: 1 if another holds it | `sm1::trylock_row` |
| `catkv_scan`, `catkv_scannxt`, `catkv_scanend` | the keys under a prefix, in order | `sm1::scan` |

### 3.3 Keys

Every key is a kind (`int2`), then the kind's own columns,
encoded ascending by sm2's `encode_key`
(`sm3/src/catalog/key.cc`). A kind's keys are contiguous, and
ordered by their columns. The kinds' values are on disk, so
frozen.

| Kind | Value | Key after the kind | Holds |
| --- | --- | --- | --- |
| `META` | 1 | — | the format version, `int4` |
| `NAME` | 2 | namespace oid `int8`, name `text` | what the name names: kind `int2`, oid `int8`, and an index's table oid `int8` |
| `NAMESPACE` | 3 | oid `int8` | the namespace's blob |
| `TABLE` | 4 | oid `int8` | the table's own fields |
| `COLLATION` | 5 | oid `int8` | the collation's blob |
| `INDEX` | 6 | table oid `int8`, index oid `int8` | one index of the table, with its key, include and read columns |
| — | 7 | — | retired: a table's oid lock was a key here |
| `COLUMN` | 8 | table oid `int8`, attnum `int8` | one column of the table |
| `INDEX_STATE` | 9 | table oid `int8`, index oid `int8` | one index's state, with why its build failed; none for a pending index |
| `STATS` | 10 | table oid `int8`, index oid `int8`, n `int8` | the table's statistics: an entry for the table, (0, 0), and one a statistic, a column's (0, attnum + 1), an index expression's (index oid, place + 1) |
| `SEQUENCE` | 11 | oid `int8` | a sequence: its namespace, name, start, increment and owner |
| `CONSTRAINT` | 12 | table oid `int8`, constraint oid `int8` | one constraint of the table: a foreign key, of its child ([`FOREIGNKEY.md`](FOREIGNKEY.md)), or a check ([`CHECK.md`](CHECK.md)) |
| `VIEW` | 13 | oid `int8` | a view: its namespace, name, the host's query and options, and the relations it reads ([`VIEW.md`](VIEW.md)) |

### 3.4 What the keys mean

- **A table is several entries.** Its own fields are under its
  `TABLE` key, each column under a `COLUMN` key, each index under
  an `INDEX` key. A table's columns are together in attnum order,
  and its indexes in oid order, the order they were made in.
- **A column is a part of its table and has no oid.** Its key is
  its table's oid and its attnum, its place, which it keeps for
  good. A dropped column's key stays.
- **An index is an object.** It is keyed by its table's oid and
  its own.
- **DDL writes the entries it changes.** Adding a column writes
  one `COLUMN` key, and a build that ends writes one
  `INDEX_STATE` key. Two DDL on one table may then write
  different keys and meet in no write-write conflict. The
  table-oid lock puts them one after the other.
- **An index's state is not in its entry.** It has an entry of
  its own, `INDEX_STATE`, written with no change to the table and
  under the table's lock IX, not X (§7.3). A pending index has
  none: no entry is what pending means, so only valid and failed
  are stored. The state goes with its index.
- **Names are keys.** Every named object owns a `NAME` key, so a
  taken name fails with sm1's `SM_ERR_DUPKEY`, and two sessions
  taking it at once meet in sm1's write-write conflict
  ([`NAMESPACE.md`](NAMESPACE.md)).
- **A table has two sequences.** `sm3.id.<oid>` hands out its
  `_id`s, and `sm3.toast.<oid>` its TOAST ids. Both are made with
  its store ([`SEQUENCE.md`](SEQUENCE.md)).
- **A view's query is in pieces**, its parts in order. A part is
  an sm2 tuple, at most 65,535 bytes, and a query may be longer:
  SQLite's corpus has one of 100 KB.
- **A view is one entry**, its reads in its value, and its name a
  `NAME` key in its namespace, shared with the relations there.
  What reads a relation is found by a pass over the views in
  memory, and stored nowhere ([`VIEW.md`](VIEW.md)).

### 3.5 Values

A value is a sequence of **parts**, each an sm2 tuple under a
schema sm3 fixes for it, preceded by its length as a varint
(`sm3/src/catalog/blob.cc`):

```
TABLE value       = table
COLUMN value      = column
INDEX value       = index | key × nkey | include × ninclude | read × nread
INDEX_STATE value = index state
SEQUENCE value    = sequence
CONSTRAINT value  = constraint | fkey | fkey pair × npairs
                  | constraint | check | check read × nread
VIEW value        = view | view read × nread | view query × npiece
STATS value       = table stats | index stats × nindex
                  | statistic | mcv × nmcv | bound × nbound
```

A `STATS` entry is the table's own, the first line, or one
statistic's, the second. Their parts are in
[`sm3/SM3_DESIGN.md`](sm3/SM3_DESIGN.md),
under "What is recorded".

| Part | Fields |
| --- | --- |
| meta | format version `int4` |
| name | kind `int2`, oid `int8`, table oid `int8` or null |
| table | namespace `int8`, name `text`, storeid `int8`, `_id` sequence `text`, next index relid `int2` |
| column | name `text`, type code `int2`, typmod `int4`, not null `bool`, dropped `bool`, missing value `bytea` or null, collation oid `int8` or null, default `bytea` or null: the host's expression, opaque, kept and never run; identity kind `int2` (`a` or `d`) or null, identity sequence `int8` or null, update default `bytea` or null: the host's expression for what an UPDATE that does not assign the column writes, opaque, kept and never run ([`../dg/ONUPDATE.md`](../dg/ONUPDATE.md)); NOT NULL constraint oid `int8` or null, and its name `text` or null, both set for a NOT NULL column ([`NOTNULL.md`](NOTNULL.md)); generated kind `int2` (`s` or `v`) or null, and the columns its expression reads `bytea` or null: each a 0-based place, 4 bytes in the host's byte order, both set for a generated column, whose default is its expression ([`GENERATED.md`](GENERATED.md)) |
| index | oid `int8`, name `text`, relid `int2`, unique `bool`, nkey `int4`, ninclude `int4`, three retired and null, eval `bytea` or null, nread `int4` or null, primary `bool` or null, constraint `bool` or null, tree `bool` or null: true, or null for false |
| index state | state `int2`, error `int2` or null, reason `text` or null, the key of a row that holds a duplicated key, `bytea` or null |
| key | attnum `int4`, desc `bool`, nulls first `bool`, collation oid `int8` or null, type code `int2` or null, typmod `int4` or null |
| include | attnum `int4` |
| read | attnum `int4`, laid out as an include part is |
| namespace | name `text` |
| collation | name `text`, locale `text`, deterministic `bool`, ICU version `text` |
| sequence | namespace `int8`, name `text`, start `int8`, increment `int8`, owner table `int8` or null, owner column `int4` or null |
| constraint | oid `int8`, name `text`, type `int2` (`f` a foreign key, `c` a check), validated `bool`, host `bytea` or null |
| fkey | parent table `int8`, parent index `int8`, npairs `int4`, on update `int2`, on delete `int2`, match `int2`, deferrable `bool`, deferred `bool` |
| fkey pair | child attnum `int4`, parent attnum `int4` |
| check | nread `int4`: the columns its expression reads ([`CHECK.md`](CHECK.md)) |
| check read | attnum `int4` |
| view | namespace `int8`, name `text`, options `bytea` or null: the host's; nread `int4`, npiece `int4` |
| view read | relation `int8`, attnum `int4`: a table's column, 0-based, or -1 for the relation whole |
| view query | a piece of the host's query, opaque, `bytea` of at most 60,000 bytes |

### 3.6 What the values mean

- **The format version is 4** (`format_version`). `META`'s
  version changes only if a key's layout or the framing does. It
  was 2 when a table's columns and indexes left its blob for keys
  of their own, 3 when an index's state left the index part, and
  is 4 since the built-in namespaces took PostgreSQL's oids.
- **A part evolves as a tuple does.** A field appended to a
  part's schema reads as its missing value in a value written
  before it ([`sm2/TUPLE.md`](sm2/TUPLE.md)), so a value needs no
  version of its own. A new kind, as `SEQUENCE` was, changes no
  key's layout and leaves the version as it is.
- **An index's state** is 0 pending, 1 valid or 2 failed, in the
  index state part. The index part's three attributes that once
  held it are retired and left null.
- **A partial or an expression index keeps a blob.** Its eval is
  its predicate and its expressions as the host serialized them,
  bytes sm3 does not read. Its read parts are the columns they
  read. A plain index has neither.
- **A key that is an expression** has attnum -1 and gives its own
  type code and typmod, which a column's key leaves null.
- **A failed index keeps why**, in its state: its error is the
  code its build failed with, and its reason that error's
  message. Both are null in any other state.
- **Column positions are 0-based**, in key and include parts as
  in memory.
- **A missing value is its bytes**, a varlena for a
  variable-width type, as sm2's `attribute_t` holds it.
- **Each part is held to 64 KB**
  ([`sm2/TUPLE.md`](sm2/TUPLE.md)). An index's
  value, a sequence of parts, is not, and is an sm1 value of up
  to 64 MB.
- **Corruption fails.** A value cut short, with bytes past its
  last part, or with a required field null, is `SM_ERR_CORRUPT`.

---

## 4. In memory

### 4.1 Definitions

`sm3/include/sm3.h` declares what `cat_open` builds.

- **`table_rec_t`** is a **record**: the table as the Catalog
  keeps it, plain values with types and collations by code and
  oid.
- **`table_def_t`** is the same table resolved for use, and
  carries the record it was built from. It holds the table's oid,
  namespace, name, storeid, `_id` sequence, and the name of its
  TOAST sequence, so an open makes no name. It holds its
  `column_def_t`s and `index_def_t`s.
- **A column** has its sm2 type, typmod, flags, missing value and
  collation.
- **An index** has its `key_column_t`s and INCLUDE attributes
  built for sm2, and its entries' value laid out
  (`sm2::idx_layout_t`).
- **The table's sm2 `schema` and `tup_desc_t`** are built with
  the definition, so a reader never lays out a row itself. Every
  open of the table borrows these (`rel_open_desc`,
  `idx_open_layout`), held alive by the definition the handle
  holds. A generation's layouts go with it.
- **A definition is never copied**, since its `desc` views its
  `schema`.

### 4.2 The shared catalog

**`catalog_t`** is opaque and shared by sessions. A reader-writer
mutex, `mu`, guards its maps. A change replaces a definition and
never edits one in place. It holds:

- Every committed table, by oid and by namespace and name, as
  `std::shared_ptr<const table_def_t>`.
- A map of committed indexes' names, by namespace and name, built
  at load and kept at each publish.
- Every namespace's name, by oid
  ([`NAMESPACE.md`](NAMESPACE.md)).
- Every collation, opened by `cat_open` at its recorded ICU
  version and closed by `cat_close`, and the namespace of each.
- Each sequence's namespace and name by its oid, and its oid by
  them ([`SEQUENCE.md`](SEQUENCE.md)).
- Every view, by oid and by namespace and name, and the register
  of every session's temporary views ([`VIEW.md`](VIEW.md)).
- Each index's state, by index oid (§4.5).
- Each table's statistics, loaded on first use and kept:
  `nullptr` if it was never analyzed, no entry if not yet asked
  about.
- The counts of §4.4.
- The host's evaluator of partial and expression indexes, and
  whether the host reads TOAST pointers.
- The btree reaper, once attached, and a count of transactions
  begun, its measure of busy ([`BTREEINDEX.md`](BTREEINDEX.md)).
- The environment and database name, for a load of its own.

The Index Builder's queue and workers are not here: they are the
process's, not a Catalog's ([`INDEXBUILD.md`](INDEXBUILD.md)).

### 4.3 A session

**`session_t`** is an sm1 session, the Catalog it reads, the open
transaction's overlay (§1.4), and the session's temporary
catalog.

**The temporary catalog** holds the definitions of the session's
temporary tables, their names, which are in no namespace, their
indexes' states, and its temporary views
([`TEMPTABLE.md`](TEMPTABLE.md), [`VIEW.md`](VIEW.md)).

- A change to a temporary table is made in the overlay like any
  other, marked as temporary.
- `commit` moves it to the temporary catalog, not the shared one,
  and takes the mutex only if something else is to be published.
- A lookup through the session finds the overlay first, then the
  temporary catalog, then the shared catalog.
- `cat_put_table`, `cat_del_table` and `cat_put_index_state`
  write nothing to the store for a temporary table and take no
  lock. `cat_lock_table` takes none.

### 4.4 The counts

All these counts are in memory and start at 0 at each load. No
snapshot and no host cache survives a restart.

- **The count of publishes** (`catalog_t::seq`, `cat_publishes`)
  is how many commits published to the shared catalog: DDL, an
  analyze's statistics, an index made valid. The Catalog
  remembers for an index the count at which it became valid
  (`valid_seq`). A session notes the count before it takes a
  snapshot, and may scan an index only if it was valid by then
  (`index_usable`, [`INDEXBUILD.md`](INDEXBUILD.md)).
- **A table's version** (`cat_table_version`) comes from the same
  count. The Catalog remembers for each table the count at which
  its definition, and its statistics, were last published. The
  version is the greatest of those and its indexes' counts of
  becoming valid. It is `table_version_own` while the session's
  own transaction has changed the definition or the statistics. A
  table not changed since the load is at 0.
- **A view's version** (`cat_view_version`) works the same way.
- **The namespaces' epoch** (`cat_namespace_epoch`) is the count
  at which a namespace was last made or dropped
  ([`NAMESPACE.md`](NAMESPACE.md)).
- **The catalog's epoch** (`cat_epoch`) is the count of publishes
  plus the count of changes to the session's temporary catalog.
  It moves whenever what the session can look up does. It is
  `epoch_own` while the session's transaction has DDL of its own,
  which no epoch dates. A host that caches what it looks up reads
  it before each lookup, and keeps an answer while it reads the
  same ([`../dg/SCHEMACACHE.md`](../dg/SCHEMACACHE.md)).

A host that caches what it learns of a table under the table-oid
lock (`sm_table_def_t::version`) knows from the version whether
the cache is good, with no word from the session that changed it
([`../dg/SHMEM.md`](../dg/SHMEM.md)). Read under IS or IX, the
version changes only by an analyze or an index made valid, since
DDL takes X.

### 4.5 Index states

Each index's state is in a map by index oid, beside the
definitions and not in them. A definition is immutable and is
replaced whole, under the table's lock X. A state changes under
IX, while transactions hold the definition, and two indexes of
one table may change at once. An overlay holds the states its
transaction has set, and `commit` publishes them with everything
else, under the mutex, so the map and the store never differ.

---

## 5. Transactions and publishing

### 5.1 Session calls

`session_open(cat, s, ss)` opens a session over the sm1 session
`s`. `session_close` drops its overlay and its pins, and takes
its temporary views out of the register. The transaction calls
wrap sm1's and keep the overlay in step.

- **`begin`** notes the count of publishes before sm1 takes the
  snapshot: what was published by then, the snapshot sees whole.
  It empties the overlay and drops the temporary views another
  session has doomed ([`VIEW.md`](VIEW.md)).
- **`stmt_begin`** does the same for a READ COMMITTED statement,
  the only kind that takes a snapshot of its own.
  `stmt_begin(s, outer)` and `stmt_end(s, outer)` keep the outer
  statement's count with its snapshot. `stmt_advance` moves sm1's
  mark and changes no count.
- **`savepoint`** sets the depth that later changes are tagged
  with.
- **`rollback(d)`** drops the changes tagged `d` or deeper, since
  the mark at `d` survives.
- **`forget(d)`** re-tags them `d - 1`, so a later mark that
  reuses the depth cannot drop them.
- **`abort`** releases the pins and drops the whole overlay.
- **`commit`** publishes (§5.2).
- **`prepare`** prepares as sm1 does, but refuses a transaction
  that made DDL, `SM_ERR_INVALID`: its overlay would have to
  outlive the session.

### 5.2 Commit

With shared changes to publish, `commit` holds the mutex across
sm1's commit and the publish:

1. Release the transaction's pins.
2. Take the mutex exclusive. Readers of the Catalog wait from
   here.
3. Commit in sm1, which releases the table-oid locks. A failure
   drops the overlay and returns.
4. Move the temporary changes to the temporary catalog
   (`publish_temp`).
5. Publish the rest (`publish`), then release the mutex.

A transaction that gets a table-oid lock at the sm1 commit waits
on the mutex to read the definition. So it sees the new
definition, never the old.

With no shared change, `commit` takes no mutex. It commits in sm1
and moves the temporary changes. A change to a temporary view
counts as shared, as it goes in the register of temporary
readers.

`publish` uses one new count for all it changes:

1. Add the collations.
2. Apply the namespaces. If any changed, set the namespaces'
   epoch to the new count.
3. Apply the sequences.
4. Apply the statistics, dating each table's.
5. Apply the index states. An index valid now and not before
   dates its `valid_seq` from this publish. An index that went
   loses its count.
6. Apply the tables: free the old definition's index names, add
   the new one's, and date the table. 6a. Apply the foreign keys
   marked valid (§7.4) to the tables now in the catalog, and date
   each table.
7. Apply the views. A drop of another session's temporary view
   dooms it for that session's next `begin`.
8. Store the new count, after the namespaces' epoch: one who
   reads the count moved reads the epoch moved too.
9. Apply the table names.

---

## 6. Lookups

`cat_init` and `cat_open` need no session. Each names its
database by environment and name, opens an sm1 session of its
own, works in a READ COMMITTED transaction of its own
(`in_own_txn`), and closes the session. The other calls take a
session or a catalog. Each reports failure as sm1 does.

### 6.1 Making and loading

- **`cat_init(env, dbname, opt)`** makes a new database's
  Catalog, whole or not at all: `META`, the sequences `sm3.oid`
  and `sm3.tree_page`, and the `pg_catalog` and `public`
  namespaces at their fixed oids. `opt` is the options its
  session opens the database with, as `sm1::opendb` takes them. A
  database with a Catalog fails with `SM_ERR_EXISTS`.
- **`cat_open(env, dbname, cat)`** loads a Catalog (below).
- **`cat_close(cat)`** frees the Catalog and its collations. Its
  caller first detaches a Catalog attached to the Index Builder.
- **`cat_set_evaluator(cat, eval)`** gives the Catalog the host's
  evaluator, once. A second call is `SM_ERR_INVALID`.
- **`cat_set_toast(cat, on)`** says whether the host reads TOAST
  pointers, and so whether a table's long datums are stored out
  of line ([`TOAST.md`](TOAST.md)).

`cat_open` reads in this order:

1. Read `META`. None is `SM_ERR_NOTFOUND`, another format
   `SM_ERR_VERSION`.
2. Scan the namespaces.
3. Scan the collations, and open each at its recorded ICU
   version. A collation whose version differs fails as sm2
   refuses it.
4. Scan the names, for each collation's and sequence's namespace,
   which only a `NAME` key records for a collation.
5. Scan the tables, columns, indexes and constraints, one kind
   each, and put each table's record together.
6. Derive each table's `referenced_by` from the foreign keys.
7. Scan the index states.
8. Build every definition, and the maps of table and index names.
9. Scan the views.

Statistics are not read: each table's are loaded when first asked
for. A definition that contradicts itself is `SM_ERR_CORRUPT`: an
unknown type or collation, an index on a column that is not
there, a foreign key to no table.

### 6.2 Committed lookups

These need no session, and see nothing a transaction has not
committed.

- **`cat_get_table(cat, oid)`** and **`cat_get_table(cat, nspoid,
  name)`** find a table, or nullptr.
- **`cat_list_tables(cat, nspoid)`** lists the names of a
  namespace's tables, sorted.
- **`cat_get_table_by_store(cat, storeid)`** finds the table
  whose rows are in a store, for a lock's table
  (`sm1::lock_status`), by a scan.
- **`cat_get_namespace(cat, name, oid)`** and
  **`cat_list_namespaces(cat)`** do the same for namespaces.
- **`cat_get_index_state(cat, index_oid)`** gives an index's
  committed state, with the error and reason of a failed one. An
  index with none recorded is pending.

### 6.3 A session's lookups

- **`cat_lock_table(ss, oid, mode, def, fresh)`** takes the
  table's oid lock, held to the end of the transaction, then sets
  `def` to its definition as the session sees it, which cannot
  change until then. A table dropped while the caller waited is
  `SM_ERR_NOTFOUND`. `fresh` says the shared catalog has no
  definition of it: the transaction made it, or it is the
  session's temporary table, and no other transaction sees its
  rows.
- **`cat_trylock_table`** is the same, never waiting: 1, with
  nothing taken, if another transaction holds the lock in a mode
  that conflicts.
- **`cat_get_table(ss, oid)`**, **`cat_get_table(ss, nspoid,
  name)`**, **`cat_list_tables(ss, nspoid)`** and
  **`cat_get_index(ss, nspoid, name, table, index)`** look up as
  the session's transaction sees it
  ([`NAMESPACE.md`](NAMESPACE.md)).
- **`cat_get_index_state(ss, index_oid)`** gives an index's state
  as the session's transaction sees it.
- **`cat_get_namespace`**, **`cat_get_namespace_name`** and
  **`cat_list_namespaces`** are in
  [`NAMESPACE.md`](NAMESPACE.md).
- **`cat_get_sequence`**, **`cat_get_sequence_name`**,
  **`cat_list_sequences`** and **`cat_get_sequence_def`** are in
  [`SEQUENCE.md`](SEQUENCE.md).
- **`cat_lock_view(ss, oid, mode, def)`** takes a view's oid lock
  and gives its definition, as `cat_lock_table` does a table's. A
  temporary view takes none.
- **`cat_get_view(ss, oid)`** and **`cat_get_view(ss, nspoid,
  name, oid)`** find one with no lock, **`cat_list_views(ss,
  nspoid)`** lists a namespace's, and **`cat_view_readers(ss,
  oid)`** gives the views that read a relation, other sessions'
  temporary ones among them ([`VIEW.md`](VIEW.md)).
- **`cat_get_fkey(ss, oid, table, def)`** finds a foreign key and
  its table, the child, with no lock.
- **`cat_get_stats(ss, oid, out)`** gives a table's statistics as
  the session sees them: its transaction's own, else the
  committed ones. The committed ones are loaded in a READ
  COMMITTED transaction of their own on first use and then kept,
  so a plan takes no lock and reads nothing. Statistics typed by
  a definition the transaction changed stay its own and are not
  kept.

---

## 7. Puts and dels

The Catalog's writes are few, and named for what a repository
does. Each writes in the session's transaction and records the
change in its overlay. A missing table, namespace or collation is
`SM_ERR_NOTFOUND`.

### 7.1 Tables

- **`cat_new_oid(ss, oid)`** takes the next oid from the sequence
  `sm3.oid`, and fails past `last_oid`.
- **`cat_get_table_rec(ss, table_oid, rec)`** reads a table's
  record in order to change it. It takes the table's oid lock X,
  so it waits out the table's readers and writers and they wait
  for it. It then takes the record from memory, which has every
  commit by the time the lock is granted. It does not read the
  store: the snapshot may be older than a column another has just
  added, and DDL must change the table as it now is.
- **`cat_put_table(ss, table_oid, rec)`** stores a table's record
  (below).
- **`cat_del_table(ss, table_oid)`** erases a table's record, the
  names it took, its index states and its statistics. Its store
  and its sequences are the caller's to drop.

`cat_put_table` of a table that is not there adds it:

1. Require the namespace, `SM_ERR_NOTFOUND` if none, and lock it
   shared against a drop.
2. Insert the table's name and each index's. A taken name fails
   before the record is written.
3. Write the table's own fields, each column, each index and each
   foreign key.
4. Build the definition into the overlay.

`cat_put_table` of a table that is there replaces it, under its
table-oid lock X:

1. Refuse a record with fewer columns, or in another namespace. A
   column cannot be taken out, only marked dropped.
2. Insert the names the record newly takes. One another object
   has is `SM_ERR_DUPKEY`, and leaves the table as it was.
3. Erase the names no longer in the record. Steps 2 and 3 are all
   a rename is, and all that naming a new index is.
4. Write only the entries that differ from the record in memory:
   the table's own fields, a column, an index, a foreign key.
5. Erase the key of an index or foreign key no longer there, with
   the index's state and its expressions' statistics.
6. Erase the statistics of a column dropped or of a changed type.
   A type change also forgets the table's statistics in memory at
   commit, to load them again.
7. Build the definition into the overlay.

The indexes in a record stay in the order of their oids, the
order they load in. Two of a table's objects cannot share a name.

Each entry is written under an X lock on its key, over its newest
committed version. At READ COMMITTED a `catkv_get_latest` lets
the write through. At REPEATABLE READ the lock fails on a version
newer than the snapshot.

### 7.2 Other objects

- **`cat_put_namespace`** and **`cat_del_namespace`** are in
  [`NAMESPACE.md`](NAMESPACE.md).
- **`cat_put_sequence`** and **`cat_del_sequence`** are in
  [`SEQUENCE.md`](SEQUENCE.md).
- **`cat_put_collation(ss, collation_oid, namespace_oid, name,
  locale, deterministic)`** stores a new collation. The Catalog
  opens it, to check the locale and record the ICU version it
  opens at, and keeps it open for definitions to point at. It
  stays open in the overlay, for the transaction's DDL to use at
  once. There is no del of a collation.
- **`cat_put_stats(ss, table_oid, stats)`** records a table's
  statistics, replacing any it had. It takes the table's oid lock
  IS, then an X lock on its `STATS` entry, so two analyzes of one
  table queue. Each statistic must name a column or an index
  expression of the table, and its values must be of that one's
  type (`SM_ERR_INVALID`). A temporary table's are refused.

### 7.3 Index states

- **`cat_put_index_state(ss, table_oid, index_oid, status)`**
  stores an index's state. It takes the table's lock IX, as a
  writer of the table does, so it waits for no reader or writer
  and none waits for it. Only DDL on the table is kept out, which
  stops the index being dropped under it.
- **IX is enough** because a state seen late is seen as pending,
  and pending is always safe: the index is maintained and not
  scanned. A table opened before the change goes on with the
  state it saw (`tbl_index_state`). The count of publishes keeps
  a snapshot older than the index's last entries off it, whether
  or not its transaction had opened the table.
- **A state is set once.** The argument above rests on a state
  only ever leaving pending. An index that is valid or failed is
  `SM_ERR_INVALID` to set again. Of two transactions setting one
  index's state, the second waits for the first on the key's lock
  and is then refused. Setting a pending index pending changes
  nothing.
- **Erasing an index erases its state**, in `cat_put_table` or
  `cat_del_table`.

### 7.4 A foreign key marked valid

- **`cat_validate_fkey(ss, table_oid, fkey_oid)`** sets a foreign
  key's `validated` flag. It takes the table's lock IX and locks
  the key's own record X, so the table's writers go on and two
  validations of one key meet. It writes the key's record only.
- **IX is enough** because nothing reads the flag but DDL and
  `pg_constraint.convalidated`, the key is enforced whatever it
  says, and it only goes from false to true. A session that sees
  it late is only careful.
- **It is published at commit** into the definition then in the
  catalog (§5.2, step 6a), not into the one the session read: of
  two transactions validating two keys, the second would
  otherwise undo the first in memory. The table's version moves.
- **The session sees it at commit.** Until then its own reads of
  the table show the flag unset.
- **A table the transaction changed, or a temporary table,**
  takes `cat_put_table` and the table X, as any DDL does.
  `latest_table` adds the keys this transaction marked to the
  record it returns, so a later DDL keeps them.

---

## 8. DDL

The DDL is in `sm3/src/ddl/ddl.cc`. Each call is one statement's
work. Every call that changes a table begins with
`cat_get_table_rec` and ends with `cat_put_table`, so it holds
the table's oid lock X to the end of its transaction.

### 8.1 Tables

- **`create_table(session, nspoid, name, columns, oid)`** checks
  the name, the column names, and each column's type code and
  typmod (`SM_ERR_INVALID`). It takes an oid, makes the table's
  store, its `_id` sequence and its TOAST sequence, and puts its
  record. If the put fails, a taken name or no such namespace, it
  drops the store and the sequences again, so a transaction that
  goes on leaves none behind. After a retryable error the
  transaction has been aborted, and they have gone with it.
  `nsp_temp` is `SM_ERR_NOTFOUND`.
- **`create_temp_table(session, name, columns, oid)`** is
  `create_table` in no namespace: a temporary store and
  sequences, and a definition in the session's temporary catalog
  alone ([`TEMPTABLE.md`](TEMPTABLE.md)). An identity column is
  `SM_ERR_INVALID`, as no sequence is temporary at sm3.
- **`drop_table(session, oid)`** dels the table's record and
  drops its store, its sequences, and the sequences its columns
  own. A table a view reads, or another table's foreign key
  references, is `SM_ERR_INUSE`. Its own foreign keys leave their
  parents' `referenced_by`.
- **`rename_table(session, oid, name)`** puts the record back
  under the new name. The put takes the new name before it frees
  the old.
- **`truncate_table(session, oid)`** empties the table (§2.2).

### 8.2 Columns

- **`add_column(session, oid, col, missing)`** appends a column.
  A row written before it reads `missing`, its bytes, checked to
  be a datum of the column's type, or null without one. An
  identity column is `SM_ERR_INVALID`.
- **`NOT NULL` with no missing value needs an empty table**
  (`need_empty`). A savepoint is set, and the table-oid lock X is
  taken only if it is free now: busy is `SM_ERR_BLOCKED`. The
  lock is kept only if the table holds no live row. A deleted row
  can leave keys in the store, so a store that is not empty is
  scanned (`has_live_row`). The scan reads the latest committed
  rows. A live row is `SM_ERR_NOTNULL`. On a failure the
  savepoint lets the lock go.
- **`set_not_null(session, oid, name, constraint)`** makes a
  live column `NOT NULL`, its constraint named `constraint`, or
  by sm3 when it is empty. It opens the table exclusive and reads
  the latest committed rows. This includes a row committed after
  a `REPEATABLE READ` snapshot. A row that holds a null is
  `SM_ERR_NOTNULL`.
- **`set_identity(session, oid, name, identity, seq)`** makes a
  column an identity column whose values come from `seq`
  ([`SEQUENCE.md`](SEQUENCE.md)). `ADD COLUMN` with a volatile
  default or of an identity column is `add_column`, the rows
  given their values, then these two.
- **`rename_column(session, oid, name, new_name)`** puts the
  record back with the live column renamed. An index names its
  columns by place, so it is untouched.
- **`drop_column(session, oid, name)`** marks the column dropped.
  It keeps its place and reads as null, and its name can be added
  again. A column an index uses is `SM_ERR_INVALID`. One a
  foreign key of the table or a view uses is `SM_ERR_INUSE`. The
  sequences it owns go with it.
- **`alter_column_type(session, oid, name, to, conv)`** changes a
  column's type, under X on the table. It refuses an identity
  column (`SM_ERR_INVALID`), a column something would read stale
  (`SM_ERR_INUSE`), and a change with no converter whose values
  would not stay (`SM_ERR_INVALID`). With a converter and rows it
  rewrites the table; else it puts the record alone.
- **`column_use(session, oid, at, use, by)`** says what keeps a
  column from changing type, a view first.
  **`retype_keeps_values`** says whether its values stay with no
  converter: the same type code and typmod, or `text` and
  `varchar` with a typmod no narrower.

### 8.3 Indexes, collations, foreign keys and views

- **`create_index(session, table, name, keys, includes, unique,
  oid, primary, constraint, tree)`** adds an index to the record,
  with its key columns, their direction, null order and
  collation. A collation is taken only on `text`, `varchar` or
  `bpchar`. It takes the table's next relid, which is never
  handed out again.
- **The index starts pending.** `create_index` does not fill it.
  A table with no definition in the shared catalog has no other
  reader, so the call builds the index itself, over the rows the
  transaction wrote, and records it valid. Any other table's
  index goes to the Index Builder (§9).
- **`primary`** marks the table's primary key, which keys its
  rows: unique, on columns alone, each `NOT NULL`, and the
  table's only one. Its entries are the rows, so it is valid at
  once, and a table with rows is rewritten.
- **`constraint`** marks an index that a `UNIQUE` or
  `PRIMARY KEY` constraint made, not `CREATE UNIQUE INDEX`: a
  mark for the host, unique, on columns alone. An index written
  before the mark reads it false.
- **`tree`** makes a B+tree of its own pages
  ([`BTREEINDEX.md`](BTREEINDEX.md)). A tree that is a primary
  key is `SM_ERR_INVALID`.
- **`create_index(…, eval, reads, oid, …)`** is the same for a
  partial or an expression index: `eval` is its blob, `reads` the
  columns it reads. It needs the Catalog to have an evaluator.
- **`set_index_state(session, table, oid, state, error, reason,
  dup)`** puts an index's state, as the build does when it ends,
  with the error and its message for a failed one, and the key of
  a row that holds a duplicated key. It is `cat_put_index_state`
  and nothing more, so it does not wait for the table's users.
- **`drop_index(session, table, oid)`** erases the index's
  entries, one by one over its relid in the table's store, and
  takes it out of the record. A primary key is rekeyed instead
  (§2.2). A tree's pages go by `sm1::on_commit`: a rollback
  brings the index back, and a crash first leaves them until the
  store goes. A parent index a foreign key uses is
  `SM_ERR_INUSE`.
- **`create_collation(session, nspoid, name, locale,
  deterministic, oid)`** takes an oid and puts the collation.
  Only sm3 has this call: `sm.h` does not. `create_table` and
  `add_column` set no column collation, so a collation reaches a
  table through an index key.
- **`create_fkey(session, table, def, oid)`**, **`drop_fkey`**,
  **`validate_fkey`**, **`rename_fkey`** work on a foreign key of
  the table, its child, under its lock X and, to add or drop one,
  its parent's. Its entry is the child's. The parent's
  `referenced_by` is derived, at load and by these, and written
  nowhere ([`FOREIGNKEY.md`](FOREIGNKEY.md)).
- **`create_view(session, nspoid, name, def,
  oid)`**, **`replace_view`**, **`rename_view`**, **`drop_view`**
  work on a view, which is its record alone, with nothing outside
  the Catalog to make or drop. Each relation it reads is locked
  IS to the end of the transaction. `nsp_temp` makes a temporary
  view, which alone may read a temporary relation. A view another
  view reads is `SM_ERR_INUSE` to drop, and a replace whose reads
  reach the view itself is `SM_ERR_INVALID`
  ([`VIEW.md`](VIEW.md)).
- **`create_namespace`**, **`drop_namespace`** are in
  [`NAMESPACE.md`](NAMESPACE.md).
- **`create_sequence`**, **`drop_sequence`**, **`own_sequence`**,
  **`nextval`** are in [`SEQUENCE.md`](SEQUENCE.md).

### 8.4 Decimal columns

sm has no `decimal(p, s)`. A decimal column is sm2's `decimal64`
or `decimal128`, one per width, with its `(p, s)` typmod, as the
host declares it.

**Decision: the host chooses the width, not sm3.** SQL's
`decimal(p, s)` and `numeric(p, s)`, `decimal(p)` and a bare
`numeric` are resolved above sm, to one of the two and a declared
`(p, s)`. digres's rules, and its default for a bare `numeric`,
are in [`../dg/DECIMAL.md`](../dg/DECIMAL.md). sm3 takes the type
and typmod it is given, and supplies no default. A typmod the
type refuses, `-1` or a precision past its 17 or 36 digits among
them, fails `CREATE TABLE` with `SM_ERR_INVALID`. Changing a
column from `decimal64` to `decimal128` changes its type and so
rewrites the table.

---

## 9. The Index Builder

The Index Builder is no part of the Catalog
([`INDEXBUILD.md`](INDEXBUILD.md)). It runs a service and waits
on it, so its calls are its own, `builder_*`, in
`sm3/src/builder/builder.cc`. It meets the Catalog in three
places:

- **`create_index`** hands an index it leaves pending to the
  queue, `builder_enqueue`, in a function it gives
  `sm1::on_commit`. The function runs once the transaction has
  committed, and not at all if the transaction aborts or rolls
  back past the index.
- **`enqueue_pending_indexes(cat)`** queues every pending index,
  by a walk of the committed tables, namespace by namespace. It
  is called when a database is opened, for the indexes whose
  `on_commit` function a crash lost. The Index Builder itself
  never looks for work: it is given a table and an index, checks
  the index is still pending, and fills it.
- **`set_index_state`** marks an index valid or failed when its
  build ends.

---

## 10. Source and C API

```
sm1/src/access.cc          store 0 under "U" for callers
sm2/src/rel/catkv.cc       the catalog key-value store
sm3/include/sm3.h          definitions and calls
sm3/src/catalog/catalog.h  keys, records and parts (internal)
sm3/src/catalog/internal.h catalog_t, session_t and the overlay
sm3/src/catalog/key.cc     keys
sm3/src/catalog/blob.cc    values: parts and their framing
sm3/src/catalog/load.cc    cat_open and the committed lookups
sm3/src/catalog/ddl.cc     cat_init, and the puts and dels
sm3/src/catalog/session.cc the session, its transaction, publishing, its lookups
sm3/src/ddl/ddl.cc         the DDL: a caller of the Catalog
sm3/src/table/table.cc     rewrite_table
sm3/src/builder/builder.cc the Index Builder: a caller of the Catalog
src/sm.cc                  the C API: sm_cat_*, a catalog per database
```

The C API (`include/sm.h`) keeps each database's Catalog in its
`sm_env_t`, made by `sm_mkdb` and loaded by the first
`sm_opendb`. It keeps an sm3 session in each `sm_session_t`, so
its transaction calls keep the overlay in step. `sm_cat_*` are
the calls above as the host needs them: namespaces, tables and
their columns, indexes, sequences, foreign keys and views, looked
up, listed, locked, created, altered and dropped, with
definitions as plain C structs. A table's statistics are
`sm_tbl_stats`, on an open table. digres and `rel` keep no
catalog of their own.

---

## 11. Not built

- **Dependencies in general**, such as refusing `DROP COLLATION`
  in use: by a scan of the tables' records, until they earn a key
  kind, the **dependency index**. What is built is each kind's
  own, a view's reads and a foreign key's parent, both derived in
  memory and stored nowhere.
- **`DROP COLLATION`** itself. A namespace that has held a
  collation therefore stays.
- **Startup cost for thousands of tables** is unmeasured: load
  time and memory for a schema with many partitions. `cat_open`'s
  scan of every name adds to it.
- **A commit-ordered change feed.** Logical decoding could be fed
  from sm1 and not from a second copy in PostgreSQL's WAL, but
  sm1 has no such feed yet
  ([`LOGICALDECODE.md`](LOGICALDECODE.md)).
- **A 64-bit `Oid` in digres**, which removes `last_oid`.

---

## 12. Cases

| Case | What happens |
| --- | --- |
| Abort | `abort` drops the overlay and closes its collations; sm1 undoes the entries, stores and sequences |
| Savepoint rollback | changes tagged the depth or deeper go, with sm1's undo of their entries; earlier changes stay |
| Commit fails | the overlay goes and the old definitions stay; the mutex was held, so no reader saw a change |
| Crash | no overlay survives; uncommitted entries are dead; `cat_open` rebuilds memory; `enqueue_pending_indexes` queues what `on_commit` lost; the counts restart at 0 |
| Two-phase commit | `prepare` refuses a transaction with any overlay change, an analyze's statistics and temporary changes among them, so no overlay crosses a restart |
| Two DDL on one table | the table-oid lock X runs them one after the other, even when they write different keys |
| Two DDL taking one name | they meet on the `NAME` key: `SM_ERR_DUPKEY`, or sm1's write-write conflict |
| DDL against readers | X waits for each IS and IX holder, and new openers wait for X; a deadlock is sm1's detector's to end |
| Reader waits on DDL, then the table is gone | `cat_lock_table` reads memory after the lock and fails with `SM_ERR_NOTFOUND` |
| REPEATABLE READ first touches a table after DDL | it gets the new definition with its old data snapshot, as in PostgreSQL |
| REPEATABLE READ snapshot older than the table's last DDL, and it runs DDL | `cat_get_table_rec` gives the table as it now is; the X lock on a key another changed after the snapshot fails with `SM_ERR_CONFLICT` |
| READ COMMITTED DDL over a key another changed | `catkv_get_latest` under the key's X lock lets the write through |
| SERIALIZABLE | a definition read from memory records no SIREAD; the OID lock orders DDL and users |
| Open handle or pin across DDL | the handle holds its generation alive; X cannot be granted while the handle's transaction holds IS or IX |
| Old snapshot and a new index | the count of publishes keeps the snapshot off an index that became valid after it |
| `TRUNCATE` or a rewrite, then abort | the new store goes and the old store stays, with the record as it was |
| Temporary table | no store entry and no lock; its changes go to the temporary catalog at commit, and no other session sees them |
| Table with no primary key | its rows are keyed by `_id` from `sm3.id.<oid>`; the sequence survives a `TRUNCATE` |

---

## 13. Rejected

- **A versioned catalog**, in which an old snapshot sees an old
  definition. PostgreSQL does not give one, and the lock-based
  design needs none.
- **A hybrid**, with PostgreSQL's heap catalogs kept for some
  objects. DDL atomicity would sit exactly on the seam.
- **Locking the `TABLE` key** in place of an OID lock. DDL
  rewrites that key, and a lock on a written key fails at
  REPEATABLE READ for a version newer than the snapshot. That
  reader waited out the DDL and should get the new definition.
  The key kind 7 once held such a lock, and is retired.
- **A table's definition in one blob.** It could pass the 64 KB
  row limit, and two DDL would rewrite one key.
- **An index's state in its definition or its entry.** A state
  changes under IX while transactions hold the definition.
- **Reading the store in `cat_get_table_rec`.** The snapshot may
  be older than the table's last DDL.
- **Many tables per sm1 store**, with a wider object id as the
  relid, which makes the relation count a tuning decision.
  `DROP TABLE` becomes a range delete, not sm1's O(1) drop. It
  was rejected for v1.
- **Reusing relids** under a rule tied to the GC horizon. A
  dropped index's versions linger, and a counter that never goes
  back needs no such rule.
- **Wrapping the oid counter**, and checking that each oid is
  free, as PostgreSQL does. sm stops at `last_oid`.
- **Preparing a transaction that made DDL.** Its overlay would
  have to outlive the session.
- **sm3 choosing a decimal's width.** The host declares it.

---

## 14. Tests

```
sm2/test/test_catkv.cc     the catalog key-value store
sm3/test/test_layout.cc    keys and values
sm3/test/test_catalog.cc   init, load and create
sm3/test/test_ddl.cc       the rest of the DDL
sm3/test/test_altertype.cc a column's type
sm3/test/test_cluster.cc   a primary key added and dropped, rewrites
sm3/test/test_overlay.cc   the overlay: visibility, rollback, abort
sm3/test/test_lock.cc      table-oid locks, and publishing under them
sm3/test/test_temp.cc      the temporary catalog
sm3/test/test_fkey.cc      foreign keys
sm3/test/test_view.cc      views, temporary ones among them
sm3/test/test_stats.cc     statistics, recorded and read
rel/test/test_catalog.rel  DDL through the C API, end to end
rel/test/test_epoch.rel    the catalog's epoch
rel/test/test_truncate.rel TRUNCATE
```
