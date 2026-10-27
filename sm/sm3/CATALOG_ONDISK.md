# CATALOG_ONDISK — sm3's catalog on disk

Where sm3's catalog is stored, and in what bytes: its store,
its keys and its values. `CATALOG_INMEM.md` is what sm3 builds
from them in memory, how a session's DDL changes them, and the
calls.

**Status: built**, at format 4 (§3).

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
  start at 16384, PostgreSQL's `FirstNormalObjectId`. Oids below
  it are for built-in objects, and are PostgreSQL's own numbers,
  since digres compiles its built-ins in with them (`PART4.md`
  §10): `pg_catalog` is 11, `public` is 2200. `nextseq` is not
  transactional, so an aborted DDL leaves a gap, as PostgreSQL's
  oid counter does. They stop at `last_oid`, 2^32 - 1: digres's
  oid is 64 bits (`PART4.md` §10), but for now it keeps
  PostgreSQL's 32-bit `Oid`, so a larger one would wrap onto a
  built-in one; the cap goes when that is fixed. Past it,
  `cat_new_oid` fails with `SM_ERR_INVALID`; PostgreSQL wraps its
  counter instead, and checks each oid it hands out is free,
  which sm does not.

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
| `catkv_trylock` | as `catkv_lock`, never waiting: 1 if another holds it | `sm1::trylock` |
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
| `TABLE` | 4 | oid `int8` | the table's own fields |
| `COLLATION` | 5 | oid `int8` | the collation's blob |
| `INDEX` | 6 | table oid `int8`, index oid `int8` | one index of the table, with its key, include and read columns |
| — | 7 | — | retired: a table's oid lock was a key here, and is now an `sm1::LOCKTYPE_OID` lock on its oid |
| `COLUMN` | 8 | table oid `int8`, attnum `int8` | one column of the table |
| `INDEX_STATE` | 9 | table oid `int8`, index oid `int8` | one index's state, with why its build failed; none for a pending index |
| `STATS` | 10 | table oid `int8`, index oid `int8`, n `int8` | the table's statistics, an entry for the table, (0, 0), and one a statistic: a column's (0, attnum + 1), an index expression's (index oid, place + 1); `SM3_DESIGN.md` §6.6 |
| `SEQUENCE` | 11 | oid `int8` | a sequence: its namespace, name, start and increment |
| `CONSTRAINT` | 12 | table oid `int8`, constraint oid `int8` | one constraint of the table, its child: a foreign key (`../FOREIGNKEY.md`) |
| `VIEW` | 13 | oid `int8` | a view: its namespace, name, the host's query and options, and the relations it reads (`../VIEW.md`) |

- **A table is several entries.** Its own fields are under
  its `TABLE` key, each column under a `COLUMN` key, and each
  index under an `INDEX` key. A column is a part of its table
  and has no oid: its key is its table's oid and its attnum,
  its place, which it keeps for good, so a dropped column's
  key stays. An index is an object and is keyed by its
  table's oid and its own. A table's columns are therefore
  together in attnum order and its indexes in oid order,
  which is the order they were made in.
- **DDL writes the entries it changes.** Adding a column
  writes one `COLUMN` key, and a build that ends rewrites one
  `INDEX` key. Two DDL on one table may then write different
  keys and meet in no write-write conflict: the table's
  oid lock is what puts them one after the other.
- **An index's state is not in its entry.** It has an entry of
  its own, `INDEX_STATE`, written without the table being
  changed and under the table's lock IX, not X
  (`CATALOG_INMEM.md` §2.1). A
  pending index has none: no entry is what pending means, so
  only valid and failed are ever stored. It goes with its
  index.
- **Names are keys.** Every named object owns a `NAME` key, so a
  name taken in its namespace fails with sm1's `SM_ERR_DUPKEY`,
  and two sessions taking it at once meet in sm1's write-write
  conflict. A namespace's own name is in namespace 0, which is
  no namespace.
- **A table's `_id` sequence is `sm3.id.<oid>`**, made with
  its store.
- **A sequence's counter is `sm3.seq.<oid>`**, the sm1 sequence
  made and dropped with its `SEQUENCE` entry, in the same
  transaction; `nextval` is sm1's `nextseq` on it, and so not
  transactional. Its name is a `NAME` key in its namespace, so it
  shares names with the namespace's tables and indexes, as a
  relation does in PostgreSQL. Its namespace and name are kept in
  memory, by its oid and its oid by them, for a namespace's drop
  (`CATALOG_INMEM.md` §2) and so that it is found either way as a
  table is: the transaction's own creates and drops, then every
  committed one, not as of the snapshot, as PostgreSQL finds a
  name in its latest catalog. Its record and counter are read
  from the store in the transaction.
- **A sequence may be owned by a column**, SQL's `OWNED BY`, of
  a table in its namespace: its owner fields name the table and
  the column's place, and are null otherwise, as in a sequence
  written before them. Dropping the table or the column drops
  it; dropping it alone is `SM_ERR_INUSE`. The table's drop
  finds what it owns by reading its namespace's sequences.
- **An identity column names its sequence**, in its identity
  fields, and owns it from the `create_table` that made it:
  the sequence must be in the table's namespace and owned by
  none. Its owner is then fixed: `own_sequence` on it is
  `SM_ERR_INUSE`. The column names it so the host finds it
  without reading the namespace's sequences.
- **A view's query is in pieces**, its parts in order: a part
  is an sm2 tuple, at most 65,535 bytes, and a query may be
  longer. SQLite's corpus has one of 100 KB.
- **A view is one entry**, its reads in its value, and its
  name a `NAME` key in its namespace, shared with the
  relations there. It has a table's oid lock, on its oid. What
  reads a relation is found by a pass over the views in memory,
  and stored nowhere (`../VIEW.md` §4).
- **A table's oid lock is no longer a key here.** It is an
  `sm1::LOCKTYPE_OID` lock on the table's oid
  (`SM3_DESIGN.md` §2). Locking the `TABLE` key instead would
  not do: DDL rewrites it, and a lock on a key that was
  written fails at REPEATABLE READ for a version newer than
  the snapshot, which is exactly the reader that waited out
  the DDL and should get the new definition.

---

## 3. Values

A value is a sequence of **parts**, each an sm2 tuple under a
schema sm3 fixes for it, preceded by its length as a varint
(`src/catalog/blob.cc`):

```
TABLE value       = table
COLUMN value      = column
INDEX value       = index | key × nkey | include × ninclude | read × nread
INDEX_STATE value = index state
SEQUENCE value    = sequence
CONSTRAINT value  = constraint | fkey | fkey pair × npairs
VIEW value        = view | view read × nread | view query × npiece
STATS value       = table stats | index stats × nindex
                  | statistic | mcv × nmcv | bound × nbound
```

A `STATS` entry is the table's own, the first line, or one
statistic's, the second; their parts are in
`SM3_DESIGN.md` §6.6, and are not repeated below.

| Part | Fields |
| --- | --- |
| meta | format version `int4` |
| name | kind `int2`, oid `int8`, table oid `int8` or null |
| table | namespace `int8`, name `text`, storeid `int8`, `_id` sequence `text`, next index relid `int2` |
| column | name `text`, type code `int2`, typmod `int4`, not null `bool`, dropped `bool`, missing value `bytea` or null, collation oid `int8` or null, default `bytea` or null: the host's expression, opaque, kept and never run; identity kind `int2` (`a` or `d`) or null, identity sequence `int8` or null, update default `bytea` or null: the host's expression for what an UPDATE that does not assign the column writes, opaque, kept and never run (`../../dg/ONUPDATE.md`) |
| index | oid `int8`, name `text`, relid `int2`, unique `bool`, nkey `int4`, ninclude `int4`, three retired and null, eval `bytea` or null, nread `int4` or null, primary `bool` or null, constraint `bool` or null: true, or null for false |
| index state | state `int2`, error `int2` or null, reason `text` or null, the key of a row that holds a duplicated key, `bytea` or null (`../ROWKEY.md` §2.5) |
| key | attnum `int4`, desc `bool`, nulls first `bool`, collation oid `int8` or null, type code `int2` or null, typmod `int4` or null |
| include | attnum `int4` |
| read | attnum `int4`, laid out as an include part is |
| namespace | name `text` |
| collation | name `text`, locale `text`, deterministic `bool`, ICU version `text` |
| sequence | namespace `int8`, name `text`, start `int8`, increment `int8`, owner table `int8` or null, owner column `int4` or null |
| constraint | oid `int8`, name `text`, type `int2` (`f`), validated `bool`, host `bytea` or null |
| fkey | parent table `int8`, parent index `int8`, npairs `int4`, on update `int2`, on delete `int2`, match `int2`, deferrable `bool`, deferred `bool` |
| fkey pair | child attnum `int4`, parent attnum `int4` |
| view | namespace `int8`, name `text`, options `bytea` or null: the host's; nread `int4`, npiece `int4` |
| view read | relation `int8`, attnum `int4`: a table's column, 0-based, or -1 for the relation whole |
| view query | a piece of the host's query, opaque, `bytea` of at most 60,000 bytes |

- **A part evolves as a tuple does.** A field appended to a
  part's schema reads as its missing value in a value written
  before it (`sm2/TUPLE.md` §2), so a value needs no version of
  its own. `META`'s version changes only if a key's layout, or
  the framing, does: it was 2 when a table's columns and
  indexes left its blob for keys of their own, 3 when an
  index's state left the index part for one too, and is 4
  since the built-in namespaces took PostgreSQL's oids. A new
  kind, as `SEQUENCE` was, changes no key's layout, and leaves
  it as it is.
- **An index's state** is 0 pending, 1 valid or 2 failed
  (`SM3_DESIGN.md` §3), in the index state part. The index
  part's three attributes that once held it are retired and
  left null.
- **A partial or an expression index keeps a blob.** Its eval
  is its predicate and its expressions as the host serialized
  them, bytes sm3 does not read, and its read parts are the
  columns they read (`SM3_DESIGN.md` §3). A plain index has
  neither. A key that is an expression has attnum -1 and gives
  its own type code and typmod, which a column's key leaves
  null.
- **A failed index keeps why**, in its state: its error is the
  code its build failed with and its reason that error's
  message, both null in any other state.
- **Type codes are sm2's frozen codes** (`sm2/TYPE.md` §3); code
  7, the retired `decimal`, is never written. A decimal
  column is stored as the sm2 type its host declared, with
  no mapping in sm (`SM3_DESIGN.md` §2.1).
- **Column positions are 0-based**, in key and include parts as
  in memory.
- **A missing value is its bytes**, a varlena for a
  variable-width type, as sm2's `attribute_t` holds it.
- **The next index relid is kept** in the table part, so a
  relid is never reused, across restarts too (`SM3_DESIGN.md`
  §4).
- **Each part is held to 64 KB** (`sm2/TUPLE.md` §1); an index's
  value, a sequence of them, is not, and is an sm1 value of up
  to 64 MB.
- A value cut short, with bytes past its last part, or with a
  required field null, is `SM_ERR_CORRUPT`.
