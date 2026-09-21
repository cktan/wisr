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
  schema versions, held in memory and persisted through sm1
  relations in the same transactions as the data. It also
  replaces PostgreSQL's heap catalogs and sequences, which the
  fork can no longer keep on heap (`PART3.md` §4).
- **Secondary indexes.** Unique and non-unique, maintained
  transactionally with the base row, built online on a populated
  relation.
- **A scan interface** that takes a predicate and derives the
  key range from it, rather than returning rows to be discarded
  above.
- **Statistics** good enough for a cost-based planner above.
  They are not optional: the planner is kept, so it needs row
  counts, sizes, histograms and correlations, and `ANALYZE`
  needs a sampling path.

## Not in scope

- A query language, a planner, or an executor. sm2 answers
  questions about rows; it does not decide which questions to
  ask.
- Anything sm1 already does: transactions, isolation, locking,
  durability, recovery, garbage collection. sm2 calls them; it
  does not reimplement them.
- Replication, point-in-time recovery, and logical decoding.
  They read PostgreSQL's WAL, which does not carry data held in
  RocksDB (`PART3.md` §4); rebuilding them is a subsystem of its
  own, at the storage tier.
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
  transaction layers. The fork is **threaded**, a thread per
  connection instead of a process (`PART3.md` §2), so sm1 and
  sm2 are linked into the one server process and every backend
  thread shares sm1's transaction table, lock manager and SSI
  graph. sm2 is therefore built to be *friendly*
  to that target — a record format that is cheap to turn into a
  PostgreSQL tuple, a key encoder that takes a collation, and
  statistics a planner can use — without taking a dependency on
  it.
- **Batch-shaped API: open.** It was decided because a
  PostgreSQL backend was a separate process per connection that
  could not link sm1, and behind an IPC boundary a per-row API
  is unsalvageable. The fork is now threaded, so the boundary
  between the executor and sm2 is a function call and the shape
  is no longer forced (`PART3.md` §2). Whether sm2 keeps sets of
  rows in and out for other reasons, filter pushdown and fewer
  calls per scan, is decided in `DESIGN.md`. Nothing else here
  depends on the answer.
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
- **Writes at READ COMMITTED follow sm1's protocol.** A write
  that meets a row committed after the transaction began is
  refused with `SM_ERR_UPDATED`; sm2 reads the newest version
  with `sm_read_latest`, re-checks the statement's filter, and
  only then repeats the write (`PART3.md` §5a,
  `sm1/DESIGN.md` §5.4). Index entries are computed from that
  newest image.
- **Collated keys store the sort key and the original.** The
  collation transform is lossy and irreversible, so the value is
  stored twice, as YugabyteDB does (`PART3.md` §6).

## Open questions

Named here so they are decided deliberately, in `DESIGN.md`,
before anything is written to disk:

- **Over-long keys.** `SM_MAX_KEYLEN` is 4096 and bounds
  `sk ‖ pk` together. Collation sort keys can be several times
  the length of their source text. Truncate and recheck, hash
  and recheck, or reject?

  *Suggested answer: reject for the primary key, truncate and
  recheck for a secondary index, never hash.* The primary key is
  the row's identity and the base relation's key, so it must be
  exact: an encoded primary key over a fixed limit is refused
  when the row is written, which is what PostgreSQL does for an
  oversize btree entry (its limit is roughly 2.7KB, from memory,
  not checked against the source). A secondary index can do
  better, because the original value is stored beside the sort
  key anyway. Truncating a sort key keeps the order as a
  coarsening: if `a <= b` then `trunc(a) <= trunc(b)`, so a
  range scan on the truncated keys returns a superset, and the
  recheck against the stored original removes the extras. Two
  costs come with it. An entry is flagged when its key was cut,
  because the index can no longer promise order among entries
  that share a prefix, and a unique check must compare originals
  over the prefix range rather than trust key equality. A hash
  is out, because it keeps equality but destroys order, and an
  index that cannot serve a range or an `ORDER BY` is not worth
  having. The budget for the sort key is `4096` less the length
  of the encoded primary key, so the primary key limit should
  leave most of it.

- **Collation stability.** A sort key is only valid for the
  collation library version that produced it. The version is
  recorded per relation, as `pg_collation.collversion` exists to
  do for PostgreSQL (`PART3.md` §6). What remains open is what
  sm2 does when the recorded version and the library's differ:
  refuse the index, or rebuild it?

  *Suggested answer: pin the library so it rarely differs, and
  refuse on a mismatch, with an explicit rebuild.* Link a
  specific ICU version into the server instead of using whatever
  the operating system has, so a mismatch happens only when the
  operator upgrades on purpose. When it does happen, compare the
  recorded version at open. Refuse to use, and refuse to write
  to, any index or collated primary key built under the old
  version, with a message that says why. PostgreSQL only warns
  and carries on, but continuing to write sort keys from a new
  version into an old index corrupts it further, and a unique
  index quietly stops being unique. Rebuilding a secondary index
  is an online build (`DESIGN.md` §5). A collated primary key is
  harder, because the table is clustered on it, so the rebuild
  rewrites the table; that is why the refusal has to be the
  default and the rebuild a command someone chooses to run.

- **Non-deterministic collations.** Equal keys are not
  byte-equal under them, which is what unique indexes rest on.
  Support or refuse?

  *Suggested answer: refuse at first, and design so that
  accepting them later needs no format change.* Under a
  non-deterministic collation two different strings can compare
  equal, and the sort key is the same for both, so a unique
  check on the sort key gives the right answer. What gets hard
  is everywhere else equality is used: hashing for joins and
  grouping, and the rule that a deterministic collation breaks
  ties bytewise while a non-deterministic one does not.
  PostgreSQL has supported them only since version 12 (from
  memory, not checked) and they are rare, so the first version
  rejects a non-deterministic collation when a
  column or index is created, with a clear error, and never
  silently. The collation carries a `deterministic` flag in the
  catalog from the start, chosen when the column is created and
  never changed. A deterministic collation's key is the sort key
  followed by the original bytes as a tie-break; a
  non-deterministic one's is the sort key alone. Because the flag
  is per column, a later release can accept the second kind
  without touching data already on disk.
