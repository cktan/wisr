# LOCK — what gets locked, and how

Locking in this stack is spread over `sm1/SM1_DESIGN.md` §5
and §7, `sm1/WRITELOCK.md`, `sm3/SM3_DESIGN.md` §2 and
`sm3/CATALOG3.md` §0.4. This file gathers it: the two
mechanisms, the interface, what is a lock subject and what only
looks like one, and the scenarios that are easy to get wrong.

It describes the code as it is.

---

## 0. There are two mechanisms, and one rule

**The version is the lock.** A write puts its uncommitted
version under a short latch and keeps no lock entry. The next
writer of that key finds the version, waits for the
transaction that wrote it, and starts again
(`sm1/WRITELOCK.md`). A transaction's memory does not grow
with the keys it writes.

**The lock table is the other.** `sm1::lock` puts an entry in
a shared table, in one of Gray's modes, held to the end of the
transaction. This is for ordering access to something the
caller is **not** writing.

**The rule that decides which:**

> Write the key and the version is the lock. Need ordering on
> something you are not writing, and you need `sm1::lock`.

Everything in §2 and §3 follows from it. A **latch** is
neither: a mutex hashed on store and key, held only inside a
call, so that check-then-write is one step.

---

## 1. The interface

```cpp
int sm1::lock(session_t*, locktype_t, uint64_t storeid,
              std::string_view key, lockmode_t);
```

**`locktype_t` says what the name means**, and it selects a
path, not just a prefix:

| type | what sm1 does |
| --- | --- |
| `LOCKTYPE_ROW` | validates the store and key, waits for an uncommitted writer of the key, and once held runs the currency check of §1.1 |
| `LOCKTYPE_SCHEMA` | a name in no store: no store is read, no version is checked, no latch is taken. A lock-table entry and nothing else |
| `LOCKTYPE_OBJECT` | reserved; no caller yet |

The lock table is keyed `type ‖ be64(storeid) ‖ userkey`, so
**two locks of different types never conflict**, however equal
their storeid and key.

**`lockmode_t` is Gray's five**, defined once in the root's
`include/sm_lock.h` and shared by sm1 and the C wrapper:
`SM_LOCKMODE_SH`, `_EX`, `_IS`, `_IX`, `_SIX`. The intention
modes lock a name standing for a larger object to announce
locks below it; sm1 keeps no hierarchy itself, only the
conflict table on each name.

**Lifetime.** To the end of the transaction, or until an
`sm1::rollback` past the point the lock was taken. A
**conversion is never undone** by a rollback: the name stays
locked in the stronger mode until the transaction ends.

**Across a restart.** A prepared transaction's lock set —
`(key, mode)` per lock — is in its `PREPARED` record, and
recovery re-acquires each in its mode. So a lock name must be
stable across restarts and builds: no host byte order, no
per-process hash seed.

### 1.1 The currency check

A successful `LOCKTYPE_ROW` lock means **what the caller reads
next under it is current**. That is what makes "lock, then
read, then act" safe, and it is why the call fails with
`SM_ERR_CONFLICT` when another transaction committed a version
of the key after this transaction's snapshot — at REPEATABLE
READ and SERIALIZABLE. At READ COMMITTED it does not fail
there; the caller reads with `sm1::read_latest` instead.

`LOCKTYPE_SCHEMA` runs no such check, because no version of
such a name can exist.

---

## 2. The lock subjects

Things `sm1::lock` must name, because ordering is needed on
something the caller is not writing.

| # | Subject | Name | Guards | Today |
| --- | --- | --- | --- | --- |
| 1 | a table's definition | `SCHEMA`, `be64(oid)` | DDL against readers and writers of the in-memory catalog | built: `cat_lock_table` → `lock_table` → `sm1::lock` |
| 2 | a heap row, read intent | `ROW`, `(storeid, relid ‖ rowno)` | `FOR SHARE`, `FOR UPDATE`, an index build against a writer | built: `tbl_lock` → `rel_lock` → `sm1::lock` |
| 3 | a catalog key, lock-then-read | `ROW`, `(store 0, "U" ‖ key)` | DDL takes X, reads the row current, may rewrite it | built: `catkv_lock` → `sm1::lock` |
| 4 | a namespace | `(oid)` | `DROP SCHEMA` against `CREATE` in it | not built |
| 5 | a collation or type in use | `(oid)` | `DROP COLLATION` with a column using it | not built |
| 6 | an index as an object | `(oid)` | `DROP INDEX` against a scan on it | subsumed by 1 |
| 7 | a sequence as an object | `(oid)` | `CREATE` / `DROP SEQUENCE` | no sm3 sequence object yet |

Every name in use today is **fixed-width**: 9 bytes for a
schema lock, 11 for a heap row, 13 for the catalog keys DDL
locks. Nothing in sm1 requires that; `max_keylen` 4096 bounds
a name.

---

## 3. What only looks like a lock subject

Each of these is ordered by a write, so the lock table is not
involved.

- **A unique index key.** Dedup is two rows producing the
  *same* sm1 key, which the inserter writes. §4.1.
- **A name in a namespace.** `CREATE TABLE` inserts the
  `name:` key; a taken one is `SM_ERR_DUPKEY`.
- **A non-unique index entry.** `entry_key` appends the
  `rowno` whenever the entry is not uniquely keyed, so two
  rows never share one. Nothing to contend on.
- **A row write, update or delete.** The uncommitted version
  is the lock.
- **Gaps and ranges.** Phantoms are SSI's SIREAD range
  predicates, checked by `ssi.on_write`. Nothing is held on a
  gap and a conflict is a serialization failure, not a wait.
- **A sequence's position.** `sm1::nextseq` is outside MVCC,
  takes no locks and is not undone by an abort.
- **A TOAST chunk.** It belongs to its row, covered by 2.

---

## 4. Tricky scenarios

### 4.1 Two transactions insert the same unique key

A unique index's `entry_key` omits the `rowno` when no key
column is null, so both rows produce one key.

```
T1  idx_insert id=42 -> relid|02|80..2a   under the latch:
                                          absent, put version (xid T1)
T2  idx_insert id=42 -> the same key      under the latch: finds T1's
                                          uncommitted version
T2                                        drops the latch, waits for T1
T1  commit
T2  retry -> the key is there committed -> SM_ERR_DUPKEY
```

- **No lock entry is taken**, by either. A million rows into a
  table with a unique index holds none.
- **If T1 aborts**, T2's retry finds no live version and
  succeeds. Waiting rather than failing is what turns a benign
  race into a success.
- **A snapshot read could not do this.** T1's uncommitted
  entry is invisible to T2's snapshot, so a "does it exist?"
  check would let both commit. Uniqueness has to hold at READ
  COMMITTED too, where there is no SSI to catch it, which is
  why `sm1::insert` is judged there against the newest
  committed version and not against the snapshot.
- **A key with a null** gets the `rowno` appended
  (`has_null`), so several NULLs coexist, as in PostgreSQL —
  and such an entry behaves like a non-unique one.

### 4.2 Why the schema lock is a type, not a key

DDL and a reader need ordering on a table's **definition**,
which lives in memory. There is no row to wait on, so the lock
table is the only mechanism — subject 1.

The trap is which name to lock. Lock the `tbl:` key, which
every `ALTER TABLE` rewrites, and §1.1's check fires:

```
T2  begins (snapshot S2)
T1  ALTER TABLE ... commits        a version of tbl:<oid> now newer than S2
T2  lock tbl:<oid>  ->  SM_ERR_CONFLICT at REPEATABLE READ
```

T2 is the reader that waited out the DDL and should be handed
the new definition. Failing it is wrong.

The design once dodged this by locking a `LOCK` key that no
DDL ever wrote — correct, but a convention the next reader had
to notice. `LOCKTYPE_SCHEMA` makes it structural: no version
of that name can exist, so the check cannot fire, at any
isolation level.

**Both locks are still taken**, and they do different jobs.
`open_table` calls two wrappers; each is one `sm1::lock`:

```
sm3::lock_table(s, 16384, X)
  -> sm1::lock(s, LOCKTYPE_SCHEMA, 0, be64(16384), SM_LOCKMODE_EX)
     mutual exclusion; no store, no version

sm2::catkv_lock(kv, entity_key(table, 16384), X)
  -> sm1::lock(s, LOCKTYPE_ROW, 0, "U" | kind(4) | be64(16384),
               SM_LOCKMODE_EX)
     currency, because that key IS written
```

At READ COMMITTED the second one lets the DDL overwrite a
newer version after a `catkv_get_latest`; at REPEATABLE READ
it is the correct `CONFLICT`.

### 4.3 `FOR UPDATE` against a writer

```
T1  sm2::rel_lock(rel, 7, X)
      -> sm1::lock(s, LOCKTYPE_ROW, 1001, be16(1)|be64(7), SM_LOCKMODE_EX)
T2  sm2::rel_update(rel, 7, ...)
      -> sm1::update(s, 1001, be16(1)|be64(7), ...)
         blocks: a write waits for a conflicting lock on the
         key it writes
```

This works only because both name `(storeid, relid ‖ rowno)` —
the lock and the write meet on the data key.

**So row locks cannot be moved to a namespace of their own.**
Give them a synthetic name and a writer, which takes no entry,
would never see them: `FOR UPDATE` would silently stop
blocking writers. That is the constraint behind §1's split
between `ROW` and the other types.

### 4.4 A concurrent index build against a writer

The build must not add an entry for a row while a writer is
changing it. The entry key is the obvious thing to serialize
on, and it is the wrong one — the build locks the **heap row**
instead:

```cpp
// Hold the row shared to the end of the batch: a writer
// changes the heap row first, so it and the build never
// work on one row at once (SM3_DESIGN.md §3).
sm2::rel_lock(table->rel, r, SM_LOCKMODE_SH)
```

The build is also the only thing that writes index entries
without writing the heap row — which is exactly why §3's claim
that a non-unique entry needs no lock holds: every writer of
such an entry holds a lock or a version on its heap row.

### 4.5 Lock, read current, act — at READ COMMITTED

`sm1::read_latest` reads the newest committed version, past
the snapshot. It is refused unless something keeps that
version from changing: a lock the transaction already holds,
or its own write still in flight.

```
T1  lock(ROW, store, k, X)        held; at RC, no currency check
T1  read_latest(k)                the newest committed value
T1  update(k, ...)                allowed: the X holder was marked
```

At REPEATABLE READ the lock itself would already have failed
on a newer version, which is the point: the levels differ in
*where* the staleness is caught, not in whether it is.

### 4.6 Two conversions deadlock

A transaction that holds a name and asks for a mode its
current one does not cover converts, waiting for the other
holders that conflict.

```
T1  lock(k, IX)      ok
T2  lock(k, IX)      ok, IX and IX are compatible
T1  write k   ->     wants X, waits for T2
T2  write k   ->     wants X, waits for T1
```

sm1's detector picks a victim and fails it with
`SM_ERR_DEADLOCK`; the younger transaction loses. Under
`sm1::set_qa_nowait` the call returns `SM_ERR_BLOCKED` instead
of waiting at all.

### 4.7 `TRUNCATE` and why the lock name is the oid

`cat_truncate_table` makes a new store, points the table's
record at it, and drops the old one — under the schema lock X.

A lock named by **storeid** would move under it: a stale
reader could hold `(old storeid, …)` while a DDL holds `(new
storeid, …)`, and both would believe they had the table's
schema lock. The oid is the only identity stable for the
table's whole life, so it is the name.

This is also why the lock cannot live *in* the table's store:
it has to outlive `DROP TABLE`'s `rmstore` — `CATALOG.md` §5
promises that a table dropped while a caller waited is
`SM_ERR_NOTFOUND`, which means getting the lock and *then*
finding no definition — and it has to pre-date the store,
since `CREATE TABLE` inserts the name before making the store.

### 4.8 Publishing under the lock

Releasing the schema lock is not enough. sm1's commit releases
it, and a waiter could then read the shared catalog before the
DDL's new definition was published into it.

So `commit` holds the catalog's mutex **exclusive across both**
sm1's commit and the publish. A transaction that then gets the
schema lock waits on that mutex, and so sees the new
definition, never the old.

---

## 5. Traps worth writing down

- **A synthetic lock name costs an entry.** Any name sitting
  where no version exists has to be a real lock-table entry.
  Moving dedup off the key it writes — hashing the value, say
  — would put one entry per inserted row back into every
  transaction, which is the cost `WRITELOCK.md` removed. The
  hash is not the problem; leaving the written key is.
- **Hashing a lock *name* is safe; hashing a *key* is not.** A
  name collision costs a false wait, and sometimes a false
  deadlock abort, because the exact operation still decides.
  A key collision would refuse a value that is free.
- **Deadlock is reported, not avoided.** There is no lock
  ordering rule in the stack; `SM_ERR_DEADLOCK` is retryable
  and the caller retries.
- **Subjects 4 to 7 are not built.** `DROP COLLATION` in use
  is `CATALOG.md` §7's open item, and `DROP INDEX` leans on
  the table's schema lock.
