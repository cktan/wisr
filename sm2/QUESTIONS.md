# QUESTIONS — sm2, before any code

What has to be decided before sm2 can be written, gathered in
one place so none of it is discovered halfway through a format
that is already on disk.

Two kinds of entry, and the difference matters:

- **[BLOCKING]** — work stops until it is answered, or the
  answer changes bytes that are already written.
- **[ASSUMABLE]** — there is a sane default, named here as
  the suggestion. Silence is taken as agreement, and the
  answer is recorded in the document named at the end of the
  entry.

Ordered by the step of `PLAN.md` each one blocks. §1 is first
because it is the only section where the documents already
contain an answer that nobody has agreed to.

---

## 1. Ratify the five suggested answers

`OBJECTIVE.md` carries five open questions with a *suggested*
answer written under each. A suggestion is not a decision, and
four of the five change what goes on disk. **These need a yes
or a no, one at a time.**

| # | Question | Suggested | Changes disk? |
| --- | --- | --- | --- |
| 1.1 | Bare `numeric`, with no declared precision | refuse it at column creation | no — a later type code can add it |
| 1.2 | numeric NaN and ±Infinity | reserve the extreme magnitudes as sentinels | **yes** |
| 1.3 | Over-long keys against `SM_MAX_KEYLEN` | reject for a primary key, truncate and recheck for a secondary index, never hash | **yes** |
| 1.4 | Collation version drift | pin ICU, refuse on mismatch, rebuild by command | **yes** — the recorded version |
| 1.5 | Non-deterministic collations | refuse in v1; carry the flag from the start | **yes** — the tie-break rule |

1.2 is the one that cannot wait. Retrofitting a sentinel
changes the meaning of numerics already written, so it is
decided before the first `numeric` is stored or not at all.

### Answers

Recorded here as given; `OBJECTIVE.md`, `DESIGN.md` and
`TYPE.md` are not yet updated to match.

**1.1 — No, take a default instead (2026-09-22).** A bare
`numeric` is accepted and stored as `numeric(38, 10)`: 28
integer digits and 10 fractional. A datum with more than 10
fractional digits is rounded to 10, as PostgreSQL rounds for a
declared `numeric(p,s)`. A datum whose integer part needs more
than 28 digits is an error. This overrides the suggestion to
refuse at column creation.

**1.2 — No, refuse them (2026-09-22).** A numeric NaN,
`+Infinity` or `-Infinity` is rejected on insert with a clear
error; no sentinel is reserved. The door stays open anyway: a
precision of 38 digits never reaches the 128-bit extremes, so
those integers are never written and could still be reserved
later without changing the meaning of stored bytes, provided
the codec keeps rejecting any value outside the declared
precision.

**1.3 — Yes (2026-09-22).** An encoded primary key over
`SM_MAX_KEYLEN` refuses the row. A secondary index key over
the limit has its sort key truncated to fit, the entry is
flagged as cut, and a scan rechecks against the stored
original; a unique check on a cut key compares originals over
the prefix range, following 3.5's rule for NULLs. Keys are
never hashed.

**1.4 — Yes (2026-09-22).** A specific ICU version is linked
into the server, so drift happens only on a deliberate
upgrade. The collation version is recorded with the schema
and compared at open. On a mismatch, every index and collated
primary key built under the old version refuses reads and
writes, with a message saying why. Rebuilding is an explicit
command: an online build for a secondary index, a table
rewrite for a collated primary key.

**1.5 — Yes (2026-09-22).** A non-deterministic collation is
rejected with a clear error when a column or index is
created. The schema carries a per-column `deterministic` flag
from the start, set at creation and never changed. A
deterministic collation's key is the sort key followed by the
original bytes as a tie-break; a non-deterministic one's is
the sort key alone. A later release can accept the second kind
without a format change.

---

## 2. Types — blocks `PLAN.md` step 1

**2.1 [BLOCKING] Are the type codes assigned now?**
`TYPE.md` §3 gives each type a code and calls the numbering
illustrative. The codes go in the schema and are never reused,
so they are permanent from the first tuple written.
*Suggested: freeze the numbering in `TYPE.md` §3 as it stands,
and leave a documented gap (say 64–127) for types added
later.* → `TYPE.md`

**2.2 [BLOCKING] Is `enum` in the first set?**
`FEATURELIST.md` §1, at the top level, marks enums supported;
`DESIGN.md` §2 does not list them. One of the two is wrong.
*Suggested: out of sm2's first set.* An enum's values live in
the catalog, which is sm3's, so the type cannot be closed over
inside sm2. It is `int4` on disk with sm3 holding the labels —
which is PostgreSQL's arrangement too. → `DESIGN.md` §2,
`FEATURELIST.md`

**2.3 [ASSUMABLE] Why are `text` and `varchar` separate codes,
and who enforces `varchar(n)`?**
`TYPE.md` §3 gives them codes 8 and 9 with identical
representations.
*Suggested: keep both codes, because PostgreSQL distinguishes
them and a round trip through sm2 should not silently rewrite
a column's declared type; enforce the length limit in sm3,
with the constraints, not in the codec.* → `TYPE.md` §3.5

**2.4 [BLOCKING] Decimal128 only in v1, or 128 and 256?**
Supporting only 128 caps precision at 38 digits. Supporting
both means two widths for one type code, which the schema has
to carry.
*Suggested: both from the start, chosen by the declared
precision.* Adding the 256-bit width later would mean a second
type code for what users call one type. → `TYPE.md` §3.4

**2.5 [ASSUMABLE] Does v1 need `in`/`out`/`recv`/`send`?**
The ADT table in `DESIGN.md` §2.3 lists them, but sm2 has no
parser and no wire protocol.
*Suggested: `in`/`out` yes, because every test and every
golden-bytes fixture needs to write a value as text; `recv`
and `send` deferred, with the slots left in the descriptor.*
→ `DESIGN.md` §2.3

---

## 3. Key encoding — blocks `PLAN.md` step 2

**3.1 [BLOCKING] Does a `NOT NULL` column skip its null marker
byte?**
`TYPE.md` §2 says it does, which saves a byte per column per
key. The cost is that the key layout depends on nullability,
so `ALTER COLUMN ... DROP NOT NULL` rewrites every key in the
table and every index over it.
*Suggested: always emit the marker, even for `NOT NULL`.* The
byte is cheap, and a schema change that silently rewrites a
whole table is the kind of surprise this level exists to
avoid. This reverses what `TYPE.md` §2 currently says.
→ `TYPE.md` §2

**3.2 [ASSUMABLE] Confirm the escape scheme.**
`TYPE.md` §3.6 proposes `0x00` → `0x00 0xFF` with `0x00 0x00`
as terminator, verified order-preserving and prefix-free.
*Suggested: as written.* It is what CockroachDB and
FoundationDB use, and the alternatives cost either a length
prefix, which breaks ordering, or a wider alphabet.
→ `TYPE.md` §3.6

**3.3 [ASSUMABLE] Does the final column of a key still get its
terminator?**
Dropping it on the last column saves two bytes.
*Suggested: always emit it.* sm3 appends the primary key to
every index entry (`sm3/DESIGN.md` §3), so "last" is not a
property the encoder can see, and a conditional rule is a bug
waiting for the first covering index. → `TYPE.md` §2

**3.4 [BLOCKING] How wide is sm3's key prefix, and who spends
the `SM_MAX_KEYLEN` budget?**
`DESIGN.md` §5 gives `rel` an opaque prefix and `TYPE.md` §2
says the 4096 bound covers it. Nothing says how big it is, so
nothing can compute the budget left for a sort key.
*Suggested: a fixed 8 bytes, big-endian, set by sm3; sm2
validates the total and reports the remaining budget through
the API so sm3 never has to guess it.* → `DESIGN.md` §5,
`sm3/DESIGN.md` §4

**3.5 [ASSUMABLE] Are NULLs distinct in a unique index?**
A NULL key column encodes as its marker byte alone
(`TYPE.md` §2), so two NULLs are byte-equal. sm3's unique
check compares `sk` bytes without the pk suffix
(`sm3/DESIGN.md` §3), and so would reject a second NULL. SQL
says NULLs are distinct by default; PostgreSQL 15 added
`NULLS NOT DISTINCT` as an opt-in (from memory, not checked).
*Suggested: the encoding stays as it is, and sm3 skips the
uniqueness check for any entry with a NULL key column, unless
the index is declared `NULLS NOT DISTINCT`.* sm3 has the null
flags when it builds the key, so no decode is needed. If 1.3
truncates secondary keys, the recheck of originals on a cut
key follows the same rule. → `sm3/DESIGN.md` §3

---

## 4. Tuple format — blocks `PLAN.md` step 3

The first two are holes in the design as written, not
preferences.

**4.1 [BLOCKING] Is `rel` handed one schema, or every schema
version?**
`DESIGN.md` §4 stamps each tuple with a schema version so that
`ADD COLUMN` is O(1) and an old tuple is read under its own
layout. But `DESIGN.md` §5 opens a `rel` with *a* schema,
singular. A tuple stamped version 3 cannot be decoded by
schema version 7: the column count, the null bitmap width and
the offsets are all different. As written, sm2 cannot read any
tuple it did not just write.
*Suggested: `rel` is opened with a **schema registry** — a
lookup from version number to schema — not a single schema.
sm3's catalog owns it and keeps every version that any live
tuple might carry; sm2 asks it for the version it finds in the
stamp.* This also decides who may garbage-collect an old
version: sm3, once a rewrite proves no tuple still carries it.
→ `DESIGN.md` §4 and §5

**4.2 [BLOCKING] Where do out-of-line chunks live, and what
are their keys?**
`DESIGN.md` §4 says large datums are chunked out of line above
a threshold, and stops there. Chunks need keys, and the only
key space in an sm2 relation is the primary key space, which
they would collide with.
*Suggested: chunks live in the same sm1 relation, under the
`rel`'s prefix with a reserved discriminator byte that sorts
above every primary key, keyed by
`prefix ‖ 0xFF ‖ chunk_id ‖ seq`.* One relation keeps the
write atomic with the row for free, the discriminator keeps
chunks out of every table scan's range, and `chunk_id` comes
from an sm1 sequence. The alternative — a second sm1 relation
— costs a relation per table and fights the multiplexing sm3
does in `sm3/DESIGN.md` §4. → `DESIGN.md` §4

**4.3 [BLOCKING] Are datums aligned inside the tuple, or
packed?**
This is the whole "cheap to turn into a PostgreSQL tuple"
claim (`DESIGN.md` §2.1). Aligned means a decoded datum can be
pointed at in place; packed means every fixed-width read is a
`memcpy` to a local.
*Suggested: aligned, to the type's natural alignment, as
PostgreSQL does.* Packing saves a few bytes per tuple and
gives them back on every read, and the conversion story in
§2.1 is the reason this level exists. → `DESIGN.md` §4

**4.4 [ASSUMABLE] What is the chunking threshold?**
*Suggested: start at 2KB, make it a per-relation setting with
that default, and measure before defending the number.* sm1
allows 64MB, but undo, lock and read sets are linear in keys
touched (`sm1/DESIGN.md` §13). → `DESIGN.md` §4

---

## 5. The `rel` API — blocks `PLAN.md` step 4

**5.1 [ASSUMABLE] What is the header called, and what prefix
do the symbols take?**
`DESIGN.md` uses bare `rel_insert` and `tuple_encode`, which
will share one binary with sm1 and sm3.
*Suggested: `sm2/include/sm2.h`, with every exported symbol
prefixed `sm2_` — `sm2_rel_insert`, `sm2_tuple_encode` — and
the documents keep the short names for readability, saying so
once.* → `DESIGN.md` §5

**5.2 [ASSUMABLE] Who owns a decoded datum's memory?**
sm1's rule is that handed-out byte strings point into
engine-owned memory, are not NUL-terminated, must not be
freed, and have a stated validity window.
*Suggested: the same rule, with the window being "until the
next call on this `rel`", which is what sm1 does for
`sm_read`.* A codec that allocates per datum will dominate the
profile of every scan. → `DESIGN.md` §5

**5.3 [ASSUMABLE] Does sm2 get its own error space?**
`DESIGN.md` §5 says sm1's errors pass through untranslated,
but sm2 has failures of its own: a key over the limit, a datum
that does not match its column's type, an unknown schema
version, a corrupt tuple.
*Suggested: extend sm1's `sm_errno()` rather than invent a
second mechanism, with sm2's codes in a reserved range, so one
`sm_errmsg()` serves the whole stack.* → `DESIGN.md` §5

**5.4 [ASSUMABLE] Does sm2 enforce `NOT NULL`?**
The tuple has a null bitmap, so it *can*.
*Suggested: no.* Constraints are sm3's, and a codec that
rejects data is a codec with a policy in it. sm2 stores what
it is given. → `DESIGN.md` §5, `OBJECTIVE.md`

**5.5 [ASSUMABLE] Does a `rel` copy its schema or borrow it?**
Already open in `DESIGN.md` §5, and 4.1 changes its shape:
the question becomes whether `rel` copies the registry.
*Suggested: borrow, with sm3 required to keep it alive for the
`rel`'s lifetime and to take the schema-row lock before
changing it (`sm3/DESIGN.md` §2) — which it must do anyway.*
→ `DESIGN.md` §5

---

## 6. Mechanics — blocks the first line of code

**6.1 [BLOCKING] What is sm2's build?**
sm2 has no Makefile, and the top-level `LEVELS` does not list
it.
*Suggested: mirror sm1 exactly — `include/`, `src/`, `test/`,
a Makefile with the same targets, `sm2` added to `LEVELS`, and
doctest for the tests.* Nothing here deserves novelty.
→ `PLAN.md`

**6.2 [BLOCKING] What generates the property-based tests?**
`PLAN.md` steps 1 and 2 call for properties over generated
values, and sm1 has no such library.
*Suggested: hand-rolled generators with a fixed seed printed
on failure, in the style of sm1's stress tests, rather than a
new dependency.* → `PLAN.md`

**6.3 [BLOCKING] Where do the PostgreSQL fixtures come from?**
`PLAN.md` step 1 pins every representation to "a fixture taken
from a running PostgreSQL", which is what makes `DESIGN.md`
§2.1 a claim rather than an intention. That needs a PostgreSQL
somewhere, and `OBJECTIVE.md` forbids a build dependency on
one.
*Suggested: generate the fixtures once, by hand, on a
developer's machine; check the resulting table into
`test/pg_fixtures.h` with the PostgreSQL version recorded in
it; never build against PostgreSQL.* → `PLAN.md`

**6.4 [ASSUMABLE] Can sm2 land in pieces?**
*Suggested: yes — steps 1 to 3 are a library with no sm1
dependency at all and can land and be used before `rel`
exists.* It is also the order that gets the permanent formats
under test earliest. → `PLAN.md`

---

## 7. Not questions

Settled, and listed so nobody reopens them by accident:

- What the types are, and that their representation is
  PostgreSQL's — `DESIGN.md` §2.1, `TYPE.md` §3.
- That numeric is Arrow's decimal, and why — `DESIGN.md` §2.2.
- That a type is an ADT in `pg_type`'s shape — `DESIGN.md`
  §2.3.
- That interval keeps three components, and how its key
  separates PostgreSQL's ties — `DESIGN.md` §2.4, `TYPE.md`
  §3.10.
- That a key is injective but need not be reversible, and that
  every column lives in the tuple — `DESIGN.md` §3, §5.
- That the comparison is `memcmp` and the locale work happens
  once in the encoder — `DESIGN.md` §3.
- That the tuple codec is an API of its own — `DESIGN.md` §4.
- That sm2 is tuple-at-a-time; batching is sm3's question —
  `OBJECTIVE.md`, `sm3/DESIGN.md` §5.
- That the catalog, indexes, predicates and statistics are not
  sm2's — `DESIGN.md` §6.
