# BULKLOAD — loading many rows in one transaction

`COPY`, `CREATE TABLE AS`, a large `INSERT ... SELECT` and a
restore all put millions of rows into one table in one
transaction. Row at a time through `tbl_insert` that works,
and costs memory in proportion to the rows. This file is the
design of a loader that does not, in three modes:
**mini-batch**, which writes 100 rows at a time and lets other
writers go on; **batch mode**, which writes rows a chunk at a
time through RocksDB's ordinary write path with other writers
kept out; and **file mode**, which writes them to sorted files
that RocksDB ingests whole.

**Status.** File mode into an empty table is built, at every
level and in digres, whose `COPY` uses it (§13 below;
`sm1/API.md`, `sm3/include/sm3.h`, `include/sm.h`,
`../dg/WORKNOTE.md`). A load into a table with rows inserts
its rows one by one. Batch mode, mini-batch and file mode into
a table with rows are not built. What is said here of RocksDB
was checked against 8.9.1 (§10).

**Measured**, 300,000 rows of two integers and a short text by
digres's `COPY`, digres a debug build: 4.0 s row by row and
1.6 s from files with no index; 6.6 s and 1.7 s with a primary
key; 13.0 s and 2.2 s with a primary key and two more indexes.

---

## 0. The proposal in brief

**One loader, three modes: mini-batch, batch mode and file
mode**

- Batch and file mode hold one lock on the table that keeps
  other writers out: SIX, or X when file mode fills an empty
  table. Under it, a lock on each row says nothing new, so
  none is taken.
- Mini-batch holds the table IX, as any writer does, so other
  writers go on. It keeps no row lock either: since
  `sm1/WRITELOCK.md`, no write does (§4.5).
- All are one transaction. Every version carries its `xid`,
  and the load commits or aborts whole, by the one status
  record every transaction has.
- They differ in the lock they take and in how the versions
  reach RocksDB.

| | Mini-batch | Batch mode | File mode |
| --- | --- | --- | --- |
| The table | any; rows are appended | any; rows are appended | any. Empty, made or truncated in this transaction: everything goes to files. With rows: the heap goes to files and the index entries in batches |
| Lock | IX: other writers and readers pass | SIX: readers pass | X for an empty table, which readers wait for; SIX for one with rows |
| Row locks | none | none | none |
| How rows are written | 100 at a time, one RocksDB write batch each | a chunk at a time, one RocksDB write batch each | into sorted files, ingested whole at the end |
| Log, memtable, compaction | all three, as any write | all three, as any write | none for what goes to files |
| A duplicate key is found | within 100 rows | within its chunk | at the end in an empty table; within its chunk in one with rows |
| Memory | 100 rows | one chunk | the sort budget |
| Meant for | `INSERT ... SELECT`, and any load others must not wait for | a load others may wait for | `COPY`, a restore, `CREATE TABLE AS` |

**What all keep**

- Atomic. No reader sees part of a load.
- Every check a row gets today: `NOT NULL`, unique indexes,
  the host's evaluator for partial and expression indexes.
- Memory that does not grow with the load.

**What they give up**

- In batch and file mode, other writers of the table wait
  until the load's transaction ends, and readers too when file
  mode fills an empty table. Mini-batch makes no one wait.
- A statement that must read the table as it loads, for
  `ON CONFLICT` or a trigger that queries it, uses
  `tbl_insert`.
- They insert. There is no bulk update or delete.

**What has to be built**

- sm1: a loader on a store, writing in batches or to files,
  locking unique keys when the table is not held, and a sweep
  of leftover files at startup.
- sm2: heap and index inserts that feed it.
- sm3: a loader on an open table.
- `include/sm.h` and `rel`: the calls, and a verb.

**What is not known**

- Several of RocksDB's behaviours file mode leans on (§10).

**Not measured, by decision.** File mode writes each row once,
past the log, the memtable and the compactions, so it wins on
any load big enough to leave the small-load path (§5.7). No
benchmark is run before building it. The knobs start at
defaults chosen here, to be tuned in use.

---

## 1. The problem

`tbl_insert` on a table with `k` indexes writes `1 + k` keys.
For each key sm1 does this (`sm1/SM1_DESIGN.md` §5):

| Step | Cost |
| --- | --- |
| take the key's lock | the lock table's one mutex, and an entry in `T.locks` kept to the end of the transaction |
| take a write timestamp | one use of the global counter and its lease |
| write the version | one RocksDB `Put`: logged, then put in a memtable, flushed, and compacted down the tree |
| remember it for a savepoint, if one is set | an entry in `T.undo`, with the key inline, kept to the end |
| tell SSI | one lookup |

**Memory.** The lock entries are kept for the life of the
transaction, and the undo entries too when a savepoint is
set, so a load of `n` rows holds `n × (1 + k)` entries or
twice that. `sm1/SM1_DESIGN.md` §13 records it: a transaction
writing tens of millions of keys costs gigabytes, and "bulk
loads want batching into many transactions."

Batching into many transactions is what this design avoids.
It would bound the memory and lose the atomicity: a failed
`COPY` would leave half its rows committed, and a reader
would see a table half loaded.

**Writing everything several times.** A row put through the
ordinary path is written to the log, then to a level-0 file,
then rewritten at each level compaction moves it down. A
loaded row is written once, into a file that is already where
it will stay.

---

## 2. What a load must keep

1. **One transaction.** All of it commits or none does, and
   it may share its transaction with other statements.
2. **Readers never wait on a lock.** No reader of the table
   is held up by a load, as none is by a write.
3. **One way to make a row's entries.** The loader calls the
   routines `tbl_insert` calls to check a row and to make its
   index entries. It differs only in how they reach RocksDB.
4. **Bounded memory.** What a load holds does not grow with
   the rows loaded.
5. **Crash safe by what is already there.** No log of its
   own, and no recovery but deleting files left behind.
6. **Tuple at a time across the API** (`DESIGN.md` §5). The
   host hands rows over one by one.

---

## 3. What the modes share

### 3.1 A table lock stands in for the row locks

A writer holds its table's oid lock IX, and a reader IS
(`sm3/SM3_DESIGN.md` §2). A loader in batch or file mode takes
it **SIX**. One in file mode filling an empty table holds it
**X** already (§5.1); X covers SIX, so all that follows holds
for it too. Mini-batch takes it **IX**, as a writer does, and
§4.5 says what it does instead.

| Held by another | Against the loader's SIX |
| --- | --- |
| IS, a reader | compatible: readers pass |
| IX, a writer or an index build's batch | waits, or is waited for |
| S, SIX, X: another loader or a DDL | waits, or is waited for |

So once the loader holds SIX, it is the only transaction that
can write any key of the table's store, and it stays so to
the end of its transaction. Three things follow.

- **A row lock would exclude no one.** The only transactions
  it could keep out are already kept out. The loader takes
  none, and `T.locks` does not grow.
- **Every other version in the store is settled.** SIX waited
  out every writer in flight, prepared ones among them. What
  the loader finds under a key is committed or dead, never
  another transaction's unfinished write.
- **A reader's row lock does not meet the loader.** A reader
  may lock an existing row shared (`tbl_lock`), as a host
  does to check a foreign key. The loader touches no existing
  row: it adds rows under new rownos and adds index entries.

A table made in the same transaction is held X by its
creator, which covers SIX. It needs no case of its own.

sm1 keeps no lock hierarchy and cannot know that one key
stands for a table. So sm1's loader is told which lock covers
it, and in batch and file mode refuses unless the transaction
holds that lock SIX or X. The check is cheap and it is what
stops a caller that forgot the lock from writing unprotected.

### 3.2 The rows

A row handed to the loader gets what `tbl_insert` gives it,
by the same routines (requirement 3):

1. `NOT NULL` is checked.
2. A `rowno` is taken. The loader takes rownos from the
   table's sequence a hundred at a time, ahead of need, and
   hands them out as rows arrive. It needs nothing else: a
   sequence hands out each value once and in rising order,
   so the load's rownos are its own and ascend, whoever else
   draws. With other writers kept out, in batch and file
   mode, they are also above every row the table holds.
3. The tuple is encoded.
4. The entry of every index is made, calling the host's
   evaluator where an index needs it.

What differs between the modes begins after that: where the
heap version and the index versions go, and when a unique key
is checked.

### 3.3 Timestamps and `seq`

A version's key is `userkey | ~wts | ~seq`
(`sm1/SM1_DESIGN.md` §3.1). An ordinary write takes one `wts`
and one `seq` for each key. A load takes one of each for many
keys: for 100 rows in mini-batch, for a chunk in batch mode,
for the whole load in file mode.

- **One `wts` is enough** because of the lock. The rule `wts`
  serves is that a key's versions sort in commit order. Every
  older version of any key in the store was written before
  the loader got its lock, so a timestamp taken after is
  newer than all of them.
- **One `seq` is enough** because a load writes each key
  once. `seq` exists so that a transaction writing one key
  twice makes two versions. The heap keys are distinct
  rownos, and a repeated unique key is refused.

One `seq` for many keys is also what makes a large load
possible. `seq` is four bytes and counts a transaction's
writes, and sm1 refuses the 2^32nd.

### 3.4 Serializable readers

A serializable reader's SIREAD locks are checked by every
write, a key at a time (`sm1/SM1_DESIGN.md` §7.1). The loader
makes the same call for every key it writes. It keeps nothing
in the loader's transaction, so it does not grow, and a store
no serializable reader has touched pays one lookup a key.

A loader that is itself serializable reads nothing but the
unique keys it checks, and those reads are recorded like any
other. A large serializable load into a table with a unique
index and rows therefore still grows a read set. READ
COMMITTED is the level to load at.

### 3.5 Savepoints

An ordinary write made while a savepoint is set is listed in
`T.undo`, so that a rollback can delete it
(`sm1/SM1_DESIGN.md` §6.5). A load lists nothing, in any
mode.

- **With no savepoint set** nothing is listed by any write,
  and there is nothing to decide.
- **With one set**, a rollback to it could not undo the load.
  Until `sm1/SAVEPOINT.md` is built, opening a loader marks
  the savepoints then set lost, by the mechanism that marks
  them lost at the undo list's cap: a rollback to one fails
  with `SM_ERR_UNDOLIMIT`, and the transaction is aborted
  (`sm1/SM1_DESIGN.md` §6.5).
- **Once it is built**, a rollback marks a range of `seq`
  dead, and a load's versions are covered by it like any
  other. The refusal goes, with no code here.

A failed load needs none of this. The transaction aborts, and
an abort deletes nothing.

### 3.6 Indexes

A load maintains every index of the table, in any mode.
An index that is still pending is maintained too, as any
writer maintains it.

A restore does not need that. `pg_dump` loads a table's rows
before it creates the indexes and the primary key, so the
load there writes the heap alone, and the indexes are the
concurrent build's (`sm3/SM3_DESIGN.md` §3). That build goes
a row at a time through the ordinary path, and how fast it is
has not been measured (`NEXT.md` §2). For a restore it is the
larger half of the work, and sorting an index into files is
what §5.3 builds. Giving the build the same streams is the
natural next step and is not part of this design.

A backfill batch holds the table-oid lock IX. So a load waits
for the batch in flight, and the build waits for the load.

`TRUNCATE` and then `COPY` in one transaction is the other
common way to fill a table. The truncate gives the table a new,
empty store under the table-oid lock X (`sm3/SM3_DESIGN.md` §4),
which is exactly what file mode requires (§5.1).

---

## 4. Batch mode and mini-batch

### 4.1 A chunk

The loader buffers rows and writes them a chunk at a time. A
chunk is bounded in bytes, a knob, 4 MB by default.

For each chunk:

1. **Check the unique keys** (§4.2).
2. **Tell SSI** of each key (§3.4).
3. **Take one write timestamp and one `seq`** (§3.3).
4. **Write everything in one RocksDB batch**: the heap
   versions and the index versions together, logged and not
   synced, as every uncommitted write is.

Nothing is appended to `T.locks` or `T.undo`. What the loader
holds in memory is the chunk it is filling.

A chunk is written when the buffer fills, inside the call that
hands over a row, and the last one when the loader closes.
There is no call to write it sooner. The loading statement
does not see its own rows in any case, as no statement sees
its own writes (`sm1/MVCC.md` §10), and the next statement
begins after the loader has closed.

The transaction may also write the table the ordinary way
while a batch loader is open. Such a write does not see rows
still buffered, and the chunk's own check finds the clash
when it is written.

### 4.2 Unique keys

A non-unique entry ends in its row's `rowno`, so it is new by
construction, and the heap key is the `rowno` itself. Only a
unique index's key can collide.

| Collides with | Found by |
| --- | --- |
| a row in the same chunk | sorting the chunk's keys for that index; equal neighbours collide |
| a row of an earlier chunk, or written earlier by this transaction | a read of the key, which sees the transaction's own versions |
| a row already in the table | the same read |

The read is the check `sm1::insert` does today
(`sm1/SM1_DESIGN.md` §5, step 3), with the same outcomes: a
live version is `SM_ERR_DUPKEY`, and a version committed after
the snapshot of a transaction above READ COMMITTED is
`SM_ERR_CONFLICT`. It costs one seek a unique key, which a
bloom filter answers for a key that was never there.

**An index with nothing under its relid needs no reads of
the table.** The loader looks once when it opens. No one else
can write the index while SIX is held, so the only keys it
can meet are its own.

A duplicate is reported when its chunk is written: within a
few thousand rows of the row that caused it.

### 4.3 Commit, abort, crash and prepare

Nothing here is new.

- **Commit** is the one status record. A commit or prepare
  with a loader open is refused; the loader is closed first.
- **Abort** writes nothing. The versions resolve as dead and
  compaction drops them.
- **A crash** before commit leaves no status record, which is
  an abort.
- **Prepare** records the locks the transaction holds, with
  their modes. The SIX table-oid lock is one of them, so after a
  restart an in-doubt load still keeps writers out, as the
  row locks would have. A mini-batch load's locks on unique
  keys are recorded like any others.

### 4.4 What batch mode does not save

Every row is still written to RocksDB's log, then to a
level-0 file, then rewritten as compaction moves it down. It
saves the memory of §1 and the per-key lock, timestamp and
write call. It does not save the bytes written.

### 4.5 Mini-batch

Mini-batch is batch mode with the table held IX, so other
writers go on, as PostgreSQL's `COPY` and `INSERT` let them.

- **A batch is 100 rows**, buffered and written as one
  RocksDB batch, with one `wts` and one `seq`. Its rownos are
  reserved together, 100 drawn ahead (§3.2).
- **Heap keys and non-unique entries take no lock.** Each ends
  in a rowno from the reserved block, which no one else can
  draw, so no other transaction can write the same key.
- **Each unique key is checked as any write checks it**
  (`sm1/WRITELOCK.md` §3.2): under its latch, waiting for
  another transaction's uncommitted version of it, or for an
  explicit lock on it, and then read as §4.2 reads it. No lock
  is kept. The batch takes its `wts` only once every unique key
  of it is checked, with no other writer of them in flight,
  which keeps a key's versions in commit order (§3.3).
- **Every other check is batch mode's**, at 100 rows: a
  duplicate within the batch by sorting, against the table by
  the read, `SM_ERR_CONFLICT` above READ COMMITTED.
- **It keeps no undo list**, as batch mode, so it treats
  savepoints as §3.5 says.

What it saves over `tbl_insert` is a RocksDB write, a
timestamp and a sequence draw for each row. Neither keeps row
locks (§6.1).

One small difference from the ordinary path: a transaction may
lock a row that does not exist yet (`tbl_lock`), and such a
lock on a reserved rowno is not met when the row is written.
PostgreSQL cannot lock a row that does not exist, so nothing
in the fork does this.

---

## 5. File mode

### 5.1 Two cases: an empty table, and one with rows

The loader decides which when it opens.

**An empty table.** Its store was made in the loader's
transaction, by `CREATE TABLE` or by `TRUNCATE`, and holds
nothing yet: the store is not the one in the table's
committed definition, and one seek finds no key in it. Such a
table is held X by that transaction already. Everything goes
to files: the heap and every index. This is a restore,
`CREATE TABLE AS`, and `TRUNCATE` then `COPY`.

**A table with rows**, or one the transaction has written to,
under SIX. The heap goes to files and the index entries go in
batches, as in batch mode (§4). This is a `COPY` that appends.

The split follows where each part can collide. A heap key is
a `rowno` the load drew (§3.2), so it is new and above every
existing row, and a heap file meets nothing. An index key is
a column value, which may sit anywhere among the entries the
index has: a unique one must be checked against them, and a
file of them would overlap them. Batch mode already does both
of those right, so the index entries take its path.

In the empty case, what the rule removes:

- **No unique check against the table.** Nothing is there to
  collide with; a duplicate can only be within the load, and
  the merge finds it (§5.4).
- **No overlap.** Every file overlaps nothing in the store, so
  RocksDB places each at the bottom of the tree, and no
  memtable holds a key in its range to be flushed first.
- **No other session at all.** X keeps readers out as well as
  writers, so nothing is read or written in the store but by
  the loader's own transaction.

What it costs: readers of a table being filled wait for the
transaction. In the three cases they wait already, behind the
X that made the store.

In the case with rows, the heap's bytes, usually most of a
load, skip the log and the compactions, and readers pass. The
index entries pay batch mode's costs (§4.4).

### 5.2 The files

A file is written with RocksDB's `SstFileWriter`, using the
store's own options and comparator, into a directory of the
database kept for loads. A key must be added in the
comparator's order, which is the whole of the difficulty.

What goes into a file is exactly what a write would have put
into RocksDB: a version, keyed `userkey | ~wts | ~seq`, with
the transaction's `xid` in its value
(`sm1/SM1_DESIGN.md` §3.1). A version in an ingested file is
no different from one in a memtable. It is uncommitted until
the transaction's status record says otherwise, so the files
can be handed to RocksDB before the commit.

sm1's loader takes keys in **streams**. A stream is declared
ascending or not, and unique or not.

| Stream | Declared | So sm1 |
| --- | --- | --- |
| the heap | ascending | writes each key straight into a file |
| a non-unique index | unsorted | sorts it (§5.3) |
| a unique index | unsorted, unique | sorts it and checks it (§5.4) |

**The heap is ascending for free.** A row's key is its
`rowno`, and a load's rownos ascend (§3.2). A heap file is
started, filled as rows arrive, and closed at a size limit,
256 MB by default, and the next one started.

### 5.3 Sorting an index

An index's entries arrive in the heap's order, not the key's.
It is an external sort, in the plain two-phase form.

1. **Runs.** Entries collect in memory up to a budget, a
   knob, 256 MB by default for the whole load. When it is full
   they are sorted and written out as a run, and the memory is
   reused.
2. **Merge.** At the end the runs are merged into the files
   to ingest, which do not overlap each other. A stream that
   never filled its budget is sorted and written once, with
   no run.

Memory is the budget, shared by a table's streams. Disk is,
for a moment, twice the index: the runs and the merged files
together, until the runs are deleted.

**The alternative is to skip the merge**: write each run as a
file and ingest them all. RocksDB accepts files that overlap
each other and its compaction merges them later. That is less
code, and it moves the merge into the database: tens of
overlapping files in level 0, compaction rewriting the index
at least once more, and reads of the index slow until it has.
The merge here is a small routine over sorted streams, and it
is where §5.4 does its work. The merge is kept.

### 5.4 Unique keys

In an empty table a unique key can collide only with another
row of the same load. The merge finds it: equal keys come out
as neighbours. No key is read from the store.

In a table with rows the index entries go in batches, and a
unique key is checked as batch mode checks it (§4.2): within
its chunk and by a read of the store.

**In an empty table all of this happens at the end.** A
duplicate in the tenth row of a hundred million is found after
the last row is handed over. PostgreSQL finds it at the tenth
row. The work lost is the price of sorting before checking.

### 5.5 Closing: ingestion

Closing the loader finishes the merges and then ingests every
file of the load in one call, which RocksDB applies
atomically.

- **Files are moved, not copied.** The load directory is on
  the database's filesystem so that RocksDB can link them in.
- **In an empty table every file overlaps nothing**, the
  store being new (§5.1), so RocksDB places each at the
  bottom of the tree, where no compaction has reason to
  rewrite it, and flushes no memtable first.
- **In a table with rows only heap files are ingested.**
  Their rownos are above every row the table holds, so they
  overlap no row. A file at the bottom of the tree may still
  span the end of the heap and the start of the first index,
  whose keys sort next in the store; a heap file inside that
  span lands higher, and costs a later compaction.

From then the transaction sees its loaded rows, by the load's
`seq`, and other transactions do not, by its `xid`.

Before the close the heap's rows are not in the database. In
a table with rows the index entries already are, written in
batches, and point at rows not there yet. So while a file
loader is open on a table, the transaction's reads of that
table are refused, and so are its ordinary writes: a read
would meet those entries, and a write could not see the rows
in the files, and the load's one timestamp (§3.3) was taken
before it.

### 5.6 Commit, abort, crash and prepare

- **Commit** is the one status record, as ever. A commit with
  a loader still open is refused.
- **Abort before the close** deletes the load's files. In a
  table with rows the index entries written in batches are
  dead with the rest.
- **Abort after the close** writes nothing, as any abort. An
  empty table's store was made by the transaction, so the
  abort drops it whole, files and all. In a table with rows
  the heap files' versions are dead, and wait for the sweep
  below to be dropped.
- **A crash before the close** leaves files in the load
  directory that no database knows. Opening the database
  empties the directory.
- **A crash after the close and before the commit** leaves no
  status record, which is an abort.
- **Prepare** needs the loader closed, and then is §4.3's.

**Garbage collection is unchanged, and has one cost here.** A
loaded version carries its `xid` until compaction stamps the
commit timestamp into it (`sm1/SM1_DESIGN.md` §8.3). Files at
the bottom of the tree are not visited by ordinary
compaction, so a load's versions are stamped by the periodic
sweep, which rewrites every store. Until then each read of
one resolves its `xid`, from the commit cache or the status
table. It is correct, and it is the same for any cold file.
Dead versions from a load aborted after its close wait for
the same sweep.


### 5.7 A small load

Each ingestion adds files, and a file of a hundred rows at
the bottom of the tree stays a file of a hundred rows: no
compaction has reason to merge it with its neighbours. A
thousand small loads would leave a thousand such files.

The host cannot always know a load's size when it starts; a
`COPY` reads until its input ends.

**Decision proposed: a file load that ends small is written
as a batch.** The loader keeps its first rows in memory and
starts no file until they pass one chunk, §4.1's size. If the
load is closed before that, the rows are written as that one
chunk. It is the reason the modes are one loader.

### 5.8 How PostgreSQL does the same

PostgreSQL skips the WAL for a table's contents under nearly
the same rule. What follows is from memory of its source and
documentation, and is among the claims to check (§10).

**Its conditions.** Both must hold:

1. `wal_level = minimal`. The default since PostgreSQL 10 is
   `replica`, and at `replica` or `logical` everything is
   logged, because standbys and logical decoding replay the
   WAL. On a default installation `CREATE TABLE AS` is fully
   logged.
2. The table's storage file was created in the current
   transaction: `CREATE TABLE AS`, `CREATE TABLE` then `COPY`,
   or `TRUNCATE` then `COPY`, since `TRUNCATE` gives the table
   a new file.

**How it stays crash-safe.** Rows go to the new file's pages
with no WAL records. At commit, before the commit record, the
file is made durable. Since PostgreSQL 13 the choice is by
size, against `wal_skip_threshold`, 2 MB by default: a small
file is written into the WAL whole, as full-page images, and a
large one is flushed with `fsync`. An abort or a crash undoes
nothing: the file belonged to a table that never committed,
and is discarded, or after a crash left behind as an orphan.
The commit record is what makes the table exist, so its
contents need only be on disk before it.

Two neighbours are not the same thing. `COPY ... FREEZE`
uses the same condition to write rows already frozen, saving
a later rewrite by vacuum, not WAL. An unlogged table never
logs its contents at any `wal_level` and is emptied after a
crash: its data is not durable at all.

| | PostgreSQL | wisr, file mode |
| --- | --- | --- |
| Condition | storage file created in this transaction, and `wal_level = minimal` | for everything: store created in this transaction, by `CREATE TABLE` or `TRUNCATE`, and empty. For the heap alone: any table (§5.1) |
| Contents logged | no | no: written to sorted files and ingested, past RocksDB's log |
| Made durable before commit by | `fsync`, or full-page images for a small file | RocksDB's ingestion, which records the files in its manifest before it returns (§10) |
| Abort or crash | the file is discarded | the store is dropped; after a crash the startup sweep drops it |
| Replication | must be off | no constraint: nothing replays RocksDB's log elsewhere |

For an empty table file mode follows PostgreSQL's rule. It
goes further in two ways. A load that appends to a table with
rows still keeps its heap out of the log, which PostgreSQL
never does, because new rownos meet no existing row. And, the
last row, PostgreSQL gives the saving up on any server with
standbys, where wisr does not have to.

---

## 6. Choosing a mode

The loader is told its mode when it opens. The choice is the
host's, by what the statement needs:

| The statement | Path |
| --- | --- |
| writes a few rows | `tbl_insert` |
| reads the table as it writes: `ON CONFLICT`, a trigger that queries it | `tbl_insert` |
| must not make other writers of the table wait | mini-batch |
| may make other writers wait | batch mode |
| is large, may make other writers wait, and does not read the table until it ends | file mode |

By statement, that is usually:

- **A restore, `CREATE TABLE AS`, `TRUNCATE` then `COPY`**:
  file mode, everything to files.
- **A `COPY` that appends**: file mode, the heap to files and
  the index entries in batches.
- **A `COPY` that appends, where other writers must not
  wait**: mini-batch.
- **A `COPY` into a table with triggers that read it**:
  `tbl_insert`, as PostgreSQL's own `COPY` falls back to one
  row at a time for such a table.
- **`INSERT ... SELECT`**: mini-batch, which blocks no one,
  as PostgreSQL's does not; batch mode where the host may
  block writers. The planner's row estimate says whether it
  is worth leaving `tbl_insert` for.

### 6.1 The row locks each path holds

Since `sm1/WRITELOCK.md`, a write keeps no row lock: its
uncommitted version stands for the lock, and another writer of
the key waits for its transaction. Locks held for 1,000 rows
into a table with two unique indexes and one non-unique one,
to the end of the transaction:

| Path | Row locks, before `WRITELOCK.md` | Row locks, now |
| --- | --- | --- |
| `tbl_insert` | 4,000: the heap row, both unique keys and the non-unique entry, for each row | 0 |
| mini-batch | 2,000: the two unique keys, for each row | 0 |
| batch mode, file mode | 0: the table lock, SIX or X, keeps other writers out | 0 |

A lock cost on the order of 150 to 250 bytes, an estimate from
the structures and not measured: ten million rows through
`tbl_insert` held several gigabytes. Now a load's memory is
bounded in every mode, and batch and file mode's table lock is
a choice about throughput, not memory.

**How PostgreSQL holds none.** It does lock, but only for an
instant; the lasting "lock" is the uncommitted index entry
itself. Say transactions A and B both insert `id = 7`:

1. A takes the latch of the B-tree leaf page where `7`
   belongs: an exclusive lock in memory, held for
   microseconds.
2. A checks the page for `7` and finds none.
3. A writes its entry for `7`, still holding the latch, then
   releases it. The entry is on the page, uncommitted, marked
   with A's transaction id.
4. B takes the same latch and finds A's entry.
5. B sees that A is still running, releases the latch, and
   waits for A to end.
6. When A ends, B checks again: a duplicate if A committed,
   free if A aborted.

| Mechanism | Lasts | Purpose |
| --- | --- | --- |
| the page latch | microseconds | makes check-then-write one step, so A and B cannot both check before either writes |
| the uncommitted entry | until A ends | tells everyone else the key is taken, or about to be |
| A's lock on its own transaction id | A's life, one per transaction | lets B wait for A to end |

The last is the only lasting lock, one per transaction however
many rows it writes: 1,000 rows cost one lock, not 2,000.

**Why sm1 held row locks.** It has no pages, so no latch to
make check-then-write one step: checking a key and writing it
are two RocksDB operations. The row lock on the key is what
keeps A and B from both checking before either writes, and,
held to commit, what makes B wait for A.

**How sm1 does the same.** It splits the row lock into
PostgreSQL's two roles (`sm1/WRITELOCK.md`):

1. A short latch on the key, held only across check-then-write
   and then released: the page latch's part.
2. A write that finds another transaction's uncommitted
   version of its key waits for that transaction to end, and
   checks again. sm1 knows every running transaction, in its
   active table, so it needs one wait point per transaction,
   and deadlock detection sees a wait on a transaction as an
   edge to it.

No write keeps a lock once it returns, in any path.

---

## 7. The interface

### 7.1 sm3, on an open table

On a table opened IX or stronger for mini-batch, and SIX or X
for batch and file mode; file mode on an empty table needs it
X, which the transaction that made the table holds already
(§5.1):

```cpp
enum class load_mode_t { mini_batch, batch, files };
struct loader_t;

// Begin a load into table in mode.
int tbl_load_open(table_t* table, load_mode_t mode, loader_t*& loader);

// Hand over one row. Its rowno is known at once. Checks NOT
// NULL and makes every index's entry, as tbl_insert does; a
// full buffer is written here, and a duplicate it finds is
// reported here.
int tbl_load_add(loader_t* loader, std::span<const sm2::datum_t> values,
                 std::span<const bool> isnull, uint64_t& rowno);

// Finish: write the last batch, or sort, check and ingest, and
// free the loader, which is freed even on error. The
// transaction is still to commit.
int tbl_load_close(loader_t* loader);
```

`include/sm.h` has the same three as `sm_tbl_load_open`,
`sm_tbl_load_add` and `sm_tbl_load_close`, in its C style. They
take the table handle and no session: the handle carries its
session, as `sm_tbl_insert`'s does. The chunk size, the sort
budget, the file size and the small-load threshold are options
of the database, not arguments; mini-batch's 100 rows is fixed.

**Errors.** `tbl_load_add` fails at once for what it can see
in the row: a NULL in a `NOT NULL` column, a key too long for
a unique index, an evaluator that fails. A duplicate key fails
the call that writes it: a later `add` or the `close`; in file
mode in an empty table, the `close`. The message names the
index and the rownos. After any failure the transaction is to
be aborted.

### 7.2 sm1 and sm2, underneath

```cpp
// sm1: a load into one store. cover names the lock that keeps
// other writers out, held SIX or X; with exclusive false the
// caller holds no such lock, and checked keys are checked
// as any write checks them, under their latches, waiting for
// another writer in flight (mini-batch). Opening marks the
// savepoints then set lost (§3.5).
int load_open(session_t* session, uint64_t storeid,
              uint64_t cover_storeid, std::string_view cover_key,
              bool exclusive, loader_t*& out);

// Add one new key. check: it must not exist, so it is read,
// waiting first for another writer of it when the load is not
// exclusive; false: the caller vouches it is new. Every key is checked against its
// batch either way.
int load_put(loader_t* loader, std::string_view key,
             std::string_view value, bool check);

// Write the last batch and free the loader.
int load_close(loader_t* loader);

// sm2: a row under a fresh rowno, and an index entry, put into
// a loader as rel_insert and idx_insert would write them; an
// entry of a unique index is put with check set.
int rel_load(rel_t* rel, sm1::loader_t* loader,
             std::span<const datum_t> datums,
             std::span<const bool> isnull, uint64_t& rowno);
int idx_load(index_t* index, sm1::loader_t* loader,
             std::span<const datum_t> key, std::span<const bool> key_isnull,
             uint64_t rowno, std::span<const datum_t> include,
             std::span<const bool> include_isnull);
```

The batch size, 100 rows or a chunk's bytes, is the caller's:
sm3 closes a batch with a write when it is full. A commit or
prepare with a loader open fails with `SM_ERR_INVALID`; an
abort leaves the loader dead, for `load_close` to free.
---

## 8. What it costs

Every mode:

- **In batch and file mode writers wait for the whole
  transaction**, not for the load alone: the lock is held to
  commit, as every lock is. PostgreSQL's `COPY` and `INSERT`
  shut no writer out; mini-batch does not either.
- **A load is a long transaction.** It holds back garbage
  collection across the database for as long as any long
  transaction does.
- **No rollback to a savepoint set before the load**, until
  `sm1/SAVEPOINT.md` is built (§3.5).
- **A second way to write in sm1.** It shares the version
  format, the visibility rules and the commit with the
  ordinary one.

Mini-batch:

- **The bytes are still written several times** (§4.4).

Batch mode:

- **The bytes are still written several times** (§4.4).
- **A duplicate is reported a chunk late.**

File mode:

- **In an empty table a late duplicate wastes the load**
  (§5.4), and readers wait for the transaction (§5.1).
- **In a table with rows the index entries pay batch mode's
  costs**, and only the heap skips the log (§5.1).
- **Nothing is readable before the close** (§5.5).
- **Disk for the sort**: for a moment, twice the indexes.
- **Ingestion pauses the database's writes for a moment**
  while RocksDB fits the files in.
- **Loaded rows cost a status lookup for longer** (§5.6).
- **The first code in the stack that writes files of its
  own**, with a directory to sweep.

---

## 9. Alternatives considered

| Alternative | Why not |
| --- | --- |
| Many small transactions | Bounded memory, but not atomic: a failed load leaves rows behind and readers see a table half loaded |
| One mode only, batches | Leaves the largest loads paying for the log and for compaction, which is most of their cost |
| One mode only, files | Cannot serve a statement that reads the table as it writes, and makes every duplicate a late one |
| Keep the row locks and undo, spilled to disk | Atomic and blocks no one, but keeps every per-row cost and adds a spill file with a life of its own |
| Lock escalation: trade row locks for a table lock past a threshold | The same end state as §3.1, reached by a conversion in mid-statement that can deadlock, and sm1 has no hierarchy to escalate along |
| Ingest each index's runs unmerged | §5.3 |
| Skip the log for an ordinary write (`disableWAL`) | Saves the log and keeps the memtable and the compactions. Ingestion saves all three |

Other RocksDB facilities for loading, and why they are not
used:

- **`PrepareForBulkLoad`**, a preset that turns compaction
  off and compacts once at the end. It is set for a whole
  column family, and the store's other writers and readers
  would live with it.
- **The vector memtable**, fast to fill and slow to read. The
  same, and a unique check reads.
- **`CreateColumnFamilyWithImport`**, which makes a column
  family from finished files. It would suit a table made and
  loaded in one step, but the store exists before the load
  does (`sm1/SM1_DESIGN.md` §11.2), and plain ingestion into
  an empty store ends in the same place.

---

## 10. To check, first

Checked on 2026-10-06 against RocksDB 8.9.1: its source
(`db/external_sst_file_ingestion_job.cc`,
`db/db_impl/db_impl.cc`, `db/version_set.cc`,
`table/sst_file_writer.cc`) and a throwaway program over the
installed library, not kept. Each check, and what the design
takes from it:

| Check | Found | So the loader |
| --- | --- | --- |
| `SstFileWriter` with a custom comparator, its files ingested into that column family | **Yes**: a file written with the column family's own options, a non-bytewise comparator among them, ingests and reads back in order | builds the writer's options from the store's own, `GetOptions(cf)` |
| A file written with the wrong comparator is refused | **No**: RocksDB does not check the comparator's name, and ingests such a file, whose keys are then out of order | must never write with any options but the store's; nothing below it would notice |
| One call, many files, applied atomically | **Yes**: one ingestion job per column family, its files in one manifest edit; `IngestExternalFiles` groups several column families | ingests a load's files in one call |
| Files that overlap nothing land at the bottom | **Yes**, a snapshot open or not: three files at the bottom level after ingesting two above existing data | §5.5 holds |
| Files that overlap each other | all go to level 0, with a new sequence number | must never put two overlapping files in one call: the heap's files, and each index's merged files, are disjoint |
| `fail_if_not_bottommost_level` | refuses with `TryAgain` a file that cannot go to the bottom | may set it for an empty table, to turn a placement surprise into an error |
| Durable once the call returns | **Yes**: a moved file is linked, reopened and synced; a copied one is synced; the data directory is synced; the manifest edit is written and synced before the call returns | needs no sync of its own before the commit record |
| What ingestion blocks | linking and syncing the files come first and block nothing. Then every write to the **whole database** waits, while RocksDB checks for a memtable to flush, flushes it if one overlaps, picks levels, and writes and syncs the manifest | holds the database's writes for a manifest sync and, when it must, a flush; in an empty table nothing overlaps, so never a flush |
| An iterator or snapshot opened before does not see the files | **Yes**: an iterator opened before saw the old rows only, and a read at an earlier snapshot did not find an ingested key | §5.5 holds; sm1's own visibility would hide the uncommitted rows anyway |
| The compaction filter sees ingested keys | **Yes**: a forced compaction showed the filter every key, ingested ones included | stamping and garbage collection work on loaded rows as on any (§5.6) |
| A write batch of a chunk | sm1's stores keep RocksDB's 64 MB memtable, and a 4 MB batch is far below it | §4.1 holds; such a batch stalls only as any write does |
| No caller under IS locks or writes a key the loader inserts | **Yes**, from sm3's code: a write needs IX; under IS only `tbl_lock` shared, on a row that exists, and never on an index key | §3.1 holds |

Two findings change the design: the comparator must come from
the store, since a wrong one would be taken silently; and the
files of one call must be disjoint, or all of them land in
level 0.

Nothing is measured first (§0). The defaults of the knobs are
§4.1's chunk, §5.2's file size, §5.3's sort budget, and one
chunk as §5.7's threshold.

---

## 11. What has to be built

| Level | What | For |
| --- | --- | --- |
| sm1 | a loader on a store: the covering-lock check, the unique check, the SSI calls | §3 |
| sm1 | batch mode: one `wts` and `seq` a chunk, one write batch | §4 |
| sm1 | mini-batch: unique keys checked as any write checks them, then the batch | §4.5 |
| sm1 | file mode: streams, files, the sort and the merge, ingestion; the load directory, emptied when the database opens; a loader's files deleted by abort | §5 |
| sm1 | commit and prepare refused with a loader open; a rollback refused past a load, until `sm1/SAVEPOINT.md` | §3.5, §4.3, §5.6 |
| sm1 | options: the chunk size, the sort budget, the file size, the small-load threshold | §4.1, §5 |
| sm2 | `rel_load` and `idx_load`, and a cache of drawn rownos in `rel_t` | §3.2, §7.2 |
| sm3 | `tbl_load_*`, over `tbl_insert`'s own routines; file mode's two cases, told apart when the loader opens; the table's own reads and writes refused while a file loader is open | §5.1, §5.5, §7 |
| top | `sm_tbl_load_*` in `include/sm.h` | §7 |
| `rel` | a `load` verb, in each mode | testing |
| fork | each bulk statement choosing a path | §6 |

---

## 12. Order of work

Batch mode comes first: it is the smaller, mini-batch and file
mode's small loads need it, and it puts the lock, the checks
and the interface in place.

1. The checks of §10.
   → verify: each written up here, and the design changed
   where a check fails.
2. sm1's loader in batch mode, tests first.
   → verify: refused without the covering lock; a duplicate
   inside a chunk, against a committed row, and against the
   transaction's own, under each isolation level; `T.locks`
   and `T.undo` do not grow; a rollback past a load is
   refused.
3. sm2's and sm3's loader, in batch mode.
   → verify: after a load the indexes and the heap agree with
   a brute-force scan, with a partial and an expression index
   among them; the next statement reads the rows.
4. Two sessions, as `rel` scripts and with `qa_nowait`.
   → verify: a reader passes a load in flight and sees none
   of it; a writer is blocked until it commits; an aborted
   load leaves nothing; a prepared load still blocks writers
   after a restart, and commits.
5. Mini-batch, through sm1, sm2, sm3 and `rel`.
   → verify: a writer of other rows goes on during the load; a
   second inserter of a unique key waits for the load's
   transaction, then fails or goes ahead as it commits or
   aborts; locks are held on unique keys only.
6. File mode for the heap: one ascending stream, into a table
   with rows, the index entries in batches.
   → verify: the rows invisible to others until commit and
   gone after an abort, before the close and after it; the
   transaction's reads and writes of the table refused while
   the loader is open; files left by a crash are deleted at
   open.
7. Sorted streams and the unique check, for an empty table.
   → verify: told apart from a table with rows, and from a
   new one already written to; runs and merge with a budget
   of a few rows; a duplicate inside the load is found.
8. File mode through sm2, sm3 and `rel`, and the small load.
   → verify: step 3's and step 4's checks again in file mode,
   for a load on each side of the threshold.

The fork's own work in §11 is outside this repository.

---

## 13. What was built first, and how it differs

File mode for an empty table was built before the modes §12
puts ahead of it: it is the mode that writes a load once, and
the one case that needs nothing of the others. It is §5
narrowed, with these differences.

**Which tables**

- **Any empty table**, not only one made or truncated in the
  transaction (§5.1). Empty means its store holds no version
  of any kind: a new table and a truncated one are empty, and
  one whose rows were deleted is not, until they are purged.
- **sm3 takes the table's lock exclusive**, to the end of the
  transaction, and then looks again that the store is empty.
  So no one else reads or writes the table while it loads.
  sm3 looks once before it asks for the lock, so that a load
  into a table with rows keeps no one out.
- **The lock is taken only if it is free at once**
  (`sm1::trylock`). A load never waits for it: with another
  transaction using the table, a reader included, the rows are
  inserted. Waiting would hold a load behind any reader, for a
  gain it can do without; and the loader already holds the
  table's lock to write, so two loads each waiting to make
  theirs exclusive would wait on each other.
- **sm1 checks the store is empty, and trusts the lock.** The
  covering-lock argument of §7.2 is not there.
- **A table with rows is loaded by `tbl_insert`**, a row at a
  time, through the same three calls: a host with many rows
  opens a load whatever the table holds.

**Which rows**

- **A row to be stored out of line ends the files.** A load
  has no stream for TOAST's chunks. The first row
  `rel_insert` would move a datum out of, or that is too long
  for a tuple, closes sm1's loader -- what was handed over is
  put into the table then -- and that row and every one after
  it are inserted by `tbl_insert`.

**The streams, and the checks**

- **The heap is one ascending stream, and each index one that
  is sorted**, a partial or an expression index among them,
  its entries made as `tbl_insert` makes them.
- **No stream is declared unique.** With the store empty and
  no one else in it, two equal keys are both the load's, in a
  unique index or not, so every stream is checked alike: a
  key equal to another fails with `SM_ERR_DUPKEY`, when the
  load closes.
- **A stream is a range of its own**, which the close checks:
  two that overlap fail with `SM_ERR_INVALID`.
- **A serializable load tells SSI of every key**, as any write
  does (§3.4); at the other levels nothing is told.

**The rest**

- **A small load is one write batch** (§5.7), with no batch
  mode beneath it: the keys are held in memory up to
  `load_small_bytes`, 4 MB, and a load closed before that is
  written in one RocksDB write.
- **While a load is open**, the transaction cannot commit, and
  its other writes to that store are refused: they could not
  be checked against keys still on their way.
- **A prepare is refused** after a load, as with one open: a
  `PREPARED` record names the keys written, and a load keeps
  none.
- **An abort after the close** leaves a table that was
  committed empty with the load's versions in it, dead, for
  the sweep to drop (§5.6); to every reader it is empty.
- **The files are temporary files.** Sorted files and runs
  alike are written under `base/pgsql_tmp/sm/load`, one
  directory a load, removed at its close or abort. §5.2 put
  them in a directory of the database; but a database is to be
  its directory and nothing else (`../dg/CODE.md`), and a load
  not yet closed is no part of it. `sm1::attach` empties the
  temporary instance, so nothing sweeps a database for them.
  RocksDB links a file in when the two directories are on one
  filesystem, and copies it when they are not.
- **The options** are `load_small_bytes`, `load_sort_bytes`
  and `load_file_bytes` (`include/sm_options.h`).
- **The calls**: `sm1::load_open`, `load_stream`, `load_put`,
  `load_close` and `store_empty`; `sm2::rel_load` and
  `idx_load`; `sm3::tbl_load_open`, `tbl_load_add`,
  `tbl_load_close`; `sm_tbl_load_begin`, `sm_tbl_load_add`,
  `sm_tbl_load_end`. `rel` has no verb for it yet.
