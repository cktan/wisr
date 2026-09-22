# DESIGN — wisr

How the stack in `OBJECTIVE.md` fits together: the levels,
the build, and the decisions that cross level boundaries.
Each level's own mechanics — sm1's MVCC, sm2's tuple
format, sm3's catalog — are argued in that level's own
`DESIGN.md`; this document does not repeat them.

---

## 1. The stack

```
        ┌──────────────────────────────────────────────┐
        │  sm3/  schema and indexes                    │
        │  catalog · secondary indexes ·               │
        │  scan-by-predicate · statistics              │
        └────────────────────┬─────────────────────────┘
                             │  sm2's tuple relation
        ┌────────────────────┴─────────────────────────┐
        │  sm2/  types and tuples                     │
        │  type system · key encoding ·                │
        │  tuple format · relation of tuples         │
        └────────────────────┬─────────────────────────┘
                             │  sm1/include/sm.h
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
handle of its own and reaches storage only through `sm_*`
calls. sm3 stands in the same relation to sm2 — it never
encodes a key or decodes a tuple itself — with one
deliberate exception: `sm_lock`, which sm2 has nothing to
add to and which sm3's DDL and index-build protocols are
built on (`sm3/DESIGN.md` §2). That boundary is why each
level's `OBJECTIVE.md` can promise it is "useful and
testable on its own" — the whole lower stack is one
dependency.

---

## 2. Build

The top-level `Makefile` only delegates; each level has a
real one.

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
| sm1 | built and tested; the design of record is `sm1/DESIGN.md` |
| sm2 | designed, not yet implemented — every open section of `sm2/DESIGN.md` is marked "to be written"; `sm2/PLAN.md` has the build order |
| sm3 | designed, not yet implemented — the same, in `sm3/DESIGN.md` and `sm3/PLAN.md`; it cannot start before sm2's two formats are settled |

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

- **The `sm_*` C API is the only channel between sm1 and
  sm2.** sm2 may not read RocksDB directly, and may not
  depend on sm1's internals — the `SECINDEX.md`-style gap
  analysis in `sm1/` is how a missing primitive gets added
  to sm1's surface instead of worked around from outside it.
- **One encoder, one tuple format, two callers.** The key
  encoding and the tuple layout are sm2's alone
  (`sm2/DESIGN.md` §3, §4), and sm3 builds its index entries
  — `encode(sk) ‖ encode(pk)` — by calling sm2's encoder
  with the same per-column arguments. Two encoders that have
  to agree bytewise is a corruption bug waiting to be
  written, so there is only one.
- **sm3 reaches sm1 for locks, and for nothing else.** sm2
  has nothing to add to `sm_lock`, and sm3's DDL and
  index-build protocols rest on it (`sm3/DESIGN.md` §2), so
  that one call is the sanctioned exception to sm3 speaking
  only to sm2. Every other sm1 facility sm3 needs arrives
  through sm2's tuple relation.
- **Object multiplexing.** sm1 targets hundreds to low
  thousands of relations (`sm1/DESIGN.md` §13); sm3 maps
  many logical tables and indexes onto few sm1 relations by
  prefixing its own object id into the key
  (`sm3/DESIGN.md` §4). sm2 provides the hook — a `rel` is
  opened with an opaque prefix it prepends and strips and
  never interprets (`sm2/DESIGN.md` §5) — without learning
  what a table is. This is why `sm_rmrel`'s O(1) column
  family drop is not what a `DROP TABLE` costs at sm3 — that
  trade is made explicitly there, not assumed here.
- **Batch-shaped API: a question for sm3, not sm2.** Sets of
  rows in and out was forced by an IPC boundary between a
  PostgreSQL backend process and sm1; the fork now moves
  PostgreSQL to threads (`PART4.md` §2), so the boundary is
  a function call and the shape is no longer forced. sm2 is
  tuple-at-a-time (`sm2/OBJECTIVE.md`); whether sm3 batches
  above it, for filter pushdown and fewer calls per scan, is
  open and belongs where the filters are
  (`sm3/DESIGN.md` §5).
- **READ COMMITTED writes wait, then proceed.** PostgreSQL's
  default level does not fail a concurrent update of the same
  row, so sm1's first-committer-wins rule is relaxed there
  (`sm1/DESIGN.md` §6.2). It needs a new version ordering in
  sm1 and a row re-check in the executor.
- **One commit point.** The catalog lives in sm3, in memory
  and written through to sm2, so a DDL statement and the data
  it changes commit in one sm1 transaction, and recovery is
  RocksDB's alone (`PART4.md` §4, `sm3/DESIGN.md` §2).
- **READ COMMITTED writes are resolved at sm3, not sm2.**
  `SM_ERR_UPDATED` is answered by re-checking the
  statement's filter against the newest image, and filters
  exist only at sm3. sm2 surfaces the error and offers
  `rel_read_latest`; sm3 does the re-check
  (`sm3/DESIGN.md` §3).
- **No backward scans, at any level, ever.** Settled in
  `sm1/OBJECTIVE.md`, built into sm2's encoding and
  inherited by every index sm3 builds. The fork's planner is
  changed never to ask for a backward scan (`PART4.md` §5);
  descending order comes from inverted key bytes, scanned
  forward.

---

## 6. What this document is not

It is not a substitute for `sm1/DESIGN.md`,
`sm2/DESIGN.md` or `sm3/DESIGN.md` — read those for how a
level works. It is
not a restatement of `PART4.md` — read that for the
PostgreSQL target in full. It exists so a reader can see the
whole stack, and the handful of decisions that bind it
together, without first reading either level end to end.
