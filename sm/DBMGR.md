# DBMGR: the Database Manager

The Database Manager gives the stack its environments, databases,
sessions and stores. It owns the home directory and its root db.
It opens and closes databases. It lays out each database's column
families and key classes. It makes commits durable through
RocksDB's write-ahead log (WAL), recovers a database at open, and
keeps sm1's store catalog and its DDL. Its code is in `sm1/src/`:
`env.cc`, `env.h`, `db.cc`, `db.h`, `session.cc`, `session.h`,
`catalog.cc`, `walfs.cc`, `walfs.h` and `recover.cc`.

---

## 1. Scope

### 1.1 The decisions of level 1

sm1 is a multi-version (MVCC) storage manager for OLTP over
RocksDB. RocksDB is an ordered, durable, crash-atomic key-value
store and nothing more. Every notion of row version, transaction,
isolation and visibility is sm1's.

| Decision | Choice |
| --- | --- |
| Isolation mechanism | Snapshot isolation everywhere; SERIALIZABLE by SSI, which is optimistic (XCTMGR.md) |
| Uncommitted writes | Written eagerly into RocksDB, and resolved by a commit status table (§6.6) |
| `sm1::prepare` | Real 2PC, with a durable prepare record (XCTMGR.md) |
| Deployment | One process, many threads, one shared RocksDB instance a database |

**In scope.** Stores as ordered maps from key to value; MVCC
reads and scans; read-committed, repeatable-read and serializable
transactions; nested savepoints; two-phase commit; crash
recovery; garbage collection of dead versions.

**Not in scope.** A query language, column types or schemas
(values are opaque byte strings), replication, multi-process or
remote access, and online backup. sm1 has no secondary index:
level 3 builds indexes out of stores. Nothing in the design
forecloses these.

### 1.2 Backward scans: not in scope, ever

Iteration is forward only, in ascending bytewise key order. No
index serves a backward scan: not the clustered primary key, and
not a secondary index a level above builds out of stores.
Descending order is built into the key. An index with inverted
key bytes is scanned forward. The stack keeps no mirror copy of
an index for the other direction (`PART4.md`).

The engine cost was part of the reason:

- A versioned key's versions sort newest first. A backward scan
  meets the oldest version first, and must walk to the start of
  each key's run to pick the visible one.
- It needs `SeekForPrev` to work with the prefix-seek setup of a
  scan (ROWSTORE.md).
- For SSI, the traversed range runs from the last key examined up
  to the end key, which inverts a scan's bookkeeping.

A chained store's rows do not pay the first two: a row's newest
version is one key, and a scan of those keys runs in total order.
The cost stays on versioned keys (secondary index entries, TOAST,
the catalog), and the SSI inversion stays on every scan. A B-tree
Index holds unversioned entries and could step back by a left
link (BTREEINDEX.md).

### 1.3 Assumptions

- A `session_t` is used by one thread at a time. Concurrent use
  of one session from two threads is a caller error.
- A process reaches its databases through an environment,
  `sm1::env_t`, on a home directory (§4).
- Multiple sessions may open the same database. They share one
  `db_t`, and so one RocksDB handle, held in the environment's
  registry of open databases (§5).
- Keys and values are arbitrary byte strings. Keys order bytewise
  (`memcmp`), which is what `sm1::scan` exposes.
- The language is C++, RocksDB's native API, and so is the
  interface, in `namespace sm1`. Only the top level wraps the
  stack in a C API (`DESIGN.md`).

---

## 2. A worked example

The calls below make a home directory, a database and a store,
then survive a crash.

```
sm1::init(home);              // global/, base/, the root db
sm1::attach(home, env);       // sweep base/, make the temporary
                              // instance, recover every database
sm1::mkdb(env, "shop");       // next_dbid 1 -> 2, base/1 built,
                              // then "Dshop" -> 1 in the root db
sm1::opendb(env, "shop", s);  // db_open: one db_t, in env->dbs
sm1::begin(s, level);         // snapshot registered
sm1::mkstore(s, id);          // id = 1000: CF store.1000 made now,
                              // catalog row "R"|be64(1000) written
sm1::insert(s, id, k, v);     // a version in CF store.1000
sm1::commit(s);               // CF:txn record, one WAL sync
sm1::begin(s, level);
sm1::mkstore(s, id2);         // id2 = 1001: CF store.1001
sm1::insert(s, id2, k, v);
                              // the process dies here
sm1::attach(home, env);       // WAL replay; the second transaction
                              // has no status record, so it is
                              // dead; CF store.1001 has no live
                              // catalog row, so it is dropped
```

The name `shop` never reaches the filesystem. The first
transaction's row needs no redo. The second transaction needs no
undo. §9 gives the reasons.

---

## 3. Architecture

```
        ┌──────────────────────────────────────────────────────┐
        │ sm1:: C++ API (session, xct, store, insert/read/scan)│
        └───────────────────────┬──────────────────────────────┘
                                │
   ┌────────────┬───────────────┼───────────────┬──────────────┐
   │            │               │               │              │
┌──┴──────┐ ┌───┴──────┐ ┌──────┴──────┐ ┌──────┴─────┐ ┌──────┴─────┐
│ Store   │ │ Row      │ │ Transaction │ │ Lock       │ │ SSI        │
│ catalog │ │ Store    │ │ Manager     │ │ Manager    │ │ tracker    │
│ (§7)    │ │ read /   │ │ ts, status, │ │ (explicit  │ │ (SIREAD,   │
│         │ │ scan /   │ │ commit seq, │ │  locks,    │ │  rw-edges, │
│         │ │ write    │ │ 2PC         │ │  deadlock) │ │  pivots)   │
└─────────┘ └────┬─────┘ └──────┬──────┘ └────────────┘ └────────────┘
                 │              │
        ┌────────┴──────────────┴────────┐        ┌──────────────────┐
        │  Version codec (key/value fmt) │        │ Garbage          │
        └────────────────┬───────────────┘        │ Collector        │
                         │                        └────────┬─────────┘
                 ┌───────┴────────────────────────────────┴────────┐
                 │                  RocksDB                        │
                 │  CF:store.*  CF:txn   CF:meta   WAL   compaction│
                 └─────────────────────────────────────────────────┘
```

The only durable state is inside RocksDB. Everything else is
volatile, and is rebuilt at open (§9): the locks, the SSI graph,
the active transaction table and the commit cache.

---

## 4. The home directory

### 4.1 Layout

A home directory follows PostgreSQL's layout, one name for each
concept:

```
{homedir}/global/root      the root db              (global/pg_database)
{homedir}/base/{dbid}/     one per database         (base/{oid})
{homedir}/base/pgsql_tmp/  temporary files          (base/pgsql_tmp)
```

`base/pgsql_tmp` is no database. It holds what does not outlive a
session: the host's temporary files and, in `base/pgsql_tmp/sm`,
the instance that keeps temporary stores (§7.8). `sm1::attach`
makes that instance anew, and `sm1::detach` closes it.

### 4.2 The root db

A `dbname` is a caller's string, not a path component. The root
db, `{homedir}/global/root`, maps it to storage. The root db is
an ordinary RocksDB with the default column family, the bytewise
comparator, and none of the machinery below. It is not an
`sm1::session_t` database. It has no `CF:txn`, `CF:meta` or
catalog, and `sm1::opendb` cannot reach it. It is the
`pg_database` of this design. It sits in `global` for the same
reason: it describes the whole home directory.

```
key = "D" | dbname         →  dbid (8B, big-endian)
key = "M" | "next_dbid"    →  8B high-water mark
key = "M" | "version"      →  home directory format version
key = "M" | "wal_prealloc" →  8B size a new log is zero-filled to (§8.4)
```

A database directory has its `dbid` as its decimal name, so
`base` holds no other number. Whatever a caller names a database,
the name never reaches the filesystem, and `base` cannot collide
with `global`. A database name is 1 to `sm1::max_dbname` (63)
bytes, and any byte but NUL. Names compare bytewise and never
fold. An over-long name is an error and not a truncation
(`sm1/API.md`).

One process holds the root db, so it needs no concurrency control
beyond the environment's mutex, `env_t::mu`. Writes to it are
synchronous.

### 4.3 init, attach and detach

`sm1::init` builds a home once in its life: `global`, `base`, the
root db, and in one synced batch the `"version"` key and
`next_dbid` 1. It takes no lock, because `mkdir` settles a race:
the loser of `mkdir(global)` sees `EEXIST`, and `sm1::init` then
fails with `SM_ERR_EXISTS`. The split from `sm1::attach` is
PostgreSQL's, between `initdb` and starting a server.

`sm1::attach`:

1. Install the out-of-memory new-handler, unless the process has
   one (§8.3).
2. Divide `block_cache_bytes`: a quarter to the buffer pool, the
   rest to one LRU block cache (BUFMGR.md).
3. Open the root db, and check its format version.
4. Decide how new logs are written (§8.4).
5. Destroy every numbered directory of `base` that no `"D"` row
   names (§4.5).
6. Make the temporary instance anew, before any database opens.
7. Recover every database the root db names (§9.1), with the
   default options, and close it again.
8. Start the reaper thread (§5.2).

Step 7 is at attach so that no database waits for its first
session to be recovered, and so that a database that cannot be
recovered is found at once: it fails the attach. A prepared
transaction stays in doubt, and the next `sm1::opendb` restores
it again.

Nothing creates a home directory implicitly. `sm1::attach` has no
first-use branch, and a mistyped path is an error
(`SM_ERR_NOTFOUND`) and not a new empty home.

`sm1::detach` refuses with `SM_ERR_INUSE` while sessions are
open. After it returns, the caller may hand the home to another
process, which must not find this one's sessions still writing.
Its `force` flag closes those sessions first, by the path
`sm1::closedb` takes, so a caller that is winding down has one
call that always completes. This matters most after an error,
when the caller may not be able to walk its session list. Force
changes nothing about what closing means: a prepared transaction
stays in doubt, because it is durable and owned by its
coordinator, and not by the process that prepared it. Force does
not preempt other threads: none may be inside a call on those
sessions. Detach then stops the reaper, closes the databases left
idle, and closes the temporary instance and the root db.

### 4.4 One process per home directory

**Warning: one process per home directory is the caller's rule.**
The library takes no lock of its own. A caller that runs several
processes serializes their use of a home itself.

RocksDB still locks each directory it opens, the root db too, so
a second attach usually fails there, but only per database. The
work that spans the home assumes it is alone, and a second
process breaks it: the sweep of `base` at attach, `sm1::mkdb`
choosing a `dbid`, and `sm1::rmdb` checking for open sessions.

### 4.5 The mapping is the truth

A directory that no `"D"` row names is garbage. The two
operations are ordered so that a crash can only leave that:

- `sm1::mkdb` raises `next_dbid` durably, builds
  `{homedir}/base/{dbid}` with `CF:txn`, `CF:meta` and
  `CF:store.0`, and only then commits `"D" | dbname → dbid`.
- `sm1::rmdb` deletes `"D" | dbname`, and only then destroys the
  directory (`rocksdb::DestroyDB`, then whatever else is left).

Neither can leave a name that points at a directory missing or
unbuilt. `sm1::attach` sweeps the other way: it destroys every
entry of `base` whose number is in no `"D"` row, before any
database opens. Entries that are not numbers are left alone. A
`dbid` is never reused, as a storeid is not, so the sweep cannot
take a live database for the remains of a dead one.

This is the pattern of the orphan sweep (§9.1) one level up:
eager cleanup on the normal path, and a startup sweep for the
crash path. It is why a name never reaches the filesystem at
either level: a store is `CF:store.<storeid>`, and a database is
a directory named by `dbid`.

`sm1::rmdb` refuses with `SM_ERR_INUSE` while a session holds the
database open. It closes a database left idle first. Sessions in
other processes are ruled out by §4.4. Neither call builds a path
from `dbname`, so neither can be steered by one.

---

## 5. Databases and sessions

### 5.1 Opening a database

`env_t::dbs` is the registry of open databases, by name, under
`env_t::mu`. A `db_t` is the one shared object of an open
database: its RocksDB handle, its stores, its allocators, and the
state of every other module.

`sm1::opendb` never creates a database. It does these steps:

1. Look for `dbname` in the registry.
2. If it is open and has sessions, join it. The new session's
   options are ignored.
3. If it is open and idle, and the session asks for other
   options, close it. Go on to step 5.
4. If it is open and idle otherwise, join it.
5. Find the `dbid` in the root db, and call `db_open()`.
6. Give the new `db_t` the temporary instance, the buffer pool,
   and its `dbid`. A database opened only to be recovered has
   none of these.
7. Count the session, register it for `sm1::detach`, and register
   it with the Lock Manager's fast path (LOCKMGR.md).

`db_open()` is in §13.3.

### 5.2 Closing a session and a database

`sm1::closedb` does these steps:

1. Close the session's cursors, and let go of its lent row, so no
   iterator outlives the database.
2. Abort an active transaction. A prepared one stays in doubt.
3. Drop the temporary stores the session still holds.
4. Unregister the session.
5. If it was the last session, close the database at once when
   `idle_close_ms` is 0. Else note the time and wake the reaper.

The reaper is one thread an environment. It closes each database
that has had no session for `idle_close_ms`, 60 seconds by
default, and sleeps until the next is due. A session that comes
in meanwhile finds the database as it was: caches warm, and no
reopen to pay. The reaper runs until `sm1::detach`.

`db_t::~db_t` closes a database in this order:

1. Stop the GC worker (GC.md).
2. Free the SSI nodes and the in-doubt transaction objects. Their
   PREPARED records restore them at the next open.
3. Keep each sequence's place (§7.7).
4. Wait out running compactions, whose filters read the handles.
5. Drop the temporary stores a session left, and destroy every
   store's handle, whatever copies are left.
6. If the database ever had a temporary store, pause and resume
   the temporary instance's background work. That waits for a
   compaction there, whose filter reads this database
   (TEMPTABLE.md).
7. Destroy the fixed handles, and close RocksDB.

### 5.3 A session as a database sees it

A `session_t` is one thread's connection to one database, and at
most one transaction. The Transaction Manager owns most of its
fields (XCTMGR.md). These are the Database Manager's:

| Field | What it is |
| --- | --- |
| `env`, `db` | what the session belongs to |
| `created`, `dropped` | the storeids the transaction made and dropped (§7.2) |
| `temp_stores` | the temporary stores the session holds (§7.8) |
| `store_cache` | which storeids it found live, and at which snapshot (§7.4) |
| `store_handles` | its own copies of store handles (§7.6) |
| `scans`, `batch_iters` | what must close before the database does |

`sm1::prepare` moves the transaction into a `session_t` the
database owns, which has `prepared` set and no thread. A restored
in-doubt transaction is one of these too (§9.1). It is a session
to the Lock Manager and the Garbage Collector, and it is in no
session list.

---

## 6. Stores and column families

### 6.1 One column family a store

A database directory is one RocksDB instance. It has these column
families (CFs):

| CF | Comparator | What it holds |
| --- | --- | --- |
| `default` | bytewise | nothing; RocksDB requires it |
| `txn` | bytewise | the commit status table (§6.5) |
| `meta` | bytewise | bootstrap state (§6.7) |
| `store.<storeid>` | `wisr.store.v2` | one store's keys; `store.0` is the catalog (§7.1) |
| `chain.<storeid>` | `wisr.store.v2` | one chained store's keys (ROWSTORE.md) |

A **store** is an ordered map of keys to values, named by a
storeid. A store is exactly one column family, and is never split
over several. The key carries no storeid: the column family is
the store. A `storeid` is a 64-bit id that `sm1::mkstore`
allocates. It appears in decimal in the column family's name.
`db_open()` fails with `SM_ERR_CORRUPT` on a column family of any
other name, and on a database without `txn`, `meta` or `store.0`.

A storeid is never reused, so it never names two stores, and one
`DropColumnFamily` removes a dropped store (§7.3).

### 6.2 Key classes

The first byte of every key of a store is its **class**. sm1 adds
it when it builds a key and strips it when it hands a userkey
out. No caller sees it.

| Class | Constant | The key after it | Ordering |
| --- | --- | --- | --- |
| `0x00` | `key_class_versioned` | userkey, `~wts` (8B), `~seq` (4B) | userkey, then tail |
| `0x01` | `key_class_current` | a chained key's newest version | bytewise |
| `0x02` | `key_class_old` | `be64(id)`: a chained key's older version | bytewise |
| `0x04` | `key_class_page` | a page, unversioned | bytewise |

ROWSTORE.md gives the version layouts of classes `0x00` to
`0x02`, and BTREEINDEX.md the layout of a page. A page is no
version: the compaction filter keeps it as it is (GC.md), and a
bulk load's check that a store is empty and a load's rollback
leave it out (`BULKLOAD.md`).

Pages sort last. So a chained store's last key below class `0x04`
is its highest old id, which `chain_next_old()` reads at open.

### 6.3 The comparator

Store CFs use a custom comparator, `wisr.store.v2`:

1. Compare the class bytes.
2. Within class `0x00`, compare the userkeys (the key less the
   class and the 12-byte tail) with `memcmp`, shorter first on a
   tie. Then compare the tails with `memcmp`.
3. Within any other class, compare bytewise.

RocksDB's default bytewise comparator is wrong here. It compares
a key's tail against the extra bytes of a longer userkey that
begins with it. The versions of `"a"` (`61 FF FF ...`, as `~wts`
of any `wts` below 2^56 starts with `FF`) would sort after those
of `"ab"` (`61 62 ...`). Scans would return `"ab"` before `"a"`,
and a point read could stop early on a foreign key. With
`wisr.store.v2`, a key's versions are contiguous, and userkeys
sort in `memcmp` order.

**Warning: every versioned key given to RocksDB for a store CF
carries the class byte and the full 12-byte tail.** That means
writes, seek targets and iterator bounds alike.

`FindShortestSeparator` and `FindShortSuccessor` are no-ops. That
is safe, and index blocks are a little larger.
`CanKeysWithDifferentByteContentsBeEqual` is false, as a userkey
splits from its tail by length alone: a data block's hash index
needs that (§10.1).

RocksDB persists the comparator's name, and refuses to open a CF
with another. So the name is part of the format: a change of
order means a new name and a rewrite.

### 6.4 The prefix extractor

The extractor, `wisr.prefix.v2`, strips a versioned key's 12-byte
tail, so the bloom key is its class and userkey. The comparator
keeps each userkey's versions contiguous, which a prefix
extractor requires. A current or old key is its own prefix, as a
`Get` looks it up whole.

**Warning: every iterator on a store CF must state its seek
mode.** RocksDB's default is prefix seek, and a scan that
inherits it returns at most one userkey (ROWSTORE.md).

### 6.5 `CF:txn`: the commit status table

```
key   = xid (8B, big-endian)
value = state (1B: PREPARED 1, COMMITTED 2, ABORTED 3)
      | snapshot_ts (8B)
      | commit_ts (8B, zero until committed)
      PREPARED only:
      | prepared_at (8B, epoch seconds)
      | gid (be32 length, bytes)
      | locks (be32 count, then 20B each)
      | created (be32 count, then be64 storeids)
      | dropped (be32 count, then be64 storeids)
```

Only a writing transaction gets a record, and a read-only one
that prepares. A record is written only at prepare, commit or the
abort of a prepared transaction. Nothing is written at
`sm1::begin`. Absence of a record has a meaning (§6.6).

A lock's 20 bytes are its type, its two id words, its mode and
its kind (`lock_put_record()`). The PREPARED fields exist for
recovery:

- `locks` re-acquires the locks a prepared transaction holds,
  each in its mode: those `sm1::lock_row` and `sm1::lock_oid`
  took. A write keeps none (LOCKMGR.md).
- `created` lists the stores it made, so recovery keeps their
  CFs.
- `dropped` lists the stores it dropped, so a commit after a
  restart still queues their CFs for removal.
- `prepared_at` is the wall-clock time of `sm1::prepare`,
  reported by `sm1::list_prepared`. It must be durable, because
  the listing is mostly needed after a restart.

All of these go when the record flips to COMMITTED or ABORTED.
The record is one value, however many locks it lists.

### 6.6 The status resolution rule

Given a version's `xid`, a reader determines the writer's fate in
this order:

0. If the version's header carries a stamped `commit_ts`, it
   committed then. Only the compaction filter writes a stamp, and
   only for committed versions (GC.md). A stamp is authoritative.
   It must be checked first: once the `CF:txn` record is swept,
   it is the only evidence of the commit. The caller checks it,
   and its own xid, before `xct_status()`.
1. If `xid` is in the in-memory active transaction table, the
   writer is in flight: running or prepared. The version is not
   visible, unless it is the reader's own.
2. If `xid` is in the in-memory commit cache, use its
   `commit_ts`.
3. Look up `CF:txn`. COMMITTED gives `commit_ts`. ABORTED is
   dead. PREPARED is in flight.
4. With none of the above, the writer never committed. It is
   dead.

Step 4 makes crash recovery free of an undo pass. A crashed
transaction leaves versions with no status record, and **absence
of a record is the abort**. It holds at runtime too: an ordinary
abort writes no record. A transaction whose commit status write
failed is different: that record may be durable, so the process
aborts and WAL replay judges it (§8.3).

The process is the only one with the database open, so the active
table is exhaustive. Two orderings keep a live transaction from
being misjudged:

- A transaction enters the active table no later than its xid
  allocation, before its first version reaches RocksDB.
- At commit, the status record is written first. Then the xid
  enters the commit cache and leaves the active table. Only then
  is its `commit_ts` published. A reader that misses it in the
  active table finds it in the cache or `CF:txn`. A reader that
  still finds it there cannot hold a snapshot that includes the
  commit.

The commit cache is bounded by `commit_cache_entries`. A commit
makes an entry, and so does a lookup that misses the cache and
finds a COMMITTED record, so a transaction's other versions do
not each pay for the read. Eviction cannot change an answer: the
record was written before the insert, and a sweep deletes it only
once every version it covers is stamped (GC.md). XCTMGR.md gives
the cache's design.

### 6.7 `CF:meta`: bootstrap state

| Key | Value |
| --- | --- |
| `version` | database format version, `WISR_DB_VERSION`, now 6 |
| `next_ts` | durable high-water mark of the timestamp allocator |
| `next_objid` | durable high-water mark of the object-id allocator |
| `"seq." \| be64(seqid)` | one lease for each sequence in use (§7.7) |

XCTMGR.md gives the leases. A storeid and a seqid come from the
object-id allocator. Ids 1 to 999 are reserved for objects a
database is made with, so the first one handed out is 1000
(`WISR_FIRST_OBJID`).

---

## 7. The store catalog and DDL

### 7.1 Store 0

There are no store names. The catalog is `storeid` 0, an ordinary
MVCC store in `CF:store.0`:

```
key = "R" | be64(storeid)  →  { }                         (store)
key = "S" | name           →  { seqid, start, increment } (sequence)
key = "U" | anything       →  anything                    (the level above)
```

The `"U"` rows are the level above's catalog (CATALOG.md). sm1
stores and versions them like any row, and never reads them:
`sm1::lsstore` scans `["R", "S")`, and the orphan sweep keeps a
store only for an `"R"` row. Callers reach store 0 under `"U"`
alone (`sm1::upper_prefix`), so they cannot touch a store's or a
sequence's row. A layer above keeps its catalog there, in a store
that is always present, so it has no store to find.

A store's row carries nothing. That the row exists at a snapshot
is what makes the store exist there.

Big-endian storeids make the catalog's key order numeric. So
`sm1::lsstore` is one scan, and returns storeids in ascending
order, which is creation order.

There is no next-storeid row. Storeids come from the lease in
`CF:meta`. A catalog row updated by every `sm1::mkstore` would
make concurrent creates of different stores contend on one row
and fail with `SM_ERR_CONFLICT`.

### 7.2 mkstore, rmstore and lsstore

Because the catalog is an ordinary store, DDL is transactional
for free. `sm1::mkstore` inserts a catalog row, and
`sm1::rmstore` deletes it. Both roll back with the transaction.
They follow the Row Store's write path.

`sm1::mkstore`:

1. Require an active transaction. Refuse a read-only one, unless
   the store is temporary.
2. Allocate a storeid from the lease.
3. Create the column family, and add the storeid to `created`.
4. For a temporary store, stop here: it has no catalog row.
5. Insert `"R" | be64(storeid)` through `access_write()`.
6. If the insert fails, drop the column family. A retryable
   failure has already aborted the transaction, and that dropped
   it.

`sm1::mkstore` always allocates a fresh storeid, so it cannot
meet a live one, and a dropped storeid is never made again. Its
newest catalog version is a tombstone, and stays one.

`sm1::rmstore`:

1. Require an active transaction. Refuse storeid 0 with
   `SM_ERR_INVALID`.
2. Look the storeid up at the transaction's view. Fail with
   `SM_ERR_NOTFOUND` if it is not live.
3. Refuse a store with a bulk load open into it: the load would
   ingest into a dropped store.
4. For a temporary store, add it to `dropped` and stop. It goes
   at commit.
5. Delete the catalog row, and add the storeid to `dropped`.

`sm1::lsstore` is a scan of the catalog over `["R", "S")`, so
under SERIALIZABLE it registers the whole range as read. A
concurrent `sm1::mkstore` or `sm1::rmstore` of any storeid is an
rw-conflict with it.

### 7.3 Column families are not transactional

**The column family is the one part of DDL outside the
transaction:** `CreateColumnFamily` and `DropColumnFamily` take
effect at once.

- `sm1::mkstore` creates the CF **eagerly**, before the catalog
  row, so the creating transaction can write into it at once.
  Nobody else can reach it until the row commits.
- If the transaction aborts or rolls back past the
  `sm1::mkstore`, it drops the CF itself (XCTMGR.md). If it
  crashes, the orphan sweep drops it (§9.1).
- `sm1::rmstore` does not drop the CF at commit. A concurrent
  snapshot may still legitimately read the store through the same
  storeid, while newer snapshots find no catalog row. The Garbage
  Collector removes the CF once the watermark passes the drop
  (GC.md).

A storeid that a caller could choose would have to be refused
until the old CF was gone, or else name two stores at once. That
is why storeids are allocated and never reused.

A CF's creation is durable in RocksDB's MANIFEST before
`sm1::mkstore` writes its catalog row, so a committed catalog row
never lacks its CF.

### 7.4 The store cache

Every data call looks its storeid up first. A lookup that read
`CF:store.0` each time would build a second iterator for every
point read, about a third of its cost. So a session caches which
storeids are live (`session_t::store_cache`, `catalog_lookup()`).

The answer depends on the snapshot. After a committed
`sm1::rmstore`, a transaction with an older snapshot must still
find the store. A cache invalidated at DDL commit would hide it.
So the cache is checked by timestamp:

- `db_t::last_ddl_ts` is the largest `commit_ts` of a transaction
  that made or dropped a store: its `created` or `dropped` was
  not empty. A temporary store counts here too. Commit raises it
  before it publishes that `commit_ts`, so a snapshot that
  includes a DDL commit always sees it. A restored prepared
  transaction has both lists from its record. A commit that
  writes only `"U"` or `"S"` rows leaves it alone, as no store
  lookup reads them.
- Each entry records the snapshot it was read at, the transaction
  that read it (`begin_seq`), and whether the store was live. A
  hit is used in two cases. One is that the lookup is at that
  very snapshot, whatever committed since. The other is that
  `last_ddl_ts` is at or below both that snapshot and the
  lookup's: no DDL committed between the two, so the answer is
  the same at both. Otherwise the lookup reads `CF:store.0` and
  replaces the entry.
- Without the first case, a transaction older than the last DDL,
  anyone's, would read `CF:store.0` at each lookup for the rest
  of its life.
- A transaction that has made or dropped a store neither uses nor
  fills the cache. Its lookups see its own uncommitted catalog
  versions, which no entry stands for. That covers what eviction
  at `sm1::mkstore` and `sm1::rmstore` would do, and a rollback
  to a savepoint too.
- The cache starts over at 4096 entries (`store_cache_max`).

The comparison is against both snapshots because a session's
snapshots do not only move forward. Under read committed, a
lookup at the begin snapshot can follow one at a newer statement
snapshot (XCTMGR.md).

A temporary store has no row. `catalog_lookup()` answers from the
session: live if the session holds it and has not dropped it. To
any other session the storeid names nothing.

### 7.5 A lookup under SERIALIZABLE

Under SERIALIZABLE a lookup is also a read for SSI. That a
storeid names a live store is something the transaction observed,
and a concurrent `sm1::rmstore` writes the catalog key it depends
on. Without an rw-edge for it, DDL admits write skew:

```
T1: sm1::read(S, x)             T2: sm1::read(R, k)    # looks up R
T1: sm1::rmstore(R)             T2: sm1::update(S, x)
```

`T1 ──rw──▶ T2` comes from `x`. `T2 ──rw──▶ T1` exists only if
T2's lookup counts as a read of `"R" | be64(R)`. So under
SERIALIZABLE:

- A lookup is a point read of the catalog key. The SIREAD lock on
  `"R" | be64(storeid)` in `store.0` is taken first, whether or
  not the storeid is found. Versions of concurrent writers it
  steps past run `on_skip` (XCTMGR.md).
- A cache hit does not stand in for that read. An in-flight
  `sm1::rmstore` has not raised `last_ddl_ts`, so a hit is valid
  by §7.4. It steps past nothing, and the dropper's `on_write`
  ran before any SIREAD existed: the edge is missed. So a
  serializable lookup uses a hit only if the entry's `begin_seq`
  is the current transaction's. A read-only transaction has no
  xid, which is why the entry keeps `begin_seq`. Its SIREAD
  catches every later writer of the storeid (scenario c35,
  `sm1/mvcc`).

### 7.6 Store handles

`db_t::stores` maps a storeid to its `store_t`, under `store_mu`,
a reader-writer lock, because stores are made and dropped while
others read. There is one map for each open database. A `store_t`
holds the RocksDB instance, the CF handle, the owner of a
temporary store, the chained flag, and four shared pointers:
`mem`, `files`, `next_old` and `hold`.

`hold` owns the handle (`cf_owner_t`). The last copy of a
`store_t` destroys it, so a copy taken before a drop stays good
after it. `db_t::~db_t` destroys every handle, copies left or
not, because RocksDB needs them gone before it closes.

A copy of a `store_t` counts up those shared pointers. With 16
sessions reading by key, those atomics and `store_mu`'s lock word
took 37% of the samples: a read waited on a cache line, not on
work. So a session keeps its own handles:

- **The database counts changes to its map.**
  `db_t::stores_version`, an atomic, rises by one with release
  order in `db_create_store_cf()`, and in `db_drop_store_cf()`
  before the handle goes. A reader loads it with acquire order,
  and takes no lock.
- **A session keeps the handles it read.**
  `session_t::store_handles` maps a storeid to a `store_t` and
  the version it was read at. `session_store()` returns a
  reference to the session's entry when that version is current.
  Else it reads the version, then the entry under `store_mu`
  shared, and keeps both. A store not there gives a `store_t`
  with no CF.
- **The read path makes no copy.** `chained_store()`, its
  callers, `catalog_lookup()` and `store_of()` hold the handle by
  `const store_t &`.
- **An entry lives as long as the session's map holds it.** It is
  refreshed in place, and never erased while the session runs a
  call, so a reference stays good across a nested lookup. At a
  transaction's end, the entries of an older version are erased,
  so a session that made and dropped many temporary tables keeps
  none of them.
- **A dropped store is no more dangerous than a copy.** What
  keeps a store from being dropped under a reader is its table's
  lock, and its catalog row read at the reader's view. A lookup
  after the drop finds it gone, as the version rises first.

The GC worker, recovery and the sweep read `db_t::stores` under
`store_mu`. Bulk load and statistics call `db_store()`, one
lookup a call.

`db_read_enter()`, 1.5% of those samples, and RocksDB's own
atomics in a read are as they were. They wait for a measurement.

### 7.7 Sequences

SEQUENCE.md covers sequences above sm1. sm1's part is this.

A sequence is a catalog row of its own, `"S" | name`, so
`sm1::mkseq` and `sm1::rmseq` are transactional as `sm1::mkstore`
is, down to the wait that makes two concurrent creates of one
name conflict (`SM_ERR_EXISTS`). The row holds
`{ seqid, start, increment }`. The `seqid` comes from the
object-id allocator, so a name created again after a drop names a
different sequence.

The row does not hold the position. The values are not
transactional: `sm1::nextseq` takes no locks, never blocks, and
an abort gives nothing back. An MVCC row would be wrong twice:
its writer would lock the sequence for the rest of its
transaction, and a rollback would hand the same values out again.
The position lives in `CF:meta` under a lease, where nothing is
undone. The `seqid` keys it and not the name, so a sequence
created again under an old name starts at its own `start`.

- `sm1::nextseq` reads the definition at the transaction's view
  when there is one, so a transaction can use a sequence it just
  created. With no transaction it reads at `visible_ts`.
- That is a catalog read each call. `sm1::seqref` makes it once,
  into a `seqref_t` that `sm1::nextseq` then draws from. It is
  good while the sequence is not dropped, which its holder sees
  to. sm2 draws a table's `_id`s so.
- A lease is a block of `lease` steps, persisted before any value
  under it is handed out. A crash skips the rest of the block.
- A clean close writes each sequence's next value as its lease,
  in one synced batch (`keep_seq_positions()`), so a clean reopen
  skips nothing.
- The last value is the largest one at or below `UINT64_MAX`. A
  further call fails with `SM_ERR_INVALID`, rather than wrapping
  onto values already handed out.
- A committed `sm1::rmseq` deletes the lease, best effort.
  Nothing reads it once the row is gone. A lease left by a crash,
  by an abort after `sm1::nextseq`, or by a prepared
  transaction's drop is garbage and not a hazard, as the name's
  next incarnation has a new `seqid`.

### 7.8 Temporary stores

**A temporary table and its indexes are one temporary store, and
a store is temporary through its column family.** The property
belongs to the whole store and to no key in it. sm1 does not know
what a store holds. Above it, sm3 puts a table's rows, its
indexes and its TOAST data in one store. So no marking exists at
any level for a row or a key.

A temporary store is a column family of one unlogged RocksDB
instance that is kept apart from every database,
`base/pgsql_tmp/sm`. Its name there is `<dbid>.<storeid>`,
because two databases hand out the same storeids. `sm1::mkstore`
makes one when asked, with no row in store 0. It is its session's
alone, and is dropped when the session closes. TEMPTABLE.md is
the design.

---

## 8. The WAL

### 8.1 Where durability comes from

Durability is RocksDB's WAL. Each `WriteBatch` is atomic, and is
durable once a sync of the WAL covers it: its own `sync = true`,
or a later `SyncWAL()`. sm1 adds no log of its own. With eager
writes, one thing must be atomic and durable at commit: **the
status record**.

| Write | Synced |
| --- | --- |
| a version | no |
| a COMMITTED record | no, then a group sync past it (§8.2) |
| a PREPARED record | `sync = true` |
| a prepared transaction's ABORTED record | `sync = true` |
| a lease in `CF:meta` | `sync = true`, before any value under it |
| the seed of `CF:meta` at `sm1::mkdb` | `sync = true` |
| every write to the root db | `sync = true` |
| a temporary store's write | not logged |

XCTMGR.md gives the commit sequence, and its commit point. A
commit calls `db_sync_wal()` after it writes its record, unless
`durability_sync` is 0. That setting trades a bounded window of
committed-transaction loss for throughput. The database stays
crash-consistent, and is no longer crash-durable. Isolation does
not weaken.

### 8.2 Group sync

A commit's sync runs outside RocksDB's write queue.

**Why.** Each transaction makes two writes through RocksDB's one
write queue: the version and the record. RocksDB forms a group of
the waiting writers, and its leader writes the WAL for all of
them. A group led by a synced write syncs the WAL once, and takes
in the unsynced writes queued behind it
(`WriteThread::EnterAsBatchGroupLeader`). So an insert's version
waits for a sync it does not need. At 16 clients, about 85% of a
session's samples waited in the queue. A PostgreSQL insert writes
its WAL record to a buffer and does not wait. Only the commit
waits, in `XLogFlush()`, and one backend's flush covers every
record before it.

**Design.** The record is written unsynced. The commit then waits
for a sync past its record, as `XLogFlush()` does.
`db_sync_wal()` does these steps:

1. Read `GetLatestSequenceNumber()`: the target. RocksDB sets it
   after a group's WAL write and memtable insert, before its
   writers return, so it covers the record.
2. Lock `sync_mu`.
3. If `synced_seq` is at or past the target, return: another
   commit's sync covered this record.
4. If another commit leads a sync, join the line, unlock, and
   sleep on a semaphore of its own. Woken as covered, return.
   Woken as the next leader, go to step 6.
5. Become the leader, and unlock.
6. Read `GetLatestSequenceNumber()` again, as `upto`. Every write
   at or below `upto` is in the WAL file now.
7. Call `FlushWAL(true)`, outside the write queue. Writers go on
   meanwhile. Under `manual_wal_flush` it first writes RocksDB's
   WAL buffer to the file, which `SyncWAL()` alone does not. Then
   it calls `SyncWAL()`, which syncs every log up to the current
   one.
8. If the sync failed, abort the process (§8.3).
9. Lock `sync_mu`, and set `synced_seq` to `upto`. Take the
   commits at or below `upto` out of the line. Hand the next sync
   to the oldest commit left, if any: `syncing` stays set for it.
10. Unlock. Wake the next leader first, then the covered commits.

Rules of this design:

- **The handoff wakes one leader.** PostgreSQL's
  `LWLockRelease()` also wakes each backend on a semaphore of its
  own. A woken commit takes no lock.
- **The xid stays in the active table until the sync is done.**
  So every reader, the compaction filter too, finds it in flight
  before it reaches `CF:txn` (`xct_status()` step 1), and no
  reader takes the unsynced record for a commit.
- **Only the commit record changes.** The PREPARED record and a
  prepared transaction's ABORTED record keep `sync = true`. They
  are rare, and a group led by one of them syncs inside the
  queue.
- **The wait is not cut short by an interrupt,** unlike every
  other wait in sm (`CODE.md`). The record is written, so an
  interrupted commit has an unknown outcome, and only an abort
  could settle it. The wait is bounded by two syncs: the one that
  runs, and the next. Each finishes or aborts the process.
- **A leader does not wait for followers.** The waiters behind a
  sync make the next group. So `group_commit_us` is accepted and
  unused.

### 8.3 A failed write is fatal

**Warning: a failed status record write, or a failed WAL sync,
aborts the process (`sm0::fatal()`, `CODE.md`).** The engine
keeps no failed state.

- A failed write or sync leaves the outcome unknown. The record
  may or may not have reached the WAL: a sync can fail after the
  bytes are written.
- A second sync proves nothing. After a failed fsync, Linux may
  drop the dirty pages and clear the error, as PostgreSQL found
  in 2018.
- A write that RocksDB refuses before it reaches the WAL aborts
  too. RocksDB refuses writes after a background error, such as a
  full disk in a flush. The status does not tell this refusal
  from a failure after the write. PostgreSQL does the same: a
  failed WAL write is a PANIC, a full disk included. While the
  disk stays full, each restart aborts again at its first commit.
- The rule covers every status record: COMMITTED, PREPARED and
  ABORTED.

So no commit that the failed write or sync was to cover returns,
and none is acknowledged. The next attach's WAL replay decides
each transaction from what the WAL holds. A record that reached
the OS survives the abort, and is committed.

Running out of memory is fatal too. `sm1::attach` installs a
new-handler, unless the process has one. A `std::bad_alloc`
thrown from inside RocksDB leaves it unreliable: later writes
hang, a close can crash, and a record's fate is unknown. No
caller can tell that case from a harmless one. So the handler
reports on stderr and the log hook, and aborts. WAL replay
recovers every committed transaction. A process's own handler is
kept, and one that throws is unsupported.

### 8.4 Zero-filled logs

RocksDB appends to its log, so every sync also commits the file's
new size to the file system's journal. On WSL2 with ext4:

| 100 bytes, then `fdatasync` | a sync |
| --- | --- |
| appended, the file growing | 2,776 µs |
| written over bytes the file has | 545 µs |

PostgreSQL zero-fills its WAL segments before use, so its syncs
never grow a file. That was the gap between one client's update
in digres, 3.2 ms, and in PostgreSQL, 1.4 ms. Sharing did not
explain it: at 16 clients digres shared a sync among 2.6 commits,
and PostgreSQL among 2.9. So a home where it pays writes each new
log over zeros.

- **A file system wrapper** (`walfs_t`, `sm1/src/walfs.cc`) is a
  `rocksdb::FileSystemWrapper` over the default file system,
  given to each database's instance through its `Env`
  (`env_t::wal_env`, `db_open()`). It changes one call:
  `NewWritableFile` of a log, `NNNNNN.log`. Every other file and
  call passes through.
- **A new log is zero-filled first.** The wrapper writes
  `wal_prealloc_bytes` (64 MB) of zeros and syncs them once.
  RocksDB then writes from offset 0 over the zeros, and its syncs
  leave the size alone. A log that outgrows the size grows as
  usual. At a clean close RocksDB truncates the file to what it
  wrote.
- **Recovery reads a zeroed tail as the end.** In
  `db/log_reader.cc`, a header of zeros is a record of type 0 and
  length 0, skipped without a corruption to the end of the file.
- **A torn last record** was never synced, and its commit was
  never acknowledged. It reads as a bad checksum, where an
  appended log reads as a short file. Under
  `kPointInTimeRecovery`, RocksDB's default and the mode sm1
  runs, both end the replay there, with every record before it.
- **The zeroing is off the write path.** RocksDB makes a new log
  in the write that switches memtables. So a thread (`zero_main`)
  zeroes the next log ahead, as a spare, `sm_walspare`, beside
  the log that called for it. There is one spare a home, and any
  database's next log may take it, by `ReuseWritableFile`, a
  rename. A log with no spare ready waits for the one being made.
  With none being made it is zeroed in place. If the rename
  fails, as when the spare's database was dropped, the log is
  made the long way.
- **A directory's first log starts no spare,** so a database that
  never switches logs writes no second 64 MB. It removes a spare
  a crash left there. RocksDB ignores such a file until then.
  "First" counts from the attach: attach's recovery open is a
  directory's first, or `sm1::mkdb`'s for a new database.
- **The temporary instance and the root db are not wrapped.** The
  temporary instance writes no log.

**Who decides.** Only WSL zero-fills (`wal_on_wsl()`, from
`/proc/sys/kernel/osrelease`). Elsewhere a tree's load paid 64 MB
a log for a commit sync no one had measured to need it. Off WSL a
home keeps 0, with nothing timed.

On WSL, sm decides once for the home, at its first attach, which
is digres's `initdb`. There is no option and no setting, because
what a sync of a growing file costs is the file system's
business:

1. Read `Mwal_prealloc` from the root db. If it is there, use it.
2. Time 8 syncs after a 100-byte append and 8 after a 100-byte
   overwrite, in a scratch file in `base/` (`wal_probe()`).
3. Zero-fill if the median append sync costs at least 1.5 times
   the median overwrite sync (`wal_prealloc_pays()`).
4. Keep the answer, 64 MB or 0, in the root db.

A probe that fails zero-fills nothing and keeps nothing, so the
next attach probes again. A home moved to another file system
keeps its first answer.

---

## 9. Recovery

There is no redo pass and no undo pass. RocksDB's WAL replay
restores the key-value store to the last durable write.
Everything else follows from the status resolution rule (§6.6).

### 9.1 Startup

`db_open()` runs at `sm1::attach` for every database, and again
at the first `sm1::opendb`:

1. List the column families, and open RocksDB with every one.
   This replays the WAL.
2. Read `CF:meta`: the format version, the persisted `next_ts`,
   and `next_objid`.
3. Set `global_ts` to `next_ts + 1`, and `visible_ts` to
   `next_ts` (XCTMGR.md).
4. Scan `CF:txn` for PREPARED records. COMMITTED and ABORTED
   records need no action.
5. For each PREPARED record, restore the transaction:
   1. Put its xid in the active table.
   2. Mark it restored, so a serializable reader that skips its
      versions is doomed (XCTMGR.md). Its `created` and `dropped`
      come from the record.
   3. Re-acquire the locks in its record, each in its mode. Its
      writes need none: their versions are in flight again, so a
      writer waits for it as before.
   4. Pin `gc_watermark` at its `snapshot_ts`.
   5. Register it under its gid.
6. Run the orphan sweep. Read the catalog, and drop every store
   CF, `store.<storeid>` or `chain.<storeid>`, with neither a
   live catalog row nor an entry in a prepared record's
   `created`. A live row is a name's newest committed version,
   and not a tombstone.
7. Set `gc_ready`, and start the GC worker (GC.md).
8. Open for sessions.

The sweep must follow the restore: it needs the prepared records,
and it must find their versions in flight. Both run before the
compaction filter may collect.

Step 6 catches the column families that eager cleanup never
reached: a crash between `CreateColumnFamily` and commit, and a
committed drop whose `DropColumnFamily` had not run.

No abort step exists. A transaction that was running at the crash
is absent from the fresh active table and has no status record,
so §6.6 step 4 declares it dead the first time anyone looks at
one of its versions. The next compaction drops its versions. The
GC worker puts back a chained key's (GC.md). No scan of the data
is needed. Recovery time does not depend on the size of the
crashed transactions.

### 9.2 What each state becomes

| At the crash | After the restart |
| --- | --- |
| active, never wrote | nothing to do |
| active with eager writes, no record | dead; GC drops its versions |
| active, made a store | dead; the orphan sweep drops its CF |
| wrote its COMMITTED record, WAL synced | committed, visible |
| in the WAL write when the power was cut | atomic in RocksDB; with no record in the WAL, dead |
| wrote its record, sync not done, process died | the record is in the OS's page cache: committed; the client was not told |
| wrote its record, sync not done, power lost | the record may be lost: dead if so; the client was not told |
| prepared | restored in doubt, locks re-acquired, awaits its coordinator |
| wrote ABORTED | aborted |

### 9.3 In-doubt transactions

A restored prepared transaction blocks writers on its keys, as
its versions are in flight, and it pins GC. `sm1::list_prepared`
shows them by `gid`, so the coordinator can finish them. An
operator's escape hatch, `sm1::abort_prepared`, is required: an
abandoned prepared transaction otherwise blocks GC forever. This
is the hazard of any 2PC system, and it is exposed and not
hidden.

### 9.4 Torn or corrupt state

RocksDB checksums every block and WAL record. A checksum failure
at open is `SM_ERR_CORRUPT`. The design attempts no repair beyond
RocksDB's own truncation of a torn tail record. That is safe here
because a torn tail can only lose commit records at the very end
of the log, and losing an unacknowledged commit record cannot be
told from the transaction never having committed.

---

## 10. Configuration

### 10.1 Column family options

`fixed_cf_options()` and `store_cf_options()` set these. The
options are fixed in code, and no caller sets them.

| Option | Applies to | Value | Why |
| --- | --- | --- | --- |
| compression | every CF, every level | LZ4 | a block takes less disk; a read from disk decompresses it before the block cache holds it, so a scan of cached blocks pays nothing more |
| block cache | every CF of every instance | the environment's one cache | a table's blocks compete with every other's for one budget, as in PostgreSQL's shared buffers |
| `block_size` | store CFs | 16 KB (`store_block_size`), as MyRocks; fixed CFs keep 4 KB | a scan looks up a quarter as many blocks, about 19% off a scan of 566-byte rows; a point read by key was no slower |
| `data_block_index_type` | store CFs | `kDataBlockBinaryAndHash` | a `Get` jumps to its restart interval by a table of one byte a bucket, and does not search the block's restart points; 0.3 thousand instructions fewer a query on a row read by key; a scan pays nothing |
| `block_restart_interval` | store CFs | 4 (`store_restart_interval`) | a `Get` reads entries one by one from the restart point; 8.5% fewer instructions a query measured with `MultiGet`; tables grew under 4% |
| bloom filter | store CFs | 10 bits a key, on the prefix; `whole_key_filtering` off | whole version keys are never looked up |
| `prefix_extractor` | store CFs | `wisr.prefix.v2` (§6.4) | |
| `comparator` | store CFs | `wisr.store.v2` (§6.3) | |
| `compaction_filter_factory` | store CFs | `gc_filter_factory()` (GC.md) | |
| `memtable_max_range_deletions` | store CFs | 1000 (`store_max_range_deletions`) | GC.md; needs RocksDB 8.9 or later |
| `row_cache` | store CFs | none, and it must stay none | `DeleteRange` returns `NotSupported` with one (GC.md) |

A data block of more than 253 restart intervals goes without the
hash index. `CF:txn` benefits from a large block cache, as it is
small and hot. Fixed CFs are small and read by key.

Store CFs are to share one `WriteBufferManager`, so total
memtable memory is a single budget and not `write_buffer_size`
times the stores. It is not built.

### 10.2 Environment options

`sm_env_options_t` (`include/sm_options.h`), taken by
`sm1::attach`:

| Field | Default | Meaning |
| --- | --- | --- |
| `block_cache_bytes` | 128 MB | the budget of the block cache and the buffer pool; at least 1 |
| `max_sessions` | 0 | if above 0, the pool has frames enough for each session (BUFMGR.md) |

### 10.3 Database options

`sm_options_t`, taken by `sm1::mkdb` and the `sm1::opendb` that
first opens a database. `sm1::mkdb` uses only `rocksdb_options`.
Recovery at attach uses the defaults.

| Field | Default | Meaning |
| --- | --- | --- |
| `durability_sync` | 1 | §8.1 |
| `group_commit_us` | 200 | unused (§8.2) |
| `deadlock_ms` | 50 | LOCKMGR.md |
| `ssi_max_entries` | 1 << 20 | unused |
| `ts_lease` | 1000000 | the size of each lease; a larger one is the first lever on the cost of a renewal (XCTMGR.md) |
| `rocksdb_options` | NULL | a RocksDB `DBOptions` string, such as `manual_wal_flush=true`; column family options do not pass |
| `commit_cache_entries` | 65536 | slots, rounded up to even; at least 1 (XCTMGR.md) |
| `savepoint_undo_bytes` | 64 MB | XCTMGR.md |
| `idle_close_ms` | 60000 | §5.2 |
| `load_small_bytes`, `load_sort_bytes`, `load_file_bytes` | 4 MB, 256 MB, 256 MB | `BULKLOAD.md` |

`db_open()` refuses a value out of range with `SM_ERR_INVALID`.

`max_total_wal_size` bounds how long a rarely-written CF can hold
old WAL files alive. sm1 does not set it. A host sets it in
`rocksdb_options`.

### 10.4 Logging

wisr writes no log of its own. A host installs a process-wide
hook with `sm1::set_log` (`sm1/API.md`). The engine reports
through it only what no caller can learn from a return value:

- failures on the GC thread;
- a fatal failure (§8.3);
- versions the compaction filter kept out of doubt;
- a `CF:txn` sweep abandoned or completed;
- a snapshot that holds back `gc_watermark` for over a minute;
- what recovery and `sm1::attach` restore or clean up.

---

## 11. Errors

`include/sm_error.h` defines the codes, with stable values, and
`sm1/API.md` documents them. sm1 raises these:

| Module | Codes |
| --- | --- |
| Database Manager | `SM_ERR_NOTFOUND` (store, database or home); `SM_ERR_EXISTS` (home, database, sequence or gid); `SM_ERR_INUSE`; `SM_ERR_VERSION`; `SM_ERR_CORRUPT` (§9.4, or a malformed record) |
| Row Store | `SM_ERR_CONFLICT`, `SM_ERR_UPDATED`, `SM_ERR_NOTFOUND` (key), `SM_ERR_DUPKEY`, `SM_ERR_MOVED` |
| Transaction Manager | `SM_ERR_SERIALIZATION`, `SM_ERR_READONLY`, `SM_ERR_NOSAVEPOINT`, `SM_ERR_UNDOLIMIT`, `SM_ERR_DOOMED`, `SM_ERR_SCANCLOSED`, `SM_ERR_SELFMODIFIED`, `SM_ERR_NOTRANS`, `SM_ERR_INTRANS`, `SM_ERR_PREPARED` |
| Lock Manager | `SM_ERR_DEADLOCK`, `SM_ERR_BLOCKED`, `SM_ERR_INTERRUPTED` |
| any | `SM_ERR_INVALID` (a bad argument, or a call the state forbids); `SM_ERR_IO`, `SM_ERR_NOMEM`, `SM_ERR_PERMISSION`, `SM_ERR_DISKFULL`, from a RocksDB status (`set_status_error()`) or `errno` |

`SM_ERR_CONFLICT`, `SM_ERR_SERIALIZATION` and `SM_ERR_DEADLOCK`
are the three **retryable** errors. On all three the engine has
already aborted the transaction. The caller must not call
`sm1::abort`, and retries by beginning a fresh transaction. Under
`sm1::xctlevel_t::serializable` they are a normal outcome.

`SM_ERR_UPDATED`, `SM_ERR_MOVED`, `SM_ERR_BLOCKED`,
`SM_ERR_INTERRUPTED`, `SM_ERR_READONLY`, `SM_ERR_SCANCLOSED` and
`SM_ERR_SELFMODIFIED` do not abort.

`sm1::errnum()` and `sm1::errmsg()` read the code and the message
of the last error on the calling thread. Two consequences follow:

- A successful call leaves both untouched. So every path that
  returns an error indication must set them first. Nothing may
  rely on a stale value, and nothing may clear them on success.
- `sm1::errmsg()` is never NULL, and its result stays valid until
  the next failing call on that thread. So the engine owns the
  message in thread-local storage. Lazy formatting is worthwhile:
  under SSI a retry loop can raise `SM_ERR_SERIALIZATION`
  constantly and never read the text.

---

## 12. Compaction on another machine

Status: research, not built. Garbage collection rides on RocksDB
compaction (GC.md), and compaction is the engine's most expensive
background work. It can move to another machine in principle, and
RocksDB supports that. The filter as written cannot be shipped.
The fix is a contained change to how its inputs are supplied.

### 12.1 What RocksDB provides

`CompactionService`, in the RocksDB 8.9 this builds against: the
primary hands a serialized job to `CompactionService::StartV2()`,
the remote side runs `DB::OpenAndCompact()`, a read-only
operation that writes to an output directory, and the primary
installs the resulting SST files. On shared storage the worker
opens the immutable inputs in place. Otherwise they are copied to
it, and the outputs come back.

### 12.2 Why the filter cannot be shipped as it stands

The worker supplies its own filter:
`CompactionServiceOptionsOverride` carries
`compaction_filter_factory`. That suits a filter that is a pure
function of its bytes. `gc_filter_t` is not one. Each `Filter()`
call reaches into the primary's memory, and so does
`filter_chained()`:

| What it needs | Where that lives |
| --- | --- |
| `xct_status(db, xid, &commit_ts)` | the active table, the commit cache, and `CF:txn` |
| the watermark it judges against | the registered snapshots |
| `db->purges`, `gc_cv` | the in-process purge queue |
| `db->repairs` (`chain_report()`) | the in-process queue of chained keys to put back |
| `db->gc_filter_errors` | the counter the `CF:txn` sweep consults |

The first two are inputs the worker lacks. The last three are
side effects that travel outward. A remote worker cannot perform
them, and would have to return them.

### 12.3 Why it is tractable

The filter's inputs are monotone, which makes them safe to
precompute:

- A transaction's status moves one way. Once COMMITTED, always
  COMMITTED, and `commit_ts` never changes.
- A watermark lower than the true one is always safe. It collects
  less, and never wrongly.
- Keeping a version is always safe. Only dropping and stamping
  must be right.

So a job can carry its own answers:

1. **Resolve the xids up front.** Collect the distinct xids in
   the job's input files, resolve each on the primary, and send
   an `xid → (status, commit_ts)` map with the job. The map is
   small next to the data. It turns `xct_status()` into a lookup,
   and removes the need for a worker to read a moving `CF:txn`
   consistently.
2. **Freeze the watermark into the job.** It is already a
   constructor argument of `gc_filter_t`.
3. **Return the side effects.** The queued purges, the chained
   keys to repair and the doubt count become part of the job's
   result. The primary applies them when it installs the output.
   The `DeleteRange` stays on the primary in any case: it is a
   write.

Step 1 is worth doing with no remote worker in sight. It replaces
a status resolution for each key with one batch for each
compaction.

### 12.4 What does not fit

The `CF:txn` sweep is the pass one would most want to offload,
and it does not fit. It is a forced full compaction of every
store. Its correctness rests on state a worker lacks: the
watermark at the pass's start, the read epochs, and the verdict
on whether any version stayed unstamped. It needs its own
argument before it could move.

### 12.5 When it would pay

Offloading trades local CPU and I/O for network I/O and
coordination. It pays when the primary is CPU-bound on compaction
and the storage is shared. It loses when fragments cross the
network to relieve a machine that was not the bottleneck. Which
of these describes wisr is not known, as no benchmark measures
it. No work here should start before that number exists.

### 12.6 What it does not cost

Remote compaction distributes a stateless computation over
immutable inputs. That is a different problem from letting
several processes use one database, which `sm1/SM1_OBJECTIVE.md`
rules out and `PART4.md` shows to be the hard blocker for
PostgreSQL. It needs no part of that decision reopened.

---

## 13. Implementation

### 13.1 Source layout of sm1

Paths are relative to `sm1/`.

```
include/sm1.h           public C++ API, namespace sm1
src/env.cc              init/attach/detach, root db, db sweep, reaper
src/db.cc               mkdb/rmdb, CF setup, CF create/drop, storeid→CF map,
                        allocators, sequencer, db_sync_wal
src/session.cc          opendb/closedb, the registry of open databases
src/xct.cc              Transaction Manager: timestamps, status, commit, 2PC
src/version.cc          key/value codec, comparator, prefix extractor
src/access.cc           read / scan / insert / update / delete, visibility
src/chain.cc            chained stores: codec, chain_ calls, chain_restore,
                        the GC worker's repair
src/load.cc             bulk load into an empty store (BULKLOAD.md)
src/pin.cc              raw pins (ROWSTORE.md)
src/page.cc             pages: pin, write, drop (BUFMGR.md)
src/bufpool.cc          the buffer pool alone: frames, pins, the clock
src/lockmgr.cc          locks, write latches, waits-for graph, deadlocks
src/locktab.cc          the lock table alone: ids, modes, holders
src/ssi.cc              SIREAD locks, rw-edges, validation
src/catalog.cc          store catalog: lookup, mkstore/rmstore, lsstore
src/seq.cc              named sequences: mkseq, rmseq, nextseq
src/gc.cc               compaction filter, watermark, purge, drops, sweep
src/recover.cc          prepared-transaction restore, orphan CF sweep
src/stats.cc            approxsize, probes, compact
src/memcount.cc         memtable bytes of a store's writes, by key prefix
src/error.cc            set_status_error: a RocksDB status as an error
src/walfs.cc            the file system that zero-fills each new WAL
test/                   the engine's tests
mvcc/                   isolation: MVCC.md and one file per scenario
bench/                  the lock table's benchmark
example/                small programs against the public API
```

General C and C++ utilities, anything not specific to the engine,
live in level 0, `sm0/`, in `namespace sm0`: the error state and
the log hook, which `sm1.h` brings into `namespace sm1`, and the
codec, defer blocks, `format_into` and the vendored doctest.
`src/` holds only the engine.

### 13.2 What `db_t` holds

`db_t` (`sm1/src/db.h`) is one struct for all modules. These
parts are the Database Manager's:

| Fields | Guarded by | Use |
| --- | --- | --- |
| `rdb`, `cf_default`, `cf_txn`, `cf_meta` | set at open | the instance |
| `tmp`, `dbid`, `bufpool`, `tmp_listener` | set at the first session | what the environment lends |
| `nsessions`, `idle_since`, `idle_close_ms` | `env_t::mu` | §5.2 |
| `opts`, `rocksdb_options` | set at open | what `other_options()` compares |
| `stores`, `had_temp` | `store_mu` | §7.6 |
| `stores_version`, `last_ddl_ts` | atomic | §7.6, §7.4 |
| `synced_seq`, `syncing`, `sync_head`, `sync_tail` | `sync_mu` | §8.2 |

A `sync_waiter_t` lives on a waiting commit's stack: the target,
the `lead` flag, the next waiter, and a binary semaphore. The
hooks `test_before_commit`, `test_before_sync`,
`test_before_prepare` and `test_before_abort_prepared` stand in
for a failed write, and are empty outside tests.

`env_t` (`sm1/src/env.h`) holds the home's path, the block cache,
the buffer pool, `wal_env`, the root db, the temporary instance,
the registry, the session set and the reaper. `env_t::mu` guards
the root db, the registry and the session set.

### 13.3 `db_open()`

`db_open()` runs §9.1. Around those steps it also does the
following:

- It refuses an option out of range, and parses
  `rocksdb_options`.
- It gives each store CF its options, GC filter included, and
  each other CF a fixed CF's.
- It adds the memtable listener, and sets the environment's
  `Env`.
- It starts each store's memtable counts at 0, as recovery
  flushed its memtables, and reads a chained store's `next_old`.
- It refuses a database with an unknown CF, or without a fixed CF
  or the catalog.
- It sizes the commit cache.

---

## 14. Trade-offs

- **A column family for each store.** It buys instant physical
  drop, tuning for each store, and compaction statistics for each
  store. Costs: every CF is opened at startup, so open time grows
  with the stores. Each CF carries its own memtable and LSM
  metadata, so very many small stores need the shared memory
  budget (§10.1). CF create and drop are outside MVCC, which is
  why `sm1::mkstore` needs eager cleanup and a recovery sweep.
  The target is hundreds to low thousands of stores a database,
  where this trade pays. Past that, the answer is not to tune it
  but to replace it. Fold the storeid into the key as an 8-byte
  prefix in one shared CF. Create and drop become pure MVCC
  operations, and the recovery sweep goes. The cost is the O(1)
  drop, which becomes a `DeleteRange` over the store, and all
  tuning for each store.
- **One process.** It simplifies everything: no shared memory and
  no lock table in shared memory. It is what RocksDB wants.
  Multi-process or remote access means a server front-end, not a
  change to this design.
- **A format mismatch is refused, not migrated.** A home or
  database in another format fails with `SM_ERR_VERSION`.
- **Eager writes cost reads.** Every read may step past
  uncommitted or recent versions and resolve a status. The commit
  cache, the stamps and a seek that usually lands on the answer
  reduce this. In return, a transaction's size is bounded by
  disk, abort is O(1), and recovery needs no undo.

XCTMGR.md gives the trade-offs of `visible_ts`, of SSI's false
aborts, of a transaction's sets in RAM, of a timestamp for every
write, and of the read committed protocol under a race detector.

GC.md gives the trade-off of reclaiming space by compaction.

---

## 15. Cases

| Case | What happens |
| --- | --- |
| crash in `sm1::mkdb`, before the name | the directory is unnamed; the next attach destroys it |
| crash in `sm1::rmdb`, after the name goes | the same |
| crash in `sm1::mkstore`, after the CF | no committed row; the orphan sweep drops the CF |
| crash after a committed `sm1::rmstore` | the row is a tombstone; the orphan sweep drops the CF |
| store made, then the transaction aborts | `drop_created()` drops the CF, newest first; a failure leaves an orphan for the sweep |
| rollback past `sm1::mkstore` | the CF is dropped |
| rollback past `sm1::rmstore` | the CF stays |
| prepared transaction made a store, then a crash | its record's `created` keeps the CF |
| prepared transaction dropped a store, then a crash, then commit | its record's `dropped` queues the CF for GC |
| CREATE TABLE, TRUNCATE, an ALTER's rewrite | `stores_version` rises; a session's next lookup reads the new store |
| a store dropped at commit, or made and rolled back | `stores_version` rises before the handle goes; a lookup after finds no CF |
| a prepared object, a restart | a session of its own, its `store_handles` empty; nothing of the map is on disk |
| a reader, a second writer, or the compaction filter, before the sync | the xid is in the active table and `visible_ts` has not moved: in flight, and no stamp |
| the `CF:txn` sweep | drops records at or below W0, which is below `visible_ts`: never an unsynced one |
| 2PC across a restart | PREPARE stays synced; COMMIT PREPARED goes through `finish_commit()`, the same wait |
| a commit of DDL, or of temporary stores alone | the same record and wait; `last_ddl_ts` set after the sync; `NEXT.md` lists skipping the sync of a commit of temporary stores alone |
| bulk load | the load's files go in before the record; ingested files have no log |
| a log switch during a sync | `SyncWAL()` syncs every log up to the current one |
| older commits in progress | a commit waits for them after its sync (XCTMGR.md) |
| a log past 64 MB | it grows as usual from there |
| crash while the zeros are written | the log is empty or zeros: nothing to replay |
| a database's directory copied | the logs are bigger by their zeros until RocksDB deletes them after a flush |
| a file system with no cost to growing a file | the first attach keeps 0: RocksDB's file system, unwrapped |

---

## 16. Rejected

- **A group sync by batching a transaction's versions at
  commit.** One synced write a transaction. It loses the
  uncommitted version that stands for the lock, as another writer
  cannot see it, and every read path would have to merge the
  batch. It is a rewrite of the write path.
- **Versions with `disableWAL`.** It loses committed data: a
  crash keeps the record and loses the versions.
- **`enable_pipelined_write`.** It does not help the queue.
- **A wake of every waiter when a sync ends.** Each woken commit
  took `sync_mu` in turn before one of them led, and the WAL sync
  sat idle 27% of the time. So a leader hands off to one commit.
- **`recycle_log_file_num`.** It reuses old log files at full
  size, and RocksDB turns it off under `kPointInTimeRecovery`,
  the mode sm1 runs.
- **Zero-filling the MANIFEST too.** It is synced on every store
  made or dropped, which DDL alone does.
- **Zero-filling off WSL.** A tree's load paid 64 MB a log for a
  commit sync no one had measured to need it.
- **An option or a setting for zero-filling.** The cost is the
  file system's, so sm decides at first attach (§8.4).
- **A cache of store lookups invalidated at DDL commit.** It
  hides a store from a transaction whose older snapshot may still
  read it (§7.4).
- **A next-storeid row in the catalog.** Concurrent creates of
  different stores would conflict on it (§7.1).
- **A `created_ts` field in a store's row.** Nothing reads it.
- **A storeid chosen by the caller.** It would have to be refused
  until GC dropped the old CF, or else name two stores at once
  (§7.3).
- **A sequence's position in its MVCC row.** It would lock the
  sequence and give values back on a rollback (§7.7).
- **A temporary store as an unlogged column family of the
  database's own instance.** RocksDB has no temporary column
  family. Each row's `Put` could carry `disableWAL`, while commit
  status stays logged. The flag is on each write, so the store
  handle would have to decide it, and one path that forgets it
  logs a temporary store's data. The orphan sweep would drop its
  CF after a crash. sm1 would have to refuse any other session
  itself. `cf_paths` could keep its files apart. A separate
  instance gives all of this, and writes no log at all
  (TEMPTABLE.md).
- **A block restart point at every entry.** Under 1% fewer
  instructions than an interval of 4, but keys no longer share a
  prefix. The narrow table grew 15%, and its scans slowed by up
  to 13%.
- **Errors as thread-local globals.** `sm1::errnum()` and
  `sm1::errmsg()` are functions (`sm1/API.md`).

---

## 17. Tests

Run on the debug build (`CLAUDE.md`). Paths are relative to
`sm1/`.

### 17.1 The Database Manager

- **Environment** (`test/test_env.cc`): `sm1::init` once a home;
  attach requires an initialized home and refuses an unknown
  format; the sweep; the temporary instance made anew; attach
  recovers every database and fails on one it cannot; one block
  cache; running out of memory aborts.
- **Databases** (`test/test_db.cc`): listing order; a `dbid` is
  never given again; `sm1::rmdb` removes the name and the
  directory; a malformed `rocksdb_options` is refused.
- **Sessions** (`test/test_session.cc`, `test/test_2pc.cc`): an
  open database cannot be removed; detach honors force, and a
  second attach then works; a database stays open for its idle
  time. A forced close aborts an active transaction. A prepared
  one survives, and `sm1::list_prepared` reports its `gid` and
  `prepared_at`.
- **DDL.** Concurrent `sm1::mkstore` calls both commit. A second
  concurrent `sm1::rmstore` of one store fails with
  `SM_ERR_CONFLICT`. Storeids rise. A dropped store stays
  readable at an older snapshot. `sm1::rollback` past
  `sm1::rmstore` keeps the CF, and past `sm1::mkstore` drops it.
  Under SERIALIZABLE, `sm1::lsstore` followed by concurrent
  creates is a conflict.
- **Store handles.** `test/test_chain.cc`: B drops a store A
  read, and A's handle has none; at the end of A's transaction
  its stale handle is gone; the same for a store aborted, or
  rolled back to a savepoint. `rel/test/test_catalog.rel`: a
  table dropped and made again reads as the new one. A table made
  again is a new storeid, so only the unit test can see a version
  not raised. dg's `make tsan`: sessions make and drop temporary
  tables while others read.
- **Sequences** (`test/test_seq.cc`): DDL follows commit, abort
  and rollback, and values do not. A sequence is usable by its
  own transaction, resumes above its lease after a kill, and
  never repeats a value. A name created again starts over.
  `UINT64_MAX` hands out its last value and then fails.
- **Group sync** (`test/test_groupsync.cc`), each shown to fail
  with the code it covers broken. A commit survives a power cut
  once `sm1::commit` returns, on a file system of the test's own
  that truncates each file to its last sync, and the same under
  `manual_wal_flush=true`. A commit is not visible until its sync
  is done. A failed sync aborts the process, in a child. Fifteen
  commits behind a stalled sync share one more sync. The next
  sync goes to the oldest commit in line. `durability_sync = 0`
  makes no sync.
- **Zero-filled logs** (`test/test_crash.cc`). The first attach
  keeps a decision, and a second keeps it unchanged. A log's size
  follows what is kept. The probe leaves no file. The rule's
  edges. Every acknowledged commit survives a kill, and nothing
  more. A record torn in the zeros ends the replay, and the
  database opens.
- **Crash** (`test/test_crash.cc`). A commit loop is killed at
  random points, and the surviving state must be a legal prefix
  of the commit order. A harness that kills at every RocksDB
  write boundary is not built.
- **Status resolution.** A failed commit status write or sync
  aborts the process, and after reopen the transaction is
  committed or dead according to the WAL. A failed prepare's
  record does the same, and the transaction is then listed or
  dead. A workload with `commit_cache_entries = 1` reads the same
  results as with a large cache. That workload is one writer; a
  concurrent one is not built.
- **mvcc.** No scenario reaches a log file or can stall a sync.
  Scenario c35 covers §7.5.
- **rel, dg.** Their suites run unchanged. Nothing SQL-visible
  depends on a sync or a log's size.

### 17.2 The rest of sm1

`sm1/mvcc/MVCC.md` states the isolation rules as a table of
scenarios, with the outcome at REPEATABLE READ, at a mixed pair
of levels, and at SERIALIZABLE. One file a scenario runs it in
all those pairings. The unit tests in `sm1/test` cover the rest:

| Area | What the tests prove |
| --- | --- |
| key order | userkeys that prefix one another (`""`, `"a"`, `"a\x00"`, `"a\xFF"`, `"ab"`) scan in `memcmp` order, and a point read finds its own newest visible version |
| own writes | at each level a transaction reads back a key it inserted, updated twice and deleted, and after `sm1::rollback` the value from before the savepoint; it updates a key it inserted and reinserts a key it deleted; a cursor opened before a savepoint survives a rollback and returns the same rows, whether or not the transaction writes again; one opened after is closed, and no surviving cursor yields a row written after the mark; a rollback to a depth the stack does not hold fails with `SM_ERR_NOSAVEPOINT`; a key locked before a savepoint and rewritten after it stays locked |
| nested savepoints | three marks deep, a rollback to the middle one undoes the writes after it, keeps those before it, drops the mark inside it, and keeps itself for a second rollback; `sm1::forget` keeps writes and locks, and its depth is handed out again; past `sm1::max_savepoints` fails |
| the old row | an update returns the committed value, then its own previous value on a second update; a delete returns the last value written; an empty old value comes back empty; a failed write reports none; a read releases the pin, unless pinned into the caller's ref |
| cursor reposition | a seek resumes with the bounds and view the cursor opened with; a seek backward, or below the start key, fails; under SERIALIZABLE a write skew through skipped keys is caught, and a control that never seeks commits |
| shared locks | two holders; upgrades, and their deadlock; `sm1::lock_row`'s check at each level; a rollback releases and never downgrades; the index-build protocol end to end |
| deadlock | a cycle in both orders: the youngest fails; a doomed transaction's next call fails and changes nothing |
| scan seek mode | a scan from an absent `skey`, and over many userkeys, returns every key; it needs enough keys to leave the memtable, as the failure needs the bloom filter to answer from an SST |
| read committed after a write | a transaction sees a later commit; updating it fails with `SM_ERR_UPDATED`, and goes ahead after `sm1::read_latest` |
| isolation | Adya's anomalies: dirty read, lost update, non-repeatable read, phantom, and write skew, which must be observable at RR and refused at SERIALIZABLE. At SERIALIZABLE: a phantom while the reader's cursor is open (`c18`), an insert of a key the reader found missing (`c05`), a point read past a concurrent writer (`c16`), a scan opened after a writer committed into its range (`c23`), a scan closed right after a key that is then written (`c24`), a scan past a key with only concurrent writers' versions, a scan left open at `sm1::commit`, write skew through `sm1::rmstore` with the storeid in both caches (`c35`), and Fekete's read-only anomaly (`c07`, `c32`). A transaction that reads, outlives every writer, then writes commits, on an idle database too (`c19`). With a prepared or committed pivot the partner fails, and only if T3 committed first. A pivot whose neighbour aborts commits (`c26`, `c27`) |
| 2PC | kill between prepare and commit, and both resolutions; the write-skew history across a restart; `prepared_at` survives; read-only prepares; a prepared drop |

A concurrent random-operation checker against a model is not
built. It would check that histories are serializable at
SERIALIZABLE and snapshot-isolated at RR. GC.md lists the Garbage
Collector's tests, and XCTMGR.md the Transaction Manager's.

