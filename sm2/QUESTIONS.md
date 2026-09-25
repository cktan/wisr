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

Ordered by the step of `SM2_DESIGN.md` §8.2 each one blocks.
Sections 1 to 3 — the suggested answers in `SM2_OBJECTIVE.md`,
types, and key encoding — are answered and have moved into
`SM2_DESIGN.md`, `TYPE.md` and `sm3/SM3_DESIGN.md`; 4.1 and 4.3
— the schema a `rel` is handed and alignment — into
`SM2_DESIGN.md` §4, the first answered by keeping no schema
version and the second by laying datums out unpadded, not
aligned; 5.1 and 5.3 — the header and sm2's errors — and 5.2,
5.4, 5.5 and 5.6 — a decoded datum's memory, `NOT NULL`, the
schema a `rel` borrows, and heap and index `rel`s — into
`SM2_DESIGN.md` §5, which empties section 5; and 6.1 to 6.4 —
the build, the property tests, the PostgreSQL fixtures, and
landing sm2 in pieces, which it did — into `SM2_DESIGN.md` §8.
The numbering of what remains is kept so that a question's
number never changes.

---

## 4. Tuple format — blocks step 3 (`SM2_DESIGN.md` §8.2)

4.2 is a hole in the design as written, not a preference.

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

**4.4 [ASSUMABLE] What is the TOAST threshold?**
*Suggested: start at 2KB, make it a per-relation setting with
that default, and measure before defending the number.* sm1
allows 64MB, but undo, lock and read sets are linear in keys
touched (`sm1/SM1_DESIGN.md` §13). → `SM2_DESIGN.md` §4

---

## 7. Not questions

Settled, and listed so nobody reopens them by accident:

- What the types are, and that their representation is
  PostgreSQL's — `SM2_DESIGN.md` §2.1, `TYPE.md` §3.
- That `decimal` follows DuckDB's decimal, and why —
  `SM2_DESIGN.md` §2.2.
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
  `SM2_OBJECTIVE.md`, `sm3/SM3_DESIGN.md` §5.
- That the catalog, indexes, predicates and statistics are not
  sm2's — `SM2_DESIGN.md` §6.
- The type codes, `enum` as sm3's, and `text` / `varchar` —
  `SM2_DESIGN.md` §2.
- `decimal(p, s)` only: its widths, its bare default, and the
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
- sm2's build and source layout — `SM2_DESIGN.md` §8.1.
- That property tests are hand-rolled, seeded and replayable,
  with no new dependency — `SM2_DESIGN.md` §8.3.
- That the PostgreSQL fixtures are PostgreSQL 18's on-disk
  bytes, read through `pageinspect` on the development machine
  and checked in — `SM2_DESIGN.md` §8.3.
- That sm2's interface is `include/sm2.h` in `namespace sm2`,
  and its failures go through sm1's error state with codes of
  its own appended to `sm1::errnum_t` — `SM2_DESIGN.md` §5.
