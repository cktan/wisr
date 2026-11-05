# INDEXBUILD: Index Builder

The Index Builder fills an index on a table that has rows, while
the table's readers and writers go on. Neither is shut out for
longer than a catalog change takes. There is one build, and it is
concurrent. The code is `sm3/src/builder/builder.cc` (the queue,
the workers, the wait), `tbl_index_backfill` in
`sm3/src/table/table.cc` (a batch), `create_index` and
`enqueue_pending_indexes` in `sm3/src/ddl/ddl.cc`,
`cat_put_index_state` in `sm3/src/catalog/ddl.cc`, `index_usable`
in `sm3/src/catalog/session.cc`, and `start_builder` in
`src/sm.cc`. It builds both kinds of index: entries and writes
are in LSMINDEX.md and BTREEINDEX.md.

## 1. Overview

### 1.1 A worked example

```sql
-- jobs holds a million committed rows, and writers are running.
CREATE UNIQUE INDEX jobs_owner ON jobs (owner);
```

| Who | Call | What it does |
| --- | --- | --- |
| session | `create_index` | takes the table-oid lock X, and records the index, pending |
| session | `commit` | publishes the definition; `sm1::on_commit` queues the index |
| worker | `begin`, `tbl_open` (IX) | starts a batch; a writer's IX goes on beside it |
| worker | `tbl_index_backfill(from "")` | adds the entries of the first 1000 rows, each under its row's shared lock |
| worker | `commit` | ends the batch, and lets go of its rows |
| worker | the same, from the last key on | until a batch finds fewer rows than it asked for |
| worker | `set_index_state(valid)`, `commit` | publishes the state, and waits for no one |
| session | `builder_wait` | returns 0; the next snapshot may scan the index |

Meanwhile a writer maintains `jobs_owner` as any index. If two
rows hold one owner, the batch that reaches the second fails the
build. `builder_wait` then returns `SM_ERR_DUPKEY`, and the index
can only be dropped.

### 1.2 Terms

- A **pending** index is maintained by every write, but is not
  yet filled from the rows that were there before it.
- A **building** index is a pending index that a worker is
  filling now. Only the Index Builder knows this, and nothing
  records it.
- The **walk** is the build's one pass over the rows, in key
  order.
- A **batch** is one transaction of the walk.
- A **worker** is a thread that runs builds.

## 2. The protocol

### 2.1 The danger

A writer that does not know the index exists changes the rows and
writes no entry. PostgreSQL's `CREATE INDEX CONCURRENTLY` meets
this by entering the index as not ready and not valid, waiting
out older transactions, scanning the table twice, and waiting
again. Here the table-oid lock does the waiting, and the row lock
stands in for the second scan.

### 2.2 The steps

1. **Create.** `create_index` takes the table-oid lock X, as
   every DDL does, and records the index as pending. A writer
   holds that lock IX to the end of its transaction. So when this
   commits, no writer is left that does not know the index. The
   next writer reads a definition that lists it.
2. **Maintain.** From then on, every writer maintains the index
   as any other, with one allowance. While the index is pending,
   an entry to remove that is not there is no error, since the
   walk may not have reached the row. A row a writer touches is
   covered from then on.
3. **Backfill.** A worker walks the rows in key order, in
   batches. Each batch is a short READ COMMITTED transaction
   under the table-oid lock IX. For each row it takes the row's
   lock shared, reads the newest committed version, and adds the
   entry if it is not there (§5).
4. **Validate.** When the walk reaches the end, the index is
   whole. The worker marks it valid in a transaction of its own,
   under the table-oid lock IX, so it waits for no one using the
   table.

### 2.3 Why the backfill is safe

The safety rests on the order a writer works in. A writer changes
the row before any index (LSMINDEX.md). Its uncommitted version
of the row stands until it commits, and the batch's row lock
waits for that. A writer in turn waits for the batch's row lock.
So a batch and a writer never work on one row at once:

- Either the batch waits for the writer, and then finds the entry
  already made.
- Or the writer waits for the batch, and then finds the entry to
  remove.

No second scan is needed, and no lock stronger than a row's.

### 2.4 Nothing to catch up

Some designs build an index beside the table, log the writes made
meanwhile, then lock the table exclusively to apply the log and
switch the index in. There the exclusive lock does real work.
This build has no such phase. Every writer has maintained the
index since `create_index` committed. So when the walk ends,
marking the index valid changes one fact about it and nothing in
it.

## 3. Index state

### 3.1 The states

| State | A write to the table | A scan |
| --- | --- | --- |
| pending | maintains the index; an entry to remove that is not there is no error | may not use it |
| valid | maintains the index; a missing entry is an error | may use it, if its snapshot is new enough (§4) |
| failed | leaves the index alone | may not use it |

A primary key is valid from the start: it has no entries to
build.

### 3.2 Who sets it

- `create_index` leaves an index pending, or valid if it filled
  it itself (§11).
- The Index Builder sets valid or failed when a build ends.
- Dropping the index, or its table, erases the state with it.

### 3.3 A state seen late does no harm

An index's state is like a statistic and unlike a definition. A
stale definition is corruption: a row decoded by last week's
columns. A stale state is not, because a state only ever leaves
pending:

| The index is | A transaction still sees | What it does | Harm |
| --- | --- | --- | --- |
| valid | pending | maintains it, tolerates a missing entry, does not scan it | none: a plan it could have had |
| failed | pending | goes on maintaining it | none: entries no one reads; a unique one may still refuse a duplicate, as it did while pending |

Pending is the reading that is always safe, and it is the only
stale reading there is. So no transaction has to be waited out
before a state is set.

A table handle makes this concrete. It notes each index's state
at `tbl_open` and goes by that until it closes (`load_states`,
`tbl_index_state`). A state set meanwhile reaches it at its next
open.

### 3.4 Why the state is kept by index

A table's definition is immutable and is replaced whole, by a
transaction holding the table's lock X. A state is set under IX,
while other transactions hold the definition, so it cannot be a
field of it. Two builds may also end on two indexes of one table
at once. If the state were part of the table's record, the second
to commit would write the first one's old state back. So:

- On disk an index's state is a catalog entry of its own, of kind
  `INDEX_STATE`, under the table's oid and the index's
  (CATALOG.md). A pending index has no entry: no entry is what
  pending means. `create_index` writes nothing for it.
- In memory it is in a map by index oid, beside the definitions
  and not in them (`catalog_t::index_states`).
- A temporary table's index keeps its state in the session
  (`overlay_state`).

### 3.5 Why IX, and not no lock at all

IX admits every reader and writer of the table and keeps out only
DDL, which takes X. That stops the index, or its table, being
dropped while its state is written, and an entry being left
behind for an index that is gone. `cat_put_index_state` takes it,
as a batch does.

### 3.6 Memory and the store never differ

Statistics are published after their commit returns, and may lag
the store. A state may not. The transaction that sets it holds
the change in its session's overlay, where it sees it at once.
`commit` publishes it under the catalog's mutex with the rest of
the transaction's DDL. So no reader of the catalog sees one
without the other. If the process dies between the two, the next
`cat_open` reads the store.

### 3.7 A state is set once

The argument of §3.3 rests on a state only leaving pending, so
`cat_put_index_state` enforces it. An index that is valid or
failed is `SM_ERR_INVALID` to set again, whatever to. Otherwise a
change from valid to failed, or back to pending, would be seen
late as valid, which is not the safe reading.

Two transactions that set one pending index at once meet on its
state's key. The second waits for the first, finds a state there
when that commits, and is refused. An index whose build failed is
dropped and made again. It is not reset.

### 3.8 What each guarantee rests on

Validating under the table-oid lock X would have given four
things. Each comes from elsewhere, and X would make the last step
of every build wait for the longest transaction using the table.

| X would give | Now given by |
| --- | --- |
| no transaction holds a stale state | a stale state is pending, which is safe (§3.3) |
| no old snapshot scans the new index | the count of publishes, for every transaction (§4) |
| the index is not dropped meanwhile | IX against DDL's X (§3.5) |
| two state changes do not overwrite each other | a state for each index, not each table (§3.4) |

## 4. Readers: the count of publishes

Entries are versioned as rows are, so a snapshot older than the
last batch sees an LSM Index partly filled. PostgreSQL waits such
transactions out. Here the harm is not a stale state but a state
newer than the snapshot, and no lock ever stopped it. The catalog
counts its publishes instead (`catalog_t::seq`), in memory only,
and records the count at which each index became valid
(`valid_seq`).

A session notes the count before it takes a snapshot, at `begin`
and at each statement of a READ COMMITTED transaction. What was
published by then, the snapshot sees whole. A session's
statements begin through sm3's `stmt_begin`, because that is
where the count is noted. A statement inside another saves the
outer note and gives it back.

`index_usable` decides, in this order:

1. If the index is not valid as the session sees it, refuse.
2. If it is not valid in the committed catalog, the session's own
   transaction made it valid: allow.
3. If the session is at READ COMMITTED and has begun no
   statement, each call reads at a fresh snapshot: allow.
4. If the catalog has no count for it, it was valid when the
   catalog loaded: allow.
5. Allow if the count at which it became valid is at or below the
   session's note.

- `tbl_index_usable` answers for a planner, and `tbl_idx_scan`
  and `tbl_idx_scan_rows` refuse an index that is not usable.
- The guard covers every transaction, one that has opened the
  table included, since marking the index valid waits for none of
  them.
- Writers maintain an index whatever their snapshot.
- The count needs no disk: no snapshot survives a restart.

## 5. Batches

A row moved between a batch's scan and its read is gone from its
key: `rel_read_latest()` gives `SM_ERR_MOVED`, and the build
treats that as a row deleted. Its mover made the entry for its
new key.

### 5.1 The walk

The walk is in the rows' key order (`tbl_scan_rows`), the primary
key's or the `_id`'s: one sorted run. A batch resumes from the
least key past the last row it covered, which is that key with
`\0` appended. Only the worker's memory keeps this. A B-tree
gains from sorted input. RocksDB sorts what it is given when it
flushes, and each row is read again under its lock whatever the
order.

`builder_attach` sets the batch's rows, 1000 by default.

### 5.2 One batch

`build_one` runs each batch in its own READ COMMITTED
transaction. It opens the table with the table-oid lock IX,
checks the definition still lists the index and that it is
pending, and calls `tbl_index_backfill`:

1. Refuse a table not opened to write, an index that is not
   pending, and a primary key.
2. Refuse a level other than READ COMMITTED, unless the table is
   fresh (§11). Only READ COMMITTED reads a newest version.
3. Open `tbl_scan_rows`, seek to `from`, and collect up to
   `max_rows` row keys. Close the scan.
4. For each row key, take the row's lock shared. The lock lasts
   to the end of the batch.
5. Read the row's newest committed version. If it is deleted, go
   to the next row.
6. Copy the tuple, since the index calls reuse session memory.
   Deform it, and fetch what the index needs out of line.
7. Make the entry. If the row does not belong in a partial index,
   go to the next row.
8. Add the entry. If it is there already, apply §5.3.
9. Set `done` if the batch found fewer rows than `max_rows`, and
   `next` to the least key past the last row.

An evaluator sees each row's newest image under the row's lock,
as the build reads any row. It runs on the worker's thread
(LSMINDEX.md, `../dg/EXPRINDEX.md`).

### 5.3 An entry that is there

`add_entry` fails with `SM_ERR_DUPKEY` when the key is there.
Adding only what is absent is what makes a restart safe.

- **The key holds the row key** (a non-unique index, or a key
  with a NULL). The entry is this row's, made by a writer. Go on.
- **The key holds no row key** (a unique index, no NULL). The
  entry may be another row's. Scan the index at the equal bound,
  and look for an entry that names this row.
- **An entry names this row.** Go on.
- **None does.** Two rows have one key. Fail with
  `SM_ERR_DUPKEY`, and put the index and this row's key in
  `sm1::errdup()`.

### 5.4 How a batch ends

| The batch | `build_one` |
| --- | --- |
| commits | goes on from `next`, or to `set_state` valid if `done` |
| finds the index gone or no longer pending | aborts, and ends quietly |
| fails with a retryable error | aborts, and runs the same batch again |
| is interrupted, or its catalog is detaching | aborts, and ends; the index stays pending |
| cannot open the table | ends quietly: the table has gone |
| fails in any other way | aborts, and marks the index failed, with the error |

The error is kept before the abort, which may not keep it. A
retried batch starts from the same `from`. Retries have no limit
and no delay.

## 6. Workers

The Index Builder is the process's: `g_builder`, one queue of
pending indexes over every catalog attached to it, and at most N
builds at once. N defaults to 4, and `builder_set_max_workers`
sets it. The C API has no call for it.

- **A worker is a thread**, started when a build is queued and
  fewer than N are alive. It exits when it finds the queue empty,
  so a process with nothing to build holds no thread.
- **A worker takes an index from the queue**, which makes the
  index building. It opens a session of its own on the index's
  database, through the function given at attach. Without a
  session the build is given up.
- **Each batch is a transaction of its own, and so is marking the
  index valid.** That last transaction holds no row.
- **One routine does a batch**, and tests call it directly.
- **A worker that dies of an exception gives the build up.** It
  has no caller to pass it to. The index stays pending.
- **A thread that will not start** leaves the index queued if
  another worker is alive. With none, the index leaves the queue
  and stays pending. `builder_enqueue` never throws, as it runs
  at commit.
- **Ended workers are joined** at the next `enqueue`, and at the
  last detach.
- **One mutex guards the builder.** A commit queues an index
  while it holds its catalog's mutex. So the builder's mutex is
  taken after a catalog's, and no call into a catalog is made
  under it.

The builder is a caller of the catalog. It reads and changes it
through the catalog's own calls, and the catalog knows nothing of
it. The DDL knows the queue. The builder does not look for work:
it is given a table and an index, checks the index is still
pending, and fills it.

**Queuing.** `create_index` gives `sm1::on_commit` a function
that calls `builder_enqueue`. So an index is queued once its
transaction has committed, and never if that aborts. The function
runs before the commit publishes the new definition. A worker
that takes the index at once waits for the publish when it opens
the table. An index is in the set `active` from queued to
finished, so it is never queued twice.

## 7. Attach and detach

**Attach.** `builder_attach` takes a catalog, a function that
opens an sm1 session on its database, and the batch's rows. A
catalog may be attached once. `start_builder` in `src/sm.cc`
attaches each catalog as its database opens, then calls
`enqueue_pending_indexes` (§8). An evaluator must be registered
before this, since a worker may start at once.

**Detach.** `builder_detach` comes before `cat_close`, and before
the database closes under the workers' sessions:

1. Mark the catalog stopping, so a commit on its way in is turned
   away.
2. Drop what is queued for it.
3. Raise the stop bit in each of its workers (`sm_interrupt`).
4. Wait until none of its builds is running.
5. Forget the catalog. If it was the last, join every worker.

- Each build ends after the batch it is in. The index stays
  pending, for the next attach.
- A worker that waits on another transaction, a prepared one
  perhaps, is interrupted. So a detach does not wait for that
  transaction.
- The stop bit stays until the worker takes it (`sm_take`). So a
  bit raised between two waits ends the next one. The worker
  takes the bit when its build ends, lest it end the next build's
  waits. The detach raises it under the builder's mutex, so never
  after the worker took it.
- **The detach's own wait is not cut short by an interrupt**,
  unlike every other wait in sm. A detach that returned first
  would leave a build using a catalog its caller is closing. The
  wait is bounded, since each build is interrupted and ends after
  its batch.
- A `set_state` under way gives up once its catalog is stopping.
  So a walk finished at that moment leaves the index pending.

## 8. Restart

A build unfinished at a restart starts again from the first row.
Adding only what is absent makes that safe, and no progress is
kept on disk.

The functions given to `sm1::on_commit` are lost in a crash. So
`enqueue_pending_indexes` walks the catalog when a database opens
and queues every index still pending. That is how a build
survives a restart. `builder_attach` must come first. A temporary
table is not in the shared catalog, and is never queued.

The same call finds the indexes a detach, a lost session or a
thread that would not start left pending. Nothing else retries
them.

## 9. Waiting for a build

`builder_wait` blocks until the index is valid, and fails if the
build failed. C API: `sm_cat_index_wait`. digres calls it after
the commit of `CREATE INDEX` (`smstore_wait_indexes()`).

- **It waits on the index's state, not on a thread.** So any
  session may wait, for a build another session or an earlier run
  of the process began.
- **It runs outside a transaction**, so that the caller's next
  snapshot is new enough to scan the index. Inside one it fails
  with `SM_ERR_INTRANS`.
- **An interrupt ends the wait.** A bit of the thread's wait mask
  ends it with `SM_ERR_INTERRUPTED`, and the bit stays for the
  caller to take.
- **A pending index with no build coming** fails with
  `SM_ERR_INVALID`: a build a detach cut short, or one given up.
  An index is queued before it is published, so one seen pending
  and not active has none coming.
- **A missing table or index** fails with `SM_ERR_NOTFOUND`.

The wait reads a generation count first. Any change after that
wakes it, so it cannot miss the end of a build.

## 10. A failed build

Any error from a batch that is neither retryable nor an interrupt
fails the build: a duplicate, a key too long for a unique index,
an evaluator's failure, or anything else.

- **A unique index finds some duplicates late.** A duplicate of a
  row not yet reached is found only when the walk reaches that
  row and its entry meets another row's. PostgreSQL likewise
  leaves an invalid index behind.
- **A failed build keeps its reason.** The state keeps the
  error's code and message, and for a duplicate the key of a row
  that holds it, across restarts.
- **`builder_wait` returns them** to whoever waits, the row's key
  in `sm1::errdup()`, so the host can read the row and show the
  key.
- **A failed index is not maintained and cannot be scanned.** It
  can only be dropped, which erases the entries it has.
  `SM_ERR_KEYTOOLONG` and an evaluator's error are kept the same
  way.

## 11. A build in the session

No other transaction sees a table this one made, so the build has
nothing to wait for. `create_index` runs the backfill itself over
the rows the transaction has written, and records the index valid
at once. It is the same routine, called from a second place, not
a second build.

- It runs when the table is absent from the shared catalog
  (`cat_get_table`). A temporary table is always absent, so its
  indexes are always built this way (TEMPTABLE.md).
- It opens the table exclusive, in batches of 1024 rows.
- `table_t::fresh` says so: `tbl_index_backfill` reads each row
  at the snapshot and takes no row lock, at any isolation level.
- A failure fails `create_index`, with `SM_ERR_DUPKEY` for two
  rows with one key.

A primary key is never built: its entries are the rows. Adding
one to a table with rows rewrites the table (LSMINDEX.md).

## 12. Costs

- **A writer above READ COMMITTED can lose a race with the
  walk.** One that updates a row backfilled after its snapshot
  gets `SM_ERR_CONFLICT` from the LSM entry, though the row had
  not changed, and retries.
- **Uniqueness is not enforced against a row the walk has not
  reached.** So a build can fail late, over a row committed while
  it ran.
- **Four workers are not four times the speed.** Builds contend
  on sm1's lock table and on RocksDB's write path. The default is
  unmeasured.
- **Two indexes on one table walk its rows twice**, where one
  pass could fill both.
- **A table's oid is a hot name.** Every reader and writer of the
  table locks it, batches included, and one name is one partition
  of the lock table. A benchmark is likely to point there first.
- **A restart repeats the whole walk.** Rows already covered cost
  a read and a refused insert each.
- **A batch holds a row lock for each row**, until it commits.
  `batch_rows` bounds that.

## 13. Implementation

### 13.1 `sm3/src/builder/builder.cc`

- `target_t` is an attached catalog: its session function, batch
  rows, `stop`, and `busy`, the workers building one of its
  indexes.
- `task_t` is one build: catalog, table, index.
- `builder_t` holds `mu`, `cv`, `gen`, `max_workers`, `catalogs`,
  `queue`, `active` (queued or being built), `building`,
  `workers` (each build's thread, for a detach to interrupt),
  `running`, `threads` and `finished`.
- `changed()` bumps `gen` and wakes the waiters.
- `stopping()` says a catalog is detaching or gone.
- `set_state()` sets a state in a transaction of its own, again
  after a retryable error, until the catalog stops.
- `build_one()` is §5.2's outer loop and §5.4's table.
- `work()` is a worker's loop: take a task, open a session, run
  `build_one`, close, repeat.
- `enqueue()` adds to the queue, joins ended workers, and starts
  a worker if it may.

Public: `builder_attach`, `builder_detach`, `builder_enqueue`,
`builder_set_max_workers`, `builder_building`, `builder_wait`
(`sm3.h`).

### 13.2 Elsewhere

- `tbl_index_backfill` in `sm3/src/table/table.cc` is §5.2 and
  §5.3. It shares `fetch_for`, `make_entry` and `add_entry` with
  a write.
- `remove_entry` there is §2.2's allowance.
- `create_index` in `sm3/src/ddl/ddl.cc` chooses §11 or the
  queue. For a tree it first makes the empty tree.
- `set_index_state` is `cat_put_index_state` and nothing more.
  `cat_put_index_state` is §3.5 and §3.7.
- `publish()` in `sm3/src/catalog/session.cc` records `valid_seq`
  for an index that became valid, under the count of that
  publish.

## 14. Cases

| Case | What happens |
| --- | --- |
| TOAST | `fetch_for` reads the row's values, so an entry holds no pointer |
| Non-unique | an entry already there is the row's: go on |
| Unique | a key another row holds fails the build (§5.3, §10) |
| Primary key | no build (§11) |
| Expression, partial | the evaluator runs on the worker's thread; a row that does not belong is skipped; its failure fails the build |
| INCLUDE | taken from the same image |
| Cut keys | non-unique cut as in a write; unique fails with `SM_ERR_KEYTOOLONG` |
| Update A→B→A during a build | the writer's own entries; a remove that finds none is no error; the batch finds the entry or adds it, after the writer ends |
| INCLUDE-only update | as any update: remove tolerated, add made |
| Primary key change | the mover makes the new key's entry; a batch that reaches the old key between its scan and its read fails the build (§5) |
| Delete then reinsert | the batch skips a row deleted under it; a new row's writer made its entry |
| Writer aborts | its entries die with it; the batch waited on the row, so it reads what stands |
| Savepoint rollback | as an abort, for the writes it undoes |
| Undo-cap overflow | a batch sets no savepoint, so it has none to lose |
| Crash | the index stays pending, and the build restarts from the first row (§8) |
| 2PC across restart | a prepared writer holds its row, and a batch waits for it; a detach interrupts that wait; DDL cannot be prepared |
| Two writers | as for any index; the batch settles with each on the row |
| Two builds on one table | each its own state, walk and worker |
| Old RR snapshot | kept off the index by the count (§4); it still maintains it |
| RC re-read | the writer's, unchanged |
| SSI | a serializable reader cannot use the index before its snapshot allows; a batch is READ COMMITTED |
| Open cursor | a cursor on the table is unaffected; the index appears at the next snapshot |
| GC watermark | a batch's entries are ordinary versions |
| `CF:txn` sweep | as for any committed version |
| `DROP INDEX` during a build | waits for the batch under X; the next batch finds it gone and ends quietly |
| `DROP TABLE` during a build | the next batch cannot open it, and the build ends quietly |
| `TRUNCATE` during a build | the state carries over; the next batch reads the new store from `from`; writers covered every row since |
| `ALTER` rewrite during a build | `copy_rows` fills a pending index in the new store, so later batches find entries there; a duplicate fails the rewrite, not the build |
| Temporary table | built in the session (§11) |
| Keyless (`_id`) table | the walk is in `_id` order |
| Bulk load during a build | a load into a table with rows inserts each row; an empty table's load needs the lock X now, and a batch's IX turns it to inserts |
| Bulk load and rollback | as a writer's abort |

## 15. Rejected

- **A build under the table-oid lock S**, as PostgreSQL's plain
  `CREATE INDEX` does, in one transaction. It is simpler, but
  shuts writers out, and is a second build to keep. Its one
  transaction holds a lock and an undo entry in memory for every
  row.
- **PostgreSQL's two scans and waits.** The table-oid lock and
  the row lock do the same work (§2.1).
- **A side log, applied under an exclusive lock.** This build has
  nothing to catch up (§2.4).
- **Validating under X.** The last step of every build would wait
  for the longest transaction using the table (§3.8).
- **The state in the table's record.** It is set under IX, and
  two builds could overwrite each other (§3.4).
- **A state that can change again.** A change from valid would be
  seen late as valid (§3.7).
- **A thread for each build.** Less code, but a restore that
  creates hundreds of indexes would start hundreds of threads,
  and nothing would restart a build after a crash.
- **Progress on disk.** A restart repeats the walk, and adding
  only what is absent makes that safe.
- **Building on the session's thread** to keep the evaluator off
  the workers. It is a second build path, and a `CREATE INDEX`
  that holds its session (`../dg/EXPRINDEX.md`).

## 16. Tests

- **mvcc:** none. sm1 knows no index or build.
- **sm3:** `sm3/test/test_table.cc` checks each index against a
  brute-force scan of the rows.
  - Batches: "a build fills an index in batches, and it then
    agrees".
  - Row races, from either side: "a build waits for a writer", "a
    writer waits for a build", "a deleter waits for a build", "a
    build skips a row deleted under it".
  - Failure: "a unique build fails on a duplicate it reaches
    late", "a failed index is not maintained", "a failed build
    keeps its reason", "a build fails on any error the rows
    cause".
  - The session's build: "an index made in its table's
    transaction is filled at once".
  - The guard: "a snapshot older than a build is kept off the
    index", "read committed may use an index from its next
    statement on", "after a restart every valid index is usable".
  - The background: "writers go on while an index is built",
    "more builds than workers all end", "a build left unfinished
    is queued when the builder starts", "a build that waits on a
    transaction holds no one else up".
  - Evaluators: "an evaluated index is built in the background,
    and inline", "an evaluated build and a writer settle on the
    row's lock".
- **sm3, state:** `test_lock.cc`: marking an index valid returns
  at once with a reader and a writer on the table; two sessions
  set two indexes of one table in transactions open together; a
  handle opened before still has the index pending; a REPEATABLE
  READ transaction that opens the table again sees it valid and
  still may not scan it; of two transactions setting one index's
  state, the second waits and is refused. `test_ddl.cc` has every
  refused transition, and a state surviving a restart.
- **sm3, others:** `test_toast.cc` (a build reads values out of
  line), `test_cluster.cc` (a build on a keyed table),
  `test_tree.cc` (a tree's build).
- **Two sessions on one thread:** a step that would wait is
  scripted with sm1's `set_qa_nowait`. What needs real threads,
  the background build, has them.
- **rel:** `rel/test/test_build.rel`, `test_partial.rel`.
- **dg:** `dg/test/server.sh` (a build a crash cut short, resumed
  on an adopted worker, and a duplicate found by a worker),
  `dg/test/slt/expr_index.test`.
- **Not tested:** a row moved between a batch's scan and its read
  (§5).
