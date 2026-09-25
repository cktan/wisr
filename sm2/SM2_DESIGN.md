# DESIGN — sm2, types and tuples

The level that gives sm1's opaque byte strings a type and a
shape. It is three things and no more: a **type system**, the
two on-disk **encodings** those types get, and a **relation
whose rows are tuples** instead of raw bytes. Everything a
schema needs above that — a catalog, secondary indexes,
predicate scans, statistics — belongs to sm3 and is argued in
`sm3/SM3_DESIGN.md`.

This document is the design of record; it follows the
decisions in `SM2_OBJECTIVE.md`:

| Decision | Choice |
| --- | --- |
| Types | a fixed set, each an ADT in `pg_type`'s shape, each with a stable on-disk type code |
| Representation | PostgreSQL's, type by type, little-endian — except `decimal`, which follows DuckDB's |
| Key encoding | order-preserving per type; descending is the inverted encoding |
| Tuple format | null bitmap, fixed/variable split, a schema version stamp per row |
| Tuple codec | an API of its own, with its own header and its own tests |
| Row identity | `rowno`, a 64-bit number from a per-table sequence; the table is a heap and the primary key is an index (§5) |
| `rel` | one sm1 store, keys encoded and tuples decoded by sm2 |
| API shape | one tuple at a time; batching belongs where filters do (`sm3/SM3_DESIGN.md`) |
| PostgreSQL | a target that shapes decisions, never a build dependency |

**Status: nothing here is implemented.** This file exists so
the formats are argued before they are written, because two of
them — the key encoding and the tuple layout — are on disk
and effectively permanent once any data exists. `PLAN.md` has
the order of work. The PostgreSQL target (`PART4.md`) shapes
these decisions but is never a build dependency: sm2 must
build and be tested without a PostgreSQL source tree. A
throwaway fork spike, built early to find which wall bites
first, is how to learn what the permanent formats must carry
before data exists (`PART4.md` §7).

---

## 1. What sm1 provides, and what it costs

sm2 is written against the API in `sm1/include/sm1.h`. The
parts that matter at this level, and the constraints that come
with them.

**Deployment: one process, one thread per session.** The
PostgreSQL fork is threaded (`PART4.md` §2), not process per
connection, so sm1, sm2 and sm3 are linked into the one server
process and every backend thread shares sm1's transaction
table, lock manager and SSI graph. A session is used by one
thread at a time (`sm1/API.md`). The boundary above sm2 is a
function call, not an RPC, so a per-tuple interface costs
nothing that an IPC boundary would have made it cost.

| sm1 gives sm2 | and the constraint it brings |
| --- | --- |
| stores as ordered key→value maps, clustered on the key | keys are ordered **bytewise only**; there is no comparator to install (`sm1/SM1_OBJECTIVE.md`) — so order has to be built into the encoding (§3) |
| opaque values of arbitrary bytes | the tuple layout is sm2's alone; sm1 never looks inside one (§4) |
| transactions at RC / RR / SERIALIZABLE | sm2 opens none of its own; every `rel` call runs inside the caller's transaction (§5) |
| `sm1::update` / `sm1::erase` returning the **old image** | `rel_update` / `rel_delete` can hand back the old *tuple*, which is what index maintenance above needs |
| `SM_ERR_DUPKEY` from `sm1::insert` | a unique index keyed without `rowno` gets its uniqueness from sm1 for free — the primary key index among them (§5, `sm3/SM3_DESIGN.md` §3) |
| `SM_ERR_UPDATED` and `sm1::read_latest` at READ COMMITTED | sm2 surfaces the error and offers `rel_read_latest`; it cannot resolve it, because the re-check needs a filter and filters are sm3's (§5) |
| nested savepoints | a statement can be undone without losing the transaction, and sm2 holds no state that has to be undone with it |
| forward scans with `sm1::scan_seek` | probe loops cost one cursor, not one per probe |
| `sm1::max_keylen` 4096, `sm1::max_datalen` 64MB | an encoded index key must fit 4096 bytes, prefix included; a datum above the TOAST threshold is stored out of line (§4) |
| no backward scan, ever | a descending column is stored with inverted bytes and scanned forward; there are no mirror relations, and the fork's planner never asks for a backward scan (§3) |

sm1 keeps one column family per store and handles hundreds
to low thousands of them. sm3 gives each table one sm1
store, holding the table's rows, its indexes and its
TOAST data, told apart by the 2-byte object prefix in
§5. A `DROP TABLE` is then one `sm1::rmstore`, sm1's O(1) column
family drop. The price is that the number of tables,
partitions included, is bounded by how many stores sm1
handles; that is accepted for v1 (§7).

---

## 2. Types

**`TYPE.md` is the per-type reference** — every type's native
representation, its order and its key bytes, one section
each. This section argues the decisions behind it; that file
is what you read while writing code.

*To be written.* The rules it must satisfy:

- The set is fixed and chosen from what the PostgreSQL target
  needs first: boolean, the integer widths, `float4` and
  `float8`, `decimal`, `text` / `varchar` / `bytea`, `date`,
  `time`, `timestamp`, `timestamptz`, `interval`, `uuid`.
  Arrays, composites, ranges and user-defined types are not in
  the first set; the format must leave room to add them
  without changing what is already on disk.
- **`enum` is not an sm2 type.** Its labels live in the
  catalog, which is sm3's, so the type cannot be closed over
  inside sm2. sm2 stores an enum as `int4` and sm3 holds the
  label mapping, which is PostgreSQL's arrangement too. An
  index on an enum orders by the `int4`, so sm3 assigns values
  in declared order if label order is what an index should
  give.
- **`text` and `varchar` are separate types with identical
  bytes.** They keep two codes so a round trip through sm2
  never rewrites a column's declared type. The `varchar(n)`
  length limit is a constraint, and constraints are sm3's;
  sm2's codec stores what it is given.
- A type is three things together: an in-memory datum, an
  order-preserving key encoding (§3), and a tuple encoding
  (§4). A type is not added until it has all three.
- Every type has a **small stable code that goes on disk**, in
  the schema rather than in each row. The codes in `TYPE.md`
  §3 are frozen as they stand, `bool` = 1 through `uuid` = 16:
  assigned once, never renumbered and never reused, so a type
  removed from the set does not free its number. Codes 64–127
  are reserved for types added later, and 0 is never a code.
- **Null is a property of the column, not of the type.** It is
  carried by the tuple's null bitmap (§4) and by a null
  marker in the key encoding (§3), so no type needs a reserved
  datum to mean "absent".
- **Collation is a property of the column too**, not of
  `text`. Two `text` columns under different collations sort
  differently and are encoded differently (§3); they are the
  same type.
- A type's *comparison* order is its own, and is what §3 must
  reproduce bytewise. Where SQL's order is already odd — IEEE
  negative zero, NaN — the choice is written down in §3 rather
  than left to the encoder.

### 2.1 The representation is PostgreSQL's

**Decision: a datum's bytes are what PostgreSQL makes them,
type by type, with `decimal` the one exception.** Not a format
of our own that happens to hold the same information — the
same widths, the same epochs, the same units, the same struct
layouts.

The reason is the conversion in §4 and in `PART4.md` §6. The
executor works in tuple slots, and a bespoke per-datum
encoding pays a translation on every column of every row, in
both directions, forever. Adopting PG's representation makes
that cost **per tuple rather than per datum**: the framing
around the columns still has to be built (§4), but the column
bytes themselves are copied, not converted.

| Type | Representation | Bytes |
| --- | --- | --- |
| `bool` | 0 or 1 | 1 |
| `int2` / `int4` / `int8` | two's complement | 2 / 4 / 8 |
| `float4` / `float8` | IEEE-754 | 4 / 8 |
| `decimal` | **after DuckDB's**, not PG's — see §2.2 | 8 or 16 |
| `date` | days since 2000-01-01 | 4 |
| `time` | microseconds since midnight | 8 |
| `timestamp` / `timestamptz` | microseconds since 2000-01-01 00:00:00, UTC for `timestamptz` | 8 |
| `interval` | PG's three components — see §2.4 | 16 |
| `uuid` | the 16 bytes in their sort order | 16 |
| `text` / `varchar` / `bytea` | the payload bytes alone | variable |

The widths, epochs and units above are from memory and are
not checked against the source; step 1 of `PLAN.md` pins each
one to a fixture taken from a running PostgreSQL, which is
the only way this table becomes a claim rather than an
intention.

Two places need sm2 to be more precise than PostgreSQL is:

- **Byte order.** A stored PG datum is native-endian, which is
  why `pg_upgrade` refuses to cross architectures. An on-disk
  format cannot be that vague, so sm2 fixes **little-endian**
  and says so. On every machine the fork targets the copy is
  still a `memcpy`; on a big-endian one it is a byte swap, and
  the file is still readable.
- **Variable-length datums.** `text`, `varchar` and `bytea`
  carry the payload only. The varlena header, the 1-byte short
  form and the TOAST pointer are PostgreSQL's framing, and
  framing is the tuple's business (§4) — sm2 has its own
  framing, and its own TOAST in its own format.

### 2.2 Except decimal, which follows DuckDB's

**Decision: `decimal` is a fixed-width decimal, with DuckDB's
`DECIMAL` as its reference.** PG's numeric is a varlena of
base-10000 digits with a weight and a display scale,
effectively unbounded in length. DuckDB's is a two's-complement
integer, the value times 10^s, with `(precision, scale)` held
in the schema and at most 38 digits of precision. sm2 takes that
with two of DuckDB's four widths: 64 bits for a precision up to
18, 128 bits up to 38. (DuckDB also has 16- and 32-bit forms up
to 4 and 9 digits; sm2 does not.) An earlier draft took Arrow's
decimal, with a 256-bit form up to 76 digits; DuckDB's limit of
38 was preferred. Three things follow, and they are why the
exception is worth making:

1. **The key encoding falls out.** A fixed-width two's
   complement integer, sign bit flipped and written
   big-endian, is already order-preserving (§3). PG's digit
   form needs a bespoke order-preserving encoder over a datum
   with no length bound, which is the hardest single item §3
   would otherwise carry.
2. **It is a fixed-width column** in the tuple (§4), not a
   variable one.
3. **Comparison is integer comparison**, once the scale is
   fixed per column.

The costs are real and are named here rather than discovered
later:

- **Precision stops at 38 digits.** PG allows 1000 for a
  declared typmod, and far more for an undeclared one. A
  column that needs more has no representation at all.
- **There is no NaN and no infinity.** PG's numeric has had
  NaN throughout and `Infinity` since version 14 (from memory,
  not checked against the source); a fixed-width decimal is an
  integer and has neither, so sm2 refuses them on insert.
- **The scale must be declared.** The decimal puts `(precision,
  scale)` in the schema; PG allows a bare `numeric` where
  every datum carries its own. A bare `decimal` column has no
  equivalent, so it defaults to `decimal(18, 3)`, as DuckDB's
  bare `DECIMAL` does, which can round a datum its user believed
  exact.
- **It is the one type that is converted** at the executor
  boundary, in both directions, while §2.1 makes every other
  type a copy.

What a column may declare, and what a datum may hold:

- **`decimal(p, s)`, with `numeric` as an alias.** The type is
  named `decimal`, and `numeric` means the same, as `decimal`
  does in PostgreSQL, where it is an alias of `numeric`. As
  there, the alias is the SQL layer's to resolve: sm2's
  descriptor carries the one name. The names are PostgreSQL's
  but the type is not, so a schema ported from PostgreSQL keeps
  its `numeric(p, s)` columns and gets at most 38 digits, no
  NaN and no infinity (above). An earlier draft named the type
  `number`, and refused `NUMERIC` and `DECIMAL`, to keep that
  difference in view; the familiar names were preferred.
- **One type code, two widths**, chosen by declared precision:
  8 bytes up to 18 digits, 16 bytes from 19 to 38. A column
  declares its precision, so a column's datums all have one
  width; the width is the typmod's, not the datum's.
- **`decimal(p)` is `decimal(p, 0)`**, as in PostgreSQL.
- **A bare `decimal` is `decimal(18, 3)`**, DuckDB's default:
  15 integer digits and 3 fractional, in 8 bytes. A datum with
  more fractional digits is rounded to 3, as PostgreSQL rounds
  for a declared typmod; one whose integer part needs more
  than 15 digits is an error. Refusing a bare `decimal` was the
  alternative; the default was chosen over it, and its cost is
  in §7. A second,
  variable-width representation was rejected outright: two
  `decimal`s on disk means two key encodings and two
  comparison paths, which is most of what this section
  avoids.
- **NaN and infinities are refused** on insert with a clear
  error, and no value is reserved as a sentinel. That door is
  not closed: 18 digits do not reach the 64-bit limit, nor 38
  the 128-bit one, so the extreme integers are never written
  and could still be reserved later without changing what
  stored bytes mean — provided the codec keeps refusing every
  value outside the declared precision.

### 2.3 A type is an ADT, in PostgreSQL's shape

**Decision: a type is a descriptor of function pointers, as
`pg_type` is.** PostgreSQL does not scatter its types through
the code; it makes each one a row of functions — `typinput`,
`typoutput`, `typreceive`, `typsend`, `typlen`, `typbyval`,
`typalign` — with the comparison and hash functions arriving
through the operator class. sm2 does the same thing in one
struct per type:

| Entry | What it is | PG's |
| --- | --- | --- |
| `in` / `out` | text form, parsed and printed | `typinput` / `typoutput` |
| `recv` / `send` | binary form over the wire — **deferred**: the slots exist in v1, unfilled, as sm2 has no wire protocol | `typreceive` / `typsend` |
| `cmp` | the type's own total order (§2) | the btree opclass support function |
| `hash` | for grouping and hash joins | the hash opclass support function |
| `key_encode` | the order-preserving, injective byte string (§3) | *no equivalent* |
| `key_decode` | the inverse, where one exists — for index-only scans, not for reads (§3) | *no equivalent* |
| `len`, `byval`, `align` | fixed or variable, and how it packs (§4) | `typlen`, `typbyval`, `typalign` |

`key_encode` is the one entry with nothing behind it in
PostgreSQL, and the reason is structural: a PG btree holds
datums and calls `cmp`, so it never needs a byte string that
sorts correctly on its own. sm1 compares with `memcmp` and
nothing else (§1), so sm2 does.

v1 fills `in` and `out`, because every test and every
golden-bytes fixture writes its values as text. `recv` and
`send` wait for a wire protocol, which sm2 does not have.

What this buys is that adding a type is adding a descriptor.
The alternative — a `switch` in the key encoder, another in
the tuple codec, another in the scan path — is three places to
forget, and the first one forgotten is a corrupt index.

### 2.4 Interval keeps PostgreSQL's three components

**Decision: interval is PostgreSQL's datum, all three components
of it** — months `int32`, days `int32`, and microseconds
`int64`, 16 bytes. They are kept apart because they are not
convertible into one another: a month is 28 to 31 days, and a
day is 23, 24 or 25 hours across a daylight-saving boundary.
So `'1 mon'` and `'30 days'` are different datums, and adding
them to the same timestamp gives different answers.

The consequence lands on §3: **interval has no natural total
order**, and a key encoding needs one. PostgreSQL supplies an
order by flattening — `interval_cmp_value` reduces the three
components to a single microsecond count at 30 days to the month
and 24 hours to the day, computed in 128 bits (checked against
PostgreSQL 18's source; the 128 bits are arithmetically
necessary — over the full `int32`/`int64` component ranges the
count reaches about 5.8e21, well past `int64`). That order is deliberately **not
injective**: `'1 mon'` and `'30 days'` both flatten to
2592000000000, and PG's btree calls them equal.

Equal keys are exactly what a primary key cannot have, so §3
says what an interval key is.

Whether `interval` is in the first set is settled here: it
is.

---

## 3. Key encoding

The per-type encodings are in `TYPE.md` §3; the rules they
all have to satisfy are here.

*To be written.* The rules it must satisfy:

- `encode(a) < encode(b)` bytewise **iff** `a < b` in the
  type's own order, for every supported type.
- **Injective, not reversible.** Distinct datums must get
  distinct keys, because a unique index — the primary key's
  among them — is enforced on key bytes: two datums that
  collide are one entry, and a unique constraint that cannot
  tell them apart is not a constraint. But the
  key does *not* have to be decodable back into the datum —
  §5 keeps every column in the tuple, so the tuple is what
  a read returns and the key never has to give a datum back.
  `key_decode` exists (§2.3) for index-only scans in sm3,
  where skipping the base read is the whole point; it is an
  optimization, not a correctness requirement.

  Two consequences worth stating, because they look like
  exceptions and are not. Where PostgreSQL itself calls two
  datums **equal**, they may share a key — that is agreement
  with PG, not a collision; `-0.0` and `0.0` are the case.
  And where a key is deliberately **not** injective, the
  truncated secondary-index key of §3.2, the entry is
  flagged and rechecked against the base row, which is a
  documented exception rather than a silent one.
- Multi-column keys concatenate without ambiguity: no encoded
  datum may be a prefix of another that should sort after it.
- Per column: ascending or descending, and nulls first or
  last. A descending column is the ascending encoding with its
  bytes inverted, since sm1 will not scan backwards.
- Text ordering is a **collation sort key**, not the UTF-8
  bytes. The key is the sort key **followed by the original
  bytes as a tie-break**. The reason is injectivity, not
  recoverability: a deterministic collation can give two
  distinct strings the same sort key, and PostgreSQL then
  orders them bytewise and keeps them as two datums, so a
  key that stopped at the sort key would merge two rows into
  one. The tuple holds the datum regardless (§5).
- **decimal** needs nothing bespoke, because §2.2 made it a
  fixed-width two's-complement integer: flip the sign bit,
  write the bytes big-endian, and it is an integer's key.
  Two datums only compare at a common scale, and the schema
  fixes the scale per column, so the encoder never rescales.
- **interval** is the one type whose PostgreSQL order is not
  injective (§2.4), and a primary key cannot have two
  distinct datums under one key. So its key is PG's flattened
  datum followed by just enough of the original to tell
  PG's equals apart:

  ```
  key(interval) = span(int128) ‖ months(int32) ‖ days(int32)
  ```

  24 bytes, each component sign-flipped and big-endian, and
  inverted as a whole for a DESC column like any other. The
  microsecond component is not stored, because it is recoverable:
  PostgreSQL's flattening is exactly

  ```
  span = time + (months * 30 + days) * USECS_PER_DAY
  ```

  so `time = span - (months * 30 + days) * USECS_PER_DAY`.
  Dropping it is a space saving, not a requirement — the
  tuple holds the interval either way (§5) — but it is also
  what makes the key injective for free, since the three
  components are recoverable from it. Both the identity and the
  injectivity are arithmetic, so step 2 of `PLAN.md` checks
  them by property over generated intervals rather than by
  example. The
  order this gives **refines** PostgreSQL's — it agrees
  wherever PG is decisive, and where PG calls two distinct
  datums equal it breaks the tie deterministically rather
  than losing one of them. That is the same shape as the
  collated-text rule above: an ordering datum that is not
  injective, with the identity it dropped appended behind
  it.
- The encoder's signature takes, per column,
  `(type, collation, direction, nulls-order)`. Baking in
  "ascending, nulls low, bytewise" makes half of SQL's `ORDER
  BY` clauses unrepresentable.
- PostgreSQL orders text with `strcoll` or ICU, not `memcmp`,
  so the collation is an input to the encoder and not a
  property of the bytes. Sort keys are not stable across
  collation library versions, and PostgreSQL has had index
  corruption from exactly that (`pg_collation.collversion`
  exists for it), so the collation **version is recorded with
  the schema** and a mismatch is detected, not trusted. Where
  that is persisted, once schemas are, is `sm3/SM3_DESIGN.md` §2.
- **The comparison is `memcmp`, always.** sm1 has no
  comparator to install (§1), so the order has to be in the
  bytes before sm1 ever sees them. `strcoll` is never called
  at compare time: it is slow per comparison, it wants
  NUL-terminated input where a key may contain `0x00`, it
  reads process-global locale state, and it is a *comparator*,
  which is the one thing sm1 cannot be given. The
  locale-aware work happens **once, in the encoder** —
  `strxfrm`, or ICU's `ucol_getSortKey` — and sm1 `memcmp`s
  what it produced. A column that needs no locale order skips
  the transform and uses the UTF-8 bytes, which is codepoint
  order.
- **`strxfrm`'s caveats**, because they land on the format
  rather than on the code: its output cannot carry an
  embedded `0x00`, which is what makes the `0x00`-terminated
  framing in `TYPE.md` §2 safe to wrap around a sort key; it
  runs roughly 2-4x the length of its input, which is spent
  against `sm1::max_keylen` and is the arithmetic behind the
  over-long-key rule in §3.2; it must be called
  as `strxfrm_l` with an explicit `locale_t` rather than
  through `setlocale`; and glibc's collation is not stable
  across versions, so a stable key means pinning ICU and
  using `ucol_getSortKey` — which is what §3.3 settles on.
- The ban on backward scans stands, and the planner is changed
  not to ask for one (`PART4.md` §5). A DESC column is
  therefore the inverted encoding scanned forward, and an
  index-ordered `ORDER BY x DESC LIMIT n` becomes a top-N sort
  over a forward scan (`sm3/SM3_DESIGN.md` §7).

This encoder is sm2's, but sm3 is its other caller: a
secondary index entry is `encode(sk) ‖ rowno`
(`sm3/SM3_DESIGN.md` §3), built from the same function with the
same per-column arguments. It is one encoder with two callers,
not two encoders that have to agree.

### 3.1 Framing: null markers and terminators

The byte-level rules are in `TYPE.md` §2 and §3.6; the
decisions behind them are here.

- **Every key column carries a null marker byte, `NOT NULL`
  or not**, primary key columns included. Skipping it for a
  `NOT NULL` column would save a byte per column per key, but
  it would make the key layout depend on nullability, so
  `SET` or `DROP NOT NULL` on a key column would rewrite every
  key in the table and every index over it. The byte is
  cheap; a schema change that silently rewrites a table is
  not.
- **Variable-length datums are escaped and terminated**:
  inside the datum `0x00` is written `0x00 0xFF`, and
  `0x00 0x00` ends it. That keeps the encoding
  order-preserving and prefix-free, which §3's concatenation
  rule needs. It is the scheme CockroachDB and FoundationDB
  use; the alternatives cost a length prefix, which breaks
  ordering, or a wider alphabet.
- **The last column keeps its terminator.** Dropping it would
  save two bytes, but sm3 appends `rowno` to most index
  entries and not to others (`sm3/SM3_DESIGN.md` §3), so which
  column is last is not something the encoder can see. The
  rule has no condition in it.

### 3.2 Over-long keys

`sm1::max_keylen` is 4096 and bounds the whole key: the 2-byte
object prefix (§5), then the encoded index columns, then the
8-byte `rowno` where the entry carries one. A row's key is the
prefix and `rowno` alone, so only index keys can be long.
Collation sort keys run several times the length of their
text, so the limit is reachable.

- **An over-long primary key refuses the row.** The primary
  key index enforces uniqueness on exact key bytes
  (`sm3/SM3_DESIGN.md` §3), so its key must be exact. PostgreSQL
  likewise refuses an oversize btree entry.
- **An over-long secondary index key is truncated, flagged and
  rechecked.** Truncation keeps order as a coarsening — if
  `a <= b` then `trunc(a) <= trunc(b)` — so a range scan over
  truncated keys returns a superset, and a recheck against
  the original, which the tuple holds, removes the extras. The
  entry is flagged as cut, because the index can no longer
  promise order among entries that share the prefix. A unique
  check on a cut key compares originals over the prefix range
  rather than trusting key equality, and treats NULLs as
  distinct, as every unique check does (`DESIGN.md` §5 at the
  top level).
- **A key is never hashed.** A hash keeps equality and
  destroys order, and an index that cannot serve a range or an
  `ORDER BY` is not worth having.

The index columns' budget is 4094 bytes, less 8 for `rowno`
where the entry carries one. It no longer depends on the
primary key's length, because an index entry points at
`rowno`, not at the primary key.

### 3.3 Collation version and determinism

- **ICU is pinned.** A specific ICU version is linked into the
  server rather than whatever the operating system has, so the
  collation version changes only when an operator upgrades on
  purpose. glibc's collation is not stable across versions,
  which is why it is not the source of sort keys.
- **A version mismatch is refused.** The version is recorded
  with the schema (`sm3/SM3_DESIGN.md` §2) and compared at open.
  Every index built under another version, the primary key
  index included, refuses reads and writes, with a message
  saying why.
  PostgreSQL only warns and carries on, but writing new sort
  keys into an old index corrupts it further, and a unique
  index quietly stops being unique.
- **Rebuilding is a command someone chooses to run.** Any
  index, the primary key index included, is rebuilt online
  (`sm3/SM3_DESIGN.md` §3). The table itself never needs it: rows
  are keyed by `rowno`, which no collation touches. Refusal is
  still the default rather than an automatic rebuild, because
  a rebuild of a large index is work an operator should
  schedule.
- **Non-deterministic collations are refused** when a column
  or index is created, with a clear error. Under one, two
  different strings compare equal, and hashing, grouping and
  the bytewise tie-break all have to change with it.
- **The `deterministic` flag is carried from the start**,
  per column, set at creation and never changed. A
  deterministic collation's key is the sort key followed by
  the original bytes as a tie-break; a non-deterministic
  one's would be the sort key alone. Because the flag is
  already in the schema, a later release can accept the
  second kind without touching data on disk.

---

## 4. The tuple format and its codec

**Decision: the tuple codec is an API of its own**, not a
private detail of the tuple relation. It is a separate
component with its own header, designed, built and tested
before anything calls it:

```
tuple_encode(schema, datums[], nulls[])  -> bytes
tuple_decode(schema, bytes, ordinal)     -> one datum
```

Three reasons to split it out rather than let it live inside
§5:

- **It is the part with a permanent on-disk format.** It
  deserves its own tests and its own golden-bytes fixtures,
  not tests that reach it through a relation, a session and a
  transaction.
- **It has two callers already.** sm3 stores INCLUDE columns
  in its index entries (`sm3/SM3_DESIGN.md` §3) and decodes them
  with this same codec, without a base relation in sight.
- **It is the only part of sm2 that does not touch sm1.** A
  codec with no database under it is testable in a unit test
  with no database in it.

**The datums are PostgreSQL's; the framing is sm2's.** §2.1
fixes what each column's bytes are, and stops there. The
tuple around them — the null bitmap, the fixed/variable
split, the alignment, the schema version stamp, the offsets —
owes `HeapTupleHeader` nothing and is designed here on its own
merits. This is also where sm2's TOAST lives, in sm2's own
format rather than PostgreSQL's: a `text` datum in an sm2
tuple is payload bytes with its length in the framing, with
no varlena header, and a TOASTed datum is replaced by sm2's
own TOAST pointer rather than PostgreSQL's (§2.1).

*To be written.* The rules the format itself must satisfy:

- A null bitmap, a fixed/variable split, and enough alignment
  discipline that reading a column does not require decoding
  every column before it.
- A **schema version stamp per row**, so `ADD COLUMN` is O(1):
  an old row is read under its own layout and the missing
  column comes from its default. The stamp is a number; what
  it resolves to is a schema, which sm2 is handed (§5) and
  sm3 persists (`sm3/SM3_DESIGN.md` §2).
- Tolerance for dropped columns, since the column at a given
  position may no longer exist.
- **Cheap to turn into a PostgreSQL tuple.** The executor
  works in tuple slots, so a bespoke encoding pays a
  conversion on every row, in both directions. The threaded
  fork removes the IPC boundary, not the conversion.
- Large datums are **TOASTed** above a threshold: stored out
  of line as chunks, with a TOAST pointer left in the tuple.
  sm1 permits a 64MB value, but a transaction's undo, lock and
  read sets are linear in the keys it touches
  (`sm1/SM1_DESIGN.md` §13), and very large datums hurt LSM
  compaction, so the threshold is well below sm1's ceiling.
  The TOAST data lives in the table's own sm1 store, under
  the object prefix `0xFFFF` (§5), so writing a row and its
  TOAST data is one atomic sm1 write, no chunk ever falls
  inside a scan of the rows or of an index, and `DROP TABLE`
  takes the TOAST data with it.

---

## 5. The tuple relation

*To be written.* sm1's store is an ordered map of byte
string to byte string. sm2's is that store with the types
put back: a row goes in as a tuple and comes out as a tuple,
and §3 and §4 happen inside. It is the whole of sm2's API
surface; §2 to §4 are what it is built out of.

A `rel` is opened over exactly one sm1 store, with three
things:

- a **schema** — the ordered columns, their types and
  collations, and per key column its direction and null
  ordering;
- an **object prefix** of exactly 2 bytes, big-endian,
  prepended to every key sm2 writes and stripped from every
  key it reads. sm3 chooses the value and sm2 never interprets
  it, so sm2 never learns what a table is. It exists because
  one sm1 store holds one table and everything that belongs
  to it (`sm3/SM3_DESIGN.md` §4):

  | Prefix | Object |
  | --- | --- |
  | `0x0000` | the table's rows, keyed by `rowno` |
  | `0x0001`–`0xFFFE` | its indexes, one prefix each |
  | `0xFFFF` | its TOAST data (§4) |

  Index prefixes are allocated in increasing order and never
  reused, so a dropped index's leftover versions can never be
  read as another index's entries, and no rule tied to sm1's
  garbage collection is needed. `DROP INDEX` is a range delete
  over its prefix. The prefix is fixed-width, so no object's
  prefix can be the start of another's. It spends 2 of
  `sm1::max_keylen`'s 4096 bytes, and sm2 checks the whole key
  against the limit (§3.2);
- an **sm1 session**, which is where the transaction lives.

**Decision: the table is a heap keyed by `rowno`, and the
primary key is an index**, as in PostgreSQL. A row's sm1 key
is `0x0000 ‖ rowno`:

- **`rowno` is an unsigned 64-bit number**, written as 8
  bytes big-endian, drawn from an sm1 sequence that belongs to
  the table (`sm1/API.md`, the sequence API). It is never
  reused, survives a crash, and may have gaps. The whole 64-bit
  range is usable.
- **An update keeps its `rowno`.** sm1 writes the new version
  under the same key, so a row's identity never changes and an
  index entry that points at it stays valid. PostgreSQL's heap
  gives a new TID to every non-HOT update and must insert into
  every index; this does not.
- **The primary key is an index like any other**, sm3's to
  build and maintain (`sm3/SM3_DESIGN.md` §3), keyed by the
  encoded key columns and mapping to `rowno`. A table without
  a primary key needs nothing extra: every table is a heap.
- **`rowno` is PostgreSQL's `ctid`.** The fork redefines
  `ctid` as a `uint64_t` rather than PostgreSQL's 6-byte
  `ItemPointerData` (`PART4.md` §3), so every `rowno` is a
  valid `ctid`. It differs from PostgreSQL's in one way: it
  names the row, not the row version, so it does not change
  on `UPDATE`.

Why a heap and not a table clustered on its primary key:
PostgreSQL's planner and executor assume a heap with every
index secondary, a table may have no primary key at all,
changing a primary key must not rewrite every index, and a
fixed 8-byte `rowno` is a smaller index suffix than an
encoded primary key. The costs are in §7.

The operations, and what each one is over:

| sm2 | what it adds | over |
| --- | --- | --- |
| `rel_insert` | writes a tuple under a new `rowno` and returns the `rowno` | `sm1::nextseq`, `sm1::insert` |
| `rel_read` | reads a row by `rowno` and decodes it into a tuple | `sm1::read` |
| `rel_update`, `rel_delete` | by `rowno`; hands back the old image as a **tuple**, not bytes | `sm1::update`, `sm1::erase` |
| `rel_read_latest` | the newest committed image, decoded | `sm1::read_latest` |
| `rel_scan`, `rel_scan_seek` | tuples in `rowno` order, forward only | `sm1::scan`, `sm1::scan_seek` |

How sm3 writes and scans index entries through sm2 — keyed by
encoded columns rather than by `rowno` — and who draws the
`rowno` from the sequence are open (`QUESTIONS.md` 5.6).

The rules it must satisfy:

- **sm2 is a codec, not a state machine.** Every durable
  effect is an sm1 call inside the caller's transaction, and
  sm2 keeps nothing across calls that a rollback would have to
  undo. This is what makes savepoints and abort cost sm2
  nothing at all.
- **Errors pass through; they are not translated.**
  `SM_ERR_UPDATED` in particular is not retried here. The
  retry needs the statement's filter re-checked against the
  newest image (`sm1/SM1_DESIGN.md` §5.4), and filters live above
  sm2; `rel_read_latest` is what sm2 provides so sm3 can do it
  (`sm3/SM3_DESIGN.md` §3).
- **sm2's own failures go through sm1's error state.** A key
  over the limit, a datum that does not match its column's
  type, an unknown schema version, a corrupt tuple: sm2
  records each with `sm1::seterr`, its codes appended to
  `sm1::errnum_t` under a comment naming sm2, so one
  `sm1::errmsg()` serves the whole stack (`DESIGN.md` §5 at the
  top level).
- **Running out of memory is `SM_ERR_NOMEM`.** A
  `std::bad_alloc` reaches sm2 only from its own allocations,
  since inside sm1 it is fatal to the process; every sm2 entry
  point catches it and returns -1 with `SM_ERR_NOMEM`
  (`DESIGN.md` §5 at the top level). A type's `cmp` and `hash`
  have no error to return, so they must not allocate.
- **The interface is `include/sm2.h`, in `namespace sm2`**, as
  every level's is (`OBJECTIVE.md` at the top level):
  `sm2::rel_insert`, `sm2::tuple_encode`. This document writes
  the short names — `rel_insert` for `sm2::rel_insert`.
- **Every column is in the tuple.** An encoded key — the
  primary key index's or any other's — is an ordering device,
  not storage, so an indexed column is written twice: encoded
  in the index, native in the tuple. The tuple is what a read
  returns, and `key_encode` never has to be reversible, which
  is a hard promise to keep over collation sort keys,
  truncated keys and any future lossy encoding. The cost is
  named in §7.
- **Decoding is per column, not per row.** Reading one column
  of a wide tuple must not cost the whole tuple, or a
  predicate above sm2 pays for every column it does not look
  at. `rel` gets this from `tuple_decode`'s by-ordinal
  signature (§4); it does not implement it a second time.
- **`rel` calls the tuple codec, it does not contain it.**
  §4 is a component with its own header and its own tests,
  and `rel` is one of its two callers — sm3's INCLUDE columns
  are the other.
- **Forward only.** A `rel` scan inherits sm1's ban, and the
  descending case is already handled in the encoding (§3).

Open: whether a `rel` copies the schema it is opened with or
borrows it — sm3's catalog is the owner, and a schema change
under a live `rel` is what makes the answer matter; and
whether the tuple-at-a-time shape above is enough, or a
batched form earns its keep once there are predicates to push
down, which is a question for the level that has them
(`sm3/SM3_DESIGN.md` §5).

---

## 6. What sm2 leaves to sm3

Named here so the boundary is not re-argued in both documents.
sm3 is a client of sm2 the way sm2 is a client of sm1.

- **The catalog.** sm2 is *handed* a schema; it neither
  persists one nor looks one up, and it has no notion of a
  table name, a DDL statement or a schema lock.
- **Secondary indexes.** sm2 maintains no index. It gives sm3
  the two things index maintenance needs: the encoder (§3) and
  the old image as a tuple (§5).
- **Object mapping.** Which sm1 store holds a table, and
  which prefix each of its indexes gets, is sm3's; sm2 only
  prepends and strips the prefix (§5).
- **The primary key.** sm2's table is a heap keyed by
  `rowno`; the primary key is one of sm3's indexes, and its
  uniqueness is sm3's (§5).
- **Predicates.** sm2 has no filter, and therefore cannot
  resolve `SM_ERR_UPDATED` or derive a key range from a
  `WHERE` clause.
- **Statistics.** sm2 counts nothing.

---

## 7. Known trade-offs

*To be filled in as the design settles, in the manner of
`sm1/SM1_DESIGN.md` §13 — the costs this design accepts, written
down where the next reader will find them.*

Accepted so far:

- **Collated keys are stored twice.** The sort key is lossy,
  so the original datum is stored beside it (§3).
- **A tuple is not a PostgreSQL tuple — but its columns are
  PostgreSQL datums.** §2.1 makes the conversion at the
  executor boundary per tuple, not per datum: the framing is
  rebuilt, the column bytes are copied. The threaded fork
  removed the IPC boundary but not this (`PART4.md` §2).
- **decimal is the exception, and pays for it.** It follows
  DuckDB's decimal, not PostgreSQL's numeric (§2.2), so it is the
  one type converted datum by datum, it caps at 38 digits
  where PG allows 1000, and it has no NaN and no infinity.
  Bought in exchange for a `decimal` key encoding that is
  an integer's, and a fixed-width `decimal` column.
- **Little-endian is in the format.** §2.1 fixes the byte
  order that PostgreSQL leaves native, so a big-endian host
  byte-swaps on every datum where others `memcpy`.
- **Indexed columns are stored twice**, encoded in the index
  and native in the tuple (§5), the primary key's included, as
  in PostgreSQL. It buys a tuple that is complete on its own
  and a `key_encode` that is allowed to be lossy.
- **A collated text index column is stored three ways**: the
  sort key and the tie-break original in the index key (§3),
  and the datum in the tuple. The tie-break is not optional —
  without it two distinct strings can share one key.
- **A primary key lookup is two reads.** The primary key index
  gives `rowno`, and the row is a second read (§5). A table
  clustered on its primary key would answer in one. Both keys
  are in the same sm1 store, and PostgreSQL pays the same
  price, which its planner already costs.
- **A primary key range scan loses locality.** Rows are in
  `rowno` order, roughly insertion order, so a range over the
  primary key reads rows scattered across the heap.
  PostgreSQL has the same property; InnoDB and SQL Server
  cluster to avoid it.
- **Every insert writes the row and its primary key entry**,
  two sm1 keys where a clustered table writes one, plus one
  per other index as before.
- **`ctid` is redefined.** Exposing `rowno` as `ctid` means
  widening PostgreSQL's 6-byte `ItemPointerData` to 64 bits in
  the fork, which reaches index tuples, heap tuple headers and
  TID bitmaps (`PART4.md` §3). And since `rowno` names a row,
  not a version, code that expects `ctid` to change on
  `UPDATE` sees it stay put.
- **Descending order costs an encoding, not a structure.**
  Inverted bytes scanned forward means no mirror relation and
  no second copy, but it also means a column's direction is
  fixed when the key is written: changing `ASC` to `DESC`
  rewrites the keys.
- **The type set is closed at the format.** New types are
  cheap, but only while the type code and the tuple layout
  leave room for them; that room is spent once.
- **A bare `decimal` can round.** It defaults to
  `decimal(18, 3)` (§2.2), so a datum a user believed exact
  loses digits past the third fractional place, with no error,
  and one of 10^15 or more is refused.
  Refusing a bare `decimal` would have been louder; the default
  was chosen for compatibility.
- **Every key column pays a null marker byte**, `NOT NULL`
  ones included (§3.1). It buys a key layout that no
  nullability change can rewrite.
- **Tables are bounded by sm1's store count.** One sm1
  store per table (§5) makes `DROP TABLE` O(1), and caps
  tables, partitions included, at the hundreds to low
  thousands of stores sm1 handles. Accepted for v1.
