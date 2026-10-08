# GLOSSARY — the words this stack uses

One place for the terms of art, with a pointer to where each
one is defined. The stack is layered and a word can change
meaning between levels, so §0 comes first: it lists the words
that mean more than one thing, which is where most of the
confusion lives. §1 does the same for the identifiers.

---

## 0. Words that mean more than one thing

**key.** Four things.

- a **userkey** — the caller's bytes that name a row in an
  sm1 store. It is what a version chain hangs off, what a
  lock locks, and what a scan orders by (`sm1/SM1_DESIGN.md`
  §3.1).
- an **index key** — the encoded key columns of an index
  entry, produced by sm2's `encode_key`. It *becomes part of*
  a userkey once the relid is prepended
  (`sm2/TYPE.md` §2).
- a **key column** — one column of an index definition, with
  its direction, null order and collation.
- a **catalog key** — a key in store 0 under `"U"`
  (`sm3/CATALOG3.md` §3).

**prefix.** Two things, both of them about position in a key
and neither of them an identifier.

- the **upper prefix** — the single byte `"U"`, the only part
  of store 0 a level above sm1 may use
  (`sm1/SM1_DESIGN.md` §11).
- a **scan prefix** — any leading byte string a range is
  derived from, its exclusive upper bound being that string's
  `successor`. **Prefix-free** (§7) is this sense too.

The 2 bytes at the head of every userkey in a table's store
are a **relid**, not a prefix; see **relation** below.

**relation**, **rel.** A store is a collection of rels: a
table's heap, one per index, one for TOAST. Each is named
within its store by a **relid**, a `uint16`, and nowhere
else: a rel is reached as its store and its relid, and has no
wider identity (§1, §8). PostgreSQL uses the word
the same way — `pg_class` holds tables, indexes and toast
relations alike, and `relkind` tells them apart — so a table
is *one* catalog object and *several* rels.

**schema.** Two things, and a third that is neither.

- sm2's **schema** — a vector of `attribute_t`, the physical
  layout of a row, with the `tup_desc_t` built over it.
- SQL's **`SCHEMA`** — a **namespace** in this stack's words:
  a container that scopes object names.
- a table's oid lock was once called its *schema lock*, in
  sm2's sense: a lock on the table's *definition*, and
  nothing to do with a namespace. It is the **table-oid
  lock** now (`sm3/CATALOG3.md` §0.4).

**version.** Three things: a **row version**, one entry in a
key's MVCC chain; the catalog's **format version**, in its
`meta` entry; and a collation's **ICU version**, recorded so a
changed library is detected.

**lock.** Three mechanisms, only the first of which is an
entry in a table.

- a **lock-table entry**, taken by `sm1::lock` in one of
  Gray's modes and held to the end of the transaction.
- an **uncommitted version**, which a write leaves behind and
  the next writer of the key waits on. A write keeps no
  lock-table entry (`sm1/WRITELOCK.md`).
- a **latch**, a short mutex hashed on store and key, held
  only inside a call, which makes check-then-write one step.

**index.** Two things: an sm3 **index**, an object that maps
an encoded key to a `rowno`; and "index" used loosely of a
lookup structure in the catalog — the **name index** and the
**dependency index** of `sm3/CATALOG3.md`, which are key
shapes and not objects.

**part.** Two things, one replacing the other: in
`sm3/CATALOG.md` §3 a part is one sm2 tuple inside a
hand-framed catalog value; in `sm3/CATALOG3.md` §0.1 a part is
a member of an object that has no oid of its own — a column,
an index key column.

**catalog.** Three things: sm1's **store 0**; the `"U"` region
of store 0, which is sm3's catalog; and the in-memory
`catalog_t` that `cat_open` builds and sessions share.

**name.** An object's name is unique within its namespace. A
**sequence name** is a bare string key under `"S"` in store 0
and belongs to no namespace. A **database name** and a
**column family name** are neither.

---

## 1. The identifiers, side by side

Six numbers name things, and none of the six is ever reused.

| id | width | scope | comes from | names |
| --- | --- | --- | --- | --- |
| `storeid` | uint64 | database | the objid lease in `CF:meta` | a store, and so its column family |
| `seqid` | uint64 | database | the same objid lease | a sequence |
| `oid` | uint64 | database | the sequence `sm3.oid` | a catalog object |
| relid | uint16 | one store | a constant, or that table's counter | a rel in that store |
| `rowno` | uint64 | one table | the sequence `sm3.rowno.<oid>` | a row in the heap |
| `attnum` | int32 | one table | the column's position | a column |

- **Never reused** means the same thing in every row: a
  reused id would name two things at once. A dropped store's
  column family outlives its `rmstore` until GC
  (`sm1/SM1_DESIGN.md` §11); a dropped index's relid is
  never handed out again, so its surviving entries cannot be
  read as a new index's; a dropped column keeps its attnum,
  so no later column shifts under a row already written.
- **Scope buys width.** A relid is 2 bytes because it
  only has to be unique within one store, and it sits in
  every key of every index. An oid would cost 6 more bytes
  per entry. It is the counterpart of PostgreSQL's
  `relfilenode`, scoped to one store rather than the
  database.
- **A table's heap is always relid 1**, a constant and not a
  number drawn from the counter, and **0 is unused**, so a
  zeroed relid is never a live rel. Which relid means what is
  written down once, in `sm3/SM3_DESIGN.md` §4.
- **An index therefore has two identities**: its **oid** in
  the catalog, and its **relid** in its table's store, with
  the catalog holding the mapping between them.
- `rowno` starts at 1, so `rowno = 0` names no row. It is the
  key of an entry in the heap rel, in the same place an
  index's encoded key sits in its own.
- Ids 1 to 999 are reserved in both database-wide spaces:
  sm1's objid lease starts at 1000, and so does `sm3.oid`,
  which leaves `pg_catalog` = 1 and `public` = 2 as fixed
  oids (`sm3/CATALOG.md` §1).

---

## 2. The stack

**level** — one of the four layers of `README.md`: RocksDB
(0), the MVCC storage manager `sm1/` (1), types and tuples
`sm2/` (2), schema and index maintenance `sm3/` (3), with the
C wrapper above them all. Each level is a client of the one
below, and no level reaches past its neighbour.

**the wrapper** — `include/sm.h` and `src/`, the one API the
host calls. Belongs to no level.

---

## 3. Level 1: stores and keys

**store** — sm1's unit of key space, named by a `storeid`.
Every store *is* a RocksDB column family, `CF:store.<storeid>`,
but nothing above level 0 calls it one.

**store 0** — sm1's own catalog, an ordinary MVCC store
holding a `"R"` row per store, a `"S"` row per sequence, and
whatever the level above keeps under `"U"`
(`sm1/SM1_DESIGN.md` §11).

**`CF:meta`** — the column family holding what is outside
MVCC: the object-id lease and a sequence's position.

**`CF:txn`** — the column family holding transaction status,
which a reader consults to turn a writer's `xid` into a fate.

**objid lease** — the allocator in `CF:meta` that storeids and
seqids both draw from, leased in blocks so an allocation is
not a write to a shared row.

**`max_keylen`, `max_datalen`** — 4096 bytes for a userkey, 64
MB for a value.

---

## 4. Level 1: versions and visibility

**row version** — one RocksDB key-value pair: the userkey
followed by the version tail, holding one value of that key.
There is no "row" object on disk, only a chain of versions
(`sm1/SM1_DESIGN.md` §3.1).

**version tail** — `~wts | ~seq`, 12 bytes, bitwise
complemented so that a key's newest version sorts first.

**`wts`** — the write timestamp a version was written at.

**`seq`** — a write's number within its own transaction, which
is what statement visibility and savepoint rollback are
expressed in; no command id is stored besides it.

**`xid`** — the transaction that wrote a version, in the
version's header while it is unstamped.

**`commit_ts`** — the timestamp a transaction committed at,
which replaces the `xid` in the header once a version is
**stamped** by compaction. An **unstamped** version's fate is
looked up instead.

**tombstone** — a version that records a delete.

**snapshot** — one number, `snapshot_ts`, taken as
`visible_ts`. A reader sees every version whose commit is at
or below it.

**`visible_ts`** — the largest timestamp below which every
commit is published, so that a snapshot never sees a gap
(`sm1/MVCC.md` §3).

**view** — what a read actually sees: its `snapshot_ts`, its
own `xid`, and the `seq` high-water mark that decides which of
its own writes are visible to this statement.

**commit cache** — an in-memory map from `xid` to `commit_ts`,
the fast answer for a recently committed transaction. Never on
disk, and empty after a restart (`sm1/COMMITCACHE.md`).

**watermark** — the oldest snapshot still in use. Garbage
collection may remove versions at or below it, and a dropped
store's column family goes once it passes.

**isolation level** — READ COMMITTED, REPEATABLE READ or
SERIALIZABLE. The scenarios in `sm1/mvcc/` are the
specification of what each must do.

---

## 5. Level 1: transactions

**savepoint** — a mark a transaction may roll back to.
`rollback(d)` drops the writes made at depth `d` or deeper;
`forget(d)` re-tags them one shallower, so a later mark that
reuses the depth cannot drop them.

**prepared transaction** — a transaction held across a
restart for two-phase commit, named by a **gid**, with its
locks saved and taken again by recovery.

**fail-stop** — the state a database enters when it cannot go
on safely; writes are refused from then on.

---

## 6. Level 1: locks and conflicts

**Gray's modes** — `IS`, `IX`, `S`, `SIX`, `X`, with sm1
keeping no hierarchy of its own: the intention modes lock a
key that *stands for* a larger object, and sm1 only applies
the conflict table on each key.

**write-write conflict** — two transactions writing one key.
The second waits for the first's uncommitted version, then
gets `SM_ERR_CONFLICT` at REPEATABLE READ and SERIALIZABLE, or
`SM_ERR_UPDATED` at READ COMMITTED, which does not abort.

**SSI** — serializable snapshot isolation, which is how
SERIALIZABLE is implemented: reads are tracked and cycles
refused, rather than readers blocking writers.

**SIREAD lock** — a record of what a serializable transaction
read: a point read records a key, a scan records the range it
actually traversed. Not a lock in the waiting sense
(`sm1/SIREAD.md`).

**range predicate** — the range half of a SIREAD lock, and
the **phantom** defence: a later insert into that range
conflicts even though no row was there at read time.

**rw-edge** — a read-write dependency between two
transactions, kept on both ends, out of which a cycle is
detected.

---

## 7. Level 2: types, datums and keys

**type code** — sm2's frozen number for a type, assigned once
and never reused or renumbered. Code 7, the retired
`decimal`, is never written (`sm2/TYPE.md` §3).

**typmod** — a type's modifier, such as a decimal's precision
and scale.

**datum** — one value of one column, in its **native**
encoding: what a tuple holds and what `cmp` compares.

**key encoding** — the same value turned into bytes whose
`memcmp` order is the value's order, because sm1 takes no
comparator. Every type has both encodings, and they differ
(`sm2/TYPE.md` §1).

**varlena** — the framing of a variable-width datum
(`sm2/VARLENA.md`).

**null marker** — the one byte every column's key encoding
begins with, saying null or not and in which null order, so a
nullability change never rewrites a key.

**prefix-free** — the property that makes a multi-column key
safe to concatenate with nothing between the columns: no
encoded datum may be a prefix of another. Variable-width
encodings get it from **escaping** — each `0x00` becomes
`0x00 0xFF`, then `0x00 0x00` terminates.

**sort key** — a collation's transform of a string, computed
once in the encoder, never at compare time. A **tie-break**,
the escaped original, follows it, because two distinct
strings can share a sort key.

**cut** — a secondary index key truncated to fit
`max_keylen`. A cut key keeps its `rowno` so cut keys that
meet stay apart; a unique index's key is never cut and fails
with `SM_ERR_KEYTOOLONG` instead.

---

## 8. Level 2: rows, relations and indexes

**tuple** — a row's bytes: a header, null bitmap and the
datums, held to 64 KB (`sm2/TUPLE.md`).

**`attribute_t`** — one column's physical description;
**schema** is a vector of them, and **`tup_desc_t`** the
layout built over it.

**deform** — take a tuple apart into one datum per column.

**rel** — sm2's name for a relation: one member of one sm1
store, named by its relid. A **heap** rel is keyed by
`rowno`; an **index** rel is keyed by its encoded key
columns. Both are entries in
a rel, differing only in what their keys hold.

**index entry** — one key-value pair of an index: the key is
the relid, the encoded key columns and, when the entry is not
uniquely keyed, the `rowno`; the value holds the `rowno`, the
cut flag and the **INCLUDE** columns.

**INCLUDE column** — a column carried in an index entry's
value, not in its key, so a scan can answer from the index.

**catkv** — sm2's key-value store over store 0 under `"U"`:
the level above's catalog bytes, which sm2 keeps and never
reads.

---

## 9. Level 3: the catalog

**object** — a thing with an **oid** and a name unique in its
namespace: a namespace, table, index, collation, constraint
(`sm3/CATALOG3.md` §0.1).

**part** — a member of an object with no oid, identified by
its position: a column, an index key column.

**namespace** — a named container that scopes object names;
SQL's `SCHEMA`. It stores nothing but its own name, which
itself sits in namespace 0.

**attnum** — a column's position in its table, 0-based. A
**dropped** column keeps its attnum and reads as null.

**missing value** — what a row written *before* a column was
added reads for that column. Not a `DEFAULT`, which serves an
insert that omits the column.

**table-oid lock** — a table's lock: one sm1 lock of type
`LOCKTYPE_OID` on the table's oid, taken in Gray's modes
before a transaction reads or writes the table's definition
and held to the end of the transaction. It is what makes an
in-memory catalog safe. The name it locks is a key in no
store, which nothing writes, so the lock only orders and
never fails as a row lock can (`LOCK.md` §4.2,
`sm3/CATALOG3.md` §0.4). A namespace has the same kind of
lock on its own oid. It was called the schema lock.

**overlay** — a session's own uncommitted DDL, tagged with the
savepoint depth it was made at, so the transaction sees its
changes and no one else does until it commits.

**publish count** — how many commits have published DDL,
counted in memory only, which is what lets a session decide
whether an index was valid before its snapshot.

**index state** — building, valid or failed. A failed index
keeps the error code its build failed with and that error's
message.

**eval blob** — a partial or expression index's predicate and
expressions as the host serialized them: bytes sm3 does not
read. Its **read columns** are the columns they use.

**referenced and dependent** — the two ends of a dependency.
The referenced object is the one a `DROP` is aimed at; the
dependent is the one that would be left broken
(`sm3/CATALOG3.md` §0.5).

**tag** — one ASCII byte naming a field in a catalog value,
frozen per kind. An uppercase tag is a **flag**, whose
presence is its truth and which always names the non-default
condition (`sm3/CATALOG3.md` §4).

---

## 10. Error codes

One namespace for the whole stack, defined in
`include/sm_error.h`.

| code | means |
| --- | --- |
| `SM_ERR_CONFLICT` | a write lost a write-write race; retryable, aborts |
| `SM_ERR_UPDATED` | the same at READ COMMITTED; does not abort — re-read and write again |
| `SM_ERR_DEADLOCK` | chosen as a deadlock victim; retryable |
| `SM_ERR_BLOCKED` | would have waited, under the no-wait test setting |
| `SM_ERR_DUPKEY` | an insert found the key already there |
| `SM_ERR_NOTFOUND` | no such store, key, or object |
| `SM_ERR_EXISTS` | the object is already there |
| `SM_ERR_INVALID` | the call's arguments cannot be honoured |
| `SM_ERR_NOTRANS` | no active transaction |
| `SM_ERR_PREPARED` | the session's transaction is prepared and does nothing else |
| `SM_ERR_KEYTOOLONG` | a unique index key past `max_keylen` |
| `SM_ERR_CORRUPT` | stored bytes contradict themselves |
| `SM_ERR_VERSION` | a format version this build does not read |
| `SM_ERR_NOMEM`, `SM_ERR_DISKFULL`, `SM_ERR_IO` | out of memory, out of space, the device failed |
