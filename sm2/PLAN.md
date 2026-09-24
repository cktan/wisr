# PLAN — sm2

The order of work, and why it is this order. `SM2_OBJECTIVE.md`
says what sm2 is for; `SM2_DESIGN.md` is where each step's
argument goes before its code does; `QUESTIONS.md` is what
still has to be answered before any of it can be written, step
by step. `sm3/PLAN.md` picks up where this one stops.

The principle: **the two on-disk formats come first and get
the most scrutiny.** A key encoding and a tuple layout are
permanent the moment any data exists, and neither can be fixed
later by a patch. Everything else in sm2 — and everything in
sm3 — is code that can be rewritten.

## Source layout

sm2 mirrors sm1: nothing about its build deserves novelty.

```
sm2/
  Makefile      the same targets as sm1's; sm2 joins LEVELS
  include/      the public header (`QUESTIONS.md` 5.1)
  src/type/     steps 1 and 2: the ADT table, each type's
                representation, and its key encoding
  src/ryu/      PostgreSQL 18's float printers, copied verbatim
  src/tuple/    step 3: the tuple codec
  src/rel/      step 4: the tuple relation
  test/         doctest, as in sm1
```

The key encoding lives in `src/type`, not in a directory of
its own: `key_encode` is one of a type's ADT functions
(`SM2_DESIGN.md` §2.3), so a type's representation and its
key sit side by side, and a multi-column key is composed from
them. Dependencies run one way — `rel` calls `tuple`, `tuple`
calls `type` — and only `src/rel` calls sm1's storage
functions. Every directory may include `sm1/include/sm1.h`
all the same, since sm2 reports its failures through sm1's
error state (`SM2_DESIGN.md` §5); `src/type` does already.

## 1. Types

The vocabulary the two formats are written in, so it comes
before them.

- The first set: boolean, the integer widths, `float4`,
  `float8`, `number`, `text` / `varchar` / `bytea`, `date`,
  `time`, `timestamp`, `timestamptz`, `interval`, `uuid` —
  each one spelled out in `TYPE.md` §3.
- **The ADT table first** (`SM2_DESIGN.md` §2.3): one descriptor
  of function pointers per type, so that every step after
  this one adds a descriptor instead of a `switch`.
- PostgreSQL's representation per type (`SM2_DESIGN.md` §2.1),
  little-endian, with variable-length datums carrying the
  payload alone.
- `number` as Arrow's decimal (`SM2_DESIGN.md` §2.2), and
  interval as PG's three components (`SM2_DESIGN.md` §2.4).
- A stable on-disk type code per type, assigned once.
- Null and collation as properties of a column, not of a type.
- The `number` rules in `SM2_DESIGN.md` §2.2: only
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
- The key rules in `SM2_DESIGN.md` §3.1–§3.3: a null marker on
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
(`SM2_DESIGN.md` §3): the key round trips, and its order refines
PostgreSQL's — never contradicting it where PG is decisive,
and separating the datums PG calls equal.

## 3. Tuple format, and the tuple codec

Its own API, with its own header, built before the relation
that calls it (`SM2_DESIGN.md` §4).

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
  (`SM2_DESIGN.md` §5).
- Rows keyed by `rowno`, an unsigned 64-bit number from the
  table's sm1 sequence, kept across updates (`SM2_DESIGN.md` §5).
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

**Property tests are hand-rolled**, with no new dependency.
Each type has a generator weighted toward small values and its
boundaries. A property runs every pair of boundary values
first, then N random pairs, from a fixed default seed that the
environment variable `WISR_SEED` overrides; a failing property
prints its seed through doctest, so any failure replays exactly.
There is no shrinking: size-biased generation keeps failures
small, and a simple shrink for byte strings is added only if a
failure proves hard to read.

**The PostgreSQL fixtures are observed on disk, once**, on
PostgreSQL 18. For every type but `number`,
`test/pg_fixtures.h` holds each fixture value with the bytes
PostgreSQL stores for it, read through the
`pageinspect` extension: a one-column table per type, the values
inserted, and each tuple's data bytes taken from
`heap_page_items(get_raw_page(...))`, with the varlena header
stripped, since sm2 keeps the payload alone. The values are the
generators' boundaries plus representative ones — multibyte
UTF-8, the empty string, timestamps at 1970 and at PostgreSQL's
2000 epoch. The script that produces the header is checked in
beside it, and the header records the PostgreSQL version and
the date it was generated. It runs on a developer's machine
with a PostgreSQL server and superuser rights; the build never
touches PostgreSQL. The bytes hold on little-endian hosts only,
which the fixture test asserts.

The floats add 2,500 seeded random values each, since
PostgreSQL's float text is not always the shortest decimal:
2,000 random finite bit patterns and 500 integers just past
the width's exact range, where most of the exceptions lie.

The fixtures are generated on the development machine, from
PostgreSQL 18.6 built from source: tag `REL_18_6`, cloned into
`~/p/postgres18` and installed under `~/pg18` with
`pageinspect`, its data directory at `~/pg18/data` with
superuser `postgres` and trust authentication. The server runs
only for a fixture run:

```
~/pg18/bin/pg_ctl -D ~/pg18/data -l ~/pg18/server.log \
    -o "-p 5418 -k $HOME/pg18" -w start
~/pg18/bin/psql -h ~/pg18 -p 5418 -U postgres
~/pg18/bin/pg_ctl -D ~/pg18/data -w stop
```

## What is deliberately not here

- No catalog, no secondary index, no predicate scan, no
  statistics, and no choice of which prefix means what.
  All of it is `sm3/PLAN.md`.
- No SQL, no planner, no executor.
- No PostgreSQL dependency. `PART4.md` explains what the PG
  target forces on the decisions above; none of it requires a
  PostgreSQL source tree to build or test sm2.
