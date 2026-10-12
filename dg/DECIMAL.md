# Decimal in digres

PostgreSQL's `numeric` is replaced by a fixed-width decimal of
at most 38 digits, the one sm stores (`../sm/sm2/TYPE.md`
§3.4). Reviewed; §4 records the answers. Not yet built.

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

### 2.1 One SQL type, still called numeric

- **The type keeps PostgreSQL's oid, 1700, and its name,
  `numeric`, with `decimal` its alias**, as in PostgreSQL. What
  changes is its representation and its range, not its name: a
  client, a driver or a query that says `numeric` is unchanged.
- **Its text is PostgreSQL's**, and so is its binary wire
  format (`numeric_send`, `numeric_recv`), which drivers decode:
  the value is converted to PostgreSQL's base-10000 digits on
  the way out, and from them on the way in.

### 2.2 A value carries its scale

PostgreSQL's executor does not carry a typmod through an
expression: `a * b`, a function's result, an aggregate's are
typmod -1. A scaled integer means nothing without its scale,
so the in-memory value carries it:

```
typedef struct decimal_t {
  int128 value;   /* the number times 10^scale */
  int16  scale;   /* 0 to 38 */
  int16  pad[7];  /* to 32 bytes, zero */
} decimal_t;      /* pass by reference, typlen 32 */
```

- **Fixed length, by reference**, 32 bytes: no varlena header,
  no detoasting.
- **The precision is not kept**: a value's digits are what
  they are, at most 38; a column's or a cast's precision is a
  check, as PostgreSQL's typmod is.
- **Equal values compare equal at any scale**: `1.5 = 1.50`.
  Comparison rescales the lower scale up (at most 38 digits,
  so it fits in 128 bits after a check) and compares the
  integers; hashing first strips trailing zeros, so equal
  values hash alike.

### 2.3 Stored as sm2's decimal, at the column's scale

- **A column is `numeric(p, s)`**: `p <= 18` is sm2's
  `decimal64`, `p <= 38` its `decimal128`. Its typmod is
  PostgreSQL's, which sm2 takes as is.
- **Writing rescales to the column's scale**, rounding half
  away from zero as PostgreSQL's assignment does, and checks
  the precision; reading attaches the column's scale. Both are
  integer work at the sm boundary, in `smstore.c`: a row's
  insert and update, its deform, an index scan's keys, an
  index-only scan's values, and statistics' values.
- **An unconstrained `numeric` column is `numeric(18, 3)`**,
  DuckDB's default for a bare `DECIMAL`, as sm2 requires a
  typmod (§4.1). A value with more places is rounded to three,
  as an assignment to `numeric(18, 3)` is.

### 2.4 Literals and casts

- **A literal with a point or an exponent is `numeric`** with
  the scale it is written with: `1.50` has scale 2, `1e3`
  scale 0. More than 38 significant digits is an error, not a
  `float8` as DuckDB makes it, which would drop digits unasked
  (§4.2).
- **Casts**: from and to the integers, `float4`, `float8`,
  `text`, `money`; `numeric(p, s)` rescales and checks, as
  PostgreSQL's typmod coercion does.

### 2.5 Arithmetic

Results are exact or an error, never silently wrong.

- **`+`, `-`**: scale `max(s1, s2)`.
- **`*`**: scale `s1 + s2`; past 38 digits, rounded to fewer
  places where the integer part still fits, else an error.
- **`/`**: PostgreSQL's choice of scale (`select_div_scale()`,
  at least 16 significant digits, at most 38 in all), rounded.
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

- **`sum(int8)`, `sum(numeric)`** accumulate in 128 bits:
  exact to 38 digits, then an error, where PostgreSQL never
  overflows (§4.5).
- **`avg` of integers and numerics** divides by the count at
  `/`'s scale (§2.5).
- **`stddev`, `variance`** of them: in `long double`.

### 2.7 What else used numeric

Each is moved to `decimal_t`, keeping its SQL signature:

- `extract()`, `date_part` (`timestamp.c`, `date.c`);
- jsonb's numbers and jsonpath's arithmetic, as DuckDB's JSON
  does (§4.4): jsonb keeps each number's text as written, so a
  document is stored whole, `1e400` and a 50-digit integer
  included; a number taken as a value -- `->>` cast, a jsonpath
  comparison or arithmetic -- is a decimal if it fits in 38
  digits, and an error if not;
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
   DuckDB's `DECIMAL(18, 3)`.
2. **A literal past 38 digits** is an error.
3. **`sqrt`, `ln`, `exp`, `power`** in `long double`, some 33
   digits, are enough.
4. **jsonb** does as DuckDB's JSON: DuckDB's JSON is text, so a
   number is kept as written, and extracting one gives a
   `BIGINT`, `UBIGINT` or `DOUBLE` as fits, a `DECIMAL` cast
   failing when it does not. Here a number is kept as written,
   and is a decimal when taken as a value, an error past 38
   digits.
5. **`sum(int8)` past 38 digits** is an error.
