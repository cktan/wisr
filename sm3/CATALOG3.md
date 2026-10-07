# CATALOG3 — the catalog's layout

`CATALOG.md` is the catalog **as built**: seven key kinds, and
a value that is a sequence of hand-framed sm2 tuples with a
table's columns and indexes nested inside one of them.
`CATALOG2.md` is the inventory of what a catalog has to hold,
and it ends by saying the layout comes after. This is that
layout.

**Status: proposed.** None of it is built. The format sm3
writes is "the one format sm3 writes that lasts"
(`SM3_DESIGN.md` §2), and there are no users yet, so there is
no migration to pay for.

The shape is Redis's: a flat keyspace of colon-delimited
names, one key per object, and a value that is a hash of
tagged fields. What it does not take from Redis is RESP
framing, field names inside the value, and unordered `SCAN` —
store 0's scans are ordered, and §3 spends that order rather
than giving it away.

---

## 0. The entities

§3 has twelve key shapes. Which of the things they name
has an identity of its own is what decides that shape, so the
things come first.

### 0.1 Objects, parts, and bookkeeping

- **An object** has an **oid** — a 64-bit identifier from the
  sequence `sm3.oid`, unique in the database and never reused
  — and a **name** unique within its namespace. §5.10 lists
  the kinds: namespace, table, index, collation, constraint.
  An object is reached by its oid, renamed without being
  rewritten, and may be referred to from elsewhere.
- **A part** has no oid. Its identity is its **position**
  inside the object that owns it: a column by its attnum
  within its table, an index's key column by its place in
  that index. Nothing may point at a part, it is dropped with
  its owner, and it means nothing apart from it.
- **Bookkeeping keys** are neither. The format version, the
  name index and the dependency index describe objects without
  being any. A table's schema lock is not even a key: it is an
  sm1 locktype (§0.4).

This division is why §3's keys nest. An object gets a key
word and an oid; a part gets its owner's key extended by its
position. An oid on a part would buy nothing — nothing may
point at it — and would cost an entry in the name index and
a number from the sequence.

### 0.2 Namespace

A **namespace** is a named container for object names: the
SQL `SCHEMA`. It holds nothing, and stores nothing beyond its
own name. Its only job is to scope names, so two tables may
both be called `orders` as long as they sit in different
namespaces — which is why the name index is keyed by
`(namespace oid, name)` and not by the name alone.

Two exist from `cat_init`: `pg_catalog` at oid 1 and `public`
at oid 2 (`CATALOG.md` §1). A namespace's own name is in no
namespace: it is recorded under namespace oid 0, so that
namespace names are unique across the database. Which
namespace an unqualified name resolves to — the search path —
is the SQL layer's, not sm3's (`CATALOG.md` §7).

### 0.3 Table, column, index, constraint, collation

- **A table** is a heap of rows in one sm1 store, keyed by
  `rowno`. The heap is one rel in that store, always at
  relid 1, and the table's indexes are rels beside it
  (`SM3_DESIGN.md` §4). The catalog holds the table's
  identity, which store holds it, and the sequence that hands
  out its rownos.
- **A column** is a part of a table: a name, an sm2 type and
  typmod, and what a row needs in order to be read — among
  it the `missing` value, which is what a row written before
  the column was added reads for it. Its attnum is its
  position, and a dropped column keeps its attnum for ever,
  so no later column shifts under a row already written.
- **An index** is an ordered mapping from an encoded key to a
  `rowno`, under its own relid inside its table's store
  (`SM3_DESIGN.md` §4). It is an object, not a part of its
  table: it has an oid and a name, so `DROP INDEX` can name
  it. Its **key columns**, its **INCLUDE attributes** and the
  **columns its predicate reads** are parts of it, each
  identified by its position.
- **A constraint** is a rule its table's rows must satisfy —
  `CHECK` or `FOREIGN KEY` — an object so that
  `ALTER TABLE … DROP CONSTRAINT` can name it.
- **A collation** is a named ICU locale together with the ICU
  version it was created against, so that a changed library
  is detected instead of silently reordering an index
  (`sm2/SM2_DESIGN.md` §3).

### 0.4 A table's schema lock

**What it is.** One sm1 row lock per table, taken in Gray's
modes before a transaction reads or writes that table's
definition and held to the end of the transaction: IS to read
it, IX to write rows under it, S for a plain index build, X
for DDL (`SM3_DESIGN.md` §2). It does not lock the table's
rows — sm1 locks those itself. It is what makes an
**in-memory** catalog safe: the definition a transaction uses
is a pointer into shared memory, DDL replaces it at commit,
and this lock is the only thing stopping that replacement
from happening under a reader. Without it DDL has nothing to
wait on.

**It is not a key.** `sm1::lock` takes a **locktype**, and a
schema lock is `LOCKTYPE_OID`: a name in a namespace of its
own, in no store, which nothing ever writes. sm1 reads no
store for it and checks no version, because none can exist. So
the catalog holds nothing for it — no `lock:` key, no entry,
no kind of its own.

**Why the name is the table's oid.** Every session that locks
the table must compute the same bytes, so the name has to come
from an identity stable for the table's whole life. A table
name moves under `ALTER TABLE … RENAME`; a storeid is the
store's identity and not the table's, and `TRUNCATE` swaps it
while the lock must not move. The oid is the one thing that
never changes, so the name is `be64(oid)` — big-endian
because it is persisted in a prepared transaction's lock set.

**What the type buys.** A lock on a key that *is* written
fails with `SM_ERR_CONFLICT` for a version committed after the
caller's snapshot. That would hit exactly the reader which
waited out a DDL and should be handed the new definition. The
earlier design dodged this by locking a key deliberately never
written; `LOCKTYPE_OID` makes the property explicit instead
of a convention the next reader has to notice.

### 0.5 Dependency, and what a dependent is

A **dependency** is a recorded fact that one object uses
another, kept so that a `DROP` can be refused before it
breaks something. It has two ends:

- the **referenced** object: the one being used, the one the
  `DROP` is aimed at;
- the **dependent** object: the one that would be left broken
  if the referenced object went away — the one
  `DROP … RESTRICT` names in its error, and the one a
  `CASCADE` would remove.

So `dep:<kind>:<oid>:<kind>:<oid>` reads *referenced, then
dependent*. The referenced end comes first because the
question is only ever asked from that end: "is anything still
using this collation?" is a scan of `dep:04:<oid>:`, and the
first key it returns is the dependent to name in the error.

In a table catalog the dependents are these:

| Dependent | Referenced | Because |
| --- | --- | --- |
| table | its namespace | `DROP SCHEMA RESTRICT` must refuse |
| table | a collation | a column of it is declared with one |
| index | its table | `DROP TABLE` takes its indexes with it |
| index | a collation | a key column compares with it |
| constraint | its table | dropped with the table |
| constraint | another table | a `FOREIGN KEY` points at it |

**A dependent is always an object, never a part.** A column
is what actually uses a collation, but a column has no oid
(§0.1), so the dependency is recorded against its table.
Refusing the `DROP` needs no more: the table is in memory, so
naming the offending column in the message is a walk of its
columns. PostgreSQL's `pg_depend` carries an `objsubid` — the
attnum — beside the oid for exactly this; if a message or a
`DROP COLUMN` cleanup turns out to need that precision, the
subid becomes one more fixed-width component of the key. It
is left out until it does.

**A dependency is derived, not authoritative.** Every fact in
`dep:` can be recomputed by scanning the tables' columns and
indexes, which is what the catalog does today
(`CATALOG.md` §7). These keys exist only to make the question
cheap at the referenced end, which means every DDL that
creates or removes a reference must write or erase the
matching key. It is an index, and it can be rebuilt.

---

## 1. What this settles

Three choices, taken here so the rest reads as one design.

- **An oid is 16 lowercase hex digits.** Fixed width, so
  byte order is numeric order; hex rather than 20 digits of
  decimal because it is shorter and a hex dump of store 0 is
  where it will be read.
- **A field's tag is one ASCII byte**, its meaning frozen per
  kind. This is the one departure from Redis, whose hash
  fields are self-describing names: a letter is one byte
  against eight to twenty, the key already says what the
  object is, and a frozen tag is what gives the value the
  evolution rule of §4.
- **Scope stays open.** `CATALOG2.md` §0 asks how far down
  `pg_catalog` the catalog is meant to go and says to decide
  that first. This layout is built so the answer changes §5's
  field tables and not the shape: a new object kind is a new
  key word, nothing nests inside anything else, and no enum
  has to widen. §5 fills in what `CATALOG2.md` §1 to §7 holds
  today, plus the eight gaps §8 calls live bugs rather than
  missing features. A sixty-kind `pg_catalog` adds key words
  beside them and changes nothing above.

---

## 2. Where it lives

Unchanged from `CATALOG.md` §1, which this file does not
reopen: a key-value map in sm1's own catalog store, store 0,
under the key prefix `"U"`, reached through sm2's `catkv_*` in
the session's transaction. One commit point with the tables,
beside sm1's `"R"` and `"S"` rows and invisible to them,
present in every database from its birth. sm3's oids still
come from the sm1 sequence `sm3.oid`.

What changes is only the bytes of a key (§3) and the bytes of
a value (§4).

---

## 3. Keys

Two invariants carry the design.

1. **Every fixed-width component is padded to its full
   width**, so byte order is numeric order. This is what a
   bare decimal oid gets wrong — `"10"` sorts before `"9"` —
   and the cost would be a table's columns handed back out of
   attnum order.
2. **At most one variable-length component, and it is last.**
   A `:` inside a table name is then harmless: nothing after
   it has to be parsed, and no name can reach into the next
   key's territory. A kind that ever needs two variable
   components takes an oid instead.

```
oid     = 16 lowercase hex digits       a uint64, in numeric order
attnum  = 4 lowercase hex digits        0-based, as CATALOG.md §3
seqno   = 2 lowercase hex digits        a position within an index
kind    = 2 lowercase hex digits        an object kind of §5.10

meta                                    the catalog itself
ns:<oid>                                a namespace
coll:<oid>                              a collation
tbl:<oid>                               a table
tbl:<oid>:att:<attnum>                  one of its columns
tbl:<oid>:con:<oid>                     one of its constraints
tbl:<oid>:idx:<oid>                     one of its indexes
tbl:<oid>:idx:<oid>:inc:<seqno>         an INCLUDE attribute
tbl:<oid>:idx:<oid>:key:<seqno>         a key column
tbl:<oid>:idx:<oid>:rd:<seqno>          a column its eval reads
name:<nspoid>:<name>                    the unique-name index
dep:<kind>:<oid>:<kind>:<oid>           referenced, then dependent

reserved, and not defined here: seq:, typ:, mv:
```

**The hierarchy is the point.** A parent key is a proper
prefix of its children, so one scan of `tbl:<oid>:` streams a
whole table definition in the order it is built: its columns
in attnum order, its constraints, then each index followed
immediately by its own include, key and read columns. One
scan of `tbl:` does every table in oid order, so `cat_open`
becomes one scan and two more (`ns:`, `coll:`) instead of a
scan per kind and a blob to take apart. The bounds cost
nothing new: `catkv_scan` already raises a prefix's last byte,
and the bound on `tbl:` is `tbl;`, since `;` is `:` + 1.

**A schema lock sits outside its table's range**, for the
two reasons §0.4 gives: the key must be one no DDL writes,
and a scan of a table's parts should not have to skip it.

**A name is still a key**, so a name taken in its namespace
fails with sm1's `SM_ERR_DUPKEY`, and two sessions taking it
at once meet in sm1's write-write conflict. Raw UTF-8 is the
right encoding here precisely because it is not
collation-folded: byte uniqueness is what the duplicate check
wants.

**A dependency's key is its own index** (§0.5): the
referenced object comes first, so `CATALOG.md` §7's open item
— refusing a `DROP COLLATION` still in use — becomes a scan
of `dep:04:<oid>:` instead of a scan of every table blob.

**What leaves.** The seven-value key-kind enum and the
`encode_key` calls in `src/catalog/key.cc`: a key is now
assembled from a word, fixed-width hex and at most one tail.
sm2's `encode_key` remains sm3's tool for real index keys; it
was never the right tool for these.

---

## 4. Values

A value is a hash of tagged fields, written with tags
**non-decreasing**:

```
value = { tag:1 , len:varint , bytes[len] }*
```

`varint` is unsigned LEB128, as `src/catalog/blob.cc` already
writes and as sm2 frames `natts`.

- **An uppercase tag is always a flag; a lowercase tag always
  carries bytes.** A flag has `len = 0`: presence is truth,
  absence is false, and a boolean costs one byte in all.
- **A flag always names the non-default condition**, so the
  common value is the absent one. `not null` is a flag; a
  collation's `deterministic`, which defaults to true, is
  stored as the flag `N`, not deterministic.
- **An integer is minimal-width big-endian**, its width given
  by `len`. Text is UTF-8 with no terminator; `bytea` is its
  bytes, as `attribute_t` holds them.
- **A tag marked repeatable** may appear more than once, and
  then consecutively, which keeps tags non-decreasing. No
  other tag may repeat.
- **An unknown tag is skipped, not an error** — its `len`
  makes it skippable. So a purely additive field needs no
  `META` bump, and an older binary reads a newer value's
  known fields. This is strictly better than today, where
  bytes past the last part are `SM_ERR_CORRUPT`.
- **`META` moves only when the framing or §3's key grammar
  does**, which is narrower than today's rule.

`SM_ERR_CORRUPT` is a `len` running past the end of the
value, a decreasing tag, a non-repeatable tag appearing
twice, or a required field absent.

**Five count fields disappear.** `natts` and `nindex` from
the table, `nkey`, `ninclude` and `nread` from each index
(`CATALOG.md` §3) exist only to frame a nested blob. With a
key per part there is nothing to frame, and no count can
disagree with the number of keys present —
`CATALOG2.md` §10's "whether that is caught is luck".

**The 64 KB part ceiling stops mattering.** A column's
missing value, a DEFAULT and a generation expression each sit
in their own key's value, bounded by sm1's 64 MB, so
`SM3_DESIGN.md` §2's worry about a wide table's defaults
overflowing one part is gone. That is the reason
`CATALOG2.md` §4 wants a key per column, and it is paid here.

---

## 5. The fields, kind by kind

Tags are listed in ASCII order, which is the order they are
written in. "Req" is whether the field must be present.

### 5.1 `meta`

| tag | field | encoding | req |
| --- | --- | --- | --- |
| `v` | format version | int4 | yes |

The oid allocator is not here: it is the sm1 sequence
`sm3.oid` (`CATALOG.md` §1). Bootstrap — how the catalog's own
schemas are known before it is loaded — is still open
(`CATALOG2.md` §1).

### 5.2 `ns:<oid>` — a namespace

| tag | field | encoding | req |
| --- | --- | --- | --- |
| `n` | name | text | yes |

### 5.3 `coll:<oid>` — a collation

| tag | field | encoding | req |
| --- | --- | --- | --- |
| `N` | not deterministic | flag | — |
| `i` | ICU version it was opened at | text | yes |
| `l` | locale | text | yes |
| `n` | name | text | yes |
| `s` | namespace oid | oid | yes |

### 5.4 `tbl:<oid>` — a table

| tag | field | encoding | req | note |
| --- | --- | --- | --- | --- |
| `a` | parent table oid | oid | — | a partition's parent |
| `b` | partition bounds | bytea | — | host-serialized |
| `h` | partition strategy | int2 | — | range, list, hash |
| `j` | partition key | bytea | — | host-serialized |
| `k` | relkind | int2 | — | absent: an ordinary table |
| `n` | name | text | yes | |
| `p` | next index relid | int2 | yes | never reused |
| `q` | rowno sequence name | text | yes | `sm3.rowno.<oid>` |
| `s` | namespace oid | oid | yes | |
| `t` | storeid | oid | yes | sm1's store |

`k`, `a`, `h`, `j` and `b` are `CATALOG2.md` §8's items 1 and
5. What is inside `j` and `b` is not settled here: they are
the host's bytes, as an index's `eval` is, and sm3 does not
read them. Persistence is deliberately absent — a temporary
table is never written to store 0 (`CATALOG.md` §7), and
unlogged tables and matviews are unsupported.

### 5.5 `tbl:<oid>:att:<attnum>` — a column

| tag | field | encoding | req | note |
| --- | --- | --- | --- | --- |
| `N` | not null | flag | — | |
| `X` | dropped | flag | — | the key keeps the slot |
| `c` | collation oid | oid | — | |
| `d` | DEFAULT expression | bytea | — | host-serialized |
| `g` | generation expression | bytea | — | |
| `m` | typmod | int4 | — | absent: none |
| `n` | name | text | yes | |
| `q` | identity sequence oid | oid | — | |
| `t` | type code | int2 | yes | sm2's frozen codes |
| `v` | missing value | bytea | — | |
| `y` | statistics target | int4 | — | `STATISTICS.md` §3.3 |

The attnum is in the key, not a field. `d`, `g`, `q` and `y`
are `CATALOG2.md` §8's items 4, 6 and 7. A dropped column
keeping its place is now literal: its key stays, with `X` set
and its name free to be taken again. Type code 7, the retired
`decimal`, is never written.

### 5.6 `tbl:<oid>:con:<oid>` — a constraint

| tag | field | encoding | req | note |
| --- | --- | --- | --- | --- |
| `a` | referencing attnums | int4, repeatable | — | |
| `b` | referenced attnums | int4, repeatable | — | |
| `e` | ON DELETE action | int2 | — | |
| `f` | referenced table oid | oid | — | |
| `n` | name | text | yes | |
| `t` | constraint type | int2 | yes | check, foreign key |
| `u` | ON UPDATE action | int2 | — | |
| `x` | CHECK expression | bytea | — | host-serialized |

`CATALOG2.md` §8's items 2 and 3, and the second half of §10's
"a row per column and a row per constraint". A constraint is
the clearest case for a key of its own: it is the part of a
table whose count is least bounded.

### 5.7 `tbl:<oid>:idx:<oid>` — an index

| tag | field | encoding | req | note |
| --- | --- | --- | --- | --- |
| `U` | unique | flag | — | |
| `e` | error code | int2 | — | failed only |
| `m` | access method | int2 | — | absent: btree |
| `n` | name | text | yes | |
| `p` | relid | int2 | yes | |
| `r` | failure reason | text | — | failed only |
| `s` | build state | int2 | — | absent: valid |
| `v` | eval blob | bytea | — | partial or expression |

State 0 is building, 1 valid, 2 failed (`SM3_DESIGN.md` §3);
absent reads as valid, which keeps `CATALOG.md` §3's rule, so
a building or failed index writes `s` and a plain one does
not. `m` is `CATALOG2.md` §5's access method, true as "btree
implicitly" until the first hash, GiST or GIN index.

### 5.8 An index's columns

`tbl:<oid>:idx:<oid>:key:<seqno>` — a key column:

| tag | field | encoding | req | note |
| --- | --- | --- | --- | --- |
| `D` | descending | flag | — | |
| `F` | nulls first | flag | — | |
| `a` | attnum | int4 | yes | -1: an expression |
| `c` | collation oid | oid | — | |
| `m` | typmod | int4 | — | an expression's own |
| `o` | operator class oid | oid | — | absent: the type's |
| `t` | type code | int2 | — | an expression's own |

`tbl:<oid>:idx:<oid>:inc:<seqno>` — an INCLUDE attribute — and
`tbl:<oid>:idx:<oid>:rd:<seqno>` — a column the eval reads —
each hold one field, `a`, the attnum.

### 5.9 `name:…`, `dep:…`

A table's schema lock is not here: it is an
`sm1::LOCKTYPE_OID` lock on the oid, storing nothing
(§0.4).

`name:<nspoid>:<name>`:

| tag | field | encoding | req | note |
| --- | --- | --- | --- | --- |
| `k` | object kind | int2 | yes | §5.10 |
| `o` | the object's oid | oid | yes | |
| `t` | table oid | oid | — | an index's table |

`dep:<kind>:<oid>:<kind>:<oid>`:

| tag | field | encoding | req | note |
| --- | --- | --- | --- | --- |
| `t` | dependency type | int2 | — | absent: normal |

### 5.10 Object kinds

| code | kind |
| --- | --- |
| 01 | namespace |
| 02 | table |
| 03 | index |
| 04 | collation |
| 05 | constraint |

06 sequence, 07 type and 08 materialized view are reserved for
`CATALOG2.md` §7. `META` and `NAME` are no longer kinds at
all — they were key kinds and are key words now — and `LOCK`
is not even that: a schema lock is an sm1 locktype and stores
nothing. Only `name:` and `dep:` need a code for an object
kind.

Nothing in `CATALOG2.md` §9 is here: the sm2 `schema`, its
`tup_desc_t`, and an index's `key_column_t` and `attribute_t`
vectors are built from the columns at `cat_open` and never
stored.

---

## 6. What this changes above the bytes

- **`cat_open` is three scans.** `ns:`, `coll:`, `tbl:`. A
  table's columns arrive in attnum order because the keys do,
  and each index is followed by its own columns, so a
  definition is built in one pass with no counts to trust.
- **`ALTER TABLE ADD COLUMN` writes one key.** Not a rewrite
  of the whole table value. `DROP COLUMN` writes one key.
  `cat_set_index_state`, which the build calls when it ends,
  rewrites one index's key and leaves the table alone.
- **The schema lock now does all the serializing.** Today
  every DDL rewrites the `TABLE` entry, so two concurrent
  DDLs on one table also collide in sm1's write-write
  conflict. With the parts split, two sessions adding
  different columns to one table write different keys and
  collide nowhere — the IX schema lock is what stops them.
  That was already the mechanism of record (`CATALOG.md` §2,
  `SM3_DESIGN.md` §2), so the outcome is unchanged, but the
  belt is gone and only the braces are left. It belongs in a
  test.
- **`cat_index` can stay a scan.** It looks an index up by
  name through its table's indexes today, since no map of
  index names is kept; `name:` entries already carry the
  index's table oid, so the lookup is a `catkv_get`, and the
  scan goes away if that is wanted.
- **`cat_drop_table` erases more keys.** Its columns, its
  constraints, its indexes and their columns, by one scan of
  `tbl:<oid>:` and an erase per key, the way `cat_drop_index`
  already walks its keys because sm1 has no range delete.

---

## 7. Costs

- **More keys.** A twenty-column, three-index table is one
  key plus 20, plus 3 and their key columns — about 30 where
  today it is one. They are read in one sequential scan, so
  the cost is bytes rather than seeks.
- **A repeated prefix.** `tbl:<oid>:att:` is 25 bytes before
  the attnum. Ten thousand columns is roughly 300 KB of key
  bytes across the database, against sm1's 4096-byte key
  limit, which nothing here comes near.
- **Startup cost for thousands of tables is still
  unmeasured** (`CATALOG.md` §7). This layout makes it one
  scan instead of several and removes the blob framing from
  the path, which should help, but that is a prediction and
  not a measurement.

---

## 8. Open

- **`CATALOG2.md` §0's scope**, which §1 above leaves where it
  found it.
- **Bootstrap**: how the catalog's own schemas are known
  before it is loaded. With tagged fields a reader needs the
  tag tables of §5 and not a schema per part, which is a
  smaller thing to hard-code than it was, but it is still
  hard-coded.
- **What is inside a partition key and its bounds**, and
  whether sm3 reads either or treats both as the host's bytes
  the way it treats `eval`.
- **NULLS NOT DISTINCT, reloptions, and which constraint an
  index implements** (`CATALOG2.md` §5), left out rather than
  guessed at.
- **Owner, ACL and comment**, on every kind
  (`CATALOG2.md` §2 to §6). Each is a lowercase tag in the
  kind that owns it when it arrives, and needs nothing from
  this layout.
