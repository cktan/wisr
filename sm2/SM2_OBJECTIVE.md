# OBJECTIVE — sm2

Build the level that turns sm1's ordered byte strings into
**tuples**: typed columns, the two encodings those types get
on disk, and a relation whose rows are tuples rather than raw
bytes.

sm1 stops exactly where this begins. `sm1/SM1_OBJECTIVE.md` says
its values are "opaque byte strings" and its keys are ordered
"bytewise (`memcmp`)" with the encoding left to the caller.
sm2 is the caller that supplies that encoding, and this
document is what it is for.

sm2 stops well short of a schema. A catalog, secondary
indexes, predicate scans and statistics are sm3's, built on
sm2 the way sm2 is built on sm1 (`sm3/SM3_OBJECTIVE.md`).

## Scope

- **Types.** A fixed set of column types, each with an
  in-memory datum, a key encoding and a tuple encoding, and a
  stable on-disk type code.
- **Key encoding.** Order-preserving encodings that turn typed
  datums into byte strings whose `memcmp` order is the
  intended order, including multi-column keys, ascending and
  descending columns, null ordering, and collated text.
- **A tuple format, and a tuple codec API of its own.**
  Columns with types and nulls, in a layout that survives
  schema change without rewriting the table — encoded and
  decoded through a component with its own header, built and
  tested before the relation that uses it.
- **A tuple relation.** sm1's store with the types put
  back: tuples in, tuples out, encoded and decoded by sm2,
  inside the caller's sm1 transaction.

## Not in scope

- **A catalog.** sm2 is handed a schema; it does not persist
  one, look one up, or know what a table is. That is sm3's,
  and so is DDL and the locking that makes it safe.
- **Secondary indexes.** sm2 gives sm3 what index maintenance
  needs — the encoder, and the old image as a tuple — and
  maintains no index itself.
- **Predicates, scans by predicate, and statistics.** sm2 has
  no filter and counts nothing. Both are sm3's.
- **Mapping tables onto stores.** An sm2 relation is one sm1
  store. sm2 prepends a 2-byte object prefix chosen by sm3, so
  a table's rows, indexes and TOAST data can share one sm1
  store; which prefix means what is sm3's.
- A query language, a planner, or an executor.
- Anything sm1 already does: transactions, isolation, locking,
  durability, recovery, garbage collection. sm2 calls them; it
  does not reimplement them.
- A PostgreSQL dependency. sm2 must be useful and testable on
  its own, and buildable without a PostgreSQL source tree. The
  PG target shapes sm2's *decisions* (see `PART4.md` at the
  top level), never its *build*.

## Decisions

These are settled and drive `SM2_DESIGN.md`.

- **Row identity is `rowno`, and the table is a heap.** A row
  is keyed by an unsigned 64-bit `rowno` from a per-table sm1
  sequence, never reused and kept across updates; the primary
  key is an index over it, as in PostgreSQL. The fork
  redefines `ctid` as a `uint64_t`, so `rowno` is `ctid`
  (`SM2_DESIGN.md` §5). This reverses an earlier choice to
  cluster the table on its primary key, as YugabyteDB does: a
  heap is what PostgreSQL's planner and executor assume, it
  serves a table with no primary key, and a primary key change no
  longer rewrites every index. It costs a second read on a
  primary key lookup and locality on a primary key range
  scan.
- **Every column is in the tuple.** An encoded index key is
  an ordering device, not storage: an indexed column is
  written twice, encoded in the index and native in the
  tuple, and the tuple is what a read returns. It costs the
  duplicate; it buys a tuple that is complete on its own and
  the next decision (`SM2_DESIGN.md` §5).
- **A key must be injective, not reversible.** Distinct datums
  get distinct keys, because unique indexes are enforced on key
  bytes. A key need not decode back to its datum, because the
  tuple holds it — so a lossy encoding is allowed where it is
  order-preserving and made injective by a tie-break, and
  `key_decode` is an optimization for sm3's index-only scans
  rather than a correctness requirement (`SM2_DESIGN.md` §3).
- **A datum's representation is PostgreSQL's**, type by
  type: the same widths, epochs, units and struct layouts,
  so that crossing into a tuple slot copies the column bytes
  instead of translating them. Two places need sm2 to be
  stricter than PostgreSQL is — the byte order, fixed
  little-endian, and variable-length datums, which carry the
  payload alone and leave framing to the tuple
  (`SM2_DESIGN.md` §2.1).
- **Except `decimal`, which follows DuckDB's**: a fixed-width
  two's-complement integer with the scale in the schema, 64
  bits to 18 digits of precision and 128 to 38. It buys a
  `decimal` key encoding that is just an integer's, and a
  fixed-width `decimal` column; it costs PG's unbounded
  precision, PG's NaN and infinity, and a per-datum
  conversion for this one type (`SM2_DESIGN.md` §2.2).
- **A type is an ADT, in PostgreSQL's shape** — one
  descriptor of function pointers per type, as `pg_type` is,
  with `key_encode` added because sm1 compares with `memcmp`
  and a PG btree never needed one. Adding a type is adding a
  descriptor, not editing a `switch` in three places
  (`SM2_DESIGN.md` §2.3).
- **Interval keeps PostgreSQL's three components**, months, days
  and microseconds apart, because they are not convertible.
  Its order is PG's flattened 128-bit datum, which is not
  injective, so its *key* appends the months and days behind
  that datum to restore identity — a total order that refines
  PostgreSQL's rather than contradicting it
  (`SM2_DESIGN.md` §2.4, §3).
- **The tuple codec is a separate API.** It holds the
  permanent on-disk format, it has two callers (the tuple
  relation, and sm3's INCLUDE columns), and it is the only
  part of sm2 that touches no database — so it is built and
  tested on its own (`SM2_DESIGN.md` §4).
- **One encoder, two callers.** The key encoder is sm2's, and
  sm3 builds its index entries — `encode(sk) ‖ rowno` —
  from the same function with the same per-column arguments.
  Two encoders that have to agree is a bug waiting to be
  written.
- **sm2 is a codec, not a state machine.** Every durable
  effect is an sm1 call inside the caller's transaction, and
  sm2 keeps no state across calls. Abort and savepoint
  rollback therefore cost sm2 nothing.
- **The eventual consumer is a forked PostgreSQL**, keeping
  its parser, planner and executor and replacing its storage
  and transaction layers. The fork is **threaded**, a thread
  per connection instead of a process (`PART4.md` §2), so sm1,
  sm2 and sm3 are linked into the one server process and every
  backend thread shares sm1's transaction table, lock manager
  and SSI graph. sm2 is therefore built to be *friendly* to
  that target — a tuple format that is cheap to turn into a
  PostgreSQL tuple, and a key encoder that takes a collation —
  without taking a dependency on it.
- **One tuple at a time.** The IPC boundary that once forced
  a batch-shaped API is gone with the threaded fork
  (`PART4.md` §2), so the call above sm2 is an ordinary
  function call. Whether a batched form earns its keep for
  filter pushdown is a question for the level that has filters
  (`sm3/SM3_OBJECTIVE.md`), and nothing in sm2 depends on the
  answer.
- **Descending order is inverted bytes, scanned forward.** sm1
  does not scan backwards, and the fork's planner is changed
  never to ask it to (`PART4.md` §5). So a descending column
  is the ascending encoding with its bytes inverted, and there
  are no mirror relations.
- **Writes at READ COMMITTED follow sm1's protocol, and sm2
  does not resolve them.** A write that meets a row committed
  after the transaction began is refused with `SM_ERR_UPDATED`
  (`sm1/SM1_DESIGN.md` §5.4). Resolving it means re-checking the
  statement's filter against the newest image, and filters are
  sm3's; sm2 surfaces the error and provides
  `rel_read_latest`.
- **Collated keys store the sort key and the original.** Not
  to recover the datum — the tuple does that — but because
  a deterministic collation can give two distinct strings one
  sort key, and PostgreSQL keeps them as two datums. Without
  the tie-break the two would share a key and become one row
  (`PART4.md` §6, `SM2_DESIGN.md` §3).

## Decided questions

Five questions about the encoding, decided in `SM2_DESIGN.md`
before anything is written to disk. They are sm2's and not
sm3's because they are about the encoding, even though it is
sm3's indexes that feel them most.

- **Bare `decimal`.** A fixed-width decimal takes its scale
  from the schema; PostgreSQL allows `numeric` with no typmod
  at all, where each datum carries its own weight and display
  scale. A bare `decimal` column has no fixed-width
  representation.
  Refuse it, pick a default, or keep a second variable-width
  representation for it?

  *Decided: a default of `decimal(18, 3)`*, DuckDB's
  (`SM2_DESIGN.md` §2.2). A bare `decimal` has 15 integer digits
  and 3 fractional. A datum with more fractional digits is
  rounded to 3, as PostgreSQL rounds for a declared typmod; one
  whose integer part needs more than 15 digits is an error.
  `decimal(p)` means `decimal(p, 0)`. The cost is accepted
  knowingly: a datum a user believed exact can lose digits past
  the third fractional place without an error. A second
  variable-width representation is rejected, because it means
  two `decimal`s on disk with two key encodings and two
  comparison paths, which is most of what §2.2 was avoiding.
  `numeric` is an alias of `decimal` (`SM2_DESIGN.md` §2.2), so a
  schema ported from PostgreSQL keeps its `numeric(p, s)`
  columns, within 38 digits.

- **`decimal` NaN and infinity.** PG's numeric has both; a
  fixed-width decimal is an integer and has neither. Refuse
  them, or reserve values?

  *Decided: refuse them* (`SM2_DESIGN.md` §2.2). A NaN or an
  infinity is rejected on insert with a clear error, and no
  value is reserved as a sentinel. The door stays open: a
  precision of 18 digits does not reach the 64-bit limit, nor
  38 the 128-bit one, so the extreme integers are never
  written and could still be reserved later without changing
  the meaning of stored bytes — provided the codec keeps
  rejecting any value outside the declared precision.

- **Over-long keys.** `sm1::max_keylen` is 4096 and bounds an
  index entry's whole key. Collation sort keys can be several
  times the length of their source text. Truncate and recheck,
  hash and recheck, or reject?

  *Decided: reject for the primary key, truncate and recheck for
  a secondary index, never hash* (`SM2_DESIGN.md` §3.2). The
  primary key index enforces uniqueness on exact key bytes, so it
  must be exact: an encoded primary key over a fixed limit is
  refused when the row is written, which is what PostgreSQL does
  for an oversize btree entry (its limit is roughly 2.7KB, from
  memory, not checked against the source). A secondary index can
  do better, because the original datum is stored beside the sort
  key anyway. Truncating a sort key keeps the order as a
  coarsening: if `a <= b` then `trunc(a) <= trunc(b)`, so a range
  scan on the truncated keys returns a superset, and the recheck
  against the stored original removes the extras. Two costs come
  with it. An entry is flagged when its key was cut, because the
  index can no longer promise order among entries that share a
  prefix, and a unique check must compare originals over the
  prefix range rather than trust key equality, treating NULLs as
  distinct (`sm3/SM3_DESIGN.md` §3). A hash is out, because it
  keeps equality but destroys order, and an index that cannot
  serve a range or an `ORDER BY` is not worth having. The budget
  for an index key is `4094` after the prefix, less 8 for `rowno`
  where the entry carries one.

- **Collation stability.** A sort key is only valid for the
  collation library version that produced it. The version is
  recorded with the schema, as `pg_collation.collversion` exists
  to do for PostgreSQL (`PART4.md` §6). The question was what
  happens when the recorded version and the library's differ:
  refuse, or rebuild?

  *Decided: pin the library so it rarely differs, and refuse on a
  mismatch, with an explicit rebuild* (`SM2_DESIGN.md` §3.3).
  Link a specific ICU version into the server instead of using
  whatever the operating system has, so a mismatch happens only
  when the operator upgrades on purpose. When it does happen,
  compare the recorded version at open. Refuse to use, and refuse
  to write to, any index built under the old version, the primary
  key index included, with a message that says why. PostgreSQL
  only warns and carries on, but continuing to write sort keys
  from a new version into an old index corrupts it further, and a
  unique index quietly stops being unique. Rebuilding any index,
  the primary key's included, is an online build
  (`sm3/SM3_DESIGN.md` §3); the table is keyed by `rowno` and
  never needs rewriting. The refusal is still the default and the
  rebuild a command someone chooses to run.

- **Non-deterministic collations.** Equal keys are not
  byte-equal under them, which is what unique indexes rest on.
  Support or refuse?

  *Decided: accept them for ICU collations, keyed by the sort
  key alone* (`SM2_DESIGN.md` §3.3). The first decision was to
  refuse them and design so that accepting them later needed no
  format change; that design is what was adopted. Under a
  non-deterministic collation two different strings can compare
  equal, and the sort key is the same for both, so a unique check
  on the sort key gives the right answer. What gets hard is
  everywhere else equality is used: hashing for joins and
  grouping, and the rule that a deterministic collation breaks
  ties bytewise while a non-deterministic one does not.
  PostgreSQL has supported them since version 12. The collation
  carries a `deterministic` flag in the schema from the start,
  chosen when the column is created and never changed. A
  deterministic collation's key is the sort key followed by the
  original bytes as a tie-break; a non-deterministic one's is the
  sort key alone. Because the flag is per column, accepting the
  second kind touched no data already on disk.
