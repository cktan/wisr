# SIREAD locks

A SIREAD lock is a **snapshot isolation read lock**. The name and
idea come from Cahill, Röhm and Fekete, "Serializable Isolation
for Snapshot Databases" (SIGMOD 2008). PostgreSQL adopted it for
its SERIALIZABLE level (Ports & Grittner, VLDB 2012), and it
appears in `pg_locks` as `SIReadLock`.

## It isn't really a lock

It never blocks anyone and nobody waits on it. It is a note that
says "transaction T read key k, or range [a, b]." Its only
purpose is to let a *later write* see that it conflicts with an
earlier read.

## Why it's needed

Snapshot isolation stops write–write conflicts but misses
read–write ones. Take two on-call doctors, where the rule is that
at least one stays on call:

```
T1: read on_call(alice), read on_call(bob) → both on call
T2: read on_call(alice), read on_call(bob) → both on call
T1: write alice = off
T2: write bob   = off
both commit  → nobody on call (write skew)
```

The two transactions wrote different keys, so SI sees no
conflict. What SI misses is that each one *read* something the
other one *wrote*.

## How SIREAD catches it

See `SM1_DESIGN.md` §7.1–7.3.

1. T1's reads leave SIREAD entries on `alice` and `bob`, and so
   do T2's.
2. When T2 writes `bob`, it finds T1's SIREAD on `bob` and
   records an rw-edge T1 → T2: T1 read something T2 overwrote.
3. When T1 writes `alice`, it finds T2's SIREAD there and records
   T2 → T1.
4. Each transaction now has an edge coming in and an edge going
   out; that transaction is called the *pivot*. Any cycle in the
   serialization graph contains a pivot, so the engine aborts it
   at commit with `SM1_ERR_SERIALIZATION`. Here both are pivots,
   so both can fail (see "What wisr stores").

## The SSI graph

SSI stands for **Serializable Snapshot Isolation**. The SSI graph
is the set of transactions the SSI tracker knows about, connected
by rw-antidependency edges. SIREAD entries are how those edges
get discovered.

### The theory behind it

Serializability theory models a set of transactions as a
**dependency graph** (Adya's DSG). Each transaction is a node,
and an edge T1 → T2 means T1 must come before T2 in any
equivalent serial order. There are three edge types:

| Edge | Meaning |
| --- | --- |
| ww | T1 wrote x, then T2 overwrote it |
| wr | T1 wrote x, then T2 read T1's version |
| **rw** | T1 read x, and concurrent T2 wrote a newer version T1 didn't see |

A history is serializable exactly when this graph has no cycle.

### Why SSI only tracks rw edges

Under snapshot isolation the ww and wr edges are already under
control. Row locks plus first-committer-wins (`SM1_DESIGN.md`
§5.2) keep concurrent writers of the same key apart, and a
snapshot never sees uncommitted data. The only edges that can
close a cycle are **rw** edges between concurrent transactions.
Fekete et al. (2005) proved that every cycle under SI contains
two consecutive rw edges:

```
T_in ──rw──▶ T_pivot ──rw──▶ T_out
```

So SSI doesn't search the graph for cycles, which would be
expensive. It watches for this *dangerous structure* and aborts
when it appears (§7.3). The check is conservative: not every
pivot is part of a real cycle, and those cases become the false
aborts.

### What wisr stores

- **Nodes.** Active transactions, plus committed ones that might
  still conflict with a live transaction (§7.5). A committed node
  can still gain edges.
- **Edges.** Created in two places (§7.2):
  - A **writer** finds a SIREAD entry on the key it wrote and
    adds reader → writer.
  - A **reader** skips a version written by a concurrent
    transaction and adds reader → writer (`on_skip`, §10.1).
- **Flags.** Each node has `in_conflict` and `out_conflict`,
  and a `doomed` bit.

wisr keeps only the flags, following Cahill et al.'s original
rules (`SM1_DESIGN.md` §7.3): an edge that would make an
already-committed transaction a pivot dooms the running
transaction completing it, and a transaction with both flags,
or doomed, fails at commit. PostgreSQL also keeps edge targets,
so it can check which end of a dangerous structure commits
first and abort fewer transactions. That refinement is a
possible later step; the flags alone are sound, just more
conservative.

In the on-call doctors example above, the graph is a two-node
cycle:

```
T1 read bob,   T2 wrote bob    →  T1 ──rw──▶ T2
T2 read alice, T1 wrote alice  →  T2 ──rw──▶ T1
```

Each transaction has both an incoming and an outgoing edge, so
each is a pivot, and with flags alone both fail at commit with
`SM1_ERR_SERIALIZATION`. PostgreSQL's refinement would fail only
the one committing second.

The graph lives only in memory and isn't rebuilt at restart.
That is why §8.4 has its `restored` rule: a prepared transaction
brought back after a crash has lost its edges and flags, so the
engine is conservative about it and dooms any serializable reader
that skips one of its versions.

## Details specific to this design

- **Ranges cover phantoms.** A scan records the key range it
  actually walked, not just the rows it found. A later insert
  anywhere in that range still creates an edge (§7.1).
- **Reads that find nothing count.** A point read that finds no
  row still records the key.
- **They outlive commit.** A committed reader can still be the
  source of an edge from a transaction that writes after it. The
  entries are dropped only when no active snapshot could still
  conflict with them.
- **Only SERIALIZABLE transactions create them or check them.**
  As in PostgreSQL, READ COMMITTED and REPEATABLE READ
  transactions skip SSI entirely, so serializability holds
  among SERIALIZABLE transactions only (§7.4).
- **Read-only transactions could free them early.** Once such a
  transaction commits and its snapshot is proven *safe*, its
  entries could go (the safe-snapshot rule in §7.4). Not
  implemented yet: they are kept like any other.
- **They don't survive a restart.** They are kept only in memory,
  which is why §8 has to handle prepared transactions losing
  their SIREAD entries after a crash.

## The tradeoff

As §7.3 puts it, SSI never takes blocking read locks and never
deadlocks on reads. In exchange it sometimes aborts a transaction
that would in fact have been serializable.
