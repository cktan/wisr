# SEQUENCE: Sequence

A Sequence is a named counter. Its definition is transactional
and its values are not. It has two levels: sm1's counter, a name
with a persistent position, and sm3's object, a counter with a
Namespace, a name among the relations, and an owner. sm1's is the
way any level gets unique ids, and sm3's is SQL's
`CREATE SEQUENCE`. The code is in `sm1/src/seq.cc`,
`sm1/src/db.cc` (`db_next_seq`, `db_last_seq`, `db_forget_seq`,
`keep_seq_positions`), `sm3/src/catalog/ddl.cc`
(`cat_put_sequence` and the calls beside it) and
`sm3/src/ddl/ddl.cc` (`create_sequence`, `drop_sequence`,
`own_sequence`, `nextval`). sm1's store catalog is in `DBMGR.md`.

---

## 1. Design

### 1.1 A worked example

```sql
CREATE SEQUENCE s START 100 INCREMENT 5;
SELECT nextval('s');   -- 100
BEGIN;
SELECT nextval('s');   -- 105
ROLLBACK;
SELECT nextval('s');   -- 110, not 105
```

1. `create_sequence` takes an oid, say 16400.
2. `cat_put_sequence` takes its Namespace's lock S, then inserts
   a `NAME` key and a `SEQUENCE` key in the Catalog.
3. `sm1::mkseq` makes the counter `sm3.seq.16400`: an `"S"` row
   in store 0 with a new seqid, in the same transaction.
4. `nextval` calls `sm1::nextseq`. The first draw persists a
   lease covering the next block of values, with a synced write,
   then hands out 100.
5. The draws that follow take a value from memory, under a mutex.
6. The `ROLLBACK` undoes nothing of the counter, as the position
   is outside MVCC. So 105 is spent.

Had the transaction made the Sequence, the `ROLLBACK` would have
taken the `"S"` row and the Catalog keys with it.

### 1.2 Decisions

- **The definition is transactional.** A Sequence is a catalog
  row, so `sm1::mkseq` and `sm1::rmseq` commit and roll back like
  `sm1::mkstore` (`DBMGR.md`), down to the wait that makes two
  concurrent creates of one name conflict.
- **The values are not transactional**, like a PostgreSQL
  sequence. `sm1::nextseq` needs no transaction, takes no lock,
  never waits on another transaction, and an abort gives nothing
  back.
- **The position is outside MVCC.** It lives in `CF:meta`, under
  a lease. An MVCC row would be wrong twice over: its writer
  would lock the Sequence for the rest of its transaction, and a
  rollback would hand the same values out again.
- **Values are unique and strictly increasing, and not dense.**
  Besides an abort, a crash skips the rest of a block reserved in
  advance.
- **A seqid, not the name, keys the position.** A seqid comes
  from the object-id lease, the one storeids come from, so the
  two never collide and none is reused. A name created again
  after a drop names a new Sequence, which starts at its own
  `start` and does not resume the old lease.
- **A clean close loses nothing.** Closing a database writes each
  Sequence's next value as its lease. Only a crash skips values.
- **A Sequence never wraps.** The last value is the largest one
  at or below `UINT64_MAX`. A further call fails with
  `SM_ERR_INVALID`, and does not wrap onto values already handed
  out.
- **An increment of 0 is refused.** A Sequence that never
  advances is a mistake, not a feature.
- **A Sequence only goes forward.** It is a name and a counter:
  there is no `setval`, no `ALTER SEQUENCE`, and no minimum,
  maximum, cycle or cache.
- **sm3's Sequence is a relation by name.** Its name is a `NAME`
  key in its Namespace, shared with the Namespace's tables and
  indexes, as a relation's is in PostgreSQL.
- **sm3 finds a Sequence as it finds a table**: the transaction's
  own creates and drops, then every committed one, from memory,
  not as of the snapshot, as PostgreSQL finds a name in its
  latest catalog. A draw reads the same way, at the newest
  committed state and the transaction's own writes: a Sequence is
  not transactional, and a snapshot could show one whose state is
  gone. Its record is read from the store in the transaction.
- **A Sequence has no lock of its own.** Its record is a catalog
  key, written under that key's X lock. Its position is ordered
  by a mutex (`LOCKMGR.md`).

---

## 2. Implementation: sm1's counter

### 2.1 The definition

A Sequence's definition is a row in store 0, written through the
ordinary write path:

```
key = "S" | name   →   { seqid, start, increment }   (three be64s, 24 bytes)
```

A row of another size is `SM_ERR_CORRUPT`. A name is 1 to
`sm1::max_seqname` (63) bytes, any byte but NUL.

- **`sm1::mkseq`** needs an active transaction. It looks the name
  up at the transaction's view: a live name is `SM_ERR_EXISTS`.
  It allocates a seqid and inserts the row. A concurrent insert
  of the name is reported as `SM_ERR_EXISTS` too.
- **`sm1::rmseq`** looks the name up the same way: none is
  `SM_ERR_NOTFOUND`. It deletes the row and remembers the seqid
  in `dropped_seqs`, which a rollback to a savepoint cuts back.
- **A commit that writes only `"S"` or `"U"` rows leaves
  `last_ddl_ts` alone**: no store lookup reads them (`DBMGR.md`).
- **The startup sweep and `sm1::lsstore` pass over `"S"` rows.**

### 2.2 The position

A Sequence's position has two homes.

- **`CF:meta`** holds its lease, under `"seq." | be64(seqid)`: a
  be64, the first value **not** covered by the lease. A Sequence
  with no lease row has handed out nothing, and starts at its
  `start`.
- **`db_t::seqs`** holds a `seq_state_t` for each Sequence in use
  in this process: its increment, the next value, the limit, and
  whether it is exhausted. The first draw builds it, and it stays
  until the database closes.

It is the scheme of the timestamp lease, with the Sequence's
increment as its step. The block is `ts_lease` values, 1,000,000
by default. `alloc_mu` guards every lease.

**Caution:** `alloc_mu` is also the commit sequencer's mutex. A
renewal holds it across a synced write, so commits wait while a
block is renewed. It happens once a block, and at the first draw
of each Sequence after an open.

### 2.3 A draw

`sm1::nextseq(session, name, out)`:

1. Check the name. Refuse a session holding a prepared
   transaction (`SM_ERR_PREPARED`), and a transaction a rollback
   doomed (`may_draw`).
2. If the name is the session's temporary Sequence, draw from it
   (§2.7).
3. Look the row up at the transaction's view, or at `visible_ts`
   with no transaction. None is `SM_ERR_NOTFOUND`.
4. Call `db_next_seq`.

The view means a Sequence the transaction made is usable before
it commits. At a statement's mark, a Sequence the statement
itself made is found only after `sm1::stmt_advance`, as any row
is (`XCTMGR.md`). Under SERIALIZABLE the lookup is a read, and
takes a SIREAD entry on the `"S"` row. It never waits.

`db_next_seq(db, seqid, start, increment,
out)`, under `alloc_mu`:

1. On first use in this process, set `next` and `limit` to the
   lease if there is one, else to `start`. Resuming at the lease
   skips the rest of that block. Only a confirmed missing
   lease starts anew. A malformed lease or a read error fails
   the call without changing the lease or memory state.
2. Refuse an exhausted Sequence with `SM_ERR_INVALID`.
3. If `next` has reached `limit`, extend by a whole block,
   stopping at the largest value. If no whole step fits, the new
   limit is `UINT64_MAX`.
4. Persist the new limit with a synced write, before any value
   under it is handed out. A failure is `SM_ERR_IO` or
   `SM_ERR_DISKFULL`, and nothing is handed out.
5. Hand out `next`.
6. Mark the Sequence exhausted if another step would wrap; else
   add the increment.

Distinct sessions may draw at once, and never get one value
twice.

### 2.4 A draw with no lookup

The lookup in §2.3 is a catalog read each call.
`sm1::seqref(session, name, ref)` makes it once, into a
`seqref_t`. The `sm1::nextseq` that takes a ref then draws with
no lookup:

```cpp
struct seqref_t {
    std::string name;   /* a temporary sequence's; else "" */
    uint64_t seqid = 0; /* 0 for a temporary sequence */
    uint64_t start = 0;
    uint64_t increment = 0;
};
```

`seqid` 0 means temporary, as no object id is below 1000. A ref
is good while the Sequence is not dropped, which its holder sees
to. sm2 draws a table's `_id`s so, the ref taken at the rel's
first draw (`next_id`), and held safe by the table's lock. A
tree's page ids are the same (`next_page`).

### 2.5 The last value

`sm1::lastseq(session, name, out)` reports the last value handed
out and draws none: it extends no lease and writes nothing. It
returns 1 if the Sequence never handed one out. It needs no
transaction, and reads the definition as `sm1::nextseq` does.

- In use in this process, it is exact.
- Not in use, it is the lease less the increment. After a clean
  close that is exact. After a crash, until the next draw, it is
  an upper bound, since the crash skips the rest of the block.

sm3 reads it for N, the last `_id` of a table, in its size
estimate.

### 2.6 Close and drop

- **A clean close** (`keep_seq_positions`, in `db_t`'s
  destructor) writes each Sequence's `next` as its lease, in one
  synced batch, however many there are. A Sequence exhausted, or
  at the end of its block, or with none handed out, is left as it
  is. A failure is logged, and the next open skips the rest of
  each block, as after a crash. A database stays open for
  `idle_close_ms` after its last session closes, so sessions that
  come and go do not close and reopen it.
- **A committed `sm1::rmseq`** calls `db_forget_seq`, which
  erases the state and deletes the lease, best effort. Nothing
  reads the lease once the row is gone. A lease left by a crash,
  by an abort after a draw, or by a prepared transaction's drop
  is garbage and no hazard: the name's next Sequence has a new
  seqid.

### 2.7 Temporary Sequences

With `temporary`, `sm1::mkseq` makes an entry in the session's
memory (`temp_seqs`): no row in store 0, no lease in `CF:meta`,
nothing written (`TEMPTABLE.md`).

- No other session knows its name, and it goes when the session
  closes.
- To its session, the name means the temporary Sequence, whatever
  else has it.
- Making and dropping one follow the transaction:
  `temp_seqs_made` and `temp_seqs_dropped` are cut back by a
  rollback. Its values do not.
- A drop hides it from the transaction, and it goes at commit.
  Until then it holds its name.
- A transaction declared read-only may make and drop one. One
  that has cannot `sm1::prepare`.
- Its draw has the same end as §2.3, with no lease.

A temporary table counts its `_id`s with one, and its TOAST ids.

---

## 3. Implementation: sm3's object

### 3.1 The record

A Sequence is a `SEQUENCE` key by oid, with one `sequence` part:
namespace, name, start, increment, owner table and owner column
(`CATALOG.md`). Its counter is the sm1 Sequence `sm3.seq.<oid>`
(`sequence_counter`), made and dropped with the key in the same
transaction.

Memory keeps only its Namespace and name: `sequence_by_oid` and
`sequence_names`, filled at `cat_open` from the `NAME` keys, and
changed at publish. They serve a Namespace's drop, and a lookup
either way. The overlay holds the transaction's creates and drops
(`sequence_change_t`). A change of owner is not in the overlay:
the record is in the store, and rolls back there.

### 3.2 Create, drop and find

- **`create_sequence(session, nspoid, name, start, increment,
  oid)`** refuses an empty name and an increment of 0
  (`SM_ERR_INVALID`), takes an oid, and calls `cat_put_sequence`.
- **`cat_put_sequence`** takes the Namespace's lock S, so a drop
  of the Namespace waits this transaction out. It then inserts
  the `NAME` key, the `SEQUENCE` key and the counter. The name
  goes first, so a taken one, a relation's among them, fails with
  `SM_ERR_DUPKEY` before anything is made. No such Namespace is
  `SM_ERR_NOTFOUND`.
- **`drop_sequence(session, oid)`** reads the record under an X
  lock on its key. A Sequence with an owner is `SM_ERR_INUSE`: it
  goes with its table or column. Else it calls
  `cat_del_sequence`.
- **`cat_del_sequence`** reads the record under that lock, erases
  the key and the name, and drops the counter. None is
  `SM_ERR_NOTFOUND`.
- **`cat_get_sequence(ss, nspoid, name, oid)`** and
  **`cat_get_sequence_name(ss, oid, nspoid, name)`** find a
  Sequence from memory: the transaction's own creates and drops,
  the newest first, then the shared maps. They give 0, or 1 if
  none, an oid of another kind among them.
- **`cat_list_sequences(ss, nspoid, names)`** lists a Namespace's
  the same way: the committed names, then the transaction's
  creates and drops in order. So it never names one they do not
  find.
- **`cat_get_sequence_def(ss, oid, def)`** reads the record from
  the store, so it needs a transaction open. It gives 0, or 1 if
  none at the snapshot.

### 3.3 `nextval`

`nextval(session, oid, out)` is `sm1::nextseq` on
`sm3.seq.<oid>`. It takes no lock, and it is not transactional. A
Sequence not there is `SM_ERR_NOTFOUND`, and a draw past
`UINT64_MAX` is `SM_ERR_INVALID`. It looks the name up at each
call: it holds no ref, as nothing holds a Sequence against a
drop.

### 3.4 Ownership

A Sequence may be owned by a column, SQL's `OWNED BY`, of a table
in its Namespace. Its owner fields name the table and the
column's place. They are null otherwise, as in a Sequence written
before them. Dropping the table or the column drops it, and
dropping it alone is `SM_ERR_INUSE`.

`own_sequence(session, oid, table, attnum)`, with table 0 for
none:

1. Take the table's lock IS, to the end of the transaction, so
   the table outlives this. The table goes first, as a drop of it
   takes it before the Sequence.
2. Read the record under an X lock on its key.
3. If it has an owner, read that table's record, and refuse an
   identity column's Sequence with `SM_ERR_INUSE`.
4. With table 0, clear the owner and put the record.
5. Refuse a table in another Namespace, or a column not live,
   with `SM_ERR_INVALID`.
6. Set the owner and put the record.

**An identity column names its Sequence** in its identity fields,
and owns it for good. The column names it so the host finds it
with no read of the Namespace's Sequences.

- `create_table` reads each identity column's Sequence under its
  lock. It must be in the table's Namespace and owned by none.
  Once the table is put, the column owns it.
- `set_identity` does the same for a live `NOT NULL` integer
  column with no default.
- An identity column is a `NOT NULL` `smallint`, `integer` or
  `bigint` with a Sequence and no default, insert or update.
- `add_column` of an identity column, a temporary table's
  identity column, and a type change of one are `SM_ERR_INVALID`.

`drop_owned_sequences(session, oid, nspoid,
attnum)` serves `drop_table` and `drop_column`, with attnum -1
for any column. A Sequence is owned only in its table's
Namespace, so only that one is looked in:

1. List the Namespace's Sequences from memory.
2. Read each record with no lock, and pass over those the table
   does not own.
3. Read each one left again under its key's lock. The drop is of
   the newest record.
4. Drop it if the table still owns it.

The list is from memory because a scan of the store in a
statement misses the statement's own drops. So two tables dropped
in one statement do not trip on each other's Sequences.

---

## 4. The Sequences sm3 makes for itself

Names beginning `sm3.` are sm3's own. `sm_mkseq`, `sm_rmseq` and
`sm_nextseq` refuse them with `SM_ERR_INVALID`.

| sm1 name | Made by | Start, increment | Drawn by | Dropped by |
| --- | --- | --- | --- | --- |
| `sm3.oid` | `cat_init` | 16384, 1 | `cat_new_oid`, by name | nothing |
| `sm3.tree_page` | `cat_init` | 1, 1 | sm2's `next_page`, by a ref | nothing |
| `sm3.id.<oid>` | `create_table`, temporary with the table | 1, 1 | sm2's `next_id`, by a ref | `drop_table` |
| `sm3.toast.<oid>` | `create_table`, temporary with the table | 1, 1 | sm2, by name, once the host reads TOAST pointers | `drop_table` |
| `sm3.seq.<oid>` | `cat_put_sequence` | the user's | `nextval` | `cat_del_sequence` |

- `_id` starts at 1, so `_id = 0` names no row.
- A failed `create_table` drops its two Sequences again, unless a
  retryable error already aborted the transaction.
- A `TRUNCATE` or a rewrite keeps a table's Sequences, so new
  rows go on from the last `_id`.
- `sm_mkseq`, `sm_rmseq` and `sm_nextseq` give the host sm1's
  counter directly, with a name and no Namespace.

---

## 5. The calls

| sm1 | sm3 | `sm.h` |
| --- | --- | --- |
| `mkseq` | `create_sequence`, `cat_put_sequence` | `sm_mkseq`, `sm_cat_create_sequence` |
| `rmseq` | `drop_sequence`, `cat_del_sequence` | `sm_rmseq`, `sm_cat_drop_sequence` |
| `nextseq` | `nextval` | `sm_nextseq`, `sm_cat_nextval` |
| `seqref` | — | — |
| `lastseq` | — | — |
| — | `own_sequence` | `sm_cat_own_sequence` |
| — | `cat_get_sequence` | `sm_cat_sequence_oid` |
| — | `cat_get_sequence_name` | `sm_cat_sequence_name` |
| — | `cat_get_sequence_def` | `sm_cat_sequence_def` |
| — | `cat_list_sequences` | `sm_cat_list_sequences` |

`sm1/API.md` lists each sm1 call's errors.

digres narrows what SQL may ask
(`../dg/src/backend/access/sm/smstore.c`). A Sequence counts up
from 1 or more, by 1 or more. `AS smallint` and `AS integer` are
taken, and sm keeps no bound: a value past the type's range fails
where it is stored. A draw past `PG_INT64_MAX` fails with
`ERRCODE_SEQUENCE_GENERATOR_LIMIT_EXCEEDED`. `currval` is kept by
the host.

---

## 6. Not built

- **`setval`, `ALTER SEQUENCE`** and the options: `MINVALUE`,
  `MAXVALUE`, `CACHE`, `CYCLE`. A descending Sequence.
- **`TRUNCATE ... RESTART IDENTITY`** and `SELECT` from a
  Sequence.
- **`CREATE TEMPORARY SEQUENCE`.** sm1 has temporary Sequences,
  and sm3 uses them for a temporary table's own counters. sm3 has
  no temporary Sequence object.

---

## 7. Cases

| Case | What happens |
| --- | --- |
| Abort or savepoint rollback after a draw | the value is spent; the next draw goes on past it |
| Abort of a `mkseq` | the row goes; a lease its draws wrote is garbage |
| Rollback past an `rmseq` | the row is back, and `dropped_seqs` is cut back, so the lease stays |
| Crash | the Sequence resumes at its lease, above every value handed out; the rest of the block is skipped |
| Clean close | the next open resumes at the exact next value |
| Two-phase commit | a draw inside is spent whatever the outcome; a prepared session cannot draw; a prepared drop leaves its lease behind |
| Two sessions drawing | `alloc_mu` orders them; no value is given twice |
| Two sessions making one sm1 name | the second waits, then fails with `SM_ERR_EXISTS` or sm1's `SM_ERR_CONFLICT` |
| A name dropped and made again | a new seqid, from its own `start` |
| A statement that made the Sequence | it draws only after `sm1::stmt_advance` |
| READ ONLY transaction | `sm1::mkseq` and `sm1::rmseq` are refused, a temporary one aside; `sm1::nextseq` is not |
| SERIALIZABLE | a lookup by name takes a SIREAD entry on the `"S"` row; a draw by a ref takes none; neither waits |
| Old REPEATABLE READ snapshot, a Sequence committed after it | `cat_get_sequence` finds it from memory, and `nextval` draws from it, as a draw reads the newest state; `cat_get_sequence_def` gives 1, as it reads at the snapshot |
| Old REPEATABLE READ snapshot, a Sequence dropped after it | `cat_get_sequence` does not find it, and a `nextval` by a held oid is `SM_ERR_NOTFOUND`, as a draw reads the newest state |
| Old snapshot at `drop_table`, a Sequence committed in the Namespace after it | `drop_owned_sequences` lists it from memory, finds no record at the snapshot, and passes over it: the table cannot own it |
| `own_sequence` committed after `drop_table`'s snapshot | the drop waits on the table lock, then its first read, at the snapshot, sees no owner; the Sequence stays, naming a table that is gone |
| Sequence dropped with a ref held | the ref's draws go on, with new state; a holder must keep the Sequence from being dropped |
| Last value handed out | the next draw fails with `SM_ERR_INVALID`, and every one after |
| Exhausted, then a reopen | the lease on disk is `UINT64_MAX`, the last block's, so the state is built exhausted: the draw fails, and the last value is not handed out twice |
| A Namespace's drop | a Sequence in it is `SM_ERR_INUSE` |
| Temporary table | its counters are in the session, and go with it |

---

## 8. Rejected

- **The position in an MVCC row.** Its writer would lock the
  Sequence for the rest of its transaction, and a rollback would
  hand the values out again.
- **The lease keyed by name.** A Sequence created again under an
  old name would resume the old lease.
- **Wrapping at `UINT64_MAX`.** It would hand out values already
  handed out.
- **Listing a Namespace's Sequences by a scan of the store.** A
  scan in a statement misses the statement's own drops, so it
  would name Sequences that a lookup does not find.
- **A lock on a Sequence as an object.** Its record is a catalog
  key, and its Namespace's lock holds it against a drop of the
  Namespace.

---

## 9. Tests

- `sm1/test/test_seq.cc`: a Sequence's start and increment, a
  draw by a ref, `mkseq` and `rmseq` following commit, abort and
  rollback while the values do not, a lease deleted at a drop,
  the last value, one name in two databases, survival of a reopen
  across many leases, concurrent sessions, a kill, and `lastseq`.
- `sm1/test/test_temp.cc`: a temporary Sequence counts in the
  session and writes nothing, and its making and dropping follow
  the transaction.
- `sm1/test/test_savepoint.cc`: a doomed transaction cannot draw.
- `sm3/test/test_ddl.cc`: a Sequence made in a Namespace, found
  by name and counting; read back by oid; holding its Namespace;
  found as committed and not as of a snapshot; owned by a column;
  two owners dropped in one transaction; an identity column.
- `rel/test/test_seq.rel`: values across aborts, savepoints and
  2PC, and `_id`s across aborts.
- `rel/test/test_doomed.rel`: a draw refused in a doomed
  transaction.
- `../dg/test/server.sh`: Sequences through SQL.
