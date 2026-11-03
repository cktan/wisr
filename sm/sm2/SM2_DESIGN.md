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
| Representation | PostgreSQL's, type by type, little-endian — except the decimals, which follow DuckDB's, with their scale |
| Key encoding | order-preserving per type; descending is the inverted encoding |
| Tuple format | null bitmap, fixed/variable split, an attribute count per row, as PostgreSQL's `natts` |
| Tuple codec | an API of its own, with its own header and its own tests |
| Row identity | `_id`, a 64-bit number from a per-table sequence; the table is keyed by its primary key, or by `_id` without one (§5) |
| `rel` | one sm1 store, keys encoded and tuples decoded by sm2 |
| API shape | one tuple at a time; batching belongs where filters do (`sm3/SM3_DESIGN.md`) |
| PostgreSQL | a target that shapes decisions, never a build dependency |

**Status: all four steps of §8.2 are built** — the types, the
key encoding, the tuple codec and the tuple relation, heap and
index — and TOAST with it (§4.2). This file was written so the
formats were argued before they were written, because two of
them — the key encoding and the tuple layout — are on disk and
effectively permanent once any data exists. §8 has the order
of work. The PostgreSQL target (`PART4.md`) shapes
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
| `SM_ERR_DUPKEY` from `sm1::insert` | a unique index keyed without its row's key gets its uniqueness from sm1 for free, and so does the primary key, the rows' own key (§5, `sm3/SM3_DESIGN.md` §3) |
| `SM_ERR_UPDATED` and `sm1::read_latest` at READ COMMITTED | sm2 surfaces the error and offers `rel_read_latest`; it cannot resolve it, because the re-check needs a filter and filters are sm3's (§5) |
| nested savepoints | a statement can be undone without losing the transaction, and sm2 holds no state that has to be undone with it |
| forward scans with `sm1::scan_seek` | probe loops cost one cursor, not one per probe |
| `sm1::max_keylen` 4096, `sm1::max_datalen` 64MB | an encoded index key must fit 4096 bytes, relid included; a datum above the TOAST threshold is stored out of line (§4) |
| no backward scan, ever | a descending column is stored with inverted bytes and scanned forward; there are no mirror relations, and the fork's planner never asks for a backward scan (§3) |

sm1 keeps one column family per store and handles hundreds
to low thousands of them. sm3 gives each table one sm1
store, holding the table's rows, its indexes and its
TOAST data, told apart by the 2-byte relid in
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

The rules it satisfies:

- The set is fixed and chosen from what the PostgreSQL target
  needs first: boolean, the integer widths, `float4` and
  `float8`, `decimal64` and `decimal128`, `text` / `varchar` /
  `bytea`, `date`, `time`, `timestamp`, `timestamptz`,
  `interval`, `uuid`.
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
  length limit is a coercion the host applies before the value
  arrives (`DESIGN.md` §5); sm2's codec stores what it is
  given.
- A type is three things together: an in-memory datum, an
  order-preserving key encoding (§3), and a tuple encoding
  (§4). A type is not added until it has all three.
- Every type has a **small stable code that goes on disk**, in
  the schema rather than in each row. The codes in `TYPE.md`
  §3 are frozen as they stand, `bool` = 1 through `name` = 20,
  7 retired (§2.2): assigned once, never renumbered and never
  reused, so a type removed from the set does not free its
  number. Codes 64–127 are reserved for types added later, and
  0 is never a code.
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
type by type, with the decimals the one exception.** Not a format
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
not checked against the source; step 1 of §8.2 pins each
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
  framing, and its own TOAST in its own format. In a tuple a
  variable datum is stored as a PostgreSQL varlena, header and
  all, so PostgreSQL can read it in place (§4.1,
  `VARLENA.md`); a type's functions take the payload alone.

### 2.2 Except decimal, which follows DuckDB's

**Decision: the decimals are fixed-width, with DuckDB's
`DECIMAL` as their reference, and there are two of them:
`decimal64` and `decimal128`.** PG's numeric is a varlena of
base-10000 digits with a weight and a display scale,
effectively unbounded in length. DuckDB's is a two's-complement
integer, the value times 10^s, with `(precision, scale)` held
in the schema and at most 38 digits of precision. sm2 takes that
with two of DuckDB's four widths, each its own type:
`decimal64`, 64 bits for a precision up to 17, and
`decimal128`, 128 bits up to 36, the scale in the low bits of
each, which costs a digit and two of DuckDB's 18 and 38
(§2.2). (DuckDB also has 16- and
32-bit forms up to 4 and 9 digits; sm2 does not.) An earlier
draft took Arrow's decimal, with a 256-bit form up to 76
digits; DuckDB's limit of 38 was preferred. Three things
follow, and they are why the exception is worth making:

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

- **Precision stops at 36 digits.** PG allows 1000 for a
  declared typmod, and far more for an undeclared one. A
  column that needs more has no representation at all.
- **There is no NaN and no infinity.** PG's numeric has had
  NaN throughout and `Infinity` since version 14 (from memory,
  not checked against the source); a fixed-width decimal is an
  integer and has neither, so sm2 refuses them on insert.
- **The scale must be declared.** The decimal puts `(precision,
  scale)` in the schema; PG allows a bare `numeric` where
  every datum carries its own. A bare `decimal` column has no
  equivalent, so its host gives it a declared `(p, s)` —
  digres gives `(17, 3)`, the widest `decimal64` — which can
  round a datum its user believed exact.
- **They are the types that are converted** at the executor
  boundary, in both directions, while §2.1 makes every other
  type a copy.
- **SQL's `decimal(p, s)` is not an sm type.** The host maps
  it to one of the two, choosing the width
  (`sm3/SM3_DESIGN.md` §2.1; digres's rules are
  `../../dg/DECIMAL.md` §2.3), so the names a user writes are
  resolved above sm.

What a column may declare, and what a datum may hold:

- **Two types, one per width.** `decimal64` is 8 bytes and,
  like `int8`, passed by value; `decimal128` is 16 bytes and
  passed by reference, like `uuid`. Each takes PostgreSQL's
  numeric typmod, `(p, s)`, and accepts any precision up to its
  capacity — 17 for `decimal64`, 36 for `decimal128` — so a
  `decimal128(10, 2)` is legal, if wider than it need be. Every
  sm2 type's width is then its type's alone, not its typmod's.
  An earlier design had one type, `decimal`, code 7, whose
  typmod chose the width; it was split so that no width
  depends on a typmod, and code 7 is retired.
- **The typmod is required.** There is no bare form: `-1` is
  refused, like any typmod outside the type's range, by the
  type's `typmod_valid`, which `rel_open` and the codec check
  through `datum_width`. The host resolves a bare `decimal`,
  `decimal(p)` and the `numeric` alias to a declared `(p, s)`;
  sm takes what it is given.
- **The scale is in the datum**, below the value:
  `(value << 5) | s` for `decimal64`, `(value << 6) | s` for
  `decimal128` (`TYPE.md` §3.4), so a datum means a value
  without its typmod, and a host can pass a `decimal64` by
  value as it is. It is always the column's scale, so the
  scale bits of a column's datums are equal.
- **One key rule at two widths.** Both are the same
  two's-complement integer, so the key is §3.2's at 8 or 16
  bytes and comparison is integer comparison. A variable-width
  representation was rejected outright: a second key encoding
  and comparison path is most of what this section avoids.
- **NaN and infinities are refused** on insert with a clear
  error, and no value is reserved as a sentinel. That door is
  not closed: 17 digits do not reach the limit of the 59 bits
  above the scale, nor 36 that of the 122, so the extreme
  integers are never written and could still be reserved
  later without changing what
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
| `hash` | for grouping and hash joins — **deferred**: the slot exists, unfilled, until a step needs it | the hash opclass support function |
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
count reaches about 5.8e21, well past `int64`). That order is
deliberately **not injective**: `'1 mon'` and `'30 days'` both
flatten to 2592000000000, and PG's btree calls them equal.

Equal keys are exactly what a primary key cannot have, so §3
says what an interval key is.

Whether `interval` is in the first set is settled here: it
is.

---

## 3. Key encoding

The per-type encodings are in `TYPE.md` §3; the rules they
all have to satisfy are here.

The rules it satisfies:

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
  injectivity are arithmetic, so step 2 of §8.2 checks
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
secondary index entry is `encode(sk) ‖ row key`, the row key
the primary key's encoding or `_id` (`sm3/SM3_DESIGN.md` §3),
built from the same function with the same per-column
arguments. It is one encoder with two callers, not two
encoders that have to agree.

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
  save two bytes, but sm2 appends the row's key to most index
  entries and not to others (`sm3/SM3_DESIGN.md` §3), so which
  column is last is not something the encoder can see. The
  rule has no condition in it.

### 3.2 Over-long keys

`sm1::max_keylen` is 4096 and bounds the whole key: the 2-byte
relid (§5), then the encoded index columns, then the row's key
where the entry carries one: 8 bytes of `_id`, or the primary
key's encoding, at most `max_pk_len`, 1024 bytes. A row's own
key is refused past that, so only index keys can be cut.
Collation sort keys run several times the length of their
text, so the limit is reachable.

- **An over-long key in a unique index refuses the row.** A
  unique index, the primary key's among them, enforces
  uniqueness on exact key bytes (`sm3/SM3_DESIGN.md` §3), so
  its key must be exact. PostgreSQL likewise refuses an
  oversize btree entry.
- **An over-long key in a non-unique index is truncated,
  flagged and rechecked.** Truncation keeps order as a
  coarsening — if `a <= b` then `trunc(a) <= trunc(b)` — so a
  range scan over
  truncated keys returns a superset, and a recheck against
  the original, which the tuple holds, removes the extras. The
  entry is flagged as cut, because the index can no longer
  promise order among entries that share the prefix.
- **A key is never hashed.** A hash keeps equality and
  destroys order, and an index that cannot serve a range or an
  `ORDER BY` is not worth having.

The index columns' budget is 4094 bytes, less the longest row
key an entry may carry where it carries one: 8 for a table
keyed by `_id`, 1024 for one keyed by a primary key.

sm2's part is the encoder's: `encode_key` takes the columns'
budget, `key_budget(suffix)`, and what to do past it —
`overflow_t::refuse`, which fails with `SM_ERR_KEYTOOLONG`, sm2's
first error code, or `overflow_t::truncate`, which keeps the
budget's bytes and says it cut. The index `rel` (§5) uses
both: in a unique index it refuses an over-long entry key,
and in a non-unique one it cuts the key and marks it cut in
the entry's value; which rows to recheck, and how, are sm3's
(`sm3/SM3_DESIGN.md` §3).
A cut key can never equal a whole one, since whole keys are
prefix-free (§3.1), so a flagged entry is never mistaken for an
exact one.

### 3.3 Collation version and determinism

- **ICU is pinned.** A specific ICU version is linked into the
  server rather than whatever the operating system has, so the
  collation version changes only when an operator upgrades on
  purpose. glibc's collation is not stable across versions,
  which is why it is not the source of sort keys. For now sm2
  links the system's ICU, 74 on the development machine: the
  version check below turns an operating-system upgrade into a
  refusal rather than a silent reordering, and pinning is left
  to packaging.
- **A collation is opened by locale**, a BCP 47 tag as
  PostgreSQL's ICU collations take (`en-US`,
  `und-u-ks-level2`), with `C` and `POSIX` meaning byte order.
  A language ICU does not know is refused, as PostgreSQL's
  `icu_validate_locale` refuses it; ICU itself would quietly
  sort it by its root order. One collation serves every
  session, on the strength of ICU's documentation that const
  operations on a collator, the sort key among them, are safe
  from several threads at once — not yet checked against ICU's
  source.
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
  (`sm3/SM3_DESIGN.md` §3). A table keyed by a primary key
  under a collation is rewritten instead (`../CLUSTER.md` §8);
  one keyed by `_id` never needs it. Refusal is
  still the default rather than an automatic rebuild, because
  a rebuild of a large index is work an operator should
  schedule.
- **Non-deterministic collations are accepted**, for ICU
  only: byte order is refused as non-deterministic, as
  PostgreSQL refuses it for libc. Under one, strings ICU calls
  equal are equal, as PostgreSQL's `varstr_cmp` makes them by
  skipping its bytewise tie-break.
- **The `deterministic` flag is per column**, set at creation
  and never changed. A deterministic collation's key is the
  sort key followed by the original bytes as a tie-break; a
  non-deterministic one's is the sort key alone, then
  `0x00 0x00` (`TYPE.md` §3.5). Equal strings then share a
  key, so a unique index refuses `'ABC'` after `'abc'` under a
  case-insensitive collation with sm1's `SM_ERR_DUPKEY`, and
  an equality bound finds both in a non-unique one: PostgreSQL's
  answers, with no machinery of sm2's own.
- **Such a key holds no string.** Its `key_decode` fails with
  `SM_ERR_INVALID`. Nothing needs it: every column lives in
  the tuple (§5), which returns the bytes as written, as
  PostgreSQL's index holds the original datum.
- **Equality above sm2 must follow.** Hashing, grouping,
  `DISTINCT` and a recheck that compares strings must compare
  them under the collation — the key, or the sort key — not
  the datum's bytes, as PostgreSQL's `hashtext` hashes the sort
  key (`sm3/SM3_DESIGN.md` §3). A type's `cmp` stays byte
  order, so it is not that comparison.

---

## 4. The tuple format and its codec

**Decision: the tuple codec is an API of its own**, not a
private detail of the tuple relation. It is a separate
component with its own header, designed, built and tested
before anything calls it:

```
tup_encode(schema, datums[], nulls[])  -> bytes
tup_decode(schema, bytes, ordinal)     -> one datum
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
split, the attribute count, the slots —
owes `HeapTupleHeader` nothing and is designed here on its own
merits. This is also where sm2's TOAST lives, in sm2's own
format rather than PostgreSQL's: a `text` datum in an sm2
tuple is a PostgreSQL varlena, and a TOASTed datum is replaced
by sm2's own TOAST pointer, in PostgreSQL's external form with
a tag of sm2's own (§2.1, §4.1).

The rules the format itself satisfies, then the layout that
meets them (§4.1):

- A null bitmap, a fixed/variable split, and a way to find a
  column without decoding every column before it.
- **An attribute count per row, as PostgreSQL's `natts`**, so
  `ADD COLUMN` is O(1). Columns are only ever appended: a row
  holds the first `natts` of them, and a column past a row's
  count reads as its missing value — null, or a default kept
  once in the catalog, PostgreSQL's `attmissingval`
  (`sm3/SM3_DESIGN.md` §2). There is **no schema version**. A
  change to a column's stored bytes, such as a new type,
  rewrites the table, as it does in PostgreSQL, so every row
  reads under the one current schema, which sm2 is handed
  (§5). A version stamp per row was the earlier draft; it
  would have let a type change skip the rewrite, at the price
  of sm3 keeping every schema a table ever had and sm2
  resolving a stamp on every read.
- **A dropped column keeps its place**, marked dropped, as
  PostgreSQL's `attisdropped` marks it: an old row's bytes for
  it are skipped, a new row stores it as null, and the columns
  after it do not move.
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
  A tuple is at most 64 KB (§4.1), so TOAST must be decided
  per tuple, not per datum: as PostgreSQL does, it moves the
  largest datums out of line until the tuple fits, since many
  columns each under a datum threshold can still pass the
  limit. The fixed part always fits: PostgreSQL's 1600
  columns at 16 bytes, or a 22-byte TOAST pointer each, stay
  under 35 KB.
  The TOAST data lives in the table's own sm1 store, under
  the relid `0xFFFF` (§5), so writing a row and its
  TOAST data is one atomic sm1 write, no chunk ever falls
  inside a scan of the rows or of an index, and `DROP TABLE`
  takes the TOAST data with it.

### 4.1 Layout

```
[ header | null bitmap | fixed part | var data ]
```

The bytes are specified in `TUPLE.md`, and a variable-width
datum's in `VARLENA.md`; this section is why they are so.

**Decision: the datums are serial, in schema order, split into
a fixed part and a variable part, with no padding.** The fixed
part is a table with an entry for every non-null column, in
schema order, end to end: a fixed-width column's datum, or a
variable-width column's **slot**, the position of its varlena
in the tuple. The variable part holds those varlenas, in
schema order too.

- **No alignment.** PostgreSQL pads each attribute to its
  `typalign` because its tuples start MAXALIGNed on aligned
  pages, so the padding buys aligned loads. An sm2 tuple is a
  RocksDB value, which starts wherever it lies in a block, so
  padding inside it would align offsets relative to an
  unaligned start and buy nothing. A fixed-width datum is read
  with `memcpy` instead, which on the targets, x86-64 and
  ARMv8, is one unaligned load (checked: `ldr` on aarch64).
  Packing by alignment — 8-byte columns first, then 4, 2, 1 —
  was considered and rejected: its one gain, aligned loads, is
  the one it cannot deliver here, and it costs a permutation
  from schema order to the tuple's.
- **Schema order, not permuted.** The tuple reads in the order
  the schema declares, so there is no map, and a new schema
  version that appends a column lays out its rows by the same
  rule.
- **A null takes no space**, as in PostgreSQL, in either part:
  its bit in the null bitmap is all it has. A column's entry
  is then at the sum of the entry widths of the non-null
  columns before it — a fixed column's width, a variable one's
  2-byte slot — computed from the null bitmap and the schema's
  widths, never by walking the data. That is O(columns)
  arithmetic over the bitmap per column read, rather than
  PostgreSQL's walk over the bytes once an earlier column is
  null or of variable width. Keeping an entry for a null
  column would have made every position a constant, at the
  cost of its width per null; the space was preferred.
- **Every column is found the same way.** Because a variable
  column has a fixed-width slot in the table, its entry is
  found by the same arithmetic as a fixed column's, and its
  varlena is one step further, at the position the slot
  holds. When no column is null, which is the common case,
  every entry's position is a constant of the schema and
  `natts`, precomputed once: the row reads like a C struct.
- **A variable datum is a PostgreSQL varlena**, its bytes
  exactly as PostgreSQL's `varatt.h` lays them out, so the glue
  can hand PostgreSQL a `Datum` pointing into the tuple: a
  1-byte header up to 126 bytes, a 4-byte one past that, and
  forms for compressed data and TOAST pointers, neither built
  yet (`VARLENA.md` §2). A datum's header says what it
  is, so a TOAST pointer travels through the executor's slot
  as itself, and is fetched only when a function needs the
  value, and an `UPDATE` that leaves the column alone writes it
  back without a fetch — which is why the mark lives in the
  datum and not in a bitmap beside the null bitmap, the first
  draft, which would have had the executor carry a flag with
  every datum. And a `Datum` is one pointer, so the length must
  be in the bytes it points at; an earlier draft's tag byte
  without a length could not be handed out alone.
- **Alignment: every varlena in place.** PostgreSQL reads a
  4-byte header only on data it knows is aligned, and pads to
  make it so; an sm2 tuple starts unaligned inside a RocksDB
  block, so padding would buy nothing (above). The codec
  stores every varlena in its shortest form, unpadded: up to
  126 bytes takes the 1-byte header, which PostgreSQL reads
  unaligned; a longer one takes the 4-byte header, which the
  glue hands to the executor in place too, unaligned. Its
  header is one 4-byte load, which on the targets, x86-64 and
  ARMv8, takes any address, as a fixed-width datum's does
  (above). A TOASTed datum is copied when fetched. Copying
  every tuple into an aligned buffer and padding within it was
  the alternative; it would copy every row for loads the
  targets do unaligned anyway. The codec takes either
  header on input — PostgreSQL makes 4-byte headers for short
  data after it detoasts — and writes the shortest.
- **A slot holds where its varlena starts, and nothing else.**
  The varlena's header already holds its length, so the slot
  does not repeat it, and a position from the tuple's start
  goes straight to the bytes. A slot is always 2 bytes.
- **A tuple is at most 64 KB, 65535 bytes, TOAST aside.** It
  is the furthest a 2-byte slot reaches, and the writer refuses
  a longer row with `SM_ERR_ROWTOOLONG`, which the glue maps to
  PostgreSQL's `program_limit_exceeded`, as for an over-long
  key. Every slot is then 2 bytes and every entry's width a
  constant of the schema, so a column's position can be
  precomputed per table, not per tuple. It is eight times
  PostgreSQL's 8 KB page, which bounds its tuples. The cost is
  that until TOAST is built a row past 64 KB cannot be stored
  at all. An earlier layout widened every slot to 4 bytes,
  under a header flag, in a tuple past 64 KB, which let the
  TOAST threshold be set on its own merits but made the slot
  width, and every position after a variable column, depend
  on the tuple. Two layouts came before the slot:
  a separate table of end offsets after the fixed part, which
  stored each length twice once the varlena had its own
  header; and dropping positions altogether and walking the
  varlena headers, as PostgreSQL does, which saves the 2 bytes
  but makes a variable column cost a walk over those before
  it. The slot costs the same 2 bytes as the offset table did
  and buys O(1) access to every column.
- **The header is a flag byte and `natts`**, two bytes for a
  row of up to 127 columns: a flag for has-nulls, the rest
  reserved, then `natts` as a varint (`TUPLE.md` §2). The
  has-nulls flag plays the part of PostgreSQL's
  `HEAP_HASNULL`. `0x02` was once to mean a TOAST bitmap was
  present, before the TOAST mark moved into the varlena, and
  `0x04` 4-byte slots, before the 64 KB limit; both are
  reserved now, and no stored tuple uses either. The reserved
  bits are refused rather than ignored, so a tuple written
  with a feature added later is never misread by a reader too
  old to know it. PostgreSQL packs `natts` into 11 bits of
  `t_infomask2`; a varint costs one byte to 127 columns and
  has no ceiling.
- **The null bitmap follows, when present**: a bit for each of
  the row's `natts` columns, set for a null, rounded up to
  whole bytes.

Open: whether sm2 ever compresses a datum inline, as
PostgreSQL does, which its compressed 4-byte form already
provides for.

### 4.2 TOAST

**Decision: a datum out of line is named by an id of its own,
and never changes.** The heap `rel` moves a row's long datums
out of the tuple and into the table's store, and leaves a
pointer for each. Built in `src/rel/toast.cc` and
`src/rel/rel.cc`, with `test/test_toast.cc`.

- **The key of a chunk is `0xFFFF ‖ id ‖ seq`**: the TOAST
  relid, the datum's id as 8 bytes big-endian, and the chunk's
  number as 4. A datum's chunks are next to one another and in
  order, so reading one back is one scan.
- **The id is from a sequence of the table's own**, handed to
  `rel_open` as `toast_seq`. Not the `_id` sequence: `ANALYZE`
  samples a keyless table's `_id`s between 1 and its limit
  (`sm3/SM3_DESIGN.md` §6), and each id drawn from it would be
  a hole a sample falls into. A `rel` opened with no
  `toast_seq` moves nothing out, as before.
- **A datum is written once and only ever erased.** A pointer
  names the same bytes for as long as it names any. The
  alternative was to key a chunk by its row and column, with
  no id: a replaced datum would then overwrite the same keys,
  two writes cheaper, but a pointer read before the change and
  fetched after it would give the new bytes under the old
  length. An id makes that an error, not wrong data. Nor was
  the other scheme's pointer any smaller, since a pointer
  travels through the executor apart from its row and would
  have had to name the row and column itself.
- **The pointer is 22 bytes**: `0x01`, the vartag `0x40`, then
  the datum's length, the store's id and the datum's id,
  little-endian (`toast_ptr_t`, `VARLENA.md` §2). The store's
  id is there so that a pointer carried to another table is
  known for what it is, and refused.
- **Which datums move.** A tuple longer than
  `toast_threshold`, 2000 bytes, has its longest variable-width
  datums moved out, longest first, until it is not; none
  shorter than `toast_min`, 64 bytes, is moved. So a row of
  many short datums can stay longer than the threshold, and
  only `max_tup_len` refuses it. 2000 is PostgreSQL's figure.
- **A chunk is `toast_chunk`, 8192 bytes.** PostgreSQL's is
  about 2000, for its page. Here each chunk is a key in sm1's
  lock, undo and read sets (`sm1/SM1_DESIGN.md` §13), so fewer
  and longer ones are cheaper.
- **The three sizes are constants**, not settings of a table,
  until something measures them.
- **The row is written before its chunks.** The ids are drawn,
  the tuple is encoded with its pointers, the row is written,
  and only then the chunks. sm1's refusal of the row --
  `SM_ERR_UPDATED` under READ COMMITTED above all, which is no
  abort (§5) -- then leaves no chunk behind.
- **Two rows never share a datum.** There is no count of a
  datum's users, so:
  - an insert given a pointer reads the value and stores it
    again under a new id;
  - an update keeps a pointer only if it is the one the row
    holds for that attribute, which is what an update that
    leaves the column alone gives back, and writes nothing for
    it. Any other is refused, after the row is written, so the
    caller rolls back, as after any write that fails past its
    first step.
- **A datum the row no longer holds is erased**, by the update
  that replaced it and by the delete of its row, from the old
  tuple sm1 hands back. A caller that needs the old values
  after the write -- sm3, for the index entries to remove --
  writes with `hold`, and the erase waits for its
  `rel_release`. Those are sm1 erases in the caller's
  transaction: a rollback brings the chunks back, and a
  snapshot older than the commit still reads them.
- **A fetch reads at the transaction's snapshot**
  (`toast_fetch`). PostgreSQL reads TOAST with a snapshot of
  its own that sees any chunk not yet vacuumed; sm1 has no
  such read. So a pointer is to be fetched in the statement
  that read it: at READ COMMITTED a later statement may find
  the row changed and the datum gone, which is
  `SM_ERR_NOTFOUND`.
- **A fetch needs no rel.** The pointer names its store, and a
  store's datums out of line are always under relid `0xFFFF`,
  so `toast_fetch` given a session reads one by the pointer
  alone, as a host does where it has a value but not its table
  (`sm_toast_fetch`).
- **A key is never made of a pointer.** `encode_key` refuses
  one: the caller fetches the value first.
- **To do: a column of an index's key is never moved out of
  line.** A key is held to `max_pk_len`, 1024 bytes, for the
  primary key, and to `sm1::max_keylen` for an index entry, so
  no key column needs TOAST to fit. Kept in line, a key is
  encoded from the row as it is, and the fetches for key
  columns, in `encode_cols` and in sm3's index writes
  (`sm3/SM3_DESIGN.md` §3.4), go. An insert or update given a
  pointer for a key column stores the value in line. Not
  built.
- **Not built**: compression, and a fetch of part of a datum.
  Above sm2, sm3 gives a table its sequence and fetches for
  its indexes (`sm3/SM3_DESIGN.md` §3.4), once the host turns
  TOAST on, as digres does.

---

## 5. The tuple relation

sm1's store is an ordered map of byte string to byte
string. sm2's is that store with the types
put back: a row goes in as a tuple and comes out as a tuple,
and §3 and §4 happen inside. It is the whole of sm2's API
surface; §2 to §4 are what it is built out of.

A `rel` is opened over exactly one sm1 store, with three
things:

- a **schema** — the ordered columns, their types and
  collations, and per key column its direction and null
  ordering;
- a **relid** of exactly 2 bytes, big-endian, prepended to
  every key sm2 writes and stripped from every key it reads.
  It exists because one sm1 store holds one table and
  everything that belongs to it. sm3 chooses the value and sm2
  never interprets it, so sm2 never learns what a table is:
  which relid means what is sm3's, in `sm3/SM3_DESIGN.md` §4.
  Two of its properties are sm2's own, though. A relid is
  fixed-width, so no rel's relid can be the start of
  another's; and it spends 2 of `sm1::max_keylen`'s 4096
  bytes, which sm2 checks the whole key against (§3.2);
- an **sm1 session**, which is where the transaction lives.

**Decision: a table is keyed by its primary key, and a row of
a table without one by its `_id`** (`../CLUSTER.md`,
`../ROWKEY.md` §2.1). A row's sm1 key is `relid ‖ encode(pk)`,
or `relid ‖ _id` for a table without a primary key, and its
value is its tuple alone:

- **`_id` is an unsigned 64-bit number**, written as 8 bytes
  big-endian, drawn from an sm1 sequence that belongs to the
  table (`sm1/API.md`, the sequence API), by a row of a table
  without a primary key alone. It is never reused, survives a
  crash, and may have gaps. It never changes. A keyless row's
  `_id` is read from its key; a keyed row has none, and the
  calls that give one give 0.
- **`encode(pk)` is §3's encoding** of the primary key's
  columns, never cut: past `max_pk_len`, 1024 bytes, the row
  is refused with `SM_ERR_KEYTOOLONG`, so that every index
  entry, which ends with it, keeps room for its own columns.
  A duplicate is sm1's `SM_ERR_DUPKEY` on the row's key.
- **There is no `_id` index.** One, under relid `id_relid`
  (0xFFFE), went with a keyed row's `_id`, and `rel_find`,
  `rel_find_latest` and `rel_lock_id` with it; the relid stays
  one no index takes.
- **An update keeps its key** unless the primary key changed:
  then the row **moves**, its old key erased with a move's
  tombstone and the row inserted under the new one.
- **`_id` is no TID.** A query reads it as the system column
  `_id`, an `int8`, of a table without a primary key alone, and
  names no `ctid` (`PART4.md` §3, `../ROWKEY.md` §3.1). It
  names the row, not the row version, so it does not change on
  `UPDATE`. digres names a row by its key, not by `_id`
  (`../ROWKEY.md`): every call that names a row takes its key,
  and `rel_key` encodes one from the key's values.

**A rel's rows are chained when its store is**
(`../VCHAIN.md`): a store made chained, by `sm1::mkstore`,
keeps a row's newest version under its plain key and the older
ones in a chain behind it, and `rel_open` asks
`sm1::store_chained` which kind it is over. Over a chained
store a `rel` reads, writes, locks, scans and sizes its rows
with sm1's `chain_` calls, as the table below names them, and
so does the primary key's index `rel`, whose entries are the
rows. A secondary index's entries and the TOAST data stay
sm1's versioned keys, under the plain calls.

Why clustered: on an LSM a heap's key is a key like any other,
so a heap pays an index's price on every read through an index
(`../CLUSTER.md` §1). The costs are in §7.

The operations, and what each one is over; a row is named by
its key, as `rel_insert` gives it:

| sm2 | what it adds | over |
| --- | --- | --- |
| `rel_insert` | writes a tuple under its key, and returns the key, and a keyless row's new `_id`, 0 for a keyed row; the tuple is encoded from datums, or handed in pre-encoded and checked by `tup_check` before an `_id` is drawn; the `_id` from a `seqref_t` looked up at the rel's first draw, and a keyless row's key, made of it, written unchecked, as none was before | `sm1::seqref`, `sm1::nextseq`, `sm1::insert`, `sm1::insert_new`; chained, `sm1::chain_insert`, `sm1::chain_insert_new` |
| `rel_key` | the key of a row from its primary key's values, or a keyless row's from its `_id`, encoded; no read (`../ROWKEY.md` §1) | — |
| `rel_row_key` | the key of a row from its `_id` and all its values, as `rel_key` | — |
| `rel_reserve`, `rel_insert_at` | draws a keyless row's `_id` early, for a caller that writes something of the row first, then inserts as `rel_insert` under it | `sm1::nextseq`, then as `rel_insert` |
| `rel_read` | reads a row by its key, and its `_id`, from the key, 0 for a keyed row | `sm1::read`; chained, `sm1::chain_read` |
| `rel_read_batch` | the rows at n keys, in one call, each into a pin (`../BATCHREAD.md`) | `sm1::read` each; chained, `sm1::chain_read_batch` |
| `rel_trylock` | as `rel_lock` (below), never waiting: 1 with nothing taken if it would have | `sm1::trylock_row`; chained, `sm1::chain_trylock_row` |
| `rel_update`, `rel_delete` | by key; hands back the old image as a **tuple**, not bytes; `rel_update` takes datums or a pre-encoded tuple, checked as `rel_insert`'s is, and moves a row whose primary key changed, its old key erased with a move's tombstone (`../ROWKEY.md` §2.3) | `sm1::update`, `sm1::erase`, `sm1::erase_moved`, `sm1::insert`; chained, `sm1::chain_update`, `sm1::chain_erase`, `sm1::chain_erase_moved`, `sm1::chain_insert` |
| `rel_read_latest` | the newest committed image, decoded | `sm1::read_latest`; chained, `sm1::chain_read_latest` |
| `rel_load`, `idx_load` | puts a row, or an index's entry, into a bulk load of the store, as `rel_insert` or `idx_insert` would write it; nothing is in the store until the load is closed | `sm1::load_put` |
| `rel_scan`, `rel_scan_seek` | tuples in `_id` order, forward only, of a keyless rel; a keyed one's are scanned by key (`idx_scan_rows`) | `sm1::scan`, `sm1::scan_seek`; chained, `sm1::chain_scan`, `sm1::scan_seek` |
| `rel_scan_seek_key` | moves a scan in key order, `idx_scan_rows`' or a keyless rel's, forward to a key | `sm1::scan_seek` |
| `rel_keys_at`, `rel_range_size` | the keys at fractions of the rel's bytes, by bisecting its key range on the store's size estimates, and the bytes of a key range; no row read (`../ROWKEY.md` §2.4) | `sm1::approxsize`; chained, `sm1::chain_approxsize` |
| `relid_size` | the bytes under a relid, a rel's or an index's, in files and in memtables apart; no open rel and no transaction (`sm3/SM3_DESIGN.md` §6.2) | `sm1::approxsize`, and on a chained store `sm1::chain_approxsize` too, the two files' estimates added |

**Decision: two kinds of `rel`, over the same sm1 store, told
apart by their relid.** Index entries are keyed by encoded
columns, and sm3 never encodes a key itself, so sm2 gives it a
`rel` for them too:

- A **`rel`** is keyed by its primary key or `_id`, under the
  rows' relid, and its value is the row's tuple. Its
  `rel_insert` draws a keyless row's `_id` from the table's
  sequence, named when the `rel` is opened, and returns it, so
  the sequence is drawn in one place.
- An **index `rel`** is keyed by the encoded columns it is
  opened with — `encode(sk) ‖ row key`, or `encode(sk)` alone
  for a unique index (`sm3/SM3_DESIGN.md` §3) — under the
  index's relid, and its value is a tuple holding the cut
  flag, the INCLUDE columns and the row's key. It is how sm3
  writes, deletes and
  scans index entries. A table's primary key is an index
  `rel` too (`idx_open_primary`), with no entries of its own:
  its entries are the rows, and only read, and
  `idx_scan_rows` hands them out in key order as rows.

`rel_lock` takes a row's lock without writing it, as
`sm1::lock_row` does, or `sm1::chain_lock_row` over a chained
store: `SELECT … FOR SHARE` and `FOR UPDATE`, and what sm3's
index build holds on a row while it adds the row's entry
(`sm3/SM3_DESIGN.md` §3).

An index `rel` may instead be opened over a **tree**
(`../BTREE.md`, `src/tree/`): its entries a B-link tree of
sm1's pages under its relid, written outside every
transaction, never versioned, ending with the row's key in a
unique index too, and holding no INCLUDE column. An entry
inserted again is no change, `idx_delete` removes nothing, and
a scan hands out every entry in range, dead or not, for the
caller to check against its row (`idx_entry_key`); vacuum's
walk (`idx_vacuum`) removes the dead ones.

sm2 prepends and strips the relid for both kinds, and the
table above is the `rel`'s; an index `rel`'s operations are
the same shape, keyed by its columns (`idx_open`,
`idx_insert`, `idx_delete`, `idx_scan`):

- **An entry's key** is `relid ‖ encode(sk)`, then the row's
  key — always in a non-unique index, and in a unique one
  only for an entry with a null key column, so that nulls stay
  distinct. A unique key without one is sm1's uniqueness unit,
  and a duplicate is sm1's `SM_ERR_DUPKEY`. A unique index's
  key is never cut (§3.2).
- **An entry's value** is a tuple: a flag saying the key was
  cut, the INCLUDE columns, then the row's key, which a unique
  entry's key does not carry, in either kind of table, with no
  `_id` (`../ROWKEY.md` §2.1). A keyless row's `_id`, which a
  scan hands out, is read from that key.
- **An entry gives its key columns back** (`idx_scan_key`),
  for index-only scans: the key decodes past the relid, its
  columns' datums as a tuple gives them, and the row's key is
  what remains, if anything. A cut key, or a text column under
  a non-deterministic collation, keeps no datum (§2.3); then
  the caller reads them from the row.
- **`idx_find` gives the row a unique index has a key for**,
  as the snapshot shows it, locking nothing; `idx_find_latest`
  its newest committed version, under a shared lock on the
  key, at READ COMMITTED.
- **`idx_delete` removes an entry committed since the
  transaction began.** An entry is an sm1 key, so at READ
  COMMITTED sm1 refuses to erase one committed after the
  transaction began, with `SM_ERR_UPDATED`, until its newest
  version has been read (`sm1/SM1_DESIGN.md` §5.4). An entry
  has no filter to re-check: whether to write was decided on
  the row, whose lock the caller holds. So `idx_delete`
  reads the newest version itself and repeats the erase, and
  the caller never sees the code from an index.
- **A scan's bounds** are the values of the index's leading
  columns, inclusive or not, in the index's own order, after
  each column's direction and null ordering; mapping a
  predicate onto that order is sm3's, as predicates are (§6).
  A bound becomes an sm1 key through the encoding's
  prefix-freeness: the encoding P of the first j columns
  begins exactly the keys whose first j columns hold those
  values, contiguously, so a bound is P or its successor, the
  least string above every key that starts with P. A bound
  too long to encode is cut where keys are cut, which can only
  widen the scan, and the scan says so.
- **A key's columns past their budget are cut at one length**,
  `key_budget` of the longest row key an entry may end with:
  8 bytes in a table keyed by `_id`, `max_pk_len` in a keyed
  one. So a cut bound orders against cut keys as the whole one
  would against theirs.
- **Index relids are below 0xFFFE, once the `_id` index's, and
  TOAST's**
  (`sm3/SM3_DESIGN.md` §4), which is also what guarantees every
  bound a successor.

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
  type, a row with more attributes than its schema, a corrupt
  tuple: sm2
  records each with `sm1::seterr`, its codes appended to
  `sm_errnum_t` (`../include/sm_error.h`) under a comment
  naming sm2, so one
  `sm1::errmsg()` serves the whole stack (`DESIGN.md` §5 at the
  top level).
- **Running out of memory is fatal to the process** once
  `sm1::attach` has installed its new-handler; before that, a
  `std::bad_alloc` passes through sm2 unchanged, as any
  exception does, and sm2 catches nothing (`DESIGN.md` §5 at
  the top level). A type's `cmp` and `hash` have no error to
  return, so they must not allocate.
- **The interface is `include/sm2.h`, in `namespace sm2`**, as
  every level's is (`OBJECTIVE.md` at the top level):
  `sm2::rel_insert`, `sm2::tup_encode`. This document writes
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
  at. `rel` gets this from `tup_decode`'s by-ordinal
  signature (§4); it does not implement it a second time.
- **`rel` calls the tuple codec, it does not contain it.**
  §4 is a component with its own header and its own tests,
  and `rel` is one of its two callers — sm3's INCLUDE columns
  are the other.
- **Forward only.** A `rel` scan inherits sm1's ban, and the
  descending case is already handled in the encoding (§3).
- **A decoded datum points into memory sm2 does not copy**:
  a `rel` hands out the tuple as sm1's view of it, and a datum
  decoded from it points into that. It is not NUL-terminated,
  must not be freed, and stays valid, as sm1's reads do, until
  the next call on the `rel`'s session — by that `rel`, another
  over the same session, or anything else that calls sm1 on it.
  A codec that allocated per datum would dominate the profile
  of every scan.
- **`NOT NULL` is not enforced here.** The tuple's null bitmap
  could, but constraints are sm3's, and a codec that rejects
  data is a codec with a policy in it; sm2 stores what it is
  given.
- **A `rel` borrows its schema.** sm3's catalog owns it and
  keeps it alive for the `rel`'s lifetime, and takes the
  table-oid lock before changing it (`sm3/SM3_DESIGN.md` §2),
  which it must do anyway. With no schema version (§4) a `rel`
  has one schema, the current one, so there is nothing to copy
  for.

Rows read by key, many in one call, are built
(`rel_read_batch`, `../BATCHREAD.md`); a scan stays tuple at a
time (`sm3/SM3_DESIGN.md` §5).

---

## 6. What sm2 leaves to sm3

Named here so the boundary is not re-argued in both documents.
sm3 is a client of sm2 the way sm2 is a client of sm1.

- **The catalog.** sm2 is *handed* a schema; it neither
  persists one nor looks one up, and it has no notion of a
  table name, a DDL statement or a table-oid lock. It does keep
  sm3's catalog bytes for it — `catkv_*`, a key-value store in
  sm1's store 0 under sm1's upper prefix (`sm3/CATALOG_ONDISK.md`
  §1) — but never reads them.
- **Secondary indexes.** sm2 maintains no index. It gives sm3
  the two things index maintenance needs: the encoder (§3) and
  the old image as a tuple (§5).
- **Object mapping.** Which sm1 store holds a table, and
  which relid each of its indexes gets, is sm3's; sm2 only
  prepends and strips the relid (§5).
- **The primary key's definition.** sm2 keys a `rel` by the
  columns it is opened with, and finds a duplicate; which
  columns, and adding or dropping a primary key, which rewrites
  the table, are sm3's (§5, `../CLUSTER.md` §8).
- **Predicates.** sm2 has no filter, and therefore cannot
  resolve `SM_ERR_UPDATED` or derive a key range from a
  `WHERE` clause.
- **Statistics.** sm2 counts nothing.

---

## 7. Known trade-offs

The costs this design accepts, in the manner of
`sm1/SM1_DESIGN.md` §13, written down where the next reader
will find them:

- **Collated keys are stored twice.** The sort key is lossy,
  so the original datum is stored beside it (§3).
- **A tuple is not a PostgreSQL tuple — but its columns are
  PostgreSQL datums.** §2.1 makes the conversion at the
  executor boundary per tuple, not per datum: the framing is
  rebuilt, the column bytes are copied. The threaded fork
  removed the IPC boundary but not this (`PART4.md` §2).
- **decimal is the exception, and pays for it.** It follows
  DuckDB's decimal, not PostgreSQL's numeric (§2.2), so it is the
  one type converted datum by datum, it caps at 36 digits
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
- **A keyed row has no `_id` to be read by**: it is read by
  its key, one read, and a range over the primary key is one
  sorted run (`../ROWKEY.md` §2.1).
- **Every insert writes the row**, one sm1 key, plus one per
  index; a keyed table's wrote an `_id` entry too, before its
  rows lost their `_id`.
- **A primary key change moves the row**, and rewrites every
  index entry of it, whose suffix and value name the row by
  its key (`../CLUSTER.md` §7).
- **An index entry ends with the row's key**, wider than 8
  bytes for most primary keys, and a key's columns are cut at
  `key_budget(max_pk_len)`, 3070 bytes, in a keyed table.
- **`ctid` is gone.** A query that reads `ctid` fails, and
  reads `_id` instead, in a table without a primary key
  (`PART4.md` §3); inside, the fork names a row by its key,
  with no TID at all (`../ROWKEY.md`). And since `_id` names a
  row, not a version, code that expects it to change on
  `UPDATE` sees it stay put.
- **Descending order costs an encoding, not a structure.**
  Inverted bytes scanned forward means no mirror relation and
  no second copy, but it also means a column's direction is
  fixed when the key is written: changing `ASC` to `DESC`
  rewrites the keys.
- **The type set is closed at the format.** New types are
  cheap, but only while the type code and the tuple layout
  leave room for them; that room is spent once.
- **A bare `decimal` can round.** Its host gives it a default,
  digres `decimal(17, 3)` (§2.2), so a datum a user believed
  exact loses digits past the third fractional place, with no
  error, and one of 10^14 or more is refused.
  Refusing a bare `decimal` would have been louder; the default
  was chosen for compatibility.
- **Every key column pays a null marker byte**, `NOT NULL`
  ones included (§3.1). It buys a key layout that no
  nullability change can rewrite.
- **Tables are bounded by sm1's store count.** One sm1
  store per table (§5) makes `DROP TABLE` O(1), and caps
  tables, partitions included, at the hundreds to low
  thousands of stores sm1 handles. Accepted for v1.

---

## 8. Structure, build order and testing

The order of work, and why it is this order; it was `PLAN.md`
until this section took it in.

The principle: **the two on-disk formats come first and get
the most scrutiny.** A key encoding and a tuple layout are
permanent the moment any data exists, and neither can be fixed
later by a patch. Everything else in sm2 — and everything in
sm3 — is code that can be rewritten.

### 8.1 Source layout

sm2 mirrors sm1: nothing about its build deserves novelty.

```
sm2/
  Makefile      the same targets as sm1's; sm2 joins LEVELS
  include/      the public header (§5)
  src/type/     steps 1 and 2: the ADT table, each type's
                representation, and its key encoding
  src/ryu/      PostgreSQL 18's float printers, copied verbatim
  src/tuple/    step 3: the tuple codec
  src/rel/      step 4: the tuple relation
  test/         doctest, as in sm1
```

The key encoding lives in `src/type`, not in a directory of
its own: `key_encode` is one of a type's ADT functions
(§2.3), so a type's representation and its
key sit side by side, and a multi-column key is composed from
them. Dependencies run one way — `rel` calls `tuple`, `tuple`
calls `type` — and only `src/rel` calls sm1's storage
functions. Every directory may include `sm1/include/sm1.h`
all the same, since sm2 reports its failures through sm1's
error state (§5); `src/type` does already.

### 8.2 Build order

| Step | State |
| --- | --- |
| 1. Types | built |
| 2. Key encoding | built |
| 3. Tuple format and codec | built |
| 4. Tuple relation | built: the heap and index `rel`s, and TOAST (§4.2) |

#### Step 1. Types

The vocabulary the two formats are written in, so it comes
before them.

- The first set: boolean, the integer widths, `float4`,
  `float8`, `decimal64`, `decimal128`, `text` / `varchar` /
  `bytea`, `date`, `time`, `timestamp`, `timestamptz`,
  `interval`, `uuid` —
  each one spelled out in `TYPE.md` §3.
- **The ADT table first** (§2.3): one descriptor
  of function pointers per type, so that every step after
  this one adds a descriptor instead of a `switch`.
- PostgreSQL's representation per type (§2.1),
  little-endian, with variable-length datums carrying the
  payload alone.
- `decimal` after DuckDB's decimal (§2.2),
  and interval as PG's three components (§2.4).
- A stable on-disk type code per type, assigned once.
- Null and collation as properties of a column, not of a type.
- The `decimal` rules in §2.2: only
  `decimal(p, s)`, a bare `decimal` resolved by the host
  (digres's `decimal(17, 3)`), and NaN and infinity refused.

**Verify:** for every type, a datum round trips through the
in-memory representation, and the type's own comparison
function agrees with hand-written expectations at the
boundaries (min, max, zero, negative zero, NaN, empty
string). For every type but `decimal`, a datum's bytes equal
what PostgreSQL stores for the same datum — a table of
fixtures taken from PG, which is the only way "the
representation is PostgreSQL's" is a claim rather than an
intention.

#### Step 2. Key encoding

The foundation. sm1 orders keys bytewise only, so every
ordered thing above rests on this, including sm3's indexes.

- Order-preserving encodings per type: integers (sign-flipped
  big-endian), floats (IEEE-754 total order), `decimal`,
  timestamps, text (collation sort key).
- Multi-column keys, ascending and descending per column,
  nulls first or last.
- The key rules in §3.1–§3.3: a null marker on
  every column, refuse an over-long primary key,
  truncate and flag an over-long secondary key, refuse a
  mismatched collation version, and key a non-deterministic
  collation by its sort key alone.

**Verify:** property-based tests, not examples. For every type
and every column order, `encode(a) < encode(b)` iff `a < b`,
over generated datums including the boundaries (min, max, zero,
negative zero, NaN, empty string, the longest legal key). Round
trip where the encoding is reversible, and prove the documented
loss where it is not. Interval gets two properties of its own
(§3): the key round trips, and its order refines
PostgreSQL's — never contradicting it where PG is decisive,
and separating the datums PG calls equal.

#### Step 3. Tuple format, and the tuple codec

Its own API, with its own header, built before the relation
that calls it (§4).

- `tup_encode` and `tup_decode`, the latter by ordinal so
  one column costs one column.
- Null bitmap, fixed/variable split, no padding
  (§4.1).
- An attribute count per row, as PostgreSQL's `natts`, so
  `ADD COLUMN` is O(1) and a dropped column does not shift the
  ones after it; no schema version (§4).
- TOAST for large datums, below sm1's 64MB ceiling: built
  after the rest of the step, in the heap `rel` (§4.2). The
  codec stores one of sm2's pointers as it is, and still
  refuses a compressed varlena.

**Verify:** a row written with N attributes reads correctly
after columns are added — each missing one as null or its
default — and after a column is dropped; the golden-bytes test
that a tuple written today still decodes tomorrow. All of it
in a unit test with no database in it — the codec touches sm1
nowhere, which is half the reason it is a separate API.

#### Step 4. The tuple relation

The three above, joined to sm1.

- `rel_insert`, `rel_read`, `rel_update`, `rel_delete`,
  `rel_read_latest`, `rel_scan`, `rel_scan_seek`, over the
  codec of step 3 rather than a second copy of it.
- The 2-byte relid, chosen by sm3, opaque to sm2
  (§5).
- Rows keyed by the primary key, or without one by `_id`, an
  unsigned 64-bit number from the table's sm1 sequence (§5,
  `../CLUSTER.md`, `../ROWKEY.md`); first built keyed by
  `rowno` in every table, with the primary key an index. Rows
  chained when the store is (`../VCHAIN.md`).
- Errors from sm1 passed through, not translated —
  `SM_ERR_UPDATED` in particular.
- Per-column decoding, so a reader that wants one column does
  not pay for the row.

**Verify:** for generated schemas and rows, what goes in comes
back out, under every isolation level; the old image returned
by an update or a delete is the tuple that was there; a
savepoint rollback leaves no sm2 state behind, because there
is none to leave; an update keeps its key unless its primary
key changed, when the row moves; two `rel`s over the same sm1
store with different relids never see each other's rows.

### 8.3 Testing

sm1's two-layer arrangement is worth copying, because it
earned its keep: C++ tests for contract and single-session
mechanics, and a declarative scenario suite for anything where
two sessions interleave. Most of sm2 is the first kind — a
codec is tested by property, not by interleaving — and the
concurrency questions that need the second kind mostly arrive
with sm3 (`sm3/SM3_DESIGN.md` §8).

Neither: two-session scenarios above sm1 are `rel` scripts
(`../rel/REL.md`), which drive sm2 and sm3 through `sm.h`
(`sm3/SM3_DESIGN.md` §8.2).

**Property tests are hand-rolled**, with no new dependency.
Each type has a generator weighted toward small values and its
boundaries. A property runs every pair of boundary values
first, then N random pairs, from a fixed default seed that the
environment variable `WISR_SEED` overrides; a failing property
prints its seed through doctest, so any failure replays exactly.
There is no shrinking: size-biased generation keeps failures
small, and a simple shrink for byte strings is added only if a
failure proves hard to read.

**The PostgreSQL fixtures are observed on disk, once**, on
PostgreSQL 18. For every type but `decimal`,
`test/pg_fixtures.h` holds each fixture value with the bytes
PostgreSQL stores for it, read through the
`pageinspect` extension: a one-column table per type, the values
inserted, and each tuple's data bytes taken from
`heap_page_items(get_raw_page(...))`, with the varlena header
stripped, since sm2 keeps the payload alone. The values are the
generators' boundaries plus representative ones — multibyte
UTF-8, the empty string, timestamps at 1970 and at PostgreSQL's
2000 epoch. The script that produces the header is checked in
beside it, and the header records the PostgreSQL version and
the date it was generated. It runs on a developer's machine
with a PostgreSQL server and superuser rights; the build never
touches PostgreSQL. The bytes hold on little-endian hosts only,
which the fixture test asserts.

The date and time types add 500 seeded random values each,
made by PostgreSQL itself, half within a few thousand years of
2000 and half anywhere in range. `interval` adds 500 seeded
random ones, spelled in PostgreSQL's verbose input form so
that the ISO 8601 text is PostgreSQL's own printing, `uuid`
500 made from seeded `random()` through `md5`, and `text`,
`varchar` and `bytea` 500 each, the text ones from random code
points in every UTF-8 length. The floats add 2,500 seeded
random values each, since PostgreSQL's float text is not
always the shortest decimal:
2,000 random finite bit patterns and 500 integers just past
the width's exact range, where most of the exceptions lie.

The fixtures are generated on the development machine, from
PostgreSQL 18.6 built from source: tag `REL_18_6`, cloned into
`~/p/postgres18` and installed under `~/pg18` with
`pageinspect`, its data directory at `~/pg18/data` with
superuser `postgres` and trust authentication. The server runs
only for a fixture run:

```
~/pg18/bin/pg_ctl -D ~/pg18/data -l ~/pg18/server.log \
    -o "-p 5418 -k $HOME/pg18" -w start
~/pg18/bin/psql -h ~/pg18 -p 5418 -U postgres
~/pg18/bin/pg_ctl -D ~/pg18/data -w stop
```

### 8.4 What is deliberately not here

- No catalog semantics, no choice of which indexes exist, no
  predicate scan, no statistics, and no choice of which relid
  means what. All of it is `sm3/SM3_DESIGN.md` §8; sm2 gives
  only the index `rel` and the catalog's bytes (`catkv`, §5).
- No SQL, no planner, no executor.
- No PostgreSQL dependency. `PART4.md` explains what the PG
  target forces on the decisions above; none of it requires a
  PostgreSQL source tree to build or test sm2.
