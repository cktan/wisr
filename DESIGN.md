
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
design forecloses them; §12 notes where they would attach.

**Assumptions.**

- A `session_t` is used by one thread at a time. Concurrent use of one
  session from two threads is a caller error, not a supported mode.
- Multiple sessions in a process may attach to the same `dbpath`; they
  share one RocksDB handle (RocksDB permits a single writer process
  per directory, so this is enforced by a process-wide registry).
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
  `userkey|~snapshot_ts|0xFFFFFFFF`, which lands on the newest
  version whose `xid <= snapshot_ts` — usually the answer.
- **`seq`** is a per-transaction write counter. It exists so that a
  transaction writing the same key twice creates two distinct
  versions, which is what makes savepoint rollback a pure delete
  (§6.5) instead of requiring an undo pre-image. Complemented so the
  latest write within a transaction sorts first.
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
`relid → handle`, shared by every session attached to the
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
          writeset: [ (relid, userkey) ... ]   (PREPARED only),
          created:  [ relid ... ]              (PREPARED only) }
```

Only **writing** transactions ever get an entry, and only at
prepare/commit/abort time — nothing is written at `sm_begin`. Absence
of a record is meaningful (§3.4).

The `writeset` is carried only in a `PREPARED` record, and only
so that recovery can re-acquire the row locks a prepared
transaction still holds. `created` lists the relations the
transaction made with `sm_mkrel`, so that recovery keeps their
column families (§9.1). Both are dropped when the record flips
to COMMITTED. For a prepared transaction with a very large write
set the list spills to additional keys `xid | chunkno` in the
same CF.

### 3.3 `CF:meta` — bootstrap state

Format version, and the durable high-water marks for the timestamp
and relid allocators (§4.2).

### 3.4 The status resolution rule

Given a version's `xid`, a reader determines the writer's fate as:

1. `xid` is in the in-memory **active transaction table** → still
   running or prepared; not visible (unless it is the reader's own).
2. `xid` is in the in-memory **commit cache** → use its `commit_ts`.
3. Look up `CF:txn`. `COMMITTED` → `commit_ts`; `ABORTED` → dead.
4. **No record, and `xid < recovery_watermark`** → the writer died
   before committing. Dead.

Step 4 is what makes crash recovery free of any undo pass: a
crashed transaction leaves versions in relation CFs with no
status record, and absence of a record for an id below the
watermark *is* the abort. `recovery_watermark` is set at startup
to the persisted timestamp high-water mark, so no live
transaction can be misjudged (§4.2).

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

The counter is volatile, so `CF:meta` holds `next_ts`, advanced in
**leases**: the allocator persists `global_ts + LEASE` (e.g. 1M) and
only re-persists when the counter reaches the persisted value. On
startup, `global_ts` resumes at the persisted value — never below any
id ever handed out. `recovery_watermark` is set to that same value,
so every `xid` from before the crash is `< recovery_watermark` and
rule 3.4/4 applies to it. Relids are allocated the same way.

### 4.3 `visible_ts`: making snapshots correct

A snapshot must not be able to see a *gap* — a transaction with
`commit_ts <= snapshot_ts` that has not yet published its status.
Rather than maintaining an in-flight-transaction list per snapshot
(Postgres-style), commits are **published in `commit_ts` order**:

```
commit sequencer (ring of slots, indexed by commit_ts)

  ts:   ...  104     105     106     107     108  ...
        [done]  [done]  [ WAL ]  [done]  [ ... ]
                          ▲
                  visible_ts = 105 — cannot advance past the
                  first unfinished slot, so 107 stays hidden
                  until 106 lands
```

- A committing transaction claims `commit_ts`, writes its status
  record (WAL-synced), then marks its slot done.
- A single lightweight advancer moves `visible_ts` forward over the
  contiguous run of done slots.
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
    resolve V.xid  →  commit_ts | dead | in-flight
    visible iff resolved COMMITTED and commit_ts <= snapshot_ts
```

A tombstone that is visible means *the key does not exist*.

---

## 5. Write path and write-write conflicts

Writes are eager: `sm_insert`/`sm_update`/`sm_delete` put a version
into RocksDB immediately, before commit.

```
sm_update(rel, k, v):
  1. relid  = catalog.lookup(rel)                       # MVCC read
  2. lock   = lockmgr.acquire_x(relid, k, T)            # blocking, §5.1
  3. latest = newest committed version of (relid, k)
     if latest.commit_ts > T.snapshot_ts:               # §5.2
         abort with SM_ERR_CONFLICT
     if latest is absent or tombstone: SM_ERR_NOTFOUND  # (insert: DUPKEY)
  4. if T.xid == 0: T.xid = global_ts++
  5. db.Put(cf(relid), encode(k, ~T.xid, ~T.seq++), encode(flags, v))
  6. T.undo.push(relid, k, seq)                         # for savepoint
  7. ssi.on_write(T, relid, k)                          # §7
```

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

### 5.2 First-committer-wins

Step 3 rejects a write when the key was modified by a transaction that
committed after our snapshot. Together with the row lock (which
prevents the concurrent case), this is the standard snapshot-isolation
write rule and it is what makes the §4.1 invariant hold. It applies at
**all three isolation levels**, including read committed — with the
caveat noted in §7.4.

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

A fresh `snapshot_ts = visible_ts` is taken **at the start of each
statement** (each `sm_read`, and each `sm_scan` — a scan keeps its
snapshot for the life of the cursor, so one scan is internally
consistent). Writes still obey §5.2, so a write can fail with
`SM_ERR_CONFLICT` even here; the engine does not silently re-read the
newer row (no Oracle-style restart), it reports the conflict.

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
`T.created`, a vector of relids in creation order.

```
sm_savepoint():
    T.sp_mark   = T.undo.size()        # replaces any previous savepoint
    T.sp_locks  = T.locks.size()
    T.sp_rels   = T.created.size()

sm_rollback():
    m = (savepoint exists) ? T.sp_mark : 0          # else roll back to begin
    for e in reverse(T.undo[m..]):
        db.Delete(cf(e.relid), encode(e.key, ~T.xid, ~e.seq))
    r = (savepoint exists) ? T.sp_rels : 0
    for relid in reverse(T.created[r..]):
        db.DropColumnFamily(cf(relid))              # §11
    T.created.truncate(r)
    T.undo.truncate(m)
    T.seq_hwm = (m > 0) ? T.undo[m-1].seq : 0       # own-write visibility
    release locks acquired after the mark
    ssi read set is NOT rolled back                  # conservative, correct
    transaction stays ACTIVE
```

Rolling back is a delete of exactly the versions written since the
mark. Nothing else can see them, so there is no visibility window to
worry about, and `seq` guarantees a re-write of an already-written key
did not clobber the pre-savepoint version. `seq_hwm` in the own-write
visibility test (§4.4) means a rollback is correct even before the
deletes are physically applied.

Releasing locks acquired after the mark is safe: the row was untouched
before the savepoint, so no one observed our intent on it.

### 6.6 Abort

`sm_abort` writes an `ABORTED` status record (not WAL-synced — it is a
hint; absence of a record means the same thing after a crash),
drops the column families of relations in `T.created` (no other
transaction can see them), releases locks and SSI state, and
returns. The versions stay in their relation CFs until GC (§8.3).
Abort is O(1) in the write-set size.

---

## 7. SSI — serializable snapshot isolation

Cahill/Ports-style: run at snapshot isolation, track **rw-antidepen-
dencies**, and abort a transaction that sits at the pivot of a
dangerous structure.

### 7.1 What is tracked

- **SIREAD locks.** Every read by a SERIALIZABLE transaction records
  what it read: a point read records `(relid, userkey)`; a scan
  records the **range actually traversed**, `(relid, [lo, hi))`, which
  is the predicate. Ranges are the phantom defence — a later insert
  into that range conflicts even though the row did not exist at read
  time. SIREAD locks survive the transaction's own commit; they are
  dropped only when no active snapshot could still conflict with them.
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
created (mark the pivot as doomed; it fails at its own commit). This
is conservative — some aborted transactions would in fact have been
serializable — which is the accepted cost of SSI in exchange for never
taking a read lock and never deadlocking on reads.

### 7.4 Interaction with the other levels

Only SERIALIZABLE transactions take SIREAD locks or are candidates for
serialization failure. But a SERIALIZABLE transaction's correctness
depends on seeing *writes by everyone*, including RR and RC ones, so
the write path (§5, step 7) always calls into the SSI tracker.
Read-only serializable transactions get the standard safe-snapshot
optimization: if no concurrent transaction has an out-conflict, the
read-only transaction can never be a pivot and its tracking is dropped.

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
  1. if SERIALIZABLE: ssi.validate(T)         → may SM_ERR_SERIALIZATION
  2. commit_ts = global_ts++;  slot = sequencer.claim(commit_ts)
  3. batch = { CF:txn[T.xid] = {COMMITTED, T.snapshot_ts, commit_ts} }
  4. db.Write(batch, sync = durability_sync)   ← group-committed
  5. commit_cache.insert(T.xid → commit_ts)
     slot.done();  advancer moves visible_ts                    (§4.3)
  6. lockmgr.release(T);  ssi.on_commit(T)
```

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
    if committed and commit_ts > gc_watermark → KEEP as-is
    if a newer version of the same userkey with
       commit_ts <= gc_watermark was already seen → DROP
    otherwise                            → KEEP, and stamp commit_ts
                                           into the header (hint bit)
```

The stamping is why the header carries `commit_ts`: over time most
versions answer visibility without a status lookup at all, so the
commit cache and `CF:txn` are consulted mainly for the recent tail.

`gc_watermark = min(snapshot_ts of active sessions, snapshot_ts of
prepared transactions, oldest open scan)`. A `CF:txn` record can be
dropped once no version referencing it survives; conservatively,
records with `commit_ts < gc_watermark` are dropped on a periodic
sweep after a full compaction pass of every relation CF. One
compaction filter factory serves all relation CFs.

Dropping a relation (`sm_rmrel`) marks it dropped in the catalog at
commit; once `gc_watermark` passes that commit, a background task
issues `DropColumnFamily` on the relation's CF.

### 8.4 Two-phase commit

`sm_prepare` makes the transaction durable but not visible.

```
sm_prepare(T, gid):
  1. if SERIALIZABLE: ssi.validate(T)         ← validation happens HERE
  2. rec = {PREPARED, xid, gid, snapshot_ts, writeset = keys of T.undo,
            created = T.created}
  3. db.Write({CF:txn[T.xid] = rec}, sync = true)      ← always synced
  4. T.state = PREPARED;  locks and SSI state are RETAINED
```

After a successful prepare the transaction is in-doubt: its
versions sit in relation CFs, its status record says `PREPARED`,
and readers treat it as in-flight (invisible). It holds its row
locks, and it pins `gc_watermark` at its `snapshot_ts`.

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
validation, so they were checked against each other — which is what
makes the post-recovery state (§9.3) safe without replaying the SSI
graph.

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
3. global_ts = next_ts;  recovery_watermark = next_ts;
   visible_ts = next_ts.
4. Scan CF:txn for records in state PREPARED.
   (COMMITTED / ABORTED records need no action.)
5. For each prepared record:
      - register an entry in the active transaction table (in-doubt)
      - re-acquire an X row lock for every key in its writeset
      - pin gc_watermark at its snapshot_ts
6. Orphan CF sweep: read the catalog (rel.0) and drop every
   rel.<relid> CF that has neither a committed, not-dropped
   catalog row nor an entry in a prepared record's `created`.
7. Start the GC / compaction filter, deadlock detector, commit advancer.
8. Open for sessions.
```

Step 6 catches the column families that eager cleanup (§6.5,
§6.6) never reached: a crash between `CreateColumnFamily` and
commit, and a committed drop whose `DropColumnFamily` had not yet
run. Creating a CF is durable in the RocksDB MANIFEST before
`sm_mkrel` writes its catalog row, so a committed catalog row
never lacks its CF.

Step 3 is the whole of "abort recovery": every transaction that was
running at the crash has `xid < recovery_watermark` and no status
record, so rule §3.4/4 declares it dead the first time anyone looks at
one of its versions. Its orphaned versions are dropped by the next
compaction. No scan of the data is needed, and recovery time is
independent of the size of the crashed transactions.

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
    relid = catalog.lookup(rel)
    snap  = (level == READ_COMMITTED) ? visible_ts : T.snapshot_ts
    it = db.NewIterator(cf(relid))
    it.Seek(key | ~snap | 0xFFFFFFFF)
    while it.Valid() and userkey(it.key()) == key:
        v = decode(it)
        if visible(v, snap, T):                          # §4.4
            if SERIALIZABLE: ssi.on_read(T, relid, key, v)
            return v.tombstone ? NOT_FOUND : v.payload
        it.Next()                                        # skip invisible
    return NOT_FOUND
```

The loop normally runs once. It iterates only past versions written by
transactions that are concurrent with this reader on this exact key.

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

`sm_scan_next` walks version keys, and for each distinct `userkey`
emits the first visible version and then skips the rest of that key's
versions:

```
while iterator valid:
    uk = userkey(it.key())
    v  = first visible version in the run of versions for uk
    seek past the run (Seek to uk|0xFF..)            # or Next()-chase
    if v exists and not tombstone: yield (uk, v.payload); return
```

For SERIALIZABLE, `sm_scan_end` registers the SIREAD range actually
covered, `[skey, last_key_examined]`, as the predicate (§7.1). If the
cursor is abandoned early, the range recorded is the shorter one
actually read — which is both correct and tighter than the requested
range.

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
  usefully fail. It is still where the range the cursor
  actually traversed gets registered for conflict tracking
  (§7.1), so a serializable transaction must call it — the
  registered range is the part traversed, not the part
  requested, which makes closing early both correct and
  tighter than draining.

---

## 11. Catalog and DDL

The catalog is relation `relid = 0`, an ordinary MVCC relation in
`CF:rel.0`:

```
key = "R" | relname        →  { relid, created_ts, dropped (bool) }
key = "N"                  →  next relid hint (also mirrored in CF:meta)
```

Because it is an ordinary relation, DDL is transactional for free:
`sm_mkrel` inserts a catalog row, `sm_rmrel` writes a version with
`dropped = true`, and both roll back with the transaction. A session
caches `relname → relid` and invalidates the cache entry when it
observes a catalog version newer than the cached one.

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

`sm_mkdb` creates the directory, `CF:txn`, `CF:meta` with its
initial contents, and the catalog's `CF:rel.0`. `sm_rmdb` is
`rocksdb::DestroyDB` after verifying no session in this process
holds the path open.

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
SM_ERR_INUSE           database still attached in this process
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
`ssi_max_entries`, `ts_lease`, plus pass-through RocksDB options
(block cache size, write buffer size, compaction style). Bloom filters
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
src/db.cc               mkdb/rmdb, open registry, CF setup, relid→CF map
src/session.cc          attach/detach, per-session state and buffers
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
   `mkdb/attach/mkrel/insert/read/scan` under one transaction at a time.
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
- **Isolation.** The Jepsen/Adya anomaly catalogue as unit tests: dirty
  read, lost update, non-repeatable read, phantom, write skew (must
  pass at SERIALIZABLE, must be *observable* at RR — asserting the
  anomaly is present is as important as asserting it is absent).
- **Crash.** A fault-injection harness that kills the process at every
  RocksDB write boundary during a workload, restarts, and checks that
  the surviving state is exactly one of the legal prefixes of the
  commit order.
- **2PC.** Kill between prepare and commit; verify the transaction is
  restored in-doubt, and that both resolutions produce the right state.
- **GC.** Assert that a long-running reader's snapshot is never
  violated by the collector, and that space is actually reclaimed once
  it finishes.
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
