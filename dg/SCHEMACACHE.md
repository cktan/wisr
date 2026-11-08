# SCHEMACACHE — a session's schema, kept across transactions

**Status: built.** §7's six steps: the epoch, a name's relation
(a point read's name lookup from 3,000 instructions to 500), the
version check after a lock (900 to 100), statistics rows (about
4,700 to 1,300, with pg_statistic's descriptor made once a
session), size estimates (2,400 to 900), and sm3's table open
(3,400 to 1,800). A point read is at about 110,000 user
instructions to PostgreSQL's 107,000-108,500, from 122,400; its
latency and CPU time a query are now below PostgreSQL's.

A point read, one row by its key, `select c1 from wide_mixed
where id = :k` on the old bench's million rows
([`../bench.old/DESIGN.md`](../bench.old/DESIGN.md);
`../bench/q0-pkget.sql` is the same shape),
runs 16,000 more instructions in digres than in PostgreSQL, and
nearly all of them go to asking sm, in every transaction, what
the schema is: which table a name means, what its definition and
version are, how big it is, what its statistics say. PostgreSQL
answers the same questions from its system caches, which outlive
transactions. This file proposed that digres do the same: each
session keeps those answers, and knows they are still good from
one number sm gives it, the catalog's epoch.

---

## 0. In brief

- **sm gives an epoch** (`sm_cat_epoch`): a number that moves
  whenever a DDL, an ANALYZE or an index build is published to
  the session's view of the catalog, and reads `UINT64_MAX`
  while the session's own transaction has DDL of its own.
- **digres caches what it asks sm** per session: a name's
  relation, a table's statistics rows, its size estimate, and
  whether its relcache entry is current. Each entry carries the
  epoch it was made at, and is good while the epoch is the same.
- **Nothing else changes.** A table is still locked in sm in
  each transaction that reads it; the cache only spares the
  lookups around the lock.

## 1. Why

Instructions a query, in thousands, sampled every 100,000
instructions over 20,000 point reads, on
`../bench.old/run.sh`'s loaded data (release builds, 2026-10-25):

| | PostgreSQL | digres | gap |
| --- | --- | --- | --- |
| all | 106.3 | 122.4 | +16.1 |
| parse | 8.3 | 11.4 | +3.1 |
| analyze | 23.1 | 31.5 | +8.4 |
| plan | 31.7 | 39.3 | +7.6 |
| execute | 27.2 | 27.9 | +0.7 |
| commit | 4.0 | 4.2 | +0.2 |

The executor is level, and so is commit. Of the gap, these are
digres asking sm what PostgreSQL finds in a cache:

| What | gap | where |
| --- | --- | --- |
| a name's relation | 2.8 | `smstore_relation_oid`: `sm_cat_table_oid`, then `_index_oid`, then `_view_oid` |
| a table's version, checked after its lock | 0.9 | `cat_table_version`, `sm_tbl_version`, `RelationCheckSmVersion` |
| a table opened in sm | 3.4 | `sm3::tbl_open`: the lock, the definition, its rels |
| statistics, for costing | 3.0 | `btcostestimate` → `smstore_statistic_tuple`: `table_stats`, `relation_open`, the kept row |
| a table's and an index's size | 2.4 | `smstore_estimate_size`, `smstore_estimate_index_size`, under the 10 s cache ([`STATISTICS.md`](STATISTICS.md#22-a-tables-size-live-bytes-scaled-rows) §2.2) |
| whether a table has children | 1.2 | `has_subclass`; none can, and it now says so |

The rest is sq's fixed cost a parse (3.1) and the planner's own
work a little above PostgreSQL's.

## 2. What PostgreSQL does

PostgreSQL's syscache and relcache keep catalog rows and relation
descriptors for the life of a backend. A DDL's commit sends
invalidation messages; a backend processes them when it next
takes a lock or starts a command, and drops what they name. A
catalog lookup reads the newest committed catalog, not the
transaction's snapshot. `RangeVarGetRelidExtended` looks a name
up, locks the relation, and looks the name up again if any
invalidation arrived meanwhile.

digres already keeps that last loop: when a table's version has
moved under its lock, `invalidate_sm_entry()` (relcache.c) drops
the relcache entry and counts a message received, and
`RangeVarGetRelidExtended` looks the name up again
([`SHMEM.md`](SHMEM.md)
§5.1). What digres lacks is the caches themselves.

## 3. The epoch

```
uint64_t sm_cat_epoch(sm_session_t* session);
```

- **It moves with every publish the session can see.** sm3's
  catalog counts its publishes (`cat->seq`,
  [`../sm/CATALOG.md`](../sm/CATALOG.md#4-in-memory) §4); every
  commit of shared DDL,
  an ANALYZE's statistics and an index made valid are publishes.
  A session's temporary catalog changes without one
  (`publish_temp`), so the session counts those itself. The epoch
  is the sum of the two counts: each only grows, so the sum moves
  whenever either does.
- **`UINT64_MAX` while the session's transaction has DDL of its
  own** (sm3's `made_ddl`), as `table_version_own` is for one
  table. The transaction then sees its overlay, which no epoch
  dates, and every lookup goes to sm.
- **Per database.** A catalog is a database's; so is the epoch.
- **Read with no lock**: the count is an atomic.

**The rule for a cache**: read the epoch *before* the lookup and
tag the entry with it. A publish after the read moves the epoch,
so an entry made from data a publish was changing is never used
again. Use an entry only while the epoch read now equals its tag;
when it moves, drop every entry at once.

## 4. What is cached

All in the session (`tcb_t`, [`CODE.md`](CODE.md#3-globals) §3),
freed with it.

### 4.1 A name's relation

`smstore_relation_oid(name, nspoid)` → `(oid, kind)`, misses
included: an unqualified name is looked for in each namespace of
the search path, and most of those lookups miss. An index's entry
also remembers its table, as `remember_index()` does today.

### 4.2 A relcache entry's currency

`smstore_lock_relation()` takes the lock, then asks for the
table's version and compares it with the relcache entry's
(`RelationCheckSmVersion`). If the epoch is the one the entry was
checked at, nothing was published since, and the comparison is
skipped. The lock is still taken: it is what makes a DDL wait.

### 4.3 Statistics rows

`smstore_statistic_tuple(relid, attnum)` keeps its rows today,
good while sm hands out the same statistics; but it asks for the
statistics and opens the relation to know that. Keyed by the
epoch, the row is used with neither: an ANALYZE, and a change of
the column's type or collation, are publishes.

### 4.4 Size estimates

`smstore_estimate_size()` and `smstore_estimate_index_size()`
keep the bytes for 10 seconds while the table's version holds
([`STATISTICS.md`](STATISTICS.md#22-a-tables-size-live-bytes-scaled-rows)
§2.2). With the epoch, they keep the estimate
itself, pages and tuples, for the same 10 seconds while the
epoch holds, and skip opening the table, its statistics and its
version.

### 4.5 sm3's table open, trimmed

Not a cache in digres, but the same idea in sm3: what an open
learns from the definition's generation is learned once with it
(as its layouts are,
[`../sm/CATALOG.md`](../sm/CATALOG.md#4-in-memory) §4). Whether
the store is
chained is fixed when the store is made, so it is read once into
the definition instead of from sm1 at every open; whether this
transaction made the table comes from the lock's own lookup
instead of a second one. The name of the table's TOAST sequence
is made with the definition, whether its layout has a column of
variable width with the layout, and an index's state is read
without a copy of a failed one's error (a point read's parse
analysis from 31,000 instructions to 30,400).

### 4.6 An index's usability

`get_relation_info()` asks of each index whether the transaction
may scan it (`smstore_index_usable()`, sm's
`sm_tbl_index_usable`): built, valid, and valid since a count its
snapshot is not older than. An index found usable is kept with
the epoch, and is usable while the epoch holds: a valid index
stays valid until it is dropped, a publish, and a session's
snapshots only move forward, a transaction's at its start and a
READ COMMITTED statement's at its own. One found unusable is not
kept: its build may end, or a newer snapshot see it, with no
publish between. The epoch is a guard more than the proof: an
index's oid is never used again, and a dropped index leaves its
table's list of indexes, so a usable index is never asked about
once it is not; `server.sh` can break the unusable side of this
and not the epoch's.

## 5. The cases

| Case | What happens |
| --- | --- |
| DDL committed by another session | a publish: the epoch moves, every entry goes |
| ANALYZE | its statistics are published: the epoch moves |
| an index made valid by the builder | a publish: the epoch moves |
| DDL in this transaction | the epoch reads `UINT64_MAX`: no entry used or made until it commits or rolls back |
| this transaction rolls its DDL back, to a savepoint or whole | the overlay goes; the epoch reads the count again, unchanged, and entries made before the DDL are good, as nothing was published |
| a temporary table made, altered or dropped | `publish_temp` counts it: the epoch moves for this session alone |
| DROP, then CREATE under the same name | a publish: the name's entry goes |
| a name looked up, then the table renamed before the lock | the lock waits for the rename's X; once it is had, the relcache's version check finds the change, counts a message, and the name is looked up again with the epoch moved, so from sm (§2) |
| REPEATABLE READ, an old snapshot | a catalog lookup reads the newest catalog, in PostgreSQL and in sm alike; unchanged |
| REPEATABLE READ, a snapshot older than an index | the index is unusable to it, and not kept so: the next transaction asks sm again and may use it (§4.6) |
| a prepared transaction | sm refuses PREPARE after DDL; nothing to date |
| crash, restart | the caches are memory, made again |
| another database | its own catalog, its own epoch |
| a session in a long transaction while others commit DDL | each lookup reads the epoch, finds it moved, and asks sm, as today |

## 6. What it buys, and what it does not

Of the 16,100 instructions, the cache and the trimmed open should
take about 9,000: names 2,800, version checks 900, statistics
about 2,500, sizes about 2,000, and the open about 1,000; with
`has_subclass`, about 10,000. That leaves digres some 6,000
behind PostgreSQL, in sq's parse, the planner's own paths, and
the column aliases; each is its own step after this one.

The cost is one sm call a lookup, an atomic load, and a hash
probe; memory of a few entries a table a session.

## 7. Order of work

Each step is measured by a point read's instructions, before
and after, PostgreSQL's beside it.

1. **`sm_cat_epoch`** in sm: the count, the session's temporary
   count, and `UINT64_MAX` with DDL of the transaction's own.
   Tested in sm3 and through `rel`: it moves on each kind of
   publish of §5, on a temporary table's change for its session
   only, and reads `UINT64_MAX` until commit or rollback.
2. **Names** (§4.1).
3. **The version check** (§4.2).
4. **Statistics rows** (§4.3).
5. **Size estimates** (§4.4).
6. **sm3's open** (§4.5).

Each with `test/server.sh` checks in the manner of the size
cache's: a cached answer used, and dropped after another
session's DDL, an ANALYZE, a temporary table's change, and the
session's own DDL, each check proven by breaking it.

## 8. Not here

- **One cache for all sessions.** Sessions are threads and
  could share, but a shared cache needs a lock or a lock-free
  map, and the epoch would have to be the shared catalog's
  alone. Later, if a session's own cache is not enough.
- **Keeping a table open in sm across transactions.** The lock
  is a transaction's; so is the handle that carries it.
- **sq's fixed cost a parse.** A step of its own.
