# DESIGN — wisr

How the stack in `OBJECTIVE.md` fits together: the levels,
the build, and the decisions that cross level boundaries.
Each level's own mechanics — sm1's MVCC, sm2's record format
— are argued in that level's own `DESIGN.md`; this document
does not repeat them.

---

## 1. The stack

```
        ┌────────────────────────────────────────────┐
        │  sm2/  records and indexes                  │
        │  types · record format · catalog ·          │
        │  secondary indexes · scan-by-predicate       │
        └────────────────────┬─────────────────────────┘
                              │  sm1/include/sm.h
        ┌────────────────────┴─────────────────────────┐
        │  sm1/  MVCC storage manager                  │
        │  relations · transactions · isolation ·      │
        │  locks · 2PC · recovery · GC                 │
        └────────────────────┬─────────────────────────┘
                              │  RocksDB's own API
        ┌────────────────────┴─────────────────────────┐
        │  RocksDB — ordered, durable, crash-atomic     │
        │  key-value store                              │
        └────────────────────────────────────────────────┘
```

sm2 is a client of sm1, nothing more: it holds no RocksDB
handle of its own and reaches storage only through `sm_*`
calls. That boundary is why `sm2/OBJECTIVE.md` can promise
that sm2 "must be useful and testable on its own" — the
whole lower stack is one dependency, sm1's public header.

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
a Makefile to delegate to; `sm2` joins it when it has one.
Adding a level to the stack means adding its name there,
nowhere else.

---

## 3. Status

| Level | Status |
| --- | --- |
| sm1 | built and tested; the design of record is `sm1/DESIGN.md` |
| sm2 | designed, not yet implemented — every open section of `sm2/DESIGN.md` is marked "to be written"; `sm2/PLAN.md` has the build order |

sm1 was built first and alone, on purpose: it is usable
without sm2 (an MVCC key-value store is a complete product
by itself), and every sm2 decision needs sm1's real API and
real behavior to check itself against, not a sketch of one.

---

## 4. Testing across levels

Every level uses the same two-layer scheme, established in
`sm1/mvcc/DRIVER.md`:

- ordinary tests for contract and single-session mechanics —
  C++ unit tests in each level's `test/`.
- a **declarative scenario suite** for anything where two
  sessions interleave — the isolation and concurrency
  questions that read as unreadable imperative test code.

`sm2/PLAN.md` plans to reuse this rather than invent a
second scheme, and defers only the question of whether sm2
needs a driver of its own or can extend sm1's — "a decision
for the first scenario that needs it, not before."

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
- **Object multiplexing.** sm1 targets hundreds to low
  thousands of relations (`sm1/DESIGN.md` §13); sm2 maps
  many logical tables and indexes onto few sm1 relations by
  prefixing its own object id into the key
  (`sm2/DESIGN.md` §6). This is why `sm_rmrel`'s O(1) column
  family drop is not what a `DROP TABLE` costs at sm2 — that
  trade is made explicitly there, not assumed here.
- **Batch-shaped API at sm2, row-shaped at sm1.** sm2's calls
  take and return sets of rows (`sm2/OBJECTIVE.md`). That was
  forced by an IPC boundary between a PostgreSQL backend
  process and sm1; the fork now moves PostgreSQL to threads
  (`PART3.md` §2), so the boundary is a function call and the
  shape is no longer forced. Whether sm2 keeps it, for filter
  pushdown and fewer calls per scan, is open.
- **No backward scans, at any level, ever.** Settled in
  `sm1/OBJECTIVE.md` and inherited by every index sm2
  builds. A caller that needs descending order keeps a
  second relation with inverted key bytes and scans it
  forward.

---

## 6. What this document is not

It is not a substitute for `sm1/DESIGN.md` or
`sm2/DESIGN.md` — read those for how a level works. It is
not a restatement of `PART3.md` — read that for the
PostgreSQL target in full. It exists so a reader can see the
whole stack, and the handful of decisions that bind it
together, without first reading either level end to end.
