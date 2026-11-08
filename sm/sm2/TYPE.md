# TYPE — sm2, the types one by one

The reference for every type sm2 supports: how its datum is
represented, how it is ordered, and what its key bytes are.
[`TYPE.md`](TYPE.md#5-why-the-types-are-so) §5 argues *why* these
choices; this file is the
table you look things up in while writing code.

**Status: built**, every type here, with its text, its order
and its key ([`TYPE.md`](TYPE.md#9-tests) §9). Where a fact about
PostgreSQL is from memory rather than checked against the
source it says so, and [`TYPE.md`](TYPE.md#9-tests) §9 step 1
pins each
one to a fixture taken from a running PostgreSQL.

---

## 1. The two representations

Every datum has two forms, and the rule for which is used is
mechanical:

| Form | Where it is used | What it is |
| --- | --- | --- |
| **native** | in a tuple, always | PostgreSQL's own representation of the datum ([`TYPE.md`](TYPE.md#51-the-representation-is-postgresqls) §5.1) |
| **key** | only when the datum is a key, or part of one | `key_encode(datum)` — a byte string whose `memcmp` order is the type's order ([`../LSMINDEX.md`](../LSMINDEX.md#2-key-encoding) §2) |

A tuple stores native datums and nothing else. A key column
is *also* encoded into the sm1 key, so it appears twice: once
encoded in the key, once native in the tuple. The tuple is
what a read returns; the key exists to order and to identify.

Three rules follow, and they apply to every type below.

- **`key_encode` is injective.** Distinct datums get distinct
  keys. A unique index, the primary key's included, is
  enforced on key bytes, so two datums that share a key are
  one entry, and a unique constraint that cannot tell them
  apart is not a constraint.
- **`key_encode` need not be reversible.** The tuple holds
  the datum, so nothing depends on getting it back out of the
  key. `key_decode` is provided for sm3's index-only scans, and
  is an optimization rather than a requirement. Every type but
  `bpchar` has one (§3.5); a float's gives back the datum its
  key keeps, `0` for `-0` and the quiet NaN for any NaN (§3.3),
  which PostgreSQL calls equal to the original.
- **Where PostgreSQL calls two datums equal, they may share a
  key.** That is agreement with PostgreSQL, not a collision.
  `-0.0` and `0.0` are the case that comes up.

## 2. Rules that apply to every key

Applied in this order, around whatever the per-type encoding
in §3 produces.

**Null.** Every column's key is preceded by one marker byte:
`0x00` for null, `0x01` for non-null when nulls sort first;
`0x01` and `0x02` when they sort last. A `NOT NULL` column
carries the marker too, so a nullability change never
rewrites a key ([`../LSMINDEX.md`](../LSMINDEX.md#22-framing)
§2.2). No type needs a reserved
value to mean absent. First and last mean in the finished key:
since a descending column's marker is inverted with the rest
(below), a descending column with nulls first takes the
nulls-last markers, which inversion turns into nulls first.

**Direction.** A `DESC` column is the ascending encoding with
every byte inverted, marker included, because sm1 will not
scan backwards
([`SM2_DESIGN.md`](SM2_DESIGN.md#2-what-the-level-depends-on)
§2). Inversion is applied last,
to the finished per-column bytes.

**Concatenation.** A multi-column key is the per-column keys
joined end to end with nothing between them. For this to be
unambiguous no encoded datum may be a prefix of another, so
every variable-length encoding is made prefix-free by §3's
escaping rule. Fixed-width encodings are prefix-free already.

**Length.** `sm1::max_keylen` is 4096 and bounds the whole
concatenation, the 2-byte relid included. What
happens when a key does not fit is in
[`../LSMINDEX.md`](../LSMINDEX.md#24-cut-keys) §2.4:
refused for a primary key, truncated and rechecked for a
secondary index.

---

## 3. The types

Widths are bytes. "Key" is the encoded length, before the
null marker and before any inversion.

| Type | Code | Native | Width | Key | Key width |
| --- | --- | --- | --- | --- | --- |
| `bool` | 1 | `0` or `1` | 1 | the byte as is | 1 |
| `int2` | 2 | two's complement, LE | 2 | sign-flipped, BE | 2 |
| `int4` | 3 | two's complement, LE | 4 | sign-flipped, BE | 4 |
| `int8` | 4 | two's complement, LE | 8 | sign-flipped, BE | 8 |
| `float4` | 5 | IEEE-754 binary32, LE | 4 | total order, BE | 4 |
| `float8` | 6 | IEEE-754 binary64, LE | 8 | total order, BE | 8 |
| `text` | 8 | payload bytes | var | collation sort key ‖ escaped original | var |
| `varchar` | 9 | payload bytes | var | as `text` | var |
| `bytea` | 10 | payload bytes | var | escaped bytes | var |
| `date` | 11 | days since 2000-01-01, `int32` | 4 | sign-flipped, BE | 4 |
| `time` | 12 | µs since midnight, `int64` | 8 | sign-flipped, BE | 8 |
| `timestamp` | 13 | µs since 2000-01-01 00:00:00, `int64` | 8 | sign-flipped, BE | 8 |
| `timestamptz` | 14 | as `timestamp`, in UTC | 8 | sign-flipped, BE | 8 |
| `interval` | 15 | `{time int64, day int32, month int32}` | 16 | `span` ‖ months ‖ days | 24 |
| `uuid` | 16 | the 16 bytes | 16 | the same 16 bytes | 16 |
| `decimal64` | 17 | after DuckDB's decimal, two's complement, LE | 8 | sign-flipped, BE | 8 |
| `decimal128` | 18 | as `decimal64` | 16 | sign-flipped, BE | 16 |
| `bpchar` | 19 | payload bytes, padded as given | var | as `text`, of the value without its trailing spaces | var |
| `name` | 20 | the bytes, NUL-padded to 64 | 64 | as `text`, of the bytes before the NULs | var |

The codes are frozen: assigned once, never reused or
renumbered ([`TYPE.md`](TYPE.md#5-why-the-types-are-so) §5). Code
7 was `decimal`, before
it was split into `decimal64` and `decimal128`, and is retired.
Codes 64–127 are reserved for types added later; 0 is never a
type code.

### 3.1 bool

Native is one byte, `0` or `1`. The key is that byte
unchanged: `false` sorts before `true`, which is
PostgreSQL's order.

### 3.2 int2, int4, int8

Native is two's complement, little-endian
([`TYPE.md`](TYPE.md#51-the-representation-is-postgresqls) §5.1).

The key flips the sign bit and writes big-endian:

```
key(v) = BE( v + 2^(n-1) )        n = 16, 32, 64
```

Two's complement puts negatives above positives under
`memcmp`, because their top bit is set; adding the offset
maps `[-2^(n-1), 2^(n-1))` onto `[0, 2^n)` monotonically, and
big-endian makes `memcmp` compare most significant byte
first. Checked by property over the full range including
both bounds.

### 3.3 float4, float8

Native is IEEE-754, little-endian.

The text is PostgreSQL's `float4out` and `float8out`, whose
Ryu code sm2 copies verbatim into `src/ryu/`. It is nearly
always the shortest decimal that reads back to the same value,
but not always: float4 `122908496` prints `1.22908496e+08`
where the shortest is `1.229085e+08`. No other printer
matches it, `std::to_chars` included.

The key is the IEEE **total order**, with two normalisations
first, and only then the bit transform:

```
if isnan(v):   bits = 0x7FF8000000000000     # one canonical NaN
elif v == 0.0: bits = bits_of(+0.0)          # -0.0 becomes +0.0
else:          bits = bits_of(v)

if bits >> 63: bits = ~bits                  # negative: flip all
else:          bits = bits ^ (1 << 63)       # positive: flip sign

key = BE(bits)
```

Both normalisations exist to match PostgreSQL, which orders
floats with `float8_cmp_internal` (from memory, not checked
against the source):

- **NaN is greater than everything**, `+Infinity` included,
  and all NaNs are equal. Canonicalising every NaN to one
  quiet pattern gives exactly that, because `0x7FF8…` is
  above `+Infinity`'s `0x7FF0…` and the transform preserves
  it. Canonicalising *before* the transform matters: a
  negative NaN would otherwise land at the bottom.
- **`-0.0` equals `0.0`.** PostgreSQL compares with `<` and
  `>`, so it calls them equal and a unique index accepts only
  one of the two. Keeping their keys distinct would let both
  in, which is a divergence in constraint behaviour, so the
  key folds them together. The tuple still holds whichever
  the caller inserted, sign and all — this is the clearest
  case for why keys are injective over *PostgreSQL's* notion
  of distinct, not over bit patterns.

The constants above are `float8`'s. `float4` is the same
rule at 32 bits: canonical NaN `0x7FC00000`, sign bit
`1 << 31`, and a 4-byte big-endian result.

Verified: the resulting order agrees with PostgreSQL's
comparison on every pair drawn from `±Infinity`, `±0.0`, NaN,
the subnormal and huge boundaries, and random datums.

### 3.4 decimal64, decimal128

SQL's `decimal(p, s)`, and `numeric` its alias, are not sm
types: the host maps them to one of these two, choosing the
width ([`../CATALOG.md`](../CATALOG.md#84-decimal-columns) §8.4).

Native follows DuckDB's decimal
([`TYPE.md`](TYPE.md#52-the-decimals-follow-duckdbs) §5.2), with
the scale in it: the value times 10^s as a two's complement
integer, shifted left past s itself, little-endian —
`(value << 5) | s` in 8 bytes for `decimal64`,
`(value << 6) | s` in 16 for `decimal128`. The typmod is
PostgreSQL's numeric typmod, `((p << 16) | s) + 4`, with
`0 <= s <= p` and `1 <= p <= 17` for `decimal64`, `36` for
`decimal128`, the digits the bits above the scale hold; `-1`
is refused. A datum's scale is its column's: `out` and the key
encoder refuse one whose scale is not the typmod's, as its key
would sort wrongly among the column's. The caller rescales a
value of another scale first. Only the caller can round a scan
bound, down for a lower bound and up for an upper one, as dg's
`numeric_bound()` does.

The key is §3.2's, at the datum's width — sign bit flipped,
big-endian. Nothing bespoke, which is the point of the whole
exception.

Two datums compare correctly only at a common scale, where
their scale bits are equal, and the schema fixes the scale per
column, so the encoder never rescales. Comparing two columns
of different scale, or a `decimal64` with a `decimal128`, is a
cast, above sm2.

Decided in [`TYPE.md`](TYPE.md#52-the-decimals-follow-duckdbs)
§5.2: a typmod is required, and NaN
and infinity are refused on insert, so no value is reserved as
a sentinel.

### 3.5 text, varchar, bpchar, name

Native is the payload bytes alone — no varlena header, no
1-byte short form, no PostgreSQL TOAST pointer
([`TYPE.md`](TYPE.md#51-the-representation-is-postgresqls) §5.1).
Length lives in the tuple's framing.

The bytes must be valid UTF-8 with no NUL, as in a PostgreSQL
database whose encoding is UTF8: no overlong form, no
surrogate, nothing past U+10FFFF. `in` and `out` both check,
and the text is the datum itself. `cmp` orders the bytes,
which is codepoint order.

The key depends on the column's collation, which is a
property of the column and not of the type:

```
key = sortkey(collation, datum) ‖ 0x00 0x00 ‖ escape(datum)
```

- `sortkey` is the collation's transform, run **once here in
  the encoder** — ICU's `ucol_getSortKey` —
  never `strcoll` at compare time, because sm1 compares with
  `memcmp` and takes no comparator
  ([`../LSMINDEX.md`](../LSMINDEX.md#2-key-encoding) §2). Its
  output carries no embedded `0x00`, which is what lets the
  `0x00 0x00` separator above sit between it and the
  tie-break. Budget 2-4x the input length against
  `sm1::max_keylen`.
- The original follows as a **tie-break**, escaped by §3.6's
  rule. It is there for injectivity, not recoverability: a
  deterministic collation can give two distinct strings the
  same sort key, and PostgreSQL then orders them bytewise and
  keeps them as two datums. Without the tie-break they would
  share a key and become one row.
- For a byte-ordered collation — `C`, `POSIX` — the sort key
  *is* the original, so the key is just `escape(datum)` and
  the tie-break is dropped. Worth special-casing: it is the
  common case in practice and it halves the key.
- ICU's sort key ends in a zero byte of its own and holds no
  other; the key leaves that byte out, and the `0x00 0x00` that
  follows is where decoding finds the original.
- The type's `cmp` stays byte order: it takes no collation, so
  the collation's order is in the key alone.
- The collation's version is recorded with the schema
  ([`../CATALOG.md`](../CATALOG.md#1-design) §1); a mismatch is
  refused, not trusted.
- Under a **non-deterministic** collation the tie-break is
  left out: the key is `sortkey ‖ 0x00 0x00`, so strings ICU
  calls equal share a key, and `key_decode` fails with
  `SM_ERR_INVALID`, since the key holds no string
  ([`../LSMINDEX.md`](../LSMINDEX.md#23-text-and-collations)
  §2.3). Byte order is always
  deterministic.
- `text` (8) and `varchar` (9) share every byte and keep
  separate codes only so a round trip never rewrites a
  column's declared type. The `varchar(n)` limit is a
  coercion the host applies; sm2's codec stores what it is
  given ([`TYPE.md`](TYPE.md#5-why-the-types-are-so) §5).
- `bpchar` (19) is PostgreSQL's `char(n)`, whose trailing
  spaces count for nothing: `'a'` and `'a  '` are one value.
  It stores what it is given, the host's padding and all, and
  prints it back; its `cmp` and its key leave the trailing
  spaces out, so the two are one key, and a unique index takes
  one of them. A key so holds no value to give back: the type
  has no `key_decode`, and an index scan reads the row instead,
  as under a non-deterministic collation.
- `name` (20) is PostgreSQL's `name`, its `NameData`: 64
  bytes, valid UTF-8 and then NULs, so at most 63 bytes of
  string. Native is those 64 bytes, as PostgreSQL stores them:
  a fixed-width datum, not a varlena. `in` refuses a string of
  64 bytes or more, which PostgreSQL's `namein` clips; that is
  the host's. Its `cmp` and its key are a `text`'s, of the
  bytes before the NULs, so names order bytewise, as under
  PostgreSQL's collation `C`, which a `name` column has; its
  key decodes, padded again.

### 3.6 bytea

Native is the payload bytes. `memcmp` over raw bytes is
already PostgreSQL's `bytea` order, so the key is the bytes
with one transform: **escaping, to make them prefix-free** so
that a multi-column key concatenates unambiguously.

```
escape(s) = s with each 0x00 replaced by 0x00 0xFF,
            then 0x00 0x00 appended
```

The terminator `0x00 0x00` cannot occur inside the escaped
body, since every interior `0x00` is followed by `0xFF`. So
no encoded datum is a prefix of another, and `('a', 'b')`
and `('a\0', 'b')` get different keys that sort in the right
relative order. Verified order-preserving, reversible and
prefix-free over generated byte strings, and verified that a
two-column concatenation sorts lexicographically by column.

The text is PostgreSQL's with `bytea_output` hex, its default:
`\x` and two lower-case hex digits a byte. PostgreSQL also
reads upper case, spaces between pairs and the escape format,
but never prints them, so sm2 refuses them.

### 3.7 date

Native is days since 2000-01-01 as `int32`, PostgreSQL's
epoch. The key is §3.2's `int4` key.

PostgreSQL reserves the extreme values for `-infinity` and
`infinity`; being the extremes, they sort at the ends of the
range with no special handling.

The text is PostgreSQL's under DateStyle ISO: `2000-01-01`,
the year padded to at least four digits and a year before 1 AD
followed by ` BC`. The range is PostgreSQL's, 4714-11-24 BC
(Julian day 0) through 5874897-12-31; PostgreSQL's
documentation calls the start "4713 BC", but its code starts a
year earlier.

### 3.8 time

Native is microseconds since midnight as `int64`, so the
range is `0` to `86400000000` inclusive — PostgreSQL admits
`24:00:00`. The key is §3.2's `int8` key. The sign flip is a
formality for a datum that is never negative, and is kept so
that every fixed-width integer key is built the same way.

The text is `HH:MM:SS`, with a fraction of up to six digits and
no trailing zeros. A precision typmod `p`, 0 to 6, bounds the
fraction's digits, as in `time(3)`: PostgreSQL rounds its input
to `p` digits and so never prints more, and sm2 refuses text
with more rather than rounding it, since only canonical text is
input. A typmod of -1 allows six; any other is invalid. The same
holds for the timestamps.

### 3.9 timestamp, timestamptz

Native is microseconds since 2000-01-01 00:00:00 as `int64`.
`timestamptz` is the same number held in UTC; the two differ
in how they are rendered and in nothing else on disk, which
is also PostgreSQL's arrangement. The key is §3.2's `int8`
key, and PostgreSQL's reserved infinities sort at the ends as
in §3.7.

The text is a date and a time as in §3.7 and §3.8, joined by a
space, with ` BC` last. `timestamptz` prints in UTC, as
PostgreSQL does under TimeZone UTC — `2000-01-01 00:00:00+00` —
since sm2 has no session time zone; showing a value in another
zone is the caller's business. The range runs from 4714-11-24
00:00:00 BC to 294276-12-31 23:59:59.999999.

### 3.10 interval

Native is PostgreSQL's struct, 16 bytes:

```
{ time int64 (µs), day int32, month int32 }
```

The three components are kept apart because they are not
convertible: a month is 28 to 31 days, a day is 23, 24 or 25
hours across a daylight-saving boundary. So `'1 mon'` and
`'30 days'` are different datums
([`TYPE.md`](TYPE.md#54-interval-keeps-postgresqls-three-components)
§5.4).

All three components at their maximum is `infinity`, all at
their minimum `-infinity`, as in PostgreSQL 17 and later; every
other 16 bytes is a finite interval. The text is PostgreSQL's
under IntervalStyle iso_8601 — `P1Y2M3DT4H5M6.5S`, `PT0S` for
zero — each field signed on its own, and the seconds carrying
one sign for their fraction too (`PT-0.5S`). Months print as
years and months of one sign, so `P-1Y2M` is stored as -10
months and printed `P-10M`. The typmod is PostgreSQL's: a field
range and a precision. PostgreSQL zeroes the fields below the
range's last one and rounds the seconds to the precision, and
sm2 accepts only text they would leave unchanged, as for
`time(p)` (§3.8).

That leaves interval with no natural total order.
PostgreSQL's order comes from flattening the three components
into one microsecond count at 30 days to the month and 24
hours to the day, in 128 bits:

```
span = time + (month * 30 + day) * USECS_PER_DAY
```

which is **not injective** — `'1 mon'` and `'30 days'` both
flatten to `2592000000000`, and PostgreSQL's btree calls them
equal. A primary key cannot have that, so the key is the
flattened datum with enough of the original behind it to
separate PostgreSQL's ties:

```
key = span(int128) ‖ months(int32) ‖ days(int32)
```

24 bytes, each component sign-flipped and big-endian. The
microsecond component is not in the key because it follows from
the other three:

```
time = span - (month * 30 + day) * USECS_PER_DAY
```

The order this gives **refines** PostgreSQL's: it agrees
wherever PostgreSQL is decisive, and where PostgreSQL calls
two distinct datums equal it breaks the tie deterministically
instead of merging two rows into one.

128 bits are arithmetically necessary, not defensive: over
the full component ranges `span` reaches about `5.8e21`, well
past `int64`. Verified — the identity, the injectivity and
the refinement all checked by property over generated
intervals.

### 3.11 uuid

Native is the 16 bytes. PostgreSQL orders `uuid` by
`memcmp` over exactly those bytes, so the key is the bytes
unchanged: fixed width, prefix-free, nothing to transform.

The bytes are in the uuid's own order, not swapped to
little-endian: a uuid is a byte string, not an integer. The
text is PostgreSQL's `uuid_out`, lower-case hex in groups of 8,
4, 4, 4 and 12 joined by hyphens. PostgreSQL also reads upper
case, braces and missing hyphens, but never prints them, so sm2
refuses them.

---

## 4. What is not here yet

Not in the first set, and the format leaves room for each
without disturbing what is already written
([`TYPE.md`](TYPE.md#5-why-the-types-are-so) §5):
arrays, composites, ranges, domains, enums, JSON and JSONB,
the network and geometric types, `money`, `xml`, and bit
strings. [`FEATURELIST.md`](../FEATURELIST.md), at the top level,
tracks which of
them are wanted and what each one needs.

`interval` was once on this list as a candidate for exclusion,
on the grounds that it has no total order. §3.10 is the
answer, so it is in.

---

## 5. Why the types are so

§3 says what each type is. This section says why. The rules the
set satisfies:

- **The set is fixed**, and comes from what the PostgreSQL target
  needs first. Arrays, composites, ranges and user-defined types
  are not in it. The format leaves room to add them without
  changing what is on disk (§4).
- **`enum` is not an sm2 type.** Its labels are catalog data, so
  the type cannot be closed over inside sm2. sm2 would store an
  enum as `int4`, with the labels above it, as PostgreSQL does.
  An index on an enum then orders by the `int4`, so whoever
  assigns the values assigns them in declared order, if an index
  should give label order. sm3's Catalog holds no labels yet: it
  has no entry kind for them.
- **A type is three things together:** an in-memory datum, an
  order-preserving key encoding (§3), and a tuple encoding
  ([`TUPLE.md`](TUPLE.md)). A type is not added until it has all
  three.
- **A type code goes on disk in the schema**, not in each row. A
  type removed from the set does not free its number (§3).
- **Null is a property of the column, not of the type.** The
  tuple's null bitmap and the key's null marker (§2) carry it. So
  no type needs a reserved datum to mean "absent".
- **Collation is a property of the column too.** Two `text`
  columns under different collations sort differently and are
  encoded differently, and they are the same type.
- **A type's comparison order is its own**, and the key must
  reproduce it bytewise. Where SQL's order is odd, as for IEEE
  negative zero and NaN, §3.3 writes the choice down.

### 5.1 The representation is PostgreSQL's

**Decision: a datum's bytes are what PostgreSQL makes them, type
by type, with the decimals the one exception.** This means the
same widths, the same epochs, the same units and the same struct
layouts, not a format of our own that holds the same information.

The executor works in tuple slots
([`../PART4.md`](../PART4.md#6-what-this-forces-on-sm2-and-sm3-now)
§6). A bespoke
encoding of each datum pays a translation on every column of
every row, in both directions. With PostgreSQL's representation
the cost is per tuple, not per datum: the framing around the
columns must still be built ([`TUPLE.md`](TUPLE.md)), but the
column bytes
are copied, not converted. `test/pg_fixtures.h` pins each width,
epoch and unit to bytes PostgreSQL stored (§9).

Two places need sm2 to be more precise than PostgreSQL is:

- **Byte order.** A stored PostgreSQL datum is native-endian,
  which is why `pg_upgrade` refuses to cross architectures. An
  on-disk format cannot be that vague, so sm2 fixes
  **little-endian**. On every machine digres targets the copy is
  still a `memcpy`. On a big-endian machine it is a byte swap,
  and the file stays readable.
- **Variable-width datums.** A type's functions take the payload
  alone. The varlena header, the 1-byte short form and the TOAST
  pointer are framing, and framing is the tuple's business. In a
  tuple a variable-width datum is a PostgreSQL varlena, header
  and all, so PostgreSQL can read it in place
  ([`VARLENA.md`](VARLENA.md)).

### 5.2 The decimals follow DuckDB's

**Decision: the decimals are fixed-width, with DuckDB's `DECIMAL`
as their reference, and there are two of them: `decimal64` and
`decimal128`.** PostgreSQL's numeric is a varlena of base-10000
digits with a weight and a display scale, with no real bound on
its length. DuckDB's is a two's-complement integer, the value
times 10^s, with `(precision, scale)` in the schema and at most
38 digits. sm2 takes that at two of DuckDB's four widths, each
its own type: 64 bits for a precision up to 17, and 128 bits up
to 36. The scale in the low bits costs a digit and two of
DuckDB's 18 and 38.

Three things follow, and they are why the exception is worth
making:

1. **The key encoding falls out.** A fixed-width two's-complement
   integer, sign bit flipped and written big-endian, is already
   order-preserving. PostgreSQL's digit form would need a bespoke
   order-preserving encoder over a datum with no length bound.
2. **It is a fixed-width column** in the tuple.
3. **Comparison is integer comparison**, once the column fixes
   the scale.

What a column may declare, and what a datum may hold:

- **Two types, one for each width.** `decimal64` is 8 bytes and,
  like `int8`, passed by value. `decimal128` is 16 bytes and
  passed by reference, like `uuid`. Each takes PostgreSQL's
  numeric typmod and any precision up to its capacity, so a
  `decimal128(10, 2)` is legal, if wider than it need be. Every
  sm2 type's width is then its type's alone, not its typmod's.
- **The typmod is required.** There is no bare form. `-1` is
  refused, like any typmod outside the type's range, by the
  type's `typmod_valid`, which `datum_width` calls. The host
  resolves a bare `decimal`, `decimal(p)` and the `numeric` alias
  to a declared `(p, s)`. sm takes what it is given.
- **SQL's `decimal(p, s)` is not an sm type.** The host maps it
  to one of the two, choosing the width
  ([`../../dg/DECIMAL.md`](../../dg/DECIMAL.md)
  §2.3). So the names a user writes are resolved above sm.
- **The scale is in the datum**, below the value (§3.4). So a
  datum means a value without its typmod, and a host can pass a
  `decimal64` by value as it is. It is always the column's scale,
  so the scale bits of a column's datums are equal.
- **One key rule at two widths.** Both are the same
  two's-complement integer, so the key is §3.2's at 8 or 16
  bytes.
- **NaN and the infinities are refused** on insert with a clear
  error, and no value is reserved as a sentinel. That door is not
  closed: 17 digits do not reach the limit of the 59 bits above
  the scale, nor 36 that of the 122. So the extreme integers are
  never written, and could be reserved later without changing
  what stored bytes mean, as long as the codec keeps refusing
  every value outside the declared precision.

### 5.3 A type is an ADT, in PostgreSQL's shape

**Decision: a type is a descriptor of function pointers, as
`pg_type` is.** PostgreSQL makes each type a row of functions,
with the comparison and hash functions arriving through the
operator class. sm2 does the same in one struct a type, `type_t`
in `include/sm2.h`:

| Entry | What it is | PostgreSQL's |
| --- | --- | --- |
| `in` / `out` | text form, parsed and printed | `typinput` / `typoutput` |
| `cmp` | the type's own total order | the btree opclass support function |
| `key_encode` | the order-preserving, injective byte string (§3) | no equivalent |
| `key_decode` | the inverse, where one exists: for index-only scans, not for reads | no equivalent |
| `typmod_valid` | whether a typmod is one the type accepts; null accepts any | `typmodin` |
| `len`, `byval`, `align` | fixed or variable width, and how PostgreSQL passes and aligns it | `typlen`, `typbyval`, `typalign` |
| `recv` / `send` | binary form on the wire: **deferred**, null, as sm2 has no wire protocol | `typreceive` / `typsend` |
| `hash` | for grouping and hash joins: **deferred**, null, until something needs it | the hash opclass support function |

`key_encode` has nothing behind it in PostgreSQL. A PostgreSQL
btree holds datums and calls `cmp`, so it never needs a byte
string that sorts correctly on its own. sm1 compares with
`memcmp` and nothing else, so sm2 does.

`in` and `out` are filled, because every test and fixture writes
its values as text. Only canonical text is input: what PostgreSQL
prints under DateStyle ISO, IntervalStyle iso_8601 and TimeZone
UTC.

Adding a type is then adding a descriptor.

### 5.4 Interval keeps PostgreSQL's three components

**Decision: `interval` is PostgreSQL's datum, all three
components of it.** §3.10 gives the bytes. The components are not
convertible into one another, so `'1 mon'` and `'30 days'` are
different datums, and adding them to one timestamp gives
different answers.

So `interval` has no natural total order, and a key encoding
needs one. PostgreSQL's `interval_cmp_value` flattens the
components to one microsecond count, in 128 bits (checked against
PostgreSQL 18's source). That order is deliberately not
injective, and a primary key cannot have equal keys for different
datums. §3.10 says what the key is. Its shape is that of a
collated text key: an ordering value that is not injective, with
the identity it dropped appended behind it.

## 6. Why the keys are so

[`../LSMINDEX.md`](../LSMINDEX.md#2-key-encoding) §2 holds the
rules a key satisfies: `memcmp`
order, injective and not reversible, prefix-free, framing,
collations and cut keys. This section holds the rest.

- **`strcoll` is never called at compare time.** It is slow for
  each comparison, it wants NUL-terminated input where a key may
  hold `0x00`, and it reads process-global locale state. It is
  also a comparator, which is the one thing sm1 cannot be given.
  The locale-aware work runs once, in the encoder.
- **Sort keys come from ICU alone.** `C` and `POSIX` mean byte
  order, and any other collation is ICU's
  (`src/type/collation.cc`). glibc's collation is not stable
  across versions.
- **`key_encode` lives in `src/type`**, not in a directory of its
  own. It is one of a type's ADT functions, so a type's
  representation and its key sit side by side, and a multi-column
  key is composed from them (`src/type/key.cc`).
- **The encoder takes each column's description**,
  `key_column_t`, so a descending column is the ascending
  encoding inverted and scanned forward (§2). The fork's planner
  never asks for a backward scan
  ([`../LSMINDEX.md`](../LSMINDEX.md#55-what-a-scan-leaves-to-the-host)
  §5.5).

## 7. Costs accepted

- **A decimal is converted.** It is the one type converted datum
  by datum at the executor boundary, in both directions, where
  §5.1 makes every other type a copy.
- **Precision stops at 36 digits.** PostgreSQL allows 1000 for a
  declared typmod, and more for an undeclared one. A column that
  needs more has no representation.
- **A decimal has no NaN and no infinity.** PostgreSQL's numeric
  has both. A fixed-width decimal is an integer and has neither.
- **A bare `decimal` can round.** The scale must be declared, so
  its host gives it a default: digres gives `(17, 3)`, the widest
  `decimal64`'s precision. A datum a user believed exact loses
  digits past the third fractional place, with no error, and one
  of 10^14 or more is refused. Refusing a bare `decimal` would be
  louder. The default was chosen for compatibility.
- **Little-endian is in the format.** A big-endian host
  byte-swaps every datum where others `memcpy`.
- **A collated text key column is stored three ways:** the sort
  key and the tie-break original in the key, and the datum in the
  tuple. The tie-break is not optional: without it two distinct
  strings can share one key.
- **A column's direction is fixed when its key is written.**
  Inverted bytes scanned forward mean no mirror relation and no
  second copy, but changing `ASC` to `DESC` rewrites the keys.
- **The type set is closed at the format.** New types are cheap,
  but only while the type code and the tuple layout leave room
  for them. That room is spent once.

## 8. Rejected

- **A format of our own for each datum.** Every column of every
  row would pay a translation at the executor (§5.1).
- **Arrow's decimal**, with a 256-bit form up to 76 digits.
  DuckDB's limit of 38 was preferred.
- **One `decimal` type whose typmod chose the width**, code 7. It
  was split so that no width depends on a typmod, and code 7 is
  retired.
- **A variable-width decimal.** A second key encoding and
  comparison path is most of what §5.2 avoids.
- **A `switch` on the type** in the key encoder, another in the
  tuple codec, another in the scan path. That is three places to
  forget, and the first one forgotten is a corrupt index (§5.3).
- **A length prefix on a variable-width key.** It breaks
  ordering. A wider alphabet costs more than `0x00 0xFF` escaping
  does.
- **glibc's `strxfrm_l` for sort keys.** Its keys change across
  glibc versions, so a stable key means ICU.

## 9. Tests

Run on the debug build ([`../../CLAUDE.md`](../../CLAUDE.md)).
Paths are relative to
`sm2/`.

- **Types** (`test/test_type.cc`). For every type, a datum round
  trips, and `cmp` agrees with hand-written expectations at the
  boundaries: min, max, zero, negative zero, NaN, the empty
  string.
- **Keys** (`test/test_key.cc`). These are property tests, not
  examples. For every type and column order, `encode(a) <
  encode(b)` if and only if `a < b`, over generated datums with
  the boundaries among them. A key round trips where it is
  reversible, and the documented loss is proved where it is not.
  `interval` has two properties of its own: its key round trips,
  and its order refines PostgreSQL's.
- **Fixtures** (`test/test_pg_fixtures.cc`). A datum's bytes
  equal what PostgreSQL stores for the same value, both ways
  through `in` and `out`. This is the one way "the representation
  is PostgreSQL's" is a claim and not an intention. `decimal64`,
  `decimal128`, `bpchar` and `name` have no fixtures.

**Property tests are hand-rolled**, with no new dependency
(`test/gen.h`). Each type has a generator weighted to small
values and its boundaries. A property runs every pair of boundary
values first, then N random pairs, from a fixed default seed. The
environment variable `WISR_SEED` overrides the seed. A failing
property prints its seed through doctest, so any failure replays
exactly. There is no shrinking: size-biased generation keeps
failures small.

**The PostgreSQL fixtures are observed on disk, once.**
`test/pg_fixtures.h` holds each value with the bytes PostgreSQL
stores for it, read through the `pageinspect` extension.
`test/pg_fixtures.sh` produces the header:

1. Make a one-column table for each type.
2. Insert the values.
3. Take each tuple's data bytes from
   `heap_page_items(get_raw_page(...))`.
4. Strip the varlena header, since a type's functions take the
   payload alone.

The header records the PostgreSQL version and the date. The
script needs a server and superuser rights on a developer's
machine, and the build never touches PostgreSQL. The bytes hold
on little-endian hosts only, which the test asserts.

The values are the generators' boundaries and representative
ones: multibyte UTF-8, the empty string, timestamps at 1970 and
at PostgreSQL's 2000 epoch. On top of these:

| Types | More values, all seeded |
| --- | --- |
| date and time types | 500 each, made by PostgreSQL, half within a few thousand years of 2000 and half anywhere in range |
| `interval` | 500, in PostgreSQL's verbose input form, so the ISO 8601 text is PostgreSQL's own printing |
| `uuid` | 500, from `random()` through `md5` |
| `text`, `varchar`, `bytea` | 500 each, the text ones from random code points of every UTF-8 length |
| `float4`, `float8` | 2,500 each, since PostgreSQL's float text is not always the shortest decimal: 2,000 random finite bit patterns, and 500 integers just past the width's exact range, where most of the exceptions lie |

The development server is PostgreSQL 18.6 built from source: tag
`REL_18_6`, cloned into `~/p/postgres18` and installed under
`~/pg18` with `pageinspect`. Its data directory is `~/pg18/data`,
with superuser `postgres` and trust authentication. It runs only
for a fixture run:

```
~/pg18/bin/pg_ctl -D ~/pg18/data -l ~/pg18/server.log \
    -o "-p 5418 -k $HOME/pg18" -w start
~/pg18/bin/psql -h ~/pg18 -p 5418 -U postgres
~/pg18/bin/pg_ctl -D ~/pg18/data -w stop
```
