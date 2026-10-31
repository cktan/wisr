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
- **No count of holders by mode.** A conflict check must still
  find the asker's own hold among the holders, a walk; that walk
  now gathers the other holders' modes as bits too
  (`locktab_t::probe`), so the conflict check is one AND and no
  second walk is made, with nothing to keep in step with a
  conversion. On entries of about 16 holders: 502 instructions a
  lock, not 903.

**Measured** (`sm/NEXT.md` §1 item 4): 383 to 453 instructions a
lock and its release on one thread, against 615 to 970 in the
map's partitions; no allocation a lock but in `big`, a million
locks, whose entries outrun the free list; above the one-mutex
baseline at every thread count but `hot` at 2, 17% under.

**The alternative**, as `NEXT.md` words it: a pool that grows by
blocks and is never given back, as PostgreSQL's `dynahash` in
shared memory, sized once. It never frees under the mutex, but a
transaction of a million locks keeps their memory for the life of
the database.
