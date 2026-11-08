# DESIGN — sm2, types and tuples

sm2 is level 2 of the stack. It gives sm1's opaque byte strings a
type and a shape. It is a type system, the two on-disk encodings
those types get, and a relation whose rows are tuples. This
document is an overview. Each module document holds its own
design. [`SM2_OBJECTIVE.md`](SM2_OBJECTIVE.md) says what the
level is for.

## 1. What the level provides

| Decision | Choice |
| --- | --- |
| Types | a fixed set, each an ADT in `pg_type`'s shape, each with a stable on-disk type code |
| Representation | PostgreSQL's, type by type, little-endian, but the decimals, which follow DuckDB's, with their scale |
| Key encoding | order-preserving for each type; descending is the inverted encoding |
| Tuple format | null bitmap, fixed and variable parts, an attribute count in each row, as PostgreSQL's `natts` |
| Tuple codec | an API of its own, with its own tests |
| Row identity | the primary key, or `_id` without one, a 64-bit number from a table's sequence |
| `rel` | a relation in one sm1 store, under a 2-byte relid, keys encoded and tuples decoded by sm2 |
| Index | an index `rel`: versioned entries, or a B+tree of sm1 pages |
| API shape | one tuple at a time |

All of it is built, TOAST included. The API is `include/sm2.h`,
in `namespace sm2`.

## 2. What the level depends on

sm2 is written against `../sm1/include/sm1.h`, and holds no
RocksDB handle.

| sm1 gives sm2 | and the constraint it brings |
| --- | --- |
| stores as ordered maps, clustered on the key | keys order bytewise only, with no comparator to install, so order is built into the encoding |
| opaque values | the tuple layout is sm2's alone |
| transactions at the three levels | sm2 opens none: every `rel` call runs in the caller's transaction |
| `sm1::update` and `sm1::erase` returning the old image | `rel_update` and `rel_delete` can hand back the old tuple, which index maintenance needs |
| `SM_ERR_DUPKEY` from `sm1::insert` | a unique index keyed without its row's key, and the primary key, get uniqueness for free |
| `SM_ERR_UPDATED` and `sm1::read_latest` | sm2 surfaces the error and offers `rel_read_latest`; it cannot resolve it, because the re-check needs a filter, and filters are above it |
| `sm1::max_keylen` 4096, `sm1::max_datalen` 64 MB | an encoded key must fit, relid included; a long datum goes out of line |
| no backward scan, ever | a descending column is inverted bytes scanned forward, with no mirror relation |
| pages, `sm1::pin_page` and `sm1::page_write` | a tree's pages are sm1's, under the index's relid |

sm2 also uses `../sm0`, `../include`, ICU for sort keys, and a
verbatim copy of PostgreSQL 18's float printers. It takes no
other PostgreSQL code: PostgreSQL is a target that shapes
decisions, never a build dependency.

## 3. Modules

| Module | Document | Summary | Code in `src/` |
| --- | --- | --- | --- |
| Types | [`TYPE.md`](TYPE.md) | each type's representation, order and key bytes, and why | `type/` |
| Tuple | [`TUPLE.md`](TUPLE.md) | a row's bytes, the codec, and why | `tuple/` |
| Varlena | [`VARLENA.md`](VARLENA.md) | a variable-width datum's bytes | `tuple/` |
| Row Store | [`../ROWSTORE.md`](../ROWSTORE.md) | the tuple relation: rows, their keys, `rel_*` | `rel/rel.cc` |
| LSM Index | [`../LSMINDEX.md`](../LSMINDEX.md) | the key encoder's rules, entries, cut keys, the index `rel` | `rel/index.cc`, `type/key.cc`, `type/collation.cc` |
| B-tree Index | [`../BTREEINDEX.md`](../BTREEINDEX.md) | an index as a B+tree of its own pages | `tree/`, the tree paths of `rel/index.cc` |
| TOAST | [`../TOAST.md`](../TOAST.md) | a row's long datums, out of line in the table's store | `rel/toast.cc` |
| Catalog | [`../CATALOG.md`](../CATALOG.md) | `catkv`, the bytes of sm3's catalog, which sm2 stores and never reads | `rel/catkv.cc` |

## 4. Structure

**Principle: the two on-disk formats come first and get the most
scrutiny.** A key encoding and a tuple layout are permanent the
moment any data exists, and no patch can fix them. Everything
else in sm2, and in sm3, is code that can be rewritten.

### 4.1 Source layout

```
Makefile      the same targets as sm1's
include/      the public header, sm2.h
src/type/     the ADT table, each type's representation and its key,
              multi-column keys, collations
src/ryu/      PostgreSQL 18's float printers, copied verbatim, built as C
src/tuple/    the tuple codec
src/rel/      the row and index relations, TOAST, catkv
src/tree/     the B+tree and its page format
test/         doctest, as in sm1
```

Dependencies run one way: `rel` calls `tree`, `tuple` and `type`,
and `tuple` calls `type`. Only `src/rel` and `src/tree` call
sm1's storage functions. Every directory may include `sm1.h`,
because sm2 reports its failures through sm1's error state.

### 4.2 Build

```
make -j4              libsm2.a and the test binary
DEBUG=1 make -j4      the same, -O0 -g, with assert()
make format           clang-format src/ and test/
```

The build makes sm1's library through sm1's own Makefile when it
is missing or out of date. `include/sm2.h`, `src/ryu/` and
`test/pg_fixtures.h` are not formatted.

### 4.3 Tests

```
DEBUG=1 make -j4 test    the unit tests
make tsan                the B+tree's tests, under the thread sanitizer
```

Most of sm2 is tested by property, not by interleaving.
Two-session scenarios above sm1 are `rel` scripts
([`../rel/REL.md`](../rel/REL.md)), which drive sm2 and sm3
through
`../include/sm.h`. Each module document says what its tests
prove:

| Area | Tests | Where it is told |
| --- | --- | --- |
| types, keys, PostgreSQL fixtures | `test_type.cc`, `test_key.cc`, `test_pg_fixtures.cc` | [`TYPE.md`](TYPE.md#9-tests) §9 |
| the tuple codec | `test_tuple.cc` | [`TUPLE.md`](TUPLE.md#14-tests) §14 |
| the relations | `test_rel.cc`, `test_cluster.cc`, `test_index.cc`, `test_catkv.cc` | [`../ROWSTORE.md`](../ROWSTORE.md#13-tests) §13, [`../LSMINDEX.md`](../LSMINDEX.md#13-tests) §13 |
| TOAST | `test_toast.cc` | [`../TOAST.md`](../TOAST.md#6-tests) §6 |
| the tree | `test_page.cc`, `test_tree.cc`, `test_tree_index.cc` | [`../BTREEINDEX.md`](../BTREEINDEX.md#15-tests) §15 |

## 5. Cross-level decisions

### 5.1 What sm2 leaves to sm3

sm3 is a client of sm2, as sm2 is of sm1.

- **The catalog.** sm2 is handed a schema. It neither persists
  one nor looks one up, and it has no notion of a table name, a
  DDL statement or a table-oid lock. It keeps sm3's catalog bytes
  for it, `catkv_*`, in sm1's store 0 under sm1's upper prefix,
  and never reads them.
- **Which indexes exist.** sm2 maintains no index. It gives sm3
  the encoder, the index `rel`, and the old image as a tuple.
- **Object mapping.** Which store holds a table, and which relid
  each index gets, is sm3's. sm2 prepends and strips the relid,
  and never interprets it.
- **The primary key's definition.** sm2 keys a `rel` by the
  columns it is opened with, and finds a duplicate. Which
  columns, and adding or dropping a primary key, which rewrites
  the table, are sm3's.
- **Predicates.** sm2 has no filter. So it cannot resolve
  `SM_ERR_UPDATED`, or derive a key range from a `WHERE` clause.
- **Statistics.** sm2 counts nothing. It answers sizes and
  positions by key range
  ([`../STATISTICS.md`](../STATISTICS.md#98-the-calls) §9.8).
- **Everything above the stack.** sm2 has no SQL, no planner and
  no executor, and builds and tests with no PostgreSQL source
  tree ([`../PART4.md`](../PART4.md)).

### 5.2 Deployment

The fork is threaded
([`../PART4.md`](../PART4.md#2-the-blocker-the-process-model)
§2), so all the levels link
into one server process, and a session is used by one thread at a
time. The boundary above sm2 is a function call, not an RPC, so
an interface of one tuple at a time costs nothing an IPC boundary
would add.

Trade-offs have module owners:
[`TYPE.md`](TYPE.md#7-costs-accepted) §7,
[`TUPLE.md`](TUPLE.md#12-costs-accepted) §12,
[`../ROWSTORE.md`](../ROWSTORE.md#28-costs) §2.8,
[`../LSMINDEX.md`](../LSMINDEX.md#9-costs-accepted) §9, and
[`../DBMGR.md`](../DBMGR.md#14-trade-offs) §14
for the bound on tables that one store a table brings.
