# sm's decimals carry their scale: the plan

sm2's `decimal64` and `decimal128` hold a scaled integer and
nothing else; the scale is the column's typmod's
(`sm2/TYPE.md` §3.4, `sm2/SM2_DESIGN.md` §2.2). This plan puts
the scale into the datum itself, at the same widths, so that a
datum means a value on its own. Reviewed; §5 records the
answers. Built: §3 and §4.

## 1. Why

- **digres wants `numeric` by value** (`../dg/DECIMAL.md`
  §5). Most numerics fit in 17 digits. PostgreSQL's executor
  loses the typmod through an expression, so dg's value must
  carry its scale (`../dg/DECIMAL.md` §2.2). An 8-byte datum
  that already holds it is one dg can hand to PostgreSQL as a
  `Datum` with no conversion, where today `numeric_from_sm()`
  (`../dg/src/backend/access/sm/smstore.c`) pallocs a 32-byte
  `decimal_t` for every value read.
- **The cost**: a digit of `decimal64` and two of
  `decimal128` (§2), and DuckDB's decimal is no longer sm's
  format byte for byte.

## 2. The format

The datum stays a two's complement integer, little-endian, at
the same width; its low bits are the scale.

| Type | Width | Datum | Scale | Value bits | Precision |
| --- | --- | --- | --- | --- | --- |
| `decimal64` | 8 | `(value << 5) \| scale` | 0 to 17 | 59 | 1 to 17 |
| `decimal128` | 16 | `(value << 6) \| scale` | 0 to 36 | 122 | 1 to 36 |

- **value** is the number times 10^scale, as now. Read back by
  an arithmetic shift right.
- **59 bits hold 17 digits**: 2^58, some 2.88 × 10^17, is past
  10^17 - 1 but short of 10^18 - 1. **122 bits hold 36**:
  2^121, some 2.66 × 10^36, is past 10^36 - 1 but short of
  10^37 - 1. 18 scales need 5 bits and 37 need 6, so the lower
  precisions free no bit.
- **The scale is in the low bits** so that two datums of one
  scale order as their words do: the scale bits are equal, and
  the value's order is the word's.
- **The typmod stays `(p, s)`**, PostgreSQL's numeric typmod.
  The precision is still a check, and the scale is still the
  column's: every datum of a column has the column's scale.

## 3. What changes

### 3.1 sm2: `sm2/src/type/decimal.cc`

- **`max_precision()`** is 17 and 36.
- **`decimal_in()`** packs: `(value << k) | s`, k 5 or 6.
- **`decimal_out()`** unpacks, and refuses with
  `SM_ERR_INVALID` a datum whose scale is not the typmod's, as
  it now refuses one past the precision.
- **`decimal_cmp()`, `decimal_key()`, `decimal_unkey()`** are
  unchanged. Within a column the scale bits are constant, so
  the packed word, sign bit flipped and big-endian, still
  orders the column's values, and the key decodes back to the
  datum, scale included.
- **sm never rescales.** A datum at a scale other than its
  column's is refused, not converted; the host rounds to the
  column's scale before it writes, as dg's `numeric_to_sm()`
  does now.

### 3.2 sm2 and sm3 documents

- `sm2/TYPE.md` §3.4 and `sm2/SM2_DESIGN.md` §2.2: the format
  of §2, and why it parts from DuckDB's.
- `sm3/SM3_DESIGN.md` §2.1: `p <= 17` is `decimal64`,
  `18 <= p <= 36` is `decimal128`, past 36 refused at
  `CREATE TABLE`; a bare `decimal` or `numeric` is `(17, 3)`
  (§5).

### 3.3 dg, at the border only

- **`numeric_to_sm()`** packs the scale into the word it
  passes, after rounding to the column's scale as now.
- **`numeric_from_sm()`** unpacks the value and the scale.
- **`type_code()`** splits at 17, not 18, and a column past 36
  digits is refused.
- **`numeric_bound()`** packs its bound the same way.
- dg's in-memory `decimal_t` is left as it is. Making
  `numeric` by value in dg is the next piece of work
  (`../dg/DECIMAL.md` §5), not this one.

## 4. Order of work

Done in a worktree on its own branch, since `main` holds
staged work of its own.

1. **Baseline**: `make -j4 test` in `sm/` and
   `make -C ../dg -j4 test` pass before any change.
   → verify: both suites' logs saved, all passing.
2. **sm2's format** (§3.1), with its tests: a round trip at
   precision 17 and 36, a datum of the wrong scale refused, a
   precision past 17 or 36 refused by the typmod.
   → verify: `make -C sm2 -j4 test`.
3. **sm3's mapping** (§3.2) and its catalog test.
   → verify: `make -C sm3 -j4 test`.
4. **dg's border** (§3.3).
   → verify: `server.sh` stores `numeric(10, 2)`,
   `numeric(17, 17)` and `numeric(30, 5)`, indexes and keys
   them, and refuses `numeric(37, 0)`.
5. **The documents** (§3.2).
6. **The whole suite**, sm's and dg's, then `make format`.

Tests whose cases sit at precision 18 or 38 change with the
format: `sm2/test/test_type.cc`, `sm2/test/test_pg_fixtures.cc`
(its fixtures past 17 digits move to `decimal128`, past 36
are dropped), `sm3/test/test_catalog.cc`, and dg's decimal
regression files. None is an isolation scenario of
`sm1/mvcc`.

## 5. Answers

1. **The bare default.** A bare `decimal` or `numeric` was
   DuckDB's `(18, 3)` (`sm3/SM3_DESIGN.md` §2.1), which no
   longer fits `decimal64`. Settled: **`(17, 3)`**, a
   `decimal64`, by value, 14 integer digits; `(18, 3)` as a
   `decimal128` was the alternative, 16 bytes for every bare
   numeric.
2. **Databases already written.** Settled: **there is no
   existing database.** The format changes with no migration
   and no format version; databases already written are not
   considered until sm is in production.
