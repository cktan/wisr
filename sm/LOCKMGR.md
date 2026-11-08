# LOCKMGR: the Lock Manager

The Lock Manager puts transactions in order where they meet on
one thing. It keeps row locks and object locks, finds deadlocks,
and makes a write's check-then-put one step. It also decides what
a write does when it meets another writer of its key. The code is
in `sm1/src/lockmgr.cc`, `sm1/src/locktab.cc` and
`sm1/src/locktab.h`; a write's half of the protocol is in
`sm1/src/access.cc` and `sm1/src/chain.cc`. The API is in
`sm1/include/sm1.h`, and the modes are in `include/sm_lock.h`.

**Warning.** The scenarios in `sm1/mvcc` are the specification of
this module. A change here that makes one fail is presumed wrong.
`lock_row_hash` and the values of the modes and types are
persisted in a `PREPARED` record: they must give the same result
in every build.

## 1. Design

### 1.1 A worked example

Table `t` has oid 16384 and store 1001. It has no primary key,
and `k` is the key of its row with `_id` 7. Three transactions
run at READ COMMITTED. A table's store is chained, so sm2 calls
the `chain_` form of each row call; each form is the same
protocol.

```
T1: SELECT * FROM t WHERE id = 7 FOR UPDATE
  sm1::lock_oid(s1, 16384, SM_LOCKMODE_IS)    a slot of s1's own
  sm1::lock_row(s1, 1001, k, SM_LOCKMODE_EX, true, 1)
    take the latch of (1001, k)
    no version in flight, no holder: lock_try takes the lock
    let the latch go

T2: UPDATE t SET n = 1 WHERE id = 7
  sm1::lock_oid(s2, 16384, SM_LOCKMODE_IX)    a slot of s2's own
  sm1::update(s2, 1001, k, ...)
    take the latch of (1001, k)
    lock_write_check finds T1's lock: let the latch go
    lock_wait sleeps on the partition's lock_cv

T1: COMMIT
  lock_release_all drops the hold and wakes lock_cv

T2: wakes, takes the latch, finds no lock and no version in
    flight, puts its version, lets the latch go. It keeps no lock.

T3: UPDATE t SET n = 2 WHERE id = 7
    takes the latch, finds T2's version in flight, lets it go
    lock_wait_xact sleeps on xact_cv

T2: COMMIT
  lock_wake notifies xact_cv

T3: wakes, takes the latch, finds a commit after its begin
    the write fails with SM_ERR_UPDATED; T3 now holds k X
  sm1::read_latest(s3, 1001, k)    T2's row, and the mark set
  sm1::update(s3, 1001, k, ...)    goes through; the mark clears
```

### 1.2 Three mechanisms, one rule

The word "lock" in this stack means one of three things.

- **A lock-table entry.** `sm1::lock_row` and `sm1::lock_oid` put
  a hold in a shared table, in one of Gray's modes, held to the
  end of the transaction.
- **An uncommitted version.** A write leaves its version in the
  store and keeps no entry. The next writer of the key finds the
  version, waits for the transaction that wrote it, and starts
  again. This is the rule of PostgreSQL, whose tuple carries its
  writer's transaction id.
- **A latch.** A mutex hashed on store and key, held only inside
  a call, so that check-then-write is one step.

The rule decides which applies:

> Write the key and the version is the lock. Need order on
> something you are not writing, and you need a lock:
> `sm1::lock_row` on a key, `sm1::lock_oid` on an object.

A transaction's memory therefore does not grow with the keys it
writes. A load of a million rows with three indexes holds no lock
entries, where a held write lock would cost four million.
Mini-batch needs no lock path of its own
([`BULKLOAD.md`](BULKLOAD.md#45-mini-batch) §4.5).

A plain read takes no lock. Nothing on disk records a lock but
the `PREPARED` record.

### 1.3 What a lock names

**A lock is its id, not its key.** An id is a type and two 64-bit
words. A row lock's id is `(LOCKTYPE_ROW, storeid, hash(key))`.
An OID lock's id is `(LOCKTYPE_OID, 0, oid)`, and it is exact.
The type is the top bit of the first word, so two locks of
different types never conflict, however equal their words.

The mode stands beside the id, not in it. Every mode of one name
must meet in one table entry, or S and X would never conflict.

No part of the Lock Manager keeps a row's key: not the table, the
lock list, the `PREPARED` record or `sm1::lock_status`. A hold of
a million rows with long primary keys copies no key bytes.
Conflicts are decided on the hash, so two rows of a store whose
keys hash alike are one lock (§4.9). It is a false conflict,
never a missed one, and with 64 bits a workload does not meet it.

**The caller passes the name whole.** sm1 does the hashing.

- `sm1::lock_row`: `storeid` is the row's store and `key` its sm1
  key, as `sm1::read` takes them. `rel_lock` on a row of table
  store 12, relid 1, calls `sm1::lock_row(s, 12, key,
  SM_LOCKMODE_EX, false, 1)`.
- `sm1::lock_oid`: the oid itself, in no store. `cat_lock_table`
  on table 16384 calls `sm1::lock_oid(s, 16384, SM_LOCKMODE_IS)`.

**The kind.** `sm1::lock_row` takes a `uint16_t kind`, 0 by
default, which sm1 keeps beside the id and never reads. sm2
passes the key's relid. It goes in the lock list entry, in the
`PREPARED` record and in `sm1::lock_status`. So `sm_lock_status`
in `src/sm.cc` tells a row's lock from an index entry's with no
key: a kind of `sm3::heap_relid` in a table's store is
`SM_LOCKNAME_ROW`. A lock already held keeps the kind it was
taken with. A write gives no kind. A write waiting on a lock
lists its holder's kind, and the lock a refused write takes
(§1.9) has kind 0 and is listed as a key's. `pg_locks` reports a
row lock by its table, not by which row.

**One type serves every object.** Every sm3 object draws its oid
from the one sequence `sm3.oid`, so an oid names one object of
any kind.

### 1.4 Modes and conversion

`sm_lockmode_t` is Gray's five, for locking at several
granularities, defined once in `include/sm_lock.h` and shared by
sm1 and the C wrapper. Its values are persisted.

| Held \ asked | IS | IX | S | SIX | X |
| --- | --- | --- | --- | --- | --- |
| IS | ok | ok | ok | ok | no |
| IX | ok | ok | no | no | no |
| S | ok | no | ok | no | no |
| SIX | ok | no | no | no | no |
| X | no | no | no | no | no |

sm1 keeps no hierarchy. A caller that makes one name stand for a
larger object takes an intention mode on it. So sm3's readers
(IS) and writers (IX) of a table coexist, and a builder that must
stop writers but not readers takes S.

**A conversion.** A transaction that holds a name and asks a mode
its hold does not cover converts to the weakest mode covering
both (`lock_cover`). IS is below IX and S, those two meet at SIX,
and X is above all. It waits for the other holders that conflict
with the new mode, then changes its hold in place. It never
releases what it holds. A write converts its own explicit lock to
X.

A conversion is what makes two transactions on one key deadlock
(§4.6).

### 1.5 The calls

```cpp
int sm1::lock_row(session_t*, uint64_t storeid, std::string_view key,
                  lockmode_t, bool as_read = false, uint16_t kind = 0);
int sm1::lock_oid(session_t*, uint64_t oid, lockmode_t);
```

`sm1::trylock_row` and `sm1::trylock_oid` take the same lock if
it can be had at once. They return 1 with nothing taken if not.
SQL's `NOWAIT` and `SKIP LOCKED` ask this way, and a bulk load
asks for its table so. `sm1::chain_lock_row` and
`sm1::chain_trylock_row` are the forms for a chained key.

`sm1::lock_oid` reads no store, checks no version and takes no
latch. It takes a table entry or, in IS or IX, a slot of the
session's own (§3.4).

`sm1::lock_row` follows these steps:

1. Check for an active transaction, a known mode, and a live
   store and legal key at the view.
2. In a temporary store, return 0: there is no one to lock
   against ([`TEMPTABLE.md`](TEMPTABLE.md)). With `as_read`, a
   row changed since
   the statement began is refused first.
3. Take the key's latch.
4. Read the newest version of the key.
5. With `as_read`, fail with `SM_ERR_SELFMODIFIED` if this
   transaction wrote that version after the statement began.
6. If no other transaction's version is in flight and `lock_try`
   succeeds, let the latch go and go to step 9.
7. Let the latch go. A try form returns 1 here.
8. Wait for the writer in flight, or for the holders that
   conflict, and go to step 3.
9. Above READ COMMITTED, run the currency check of §1.6.

**`as_read`** says the running statement read the row, as `FOR
UPDATE` and `FOR SHARE` do. PostgreSQL's `FOR UPDATE` passes over
a row its transaction changed later in the statement, and
`SM_ERR_SELFMODIFIED` is not an abort. Without `as_read` no such
check runs: a caller locks a key to read its newest version, its
own included.

An explicit lock waits for an uncommitted version, as `FOR
UPDATE` in PostgreSQL waits for an uncommitted update. The check
costs a read of the key.

`sm1::wait_writer` waits for a transaction `sm1::read_newest`
named: a unique tree's check uses it
([`BTREEINDEX.md`](BTREEINDEX.md)). It is the
wait of §1.11 on its own, with no lock.

### 1.6 The currency check

A successful `sm1::lock_row` means that what the caller reads
next under it is current. That makes "lock, then read, then act"
safe.

Above READ COMMITTED, the call fails with `SM_ERR_CONFLICT` and
aborts when another transaction committed a version of the key
after this transaction's snapshot. At READ COMMITTED no check
runs. The call returns 0 with the lock held, and the caller reads
the key with `sm1::read_latest`, which requires that lock. T1
commits an update of `id = 7` after T2's snapshot, and T2 runs
`SELECT * FROM t WHERE id = 7 FOR UPDATE`:

```
T2 at READ COMMITTED:
  sm1::lock_row(t, key(7), X)  -> 0, no check
  sm1::read_latest(t, key(7))  -> T1's version
T2 at REPEATABLE READ:
  sm1::lock_row(t, key(7), X)  -> SM_ERR_CONFLICT, and T2 fails
```

The levels differ in where staleness is caught, not in whether it
is. `sm1::lock_oid` runs no such check: no version of an oid can
exist.

The check reads no version of its own. It uses the `commit_ts`
of the read under the latch, step 4 of `sm1::lock_row`. No
version can come between that read and the check:

- The lock is taken only with no other version in flight.
- Every writer calls `lock_write_check` under the latch
  (`sm1::insert`'s steps, in §1.7). With the lock held, the writer
  waits.

### 1.7 A write

The write path is [`ROWSTORE.md`](ROWSTORE.md)'s. This section
gives its
protocol with the Lock Manager. `sm1::insert`, `sm1::update` and
`sm1::erase`, and their `chain_` forms, do these steps:

1. Take the latch of `(storeid, key)`.
2. Unless the store is temporary, call `lock_write_check`. If
   another transaction holds a lock that conflicts with a write,
   let the latch go, call `lock_wait` for X, and go to step 1.
3. Read the newest version of the key. If another transaction
   wrote it and still runs, let the latch go, call
   `lock_wait_xact`, and go to step 1.
4. Make the checks: `SM_ERR_SELFMODIFIED`, the write-write rule
   of §1.8, `SM_ERR_DUPKEY`, `SM_ERR_NOTFOUND`, `SM_ERR_MOVED`.
5. Take `wts` and put the version.
6. Let the latch go.

`lock_write_check` takes the partition's mutex for a look-up, and
adds nothing. A key with no entry is the common case. If the
transaction holds its own lock on the key, it converts to X here.

Step 3 is what replaces a held lock. It needs the three-way
status `xct_status` gives: a version whose writer aborted or
crashed is skipped. Step 2 keeps a write behind an explicit lock.
Such a lock is a `FOR SHARE` of the row, a foreign key check on a
parent row, or an index build's shared lock on a row it
backfills.

`sm1::insert_new` is an insert of a key that no version was ever
written for, as sm2 knows of a key made of a row's `_id`. It
leaves out step 3's read. Step 2 stays. A debug build makes the
read and asserts it finds nothing.

**A transaction's own version.** A write skips the write-write
rule when the newest version is its own. No other transaction can
commit the key while that version is in flight, because every
other writer waits for it. Versions undone by `sm1::rollback` are
invisible, and the check falls through to the committed version.

**Versions sort in commit order.** A second writer takes `wts`
only once no other running transaction has a version of the key.
So the first writer ended. If it committed, it took its
`commit_ts` before it left the active table, and the second `wts`
comes later from the same counter (`assert(commit_ts < wts)`).

### 1.8 The write-write rule

A write-write conflict is two transactions writing one key. The
wait of step 3 covers the concurrent case. Step 4 covers a commit
after this transaction's snapshot. Together they are the standard
snapshot-isolation write rule.

| Level | A commit after the snapshot | Result |
| --- | --- | --- |
| REPEATABLE READ, SERIALIZABLE | first committer wins | `SM_ERR_CONFLICT`; the transaction aborts; retryable |
| READ COMMITTED, update or delete | the caller must re-read (§1.9) | `SM_ERR_UPDATED`; no abort |
| READ COMMITTED, insert | no check | goes ahead against the newest version |

An insert has no predicate to evaluate again. It is judged
against the newest committed version, and `SM_ERR_DUPKEY` if that
is live, as PostgreSQL's unique check does. Writes at the
non-serializable levels still feed the SSI tracker, and those
levels are never its victims ([`XCTMGR.md`](XCTMGR.md)).

### 1.9 READ COMMITTED: write, re-read, write

**Why.** PostgreSQL's executor re-reads a row after a lock wait,
and clients written for it have no retry loop. Two READ COMMITTED
sessions each add one to `R1`, which holds 0. T1 updates it and
T2 waits for T1:

| | T2's update, after T1 commits | R1 |
| --- | --- | --- |
| PostgreSQL | re-reads R1 (1), checks `WHERE` again, writes 2 | 2 |
| sm1 | `SM_ERR_UPDATED`; T2 reads the latest (1), checks again, writes 2 | 2 |

In the variant `UPDATE R1 SET n = 1 WHERE n = 0`, T2's check must
fail and T2 must write nothing. So letting the write through is
not enough: the caller has to look at the newest version first.

**The exchange.** The update or delete waits for any writer in
flight, as every write does. It then fails with `SM_ERR_UPDATED`,
and takes the key's lock X before it returns. This is the one
write that leaves a lock behind. It holds the newest committed
version still between the re-read and the repeated write.
`sm1::retryable()` is 0 and the transaction stays active. The
caller calls `sm1::read_latest`, evaluates its `WHERE` clause and
new values from the row, and repeats the write. The executor's
side belongs to `dg/`.

**The re-read is enforced.** `sm1::read_latest` marks the lock
entry (`latest`), and the write lets itself through only on a
marked lock.

| Event | Effect on the mark |
| --- | --- |
| first write refused with `SM_ERR_UPDATED` | lock taken, mark not set |
| `read_latest` on that row | mark set |
| write repeated with the mark set | write goes through; mark cleared |
| write repeated without the mark | refused again with `SM_ERR_UPDATED` |
| rollback to a savepoint taken before the lock | lock released, mark gone with it |

- The mark is one bit on the session's own lock list, so no mutex
  guards it. Only the exclusive holder reads or sets it.
- A successful write clears it. Left set, a later write would
  pass the check with no fresh read.
- The mark is single-use. A later write needs no read while the
  newest version is the transaction's own. It needs one again if
  a rollback to a savepoint set after the lock undoes those
  writes.
- sm1 forces the read, not the evaluation. Whether the caller
  evaluates its predicate against the row is the caller's.

**`sm1::read_latest`** returns the newest committed version past
the snapshot. Something must keep that version from changing:

- At READ COMMITTED it needs one of three: a lock on the key in
  any mode, a write of the key still in effect, or a temporary
  store. Otherwise it fails with `SM_ERR_INVALID`.
- Above READ COMMITTED it reads only a version that is the
  transaction's own. An insert's arbiter finds so a key its own
  statement took (scenario `a19`).
- It sets the mark only under X. A shared holder cannot write
  without a conversion. A caller that waited in `sm1::lock_row`
  shared, as sm3's index build does, uses it to see what
  committed meanwhile.
- It fails with `SM_ERR_MOVED` for a row moved to another key,
  where it returns 1 for a plain tombstone. So a writer past
  `SM_ERR_UPDATED` learns the row moved, and does not skip it
  ([`ROWSTORE.md`](ROWSTORE.md)).

Nothing else changes for this exchange. REPEATABLE READ,
SERIALIZABLE, SSI and 2PC are as they were. The garbage collector
is too: a READ COMMITTED transaction already holds its begin
snapshot, and the newest committed version is never collected
([`GC.md`](GC.md)). Version order needs no help: the write that
goes ahead
takes a `wts` after the lock, so it sorts first.

### 1.10 Lifetime

- A lock lasts to the end of the transaction, or to an
  `sm1::rollback` past the point it was taken. Release is strict,
  so there are no cascading aborts.
- **A conversion is never undone** by a rollback. The name stays
  locked in the stronger mode until the transaction ends.
- A re-acquire of a held name adds no list entry, so a rollback
  releases only what was first taken after the savepoint.
- A rollback to a savepoint releases explicit locks alone: there
  are no write locks. It wakes the waiters, who find the deleted
  versions gone.
- A read-only transaction can hold locks, so its commit path
  releases them too.
- A prepared transaction keeps its locks across a restart (§3.9).

### 1.11 Waiting

A session waits on a lock, or on a transaction.

- **A wait on a lock** ends when no other holder conflicts. It
  takes nothing: the caller starts again. The table keeps no
  queue, so a woken waiter repeats its whole request. So a strong
  lock can wait behind weak locks that come after it.
- **A wait on a transaction** ends when the transaction commits
  or aborts, or when a rollback to a savepoint runs, which may
  have deleted the version. So a rollback that removes the
  version lets the writer through with no wait for the whole
  transaction.

**Every wait can be cut short.** sm1 waits through `sm_wait()`
([`sm0/INTERRUPT.md`](sm0/INTERRUPT.md)). A bit in the thread's
wait mask ends the
wait with `SM_ERR_INTERRUPTED`: not an abort, with the
transaction left active, as after `SM_ERR_BLOCKED`. The interrupt
goes to a thread, because it is the thread that blocks. sm1 takes
no bit, so the bit stays pending until the host takes it and
every later wait fails at once too. An interrupt is then never
lost to a wait that began a moment late. A host builds query
cancel, statement timeout and lock timeout from this. Two waits
of a commit past its record cannot be cut short
([`XCTMGR.md`](XCTMGR.md)). A
pin's wait is the Buffer Manager's ([`BUFMGR.md`](BUFMGR.md)).

**`sm1::set_qa_nowait`, for QA only.** It turns a wait into a
failure: a write, an `sm1::lock_row` or an `sm1::lock_oid` that
would block returns `SM_ERR_BLOCKED` at once. It is not an abort
and not retryable. The transaction holds exactly what it held,
and the caller polls by repeating the call. This lets one thread
play several sessions, and a script say "this step blocks" with
no thread and no sleep ([`rel/REL.md`](rel/REL.md)). The price is
deadlock
detection: a polling session records no edges and is never a
victim, so sessions polling each other poll for ever. That cycle
is the caller's to break.

### 1.12 Deadlocks

Deadlock is reported, not avoided. The stack has no lock ordering
rule. `SM_ERR_DEADLOCK` is retryable, and the caller retries.

Deadlocks are found as PostgreSQL finds them. A waiter that has
slept `deadlock_ms` searches the waits-for graph from itself, and
again each `deadlock_ms` after. So the first member of a cycle
whose time is up finds it. The default is 50 ms, set for a
database or by `sm1::set_deadlock_ms` for a session: PostgreSQL's
`deadlock_timeout`. A wait to upgrade a lock it holds also
searches once as it begins, as PostgreSQL's `ProcSleep` does. So
two upgraders of one lock need no timer.

A wait that ends within `deadlock_ms`, as most do, makes no
search. There is no detector thread.

If the search reaches its start, the **youngest** transaction on
the cycle, the last to begin, fails with `SM_ERR_DEADLOCK`. The
victim need not be the waiter that found the cycle (§3.7).

The waits-for edges form a graph, not a chain, because a waiter
waits on every holder whose mode conflicts. That is the cost of
shared modes. A wait on a transaction is an edge to the session
that runs it. A prepared transaction waits on no one, so no cycle
passes through it.

## 2. Lock subjects

### 2.1 What a lock names

These are the things a lock must name, because order is needed on
something the caller is not writing.

| Subject | Lock | Guards | State |
| --- | --- | --- | --- |
| a table's definition | OID, its oid | DDL against readers and writers of the in-memory catalog | built: `cat_lock_table`, `lock_table`, `sm1::lock_oid` |
| a row, read intent | row, `(storeid, relid ‖ row key)` | `FOR SHARE`, `FOR UPDATE`, an index build against a writer | built: `tbl_lock`, `rel_lock`, `sm1::lock_row` |
| a unique index's key, for a newest read | row, the entry's key, SH | `read_latest` of the key, against a writer of it | built: `idx_find_latest` |
| a catalog key, lock-then-write | row, `(store 0, "U" ‖ key)` | DDL takes X on each key it is about to write, reads it current, writes it | built: `catkv_lock` |
| a table's `STATS` entry | row, a catalog key, X | a second analyze waits for the first, so both do not sample | built: `cat_lock_stats` |
| a namespace | OID, its oid | `DROP SCHEMA` against `CREATE` in it | built: `cat_put_table`, `cat_put_collation` and `cat_put_sequence` take S, `cat_del_namespace` X |
| a view's definition | OID, its oid | `DROP VIEW`, `CREATE OR REPLACE` against a query expanding it | built: `cat_lock_view` |
| a collation or type in use | OID, its oid | `DROP COLLATION` with a column using it | not built ([`CATALOG.md`](CATALOG.md)) |
| an index as an object | none | `DROP INDEX` against a scan on it | subsumed by the table's lock |
| a sequence as an object | none | `CREATE` and `DROP SEQUENCE` | its record is a catalog key, read in the transaction ([`SEQUENCE.md`](SEQUENCE.md)) |

Nothing in sm1 requires a width of a key: `max_keylen`, 4096,
bounds it, and a lock keeps none of it.

### 2.2 What only looks like a subject

Each of these is ordered by a write, so the lock table is not
involved.

- **A unique index key.** Dedup is two rows making the same sm1
  key, which the inserter writes (§4.1).
- **A name in a namespace.** `CREATE TABLE` inserts the name's
  key. A taken one is `SM_ERR_DUPKEY`.
- **A non-unique index entry.** `entry_key` appends the row's key
  when the entry is not uniquely keyed, so two rows never share
  one. An index build is the only thing that writes entries
  without writing the row, and it locks the row (§4.4). So every
  writer of such an entry holds a lock or a version on its row.
- **A primary key.** The rows are keyed by it, so two rows with
  one key are one sm1 key, ordered as a unique entry is.
- **A row write, update or delete.** The uncommitted version is
  the lock.
- **Gaps and ranges.** Phantoms are SSI's SIREAD range
  predicates, checked at a write. Nothing is held on a gap, and a
  conflict is a serialization failure, not a wait
  ([`XCTMGR.md`](XCTMGR.md)).
- **A sequence's position.** `sm1::nextseq` is outside MVCC,
  takes no locks and is not undone by an abort.
- **A TOAST chunk.** It belongs to its row.

## 3. Implementation

### 3.1 The lock id

`lockid_t` in `src/locktab.h` holds two words. `hi` is the
storeid, its top bit set for an OID lock; `lo` is the oid, or
`lock_row_hash(key)`. An OID lock's storeid is 0.

`lock_row_hash` in `src/lockmgr.cc` must give the same value in
every build, as a `PREPARED` record keeps it: no host byte order,
no per-process seed, not `std::hash`.

1. Start from the key's length, so a key and its zero-padded self
   differ.
2. For each 8 bytes, read big-endian, XOR them in and apply
   `lock_fmix64`, MurmurHash3's 64-bit finalizer.
3. XOR in the tail and apply `lock_fmix64` again.

`lockid_hash_t` is another hash, of the two words, for the table:
`lo ^ (hi * 0x9e3779b97f4a7c15)`, finalized. Every bit of both
words reaches every bit of the hash. It is never persisted.
`lock_test_rows_collide` is a test hook: while set, every row's
second word is 0.

### 3.2 The lock table

`locktab_t` is a module of its own. It knows ids, modes and
opaque owners, and nothing of sessions. It finds, adds and drops
holds, and says which conflict with a mode. A holder's owner is
its session, and its `first` is the lock list entry that made the
hold. Waiting, the waits-for graph, the deadlock search, the fast
path and the lock list stay in `src/lockmgr.cc`. That is its one
caller, and it holds a partition's mutex across each call. So the
table is measured alone (`sm1/bench/locktab_bench.cc`).

**16 partitions**, as in PostgreSQL. An id's partition is the top
4 bits of its hash. Each has a mutex, on a cache line of its own,
and a condition variable, `lock_cv[i]` in `db_t`. So a lock, a
conversion, a write's check and a slot's move take one mutex, and
lockers in different partitions never meet.

**A partition's hash table** is its own, as PostgreSQL's
`dynahash` is.

- A power-of-two bucket array, indexed by the low bits of the
  hash, so no division. It starts at 64 buckets.
- An entry keeps its id, its hash, its first three holders
  inline, and the rest in an array of its own. Holders stay in
  the order they came. The array starts at 4 and doubles.
- A dropped entry goes on the partition's free list, up to 1,024.
  So a steady workload allocates no entry under the mutex, and a
  transaction of a million locks leaves no more than that behind.
- The array doubles past one entry a bucket, and goes to a
  quarter under one in eight. A resize makes a new array. Each
  later add and drop moves up to 8 buckets holding entries,
  looking at up to 64, so no call rehashes the partition. A
  look-up meanwhile looks in both arrays.

A lock walks its entry's holders once (`locktab_t::probe`). The
walk finds the asker's own hold and gathers the others' modes as
bits, and one AND checks them against the asked mode's conflicts.
The holders in conflict are named only when there are some.

**A hold goes with its first entry.** `part_t::drop` removes a
holder only if the given entry made the hold. The last holder out
removes the table entry. `part_t::join` covers an existing hold's
mode, or makes a hold. `part_t::move_owner` changes a hold's
owner, for a prepare.

**Host wait timers.** `set_lock_wait_hook()` reports a conflict
that requires a wait for a lock or an uncommitted version.

1. Release the wait mutex and call the start hook once.
2. Take the mutex and check the conflict again.
3. Wait until the conflict ends, an interrupt, or a deadlock.
4. Release the mutex and call the end hook if requested.

Wakeups do not restart the hook. Neither hook holds a lock
mutex: cancelling a host timer can wait for a wakeup that needs
that mutex. An uncontended operation calls neither hook.

### 3.3 The lock list

A session's `locks` is a `lock_lists_t`: one `lock_list_t` for
each partition, as PostgreSQL keeps a backend's `myProcLocks`. An
entry goes in its id's partition's list from the start, a
fast-path slot's too, so a sweep that moves a slot changes no
list.

Each list is an sm0 `blist_t`, blocks of 32 whose entries never
move. So a holder keeps a pointer to its first entry, a slot
keeps a pointer to its own, and a savepoint keeps a mark in each
list. A cut keeps up to four blocks for the locks to come.
`lock_try` calls `reserve()` before it takes the partition's
mutex, so a lock allocates nothing under the mutex for the list.
Finding a lock of the session's own searches one list, the newest
entry first.

An entry, `lockref_t` in `src/db.h`, holds:

| Field | What |
| --- | --- |
| `id`, `type`, `part` | the lock, and its partition |
| `mode` | held now; the first entry of a hold follows conversions |
| `kind` | the caller's |
| `latest` | the mark of §1.9; `s->latest_marks` counts them, so a write with none skips the scan |
| `slot` | its fast-path slot, or -1 |
| `strong` | counted in `fp_strong` |

One id is one hold, with one entry, with one exception: an OID
lock may have a second entry. A weak lock can take a slot while
its session already holds the id in the table. A sweep then moves
that slot in (`fp_move`). The move raises the hold's mode, the
entry is released alone, and a rollback past it keeps the hold
(scenario `h07`).

### 3.4 The fast path

This is PostgreSQL's fast path for a relation's weak locks (its
`lock.c`, 9.2). A table's readers and writers take IS and IX on
its oid, which never conflict with each other. So only a strong
lock, S, SIX or X, which DDL takes, needs to see them. Row locks
take no fast path.

An OID lock in IS or IX goes in one of 32 slots of its session's
own (`fp`, under `fp_mu`) and not in the table. It may do so
while no strong lock is held or asked on a key that hashes with
it. That is, one of 1,024 counts in the database, `fp_strong`, is
0. The count is chosen by `lockid_hash_t`, so tables 1,024 oids
apart share a count only by chance.

A weak lock, `fp_take`:

1. Take the session's `fp_mu`.
2. Read the count. If it is not 0, move the session's own slot
   for the id to the table, and go to the table.
3. If the session has a slot for the id, raise its mode: IS and
   IX cover each other as IX, still weak.
4. If every slot is taken, go to the table.
5. Else fill a slot, and add its list entry.

A strong lock, `lock_oid`:

1. Unless the session's first entry on the id is already
   `strong`, raise the count, then sweep (`fp_sweep`). The sweep
   walks `fp_sessions` under `fp_reg_mu`, and moves each slot for
   the id into the table under that session's `fp_mu`.
2. Lock as any lock does, meeting the weak holders in the table,
   and in the waits-for graph.
3. On success, set `strong` on the first entry: the count goes
   with it, as a conversion outlives a rollback past later
   entries. On failure, lower the count at once.

**No lock is missed.** The two orders meet at the weak session's
`fp_mu`. A weak locker reads the count under it, and a sweep
takes it after the count is raised. So either the sweep finds the
slot, or the weak locker sees the count.

A slot is released at commit, abort or a rollback past it under
`fp_mu` alone. A transaction whose locks are all in slots takes
no partition's mutex at all. A second weak lock asked during a
strong lock's wait goes to the table and is granted. The strong
locker is not yet a holder, and sm1 keeps no queue.

### 3.5 Write latches

`db_t` has 4,096 mutexes, `write_latch`. `lock_latch` hashes the
key with `std::hash` and mixes the storeid in. It is never
persisted, so a process's hash serves.

- Two writes of one key always meet on the latch. Two writes of
  different keys meet only when they hash alike, which costs a
  moment's wait and never a wrong answer.
- A latch is held only while a call checks its key and puts its
  version. It is never held across a wait.
- A thread holds one latch at a time, so latches cannot deadlock.
- `sm1::lock_row` takes it too, so no write of the key comes
  between its look at the newest version and `lock_try`.
- Savepoint rollback and the chained store's garbage collection
  take a key's latch for the same reason
  ([`XCTMGR.md`](XCTMGR.md), [`GC.md`](GC.md)).

### 3.6 Waits and wake-ups

`lock_wait` sleeps under the partition's mutex on `lock_cv[i]`.
`lock_wait_xact` sleeps under `xact_mu` on `xact_cv`. Each sleeps
by `wait_once`, one `sm_wait()` to a deadline.

| Event | Wakes |
| --- | --- |
| a release in partition `i` | `lock_cv[i]` |
| a transaction's end, past the active table | `xact_cv` (`lock_wake`) |
| a rollback to a savepoint | `xact_cv`, with `rollback_epoch` raised under `xact_mu` |
| a prepare's move of the holders | every `lock_cv` |
| a doom | every `lock_cv` and `xact_cv` |

`lock_wait` returns 0 when no holder conflicts, -1 if doomed, 1
under `qa_nowait`, and 2 if interrupted. `lock_wait_xact` returns
the same, with 0 when the transaction is gone from the active
table or `rollback_epoch` moved. `lock_wait_error` turns these
into `SM_ERR_DEADLOCK`, which aborts, `SM_ERR_BLOCKED` and
`SM_ERR_INTERRUPTED`.

The owner of an `xid` comes from `xid_owner`, a map beside the
active table, under `txn_mu`. A write's wait on a lock carries no
kind, so `lock_status` shows the kind of the lock's oldest
holder.

### 3.7 The deadlock search

`check_deadlock` runs from `wait_once` at the deadline, and from
`lock_wait` as an upgrade begins. The waiter lets its own mutex
go first, as the mutexes are taken in order.

1. Take every lock mutex (`lock_all_t`).
2. Walk depth-first from the session, with a visited set. The
   path supplies the cycle.
3. If no path returns to the session, return. Another cycle is
   for its own members to find.
4. Choose the member with the largest `begin_seq` as the victim.
5. Store `SM_ERR_DEADLOCK` in the victim's `doomed`.
6. Notify every `lock_cv` and `xact_cv`.

The search holds up every locker while it runs, which is why a
wait searches only after it has slept.

**The edges are read from the lock table as the walk goes**
(`lock_blockers`), as PostgreSQL's detector reads its table. A
waiter on a lock waits on the holders that hold, now, a mode
conflicting with the one it asks. A waiter on a transaction waits
on its owner, while the transaction runs and `rollback_epoch` has
not moved. Every lock a waiter can wait on is in the table. A
strong lock moves the weak ones from their slots before it waits,
and a weak lock waits only on strong ones.

A waiter's own list, `waiting_for`, is not the graph. It records
what its wait last saw, and it goes stale the moment a holder
releases.

**Telling the victim.** Every member of a cycle is waiting, so
the victim wakes in its own wait, finds `doomed` set, and fails
from there. `xct_check_active` also fails a doomed transaction at
the entry of any later call, so a doomed transaction can only be
aborted. `doomed` is atomic. It holds `SM_ERR_DEADLOCK` for a
victim, or `SM_ERR_DOOMED` for a failed rollback
([`XCTMGR.md`](XCTMGR.md)).

### 3.8 Release

`lock_release_from(s, mark)` serves commit, abort and a rollback
alike, as PostgreSQL's `LockReleaseAll` does. It holds `fp_mu`
throughout. For each partition in order:

1. Walk the session's list past the mark.
2. Free each lock's slot, or drop its hold from the table. The
   partition's mutex is taken once, at the first hold, and not at
   all for a list with none.
3. Lower `fp_strong` for a `strong` entry, only once its hold is
   gone: only then may a weak lock take a slot.
4. Lower `latest_marks` for a marked entry.
5. Wake the partition's waiters if its mutex was taken.
6. Cut the list back to the mark, keeping the blocks.

### 3.9 Prepare and recovery

A prepared transaction's uncommitted versions are its write
locks. After a restart, recovery puts it back in the active
table, in doubt, so a writer waits for it as before
([`XCTMGR.md`](XCTMGR.md)). The `PREPARED` record therefore lists
explicit
locks alone.

An entry of the record is `lock_record_size`, 20 bytes: the type,
the id's two words big-endian, the mode, and the kind.
`lock_from_record` refuses a type that disagrees with the id's
top bit, or an unknown mode.

`lock_transfer` hands the locks to the prepared transaction's
object, `g`:

1. Under `s->fp_mu`, move every slot into the table, as `g` is in
   no sweep's list.
2. Take every lock mutex.
3. Change each hold's owner to `g`.
4. Swap the list into `g`: a holder's `first` points into it.
5. Wake every `lock_cv`, as waiters read their holders again.

`xid_owner` is pointed at `g` first, so a writer waiting for the
`xid` now waits on it. A prepared transaction waits on no one.

`lock_restore` puts the ids back with no hash. A strong OID lock
counts in `fp_strong` again, so the fast path stays off its key.
A second entry joins the hold its first entry made.

### 3.10 `lock_status` and `waits_for`

`sm1::lock_status` reports each hold of the table, each slot, and
each session waiting on a lock. It holds each session's `fp_mu`,
then every lock mutex. A hold shows the kind of the entry that
made it, and the mode that covers every entry of its id. A row
lock reports its id's word in place of a key. A writer waiting
behind an uncommitted version asks no lock and is not listed.
`sm1::waits_for` names whom it waits for, from `lock_blockers`.

### 3.11 Mutex order

A thread takes these in this order:

1. the environment's `mu`
2. `fp_reg_mu`
3. a session's `fp_mu`; `sm1::lock_status` holds many, in the
   order of their addresses
4. the partitions' mutexes, in their order
5. `xact_mu`
6. `txn_mu`

A key's latch comes before a partition's mutex: `lock_try` and
`lock_write_check` run under it. `lock_all_t` takes step 4's
mutexes and `xact_mu`. The deadlock search, `sm1::lock_status`,
`sm1::waits_for`, and a prepare's and restore's moves use it.

### 3.12 What a transaction holds in memory

Row values stream to RocksDB, so disk bounds a transaction. Three
structures grow in memory instead:

- the lock list, one entry for each lock taken, with no key;
- the undo list, while a savepoint is set;
- the SSI read set.

The last two keep the key inline, so a transaction touching tens
of millions of keys costs gigabytes. That is the price of O(1)
abort, and a real limit on one transaction's size
([`XCTMGR.md`](XCTMGR.md)).

## 4. Cases

### 4.1 Two transactions insert one unique key

A unique index's `entry_key` omits the row's key when no key
column is null, so both rows make one key.

```
T1  idx_insert id=42 -> relid|02|80..2a   under the latch:
                                          absent, put version (xid T1)
T2  idx_insert id=42 -> the same key      under the latch: finds T1's
                                          uncommitted version
T2                                        lets the latch go, waits for T1
T1  commit
T2  retry -> the key is there committed -> SM_ERR_DUPKEY
```

- Neither takes a lock entry.
- If T1 aborts, T2's retry finds no live version and succeeds.
  Waiting turns a benign race into a success.
- A snapshot read could not do this. T1's entry is invisible to
  T2's snapshot, so both would commit. Uniqueness holds at READ
  COMMITTED too, where no SSI catches it. That is why
  `sm1::insert` is judged against the newest committed version,
  not the snapshot.
- A key with a null gets the row's key appended (`has_null`), so
  several NULLs coexist, as in PostgreSQL.

### 4.2 Why the table-oid lock is a type, not a key

DDL and a reader need order on a table's definition, which lives
in memory. There is no row to wait on, so the lock table is the
only mechanism.

The trap is which name to lock. Lock the `tbl:` key, which `ALTER
TABLE` may write, and §1.6's check fires:

```
T2  begins (snapshot S2)
T1  ALTER TABLE ... commits        a version of tbl:<oid> newer than S2
T2  lock tbl:<oid>  ->  SM_ERR_CONFLICT at REPEATABLE READ
```

T2 is the reader that waited out the DDL and should be given the
new definition. Failing it is wrong. `LOCKTYPE_OID` makes this
structural: no version of the name can exist, so the check cannot
fire at any level.

**The table-oid lock orders, and row locks guard what is
written.** DDL takes two kinds of lock, at two moments.

First it takes the table's lock X in `cat_get_table_rec`, and
reads the table's record from the catalog in memory, not from the
store. Memory has every commit by the time the lock is granted,
so the record is the table as it now is. A read of the store at
REPEATABLE READ shows the snapshot's table, without the column
another transaction just added.

```
sm3::lock_table(s, 16384, X)
  -> sm1::lock_oid(s, 16384, SM_LOCKMODE_EX)
```

Then, in `cat_put_table`, it locks each catalog key it is about
to write, and only those. A table's own fields, one column and
one index each have a key ([`CATALOG.md`](CATALOG.md)).

```
sm2::catkv_lock(kv, part_key(column, 16384, 3), X)
  -> sm1::lock_row(s, 0, "U" | kind(8) | be64(16384) | be64(3),
                   SM_LOCKMODE_EX)
```

The pair reads twice over: the wrapper says which object, and
`sm1::lock_row` says which bytes. That gap comes of sm1 not
knowing what a table is.

At READ COMMITTED the row lock lets the DDL write over a newer
version after a `catkv_get_latest`. At REPEATABLE READ it is the
correct `SM_ERR_CONFLICT`, only when the DDL writes the very key
another transaction changed. Two DDL statements that write
different keys of one table meet in no row lock: the table-oid
lock alone orders them.

### 4.3 `FOR UPDATE` against a writer

```
T1  sm2::rel_lock(rel, id_key(7), X)
      -> sm1::lock_row(s, 1001, be16(1)|be64(7), SM_LOCKMODE_EX)
T2  sm2::rel_update(rel, id_key(7), 7, ...)
      -> sm1::update(s, 1001, be16(1)|be64(7), ...)
         blocks: a write waits for a conflicting lock on its key
```

This works only because both name `(storeid, relid ‖ row key)`:
the lock and the write meet on the data key. In a table with a
primary key the row key is its encoding. A move of the row erases
that key, so it too waits for the lock.

So row locks cannot go in a namespace of their own. A writer,
which takes no entry, would never see them, and `FOR UPDATE`
would silently stop blocking writers.

### 4.4 sm3's protocols

Two protocols rely on a writer holding its table row to commit.
They still hold.

**A concurrent index build.** The build must not add an entry for
a row while a writer changes it. The entry key is the obvious
thing to lock, and the wrong one. The build locks the **row**
shared and reads it with `sm2::rel_read_latest`, to the end of
the batch ([`INDEXBUILD.md`](INDEXBUILD.md)). Its explicit lock
waits for a
writer's uncommitted version (§1.5), and a writer waits for its
lock (§1.7). So the two never work on one row at once.

**A foreign key check** locks the parent row shared. A writer of
the parent waits for it, and it waits for an uncommitted write of
the parent: the same pair.

### 4.5 Lock, read current, act at READ COMMITTED

```
T1  lock(ROW, store, k, X)        held; at RC, no currency check
T1  read_latest(k)                the newest committed value; mark set
T1  update(k, ...)                allowed: the X holder was marked
```

At REPEATABLE READ the lock itself would have failed on a newer
version.

### 4.6 Two conversions deadlock

```
T1  lock(k, IX)      ok
T2  lock(k, IX)      ok, IX and IX are compatible
T1  lock(k, X)  ->   converts, waits for T2; searches once, finds no cycle
T2  lock(k, X)  ->   converts, waits for T1; searches once, finds the cycle
```

The younger transaction fails with `SM_ERR_DEADLOCK`, with no
timer run out. Under `sm1::set_qa_nowait` the call returns
`SM_ERR_BLOCKED` and does not wait.

### 4.7 `TRUNCATE`, and why the lock's name is the oid

`truncate_table` makes a new store, points the table's record at
it, and drops the old one, under the table-oid lock X.

A lock named by storeid would move under it. A stale reader could
hold the old storeid's lock while DDL holds the new one's, and
both would believe they held the table. The oid is the only
identity stable for the table's whole life.

The lock also cannot live in the table's store. It has to outlive
`DROP TABLE`'s `rmstore`: a table dropped while a caller waited
is `SM_ERR_NOTFOUND`, which means getting the lock and then
finding no definition. It has to come before the store too, as
`CREATE TABLE` inserts the name before it makes the store.

### 4.8 Publishing under the lock

Releasing the table-oid lock is not enough. sm1's commit releases
it, and a waiter could then read the shared catalog before the
DDL's new definition was published. So sm3's commit holds the
catalog's mutex exclusive across both sm1's commit and the
publish. A transaction that gets the table-oid lock then waits on
that mutex, and sees the new definition
([`CATALOG.md`](CATALOG.md)).

### 4.9 What a shared id costs

| Case | Outcome |
| --- | --- |
| two keys of one transaction hash alike | one lock, in the stronger mode, with the kind of the first. Safe: another transaction's write of either meets it |
| two keys of two transactions hash alike | they block each other, and could deadlock. A false wait, and rarely a false deadlock abort: the exact operation still decides |
| `read_latest` of a key under a lock of another that hashes alike | let through. Safe: no other writer of the key passes the shared id |
| a write converting its own lock | converts the id's hold; with keys alike, both keys' |
| savepoint rollback | a lock first taken past the mark goes; a conversion is never undone |
| prepare, crash, restart | the record keeps ids, and the hash is fixed |
| `FOR UPDATE` of a million rows with long primary keys | no key bytes kept, none copied under a mutex |
| `pg_locks` | a row lock's table, but not its row |

### 4.10 What keeping no write lock costs

- **A read in `sm1::lock_row`** of a key that may be written, to
  find an uncommitted version.
- **False waits on a shared latch**, a moment each, when two keys
  hash alike.
- **A wait is a retry.** A writer that waited starts again, a
  second look-up of the key.
- **More edges.** A cycle through a wait on a transaction is
  found as one through a lock, over more edges.

### 4.11 Traps

- **A synthetic lock name costs an entry.** Any name where no
  version exists has to be a real table entry. Moving dedup off
  the key it writes, hashing the value say, would put one entry
  for each inserted row back into every transaction. The hash is
  not the problem. Leaving the written key is.
- **Hashing a lock's name is safe; hashing a key is not.** A name
  collision costs a false wait. A key collision would refuse a
  value that is free.

## 5. Rejected

| Alternative | Why not |
| --- | --- |
| A write keeps its row lock to the end | a transaction's memory grows with every key written: four million entries for a million rows with three indexes |
| Drop the held lock for inserts only | updates and deletes keep theirs, and a mixed rule is two write paths to reason about. The version serves all three as easily |
| PostgreSQL's in-tuple row locks for `FOR SHARE` (multixact) | explicit locks are few next to writes, except a foreign key check for each child row. Worth doing later if those turn out many |
| The lock keeps the row's key, to tell apart keys that hash alike | it needs key bytes in the list, the record and under a mutex, for a case a 64-bit hash makes rare |
| The id keeps the relid in the first word's high bits | sm1 then knows a relid, which is sm2's. The caller's `kind` keeps sm1 out of it |
| `pg_locks` reports a row lock with no kind, and the test counts by mode alone | simplest, but `pg_locks` says less |
| A `LOCK` key no DDL writes, for a table's lock | correct, but a convention the next reader has to notice. `LOCKTYPE_OID` is structural |
| A lock named by storeid | it moves under `TRUNCATE` (§4.7) |
| Row locks in a namespace of their own | a writer would never meet them (§4.3) |
| A detector thread | a waiter that has slept searches for itself, as PostgreSQL's does. Most waits end first and make no search |
| Walk each waiter's own `waiting_for` list | it goes stale as holders release, so it found cycles that no longer existed and doomed a transaction that need not fail (`e07`) |
| The host polls, with every wait a refused call it repeats | it loses the waits-for graph, and with it deadlock detection, as `set_qa_nowait` shows. Interrupts serve instead |
| Skip `sm1::lock_row`'s read for keys never written, with sm2 saying so in `catkv_lock` | designed, not built: nothing showed the read to matter. A table's lock reads no store since it became an OID lock |
| Show whether a lock is held (`held`) above sm1 | nothing in sm2, sm3 or dg needs it, so it stays inside sm1 |

## 6. Tests

**`sm1/mvcc`**, the specification:

| Scenarios | What they hold |
| --- | --- |
| `b09`, `b10`, `b22`, `b23` | a write waits for an uncommitted update or delete, and its commit or abort |
| `b11`, `b17`, `b24` | a write waits for an uncommitted insert |
| `b14`, `b35`, `b36` | first committer wins, and read-modify-write |
| `b16`, `b20`, `b21` | deadlocks through waits on a transaction, and the victim |
| `b19` | several waiters, with no queue |
| `b25`, `b31` | a rollback to a savepoint releases a waiter, and explicit locks |
| `b26`, `b27`, `b28`, `b29`, `b30` | shared locks, both directions of block, upgrades |
| `b33`, `b34` | an explicit lock waits for an uncommitted write |
| `a18`, `a19` | a lock on a row changed since; `read_latest` of an own version |
| `h01` to `h08` | table locks: the fast path, slots, conversions past a savepoint |
| `e02`, `e03`, `e06`, `e07` | stress: serializable, repeatable read, read committed, table locks |

**Unit tests:**

- `sm1/test/test_locktab.cc`: the table alone. Gray's table, a
  hold and its first entry, `join`, `probe`, `move_owner`,
  spreading of the hash, resize a few buckets at a time,
  `free_max`, holders past the inline three.
- `sm1/test/test_lock.cc`: the modes through the API,
  conversions, interrupts, `trylock`, `waits_for`, `lock_status`
  with the kind, a write that keeps no entries, a refused write
  that keeps one, keys that hash alike
  (`lock_test_rows_collide`), the fixed row hash, the fast path,
  the deadlock timer, cycles across partitions and through a wait
  on a transaction.
- `sm1/test/test_2pc.cc`: a prepared transaction's locks and
  writes across a restart.
- `sm1/test/test_read_latest.cc`: the rules of §1.9.

**`rel`:** `rel/test/test_blocking.rel`, `test_recheck.rel`,
`test_doomed.rel`, `test_fkey.rel`.

**dg:** `../dg/test/select_for_update_many.sql` counts a
session's `tuple` locks, from the kind.

**Benchmark:** `sm1/bench/locktab_bench.cc` measures the table
alone.
