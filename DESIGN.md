
# DESIGN — wisr storage manager

A multi-version (MVCC) storage manager for OLTP, layered over RocksDB.
RocksDB is used only as an ordered, durable, crash-atomic key-value
store; every notion of *row version*, *transaction*, *isolation*, and
*visibility* is implemented here.

This document is the design of record. It follows the decisions in
`OBJECTIVE.md`:

| Decision | Choice |
| --- | --- |
| Isolation mechanism | Snapshot isolation everywhere; SERIALIZABLE via SSI (optimistic) |
| Uncommitted writes | Written eagerly into RocksDB, resolved via a commit-status table |
| `sm_prepare` | Real 2PC with a durable prepare record |
| Deployment | One process, many threads, one shared RocksDB instance |

---

## 1. Scope and non-goals

**In scope.** Relations as ordered key→value maps; MVCC reads and
scans; read-committed / repeatable-read / serializable transactions;
one savepoint per session; two-phase commit; crash recovery; garbage
collection of dead versions.

**Not in scope (for now).** Secondary indexes, a query language,
column types or schemas (values are opaque byte strings), replication,
multi-process or remote access, and online backup. Nothing in the
design forecloses them.

**Assumptions.**

- A `session_t` is used by one thread at a time. Concurrent use of one
  session from two threads is a caller error, not a supported mode.
- A process reaches its databases through an environment
  (`sm_env_t`) on a home directory; a database is the
  directory `{homedir}/{dbname}`. `sm_init` takes a pidlock
  on `homedir`, so one process, and one environment in it,
  owns the directory and every database under it; a lock
  left by a dead process does not block the next one.
  Multiple sessions may open the same database; they share
  one RocksDB handle, held in the environment's registry of
  open databases.
- Keys and values are arbitrary byte strings. Keys are ordered
  bytewise (`memcmp`), which is what `sm_scan` exposes.
- Implementation language is C++ (RocksDB's native API), exposing a C
  ABI for the `sm_*` surface.

---

## 2. Architecture

```
        ┌──────────────────────────────────────────────────────┐
        │  sm_* C API   (session, xct, rel, insert/read/scan)  │
        └───────────────────────┬──────────────────────────────┘
                                │
   ┌────────────┬───────────────┼───────────────┬──────────────┐
   │            │               │               │              │
┌──┴──────┐ ┌───┴──────┐ ┌──────┴──────┐ ┌──────┴─────┐ ┌──────┴─────┐
│ Catalog │ │ Access   │ │ Transaction │ │ Lock mgr   │ │ SSI        │
│ (relid  │ │ methods  │ │ manager     │ │ (row X     │ │ tracker    │
│ lookup) │ │ read /   │ │ ts, status, │ │  locks,    │ │ (SIREAD,   │
│         │ │ scan /   │ │ commit seq, │ │  deadlock  │ │  rw-edges, │
│         │ │ write    │ │ 2PC         │ │  detect)   │ │  pivots)   │
└─────────┘ └────┬─────┘ └──────┬──────┘ └────────────┘ └────────────┘
                 │              │
        ┌────────┴──────────────┴────────┐        ┌──────────────────┐
        │  Version codec (key/value fmt) │        │ GC / compaction  │
        └────────────────┬───────────────┘        │ filter           │
                         │                        └────────┬─────────┘
                 ┌───────┴────────────────────────────────┴────────┐
                 │                  RocksDB                        │
                 │  CF:rel.*  CF:txn   CF:meta   WAL   compaction  │
                 └─────────────────────────────────────────────────┘
```

The only durable state is inside RocksDB. Everything else — locks,
SSI graph, active-transaction table, the commit-timestamp cache — is
volatile and reconstructible (§9).

---

## 3. Physical layout

One column family per relation, plus two fixed ones: `CF:txn`
and `CF:meta`.

### 3.1 `CF:rel.<relid>` — row versions

Each relation's versions live in their own column family, named
by `relid` in decimal (`rel.0` is the catalog, §11).

```
key   = userkey (variable)
      | ~xid   (8B, big-endian, bitwise complement)
      | ~seq   (4B, big-endian, bitwise complement)

value = flags   (1B)   bit0 = tombstone (this version is a delete)
                       bit1 = commit_ts is stamped in this header
      | commit_ts (8B) valid only if flags bit1; a hint, see §8.3
      | payload  (variable, absent for a tombstone)
```

Notes on the encoding:

- **`relid`** is a stable 64-bit id assigned by the catalog, not
  the relation name, and it names the column family. Renames and
  drop/recreate are cheap, a relid is never reused (§4.2), and a
  dropped relation is removed with one `DropColumnFamily`. The
  key itself carries no relid; the column family is the relation.
- **Complemented `xid`** makes versions of a key sort **newest
  first**. A point read becomes one `Seek` to
  `userkey|~snapshot_ts|0x00000000`, which lands on the newest
  version whose `xid <= snapshot_ts` — usually the answer. A
  transaction that has written seeks from the larger of its own
  `xid` and its snapshot instead (§10.1).
- **`seq`** is a per-transaction write counter, starting at 1.
  It exists so that a transaction writing the same key twice
  creates two distinct versions, which is what makes savepoint
  rollback a pure delete (§6.5) instead of requiring an undo
  pre-image. Complemented so the latest write within a
  transaction sorts first.
- `userkey` is stored raw and the two suffixes are fixed-width, so a
  version key is parsed from the tail. No escaping needed.
- **Comparator.** Relation CFs use a custom comparator,
  `wisr.version.v1`: compare the `userkey` parts (key minus the
  12-byte tail) with `memcmp`, shorter-is-less on a tie, then
  compare the tails with `memcmp`. RocksDB's default bytewise
  comparator is wrong here. With it, a key's tail is compared
  against the extra bytes of a longer userkey that begins with
  it: the versions of `"a"` (`61 FF FF …`, since `~xid` of any
  xid below 2^56 starts with `FF`) sort after those of `"ab"`
  (`61 62 …`). Scans would return `"ab"` before `"a"`, and a
  point read could stop early on a foreign key. With the custom
  comparator, a key's versions are contiguous and userkeys
  sort in `memcmp` order, which is the order `sm_scan` exposes.

  Every key handed to RocksDB for a relation CF — writes, seek
  targets, iterator bounds — carries the full 12-byte tail.
  `FindShortestSeparator` and `FindShortSuccessor` are no-ops
  (safe; index blocks are a little larger). RocksDB persists the
  comparator name and refuses to open a CF with another, so the
  name is part of the format: changing the ordering means a new
  name and a rewrite. `CF:txn` and `CF:meta` keep the bytewise
  comparator.

Column family handles are held in a process-wide map
`relid → handle`, shared by every session open on the
database and guarded by a reader-writer lock, because relations
are created and dropped concurrently with reads. A handle is
reference-counted and destroyed only after its CF is dropped and
the last user releases it.

### 3.2 `CF:txn` — commit status table

```
key   = xid (8B, big-endian)
value = { state: PREPARED | COMMITTED | ABORTED,
          snapshot_ts, commit_ts,
          gid (global txn id string, 2PC only),
          prepared_at: epoch seconds           (PREPARED only),
          writeset: [ (relid, userkey) ... ]   (PREPARED only),
          created:  [ relid ... ]              (PREPARED only),
          dropped:  [ relid ... ]              (PREPARED only) }
```

Only **writing** transactions ever get an entry, plus read-only
ones that prepare (§8.4), and only at prepare/commit/abort time —
nothing is written at `sm_begin`. Absence of a record is
meaningful (§3.4).

The `writeset` is carried only in a `PREPARED` record, and only
so that recovery can re-acquire the row locks a prepared
transaction still holds. `created` lists the relations the
transaction made with `sm_mkrel`, so that recovery keeps their
column families (§9.1); `dropped` lists those it removed with
`sm_rmrel`, so that a commit after restart still queues their
column families for removal (§8.3). `prepared_at` is the
wall-clock time of `sm_prepare`, reported by `sm_list_prepared`
(§8.5); it must be durable because the listing is mostly needed
after a restart. All four are dropped when the record flips to
COMMITTED or ABORTED. For a prepared transaction with a very
large write set the list spills to additional keys
`xid | chunkno` in the same CF, deleted in the same batch as the
flip.

### 3.3 `CF:meta` — bootstrap state

Format version, and the durable high-water marks for the timestamp
and relid allocators (§4.2).

### 3.4 The status resolution rule

Given a version's `xid`, a reader determines the writer's fate as:

0. The version's header carries a stamped `commit_ts` (flags
   bit1, §3.1) → committed at that `commit_ts`. The stamp is
   written only by the compaction filter, only for committed
   versions (§8.3), so it is authoritative, and it must be
   checked first: once the `CF:txn` record is swept, it is the
   only evidence that the version committed.
1. `xid` is in the in-memory **active transaction table** → still
   running or prepared; not visible (unless it is the reader's own).
2. `xid` is in the in-memory **commit cache** → use its `commit_ts`.
3. Look up `CF:txn`. `COMMITTED` → `commit_ts`; `ABORTED` → dead.
4. **None of the above** → the writer never committed. Dead.

Step 4 is what makes crash recovery free of any undo pass: a
crashed transaction leaves versions in relation CFs with no
status record, and absence of a record *is* the abort. It holds
at runtime too — a transaction whose `ABORTED` hint was never
written leaves the same trace. A transaction whose *commit*
status write failed does not: that record may still be durable,
so the engine stops instead of judging it (§8.2). The process is
the only one with the database open, so the active transaction
table is exhaustive; two orderings keep a live transaction from
being misjudged:

- A transaction enters the active table no later than its xid
  allocation, before its first version reaches RocksDB.
- At commit, the status record is written first, then the xid
  enters the commit cache and leaves the active table, and only
  then is its `commit_ts` published (§8.2). A reader that misses
  it in the active table finds it in the cache or `CF:txn`; a
  reader that still finds it there cannot hold a snapshot that
  includes its commit.

The commit cache is bounded by `commit_cache_entries` and evicts
the oldest `commit_ts` first. Eviction cannot change an answer:
the status record was written before the insert, so a reader
that misses the cache finds it in `CF:txn`, and the record is
swept only once every version it covers is stamped (§8.3).

---

## 4. Timestamps, ids, and snapshots

### 4.1 One counter

A single monotonically increasing 64-bit atomic counter,
`global_ts`, produces both timestamps and transaction ids. A
transaction's **id is its first-write timestamp**: `xid == start_ts`.
Read-only transactions never allocate one.

- `sm_begin` takes a **snapshot**: `snapshot_ts = visible_ts` (§4.3).
  It does *not* burn a counter value.
- The first write in a transaction allocates `xid = global_ts++`.
  Because `snapshot_ts <= visible_ts < xid`, always `xid > snapshot_ts`.
- Commit allocates `commit_ts = global_ts++`, so `commit_ts > xid`.

**Invariant (version chain order).** For one key, committed versions in
descending `xid` order are also in descending `commit_ts` order.

*Why.* Two transactions that both write key `k` cannot overlap: the row
lock (§5) serializes them, and the later one aborts if the earlier
committed after its snapshot. So if `T2` committed a version of `k`
after `T1` did, `T2`'s snapshot was taken at or after `T1.commit_ts`,
hence `T2.xid > T2.snapshot_ts >= T1.commit_ts > T1.xid`. ∎

This is what lets a reader seek by `xid` and get commit order.

### 4.2 Surviving restart

The counter is volatile, so `CF:meta` holds `next_ts`, advanced
in **leases**: the allocator persists `global_ts + LEASE` (e.g.
1M) and only re-persists when the counter reaches the persisted
value. On startup, `global_ts` resumes one past the persisted
value — above any id ever handed out. `visible_ts` is set to the
persisted value itself, so every `xid` allocated after restart is
`> visible_ts`, keeping §4.1's `xid > snapshot_ts`. The persisted
value itself is never handed out. Relids are allocated the same
way.

### 4.3 `visible_ts`: making snapshots correct

A snapshot must not be able to see a *gap* — a transaction with
`commit_ts <= snapshot_ts` that has not yet published its status.
Rather than maintaining an in-flight-transaction list per snapshot
(Postgres-style), commits are **published in `commit_ts` order**:

```
commit sequencer (ordered set of commit_ts in progress)

  in progress: { 106 }          done: 104, 105, 107

  visible_ts = 105 — one below the smallest commit_ts in
               progress, so 107 stays hidden until 106 lands
```

- `sequencer.claim` allocates `commit_ts = global_ts++` and
  inserts it into the set under one mutex, and the advancer
  reads the set and `global_ts` under the same mutex. So no
  value can be handed out without being either in the set or
  above what the advancer saw.
- A committing transaction claims `commit_ts`, writes its status
  record (WAL-synced), then marks it done, removing it from the
  set. If the write fails, it is *not* marked done: the database
  fails stop (§8.2), and `visible_ts` stays below that
  `commit_ts` until restart.
- `global_ts` also produces xids (§4.1), but xids never enter the
  set, so only commits in progress can hold back `visible_ts`.
- A single lightweight advancer sets `visible_ts` to one below
  the smallest value in the set, or to `global_ts - 1` when the
  set is empty. New values are always above everything already
  handed out, so `visible_ts` never moves backwards.
- The set holds at most one entry per session, so it needs no
  sizing and no backpressure: a stalled commit holds back
  `visible_ts` but never blocks allocation. A ring of slots
  indexed by timestamp would not have that property: while one
  slot waits on a slow WAL sync, every xid and `commit_ts`
  allocated behind it needs a slot too, so the ring must either
  wrap onto unpublished slots or block all allocation.
- A snapshot is simply the current `visible_ts`. Everything with
  `commit_ts <= visible_ts` is durably committed and published.

The cost is that one slow commit briefly holds back new snapshots
(bounded by the WAL sync, and by group commit it is one sync for the
whole batch). The benefit is that a snapshot is a single integer:
visibility tests are integer comparisons, snapshots are free to take,
and there is no per-snapshot transaction list to copy or search.

### 4.4 Visibility test

Version `V` written by `xid`, read at `snapshot_ts` by transaction `T`:

```
if V.xid == T.xid:                      # own write
    visible iff V.seq <= T.seq_hwm      # savepoint-aware, §6.5
else:
    resolve V (§3.4)  →  commit_ts | dead | in-flight
    visible iff resolved COMMITTED and commit_ts <= snapshot_ts
```

A tombstone that is visible means *the key does not exist*.

---

## 5. Write path and write-write conflicts

Writes are eager: `sm_insert`/`sm_update`/`sm_delete` put a version
into RocksDB immediately, before commit.

```
sm_update(rel, k, v):
  1. relid  = catalog.lookup(rel)                       # MVCC read, §11
  2. lock   = lockmgr.acquire_x(relid, k, T)            # blocking, §5.1
  3. latest = newest own version of (relid, k) visible to T  # §4.4
     if latest is absent:
         latest = newest committed version of (relid, k)
         if latest.commit_ts > T.snapshot_ts:           # §5.2
             abort with SM_ERR_CONFLICT
     if latest is absent or tombstone: SM_ERR_NOTFOUND  # (insert: DUPKEY)
  4. if T.xid == 0: T.xid = global_ts++
  5. seq = T.seq++;  T.seq_hwm = seq                   # T.seq starts at 1
     db.Put(cf(relid), encode(k, ~T.xid, ~seq), encode(flags, v))
  6. T.undo.push(relid, k, seq)                         # for savepoint
  7. ssi.on_write(T, relid, k)                          # §7
```

Step 3 consults the transaction's own writes first, so a key it
inserted can be updated and a key it deleted can be re-inserted.
An own version still visible under `seq_hwm` means the row lock
has been held since that write, so no other transaction can have
committed `k` in between and the conflict check is skipped. Own
versions undone by `sm_rollback` are invisible (§6.5), and the
check falls through to the committed version as if they had
never been written.

Step 5 is an ordinary RocksDB `Put` into the memtable, WAL-logged but
**not synced** — an uncommitted write does not need durability, only
the commit record does (§8.2).

### 5.1 Row locks

An in-memory sharded hash table, `(relid, hash(userkey))` → owner
transaction + waiter queue. Exclusive only; readers never take one.
Locks are released at commit/abort — strict, so no cascading aborts.

A waiter blocks on a condition variable with a deadlock check: a
background detector walks the waits-for graph every `deadlock_ms`
(default 50ms) and aborts the **youngest** transaction in a cycle with
`SM_ERR_DEADLOCK`. Cycles are only possible among writers, so the graph
stays small.

Locks are keyed by hash, so a collision makes two distinct keys share
a lock entry — correct (it only over-serializes), and the entry stores
the full key so the common case is exact.

A transaction appends a lock entry to `T.locks` only when it
first acquires it. Re-acquiring an entry it already holds, for
the same key or a colliding one, appends nothing, so §6.5's
rollback releases only entries first taken after the savepoint.

### 5.2 First-committer-wins

Step 3 rejects a write when the key was modified by a transaction that
committed after our snapshot. Together with the row lock (which
prevents the concurrent case), this is the standard snapshot-isolation
write rule and it is what makes the §4.1 invariant hold. It applies at
**all three isolation levels**, including read committed (§6.2).
The caveat for the non-serializable levels is §7.4's: their
writes still feed the SSI tracker, but they are never
serialization victims themselves.

---

## 6. Transactions

### 6.1 Lifecycle

```
                sm_begin
                   │  snapshot_ts = visible_ts
                   ▼
              ┌─────────┐   first write: xid = global_ts++
              │ ACTIVE  │◄──── sm_savepoint / sm_rollback
              └────┬────┘
        sm_prepare │        │ sm_commit
                   ▼        ▼
            ┌──────────┐  (validate → commit_ts → status record
            │ PREPARED │   → WAL sync → publish → release)
            └────┬─────┘
   sm_commit /   │
   sm_abort      ▼
            COMMITTED / ABORTED
```

### 6.2 Read committed

A fresh statement snapshot `snap = visible_ts` is taken **at the
start of each read statement** (each `sm_read`, and each
`sm_scan` — a scan keeps its snapshot for the life of the
cursor, so one scan is internally consistent). It is a local of
the statement; `T.snapshot_ts` is still the one taken at
`sm_begin` and is never refreshed.

Writes still obey §5.2, and they check against `T.snapshot_ts`,
not the statement snapshot, so a write can fail with
`SM_ERR_CONFLICT` even here — including on a row the transaction
has just read, if that row was committed after `sm_begin`. The
engine does not silently re-read the newer row (no Oracle-style
restart), it reports the conflict. Checking against the begin
snapshot is what keeps §4.1's invariant at this level: it is
always below `T.xid`, whereas a statement snapshot taken after
the first write can pass it. A read-committed transaction with
`T.xid = 50` that updated a row committed at 70 against a
statement snapshot of 75 would put a version `k|~50` that sorts
*behind* the committed `xid 60` version: readers would return
the older value, and GC would drop the newer commit as
superseded.

### 6.3 Repeatable read

One `snapshot_ts` taken at `sm_begin`, held to the end. This is
snapshot isolation: no lost updates, no non-repeatable reads, no
phantoms — but write skew is possible. That is the standard SI/RR
trade and it is what `SM_XCT_REPEATABLE_READ` means here.

### 6.4 Serializable

Repeatable read plus SSI validation (§7).

### 6.5 Savepoints

The API allows exactly one savepoint per session. It is a mark into the
per-transaction **undo list** — an in-memory vector of
`(relid, userkey, seq)` triples in write order, holding no payloads,
so a large transaction costs bytes per write, not rows. Relations
created by the transaction are tracked alongside it in
`T.created`, a vector of relids in creation order, and relations
it dropped in `T.dropped`, likewise.

```
sm_savepoint():
    T.sp_mark   = T.undo.size()        # replaces any previous savepoint
    T.sp_locks  = T.locks.size()
    T.sp_rels   = T.created.size()
    T.sp_drops  = T.dropped.size()

sm_rollback():
    if no savepoint: fail with SM_ERR_NOSAVEPOINT    # API.txt divergence 8
    m = T.sp_mark
    for e in reverse(T.undo[m..]):
        db.Delete(cf(e.relid), encode(e.key, ~T.xid, ~e.seq))
    r = T.sp_rels
    for relid in reverse(T.created[r..]):
        db.DropColumnFamily(cf(relid))              # §11
    T.created.truncate(r)
    T.dropped.truncate(T.sp_drops)                  # the undo restored their rows
    T.undo.truncate(m)
    T.seq_hwm = (m > 0) ? T.undo[m-1].seq : 0       # own-write visibility
    T.seq     = T.seq_hwm + 1                        # live seqs stay a prefix
    release locks acquired after the mark
    ssi read set is NOT rolled back                  # conservative, correct
    transaction stays ACTIVE
```

Rolling back is a delete of exactly the versions written since the
mark. Nothing else can see them, so there is no visibility window to
worry about, and `seq` guarantees a re-write of an already-written key
did not clobber the pre-savepoint version.

`seq_hwm` is the last `seq` still in effect: each write sets it,
and a rollback lowers it to the mark. Rewinding `T.seq` with it
keeps the own versions a transaction can see an exact prefix
`1..seq_hwm` of its writes. Without the rewind, the next write
would raise `seq_hwm` past the undone numbers and §4.4 would
admit the undone versions again wherever they are still
readable — in particular through a cursor opened before the
rollback, whose RocksDB snapshot does not see the deletes. With
the rewind, a later write reuses an undone number, and the
deletes, applied before `sm_rollback` returns, have already
removed the version that carried it.

Releasing locks acquired after the mark is safe: the row was untouched
before the savepoint, so no one observed our intent on it.

### 6.6 Abort

`sm_abort` writes an `ABORTED` status record (not WAL-synced — it is a
hint; absence of a record means the same thing after a crash),
drops the column families of relations in `T.created` (no other
transaction can see them), closes the session's open scan cursors
and their RocksDB snapshots, releases locks and SSI state, and
returns. The versions stay in their relation CFs until GC (§8.3).
Abort is O(1) in the write-set size.

---

## 7. SSI — serializable snapshot isolation

Cahill/Ports-style: run at snapshot isolation, track **rw-antidepen-
dencies**, and abort a transaction that sits at the pivot of a
dangerous structure.

### 7.1 What is tracked

- **SIREAD locks.** Every read by a SERIALIZABLE transaction records
  what it read: a point read records `(relid, userkey)`, whether
  or not a row was found; a scan records `(relid, range)` for the
  requested range, bounds inclusive or exclusive as passed to
  `sm_scan`, when it opens, and narrows it to the **range
  actually traversed** when it closes (§10.2). The narrowed
  range includes its upper end, the last key the scan examined.
  That range is the predicate. Ranges are the phantom defence —
  a later insert into that range conflicts even though the row
  did not exist at read time. SIREAD locks survive the
  transaction's own commit; they are dropped only when no active
  snapshot could still conflict with them.
- **Conflict flags.** Per transaction, `in_conflict` and
  `out_conflict` — "some transaction has an rw-edge into me" / "I have
  an rw-edge out to someone".

### 7.2 Edges

```
T_writer writes k  →  for each SIREAD on k (or a range covering k)
                      held by T_reader (T_reader != T_writer):
                          edge  T_reader ──rw──▶ T_writer
                          T_reader.out_conflict = true
                          T_writer.in_conflict  = true

T_reader reads k   →  if the version it skipped was written by a
                      concurrent T_writer (committed after our
                      snapshot, or still active):
                          edge  T_reader ──rw──▶ T_writer
```

### 7.3 The abort rule

A cycle in the serialization graph always contains a transaction with
**both** an incoming and an outgoing rw-edge (a *pivot*), where the
outgoing edge's target commits first. So:

```
at commit of T:
    if T.in_conflict and T.out_conflict:
        abort T with SM_ERR_SERIALIZATION
    if T has an out-edge to some T' that already committed
       and T' also has in+out conflicts:
        abort whichever is still running
```

Concretely, the check runs at prepare/commit time and, for the case
where the pivot is not the last to commit, eagerly when the edge is
created (mark the pivot as doomed; it fails at its own commit). A
pivot that has prepared or committed cannot be doomed — it has
already been told "yes" — so then the running transaction whose
new edge completed the structure is doomed instead, whether that
edge points into the pivot or out of it (§8.4). This
is conservative — some aborted transactions would in fact have been
serializable — which is the accepted cost of SSI in exchange for never
taking a read lock and never deadlocking on reads.

### 7.4 Interaction with the other levels

Only SERIALIZABLE transactions take SIREAD locks or are candidates for
serialization failure. But a SERIALIZABLE transaction's correctness
depends on seeing *writes by everyone*, including RR and RC ones, so
the write path (§5, step 7) always calls into the SSI tracker.
Read-only serializable transactions get the standard safe-snapshot
optimization (Ports & Grittner). A read-only transaction is never
a pivot — it writes nothing, so nothing has an rw-edge into it —
but it can be the *incoming* end of a dangerous structure,
`T_ro ──rw──▶ T_pivot ──rw──▶ T_out` (Fekete's read-only
anomaly), so its SIREAD locks matter until that is ruled out.
Its snapshot is **safe** once every read-write serializable
transaction concurrent with the snapshot has finished and none
of them committed with an out-conflict to a transaction that
committed before the snapshot. If there are no such concurrent
transactions at `sm_begin`, the snapshot is safe at once. If one
of them does commit with such an out-conflict, the snapshot is
unsafe.

The API has no read-only declaration, so a transaction is known
to be read-only only when it commits with `T.xid == 0`. While it
is active it stays fully tracked, safe snapshot or not: it may
still write, and then it needs its read set as a possible pivot.
Dropping the tracking early and failing a later write instead
would fail every read-then-write transaction whose snapshot
became safe, on an idle database at once, and again on every
retry.
The optimization applies from commit on. A transaction that
commits with `T.xid == 0` releases its SIREAD locks as soon as
its snapshot is safe, instead of holding them under §7.1's rule.
Any pivot it could still reach began after its snapshot, too
late to be concurrent with a `T_out` that committed before it.

### 7.5 Memory

The tracker holds SIREAD locks and conflict flags for transactions
that have committed but might still conflict with a live one — bounded
by the oldest active snapshot. Under a long-running reader this grows;
the escape valve is the standard one: when the structure exceeds
`ssi_max_entries`, summarize the oldest committed transactions into a
coarse "something committed in this range" entry, which is safe
(over-approximating conflicts) and costs extra false aborts.

---

## 8. Durability, commit, and 2PC

### 8.1 Where durability comes from

RocksDB's WAL. Each `WriteBatch` is atomic and, when written with
`sync=true`, durable. The storage manager adds no log of its own,
because with eager writes there is exactly one thing that must be made
atomic and durable at commit: **the status record**.

### 8.2 Single-node commit

```
sm_commit(T):
  1. close T's open scan cursors               (§10.2)
     if SERIALIZABLE: ssi.validate(T)         → may SM_ERR_SERIALIZATION
     if T.xid == 0: go to step 6               ← read-only: nothing to publish
  2. commit_ts = sequencer.claim()           ← global_ts++, joins the set
  3. batch = { CF:txn[T.xid] = {COMMITTED, T.snapshot_ts, commit_ts} }
  4. db.Write(batch, sync = durability_sync)   ← group-committed
  5. commit_cache.insert(T.xid → commit_ts);  active_table.remove(T.xid)
     if T wrote CF:rel.0: last_ddl_ts = max(last_ddl_ts, commit_ts)  (§11)
     sequencer.done(commit_ts);  advancer moves visible_ts      (§4.3)
  6. lockmgr.release(T);  ssi.on_commit(T)
     queue (T.dropped, commit_ts) for CF removal     (§8.3)
```

Step 1 closes cursors first, as `API.txt` requires of `sm_commit`
and `sm_abort`: this releases their RocksDB snapshots, which pin
`gc_watermark`, and narrows each cursor's SIREAD predicate to
the range it actually traversed (§10.2) before validation runs.

**A failed status write is fail-stop.** If step 4 returns an
error, the record may or may not have reached the WAL — a sync
can fail after the bytes are written — so the transaction's
outcome is unknown until WAL replay decides it. The engine
therefore does not resolve it as dead:

- the transaction stays in the active table and its
  `commit_ts` stays in the sequencer set, so its versions
  remain in-flight to readers and to the compaction filter, and
  no snapshot can include its `commit_ts`;
- the database enters a failed state: further writes, commits,
  prepares, and GC sweeps fail with `SM_ERR_IO`, while reads on
  existing snapshots continue;
- `sm_commit` returns `SM_ERR_IO`, meaning *outcome unknown*.
  After the database is reopened, §3.4 resolves the transaction
  from whatever the WAL holds.

This matches RocksDB, which itself stops accepting writes after
a WAL write or sync error.

A read-only transaction (`T.xid == 0`) has no versions to
publish and gets no status record (§3.2); it validates, releases,
and returns without taking a `commit_ts`.

Step 4 is **the commit point**. Before it, the data versions are
already in RocksDB but resolve as in-flight or (after a crash) dead;
after it, they resolve as committed. There is no window where a
transaction is half-visible, and no redo or undo work for a data
version is ever needed.

Commits are group-committed: concurrent committers hand their batches
to one leader that issues a single `Write(..., sync=true)`. `sync` is
configurable (`durability_sync=false` trades a bounded window of
committed-transaction loss for throughput; the database is still
crash-*consistent*, just not crash-*durable*).

### 8.3 Garbage collection

Two kinds of garbage: versions from aborted transactions, and
committed versions superseded by a newer version that every live
snapshot can see.

Both are collected by a **RocksDB compaction filter**, which already
visits every key during normal compaction:

```
filter(version_key, value):
    resolve xid                                          (§3.4)
    if dead (aborted / crashed)         → DROP
    if in-flight (active / prepared)    → KEEP as-is
    if committed and commit_ts > gc_watermark → KEEP as-is
    if a newer version of the same userkey with
       commit_ts <= gc_watermark was already seen → DROP
    if tombstone                         → KEEP, stamp commit_ts, and
                                           queue (relid, version_key)
                                           for purge
    otherwise                            → KEEP, and stamp commit_ts
                                           into the header (hint bit)
```

Past the first three rules a version is committed with
`commit_ts <= gc_watermark`, so only those are purged or
stamped. The in-flight rule must come before them: compaction
runs at any time, including over versions of transactions that
are still running, and such a version has no `commit_ts` to
stamp. A stamp there would make readers skip status resolution
and see an uncommitted write, and it would outlive an abort.

A tombstone that every live snapshot sees is never "superseded",
so the rules above would keep it forever — one record per
deleted key. The filter cannot simply drop it, not even at the
bottommost level (which `CompactionFilter::Context` does not
report anyway): RocksDB picks
compaction inputs by its own key, which is the full version key,
so older versions of the same userkey may sit in an adjacent SST
file or another subcompaction this filter never sees, and
dropping the tombstone would resurrect them. Instead a
background task drains the purge queue with

```
db.DeleteRange(cf(relid), version_key, userkey | FF×12)
```

which covers the tombstone and every older version of the key
at every level. Any later write to the key has a larger `xid`,
sorts before the tombstone, and survives. A range deletion does
not affect RocksDB snapshots taken before it, and RocksDB drops
the range tombstone itself at the bottommost level. The queue is
volatile: an entry lost to a crash is queued again by the next
compaction that meets the tombstone, and a repeated entry is
harmless.

The stamping is why the header carries `commit_ts`: over time most
versions answer visibility without a status lookup at all, so the
commit cache and `CF:txn` are consulted mainly for the recent tail.

`gc_watermark = min(snapshot_ts of active sessions, snapshot_ts of
prepared transactions, oldest open scan)`. It never moves
backwards: a new snapshot is `visible_ts`, which is at or above
every snapshot already counted. One compaction filter factory
serves all relation CFs.

A read-committed transaction counts at its begin `snapshot_ts`
for its whole life, between statements too, because §6.2 checks
its writes against that snapshot. If the watermark passed it,
the tombstone purge could remove a delete committed after
`sm_begin`. A later insert of that key by the transaction would
then pass the §5.2 check with an xid below the deleter's, and a
repeated purge entry for the key would delete the committed
insert. An idle open transaction therefore holds back GC, as in
any MVCC engine.

A `COMMITTED` record can be dropped once every surviving
version of its transaction carries the stamp (rule §3.4/0). The
periodic sweep guarantees that:

1. Record `W0 = gc_watermark`, then run a full compaction pass
   over every relation CF, flushing memtables first.
2. Every committed version with `commit_ts <= W0` was already
   in RocksDB when the pass began, was visited by the filter
   with `gc_watermark >= W0`, and so was dropped or stamped.
3. Once the pass completes and every iterator created before
   it began has been released — an older iterator can still
   read pre-pass, unstamped copies — drop the `COMMITTED`
   records with `commit_ts <= W0`.

Using the watermark at the pass's *start* matters: a version
with `W0 < commit_ts <= gc_watermark_now` may have been kept
unstamped, and dropping its record would make it resolve as
dead. `ABORTED` records are hints (§6.6) and can be dropped at
any time, since rule §3.4/4 gives the same answer without them.

Dropping a relation (`sm_rmrel`) deletes its catalog row (§11).
The tombstone carries no relid, so commit queues the relids in
`T.dropped` with its `commit_ts`; once `gc_watermark` passes
that commit, a background task issues `DropColumnFamily` on each
relation's CF. The queue is volatile: an entry lost to a crash
is caught by the startup sweep (§9.1 step 6).

### 8.4 Two-phase commit

`sm_prepare` makes the transaction durable but not visible.

```
sm_prepare(T, gid):
  1. close T's open scan cursors               (§8.2)
     if SERIALIZABLE: ssi.validate(T)         ← validation happens HERE
     if T.xid == 0: T.xid = global_ts++        ← read-only: key the record
  2. rec = {PREPARED, xid, gid, snapshot_ts, prepared_at = now(),
            writeset = keys of T.undo, created = T.created,
            dropped = T.dropped}
  3. db.Write({CF:txn[T.xid] = rec}, sync = true)      ← always synced
  4. T.state = PREPARED;  locks and SSI state are RETAINED
```

Step 1 closes cursors for the reasons given in §8.2. No data
operation is accepted after prepare, so an open cursor could
never be used again, but it would pin `gc_watermark` for as long
as the transaction stays in doubt. The coordinator's commit
(§8.2 from step 2) therefore finds none left to close.

A read-only transaction still needs a durable `PREPARED` record,
because `API.txt` promises that a prepared transaction survives
a crash and can be resolved by `gid` afterwards. Step 1 gives it
an xid (entering the active table, §3.4) so that the record has
a unique key; with `T.xid == 0`, every read-only prepare would
overwrite the same `CF:txn[0]`. Its `writeset` is empty, so
after restart it holds no locks.

After a successful prepare the transaction is in-doubt: its
versions sit in relation CFs, its status record says `PREPARED`,
and readers treat it as in-flight (invisible). It holds its row
locks, and it pins `gc_watermark` at its `snapshot_ts`.

**A failed prepare write is fail-stop**, for the reason §8.2
gives for a failed commit write: if step 3 returns an error, the
`PREPARED` record may still have reached the WAL, and §9.1 would
then restore the transaction in doubt. The engine therefore does
not treat it as aborted:

- it stays in the active table with its locks and SSI state, so
  its versions remain in-flight to readers and to the compaction
  filter, and the column families in `T.created` are kept;
- the database enters §8.2's failed state, so nothing can
  resolve the transaction before the database is reopened;
- `sm_prepare` returns `SM_ERR_IO`, meaning *outcome unknown*,
  and the transaction is no longer active on its session.

After reopen, WAL replay decides: either the record survived and
the transaction is restored in doubt, listed by
`sm_list_prepared` under its `gid`, or it has no record and is
dead (§3.4). A coordinator that got `SM_ERR_IO` checks the list
and resolves by `gid`. It never received a "yes" vote, so abort
is always a correct decision.

The coordinator later calls commit or abort. Commit is §8.2 starting
at step 2, overwriting the record (dropping the now-unneeded write
set); abort writes `ABORTED`, drops the CFs in `created`, and
releases.

**Why validate at prepare.** Once prepared, a transaction cannot be
told "no". So the serialization check must have already passed, and it
must stay passed. Any transaction that would create a dangerous
structure with a prepared transaction sees the prepared transaction as
a committed-in-the-future conflict partner and aborts itself instead.
Two prepared transactions were each live during the other's
validation, so they were checked against each other.

**After a restart.** That argument does not cover a transaction
that begins after restart. A restored in-doubt transaction has
lost its SIREAD locks and conflict flags, which are volatile, so
a new transaction can write a key the prepared one read and no
edge appears. Unchecked, that admits write skew:

1. Before the crash, `T_prep` reads `A = a0`, writes `B = b1`,
   and prepares. Only `B` is in its `writeset`.
2. After restart, `T_prep` holds its lock on `B`, but its
   SIREAD lock on `A` is gone.
3. `T_new` reads `B`, skips `b1` as in-flight, and reads `b0`:
   edge `T_new ──rw──▶ T_prep`.
4. `T_new` writes `A = a1`. No SIREAD covers `A`, so the edge
   `T_prep ──rw──▶ T_new` is never created, and `T_new`
   commits.
5. The coordinator commits `T_prep`. Each read what the other
   overwrote — a cycle.

Persisting SIREAD locks in the `PREPARED` record would not be
enough alone, because the conflict flags are lost too (a
restored transaction that already had an out-edge is a pivot
the moment a new reader adds an in-edge), and it would make the
record as large as the read set. PostgreSQL persists the locks
and assumes both flags on recovery. wisr instead marks each
restored transaction `restored` (§9.1), and the tracker applies
one extra rule:

```
T_reader skips a version written by a restored transaction (§7.2)
    →  doom T_reader; it fails at prepare/commit with
       SM_ERR_SERIALIZATION
```

This is enough without replaying the SSI graph. Nothing written
by the restored transaction can be read before it commits, and
its row locks block any write to its keys, so every edge *into*
it from a post-restart transaction is an rw-edge from reading
past one of its versions — which the rule sees. Edges *out* of it
are unknowable, so every transaction with an edge in is doomed,
and no cycle can pass through it. The rule still holds after the
coordinator commits it: a reader whose snapshot predates that
commit still skips its versions and is still doomed; a reader
whose snapshot follows it sees them and forms no edge. The
tracker keeps a restored xid in its restored set, after the
transaction leaves the active table, until `gc_watermark` passes
its `commit_ts`. The cost is false aborts for serializable
readers of in-doubt keys, which is acceptable for a state that
only follows a crash.

### 8.5 The 2PC API surface

`API.txt` defines these five calls. What the design requires
of each:

```c
int  sm_prepare(sm_session_t* session, const char* gid);
int  sm_list_prepared(sm_session_t* session,
                      sm_prepared_t** out, size_t* n);
void sm_free_prepared(sm_prepared_t* list, size_t n);
int  sm_commit_prepared(sm_session_t* session, const char* gid);
int  sm_abort_prepared(sm_session_t* session, const char* gid);
```

- `gid` is what makes a prepared transaction addressable once
  the session that prepared it is gone. It goes into the
  `PREPARED` status record (§8.4 step 2) and must be unique
  among the unresolved prepared transactions of a database.
- `sm_list_prepared` is the only way to discover the in-doubt
  transactions that recovery restored (§9.3). Without it they
  hold their row locks and pin `gc_watermark` indefinitely
  with no way for an operator to name them. It requires no
  active transaction, because a coordinator calls it on a
  fresh session at startup.
- `sm_commit_prepared` and `sm_abort_prepared` resolve by
  `gid` on any session, which is the whole point: after a
  restart the resolving session is never the preparing one.
- `sm_prepare` can still fail with `SM_ERR_SERIALIZATION`,
  leaving the transaction aborted rather than prepared.
  Validation runs at prepare (§8.4), so this is the last
  moment at which the engine is able to say no.

---

## 9. Recovery

There is no redo pass and no undo pass. RocksDB's WAL replay restores
the KV store to the last durable write; everything else follows from
the status resolution rule.

### 9.1 Startup

```
1. ListColumnFamilies, then open RocksDB with every CF
   (replays WAL → memtable state as of last sync).
2. Read CF:meta → format version, persisted next_ts, next_relid.
3. global_ts = next_ts + 1;  visible_ts = next_ts.
4. Scan CF:txn for records in state PREPARED.
   (COMMITTED / ABORTED records need no action.)
5. For each prepared record:
      - register an entry in the active transaction table (in-doubt),
        marked restored so that readers skipping its versions
        are doomed (§8.4), with T.created and T.dropped from
        the record
      - re-acquire an X row lock for every key in its writeset
      - pin gc_watermark at its snapshot_ts
6. Orphan CF sweep: read the catalog (rel.0) and drop every
   rel.<relid> CF that has neither a live catalog row naming
   it (a name's newest committed version, not a tombstone) nor
   an entry in a prepared record's `created`.
7. Start the GC / compaction filter, deadlock detector, commit advancer.
8. Open for sessions.
```

Step 6 catches the column families that eager cleanup (§6.5,
§6.6) never reached: a crash between `CreateColumnFamily` and
commit, and a committed drop whose `DropColumnFamily` had not yet
run. Creating a CF is durable in the RocksDB MANIFEST before
`sm_mkrel` writes its catalog row, so a committed catalog row
never lacks its CF.

There is no "abort recovery" step: every transaction that was
running at the crash is absent from the fresh active table and
has no status record, so rule §3.4/4 declares it dead the first
time anyone looks at one of its versions. Its orphaned versions
are dropped by the next compaction. No scan of the data is
needed, and recovery time is independent of the size of the
crashed transactions.

### 9.2 What each pre-crash state becomes

| At crash | After restart |
| --- | --- |
| ACTIVE (never wrote) | nothing to do |
| ACTIVE with eager writes, no record | dead; versions dropped by GC |
| ACTIVE that created a relation | dead; its CF dropped by the orphan sweep (§9.1 step 6) |
| Wrote its COMMITTED record, WAL synced | committed, visible |
| Was in `Write(sync)` when power cut | atomically all-or-nothing per RocksDB; if the record is not in the WAL, the transaction is dead |
| PREPARED | restored in-doubt, locks re-acquired, awaits coordinator |
| Wrote ABORTED | aborted (and would be aborted anyway) |

### 9.3 In-doubt transactions

A restored prepared transaction blocks writers on its keys (it holds
locks) and pins GC. `sm_list_prepared` exposes them by `gid` so the
coordinator can finish them. An operator escape hatch
(`sm_abort_prepared`) is required, because an abandoned prepared
transaction otherwise blocks GC forever; this is the same operational
hazard as in any 2PC system and is exposed, not hidden.

### 9.4 Torn or corrupt state

RocksDB checksums every block and WAL record. A checksum failure is
surfaced as `SM_ERR_CORRUPT` at open; the design does not attempt
repair beyond RocksDB's own WAL truncation of a torn tail record —
which is safe here precisely because a torn tail can only lose commit
records at the very end of the log, and losing a commit record is
indistinguishable from the transaction never having committed.

---

## 10. Read path

### 10.1 Point read

```
sm_read(session, rel, key, &val):
    relid = catalog.lookup(rel)                          # SIREAD too, §11
    snap  = (level == READ_COMMITTED) ? visible_ts : T.snapshot_ts
    hi    = max(T.xid, snap)                             # own writes, §4.4
    if SERIALIZABLE:
        ssi.on_read(T, relid, key)                       # SIREAD, §7.1
    it = db.NewIterator(cf(relid))
    if SERIALIZABLE:
        it.Seek(key | 00×12)                             # every version
    else:
        it.Seek(key | ~hi | 0x00000000)
    while it.Valid() and userkey(it.key()) == key:
        v = decode(it)
        if visible(v, snap, T):                          # §4.4
            return v.tombstone ? NOT_FOUND : v.payload
        if SERIALIZABLE and concurrent(v, snap):         # §7.2
            ssi.on_skip(T, v.xid)                        # rw-edge
        it.Next()                                        # skip invisible
    return NOT_FOUND
```

The loop normally runs once. It iterates only past versions written by
transactions that are concurrent with this reader on this exact key.

Under SERIALIZABLE two things differ, both for §7.2:

- The SIREAD lock is taken **before** the iterator is created,
  and whether or not a row is found. Reading a missing key is a
  predicate read, so a later insert of that key must find the
  lock. Taking it first closes the window in which a write lands
  after the iterator's implicit snapshot but before the lock
  exists, where neither side would see the other.
- The seek starts at `key | 00×12`, before every version of the
  key, instead of at `~hi`. Versions with `xid > hi` sort before
  the `~hi` seek target, and they are exactly the concurrent
  writers (in-flight, or committed after `snap`) that owe an
  rw-edge; seeking past them would miss it. `concurrent(v, snap)`
  is true when `v.xid` is not the reader's own and resolves as
  in-flight or as committed with `commit_ts > snap`. The extra
  steps are only over those versions, which are rare.

Otherwise the seek starts at `max(T.xid, snap)`. Under
repeatable read, a transaction's own versions have
`xid = T.xid > snap`, so a seek at `~snap` would land after them
and a transaction could never read its own writes. Under read
committed, `snap` is refreshed per statement and can pass
`T.xid`; a seek at `~T.xid` would then land after versions
committed with `T.xid < xid <= snap`, which read committed must
see. Taking the larger of the two covers both. A `~seq` of
`0x00000000` places the seek before every own version of the
key, newest first, and §4.4 picks the one allowed by `seq_hwm`.
Versions from other transactions with `snap < xid < T.xid` are
stepped past as in-flight or committed too late, which the loop
already handles.

### 10.2 Scan

`sm_scan` opens a RocksDB iterator on the relation's CF, seeked
to `skey` and bounded by `ekey`, on a **RocksDB snapshot** taken
at cursor creation. The RocksDB snapshot gives a stable physical
view (unaffected by concurrent eager writes or compaction); the
MVCC snapshot gives the logical view. Both are needed:

```
seek target          = skey | (skey_inclusive ? 00×12 : FF×12)
iterate_upper_bound  = ekey | (ekey_inclusive ? FF×12 : 00×12)
```

The bounds carry a full tail because the comparator requires one
(§3.1). No real version has an all-`00` or all-`FF` tail, since
xid 0 means "no xid yet" and xid 2^64−1 is never reached, so
these tails fall strictly before or after every version of the
bound key.

- Without the RocksDB snapshot, other sessions' eager writes would
  appear mid-iteration and have to be filtered out anyway, and the
  iterator's own consistency would depend on memtable flush timing.
- A consequence: **a scan does not see the scanning transaction's own
  writes made after `sm_scan`**. This is deliberate — it is Halloween
  protection, and it makes "update every row I scan" safe.
- The cursor also captures `T.seq_hwm` at `sm_scan` and applies
  §4.4 with that value, not the live one. Its view of the
  transaction's own writes is then fixed like the rest: a later
  `sm_rollback` neither hides rows the cursor could already see
  nor, after the rewind of §6.5, lets reused `seq` numbers
  change what it returns.

`sm_scan_next` walks version keys, and for each distinct `userkey`
emits the first visible version and then skips the rest of that key's
versions:

```
while iterator valid:
    uk = userkey(it.key())
    v  = first visible version in the run of versions for uk
    if SERIALIZABLE:                                 # §7.2
        for each version u before v in the run,
                (every version in the run if v is absent):
            if concurrent(u, snap): ssi.on_skip(T, u.xid)
    seek past the run (Seek to uk|0xFF..)            # or Next()-chase
    if v exists and not tombstone: yield (uk, v.payload); return
```

A scan steps past concurrent writers' versions just as a point
read does, and owes the same rw-edges (§10.1). The skipped
versions matter most when they were committed between the
reader's snapshot and `sm_scan`: their writers ran `on_write`
before the range SIREAD existed, so the read side is the only
place the edge can be created. They also feed the post-restart
rule of §8.4.

For SERIALIZABLE, `sm_scan` registers the whole requested range,
`skey` to `ekey` with their inclusive flags, as a SIREAD
predicate (§7.1) when it creates the cursor, **before** it takes
the RocksDB snapshot, and `sm_scan_end` shrinks it to the range
actually covered: from `skey` through `last_key_examined`,
inclusive (through `ekey`, with its flag, if the scan reached
the end). `last_key_examined` is the last userkey whose run the
cursor inspected, whether or not it yielded a row. Registering
only at `sm_scan_end` would be wrong: a write into the range
while the cursor is open is not in the cursor's RocksDB
snapshot, so the reader never steps past it, and with no SIREAD
yet the writer finds nothing to conflict with — the rw-edge
would be missed entirely. The order within
`sm_scan` matters for the same reason, as in §10.1: a writer
puts its version before calling `on_write`, so a version put
after the snapshot is found by `on_write` only if the SIREAD
already exists. Shrinking at the end is safe, because the
reader never read past `last_key_examined`; edges already
created for writes beyond it are merely conservative. If the
cursor is abandoned early, the range kept is the shorter one
actually read.

### 10.3 API shape

`API.txt` is the interface contract. Its read-path signatures
are repeated here because three of them encode decisions from
this document rather than mere spelling:

```c
/* 0 = row, 1 = no such key, -1 = error. *data points into
 * session-owned memory valid until the next call on the session. */
int  sm_read(sm_session_t* session, const char* relname,
             const char* key, size_t klen,
             const char** data, size_t* datalen);

sm_scan_t* sm_scan(sm_session_t* session, const char* relname,
                   const char* skey, size_t sklen, int skey_inclusive,
                   const char* ekey, size_t eklen, int ekey_inclusive);

/* 0 = row returned, 1 = end of range, -1 = error. Buffers valid
 * until the next sm_scan_next on the same cursor. */
int  sm_scan_next(sm_scan_t* scan, const char** key, size_t* klen,
                  const char** data, size_t* datalen);
void sm_scan_end(sm_scan_t* scan);
```

- **Both range bounds are parameters.** Without an end key a
  range predicate cannot be recorded for SSI (§7.1), and every
  caller would otherwise scan and discard.
- **"No row" is a tri-state return, not an out-param.**
  `sm_read` returns 1 for a missing key and `sm_scan_next`
  returns 1 at end of range; this replaces the earlier
  `int* found`. Neither is an error, and neither disturbs the
  error state, so a caller must distinguish 1 from -1 before
  consulting `sm_errno()`.
- **`sm_scan_end` returns void.** Releasing a cursor cannot
  usefully fail. It is still where the predicate registered
  at `sm_scan` is shrunk to the range the cursor actually
  traversed (§10.2), so the range kept is the part traversed,
  not the part requested, which makes closing early both
  correct and tighter than draining.

---

## 11. Catalog and DDL

The catalog is relation `relid = 0`, an ordinary MVCC relation in
`CF:rel.0`:

```
key = "R" | relname        →  { relid, created_ts }
```

There is no next-relid row in the catalog. Relids come from the
lease in `CF:meta` (§4.2); a catalog row updated by every
`sm_mkrel` would make concurrent creates of *different*
relations contend on one row lock and fail with
`SM_ERR_CONFLICT`.

Because it is an ordinary relation, DDL is transactional for free:
`sm_mkrel` inserts a catalog row, `sm_rmrel` deletes it
(recording the relid in `T.dropped`), and both roll back with
the transaction. Both follow the §5 write path, reported in DDL
terms: `sm_mkrel` of a live name fails with `SM_ERR_EXISTS`
where an insert would report `SM_ERR_DUPKEY`, and `sm_rmrel` of
a missing name fails with `SM_ERR_NOTFOUND`. A dropped name's
newest version is a tombstone, so `sm_mkrel` can create it
again.

A session caches `relname → relid`, but the mapping depends on
the snapshot. After a committed `sm_rmrel` and `sm_mkrel` of
the same name, a transaction with an older snapshot must still
resolve the old relid, since it may still legitimately read the
dropped relation. A cache invalidated at DDL commit would send
it to the new one, so the cache is checked by timestamp
instead:

- The process keeps `last_ddl_ts`, the largest `commit_ts` of
  any committed transaction that wrote the catalog. Commit raises
  it before publishing that `commit_ts` (§8.2 step 5), so a
  snapshot that includes a DDL commit always sees it.
- Each cache entry records the snapshot it was read at. A hit is
  used only if `last_ddl_ts` is at or below both that snapshot
  and the lookup's; then no DDL committed between the two, and
  the mapping is the same at both. Otherwise the lookup reads
  `CF:rel.0` and replaces the entry.
- `sm_mkrel` and `sm_rmrel` evict their name's entry, and a
  lookup that resolves to the transaction's own uncommitted
  catalog version does not populate the cache.

The comparison is against both snapshots because a session's
snapshots do not only move forward: under read committed, a
lookup at the begin snapshot `T.snapshot_ts` can follow one at a
newer statement snapshot (§6.2).

Under SERIALIZABLE a lookup is also a read for SSI. The relid a
name resolves to is something the transaction observed, and a
concurrent `sm_rmrel` or `sm_mkrel` of that name writes the
catalog key it depends on. Without an rw-edge for it, DDL admits
write skew:

```
T1: sm_read(S, x)             T2: sm_read(R, k)    # looks up "R"
T1: sm_rmrel(R)               T2: sm_update(S, x)
```

`T1 ──rw──▶ T2` comes from `x`; `T2 ──rw──▶ T1` exists only if
T2's lookup counts as a read of `"R" | R`. So under SERIALIZABLE:

- A lookup is a §10.1 point read of the catalog key: the SIREAD
  lock on `"R" | relname` in `rel.0` is taken first, whether or
  not the name is found, and versions of concurrent writers it
  steps past run `on_skip`.
- A cache hit does not stand in for that read. An in-flight
  `sm_rmrel` has not raised `last_ddl_ts`, so a hit is valid by
  the rule above, yet it steps past nothing, and the dropper's
  `on_write` ran before any SIREAD existed: the edge is missed.
  Each cache entry therefore also records the transaction that
  read it from `rel.0`, and a serializable lookup uses a hit
  only if that is the current transaction. Its SIREAD already
  catches every later writer of the name.

The column family is the one part of DDL that is not
transactional: `CreateColumnFamily` and `DropColumnFamily` take
effect immediately. `sm_mkrel` therefore creates the CF
**eagerly**, before inserting the catalog row, so the creating
transaction can write into the relation at once. Nobody else can
reach the CF until the catalog row commits. If the transaction
aborts or rolls back past the `sm_mkrel`, it drops the CF itself
(§6.5, §6.6); if it crashes, the startup sweep drops it (§9.1).

`sm_rmrel` does **not** drop the CF at commit — a concurrent
snapshot may still legitimately read the relation. Physical
removal happens when `gc_watermark` passes the drop's
`commit_ts` (§8.3).

`sm_mkdb` creates the directory `{homedir}/{dbname}`, `CF:txn`,
`CF:meta` with its initial contents, and the catalog's
`CF:rel.0`. `sm_rmdb` is `rocksdb::DestroyDB` after verifying
that no session holds the database open; the pidlock on
`homedir` (§1) rules out sessions in other processes. Both
reject a `dbname` that is not a single path component, so
neither can reach a directory outside `homedir`.

---

## 12. Errors, configuration, and structure

### 12.1 Error codes

`API.txt` defines `sm_errno_t` with explicit, stable values.
This section records only which codes the engine raises and
where they come from:

```
SM_ERR_CONFLICT        write-write conflict (§5.2)
SM_ERR_SERIALIZATION   SSI validation failure (§7.3)
SM_ERR_DEADLOCK        victim of deadlock detection (§5.1)
SM_ERR_NOTFOUND        relation or key does not exist
SM_ERR_DUPKEY          insert onto a live key (§5)
SM_ERR_NOSAVEPOINT     rollback with no savepoint set (§6.5)
SM_ERR_NOTRANS         data op outside a transaction (§6.1)
SM_ERR_INTRANS         op forbidden inside a transaction (§6.1)
SM_ERR_PREPARED        illegal op on a prepared transaction (§8.4)
SM_ERR_EXISTS          database or relation already exists (§11)
SM_ERR_INUSE           database still open in this process
SM_ERR_CORRUPT         RocksDB integrity failure (§9.4)
SM_ERR_IO / SM_ERR_NOMEM / SM_ERR_PERMISSION / SM_ERR_DISKFULL
```

`CONFLICT`, `SERIALIZATION`, and `DEADLOCK` are the three
*retryable* errors. On all three the engine has **already
aborted** the transaction (§6.6): the caller must not call
`sm_abort`, and retries by beginning a fresh transaction.
Under `SM_XCT_SERIALIZABLE` they are a normal outcome, not an
exceptional one.

The code and message are read with `sm_errno()` and
`sm_errmsg()`, each reporting the last error on the calling
thread. They are functions, not the thread-local globals of
the original sketch; `API.txt` divergence 1 records why.
Two consequences for the implementation:

- A successful call leaves both untouched, so every path that
  returns an error indication must set them before returning.
  Nothing may rely on a stale value, and nothing may clear
  them on success.
- `sm_errmsg()` is never NULL and its result stays valid until
  the next failing `sm_*` call on that thread, so the message
  wants thread-local storage owned by the engine. Formatting
  it lazily is worthwhile: under SSI a retry loop can raise
  `SM_ERR_SERIALIZATION` constantly and never read the text.

### 12.2 Configuration knobs

`durability_sync` (default on), `group_commit_us`, `deadlock_ms`,
`ssi_max_entries`, `commit_cache_entries`, `ts_lease`, plus
pass-through RocksDB options (block cache size, write buffer
size, compaction style). Bloom filters
are worth enabling on relation CFs, and `CF:txn` benefits from a
large block cache since it is small and hot. For relation CFs the
prefix extractor strips the 12-byte tail, so the bloom key is the
userkey; the comparator keeps each userkey's versions contiguous,
which is what a prefix extractor requires.

Relation CFs share one block cache and one `WriteBufferManager`,
so total memtable memory is a single budget rather than
`write_buffer_size × relations`. `max_total_wal_size` bounds how
long a rarely-written CF can hold old WAL files alive.

### 12.3 Source layout

```
include/sm.h            public C API
src/env.cc              init/final, environment handle, homedir pidlock
src/db.cc               mkdb/rmdb, open registry, CF setup, relid→CF map
src/session.cc          open/close, per-session state and buffers
src/xct.cc              transaction manager, timestamps, sequencer, 2PC
src/version.cc          key/value codec, version comparator, visibility
src/access.cc           read / scan / insert / update / delete
src/lockmgr.cc          row locks, waits-for graph, deadlock detector
src/ssi.cc              SIREAD locks, rw-edges, validation
src/catalog.cc          relation name/id mapping, DDL, CF create/drop
src/gc.cc               compaction filter, watermarks, drop-relation purge
src/recover.cc          startup, prepared-transaction restore, orphan CF sweep
```

### 12.4 Build order

1. Codec, catalog, single-threaded read/write with no concurrency —
   `mkdb/open/mkrel/insert/read/scan` under one transaction at a time.
2. Timestamps, commit sequencer, status table, snapshot visibility;
   RC and RR isolation. Crash tests begin here.
3. Row locks, write-write conflict, deadlock detection.
4. Savepoints.
5. GC / compaction filter (until this exists, the database only grows —
   fine for correctness tests, not for a benchmark).
6. SSI and SERIALIZABLE.
7. 2PC and prepared-transaction recovery.

### 12.5 Testing

- **Key order.** Codec tests with userkeys that are prefixes of
  one another (`""`, `"a"`, `"a\x00"`, `"a\xFF"`, `"ab"`), each
  with several versions: a scan returns them in `memcmp` order,
  and a point read of each finds its own newest visible version.
- **Own writes.** At each isolation level, a transaction reads
  back a key it inserted, updated twice, and deleted; after
  `sm_rollback` to a savepoint it reads the pre-savepoint value.
  A cursor opened before the rollback returns the same rows
  whether or not the transaction writes again afterwards (§6.5,
  §10.2).
  It can update a key it inserted and re-insert a key it
  deleted (§5), and `sm_rollback` with no savepoint fails with
  `SM_ERR_NOSAVEPOINT`. A key locked before a savepoint and
  rewritten after it stays locked through `sm_rollback`, also
  when the rewrite is of a different key sharing its lock entry
  (§5.1).
- **Read committed after a write.** A read-committed
  transaction that has already written reads a key committed by
  another transaction after its first write, and sees it
  (§10.1). Updating that key then fails with `SM_ERR_CONFLICT`
  (§6.2). A read-committed transaction left idle while another
  deletes a key keeps that tombstone through a forced compaction
  and purge, and its later insert of the key fails with
  `SM_ERR_CONFLICT` (§8.3).
- **Isolation.** The Jepsen/Adya anomaly catalogue as unit tests: dirty
  read, lost update, non-repeatable read, phantom, write skew (must
  pass at SERIALIZABLE, must be *observable* at RR — asserting the
  anomaly is present is as important as asserting it is absent).
  At SERIALIZABLE, include the cases that need specific
  handling: a phantom inserted while the reader's cursor is still
  open (§10.2), an insert of a key the reader found missing, a
  point read past a concurrent writer's newer version (§10.1),
  a scan opened after a concurrent writer committed into its
  range (§10.2), a scan closed right after yielding a key
  followed by a concurrent write to that key (§7.1), a scan
  past a key whose only versions are concurrent writers'
  (§10.2), a scan left open at `sm_commit` (§8.2), write
  skew through `sm_rmrel` of a relation the other transaction
  reads, with the name already in both sessions' caches (§11),
  and Fekete's read-only anomaly, which must fail while the
  read-only transaction's snapshot is unsafe (§7.4). A
  transaction that reads, outlives every concurrent writer so
  that its snapshot is safe, and then writes commits, including
  on an otherwise idle database (§7.4). With a prepared or
  committed pivot, the partner is the one that fails (§7.3).
- **DDL.** Concurrent `sm_mkrel` of different names both commit;
  of the same name, the second fails with `SM_ERR_CONFLICT`
  (§11). `sm_mkrel` of a live name fails with `SM_ERR_EXISTS`.
  A dropped relation can be created again under its name, while
  a transaction with an older snapshot still reads the old one;
  `sm_rollback` past an `sm_rmrel` keeps the relation's CF.
  A session that looked up a name before a committed drop and
  recreate resolves the new relid in its next transaction
  (§11).
- **Crash.** A fault-injection harness that kills the process at every
  RocksDB write boundary during a workload, restarts, and checks that
  the surviving state is exactly one of the legal prefixes of the
  commit order.
- **2PC.** Kill between prepare and commit; verify the transaction is
  restored in-doubt, and that both resolutions produce the right state.
  Replay the §8.4 write-skew history across a restart and assert
  that the post-restart transaction fails with
  `SM_ERR_SERIALIZATION`; assert `sm_list_prepared` reports the
  original `prepared_at`. Prepare two read-only transactions
  under different gids, restart, and assert both are listed and
  resolvable (§8.4). Prepare a transaction that dropped a
  relation, restart, commit it by gid, and assert the relation's
  CF is removed once `gc_watermark` passes the commit (§3.2).
- **GC.** Assert that a long-running reader's snapshot is never
  violated by the collector, and that space is actually reclaimed once
  it finishes. Force a compaction while a transaction is still
  running and assert its versions stay invisible and unstamped,
  and are dropped after it aborts (§8.3). Delete many keys and
  assert their tombstones are purged without resurrecting older
  versions. Stall one commit's WAL sync under concurrent load and
  assert allocation continues and `visible_ts` catches up (§4.3).
- **Status resolution.** Fail a commit's status write and assert
  the database fails stop: `sm_commit` returns `SM_ERR_IO`, the
  versions stay in-flight and are not compacted away, and after
  reopen the transaction is committed or dead according to the
  WAL (§8.2). Fail a prepare's record write and assert
  `sm_prepare` returns `SM_ERR_IO`, the versions stay in-flight,
  and after reopen the transaction is either listed by
  `sm_list_prepared` or dead, according to the WAL (§8.4).
  Sweep `CF:txn` after a compaction pass and assert
  every committed row is still readable (§8.3). Run a concurrent
  workload with `commit_cache_entries = 1` and assert it reads
  the same results as with a large cache (§3.4).
- **Linearizability.** A concurrent random-op checker comparing the
  engine against a model, validating that observed histories are
  serializable at SERIALIZABLE and snapshot-isolated at RR.

---

## 13. Known trade-offs

- **Eager writes cost reads.** Every read may step past uncommitted or
  recently-committed versions and resolve a status. Mitigated by the
  commit cache, the compaction-filter hint stamping, and the fact that
  the seek usually lands on the answer. The compensation is that
  transaction size is bounded by disk, abort is O(1), and recovery
  needs no undo.
- **`visible_ts` serializes publication.** A stalled committer holds
  back new snapshots. Bounded by the WAL sync; group commit amortizes
  it. The alternative (per-snapshot in-flight lists) makes snapshots
  expensive to take and visibility expensive to test.
- **SSI aborts transactions that would have been fine.** Accepted in
  exchange for readers that never block and never deadlock. Callers
  must be able to retry.
- **Space is reclaimed by compaction, not promptly.** A workload that
  writes and aborts heavily without triggering compaction will hold
  dead versions. A periodic forced compaction over hot ranges is the
  operational answer.
- **A column family per relation.** Buys instant physical drop,
  per-relation tuning, and per-relation compaction stats. Costs:
  every CF is opened at startup, so open time grows with the
  number of relations; each CF carries its own memtable and LSM
  metadata, so very many small relations need the shared memory
  budget (§12.2); and CF create/drop is outside MVCC, which is
  why `sm_mkrel` needs eager cleanup plus a recovery sweep (§11).
- **One process.** Simplifies everything (no shared memory, no lock
  table in shm), and is what RocksDB wants. Multi-process or remote
  access means a server front-end (§1), not a change to this design.
