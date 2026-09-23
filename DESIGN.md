# DESIGN — wisr

How the stack in `OBJECTIVE.md` fits together: the levels,
the build, and the decisions that cross level boundaries.
Each level's own mechanics — sm1's MVCC, sm2's tuple
format, sm3's catalog — are argued in that level's own
`SMn_DESIGN.md`; this document does not repeat them.

---

## 1. The stack

```
                      PostgreSQL (C)
                             │  include/ — the one C API
        ┌────────────────────┴─────────────────────────┐
        │  top level  the C wrapper                    │
        │  sessions · transactions · 2PC → sm1 ·       │
        │  tables and indexes → sm3                    │
        └────────────────────┬─────────────────────────┘
                             │  sm3::
        ┌────────────────────┴─────────────────────────┐
        │  sm3/  schema and indexes                    │
        │  catalog · secondary indexes ·               │
        │  scan-by-predicate · statistics              │
        └────────────────────┬─────────────────────────┘
                             │  sm2::  the tuple relation
        ┌────────────────────┴─────────────────────────┐
        │  sm2/  types and tuples                      │
        │  type system · key encoding ·                │
        │  tuple format · relation of tuples           │
        └────────────────────┬─────────────────────────┘
                             │  sm1::
        ┌────────────────────┴─────────────────────────┐
        │  sm1/  MVCC storage manager                  │
        │  relations · transactions · isolation ·      │
        │  locks · 2PC · recovery · GC                 │
        └────────────────────┬─────────────────────────┘
                             │  RocksDB's own API
        ┌────────────────────┴─────────────────────────┐
        │  RocksDB — ordered, durable, crash-atomic    │
        │  key-value store                             │
        └──────────────────────────────────────────────┘
```

sm2 is a client of sm1, nothing more: it holds no RocksDB
handle of its own and reaches storage only through sm1's
interface. sm3 stands in the same relation to sm2 — it never
encodes a key or decodes a tuple itself — with one
deliberate exception: `sm1_lock`, which sm2 has nothing to
add to and which sm3's DDL and index-build protocols are
built on (`sm3/SM3_DESIGN.md` §2). That boundary is why each
level's `OBJECTIVE.md` can promise it is "useful and
testable on its own" — the whole lower stack is one
dependency.

The top level is not a level: it adds no notion of its own,
only a C signature over the levels' C++ interfaces, and it
is the one place an exception is caught and a PostgreSQL
error path is safe to take (`OBJECTIVE.md`). It is also the
one caller that skips a level: it calls sm1 directly for
sessions, transactions, savepoints and 2PC, which neither sm2
nor sm3 has anything to add to, and sm3 for everything else.
Below it, each level still talks only to the one beneath.

---

## 2. Build

Each level has a real `Makefile`; the top-level one
delegates to them, and builds the one thing that belongs to
no level — the C wrapper in the root's `include/` and `src/`.

```
make            build every level
make test       build and test every level
make -C sm1     build level 1 alone
```

`LEVELS` in the top-level `Makefile` lists which levels have
a Makefile to delegate to; `sm2` and `sm3` join it when they
have one. Adding a level to the stack means adding its name
there, nowhere else.

---

## 3. Status

| Level | Status |
| --- | --- |
| sm1 | built and tested; the design of record is `sm1/SM1_DESIGN.md` |
| sm2 | designed, not yet implemented — every open section of `sm2/SM2_DESIGN.md` is marked "to be written"; `sm2/PLAN.md` has the build order |
| sm3 | designed, not yet implemented — the same, in `sm3/SM3_DESIGN.md` and `sm3/PLAN.md`; it cannot start before sm2's two formats are settled |
| top level | not yet written — the C wrapper waits for the levels it wraps; until then sm1's C API in `sm1/include/sm1.h` is the only C surface, and it is replaced by a C++ interface before sm2 starts |

sm1 was built first and alone, on purpose: it is usable
without sm2 (an MVCC key-value store is a complete product
by itself), and every sm2 decision needs sm1's real API and
real behavior to check itself against, not a sketch of one.
sm2 and sm3 are split for the same reason in a different
key: sm2 is two permanent on-disk formats and a codec over
them, sm3 is protocols that can be rewritten, and keeping
the permanent part small is what lets it get all the
scrutiny (`sm2/PLAN.md`).

---

## 4. Testing across levels

Every level uses the same two-layer scheme, established in
`sm1/mvcc/DRIVER.md`:

- ordinary tests for contract and single-session mechanics —
  C++ unit tests in each level's `test/`.
- a **declarative scenario suite** for anything where two
  sessions interleave — the isolation and concurrency
  questions that read as unreadable imperative test code.

Most of sm2 is the first kind — a codec is tested by
property, not by interleaving — and most of the second kind
arrives with sm3: an index build against a concurrent
writer, DDL against DML. Both `PLAN.md`s reuse this scheme
rather than invent a second, and defer only the question of
whether a level needs a driver of its own or can extend
sm1's — "a decision for the first scenario that needs it,
not before."

---

## 5. Cross-level decisions

Decisions that constrain more than one level, gathered here
so a change to one level does not silently violate another's
design:

- **sm1's interface is the only channel into sm1.** Nothing
  above it may read RocksDB directly or depend on sm1's
  internals — the `SECINDEX.md`-style gap
  analysis in `sm1/` is how a missing primitive gets added
  to sm1's surface instead of worked around from outside it.
- **One encoder, one tuple format, two callers.** The key
  encoding and the tuple layout are sm2's alone
  (`sm2/SM2_DESIGN.md` §3, §4), and sm3 builds its index entries
  — `encode(sk) ‖ rowno` — by calling sm2's encoder
  with the same per-column arguments. Two encoders that have
  to agree bytewise is a corruption bug waiting to be
  written, so there is only one.
- **sm3 reaches sm1 for locks, and for nothing else.** sm2
  has nothing to add to `sm1_lock`, and sm3's DDL and
  index-build protocols rest on it (`sm3/SM3_DESIGN.md` §2), so
  that one call is the sanctioned exception to sm3 speaking
  only to sm2. Every other sm1 facility sm3 needs arrives
  through sm2's tuple relation.
- **A table is a heap keyed by `rowno`, and the primary key
  is an index.** A row's key is an unsigned 64-bit `rowno`
  from a per-table sm1 sequence, never reused and unchanged by
  `UPDATE` (`sm2/SM2_DESIGN.md` §5). Every index, the primary
  key's included, is sm3's and points at `rowno`
  (`sm3/SM3_DESIGN.md` §3). The fork redefines `ctid` as a
  `uint64_t`, so `rowno` is `ctid` — a core patch to
  PostgreSQL's 6-byte `ItemPointerData` (`PART4.md` §3). This
  matches the heap PostgreSQL's planner and executor assume,
  serves tables without a primary key, and keeps a primary
  key change from rewriting every index; a primary key lookup
  costs two reads.
- **One sm1 store per table.** A table's rows, all its
  indexes and its TOAST data share one sm1 store,
  told apart by a 2-byte object prefix: `0x0000` for rows,
  `0x0001`–`0xFFFE` for indexes, `0xFFFF` for TOAST. sm3
  chooses the prefix; sm2 prepends and strips it and never
  interprets it (`sm2/SM2_DESIGN.md` §5), so sm2 never learns
  what a table is. `DROP TABLE` is one `sm1_rmstore`, sm1's O(1)
  column family drop; `DROP INDEX` is a range delete over its
  prefix, and index prefixes are never reused. The cost is
  that tables, partitions included, are bounded by the
  hundreds to low thousands of stores sm1 handles
  (`sm1/SM1_DESIGN.md` §13); accepted for v1 (`sm3/SM3_DESIGN.md`
  §4).
- **The key budget is shared.** `SM1_MAX_KEYLEN` (4096) bounds
  prefix, index columns and `rowno` together. sm2's encoder
  enforces it: an over-long primary key refuses the row, and an
  over-long index key is truncated, flagged and rechecked
  against the tuple, never hashed (`sm2/SM2_DESIGN.md` §3.2). sm3
  owns the recheck and the unique check that follow from a cut
  key (`sm3/SM3_DESIGN.md` §3).
- **Unique checks are sm3's, and NULLs are distinct in them.**
  Two NULLs encode to the same bytes, so the encoding cannot
  say they differ; sm3's unique check skips any entry with a
  NULL key column, as SQL requires by default. `NULLS NOT
  DISTINCT` is refused at `CREATE INDEX` in v1; adding it
  later is a catalog flag and changes no bytes on disk
  (`sm3/SM3_DESIGN.md` §3).
- **Collation versions are pinned, recorded and enforced across
  the stack.** A specific ICU is linked into the server; sm3
  records each collation's version with the schema
  (`sm3/SM3_DESIGN.md` §2); sm2 refuses to read or write an index
  built under another version, the primary key's included, until
  an explicit online rebuild (`sm2/SM2_DESIGN.md` §3.3).
  Non-deterministic collations are refused in v1.
- **Constraints and labels are sm3's; sm2 stores what it is
  given.** The `varchar(n)` length limit is enforced by sm3,
  and an enum is `int4` to sm2 with its labels in sm3's
  catalog (`sm2/SM2_DESIGN.md` §2).
- **Batch-shaped API: a question for sm3, not sm2.** Sets of
  rows in and out was forced by an IPC boundary between a
  PostgreSQL backend process and sm1; the fork now moves
  PostgreSQL to threads (`PART4.md` §2), so the boundary is
  a function call and the shape is no longer forced. sm2 is
  tuple-at-a-time (`sm2/OBJECTIVE.md`); whether sm3 batches
  above it, for filter pushdown and fewer calls per scan, is
  open and belongs where the filters are
  (`sm3/SM3_DESIGN.md` §5).
- **READ COMMITTED writes wait, then proceed.** PostgreSQL's
  default level does not fail a concurrent update of the same
  row, so sm1's first-committer-wins rule is relaxed there
  (`sm1/SM1_DESIGN.md` §6.2). It needs a new version ordering in
  sm1 and a row re-check in the executor.
- **One commit point.** The catalog lives in sm3, in memory
  and written through to sm2, so a DDL statement and the data
  it changes commit in one sm1 transaction, and recovery is
  RocksDB's alone (`PART4.md` §4, `sm3/SM3_DESIGN.md` §2).
- **READ COMMITTED writes are resolved at sm3, not sm2.**
  `SM1_ERR_UPDATED` is answered by re-checking the
  statement's filter against the newest image, and filters
  exist only at sm3. sm2 surfaces the error and offers
  `rel_read_latest`; sm3 does the re-check
  (`sm3/SM3_DESIGN.md` §3).
- **No backward scans, at any level, ever.** Settled in
  `sm1/OBJECTIVE.md`, built into sm2's encoding and
  inherited by every index sm3 builds. The fork's planner is
  changed never to ask for a backward scan (`PART4.md` §5);
  descending order comes from inverted key bytes, scanned
  forward.

---

## 6. What this document is not

It is not a substitute for `sm1/SM1_DESIGN.md`,
`sm2/SM2_DESIGN.md` or `sm3/SM3_DESIGN.md` — read those for how a
level works. It is
not a restatement of `PART4.md` — read that for the
PostgreSQL target in full. It exists so a reader can see the
whole stack, and the handful of decisions that bind it
together, without first reading either level end to end.
