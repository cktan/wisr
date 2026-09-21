# OBJECTIVE — sm2

Build the level that turns sm1's ordered byte strings into
**rows**: typed columns, a record format, a catalog, and
secondary indexes.

sm1 stops exactly where this begins. `sm1/OBJECTIVE.md` says
its values are "opaque byte strings", its keys are ordered
"bytewise (`memcmp`)" with the encoding left to the caller, and
that "secondary indexes are not provided — they can be built by
another layer on top of this one". sm2 is that layer, and this
document is what it is for.

## Scope

- **Types and a record format.** Columns with types, nulls, and
  a layout that survives schema change without rewriting the
  table.
- **Key encoding.** Order-preserving encodings that turn typed
  column values into byte strings whose `memcmp` order is the
  intended order, including multi-column keys, ascending and
  descending columns, and null ordering.
- **A catalog.** Table and index definitions, column types and
  schema versions, kept in sm1 relations and maintained in the
  same transactions as the data.
- **Secondary indexes.** Unique and non-unique, maintained
  transactionally with the base row, built online on a populated
  relation.
- **A scan interface** that takes a predicate and derives the
  key range from it, rather than returning rows to be discarded
  above.
- **Statistics** good enough for a cost-based planner above.

## Not in scope

- A query language, a planner, or an executor. sm2 answers
  questions about rows; it does not decide which questions to
  ask.
- Anything sm1 already does: transactions, isolation, locking,
  durability, recovery, garbage collection. sm2 calls them; it
  does not reimplement them.
- A PostgreSQL dependency. sm2 must be useful and testable on
  its own, and buildable without a PostgreSQL source tree. The
  PG target shapes sm2's *decisions* (see `PART3.md` at the top
  level), never its *build*.

## Decisions

These are settled and drive `DESIGN.md`.

- **Row identity is the primary key.** The base relation is
  clustered on the primary key, as sm1 relations already are,
  and a secondary index entry is `encode(index columns) ‖
  encode(pk)`. A lookup is an index scan followed by a base
  read. There is no surrogate row id and no indirection layer.
  This matches sm1's model exactly, and it is what YugabyteDB
  does (`ybctid` is the encoded primary key).
- **The eventual consumer is a forked PostgreSQL**, keeping its
  parser, planner and executor and replacing its storage and
  transaction layers. sm2 is therefore built to be *friendly*
  to that target — a record format that is cheap to turn into a
  PostgreSQL tuple, a key encoder that takes a collation, and
  statistics a planner can use — without taking a dependency on
  it.
- **Batch-shaped API.** sm1 is an in-process library today, but
  a PostgreSQL backend is a separate process per connection and
  cannot link it (`PART3.md` §2). If sm1 and sm2 end up behind
  an IPC boundary, a per-row API is unsalvageable. sm2's calls
  therefore take and return **sets of rows** from the start:
  multi-row write, multi-key lookup, scan with the filter
  pushed down.
- **Relations are multiplexed.** sm1 gives each relation its own
  RocksDB column family and targets "hundreds to low thousands
  of relations per database" (`sm1/DESIGN.md` §13). A PostgreSQL
  schema of a few hundred tables with indexes, TOAST relations
  and partitions passes that easily, so sm2 maps many logical
  tables and indexes onto few sm1 relations by prefixing its own
  object id into the key.
- **Descending order is inverted bytes, scanned forward.** sm1
  does not scan backwards, and the fork's planner is changed
  never to ask it to (`PART3.md` §5). So a descending column
  is the ascending encoding with its bytes inverted, and there
  are no mirror relations.

## Open questions

Named here so they are decided deliberately, in `DESIGN.md`,
before anything is written to disk:

- **Over-long keys.** `SM_MAX_KEYLEN` is 4096 and bounds
  `sk ‖ pk` together. Collation sort keys can be several times
  the length of their source text. Truncate and recheck, hash
  and recheck, or reject?
- **Collation stability.** A sort key is only valid for the
  collation library version that produced it. What does sm2
  record, and what does it do when the version changes?
- **Non-deterministic collations.** Equal keys are not
  byte-equal under them, which is what unique indexes rest on.
  Support or refuse?
