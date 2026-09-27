
# DESIGN — wisr storage manager

A multi-version (MVCC) storage manager for OLTP, layered over RocksDB.
RocksDB is used only as an ordered, durable, crash-atomic key-value
store; every notion of *row version*, *transaction*, *isolation*, and
*visibility* is implemented here.

This document is the design of record. It follows the decisions in
`SM1_OBJECTIVE.md`:

| Decision | Choice |
| --- | --- |
| Isolation mechanism | Snapshot isolation everywhere; SERIALIZABLE via SSI (optimistic) |
| Uncommitted writes | Written eagerly into RocksDB, resolved via a commit-status table |
| `sm1::prepare` | Real 2PC with a durable prepare record |
| Deployment | One process, many threads, one shared RocksDB instance |

---

## 1. Scope and non-goals

**In scope.** Stores as ordered key→value maps; MVCC reads and
scans; read-committed / repeatable-read / serializable transactions;
nested savepoints; two-phase commit; crash recovery; garbage
collection of dead versions.

**Not in scope (for now).** Secondary indexes, a query language,
column types or schemas (values are opaque byte strings), replication,
multi-process or remote access, and online backup. Nothing in the
design forecloses them.

**Not in scope, ever.** Backward scans. Iteration is forward
only, in ascending bytewise key order, and no index serves a
backward scan — not the clustered primary key here, and not a
secondary index a layer above builds out of stores
(`SM1_OBJECTIVE.md`). Descending order is built into the key —
an index whose key bytes are inverted, scanned forward — and
the stack above keeps no mirror copy of an index for the other
direction (`PART4.md` §5).

The engine cost is part of why. A key's versions are stored
newest first (`key|~wts|~seq`, §3.1), so a backward scan
would meet the oldest version first and have to walk to the
start of each key's run before picking the visible one; it
would need `SeekForPrev` to work with §10.2's prefix-seek
setup; and for SSI the traversed range would run from the last
key examined up to `ekey`, inverting §10.2's bookkeeping.

**Phases.** Phase 1 is a solid MVCC storage manager: the API in
`sm1.h` and the design in this document, nothing more. Phase 2
adds the API a secondary-index layer built on top of it needs
(`sm3/SM3_DESIGN.md` §3), and it is built:
`sm1::lsstore`, which came with naming stores by storeid (§11);
`sm1::stmt_begin`, for read-committed statement snapshots
(§6.2); the savepoint stack with `sm1::forget` (§6.5);
`sm1::lock`, whose shared mode makes a safe index build
possible (§5.1); `sm1::scan_seek`, which repositions a cursor
instead of reopening it (§10.2); and the old row returned by
`sm1::update` and `sm1::erase` (§5.3). Descending scans will
not be added at all, and a batched read is left until a
benchmark asks for it (§10.1).

**Assumptions.**

- A `session_t` is used by one thread at a time. Concurrent use of one
  session from two threads is a caller error, not a supported mode.
- A process reaches its databases through an environment
  (`sm1::env_t`) on a home directory. `sm1::init` prepares a
  home directory once; `sm1::attach` gives one process, and
  one environment in it, the directory and every database
  under it. The library does not enforce that: keeping to
  one attached process is the caller's job (§3.5). A
  database is named
  by the caller but stored by number: the root db
  `{homedir}/global/root` maps `dbname` to a `dbid`, and
  the database is the directory `{homedir}/base/{dbid}`
  (§3.5).
  Multiple sessions may open the same database; they share
  one RocksDB handle, held in the environment's registry of
  open databases.
- Keys and values are arbitrary byte strings. Keys are ordered
  bytewise (`memcmp`), which is what `sm1::scan` exposes.
- Implementation language is C++ (RocksDB's native API), and
  so is the interface, in `namespace sm1`; only the top level
  wraps the stack in a C API (`DESIGN.md` §5 at the top
  level).

---

## 2. Architecture

```
        ┌──────────────────────────────────────────────────────┐
        │ sm1:: C++ API (session, xct, store, insert/read/scan)│
        └───────────────────────┬──────────────────────────────┘
                                │
   ┌────────────┬───────────────┼───────────────┬──────────────┐
   │            │               │               │              │
┌──┴──────┐ ┌───┴──────┐ ┌──────┴──────┐ ┌──────┴─────┐ ┌──────┴─────┐
│ Catalog │ │ Access   │ │ Transaction │ │ Lock mgr   │ │ SSI        │
│ (storeid│ │ methods  │ │ manager     │ │ (row X     │ │ tracker    │
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
                 │  CF:store.*  CF:txn   CF:meta   WAL   compaction│
                 └─────────────────────────────────────────────────┘
```

The only durable state is inside RocksDB. Everything else — locks,
SSI graph, active-transaction table, the commit-timestamp cache — is
volatile and reconstructible (§9).

---

## 3. Physical layout

A home directory is laid out like a PostgreSQL data
directory: the root db in `global`, one directory per
database under `base` (§3.5). Within a database: one
column family per store, plus two fixed ones: `CF:txn`
and `CF:meta`.

### 3.1 `CF:store.<storeid>` — row versions

Each store's versions live in their own column family, named
by `storeid` in decimal (`store.0` is the catalog, §11).

```
key   = userkey (variable)
      | ~wts   (8B, big-endian, bitwise complement)
      | ~seq   (4B, big-endian, bitwise complement)

value = flags   (1B)   bit0 = tombstone (this version is a delete)
                       bit1 = commit_ts is stamped in this header
      | xid      (8B) the writer, while unstamped (flags bit1 clear)
        commit_ts (8B) in its place once stamped; a hint, see §8.3
      | payload  (variable, absent for a tombstone)
```

What a tuple looks like, then: there is no "row" object on
disk, only a chain of versions, one RocksDB key-value pair
each. Take the row `"acct42"`, written by the transaction
with `xid` 50 as its first write (`seq` 1, write timestamp
`wts` 51), then updated by `xid` 60 (`wts` 61), and committed
at 70 and 80. The figure shows what sits in `CF:store.<storeid>`,
in key order, newest first:

```
key                                          value
-------------------------------------------  ---------------------
userkey        ~wts              ~seq        flags  commit_ts  payload
61 63 63 74    FFFFFFFFFFFFFFC2  FFFFFFFE    02     0000...50  "bal=9"
34 32          (wts 61)          (seq 1)            (80)
61 63 63 74    FFFFFFFFFFFFFFCC  FFFFFFFE    00     0000...32  "bal=7"
34 32          (wts 51)          (seq 1)            (xid 50)
                                             unstamped: the writer
                                             has not been swept by
                                             compaction yet, so its
                                             status is looked up in
                                             CF:txn (§3.4)
```

The `wts`-61 version is stamped because compaction has already
visited it and copied the commit timestamp in as a hint (§8.3),
replacing its `xid`; the `wts`-51 version has not been visited,
so it still carries the `xid` that names its writer. A delete
looks the same with `flags = 01` and no payload. RocksDB adds its
own 8-byte sequence-and-type trailer after our key inside its
files. It is RocksDB's business, invisible to the comparator
below, and unrelated to our `seq`.

Notes on the encoding:

- **`storeid`** is a 64-bit id the storage manager allocates at
  `sm1::mkstore` and hands to the caller. It names the column
  family, `store.<storeid>`. A storeid is never reused (§4.2), so
  it never names two stores, and a dropped store is removed with
  one `DropColumnFamily`. The key itself carries no storeid; the
  column family is the store.
- **`wts`**, the write timestamp, is taken from the one counter
  (§4.1) after the writer holds the row lock, so a key's
  versions are in commit order by construction. Complemented,
  it makes versions of a key sort **newest first**. A point
  read becomes one `Seek` to `userkey|~snapshot_ts|0x00000000`,
  which lands on the newest version whose `wts <= snapshot_ts`
  — usually the answer. A transaction that has written seeks
  from the larger of its own newest `wts` and its snapshot
  instead (§10.1). The writer's `xid` is not in the key; it is
  in the value, for status resolution (§3.4).
- **`seq`** is a per-transaction write counter, starting at 1.
  It exists so that a transaction writing the same key twice
  creates two distinct versions, which is what makes savepoint
  rollback a pure delete (§6.5) instead of requiring an undo
  pre-image. Complemented so the latest write within a
  transaction sorts first.
- `userkey` is stored raw and the two suffixes are fixed-width, so a
  version key is parsed from the tail. No escaping needed.
- **Comparator.** Store CFs use a custom comparator,
  `wisr.version.v1`: compare the `userkey` parts (key minus the
  12-byte tail) with `memcmp`, shorter-is-less on a tie, then
  compare the tails with `memcmp`. RocksDB's default bytewise
  comparator is wrong here. With it, a key's tail is compared
  against the extra bytes of a longer userkey that begins with
  it: the versions of `"a"` (`61 FF FF …`, since `~wts` of any
  wts below 2^56 starts with `FF`) sort after those of `"ab"`
  (`61 62 …`). Scans would return `"ab"` before `"a"`, and a
  point read could stop early on a foreign key. With the custom
  comparator, a key's versions are contiguous and userkeys
  sort in `memcmp` order, which is the order `sm1::scan` exposes.

  Every key handed to RocksDB for a store CF — writes, seek
  targets, iterator bounds — carries the full 12-byte tail.
  `FindShortestSeparator` and `FindShortSuccessor` are no-ops
  (safe; index blocks are a little larger). RocksDB persists the
  comparator name and refuses to open a CF with another, so the
  name is part of the format: changing the ordering means a new
  name and a rewrite. `CF:txn` and `CF:meta` keep the bytewise
  comparator.

Column family handles are held in a process-wide map
`storeid → handle`, shared by every session open on the
database and guarded by a reader-writer lock, because stores
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
          writeset: [ (storeid, userkey) ... ]   (PREPARED only),
          created:  [ storeid ... ]            (PREPARED only),
          dropped:  [ storeid ... ]            (PREPARED only) }
```

Only **writing** transactions ever get an entry, plus read-only
ones that prepare (§8.4), and only at prepare/commit/abort time —
nothing is written at `sm1::begin`. Absence of a record is
meaningful (§3.4).

The `writeset` is carried only in a `PREPARED` record, and only
so that recovery can re-acquire the row locks a prepared
transaction still holds. `created` lists the stores the
transaction made with `sm1::mkstore`, so that recovery keeps
their column families (§9.1); `dropped` lists those it removed
with `sm1::rmstore`, so that a commit after restart still queues
their column families for removal (§8.3). `prepared_at` is the
wall-clock time of `sm1::prepare`, reported by
`sm1::list_prepared` (§8.5); it must be durable because the
listing is mostly needed after a restart. All four are dropped
when the record flips to COMMITTED or ABORTED. For a prepared
transaction with a very large write set the list spills to
additional keys `xid | chunkno` in the same CF, deleted in the
same batch as the flip.

### 3.3 `CF:meta` — bootstrap state

Format version, the durable high-water marks for the timestamp
and object-id allocators (§4.2), and one lease per sequence in
use, keyed `"seq." | be64(seqid)` (§11).

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
crashed transaction leaves versions in store CFs with no
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

### 3.5 The home directory and the root db

A home directory follows PostgreSQL's layout, one name per
concept:

```
{homedir}/global/root      the root db              (global/pg_database)
{homedir}/base/{dbid}/     one per database         (base/{oid})
```

A `dbname` is a caller's string, not a path component
(`API.md`). The mapping from it to storage lives in the
**root db**, `{homedir}/global/root`: an ordinary RocksDB
with the default column family, the bytewise comparator,
and none of the machinery above. It is not an
`sm1::session_t` database, has no `CF:txn`, `CF:meta`, or
catalog, and is unreachable through `sm1::opendb`. It is the
`pg_database` of this design, and it sits in `global` for
the same reason: it describes the whole home directory
rather than any one database in it.

```
key = "D" | dbname        →  dbid (8B, big-endian)
key = "M" | "next_dbid"   →  8B high-water mark
key = "M" | "version"     →  home directory format version
```

A database directory is named by its `dbid` in decimal, so
`base` holds nothing else and there is no name space to
share: whatever a caller names a database, the name never
reaches the filesystem, and `base` cannot collide with
`global`.

`sm1::init` builds all of this once in a home directory's
life: `base`, `global`, the root db, and the `"version"`
key in it. It takes no lock, because `mkdir` settles a race
by itself — the loser sees `EEXIST`. `sm1::attach` then opens
the root db and checks that version, and `sm1::detach` closes
it. Nothing creates a home directory
implicitly, so `sm1::attach` has no first-use branch and a
mistyped path is an error rather than a new empty home.

**One process per home directory is the caller's rule.**
The library takes no lock of its own: a caller that runs
several processes serializes their use of a home directory
itself. RocksDB still locks each database directory it
opens, the root db included, so a second attach usually
fails there, but only per database; the work that spans the
home directory — the sweep of `base` at attach, `sm1::mkdb`
choosing a `dbid`, `sm1::rmdb` checking for open sessions —
assumes it is alone, and a second process breaks it.

`sm1::detach` refuses while sessions are open: after it
returns the caller may hand the home directory to another
process, which must not find this one's sessions still
writing to it. Its `force` flag closes those sessions
first, by the same path `sm1::closedb` takes, so a caller
winding down — especially after an error, when it may not
be able to walk its own session list — has one call that
always completes. Force changes nothing about what closing
a session means: a prepared transaction stays in doubt and
is found again by the recovery of the next `sm1::opendb`
(§9.1), because it is durable and owned by its
coordinator, not by the process that happened to prepare
it.

One process holds the root db, so it needs no concurrency
control beyond a mutex; writes to it are synchronous.

**The mapping is the truth.** A directory no `"D"` row
points at is garbage, and the two operations are ordered so
that a crash can only ever leave that:

- `sm1::mkdb` bumps `next_dbid` durably, creates
  `{homedir}/base/{dbid}` with its `CF:txn`, `CF:meta`,
  and `CF:store.0`, and only then commits
  `"D" | dbname → dbid`.
- `sm1::rmdb` deletes `"D" | dbname`, and only then runs
  `rocksdb::DestroyDB`.

Neither can leave a name pointing at a directory that is
missing or unbuilt. `sm1::attach` sweeps the other way: every
entry of `base` whose `dbid` is in no `"D"` row is
destroyed, before any database is opened. A `dbid` is never
reused, on the same reasoning as a `storeid` (§4.2), so the
sweep cannot mistake a live database for the remains of a
dead one.

This is the §9.1 step 6 pattern one level up — eager
cleanup on the happy path, a startup sweep for the crash
path — and it is why a name never reaches the filesystem at
either level: a store is `CF:store.<storeid>`, a database is
a directory named by `dbid`.

---

## 4. Timestamps, ids, and snapshots

### 4.1 One counter

A single monotonically increasing 64-bit atomic counter,
`global_ts`, produces both timestamps and transaction ids. A
transaction's **id is its first-write timestamp**: `xid == start_ts`.
Read-only transactions never allocate one.

- `sm1::begin` takes a **snapshot**: `snapshot_ts = visible_ts`
  (§4.3). It does *not* burn a counter value.
- The first write in a transaction allocates `xid = global_ts++`.
  Because `snapshot_ts <= visible_ts < xid`, always `xid > snapshot_ts`.
- Every write allocates a **write timestamp** `wts = global_ts++`
  after it holds the row lock (§5), and keys its version by it
  (§3.1). Commit allocates `commit_ts = global_ts++`, so
  `commit_ts > wts` for every write of the transaction, and
  `commit_ts > xid`.

**Invariant (version chain order).** For one key, committed versions in
descending `wts` order are also in descending `commit_ts` order.

*Why.* Two transactions that both write key `k` are serialized by
the row lock (§5), and a writer takes its `commit_ts` before it
releases it. If `T2` committed a version of `k` after `T1` did,
`T2` acquired the lock after `T1` released it, so `T2.wts >
T1.commit_ts > T1.wts`. ∎

The argument uses no snapshot, so it holds at every level,
including READ COMMITTED writes that pass a newer commit (§5.4).
This is what lets a reader seek by `wts` and get commit order.
Ordering by `xid`, fixed at the first write, would not: a
transaction whose `xid` predates another's commit can write the
same key after it (§6.2).

`global_ts` is an atomic. An `xid` or a `wts` is a bare
`fetch_add` (§4.3); only `commit_ts`, and a lease renewal, take a
mutex.

### 4.2 Surviving restart

The counter is volatile, so `CF:meta` holds `next_ts`, advanced
in **leases**: the allocator persists `global_ts + LEASE` (e.g.
1M) and only re-persists when the counter reaches the persisted
value. On startup, `global_ts` resumes one past the persisted
value — above any id ever handed out. `visible_ts` is set to the
persisted value itself, so every `xid` allocated after restart is
`> visible_ts`, keeping §4.1's `xid > snapshot_ts`. The persisted
value itself is never handed out. Object ids — storeids and
seqids, from one allocator `next_objid`, so the two never
collide — are allocated the same way, but never below 1000:
0 is the catalog store, and 1 to 999 are reserved for objects
a database is made with, so none is ever handed out. A
database made before the rule resumes at 1000 when it opens.

A named sequence (§11) has a lease of its own, `"seq." |
be64(seqid)`, holding the first value **not** covered by the
lease. It is the same scheme with the sequence's increment as
its step, and it is not transactional: a value is taken under
the allocator's mutex, never given back, and a crash skips the
rest of the lease. A sequence with no lease row yet has handed
out nothing, so it starts at its `start`.

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
- `global_ts` is an atomic. An xid or a write timestamp (`wts`)
  is a bare `fetch_add` with no mutex; only a lease renewal
  takes `alloc_mu`. Those values never enter the set, so only
  commits in progress can hold back `visible_ts`, and the
  advancer passing one of them is harmless: no status is
  published for it, and a version it keys resolves through
  its writer's status like any other. Only `commit_ts` needs
  the mutex, so that its `fetch_add` and its insert into the
  set are one step to the advancer.
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

Version `V`, keyed by `wts` and written by `xid` (in its value
while unstamped), read at `snapshot_ts` by transaction `T`:

```
if V.xid == T.xid:                      # own write
    visible iff V.seq <= T.seq_hwm      # savepoint-aware, §6.5
elif V.wts > snapshot_ts:               # written after the snapshot,
    invisible                           # so committed after it too
else:
    resolve V (§3.4)  →  commit_ts | dead | in-flight
    visible iff resolved COMMITTED and commit_ts <= snapshot_ts
```

A tombstone that is visible means *the key does not exist*.

---

## 5. Write path and write-write conflicts

Writes are eager: `sm1::insert`/`sm1::update`/`sm1::erase` put a
version into RocksDB immediately, before commit.

```
sm1::update(storeid, k, v):
  1. catalog.lookup(storeid)                           # MVCC read, §11
  2. lock   = lockmgr.acquire_x(storeid, k, T)          # blocking, §5.1
  3. latest = newest own version of (storeid, k) visible to T  # §4.4
     if latest is absent:
         latest = newest committed version of (storeid, k)
         if latest.commit_ts > T.snapshot_ts:
             RR, SER: abort with SM_ERR_CONFLICT        # §5.2
             RC: SM_ERR_UPDATED, unless T.lock(k).latest
                 (an insert: no check)                  # §5.4
     if latest is absent or tombstone: SM_ERR_NOTFOUND  # (insert: DUPKEY)
     if old wanted: copy latest's payload into the session buffer  # §5.3
  4. if T.xid == 0: T.xid = global_ts++
  5. wts = global_ts++                                  # after the lock, §4.1
     seq = T.seq++;  T.seq_hwm = seq                   # T.seq starts at 1
     db.Put(cf(storeid), encode(k, ~wts, ~seq), encode(flags, T.xid, v))
     T.lock(k).latest = false                          # §5.4
  6. T.undo.push(storeid, k, wts, seq)                  # for savepoint
  7. ssi.on_write(T, storeid, k)                        # §7
```

Step 3 consults the transaction's own writes first, so a key it
inserted can be updated and a key it deleted can be re-inserted.
An own version still visible under `seq_hwm` means the row lock
has been held since that write, so no other transaction can have
committed `k` in between and the conflict check is skipped. Own
versions undone by `sm1::rollback` are invisible (§6.5), and the
check falls through to the committed version as if they had
never been written.

Step 5 is an ordinary RocksDB `Put` into the memtable, WAL-logged but
**not synced** — an uncommitted write does not need durability, only
the commit record does (§8.2).

### 5.1 Row locks

An in-memory hash table, `(storeid, userkey)` → the
transactions holding it. A lock is held **shared** by any
number of them or **exclusive** by one; the compatibility
table is the usual one, S/S being the only pair that
coexists. A write takes it exclusive, `sm1::lock` takes either,
and a plain read still takes none. Locks are released at
commit/abort — strict, so no cascading aborts. The key is
exact, so two distinct keys never share an entry and never
block or deadlock each other. One mutex covers the table and
every waiter's waits-for list, which keeps a deadlock walk
consistent; sharding it is a later optimization if a
benchmark shows contention.

**Upgrades.** A transaction holding a key shared and asking
for it exclusive — including by writing it — upgrades: it
waits for the other shared holders to go, then flips the
entry, without ever releasing what it holds. An upgrade is
what makes two transactions on ONE key deadlock, which
exclusive-only locking could not do.

**A read-only transaction can hold locks.** Before `sm1::lock`,
a transaction with no `xid` held nothing, and the read-only
commit path skipped lock release for that reason. It no
longer can.

Deadlocks are found as PostgreSQL's `deadlock_timeout`
finds them. A blocked transaction sleeps `deadlock_ms`
(default 50ms), then searches the waits-for graph from
itself. If it can reach itself, the **youngest** transaction
on that cycle — the last to begin — fails with
`SM_ERR_DEADLOCK`. Short waits never pay for the search and
there is no detector thread.

The waits-for edges form a **graph, not a chain**: a waiter
blocked on a shared lock waits on every holder at once, so
the search is a depth-first walk with a visited set, and the
path it is on supplies the cycle to pick a victim from. This
is the cost of shared mode. Before it, only writers could
wait, each on exactly one holder, and following the single
pointer was enough.

A transaction appends a lock entry to `T.locks` only when it
first acquires it. Re-acquiring a key it already holds
appends nothing, so §6.5's rollback releases only entries
first taken after the savepoint.

**Telling the victim.** The victim need not be the waiter
that found the cycle. It carries an atomic `doomed` field
holding the reason: the finder stores it and wakes every
waiter, and the victim — blocked elsewhere on the same
cycle, since every member of a cycle is waiting — returns
the reason from its own wait. A transaction that is doomed
while running, between `sm1::` calls, where there is nothing
to wake, finds the field at the entry of its next call,
which fails with the reason and does nothing else. Every
entry point checks `doomed` first, so a doomed transaction
can only be aborted. Deadlock detection only ever dooms a
waiter; the running case is for the doom flag that §8.4
sets on a serializable reader.

**What a transaction holds in memory.** Row values stream
to RocksDB, so a transaction is bounded by disk, but
three structures are not: `T.locks` (one entry per
distinct key written), `T.undo` (one `(storeid, userkey,
wts, seq)` tuple per write), and the SSI read set. All three
are linear in the number of keys touched, with the
userkey stored inline, so a transaction writing tens of
millions of keys costs gigabytes. This is the price of
O(1) abort and no undo pass, and it is a real limit on
single-transaction size rather than a bug; §13 records
it.

### 5.2 First-committer-wins

Step 3 rejects a write when the key was modified by a transaction
that committed after our snapshot. Together with the row lock
(which prevents the concurrent case), this is the standard
snapshot-isolation write rule and it is what makes the §4.1
invariant hold. It applies at **repeatable read and
serializable**. Read committed does not abort the loser: it fails
the write with `SM_ERR_UPDATED` and lets it go ahead once the
caller has re-read the row (§5.4). The caveat for the
non-serializable levels is §7.4's: their writes still feed the
SSI tracker, but they are never serialization victims themselves.

### 5.3 The old row

`sm1::update` and `sm1::erase` hand back the row they replace,
through an optional out-parameter a caller may leave
`nullptr`. It costs
nothing to produce: step 3 already seeks to the deciding
version and decodes its value to learn whether the key
exists, and until now threw the payload away. A layer
maintaining secondary indexes needs that value — the old
entries to delete are computed from it — and without this it
paid a whole `sm1::read`, an iterator and a seek of its own,
for something the write had in hand.

The bytes go in the session's read buffer, the one `sm1::read`
uses, so either call invalidates what the other returned.
Sharing it is deliberate: one buffer per session, one rule to
state, and a caller that needs the value to outlive the next
call copies it, exactly as it must after `sm1::read`.

The value is only produced for a live version. A write that
fails — `SM_ERR_NOTFOUND` on a missing key, a conflict, a
deadlock — reports no old row.

### 5.4 Read committed: write, re-read, write

At READ COMMITTED an update or delete that meets a row committed
after `sm1::begin` waits for the row lock, as every write does,
and then is refused with `SM_ERR_UPDATED`. That is not an abort:
the transaction stays active, keeps the lock, and
`sm1::retryable()` is
0. The caller calls `sm1::read_latest`, which returns the newest
committed version of the row ignoring the snapshot, re-evaluates
its `WHERE` clause and new values from it, and only then repeats
the write. This is what PostgreSQL's executor does at its default
level after a lock wait, and workloads written for it rely on it
(`PART4.md` §5a).

The commonest case is a hot row, a shared counter or a
`next_order_id`. Two read committed sessions each add one to `R1`,
which holds 0; T1 updates it and T2 waits for T1's row lock:

| | T2's update, after T1 commits | R1 |
| --- | --- | --- |
| PostgreSQL | re-reads R1 (1), re-checks `WHERE`, writes 2 | 2 |
| before §5.4 | fails with `SM_ERR_CONFLICT`, T2 aborted | 1, until T2 retries |
| §5.4 | `SM_ERR_UPDATED`; T2 reads the latest (1), re-checks, writes 2 | 2 |

A client written for PostgreSQL has no retry loop, so it could not
run on the old rule. The variant is `UPDATE R1 SET n = 1 WHERE
n = 0`, claiming the row once: T2's re-check must fail and T2 must
write nothing, so letting the write through is not enough. The
caller has to look at the newest version first, and sm1 makes it.
The executor's side of the exchange belongs to the PostgreSQL
fork, not to sm1.

The re-read is enforced, not requested. `sm1::read_latest` marks
the lock entry, and the write's step 3 lets the write through
only on a marked lock. A caller cannot skip the exchange and lose
a predicate: writing again without the read is refused again.

- **The mark** is one bit on the session's own lock list
  (`sm1::session_t::lockref_t`), not on the shared lock table, so
  neither call takes `lock_mu`: only the exclusive holder reads
  or sets it. The lock is held from the refused write on, so the
  newest committed version cannot change between the read and the
  write. - **A successful write clears the mark.** Left set, a
  write after a savepoint rollback, or much later in a long
  transaction, would slip past the check without a fresh read. -
  **A rollback to a savepoint** releases the locks taken after it
  (§6.5), and their marks with them; a lock taken before the
  savepoint keeps its mark. - **`sm1::read_latest`** requires
  READ COMMITTED and a lock this transaction holds on the key,
  taken by the refused write or by `sm1::lock`; otherwise
  `SM_ERR_INVALID`. Under a shared lock it reads but sets no
  mark, since a shared holder cannot write without upgrading. A
  caller that waited in `sm1::lock(SHARED)` (a writer reading a
  schema row, §3 of `sm3/SM3_DESIGN.md`) uses it to see what
  committed while it waited. - **An insert** has no predicate to
  re-evaluate, so it is judged against the newest committed
  version (`SM_ERR_DUPKEY` if live) with no check, as
  PostgreSQL's unique check does. - **`sm1::lock`** at READ
  COMMITTED no longer fails with `SM_ERR_CONFLICT`. At the
  higher levels it is unchanged. - REPEATABLE READ, SERIALIZABLE,
  SSI (READ COMMITTED takes no part, §7.4), 2PC and the GC
  watermark are unchanged: a read committed transaction already
  pins its begin snapshot, and the newest committed version is
  never collected.

Version order needs no help from any of this: the write that goes
ahead takes a `wts` after the lock, so it sorts first (§4.1).

---

## 6. Transactions

### 6.1 Lifecycle

```
                sm1::begin
                   │  snapshot_ts = visible_ts
                   ▼
              ┌─────────┐   first write: xid = global_ts++
              │ ACTIVE  │◄──── sm1::savepoint / sm1::rollback / sm1::forget
              └────┬────┘
       sm1::prepare │        │ sm1::commit
                   ▼        ▼
            ┌──────────┐  (validate → commit_ts → status record
            │ PREPARED │   → WAL sync → publish → release)
            └────┬─────┘
   sm1::commit /  │
   sm1::abort     ▼
            COMMITTED / ABORTED
```

### 6.2 Read committed

A fresh statement snapshot `snap = visible_ts` is taken **at the
start of each read statement** (each `sm1::read`, and each
`sm1::scan` — a scan keeps its snapshot for the life of the
cursor, so one scan is internally consistent). It is a local of
the statement; `T.snapshot_ts` is still the one taken at
`sm1::begin` and is never refreshed.

**Statement boundaries.** A caller whose statement spans
several calls — an index lookup is one scan plus N reads —
would see N+1 states under that rule and could return a
mixture of them that never existed. `sm1::stmt_begin` sets a
boundary: from the first read after it, every RC read in the
session uses that one snapshot, until the next boundary or
the end of the transaction. The session state is
`T.stmt_pinned` (the boundary has been used, for the rest of
the transaction) and `T.stmt_snapshot_ts` once taken.

The snapshot is taken by the statement's first **read**, not
by the boundary call and not by a write, so a statement may
block — on a row lock, or on the `sm1::lock` a secondary-index
layer would use — and still read everything committed while
it waited. `xct_read_view` takes it; `xct_view`, which the
write path uses to validate a storeid and key, only reads it
once taken.

The GC watermark needs nothing extra. A read-committed
transaction registers its *begin* snapshot for its whole life
(§8.3), and a statement snapshot is at or above it, so
`gc_watermark` is already held below anything a pinned
statement can read.

**There is no `sm1::stmt_end`.** The next `sm1::stmt_begin`
ends the last statement, and the transaction ends the last of
all. The cost is that a transaction which uses the call once
reads that way for the rest of its life, with no way back to a
snapshot per call; the gain is one call instead of a pair to
keep balanced across rollback and prepare. A layer that wants
none of this can require REPEATABLE READ or SERIALIZABLE and
never use RC at all; that needs no engine support.

Writes check against `T.snapshot_ts`, not the statement snapshot.
A write that meets a row committed after `sm1::begin` does not
abort, as it does at the higher levels (§5.2): it waits for the
row lock, is refused with `SM_ERR_UPDATED`, and goes ahead
against the newest committed version once the caller has read it
with `sm1::read_latest` (§5.4). PostgreSQL does the same at its
default level, and workloads rely on it (`PART4.md` §5a).
REPEATABLE READ and SERIALIZABLE keep §5.2 as it is.

What makes that safe is that a version's key is its `wts`, taken
after the row lock, and not the writer's `xid` (§3.1, §4.1).
Keyed by `xid`, a read committed transaction with `T.xid = 50`
that updated a row committed at 70 would put a version `k|~50`
that sorts *behind* the committed `xid 60` version: readers would
return the older value, and GC would drop the newer commit as
superseded. Keyed by a `wts` of 75, the version sorts first, and
GC drops the older one.

The scenarios `f01`–`f10` in `mvcc/MVCC.md` execute these rules
on every build, and `e06` runs them under load with compaction.

### 6.3 Repeatable read

One `snapshot_ts` taken at `sm1::begin`, held to the end. This is
snapshot isolation: no lost updates, no non-repeatable reads, no
phantoms — but write skew is possible. That is the standard SI/RR
trade and it is what `sm1::xctlevel_t::repeatable_read` means
here.

### 6.4 Serializable

Repeatable read plus SSI validation (§7).

### 6.5 Savepoints

A savepoint is a mark into the per-transaction **undo
list** — an in-memory vector of `(storeid, userkey, wts, seq)`
tuples in write order, holding no payloads, so a large
transaction costs bytes per write, not rows. Stores
created by the transaction are tracked alongside it in
`T.created`, a vector of storeids in creation order, and
stores it dropped in `T.dropped`, likewise.

Savepoints **nest**, because a layer above needs one per
statement: index maintenance turns one row write into several,
and a unique violation on the third row of a multi-row insert
must undo the statement's partial writes. With a single
savepoint per transaction, that statement mark clobbered any
user-level SAVEPOINT, and compensating by hand instead left a
tombstone version for every write it undid. The layer pushes a
mark at each statement's start and forgets it at the end,
above whatever marks its own caller holds.

`T.savepoints` is a stack of marks in
push order, so `T.savepoints[d-1]` is the mark at depth `d`,
and a transaction holds at most `sm1::max_savepoints` (256) of
them. The cap turns runaway nesting into an error instead of
unbounded growth; nothing in the design needs that
particular value.

A depth is **reused** once the mark holding it is gone, so a
depth kept from a savepoint already rolled past or forgotten
can name a different mark later. The alternative was an id
per mark, never reused, which would make a stale reference
an error rather than a silent hit on the wrong mark. Depth
was chosen because SQL already obliges the layer above to
drop a savepoint name when its mark goes — `RELEASE`, and
rollback to an outer mark — so a correct caller cannot form
a stale depth. `API.md` states the hazard.

```
sm1::savepoint():                        # returns the new depth
    if len(T.savepoints) == sm1::max_savepoints:
        fail with SM_ERR_INVALID
    push onto T.savepoints:
        mark.undo    = T.undo.size()
        mark.locks   = T.locks.size()
        mark.stores  = T.created.size()
        mark.drops   = T.dropped.size()
        mark.scan_id = id of the latest cursor opened so far
    return len(T.savepoints)

sm1::forget(depth):                      # SQL's RELEASE SAVEPOINT
    if no mark at depth: fail with SM_ERR_NOSAVEPOINT
    T.savepoints.truncate(depth - 1)   # that mark and those inside it
    # writes stay; locks stay, now held for the enclosing mark

sm1::rollback(depth):
    if no mark at depth: fail with SM_ERR_NOSAVEPOINT # API.md divergence 8
    m = T.savepoints[depth-1].undo
    for e in reverse(T.undo[m..]):
        db.Delete(cf(e.storeid), encode(e.key, ~T.xid, ~e.seq))
    r = T.savepoints[depth-1].stores
    for storeid in reverse(T.created[r..]):
        db.DropColumnFamily(cf(storeid))            # §11
    T.created.truncate(r)
    T.dropped.truncate(mark.drops)                  # the undo restored their rows
    T.undo.truncate(m)
    T.seq_hwm = (m > 0) ? T.undo[m-1].seq : 0       # own-write visibility
    T.seq     = T.seq_hwm + 1                        # live seqs stay a prefix
    close every open cursor with id > mark.scan_id   # see below
    release locks acquired after the mark
    T.savepoints.truncate(depth)     # the mark stays; inner ones go
    ssi read set is NOT rolled back                  # conservative, correct
    transaction stays ACTIVE
```

Rolling back to depth `d` keeps the mark at `d`, so the same
mark can be rolled back to again, and drops the marks inside
it, which covered work that no longer exists. `sm1::forget`
drops marks without touching the work or the locks: a lock
taken inside a forgotten mark is still held, belonging now to
the enclosing mark, or to the transaction when there is none.

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
deletes, applied before `sm1::rollback` returns, have already
removed the version that carried it.

**Cursors opened after the mark are closed.** A cursor
holds a RocksDB snapshot and a `seq_hwm` frozen at
`sm1::scan` (§10.2), so the deletes a rollback issues are
invisible to it and its captured `seq_hwm` still admits
the undone versions. Left open it would yield rows the
transaction itself undid. Since a cursor opened after the
mark is one the rolled-back work created, closing it is
both correct and what SQL callers expect: PostgreSQL
closes cursors opened inside a subtransaction that
aborts. Cursors open *before* the mark are untouched —
they could never see the undone versions, because their
snapshot predates the writes.

This is why each mark records `scan_id`. Each
cursor gets an id from a per-session counter as it opens, so
"opened after the mark" is one comparison per open cursor.
Recording the set of open cursors instead would misjudge a
cursor opened after the mark at the address of one closed
before it. Closing releases each cursor's RocksDB snapshot,
which also lets `gc_watermark` advance past an abandoned
scan.

Releasing locks acquired after the mark is safe: the row was untouched
before the savepoint, so no one observed our intent on it.

### 6.6 Abort

`sm1::abort` writes no status record: absence of a record already
means aborted (§3.4), before and after a crash. Only aborting a
*prepared* transaction writes `ABORTED`, to supersede its
`PREPARED` record (§8.4). It
drops the column families of stores in `T.created` (no other
transaction can see them), closes the session's open scan cursors
and their RocksDB snapshots, releases locks and SSI state, and
returns. The versions stay in their store CFs until GC (§8.3).
Abort is O(1) in the write-set size.

---

## 7. SSI — serializable snapshot isolation

Cahill/Ports-style: run at snapshot isolation, track **rw-antidepen-
dencies**, and abort a transaction that sits at the pivot of a
dangerous structure.

### 7.1 What is tracked

- **SIREAD locks.** Every read by a SERIALIZABLE transaction
  records what it read: a point read records
  `(storeid, userkey)`, whether or not a row was found; a scan
  records `(storeid, range)` for the requested range, bounds
  inclusive or exclusive as passed to `sm1::scan`, when it opens,
  and narrows it to the **range actually traversed** when it
  closes (§10.2). The narrowed range includes its upper end, the
  last key the scan examined. A cursor moved on with
  `sm1::scan_seek` keeps the keys it skipped inside that range:
  the predicate runs from the start key to the furthest key
  reached, gaps included. It over-approximates what was read,
  which can only cost a serialization failure that was not
  strictly necessary, never miss one. That range is the
  predicate. Ranges are the phantom defence — a later insert into
  that range conflicts even though the row did not exist at read
  time. SIREAD locks survive the transaction's own commit; they
  are dropped only when no active snapshot could still conflict
  with them.
- **Conflict flags.** Per transaction, `in_conflict` and
  `out_conflict` — "some transaction has an rw-edge into me" / "I have
  an rw-edge out to someone".

**How they are indexed.** Every write calls
`ssi.on_write(T, storeid, k)` (§5), which must find every
SIREAD lock covering `k`. Point locks are a hash on
`(storeid, userkey)`, so that half is O(1). Ranges cannot
be, and a linear walk of active predicates under a global
lock would serialize the write path — the one place SSI
must not cost anything when no reader is watching.

Per store, ranges are held in a list in `memcmp` order
of their bounds (§3.1): `sm1::scan` inserts, `sm1::scanend`
narrows, `on_write` scans the store's list. One mutex
covers the whole tracker. A store no serializable
reader has touched has no list, so the common case — a
workload with no serializable transactions — pays one
lookup. An **interval tree** per store, answering the
stabbing query in `O(log n + m)` for `m` matches, is the
replacement once a benchmark shows many live ranges on
one store; it is not built yet.

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

A cycle in the serialization graph always contains a transaction
with **both** an incoming and an outgoing rw-edge (a *pivot*).
wisr applies the original rules of Cahill, Röhm and Fekete
(SIGMOD 2008), which need only the two flags:

```
edge R ──rw──▶ W, made by S (R or W, whichever is acting):
    if the other end has committed and is a pivot on that side
       (a committed W with out_conflict, or a committed R with
        in_conflict):
        doom S
    else:
        R.out_conflict = true;  W.in_conflict = true

at commit of T:
    if T is doomed, or T.in_conflict and T.out_conflict:
        abort T with SM_ERR_SERIALIZATION
```

A committed transaction cannot be told "no", so an edge that
would make it a pivot dooms the running transaction completing
the structure instead. Dooming never fails the call that made the
edge: the transaction runs on and fails at its commit, so
`sm1::read`, `sm1::scannxt`, and the write calls never return
`SM_ERR_SERIALIZATION`. The commit check runs after the
transaction claims its `commit_ts` and before its status record
is written; a failure hands the `commit_ts` back to the sequencer
unused. A prepared transaction (§8.4) counts as committed for
these rules.

The rules are conservative: a pivot that is not part of a real
cycle fails, and in a plain write skew both transactions can end
up pivots and both fail, where tracking edge targets and commit
order (PostgreSQL's refinement, SIREAD.md) would fail only one.
That is the accepted cost of never taking a read lock and never
deadlocking on reads; the refinement is a later step if the
false-abort rate matters.

### 7.4 Interaction with the other levels

Only SERIALIZABLE transactions take part in SSI, as in
PostgreSQL: they take SIREAD locks, their reads and writes create
edges, and they are the only candidates for serialization
failure. Reads and writes at READ COMMITTED and REPEATABLE READ
skip the tracker entirely, so serializability is guaranteed among
SERIALIZABLE transactions only; write skew between a SERIALIZABLE
transaction and a REPEATABLE READ one is not prevented. The write
path's `ssi.on_write` (§5, step 7) therefore runs only for
SERIALIZABLE writers.

Read-only serializable transactions can get the standard
safe-snapshot optimization (Ports & Grittner), described below;
it is not implemented yet, and a committed read-only transaction
keeps its SIREAD locks under §7.1's rule like any other.

A read-only transaction is never
a pivot — it writes nothing, so nothing has an rw-edge into it —
but it can be the *incoming* end of a dangerous structure,
`T_ro ──rw──▶ T_pivot ──rw──▶ T_out` (Fekete's read-only
anomaly), so its SIREAD locks matter until that is ruled out.
Its snapshot is **safe** once every read-write serializable
transaction concurrent with the snapshot has finished and none
of them committed with an out-conflict to a transaction that
committed before the snapshot. If there are no such concurrent
transactions at `sm1::begin`, the snapshot is safe at once. If
one of them does commit with such an out-conflict, the snapshot
is unsafe.

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

The tracker holds SIREAD locks and conflict flags for
transactions that have committed but might still conflict with a
live one — bounded by the oldest active snapshot. A committed
transaction's node and locks go once `gc_watermark` passes its
end: its `commit_ts`, or for a read-only one the `visible_ts`
just after its commit. No transaction concurrent with it is then
left. Under a long-running reader this grows; the escape valve is
the standard one: when the structure exceeds `ssi_max_entries`,
summarize the oldest committed transactions into a coarse
"something committed in this range" entry, which is safe
(over-approximating conflicts) and costs extra false aborts. It
is not implemented yet; `ssi_max_entries` is accepted but unused.

---

## 8. Durability, commit, and 2PC

### 8.1 Where durability comes from

RocksDB's WAL. Each `WriteBatch` is atomic and, when written with
`sync=true`, durable. The storage manager adds no log of its own,
because with eager writes there is exactly one thing that must be made
atomic and durable at commit: **the status record**.

### 8.2 Single-node commit

```
sm1::commit(T):
  1. close T's open scan cursors               (§10.2)
     if SERIALIZABLE: ssi.validate(T)         → may SM_ERR_SERIALIZATION
     if T.xid == 0: go to step 6               ← read-only: nothing to publish
  2. commit_ts = sequencer.claim()           ← global_ts++, joins the set
  3. batch = { CF:txn[T.xid] = {COMMITTED, T.snapshot_ts, commit_ts} }
  4. db.Write(batch, sync = durability_sync)   ← group-committed
  5. commit_cache.insert(T.xid → commit_ts);  active_table.remove(T.xid)
     if T wrote CF:store.0: last_ddl_ts = max(last_ddl_ts, commit_ts)  (§11)
     sequencer.done(commit_ts);  advancer moves visible_ts      (§4.3)
  6. lockmgr.release(T);  ssi.on_commit(T)
     queue (T.dropped, commit_ts) for CF removal     (§8.3)
```

Step 1 closes cursors first, as `API.md` requires of
`sm1::commit` and `sm1::abort`: this releases their RocksDB
snapshots, which pin `gc_watermark`, and narrows each cursor's
SIREAD predicate to the range it actually traversed (§10.2)
before validation runs.

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
- `sm1::commit` returns `SM_ERR_IO`, meaning *outcome unknown*.
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

Commits are group-committed by RocksDB's own write group:
concurrent `Write(..., sync=true)` calls queue behind one
leader, which appends them to the WAL with a single sync.
wisr adds no queue of its own, so `group_commit_us` is
accepted but unused; RocksDB has no knob for waiting on
followers. `sync` is
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
                                           queue (storeid, version_key)
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
db.DeleteRange(cf(storeid), version_key, userkey | FF×12)
```

which covers the tombstone and every older version of the key
at every level. Any later write to the key has a larger `wts`,
sorts before the tombstone, and survives. A range deletion does
not affect RocksDB snapshots taken before it, and RocksDB drops
the range tombstone itself at the bottommost level. The queue is
volatile: an entry lost to a crash is queued again by the next
compaction that meets the tombstone, and a repeated entry is
harmless.

One `DeleteRange` per deleted key is not free. Every range
deletion is a range tombstone that `RangeDelAggregator` must
consider on reads that overlap it, and RocksDB warns that
accumulating them in the memtable degrades read performance.
So a store CF's memtable is flushed once it holds
`store_max_range_deletions` (1,000) range tombstones:
RocksDB's `memtable_max_range_deletions`, set on every store
CF (§12.2), schedules the flush itself, and the range
tombstones reach SST files and stop being re-examined on
every read. The drainer does not flush after each batch. It
runs at least once a second, so a delete-heavy workload would
flush every store it touched about once a second, however few
range tombstones it had written — dozens of tiny level-0
files a minute, which keep level-0 compaction busy and can
stall writes — and the flush, which waits, would hold the
rest of the GC run behind it.

It does **not** merge adjacent keys into one range. A range
from one purged key's tombstone to a later purged key's
`userkey | FF×12` would also cover every version of any key
between them — live rows the filter never saw, since they may
sit in files outside the compaction — and every version of the
later key newer than its tombstone, because a key's versions
sort newest first. Deleting `k1` and `k3` would take a live
`k2` and a re-inserted `k3` with them. Nothing short of
locking the whole span could make the merge safe, so each key
gets its own range, which by construction covers only that
key's tombstone and older versions.

This path also forecloses one RocksDB option: `DeleteRange`
returns `Status::NotSupported()` when a `row_cache` is
configured, so store CFs must not have one (§12.2).

The stamping is why the header carries `commit_ts`: over time most
versions answer visibility without a status lookup at all, so the
commit cache and `CF:txn` are consulted mainly for the recent tail.

`gc_watermark = min(snapshot_ts of active sessions, snapshot_ts of
prepared transactions, oldest open scan)`. A cursor's snapshot is
never older than its transaction's begin snapshot, so the
transactions' begin snapshots suffice. Taking a snapshot and
registering it happen under the mutex the watermark is read
under, so no new snapshot can land below a watermark already in
use. It never moves
backwards: a new snapshot is `visible_ts`, which is at or above
every snapshot already counted. Each store CF has its own
compaction filter factory, which knows the CF's storeid for the
purge queue. Compactions can start inside `DB::Open`, before
the handles and the active table the filter reads exist, so
until the database is fully open the factory hands out a
filter that keeps everything.

A read-committed transaction counts at its begin `snapshot_ts`
for its whole life, between statements too, because §6.2 checks
its writes against that snapshot. If the watermark passed it,
the tombstone purge could remove a delete committed after
`sm1::begin`, and a later write of that key by the transaction
would no longer meet a commit after its snapshot: §5.4's check
would pass without the re-read. An idle open transaction
therefore holds back GC, as in any MVCC engine.

A `COMMITTED` record can be dropped once every surviving
version of its transaction carries the stamp (rule §3.4/0). The
periodic sweep guarantees that:

1. Record `W0 = gc_watermark`, then run a full compaction pass
   over every store CF, flushing memtables first.
2. Every committed version with `commit_ts <= W0` was already
   in RocksDB when the pass began, was visited by the filter
   with `gc_watermark >= W0`, and so was dropped or stamped.
3. Once the pass completes and every iterator created before
   it began has been released — an older iterator can still
   read pre-pass, unstamped copies — drop the `COMMITTED`
   records with `commit_ts <= W0`.

Step 3 is tracked with two read-epoch counters. Every reader
— a point read, a write's existence check, and a cursor for
its whole life — enters the current epoch's counter before
it opens an iterator and leaves once it is done resolving
what it read. After the pass the sweep flips the epoch; the
records go once the old epoch's counter drains. Readers
entering after the flip open their iterators on the pass's
output and see only stamped versions. The wait is not done
inline: the sweep is left pending and the GC worker finishes
it on a later run, so a long-open cursor delays the sweep
without stalling tombstone purges or store drops.

A pass during which the filter kept any version unstamped out
of doubt — an undecodable value, a failed status lookup —
deletes no records, since that version still needs its own.

Using the watermark at the pass's *start* matters: a version
with `W0 < commit_ts <= gc_watermark_now` may have been kept
unstamped, and dropping its record would make it resolve as
dead. `ABORTED` records are hints (§6.6) and can be dropped at
any time, since rule §3.4/4 gives the same answer without them.

Step 2 rests on two RocksDB behaviours, relied on and so
stated here:

- **The pass must reach the bottommost level.** A cold SST
  that no ordinary compaction has touched in weeks still
  holds versions needing a stamp. `CompactRangeOptions::`
  `bottommost_level_compaction` defaults to
  `kIfHaveCompactionFilter`, which covers this only
  because a compaction filter is installed. The sweep
  passes `kForce` explicitly instead of inheriting a
  default that a later change to the filter would
  silently invalidate.
- **The filter must see every version.** Compaction
  filters ignore snapshots — `IgnoreSnapshots()` is fixed
  at `true`, and the filter does not override it — so an
  open reader cannot cause an entry to be skipped and left
  unstamped. The pass also runs as an exclusive manual
  compaction, so no automatic compaction that began under
  an older watermark writes unstamped output alongside it.

The pass is expensive: it rewrites L0 through L6 across
every store CF, and its cost grows with the number of
stores as well as with data volume. It is therefore
triggered by the size of `CF:txn` — the space the sweep
would reclaim — and not by a timer, so a database that
commits rarely never pays for it. The GC worker checks
RocksDB's estimated key count of `CF:txn` on each run and
starts a sweep past an internal threshold of one million
records.

**Open question: the pass under a production load.** Nothing
here is decided or implemented. As built, the pass has three
costs beyond its I/O:

- **It stalls writes.** `exclusive_manual_compaction` means
  "no other compaction will run at the same time", across
  the whole database. While the pass works through the
  stores, a busy store gets no automatic compaction, so its
  level-0 files pile up to RocksDB's slowdown and then its
  stop trigger, and writes stall.
- **It trades badly.** One million `CF:txn` records are tens
  of megabytes; to reclaim them the pass rewrites every
  store, which may be terabytes.
- **It blocks the GC worker.** `CompactRange` runs inside
  `gc_run_once` and waits, so tombstone purges, store drops
  and the stuck-snapshot warnings wait behind the whole pass.
  Only the wait for pre-pass readers is off the worker.

Two fixes that come up do not work. Letting ordinary
compaction do the stamping is unsound: it never promises to
visit every file, so no record could ever be dropped, and
dropping one anyway is A.1's first refuted assertion.
Compacting one key range at a time can be made sound — fix
`W0` once for the cycle, and make each range's compaction
exclusive and bottommost — and it shortens each exclusive
window, but it is still a rewrite of every store, spread out.

The options:

- **(a) Trim the pass.** Trigger on `CF:txn`'s size relative
  to the stores' data rather than a fixed count, and run the
  pass off the GC worker. Contained, but the exclusive stall
  and the full rewrite remain.
- **(b) Compact only files that need it.** A RocksDB
  `TablePropertiesCollector` on every store CF records, per
  SST, whether it holds unstamped versions and of which
  xids, and marks such a file for compaction
  (`NeedCompact()`), so RocksDB's own background compaction
  rewrites it and the filter stamps it. The sweep flushes the
  memtables, then reads the live files' properties, and drops
  the records at or below `W0` once no live file holds an
  unstamped version of a transaction committed at or below
  `W0`; the read-epoch wait stays as it is. The argument that
  would make it sound: the filter never removes a stamp, so a
  compaction whose inputs hold no such version outputs none,
  and files created after the check are made from files it
  saw. A fully stamped cold file is never rewritten and
  nothing runs exclusively. Still to settle: what the property
  holds — a count, an xid range, or the xids themselves — and
  writing the argument down as carefully as step 2's.

(b) is the fix; (a) is what to do first if the stall bites
before (b) is built.

Dropping a store (`sm1::rmstore`) deletes its catalog row (§11).
The tombstone carries no storeid, so commit queues the storeids
in `T.dropped` with its `commit_ts`; once `gc_watermark` passes
that commit, a background task issues `DropColumnFamily` on each
store's CF. The queue is volatile: an entry lost to a crash is
caught by the startup sweep (§9.1 step 6).

Because collection rides on compaction, it inherits
compaction's costs, and the question of moving that work to
another machine is a real one. RocksDB supports it, but this
filter reads the active table, the watermark and `CF:txn`, and
writes the purge queue, so it is not a pure function of the
bytes it sees and cannot be shipped as it stands. `RCOMPACT.md`
assesses what it would take.

### 8.4 Two-phase commit

`sm1::prepare` makes the transaction durable but not visible.

```
sm1::prepare(T, gid):
  1. close T's open scan cursors               (§8.2)
     if SERIALIZABLE: ssi.validate(T)         ← validation happens HERE
     if T.xid == 0: T.xid = global_ts++        ← read-only: key the record
  2. rec = {PREPARED, xid, gid, snapshot_ts, prepared_at = now(),
            lockset = (key, mode) of every lock T holds,
            created = T.created, dropped = T.dropped}
  3. db.Write({CF:txn[T.xid] = rec}, sync = true)      ← always synced
  4. T.state = PREPARED;  locks and SSI state are RETAINED
```

Step 4 moves the transaction out of its session into an object
the database owns, registered under its `gid`: the transaction's
state, its snapshot's place in `gc_watermark`, its SSI node, and
its row locks, whose table entries are re-pointed at the new
owner. The session keeps only the `gid`. Any session can then
resolve the transaction, `sm1::closedb` leaves it untouched, and
recovery (§9.1 step 5) rebuilds exactly the same object from the
`PREPARED` record, so a transaction prepared before a crash and
one prepared in this process are handled alike. The record is a
single `CF:txn` value however large the write set; the write set
is stored as the lock keys the transaction holds, each with the
mode it is held in, so a shared lock taken by `sm1::lock` is
restored as shared. Database format version 4 added that mode
byte.

Step 1 closes cursors for the reasons given in §8.2. No data
operation is accepted after prepare, so an open cursor could
never be used again, but it would pin `gc_watermark` for as long
as the transaction stays in doubt. The coordinator's commit
(§8.2 from step 2) therefore finds none left to close.

A read-only transaction is not necessarily lock-free: `sm1::lock`
gives one locks without writing, and those go into the record
and are restored like any other. What makes it read-only is the
empty set of versions, not an empty lock set.

A read-only transaction still needs a durable `PREPARED` record,
because `API.md` promises that a prepared transaction survives
a crash and can be resolved by `gid` afterwards. Step 1 gives it
an xid (entering the active table, §3.4) so that the record has
a unique key; with `T.xid == 0`, every read-only prepare would
overwrite the same `CF:txn[0]`. Its `lockset` holds whatever
`sm1::lock` took and nothing more, so a transaction that only
read holds nothing after a restart.

After a successful prepare the transaction is in-doubt: its
versions sit in store CFs, its status record says `PREPARED`,
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
- `sm1::prepare` returns `SM_ERR_IO`, meaning *outcome unknown*,
  and the transaction is no longer active on its session.

After reopen, WAL replay decides: either the record survived and
the transaction is restored in doubt, listed by
`sm1::list_prepared` under its `gid`, or it has no record and is
dead (§3.4). A coordinator that got `SM_ERR_IO` checks the list
and resolves by `gid`. It never received a "yes" vote, so abort
is always a correct decision.

The coordinator later calls commit or abort. Commit is §8.2 starting
at step 2, overwriting the record (dropping the now-unneeded write
set); abort writes `ABORTED`, drops the CFs in `created`, and
releases. For a prepared transaction the `ABORTED` record is
synced: it must supersede the `PREPARED` record durably, or a
crash would restore the transaction in doubt again. An ordinary
abort writes no record at all, since absence already means
aborted (§3.4), and the `CF:txn` sweep (§8.3) deletes `ABORTED`
records whenever it runs.

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

`API.md` defines these four calls. What the design requires
of each:

```cpp
int prepare(session_t* session, std::string_view gid);
int list_prepared(session_t* session, std::vector<prepared_t>& out);
int commit_prepared(session_t* session, std::string_view gid);
int abort_prepared(session_t* session, std::string_view gid);
```

- `gid` is what makes a prepared transaction addressable once
  the session that prepared it is gone. It goes into the
  `PREPARED` status record (§8.4 step 2) and must be unique
  among the unresolved prepared transactions of a database.
- `sm1::list_prepared` is the only way to discover the in-doubt
  transactions that recovery restored (§9.3). Without it they
  hold their row locks and pin `gc_watermark` indefinitely
  with no way for an operator to name them. It requires no
  active transaction, because a coordinator calls it on a
  fresh session at startup.
- `sm1::commit_prepared` and `sm1::abort_prepared` resolve by
  `gid` on any session, which is the whole point: after a
  restart the resolving session is never the preparing one.
- `sm1::prepare` can still fail with `SM_ERR_SERIALIZATION`,
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
2. Read CF:meta → format version, persisted next_ts, next_cfid.
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
6. Orphan CF sweep: read the catalog (store.0) and drop every
   store.<storeid> CF that has neither a live catalog row naming
   it (a name's newest committed version, not a tombstone) nor
   an entry in a prepared record's `created`.
7. Start the GC / compaction filter and commit advancer.
8. Open for sessions.
```

Step 6 catches the column families that eager cleanup (§6.5,
§6.6) never reached: a crash between `CreateColumnFamily` and
commit, and a committed drop whose `DropColumnFamily` had not yet
run. Creating a CF is durable in the RocksDB MANIFEST before
`sm1::mkstore` writes its catalog row, so a committed catalog row
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
| ACTIVE that created a store | dead; its CF dropped by the orphan sweep (§9.1 step 6) |
| Wrote its COMMITTED record, WAL synced | committed, visible |
| Was in `Write(sync)` when power cut | atomically all-or-nothing per RocksDB; if the record is not in the WAL, the transaction is dead |
| PREPARED | restored in-doubt, locks re-acquired, awaits coordinator |
| Wrote ABORTED | aborted (and would be aborted anyway) |

### 9.3 In-doubt transactions

A restored prepared transaction blocks writers on its keys (it holds
locks) and pins GC. `sm1::list_prepared` exposes them by `gid` so
the coordinator can finish them. An operator escape hatch
(`sm1::abort_prepared`) is required, because an abandoned
prepared transaction otherwise blocks GC forever; this is the
same operational hazard as in any 2PC system and is exposed, not
hidden.

### 9.4 Torn or corrupt state

RocksDB checksums every block and WAL record. A checksum failure is
surfaced as `SM_ERR_CORRUPT` at open; the design does not
attempt repair beyond RocksDB's own WAL truncation of a torn tail
record — which is safe here precisely because a torn tail can
only lose commit records at the very end of the log, and losing a
commit record is indistinguishable from the transaction never
having committed.

---

## 10. Read path

### 10.1 Point read

```
sm1::read(session, storeid, key, &val):
    catalog.lookup(storeid)                              # SIREAD too, §11
    snap  = (level == READ_COMMITTED) ? visible_ts : T.snapshot_ts
    hi    = max(T.xid, snap)                             # own writes, §4.4
    if SERIALIZABLE:
        ssi.on_read(T, storeid, key)                     # SIREAD, §7.1
    it = db.NewIterator(cf(storeid))
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
  key, instead of at `~hi`. Versions with `wts > hi` sort before
  the `~hi` seek target, and they are exactly the concurrent
  writers (in-flight, or committed after `snap`) that owe an
  rw-edge; seeking past them would miss it. `concurrent(v, snap)`
  is true when `v.xid` is not the reader's own and resolves as
  in-flight or as committed with `commit_ts > snap`. The extra
  steps are only over those versions, which are rare.

Otherwise the seek starts at `max(T.wts_hi, snap)`, where
`wts_hi` is the largest `wts` the transaction has written.
Under repeatable read, a transaction's own versions have
`wts > snap`, so a seek at `~snap` would land after them
and a transaction could never read its own writes. Under read
committed, `snap` is refreshed per statement and can pass
`T.wts_hi`; a seek at `~T.wts_hi` would then land after versions
committed with `T.wts_hi < wts <= snap`, which read committed
must see. Taking the larger of the two covers both. A `~seq` of
`0x00000000` places the seek before every own version of the
key, newest first, and §4.4 picks the one allowed by `seq_hwm`.
Versions from other transactions with `snap < wts <= T.wts_hi`
are stepped past as in-flight or committed too late, which the
loop already handles.

**No batched read.** An index lookup followed by N base reads
costs N reads, and a call taking N keys was considered. RocksDB's
`MultiGet` does not apply: a read here is an iterator seek
across a key's version run, not a point `Get`. What a batched
call would save is per-key iterator construction and read-epoch
bookkeeping, plus seeking in sorted order, and it would need an
ownership model for N values, since the session has one read
buffer (§5.3). It is left unbuilt until a benchmark shows that
setup dominating a probe loop.

### 10.2 Scan

`sm1::scan` opens a RocksDB iterator on the store's CF, seeked
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

**Seek mode.** Store CFs carry a prefix extractor
(§12.2), and a RocksDB iterator on such a CF defaults to
*prefix seek mode*: `ReadOptions::total_order_seek` is
false, and the iterator is then allowed to return only
keys sharing the seek key's prefix — here, only the
versions of one userkey. A scan crosses userkeys, so it
must opt out. Left at the default it stops at the first
key boundary, and when `skey` itself is absent the bloom
filter answers "no such prefix" and the very first
`Valid()` is false: the scan returns nothing while
thousands of keys above `skey` exist. `sm1::scan` therefore
sets

```
ro.auto_prefix_mode = true       # or total_order_seek
```

`auto_prefix_mode` is preferred to a blanket
`total_order_seek` because `sm1::scan` always sets
`iterate_upper_bound`. RocksDB can then decide per seek:
prefix mode, and the bloom filter with it, when the bound
lies inside the seek key's prefix — the single-key range
— and total order otherwise. The flaw documented on that
option concerns userkeys shorter than a full-length
prefix, which cannot arise here: the extractor is
variable-length, so `SliceTransform::FullLengthEnabled`
is false and the affected path is never taken.

§10.1 keeps the default prefix mode. Its loop never
leaves one userkey, which is what prefix mode is for, and
the bloom filter earns its keep on a point read.

**Direction.** The iterator is only ever stepped with
`Next()`. Backward scans are not supported and will not be
(§1), so no `SeekForPrev` path exists and `auto_prefix_mode`
is never asked to serve one.

**Reposition.** `sm1::scan_seek` re-seeks the same iterator,
keeping its RocksDB snapshot, its MVCC view, and its upper
bound, so a probe loop — an index nested-loop join, an
IN-list, a skip scan — costs one cursor instead of one per
probe. It moves **forward only**, and refuses two targets:

- one before the last key examined, since the SIREAD range
  is narrowed to that key (§7.1) and moving back would
  record less than was read;
- one below `skey`. The iterator carries `iterate_upper_bound`
  but no lower bound — the start is a `Seek`, not a bound —
  so a seek under `skey` would return rows outside the range
  the cursor registered as its predicate. The cursor keeps
  the start bound it was positioned with, and compares
  against it.

A target past `ekey` is allowed: the upper bound leaves the
iterator invalid and the cursor reports its end. A cursor
already at its end stays there, rather than being repositioned
and revived, because §7.1 treats a scan that reached the end
as having read its whole range; reviving it and closing it
later would narrow that range to the last key examined, which
is less than it read.

Seeking backward was considered, for probes that do not arrive
in key order. It would need `last_key_examined` to become a
high-water mark, and a flag for a cursor that once hit the end,
or the predicate would silently shrink. Forward only costs the
layer a sort of its probe keys, which a batched index join does
anyway.

**Bound lifetime.** `iterate_upper_bound` is a
`rocksdb::Slice`, which borrows rather than copies, and
the bound outlives the `sm1::scan` call that set it.
`sm1::scan_t` owns the buffer holding `ekey | tail` for the
life of the cursor, and the iterator is destroyed before
that buffer is released.

- Without the RocksDB snapshot, other sessions' eager writes would
  appear mid-iteration and have to be filtered out anyway, and the
  iterator's own consistency would depend on memtable flush timing.
- A consequence: **a scan does not see the scanning transaction's own
  writes made after `sm1::scan`**. This is deliberate — it is
  Halloween protection, and it makes "update every row I scan"
  safe.
- The cursor also captures `T.seq_hwm` at `sm1::scan` and applies
  §4.4 with that value, not the live one. Its view of the
  transaction's own writes is then fixed like the rest. A
  cursor opened after a savepoint does not survive an
  `sm1::rollback` to it: §6.5 closes it, precisely because a
  frozen `seq_hwm` would otherwise go on admitting versions
  the rollback undid. A cursor opened before the mark is
  kept — its RocksDB snapshot predates every write the
  rollback undoes, so it never saw them — and the `seq`
  rewind of §6.5 stops a reused number from changing what it
  returns.

`sm1::scannxt` walks version keys, and for each distinct
`userkey` emits the first visible version and then skips the rest
of that key's versions:

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
reader's snapshot and `sm1::scan`: their writers ran `on_write`
before the range SIREAD existed, so the read side is the only
place the edge can be created. They also feed the post-restart
rule of §8.4.

For SERIALIZABLE, `sm1::scan` registers the whole requested
range, `skey` to `ekey` with their inclusive flags, as a SIREAD
predicate (§7.1) when it creates the cursor, **before** it takes
the RocksDB snapshot, and `sm1::scanend` shrinks it to the range
actually covered: from `skey` through `last_key_examined`,
inclusive (through `ekey`, with its flag, if the scan reached
the end). `last_key_examined` is the last userkey whose run the
cursor inspected, whether or not it yielded a row. Registering
only at `sm1::scanend` would be wrong: a write into the range
while the cursor is open is not in the cursor's RocksDB
snapshot, so the reader never steps past it, and with no SIREAD
yet the writer finds nothing to conflict with — the rw-edge
would be missed entirely. The order within
`sm1::scan` matters for the same reason, as in §10.1: a writer
puts its version before calling `on_write`, so a version put
after the snapshot is found by `on_write` only if the SIREAD
already exists. Shrinking at the end is safe, because the
reader never read past `last_key_examined`; edges already
created for writes beyond it are merely conservative. If the
cursor is abandoned early, the range kept is the shorter one
actually read.

### 10.3 API shape

`API.md` is the interface contract. Its read-path signatures
are repeated here because three of them encode decisions from
this document rather than mere spelling:

```cpp
/* 0 = row, 1 = no such key, -1 = error. data views session-owned
 * memory valid until the next call on the session. */
int  read(session_t* session, uint64_t storeid,
          std::string_view key, std::string_view& data);

/* A NULL bound means none; the empty key is a bound. */
int  scan(session_t* session, uint64_t storeid,
          const char* skey, size_t sklen, int skey_inclusive,
          const char* ekey, size_t eklen, int ekey_inclusive,
          scan_t*& out);

/* 0 = row returned, 1 = end of range, -1 = error. Views valid
 * until the next sm1::scannxt on the same cursor. */
int  scannxt(scan_t* scan, std::string_view& key,
             std::string_view& data);
void scanend(scan_t* scan);
```

- **Both range bounds are parameters.** Without an end key a
  range predicate cannot be recorded for SSI (§7.1), and every
  caller would otherwise scan and discard.
- **"No row" is a tri-state return, not an out-param.**
  `sm1::read` returns 1 for a missing key and `sm1::scannxt`
  returns 1 at end of range; this replaces the earlier
  `int* found`. Neither is an error, and neither disturbs the
  error state, so a caller must distinguish 1 from -1 before
  consulting `sm1::errnum()`.
- **`sm1::scanend` returns void.** Releasing a cursor cannot
  usefully fail. It is still where the predicate registered
  at `sm1::scan` is shrunk to the range the cursor actually
  traversed (§10.2), so the range kept is the part traversed,
  not the part requested, which makes closing early both
  correct and tighter than draining.

---

## 11. Catalog and DDL

A store is named by a `uint64_t` storeid, which `sm1::mkstore`
allocates and hands back; there are no store names. The
storeid names the store's column family, `CF:store.<storeid>`, so
resolving one costs no indirection. The catalog is itself
`storeid = 0`, an ordinary MVCC store in `CF:store.0` holding a
row per store, a row per sequence, and whatever the level above
keeps under `"U"`:

```
key = "R" | be64(storeid)    →  { }                         (store)
key = "S" | name           →  { seqid, start, increment }  (sequence)
key = "U" | anything       →  anything                    (the level above)
```

The `"U"` rows are the level above's catalog, which sm1 stores
and versions like any row but never reads: `sm1::lsstore` scans
`["R", "S")`, and the startup sweep keeps a store only for an
`"R"` row. Callers reach store 0 under `"U"` and nowhere else
(`sm1::upper_prefix`), so they cannot touch a store's or a
sequence's row. A commit that writes a `"U"` row still counts as
a write to `CF:store.0`, so it raises `last_ddl_ts`, which costs
nothing until the store cache below exists.

A store's row carries nothing: that the row exists at a
snapshot is what makes the store exist there. The design's
`created_ts` field is left out, since nothing reads it.

Big-endian storeids make the catalog's key order numeric, so
`sm1::lsstore` is one scan of the `"R"` prefix and returns
storeids in ascending order. Storeids are handed out in creation
order, so the lowest one listed is the first store the database
ever had: that is how a layer finds its own catalog store again
after a restart, having nothing else to go on.

A storeid is never reused, so it never names two stores. That
is what makes a drop safe: after `sm1::rmstore` commits, the
store's CF stays until the watermark passes the drop (§8.3),
and an older snapshot goes on reading it through the same storeid
while newer ones find no catalog row. A storeid the caller could
choose would have to be refused until GC had dropped the old
CF, or else name two stores at once.

There is no next-storeid row in the catalog. Storeids come from
the lease in `CF:meta` (§4.2); a catalog row updated by every
`sm1::mkstore` would make concurrent creates of *different*
stores contend on one row lock and fail with `SM_ERR_CONFLICT`.

Because it is an ordinary store, DDL is transactional for free:
`sm1::mkstore` inserts a catalog row, `sm1::rmstore` deletes it
(recording the storeid in `T.dropped`), and both roll back with
the transaction. Both follow the §5 write path, reported in DDL
terms: `sm1::mkstore` of a live storeid fails with
`SM_ERR_EXISTS` where an insert would report `SM_ERR_DUPKEY`,
and `sm1::rmstore` of a missing storeid fails with
`SM_ERR_NOTFOUND`. A dropped storeid's newest catalog version
is a tombstone, and it stays one: `sm1::mkstore` always
allocates a fresh storeid, so a dropped one is never created
again (above).

`sm1::lsstore` is a §10.2 scan of the catalog over `["R", "S")`,
so under SERIALIZABLE it registers the whole range as read: a
concurrent `sm1::mkstore` or `sm1::rmstore` of any storeid is an
rw-conflict with it.

A session may cache which storeids are live (not yet implemented:
every lookup reads `CF:store.0`), but the answer depends on the
snapshot. After a committed `sm1::rmstore`, a transaction with an
older snapshot must still find the store, since it may still
legitimately read it. A cache invalidated at DDL commit would
hide it, so the cache is checked by timestamp instead:

- The process keeps `last_ddl_ts`, the largest `commit_ts` of
  any committed transaction that wrote the catalog. Commit raises
  it before publishing that `commit_ts` (§8.2 step 5), so a
  snapshot that includes a DDL commit always sees it.
- Each cache entry records the snapshot it was read at. A hit is
  used only if `last_ddl_ts` is at or below both that snapshot
  and the lookup's; then no DDL committed between the two, and
  the answer is the same at both. Otherwise the lookup reads
  `CF:store.0` and replaces the entry.
- `sm1::mkstore` and `sm1::rmstore` evict their storeid's entry,
  and a lookup that resolves to the transaction's own uncommitted
  catalog version does not populate the cache.

The comparison is against both snapshots because a session's
snapshots do not only move forward: under read committed, a
lookup at the begin snapshot `T.snapshot_ts` can follow one at a
newer statement snapshot (§6.2).

Under SERIALIZABLE a lookup is also a read for SSI. That a
storeid names a live store is something the transaction observed,
and a concurrent `sm1::rmstore` of that storeid writes the
catalog key it depends on. Without an rw-edge for it, DDL admits
write skew:

```
T1: sm1::read(S, x)             T2: sm1::read(R, k)    # looks up R
T1: sm1::rmstore(R)             T2: sm1::update(S, x)
```

`T1 ──rw──▶ T2` comes from `x`; `T2 ──rw──▶ T1` exists only if
T2's lookup counts as a read of `"R" | be64(R)`. So under
SERIALIZABLE:

- A lookup is a §10.1 point read of the catalog key: the SIREAD
  lock on `"R" | be64(storeid)` in `store.0` is taken first,
  whether or not the storeid is found, and versions of
  concurrent writers it steps past run `on_skip`.
- A cache hit does not stand in for that read. An in-flight
  `sm1::rmstore` has not raised `last_ddl_ts`, so a hit is valid
  by the rule above, yet it steps past nothing, and the dropper's
  `on_write` ran before any SIREAD existed: the edge is missed.
  Each cache entry therefore also records the transaction that
  read it from `store.0`, and a serializable lookup uses a hit
  only if that is the current transaction. Its SIREAD already
  catches every later writer of the storeid.

### 11.1 Sequences

A sequence is a catalog row of its own, keyed `"S" | name`, so
`sm1::mkseq` and `sm1::rmseq` are transactional exactly as
`sm1::mkstore` is, down to the row lock that makes two concurrent
creates of one name conflict. The row holds
`{ seqid, start, increment }`, and the `seqid` comes from the
same object-id allocator as a storeid (§4.2), so a name created
again after a drop names a different sequence.

What the row does **not** hold is the sequence's position. The
values are not transactional — `sm1::nextseq` takes no locks,
never blocks, and an abort gives nothing back — so keeping them
in an MVCC row would be wrong twice over: the row's writer
would lock the sequence for the rest of its transaction, and a
rollback would hand the same values out again. The position
lives in `CF:meta` instead, under a lease keyed by `seqid`
(§4.2), where nothing is undone. The `seqid` keys it rather
than the name, so a sequence created again under an old name
starts at its own `start` rather than resuming the old one's
lease.

`sm1::nextseq` reads the definition at the transaction's view
when there is one, so a sequence a transaction has just created
is usable before it commits, and at `visible_ts` when there is
none. The last value is the largest one at or below
`UINT64_MAX`; a further call fails with `SM_ERR_INVALID` rather
than wrapping onto values already handed out.

A committed `sm1::rmseq` deletes the sequence's lease, best
effort: nothing reads it once the catalog row is gone, and a
lease left behind by a crash, an abort after `sm1::nextseq`, or a
prepared transaction's drop is garbage rather than a hazard,
since the name's next incarnation has a new `seqid`.

### 11.2 Column families

The column family is the one part of DDL that is not
transactional: `CreateColumnFamily` and `DropColumnFamily` take
effect immediately. `sm1::mkstore` therefore creates the CF
**eagerly**, before inserting the catalog row, so the creating
transaction can write into the store at once. Nobody else can
reach the CF until the catalog row commits. If the transaction
aborts or rolls back past the `sm1::mkstore`, it drops the CF
itself (§6.5, §6.6); if it crashes, the startup sweep drops it
(§9.1).

`sm1::rmstore` does **not** drop the CF at commit — a concurrent
snapshot may still legitimately read the store. Physical
removal happens when `gc_watermark` passes the drop's
`commit_ts` (§8.3).

`sm1::mkdb` allocates a `dbid`, creates the directory
`{homedir}/base/{dbid}`, `CF:txn`, `CF:meta` with its
initial contents, and the catalog's `CF:store.0`, and commits
the name last. `sm1::rmdb` drops the name first, then runs
`rocksdb::DestroyDB`, after verifying that no session holds
the database open; sessions in other processes are ruled out
by the caller's one-process rule (§3.5). §3.5 gives the
ordering and the sweep that collects what a crash leaves
behind. Neither call builds a path from `dbname`, so neither
can be steered by one.

### 11.3 Temporary stores

**Decision: temporary tables and their indexes are built on
temporary stores, and a store is temporary through its column
family.** A store is made temporary when it is created, and the
property belongs to the whole store — its column family — not
to any key in it: everything written into a temporary store is
temporary. sm1 does not know what a store holds. Above it, sm3
puts a table's heap, its indexes and its TOAST data in one
store (`DESIGN.md` §5), so a temporary table and all its
indexes are one temporary store, and no per-row or per-key
marking exists at any level.

**Status: not built, and low priority.** What follows is the
proposed mechanism, checked against RocksDB 8.9.1.

RocksDB has no temporary column family: no option marks one
so. It has the parts to build one from, and sm1's write path
suits them:

| Wanted | RocksDB | Applies to |
| --- | --- | --- |
| not logged, lost in a crash | `WriteOptions::disableWAL` | one write |
| cheap removal | `DropColumnFamily` | a column family |
| files kept apart | `ColumnFamilyOptions::cf_paths` | a column family |
| private to a session | nothing | — |

- **Unlogged rows, logged commits.** Each row version is its
  own `Put` into the store's column family (§5), apart from
  the commit record, which goes to `CF:txn` in a batch of its
  own (§8.2). A temporary store's `Put`s can carry `disableWAL`
  while commit status stays logged, so transaction state is
  crash-safe though the rows are not. The flag is per write,
  so the store handle, not each caller, must decide it: one
  path that forgets it logs a temporary store's data.
- **Swept after a crash for free.** A temporary store belongs
  to its session and never commits a catalog row, so the
  startup sweep (§9.1, step 6) already drops its column family
  as an orphan. At session end, sm1 drops it itself.
- **Private by sm1.** RocksDB knows no sessions; every session
  shares the one DB handle. sm1 must refuse any session but the
  owner that names a temporary storeid.
- **Lost on crash is the point.** Unlogged data survives only
  once a memtable flush writes it out, which is PostgreSQL's
  meaning of a temporary table, and holds because a temporary
  store never outlives the process.

Open: how `sm1::mkstore` is asked for a temporary store, and
whether its files get their own `cf_paths` directory, as
PostgreSQL's temporary tablespaces do.

---

## 12. Errors, configuration, and structure

### 12.1 Error codes

`API.md` defines `sm1::errnum_t` with explicit, stable values.
This section records only which codes the engine raises and
where they come from:

```
SM_ERR_CONFLICT        write-write conflict (§5.2)
SM_ERR_UPDATED         read committed write past a newer commit (§5.4);
                       not an abort
SM_ERR_SERIALIZATION   SSI validation failure (§7.3)
SM_ERR_DEADLOCK        victim of deadlock detection (§5.1)
SM_ERR_NOTFOUND        store or key does not exist
SM_ERR_DUPKEY          insert onto a live key (§5)
SM_ERR_NOSAVEPOINT     no savepoint at the given depth (§6.5)
SM_ERR_NOTRANS         data op outside a transaction (§6.1)
SM_ERR_INTRANS         op forbidden inside a transaction (§6.1)
SM_ERR_PREPARED        illegal op on a prepared transaction (§8.4)
SM_ERR_EXISTS          home dir, database, or store exists (§11)
SM_ERR_INUSE           database still open in this process
SM_ERR_VERSION         home directory format unsupported (§3.5)
SM_ERR_CORRUPT         RocksDB integrity failure (§9.4)
SM_ERR_IO / SM_ERR_NOMEM / SM_ERR_PERMISSION / SM_ERR_DISKFULL
```

`CONFLICT`, `SERIALIZATION`, and `DEADLOCK` are the three
*retryable* errors. On all three the engine has **already
aborted** the transaction (§6.6): the caller must not call
`sm1::abort`, and retries by beginning a fresh transaction.
Under `sm1::xctlevel_t::serializable` they are a normal outcome,
not an exceptional one.

The code and message are read with `sm1::errnum()` and
`sm1::errmsg()`, each reporting the last error on the calling
thread. They are functions, not the thread-local globals of
the original sketch; `API.md` divergence 1 records why.
Two consequences for the implementation:

- A successful call leaves both untouched, so every path that
  returns an error indication must set them before returning.
  Nothing may rely on a stale value, and nothing may clear
  them on success.
- `sm1::errmsg()` is never NULL and its result stays valid until
  the next failing `sm1::` call on that thread, so the message
  wants thread-local storage owned by the engine. Formatting
  it lazily is worthwhile: under SSI a retry loop can raise
  `SM_ERR_SERIALIZATION` constantly and never read the text.

### 12.2 Configuration knobs

`durability_sync` (default on), `group_commit_us`, `deadlock_ms`,
`ssi_max_entries`, `commit_cache_entries`, `ts_lease`, plus
pass-through RocksDB options (block cache size, write buffer
size, compaction style). Bloom filters
are worth enabling on store CFs, and `CF:txn` benefits from a
large block cache since it is small and hot. For store CFs the
prefix extractor strips the 12-byte tail, so the bloom key is the
userkey; the comparator keeps each userkey's versions contiguous,
which is what a prefix extractor requires.

Configuring that extractor obliges every iterator on a
store CF to state its seek mode, because the RocksDB
default is prefix seek and a scan that inherits it
returns at most one userkey (§10.2). Store CFs also
get no `row_cache`: the tombstone purge uses
`DeleteRange`, which returns `Status::NotSupported()`
when one is configured (§8.3). And each sets
`memtable_max_range_deletions` to
`store_max_range_deletions`, so its memtable is flushed once
the purge's range tombstones pile up in it (§8.3); the option
needs RocksDB 8.9 or later.

Store CFs share one block cache and one `WriteBufferManager`,
so total memtable memory is a single budget rather than
`write_buffer_size × stores`. `max_total_wal_size` bounds how
long a rarely-written CF can hold old WAL files alive.

**Logging.** wisr writes no log of its own; a host installs a
process-wide hook with `sm1::set_log` (`API.md`). The engine
reports through it only what no caller can learn from a
return value: failures on the GC thread, entering the failed
state (§8.2), versions the compaction filter kept out of
doubt, a CF:txn sweep abandoned or completed (§8.3), a
snapshot pinning `gc_watermark` for over a minute, and what
recovery and `sm1::attach` restore or clean up (§3.5, §9.1).
The stuck-snapshot check runs on each GC pass: each
registered snapshot records when it was taken, and one older
than the threshold is reported once. That is the operator's
signal for the classic MVCC trap, an idle transaction or a
forgotten cursor holding back reclamation.

### 12.3 Source layout

```
include/sm1.h           public C++ API, namespace sm1
include/sm1_codec.h     fixed-width big-endian integers
include/sm1_defer.h     defer blocks and scope guards
include/sm1_format.h    std::format into a fixed buffer, allocating nothing
include/doctest.h       the vendored doctest test framework, used by every level
src/env.cc              init/attach/detach, root db, db sweep
src/db.cc               mkdb/rmdb, open registry, CF setup, storeid→CF map
src/session.cc          open/close, per-session state and buffers
src/xct.cc              transaction manager, timestamps, sequencer, 2PC
src/version.cc          key/value codec, version comparator, visibility
src/access.cc           read / scan / insert / update / delete
src/lockmgr.cc          row locks, waits-for graph, deadlock detection
src/ssi.cc              SIREAD locks, rw-edges, validation
src/catalog.cc          store catalog: DDL, sm1::lsstore, CF create/drop
src/seq.cc              named sequences: sm1::mkseq, sm1::rmseq, sm1::nextseq
src/gc.cc               compaction filter, watermarks, drop-store purge
src/recover.cc          startup, prepared-transaction restore, orphan CF sweep
src/log.cc              the sm1::set_log hook the engine reports through
src/format.cc           sm1::format_into
test/                   the engine's tests: API, limits, locks, GC,
                        2PC, crash, sequences, logging
mvcc/                   isolation: MVCC.md and one file per scenario
example/                small programs against the public API
```

General C and C++ utilities — anything not specific to the
engine — live in `include/` as `sm1_*.h`, in `namespace sm1`,
where the levels above can use them too. `src/` holds only the
engine.

### 12.4 Build order

1. Codec, catalog, single-threaded read/write with no concurrency —
   `mkdb/open/mkstore/insert/read/scan` under one transaction at a time.
2. Timestamps, commit sequencer, status table, snapshot visibility;
   RC and RR isolation. Crash tests begin here.
3. Row locks (shared and exclusive), write-write conflict,
   deadlock detection.
4. Savepoints.
5. GC / compaction filter (until this exists, the database only grows —
   fine for correctness tests, not for a benchmark).
6. SSI and SERIALIZABLE.
7. 2PC and prepared-transaction recovery.

### 12.5 Testing

Isolation has a document of its own: `mvcc/MVCC.md` states
the rules of §5 to §7 as a table of scenarios -- every
write-write pair, every read-write shape -- with the outcome
each one has at REPEATABLE READ, at a mixed pair of levels,
and at SERIALIZABLE. One file per scenario in `mvcc/` runs
it in all of those pairings, and `make test` runs them. The
bullets below cover the rest of the engine.

- **Key order.** Codec tests with userkeys that are prefixes of
  one another (`""`, `"a"`, `"a\x00"`, `"a\xFF"`, `"ab"`), each
  with several versions: a scan returns them in `memcmp` order,
  and a point read of each finds its own newest visible version.
- **Own writes.** At each isolation level, a transaction reads
  back a key it inserted, updated twice, and deleted; after
  `sm1::rollback` to a savepoint it reads the pre-savepoint
  value. A cursor opened *before* the savepoint survives the
  rollback and returns the same rows whether or not the
  transaction writes again afterwards; a cursor opened *after* it
  is closed by the rollback, and a row written after the mark is
  never yielded by any surviving cursor (§6.5, §10.2).
  It can update a key it inserted and re-insert a key it
  deleted (§5), and `sm1::rollback` to a depth the stack does
  not hold fails with `SM_ERR_NOSAVEPOINT`. A key locked
  before a savepoint and rewritten after it stays locked
  through `sm1::rollback` (§5.1).
- **Nested savepoints.** Three marks deep: a rollback to the
  middle one undoes the writes after it, keeps those before
  it, and drops the mark inside it, while the middle mark
  survives for a second rollback. `sm1::forget` keeps the
  writes and releases no locks, and the depth it frees is
  handed out again by the next `sm1::savepoint`. Pushing past
  `sm1::max_savepoints` fails with `SM_ERR_INVALID` (§6.5).
- **The old row.** An update returns the committed value it
  replaced, then its own previous value on a second update,
  and a delete returns the last value written. An empty old
  value comes back empty, and a failed write reports none. A
  read on the same session overwrites
  the buffer the old row was handed out in (§5.3).
- **Cursor reposition.** A cursor seeks forward over rows and
  resumes there, with the bounds and view it opened with;
  seeking backwards, or below the start key, fails. Under
  SERIALIZABLE, a write skew whose only second edge runs
  through the keys a seek skipped is still caught, and a
  control that never seeks — so the predicate ends at the
  first key — commits instead (§7.1, §10.2).
- **Shared locks.** Two transactions hold one key shared at
  once; an exclusive request waits for both. A shared holder
  writing the key upgrades once the others go, and two
  upgraders on one key deadlock with the younger one the
  victim. At REPEATABLE READ and above `sm1::lock` runs §5.2's
  check under the lock, so it fails with `SM_ERR_CONFLICT` on a
  key committed after the caller's snapshot; at read committed
  it does not fail, and the caller reads the row with
  `sm1::read_latest` (§5.4). A savepoint rollback releases an
  explicit lock first taken after the mark but never downgrades an
  upgrade. The whole index-build protocol runs end to end: a
  builder taking the schema row exclusive blocks a writer,
  sees the row an earlier writer committed, and the blocked
  writer then fails, retries, and reads the new schema
  (§5.1, `sm3/SM3_DESIGN.md` §3).
- **Deadlock.** Build a two-transaction cycle in both orders:
  the older waiter finds the cycle and the younger is the
  victim, and the younger finds it and is itself the victim.
  Either way the youngest fails with `SM_ERR_DEADLOCK` and the
  other proceeds. Separately, doom a running transaction and
  assert its next `sm1::` call fails with that reason and
  changes nothing (§5.1).
- **Scan seek mode.** With the prefix extractor configured,
  a scan from a `skey` that is absent from the database
  returns every key above it in range, and a scan whose range
  spans many userkeys returns all of them rather than stopping
  at the first — the regression test for inheriting RocksDB's
  default prefix seek mode (§10.2). Run it with enough keys to
  push data out of the memtable, since the failure needs the
  bloom filter to answer from an SST.
- **Read committed after a write.** A read-committed
  transaction that has already written reads a key committed by
  another transaction after its first write, and sees it
  (§10.1). Updating that key then fails with `SM_ERR_UPDATED`,
  and goes ahead after `sm1::read_latest` (§5.4). A
  read-committed transaction left idle while another deletes a
  key keeps that tombstone through a forced compaction and purge
  (§8.3).
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
  (§10.2), a scan left open at `sm1::commit` (§8.2), write
  skew through `sm1::rmstore` of a store the other transaction
  reads, with the storeid already in both sessions' caches (§11),
  and Fekete's read-only anomaly, which must fail while the
  read-only transaction's snapshot is unsafe (§7.4). A
  transaction that reads, outlives every concurrent writer so
  that its snapshot is safe, and then writes commits, including
  on an otherwise idle database (§7.4). With a prepared or
  committed pivot, the partner is the one that fails (§7.3).
- **DDL.** Concurrent `sm1::mkstore` calls both commit, each
  writing a catalog row of its own; concurrent `sm1::rmstore` of
  the same store blocks, and the second fails with
  `SM_ERR_CONFLICT` (§11). Every `sm1::mkstore` hands out a
  higher storeid than the last, and a dropped store stays
  readable at an older snapshot; `sm1::rollback` past an
  `sm1::rmstore` keeps the store's CF, and past an `sm1::mkstore`
  drops it. Under SERIALIZABLE, `sm1::lsstore` followed by
  concurrent creates is a conflict (§11).
- **Sequences.** `sm1::mkseq` and `sm1::rmseq` follow commit,
  abort, and rollback like any other DDL, while the values
  `sm1::nextseq` hands out do not: an abort gives none back. A
  sequence is usable by its own transaction before it commits,
  its values step by its increment, it resumes above its lease
  after a restart or a kill, concurrent sessions never get the
  same value, a name created again after a drop starts over,
  and a sequence at `UINT64_MAX` hands out its last value and
  then fails (§11).
- **Environment lifetime.** `sm1::detach` with `force` 0 and a
  session open fails with `SM_ERR_INUSE` and leaves the env
  usable; with `force` non-zero it succeeds, and a second
  `sm1::attach` on the same home directory then works. A
  transaction left active in a
  forced-closed session is aborted — its versions resolve
  dead (§3.4) — while one left *prepared* survives: after
  reattaching, `sm1::list_prepared` still reports it with its
  original `gid` and `prepared_at` (§8.5).
- **Crash.** A fault-injection harness that kills the process at every
  RocksDB write boundary during a workload, restarts, and checks that
  the surviving state is exactly one of the legal prefixes of the
  commit order.
- **2PC.** Kill between prepare and commit; verify the transaction is
  restored in-doubt, and that both resolutions produce the right state.
  Replay the §8.4 write-skew history across a restart and assert
  that the post-restart transaction fails with
  `SM_ERR_SERIALIZATION`; assert `sm1::list_prepared` reports the
  original `prepared_at`. Prepare two read-only transactions
  under different gids, restart, and assert both are listed and
  resolvable (§8.4). Prepare a transaction that dropped a
  store, restart, commit it by gid, and assert the store's
  CF is removed once `gc_watermark` passes the commit (§3.2).
- **GC.** Assert that a long-running reader's snapshot is never
  violated by the collector, and that space is actually reclaimed once
  it finishes. Force a compaction while a transaction is still
  running and assert its versions stay invisible and unstamped,
  and are dropped after it aborts (§8.3). Delete many keys and
  assert their tombstones are purged without resurrecting older
  versions, while live keys between them and a key re-inserted
  after its delete survive the purge (§8.3).
  Stall one commit's WAL sync under concurrent load and
  assert allocation continues and `visible_ts` catches up (§4.3).
- **Status resolution.** Fail a commit's status write and assert
  the database fails stop: `sm1::commit` returns `SM_ERR_IO`, the
  versions stay in-flight and are not compacted away, and after
  reopen the transaction is committed or dead according to the
  WAL (§8.2). Fail a prepare's record write and assert
  `sm1::prepare` returns `SM_ERR_IO`, the versions stay
  in-flight, and after reopen the transaction is either listed by
  `sm1::list_prepared` or dead, according to the WAL (§8.4).
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
- **A column family per store.** Buys instant physical drop,
  per-store tuning, and per-store compaction stats. Costs: every
  CF is opened at startup, so open time grows with the number of
  stores; each CF carries its own memtable and LSM metadata, so
  very many small stores need the shared memory budget (§12.2);
  and CF create/drop is outside MVCC, which is why `sm1::mkstore`
  needs eager cleanup plus a recovery sweep (§11). The target is
  **hundreds to low thousands of stores per database**, which is
  where this trade pays. Past that, the answer is not to tune it
  but to replace it: fold the storeid into the key as an 8-byte
  prefix in one shared CF, which makes create and drop pure MVCC
  operations and removes the recovery sweep, at the cost of
  losing the O(1) drop (it becomes a `DeleteRange` over the
  store) and all per-store tuning.
- **A transaction's lock, undo, and read sets live in RAM.**
  Row values stream to disk, but `T.locks`, `T.undo`, and the
  SSI read set are linear in the keys touched (§5.1), so a
  single transaction writing tens of millions of keys costs
  gigabytes. Bulk loads want batching into many transactions.
- **Every write takes a timestamp.** A version is keyed by a `wts`
  taken after its row lock (§4.1), so the counter is used once per
  row written and not once per transaction, and its lease (§4.2,
  `ts_lease`) runs out sooner; each renewal is a synchronous
  `Put`. `global_ts` is an atomic so the common case takes no
  mutex, but no throughput benchmark has been run, so neither
  cost is measured. A larger `ts_lease` is the first lever.
- **The format changed.** Keying versions by `wts` and moving the
  `xid` into the value bumped `WISR_DB_VERSION` to 5. A database
  written before it is refused, not migrated.
- **The READ COMMITTED protocol is unproven under a race
  detector.** `e06` shows no lost increment, but a
  `-fsanitize=thread` build reports races, mostly through
  RocksDB, which is not instrumented; only some were read, and
  the existing `e03` reports one too. Treat it as inconclusive.
- **One process.** Simplifies everything (no shared memory, no lock
  table in shm), and is what RocksDB wants. Multi-process or remote
  access means a server front-end (§1), not a change to this design.

---

## Appendix A. Disposition of the architectural review

A review of this document (Andy Pavlo, CMU) raised six
risks and four details to pin down. Its valid items are
folded into the sections above. This appendix refutes the
three assertions that did not survive checking against the
RocksDB source, so that they are not raised again.

### A.1 Assertions refuted

1. **"Truncating `CF:txn` silently loses committed data in
   cold SST files under rule §3.4/4."** No: §8.3 never
   sweeps on a timer — it drops a `COMMITTED` record only
   after a full pass has stamped every surviving version
   and every pre-pass iterator has been released, using
   the watermark from the pass's start, so an unstamped
   version's record is never dropped.
2. **"The filter should drop bottommost tombstones using
   `CompactionFilter::Context::is_bottommost_level`."** No
   such field exists — the struct carries
   `is_full_compaction`, `is_manual_compaction`,
   `input_start_level`, `column_family_id`, `reason`, and
   `input_table_properties` — and the flag would not help
   anyway, because compaction inputs are chosen by full
   version key, so older versions of the same userkey can
   sit in an SST the filter never sees (§8.3).
3. **"The read-committed write-conflict divergence must be
   highlighted in user documentation."** It already is:
   §6.2 states it with its justification, and `API.md`
   states it as the second of its surprising isolation
   behaviours.

A fourth recommendation is recorded as unsafe rather than
merely wrong: presuming that a version with
`xid < min_uncompacted_xid` and no entry in an aborted set
is committed would resurrect the versions of crashed
transactions, which by construction leave no status record
anywhere. That absence *is* the abort (§3.4), and it is
what makes recovery free of an undo pass.
