# SECINDEX — building secondary indexes on sm1.h

An assessment of what the `sm1::` API (`include/sm1.h`) lacks
for a layer that maintains secondary indexes on top of it.
SM1_OBJECTIVE.md leaves secondary indexes to such a layer:
"separate stores maintained in the same transaction".

## The pattern, and what already works

A secondary index is its own store: `sk → pk` for a unique
index, `encode(sk) ‖ pk → ""` for a non-unique one. The layer
writes base and index rows in one transaction, so commit and
abort cover both.

- **Unique enforcement.** `sm1::insert` on the index store
  gives `SM_ERR_DUPKEY`; a concurrent inserter of the same key
  blocks on the row lock and then fails with `SM_ERR_CONFLICT`
  (§5.1, §5.2).
- **Halloween protection.** A cursor's view is fixed at open,
  so updating the indexed column while scanning the index does
  not revisit moved entries (§10.2).
- **Scan index, then fetch base.** `sm1::read` does not
  invalidate a cursor's output, which lives until the next call
  on that cursor.
- **Compensation.** A non-retryable error leaves the
  transaction active and unchanged, so a base insert followed
  by an index `SM_ERR_DUPKEY` can be undone by hand.

## Gaps, in order of severity

### 1. Online index build on a populated store — CLOSED

The builder scans the base store, fills the index, and
commits a row in the layer's index catalog. A concurrent
writer whose snapshot predates that commit does not know the
index exists: it modifies the base store and writes no
index entry. PostgreSQL prevents this with a ShareLock on the
table, or with CREATE INDEX CONCURRENTLY, which waits out older
transactions. Neither is expressible here:

- Row locks are exclusive only (§5.1). There is no shared or
  intent mode, and a per-store "schema row" that every
  writer writes would serialize all writers on that store.
- SSI covers this only if every writer is SERIALIZABLE. RC and
  RR transactions take no SIREAD locks and are never
  serialization victims (§7.4), so the writer → builder
  rw-edge never forms and no cycle is detected.
- No primitive waits for "all transactions begun before now"
  to finish, and no snapshot or commit timestamp is exposed.

`sm1::lock` closes it with shared-mode key locks. The protocol
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
index. `sm1::lock` was chosen because it also gives
lock-without-write, which gap 6 wanted anyway.

### 2. A single savepoint — CLOSED

Index maintenance turns one logical row write into several
physical writes. The layer needs a savepoint per statement so
that a unique violation on the third row of a multi-row insert
rolls back the statement's partial writes. With one savepoint
per transaction, that statement savepoint clobbered any
user-level SAVEPOINT. Manual compensation worked but left a
tombstone version per undone write.

Savepoints now nest: `sm1::savepoint` returns a depth,
`sm1::rollback` takes one, and `sm1::forget` is `RELEASE
SAVEPOINT`. The layer pushes a mark at the start of each
statement and forgets it at the end, above whatever savepoints
its own caller holds.

### 3. No statement snapshot across calls under READ COMMITTED — CLOSED

Under RC each `sm1::read` and each `sm1::scan` took a fresh
statement snapshot (§6.2). An index lookup is one scan plus N
base reads, so it saw N+1 snapshots, and a single statement in
the layer, spanning many `sm1::` calls, could return a mixture
of committed states that never existed at any instant.

`sm1::stmt_begin(session)` closes it: from the first read after
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
be served by a backward scan. `SM1_OBJECTIVE.md` rules one out
permanently: no index serves a backward scan, so this is a
constraint the layer is built on, not a gap awaiting a fix.

The layer's recourse is an index with inverted key bytes. It
serves only that direction. A second, mirror index for the
other direction was considered and rejected (`PART4.md` §5):
the stack builds a descending index column when one is
declared, and sorts when a query wants an order no index has.

### 5. No cursor reposition — CLOSED

Index nested-loop joins, IN-lists, and skip scans had to close
and reopen a cursor per probe. Under RC every reopen also took
a new snapshot, which compounded gap 3.

`sm1::scan_seek(scan, key, klen, inclusive)` moves an open
cursor forward, keeping its iterator, its snapshot and its
bounds. With gap 3's `sm1::stmt_begin` the pair covers the whole
complaint: one cursor per statement, one snapshot per
statement.

### 6. Minor: performance and ergonomics

- **No batched read.** Index-then-base costs N separate seeks.
  The MultiGet framing was wrong: a read here is an iterator
  seek across a key's version run (§10.1), not a point `Get`,
  so RocksDB's MultiGet does not apply. What a batched call
  would actually save is per-key iterator construction and
  read-epoch bookkeeping, plus seeking in sorted order. Left
  unbuilt until a benchmark shows that setup dominating a
  probe loop; it also needs an ownership model for N values,
  since the session has one read buffer.
- **No old-image return — CLOSED.** `sm1::update` and
  `sm1::erase` now return the row they replaced through
  out-parameters, NULL to skip. The claim that it cost two
  seeks was right about the layer and wrong about the engine:
  the write already seeks to the deciding version and decodes
  its value to check the key exists, so returning it costs a
  copy. The `sm1::read` lifetime rule applies to it, and it is
  the same buffer.
- **`sm1::lsstore`.** Implemented in phase 1, where a store is
  named by a `uint64_t` storeid that `sm1::mkstore` hands out:
  the layer will keep its own catalog store regardless, but
  listing stores helps debugging and cleanup, and the lowest
  storeid listed is how the layer finds that catalog again.
- **Key length.** `sm1::max_keylen` (4096) bounds `sk ‖ pk`
  together; encodings must be sized for it.
- **No lock-without-write.** `SELECT … FOR UPDATE` through an
  index has to write a dummy version to lock the base row.

## Summary

Every gap is settled. Gaps 1, 2, 3 and 5 are closed:
`sm1::lock`, the savepoint stack with `sm1::forget`,
`sm1::stmt_begin` and `sm1::scan_seek` are all in `sm1.h`, as is
gap 6's old-image return. Gap 4 will not be closed:
forward-only scanning is a decision, and the layer works
around it with inverted-byte stores. The batched read is
the one item left on paper, and it needs a benchmark rather
than an argument.

## Closing the gaps

What each gap became. Gaps 1, 2, 3, 5 and the old-image half
of gap 6 are implemented in `sm1.h`, `API.md`, `SM1_DESIGN.md`
and `src/`, with tests. Gap 4 is a decision against, and so, for
now, is the batched read.

### The whole roster

Every signature the gaps ask for, in one place. The sections
after this one explain each.

```cpp
namespace sm1 {

/* gap 1: online index build, and lock-without-write -- IMPLEMENTED */
enum class lockmode_t { shared = 0, exclusive = 1 };

int lock(session_t* session, uint64_t storeid,
         std::string_view key, lockmode_t mode);

/* gap 2: savepoint stack -- IMPLEMENTED */
int savepoint(session_t* session);            /* push; returns depth >= 1, or -1 */
int rollback(session_t* session, int depth);  /* undo to that mark; keeps it, drops deeper */
int forget(session_t* session, int depth);    /* forget that mark and deeper ones */

/* gap 3: RC statement snapshots -- IMPLEMENTED */
int stmt_begin(session_t* session);

/* gap 5: cursor reposition -- IMPLEMENTED */
int scan_seek(scan_t* scan, std::string_view key, int inclusive);

/* gap 6: batched read -- NOT BUILT; see the gap for why */
int mread(session_t* session, uint64_t storeid,
          std::span<const std::string_view> keys,
          std::vector<std::optional<std::string_view>>& data);

/* gap 6: old-image return -- IMPLEMENTED */
int update(session_t* session, uint64_t storeid,
           std::string_view key, std::string_view data,
           std::string_view* old = nullptr);

int erase(session_t* session, uint64_t storeid,
          std::string_view key, std::string_view* old = nullptr);

}
```

Gap 4 needs no function: it is closed. Gap 6's
lock-without-write is `sm1::lock` again, which is now in, and
its `sm1::lsstore` was already in phase 1.

**How firm each one is.** Everything but `sm1::mread` is
settled and in `sm1.h` and `API.md`. `sm1::mread` is sketched
here and nowhere else: that half of gap 6 waits on a
benchmark, so its signature is a guess, in particular how N
values would be owned.

**Three were breaking changes**, not additions: `sm1::rollback`
gained a parameter, and `sm1::update` and `sm1::erase` each
gained one, the optional `old`.

**The old-image lifetime is settled** the way the question
framed it: `*old` points into session memory and dies at the
next `sm1::` call, `sm1::read`'s window, because it is
`sm1::read`'s buffer. The layer copies only what must outlive
that call, and index maintenance usually does not -- it
derives the old entries and is done.

### Gap 1 (and lock-without-write): `sm1::lock` — DONE

```c
typedef enum { SM1_LOCK_SHARED = 0, SM1_LOCK_EXCLUSIVE = 1 } sm1_lockmode_t;
int sm1_lock(sm1_session_t* s, uint64_t storeid,
             const char* key, size_t klen, sm1_lockmode_t mode);
```

Implemented as proposed (SM1_DESIGN.md §5.1). What it cost, since
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
is released by an `sm1::rollback` past it.

**Index-build protocol.** The layer's catalog keeps one schema
row per base store, `"S"|store`.

- **Every writer** calls `sm1::lock(catalog, "S"|store, SHARED)`,
  then reads the row to learn which indexes to maintain. The
  conflict check guarantees what it reads is current.
- **The builder** begins in READ COMMITTED and:
  1. Calls `sm1::update` on the schema row. This takes the
     exclusive lock, waiting out in-flight writers and
     blocking new ones.
  2. Scans the base store. Under RC the scan's snapshot is
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
- The schema row is a hot key: every writer on the store
  locks it, so its lock-table mutex is contended. Readers
  never touch it. The table is still one mutex, not sharded,
  so this is the first place a benchmark will point at.
- The non-blocking alternative needs a wait-for-older-
  transactions primitive plus a CIC-style protocol. It admits
  stale entries when a writer's `sm1::erase` of a not-yet-built
  entry gets `NOTFOUND`: harmless for a non-unique index whose
  readers recheck the base row, false duplicates for a unique
  one. Deferred until blocking builds hurt.

### Gap 2: a savepoint stack — DONE

```c
int sm1_savepoint(sm1_session_t* s);           /* push; returns depth >= 1, or -1 */
int sm1_rollback(sm1_session_t* s, int depth); /* undo to that mark; keeps it, drops deeper marks */
int sm1_forget(sm1_session_t* s, int depth);   /* forget that mark and deeper ones */
```

Implemented as proposed (SM1_DESIGN.md §6.5). §6.5 already
recorded positions in `T.undo`, `T.created`, and `T.locks`; a
savepoint is a saved set of those positions, and the stack is
a vector of them. `sm1::forget` releases no locks; it only stops
`sm1::rollback` from returning to the mark, and a lock taken
inside a forgotten mark is then held for the enclosing one.

**It broke the old contract**, as expected: `sm1::rollback`
takes a depth now, and "calling `sm1::savepoint` again replaces
the mark" is gone. SQL-style named savepoints are built on it
in the layer.

**A depth is reused** once its mark is gone, so a stale depth
names a different mark rather than failing. An id per mark,
never reused, would have caught that; depth won because SQL
already obliges the layer to drop a name when its mark goes,
so a correct caller cannot form a stale depth. The hazard is
stated in `sm1.h` and `API.md`.

**The stack is capped** at `sm1::max_savepoints` (256), so
runaway nesting is an `SM_ERR_INVALID` rather than unbounded
growth. A layer that forgets each statement's mark never
approaches it.

### Gap 3: RC statement snapshots — DONE

`sm1::stmt_begin(session)` is implemented (SM1_DESIGN.md §6.2).
Two decisions in it are worth knowing, because they are what the
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

**There is no `sm1::stmt_end`.** The next `sm1::stmt_begin` ends
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

No proposal. `SM1_OBJECTIVE.md` settles this one against a
direction flag, and the engine cost is part of why: versions
of a key are stored newest first (`key|~wts|~seq`), so a
backward scan meets the oldest version first and must walk to
the start of each key's run before picking the visible one; it
needs `SeekForPrev` to work with the §10.2 prefix-seek setup;
and for SSI the traversed range would run from the last key
examined up to `ekey`, inverting the bookkeeping in §10.2. A
layer wanting descending order builds an inverted-byte
store instead.

### Gap 5: cursor reposition — DONE

```c
int sm1_scan_seek(sm1_scan_t* scan, const char* key, size_t klen,
                  int inclusive);
```

Implemented as proposed (SM1_DESIGN.md §10.2). Forward only: it
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

- **`sm1::lsstore`.** Done in phase 1: a scan of the `"R"` prefix
  in `store.0`, which needs an active transaction since the
  answer depends on the snapshot (SM1_DESIGN.md §11).
- **Old-image return — DONE.** Out-parameters on `sm1::update`
  and `sm1::erase`, NULL to skip, filled from the version the
  write already decoded (SM1_DESIGN.md §5.3). The lifetime
  question is answered by reusing `sm1::read`'s buffer and its
  rule: valid until the next `sm1::` call on the session,
  either call invalidating what the other returned. Both
  signatures changed, and every caller with them.
- **Batched read — left unbuilt, with reasons.** See the gap
  above: MultiGet does not apply to version runs, the real
  saving is per-key iterator setup, and N values need an
  ownership model the session's single buffer does not
  provide. Reopen it with a benchmark, not an argument.

### Order

Gaps 1, 2, 3, 5 and gap 6's old-image return are done; gap 4
will not be. The batched read is what remains, and it waits
on a benchmark. When one asks for it: update `sm1.h`,
`API.md`, and `SM1_DESIGN.md`, implement it in `src/` with tests,
then review.
