# Questions

## 1. The hand-written partition: what it keeps, and what it frees (2026-11-01)

Asked while working unattended on `sm/NEXT.md` §1 item 4's last
step, the lock table's own partition (`sm/sm1/src/locktab.h`).
Branch `wip-locktab-1`, from `main` at 42283a71.

**Recommended, and built on this branch:**

- **Entries from a free list, capped.** A dropped entry goes on
  its partition's free list, up to 1,024 of them; past that it is
  freed. A lock in a steady workload then allocates nothing under
  the mutex, and a transaction of a million locks leaves at most
  1,024 entries a partition behind it, not a million.
- **Three holders inline**, the rest in an array of the entry's
  own, so an entry is two cache lines.
- **A power-of-two bucket array**, indexed by the id's `fmix64`
  hash, that grows and shrinks by moving a few buckets at each
  add and drop into a new array, never all at once.
- **No count of holders by mode yet.** A conflict check must
  still find the asker's own hold among the holders, a walk, so
  the counts would save only the second walk; measured first.

**The alternative**, as `NEXT.md` words it: a pool that grows by
blocks and is never given back, as PostgreSQL's `dynahash` in
shared memory, sized once. It never frees under the mutex, but a
transaction of a million locks keeps their memory for the life of
the database.
