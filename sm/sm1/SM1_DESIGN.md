
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
direction (`../PART4.md` §5).

The engine cost was part of why. A versioned key's versions
are stored newest first (`key|~wts|~seq`, §3.1), so a backward
scan would meet the oldest version first and have to walk to
the start of each key's run before picking the visible one; it
would need `SeekForPrev` to work with §10.2's prefix-seek
setup; and for SSI the traversed range would run from the last
key examined up to `ekey`, inverting §10.2's bookkeeping.

A chained store's rows no longer pay the first two
(`../VCHAIN.md`): a row's newest version is one CURR key, its
older ones a chain in OLD, so a backward scan meets one key a
row and walks the chain as a forward scan does, and a scan of
CURR keys runs in total order, where `SeekForPrev` works. The
cost stays on versioned keys — secondary index entries, TOAST,
the catalog — and the SSI inversion stays on every scan. A
secondary index built as a B+tree (`../BTREE.md`) holds
unversioned entries and could step back by a left link.

**Phases.** Phase 1 is a solid MVCC storage manager: the API in
`sm1.h` and the design in this document, nothing more. Phase 2
adds the API a secondary-index layer built on top of it needs
(`../sm3/SM3_DESIGN.md` §3), and it is built:
`sm1::lsstore`, which came with naming stores by storeid (§11);
`sm1::stmt_begin`, for read-committed statement snapshots
(§6.2); the savepoint stack with `sm1::forget` (§6.5);
`sm1::lock_oid` and `sm1::lock_row`, whose modes carry sm3's
table-oid locks and the row lock of its index build (§5.1);
`sm1::scan_seek`, which repositions a cursor instead of reopening
it (§10.2); and the old row returned by `sm1::update` and
`sm1::erase` (§5.3). Descending scans will not be added at all,
and a batched read is left until a benchmark asks for it (§10.1).

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
  wraps the stack in a C API (`../DESIGN.md` §5).

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
│ (storeid│ │ methods  │ │ manager     │ │ (explicit  │ │ tracker    │
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
by `storeid` in decimal (`store.0` is the catalog, §11); a
chained store's is named `chain.<storeid>` (`../VCHAIN.md` §3).

```
key   = class  (1B, 0x00: a versioned key)
      | userkey (variable)
      | ~wts   (8B, big-endian, bitwise complement)
      | ~seq   (4B, big-endian, bitwise complement)

value = flags   (1B)   bit0 = tombstone (this version is a delete)
                       bit1 = commit_ts is stamped in this header
                       bit2 = a move's tombstone (sm1::erase_moved)
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
key                                             value
----------------------------------------------  ---------------------
class userkey      ~wts              ~seq       flags  commit_ts  payload
00    61 63 63 74  FFFFFFFFFFFFFFC2  FFFFFFFE   02     0000...50  "bal=9"
      34 32        (wts 61)          (seq 1)           (80)
00    61 63 63 74  FFFFFFFFFFFFFFCC  FFFFFFFE   00     0000...32  "bal=7"
      34 32        (wts 51)          (seq 1)           (xid 50)
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
  (§4.1) under the key's latch, when no other writer of the
  key is in flight (§5.1), so a key's
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
- **The class byte** opens every key of a store: `0x00` for the
  versioned keys this section describes; `0x01` and `0x02` for a
  chained key's newest and older versions (`../VCHAIN.md` §2).
  sm1 adds it when it builds a key and strips it when it hands a
  userkey out; no caller sees it.
- **Comparator.** Store CFs use a custom comparator,
  `wisr.store.v2`: compare the class bytes; within class `0x00`,
  compare the `userkey` parts (key minus the class and the
  12-byte tail) with `memcmp`, shorter-is-less on a tie, then
  compare the tails with `memcmp`; within the other classes,
  compare bytewise. RocksDB's default bytewise
  comparator is wrong here. With it, a key's tail is compared
  against the extra bytes of a longer userkey that begins with
  it: the versions of `"a"` (`61 FF FF …`, since `~wts` of any
  wts below 2^56 starts with `FF`) sort after those of `"ab"`
  (`61 62 …`). Scans would return `"ab"` before `"a"`, and a
  point read could stop early on a foreign key. With the custom
  comparator, a key's versions are contiguous and userkeys
  sort in `memcmp` order, which is the order `sm1::scan` exposes.

  Every versioned key handed to RocksDB for a store CF —
  writes, seek targets, iterator bounds — carries the class byte
  and the full 12-byte tail.
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
          lockset:  [ (lock key, mode) ... ]   (PREPARED only),
          created:  [ storeid ... ]            (PREPARED only),
          dropped:  [ storeid ... ]            (PREPARED only) }
```

Only **writing** transactions ever get an entry, plus read-only
ones that prepare (§8.4), and only at prepare/commit/abort time —
nothing is written at `sm1::begin`. Absence of a record is
meaningful (§3.4).

The `lockset` is carried only in a `PREPARED` record, and only so
that recovery can re-acquire the locks a prepared transaction
still holds, each in the mode it is held in: those
`sm1::lock_row` and `sm1::lock_oid` took, since a write keeps
none (§5.1, §8.4). `created` lists the stores the transaction
made with `sm1::mkstore`, so that recovery keeps their column
families (§9.1); `dropped` lists those it removed with
`sm1::rmstore`, so that a commit after restart still queues their
column families for removal (§8.3). `prepared_at` is the
wall-clock time of `sm1::prepare`, reported by
`sm1::list_prepared` (§8.5); it must be durable because the
listing is mostly needed after a restart. All four are dropped
when the record flips to COMMITTED or ABORTED. The record is one
value, however many locks it lists.

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

The commit cache is bounded by `commit_cache_entries`. An entry
is made at commit, and also by a lookup that misses the cache
and finds a `COMMITTED` record in `CF:txn`, so that a
transaction's other versions do not each pay for the read. It
is an array of slots in sets of two, a new entry taking the slot
of the smaller `xid` in its set (`COMMITCACHE.md`).
Eviction cannot change an answer:
the status record was written before the insert, so a reader
that misses the cache finds it in `CF:txn`, and the record is
swept only once every version it covers is stamped (§8.3).

### 3.5 The home directory and the root db

A home directory follows PostgreSQL's layout, one name per
concept:

```
{homedir}/global/root      the root db              (global/pg_database)
{homedir}/base/{dbid}/     one per database         (base/{oid})
{homedir}/base/pgsql_tmp/  temporary files          (base/pgsql_tmp)
```

`base/pgsql_tmp` is no database: it holds what does not
outlive a session, the host's temporary files and, in `sm`,
the instance that keeps the rows of temporary tables
(`TEMPTABLE.md` at the top level). `sm1::attach` makes that
instance anew, and `sm1::detach` closes it.

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
key = "M" | "wal_prealloc" → 8B size a new log is zero-filled to
                             (WALPREALLOC.md §2)
```

A database directory is named by its `dbid` in decimal, so
`base` holds no other number and there is no name space to
share: whatever a caller names a database, the name never
reaches the filesystem, and `base` cannot collide with
`global`.

`sm1::init` builds all of this once in a home directory's
life: `base`, `global`, the root db, and the `"version"`
key in it. It takes no lock, because `mkdir` settles a race
by itself — the loser sees `EEXIST`. `sm1::attach` then opens
the root db and checks that version, and `sm1::detach` closes
it. In between, `sm1::attach` recovers every database the
root db names (§9.1) and closes it again, so no database
waits for its first session to be recovered, and one that
cannot be is found at attach. Nothing creates a home directory
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
  once no other writer of its key is in flight (§5), and keys
  its version by it
  (§3.1). Commit allocates `commit_ts = global_ts++`, so
  `commit_ts > wts` for every write of the transaction, and
  `commit_ts > xid`.

**Invariant (version chain order).** For one key, committed versions in
descending `wts` order are also in descending `commit_ts` order.

*Why.* Two transactions that both write key `k` are serialized:
a writer that finds another transaction's version of `k` still
in flight waits for that transaction to end (§5), and a writer
takes its `commit_ts` before it leaves the active table. If `T2`
committed a version of `k` after `T1` did, `T2` took its `wts`
after finding `T1`'s version no longer in flight, so `T2.wts >
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
rest of the lease. A clean close does not: closing a database
writes each sequence's next value as its lease, in one synced
batch, so the next open goes on from there. A database stays
open for `idle_close_ms` after its last session closes, so
sessions that come and go do not close and reopen it. A sequence with no lease row yet has handed
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
- A commit returns only once `visible_ts` has reached its
  `commit_ts`: it waits, after it is done, for the older
  commits still in progress, so the snapshot its session
  takes next sees it (`COMMITVISIBLE.md`).

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
            and V.seq <= T.stmt_seq     # within a statement, §6.2
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
  2. take latch(storeid, k)                             # §5.1
     if another holds an explicit lock on k a write conflicts with:
         let the latch go; wait for it; go to 2
     if the newest version of k is another's, still in flight:
         let the latch go; wait for that transaction; go to 2
  3. latest = newest own version of (storeid, k) visible to T  # §4.4
     if latest is absent:
         latest = newest committed version of (storeid, k)
         if latest.commit_ts > T.snapshot_ts:
             RR, SER: abort with SM_ERR_CONFLICT        # §5.2
             RC: SM_ERR_UPDATED, keeping k locked X,
                 unless T.lock(k).latest
                 (an insert: no check)                  # §5.4
     if latest is a move's tombstone: SM_ERR_MOVED      # (insert: goes in)
     if latest is absent or tombstone: SM_ERR_NOTFOUND  # (insert: DUPKEY)
     if old wanted: keep latest in a pin, the session's or caller's  # §5.3
  4. if T.xid == 0: T.xid = global_ts++
  5. wts = global_ts++                                  # no other writer in flight, §4.1
     seq = T.seq++;  T.seq_hwm = seq                   # T.seq starts at 1
     db.Put(cf(storeid), encode(k, ~wts, ~seq), encode(flags, T.xid, v))
         # a chained store instead writes CURR, moving the old
         # version to OLD, in one batch (../VCHAIN.md §5)
     T.lock(k).latest = false                          # §5.4
  6. if a savepoint is set and not lost:                # §6.5
         T.undo.push(storeid, k, wts, seq)
         if T.undo is over savepoint_undo_bytes:
             free T.undo; every mark now set is lost
  7. ssi.on_write(T, storeid, k)                        # §7
  8. let the latch go
```

Step 3 consults the transaction's own writes first, so a key it
inserted can be updated and a key it deleted can be re-inserted.
An own version still visible under `seq_hwm` is still in flight,
and every other writer of `k` waits for it, so no other
transaction can have committed `k` in between and the conflict
check is skipped. Own
versions undone by `sm1::rollback` are invisible (§6.5), and the
check falls through to the committed version as if they had
never been written.

`sm1::insert_new` is `sm1::insert` of a key no version of was
ever written, which its caller knows, as sm2 knows a key made of
a row's `_id`: step 3 has nothing to find, and is left out, a
read and a seek; steps 2 and 7 stay. A debug build makes the
read, and asserts it finds no version.

`sm1::erase_moved` is `sm1::erase` of a row whose key changed,
which lives on under another (`../ROWKEY.md` §2.3): its
tombstone has bit2 set. A reader sees it as any tombstone. An
update or erase that meets it in step 3 fails with
`SM_ERR_MOVED`, and `sm1::read_latest` does where it returns 1
for a plain tombstone, so a READ COMMITTED writer past
`SM_ERR_UPDATED` learns the row moved, not that it is gone, and
fails rather than skip it. An insert goes in over it, as over
any tombstone. GC keeps the flag when it stamps the version, and
purges it as a tombstone.

Step 5 is an ordinary RocksDB `Put` into the memtable, WAL-logged but
**not synced** — an uncommitted write does not need durability, only
the commit record does (§8.2).

### 5.1 The lock table, and what a write does instead

An in-memory hash table, `(locktype, storeid, userkey)` → the
transactions holding it, each in a mode of its own. The modes are **Gray's**,
for locking at several granularities: IS, IX, S, SIX and X, with
his compatibility table (`sm1.h`, `lockmode_t`). sm1 keeps no
hierarchy; a caller that makes one name stand for a larger
object — sm3's table, by an OID lock on its oid — takes an
intention mode on it to
announce what it does below, so readers (IS) and writers (IX) of
a table coexist while a builder that must stop writers but not
readers takes S. A lock takes any mode, and a plain read
still takes none. Locks are released at commit/abort — strict,
so no cascading aborts.

**A write takes no lock** (`WRITELOCK.md`). It checks its key
and puts its version under a short **latch**, one of 4,096
mutexes chosen by hashing the store and key, held only within
the call and never while waiting. Its uncommitted version
stands for the lock: a second writer of the key, or an
`sm1::lock_row` on it, finds the version still in flight and
waits for the transaction that wrote it, then looks again. That
transaction's end wakes it, and so does a rollback to a
savepoint, which may have deleted the version. A write also waits
for another transaction's explicit lock on the key, and converts
its own explicit lock, if it holds one, to X. So a write adds no
entry to `T.locks`, with one exception: one refused with
`SM_ERR_UPDATED` keeps the key locked X until the re-read (§5.4).
A wait on a transaction is an edge in the waits-for graph to the
session running it, found through a map from `xid` to session
beside the active table; a prepared transaction waits on no one.

A lock's id in the table is two 64-bit words, `lockid_t`
(`src/locktab.h`): the storeid, its top bit flipped for an OID lock
so the two types never meet, and the oid itself or a 64-bit hash
of a row's key, `lock_row_hash`, the same in every build. An OID
lock's id is exact. Two rows of a store whose keys hash alike
are one lock: they block each other, and could deadlock, and a
transaction that locks one holds the other in the same mode. It
is a false conflict, never a missed one, and with 64 bits not
one a workload meets. A transaction's own lock list, `T.locks`,
has an entry for each id it holds, cut back at a savepoint, and
keeps no row's key: the prepared record's lock set (§8.4) holds
the ids. An entry keeps the kind `sm1::lock_row` was given,
which `sm1::lock_status` reports with the id.
A holder points to the entry that made its hold, and the hold
goes with that entry. An OID lock may have a second entry, a
fast-path slot moved to the table after the hold was made, which
a savepoint's rollback releases alone. The list is sm0's
`blist_t`, blocks whose entries never move: a holder keeps a
pointer to its first entry, a fast-path slot to its own, and a
savepoint a mark. A cut keeps up to four blocks for the locks to
come, and `lock_try` makes room before it takes `lock_mu`, so a
lock allocates nothing under the mutex for the list. The id
costs no allocation either, as the table needs none to find an
entry.
One mutex covers the table and every waiter's waits-for list,
which keeps a deadlock walk consistent.

The table is a module of its own, `src/locktab.h`, which knows
ids, the modes and opaque owners, and nothing of sessions:
it finds, adds and drops a lock's holders, and says which
conflict with a mode. A holder's owner is its session and its
first the lock list's entry; waiting, the waits-for graph, the
deadlock search, the fast path and the lock list stay in
`src/lockmgr.cc`, which holds `lock_mu` across each call. So the
table is measured alone, `bench/locktab_bench.cc`
(`../NEXT.md` §1 item 4).

**The fast path**, PostgreSQL's for a relation's weak locks
(its `lock.c`, 9.2). A table's readers and writers take IS and
IX on its oid, which never conflict with each other, so each
needs to be seen only by a strong lock, S, SIX or X, which DDL
takes. An OID lock in IS or IX is kept in one of 32 slots of
its session's own, under the session's `fp_mu`, and not in the
table, while no strong lock is held or asked on a key that
hashes with it: one of 1,024 counts in the database,
`fp_strong`, is 0, chosen by the id's hash, `fmix64` over both
words, so tables 1,024 oids apart share a count only by chance. A strong locker adds itself to its key's
count, then sweeps every session of the database (`fp_sessions`,
under `fp_reg_mu`), moving each slot for its key into the table
under that session's `fp_mu`; then it locks as any lock does,
meeting the weak holders there, in the waits-for graph too. Its
count goes when the lock is released, or at once if none is
taken. The two orders meet at the session's `fp_mu`: a weak
locker reads the count under it, and a sweep takes it after the
count is raised, so either the sweep finds the slot or the weak
locker sees the count and goes to the table. A strong lock of
the session's own on the key is counted already and sweeps
nothing; a weak lock asked while the count is not 0 moves the
session's own slot for the key to the table first. A slot is
released at commit, abort or a rollback past it, under `fp_mu`
alone: a transaction whose locks are all in slots takes no
`lock_mu` at all. A prepare moves the slots into the table,
the prepared transaction being in no sweep's list, and a
restored one counts its strong OID locks again. `lock_status`
lists the slots with the table. The mutexes are taken in one
order: the environment's, `fp_reg_mu`, a session's `fp_mu`,
`lock_mu`. Row locks take no fast path.

**Conversions.** A transaction holding a key in one mode and
asking for one it does not cover — including by writing it —
converts to the weakest mode covering both: IS below IX and S,
those two meeting at SIX, and X above all. It waits for the
other holders that conflict with the new mode, then changes its
entry in place, without ever releasing what it holds. A
conversion is what makes two transactions on ONE key deadlock,
which exclusive-only locking could not do.

**A read-only transaction can hold locks.** Before
`sm1::lock_row`, a transaction with no `xid` held nothing, and
the read-only commit path skipped lock release for that reason.
It no longer can.

**A session can ask not to wait, for QA only.**
`sm1::set_qa_nowait` turns a wait into a failure: a write, an
`sm1::lock_row` or an `sm1::lock_oid` that would block returns
`SM_ERR_BLOCKED` at once. Like `SM_ERR_UPDATED`
it is not an abort and not retryable. The transaction stays
active and holds exactly what it held, and the caller polls by
repeating the call, which fails the same way or goes through
to the checks it always made. Repeating is all a poll needs
because the table keeps no queue: a waiter that is woken
re-tries its whole request too. The price is deadlock
detection. A polling session records no waits-for edges, so it
is never a victim, and sessions polling each other's locks
poll for ever; that cycle is the caller's to break. It exists
so one thread can play several sessions, and a script can say
"this step blocks" with no thread and no sleep
(`../rel/REL.md`). It is not the interface for PostgreSQL's
`NOWAIT` and `SKIP LOCKED`: PostgreSQL passes a wait policy
with each call, and sm1 is to take one the same way, as a flag
on the calls that can wait. `SM_ERR_BLOCKED` is the code those
flags will return too.

**Every wait can be cut short.** sm1 waits through `sm_sys.h`
(`../sm0/INTERRUPT.md`): an interrupt to the waiting thread,
a bit in its wait mask (`sm_wait_mask()`), makes the wait
fail with `SM_ERR_INTERRUPTED`: not an abort, the transaction
left active, as after `SM_ERR_BLOCKED`. A session waits in one
place only, on the lock table's condition, for a lock and for
an uncommitted writer alike, and that wait is a `sm_wait()`
on `lock_cv` under `lock_mu`, so every wait there is can be
ended. The interrupt goes to a thread, not a session: it is
the thread that blocks. sm1 takes no bit: **the bit stays
pending until the host takes it**, so every later wait fails
at once too. An interrupt is then never lost to a wait that
began a moment late, nor to a caller that goes on after the
error; when it is done with it is the host's to say.
This is what a host builds query cancel, statement timeout
and lock timeout from: the bits and the timers are
`sm_sys.h`'s, and their meaning the host's. The alternative was for the host to
poll, with every wait a refused call it repeats; that loses
the waits-for graph, and with it deadlock detection, which
`set_qa_nowait` already shows.

Deadlocks are found as a transaction begins to wait: it
searches the waits-for graph from itself, so a cycle its wait
closes is found at once, as PostgreSQL finds one that a
waiter's place in a lock's queue closes. A cycle that forms
while it waits, its holders changing under it, is found when
it has slept `deadlock_ms` (default 50ms, or the session's
own, `sm1::set_deadlock_ms`: PostgreSQL's `deadlock_timeout`),
and each `deadlock_ms` after. If it can reach itself, the
**youngest** transaction on that cycle — the last to begin —
fails with `SM_ERR_DEADLOCK`. The search is a walk of the
waits-for edges under `lock_mu`, so a wait pays for one as it
begins; there is no detector thread.

**The edges are read from the lock table as the search walks**
(`lock_blockers`), as PostgreSQL's detector reads its lock
table: a waiter waits on the holders that hold, now, a mode
conflicting with the one it asks, or on the transaction it waits
for while that still runs, and was not rolled back since. A
waiter's own list, `waiting_for`, is what its wait saw when it
last looked, and goes stale the moment a holder releases: the
waiter is woken, but until it runs again its list still names
the holder, which may by then be waiting in a new transaction
on something the waiter holds. Walking those lists found such
cycles that no longer existed and doomed a transaction that
did not need to fail (`../../dg/WORKNOTE.md` step 339; `e07`).
Every lock a waiter can wait on is in the table: a strong lock
moves the weak ones from their slots before it waits, and a weak
lock waits only on strong ones (§5.1).

The waits-for edges form a **graph, not a chain**: a waiter waits
on every holder whose mode conflicts with the one it asks, and
there can be many, so the search is a depth-first walk with a
visited set, and the path it is on supplies the cycle to pick a
victim from. This is the cost of shared modes. Before it, only
writers could wait, each on exactly one holder, and following the
single pointer was enough.

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
sets on a serializable reader. A rollback that fails dooms
its own transaction with `SM_ERR_DOOMED` (§6.5).

**What a transaction holds in memory.** Row values stream
to RocksDB, so a transaction is bounded by disk, but
three structures are not: `T.locks` (one entry per key
locked with `sm1::lock_row` or `sm1::lock_oid`; a write keeps
none), `T.undo` (one
`(storeid, userkey,
wts, seq)` tuple per write made while a savepoint is set,
§6.5), and the SSI read set. All three
are linear in the number of keys touched, with the
userkey stored inline, so a transaction writing tens of
millions of keys costs gigabytes. This is the price of
O(1) abort and no undo pass, and it is a real limit on
single-transaction size rather than a bug; §13 records
it.

### 5.2 First-committer-wins

Step 3 rejects a write when the key was modified by a transaction
that committed after our snapshot. Together with the wait on
an uncommitted writer of the key (which prevents the concurrent
case), this is the standard
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

The bytes are not copied: the iterator that found the version
is kept in a raw pin (`../ROW.md` §2), the session's own, the
one `sm1::read` uses, so either call invalidates what the
other returned; or one the caller gives, which keeps the row
until the caller empties it. A write lets go of the row its
pin held only once it has succeeded, as its new value may
view that row.

The value is only produced for a live version. A write that
fails — `SM_ERR_NOTFOUND` on a missing key, a conflict, a
deadlock — reports no old row.

### 5.4 Read committed: write, re-read, write

At READ COMMITTED an update or delete that meets a row committed
after `sm1::begin` waits for any writer of it in flight, as
every write does, and then is refused with `SM_ERR_UPDATED`,
taking the key's lock X. That is not an abort: the transaction
stays active, keeps the lock, and
`sm1::retryable()` is
0. The caller calls `sm1::read_latest`, which returns the newest
committed version of the row ignoring the snapshot, re-evaluates
its `WHERE` clause and new values from it, and only then repeats
the write. This is what PostgreSQL's executor does at its default
level after a lock wait, and workloads written for it rely on it
(`../PART4.md` §5a).

The commonest case is a hot row, a shared counter or a
`next_order_id`. Two read committed sessions each add one to `R1`,
which holds 0; T1 updates it and T2 waits for T1:

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

| Event | Effect on the mark |
| --- | --- |
| first write refused with `SM_ERR_UPDATED` | lock taken, mark not set |
| `read_latest` on that row | mark set |
| write repeated with the mark set | write goes through; mark cleared |
| write repeated without the mark | refused again with `SM_ERR_UPDATED` |
| rollback to a savepoint taken before the lock | lock released, mark gone with it |

The mark is single-use, but a later write to the row needs no
read while the newest version is the transaction's own. It
needs one again if those writes are undone by a rollback to a
savepoint taken after the lock: the newest version is the
other transaction's once more, the lock is still held, and the
mark was spent. sm1 forces the read, not the re-check: whether
the caller evaluates its predicate against the image is up to
the caller.

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
  taken by the refused write or by `sm1::lock_row`; otherwise
  `SM_ERR_INVALID`. Above READ COMMITTED it reads only a newest
  version that is the transaction's own, which the snapshot
  does not hide and no other writer changes: an insert's
  arbiter finds so a key its own statement took (scenario
  `a19`). Under a shared lock it reads but sets no
  mark, since a shared holder cannot write without upgrading. A
  caller that waited in `sm1::lock_row(SHARED)` (sm3's index
  build reading a row, step 3 of `../sm3/SM3_DESIGN.md` §3)
  uses it to see what committed while it waited. - **An insert**
  has no predicate to re-evaluate, so it is judged against the
  newest committed version (`SM_ERR_DUPKEY` if live) with no
  check, as PostgreSQL's unique check does. - **`sm1::lock_row`**
  at READ COMMITTED no longer fails with `SM_ERR_CONFLICT`. At
  the higher levels it is unchanged. - REPEATABLE READ,
  SERIALIZABLE, SSI (READ COMMITTED takes no part, §7.4), 2PC and
  the GC watermark are unchanged: a read committed transaction
  already pins its begin snapshot, and the newest committed
  version is never collected.

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
block — on another writer of its key, or on a lock — and
still read everything committed while
it waited. `xct_read_view` takes it; `xct_view`, which the
write path uses to validate a storeid and key, only reads it
once taken.

The GC watermark needs nothing extra. A read-committed
transaction registers its *begin* snapshot for its whole life
(§8.3), and a statement snapshot is at or above it, so
`gc_watermark` is already held below anything a pinned
statement can read.

**A statement does not see its own writes.** `sm1::stmt_begin`
also sets a mark, `T.stmt_seq`, to the transaction's `seq` at
that moment, at every isolation level. From then every read,
a point read or a cursor, sees the transaction's own versions
only up to the mark: those written before the statement
began. This is PostgreSQL's command id. Without it, a cursor
was safe, freezing `seq_hwm` when it opened, but any read
begun later in the statement saw the statement's own writes:
the inner side of a nested-loop join re-scanned per outer row,
a correlated subquery, a host's lookup by key. That is the
Halloween problem.

- **Writes still see the newest own version**, to lock and
  check against it (§5). An update or a delete whose key's
  newest version is the transaction's own, with `seq` above
  the mark, fails with `SM_ERR_SELFMODIFIED` and writes
  nothing: the statement changed that row already and cannot
  see the change. It is not an abort. The caller skips the
  row, as PostgreSQL's update skips a row "already modified by
  me", or raises an error, as `INSERT ... ON CONFLICT` does.
  An insert is unchanged: a key the statement inserted is
  still a duplicate.
- **A lock of a row the statement read** is refused the same
  way: `sm1::lock_row` and `sm1::chain_lock_row` with `as_read`,
  as SQL's FOR UPDATE and FOR SHARE lock a row, fail with
  `SM_ERR_SELFMODIFIED` on a key whose newest version is the
  transaction's own with `seq` above the mark, and take no lock.
  PostgreSQL's FOR UPDATE passes over such a row, as a cursor's
  FETCH does after a later UPDATE or DELETE of its transaction. A
  lock without it, which a caller takes to read the key's newest
  version, its own included (an insert's arbiter, an index
  build), is unchanged. Scenario `a18`.
- **A rollback lowers the mark** with `seq` (§6.5), since the
  rewound numbers are written again by the same statement.
- **`sm1::stmt_advance` moves the mark to now** and leaves the
  snapshot, as PostgreSQL's `CommandCounterIncrement`: the
  statement's later reads see its writes so far. The rule
  above is unchanged; only the mark's value moves. It is how a
  statement uses what it made: a table, its store and its
  `_id` sequence, which `CREATE TABLE AS` makes and fills. A
  store or a sequence is looked up at the mark like a row, and
  until the advance the statement does not see it.
- **A transaction that never calls `sm1::stmt_begin`** sees all
  its own writes, as before.

**A statement inside another.** A trigger's or a function's
commands run inside the statement that fired them. Each sees
the outer's writes so far, and when it returns, the outer
resumes with its own snapshot and mark, seeing neither its own
writes nor the inner one's. `sm1::stmt_begin(session, outer)`
begins such a statement, saving the outer's snapshot and mark
in `outer`, and `sm1::stmt_end(session, outer)` puts them back,
the mark no higher than `seq`, which a rollback in between may
have lowered. PostgreSQL's executor saves and restores its
snapshot the same way around nested commands.

**A top-level statement needs no `sm1::stmt_end`.** The next `sm1::stmt_begin`
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
writer in flight, is refused with `SM_ERR_UPDATED`, and goes
ahead
against the newest committed version once the caller has read it
with `sm1::read_latest` (§5.4). PostgreSQL does the same at its
default level, and workloads rely on it (`../PART4.md` §5a).
REPEATABLE READ and SERIALIZABLE keep §5.2 as it is.

What makes that safe is that a version's key is its `wts`, taken
once no other writer of the key is in flight, and not the
writer's `xid` (§3.1, §4.1).
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
tuples in write order, each flagged if its key is chained
(`../VCHAIN.md` §6), holding no payloads, so a large
transaction costs bytes per write, not rows.

**A write is listed only while a savepoint is set.** A
rollback goes back to a mark and no further, so a write made
with no mark on the stack can never be undone by one, and
listing it would cost memory for nothing. A transaction that
sets no savepoint keeps no list at all, however much it
writes. When the last mark is forgotten the list is freed,
for the same reason. Each mark carries the `seq_hwm` of its
moment, since the write before it may not be in the list to
ask.

**The list is capped.** `savepoint_undo_bytes`, 64 MB by
default, bounds what one transaction's list may hold, each
entry counted with its key. A write that takes the list past
it is not refused. The list is freed, a warning is logged,
and every mark then on the stack is **lost**: `T.lost_marks`
records how many, from the bottom of the stack up.

- A rollback to a lost mark fails with `SM_ERR_UNDOLIMIT`.
  The versions to delete are no longer known, so the
  transaction can only be aborted, which needs no list: the
  rollback dooms it (below).
- Writes go on, and list nothing while every mark is lost.
- A savepoint set afterwards starts a new list, counted
  against the cap again, and rolls back as usual.
- Forgetting a lost mark is allowed, and it is then no longer
  counted.

The write is let through because a rollback is the rare
case. A statement that outgrows the cap and succeeds, which
is nearly all of them, is not made to fail for the sake of
one that might have been rolled back. The price is paid only
by the rare one: its rollback becomes an abort of the whole
transaction.

**A rollback that fails dooms the transaction.** One at a lost
mark, or one whose deletes RocksDB fails to read or write
(`SM_ERR_IO`), leaves the writes it was to undo in place, some
of them perhaps deleted. Nothing of that may commit, so the
rollback sets `T.doomed` to `SM_ERR_DOOMED` and keeps its own
error as the reason. Every later call on the transaction fails
with `SM_ERR_DOOMED`, naming that reason: each entry point that
checks `T.doomed` (§5.1), and an open cursor's next read or
seek, and a sequence draw. Only `sm1::abort` is let through. A
caller that has nowhere for the rollback's error to go, as
PostgreSQL's subtransaction abort has not, finds it again at
its next call. A rollback refused by its checks, at a depth
with no mark or with a bulk load open, changes nothing and
dooms nothing.

**A savepoint is expensive by design.** Setting one is a
mark and costs nothing. Holding one costs memory in
proportion to the work done after it: one tuple, with the
key inline, for every write until the last savepoint is
forgotten or the transaction ends, up to the cap above. A
rollback to that mark issues one delete for each.

This follows from two choices made elsewhere. Writes are
eager (§5), so a version to be undone is already in RocksDB
and must be deleted by its key. And visibility is decided by
`xid`, one for the whole transaction, so a version left in
place would commit with the rest. PostgreSQL pays nothing
here because each savepoint starts a subtransaction with an
xid of its own, and rolling back marks that xid aborted.

`SAVEPOINT.md` is the design that removes the cost: a
rollback records the range of `seq` it undid as dead, in the
transaction's status, and deletes nothing. It is not built.
§13 lists the cost as a known trade-off. Stores
created by the transaction are tracked alongside it in
`T.created`, a vector of storeids in creation order, and
stores it dropped in `T.dropped`, likewise.

**A bulk load lists one entry, not a key a row**
(`../BULKLOAD.md` §3.5). Opened while a savepoint is set, it
adds its store to `T.loaded_stores`, and a rollback to a mark
set before it empties the store with one `DeleteRange` in the
batch of its other deletes: the store was empty when the load
opened, and kept from every other transaction since, so all
it holds is this transaction's since then. While a load is
open no savepoint is set and none is rolled back to
(`SM_ERR_INVALID`): its keys share one `seq` and wait in its
files, and could not be undone in part.

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
        mark.seq_hwm = T.seq_hwm
        mark.locks   = T.locks.size()
        mark.stores  = T.created.size()
        mark.drops   = T.dropped.size()
        mark.scan_id = id of the latest cursor opened so far
    return len(T.savepoints)

sm1::forget(depth):                      # SQL's RELEASE SAVEPOINT
    if no mark at depth: fail with SM_ERR_NOSAVEPOINT
    T.savepoints.truncate(depth - 1)   # that mark and those inside it
    T.lost_marks = min(T.lost_marks, len(T.savepoints))
    if every mark left is lost: T.undo.clear()     # nothing can reach it
    # writes stay; locks stay, now held for the enclosing mark

sm1::rollback(depth):
    if no mark at depth: fail with SM_ERR_NOSAVEPOINT # API.md divergence 8
    if depth <= T.lost_marks: doom T; fail with SM_ERR_UNDOLIMIT
    m = T.savepoints[depth-1].undo
    for e in reverse(T.undo[m..]) if not e.chained:
        db.Delete(cf(e.storeid), encode(e.key, ~e.wts, ~e.seq))
    for e in reverse(T.undo[m..]) if e.chained:     # ../VCHAIN.md §6
        under latch(e.storeid, e.key):
            chain_restore(e.key, T.xid, mark.seq_hwm)
    r = T.savepoints[depth-1].stores
    for storeid in reverse(T.created[r..]):
        db.DropColumnFamily(cf(storeid))            # §11
    T.created.truncate(r)
    T.dropped.truncate(mark.drops)                  # the undo restored their rows
    T.undo.truncate(m)
    T.seq_hwm = mark.seq_hwm                        # own-write visibility
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
did not clobber the pre-savepoint version. A chained key has
no version of its own to delete: its newest version sits in
place, the one before moved to OLD. `chain_restore` puts back
the newest version at or below the mark's `seq_hwm` or not the
transaction's, and unlinks the entries it passes
(`../VCHAIN.md` §6).

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

**Closed, not freed.** A closed cursor stays the session's:
each `sm1::scannxt` and `sm1::scan_seek` on it fails with
`SM_ERR_SCANCLOSED`, until its caller ends it with
`sm1::scanend`, or the transaction's end does. A caller may
not know that a rollback reached one of its cursors. A SQL
cursor declared before a savepoint whose plan begins a scan
again inside it holds such a cursor, and freeing it would
leave the next `FETCH` a dangling pointer. The code is its
own, not `SM_ERR_INVALID`, so a host can tell it from a
misuse and say why: the cursor's old view cannot be
recreated, and digres reports so.

Releasing locks acquired after the mark is safe: the row was untouched
before the savepoint, so no one observed our intent on it. A
writer waiting for the transaction is woken, since a version
it waited on may be gone, and looks again.

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
- **Edges.** Per transaction, `in_edges` and `out_edges`: the
  transactions with an rw-edge into it, and those it has an
  rw-edge out to. An edge is kept on both ends, and a node that
  goes takes its edges with it, so an aborted transaction leaves
  no edge behind.

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
                          (in T_reader.out_edges and
                           T_writer.in_edges)

T_reader reads k   →  if the version it skipped was written by a
                      concurrent T_writer (committed after our
                      snapshot, or still active):
                          edge  T_reader ──rw──▶ T_writer
```

### 7.3 The abort rule

A cycle in the serialization graph always contains a
*dangerous structure* `T1 ──rw──▶ T2 ──rw──▶ T3`, T2 the
*pivot*, in which T3 commits first (Ports and Grittner,
VLDB 2012). T1 and T3 may be the same transaction: a write
skew is the cycle of two. wisr applies their rule, which the
edge lists make possible, in place of Cahill's two flags:

```
dangerous(T1, T3), for a pivot T2 that commits after T3:
    T3 has committed, and
    T1 is T3, or
       T3 committed before T1 did (or T1 is still running), and
       if T1 is read-only (declared, or committed having written
       nothing):
           T3 committed before T1's snapshot

at commit of T:
    if T is doomed, or
       for some committed T3 in T.out_edges and T1 in T.in_edges,
       dangerous(T1, T3):
        abort T with SM_ERR_SERIALIZATION

edge R ──rw──▶ W, made by R, onto a committed W:
    if for some T3 in W.out_edges that committed before W,
       dangerous(R, T3):
        doom R
```

A committed pivot cannot be told "no", so an edge that makes it
the middle of a dangerous structure dooms the running T1
completing it instead. A structure whose T3 commits after T2
fails no one: T3 then follows T2 in the serial order, and the
pivot's own commit check already let it through. Dooming never
fails the call that made the
edge: the transaction runs on and fails at its commit, so
`sm1::read`, `sm1::scannxt`, and the write calls never return
`SM_ERR_SERIALIZATION`. The commit check runs after the
transaction claims its `commit_ts` and before its status record
is written; a failure hands the `commit_ts` back to the sequencer
unused. A prepared transaction (§8.4) counts as committed for
these rules, at no known time: as T3 it counts as having
committed first, and as T1 or T2 as committing last, the
readings that can only fail more.

The rules are still conservative: a dangerous structure fails a
transaction whether or not the rest of a cycle exists. In a
plain write skew only the second to commit fails. That is the
accepted cost of never taking a read lock and never deadlocking
on reads.

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

A transaction is read-only to SSI in two ways. `sm1::begin`
can declare it, as SQL's `READ ONLY`; sm1 then refuses its
writes with `SM_ERR_READONLY`, which is what makes the rest
sound. Or it commits with `T.xid == 0`, having written
nothing. Only the declared kind gets the safe-snapshot
optimization (Ports & Grittner), described below.

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

**Declared.** §7.3's read-only test applies to a declared
transaction as T1 from its begin: a structure through it is
dangerous only if T3 committed before its snapshot. Its node
also watches for its snapshot to become safe:

```
at begin of declared T_ro (snapshot S):
    if a restored transaction is in doubt or committed after S:
        unsafe
    if a writer that committed after S has an edge out to a
       transaction committed at or before S:
        unsafe
    otherwise watch every serializable writer still running;
    none: safe at once

when a watched writer W ends:
    W committed with an edge out to a transaction committed at
    or before S:  T_ro is unsafe
    W was the last one watched, T_ro not unsafe:  T_ro is safe
```

Deciding at a writer's commit check is enough: an edge out
it gains later leads to a transaction still running then,
which commits after S. A writer that ends without committing
simply stops being watched. A safe node drops its
SIREAD locks and its edges out, and its later reads leave no
entry and make no edge: it can no longer be the T1 of a
dangerous structure. An unsafe one stays fully tracked to its
end. A writer here is any serializable transaction not itself
declared read-only. The watch is under `ssi_mu`, like the rest
of the tracker. `DEFERRABLE`, waiting at begin for a safe
snapshot, is not built.

**Undeclared.** A transaction that never declared itself is
known to be read-only only when it commits with `T.xid ==
0`; from then on §7.3's read-only test applies to it. While
it is active it stays fully tracked: it may still write, and
then it needs its read set as a possible pivot. Dropping the
tracking early and failing a later write instead would fail
every read-then-write transaction whose snapshot became safe,
on an idle database at once, and again on every retry. Its
SIREAD locks stay under §7.1's rule after it commits.

PostgreSQL applies the read-only test only to a transaction
declared `READ ONLY`, so sm1 commits some pivots that
PostgreSQL fails; `mvcc/MVCC.md` §7b has p06's.

### 7.5 Memory

The tracker holds SIREAD locks and edges for
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
     if T.xid == 0:                            ← read-only: nothing to publish
         if SERIALIZABLE: ssi.validate(T, visible_ts + 1)
                                               → may SM_ERR_SERIALIZATION
         go to step 6
  2. commit_ts = sequencer.claim()           ← global_ts++, joins the set
     if SERIALIZABLE: ssi.validate(T, commit_ts)    (§7.3)
         → on failure: sequencer.done(commit_ts), unused;
           SM_ERR_SERIALIZATION
  3. batch = { CF:txn[T.xid] = {COMMITTED, T.snapshot_ts, commit_ts} }
  4. db.Write(batch, sync = durability_sync)   ← group-committed
  5. commit_cache.insert(T.xid → commit_ts);  active_table.remove(T.xid)
     if T made or dropped a store: last_ddl_ts = max(last_ddl_ts, commit_ts)  (§11)
     sequencer.done(commit_ts);  advancer moves visible_ts      (§4.3)
     wait until visible_ts >= commit_ts      (COMMITVISIBLE.md)
  6. lockmgr.release(T);  ssi.on_commit(T)
     queue (T.dropped, commit_ts) for CF removal     (§8.3)
```

Step 1 closes cursors first, as `API.md` requires of
`sm1::commit` and `sm1::abort`: this releases their RocksDB
snapshots, which pin `gc_watermark`, and narrows each cursor's
SIREAD predicate to the range it actually traversed (§10.2)
before validation runs. A writer validates only once it holds
its `commit_ts`, so the rules of §7.3 judge it at the time it
commits; a read-only transaction takes none, and counts as
ending just after the current `visible_ts`.

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

The rules above are those of a versioned key, class `0x00`. A
chained store's keys, its CURR (`0x01`) and OLD (`0x02`)
versions, are judged by rules of their own in the same filter,
`filter_chained` (`../VCHAIN.md` §7): a CURR version committed at
or below the watermark is stamped, and a tombstone so committed
dropped, with no purge queued: RocksDB writes a delete over the
key, so no older entry of it comes back; a dead CURR version is
queued for the GC worker, which, under the key's latch, puts back
the version before it. An OLD entry goes once it is unlinked and
the watermark has passed its ts, or once its displacer committed
at or below the watermark; one kept is stamped with its writer's
and its displacer's `commit_ts`, so the `CF:txn` sweep below
holds for chained keys as well.

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
                      explicit ones: a write keeps none
            created = T.created, dropped = T.dropped}
  3. db.Write({CF:txn[T.xid] = rec}, sync = true)      ← always synced
  4. T.state = PREPARED;  locks and SSI state are RETAINED
```

Step 4 moves the transaction out of its session into an object
the database owns, registered under its `gid`: the transaction's
state, its snapshot's place in `gc_watermark`, its SSI node, and
its locks (a write keeps none, §5.1), whose table entries are
re-pointed at the new owner. The session keeps only the `gid`.
Any session can then resolve the transaction, `sm1::closedb`
leaves it untouched, and recovery (§9.1 step 5) rebuilds exactly
the same object from the `PREPARED` record, so a transaction
prepared before a crash and one prepared in this process are
handled alike. The record is a single `CF:txn` value however
large the write set; the write set is stored as the locks the
transaction holds, each by its id, with the mode it is held in
and the kind `sm1::lock_row` was given, 20 bytes
(`lock_put_record`): so a shared lock taken by `sm1::lock_row`
or `sm1::lock_oid` is restored as shared, and its id as it was
held, no key hashed again, the row hash the same in every build
(`lock_row_hash`).

Step 1 closes cursors for the reasons given in §8.2. No data
operation is accepted after prepare, so an open cursor could
never be used again, but it would pin `gc_watermark` for as long
as the transaction stays in doubt. The coordinator's commit
(§8.2 from step 2) therefore finds none left to close.

A read-only transaction is not necessarily lock-free:
`sm1::lock_row` and `sm1::lock_oid` give one locks without
writing, and those go into the record and are restored like any
other. What makes it read-only is the empty set of versions, not
an empty lock set.

A read-only transaction still needs a durable `PREPARED` record,
because `API.md` promises that a prepared transaction survives
a crash and can be resolved by `gid` afterwards. Step 1 gives it
an xid (entering the active table, §3.4) so that the record has
a unique key; with `T.xid == 0`, every read-only prepare would
overwrite the same `CF:txn[0]`. Its `lockset` holds whatever
`sm1::lock_row` and `sm1::lock_oid` took and nothing more, so a
transaction that only read holds nothing after a restart.

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
lost its SIREAD locks and edges, which are volatile, so
a new transaction can write a key the prepared one read and no
edge appears. Unchecked, that admits write skew:

1. Before the crash, `T_prep` reads `A = a0`, writes `B = b1`,
   and prepares. Only `B` has a version of it.
2. After restart, `b1` is in flight again, so a writer of `B`
   waits for `T_prep`, but its SIREAD lock on `A` is gone.
3. `T_new` reads `B`, skips `b1` as in-flight, and reads `b0`:
   edge `T_new ──rw──▶ T_prep`.
4. `T_new` writes `A = a1`. No SIREAD covers `A`, so the edge
   `T_prep ──rw──▶ T_new` is never created, and `T_new`
   commits.
5. The coordinator commits `T_prep`. Each read what the other
   overwrote — a cycle.

Persisting SIREAD locks in the `PREPARED` record would not be
enough alone, because the edges are lost too (a
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
its versions in flight make any writer of its keys wait (§5.1),
so every edge *into* it from a post-restart transaction is an
rw-edge from reading past one of its versions — which the rule
sees. Edges *out* of it are unknowable, so every transaction with
an edge in is doomed, and no cycle can pass through it. The rule
still holds after the coordinator commits it: a reader whose
snapshot predates that commit still skips its versions and is
still doomed; a reader whose snapshot follows it sees them and
forms no edge. The tracker keeps a restored xid in its restored
set, after the transaction leaves the active table, until
`gc_watermark` passes its `commit_ts`. The cost is false aborts
for serializable readers of in-doubt keys, which is acceptable
for a state that only follows a crash.

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
2. Read CF:meta → format version, persisted next_ts, next_objid.
3. global_ts = next_ts + 1;  visible_ts = next_ts.
4. Scan CF:txn for records in state PREPARED.
   (COMMITTED / ABORTED records need no action.)
5. For each prepared record:
      - register an entry in the active transaction table (in-doubt),
        marked restored so that readers skipping its versions
        are doomed (§8.4), with T.created and T.dropped from
        the record
      - re-acquire the locks in its lockset, each in its mode;
        its writes need none, their versions being in flight
        again, so a writer waits for it as before
      - pin gc_watermark at its snapshot_ts
6. Orphan CF sweep: read the catalog (store.0) and drop every
   store CF, store.<storeid> or chain.<storeid>, that has
   neither a live catalog row naming it (a name's newest
   committed version, not a tombstone) nor an entry in a
   prepared record's `created`.
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
are dropped by the next compaction, or, a chained key's, put
back by the GC worker (§8.3). No scan of the data is
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

A restored prepared transaction blocks writers on its keys (its
versions are in flight, §5.1) and pins GC. `sm1::list_prepared`
exposes them by `gid` so the coordinator can finish them. An
operator escape hatch (`sm1::abort_prepared`) is required,
because an abandoned prepared transaction otherwise blocks GC
forever; this is the same operational hazard as in any 2PC system
and is exposed, not hidden.

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
    hi    = max(T.wts_hi, snap)                          # own writes, §4.4
    if SERIALIZABLE:
        ssi.on_read(T, storeid, key)                     # SIREAD, §7.1
    it = pin.reusable ? pin.it                           # below
                      : db.NewIterator(cf(storeid))
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

**A pin's iterator is sought again.** A read into a raw pin
leaves the row in the iterator that found it (`../ROW.md` §2),
and opening that iterator -- a merging iterator over the
memtables and every level -- was over half of a point read: on
the bench's `wide_mixed`, 1.3 µs of a 2.3 µs read in key order.
So the next read into the same pin seeks the
iterator it holds instead (`reusable()` in `src/access.cc`)
when that iterator holds everything the read may see:

- **Its session and store.** The pin records which read opened
  it.
- **Every commit the view sees.** An iterator holds the store
  as it was when opened. The pin records `visible_ts` read just
  before it was opened, so every commit at or below is in it;
  it serves a view whose snapshot is no later. Under read
  committed the next statement's snapshot may pass it.
- **The session's own writes.** `session_t::write_gen` rises
  with every write, and as a bulk load closes, and never falls,
  as `seq` does on a rollback (§6.5); an iterator serves only
  reads made at the generation it was opened at.
- **The read epoch it was opened in** (§8.3 step 3). Between
  reads the pin is no reader, so the sweep may delete the
  records of versions the iterator holds unstamped; once the
  epoch has moved on, a new iterator, opened after the pass,
  holds them stamped. Within the epoch, the read entered it
  before seeking, so no deletion passes it.
- **Not SERIALIZABLE.** The SIREAD must exist before the
  iterator (above): an older iterator misses a concurrent write
  made between the two, and with it the rw-edge (scenario c36).

The same reuse with no pin of the caller's: `sm1::read`'s
lending form reads into the session's own pin. A write's
existence check (§5) and `sm1::read_latest` still open their
own. With it a read into one pin costs what RocksDB's `Get` of
the exact version key does: 3.6 against 3.3 µs at random, 0.9
against 1.1 µs in key order.

**No batched read of versioned keys.** An index lookup
followed by N base reads costs N reads, and a call taking N
keys was considered. For versioned (class `0x00`) keys RocksDB's
`MultiGet` does not apply: a read here is an iterator seek
across a key's version run, not a point `Get`. With the pin's
iterator sought again, what a batched call would add is seeking
in sorted order, which an index range already does, so it is
not built. A chained key's newest version is a point `Get`, and
for those it is built: `sm1::chain_read_batch`, on `MultiGet`
(`../BATCHREAD.md`).

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
the tail is `~wts | ~seq`, and `wts` 2^64−1 is never reached
while `wts` and `seq` both start at 1, so these tails fall
strictly before or after every version of the bound key.

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
sequence's row. A commit that writes only `"U"` or `"S"` rows
leaves `last_ddl_ts` alone: no store lookup reads them.

A store's row carries nothing: that the row exists at a
snapshot is what makes the store exist there. The design's
`created_ts` field is left out, since nothing reads it.

Big-endian storeids make the catalog's key order numeric, so
`sm1::lsstore` is one scan of the `"R"` prefix and returns
storeids in ascending order. Storeids are handed out in creation
order, so the lowest one listed is the first store the database
ever had. A layer above keeps its own catalog in store 0 under
`"U"` (below), which is always there, so it has no store to
find.

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
stores contend on one row and fail with `SM_ERR_CONFLICT`.

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

Every data call looks its storeid up first, and a lookup that
read `CF:store.0` each time would build a second iterator for
every point read, about a third of its cost. So a session
caches which storeids are live (`session_t::store_cache`,
`catalog_lookup()`). The answer depends on the snapshot: after
a committed `sm1::rmstore`, a transaction with an older
snapshot must still find the store, since it may still
legitimately read it. A cache invalidated at DDL commit would
hide it, so the cache is checked by timestamp instead:

- The database keeps `last_ddl_ts` (`db_t`), the largest
  `commit_ts` of any committed transaction that made or dropped
  a store, its `T.created` or `T.dropped` not empty. Commit
  raises it before publishing that `commit_ts` (§8.2 step 5), so
  a snapshot that includes a DDL commit always sees it. A
  prepared transaction restored at open has both lists from its
  record (§3.2).
- Each cache entry records the snapshot it was read at and
  whether the store was live then. A hit is used only if
  `last_ddl_ts` is at or below both that snapshot and the
  lookup's; then no DDL committed between the two, and the
  answer is the same at both. Otherwise the lookup reads
  `CF:store.0` and replaces the entry.
- A transaction that has made or dropped a store neither uses
  nor fills the cache: its lookups see its own uncommitted
  catalog versions, which no entry stands for. That covers what
  evicting at `sm1::mkstore` and `sm1::rmstore` would, and a
  rollback to a savepoint besides.
- The cache starts over at 4096 entries (`store_cache_max`).

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
  read it from `store.0` (its `begin_seq`, since a read-only
  transaction has no xid), and a serializable lookup uses a hit
  only if that is the current transaction. Its SIREAD already
  catches every later writer of the storeid (scenario c35).

### 11.1 Sequences

A sequence is a catalog row of its own, keyed `"S" | name`, so
`sm1::mkseq` and `sm1::rmseq` are transactional exactly as
`sm1::mkstore` is, down to the wait that makes two concurrent
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
none. That read is a catalog read each call; `sm1::seqref`
makes it once, into a `seqref_t` that `sm1::nextseq` then draws
from, good while the sequence is not dropped, which its holder
sees to: sm2 draws a table's `_id`s so. The last value is the largest one at or below
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
puts a table's rows, its indexes and its TOAST data in one
store (`../DESIGN.md` §5), so a temporary table and all its
indexes are one temporary store, and no per-row or per-key
marking exists at any level.

**Status: built, by another mechanism.** `TEMPTABLE.md` at the
top level is the design of record: a temporary store is a
column family of one unlogged RocksDB instance kept apart
from every database, not of the database's own.
`sm1::mkstore` makes one when asked, with no row in store 0;
it is its session's alone, and is dropped when the session
closes. What follows is the mechanism proposed before it,
checked against RocksDB 8.9.1, and kept for its account of
what RocksDB offers.

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

Settled since: `sm1::mkstore` takes a `temporary` flag, and a
temporary store's files are those of the instance of their
own, under `base/pgsql_tmp/sm` (`../TEMPTABLE.md` §3).

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
SM_ERR_MOVED           update, erase or read_latest meeting a move's
                       tombstone (§5); not an abort
SM_ERR_BLOCKED         a write or lock would wait, under set_qa_nowait
                       (§5.1); not an abort
SM_ERR_INTERRUPTED     a wait ended by an interrupt (§5.1); not an abort
SM_ERR_READONLY        a write in a transaction declared read-only (§7.4);
                       not an abort
SM_ERR_NOSAVEPOINT     no savepoint at the given depth (§6.5)
SM_ERR_UNDOLIMIT       the savepoint was lost at savepoint_undo_bytes (§6.5)
SM_ERR_DOOMED          a rollback failed; only an abort is let through (§6.5)
SM_ERR_SCANCLOSED      a read or seek on a cursor a rollback closed (§6.5);
                       not an abort
SM_ERR_SELFMODIFIED    update or erase of a key the running statement
                       wrote (§6.2); not an abort
SM_ERR_NOTRANS         data op outside a transaction (§6.1)
SM_ERR_INTRANS         op forbidden inside a transaction (§6.1)
SM_ERR_PREPARED        illegal op on a prepared transaction (§8.4)
SM_ERR_EXISTS          home dir, database, or store exists (§11)
SM_ERR_INUSE           database still open in this process
SM_ERR_VERSION         home directory format unsupported (§3.5)
SM_ERR_CORRUPT         RocksDB integrity failure (§9.4)
SM_ERR_INVALID         a bad argument, or a call the state forbids
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

`durability_sync` (default on), `group_commit_us` (unused),
`deadlock_ms`, `ssi_max_entries` (unused),
`commit_cache_entries`, `savepoint_undo_bytes`, `ts_lease`,
`idle_close_ms`, and a bulk load's `load_small_bytes`,
`load_sort_bytes` and `load_file_bytes` (`API.md`, Options),
plus pass-through RocksDB options (block cache size, write buffer
size, compaction style). Every column family is compressed
with LZ4, at every level: a block takes less disk, and a read
from disk decompresses it before the block cache holds it, so
a scan of cached blocks pays nothing more
(`fixed_cf_options()`).
A store CF's data blocks are 16 KB (`store_block_size`), as
MyRocks sets them, not RocksDB's 4 KB: a scan looks up a
quarter as many blocks in the block cache, about 19% off a scan
of 566-byte rows, and a point read by key ran no slower in the
benchmark (`bench/`), faster while the cache warms. Fixed CFs
keep 4 KB; they are small and read by key.
Each data block of a store CF carries a hash index
(`kDataBlockBinaryAndHash`): a table of one byte a bucket, from
a key's hash to the restart interval that holds it, so a `Get`
jumps to its interval instead of binary searching the block's
restart points. A scan, which seeks, searches as before. RocksDB
builds the table only for a comparator that says two keys of
different bytes never compare equal, which `wisr.store.v2`
does: a versioned key's userkey is split from its tail by
length alone. Measured on Q7 of the benchmark, a row read by
its key: 0.3 thousand instructions fewer a query. A scan pays
nothing, and the table is a few bytes, as a 16 KB block of
Q7's rows has eight restart intervals. A block of more than 253
restart intervals goes without it.
From the restart point the index lands on, a `Get` reads
entries one by one to its key, each past its neighbour's value:
up to 15 of them with RocksDB's 16 entries an interval. A store
CF restarts every 4 (`store_restart_interval`): measured with
`MultiGet` (`../BATCHREAD.md`) and servers pinned to the same
CPUs, Q7 and Q9 ran 8.5% fewer instructions a query, the
benchmark's sequential scans no more, and its tables grew
under 4%. A restart point at every entry was tried and
dropped: under 1% fewer instructions still, but keys no longer
share a prefix and each entry carries a restart point, so the
narrow table grew 15% and its scans slowed by up to 13%.
Store CFs have Bloom filters, ten bits a key, and `CF:txn`
benefits from a
large block cache since it is small and hot. For store CFs the
prefix extractor (`wisr.prefix.v2`) strips a versioned key's
12-byte tail, so the bloom key is its class and userkey; the
comparator keeps each userkey's versions contiguous, which is
what a prefix extractor requires. A current or old key
(`../VCHAIN.md` §2) is its own prefix, as a `Get` looks it up
whole.

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

Every CF of every instance shares one block cache, the
environment's, sized at `sm1::attach` (`env_options_t`): a
table's blocks compete with every other's for one budget, as
in PostgreSQL's shared buffers, instead of each CF keeping a
32 MB cache of its own. Store CFs are to share one
`WriteBufferManager` too, so total memtable memory is a single
budget rather than `write_buffer_size × stores`; it is not
built. `max_total_wal_size` bounds how
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
src/env.cc              init/attach/detach, root db, db sweep
src/db.cc               mkdb/rmdb, CF setup, CF create/drop, storeid→CF map
src/session.cc          opendb/closedb, the registry of open databases
src/xct.cc              transaction manager, timestamps, sequencer, 2PC
src/version.cc          key/value codec, version comparator, prefix extractor
src/access.cc           read / scan / insert / update / delete, visibility
src/chain.cc            chained stores: codec, chain_ calls, chain_restore,
                        GC's repair (../VCHAIN.md)
src/load.cc             bulk load into an empty store (../BULKLOAD.md)
src/pin.cc              raw pins (../ROW.md §2)
src/lockmgr.cc          locks, write latches, waits-for graph, deadlocks
src/locktab.cc          the lock table alone: ids, modes, holders
src/ssi.cc              SIREAD locks, rw-edges, validation
src/catalog.cc          store catalog: lookup, mkstore/rmstore, sm1::lsstore
src/seq.cc              named sequences: sm1::mkseq, sm1::rmseq, sm1::nextseq
src/gc.cc               compaction filter, watermarks, drop-store purge
src/recover.cc          startup, prepared-transaction restore, orphan CF sweep
src/stats.cc            sm1::approxsize, sm1::probes, sm1::compact
src/memcount.cc         memtable bytes of a store's writes, by key prefix
src/error.cc            set_status_error: a RocksDB status as an error
src/walfs.cc            file system zero-filling each new WAL before
                        RocksDB writes it (WALPREALLOC.md)
test/                   the engine's tests: API, limits, locks, GC,
                        2PC, crash, sequences, logging
mvcc/                   isolation: MVCC.md and one file per scenario
bench/                  the lock table's benchmark (locktab_bench.cc)
example/                small programs against the public API
```

General C and C++ utilities — anything not specific to the
engine — live in level 0, `../sm0`, in `namespace sm0`: the
error state and the log hook, which `sm1.h` brings into
`namespace sm1`, and the codec, defer blocks and
`format_into`, with the vendored doctest every level's tests
use. `src/` holds only the engine.

### 12.4 Build order

1. Codec, catalog, single-threaded read/write with no concurrency —
   `mkdb/open/mkstore/insert/read/scan` under one transaction at a time.
2. Timestamps, commit sequencer, status table, snapshot visibility;
   RC and RR isolation. Crash tests begin here.
3. Row locks (shared and exclusive, later Gray's five modes),
   write-write conflict,
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
  read on the same session releases the pin the old row was
  handed out in, unless it was pinned into the caller's ref
  (§5.3).
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
  victim. At REPEATABLE READ and above `sm1::lock_row` runs
  §5.2's check under the lock, so it fails with `SM_ERR_CONFLICT`
  on a key committed after the caller's snapshot; at read
  committed it does not fail, and the caller reads the row with
  `sm1::read_latest` (§5.4). A savepoint rollback releases an
  explicit lock first taken after the mark but never downgrades
  an upgrade. The whole index-build protocol runs end to end, the
  test modelling a layer's catalog with a schema row: a builder
  taking the schema row exclusive blocks a writer, sees the row
  an earlier writer committed, and the blocked writer then fails,
  retries, and reads the new schema (§5.1). sm3 itself orders DDL
  by table-oid locks (`../sm3/SM3_DESIGN.md` §2, §3).
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
  committed pivot, the partner is the one that fails, and only
  if T3 committed first; a pivot whose predecessor or
  successor aborts commits (§7.3).
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
  commit order. Not built: `test/test_crash.cc` kills a commit
  loop at random points, round after round, and checks the
  same prefix.
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
  serializable at SERIALIZABLE and snapshot-isolated at RR. Not
  built.

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
  gigabytes. `T.locks` holds only explicit locks: a write
  keeps none (§5.1, `WRITELOCK.md`). `T.undo` is kept
  only while a savepoint is set, so **a savepoint costs
  memory in proportion to the work done after it** (§6.5):
  cheap to set, expensive to hold over a large statement.
  `savepoint_undo_bytes` caps it, and past the cap the
  savepoint is lost: a rollback to it becomes an abort.
  `SAVEPOINT.md` designs the fix, rolled-back writes
  marked dead by their `seq`, and it is not built. A bulk
  load into an empty store has a path of its own, which lists
  one entry for the whole load (§6.5, `../BULKLOAD.md`).
- **Every write takes a timestamp.** A version is keyed by a `wts`
  taken under its key's latch (§4.1), so the counter is used
  once per
  row written and not once per transaction, and its lease (§4.2,
  `ts_lease`) runs out sooner; each renewal is a synchronous
  `Put`. `global_ts` is an atomic so the common case takes no
  mutex, but no throughput benchmark has been run, so neither
  cost is measured. A larger `ts_lease` is the first lever.
- **The format changed.** Keying versions by `wts` and moving the
  `xid` into the value bumped `WISR_DB_VERSION` to 5; it is 6
  since the store column families were named `store.<storeid>`.
  A database written in an older format is refused, not
  migrated.
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
