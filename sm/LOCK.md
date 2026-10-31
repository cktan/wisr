# LOCK — what gets locked, and how

Locking in this stack is spread over `sm1/SM1_DESIGN.md` §5
and §7, `sm1/WRITELOCK.md` and `sm3/SM3_DESIGN.md` §2. This
file gathers it: the two
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

**The lock table is the other.** `sm1::lock_row` and
`sm1::lock_oid` put an entry in a shared table, in one of
Gray's modes, held to the end of the transaction. This is for
ordering access to something the caller is **not** writing.

**The rule that decides which:**

> Write the key and the version is the lock. Need ordering on
> something you are not writing, and you need a lock:
> `sm1::lock_row` on a key, `sm1::lock_oid` on an object.

Everything in §2 and §3 follows from it. A **latch** is
neither: a mutex hashed on store and key, held only inside a
call, so that check-then-write is one step.

---

## 1. The interface

```cpp
int sm1::lock_row(session_t*, uint64_t storeid,
                  std::string_view key, lockmode_t,
                  bool as_read = false);
int sm1::lock_oid(session_t*, uint64_t oid, lockmode_t);
```

`sm1::trylock_row` and `sm1::trylock_oid` take the same lock
if it can be had at once, and never wait.

**Each call is its own path**, not just its own name:

| call | what sm1 does |
| --- | --- |
| `sm1::lock_row` | validates the store and key, waits for an uncommitted writer of the key, and once held runs the currency check of §1.1 |
| `sm1::lock_oid` | an object id in no store: no store is read, no version is checked, no latch is taken. A lock-table entry and nothing else, or, taken IS or IX, a slot of the session's own (`sm1/SM1_DESIGN.md` §5.1, the fast path) |

Each lock has a type, `LOCKTYPE_ROW` or `LOCKTYPE_OID`, which
`sm1::lock_status` reports and the `PREPARED` record keeps. A
lock is named `type ‖ be64(storeid) ‖ userkey`, an oid's with
storeid 0 and userkey `be64(oid)`, so **two locks of different
types never conflict**, however equal their storeid and key.
**The shared lock table holds only a hash, never a row's
key.** It keys an entry by two 64-bit words, the storeid and
the oid, or a hash of a row's key, and an entry holds those
words, its holders and their modes, nothing more. So conflicts
are decided on the hash, and two rows whose keys hash alike may
conflict needlessly (`sm1/SM1_DESIGN.md` §5.1). The key is kept
whole only by the transaction that took the lock, in its own
lock list, and in what that list feeds: the `PREPARED` record
and `sm1::lock_status` (`sm1/LOCKID.md` plans to drop it there
too).

**What the caller passes.** The name, whole; sm1 does any
encoding and hashing.

- **`sm1::lock_row`**: `storeid` is the row's store and `key`
  its sm1 key, as `sm1::read` takes them. The caller does not
  hash it. sm1 hashes the key into the lock table's second
  word (`lockid` in `sm1/src/lockmgr.cc`) and nowhere else: the
  key is kept whole in the lock list, the `PREPARED` record and
  `sm1::lock_status`, and two rows hashing alike only wait for
  each other, never share a lock. `rel_lock` locking a row of
  table store 12: `sm1::lock_row(s, 12, key, SM_LOCKMODE_EX)`.
- **`sm1::lock_oid`**: the oid itself. sm1 names it in no
  store, as `be64(oid)`, and the table's words are `(0, oid)`
  exactly, with no hash and so no needless conflict.
  `cat_lock_table` locking table 16384:
  `sm1::lock_oid(s, 16384, SM_LOCKMODE_IS)`.

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

A successful `sm1::lock_row` means **what the caller reads
next under it is current**. That is what makes "lock, then
read, then act" safe, and it is why the call fails with
`SM_ERR_CONFLICT` when another transaction committed a version
of the key after this transaction's snapshot — at REPEATABLE
READ and SERIALIZABLE. At READ COMMITTED it does not fail
there; the caller reads with `sm1::read_latest` instead.

At READ COMMITTED there is no `SM_ERR_CONFLICT` to ignore:
the call returns 0 with the lock held and runs no check
(`lock_versioned` in `sm1/src/access.cc` returns before
`access_check_current`). The caller then reads the key with
`sm1::read_latest`, which requires that lock. T1 commits an
update of `id = 7` after T2's snapshot, and T2 runs
`SELECT * FROM t WHERE id = 7 FOR UPDATE`:

```
T2 at READ COMMITTED:
  sm1::lock_row(t, key(7), X)  -> 0, no check
  sm1::read_latest(t, key(7))  -> T1's version
T2 at REPEATABLE READ:
  sm1::lock_row(t, key(7), X)  -> SM_ERR_CONFLICT, and T2 fails
```

`sm1::lock_oid` runs no such check, because no version of such a
name can exist.

**One type serves every object.** Every sm3 object -- table,
index, namespace, collation -- draws its oid from the one
sequence `sm3.oid`, so `be64(oid)` is unambiguous across kinds
and every subject of §2 that is an object -- a table's
definition (#1), a namespace (#4), a collation or type (#5),
an index (#6), a view's definition (#8) -- takes
`sm1::lock_oid`, with no kind byte and no collision.

---

## 2. The lock subjects

Things a lock must name, because ordering is needed on
something the caller is not writing.

| # | Subject | Name | Guards | Today |
| --- | --- | --- | --- | --- |
| 1 | a table's definition | `OID`, `be64(oid)` | DDL against readers and writers of the in-memory catalog | built: `cat_lock_table` → `lock_table` → `sm1::lock_oid` |
| 2 | a row, read intent | `ROW`, `(storeid, relid ‖ row key)` | `FOR SHARE`, `FOR UPDATE`, an index build against a writer | built: `tbl_lock` → `rel_lock` → `sm1::lock_row` |
| 2a | a row's `_id` entry | `ROW`, `(storeid, 0xFFFE ‖ _id)` | a writer that followed a moved row, against moving it again | gone, with the `_id` index (`ROWKEY.md` §2.1, §2.3) |
| 3 | a catalog key, lock-then-write | `ROW`, `(store 0, "U" ‖ key)` | DDL takes X on each key it is about to write, reads it current, rewrites it | built: `catkv_lock` → `sm1::lock_row` |
| 4 | a namespace | `OID`, `be64(oid)` | `DROP SCHEMA` against `CREATE` in it | built: `cat_put_table`, `cat_put_collation` take S, `cat_del_namespace` X → `lock_table` → `sm1::lock_oid` |
| 5 | a collation or type in use | `(oid)` | `DROP COLLATION` with a column using it | not built |
| 6 | an index as an object | `(oid)` | `DROP INDEX` against a scan on it | subsumed by 1 |
| 7 | a sequence as an object | `(oid)` | `CREATE` / `DROP SEQUENCE` | none: its record is a catalog key (3), read in the transaction; `cat_put_sequence` takes its namespace S (4) (`VIEW.md` §11) |
| 8 | a view's definition | `OID`, `be64(oid)` | `DROP VIEW`, `CREATE OR REPLACE` against a query expanding it | built: `cat_lock_view` → `lock_table` → `sm1::lock_oid` (`VIEW.md` §7) |

A name is **fixed-width** but for a row's: 9 bytes for a
table-oid lock, the width of its kind for each catalog key DDL
locks, 11 for a row of a table without a primary key, and for
a row of a table with one, the
relid and its primary key's encoding, up to 1024 bytes
(`CLUSTER.md` §4). Nothing in sm1 requires a width;
`max_keylen` 4096 bounds a name.

---

## 3. What only looks like a lock subject

Each of these is ordered by a write, so the lock table is not
involved.

- **A unique index key.** Dedup is two rows producing the
  *same* sm1 key, which the inserter writes. §4.1.
- **A name in a namespace.** `CREATE TABLE` inserts the
  `name:` key; a taken one is `SM_ERR_DUPKEY`.
- **A non-unique index entry.** `entry_key` appends the row's
  key whenever the entry is not uniquely keyed, so two rows
  never share one. Nothing to contend on.
- **A primary key.** The rows are keyed by it, so two rows
  with one key are one sm1 key, ordered as a unique entry is
  (§4.1).
- **A row write, update or delete.** The uncommitted version
  is the lock.
- **Gaps and ranges.** Phantoms are SSI's SIREAD range
  predicates, checked by `ssi_on_write`. Nothing is held on a
  gap and a conflict is a serialization failure, not a wait.
- **A sequence's position.** `sm1::nextseq` is outside MVCC,
  takes no locks and is not undone by an abort.
- **A TOAST chunk.** It belongs to its row, covered by 2.

---

## 4. Tricky scenarios

### 4.1 Two transactions insert the same unique key

A unique index's `entry_key` omits the row's key when no key
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
- **A key with a null** gets the row's key appended
  (`has_null`), so several NULLs coexist, as in PostgreSQL —
  and such an entry behaves like a non-unique one.

### 4.2 Why the table-oid lock is a type, not a key

DDL and a reader need ordering on a table's **definition**,
which lives in memory. There is no row to wait on, so the lock
table is the only mechanism — subject 1.

The trap is which name to lock. Lock the `tbl:` key, which an
`ALTER TABLE` may rewrite, and §1.1's check fires:

```
T2  begins (snapshot S2)
T1  ALTER TABLE ... commits        a version of tbl:<oid> now newer than S2
T2  lock tbl:<oid>  ->  SM_ERR_CONFLICT at REPEATABLE READ
```

T2 is the reader that waited out the DDL and should be handed
the new definition. Failing it is wrong.

The design once dodged this by locking a `LOCK` key that no
DDL ever wrote — correct, but a convention the next reader had
to notice. `LOCKTYPE_OID` makes it structural: no version
of that name can exist, so the check cannot fire, at any
isolation level.

**The table-oid lock orders, and row locks guard what is
written.** DDL takes two kinds of lock, at two moments.

First it takes the table's oid lock exclusive, in
`cat_get_table_rec`, and reads the table's record from the
catalog in memory, not from the store:

```
sm3::lock_table(s, 16384, X)
  // Hold table 16384's definition exclusively: no one may read
  // or write the table until this transaction ends.
  -> sm1::lock_oid(s, 16384, SM_LOCKMODE_EX)
     // Take the lock named 16384 in the schema namespace;
     // nothing writes that name, so no store is read and no
     // version checked.
```

Memory has every commit by the time the lock is granted, so
the record is the table as it now is, at any isolation level.
A read of the store would not do: at REPEATABLE READ it shows
the snapshot's table, without the column another transaction
has just added.

Then, in `cat_put_table`, it locks each catalog key it is
about to write, and only those — a table's own fields, one
column, one index each have a key (`sm3/CATALOG_ONDISK.md` §2):

```
sm2::catkv_lock(kv, part_key(column, 16384, 3), X)
  // Hold the catalog row of column 3 of table 16384, so the
  // write that follows goes over its newest version.
  -> sm1::lock_row(s, 0, "U" | kind(8) | be64(16384) | be64(3),
                   SM_LOCKMODE_EX)
     // Take the row lock on that key in store 0, failing if
     // another transaction committed a version of it after our
     // snapshot.
```

The pair reads twice over because the wrapper says *which
object* and `sm1::lock_row` says *which bytes*. That gap is
inherent to sm1 not knowing what a table is; the type tag
narrows it — a table-oid lock no longer pretends to be a row —
but does not close it.

At READ COMMITTED the row lock lets the DDL overwrite a newer
version after a `catkv_get_latest`; at REPEATABLE READ it is
the correct `CONFLICT`, and now only when the DDL writes the
very key another transaction changed after its snapshot. Two
DDL that write different keys of one table meet in no row
lock at all: the table-oid lock alone puts them in order.

A namespace has the same kind of lock, on its oid: shared by a
transaction putting a table or a collation in it, exclusive by
the one that drops it (`sm3/CATALOG_INMEM.md` §2.1).

### 4.3 `FOR UPDATE` against a writer

```
T1  sm2::rel_lock(rel, id_key(7), X)
      // Hold the row with _id 7 of a table without a primary
      // key exclusively without writing it: SELECT ... FOR
      // UPDATE.
      -> sm1::lock_row(s, 1001, be16(1)|be64(7), SM_LOCKMODE_EX)
         // Take the row lock on the row's key in store 1001 --
         // the same name a write of that row would wait on.
T2  sm2::rel_update(rel, id_key(7), 7, ...)
      -> sm1::update(s, 1001, be16(1)|be64(7), ...)
         // Blocks: a write waits for a conflicting lock on the
         // very key it writes.
```

This works only because both name `(storeid, relid ‖ row key)`
— the lock and the write meet on the data key. In a table with
a primary key the row key is its encoding, and a move of the
row erases that key, so it too waits for the lock
(`CLUSTER.md` §4).

**So row locks cannot be moved to a namespace of their own.**
Give them a synthetic name and a writer, which takes no entry,
would never see them: `FOR UPDATE` would silently stop
blocking writers. That is the constraint behind §1's split
between `ROW` and the other types.

### 4.4 A concurrent index build against a writer

The build must not add an entry for a row while a writer is
changing it. The entry key is the obvious thing to serialize
on, and it is the wrong one — the build locks the **row**
instead:

```cpp
// Hold the row shared to the end of the batch: a writer
// changes the row first, so it and the build never
// work on one row at once (sm3/SM3_DESIGN.md §3).
sm2::rel_lock(table->rel, r, SM_LOCKMODE_SH)
```

The build is also the only thing that writes index entries
without writing the row — which is exactly why §3's claim
that a non-unique entry needs no lock holds: every writer of
such an entry holds a lock or a version on its row.

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
T1  lock(k, X)  ->   converts, waits for T2
T2  lock(k, X)  ->   converts, waits for T1
```

sm1's detector picks a victim and fails it with
`SM_ERR_DEADLOCK`; the younger transaction loses. Under
`sm1::set_qa_nowait` the call returns `SM_ERR_BLOCKED` instead
of waiting at all.

### 4.7 `TRUNCATE` and why the lock name is the oid

`truncate_table` makes a new store, points the table's
record at it, and drops the old one — under the table-oid lock X.

A lock named by **storeid** would move under it: a stale
reader could hold `(old storeid, …)` while a DDL holds `(new
storeid, …)`, and both would believe they had the table's
table-oid lock. The oid is the only identity stable for the
table's whole life, so it is the name.

This is also why the lock cannot live *in* the table's store:
it has to outlive `DROP TABLE`'s `rmstore` —
`sm3/CATALOG_INMEM.md` §2 promises that a table dropped while
a caller waited is `SM_ERR_NOTFOUND`, which means getting the
lock and *then* finding no definition — and it has to pre-date
the store, since `CREATE TABLE` inserts the name before making
the store.

### 4.8 Publishing under the lock

Releasing the table-oid lock is not enough. sm1's commit releases
it, and a waiter could then read the shared catalog before the
DDL's new definition was published into it.

So `commit` holds the catalog's mutex **exclusive across both**
sm1's commit and the publish. A transaction that then gets the
table-oid lock waits on that mutex, and so sees the new
definition, never the old.

---

## 5. Traps worth writing down

- **A synthetic lock name costs an entry.** Any name sitting
  where no version exists has to be a real lock-table entry.
  Moving dedup off the key it writes — hashing the value, say
  — would put one entry per inserted row back into every
  transaction, which is the cost `sm1/WRITELOCK.md` removed. The
  hash is not the problem; leaving the written key is.
- **Hashing a lock *name* is safe; hashing a *key* is not.** A
  name collision costs a false wait, and sometimes a false
  deadlock abort, because the exact operation still decides.
  A key collision would refuse a value that is free.
- **Deadlock is reported, not avoided.** There is no lock
  ordering rule in the stack; `SM_ERR_DEADLOCK` is retryable
  and the caller retries.
- **Subjects 5 to 7 are not built.** `DROP COLLATION` in use
  is `sm3/CATALOG_INMEM.md` §4's open item, `DROP INDEX` leans
  on the table's oid lock, and sm3 has no sequence object.
