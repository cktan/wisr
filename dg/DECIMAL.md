# Decimal in digres

PostgreSQL's `numeric` is replaced by two fixed-width
decimals, the ones sm stores (`../sm/sm2/TYPE.md` §3.4):
`decimal64`, of at most 17 digits, passed by value, and
`decimal128`, of at most 36; `numeric` is a name for them.
Reviewed; §4 records the answers. All of §3 is built
(`WORKNOTE.md` steps 95 to 97), and so is §5, `decimal64` by
value, which made §2 what it is.

## 1. Where digres stands

Where it stood before §3 was built.

- **`numeric` is PostgreSQL's**: a varlena of base-10000
  digits, up to 131,072 digits before the point and 16,383
  after, with NaN and ±Infinity (`utils/adt/numeric.c`, some
  11,400 lines).
- **It is everywhere, not only in columns.** A literal with a
  point, `1.5`, is `numeric`. 107 built-in functions return it
  and 87 take it: `sum(int8)`, `avg(int4)`, `round(x, 2)`,
  `extract()`, `ln`, `sqrt`, `pg_size_pretty`, `to_number`,
  `generate_series`, `random(numeric, numeric)` and more.
  jsonb keeps every JSON number as one, and jsonpath computes
  with it.
- **sm does not store it.** sm2's decimals are DuckDB's: the
  value times 10^s as a two's complement integer, 8 bytes up to
  17 digits (`decimal64`), 16 up to 36 (`decimal128`), with
  `(p, s)` in the schema, not in the datum, and no NaN or
  infinity. So a table cannot have a `numeric` column today.

## 2. Decisions

### 2.1 Two types, and numeric a name for them

- **`decimal64` and `decimal128` are the types**, sm's two:
  `decimal64` takes PostgreSQL's oid for numeric, 1700, so
  what names the type by oid, a client's driver first, finds
  it there; `decimal128` takes 8383.
- **Two widths are two types because PostgreSQL makes them
  so.** By value is the type's, not the typmod's (`typbyval`
  in `pg_type`), and a by-value datum is copied as bits: one
  type cannot be by value when small and by reference when
  large.

  | Type | OID | Datum | Digits | Scale bits | sm |
  | --- | --- | --- | --- | --- | --- |
  | `decimal64` | 1700 | 8 bytes, by value: `(value << 5) \| s` | 1 to 17 | 5 | `decimal64` |
  | `decimal128` | 8383 | 16 bytes, by reference: `(value << 6) \| s` | 1 to 36 | 6 | `decimal128` |

- **`numeric` and `decimal` are names only**, with no
  `pg_type` row of their own, as `int` is a name for `int4`.
  `typenameTypeIdAndMod()` (`parse_type.c`) resolves them
  wherever a type name is: a column, a cast, a function's
  declared argument or result, `'numeric'::regtype` and
  `to_regtype()`. A bare one is `decimal64` with no typmod, so
  a cast to it keeps a value's scale, as PostgreSQL's does:
  `-2.5::numeric` is `-2.5`. A column of it is `numeric(17, 3)`
  (§2.3). `decimal64(p, s)` and `decimal128(p, s)` may be
  written too.

  | Written | Type | typmod |
  | --- | --- | --- |
  | `numeric`, `decimal` | `decimal64` | none; a column's `(17, 3)` |
  | `numeric(p)`, `decimal(p)` | as `(p, 0)` | |
  | `numeric(p, s)`, `p <= 17` | `decimal64` | `(p, s)` |
  | `numeric(p, s)`, `18 <= p <= 36` | `decimal128` | `(p, s)` |
  | `numeric(p, s)`, `p > 36` | refused | |

- **A bare `numeric` refuses past 17 digits**, where
  PostgreSQL's would keep them; a user who wants more says so,
  as `x::numeric(30, 0)`.
- **A typmod applies where PostgreSQL applies one**: to a
  column and to a cast. A function's argument and result
  ignore it, so `f(numeric)` takes a `decimal64` at any
  scale, unrounded.
- **A client sees `numeric`**: oid 1700 for either type in the
  row and parameter descriptions (`printtup.c`, `postgres.c`),
  1231 for an array of either, and `numeric(p, s)`, or
  `numeric` with no typmod, as its name wherever a type is
  named to a user (`format_type()`: `\d`, `pg_typeof()`,
  `pg_get_viewdef()`, error messages). So a client, a driver
  or a query that says `numeric` is unchanged but for a bare
  `numeric` column's scale and a bare `numeric`'s 17 digits.
  The names `decimal64` and `decimal128` are seen only in
  `pg_type`. A parameter a client types 1700 is a
  `decimal128`, which holds whatever it sends.
- **Their text is PostgreSQL's**, and so is their binary wire
  format (`numeric_send`, `numeric_recv`), which drivers
  decode: the value is converted to PostgreSQL's base-10000
  digits on the way out, and from them on the way in.

### 2.2 A value carries its scale

PostgreSQL's executor does not carry a typmod through an
expression: `a * b`, a function's result, an aggregate's are
typmod -1. A scaled integer means nothing without its scale,
so the value carries it, as sm's datum does, in the low bits
of its word:

```
decimal64:  int64,  (value << 5) | scale   /* by value, typlen 8 */
decimal128: int128, (value << 6) | scale   /* by reference, typlen 16 */
typedef struct decimal_t {  /* decimal128's, in two halves */
  uint64 lo;
  int64  hi;
} decimal_t;
```

- **sm's datum bit for bit**: no varlena header, no
  detoasting, and no conversion at the sm border (§2.3).
- **The precision is not kept**: a value's digits are what
  they are, at most 17 or 36; a column's or a cast's precision
  is a check, as PostgreSQL's typmod is.
- **Equal values compare equal at any scale**: `1.5 = 1.50`.
  Comparison rescales the lower scale up and compares the
  integers, and at one scale compares the words; hashing first
  strips trailing zeros, so equal values hash alike, a
  `decimal64` as a `decimal128` of its value.

### 2.3 Stored as sm2's decimal, at the column's scale

- **A column is `numeric(p, s)`**, and dg, not sm, chooses
  its width (`../sm/CATALOG.md` §8.4): `p <= 17` is
  sm2's `decimal64`, `p <= 36` its `decimal128`, whose datums
  carry their scale below their value
  (`sm/sm2/TYPE.md` §5.2).
  Its typmod is PostgreSQL's, which sm2 takes as is; a column
  past 36 digits is refused.
- **Writing rescales to the column's scale**, rounding half
  away from zero as PostgreSQL's assignment does, and checks
  the precision; then the value is sm's datum as it is, a
  `decimal64`'s word, a `decimal128`'s pointer. Reading takes
  sm's datum as it is too, the word into the `Datum`, a pointer
  into sm's tuple. All in `smstore.c`: a row's insert and
  update, its deform, an index scan's keys, an index-only
  scan's values, and statistics' values.
- **An unconstrained `numeric` column is `numeric(17, 3)`**,
  the widest of sm2's `decimal64`, as sm2 requires a typmod
  (§4.1): `smstore.c` gives it that typmod when the table is
  made, so a value with more places is rounded to three, as
  an assignment to `numeric(17, 3)` is. A bare `numeric`
  anywhere else keeps its value's scale (§2.1).
- **An unconstrained `decimal128` column is `numeric(36,
  3)`**, the widest of sm2's `decimal128`, at the same three
  places. `CREATE TABLE AS` makes one from a `decimal128`
  expression, such as a literal of 18 to 36 digits.
- **Each decimal column is stored as sm's type of its
  width**, whatever its precision: a `decimal128(10, 2)` is
  sm's `decimal128`, not a `decimal64`, so its datums too are
  sm's as they are, with nothing to convert on a read.

### 2.4 Literals and casts

- **A literal with a point** is a `decimal64` of at most 17
  digits, a `decimal128` of at most 36, with the scale it is
  written with: `1.50` has scale 2. One past 36 digits is a
  `float8`, as DuckDB makes it, and so is one with an
  exponent, `1e3` or `3.4e38`: refusing those broke them. Text
  past 36 significant digits, `'0.1234...'::numeric(36, 35)`,
  is refused (§4.2).
- **Casts**: from and to the integers, `float4`, `float8`,
  `text`, `money`; `numeric(p, s)` rescales and checks, as
  PostgreSQL's typmod coercion does.

  | From | To | Context |
  | --- | --- | --- |
  | `decimal64` | `decimal128` | implicit |
  | `decimal128` | `decimal64` | assignment |
  | `decimal64`, `decimal128` | itself | length coercion, rounding to the typmod |
  | `int2`, `int4` | `decimal64` | implicit |
  | `int8` | `decimal64` | assignment: 19 digits do not fit 17 |
  | `int2`, `int4`, `int8` | `decimal128` | implicit |
  | `float4`, `float8` | `decimal64`, `decimal128` | assignment |
  | `decimal64`, `decimal128` | `int2`, `int4`, `int8`, `float4`, `float8` | as PostgreSQL's numeric's |
  | `decimal64` | `money` | assignment |
  | `jsonb` | `decimal64` | explicit |

  PostgreSQL does not chain casts, so each is an entry of its
  own in `pg_cast`, and `int8` and `decimal64` have no common
  type: `coalesce(int8_col, 1.5)` is an error, and a user
  casts one of them.
- **A `decimal64` finds `decimal128`'s functions**, where it
  has none of its own, before `float8`'s, the numeric
  category's preferred type: `func_select_candidate()`
  (`parse_func.c`) counts a `decimal64` taken as a
  `decimal128` as half an exact match, and an exact
  `decimal64` one beats either.

### 2.5 Arithmetic

Results are exact or an error, never silently wrong.

- **`+`, `-`**: scale `max(s1, s2)`.
- **`*`**: scale `s1 + s2`; past 36 digits, rounded to fewer
  places where the integer part still fits, else an error.
- **`/`**: PostgreSQL's choice of scale (`select_div_scale()`,
  at least 16 significant digits, at most 36 in all), rounded.
- **A result that may pass 17 digits is a `decimal128`**.
  `decimal64`'s `+`, `-` and `*` compute it in 128 bits, its
  `/` without casting; `decimal128`'s `+`, `-` and `*` take the
  same 128-bit path while the result fits 36 digits. Only past
  that, and for the rest, is the 512-bit wide path taken.
  What `decimal64` does itself is listed in §2.8.
- **`%`, `div`, `abs`, `sign`, `ceil`, `floor`, `round(x, n)`,
  `trunc(x, n)`, `gcd`, `lcm`, `factorial`, `min`, `max`**:
  exact, as PostgreSQL's.
- **`sqrt`, `ln`, `log`, `exp`, `power`**: computed in
  `long double`, 128-bit IEEE on this aarch64 (some 33
  digits), and rounded to PostgreSQL's choice of scale. Not
  PostgreSQL's arbitrary precision; enough (§4.3).
- **Overflow is an error**: "numeric field overflow", as
  PostgreSQL says it past a typmod.
- **No NaN, no infinity**: `'NaN'::numeric` and division by
  zero are errors, as sm2 refuses them anyway.

### 2.6 Aggregates

- **`sum(int8)`, `sum(numeric)`** accumulate exactly: exact
  to 36 digits, then an error, where PostgreSQL never
  overflows (§4.5). A `decimal64`'s sum is kept in 128 bits
  in the state, with no palloc a row, and flushed to the wide
  sum when its scale changes or it is read.
- **`avg` of integers and numerics** divides by the count at
  `/`'s scale (§2.5).
- **`stddev`, `variance`** of them: in `long double`.

### 2.7 What else used numeric

Each is moved to `decimal_t`, keeping its SQL signature:

- `extract()`, `date_part` (`timestamp.c`, `date.c`);
- jsonb's numbers (§4.4, as answered after the review): a JSON
  number written with an exponent, `1e300`, is a `float8`, and
  one without, `1.5`, a numeric. The two are one kind of
  number: equal, ordered, hashed and contained across the kinds,
  a numeric and a float8 compared as float8s, as SQL compares
  them. jsonpath computes in numeric, a float8 converted when it
  is computed with; one only selected stays as it is;
- `money`'s casts (`cash.c`), `pg_lsn`'s `+` and `-`,
  `pg_size_pretty(numeric)` (`dbsize.c`), `to_char` and
  `to_number` (`formatting.c`), `random(numeric, numeric)`,
  `generate_series(numeric, ...)`, `width_bucket`;
- the planner's selectivity, which reads a value as a
  `double` (`convert_numeric_to_scalar()`).

### 2.8 What decimal64 does itself

A `decimal64` keeps to its own operators and functions where
it has them; `func_select_candidate()` chooses an exact one
first (§2.4). Each is in `decimal.c`, and
`test/select_decimal64.sql` lists them with the types they
give.

Its btree and hash opclasses are in families of their own
(`decimal64_ops`), with no cross-type operators: with them,
`a = 1` on a `decimal64` column would find `(decimal64,
decimal64)` and `(decimal64, decimal128)` equally good. A
`decimal128` key on a `decimal64` column compares as
`decimal128`s, by the implicit cast, and uses no index on the
column, which holds no value of more than 17 digits anyway.

| Operator or function | Gives | How |
| --- | --- | --- |
| `=`, `<>`, `<`, `<=`, `>`, `>=`, btree and hash opclasses, sort support | `bool`, `int4` | the words at one scale, else rescaled in 128 bits |
| `min`, `max`, unary `-`, `abs`, `sign` | `decimal64` | in 64 bits |
| `ceil`, `ceiling`, `floor` | `decimal64` | in 64 bits; a value with places has at most 16 digits before the point, so never past 17 |
| `%`, `mod`, `gcd` | `decimal64` | in 128 bits at the larger scale; no more digits than the smaller operand's before the point, nor places than the larger scale |
| `round(x)`, `round(x, n)`, `trunc(x)`, `trunc(x, n)` | `decimal64` | as numeric's; past 17 digits or places, as a carry or `n` past 17 makes, an error, not a `decimal128` |
| `+`, `-`, `*` | `decimal128` | in 128 bits, one palloc for the result |
| `/` | `decimal128` | the wide path, without casting either side |
| `lcm` | `decimal128` | in 128 bits, the wide path only past them |
| `sum`, `avg` | `decimal128` | a 128-bit sum in the state, no palloc a row |
| `to_char(x, fmt)` | `text` | numeric's, on a `decimal128` built on the stack |
| casts to and from the integers, `float4`, `float8`, `money`, `jsonb` | | §2.4 |

Everything else, `sqrt`, `ln`, `log`, `exp`, `power`, `div`,
`factorial`, `width_bucket` and the rest, is `decimal128`'s,
reached by the implicit cast: one 16-byte palloc for the
argument. Their results need more than 17 digits as often as
not, so a `decimal64` version of them would save only that
cast, next to a computation in `long double` and the wide
path.

## 3. Order of work

1. **The type**: `decimal_t`, in and out, send and receive,
   comparison and hashing, the btree and hash opclasses, casts.
   `numeric.c` is replaced by `decimal.c`.
   → verify: a regression file of PostgreSQL's own numeric
   tests, trimmed to 36 digits, gives PostgreSQL's output.
2. **Arithmetic and functions** (§2.5).
   → verify: the same file's arithmetic and functions; the
   transcendental ones agree to their scale.
3. **Aggregates** (§2.6).
   → verify: sums and averages of integers and decimals,
   overflow past 36 digits refused.
4. **Columns** (§2.3).
   → verify: `server.sh` stores `numeric(10, 2)` and
   `numeric(30, 5)`, indexes and keys them, rescales on insert,
   and makes an unconstrained column `numeric(17, 3)`.
5. **The other users** (§2.7), jsonb last.
   → verify: each one's results in `server.sh`.
6. `numeric.c` is gone.

## 4. Answers of the review

1. **An unconstrained `numeric` column** is given a default,
   DuckDB's `DECIMAL(18, 3)`; `(17, 3)` since sm's decimals
   carry their scale (`sm/sm2/TYPE.md` §5.2).
2. **A literal past 36 digits** was to be an error; building
   it showed that this refuses `1e300` as a float, so it is a
   `float8`, as in DuckDB. Text cast to numeric past 36 digits
   is still an error.
3. **`sqrt`, `ln`, `exp`, `power`** in `long double`, some 33
   digits, are enough.
4. **jsonb**: a number with an exponent is a `float8`, one
   without a numeric (an error past 36 digits). This replaced
   the first answer, to keep each number's text as written.
5. **`sum(int8)` past 36 digits** is an error.

The answers of `decimal64` by value's review (§5), each with
the alternative it turned down:

6. **The types and their names**: `decimal64` and
   `decimal128`, `numeric` and `decimal` names only (§2.1).
   Earlier drafts kept `numeric` a type: first the 16-byte one
   beside a new `decimal64`, then the 8-byte one beside a new
   `decimal128`.
7. **Arithmetic**: a `decimal128` result, by native
   `__int128` paths (§2.5). A `decimal64` result, fully by
   value, fails past 17 digits where PostgreSQL would not, and
   most of its speed comes from native integer arithmetic,
   which the 128-bit paths have anyway.
8. **A bare `numeric`**: a `decimal64` with no typmod, its
   value keeping its scale (§2.1). It was first `decimal64(17,
   3)` everywhere, a cast included, until `x::numeric` rounding
   to three places failed PostgreSQL's `int2`, `int4` and
   `int8` (`WORKNOTE.md` step 256). Making it a `decimal128`
   outside a column, as PostgreSQL's unbounded numeric, was
   the alternative.
9. **The name a user sees**: `numeric(p, s)` for both types,
   so tools that look for `numeric` keep working.
   `decimal64(p, s)` and `decimal128(p, s)`, true to the types
   but unknown to tools, was the alternative.
10. **Overload resolution**: a `decimal64` taken as a
    `decimal128` counts as a match (§2.4); else a `decimal64`
    would find `float8`'s functions.
11. **`int8` with `decimal64`**: no common type, and a user
    casts (§2.4). Widening to `decimal128`, or an implicit
    `int8` to `decimal64` that fails past 17 digits, were the
    alternatives.

## 5. decimal64 by value: built

Most numerics fit in 17 digits, so a `numeric(p <= 17)` is a
by-value `decimal64`: no palloc and no conversion at the sm
border, sorts compare `datum1` itself, and spill files and
hash tables shrink. Before it, every numeric read from sm was
converted and palloc'd, 32 bytes behind a pointer, and every
sort, hash, join key and group key on it chased that pointer.
A `decimal128` laid out as sm's makes the wide case a pointer
into sm's tuple, with no conversion either.

It keeps the scale in the datum, as sm keeps it
(`sm/sm2/TYPE.md` §5.2), which costs a digit of
`decimal64` and two of `decimal128`. Carrying `(p, s)` through
every expression, as DuckDB does, and an abbreviated sort key
were the other options. §2 is what it made, and §4's answers 6
to 11 its review.

Where it lives:

- `src/include/utils/numeric.h`: both layouts,
  `NUMERIC_MAX_PRECISION` 36, an accessor for each.
- `src/backend/utils/adt/decimal.c`: both types' in, out,
  recv, send, typmod, comparison, hash, sort support and
  casts; `decimal64`'s native `+`, `-`, `*`, `/`, `sum` and
  `avg`; `decimal128`'s 128-bit fast path before the 512-bit
  wide one.
- `src/include/catalog/`: the types, functions, operators,
  casts, opclasses and aggregates; what was `numeric` there is
  `decimal128`'s, but for what §2.8 runs on `decimal64`.
- `src/backend/parser/parse_type.c` and `parse_node.c`: the
  names and literals of §2.1 and §2.4; `parse_func.c`, the
  overload rule.
- `src/backend/access/sm/smstore.c`: sm's code 17 is
  `decimal64`, code 18 `decimal128`; the border copies.
- `src/backend/access/common/printtup.c` and
  `src/backend/utils/adt/format_type.c`: what a client sees.
