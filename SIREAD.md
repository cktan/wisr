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

See `DESIGN.md` §7.1–7.3.

1. T1's reads leave SIREAD entries on `alice` and `bob`, and so
   do T2's.
2. When T2 writes `bob`, it finds T1's SIREAD on `bob` and
   records an rw-edge T1 → T2: T1 read something T2 overwrote.
3. When T1 writes `alice`, it finds T2's SIREAD there and records
   T2 → T1.
4. Each transaction now has an edge coming in and an edge going
   out; that transaction is called the *pivot*. Any cycle in the
   serialization graph contains a pivot, so the engine aborts one
   of them with `SM_ERR_SERIALIZATION`.

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
- **Only SERIALIZABLE transactions create them.** Writes at every
  isolation level still check against them (§7.4).
- **Read-only transactions free them early.** Once such a
  transaction commits and its snapshot is proven *safe*, its
  entries can go (the safe-snapshot rule in §7.4).
- **They don't survive a restart.** They are kept only in memory,
  which is why §8 has to handle prepared transactions losing
  their SIREAD entries after a crash.

## The tradeoff

As §7.3 puts it, SSI never takes blocking read locks and never
deadlocks on reads. In exchange it sometimes aborts a transaction
that would in fact have been serializable.
