# Decimal in digres

PostgreSQL's `numeric` is replaced by two fixed-width
decimals, the ones sm stores (`../sm/sm2/TYPE.md` §3.4):
`decimal64`, of at most 17 digits, passed by value, and
`decimal128`, of at most 36; `numeric` is a name for them.
Reviewed; §4 records the answers. All of §3 is built
(`WORKNOTE.md` steps 95 to 97), and §5 by
`DG_DECIMAL_PLAN.md`, which holds its decisions in full.

## 1. Where digres stands

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
  18 digits (`decimal64`), 16 up to 38 (`decimal128`), with
  `(p, s)` in the schema, not in the datum, and no NaN or
  infinity. So a table cannot have a `numeric` column today.

## 2. Decisions

### 2.1 Two types, and numeric a name for them

- **`decimal64` and `decimal128` are the types**, sm's two
  (`DG_DECIMAL_PLAN.md` §3.1): `decimal64` takes PostgreSQL's
  oid for numeric, 1700, `decimal128` 8383.
- **`numeric` and `decimal` are names only**, resolved where a
  type name is (`parse_type.c`): a bare one is
  `decimal64(17, 3)`, everywhere, a cast included;
  `numeric(p, s)` is `decimal64` up to 17 digits, `decimal128`
  up to 36, and refused past them.
- **A client sees `numeric`**: oid 1700 for either type, 1231
  for an array of either, and `numeric(p, s)` as its name, so a
  client, a driver or a query that says `numeric` is unchanged
  but for a bare `numeric`'s scale. A parameter a client types
  1700 is a `decimal128`.
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
  its width (`../sm/sm3/SM3_DESIGN.md` §2.1): `p <= 17` is
  sm2's `decimal64`, `p <= 36` its `decimal128`, whose datums
  carry their scale below their value
  (`../sm/SM_DECIMAL_PLAN.md`).
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
  (§4.1); so is any bare `numeric` now (§2.1). A value with
  more places is rounded to three, as an assignment to
  `numeric(17, 3)` is.

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
  PostgreSQL's typmod coercion does. `decimal64` widens to
  `decimal128` implicitly, `int2` and `int4` to either, `int8`
  only to `decimal128`: `int8` and `decimal64` have no common
  type, and a user casts (`DG_DECIMAL_PLAN.md` §3.4).
- **A `decimal64` finds `decimal128`'s functions**, where it
  has none of its own, before `float8`'s, the numeric
  category's preferred type (`parse_func.c`,
  `DG_DECIMAL_PLAN.md` §3.2).

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

## 3. Order of work

1. **The type**: `decimal_t`, in and out, send and receive,
   comparison and hashing, the btree and hash opclasses, casts.
   `numeric.c` is replaced by `decimal.c`.
   → verify: a regression file of PostgreSQL's own numeric
   tests, trimmed to 38 digits, gives PostgreSQL's output.
2. **Arithmetic and functions** (§2.5).
   → verify: the same file's arithmetic and functions; the
   transcendental ones agree to their scale.
3. **Aggregates** (§2.6).
   → verify: sums and averages of integers and decimals,
   overflow past 38 digits refused.
4. **Columns** (§2.3).
   → verify: `server.sh` stores `numeric(10, 2)` and
   `numeric(30, 5)`, indexes and keys them, rescales on insert,
   and refuses an unconstrained column.
5. **The other users** (§2.7), jsonb last.
   → verify: each one's results in `server.sh`.
6. `numeric.c` is gone.

## 4. Answers of the review

1. **An unconstrained `numeric` column** is given a default,
   DuckDB's `DECIMAL(18, 3)`; `(17, 3)` since sm's decimals
   carry their scale (`../sm/SM_DECIMAL_PLAN.md`).
2. **A literal past 38 digits** was to be an error; building
   it showed that this refuses `1e300` as a float, so it is a
   `float8`, as in DuckDB. Text cast to numeric past 38 digits
   is still an error.
3. **`sqrt`, `ln`, `exp`, `power`** in `long double`, some 33
   digits, are enough.
4. **jsonb**: a number with an exponent is a `float8`, one
   without a numeric (an error past 38 digits). This replaced
   the first answer, to keep each number's text as written.
5. **`sum(int8)` past 38 digits** is an error.

## 5. decimal64 by value: built

Most numerics fit in 17 digits, so a `numeric(p <= 17)` is a
by-value `decimal64`: no palloc and no conversion at the sm
border, sorts compare `datum1` itself, and spill files and
hash tables shrink. It was built by this section's option 2,
the scale kept in the datum, as sm keeps it
(`../sm/SM_DECIMAL_PLAN.md`), which costs a digit of
`decimal64` and two of `decimal128`. Carrying `(p, s)` through
every expression, as DuckDB does, and an abbreviated sort key
were the other options. `DG_DECIMAL_PLAN.md` holds the design
and its answers; §2 here is what it made.
