# PLAN — sm2

The order of work, and why it is this order. `OBJECTIVE.md`
says what sm2 is for; `DESIGN.md` is where each step's
argument goes before its code does; `QUESTIONS.md` is what
still has to be answered before any of it can be written, step
by step. `sm3/PLAN.md` picks up where this one stops.

The principle: **the two on-disk formats come first and get
the most scrutiny.** A key encoding and a tuple layout are
permanent the moment any data exists, and neither can be fixed
later by a patch. Everything else in sm2 — and everything in
sm3 — is code that can be rewritten.

## 1. Types

The vocabulary the two formats are written in, so it comes
before them.

- The first set: boolean, the integer widths, `float4`,
  `float8`, `number`, `text` / `varchar` / `bytea`, `date`,
  `time`, `timestamp`, `timestamptz`, `interval`, `uuid` —
  each one spelled out in `TYPE.md` §3.
- **The ADT table first** (`DESIGN.md` §2.3): one descriptor
  of function pointers per type, so that every step after
  this one adds a descriptor instead of a `switch`.
- PostgreSQL's representation per type (`DESIGN.md` §2.1),
  little-endian, with variable-length datums carrying the
  payload alone.
- `number` as Arrow's decimal (`DESIGN.md` §2.2), and interval
  as PG's three components (`DESIGN.md` §2.4).
- A stable on-disk type code per type, assigned once.
- Null and collation as properties of a column, not of a type.
- The `number` rules in `DESIGN.md` §2.2: only
  `number(p, s)`, a bare `number` is `number(38, 10)`, and NaN
  and infinity are refused.

**Verify:** for every type, a datum round trips through the
in-memory representation, and the type's own comparison
function agrees with hand-written expectations at the
boundaries (min, max, zero, negative zero, NaN, empty
string). For every type but `number`, a datum's bytes equal
what PostgreSQL stores for the same datum — a table of
fixtures taken from PG, which is the only way "the
representation is PostgreSQL's" is a claim rather than an
intention.

## 2. Key encoding

The foundation. sm1 orders keys bytewise only, so every
ordered thing above rests on this, including sm3's indexes.

- Order-preserving encodings per type: integers (sign-flipped
  big-endian), floats (IEEE-754 total order), `number`,
  timestamps, text (collation sort key).
- Multi-column keys, ascending and descending per column,
  nulls first or last.
- The key rules in `DESIGN.md` §3.1–§3.3: a null marker on
  every column, refuse an over-long primary key,
  truncate and flag an over-long secondary key, refuse a
  mismatched collation version, and carry the `deterministic`
  flag while refusing non-deterministic collations.

**Verify:** property-based tests, not examples. For every type
and every column order, `encode(a) < encode(b)` iff `a < b`,
over generated datums including the boundaries (min, max, zero,
negative zero, NaN, empty string, the longest legal key). Round
trip where the encoding is reversible, and prove the documented
loss where it is not. Interval gets two properties of its own
(`DESIGN.md` §3): the key round trips, and its order refines
PostgreSQL's — never contradicting it where PG is decisive,
and separating the datums PG calls equal.

## 3. Tuple format, and the tuple codec

Its own API, with its own header, built before the relation
that calls it (`DESIGN.md` §4).

- `tuple_encode` and `tuple_decode`, the latter by ordinal so
  one column costs one column.
- Null bitmap, fixed/variable split, alignment.
- Schema version per row, so `ADD COLUMN` is O(1) and a dropped
  column does not shift the ones after it.
- TOAST for large datums, below sm1's 64MB ceiling.

**Verify:** a row written under schema version N reads correctly
under N+1, N+2 and after a column drop; the golden-bytes test
that a tuple written today still decodes tomorrow. All of it
in a unit test with no database in it — the codec touches sm1
nowhere, which is half the reason it is a separate API.

## 4. The tuple relation

The three above, joined to sm1.

- `rel_insert`, `rel_read`, `rel_update`, `rel_delete`,
  `rel_read_latest`, `rel_scan`, `rel_scan_seek`, over the
  codec of step 3 rather than a second copy of it.
- The 2-byte object prefix, chosen by sm3, opaque to sm2
  (`DESIGN.md` §5).
- Rows keyed by `rowno`, an unsigned 64-bit number from the
  table's sm1 sequence, kept across updates (`DESIGN.md` §5).
- Errors from sm1 passed through, not translated —
  `SM_ERR_UPDATED` in particular.
- Per-column decoding, so a reader that wants one column does
  not pay for the row.

**Verify:** for generated schemas and rows, what goes in comes
back out, under every isolation level; the old image returned
by an update or a delete is the tuple that was there; a
savepoint rollback leaves no sm2 state behind, because there
is none to leave; an update keeps its `rowno`; two `rel`s over
the same sm1 store with different prefixes never see each
other's rows.

---

## Testing

sm1's two-layer arrangement is worth copying, because it
earned its keep: C++ tests for contract and single-session
mechanics, and a declarative scenario suite for anything where
two sessions interleave. Most of sm2 is the first kind — a
codec is tested by property, not by interleaving — and the
concurrency questions that need the second kind mostly arrive
with sm3 (`sm3/PLAN.md`).

Whether sm2 gets its own driver or extends sm1's is a decision
for the first scenario that needs it, not before.

## What is deliberately not here

- No catalog, no secondary index, no predicate scan, no
  statistics, and no choice of which prefix means what.
  All of it is `sm3/PLAN.md`.
- No SQL, no planner, no executor.
- No PostgreSQL dependency. `PART4.md` explains what the PG
  target forces on the decisions above; none of it requires a
  PostgreSQL source tree to build or test sm2.
