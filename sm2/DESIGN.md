# DESIGN — sm2, types and tuples

The level that gives sm1's opaque byte strings a type and a
shape. It is three things and no more: a **type system**, the
two on-disk **encodings** those types get, and a **relation
whose rows are tuples** instead of raw bytes. Everything a
schema needs above that — a catalog, secondary indexes,
predicate scans, statistics — belongs to sm3 and is argued in
`sm3/DESIGN.md`.

This document is the design of record; it follows the
decisions in `OBJECTIVE.md`:

| Decision | Choice |
| --- | --- |
| Types | a fixed set, each an ADT in `pg_type`'s shape, each with a stable on-disk type code |
| Representation | PostgreSQL's, type by type, little-endian — except `number`, which is Arrow's decimal |
| Key encoding | order-preserving per type; descending is the inverted encoding |
| Tuple format | null bitmap, fixed/variable split, a schema version stamp per row |
| Tuple codec | an API of its own, with its own header and its own tests |
| Row identity | the primary key, clustered, as sm1 relations already are |
| `rel` | one sm1 relation, keys encoded and tuples decoded by sm2 |
| API shape | one tuple at a time; batching belongs where filters do (`sm3/DESIGN.md`) |
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

sm2 is written against the API in `sm1/include/sm.h`. The
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
| relations as ordered key→value maps, clustered on the key | keys are ordered **bytewise only**; there is no comparator to install (`sm1/OBJECTIVE.md`) — so order has to be built into the encoding (§3) |
| opaque values of arbitrary bytes | the tuple layout is sm2's alone; sm1 never looks inside one (§4) |
| transactions at RC / RR / SERIALIZABLE | sm2 opens none of its own; every `rel` call runs inside the caller's transaction (§5) |
| `sm_update` / `sm_delete` returning the **old image** | `rel_update` / `rel_delete` can hand back the old *tuple*, which is what index maintenance above needs |
| `SM_ERR_DUPKEY` from `sm_insert` | a duplicate primary key needs no existence check; the error passes straight through |
| `SM_ERR_UPDATED` and `sm_read_latest` at READ COMMITTED | sm2 surfaces the error and offers `rel_read_latest`; it cannot resolve it, because the re-check needs a filter and filters are sm3's (§5) |
| nested savepoints | a statement can be undone without losing the transaction, and sm2 holds no state that has to be undone with it |
| forward scans with `sm_scan_seek` | probe loops cost one cursor, not one per probe |
| `SM_MAX_KEYLEN` 4096, `SM_MAX_DATALEN` 64MB | an encoded primary key must fit 4096 bytes, prefix included; a tuple above the chunking threshold is split (§4) |
| no backward scan, ever | a descending column is stored with inverted bytes and scanned forward; there are no mirror relations, and the fork's planner never asks for a backward scan (§3) |

One thing sm1 gives that sm2 deliberately does not use: the
one-column-family-per-relation model and its limit of hundreds
to low thousands of relations. An sm2 `rel` is exactly one sm1
relation. Fitting a whole SQL schema into that budget is sm3's
problem, and sm3 solves it with the key prefix in §5.

---

## 2. Types

**`TYPE.md` is the per-type reference** — every type's native
representation, its order and its key bytes, one section
each. This section argues the decisions behind it; that file
is what you read while writing code.

*To be written.* The rules it must satisfy:

- The set is fixed and chosen from what the PostgreSQL target
  needs first: boolean, the integer widths, `float4` and
  `float8`, `number`, `text` / `varchar` / `bytea`, `date`,
  `time`, `timestamp`, `timestamptz`, `interval`, `uuid`.
  Arrays, composites, ranges and user-defined types are not in
  the first set; the format must leave room to add them
  without changing what is already on disk.
- A type is three things together: an in-memory datum, an
  order-preserving key encoding (§3), and a tuple encoding
  (§4). A type is not added until it has all three.
- Every type has a **small stable code that goes on disk**, in
  the schema rather than in each row. Codes are assigned once
  and never reused, so a type removed from the set does not
  free its number.
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
type by type, with `number` the one exception.** Not a format
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
| `number` | **Arrow's**, not PG's — see §2.2 | 16 or 32 |
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
  framing is the tuple's business (§4) — sm2 has its own, and
  its own out-of-line chunking, which is not TOAST.

### 2.2 Except number, which is Arrow's decimal

**Decision: `number` is Arrow's fixed-width decimal.** PG's
numeric is a varlena of base-10000 digits with a weight and a
display scale, effectively unbounded in length. Arrow's is a
two's-complement integer of fixed width with the scale held in
the schema: 128 bits for a precision up to 38, 256 bits up to
76. Three things follow, and they are why the exception is
worth making:

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

- **Precision stops at 38 or 76 digits.** PG allows 1000 for a
  declared typmod, and far more for an undeclared one. A
  column that needs more has no representation at all.
- **There is no NaN and no infinity.** PG's numeric has had
  NaN throughout and `Infinity` since version 14 (from memory,
  not checked against the source); an Arrow decimal is an
  integer and has neither, so sm2 refuses them on insert.
- **The scale must be declared.** Arrow puts `(precision,
  scale)` in the schema; PG allows a bare `numeric` where
  every datum carries its own. A bare `number` column has no
  Arrow equivalent, so it defaults to `number(38, 10)`, which
  can round a datum its user believed exact.
- **It is the one type that is converted** at the executor
  boundary, in both directions, while §2.1 makes every other
  type a copy.

The NaN refusal and the bare `number` default are decided in
`OBJECTIVE.md`.

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
| `recv` / `send` | binary form over the wire — **deferred**: the slots exist in v1, unfilled, as sm2 has no wire protocol (`QUESTIONS.md` 2.5) | `typreceive` / `typsend` |
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
and 24 hours to the day, computed in 128 bits (the function's
name and shape are from memory, not checked against the
source; the 128 bits are arithmetically necessary — over the
full `int32`/`int64` component ranges the count reaches about
5.8e21, well past `int64`). That order is deliberately **not
injective**: `'1 mon'` and `'30 days'` both flatten to
2592000000000, and PG's btree calls them equal.

Equal keys are exactly what a primary key cannot have, so §3
says what an interval key is.

Open: the earlier question of how to represent `number` is
settled by §2.2, and whether `interval` is in the first set is
settled here — it is. Bare `number` and `number`'s NaN are
decided in `OBJECTIVE.md`.

---

## 3. Key encoding

The per-type encodings are in `TYPE.md` §3; the rules they
all have to satisfy are here.

*To be written.* The rules it must satisfy:

- `encode(a) < encode(b)` bytewise **iff** `a < b` in the
  type's own order, for every supported type.
- **Injective, not reversible.** Distinct datums must get
  distinct keys, because a key is the row's identity: two
  rows that collide are one row, and a unique constraint
  that cannot tell them apart is not a constraint. But the
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
  truncated secondary-index key of `OBJECTIVE.md`, the entry
  is flagged and rechecked against the base row, which is a
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
- **number** needs nothing bespoke, because §2.2 made it a
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
  that is persisted, once schemas are, is `sm3/DESIGN.md` §2.
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
  against `SM_MAX_KEYLEN` and is the arithmetic behind the
  over-long-key question in `OBJECTIVE.md`; it must be called
  as `strxfrm_l` with an explicit `locale_t` rather than
  through `setlocale`; and glibc's collation is not stable
  across versions, so a stable key means pinning ICU and
  using `ucol_getSortKey` — which is what the
  collation-stability answer in `OBJECTIVE.md` settles on.
- The ban on backward scans stands, and the planner is changed
  not to ask for one (`PART4.md` §5). A DESC column is
  therefore the inverted encoding scanned forward, and an
  index-ordered `ORDER BY x DESC LIMIT n` becomes a top-N sort
  over a forward scan (`sm3/DESIGN.md` §7).

This encoder is sm2's, but sm3 is its other caller: a
secondary index entry is `encode(sk) ‖ encode(pk)`
(`sm3/DESIGN.md` §3), built from the same function with the
same per-column arguments. It is one encoder with two callers,
not two encoders that have to agree.

Decided in `OBJECTIVE.md`: an over-long primary key refuses
the row and an over-long secondary key is truncated and
rechecked, never hashed; ICU is pinned and a version mismatch
is refused until an explicit rebuild; non-deterministic
collations are refused, with the `deterministic` flag carried
from the start.

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
  in its index entries (`sm3/DESIGN.md` §3) and decodes them
  with this same codec, without a base relation in sight.
- **It is the only part of sm2 that does not touch sm1.** A
  codec with no database under it is testable in a unit test
  with no database in it.

**The datums are PostgreSQL's; the framing is sm2's.** §2.1
fixes what each column's bytes are, and stops there. The
tuple around them — the null bitmap, the fixed/variable
split, the alignment, the schema version stamp, the offsets —
owes `HeapTupleHeader` nothing and is designed here on its own
merits. This is also where sm2's out-of-line chunking lives,
which is not TOAST: a `text` datum in an sm2 tuple is payload
bytes with its length in the framing, with no varlena header
and no TOAST pointer (§2.1).

*To be written.* The rules the format itself must satisfy:

- A null bitmap, a fixed/variable split, and enough alignment
  discipline that reading a column does not require decoding
  every column before it.
- A **schema version stamp per row**, so `ADD COLUMN` is O(1):
  an old row is read under its own layout and the missing
  column comes from its default. The stamp is a number; what
  it resolves to is a schema, which sm2 is handed (§5) and
  sm3 persists (`sm3/DESIGN.md` §2).
- Tolerance for dropped columns, since the column at a given
  position may no longer exist.
- **Cheap to turn into a PostgreSQL tuple.** The executor
  works in tuple slots, so a bespoke encoding pays a
  conversion on every row, in both directions. The threaded
  fork removes the IPC boundary, not the conversion.
- Large datums are **chunked out of line** above a threshold.
  sm1 permits a 64MB value, but a transaction's undo, lock and
  read sets are linear in the keys it touches
  (`sm1/DESIGN.md` §13), and very large datums hurt LSM
  compaction, so the threshold is well below sm1's ceiling.

---

## 5. The tuple relation

*To be written.* sm1's relation is an ordered map of byte
string to byte string. sm2's is that relation with the types
put back: a row goes in as a tuple and comes out as a tuple,
and §3 and §4 happen inside. It is the whole of sm2's API
surface; §2 to §4 are what it is built out of.

A `rel` is opened over exactly one sm1 relation, with three
things:

- a **schema** — the ordered columns, their types and
  collations, which prefix of them is the primary key, and per
  key column its direction and null ordering;
- a **key prefix**, opaque to sm2, prepended to every key it
  writes and stripped from every key it reads. sm2 has no use
  of its own for it. It exists so sm3 can multiplex many
  logical tables onto one sm2 relation (`sm3/DESIGN.md` §4)
  without sm2 having to know what a table is;
- an **sm1 session**, which is where the transaction lives.

The operations, and what each one is over:

| sm2 | what it adds | over |
| --- | --- | --- |
| `rel_insert` | encodes the key columns, writes the rest as a tuple | `sm_insert`; `SM_ERR_DUPKEY` passes through |
| `rel_read` | decodes sm1's byte string into a tuple | `sm_read` |
| `rel_update`, `rel_delete` | hands back the old image as a **tuple**, not bytes | `sm_update`, `sm_delete` |
| `rel_read_latest` | the newest committed image, decoded | `sm_read_latest` |
| `rel_scan`, `rel_scan_seek` | tuples in key order, forward only | `sm_scan`, `sm_scan_seek` |

The rules it must satisfy:

- **sm2 is a codec, not a state machine.** Every durable
  effect is an sm1 call inside the caller's transaction, and
  sm2 keeps nothing across calls that a rollback would have to
  undo. This is what makes savepoints and abort cost sm2
  nothing at all.
- **Errors pass through; they are not translated.**
  `SM_ERR_UPDATED` in particular is not retried here. The
  retry needs the statement's filter re-checked against the
  newest image (`sm1/DESIGN.md` §5.4), and filters live above
  sm2; `rel_read_latest` is what sm2 provides so sm3 can do it
  (`sm3/DESIGN.md` §3).
- **Every column is in the tuple, key columns included.**
  The encoded key is not the storage of tuple for anything;
  it is an ordering device that happens to be unique. A key
  column is therefore written twice — encoded in the sm1 key,
  native in the tuple — and the tuple is what a read
  returns.

  The alternative, leaving key columns out of the tuple and
  reconstructing them from the key, saves the duplicate but
  costs more than it saves. It makes `key_encode` reversible
  by obligation, for every type, forever — which is a hard
  constraint to hold over collation sort keys, truncated
  keys and any future lossy encoding. It makes decoding a row
  a stitch of two sources instead of one call, so the tuple
  codec (§4) would need to know which ordinals are key
  columns and the `rel` layer would need to splice them back
  in on every read and every scan. Uniformity is worth the
  bytes; the cost is named in §7.
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
(`sm3/DESIGN.md` §5).

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
- **Object mapping.** Many logical tables in one sm1 relation
  is sm3's, through sm2's key prefix (§5).
- **Predicates.** sm2 has no filter, and therefore cannot
  resolve `SM_ERR_UPDATED` or derive a key range from a
  `WHERE` clause.
- **Statistics.** sm2 counts nothing.

---

## 7. Known trade-offs

*To be filled in as the design settles, in the manner of
`sm1/DESIGN.md` §13 — the costs this design accepts, written
down where the next reader will find them.*

Accepted so far:

- **Collated keys are stored twice.** The sort key is lossy,
  so the original datum is stored beside it (§3).
- **A tuple is not a PostgreSQL tuple — but its columns are
  PostgreSQL datums.** §2.1 makes the conversion at the
  executor boundary per tuple, not per datum: the framing is
  rebuilt, the column bytes are copied. The threaded fork
  removed the IPC boundary but not this (`PART4.md` §2).
- **number is the exception, and pays for it.** It is
  Arrow's decimal, not PostgreSQL's (§2.2), so it is the one
  type converted datum by datum, it caps at 38 or 76 digits
  where PG allows 1000, and it has no NaN and no infinity.
  Bought in exchange for a `number` key encoding that is
  an integer's, and a fixed-width `number` column.
- **Little-endian is in the format.** §2.1 fixes the byte
  order that PostgreSQL leaves native, so a big-endian host
  byte-swaps on every datum where others `memcpy`.
- **Key columns are stored twice**, encoded in the sm1 key
  and native in the tuple (§5). A table whose columns are
  all primary key pays for its data twice, and the loss is
  worst exactly where the row is narrow. It buys a tuple
  that is complete on its own, a tuple codec that never
  learns what a key column is, and a `key_encode` that is
  allowed to be lossy. CockroachDB and YugabyteDB make the
  other choice and reconstruct key columns from the key.
- **A collated text key column is stored three ways**: the
  sort key and the tie-break original in the key (§3), and
  the datum in the tuple. The tie-break is not optional —
  without it two distinct strings can share one key — so
  this is the price of collated text as a primary key, and
  it is a reason to prefer a surrogate key for one.
- **Descending order costs an encoding, not a structure.**
  Inverted bytes scanned forward means no mirror relation and
  no second copy, but it also means a column's direction is
  fixed when the key is written: changing `ASC` to `DESC`
  rewrites the keys.
- **The type set is closed at the format.** New types are
  cheap, but only while the type code and the tuple layout
  leave room for them; that room is spent once.
