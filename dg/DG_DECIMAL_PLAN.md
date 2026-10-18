# digres's decimals mirror sm's: the plan

sm has two decimals, `decimal64` and `decimal128`, each its
scaled value with its scale in the low bits, and leaves the
choice of width to its host (`../sm/sm2/TYPE.md` §3.4,
`../sm/sm3/SM3_DESIGN.md` §2.1). digres has one, `numeric`, a
32-byte `decimal_t` passed by reference (`DECIMAL.md` §2.2).
This plan gives digres the same two types, `decimal64`, a
`Datum` by value in the executor, and `decimal128`, so that a
value read from sm is used as sm gave it, in either width.
`numeric` and `decimal` stay as names only, which the parser
turns into one of the two. It is the to-do of `DECIMAL.md` §5,
by its option 2. Reviewed; §6 records the answers.

## 1. Why

- **Most numerics fit in 17 digits.** Today every one read
  from sm is converted and palloc'd (`numeric_from_sm()`,
  `src/backend/access/sm/smstore.c`), 32 bytes behind a
  pointer, and every sort, hash, join key and group key on it
  chases that pointer.
- **A by-value `decimal64`** is 8 bytes in the `Datum`
  itself: no palloc, no copy at the sm border, sorts compare
  `datum1` directly, spill files and hash tables shrink.
- **A `decimal128` laid out as sm's** makes the wide case a
  pointer into sm's tuple, with no conversion either.

## 2. What PostgreSQL forces

- **By value is the type's, not the typmod's** (`typbyval`
  in `pg_type`), and a by-value datum is copied as bits. One
  type cannot be by value when small and by reference when
  large, so the two widths are two types.
- **The executor loses typmods** through expressions
  (`DECIMAL.md` §2.2), which is why each value carries its
  scale; sm's layout already does.

## 3. The design

### 3.1 Two types, and numeric a name for them

| Type | OID | Datum | Digits | Scale bits | sm |
| --- | --- | --- | --- | --- | --- |
| `decimal64` | 1700 | 8 bytes, by value: `(value << 5) \| s` | 1 to 17 | 5 | `decimal64` |
| `decimal128` | new | 16 bytes, by reference: `(value << 6) \| s` | 1 to 36 | 6 | `decimal128` |

- **Each is sm's datum, bit for bit.** The border in
  `smstore.c` becomes a copy of the word for `decimal64` and a
  pointer for `decimal128`; `numeric_to_sm()` and
  `numeric_from_sm()` lose their conversion.
- **`decimal64` takes OID 1700**, numeric's, so what names
  the type by OID, a client's driver first, finds it there.
- **`decimal128` is the wide one**, up to 36 digits, where
  digres's numeric now has 38: the type of every result that
  may need more than 17 digits.
- **`numeric` and `decimal` are syntactic sugar**, with no
  `pg_type` row of their own, as `int` and `integer` are names
  for `int4`. Where a type name is resolved,
  `typenameTypeIdAndMod()` (`src/backend/parser/
  parse_type.c`), they become one of the two: for a column, a
  cast, a function's declared argument or result,
  `'numeric'::regtype` and `to_regtype()`. `decimal64(p, s)`
  and `decimal128(p, s)` may be written too.

  | Written | Type | typmod |
  | --- | --- | --- |
  | `numeric`, `decimal` | `decimal64` | `(17, 3)` |
  | `numeric(p)`, `decimal(p)` | as `(p, 0)` | |
  | `numeric(p, s)`, `p <= 17` | `decimal64` | `(p, s)` |
  | `numeric(p, s)`, `18 <= p <= 36` | `decimal128` | `(p, s)` |
  | `numeric(p, s)`, `p > 36` | refused | |

- **A bare `numeric` means `decimal64(17, 3)` everywhere**,
  not only in a column (`DECIMAL.md` §2.3): there is no
  unconstrained numeric. So `x::numeric` rounds to 3 places
  and refuses past 14 integer digits, where PostgreSQL's would
  keep them; a user who wants more says so, as
  `x::numeric(30, 0)`.
- **A typmod applies where PostgreSQL applies one**: to a
  column and to a cast. A function's argument and result
  ignore it, as in PostgreSQL, so `f(numeric)` takes a
  `decimal64` at any scale, unrounded.
- **A literal** with a point is a `decimal64` if it has at
  most 17 digits, a `decimal128` if at most 36, and a `float8`
  past that, as past 38 now (`src/backend/parser/
  parse_node.c`); as an integer literal is `int4` when it
  fits and `int8` when not. A literal keeps its own scale:
  `1.23456` is a `decimal64` at scale 5, not rounded to 3.

### 3.2 What runs on decimal64 itself

A column's values stay by value through what is done to them
most:

- **Comparison**: `=`, `<>`, `<`, `<=`, `>`, `>=` and `cmp`,
  `decimal64` against `decimal64` and across to `decimal128`.
  At one scale, an integer comparison of the words; across
  scales, the lower rescaled.
- **Indexes and sorts**: a btree and a hash opclass for each
  type, in one family per method (`btree/numeric_ops`,
  `hash/numeric_ops`) with the cross-type operators, as int4
  and int8 share `integer_ops`. So an index on either type
  serves a key of the other. Sort support compares words.
- **Hashing** alike for equal values of either type: trailing
  zeros stripped first (`DECIMAL.md` §2.2), so a hash join of
  a `decimal64` and a `decimal128` key works.
- **`min`, `max`, `count`, `abs`, unary minus and `sign`**,
  and `DISTINCT` and `GROUP BY` by the opclasses: none needs
  more digits than its argument.

### 3.3 Arithmetic: a decimal128 result, by native paths

A result that may pass 17 digits is a `decimal128`:
`numeric(15, 2) * numeric(15, 2)` needs 30, and PostgreSQL
never overflows a numeric there. What makes arithmetic slow
today is not the width but decimal.c's general path: each
operand lifted into a 512-bit `wide_t`, the digits counted to
repack, a palloc for each cast and each result. So the common
cases go around that path:

- **`decimal64` with `decimal64`**: `+`, `-`, `*`, `/` of
  their own, returning `decimal128`, computed in `__int128`
  from the two words: no cast, no `wide_t`, one 16-byte palloc
  for the result. A quotient that needs more than `__int128`
  falls back to the general path.
- **`sum` and `avg` of `decimal64`**: an `__int128`
  accumulator in the transition state, no palloc per row; the
  result a `decimal128`. Overflow past 36 digits is an error,
  as `sum` past 38 is now.
- **`decimal128` itself**: add, subtract, multiply and
  compare take an `__int128` fast path when both values and
  the result fit, before `wide_t`.
- **Everything else** (`%`, `round`, `ln`, `sqrt`, `power`,
  the casts to text, and the rest of decimal.c's ~100
  functions) is `decimal128`'s, takes a `decimal64` by the
  implicit cast, and is not written twice. What returns a
  numeric today, `sum(int8)` and `avg(int4)` among it, returns
  a `decimal128`.

### 3.4 Casts

| From | To | Context |
| --- | --- | --- |
| `decimal64` | `decimal128` | implicit |
| `decimal128` | `decimal64` | assignment |
| `decimal64`, `decimal128` | itself | length coercion, rounding to the typmod |
| `int2`, `int4` | `decimal64` | implicit |
| `int8` | `decimal64` | assignment: 19 digits do not fit 17 |
| `int2`, `int4`, `int8` | `decimal128` | implicit |
| `float4`, `float8` | `decimal64`, `decimal128` | assignment |
| `decimal64`, `decimal128` | `int2`, `int4`, `int8`, `float4`, `float8` | as numeric's now |

PostgreSQL does not chain casts, so each of these is an entry
of its own in `pg_cast`.

### 3.5 What a client sees

- **The OID on the wire is 1700** for both types, in the row
  description (`printtup.c`), so drivers decode either as
  they decode `numeric` now.
- **Their text and binary forms are numeric's**: `out`
  prints as `numeric(p, s)` does, `send` writes PostgreSQL's
  base-10000 digits.
- **Their name is `numeric(p, s)`**, or `numeric` with no
  typmod, wherever a type is named to a user: `format_type()`,
  so `\d`, `pg_typeof()`, `pg_get_viewdef()`,
  `information_schema` and error messages read as they do
  now, and tools that look for `numeric` find it. The names
  `decimal64` and `decimal128` are seen only in `pg_type`.

## 4. What changes

- `src/include/utils/numeric.h`: both layouts,
  `NUMERIC_MAX_PRECISION` 36, and an accessor for each.
- `src/backend/utils/adt/decimal.c`: `dec_value()`,
  `dec_make()` and `dec_wide()` read and write the 16-byte
  layout as `decimal128`'s; the 38-digit limit becomes 36.
  `decimal64`'s in, out, recv, send, typmod, comparison, hash,
  sort support and casts; its native `+`, `-`, `*`, `/`,
  `sum` and `avg` (§3.3); `decimal128`'s `__int128` fast
  path.
- `src/include/catalog/`: `pg_type.dat`, `pg_proc.dat`,
  `pg_operator.dat`, `pg_cast.dat`, `pg_opclass.dat`,
  `pg_amop.dat`, `pg_amproc.dat`, `pg_aggregate.dat`. What
  is `numeric` there now becomes `decimal128`'s, but for what
  §3.2 runs on `decimal64`.
- `src/backend/parser/parse_type.c` and `parse_node.c`: the
  names and literals of §3.1.
- `src/backend/access/sm/smstore.c`: code 17 is `decimal64`,
  code 18 `decimal128`; the border copies.
- `src/backend/access/common/printtup.c` and
  `src/backend/utils/adt/format_type.c`: §3.5.
- What reads `NUMERICOID` and must take both types:
  `selfuncs.c`'s `convert_numeric_to_scalar()`,
  `ruleutils.c`, jsonb and jsonpath, `xml.c`, `dbsize.c`,
  `formatting.c`.

## 5. Order of work

Done on a branch, one step at a time, each with the full dg
suite before the next.

1. **Baseline**: `make -C dg -j4 test` and `make -j4 test` in
   `sm/` pass.
2. **The wide type takes sm's 16-byte layout**, 36 digits,
   still `numeric` at OID 1700; the border a pointer for code
   18.
   → verify: `select_numeric.sql` with its expected output
   moved from 38 digits to 36; `server.sh`'s numeric columns.
3. **The wide type becomes `decimal128`**, a new OID: every
   catalog entry and every `NUMERICOID` of §4 moved to it,
   1700 left free, and `numeric` a name for `decimal128` for
   now; a client still sees 1700 and `numeric` (§3.5).
   → verify: the whole suite unchanged, as nothing a user
   sees moves.
4. **`decimal64` at OID 1700**, 8 bytes by value: in, out,
   recv, send, typmod, the casts of §3.4, the names and
   literals of §3.1, columns of code 17.
   → verify: `server.sh` reads and writes `numeric(10, 2)`,
   `numeric(17, 17)` and `numeric(30, 5)`; `1.5` is a
   `decimal64`, a 20-digit literal a `decimal128`;
   `1.23456::numeric` is `1.235`, `(2^62)::numeric` an error
   and `(2^62)::numeric(30, 0)` not; `'numeric'::regtype` is
   1700.
5. **Comparison, indexes, sorts and hashing** (§3.2).
   → verify: an index on a `decimal64` column is used for a
   `decimal128` key and ranges, and the other way; a hash join
   across the two; sorts and `GROUP BY`.
6. **Arithmetic and aggregates** (§3.3): the native
   operators, `sum` and `avg`, then `decimal128`'s fast path.
   → verify: `select_numeric.sql`'s arithmetic over
   `decimal64` columns gives what it gave, at the edges too (a
   product past 17 digits, a sum past 36); and `sum(a * b)`
   over a few million rows, timed before and after, on
   `numeric(15, 2)` columns.
7. **The client's view** (§3.5) and the other readers of §4.
   → verify: psql's `\d`, `pg_typeof()` and
   `information_schema.columns` say `numeric`; a
   binary-format fetch.
8. **The documents**: `DECIMAL.md` §2 and §5.

## 6. Answers

1. **The types and their names.** Settled: **`decimal64` and
   `decimal128` are the types**, as sm's; `decimal64` takes
   OID 1700, `decimal128` a new one; **`numeric` and `decimal`
   are names only**, which the parser resolves (§3.1). Earlier
   drafts kept `numeric` a type: first the 16-byte one beside
   a new `decimal64`, then the 8-byte one beside a new
   `decimal128`.
2. **Arithmetic.** Settled: **a `decimal128` result, by
   native `__int128` paths** (§3.3). A `decimal64` result,
   fully by value, was the alternative: it fails past 17
   digits where PostgreSQL would not, and most of its speed
   comes from native integer arithmetic, which §3.3 has
   anyway.
3. **A literal.** Settled: **a `decimal64`** while it fits, 17
   digits; past them a `decimal128` (§3.1).
4. **The OID on the wire.** Settled: **1700 for both**
   (§3.5).
5. **A bare `numeric`.** Settled: **`decimal64(17, 3)`
   everywhere** (§3.1), a cast included, so there is no
   unconstrained numeric; a user who needs more digits names
   them. Making it `decimal128` outside a column, as
   PostgreSQL's unbounded numeric, was the alternative.
6. **The name a user sees.** Settled: **`numeric(p, s)`** for
   both types (§3.5), so tools that look for `numeric` keep
   working. `decimal64(p, s)` and `decimal128(p, s)`, true to
   the types but unknown to tools, was the alternative.
