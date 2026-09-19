# SECINDEX — building secondary indexes on sm.h

An assessment of what the `sm_*` API (`include/sm.h`) lacks
for a layer that maintains secondary indexes on top of it.
OBJECTIVE.md leaves secondary indexes to such a layer: "separate
relations maintained in the same transaction".

## The pattern, and what already works

A secondary index is its own relation: `sk → pk` for a unique
index, `encode(sk) ‖ pk → ""` for a non-unique one. The layer
writes base and index rows in one transaction, so commit and
abort cover both.

- **Unique enforcement.** `sm_insert` on the index relation
  gives `SM_ERR_DUPKEY`; a concurrent inserter of the same key
  blocks on the row lock and then fails with `SM_ERR_CONFLICT`
  (§5.1, §5.2).
- **Halloween protection.** A cursor's view is fixed at open,
  so updating the indexed column while scanning the index does
  not revisit moved entries (§10.2).
- **Scan index, then fetch base.** `sm_read` does not
  invalidate a cursor's output, which lives until the next call
  on that cursor.
- **Compensation.** A non-retryable error leaves the
  transaction active and unchanged, so a base insert followed
  by an index `SM_ERR_DUPKEY` can be undone by hand.

## Gaps, in order of severity

### 1. Online index build on a populated relation

The builder scans the base relation, fills the index, and
commits a row in the layer's index catalog. A concurrent
writer whose snapshot predates that commit does not know the
index exists: it modifies the base relation and writes no
index entry. PostgreSQL prevents this with a ShareLock on the
table, or with CREATE INDEX CONCURRENTLY, which waits out older
transactions. Neither is expressible here:

- Row locks are exclusive only (§5.1). There is no shared or
  intent mode, and a per-relation "schema row" that every
  writer writes would serialize all writers on that relation.
- SSI covers this only if every writer is SERIALIZABLE. RC and
  RR transactions take no SIREAD locks and are never
  serialization victims (§7.4), so the writer → builder
  rw-edge never forms and no cycle is detected.
- No primitive waits for "all transactions begun before now"
  to finish, and no snapshot or commit timestamp is exposed.

Needed: one of `sm_lock_rel(session, rel, SHARED|EXCLUSIVE)`,
shared-mode key locks, or a wait-for-older-transactions-to-drain
primitive. The last is the smallest addition and suffices for
a CIC-style multi-phase build.

This is the only gap that makes correct behaviour impossible
rather than merely awkward or slow.

### 2. A single savepoint

Index maintenance turns one logical row write into several
physical writes. The layer needs a savepoint per statement so
that a unique violation on the third row of a multi-row insert
rolls back the statement's partial writes. With one savepoint
per transaction (`sm_savepoint` replaces the previous mark),
the statement savepoint clobbers any user-level SAVEPOINT.
Manual compensation works but leaves a tombstone version per
undone write.

Needed: nested savepoints, or at least a statement-level
savepoint distinct from the user's.

### 3. No statement snapshot across calls under READ COMMITTED

Under RC each `sm_read` and each `sm_scan` takes a fresh
statement snapshot (§6.2). An index lookup is one scan plus N
base reads, so it sees N+1 snapshots. The layer must recheck
its predicate against each fetched base row, and a single
statement in the layer, spanning many `sm_*` calls, cannot see
one consistent snapshot.

Needed: explicit statement boundaries, e.g.
`sm_stmt_begin(session)` pinning the RC snapshot until the next
boundary.

### 4. Forward-only scans

`MAX()`, `ORDER BY … DESC`, and "greatest key ≤ x" (floor
lookup, e.g. the latest version at or before time t) all need
a backward scan. Storing an index with inverted bytes gives
descending order but then serves only that direction, and
finding the maximum key means scanning the whole range.

Needed: a direction flag on `sm_scan`.

### 5. No cursor reposition

Index nested-loop joins, IN-lists, and skip scans must close
and reopen a cursor per probe. Under RC every reopen also takes
a new snapshot, which compounds gap 3.

Needed: `sm_scan_seek(scan, key, klen, inclusive)`.

### 6. Minor: performance and ergonomics

- **No batched read.** Index-then-base costs N separate seeks;
  there is no MultiGet equivalent.
- **No old-image return.** Index maintenance needs the old row
  on update and delete, so each costs read, copy (the `sm_read`
  pointer dies on the next call), then write: two seeks and an
  allocation per row.
- **`sm_lsrel`.** Implemented in phase 1, with relations
  named by caller-chosen `uint64_t` relids: the layer will keep
  its own catalog relation regardless, but listing relations
  helps debugging and cleanup.
- **Key length.** `SM_MAX_KEYLEN` (4096) bounds `sk ‖ pk`
  together; encodings must be sized for it.
- **No lock-without-write.** `SELECT … FOR UPDATE` through an
  index has to write a dummy version to lock the base row.

## Summary

Gap 1 blocks correctness. Gaps 2 and 3 are required for sound
statement semantics once an index layer exists. Gaps 4 and 5
decide whether the indexes are fast.

## Closing the gaps

Proposals only. Nothing below is in `sm.h` yet; with `src/`
holding only `strlist.cc`, these are contract changes to
`sm.h`, `API.md`, and `DESIGN.md`, cheapest to make now.

### Gap 1 (and lock-without-write): `sm_lock`

```c
typedef enum { SM_LOCK_SHARED = 0, SM_LOCK_EXCLUSIVE = 1 } sm_lockmode_t;
int sm_lock(sm_session_t* s, uint64_t relid,
            const char* key, size_t klen, sm_lockmode_t mode);
```

PostgreSQL's `SELECT FOR SHARE` / `FOR UPDATE`. The call blocks
on a conflicting holder. Once the lock is held it runs the
§5.2 first-committer-wins check: a committed version of the key
newer than `T.snapshot_ts` fails the call with
`SM_ERR_CONFLICT`. The lock is held to end of transaction and
is released by an `sm_rollback` past it.

**Index-build protocol.** The layer's catalog keeps one schema
row per base relation, `"S"|rel`.

- **Every writer** calls `sm_lock(catalog, "S"|rel, SHARED)`,
  then reads the row to learn which indexes to maintain. The
  conflict check guarantees what it reads is current.
- **The builder** begins in READ COMMITTED and:
  1. Calls `sm_update` on the schema row. This takes the
     exclusive lock, waiting out in-flight writers and
     blocking new ones.
  2. Scans the base relation. Under RC the scan's snapshot is
     taken now, after the lock, so it sees every committed
     row.
  3. Fills the index and commits.
- **Blocked writers** then get the shared lock, fail the
  conflict check (the schema row committed after their
  snapshot), retry, and see the new index.

**Tradeoffs.**

- Writers are blocked for the whole build, as with
  PostgreSQL's plain `CREATE INDEX`, not `CONCURRENTLY`.
- Shared locks change the lock design. The lock table goes
  from exclusive-only to S/X with holder sets. Deadlocks can
  form through shared locks, including S→X upgrades, so
  §5.1's "cycles are only possible among writers" no longer
  holds. A prepared transaction must record its S locks for
  recovery to restore (§8.4, §9.1).
- The schema row is a hot key: every writer on the relation
  locks it, so its lock-table shard mutex is contended.
  Readers never touch it.
- The non-blocking alternative needs a wait-for-older-
  transactions primitive plus a CIC-style protocol. It admits
  stale entries when a writer's `sm_delete` of a not-yet-built
  entry gets `NOTFOUND`: harmless for a non-unique index whose
  readers recheck the base row, false duplicates for a unique
  one. Deferred until blocking builds hurt.

### Gap 2: a savepoint stack

```c
int sm_savepoint(sm_session_t* s);           /* push; returns depth >= 1, or -1 */
int sm_rollback(sm_session_t* s, int depth); /* undo to that mark; keeps it, drops deeper marks */
int sm_forget(sm_session_t* s, int depth);   /* forget that mark and deeper ones */
```

§6.5 already records positions in `T.undo`, `T.created`, and
`T.locks`; a savepoint is a saved set of those positions, and
many savepoints are a stack of them. The layer pushes one at
the start of each statement and forgets it at the end, above
any user savepoints. `sm_forget` releases no locks; it only
stops `sm_rollback` from returning to the mark.

**Tradeoff.** This breaks the current "one savepoint, calling
again replaces it" contract. SQL-style named savepoints can be
built on it in the layer.

### Gap 3: RC statement snapshots

- **Cheap option.** The layer requires REPEATABLE READ or
  SERIALIZABLE and never uses RC. No engine change.
- **Real fix.** `sm_stmt_begin(session)`: between two such
  calls, every RC read uses one pinned snapshot. Without it,
  RC keeps taking a snapshot per call. Writes still check the
  begin snapshot, so §6.2's invariant is unaffected.

Start with the cheap option; add `sm_stmt_begin` when RC is
needed.

### Gap 4: backward scans

A flags argument on `sm_scan`, e.g. `SM_SCAN_DESC`. Versions
of a key are stored newest first (`key|~xid|~seq`), so a
backward scan meets the oldest version first and must walk to
the start of the key's run before picking the visible one. It
also needs `SeekForPrev` to work with the §10.2 prefix-seek
setup. For SSI, the traversed range runs from the last key
examined up to `ekey`.

### Gap 5: cursor reposition

```c
int sm_scan_seek(sm_scan_t* scan, const char* key, size_t klen,
                 int inclusive);
```

Forward only. It reuses the iterator and the snapshot. For SSI
it registers the whole range from the start to the current
position, skipped part included: an over-approximation, which
is safe, and one range per cursor.

### Gap 6: minor items

- **`sm_lsrel`.** Done in phase 1: a scan of the `"R"` prefix
  in `rel.0`, which needs an active transaction since the
  answer depends on the snapshot (DESIGN.md §11).
- **Batched read, old-image return.** Pure performance; leave
  them until profiling asks.

### Order

`sm_lock` first (gap 1 and lock-without-write), then the
savepoint stack, then the rest as needed. One at a time:
update `sm.h`, `API.md`, and `DESIGN.md`, then review.
