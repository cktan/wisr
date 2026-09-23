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

Ordered by the step of `PLAN.md` each one blocks. Sections 1
to 3 — the suggested answers in `OBJECTIVE.md`, types, and key
encoding — are answered and have moved into `SM2_DESIGN.md`,
`TYPE.md` and `sm3/SM3_DESIGN.md`, and 6.1, the build, into
`PLAN.md`; the numbering of what remains is kept so that a
question's number never changes.

---

## 4. Tuple format — blocks `PLAN.md` step 3

The first two are holes in the design as written, not
preferences.

**4.1 [BLOCKING] Is `rel` handed one schema, or every schema
version?**
`SM2_DESIGN.md` §4 stamps each tuple with a schema version so
that `ADD COLUMN` is O(1) and an old tuple is read under its own
layout. But `SM2_DESIGN.md` §5 opens a `rel` with *a* schema,
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
→ `SM2_DESIGN.md` §4 and §5

**4.2 [BLOCKING] What are TOAST keys?**
Where TOAST data lives is settled: in the table's own sm1
store, under the object prefix `0xFFFF` (`SM2_DESIGN.md` §4,
§5). What follows the prefix is not.
*Suggested: `0xFFFF ‖ chunk_id ‖ chunk_seq`, PostgreSQL's
names, with `chunk_id` an 8-byte big-endian value from an sm1
sequence and `chunk_seq` a 4-byte big-endian chunk number.*
The tuple's TOAST pointer carries the `chunk_id`; `chunk_seq`
keeps a datum's chunks contiguous and in order, so reading one
back is one forward scan. → `SM2_DESIGN.md` §4

**4.3 [BLOCKING] Are datums aligned inside the tuple, or
packed?**
This is the whole "cheap to turn into a PostgreSQL tuple"
claim (`SM2_DESIGN.md` §2.1). Aligned means a decoded datum can
be pointed at in place; packed means every fixed-width read is a
`memcpy` to a local.
*Suggested: aligned, to the type's natural alignment, as
PostgreSQL does.* Packing saves a few bytes per tuple and
gives them back on every read, and the conversion story in
§2.1 is the reason this level exists. → `SM2_DESIGN.md` §4

**4.4 [ASSUMABLE] What is the TOAST threshold?**
*Suggested: start at 2KB, make it a per-relation setting with
that default, and measure before defending the number.* sm1
allows 64MB, but undo, lock and read sets are linear in keys
touched (`sm1/SM1_DESIGN.md` §13). → `SM2_DESIGN.md` §4

---

## 5. The `rel` API — blocks `PLAN.md` step 4

**5.1 [ASSUMABLE] What is the header called, and what prefix
do the symbols take?**
`SM2_DESIGN.md` uses bare `rel_insert` and `tuple_encode`, which
will share one binary with sm1 and sm3.
*Suggested: `sm2/include/sm2.h`, a C++ header with
everything in namespace `sm2` — `sm2::rel_insert`,
`sm2::tuple_encode` — and the documents keep the short names
for readability, saying so once.* A prefix is no longer
needed: only the top level exposes C (`OBJECTIVE.md` at the
top level). → `SM2_DESIGN.md` §5

**5.2 [ASSUMABLE] Who owns a decoded datum's memory?**
sm1's rule is that handed-out byte strings point into
engine-owned memory, are not NUL-terminated, must not be
freed, and have a stated validity window.
*Suggested: the same rule, with the window being "until the
next call on this `rel`", which is what sm1 does for
`sm1_read`.* A codec that allocates per datum will dominate the
profile of every scan. → `SM2_DESIGN.md` §5

**5.3 [ASSUMABLE] Does sm2 get its own error space?**
`SM2_DESIGN.md` §5 says sm1's errors pass through untranslated,
but sm2 has failures of its own: a key over the limit, a datum
that does not match its column's type, an unknown schema
version, a corrupt tuple.
*Suggested: extend sm1's `sm1_errno()` rather than invent a
second mechanism, with sm2's codes in a reserved range, so one
`sm1_errmsg()` serves the whole stack.* → `SM2_DESIGN.md` §5

**5.4 [ASSUMABLE] Does sm2 enforce `NOT NULL`?**
The tuple has a null bitmap, so it *can*.
*Suggested: no.* Constraints are sm3's, and a codec that
rejects data is a codec with a policy in it. sm2 stores what
it is given. → `SM2_DESIGN.md` §5, `OBJECTIVE.md`

**5.5 [ASSUMABLE] Does a `rel` copy its schema or borrow it?**
Already open in `SM2_DESIGN.md` §5, and 4.1 changes its shape:
the question becomes whether `rel` copies the registry.
*Suggested: borrow, with sm3 required to keep it alive for the
`rel`'s lifetime and to take the schema-row lock before
changing it (`sm3/SM3_DESIGN.md` §2) — which it must do anyway.*
→ `SM2_DESIGN.md` §5

**5.6 [BLOCKING] How does sm3 write index entries through
sm2, and who draws `rowno`?**
The table is now a heap keyed by `rowno` (`SM2_DESIGN.md` §5), so
`rel` reads and writes rows by `rowno`. Index entries are keyed
by encoded columns instead — `encode(sk) ‖ rowno`, or
`encode(sk)` alone for a unique index (`sm3/SM3_DESIGN.md` §3) —
and sm3 may only reach sm1 through sm2, so sm2 needs a way to
write, delete and scan them. Separately, someone calls
`sm1_nextseq` for each new row.
*Suggested: two kinds of `rel`, opened over the same sm1
store with different prefixes: a **heap `rel`**, keyed by
`rowno`, and an **index `rel`**, keyed by the encoded columns
it is opened with, whose value is a tuple (the `rowno` and the
INCLUDE columns). `rel_insert` on a heap `rel` draws the
`rowno` from the table's sequence, named in the schema, and
returns it, so the sequence is used in one place.*
→ `SM2_DESIGN.md` §5

---

## 6. Mechanics — blocks the first line of code

**6.2 [BLOCKING] What generates the property-based tests?**
`PLAN.md` steps 1 and 2 call for properties over generated
values, and sm1 has no such library.
*Suggested: hand-rolled generators with a fixed seed printed
on failure, in the style of sm1's stress tests, rather than a
new dependency.* → `PLAN.md`

**6.3 [BLOCKING] Where do the PostgreSQL fixtures come from?**
`PLAN.md` step 1 pins every representation to "a fixture taken
from a running PostgreSQL", which is what makes `SM2_DESIGN.md`
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
  PostgreSQL's — `SM2_DESIGN.md` §2.1, `TYPE.md` §3.
- That `number` is Arrow's decimal, and why — `SM2_DESIGN.md`
  §2.2.
- That a type is an ADT in `pg_type`'s shape — `SM2_DESIGN.md`
  §2.3.
- That interval keeps three components, and how its key
  separates PostgreSQL's ties — `SM2_DESIGN.md` §2.4, `TYPE.md`
  §3.10.
- That a key is injective but need not be reversible, and that
  every column lives in the tuple — `SM2_DESIGN.md` §3, §5.
- That the comparison is `memcmp` and the locale work happens
  once in the encoder — `SM2_DESIGN.md` §3.
- That the tuple codec is an API of its own — `SM2_DESIGN.md` §4.
- That sm2 is tuple-at-a-time; batching is sm3's question —
  `OBJECTIVE.md`, `sm3/SM3_DESIGN.md` §5.
- That the catalog, indexes, predicates and statistics are not
  sm2's — `SM2_DESIGN.md` §6.
- The type codes, `enum` as sm3's, and `text` / `varchar` —
  `SM2_DESIGN.md` §2.
- `number(p, s)` only: its widths, its bare default, and the
  refusal of NaN — `SM2_DESIGN.md` §2.2.
- That `in` / `out` are filled in v1 and `recv` / `send` wait —
  `SM2_DESIGN.md` §2.3.
- Null markers, escaping and terminators — `SM2_DESIGN.md` §3.1.
- Over-long keys — `SM2_DESIGN.md` §3.2.
- Collation version and determinism — `SM2_DESIGN.md` §3.3.
- One sm1 store per table and the 2-byte object prefix —
  `SM2_DESIGN.md` §5, `sm3/SM3_DESIGN.md` §4.
- That the table is a heap keyed by a 64-bit `rowno`, the
  primary key is an index, and `rowno` is `ctid` —
  `SM2_DESIGN.md` §5, `sm3/SM3_DESIGN.md` §3.
- That NULLs are distinct in a unique index —
  `sm3/SM3_DESIGN.md` §3.
- sm2's build and source layout — `PLAN.md`, "Source
  layout".
