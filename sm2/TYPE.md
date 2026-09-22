# TYPE — sm2, the types one by one

The reference for every type sm2 supports: how its datum is
represented, how it is ordered, and what its key bytes are.
`DESIGN.md` §2 argues *why* these choices; this file is the
table you look things up in while writing code.

**Status: nothing here is implemented.** These are the
formats as designed. Where a fact about PostgreSQL is from
memory rather than checked against the source it says so, and
`PLAN.md` step 1 pins each one to a fixture taken from a
running PostgreSQL.

---

## 1. The two representations

Every datum has two forms, and the rule for which is used is
mechanical:

| Form | Where it is used | What it is |
| --- | --- | --- |
| **native** | in a tuple, always | PostgreSQL's own representation of the datum (`DESIGN.md` §2.1) |
| **key** | only when the datum is a key, or part of one | `key_encode(datum)` — a byte string whose `memcmp` order is the type's order (`DESIGN.md` §3) |

A tuple stores native datums and nothing else. A key column
is *also* encoded into the sm1 key, so it appears twice: once
encoded in the key, once native in the tuple. The tuple is
what a read returns; the key exists to order and to identify.

Three rules follow, and they apply to every type below.

- **`key_encode` is injective.** Distinct datums get distinct
  keys. A key is the row's identity, so two datums that share
  a key are one row, and a unique constraint that cannot tell
  them apart is not a constraint.
- **`key_encode` need not be reversible.** The tuple holds
  the datum, so nothing depends on getting it back out of the
  key. `key_decode` is provided where an inverse exists, for
  sm3's index-only scans, and is an optimization rather than
  a requirement.
- **Where PostgreSQL calls two datums equal, they may share a
  key.** That is agreement with PostgreSQL, not a collision.
  `-0.0` and `0.0` are the case that comes up.

## 2. Rules that apply to every key

Applied in this order, around whatever the per-type encoding
in §3 produces.

**Null.** A nullable column's key is preceded by one marker
byte: `0x00` for null, `0x01` for non-null when nulls sort
first; `0x01` and `0x02` when they sort last. A `NOT NULL`
column has no marker byte and pays nothing. No type needs a
reserved value to mean absent.

**Direction.** A `DESC` column is the ascending encoding with
every byte inverted, marker included, because sm1 will not
scan backwards (`DESIGN.md` §1). Inversion is applied last,
to the finished per-column bytes.

**Concatenation.** A multi-column key is the per-column keys
joined end to end with nothing between them. For this to be
unambiguous no encoded datum may be a prefix of another, so
every variable-length encoding is made prefix-free by §3's
escaping rule. Fixed-width encodings are prefix-free already.

**Length.** `SM_MAX_KEYLEN` is 4096 and bounds the whole
concatenation, prefix and all. What happens when a key does
not fit is in `OBJECTIVE.md`: refused for a primary key,
truncated and rechecked for a secondary index.

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
| `numeric` | 7 | Arrow decimal, two's complement, LE | 16 or 32 | sign-flipped, BE | 16 or 32 |
| `text` | 8 | payload bytes | var | collation sort key ‖ escaped original | var |
| `varchar` | 9 | payload bytes | var | as `text` | var |
| `bytea` | 10 | payload bytes | var | escaped bytes | var |
| `date` | 11 | days since 2000-01-01, `int32` | 4 | sign-flipped, BE | 4 |
| `time` | 12 | µs since midnight, `int64` | 8 | sign-flipped, BE | 8 |
| `timestamp` | 13 | µs since 2000-01-01 00:00:00, `int64` | 8 | sign-flipped, BE | 8 |
| `timestamptz` | 14 | as `timestamp`, in UTC | 8 | sign-flipped, BE | 8 |
| `interval` | 15 | `{time int64, day int32, month int32}` | 16 | `span` ‖ months ‖ days | 24 |
| `uuid` | 16 | the 16 bytes | 16 | the same 16 bytes | 16 |

Codes are illustrative until the first release; what matters
is that they are assigned once and never reused
(`DESIGN.md` §2).

### 3.1 bool

Native is one byte, `0` or `1`. The key is that byte
unchanged: `false` sorts before `true`, which is
PostgreSQL's order.

### 3.2 int2, int4, int8

Native is two's complement, little-endian (`DESIGN.md` §2.1).

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

### 3.4 numeric

Native is Arrow's decimal (`DESIGN.md` §2.2): a two's
complement integer, little-endian, 16 bytes for a declared
precision up to 38 and 32 bytes up to 76, with `(precision,
scale)` held in the schema rather than in the datum.

The key is §3.2's, at the datum's width — sign bit flipped,
big-endian. Nothing bespoke, which is the point of the whole
exception.

Two datums compare correctly only at a common scale, and the
schema fixes the scale per column, so the encoder never
rescales. Comparing two columns of different scale is a cast,
above sm2.

Open, in `OBJECTIVE.md`, and both affect this section: what a
bare `numeric` with no declared precision does, and whether
NaN and infinity get reserved sentinel values.

### 3.5 text, varchar

Native is the payload bytes alone — no varlena header, no
1-byte short form, no TOAST pointer (`DESIGN.md` §2.1).
Length lives in the tuple's framing.

The key depends on the column's collation, which is a
property of the column and not of the type:

```
key = sortkey(collation, datum) ‖ 0x00 0x00 ‖ escape(datum)
```

- `sortkey` is the collation's transform, run **once here in
  the encoder** — `strxfrm_l`, or ICU's `ucol_getSortKey` —
  never `strcoll` at compare time, because sm1 compares with
  `memcmp` and takes no comparator (`DESIGN.md` §3). Its
  output carries no embedded `0x00`, which is what lets the
  `0x00 0x00` separator above sit between it and the
  tie-break. Budget 2-4x the input length against
  `SM_MAX_KEYLEN`.
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
- The collation's version is recorded with the schema
  (`sm3/DESIGN.md` §2); a mismatch is refused, not trusted.
- A non-deterministic collation is refused at column creation
  in the first version (`OBJECTIVE.md`).

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

### 3.7 date

Native is days since 2000-01-01 as `int32`, PostgreSQL's
epoch. The key is §3.2's `int4` key.

PostgreSQL reserves the extreme values for `-infinity` and
`infinity`; being the extremes, they sort at the ends of the
range with no special handling.

### 3.8 time

Native is microseconds since midnight as `int64`, so the
range is `0` to `86400000000` inclusive — PostgreSQL admits
`24:00:00`. The key is §3.2's `int8` key. The sign flip is a
formality for a datum that is never negative, and is kept so
that every fixed-width integer key is built the same way.

### 3.9 timestamp, timestamptz

Native is microseconds since 2000-01-01 00:00:00 as `int64`.
`timestamptz` is the same number held in UTC; the two differ
in how they are rendered and in nothing else on disk, which
is also PostgreSQL's arrangement. The key is §3.2's `int8`
key, and PostgreSQL's reserved infinities sort at the ends as
in §3.7.

### 3.10 interval

Native is PostgreSQL's struct, 16 bytes:

```
{ time int64 (µs), day int32, month int32 }
```

The three components are kept apart because they are not
convertible: a month is 28 to 31 days, a day is 23, 24 or 25
hours across a daylight-saving boundary. So `'1 mon'` and
`'30 days'` are different datums (`DESIGN.md` §2.4).

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

---

## 4. What is not here yet

Not in the first set, and the format leaves room for each
without disturbing what is already written (`DESIGN.md` §2):
arrays, composites, ranges, domains, enums, JSON and JSONB,
the network and geometric types, `money`, `xml`, and bit
strings. `FEATURELIST.md`, at the top level, tracks which of
them are wanted and what each one needs.

`interval` was once on this list as a candidate for exclusion,
on the grounds that it has no total order. §3.10 is the
answer, so it is in.
