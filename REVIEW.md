# Architectural Review: wisr Storage Manager

Review of [DESIGN.md][des] against the requirements and
decisions in [OBJECTIVE.md][obj].

Reviewer: Andy Pavlo (CMU Database Group)

---

## 1. Executive Summary

Building a high-performance, multi-version concurrency
control (MVCC) storage engine on top of a key-value LSM
engine like RocksDB is one of the hardest engineering
problems in database systems. RocksDB is fundamentally
a single-version, point-in-time KV store. Simulating
multi-versioning, transaction snapshots, savepoints,
two-phase commit (2PC), and Serializable Snapshot
Isolation (SSI) without killing throughput requires deep
discipline.

Overall, [DESIGN.md][des] is an exceptionally thorough
and mathematically sound design document. It follows
every settled decision in [OBJECTIVE.md §Decisions][obj-dec]
with precision. The version key encoding, the atomic
commit sequencer, the status resolution rules, and the SSI
conflict-tracking mechanics demonstrate a sophisticated
grasp of transaction processing literature (Cahill,
Fekete, Ports, Grittner, Adya).

However, there are several critical architectural
hazards, RocksDB-specific pitfalls, and subtle
transaction anomalies that must be addressed before
writing production C++ code:

1. **The Compaction Truncation Dilemma:** Coupling
   `CF:txn` truncation to full database compaction
   risks silent data loss on cold SST files under Rule 4.
2. **Prefix Bloom Filter Scan Failure:** Using prefix
   extractors without setting `total_order_seek = true`
   on range scans silently truncates or fails scans.
3. **Range Tombstone Explosion:** Purging single-key
   tombstones via `DeleteRange` causes severe read
   degradation in RocksDB.
4. **Savepoint Rollback Visibility Anomaly:** Cursors
   opened before `sm_rollback` can return rows that the
   transaction itself rolled back.
5. **Non-Standard Read Committed Writes:** Checking
   writes against transaction-begin snapshots causes
   unexpected [`SM_ERR_CONFLICT`][err-conflict] aborts
   under Read Committed.
6. **Column Family Scalability Ceiling:** A dedicated
   Column Family per relation imposes hard scaling
   limits in RocksDB.

---

## 2. Core Architectural Wins

The following design decisions in [DESIGN.md][des] are
exemplary:

### 2.1 Custom Version Comparator (`wisr.version.v1`)
In [DESIGN.md §3.1][des-31], the specification rightly
rejects RocksDB's default bytewise comparator. Appending
version tails under standard bytewise ordering causes
prefix keys with high version suffixes to sort after
longer userkeys (e.g. `"a"` with a version tail sorting
after `"ab"`). Creating `wisr.version.v1` to compare
userkeys with `memcmp` separately from the 12-byte tail
is mandatory for scan correctness.

### 2.2 Complemented Suffixes (`~xid`, `~seq`)
Encoding suffixes as big-endian bitwise complements
([DESIGN.md §3.1][des-31]) inverts RocksDB's ascending
order, making newer versions sort first. A point read
seeking to `userkey | ~snap | 00000000` lands directly on
the first version visible to the snapshot without reverse
iteration ([DESIGN.md §10.1][des-101]).

### 2.3 Single Monotonic Counter & Publication Sequencer
The commit sequencer in [DESIGN.md §4.3][des-43] avoids
PostgreSQL-style per-snapshot active transaction lists.
Because `visible_ts` tracks the contiguous prefix of
published commits, snapshots are simply 64-bit scalar
integers, making visibility tests $O(1)$ integer
comparisons ([DESIGN.md §4.4][des-44]).

### 2.4 Restored Transaction Rule in SSI Crash Recovery
In [DESIGN.md §8.4][des-84], the design avoids logging
unbounded SIREAD locks for prepared transactions. Tagging
restored prepared transactions and dooming any concurrent
serializable reader that skips their versions completely
closes post-restart write-skew anomalies with minimal
recovery overhead.

### 2.5 Eager Uncommitted Writes with $O(1)$ Abort
Streaming uncommitted versions directly into RocksDB
memtables ([DESIGN.md §5][des-5]) ensures transactions
are unbounded by RAM. Because absence of a status record
equals an abort ([DESIGN.md §3.4][des-34]), `sm_abort`
is $O(1)$, and crash recovery requires zero undo passes.

---

## 3. Critical Correctness & Performance Risks

### 3.1 The "Full Compaction to Truncate CF:txn" Dilemma (§8.3 & §3.4)

#### The Problem
In [DESIGN.md §3.4][des-34], Status Resolution Rule 4
states:
> *"None of the above → the writer never committed. Dead."*

To reclaim space in `CF:txn`, [DESIGN.md §8.3][des-83]
specifies that `COMMITTED` records can only be dropped
once every surviving version has been stamped with
`commit_ts` by the compaction filter. To achieve this,
the design prescribes:
> *"Record $W_0 = 	ext{gc\_watermark}$, then run a full
> compaction pass over every relation CF, flushing
> memtables first... drop the `COMMITTED` records with
> $	ext{commit\_ts} \le W_0$."*

#### Why This Breaks in Production
1. **Excessive Write Amplification:** A full compaction
   pass calls `CompactRange(..., nullptr, nullptr)` across
   all column families. On large databases (e.g. 500GB+),
   this forces a complete rewrite of levels L0 through L6,
   causing massive I/O storms and write stalls.
2. **Catastrophic Data Loss on Cold SSTs:** RocksDB
   compactions are localized. Cold SST files in lower
   levels may not undergo compaction for weeks. If
   `CF:txn` records are swept without a full compaction,
   versions in cold SST files remain unstamped. When read,
   Rule 4 will evaluate them as **DEAD**, silently losing
   committed user data!

#### Recommendation
Do not tie data visibility to periodic full compactions.
- Maintain a compact, dense in-memory commit log (CLOG /
  bitset) for status checks. At 100M transactions, a
  2-bit status map takes only 25 MB of RAM.
- Maintain a durable `min_uncompacted_xid` watermark. Any
  version with `xid < min_uncompacted_xid` not explicitly
  recorded in an aborted set is presumed committed.

---

### 3.2 RocksDB Prefix Bloom Filters Break Scans (§10.2 & §12.2)

#### The Problem
[DESIGN.md §12.2][des-122] configures a prefix extractor
that strips the 12-byte tail, using `userkey` as the
bloom filter key. In [DESIGN.md §10.2][des-102],
[`sm_scan`][sm-scan] constructs an iterator and calls
`Seek(skey | ...)`.

#### The RocksDB Trap
In RocksDB, when a column family has a prefix extractor
defined, iterators default to **prefix seek mode** unless
`ReadOptions::total_order_seek = true` is explicitly set:
- If `skey` does not exist in the database, RocksDB checks
  the bloom filter for prefix `skey`. Finding no match,
  `it.Valid()` immediately returns `false`!
- The scan terminates immediately, returning zero rows,
  even when thousands of keys greater than `skey` exist!
- When keys with prefix `skey` are exhausted, prefix
  iterators do not advance to the next userkey prefix.

#### Recommendation
Update [DESIGN.md §10.2][des-102] to mandate:
```cpp
rocksdb::ReadOptions ro;
ro.total_order_seek = true; // Mandatory for multi-key range scans!
```
Reserve prefix bloom seek mode exclusively for
[`sm_read`][sm-read].

---

### 3.3 Tombstone Purging via `DeleteRange` Overhead (§8.3)

#### The Problem
In [DESIGN.md §8.3][des-83], when the compaction filter
sees a tombstone with $	ext{commit\_ts} \le
	ext{gc\_watermark}$, it queues the key for background
deletion:
```cpp
db.DeleteRange(cf(relid), version_key, userkey | FF×12);
```
The document asserts that the filter cannot drop the
tombstone because:
> *"CompactionFilter::Context does not report [bottommost
> level] anyway"*

#### Why This Is Flawed
1. **Incorrect API Assertion:** RocksDB's
   `CompactionFilter::Context` struct has provided
   `bool is_bottommost_level` since version 5.x.
2. **Range Tombstone Saturation:** Issuing thousands of
   single-key `DeleteRange` calls inserts thousands of
   range tombstones into RocksDB. During reads, RocksDB's
   `RangeDelAggregator` must search all overlapping range
   tombstones, causing dramatic read amplification and CPU
   overhead.

#### Recommendation
Use `is_bottommost_level` inside the compaction filter.
When a tombstone is at the bottommost level and no older
versions exist in the compaction, drop it directly.
Confine `DeleteRange` to bulk relation drops
([`sm_rmrel`][sm-rmrel]).

---

### 3.4 Savepoint Rollback Visibility Anomaly (§6.5 & §10.2)

#### The Problem
In [DESIGN.md §10.2][des-102]:
> *"The cursor also captures `T.seq_hwm` at `sm_scan` and
> applies §4.4 with that value, not the live one. Its view
> of the transaction's own writes is then fixed like the
> rest: a later `sm_rollback` neither hides rows the cursor
> could already see..."*

#### The Semantic Bug
Consider this execution sequence:
1. Transaction begins, creates savepoint via
   [`sm_savepoint`][sm-savepoint].
2. Transaction inserts row `"k1"` (`seq = 2`).
3. Transaction opens scan cursor `C` via
   [`sm_scan`][sm-scan]. `C` captures RocksDB snapshot
   $R_1$ and `seq_hwm = 2`.
4. Transaction calls [`sm_rollback`][sm-rollback] to undo
   `"k1"`.
5. Transaction calls [`sm_scan_next(C)`][sm-scan-next].

Because $R_1$ was taken prior to rollback, `"k1"` is
still in $R_1$. Because $C$ checks against its captured
`seq_hwm = 2`, **cursor $C$ yields `"k1"` to the user,
returning a row that the transaction itself explicitly
rolled back!**

#### Recommendation
In line with SQL engines (such as PostgreSQL),
[`sm_rollback`][sm-rollback] must close or invalidate all
scan cursors opened after the savepoint mark.

---

### 3.5 Read Committed Write Conflicts (§6.2)

#### The Problem
In [DESIGN.md §6.2][des-62], statement snapshots are
refreshed on each read, but writes check against
`T.snapshot_ts` (taken at `sm_begin`). A write fails with
[`SM_ERR_CONFLICT`][err-conflict] if the key was modified
after `sm_begin`, even if the transaction just read the
row.

#### Why This Deviates from ANSI SQL
In standard Read Committed (Postgres, Oracle, MySQL):
- An `UPDATE` does not abort when encountering a
  concurrently committed row; it waits for the row lock,
  re-reads the latest committed version, and applies the
  update.
- In Wisr, updating a concurrently modified row aborts with
  [`SM_ERR_CONFLICT`][err-conflict]. Wisr's Read
  Committed is effectively **Snapshot Isolation for
  writes** combined with **Read Committed for reads**.

This design choice stems from the physical `~xid` key
layout: an earlier allocated `xid` cannot write a version
that sorts after a later committed `xid`. This behavioral
difference must be clearly highlighted in user
documentation.

---

### 3.6 Scalability Ceiling of Column Families (§3.1 & §11)

#### The Problem
[DESIGN.md §3.1][des-31] assigns one RocksDB Column
Family per relation (`CF:rel.<relid>`).
- RocksDB is tuned for tens to hundreds of Column
  Families. Scaling to thousands of relations creates
  excessive memtable fragmentation and metadata overhead.
- `CreateColumnFamily` and `DropColumnFamily` bypass the
  WAL and write synchronously to RocksDB's `MANIFEST`.
  This forces Wisr to implement an orphan CF recovery
  sweep ([DESIGN.md §9.1][des-91]) and persist
  `created`/`dropped` CF lists in 2PC prepare records.

#### Alternative
Partitioning keys within a shared data CF:
$$	ext{key} = 	ext{relid (8B)} \mid 	ext{userkey} \mid {\sim}	ext{xid} \mid {\sim}	ext{seq}$$
This makes relation creation/drop pure MVCC operations,
eliminates orphan CF sweeps, and scales to millions of
tables.

---

## 4. Implementation Details to Pin Down

Before implementation begins, specify the following
components:

1. **SIREAD Range Predicate Index
   ([DESIGN.md §7.1][des-71]):** Every write executes
   `ssi.on_write(T, relid, k)` ([DESIGN.md §5][des-5]).
   Checking key $k$ against active range predicates
   requires an interval tree or segment index per relation
   to prevent serialized table scans under a global lock.
2. **Buffer Lifetime for `iterate_upper_bound`
   ([DESIGN.md §10.2][des-102]):** RocksDB `Slice` does
   not copy underlying memory. The struct
   [`sm_scan_t`][sm-scan-t] must own the backing buffer
   for `iterate_upper_bound` for the entire lifecycle of
   the iterator.
3. **Deadlock Victim Notification
   ([DESIGN.md §5.1][des-51]):** If the deadlock detector
   chooses an unblocked active transaction as victim, the
   transaction requires an atomic `aborted_reason` field
   checked on all subsequent `sm_*` calls.
4. **Write-Set Memory Bounds
   ([DESIGN.md §5.1 & §6.5][des-51]):** While row values
   stream to disk, the in-memory `undo` list and row locks
   scale linearly with write count. Transactions touching
   millions of keys will consume gigabytes of RAM.

---

## 5. Alignment Scorecard

| Mandate / Feature | Status | Evaluation |
| :--- | :---: | :--- |
| **Primary Key Clustered Index** | **PASS** | Meets [OBJECTIVE.md §Scope][obj-scope]; `wisr.version.v1` handles keys cleanly. |
| **MVCC on RocksDB** | **PASS** | Monotonic timestamps and complemented keys satisfy [OBJECTIVE.md §Goals][obj-goals]. |
| **Read Committed Level** | **WARN** | Functional, but first-committer-wins at `sm_begin` causes aborts. |
| **Repeatable Read Level** | **PASS** | Snapshot isolation with first-committer-wins operates as intended. |
| **Serializable Level (SSI)** | **PASS** | Full Cahill/Ports SSI implementation without read locks. |
| **Savepoint & Rollback** | **WARN** | Open cursors must be invalidated across savepoint rollback. |
| **Two-Phase Commit (2PC)** | **PASS** | Durable `PREPARED` records and post-restart SSI rule meet [OBJECTIVE.md §Decisions][obj-dec]. |
| **Crash Recovery** | **WARN** | Status Rule 4 risks data loss without a persistent watermark or CLOG. |
| **Deployment Model** | **PASS** | Single-process multi-threading with pidlocks matches [OBJECTIVE.md §Decisions][obj-dec]. |

---

## 6. Conclusion

[DESIGN.md][des] is a first-class, publishable systems
architecture. Its mathematical formulations for version
ordering, timestamp allocation, and optimistic dependency
tracking are sound. Resolving the
**compaction/status-resolution coupling**, mandating
**`total_order_seek` on scans**, and invalidating **cursors
across savepoint rollbacks** will ensure the wisr storage
manager is production-grade, durable, and correct.

<!-- Reference Links -->
[des]: file:///home/cktan/p/wisr/DESIGN.md
[des-31]: file:///home/cktan/p/wisr/DESIGN.md#L128-L149
[des-34]: file:///home/cktan/p/wisr/DESIGN.md#L195-L210
[des-43]: file:///home/cktan/p/wisr/DESIGN.md#L277-L325
[des-44]: file:///home/cktan/p/wisr/DESIGN.md#L326-L335
[des-5]: file:///home/cktan/p/wisr/DESIGN.md#L346-L362
[des-51]: file:///home/cktan/p/wisr/DESIGN.md#L376-L396
[des-62]: file:///home/cktan/p/wisr/DESIGN.md#L431-L454
[des-71]: file:///home/cktan/p/wisr/DESIGN.md#L541-L552
[des-83]: file:///home/cktan/p/wisr/DESIGN.md#L711-L813
[des-84]: file:///home/cktan/p/wisr/DESIGN.md#L883-L931
[des-91]: file:///home/cktan/p/wisr/DESIGN.md#L974-L1010
[des-101]: file:///home/cktan/p/wisr/DESIGN.md#L1048-L1067
[des-102]: file:///home/cktan/p/wisr/DESIGN.md#L1103-L1180
[des-122]: file:///home/cktan/p/wisr/DESIGN.md#L1379-L1383
[obj]: file:///home/cktan/p/wisr/OBJECTIVE.md
[obj-scope]: file:///home/cktan/p/wisr/OBJECTIVE.md#L6-L19
[obj-goals]: file:///home/cktan/p/wisr/OBJECTIVE.md#L20-L25
[obj-dec]: file:///home/cktan/p/wisr/OBJECTIVE.md#L44-L69
[err-conflict]: file:///home/cktan/p/wisr/API.md#L45
[sm-read]: file:///home/cktan/p/wisr/API.md#L618-L632
[sm-scan]: file:///home/cktan/p/wisr/API.md#L633-L670
[sm-scan-next]: file:///home/cktan/p/wisr/API.md#L671-L685
[sm-savepoint]: file:///home/cktan/p/wisr/API.md#L415-L425
[sm-rollback]: file:///home/cktan/p/wisr/API.md#L426-L446
[sm-rmrel]: file:///home/cktan/p/wisr/API.md#L551-L564
[sm-scan-t]: file:///home/cktan/p/wisr/API.md#L165
