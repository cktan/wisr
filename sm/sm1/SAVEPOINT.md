# SAVEPOINT — rolling back without an undo list

A savepoint in sm1 is cheap to set and expensive to hold:
every write made while one is set is remembered in memory, so
that a rollback can delete it (`SM1_DESIGN.md` §6.5). This
file is the design of a rollback that deletes nothing and
remembers nothing per write.

**We are leaning towards keeping the current
implementation**, the undo list, and not building this. Three
reasons:

- **A rollback is rare.** What this design saves is paid
  only while a savepoint is held, to prepare for an event
  that seldom comes.
- **The current cost is already bounded.** The undo list is
  kept only while a savepoint is set, and
  `savepoint_undo_bytes` caps it. Past the cap the
  transaction keeps writing and only the rare rollback
  becomes an abort.
- **The change is to the core.** The visibility rule, the
  status record, the commit cache, the compaction filter and
  recovery all change, and the database format with them,
  for that partial saving.

It would be worth revisiting if drivers that set a savepoint
around every statement turn out to be common for the fork.
A fourth reason, that the lock list held an entry for each
key written and would stay either way, is gone: a write keeps
no lock (`WRITELOCK.md`), so the undo list is now what a
large transaction holding a savepoint costs.

It is a design and not yet built. It changes sm1's visibility
rule, its status record and its compaction filter, so it is a
change to the core and is argued here first.

---

## 0. The proposal in brief

**The idea**

- A rollback to a savepoint does not delete the versions
  written since. It records that their `seq` numbers are
  dead: one range, however many versions.
- A version is then live only if its transaction committed
  **and** its `seq` is in no dead range of that transaction.
- The dead ranges travel with the transaction's status:
  in memory while it runs, in its status record when it
  commits or prepares.

**What it gives**

- A savepoint costs nothing to hold. The undo list is gone,
  for every transaction.
- A rollback writes nothing to RocksDB and takes the same
  time whatever was written since the mark.
- A bulk load can be rolled back to a savepoint, by either of
  its paths, with no code of its own (`BULKLOAD.md`).

**What it costs**

- Rolled-back versions stay on disk until compaction drops
  them, as an aborted transaction's do.
- A `seq` is never reused, so a transaction that rolls back
  a great deal runs out of them sooner.
- Every status lookup also asks whether a `seq` is dead. For
  a transaction that never rolled back, that is one test of
  an empty list.
- The status record changes, which is a new database format
  version.

**What does not change**

- The API. `sm1::savepoint`, `sm1::forget` and
  `sm1::rollback` mean what they meant.
- The version's key and value. `seq` is already in the key.
- Abort, which was always done this way: nothing deleted,
  the versions dead by their status.
- The lock list, `T.locks`, of explicit locks alone, since a
  write keeps none (`WRITELOCK.md`). A rollback still releases
  the locks taken after the mark, so they are still listed.

---

## 1. Today

A write puts its version into RocksDB at once (`SM1_DESIGN.md`
§5). A rollback to a savepoint must take such versions back
out, and it does so one by one:

| | Today |
| --- | --- |
| each write, while a savepoint is set | one undo entry in memory: store, key, `wts`, `seq` |
| holding a savepoint over `n` writes | `n` entries, keys inline, until the last mark is forgotten, or until they pass `savepoint_undo_bytes`, when the list is dropped and the savepoint lost |
| rolling back | one RocksDB delete for each entry after the mark |
| after a rollback | `seq` is rewound, and the next write reuses the undone numbers |

Two choices made elsewhere force it.

- **Writes are eager.** A version to undo is already in the
  database.
- **Visibility is by `xid`, one for the whole transaction.**
  A version left in place would commit with the rest.

So a transaction that sets a savepoint and then writes a
great deal holds memory for it up to the cap, and past the
cap can no longer be rolled back to that savepoint, only
aborted. That is the case
of a large statement run by a driver that savepoints every
statement, or inside a PL/pgSQL block with an exception
handler.

Abort has neither cost. It deletes nothing and lists nothing:
the versions are dead because their transaction has no commit
record (`SM1_DESIGN.md` §3.4). This design makes a rollback
to a savepoint work the same way.

---

## 2. The mechanism

### 2.1 `seq` is never reused

`seq` counts a transaction's writes, from 1, and is part of
every version's key. Today a rollback rewinds it. Here it
only ever goes up: a rollback leaves `T.seq` where it is.

That is what makes a `seq` a name for a write. A range of
`seq` then names exactly the writes made between two moments,
and none made later.

### 2.2 A mark and a rollback

A mark records `T.seq` as it was when the savepoint was set.

```
sm1::savepoint():
    mark.seq = T.seq                  # and locks, stores, scan_id
    push mark

sm1::rollback(depth):
    m = T.savepoints[depth-1]
    T.dead.add(m.seq + 1 .. T.seq)    # one range (§2.3)
    drop the stores created after the mark, restore the lists
    close the cursors opened after the mark
    release the locks taken after the mark
    T.savepoints.truncate(depth)

sm1::forget(depth):
    T.savepoints.truncate(depth - 1)  # nothing else to do
```

No delete is written. The work of a rollback no longer grows
with what was written since the mark, but for the locks it
releases.

### 2.3 The dead ranges

`T.dead` is a sorted list of ranges of `seq`, none touching
another.

Adding the range of a rollback to mark `m`:

1. Every range that starts after `m.seq` lies inside the new
   one, being work done since the mark. They are removed.
2. If the last range left ends at `m.seq`, no write survives
   between it and the new one, and it is extended instead.
3. Otherwise the new range is appended.

So the list grows only when a rollback is followed by writes
that are kept and then by another rollback. A loop that sets
a savepoint, writes, and rolls back holds one range however
often it runs. A loop that also keeps a write each time adds
a range each time, of two numbers.

No limit is proposed on the list's length. It is the analogue
of PostgreSQL's subtransaction ids, which grow the same way.

### 2.4 Visibility

The status of a version becomes a function of its `xid` and
its `seq`, where it was a function of its `xid`.

```
if V.xid == T.xid:                      # own write
    visible iff V.seq <= view.seq_hwm
                and V.seq is not in T.dead
elif V.wts > snapshot_ts:
    invisible
else:
    resolve (V.xid, V.seq)  →  commit_ts | dead | in-flight
    visible iff COMMITTED and commit_ts <= snapshot_ts
```

and §3.4's rule gains one clause: a transaction that is
COMMITTED answers *dead* for a `seq` in its dead ranges.

- **Own writes.** `seq_hwm` keeps one job, the one a cursor
  needs: a cursor sees the writes made before it opened, and
  freezes `seq_hwm` to say so. The other job, hiding undone
  writes, moves to `T.dead`. A cursor asks its transaction's
  list as it is now, so a cursor opened before a rollback
  stops seeing what the rollback undid without being told.
- **Stamped versions.** A version whose header carries a
  commit timestamp is committed and needs no lookup, as now.
  §2.6 is why no dead version is ever stamped.
- **One resolver.** Every reader of a version's fate goes
  through `xct_status`: point reads, scans, a write's check
  of the newest version, and the compaction filter. The
  `seq` is passed there, and nowhere else changes.

### 2.5 Where the ranges are kept

| The transaction is | The ranges are in |
| --- | --- |
| running | its entry in the active table, so that others can ask |
| prepared | the `PREPARED` record, and the in-doubt object rebuilt from it |
| committed | the `COMMITTED` record, and the commit cache entry made from it |
| aborted | nowhere: everything is dead |

**Others must be able to ask of a running transaction.** A
rollback wakes the writers waiting on the transaction
(`WRITELOCK.md` §3.4). Such a writer then looks again for its
key's newest version, and what it finds first is the
rolled-back one: in RocksDB still, written by a transaction
still running. Today it would have been deleted. Here the
writer resolves it, rather than waiting for its writer,
finds its `seq` dead, and steps to the version below, as if
it had never been written. So the list in the active table is
read by other threads while its owner adds to it, and is
guarded for that.

A reader meeting a running transaction's version skips it
whether or not it is dead, and needs no list.

**The commit record carries the list**, written in the one
batch that commits. A transaction that never rolled back
writes an empty one. The record is read when a status is not
in the commit cache, and the cache keeps the list with the
commit timestamp.

**The record is kept as long as it is today.** It may go only
once every surviving version of its transaction is stamped
(`SM1_DESIGN.md` §8.3), and §2.6 shows that by then every
dead one has been dropped.

### 2.6 Compaction

The filter resolves each unstamped version it meets. One rule
is added, ahead of the rules for committed versions:

```
filter(version_key, value):
    resolve (xid, seq)
    if dead (aborted, crashed, or seq in a dead range) → DROP
    if in-flight                                       → KEEP
    ... the committed rules, unchanged
```

- **A dead version is never stamped.** A stamp replaces the
  `xid` in the header and is believed without a lookup, so a
  stamped dead version would be a committed one. The filter
  has the transaction's record when it stamps, because the
  record outlives the stamping, and so it has the ranges.
- **A dead version is not "a newer version seen".** The
  filter drops an old version once it has seen a newer
  committed one below the watermark. A dead version must not
  count as that newer one, which is why the new rule comes
  first.
- **The sweep's argument stands.** The full pass visits every
  version at or below its watermark. A dead one is dropped
  and a live one stamped, so afterwards none needs the
  record.

A running transaction's dead versions are kept until it ends.
Dropping them sooner is possible, since the active table has
the list, and is not proposed.

### 2.7 Prepare and recovery

- **Prepare** writes the list into the `PREPARED` record. No
  write follows a prepare, so the list is final.
- **Recovery** rebuilds an in-doubt transaction from that
  record, list included, and its commit copies the list into
  the `COMMITTED` record.
- **A crash** of a running transaction loses its list with
  it, which is right: every version it wrote is dead.

### 2.8 What a rollback still does itself

The versions were one of several things a rollback undoes.
The others are untouched by this design.

| Undone | How | Changes |
| --- | --- | --- |
| versions written | their `seq` range is recorded dead | new |
| stores created after the mark | their column families are dropped | no |
| stores and sequences dropped after the mark | the lists are cut back; the catalog rows' deletes are versions, and are dead with the rest | no |
| locks taken after the mark | released, from `T.locks` | no |
| cursors opened after the mark | closed | no, though no longer needed for correctness (§2.4) |
| SIREAD locks | not undone, which is conservative and correct | no |
| a lock's READ COMMITTED mark | spent marks stay spent; the newest version is the other transaction's once more, so the next write needs its read again (`SM1_DESIGN.md` §5.4) | no |

---

## 3. What it costs

- **Dead versions stay until compaction.** A rollback of a
  million writes leaves a million versions on disk and in
  the way of reads, which step past them, until compaction
  visits them. An aborted transaction leaves the same.
- **`seq` runs out sooner.** It is four bytes, and a write
  past the 2^32nd is refused. Today rolled-back writes give
  their numbers back; here they do not. A transaction that
  writes and rolls back a billion rows four times is at the
  limit. Widening `seq` changes the version key, which is a
  larger format change than this one, and is not proposed.
- **A lookup more on the status path**, for versions not yet
  stamped: whether the `seq` is in the list. The list is
  empty for nearly every transaction.
- **A larger status record**, by eight bytes a range, in the
  status table and in the commit cache.
- **A new format version.** The status record gains a field,
  so `WISR_DB_VERSION` goes from 6 to 7, and an older
  database is refused, not migrated, as before.

---

## 4. Alternatives considered

| Alternative | Why not |
| --- | --- |
| Today's undo list, kept only while a savepoint is set, and capped | Built, and what this replaces. A transaction with no savepoint is free of it. One with a savepoint pays for every write up to `savepoint_undo_bytes`, and past that keeps writing but can no longer roll back to it (`SM1_DESIGN.md` §6.5) |
| The undo list spilled to disk | Bounded memory, but a file with a life of its own, and a rollback still issues a delete for each write |
| An xid for each savepoint, as PostgreSQL's subtransactions | The same effect: a rolled-back subtransaction's xid never gets a commit record, and its versions are dead by the existing rule. But a commit then writes a record for each surviving subtransaction; the active table, the commit cache, the prepare record and SSI's bookkeeping all learn that one transaction has many xids; and a transaction must know which xids are its own to read its own writes. The ranges keep one xid and one record |

---

## 5. What has to be built

All of it is in sm1.

| Where | What |
| --- | --- |
| `session.h` | `T.dead`; the mark holds `seq` and no longer an undo position; the undo list is removed |
| `access.cc` | a write appends nothing for rollback; the own-write test and both resolver calls pass `seq` |
| `xct.cc` | `savepoint`, `forget`, `rollback` as in §2.2; the range rule of §2.3; `xct_status` takes a `seq`; the list in the active table, the commit record, the commit cache, the prepare record and the in-doubt object |
| `gc.cc` | the filter passes `seq` and drops a dead version before any committed rule |
| `recover.cc` | the list restored with a prepared transaction |
| `format.cc`, `db.h` | the status record's new field; format version 7 |
| `SM1_DESIGN.md` | §3.2, §3.4, §4.4, §5, §6.5, §8.3, §8.4 and §13, rewritten to this |
| `API.md` | the cost of holding a savepoint, and the `seq` limit |

`BULKLOAD.md` loses its rule that a rollback past a load is
refused. A load's versions carry a `seq` like any other, and
a range covers them.

---

## 6. To check first

- that nothing reads a version's status but through
  `xct_status` and the own-write test: the two calls in
  `access.cc`, the one in `gc.cc`, and anything SSI does with
  a version it steps past;
- how a reader that skips a restored prepared transaction's
  version is treated (`SM1_DESIGN.md` §8.4), and that a dead
  version there must not count against it;
- that every caller of a cursor's view can reach its
  transaction's list, the prepared object's among them;
- what the commit cache's bound means once an entry can be
  large.

---

## 7. Order of work

Tests first at each step, in `test/test_savepoint.cc` unless
said.

1. The checks of §6.
   → verify: each written up here.
2. `seq` no longer rewound, with the undo list still there.
   → verify: the whole suite passes unchanged. This step is
   only to find what depended on the rewind.
3. `T.dead`, own-write visibility by it, and the range rule.
   → verify: the range list after nested rollbacks, repeated
   rollbacks to one mark, and a loop of each kind in §2.3;
   the transaction and its open cursors see what they saw
   before.
4. The resolver takes `seq`; the list in the active table.
   → verify, as a scenario in `mvcc/`: another writer takes a
   key whose only newer version was rolled back by a
   transaction still running, and writes it.
5. The commit record, the commit cache, and the format.
   → verify: after commit another session sees the kept
   writes and none rolled back, with the cache and after a
   restart without it.
6. The filter.
   → verify, in `test/test_gc.cc`: a rolled-back version is
   dropped and never stamped; an older committed version
   under a dead one survives; the sweep still removes the
   record.
7. Prepare and recovery.
   → verify, in `test/test_2pc.cc` and `test/test_crash.cc`:
   a prepared transaction's rolled-back writes stay dead
   across a restart and after its commit.
8. Remove the undo list and the deletes.
   → verify: a savepoint held over a million writes adds
   nothing to the session's memory, and a rollback writes
   nothing to RocksDB.
9. The documents of §5.
