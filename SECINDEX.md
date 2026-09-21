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

### 1. Online index build on a populated relation — CLOSED

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

`sm_lock` closes it with shared-mode key locks. The protocol
below is implemented and tested end to end: a builder holding
the schema row exclusive blocks new writers and waits out the
ones in flight, so its scan cannot miss their rows, and each
blocked writer then fails its conflict check, retries, and
reads the new schema.

This was the only gap that made correct behaviour impossible
rather than merely awkward or slow.

The drain primitive was the alternative — smaller, needing no
shared locks — but it wants a CIC-style multi-phase build in
the layer and still admits false duplicates for a unique
index. `sm_lock` was chosen because it also gives
lock-without-write, which gap 6 wanted anyway.

### 2. A single savepoint — CLOSED

Index maintenance turns one logical row write into several
physical writes. The layer needs a savepoint per statement so
that a unique violation on the third row of a multi-row insert
rolls back the statement's partial writes. With one savepoint
per transaction, that statement savepoint clobbered any
user-level SAVEPOINT. Manual compensation worked but left a
tombstone version per undone write.

Savepoints now nest: `sm_savepoint` returns a depth,
`sm_rollback` takes one, and `sm_forget` is `RELEASE
SAVEPOINT`. The layer pushes a mark at the start of each
statement and forgets it at the end, above whatever savepoints
its own caller holds.

### 3. No statement snapshot across calls under READ COMMITTED — CLOSED

Under RC each `sm_read` and each `sm_scan` took a fresh
statement snapshot (§6.2). An index lookup is one scan plus N
base reads, so it saw N+1 snapshots, and a single statement in
the layer, spanning many `sm_*` calls, could return a mixture
of committed states that never existed at any instant.

`sm_stmt_begin(session)` closes it: from the first read after
that call, every RC read in the session uses one snapshot,
until the next boundary or the end of the transaction. The
layer still rechecks its predicate against each fetched base
row — an index entry and its base row are now read at the same
snapshot, but the layer's own concurrent writes and a
transaction's own uncommitted changes still make the recheck
worth keeping.

### 4. Forward-only scans — settled, will not change

`MAX()`, `ORDER BY … DESC`, and "greatest key ≤ x" (floor
lookup, e.g. the latest version at or before time t) would all
be served by a backward scan. `OBJECTIVE.md` rules one out
permanently: no index serves a backward scan, so this is a
constraint the layer is built on, not a gap awaiting a fix.

The layer's recourse is an index with inverted key bytes. It
serves only that direction, so a column wanted both ways costs
two relations, and the cost of that is now the layer's to
carry.

### 5. No cursor reposition — CLOSED

Index nested-loop joins, IN-lists, and skip scans had to close
and reopen a cursor per probe. Under RC every reopen also took
a new snapshot, which compounded gap 3.

`sm_scan_seek(scan, key, klen, inclusive)` moves an open
cursor forward, keeping its iterator, its snapshot and its
bounds. With gap 3's `sm_stmt_begin` the pair covers the whole
complaint: one cursor per statement, one snapshot per
statement.

### 6. Minor: performance and ergonomics

- **No batched read.** Index-then-base costs N separate seeks;
  there is no MultiGet equivalent.
- **No old-image return.** Index maintenance needs the old row
  on update and delete, so each costs read, copy (the `sm_read`
  pointer dies on the next call), then write: two seeks and an
  allocation per row.
- **`sm_lsrel`.** Implemented in phase 1, where a relation is
  named by a `uint64_t` relid that `sm_mkrel` hands out: the
  layer will keep its own catalog relation regardless, but
  listing relations helps debugging and cleanup, and the lowest
  relid listed is how the layer finds that catalog again.
- **Key length.** `SM_MAX_KEYLEN` (4096) bounds `sk ‖ pk`
  together; encodings must be sized for it.
- **No lock-without-write.** `SELECT … FOR UPDATE` through an
  index has to write a dummy version to lock the base row.

## Summary

Every gap is settled. Gaps 1, 2, 3 and 5 are closed:
`sm_lock`, the savepoint stack with `sm_forget`,
`sm_stmt_begin` and `sm_scan_seek` are all in `sm.h`. Gap 4
will not be closed: forward-only scanning is a decision, and
the layer works around it with inverted-byte relations. What
is left is gap 6's two performance items, which nothing has
asked for yet.

## Closing the gaps

What each gap became. Gaps 1, 2, 3 and 5 are implemented in
`sm.h`, `API.md`, `DESIGN.md` and `src/`, with tests. Gap 4
is a decision against. Gap 6's two performance items are the
only proposals still on paper.

### The whole roster

Every signature the gaps ask for, in one place. The sections
after this one explain each.

```c
/* gap 1: online index build, and lock-without-write -- IMPLEMENTED */
typedef enum { SM_LOCK_SHARED = 0, SM_LOCK_EXCLUSIVE = 1 } sm_lockmode_t;

int sm_lock(sm_session_t* session, uint64_t relid,
            const char* key, size_t klen, sm_lockmode_t mode);

/* gap 2: savepoint stack -- IMPLEMENTED */
int sm_savepoint(sm_session_t* session);            /* push; returns depth >= 1, or -1 */
int sm_rollback(sm_session_t* session, int depth);  /* undo to that mark; keeps it, drops deeper */
int sm_forget(sm_session_t* session, int depth);    /* forget that mark and deeper ones */

/* gap 3: RC statement snapshots -- IMPLEMENTED */
int sm_stmt_begin(sm_session_t* session);

/* gap 5: cursor reposition -- IMPLEMENTED */
int sm_scan_seek(sm_scan_t* scan, const char* key, size_t klen,
                 int inclusive);

/* gap 6: batched read -- the missing MultiGet equivalent */
int sm_mread(sm_session_t* session, uint64_t relid, size_t n,
             const char* const* key, const size_t* klen,
             const char** data, size_t* datalen, int* found);

/* gap 6: old-image return -- extra out-params on the existing writes */
int sm_update(sm_session_t* session, uint64_t relid,
              const char* key, size_t klen,
              const char* data, size_t datalen,
              const char** old, size_t* oldlen);

int sm_delete(sm_session_t* session, uint64_t relid,
              const char* key, size_t klen,
              const char** old, size_t* oldlen);
```

Gap 4 needs no function: it is closed. Gap 6's
lock-without-write is `sm_lock` again, which is now in, and
its `sm_lsrel` was already in phase 1.

**How firm each one is.** Everything but the gap 6 pair is
settled: `sm_lock`, the gap 2 trio, `sm_stmt_begin` and
`sm_scan_seek` are all in `sm.h` and `API.md`. The gap 6 pair
is sketched here and nowhere else -- that gap defers both
until profiling asks, so neither signature is settled.

**Three are breaking changes**, not additions: `sm_rollback`
gained a parameter, and `sm_update` and `sm_delete` would gain
two each.

**The old-image lifetime is unsettled.** That return cannot be
a new function; it has to alter `sm_update` and `sm_delete`
themselves. An `*old` pointing into session memory would
inherit `sm_read`'s validity window -- dead at the next `sm_*`
call on the session -- which is the copy the layer is trying
to avoid in the first place. Settle the lifetime before the
signature.

### Gap 1 (and lock-without-write): `sm_lock` — DONE

```c
typedef enum { SM_LOCK_SHARED = 0, SM_LOCK_EXCLUSIVE = 1 } sm_lockmode_t;
int sm_lock(sm_session_t* s, uint64_t relid,
            const char* key, size_t klen, sm_lockmode_t mode);
```

Implemented as proposed (DESIGN.md §5.1). What it cost, since
the estimate below was written as a guess: the lock table now
holds a mode and a holder list per key; the waits-for edges
became a graph and deadlock detection a depth-first search
with a visited set; a shared holder that writes its key
upgrades in place; the `PREPARED` record carries a mode byte
per lock key, which bumped the database format to version 4;
and the read-only commit path had to start releasing locks,
since a transaction with no writes can now hold them.

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

**Tradeoffs, as they turned out.**

- Writers are blocked for the whole build, as with
  PostgreSQL's plain `CREATE INDEX`, not `CONCURRENTLY`.
- Shared locks changed the lock design, as expected. The lock
  table went from exclusive-only to S/X with holder lists,
  deadlocks can now form through shared locks and S→X
  upgrades, so §5.1's "cycles are only possible among
  writers" no longer holds, and a prepared transaction
  records each lock's mode for recovery to restore (§8.4,
  §9.1).
- The schema row is a hot key: every writer on the relation
  locks it, so its lock-table mutex is contended. Readers
  never touch it. The table is still one mutex, not sharded,
  so this is the first place a benchmark will point at.
- The non-blocking alternative needs a wait-for-older-
  transactions primitive plus a CIC-style protocol. It admits
  stale entries when a writer's `sm_delete` of a not-yet-built
  entry gets `NOTFOUND`: harmless for a non-unique index whose
  readers recheck the base row, false duplicates for a unique
  one. Deferred until blocking builds hurt.

### Gap 2: a savepoint stack — DONE

```c
int sm_savepoint(sm_session_t* s);           /* push; returns depth >= 1, or -1 */
int sm_rollback(sm_session_t* s, int depth); /* undo to that mark; keeps it, drops deeper marks */
int sm_forget(sm_session_t* s, int depth);   /* forget that mark and deeper ones */
```

Implemented as proposed (DESIGN.md §6.5). §6.5 already
recorded positions in `T.undo`, `T.created`, and `T.locks`; a
savepoint is a saved set of those positions, and the stack is
a vector of them. `sm_forget` releases no locks; it only stops
`sm_rollback` from returning to the mark, and a lock taken
inside a forgotten mark is then held for the enclosing one.

**It broke the old contract**, as expected: `sm_rollback`
takes a depth now, and "calling `sm_savepoint` again replaces
the mark" is gone. SQL-style named savepoints are built on it
in the layer.

**A depth is reused** once its mark is gone, so a stale depth
names a different mark rather than failing. An id per mark,
never reused, would have caught that; depth won because SQL
already obliges the layer to drop a name when its mark goes,
so a correct caller cannot form a stale depth. The hazard is
stated in `sm.h` and `API.md`.

**The stack is capped** at `SM_MAX_SAVEPOINTS` (256), so
runaway nesting is an `SM_ERR_INVALID` rather than unbounded
growth. A layer that forgets each statement's mark never
approaches it.

### Gap 3: RC statement snapshots — DONE

`sm_stmt_begin(session)` is implemented (DESIGN.md §6.2). Two
decisions in it are worth knowing, because they are what the
layer relies on.

**The snapshot is taken by the statement's first read**, not
by the boundary call and not by a write. A statement may
therefore lock, wait, or write first and still read everything
committed while it waited — which is what gap 1's index-build
protocol needs, since it locks the schema row and only then
scans. Taking it eagerly at the boundary call would have been
simpler to reason about, but it would have pinned a pre-lock
snapshot and the builder would have missed the commits it
waited out.

**There is no `sm_stmt_end`.** The next `sm_stmt_begin` ends
the last statement, and the transaction ends the last of all.
The cost is that a transaction which uses the call once reads
that way for the rest of its life, with no way back to a
snapshot per call; the gain is one call instead of a pair to
keep balanced across rollback and prepare.

Writes still check the begin snapshot, so §6.2's invariant is
unaffected, and the GC watermark needed no change: an RC
transaction already registers its begin snapshot for its whole
life, which sits at or below any statement snapshot.

The cheap option remains available and needs no engine
support: a layer can require REPEATABLE READ or SERIALIZABLE
and never use RC at all.

### Gap 4: backward scans — closed as won't-do

No proposal. `OBJECTIVE.md` settles this one against a
direction flag, and the engine cost is part of why: versions
of a key are stored newest first (`key|~xid|~seq`), so a
backward scan meets the oldest version first and must walk to
the start of each key's run before picking the visible one; it
needs `SeekForPrev` to work with the §10.2 prefix-seek setup;
and for SSI the traversed range would run from the last key
examined up to `ekey`, inverting the bookkeeping in §10.2. A
layer wanting descending order builds an inverted-byte
relation instead.

### Gap 5: cursor reposition — DONE

```c
int sm_scan_seek(sm_scan_t* scan, const char* key, size_t klen,
                 int inclusive);
```

Implemented as proposed (DESIGN.md §10.2). Forward only: it
reuses the iterator and the snapshot, and for SSI the range
runs from the start to the furthest position reached, skipped
part included — an over-approximation, which is safe, and one
range per cursor.

Two refusals the proposal did not spell out, both forced by
that range bookkeeping:

- **A target below `skey` fails.** The iterator carries an
  upper bound but no lower one, so a seek under `skey` would
  return rows outside the predicate the cursor registered.
- **A cursor at its end is not revived.** §7.1 counts a scan
  that reached the end as having read its whole range;
  repositioning it and closing it later would narrow that
  range to the last key examined, recording less than was
  read.

Backward seeking was considered, for probes that do not
arrive in key order. It needs `last_examined` to become a
high-water mark and a flag for a cursor that once hit the
end, or the predicate silently shrinks. Forward-only costs
the layer a sort of its probe keys, which a batched index
join does anyway.

### Gap 6: minor items

- **`sm_lsrel`.** Done in phase 1: a scan of the `"R"` prefix
  in `rel.0`, which needs an active transaction since the
  answer depends on the snapshot (DESIGN.md §11).
- **Batched read, old-image return.** Pure performance; leave
  them until profiling asks. Signatures sketched in the roster
  above, with the lifetime question the old image raises.

### Order

Gaps 1, 2, 3 and 5 are done, and gap 4 will not be. Gap 6's
two performance items are what remains, each when profiling
asks for it: update `sm.h`, `API.md`, and `DESIGN.md`,
implement it in `src/` with tests, then review.
