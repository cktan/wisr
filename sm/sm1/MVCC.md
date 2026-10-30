# MVCC — how a version becomes visible

A digest of sm1's multi-version concurrency control: the
numbers it runs on, what a version looks like on disk, and
how a reader decides whether it may see one. It is drawn from
`SM1_DESIGN.md` and from the code, and adds nothing to
either; where they say more, the section is named.

`mvcc/MVCC.md` is a different document: the isolation rules
as a table of scenarios, executed on every build. This one is
the mechanism under them.

---

## 0. In brief

- A row is a chain of **versions**, one RocksDB key-value
  pair each, newest first.
- A version records **who wrote it**, an `xid`, and not
  whether that writer committed.
- A transaction that commits is given a **`commit_ts`**. Its
  versions become visible, all at once, to every reader whose
  snapshot is at or after it.
- So a reader must turn a version's `xid` into a `commit_ts`.
  Four places can answer, tried in order: the version's own
  header, the active table, the commit cache, and the status
  table `CF:txn`.
- Nothing is undone. An aborted or crashed transaction's
  versions stay where they are and are dead because no
  commit was recorded for them.

---

## 1. The numbers

One 64-bit counter, `global_ts`, hands out every number below,
so they are all comparable (`SM1_DESIGN.md` §4.1).

| Number | Taken | Is |
| --- | --- | --- |
| `snapshot_ts` | at `sm1::begin`, without using a counter value | the point in time a reader sees the database at |
| `xid` | at a transaction's first write | the transaction's identity. A read-only transaction has none |
| `wts` | at each write, under the key's latch | the version's write timestamp, part of its key |
| `seq` | at each write, counting from 1 within the transaction | what tells two writes of one key by one transaction apart |
| `commit_ts` | at commit | the point in time the transaction becomes visible |

For one transaction, always:

```
snapshot_ts  <  xid  <  wts of each write  <  commit_ts
```

**Versions of a key sort in commit order.** A writer takes
its `wts` under the key's latch, when no other writer of the
key is in flight, and no other can take one for that key
until this one has ended, so after its `commit_ts`. So if
`T2` committed a key after `T1` did,
`T2.wts > T1.commit_ts > T1.wts`. That is why a reader
can seek by `wts` and meet versions in the order they were
committed, at every isolation level.

**Across a restart** the counter resumes above every value it
ever handed out. It is persisted in leases: `CF:meta` holds a
value ahead of the counter, renewed when the counter reaches
it (`SM1_DESIGN.md` §4.2).

---

## 2. A version on disk

Each store is a RocksDB column family, `store.<storeid>`, or
`chain.<storeid>` for a store made chained. A version is one
pair in it (`SM1_DESIGN.md` §3.1):

```
key   = class   (1 byte, 0x00: a versioned key)
      | userkey (variable)
      | ~wts   (8 bytes, big-endian, complemented)
      | ~seq   (4 bytes, big-endian, complemented)

value = flags   (1 byte)   bit 0: tombstone, this version is a delete
                           bit 1: stamped, the next field is a commit_ts
                           bit 2: a move's tombstone (sm1::erase_moved)
      | xid       (8 bytes) while unstamped: the writer
        commit_ts (8 bytes) once stamped, in its place
      | payload  (variable; none for a tombstone)
```

- The complements make a key's versions sort **newest
  first**, so a seek to `userkey | ~snapshot_ts` lands on the
  newest version written at or before the snapshot.
- The class byte opens every key and no caller sees it:
  `0x00` for the versioned keys this digest describes, `0x01`
  and `0x02` for a chained key's newest and older versions,
  which a chained store also holds (`../VCHAIN.md` §2).
- A custom comparator, `wisr.store.v2`, orders by the class
  byte, then within class `0x00` by `userkey` and by the
  12-byte tail second, so a key's versions are contiguous.
- **A write is eager.** The version is put into RocksDB at
  once, before the transaction commits. That is why a
  version cannot say whether its writer committed: when it
  was written, nobody knew.

The row `"acct42"`, written by `xid` 50 at `wts` 51 and
updated by `xid` 60 at `wts` 61:

```
userkey   wts  seq   flags      xid / commit_ts   payload
acct42    61   1     stamped    80                "bal=9"
acct42    51   1     unstamped  xid 50            "bal=7"
```

The first has been visited by compaction, which wrote the
commit timestamp into it (§8). The second still names its
writer, and a reader must look that writer up.

---

## 3. A snapshot, and `visible_ts`

A snapshot is one number: `snapshot_ts = visible_ts` at the
moment it is taken (`SM1_DESIGN.md` §4.3).

`visible_ts` is the largest timestamp such that every commit
at or below it is durably recorded and published. It exists
because commits do not finish in the order they start:

```
commit_ts claimed:   104   105   106   107
status record done:  yes   yes   no    yes

visible_ts = 105
```

107 has finished, but a snapshot that included it could not
yet say what 106 did, and would see a gap. So commits are
**published in `commit_ts` order**: `visible_ts` is one below
the smallest commit still in progress.

The price is that one slow commit holds back new snapshots
for the length of its log sync. What it buys is that a
snapshot is a single number, and visibility a single
comparison, with no list of in-flight transactions kept for
each snapshot as PostgreSQL keeps.

---

## 4. The visibility rule

A version `V`, read by transaction `T` at `snapshot_ts`
(`SM1_DESIGN.md` §4.4):

```
if V.xid == T.xid:                      # T's own write
    visible iff V.seq <= T.seq_hwm
            and V.seq <= T.stmt_seq     # within a statement
elif V.wts > snapshot_ts:               # written after the snapshot,
    invisible                           # so committed after it too
else:
    resolve V's writer  →  commit_ts | dead | in flight
    visible iff committed and commit_ts <= snapshot_ts
```

A visible tombstone means the key does not exist.

A read seeks to the newest version at or before its snapshot
and walks down the chain until one is visible. The walk
usually stops at the first version; it goes further only past
versions of transactions concurrent with the reader on that
exact key (`SM1_DESIGN.md` §10.1).

---

## 5. From `xid` to `commit_ts`

"Resolve V's writer" is the heart of it (`SM1_DESIGN.md`
§3.4; `xct_status` in `src/xct.cc`).

**A stamped version needs none of this.** Its header holds
the `commit_ts`. Only an unstamped version of another
transaction, written at or before the snapshot, is resolved:

| Step | Do | If so |
| --- | --- | --- |
| 1 | take the mutex `txn_mu`, which guards both tables below | |
| 2 | look for the `xid` in the **active table**: transactions running or prepared | *in flight*. Invisible. Stop |
| 3 | look for the `xid` in the **commit cache** | *committed*, at the `commit_ts` it holds. Stop |
| 4 | release the mutex and read `CF:txn` at key `xid` | the only step that may touch disk |
| 5 | no record, or `ABORTED` | *dead* |
| | `PREPARED` | *in flight* |
| | `COMMITTED` | *committed*, at the record's `commit_ts`; put it in the commit cache |

**Why no committed transaction is ever missed.** A committing
transaction writes its `COMMITTED` record first, and only then
moves from the active table into the cache, in one step under
the mutex. At every moment it is findable in at least one of
the three places.

**Why "no record" may mean dead.** A transaction enters the
active table before its first version reaches RocksDB. So an
`xid` that is in neither table and has no record belongs to a
transaction that ended without committing, or was running at
a crash. An `xid` is never reused, so it never will commit.
This is the whole of crash recovery for data: no undo pass,
and no scan.

A reader at snapshot 75:

| Version met | Resolved by | Result |
| --- | --- | --- |
| stamped, `commit_ts` 70 | its header | visible |
| `xid` 50, committed at 70 a moment ago | the commit cache | visible |
| `xid` 50, committed at 70, after a restart | `CF:txn`, once; the cache after that | visible |
| `xid` 62, still running | the active table | invisible |
| `xid` 58, aborted | `CF:txn`, which has no record, every time | invisible |
| `xid` 66, committed at 80 | the commit cache: 80 > 75 | invisible |

The last row is why a timestamp is needed and not a yes or
no: the writer committed, but after this reader's snapshot.

---

## 6. The status table, `CF:txn`

One record for each transaction that reached a decision
(`SM1_DESIGN.md` §3.2). Integers are big-endian.

```
key    xid (8 bytes)

value  state        1 byte    1 PREPARED, 2 COMMITTED, 3 ABORTED
       snapshot_ts  8 bytes
       commit_ts    8 bytes   zero unless COMMITTED
```

`COMMITTED` and `ABORTED` records are those 17 bytes. A
`PREPARED` record continues:

```
       prepared_at  8 bytes   wall-clock seconds
       gid          4-byte length, then the global transaction id
       locks        4-byte count; for each, a 4-byte length,
                    the lock's key, and 1 byte of mode
       created      4-byte count, then 8-byte storeids
       dropped      4-byte count, then 8-byte storeids
```

It holds what recovery needs to rebuild a transaction in
doubt: its snapshot, the locks it holds with their modes, and
the stores it made and dropped.

| Transaction | Record |
| --- | --- |
| read-only | none |
| wrote and committed | `COMMITTED` |
| wrote and aborted, or crashed | none |
| prepared, awaiting its coordinator | `PREPARED` |
| prepared, then committed or aborted | `COMMITTED` or `ABORTED`, over the `PREPARED` one |

Nothing is written at `sm1::begin`, and an ordinary abort
writes nothing at all.

---

## 7. The commit cache

A map in memory from `xid` to `commit_ts`, step 3 of §5. It
spares a read of `CF:txn` for versions that are committed and
not yet stamped.

- It is in memory only, and empty after a restart.
- An entry is made at commit, and when a lookup finds a
  `COMMITTED` record on disk.
- It is a fixed array of slots, 65,536 by default, in sets of
  two; a new entry takes the slot of the smaller `xid` in its
  set.
- It changes speed and never an answer.

`COMMITCACHE.md` has the rest.

---

## 8. How a version stops needing a lookup

Two background steps (`SM1_DESIGN.md` §8.3).

1. **Compaction stamps.** RocksDB's compaction passes every
   version through sm1's filter. A version committed at or
   below the garbage-collection watermark has its `xid`
   replaced by its `commit_ts`. A dead one is dropped. So is
   a committed one that a newer committed version, visible
   to every live snapshot, has replaced.
2. **The sweep deletes records.** Once `CF:txn` passes a
   million records, the GC worker forces a full compaction,
   after which every version at or below its watermark is
   stamped, and then deletes the `COMMITTED` records it no
   longer needs.

The watermark is the oldest snapshot still in use by a
session, a prepared transaction or an open cursor. Nothing a
live snapshot might need is collected, which is also why a
transaction left open holds back collection for the whole
database.

---

## 9. Writes and conflicts

A write puts a new version above the old one. Nothing is
overwritten (`SM1_DESIGN.md` §5).

1. Take the key's **latch**, a short mutex chosen by hashing
   the store and key, held only within the call. Under it:
   if another transaction holds an explicit lock on the key, or
   has written it and not ended, let the latch go, wait for
   that transaction, and start again. An uncommitted version is
   the lock; a write keeps no lock entry (`WRITELOCK.md`).
2. Find the newest version: the transaction's own if it has
   one, else the newest committed.
3. If that version committed **after the writer's snapshot**,
   someone else changed the row since the writer began:

   | Level | Outcome |
   | --- | --- |
   | REPEATABLE READ, SERIALIZABLE | `SM_ERR_CONFLICT`: first committer wins, and the transaction is to be retried |
   | READ COMMITTED | `SM_ERR_UPDATED`: the caller re-reads the row at its newest version and repeats the write |

4. Take a `wts`, put the version, with the writer's `xid`,
   and let the latch go.

Readers take no lock and never wait. A reader and a writer of
one row pass each other: the reader sees the version its
snapshot allows.

---

## 10. A statement and its own writes

A transaction sees its own writes. A statement, one SQL
command, does not see the writes it makes itself: only those
the transaction made before it began. This is PostgreSQL's
rule, and without it a statement's answer can depend on the
order it happens to visit rows.

**The mark.** `stmt_begin` notes the transaction's `seq` as
`stmt_seq`. Every read of the statement sees own versions only
up to it (§4). The statement's own writes take `seq` numbers
above it, so they stay hidden from the statement and appear to
the next one.

**An example.** `emp(id, boss, pay)` holds three rows,
committed:

| rowno | id | boss | pay |
| --- | --- | --- | --- |
| 1 | 1 Bob | — | 1000 |
| 2 | 2 Ann | 1 | 950 |
| 3 | 3 Cid | 2 | 500 |

One statement raises everyone whose boss earns at least 1000:

```sql
UPDATE emp e SET pay = e.pay + 100
FROM emp b WHERE e.boss = b.id AND b.pay >= 1000;
```

The executor walks `e` with a cursor and, for each row, looks
the boss up by key. In a fresh transaction, before the
statement, `seq` is 0.

| Step | Call | `seq` | What the statement sees |
| --- | --- | --- | --- |
| 1 | `stmt_begin`: `stmt_seq` = 0 | 0 | |
| 2 | open the cursor over `e` | 0 | |
| 3 | `e` = Bob: no boss, skipped | 0 | |
| 4 | `e` = Ann: read boss Bob, pay 1000; qualifies | 0 | Bob 1000 |
| 5 | update Ann's pay to 1050 | 1 | |
| 6 | `e` = Cid: read boss Ann | 1 | **Ann 950**: her new version has `seq` 1, above the mark |
| 7 | 950 < 1000: Cid not raised | 1 | |
| 8 | the next `stmt_begin`: `stmt_seq` = 1 | 1 | Ann 1050 |

Without the mark, step 6 would read Ann at 1050 and raise Cid
too. Visit the rows in another order, Cid before Ann, and Cid
would not be raised. The result would depend on the plan, which
SQL forbids. The cursor of step 2 was safe either way, having
frozen `seq_hwm` when it opened; the point read of step 6 is
the read the mark protects, as is any cursor opened later in
the statement.

**A row changed twice.** Change the condition so that a row
can be reached twice in one statement, as when a row joins two
others. The second update of it finds that the newest version
of the key is the transaction's own, with `seq` above the
mark: the statement changed it already, and cannot see that
change. The write fails with `SM_ERR_SELFMODIFIED` and writes
nothing. It is not an abort: the executor skips the row, as
PostgreSQL's does, or raises an error for `INSERT ... ON
CONFLICT`, which may not change one row twice.

**A trigger.** Say an update of `emp` fires a trigger that
reads `emp` to write an audit row. The trigger's commands run
inside the statement, as an inner statement:

| Step | Call | `stmt_seq` | Reads Ann as |
| --- | --- | --- | --- |
| 5 | update Ann to 1050 | 0 | |
| 5a | `stmt_begin(outer)`: the trigger begins | 1 | 1050: the outer's write so far |
| 5b | insert the audit row, `seq` 2 | 1 | |
| 5c | `stmt_end(outer)`: the trigger ends | 0 | 950, and no audit row |

The trigger sees what the statement has done so far, and the
statement resumes with its own view.

**No command id is stored.** `seq` is already in every
version's key (§2), and a statement's writes are one unbroken
run of it, so "written before this statement" is `seq <=
stmt_seq`. PostgreSQL stores a command id, `cmin`, in each
tuple and a second one, `cmax`, because an update or delete
stamps the old tuple's header with the command that ended it.
sm1 never writes into an existing version: a delete or update
is a new version with its own `seq`, so one number does both
jobs. The mark lives only in the session, since it matters only
while the transaction issues statements.

---

## 11. The isolation levels

| Level | Snapshot | Beyond the rule of §4 |
| --- | --- | --- |
| READ COMMITTED | a fresh one for each statement | a write that meets a newer commit re-reads and goes on (§9) |
| REPEATABLE READ | one, taken at `sm1::begin` | first committer wins. Write skew is possible: this is snapshot isolation |
| SERIALIZABLE | one, taken at `sm1::begin` | reads are tracked, and a transaction that would complete a dangerous pattern of read-write dependencies fails with `SM_ERR_SERIALIZATION` |

`SM1_DESIGN.md` §6 and §7 and `SIREAD.md` have the detail,
and `mvcc/MVCC.md` has the outcome of each scenario at each
pairing of levels.

---

## 12. Commit, abort and crash

**Commit** (`SM1_DESIGN.md` §8.2):

1. Claim a `commit_ts`.
2. Write the `COMMITTED` record to `CF:txn`, synced. **This
   is the commit point.** The versions were already in
   RocksDB; from here they resolve as committed.
3. Move the `xid` from the active table to the commit cache.
4. Publish: let `visible_ts` pass the `commit_ts`.
5. Wait until `visible_ts` covers the `commit_ts`
   (`COMMITVISIBLE.md`).
6. Release its locks, and wake whoever waits on it.

**Abort** writes nothing. The transaction leaves the active
table, and its versions resolve as dead for want of a record.
It takes the same time whatever the transaction wrote.

**A crash** is an abort of everything that was running.
RocksDB's log restores what was written; any transaction with
no `COMMITTED` record is dead by §5. There is no redo pass
and no undo pass, and recovery takes no longer for a large
transaction than a small one. A prepared transaction is the
exception: it is rebuilt from its record and waits for its
coordinator.

**A rollback to a savepoint** is not like these. The
transaction lives on and may commit, and every version it
wrote carries the same `xid`, so the versions to undo are
deleted one by one; a chained key's are not deleted, but the
key's version from before the mark is put back under it
(`../VCHAIN.md` §6). `SAVEPOINT.md` is the design that would
make it work as abort does.

---

## 13. The data structures

Everything above rests on three structures on disk and a
handful in memory. The ones in memory hold nothing that
cannot be lost: a restart rebuilds them empty, or from the
three on disk.

### 13.1 On disk

Each is a RocksDB column family of one database.

| Structure | Key → value | Holds |
| --- | --- | --- |
| `CF:store.<storeid>`, one for each store; `CF:chain.<storeid>` for a chained one | `class ‖ userkey ‖ ~wts ‖ ~seq` → `flags ‖ xid or commit_ts ‖ payload` | the versions (§2); a chained store's chained keys too (`../VCHAIN.md` §2). `store.0` is sm1's own catalog, versioned like any other store |
| `CF:txn` | `xid` → state, `snapshot_ts`, `commit_ts`, and more if prepared | the fate of each transaction that reached a decision (§6) |
| `CF:meta` | a name → a number | the format version, and the leases of the counters: `next_ts` for `global_ts`, one for object ids, one for each sequence (§1) |

RocksDB's own log makes all three durable and atomic. sm1
keeps no log of its own.

### 13.2 In memory, for the database

One `db_t` for each open database, shared by all its
sessions (`src/db.h`). It lives from the first `sm1::opendb`
until `idle_close_ms` after the last `sm1::closedb`.

| Structure | Type | For | Guarded by |
| --- | --- | --- | --- |
| `global_ts` | atomic 64-bit counter | every `xid`, `wts` and `commit_ts` (§1) | none for `xid` and `wts`; `alloc_mu` for a `commit_ts` and for renewing the lease |
| `committing` | ordered set of `commit_ts` | the commit sequencer: commits claimed and not yet published (§3) | `alloc_mu` |
| `visible_ts` | atomic 64-bit | what a new snapshot is (§3) | written by the advancer |
| `active` | hash set of `xid` | the active table: transactions running or prepared (§5, step 2) | `txn_mu` |
| `cache` | an array of slots `{ xid, commit_ts }`, in sets of two, an `xid` hashed to its set | the commit cache (§7) | `txn_mu` |
| `snapshots` | multiset of `snapshot_ts` | the snapshots in use; its smallest is the GC watermark (§8) | `snap_mu` |
| `lock_table` | hash map from `lockid_t` (the storeid, and an oid or a 64-bit hash of the key) to the list of holders, each with a mode | explicit locks, `sm1::lock`, row and OID (§9); a table's IS or IX sits in a fast-path slot of its session instead, while no S, SIX or X on the oid is held or asked (`SM1_DESIGN.md` §5.1) | `lock_mu`, with one condition variable for every waiter |
| `write_latch` | 4,096 mutexes, chosen by hashing store and key | a write's check-then-put (§9) | itself |
| `xid_owner` | hash map `xid → session` | whom a writer waits for, for deadlock detection (§9) | `txn_mu` |
| the SSI tracker | nodes, SIREAD entries on points and on ranges, a map from `xid` to node | serializable isolation (§11) | one mutex |
| `purges`, `drops` | a list of tombstones to purge, and stores to drop once the watermark passes | garbage collection's queues (§8) | `gc_mu` |
| `readers`, `read_epoch` | two counters and which is current | how the sweep knows every reader that began before its pass has finished (§8) | atomics |

Two of the mutexes carry the argument of the earlier
sections:

- **`alloc_mu`** makes claiming a `commit_ts` and entering it
  in `committing` one step. Without that, the advancer could
  move `visible_ts` past a commit it had not yet seen.
- **`txn_mu`** makes leaving `active` and entering `cache`
  one step. Without that, a reader could find a committing
  transaction in neither and call it dead.

### 13.3 In memory, for a transaction

One `sm1::session_t` for each session, holding at most one
transaction (`src/session.h`).

| Field | Is |
| --- | --- |
| `level` | the isolation level |
| `snapshot_ts` | the snapshot taken at `sm1::begin`, registered in `snapshots` |
| `stmt_snapshot_ts` | READ COMMITTED's snapshot for the statement in hand |
| `xid` | zero until the first write |
| `seq` | the last write's number within the transaction |
| `seq_hwm` | the newest own write still in effect; a rollback to a savepoint lowers it |
| `stmt_seq` | the statement's mark: its reads see own writes up to it, those made before the statement began, as PostgreSQL's command id |
| `wts_hi` | the largest `wts` written, so that a seek for the transaction's own versions starts high enough |
| `locks` | the explicit locks held, in the order taken, each with its mode; a write adds none |
| `undo` | the versions written while a savepoint is set, for a rollback to delete; capped by `savepoint_undo_bytes`, past which it is dropped and the savepoints lost |
| `savepoints` | a stack of marks into `locks`, `undo` and the lists below |
| `created`, `dropped` | stores made and removed, to drop or keep at the end |
| `scans` | the open cursors |
| `ssi` | the transaction's node in the SSI tracker, if serializable |

`undo` grows with what a transaction writes while a savepoint
is set, up to its cap; `locks` only with what it locks
explicitly. Everything else is of fixed size, which is why
abort is cheap, and why a large transaction's memory is now
bounded but for its explicit locks (`SM1_DESIGN.md` §13).

A **prepared** transaction is moved out of its session into
an object of the same type that the database owns, registered
by its global id, so that any session can resolve it. After a
restart the same object is rebuilt from the `PREPARED` record.

### 13.4 In memory, for a cursor

One `sm1::scan_t` for each open scan. It carries two views:

| View | Field | Fixed when the cursor opens |
| --- | --- | --- |
| physical | a RocksDB snapshot and an iterator on it | which bytes the cursor can reach at all |
| logical | `snapshot_ts`, `xid`, `seq_hwm` | which of those versions are visible, by §4 |

The RocksDB snapshot keeps the iterator stable while
compaction and other writers change the store under it. The
frozen `seq_hwm` keeps the cursor from seeing rows its own
transaction writes after it opened.

---

## 14. Where to read more

| For | Read |
| --- | --- |
| the full design, and the reasons | `SM1_DESIGN.md` |
| the commit cache | `COMMITCACHE.md` |
| each scenario's outcome, by level | `mvcc/MVCC.md` |
| serializable isolation | `SIREAD.md` |
| savepoints without an undo list | `SAVEPOINT.md` |
| the calls | `API.md` |
| chained keys, newest version under the plain key | `../VCHAIN.md` |
