# BULKLOAD — loading many rows in one transaction

`COPY`, `CREATE TABLE AS`, a large `INSERT ... SELECT` and a
restore all put millions of rows into one table in one
transaction. Row at a time through `tbl_insert` that works,
and costs memory in proportion to the rows. This file is the
design of a loader that does not, in two modes: **batch
mode**, which writes rows a chunk at a time through RocksDB's
ordinary write path, and **file mode**, which writes them to
sorted files that RocksDB ingests whole.

It is a design and not yet built. What is said here of
RocksDB is from memory of its documentation, and the stack
builds on 8.9.1. §10 lists what is to be checked and measured
before any of it is relied on.

---

## 0. The proposal in brief

**One loader, two modes: batch mode and file mode**

- Both hold one lock on the table that keeps other writers
  out: SIX in batch mode, X in file mode. Under it, a lock on
  each row says nothing new, so none is taken.
- Both are one transaction. Every version carries its `xid`,
  and the load commits or aborts whole, by the one status
  record every transaction has.
- They differ in how the versions reach RocksDB.

| | Batch mode | File mode |
| --- | --- | --- |
| The table | any; rows are appended | empty: made or truncated in this transaction, and nothing written to it yet |
| Lock | SIX: readers pass | X: readers wait too |
| How rows are written | a chunk at a time, one RocksDB write batch each | into sorted files, ingested whole at the end |
| Log, memtable, compaction | all three, as any write | none |
| The transaction reads its rows | after each chunk | only after the close |
| A duplicate key is found | within its chunk | at the end, within the load only |
| Memory | one chunk | the sort budget |
| Meant for | `INSERT ... SELECT`, a `COPY` that appends, and any load that reads as it goes | a restore, `CREATE TABLE AS`, `TRUNCATE` then `COPY` |

**What both keep**

- Atomic. No reader sees part of a load.
- Every check a row gets today: `NOT NULL`, unique indexes,
  the host's evaluator for partial and expression indexes.
- Memory that does not grow with the load.

**What both give up**

- Other writers of the table wait until the load's transaction
  ends, and in file mode readers too. A load that must not
  block them uses `tbl_insert`.
- They insert. There is no bulk update or delete.

**What has to be built**

- sm1: a loader on a store, with both ways of writing; a
  sequence call that draws a run of values; a sweep of
  leftover files at startup.
- sm2: heap and index inserts that feed it.
- sm3: a loader on an open table.
- `include/sm.h` and `rel`: the calls, and a verb.

**What is not known**

- How much faster either is. Nothing has measured the present
  path, so §10 begins with that.
- Several of RocksDB's behaviours file mode leans on (§10).

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

## 3. What the two modes share

### 3.1 A table lock stands in for the row locks

A writer holds its table's schema lock IX, and a reader IS
(`sm3/SM3_DESIGN.md` §2). A loader in batch mode takes it
**SIX**. One in file mode needs it **X**, which it already
holds (§5.1); X covers SIX, so all that follows holds for it
too.

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
stands for a table. So sm1's loader, in either mode, is told
which lock covers it, and refuses unless the transaction
holds that lock SIX or X. The check is cheap and it is what
stops a caller that forgot the lock from writing unprotected.

### 3.2 The rows

A row handed to the loader gets what `tbl_insert` gives it,
by the same routines (requirement 3):

1. `NOT NULL` is checked.
2. A `rowno` is taken. The loader draws rownos from the
   table's sequence a run at a time, in place of one call a
   row. No one else draws from it while SIX is held, so a
   load's rownos ascend and are above every `rowno` the table
   has.
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
keys: for a chunk in batch mode, for the whole load in file
mode.

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
(`sm1/SM1_DESIGN.md` §6.5). A load lists nothing, in either
mode.

- **With no savepoint set** nothing is listed by any write,
  and there is nothing to decide.
- **With one set**, a rollback to it could not undo the load.
  Until `sm1/SAVEPOINT.md` is built, such a rollback is
  refused and the transaction is aborted, as sm1 already
  does for a savepoint whose undo list passed its cap
  (`SM_ERR_UNDOLIMIT`, `sm1/SM1_DESIGN.md` §6.5).
- **Once it is built**, a rollback marks a range of `seq`
  dead, and a load's versions are covered by it like any
  other. The refusal goes, with no code here.

A failed load needs none of this. The transaction aborts, and
an abort deletes nothing.

### 3.6 Indexes

A load maintains every index of the table, in either mode.
An index that is still building is maintained too, as any
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

A backfill batch holds the schema lock IX. So a load waits
for the batch in flight, and the build waits for the load.

`TRUNCATE` and then `COPY` in one transaction is the other
common way to fill a table. The truncate gives the table a new,
empty store under the schema lock X (`sm3/SM3_DESIGN.md` §4),
which is exactly what file mode requires (§5.1).

---

## 4. Batch mode

### 4.1 A chunk

The loader buffers rows and writes them a chunk at a time. A
chunk is bounded in bytes, a knob whose default is to be
measured.

For each chunk:

1. **Check the unique keys** (§4.2).
2. **Tell SSI** of each key (§3.4).
3. **Take one write timestamp and one `seq`** (§3.3).
4. **Write everything in one RocksDB batch**: the heap
   versions and the index versions together, logged and not
   synced, as every uncommitted write is.

Nothing is appended to `T.locks` or `T.undo`. What the loader
holds in memory is the chunk it is filling.

From then the transaction sees the chunk's rows, by the heap
and by every index, as it sees any of its writes. A caller
that must read what it has handed over calls `flush`, which
writes the chunk in hand.

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

**An index with nothing under its prefix needs no reads of
the table.** The loader looks once when it opens. No one else
can write the index while SIX is held, so the only keys it
can meet are its own.

A duplicate is reported when its chunk is written: within a
few thousand rows of the row that caused it.

### 4.3 Commit, abort, crash and prepare

Nothing here is new.

- **Commit** is the one status record. A commit with rows
  still buffered is refused; the loader is closed first.
- **Abort** writes nothing. The versions resolve as dead and
  compaction drops them.
- **A crash** before commit leaves no status record, which is
  an abort.
- **Prepare** records the locks the transaction holds, with
  their modes. The SIX schema lock is one of them, so after a
  restart an in-doubt load still keeps writers out, as the
  row locks would have.

### 4.4 What batch mode does not save

Every row is still written to RocksDB's log, then to a
level-0 file, then rewritten as compaction moves it down. It
saves the memory of §1 and the per-key lock, timestamp and
write call. It does not save the bytes written.

---

## 5. File mode

### 5.1 Only into an empty table, held X

File mode loads only a table whose store was made in the
loader's transaction, by `CREATE TABLE` or by `TRUNCATE`,
and holds nothing yet. Such a table is held X by that
transaction already. The loader checks both when it opens:
the table's store is not the one in its committed
definition, and one seek finds no key in the store. Anything
else is refused with `SM_ERR_INVALID`, and the caller uses
batch mode.

It covers the cases file mode is for: a restore, `CREATE
TABLE AS`, and `TRUNCATE` then `COPY`. A `COPY` that appends
to a table with rows goes to batch mode.

What the rule removes:

- **No unique check against the table.** Nothing is there to
  collide with; a duplicate can only be within the load, and
  the merge finds it (§5.4).
- **No overlap.** Every file overlaps nothing in the store, so
  RocksDB places each at the bottom of the tree, and no
  memtable holds a key in its range to be flushed first.
- **No other session at all.** X keeps readers out as well as
  writers, so nothing is read or written in the store but by
  the loader's own transaction.

What it costs: an append through `COPY` pays batch mode's
log and compactions, and readers of a table being filled wait
for the transaction. In the three cases above they wait
already, behind the X that made the store.

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
and the next one started.

### 5.3 Sorting an index

An index's entries arrive in the heap's order, not the key's.
It is an external sort, in the plain two-phase form.

1. **Runs.** Entries collect in memory up to a budget, a
   knob. When it is full they are sorted and written out as a
   run, and the memory is reused.
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

Only a unique index's key can collide, and in an empty table
only with another row of the same load. The merge finds it:
equal keys come out as neighbours. No key is read from the
store.

**All of this happens at the end.** A duplicate in the tenth
row of a hundred million is found after the last row is
handed over. PostgreSQL finds it at the tenth row. The work
lost is the price of sorting before checking.

### 5.5 Closing: ingestion

Closing the loader finishes the merges and then ingests every
file of the load in one call, which RocksDB applies
atomically.

- **Files are moved, not copied.** The load directory is on
  the database's filesystem so that RocksDB can link them in.
- **Every file overlaps nothing**, the store being new and
  empty (§5.1), so RocksDB places each at the bottom of the
  tree, where no compaction has reason to rewrite it, and
  flushes no memtable first.

From then the transaction sees its loaded rows, by the load's
`seq`, and other transactions do not, by its `xid`.

Before the close nothing of the load is in the database. The
transaction cannot read the rows it has handed over, by the
heap or by any index.

While a file loader is open on a table, the transaction's
ordinary writes to that table are refused. They could not see
the rows in the files, and the load's one timestamp (§3.3)
was taken before them.

### 5.6 Commit, abort, crash and prepare

- **Commit** is the one status record, as ever. A commit with
  a loader still open is refused.
- **Abort before the close** deletes the load's files. Nothing
  reached the database.
- **Abort after the close** writes nothing, as any abort. The
  store was made by the transaction, so the abort drops it
  whole, files and all.
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


### 5.7 A small load

Each ingestion adds files, and a file of a hundred rows at
the bottom of the tree stays a file of a hundred rows: no
compaction has reason to merge it with its neighbours. A
thousand small loads would leave a thousand such files.

The host cannot always know a load's size when it starts; a
`COPY` reads until its input ends.

**Decision proposed: a file load that ends small is written
as a batch.** The loader keeps its first rows in memory and
starts no file until they pass a threshold. If the load is
closed before that, the rows are written as one chunk of
§4.1. It is the reason the two modes are one loader.

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
| Condition | storage file created in this transaction, and `wal_level = minimal` | store created in this transaction, by `CREATE TABLE` or `TRUNCATE`, and empty (§5.1) |
| Contents logged | no | no: written to sorted files and ingested, past RocksDB's log |
| Made durable before commit by | `fsync`, or full-page images for a small file | RocksDB's ingestion, which records the files in its manifest before it returns (§10) |
| Abort or crash | the file is discarded | the store is dropped; after a crash the startup sweep drops it |
| Replication | must be off | no constraint: nothing replays RocksDB's log elsewhere |

The rule of §5.1 is what lines file mode up with PostgreSQL's
own. The last row is the one real difference: PostgreSQL gives
the saving up on any server with standbys, and wisr does not
have to.

---

## 6. Choosing a mode

The loader is told its mode when it opens. The choice is the
host's, by what the statement needs:

| The statement | Path |
| --- | --- |
| must not make other writers of the table wait | `tbl_insert` |
| writes a few rows | `tbl_insert` |
| reads the table as it writes: `ON CONFLICT`, a trigger that queries it | batch mode, flushing before each read, or `tbl_insert` |
| must report a duplicate near its row | batch mode |
| is large, and fills a table made or truncated in its transaction | file mode |

By statement, that is usually:

- **A restore, `CREATE TABLE AS`, `TRUNCATE` then `COPY`**:
  file mode.
- **A `COPY` that appends**: batch mode. So does a `COPY` into
  a table with triggers that read it, flushing before each,
  as PostgreSQL's own `COPY` falls back to one row at a time
  for such a table.
- **`INSERT ... SELECT`**: batch mode. The planner's row
  estimate says whether it is worth leaving `tbl_insert` for.

---

## 7. The interface

In sm3, on a table opened SIX or X for batch mode, and on an
empty table made in the transaction for file mode (§5.1):

```cpp
enum class load_mode_t { batch, files };

// Begin a load into table.
int tbl_load_open(table_t* table, load_mode_t mode, loader_t*& loader);

// Hand over one row. Its rowno is known at once.
int tbl_load_add(loader_t* loader, std::span<const sm2::datum_t> values,
                 std::span<const bool> isnull, uint64_t& rowno);

// Batch mode: write what is buffered, so the transaction can
// read it. Refused in file mode.
int tbl_load_flush(loader_t* loader);

// Finish: write the last chunk, or sort, check and ingest.
// The transaction is still to commit.
int tbl_load_close(loader_t* loader);
```

`include/sm.h` has the same four as `sm_load_*`, in its C
style. The chunk size, the sort budget, the file size and the
small-load threshold are options of the database, not
arguments.

**Errors.** `tbl_load_add` fails at once for what it can see
in the row: a NULL in a `NOT NULL` column, a key too long for
a unique index, an evaluator that fails. A duplicate key
fails the call that writes it: in batch mode a later `add`, a
`flush` or the `close`; in file mode the `close`. The message
names the index and the rownos. After any failure the
transaction is to be aborted.

---

## 8. What it costs

Both modes:

- **Writers wait for the whole transaction**, not for the
  load alone: the lock is held to commit, as every lock is.
  PostgreSQL's `COPY` and `INSERT` shut no writer out. This
  is the design's main price, and the reason `tbl_insert`
  stays the default.
- **A load is a long transaction.** It holds back garbage
  collection across the database for as long as any long
  transaction does.
- **No rollback to a savepoint set before the load**, until
  `sm1/SAVEPOINT.md` is built (§3.5).
- **A second way to write in sm1.** It shares the version
  format, the visibility rules and the commit with the
  ordinary one.

Batch mode:

- **The bytes are still written several times** (§4.4).
- **A duplicate is reported a chunk late.**

File mode:

- **A late duplicate wastes the load** (§5.4).
- **Only into an empty table**, held X: an append goes to
  batch mode, and readers wait (§5.1).
- **Nothing is readable before the close** (§5.5).
- **Disk for the sort**: for a moment, twice the indexes.
- **Ingestion pauses the database's writes for a moment**
  while RocksDB fits the files in. How long is to be
  measured.
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

## 10. To check and to measure, first

**Check**, against RocksDB 8.9.1 and the code:

- `SstFileWriter` writes with a custom comparator and the
  column family's options, and its files ingest into that
  column family;
- one ingestion call takes many files and applies them
  atomically, and files that overlap nothing land at the
  bottom of the tree, as §5.5 says;
- an ingested file is durable once the call returns, so that
  a commit record written after it never outlives its rows;
- what ingestion blocks, and for how long: writes to the
  store and to the database;
- an iterator or snapshot opened before an ingestion does
  not see the file, and a compaction filter sees its keys as
  it sees any other;
- a write batch of the chunk size is taken by RocksDB without
  a stall of its own;
- that no caller under IS can lock or write a key the loader
  inserts, beyond `tbl_lock` on a row that exists.

**Measure**, each with a small program and no PostgreSQL:

1. **The present path.** Load ten million rows through
   `tbl_insert`, with no index and with three.
   → report: rows a second, peak memory, bytes written to
   disk against bytes loaded.
2. **Batch mode.** The same load through a prototype of
   §4.1, at several chunk sizes.
   → accept: memory flat in the row count. Report the gain,
   and pick the default chunk size.
3. **The heap by ingestion.** The same rows written to files
   and ingested into an empty store.
   → accept: memory flat in the row count. Report the gain
   over 2, where the files land, and the pause of the
   ingestion.
4. **An index by ingestion.** Sorted with a budget far below
   its size, merged and ingested into an empty index.
   → report: the gain, the disk used, and the state of the
   tree afterwards.
5. **The small load.** Loads of ten rows to a million, in
   each mode.
   → pick the threshold of §5.7.

---

## 11. What has to be built

| Level | What | For |
| --- | --- | --- |
| sm1 | a loader on a store: the covering-lock check, the unique check, the SSI calls | §3 |
| sm1 | batch mode: one `wts` and `seq` a chunk, one write batch | §4 |
| sm1 | file mode: streams, files, the sort and the merge, ingestion; the load directory, emptied when the database opens; a loader's files deleted by abort | §5 |
| sm1 | commit and prepare refused with a loader open; a rollback refused past a load, until `sm1/SAVEPOINT.md` | §3.5, §4.3, §5.6 |
| sm1 | a sequence call that draws a run of values | §3.2 |
| sm1 | options: the chunk size, the sort budget, the file size, the small-load threshold | §4.1, §5 |
| sm2 | heap and index inserts that feed the loader in place of writing | §3.2 |
| sm3 | `tbl_load_*`, over `tbl_insert`'s own routines; file mode refused unless the table's store is new to the transaction and empty | §5.1, §7 |
| top | `sm_load_*` in `include/sm.h` | §7 |
| `rel` | a `load` verb, in each mode | testing |
| fork | each bulk statement choosing a path | §6 |

---

## 12. Order of work

Batch mode comes first: it is the smaller, file mode's small
loads need it, and it puts the lock, the checks and the
interface in place.

1. The checks of §10, and measurement 1.
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
   among them; rows are readable after a flush.
4. Two sessions, as `rel` scripts and with `qa_nowait`.
   → verify: a reader passes a load in flight and sees none
   of it; a writer is blocked until it commits; an aborted
   load leaves nothing; a prepared load still blocks writers
   after a restart, and commits.
5. Measurement 2.
6. File mode for the heap: one ascending stream.
   → verify: refused on a table with a committed store, and
   on a new one already written to; the rows invisible to
   others until commit and gone after an abort, before the
   close and after it; files left by a crash are deleted at
   open.
7. Sorted streams and the unique check.
   → verify: runs and merge with a budget of a few rows; a
   duplicate inside the load is found.
8. File mode through sm2, sm3 and `rel`, and the small load.
   → verify: step 3's and step 4's checks again in file mode,
   for a load on each side of the threshold.
9. Measurements 3 to 5.

The fork's own work in §11 is outside this repository.
