# TYPE — sm2, the types one by one

The reference for every type sm2 supports: how its datum is
represented, how it is ordered, and what its key bytes are.
`SM2_DESIGN.md` §2 argues *why* these choices; this file is the
table you look things up in while writing code.

**Status: built**, every type here, with its text, its order
and its key (`SM2_DESIGN.md` §8.2). Where a fact about
PostgreSQL is from memory rather than checked against the
source it says so, and `SM2_DESIGN.md` §8.2 step 1 pins each
one to a fixture taken from a running PostgreSQL.

---

## 1. The two representations

Every datum has two forms, and the rule for which is used is
mechanical:

| Form | Where it is used | What it is |
| --- | --- | --- |
| **native** | in a tuple, always | PostgreSQL's own representation of the datum (`SM2_DESIGN.md` §2.1) |
| **key** | only when the datum is a key, or part of one | `key_encode(datum)` — a byte string whose `memcmp` order is the type's order (`SM2_DESIGN.md` §3) |

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
  is an optimization rather than a requirement. Every type has
  one; a float's gives back the datum its key keeps, `0` for
  `-0` and the quiet NaN for any NaN (§3.3), which PostgreSQL
  calls equal to the original.
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
rewrites a key (`SM2_DESIGN.md` §3.1). No type needs a reserved
value to mean absent. First and last mean in the finished key:
since a descending column's marker is inverted with the rest
(below), a descending column with nulls first takes the
nulls-last markers, which inversion turns into nulls first.

**Direction.** A `DESC` column is the ascending encoding with
every byte inverted, marker included, because sm1 will not
scan backwards (`SM2_DESIGN.md` §1). Inversion is applied last,
to the finished per-column bytes.

**Concatenation.** A multi-column key is the per-column keys
joined end to end with nothing between them. For this to be
unambiguous no encoded datum may be a prefix of another, so
every variable-length encoding is made prefix-free by §3's
escaping rule. Fixed-width encodings are prefix-free already.

**Length.** `sm1::max_keylen` is 4096 and bounds the whole
concatenation, the 2-byte object prefix included. What
happens when a key does not fit is in `SM2_DESIGN.md` §3.2:
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

The codes are frozen: assigned once, never reused or
renumbered (`SM2_DESIGN.md` §2). Code 7 was `decimal`, before
it was split into `decimal64` and `decimal128`, and is retired.
Codes 64–127 are reserved for types added later; 0 is never a
type code.

### 3.1 bool

Native is one byte, `0` or `1`. The key is that byte
unchanged: `false` sorts before `true`, which is
PostgreSQL's order.

### 3.2 int2, int4, int8

Native is two's complement, little-endian (`SM2_DESIGN.md` §2.1).

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

SQL's `decimal(p, s)`, and `numeric` its alias, are not sm2
types: sm3 maps them to one of these two
(`sm3/SM3_DESIGN.md` §2.1).

Native follows DuckDB's decimal (`SM2_DESIGN.md` §2.2): the
value times 10^s as a two's complement integer, little-endian,
8 bytes for `decimal64` and 16 for `decimal128`, with
`(precision, scale)` held in the schema rather than in the
datum. The typmod is PostgreSQL's numeric typmod,
`((p << 16) | s) + 4`, with `0 <= s <= p` and `1 <= p <= 18`
for `decimal64`, `38` for `decimal128`; `-1` is refused.

The key is §3.2's, at the datum's width — sign bit flipped,
big-endian. Nothing bespoke, which is the point of the whole
exception.

Two datums compare correctly only at a common scale, and the
schema fixes the scale per column, so the encoder never
rescales. Comparing two columns of different scale, or a
`decimal64` with a `decimal128`, is a cast, above sm2.

Decided in `SM2_DESIGN.md` §2.2: a typmod is required, and NaN
and infinity are refused on insert, so no value is reserved as
a sentinel.

### 3.5 text, varchar

Native is the payload bytes alone — no varlena header, no
1-byte short form, no PostgreSQL TOAST pointer (`SM2_DESIGN.md`
§2.1).
Length lives in the tuple's framing.

The bytes must be valid UTF-8 with no NUL, as in a PostgreSQL
database whose encoding is UTF8: no overlong form, no
surrogate, nothing past U+10FFFF. `in` and `out` both check,
and the text is the datum itself. Until the collation work of
step 2, `cmp` orders the bytes, which is codepoint order.

The key depends on the column's collation, which is a
property of the column and not of the type:

```
key = sortkey(collation, datum) ‖ 0x00 0x00 ‖ escape(datum)
```

- `sortkey` is the collation's transform, run **once here in
  the encoder** — `strxfrm_l`, or ICU's `ucol_getSortKey` —
  never `strcoll` at compare time, because sm1 compares with
  `memcmp` and takes no comparator (`SM2_DESIGN.md` §3). Its
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
  (`sm3/SM3_DESIGN.md` §2); a mismatch is refused, not trusted.
- Under a **non-deterministic** collation the tie-break is
  left out: the key is `sortkey ‖ 0x00 0x00`, so strings ICU
  calls equal share a key, and `key_decode` fails with
  `SM_ERR_INVALID`, since the key holds no string
  (`SM2_DESIGN.md` §3.3). Byte order is always
  deterministic.
- `text` (8) and `varchar` (9) share every byte and keep
  separate codes only so a round trip never rewrites a
  column's declared type. The `varchar(n)` limit is a
  coercion the host applies; sm2's codec stores what it is
  given (`SM2_DESIGN.md` §2).

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
`'30 days'` are different datums (`SM2_DESIGN.md` §2.4).

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
without disturbing what is already written (`SM2_DESIGN.md` §2):
arrays, composites, ranges, domains, enums, JSON and JSONB,
the network and geometric types, `money`, `xml`, and bit
strings. `FEATURELIST.md`, at the top level, tracks which of
them are wanted and what each one needs.

`interval` was once on this list as a candidate for exclusion,
on the grounds that it has no total order. §3.10 is the
answer, so it is in.
