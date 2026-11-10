# LSMINDEX: LSM Index

An LSM Index maps the key columns of a table's rows to the rows'
keys. Its entries are sm1 versioned keys in the table's own
store, so RocksDB's log-structured merge tree (LSM) orders them,
and sm1 gives them versions, uniqueness and recovery. This
document covers the key encoding as an index uses it, the entry,
cut keys, unique checks and scans. It also covers partial and
expression indexes, maintenance by table writes, and how an
index's kind is chosen. The code is `sm2/src/rel/index.cc` (every
path that is not a tree's), `sm2/src/type/key.cc`,
`sm2/src/type/collation.cc`, `sm3/src/table/table.cc`, and
`create_index` and `drop_index` in `sm3/src/ddl/ddl.cc`. The
other kind is the B-tree Index (BTREEINDEX.md). The Index Builder
(INDEXBUILD.md) fills an index on a table that has rows.

## 1. Overview

Four words have a fixed meaning here:

- An **entry** is one key-value pair of an index.
- A **row key** is what names a row in its table's store: the
  encoded primary key, or 8 bytes of `_id` in a table without one
  (ROWSTORE.md).
- The **key columns** are the columns of an index definition,
  each with its direction, null order and collation.
- The **host** is the program above the stack: digres, or `rel`
  in tests.

### 1.1 A worked example

```sql
CREATE TABLE jobs (id bigint PRIMARY KEY, state text NOT NULL, owner text);
CREATE INDEX jobs_state ON jobs (state) INCLUDE (owner);   -- relid 3, say
CREATE UNIQUE INDEX jobs_owner ON jobs (owner);            -- relid 4, say

INSERT INTO jobs VALUES (7, 'free', NULL);
```

One `tbl_insert` writes three keys in the table's one store.
Below, `enc()` is sm2's key encoding, and `‖` joins bytes.

| Rel | Key, after sm1's class byte | Value |
| --- | --- | --- |
| rows | `0001 ‖ enc(7)` | the tuple `(7, 'free', NULL)` |
| `jobs_state` | `0003 ‖ enc('free') ‖ enc(7)` | cut false, owner NULL, row key `enc(7)` |
| `jobs_owner` | `0004 ‖ enc(NULL) ‖ enc(7)` | cut false, row key `enc(7)` |

`jobs_owner` is unique, but this entry carries the row key,
because its key has a NULL (§4.2).

```sql
UPDATE jobs SET state = 'taken', owner = 'w1' WHERE id = 7;
```

1. `tbl_update` writes the row, and sm1 hands back the old image.
2. For `jobs_state`, sm3 erases `0003 ‖ enc('free') ‖ enc(7)` and
   inserts `0003 ‖ enc('taken') ‖ enc(7)`.
3. For `jobs_owner`, sm3 erases the NULL entry and inserts `0004
   ‖ enc('w1')`, with no row key. That key is now the unit of
   uniqueness.

An erase writes a tombstone version. A snapshot older than the
update still finds `('free', 7)`, and the Garbage Collector
(GC.md) drops it later.

```sql
SELECT id FROM jobs WHERE state = 'taken';
SELECT owner FROM jobs WHERE state = 'taken';
```

The first scan runs from `0003 ‖ enc('taken')` to that key's
successor. The host takes the row key from each entry and reads
the row. The second scan answers from the entry alone, since
`owner` is an INCLUDE column and the entry is MVCC.

### 1.2 Who does what

| Level | Its part |
| --- | --- |
| sm1 | keeps an entry's versions, refuses a duplicate key, waits for a writer of a key, tracks what SSI needs, recovers |
| sm2 | encodes keys, lays out entries, cuts keys, turns bounds into keys; it never learns what a table is |
| sm3 | keeps definitions, decides which entries a write changes, calls the evaluator, guards scans; it never encodes a key itself |
| host | derives bounds from `WHERE`, evaluates what is left, rechecks rows, re-checks a qual at READ COMMITTED, and evaluates partial and expression indexes |

## 2. Key encoding

The per-type encodings are in
[`sm2/TYPE.md`](sm2/TYPE.md#3-the-types) §3, and the byte rules
for every key in
[`sm2/TYPE.md`](sm2/TYPE.md#2-rules-that-apply-to-every-key) §2.
This section holds the rules they satisfy and why. A row key uses
the same encoder.

### 2.1 The rules

- **The comparison is `memcmp`, always.** sm1 takes no
  comparator, so the order must be in the bytes before sm1 sees
  them. So `encode(a) < encode(b)` bytewise if and only if `a <
  b` in the type's order.
- **Injective, not reversible.** Distinct datums get distinct
  keys, because a unique index works on key bytes: two datums
  that collide are one entry. A key need not decode back to its
  datum, because the tuple holds every column. `key_decode`
  exists for index-only scans alone, and is an optimization.
- **Two cases look like exceptions, and are not.** Where
  PostgreSQL calls two datums equal, they may share a key: `-0.0`
  and `0.0`. A cut key is deliberately not injective, and it is
  flagged and rechecked (§2.4).
- **Prefix-free.** A multi-column key is its columns joined with
  nothing between them. So no encoded datum may be a prefix of
  another that sorts after it.
- **Direction and null order are per column.** A descending
  column is the ascending encoding with its bytes inverted, since
  sm1 does not scan backwards. Baking in "ascending, nulls low,
  bytewise" would make half of SQL's `ORDER BY` clauses
  unrepresentable.
- **The encoder takes each column's description**,
  `key_column_t`: type, typmod, collation, direction and null
  order.
- **`decimal` needs nothing special.** It is a fixed-width
  two's-complement integer: flip the sign bit and write it
  big-endian. The schema fixes a column's scale, so the encoder
  never rescales.
- **`interval` keeps what PostgreSQL's order loses.** PostgreSQL
  flattens an interval to one span, which is not injective. The
  key is the span, then the months and the days, so a primary key
  cannot merge two datums. This order refines PostgreSQL's: it
  agrees wherever PostgreSQL is decisive, and breaks its ties
  ([`sm2/TYPE.md`](sm2/TYPE.md#310-interval) §3.10). The
  microseconds are not stored, because the span, months and days
  give them back.
- **One encoder, several callers.** sm2's `encode_key` makes row
  keys and entry keys alike, with the same per-column arguments.
  There are no two encoders to keep in agreement.

### 2.2 Framing

- **Every key column carries a null marker byte, `NOT NULL` or
  not.** Without it for a `NOT NULL` column, the key layout would
  depend on nullability. Then `SET NOT NULL` or `DROP NOT NULL`
  would rewrite every key in the table and in each index.
- **Variable-length datums are escaped and terminated.** Inside
  the datum `0x00` becomes `0x00 0xFF`, and `0x00 0x00` ends it.
  This keeps the encoding order-preserving and prefix-free.
  CockroachDB and FoundationDB use the same scheme.
- **The last column keeps its terminator.** Most entries end with
  a row key and some do not, so the encoder cannot see which
  column is last. The rule has no condition in it.

### 2.3 Text and collations

- **A text key is a collation sort key, then the original bytes
  as a tie-break.** A deterministic collation can give two
  distinct strings one sort key, and PostgreSQL keeps them as two
  datums. A key that stopped at the sort key would merge two
  rows. The layout is in
  [`sm2/TYPE.md`](sm2/TYPE.md#35-text-varchar-bpchar-name) §3.5.
- **The collation is an input to the encoder**, not a property of
  the bytes. The locale-aware work runs once, in the encoder,
  through ICU's `ucol_getSortKey`. `C` and `POSIX` mean byte
  order: the key is then the escaped bytes alone.
- **ICU's sort key spends key bytes.** It runs several times the
  length of its text, so `sm1::max_keylen` is reachable. Its
  output holds no embedded `0x00` but its end, which is why the
  `0x00` framing can wrap it.
- **ICU is the source of sort keys, and is meant to be pinned.**
  Sort keys are not stable across library versions, and
  PostgreSQL has had index corruption from that. For now sm2
  links the system's ICU; pinning is left to packaging.
- **A collation opens by locale**, a BCP 47 tag such as `en-US`
  or `und-u-ks-level2`. A language ICU does not know is refused,
  as PostgreSQL's `icu_validate_locale` refuses it, because ICU
  itself would sort it quietly by its root order.
- **One collation serves every session.** This rests on ICU's
  documentation that const operations on a collator are safe
  across threads. It is not yet checked against ICU's source.
- **A version mismatch is refused.** The catalog records ICU's
  collator version with each collation. `cat_open` opens each
  collation with that version, and a different ICU fails
  `sm2::collation_open`, so the whole catalog does not open.
  PostgreSQL only warns, but new sort keys in an old index
  corrupt it further, and a unique index quietly stops being
  unique. No command rebuilds an index under a new version, since
  none can run on a catalog that does not open. digres uses no
  collation but C, which has no version.
- **Non-deterministic collations are accepted**, for ICU alone.
  Byte order is refused as non-deterministic, as PostgreSQL
  refuses it for libc. The `deterministic` flag is set at
  creation and never changes.
- **Under a non-deterministic collation the tie-break goes.** The
  key is the sort key, then `0x00 0x00`. Strings ICU calls equal
  share a key. So a unique index refuses `'ABC'` after `'abc'`
  under a case-insensitive collation, and an equality bound finds
  both in a non-unique index. These are PostgreSQL's answers,
  with no machinery of sm2's own.
- **Such a key holds no string.** Its `key_decode` fails with
  `SM_ERR_INVALID`, and `key_decodable` says so first. A `bpchar`
  key is the same: the type has no `key_decode`. A scan reads the
  row instead.

### 2.4 Cut keys

A **cut** key is a key truncated to fit `sm1::max_keylen`, 4096
bytes. That limit bounds the whole userkey: the 2-byte relid, the
encoded columns, then the row key where the entry carries one. A
row key is 8 bytes of `_id`, or a primary key's encoding of at
most `max_pk_len`, 1024 bytes. A row key past that is refused, so
only an entry's columns can be cut.

| Entry | Columns' budget |
| --- | --- |
| no row key: a unique index, no NULL | 4094 |
| ends with a row key, keyless table | 4086 |
| ends with a row key, keyed table | 3070 |

- **A non-unique index cuts.** Truncation keeps order as a
  coarsening: if `a <= b` then `trunc(a) <= trunc(b)`. So a range
  scan over cut keys returns a superset, and a recheck against
  the row removes the extras (§5.2).
- **The entry is flagged.** Its value says cut, because the index
  no longer promises order among entries that share the prefix.
- **A key is cut at one length**, the budget of the longest row
  key the table allows. So a cut bound orders against cut keys as
  the whole bound orders against whole keys.
- **A cut key keeps its row key**, so cut keys that meet stay
  apart.
- **A cut key never equals a whole key.** Whole keys are
  prefix-free, so a flagged entry is never mistaken for an exact
  one.
- **A unique index never cuts.** A key past its budget fails with
  `SM_ERR_KEYTOOLONG`, whether or not a column is NULL (§4.3).
- **A key is never hashed.** A hash keeps equality and destroys
  order. An index that cannot serve a range or an `ORDER BY` is
  not worth having.

## 3. Entries

### 3.1 The relid

An index is one rel of its table's store, named by a 2-byte
relid. The relid values are written down in CATALOG.md. An index
has `0x0002` to `0xFFFD`. sm3 picks the relid and records it in
the catalog, and sm2 prepends it and otherwise ignores it.

- **Relids rise and are never reused.** A dropped index's
  versions linger until GC passes them. With no reuse, no later
  index reads them as its own, and no rule depends on the GC
  horizon. The ceiling is about 65,500 index creations a table,
  rebuilds included.
- **An index's two identities** are its oid in the catalog and
  its relid in its store. The catalog holds the mapping.
- **Every key below `0xFFFE` has a successor**, which a scan's
  bound needs. `idx_open` refuses a relid at or above
  `sm2::id_relid`.
- **A relid is fixed-width**, so no rel's relid starts another's.

### 3.2 The layout

```
userkey      =  relid (2B BE) ‖ encode(key columns) [‖ row key]
RocksDB key  =  0x00 ‖ userkey ‖ ~wts (8B) ‖ ~seq (4B)
RocksDB value = flags, xid or commit_ts ‖ tuple(cut, INCLUDE columns, row key)
```

The class byte `0x00` marks a versioned key. A table's store is
chained, but its entries stay versioned: only the rows chain
(ROWSTORE.md). `~wts ‖ ~seq` puts the newest version first.

- **The key carries the row key** always in a non-unique index,
  and in a unique one only for an entry with a NULL key column. A
  non-unique index needs nothing else: the row key makes each
  entry unique.
- **The value is an sm2 tuple**, laid out by `idx_layout_t`: a
  bool saying the key was cut, the INCLUDE columns, then the row
  key as a `bytea`. The row key is in the value of every entry,
  so a unique entry can name its row. A keyless row's `_id`,
  which a scan hands out, is read from that row key.
- **An entry is held to 64 KB**, the tuple limit. A longer one
  fails with `SM_ERR_ROWTOOLONG`, as PostgreSQL refuses an index
  row too long for a page.
- **An entry holds no `_id` of its own.** A keyed row has none
  (ROWSTORE.md).
- **The layout is made once per definition.** `idx_open_layout`
  borrows it, so opening a table builds no descriptor.

### 3.3 The primary key

The primary key keys the rows (ROWSTORE.md). It is an index in
the catalog, so the host proves uniqueness with it and plans
scans of it, but it has no entries.

- A scan of it is a scan of the rows. `idx_open_primary` opens
  it, and `idx_scannxt` makes an entry's value from each row.
- Its uniqueness is sm1's `SM_ERR_DUPKEY` on the row key.
- Its columns are `NOT NULL`, and it has no predicate and no
  expression. It is never a tree. `create_index` refuses each of
  these.
- A write to it fails: `need_entries` refuses.
- Its relid is recorded and holds nothing.
- Adding or dropping one on a table with rows rewrites the table
  into a new store (§7.9).

### 3.4 What sm1 gives an index

- **Unique enforcement.** An insert fails with `SM_ERR_DUPKEY`,
  and a second inserter of a key waits for the first (§4.1).
- **Halloween protection.** A cursor's view is fixed at open, so
  updating an indexed column during a scan of the index does not
  revisit moved entries.
- **Scan the index, then fetch the row.** A read does not
  invalidate a cursor's current entry, which lives until the next
  call on that cursor.
- **Undo.** A statement's partial writes undo to its savepoint,
  and a failed write leaves the transaction active. sm2 keeps
  nothing across calls, so rollback costs it nothing.
- **Isolation.** An entry is an sm1 key, so write conflicts and
  SSI's read ranges apply to it as to any key.

## 4. Unique checks

sm3 makes no uniqueness check of its own for an LSM Index. A
unique tree has a check of its own (BTREEINDEX.md).

### 4.1 The key is the unit

An entry of a unique index with no NULL key column is `relid ‖
encode(key columns)`, with no row key. So one key means one row,
and sm1's insert does the rest:

1. The first inserter finds the key absent and writes an
   uncommitted version.
2. A second inserter finds that version and waits for its
   transaction.
3. If the first commits, the second fails with `SM_ERR_DUPKEY`.
4. If the first aborts, the second succeeds.

sm1 judges an insert against the newest committed version, not
the snapshot. A snapshot read could not see the first writer.
Above READ COMMITTED, a key committed after the snapshot fails
with `SM_ERR_CONFLICT`. LOCKMGR.md has the race. No lock-table
entry is taken. `add_entry` names the index in `sm1::errdup()`,
for the host to say it, and `row_failed` does so for the primary
key.

### 4.2 NULLs are distinct

Two NULLs encode to the same bytes. So an entry with a NULL key
column carries the row key, and two such entries never meet, as
SQL requires by default. `NULLS NOT DISTINCT` is refused: digres
refuses it in `check_create_index()`, and sm3 has no such flag.
Adding it later is a catalog flag and changes no bytes on disk.

### 4.3 A unique key is never cut

Two different originals can share a cut key's bytes. A check
would then have to compare originals over the prefix range. At
READ COMMITTED that means reading a range at its newest committed
version, and nothing in the stack does that. `sm1::read_latest`
reads one key under a lock already held, and a range has no
single key to lock. So an over-long key refuses the write with
`SM_ERR_KEYTOOLONG`, as PostgreSQL refuses an oversize B-tree
entry. The primitive is built when a workload asks for it.

### 4.4 Equal means the collation's equal

Under a non-deterministic collation, strings ICU calls equal
share a key, so uniqueness and equality bounds follow it with no
work here. A type's `cmp` stays byte order. So whatever compares
strings above sm2 must compare them under the collation: a
recheck, hashing, grouping and `DISTINCT`. PostgreSQL's
`hashtext` hashes the sort key for the same reason.

### 4.5 An insert with arbiters

`tbl_insert_arbiter` is SQL's `INSERT ... ON CONFLICT`. It
inserts the row unless a row has its key in an arbiter, a unique
index the caller names. With no arbiter named, every unique index
is one. A row a partial index does not take has no key in it. A
failed index is skipped.

1. Look each arbiter's key up with `idx_find`, at the snapshot.
   The primary key is looked up last.
2. If a row is found, return 1, with nothing locked or written.
   At READ COMMITTED, first confirm the row with `find_now`: the
   snapshot may show a row deleted since, and a caller that came
   back for it would be shown it for ever. A row gone since is no
   conflict.
3. Draw a keyless row's `_id`.
4. Add the row's entry to each arbiter, then write the row, which
   is the primary key's entry. sm1's insert waits for an
   unfinished writer of the key, and says at once if a row is
   there.
5. If one of these meets a duplicate, remove the entries added so
   far. Find the row with `find_now`, and return 1 with it.
6. If `find_now` finds the row gone, start again at step 1.
7. Add the row's entry to every index that is no arbiter. A
   duplicate there is `SM_ERR_DUPKEY`, as in `tbl_insert`.

`find_now` reads a key's newest version:

1. Set a savepoint.
2. Find the row with `idx_find_latest`, which locks the key
   shared.
3. If the caller wants the tuple, lock the row shared and read
   its newest version.
4. Roll back to the savepoint, which gives the locks back at
   once.

The caller must hold nothing of the key, or it would stand
between the row's owner and the row. The lock on a key is the
entry of the row inserted, as in any insert.

- Above READ COMMITTED, a key taken since the snapshot fails the
  call with `SM_ERR_CONFLICT`.
- The row found is not locked. A caller that then finds it gone
  calls again.
- A tree arbiter's entry goes in after the row, under a savepoint
  (BTREEINDEX.md).

## 5. Scans

A scan takes key bounds and filters nothing. sm3 has no
predicate, and so no predicate scan.

### 5.1 Bounds

A bound holds the values of the index's leading columns, one or
more, NULLs allowed, and says whether an equal entry is inside.
Bounds are in the index's own order, after each column's
direction and null order. So for a descending column, `x > 5` is
an upper bound. Mapping a predicate onto that order is the
host's.

A bound becomes an sm1 key through prefix-freeness. The encoding
P of the first j columns begins exactly the keys whose first j
columns hold those values, and those keys are contiguous. So a
bound is P or its **successor**, the least string above every key
that starts with P.

| Bound | sm1 key |
| --- | --- |
| lower, inclusive | P, a start key sm1 includes |
| lower, exclusive | successor of P |
| upper, inclusive | successor of P, an end key sm1 excludes |
| upper, exclusive | P |
| none | the index's relid, or that relid's successor |
| cut, either side | as inclusive, and `widened` is set |

A bound too long to encode is cut where keys are cut. That can
only widen the scan, and the scan says so through `widened`. Keys
past the whole bound may start with the cut one, so a cut bound
is never left out.

### 5.2 The recheck

A scan may hand out rows that do not meet the query, for three
reasons: an entry was cut, a bound was cut, or a bound was wider
than the query's. The recheck is the caller's, against the row,
which holds the original. sm3 hands out the flags (`cut`,
`widened`) and compares nothing. digres rechecks every key on
every row of an LSM Index, so it reads neither flag
(`keys_met()`, `smstore.c`). Under a non-deterministic collation
the recheck must compare under the collation (§4.4).

### 5.3 Index-only scans

An entry is MVCC, so a scan needs no row to decide visibility.

- `idx_scan_key` decodes the key columns past the relid. A cut
  key, or a column whose key keeps no datum (§2.3), returns 1,
  and the caller reads those columns from the row.
- `idx_scannxt` hands out the value tuple, for the INCLUDE
  columns. `tbl_idx_value_schema` gives its schema.
- The entry never holds a TOAST pointer (§7.8), so an index-only
  scan never hands one out.

### 5.4 The calls

- `tbl_idx_scan` opens a cursor over entries between two bounds.
  It refuses an index the session may not use (INDEXBUILD.md).
- `tbl_idx_scan_rows` is a range scan of the rows for a primary
  key, in key order. It takes a tree too (BTREEINDEX.md).
- `tbl_scan_rows` is every row in key order, the order a
  sequential scan reads in. On a keyless table it is `tbl_scan`,
  by a range of `_id`. `tbl_scan` refuses a keyed table.
- `tbl_read_key` reads a row by the row key an entry names, and
  `tbl_read_keys` reads many in one call
  ([`ROWSTORE.md`](ROWSTORE.md)).
- `idx_scan_seek` moves a cursor forward to a new lower bound.
  Moving back fails.
- `tbl_idx_size` gives an index's bytes under its relid. A
  primary key adds nothing to the rows' bytes.

### 5.5 What a scan leaves to the host

- **Deriving bounds is the planner's.** It matches the index's
  columns against the conjuncts, and gets a start key, an end key
  and a residual. The kept planner does this already.
- **The residual is evaluated above.** sm3 evaluates nothing, so
  a filter pushed down would be a call back into the host for
  each row. The rows that failed would cross the API all the
  same.
- **A filter on one column does not decode a wide row.** sm2
  decodes a tuple by column, and the C API hands that through.
- **A scan is tuple at a time.** Sets of rows were forced by an
  IPC boundary that the threaded fork removed.
- **A scan is forward only.** An `ORDER BY x DESC` that an index
  could serve backwards is a top-N sort over a forward scan (§9).

## 6. Partial and expression indexes

A **partial** index holds only the rows its predicate takes. An
**expression** index has a key column that is an expression over
the row. One index may be both. digres's side is in
[`../dg/EXPRINDEX.md`](../dg/EXPRINDEX.md).

sm3 evaluates neither, and is to get no evaluator. The catalog
keeps the predicate and the expressions as one opaque blob an
index, the **eval blob**, serialized as the host likes. The host
registers one **evaluator**, a callback. Given a table, an index
and a row as values, it says whether the row belongs and gives
the expression keys.

The results are PostgreSQL's by construction: NULLs, collations,
coercions and every function. No second implementation could
promise that. This is the host's job and not a wrapper's, because
the fork is threaded and rewrites PostgreSQL's functions for
threads. How they report an error, and whether they need a
backend, are choices made as they are ported.

### 6.1 The evaluator's rules

- **No `longjmp` out of it, and no C++ exception.** Either would
  unwind through sm3's frames. It runs inside a write and on
  build workers, where a throw has nowhere to go, so one that
  throws ends the process. The C API's type is `noexcept`. sm3's
  is a `std::function`, which cannot say so, and `ask_evaluator`,
  which is `noexcept`, calls it.
- **Failure is a return code, with the error set.** The write
  fails with that error. A build fails too, and keeps it as its
  reason. In the C API the host fails with `sm_seterr`.
- **It is deterministic.** The same row image gives the same
  answer, or a later delete computes another key and misses the
  entry. PostgreSQL asks the same: an index's functions must be
  immutable.
- **It may be called from any thread**, build workers among them,
  where no executor runs.
- **It does not call the storage manager on the same session**,
  which is in the middle of a write.

### 6.2 What it computes

sm3 calls it wherever it computes an entry: on an insert, on the
old and new image of an update, on a delete's old image, in a
build, in a load, and in a rewrite. `ANALYZE` calls it too.

- **It computes only what sm3 cannot:** whether the row belongs,
  and the values of expression key columns. A plain key column
  sm3 reads from the row, so a partial index over plain columns
  costs one yes or no an image. INCLUDE columns are plain, and
  need none of this.
- **It sees a row as values and null flags**, one a column, and
  not as a tuple. A write has the new row that way, sm3 deforms
  the old image anyway, and the host needs no decoder. A
  by-reference value is good only for the call.
- **It sees values, never a TOAST pointer** (§7.8).
- **The values it hands back are owned by whoever asked.** It
  fills a `std::vector<std::string>`, one string for each
  expression column, each the bytes `sm2::datum_bytes` gives, a
  varlena for a variable-width type. A flag beside each says
  NULL. sm3 holds an old image's values and a new one's together
  when it compares them, and no rule says how long the host's
  memory lives. The cost is one copy of each value a call.
  `std::string` and `std::vector` are the two C++ types that may
  cross `include/sm.h` with owned data.
- **An expression column has no table column behind it.** Its
  `attnum` is -1, and the catalog records its type, typmod and
  collation, given at `create_index`.
- **`make_entry` checks a value's width** against the column's,
  and refuses a wrong one with `SM_ERR_INVALID`.

### 6.3 Registering it

The host registers the evaluator once an environment, with
`sm_set_index_evaluator`, before a database opens. Opening one
attaches it to the Index Builder, which may at once resume a
build. In sm3 the call is `cat_set_evaluator`. With none
registered, `create_index` refuses such an index, and a write to
its table or a build of it fails with an error that says so.
`rel` has an evaluator of its own, for a column equal to a value
and for `lower` and `upper` of a text column.

`create_index` takes `eval` and `reads`. `reads` lists the
columns the predicate and the expressions read, and needs an
`eval`. A primary key or a constraint's index has no `eval`. A
column an index reads cannot be dropped (`drop_column`).

## 7. Maintenance by table writes

### 7.1 One call writes a row and its indexes

The host writes a table through a table-level call that sm3 owns:
`tbl_insert`, `tbl_insert_arbiter`, `tbl_update` and
`tbl_delete`, on a handle from `tbl_open`. The call changes the
row, then every index the definition lists, whichever its kind.
It is the only way to write a row through the C API, so no caller
can change a row and leave its entries behind. A handle is good
for one transaction, in the mode of its table-oid lock.

sm3 drives maintenance, and the executor owns the qual. The kept
executor could have written entries through its index access
method. It does not, because a unique key's rules, the online
build, and rows and entries that never disagree are sm3's whoever
writes the entries. The last holds only if one layer writes both.
What stays above is `EvalPlanQual` (§7.7).

### 7.2 A row is named by its key

`tbl_update`, `tbl_delete`, `tbl_read`, `tbl_read_latest`,
`tbl_lock` and `tbl_trylock` take a `row_key_t`: the primary
key's values, or a keyless row's `_id`. The calls by `_id` are a
keyless table's, and refuse a keyed one. `tbl_row_key` encodes a
key, to tell two names apart. A call by key encodes it and reads
nothing more (`named()`). `tbl_insert` and `tbl_insert_arbiter`
give 0 as a keyed row's `_id`, and the arbiter hands back the
conflicting row's tuple, from which the caller takes its key.

### 7.3 The writes

The row always comes first. INDEXBUILD.md relies on that order.

`tbl_insert`:

1. Refuse a NULL in a `NOT NULL` column.
2. Write the row, which gives its row key.
3. For each maintained index: fetch what it needs out of line,
   make the entry, and add it if the row belongs.

`tbl_update`:

1. Refuse a NULL in a `NOT NULL` column.
2. Take a pin for the old image, and write the row with hold. sm1
   hands back the old image, and sm2 keeps what the old row holds
   out of line.
3. Deform the old image. It is a tuple, so sm3 decodes no bytes
   itself.
4. For each maintained index, skip it if the entry cannot have
   changed (§7.4).
5. Otherwise make the entries of both images. Remove the old one
   if the row belonged, and add the new one if it belongs.
6. Release what the old row held out of line.

`tbl_delete` is the same with no new image. It removes the old
entry from each maintained index.

- **Maintained** means every index but a failed one and the
  primary key (`maintained()`). A pending index is maintained,
  and a missing entry there is no error (`remove_entry`).
- **The old image views sm1's session memory.** The index calls
  that follow are calls on that session, so the write pins the
  image, in a pin of the call's or the caller's (ROWSTORE.md). A
  later index's old key is read from where the image is, and sm3
  copies nothing.

### 7.4 When an update leaves an index alone

An update keeps the row's key, so only indexes whose columns
changed are touched. `unchanged()` asks three things:

1. Do the key columns compare equal by their types' `cmp`?
2. Do the INCLUDE columns hold the same bytes?
3. Do the columns in `reads` hold the same bytes?

If all three answer yes, sm3 leaves the index alone and does not
call the evaluator. The tests differ on purpose. The key folds
values the type calls equal, a float's `-0` and `+0`, so `cmp`
decides whether the key moved. An INCLUDE column is stored as
written, so an entry skipped on `cmp` alone would keep `-0`
beside a row that says `+0`. What the host makes of a column it
reads is the host's, so only bytes count.

If a column in `reads` changed, sm3 evaluates both images. If
both belong with equal keys and the same INCLUDE bytes, the entry
stays.

### 7.5 A row that moves

An update that changes the primary key moves the row
(ROWSTORE.md). Every entry names the row by its key, so each is
removed and added again, under the new row key. No index is left
alone. The old key's erase is sm1's moved tombstone. So a writer
at READ COMMITTED that finds the row moved since its snapshot is
not led after it: its newest read at the old key fails with
`SM_ERR_MOVED`.

### 7.6 A failed write

**WARNING: a write sets no savepoint.** One that fails after its
first step leaves the earlier steps done. A duplicate in a unique
index, or an over-long key, leaves the row and the indexes
disagreeing inside the transaction. The caller rolls back to a
savepoint of its own, or aborts, before going on.

- sm1 expects this of a layer that makes one row write into
  several. A mark for each row was the alternative (§12).
- `SM_ERR_UPDATED` needs no rollback. It comes from the row's
  write, the first step.
- **A write refuses a NULL in a `NOT NULL` column** with
  `SM_ERR_NOTNULL`, before the row's write, so nothing is half
  done and the transaction goes on. It is the one constraint sm3
  enforces. The check is exact, it costs a flag a column, and sm3
  leans on it: a primary key column is taken never to be NULL.
- **The `varchar(n)` limit is not checked here.** It is a
  coercion with PostgreSQL's own rules, trimming spaces and
  counting characters, so it is the host's.

### 7.7 Read committed

An update or delete that meets a row committed after the
transaction began fails with `SM_ERR_UPDATED`. That does not
abort, and it is not retryable. Resolving it takes two jobs that
need different knowledge:

| | Needs | Whose |
| --- | --- | --- |
| (a) decide whether the row still qualifies, then write or skip | the statement's qual | the executor's |
| (b) remove the entries that are really there | the newest old image | sm3's |

sm1 forces the re-read and can do no more, as it has no predicate
(XCTMGR.md). When the row's write is refused, sm3 has touched no
index, and it returns the status as an outcome. The executor
reads the newest image, re-checks its filter, and repeats the
write or skips the row. sm1 refuses a write repeated without the
read, so an executor cannot forget. An insert has no filter, and
needs none of this.

**Why (a) is above sm3.** Two workers claim one job, on a table
with no index:

```sql
-- row 7: state='free', owner=NULL
UPDATE jobs SET state='taken', owner=:me WHERE state='free';
```

1. Both statements take a snapshot in which row 7 is `free`.
2. T1 writes row 7, and T2's write waits for T1.
3. T1 commits, and T2's write is refused with `SM_ERR_UPDATED`.
4. T2 reads the latest image: `state='taken'`.
5. T2 re-evaluates `state='free'`, finds it false, and writes
   nothing.

Without step 5 the job is claimed twice and nothing raises an
error. A filter may also span tables, so no image of this row
alone could decide it:

```sql
-- orders: row 42 = (id=42, cust_id=5, status='new')
-- customers: row 5 = (id=5, region='EU'), row 9 = (id=9, region='US')
UPDATE orders o SET status='hold' FROM customers c
 WHERE o.cust_id = c.id AND c.region = 'EU';
```

- **The order row changes.** T1 sets `cust_id=9`. T2 is refused,
  reads `(42, 9, 'new')`, and re-runs the join against the
  customer row it had joined, `(5, 'EU')`. `9 = 5` is false, and
  it skips the row. The qual needed a row of another table.
- **Only the customer changes.** T1 sets customer 5's region to
  `'US'`. Order 42 was not touched, so T2's write succeeds at
  once, on `'EU'` as its snapshot saw it. Only a conflict on the
  row written triggers a re-check.
- **The order changes and still qualifies.** T1 sets
  `status='shipped'`. T2 is refused, reads `(42, 5, 'shipped')`,
  finds the join holds, and writes again. sm3 removes
  `('shipped', 42)` and inserts `('hold', 42)`.

The re-check uses the customer row already joined, not the
newest. That is `EvalPlanQual`'s treatment of tables a statement
reads and does not lock. PostgreSQL gives such a table
`ROW_MARK_REFERENCE` (`select_rowmark_type()`). It fetches the
row by the ctid the scan joined, with `SnapshotAny`
(`EvalPlanQualFetchRowMark()`): that version, whatever has
committed since. digres has no ctid, so it marks the table
`ROW_MARK_COPY` and reads a copy of that row, the same version.
`dg/test/server.sh` checks it, with a table joined to itself too.

**Why (b) is sm3's.** T2 holds two old images: the one its scan
read at its snapshot, and the one the repeated write replaces.
Computing the entry to remove from the first is the bug.

```sql
-- row 7: state='free', index entry ('free', 7)
UPDATE jobs SET state='taken', owner='w1' WHERE state='free'; -- T1
UPDATE jobs SET state='done' WHERE state <> 'done';           -- T2
```

From the snapshot image, T2 would remove `('free', 7)`, which T1
already replaced, and insert `('done', 7)`. The index would then
hold two entries for one row. This happens instead:

| Step | Layer | What happens |
| --- | --- | --- |
| 1 | executor | T2's scan reads `(7, 'free', NULL)`, and builds `(7, 'done', NULL)` |
| 2 | sm1 | T1 updates row 7 and commits: the row is `(7, 'taken', 'w1')`, the index holds `('taken', 7)` |
| 3 | sm3 | `tbl_update` calls `rel_update`, which is refused with `SM_ERR_UPDATED`; sm3 has touched no index |
| 4 | executor | `tbl_read_latest` marks the row's lock and returns `(7, 'taken', 'w1')` |
| 5 | executor | `'taken' <> 'done'` is true, so it rebuilds the new tuple: `(7, 'done', 'w1')` |
| 6 | sm3 | `tbl_update` again: `rel_update` succeeds and hands back `(7, 'taken', 'w1')`, which is pinned |
| 7 | sm3 | `idx_delete('taken', 7)`, then `idx_insert('done', 7)` |

Step 6 and step 7 are the code of an ordinary update, and step 3
is that code stopping before an index. Step 7 hides one more
exchange with sm1. The entry `('taken', 7)` was committed after
T2 began, so sm1 refuses its erase with `SM_ERR_UPDATED`, takes
the key's lock, and wants the newest version read. An entry has
no filter to re-check: whether to write was decided on the row.
So `idx_delete` calls `sm1::read_latest` and repeats the erase,
and the caller never sees that code from an index.

### 7.8 Values out of line

sm2 moves a row's long datums out of its tuple (TOAST.md), and a
tuple then holds a pointer. An index is made of values, never of
a pointer.

- **What an index needs is fetched before the entry is made**, by
  every write and by a build: its key columns, its INCLUDE
  columns and its `reads` (`fetch_for`). So the evaluator never
  sees a pointer, and an entry is held to 64 KB whatever the row
  is.
- **An update tells an unchanged column by its pointer.** One
  pointer on both sides is one datum, and nothing is fetched. A
  pointer on one side and anything else on the other is taken as
  changed, and both sides are fetched. Comparing the bytes under
  the varlena header, as for a datum in line, would call a
  pointer equal to the empty text.
- **The old row's datums outlast the write.** The old entries
  come from the image the row's write hands back, and by then sm2
  would have erased what the old row held out of line. So
  `tbl_update` and `tbl_delete` write with hold, fix the indexes,
  then call `sm2::rel_release`. If a step fails,
  `sm2::rel_forget` forgets the held datums, and the rollback
  leaves the old row live.

### 7.9 Bulk load, rewrite, truncate and drop

- **Bulk load** ([`BULKLOAD.md`](BULKLOAD.md)). `tbl_load_add`
  puts a row and its entries into sm1's loader, one stream for
  each index, in the rows' order. An entry goes in as
  `idx_insert` would write it, through `idx_load`. A row a load
  does not take ends the load, and that row and those after it go
  by `tbl_insert`. In an empty table, a duplicate in a unique
  index is found when the load closes.
- **Rewrite.** Adding or dropping a primary key, and changing a
  column's type, rewrite a table with rows under the table-oid
  lock X (`rewrite_table`). `copy_rows` reads the old store in
  key order and inserts each row by `insert_row` into the new
  store, so every maintained index fills as in any insert. A
  duplicate fails the rewrite, naming the index and the row. A
  table with no row takes the new key as it is. The old store
  goes at commit.
- **Column type.** A column an expression or a partial index, a
  foreign key or a view reads keeps its type
  (`alter_column_type`).
- **`TRUNCATE`** swaps the store (CATALOG.md). Index definitions,
  relids and states carry over. `tbl_truncate` needs the table
  opened exclusive, and reopens the handle's rels on the new
  store.
- **`DROP TABLE`** is one `sm1::rmstore`, and takes the entries
  with it.
- **`DROP INDEX`** scans the keys under its relid and erases
  each, as sm1 has no range delete. It erases what its snapshot
  shows. An entry its snapshot does not show stays under a relid
  never given again, until the store goes.

## 8. The kind of an index

An index is one of two kinds, chosen when it is made and kept for
its life. Changing a kind is `DROP` and `CREATE`.

- **`USING lsm`** is the default, and makes an LSM Index.
  **`USING btree`** makes a B-tree Index. digres has one index
  access method for both, and `tree_kind()` in `smstore.c` reads
  the name. No other name is taken. sm routes by the kind.
- **`create_index` takes `tree`** and keeps it in the index's
  definition (`index_def_t::tree`). A primary key is never a
  tree, as a tree's entries are not rows.
- **Every call that touches an index branches on the kind:**
  insert, scan, the uniqueness check, the build, vacuum and
  `DROP`. A table's indexes are written in one call, whichever
  their kinds.
- **The keys of the two kinds do not meet.** An LSM Index's
  entries are class `0x00` keys under its relid. A tree's pages
  are class `0x04` under its own. One store holds both, and one
  table may have both.
- **`pg_get_indexdef()` prints the kind's name**, and so does
  psql's `\d` of a table. `\d` of an index prints `pg_am`'s name,
  `btree`, for both.
- **Both kinds are kept**, each asked for by name: two code paths
  in sm2 and sm3, and tests that run on each. `USING lsm` stays
  the default. So a tree can be measured against an LSM Index on
  one table, and left off if it fails.

Where each is the better, measured or estimated, in instructions:

| | LSM Index | B-tree Index |
| --- | --- | --- |
| an entry of a range | 2,820 | about 200 |
| a probe of a non-unique key | about 30k (`Seek`) | about 3k |
| a probe of a unique key | a `Get`, near PostgreSQL's | about 3k |
| a unique key, at most | 4,096 bytes | about 1.3 KB |
| an index-only scan | yes: an entry is MVCC | no row read on an all-visible leaf; else reads the row |
| an indexed insert, written | about 40 bytes | the page, up to 4 KB |
| dead entries | none, GC drops versions | until a reap |

## 9. Costs accepted

- **An indexed column is written twice:** encoded in the index,
  native in the tuple. In exchange the key need not decode, which
  is a hard promise to keep over sort keys and cut keys.
- **Plans are lost to the ban on backward scans.** An
  index-ordered `ORDER BY x DESC LIMIT n` and a merge join that
  wants descending order become a top-N sort over a forward scan.
  `MAX()` is expected to lose its backward index scan, and
  `MIN()` keeps its forward one. Scrollable cursors get a
  Materialize node. These lose plans, not correctness, and the
  loss is not measured.
- **An over-long key in a unique index is refused** (§4.3).
- **The evaluator's cost is unmeasured:** what one call a row
  adds to a write.
- **`DROP INDEX` is a range of erases**, not an O(1) drop.

## 10. Implementation

### 10.1 sm2: `src/rel/index.cc`

- `index_t` holds the session, store, relid, key columns, whether
  the index is unique, whether its table is keyed, and the
  layout. `rel` is set for a primary key. `tree` and `pages` are
  a tree's.
- `entry_key` builds a key: relid, whole columns, then the
  decision. It computes `suffix`, compares the columns to
  `budget()`, refuses or cuts, and appends the row key. `row_cap`
  gives the longest row key: `max_pk_len` or `id_len`.
- `entry_of` adds the value: `tup_encode` of cut, the INCLUDE
  columns and the row key as a varlena.
- `idx_insert` is `sm1::insert`. `idx_delete` is `sm1::erase`,
  repeated once after `sm1::read_latest` on `SM_ERR_UPDATED`.
  `idx_load` is `sm1::load_put`.
- `unique_key` gives a unique key's sm1 key, or 1 for a key with
  a NULL. `idx_find` reads it at the snapshot, and
  `idx_find_latest` under a shared lock, with the index's relid
  as the lock's kind. Over a primary key both use sm1's `chain_`
  calls, as its entries are rows.
- `entry_row` takes the row key out of a value, and a keyless
  row's `_id` out of that.
- `bound_key`, `lower_key` and `scan_range` turn bounds into sm1
  keys, and `successor` is §5.1's.
- `idx_scan` opens `sm1::scan`. `idx_scannxt` decodes the cut
  flag and the row key, and hands out the value. `idx_scan_key`
  calls `idx_entry_cols`, which decodes the columns and checks
  that only a row key follows them.
- The tree branches in these functions are BTREEINDEX.md's.

`encode_key` in `src/type/key.cc` has two forms. `entry_key` and
`bound_key` call the plain one and decide the length themselves.
The form with `max_len` and `overflow_t` serves `rel_key`, which
refuses a row key past `max_pk_len`.

### 10.2 sm3: `src/table/table.cc`

- `table_t` holds the definition under the table-oid lock, the
  rel, a slot for each index's `sm2::index_t`, and `states`: each
  index's state, noted at open (INDEXBUILD.md).
- `open_rels` opens the rel on the definition's layouts.
  `use_index` opens an index at its first use, so a read by the
  primary key opens no other index.
- `tbl_park` and `tbl_reopen` keep a handle for its session's
  next transaction ([`SM_API.md`](SM_API.md), `sm_tbl_close`). A
  reopen takes the lock and reads the definition, and makes the
  rel and indexes again only if the definition has changed.
- `make_entry` gives an index's entry for one image: `entry_t`
  holds `belongs`, the key datums and the evaluator's values,
  which the key points into. It keeps its arrays when made again.
- `add_entry` inserts and names the index in `sm1::errdup()` on
  `SM_ERR_DUPKEY`. `remove_entry` deletes, and takes
  `SM_ERR_NOTFOUND` as no error while the index is pending.
- `cols_same`, `unchanged` and `keys_equal` are §7.4's tests.
- `fetch_cols` and `fetch_for` are §7.8's. `image_t` holds a
  row's datums, so that a fetched value can replace a pointer in
  place.
- `insert_row`, `update_row` and `delete_row` are §7.3's.
  `insert_row` also takes a reserved `_id`, as a rewrite keeps a
  keyless row's.
- `tbl_insert_arbiter` and `find_now` are §4.5's.
- `tbl_load_open`, `tbl_load_add`, `copy_rows` and
  `rewrite_table` are §7.9's.
- `tbl_index_backfill` is INDEXBUILD.md's.

### 10.3 The definition

`create_index` in `src/ddl/ddl.cc` checks the definition: a name
and a key column; a blob for `reads`; an evaluator for a blob;
live columns; a collation only on text; a free relid. It draws
the oid, takes `next_relid`, and either fills the index or queues
it (INDEXBUILD.md). `drop_index` refuses an index a foreign key
uses.

## 11. Cases

| Case | What happens |
| --- | --- |
| TOAST | an entry holds values; what the index needs is fetched first (§7.8) |
| Non-unique | key ends with the row key; `sm1::insert` cannot meet another row |
| Unique | key alone, unless it has a NULL; sm1's `SM_ERR_DUPKEY` (§4.1) |
| Primary key | no entries; the rows are its entries (§3.3) |
| Expression | keys from the evaluator, on each image (§6) |
| Partial | an entry only for a row that belongs; update into or out of the predicate removes or adds one |
| INCLUDE | in the value, as written; compared by bytes |
| Cut keys | non-unique cut, flagged, rechecked by the caller; unique refused (§2.4) |
| Update A→B→A | each step erases one key and inserts another; a tombstone does not block an insert |
| INCLUDE-only update | key equal by `cmp`, bytes differ: entry removed and added under one key |
| Primary key change | row moves; every entry removed and added (§7.5) |
| Delete then reinsert | tombstone, then a new version of the same key; a unique key is free once the delete commits, or at once in its own transaction |
| Abort | an aborted transaction's versions are dead to every reader; sm2 keeps nothing to undo |
| Savepoint rollback | sm1 drops the writes made at that depth; a cursor begun after the savepoint fails with `SM_ERR_SCANCLOSED` |
| Undo-cap overflow | sm1's, as for any key: a savepoint past `savepoint_undo_bytes` is lost, whatever the keys are |
| Crash | entries are versions; those of an unfinished transaction are dead by `CF:txn` |
| 2PC across restart | entries stay uncommitted versions; a key's next writer waits; DDL cannot be prepared |
| Two writers | on the row first: the second waits there, before any index |
| Two inserters of a unique key | §4.1 |
| Old RR snapshot | sees old entries by their versions; may not scan a newer index (INDEXBUILD.md) |
| RC re-read | §7.7 |
| SSI | a scan records its range and a write its key, in sm1, as for any key |
| Open cursor | its view is fixed at open (§3.4) |
| GC watermark | entry versions go as any versions; a dead entry needs no vacuum |
| `CF:txn` sweep | compaction stamps `commit_ts` over the xid in an entry's value, so an entry needs the header (§12) |
| `CREATE INDEX` pending | writers maintain it; a missing entry is no error (INDEXBUILD.md) |
| `DROP INDEX` | erases the entries its snapshot shows; relid never reused |
| `TRUNCATE` | new store; definitions, relids and states carry over |
| `ALTER` rewrite | every maintained index fills in the new store; a failed index stays failed |
| Temporary table | an index of it is filled in the session; the store is chained as any table's (TEMPTABLE.md) |
| Keyless (`_id`) table | row key is 8 bytes; budget 4086 |
| Bulk load and rollback | entries go in with the rows; an aborted load leaves the table empty ([`BULKLOAD.md`](BULKLOAD.md)) |
| A wrongly immutable function | an entry a delete cannot find, which fails the write on a valid index; PostgreSQL's hazard too |
| Collation library changed | the catalog does not open (§2.3) |

## 12. Rejected

- **Hash an over-long key.** Keeps equality and destroys order
  (§2.4).
- **Cut keys in a unique index.** Needs a read of a range at its
  newest version, with a locking rule of its own. That is too
  much for the rarest path in index maintenance (§4.3).
- **`strcoll` at compare time.** It is slow for each comparison,
  wants NUL-terminated input where a key may hold `0x00`, reads
  process-global locale state, and is a comparator, which sm1
  cannot take.
- **`strxfrm_l` for sort keys.** It works, but glibc's collation
  is not stable across versions. A stable key means ICU.
- **Warn on a collation mismatch and carry on**, as PostgreSQL
  does. New sort keys in an old index corrupt it (§2.3).
- **An automatic rebuild on a mismatch.** A rebuild of a large
  index is work an operator should schedule.
- **No null marker on a `NOT NULL` column.** Saves a byte, and
  makes a schema change rewrite a table (§2.2).
- **A length prefix for variable-width datums.** It breaks
  ordering. A wider alphabet costs more.
- **Dropping the last column's terminator.** Saves two bytes, and
  needs a rule the encoder cannot see (§2.2).
- **An entry as a key alone.** A value must keep its version's
  flags and xid or `commit_ts`: the compaction filter stamps
  `commit_ts` over the xid, and can change a value, never a key.
  What else the value holds saves about 15 bytes an entry and a
  decode, which does not pay for a change of format.
- **A savepoint for each row write.** It costs a lock-table mutex
  acquisition a row, and PostgreSQL's errors abort the
  transaction or the subtransaction anyway (§7.6).
- **The executor writes entries through its access method.** The
  rows and entries could then disagree (§7.1).
- **An evaluator in sm3 over a safe subset**: comparisons with a
  constant, conjunctions, `IS [NOT] NULL`. It refuses any index
  outside the subset, and widening it means a function library
  that must agree with PostgreSQL's row for row.
- **sm3 links the fork's rewritten functions and calls them with
  no callback.** This ties the stack to PostgreSQL's function
  catalog. It stays open, if the callback ever shows in a
  profile.
- **Show the evaluator a tuple.** The host would need a schema
  and a decoder (§6.2).
- **Keep a TOAST pointer in an INCLUDE column.** A reader of the
  index would then fetch through the table.
- **A filter pushed down into a scan.** A call into the host for
  each row, and the rows that fail cross the API anyway (§5.5).
- **Sets of rows in and out of a scan.** The IPC boundary that
  forced this is gone. Rows read by key, many in one call, are
  built ([`ROWSTORE.md`](ROWSTORE.md)).

## 13. Tests

- **mvcc:** none. sm1 knows no index. sm3 has no scenario driver:
  sm1's links sm1 alone, and a level may not depend on one above
  it.
- **sm2:** `sm2/test/test_index.cc` covers bounds, key columns
  given back, unique keys and nulls, non-deterministic
  collations, cut keys, entries committed since begin, and
  misuse. `sm2/test/test_key.cc` covers the encoding, with
  property checks such as interval's identity.
- **sm3:** `sm3/test/test_table.cc` checks every index against a
  brute-force scan of the rows after each scenario: plain
  maintenance at all three isolation levels; the row race at READ
  COMMITTED, the write and the skip, and above it; races on a
  unique key. Partial and expression indexes go through the same
  cases with the tests' own evaluator.
- **sm3, others:** `test_arbiter.cc` has `tbl_insert_arbiter`.
  `test_toast.cc` has values out of line. `test_cluster.cc` has
  keyed tables and moved rows. `test_ddl.cc` has refused
  definitions and `DROP INDEX`. `test_load.cc` has bulk load.
- **Two sessions on one thread.** A step that would wait is
  scripted with sm1's `set_qa_nowait`, which fails it with
  `SM_ERR_BLOCKED`, to repeat later. Deadlocks, which a polling
  session cannot be the victim of, stay in sm1's threaded tests.
- **rel:** `rel/test/test_index.rel`, `test_recheck.rel` (runs
  §7.7's examples as written, the script playing the executor),
  `test_blocking.rel`, `test_partial.rel`, `test_pkey.rel`.
- **dg:** `dg/test/slt/index_key.test`, `index_kind.test`,
  `index_only_cut.test`, `expr_index.test`, and
  `dg/test/server.sh`.
