# CATALOG3 — the catalog's layout

`CATALOG.md` is the catalog **as built**: eight key kinds, and
values that are hand-framed sm2 tuples. A table's columns and
indexes have keys of their own there since format 2, as §3
below proposed; an index's own columns are still nested in its
value, and the keys are still sm2-encoded, not the words of §3.
§10 is the inventory this layout was drawn from: what a
catalog has yet to hold.

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
  being any. A table's oid lock is not even a key: it is an
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

### 0.4 A table's oid lock

**What it is.** One sm1 OID lock per table, taken in Gray's
modes before a transaction reads or writes that table's
definition and held to the end of the transaction: IS to read
it, IX to write rows under it, S for a plain index build, X
for DDL (`SM3_DESIGN.md` §2). It does not lock the table's
rows — sm1 orders writes to those itself. It is what makes an
**in-memory** catalog safe: the definition a transaction uses
is a pointer into shared memory, DDL replaces it at commit,
and this lock is the only thing stopping that replacement
from happening under a reader. Without it DDL has nothing to
wait on.

**It is not a key.** `sm1::lock` takes a **locktype**, and a
table-oid lock is `LOCKTYPE_OID`: a name in a namespace of its
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
- **Scope stays open.** §10.1 asks how far down
  `pg_catalog` the catalog is meant to go and says to decide
  that first. This layout is built so the answer changes §5's
  field tables and not the shape: a new object kind is a new
  key word, nothing nests inside anything else, and no enum
  has to widen. §5 fills in what `CATALOG.md` holds today,
  plus the eight gaps §10.3 numbers, five of them live bugs
  rather than missing features. A sixty-kind `pg_catalog`
  adds key words beside them and changes nothing above.

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

**A table-oid lock sits outside its table's range**, for the
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
§10.5's "whether that is caught is luck".

**The 64 KB part ceiling stops mattering.** A column's
missing value, a DEFAULT and a generation expression each sit
in their own key's value, bounded by sm1's 64 MB, so
`SM3_DESIGN.md` §2's worry about a wide table's defaults
overflowing one part is gone. That is the reason
§10.5 wants a key per column, and it is paid here.

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
(§10.2).

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

`k`, `a`, `h`, `j` and `b` are §10.3's items 1 and
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
are §10.3's items 4, 6 and 7. A dropped column
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

§10.3's items 2 and 3, and the second half of §10.5's
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

State 0 is pending, 1 valid, 2 failed (`SM3_DESIGN.md` §3);
absent reads as valid, which keeps `CATALOG.md` §3's rule, so
a pending or failed index writes `s` and a plain one does
not. `m` is §10.2's access method, true as "btree
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

A table's oid lock is not here: it is an
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
§10.2. `META` and `NAME` are no longer kinds at
all — they were key kinds and are key words now — and `LOCK`
is not even that: a table-oid lock is an sm1 locktype and stores
nothing. Only `name:` and `dep:` need a code for an object
kind.

Nothing in §10.4 is here: the sm2 `schema`, its
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
  `set_index_state`, which the build calls when it ends,
  rewrites one index's key and leaves the table alone.
- **The table-oid lock now does all the serializing.** Today
  every DDL rewrites the `TABLE` entry, so two concurrent
  DDLs on one table also collide in sm1's write-write
  conflict. With the parts split, two sessions adding
  different columns to one table write different keys and
  collide nowhere — the IX table-oid lock is what stops them.
  That was already the mechanism of record (`CATALOG.md` §2,
  `SM3_DESIGN.md` §2), so the outcome is unchanged, but the
  belt is gone and only the braces are left. It belongs in a
  test.
- **`cat_index` can stay a scan.** It looks an index up by
  name through its table's indexes today, since no map of
  index names is kept; `name:` entries already carry the
  index's table oid, so the lookup is a `catkv_get`, and the
  scan goes away if that is wanted.
- **`drop_table` erases more keys.** Its columns, its
  constraints, its indexes and their columns, by one scan of
  `tbl:<oid>:` and an erase per key, the way `drop_index`
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

- **§10.1's scope**, which §1 above leaves where it
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
  index implements** (§10.2), left out rather than
  guessed at.
- **Owner, ACL and comment**, on every kind
  (§10.2). Each is a lowercase tag in the
  kind that owns it when it arrives, and needs nothing from
  this layout.

---

## 9. The calls

The layout above is what the catalog stores. This is what it
is asked: the calls of `sm3/include/sm3.h`, each with one line
of what it does. Unlike the layout they are built, and
`CATALOG.md` §5 says how each works. `ss` is an sm3 session,
and a trailing `&` marks an output.

Every oid comes from the one sequence `sm3.oid`, so an oid
alone does not say what it names. Each argument below is
therefore named for the kind of object it must be:
`namespace_oid`, `table_oid`, `index_oid` or `collation_oid`.
The header's own parameter names are shorter — `oid`,
`nspoid`, `table`, `index` — and mean the same.

They are in two layers. The catalog's own calls, `cat_*`, are
a repository's — `get`, `list`, `put`, `del` — so its
interface reads as a REST API's would. What a statement does,
`create_*` and `drop_*` and the rest, is the DDL above it,
which calls the catalog.

### 9.1 Lifecycle

| Call | Synopsis |
| --- | --- |
| `cat_init(env, dbname, opt = nullptr)` | Makes a new database's catalog: format version, oid sequence, and the `pg_catalog` and `public` namespaces. Runs on its own session and transaction. Fails with `SM_ERR_EXISTS` if one is there. |
| `cat_open(env, dbname, cat&)` | Loads the whole catalog into memory and opens every collation. Runs on its own session and transaction. |
| `cat_close(cat)` | Frees the catalog and its collations. A catalog attached to the builder must be detached first. |
| `cat_set_evaluator(cat, eval)` | Gives the catalog the host's evaluator for partial and expression indexes. Set once. |

### 9.2 Session and transaction

| Call | Synopsis |
| --- | --- |
| `session_open(cat, s, ss&)` | Opens an sm3 session over the sm1 session `s`, with an empty overlay. |
| `session_close(ss)` | Drops the overlay and frees the sm3 session. Does not close `s`. |
| `session_sm1(ss)` | Returns the sm1 session under `ss`. |
| `session_cat(ss)` | Returns the catalog `ss` reads. |
| `begin(ss, level, read_only = false)` | Begins a transaction, noting how far the catalog had got before the snapshot. |
| `stmt_begin(ss)` | Begins a statement, which decides the indexes that statement may scan. |
| `stmt_begin(ss, outer&)` | Begins a statement inside another, saving the outer one. |
| `stmt_end(ss, outer)` | Ends an inner statement and resumes the outer one. |
| `savepoint(ss)` | Sets a savepoint; DDL from now on is tagged with its depth. |
| `rollback(ss, depth)` | Rolls back to a savepoint, dropping the DDL made since. |
| `forget(ss, depth)` | Forgets a savepoint, keeping its DDL. |
| `commit(ss)` | Commits in sm1, then publishes the transaction's DDL into the shared catalog. |
| `abort(ss)` | Aborts and drops the overlay. |
| `prepare(ss, gid)` | Prepares as sm1 does; refuses a transaction that made DDL. |

### 9.3 The catalog: get, list, put, del

The catalog is a repository: it stores a record, finds it, and
forgets it. It makes no store and no sequence, and fills no
index. Its reads return a definition, `table_def_t`, which is a
table resolved for use; its writes take a record,
`table_rec_t`, which is the table as stored.

| Call | Synopsis |
| --- | --- |
| `cat_new_oid(ss, oid&)` | Takes the next oid from the one sequence every kind of object shares. |
| `cat_get_namespace(cat, name, namespace_oid&)` | Returns whether a committed namespace of that name exists, and sets its oid. |
| `cat_get_namespace(ss, name, namespace_oid&)` | Returns whether a namespace of that name exists as the session sees it, and sets its oid. |
| `cat_list_namespaces(cat)` | Returns the committed namespace names, sorted. |
| `cat_list_namespaces(ss)` | Returns the namespace names the session sees, sorted. |
| `cat_put_namespace(ss, namespace_oid, name)` | Stores a new namespace. A taken name is `SM_ERR_DUPKEY`. |
| `cat_del_namespace(ss, namespace_oid)` | Erases an empty namespace. One holding a table or collation is `SM_ERR_INUSE`; the two built-ins are `SM_ERR_INVALID`. Waits out any put in it. |
| `cat_get_table(cat, table_oid)` | Returns a committed table's definition by oid, or null. |
| `cat_get_table(cat, namespace_oid, name)` | Returns a committed table's definition by name, or null. |
| `cat_get_table(ss, table_oid)` | Returns a table's definition by oid as the session's transaction sees it. |
| `cat_get_table(ss, namespace_oid, name)` | Returns a table's definition by name as the session's transaction sees it. |
| `cat_list_tables(cat, namespace_oid)` | Returns the names of the committed tables in a namespace, sorted. |
| `cat_list_tables(ss, namespace_oid)` | Returns the table names in a namespace as the session sees them, sorted. |
| `cat_lock_table(ss, table_oid, mode, def&)` | Takes the table's oid lock to the end of the transaction and returns its definition. |
| `cat_get_table_rec(ss, table_oid, rec&)` | Reads a table's record in order to change it: takes the table-oid lock exclusive and returns the newest record. |
| `cat_put_table(ss, table_oid, rec)` | Stores a table's record, new or replacing the old, and makes the names in it the names taken. A taken name is `SM_ERR_DUPKEY`; no such namespace is `SM_ERR_NOTFOUND`. |
| `cat_del_table(ss, table_oid)` | Erases a table's record and its names. Its store and sequence are the caller's to drop. |
| `cat_index(ss, namespace_oid, name, table_oid&, index_oid&)` | Finds an index and its table by name as the session sees it. |
| `cat_put_collation(ss, collation_oid, namespace_oid, name, locale, deterministic)` | Stores a new collation, recording the ICU version it opens at, and keeps it open. |

A table's columns and indexes are parts of its record, so they
have no put or del of their own: a caller changes the record
and puts the table.

### 9.4 DDL: create, drop, alter

Each is one statement's work, and a caller of the catalog: it
makes or drops what the object needs outside the catalog — a
store, a sequence, an index's entries — and puts the record or
dels it.

| Call | Synopsis |
| --- | --- |
| `create_namespace(ss, name, namespace_oid&)` | Takes an oid and puts the namespace (SQL's schema). |
| `drop_namespace(ss, namespace_oid)` | Dels the namespace. |
| `create_table(ss, namespace_oid, name, columns, table_oid&)` | Makes the table's store and rowno sequence and puts its record; takes both back if the put fails. |
| `drop_table(ss, table_oid)` | Dels the record and drops the store, with all rows and index entries, and the sequence. |
| `rename_table(ss, table_oid, name)` | Puts the record back under the new name. |
| `truncate_table(ss, table_oid)` | Empties the table by swapping in a new store; keeps columns, indexes and the rowno sequence. |
| `add_column(ss, table_oid, col, missing)` | Appends a column. Rows written earlier read it as `missing`, or as null if none is given. |
| `drop_column(ss, table_oid, name)` | Marks a column dropped; it keeps its place and reads as null. Refused if an index uses it. |
| `create_index(ss, table_oid, name, keys, includes, unique, index_oid&)` | Adds an index to the record as `pending` and queues it for the builder at commit. On a table made in the same transaction, fills it at once and records it `valid`. |
| `create_index(ss, table_oid, name, keys, includes, unique, eval, reads, index_oid&)` | The same for a partial or expression index: `eval` is the host's blob, `reads` the columns it reads. |
| `enqueue_pending_indexes(cat)` | Queues every pending index for the builder; called when a database is opened, for what a crash left unqueued. |
| `set_index_state(ss, table_oid, index_oid, state, error = SM_OK, reason = {})` | Sets an index to `pending`, `valid` or `failed`, keeping the error and message for a failed one. |
| `drop_index(ss, table_oid, index_oid)` | Erases the index's entries and takes it out of the record. |
| `create_collation(ss, namespace_oid, name, locale, deterministic, collation_oid&)` | Takes an oid and puts the collation. |

### 9.5 The builder, which is not the catalog

The builder fills pending indexes. It is one per process and a
caller of the catalog, not a part of it (`SM3_DESIGN.md` §3);
its calls are listed here because an index's life runs through
both.

| Call | Synopsis |
| --- | --- |
| `builder_attach(cat, open, batch_rows = 1000)` | Attaches a catalog to the process-wide builder, which from then on takes its indexes. It does not look for work. |
| `builder_detach(cat)` | Drops what is queued for the catalog and waits for its running builds to stop. |
| `builder_enqueue(cat, table_oid, index_oid)` | Gives the builder its work: puts a pending index on the queue. A duplicate or an unattached catalog is ignored. A worker checks the index is still pending, then fills it. |
| `builder_set_max_workers(max_workers)` | Sets how many builds run at once across all catalogs; default 4. |
| `builder_building(cat, index_oid)` | Returns whether a worker is filling the index now. |
| `builder_wait(ss, table_oid, index_oid)` | Waits outside a transaction until the index is valid, or returns the build's error. |

### 9.6 Not there yet

There is no call to rename a namespace, to del a collation,
or to get or list collations. `cat_index` keeps its older
name where the others would have it `cat_get_index`.

---

## 10. What a catalog has yet to hold

The layout above was drawn from an inventory: what a catalog
has to hold, kind by kind, and which of it sm3's holds. The
inventory was a file of its own, `CATALOG2.md`. What it held
that is already built is in `CATALOG.md`; what is left of it
is here — the things not held, and why each matters.

### 10.1 The question of scope

`PART4.md` §4 commits the catalog to **replacing**
PostgreSQL's heap catalogs, and names the cost as "the code
that reads the catalogs — the relation cache, syscache and
system-table scans". Read strictly, that is the whole of
`pg_catalog` in time: `pg_proc`, `pg_type`, `pg_operator`,
`pg_constraint`, `pg_depend`, `pg_amop` and the rest — some
sixty object kinds, not the handful here.

Nothing has decided how far down that list the catalog is
meant to go, and the answer changes the design rather than
sizing it. The gaps of §10.3 are a format change to one
subsystem. The whole `pg_catalog` surface is a different
shape entirely.

### 10.2 What is not held, kind by kind

| Kind | Not held today |
| --- | --- |
| catalog-wide | **bootstrap**: how the catalog's own schemas are known before it is loaded — open in `SM3_DESIGN.md` §2; PostgreSQL hard-codes its bootstrap catalogs |
| namespace | owner, ACL, comment |
| table | relkind (table, matview, partitioned); the partitioning strategy, key, bounds, and parent and child links; constraints — CHECK expressions, FOREIGN KEY columns, referenced table and actions, EXCLUSION; reloptions such as fillfactor; row-level security policies; triggers; rules; owner, ACL, comment |
| column | the **DEFAULT expression**, which is not `missing` — that only serves rows written before the column was added; a generated or identity expression and the sequence it owns; the per-column statistics target (`ALTER TABLE … SET STATISTICS`), which `STATISTICS.md` §3.3 needs; storage mode and compression method; comment, ACL |
| index | the access method — btree implicitly, which is true while `FEATURELIST.md` §2 marks only B-tree supported; the operator class of each key column, for which sm2's type and collation stand in; NULLS NOT DISTINCT; reloptions; which constraint it implements |
| collation | provider (ICU or libc), owner |

Persistence — permanent, unlogged, temporary — needs no
storing yet: a temporary table is never written to store 0,
and unlogged tables and matviews are unsupported.

Some objects have no kind at all:

| Object | Why it is needed |
| --- | --- |
| **sequence**, as an sm3 object | `CREATE SEQUENCE` needs an oid, a namespace, a name, start, increment, min, max, cache, cycle, and the column it is owned by. sm1's sequences are keyed by a bare name under `"S"` and carry none of it |
| **type** | domains (base type, constraints, default), **enums** (their labels and the order of them), composites, ranges. sm2's frozen type codes cover its built-ins only |
| **dependencies** | `CATALOG.md` §7's open item: refusing `DROP COLLATION` in use. `DROP … RESTRICT` and `CASCADE` need it; a namespace's drop gets by on a walk of the catalog in memory |
| **materialized view** | its storage and its refresh state; `FEATURELIST.md` §4 wants storage and refresh, no query semantics |

### 10.3 The gaps that are not speculative

Sorted by what is already true, since that is what decides
whether a gap blocks anything.

**A feature `FEATURELIST.md` marks supported has nowhere to
put its definition.** These are the ones that are wrong
today, not merely unbuilt.

1. partition strategy, key and bounds (range, list, hash)
2. CHECK expressions
3. FOREIGN KEY definitions and their actions
4. generated and identity column expressions
5. relkind, enough to tell a partitioned parent from a leaf,
   which item 1 needs anyway

Persistence is **not** on that list, though it looks as though
it should be, for the reason §10.2 gives.

**Something already written needs it.**

6. column DEFAULT expressions. `missing` is not it: `missing`
   serves rows written before a column was added, and a
   DEFAULT serves inserts that omit it.
7. the per-column statistics target, which
   `STATISTICS.md` §3.3 passes to `sm_analyze` and nothing can
   record.
8. dependencies, enough to refuse a `DROP` that would break
   one — `CATALOG.md` §7's own open item.

**Needed only once a feature arrives.** Listed to be designed
for, not to be built: an index's access method and the
operator class of each of its keys, which stop being implicit
with the first hash, GiST or GIN index; the objects with no
kind; and the owner, ACL and comment of every kind.

### 10.4 Derived, and not to be stored

Stated so a redesign does not persist them. `table_def_t`
holds them in memory and the store holds none:

- the sm2 `schema` vector and its `tup_desc_t`, built from
  the columns;
- an index's `key_column_t` and `attribute_t` vectors, built
  from its attnums and the column types;
- the count of publishes and the count at which each index
  became valid (`CATALOG.md` §4), which only compare against
  live snapshots, and no snapshot survives a restart.

### 10.5 What the inventory showed

**The table entry was where the first design broke.** Most of
§10.2's gaps hang off a table: partitions, constraints,
policies, triggers, defaults, generation expressions,
statistics targets. Under one value a table, each of them
would inflate the single value that every `ALTER TABLE`
rewrote and that had to stay inside the 64 KB a part is held
to (`sm2/TUPLE.md` §1). That was the argument for a row per
column and a row per constraint. The row per column, and one
per index, is built; the row per constraint waits for
constraints.

**Five gaps are live bugs, not missing features.** §10.3's
first group: a table can be partitioned, carry a CHECK or a
foreign key, or hold a generated column, and the catalog has
nowhere to write any of it down. Two more block work in
flight: the per-column statistics target, without which
statistics cannot honour `SET STATISTICS`, and a column
DEFAULT.

**The framing is the lesser problem, and the cheaper one.**
A value's parts carry no tag, and how many of each follows is
read from count fields in an earlier part. A table's counts of
columns and indexes went when those got keys of their own;
`nkey`, `ninclude` and `nread` in each index part remain
(`src/catalog/blob.cc`). A reader that has a count wrong reads
one kind of part as another, and whether that is caught is
luck. §4's tagged fields are what removes them.

---

## 11. An example: what one table takes

A table `orders` in `public`, with 1,000 columns and 10
indexes, as the catalog stores it today (`CATALOG.md` §2,
§3). Say the table got oid 1000, its indexes oids 1001 to
1010 in the order they were made, and its store is sm1's
store 7. `public` is namespace oid 2.

It takes **1,022 entries** in the catalog store:

| Kind | Entries | One for |
| --- | --- | --- |
| `TABLE` | 1 | the table's own fields |
| `COLUMN` | 1,000 | each column |
| `INDEX` | 10 | each index |
| `NAME` | 11 | the table's name and each index's name |

### 11.1 The entries

A key is a kind and then the kind's columns; a value is the
fields listed. `→` separates the two.

**`TABLE`**, keyed by the table's oid:

```
TABLE | 1000  →  namespace oid 2
                 name "orders"
                 storeid 7
                 rowno sequence "sm3.rowno.1000"
                 next index relid 12
```

It says nothing of the columns or indexes, not even how many
there are. The next index relid is 12 because the heap is
relid 1 and the ten indexes took 2 to 11; a relid is never
handed out twice, so it only goes up.

**`COLUMN`**, keyed by the table's oid and the column's
attnum, its 0-based place:

```
COLUMN | 1000 | 0    →  name "id", type int8, typmod none,
                        not null, not dropped
COLUMN | 1000 | 1    →  name "note", type text, typmod none,
                        nullable, not dropped,
                        missing value "n/a", collation oid 1500
…
COLUMN | 1000 | 999  →  name "c999", type int4, …
```

A column has no oid; the pair (1000, 3) is its whole identity.
Its entries sort by attnum, so a scan from `COLUMN | 1000`
returns the columns in the order rows are laid out by. A
dropped column keeps its entry, with `dropped` set: its place
is never given to another.

**`INDEX`**, keyed by the table's oid and the index's own:

```
INDEX | 1000 | 1001  →  oid 1001, name "orders_pkey", relid 2,
                        unique, state valid,
                        key columns: (attnum 0, ascending,
                                      nulls last)
INDEX | 1000 | 1002  →  oid 1002, name "orders_note", relid 3,
                        not unique, state pending,
                        key columns: (attnum 1, descending,
                                      nulls first,
                                      collation oid 1500),
                                     (attnum 0, ascending,
                                      nulls last)
                        include columns: attnum 2
…
INDEX | 1000 | 1010  →  …
```

An index is an object, so it has an oid, and its key carries
it. Its key columns, include columns and the columns its
predicate reads are parts of the index and live inside its one
value. Entries sort by index oid, which is the order the
indexes were made in.

**`NAME`**, keyed by a namespace's oid and a name:

```
NAME | 2 | "orders"       →  a table, oid 1000
NAME | 2 | "orders_note"  →  an index, oid 1002, of table 1000
NAME | 2 | "orders_pkey"  →  an index, oid 1001, of table 1000
…
```

### 11.2 What `NAME` is for

A `NAME` entry is how the catalog makes a name unique. That is
its job, and it is nearly its only one.

- **The key is the claim.** `NAME | 2 | "orders"` exists
  exactly while something in namespace 2 is called `orders`.
  Creating a table inserts it; a name already taken is the
  store's own `SM_ERR_DUPKEY`, with no check the catalog has
  to write.
- **It settles two sessions at once.** If two transactions
  create `orders` in `public` together, both insert the same
  key. The second waits for the first's uncommitted version,
  then fails with `SM_ERR_DUPKEY` if the first commits, or
  goes ahead if it aborts (`LOCK.md` §4.1). No lock is taken
  for this; the write is the lock.
- **One name space per namespace.** Tables, indexes and
  collations in a namespace all claim names with the same
  key shape, so a table and an index cannot share a name, as
  in PostgreSQL. A namespace's own name is claimed under
  namespace oid 0, which is no namespace:
  `NAME | 0 | "public" → a namespace, oid 2`. That is what
  makes namespace names unique in the database.
- **The value says who holds the name**: the kind of object,
  its oid, and for an index its table's oid. It lets the
  entry be read on its own.
- **A column has none.** A column's name need only be unique
  in its table, and the DDL checks that against the table's
  record. So the 1,000 columns add no `NAME` entries.

What it is **not** used for is finding things. Looking a table
up by name does not read `NAME`: `cat_get_table(…, nspoid,
name)` uses a map in memory, built when the catalog is loaded
from each table's own entry, which holds its namespace and
name. Finding an index by name walks the tables' indexes in
memory. The one thing read back out of `NAME` at load is each
collation's namespace, which a collation's own entry does not
record.

A `NAME` entry is kept in step by whatever changes the name's
owner. `cat_put_table` compares the names in the record it is
given with the names in the record it replaces: it inserts the
new ones first, so a taken name fails before anything is
changed, and then erases the ones no longer there. So:

| DDL | `NAME` entries |
| --- | --- |
| create the table | insert `orders` |
| create an index | insert the index's name |
| rename the table to `sales` | insert `sales`, then erase `orders` |
| drop an index | erase the index's name |
| add or drop a column | none |
| drop the table | erase `orders` and all ten index names |

### 11.3 What each DDL writes

| DDL | Entries written |
| --- | --- |
| create the table | 1 `TABLE`, 1,000 `COLUMN`, 1 `NAME` |
| create one index | 1 `INDEX`, 1 `NAME`, and the `TABLE` entry, for its next relid |
| an index's build ends | that 1 `INDEX`, for its state |
| add a column | 1 `COLUMN` |
| drop a column | that 1 `COLUMN`, marked dropped |
| rename the table | the `TABLE` entry, 2 `NAME` |
| truncate | the `TABLE` entry, for its new storeid |
| drop the table | all 1,022 erased, one by one |

### 11.4 The same table under §3's keys

§3 proposes spelling these keys as words. Nothing about the
example changes but the bytes:

```
tbl:00000000000003e8                          the table
tbl:00000000000003e8:att:0000 … :att:03e7     its 1,000 columns
tbl:00000000000003e8:idx:00000000000003e9     an index
name:0000000000000002:orders                  the name index
```

There the columns and indexes nest under the table's own key,
so one scan of `tbl:00000000000003e8:` returns the whole
table; today they are three kinds apart, read by three scans.
