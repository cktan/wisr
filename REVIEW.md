# Architectural Review: wisr Storage Manager

Review of [DESIGN.md][des] against the requirements and
decisions in [OBJECTIVE.md][obj], incorporating the new
PGDATA-style directory layout, environment lifecycle split,
and [`sm_strlist_t`][strlist-type] API.

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

The latest revisions to [DESIGN.md][des] and [API.md][api]
represent a major architectural maturation. Adopting a
PostgreSQL-style data directory layout
([DESIGN.md §3.5][des-35]), splitting environment
initialization ([`sm_init`][sm-init]) from runtime
attachment ([`sm_attach`][sm-attach] and
[`sm_detach`][sm-detach]), mapping databases through a root
RocksDB instance, and introducing [`sm_strlist_t`][strlist-type]
elevates the system from a prototype sketch to a robust,
industrial-strength database framework.

The transaction processing design continues to adhere
faithfully to [OBJECTIVE.md §Decisions][obj-dec]. The
mathematical foundation—complemented version tails, the
atomic publication sequencer, and optimistic SSI with
pivot detection—is first-rate.

However, while the storage and environment management
layers have taken a giant leap forward, several LSM
transactional issues remain open and should be settled
before implementing the storage engine internals. Items 1
through 4 need work; items 5 and 6 are documented
decisions, restated here so that they are chosen rather
than inherited:

1. **Full-Compaction Cost for `CF:txn` Truncation:**
   Coupling `CF:txn` truncation to a full compaction pass
   over every relation CF is correct, but it is expensive
   and it silently depends on two RocksDB behaviours that
   §8.3 never states.
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
   under Read Committed. Already documented for callers;
   a behavioural divergence, not a defect.
6. **Column Family Scalability Ceiling:** A dedicated
   Column Family per relation imposes hard scaling
   limits in RocksDB. An accepted trade-off; what is
   missing is a stated relation-count target.

---

## 2. Core Architectural Wins

### 2.1 PostgreSQL-Style Home Directory & Root DB (§3.5)
The new storage layout in [DESIGN.md §3.5][des-35] is a
huge win:
- `{homedir}/pidlock.pid`: Process exclusion (`postmaster.pid`).
- `{homedir}/global/root`: Root database (`global/pg_database`).
- `{homedir}/base/{dbid}`: Database directories (`base/{oid}`).

Decoupling caller `dbname` strings from filesystem paths
stops directory traversal vulnerabilities (`"../"`) dead in
their tracks, supports arbitrary 1..63 byte names matching
Postgres `NAMEDATALEN - 1`, and makes database creation and
deletion crash-consistent. Ordering operations such that
`sm_mkdb` commits the name last and `sm_rmdb` deletes the name
first guarantees that a crash leaves only unreferenced
directories, which [`sm_attach`][sm-attach] sweeps cleanly
at startup.

### 2.2 Clean Lifecycle Split (`init` vs `attach` / `detach`)
Separating one-time initialization ([`sm_init`][sm-init])
from per-process connection ([`sm_attach`][sm-attach] and
[`sm_detach`][sm-detach]) mirrors PostgreSQL's `initdb` vs
postmaster startup. It eliminates the fragile "create on
first use" behavior where a typo in a directory path quietly
created an empty database home. Adding
[`SM_ERR_VERSION`][err-version] ensures explicit format
validation.

### 2.3 Counted, Immutable Name Lists (`sm_strlist_t`)
The design of [`sm_strlist_t`][strlist-type] in
[PLAN.md][plan] and [`include/sm.h`][sm-h] is a model of C ABI
hygiene:
- Replaces unwieldy `char***` pointers with an opaque,
  counted handle.
- Single-allocation representation (`count` + pointers +
  contiguous string buffer) released with one
  [`sm_strlist_free`][strlist-free] call.
- Safe semantics: out-of-bounds access returns `NULL` rather
  than memory corruption, and empty lists return non-null
  handles with `count = 0`.
- Crucially, it holds no transaction or GC state, preventing
  resource pinning.

### 2.4 Custom Version Comparator (`wisr.version.v1`)
In [DESIGN.md §3.1][des-31], the specification rightly
rejects RocksDB's default bytewise comparator. Appending
version tails under standard bytewise ordering causes
prefix keys with high version suffixes to sort after
longer userkeys (e.g. `"a"` with a version tail sorting
after `"ab"`). Creating `wisr.version.v1` to compare
userkeys with `memcmp` separately from the 12-byte tail
is mandatory for scan correctness.

### 2.5 Complemented Suffixes (`~xid`, `~seq`)
Encoding suffixes as big-endian bitwise complements
([DESIGN.md §3.1][des-31]) inverts RocksDB's ascending
order, making newer versions sort first. A point read
seeking to `userkey | ~snap | 00000000` lands directly on
the first version visible to the snapshot without reverse
iteration ([DESIGN.md §10.1][des-101]).

### 2.6 Single Monotonic Counter & Publication Sequencer
The commit sequencer in [DESIGN.md §4.3][des-43] avoids
PostgreSQL-style per-snapshot active transaction lists.
Because `visible_ts` tracks the contiguous prefix of
published commits, snapshots are simply 64-bit scalar
integers, making visibility tests $O(1)$ integer
comparisons ([DESIGN.md §4.4][des-44]).

### 2.7 Restored Transaction Rule in SSI Crash Recovery
In [DESIGN.md §8.4][des-84], the design avoids logging
unbounded SIREAD locks for prepared transactions. Tagging
restored prepared transactions and dooming any concurrent
serializable reader that skips their versions completely
closes post-restart write-skew anomalies with minimal
recovery overhead.

### 2.8 Eager Uncommitted Writes with $O(1)$ Abort
Streaming uncommitted versions directly into RocksDB
memtables ([DESIGN.md §5][des-5]) ensures transactions
are unbounded by RAM. Because absence of a status record
equals an abort ([DESIGN.md §3.4][des-34]), `sm_abort`
is $O(1)$, and crash recovery requires zero undo passes.

---

## 3. Critical Correctness & Performance Risks

### 3.1 Full Compaction to Truncate `CF:txn` (§8.3 & §3.4)

#### The Problem
In [DESIGN.md §3.4][des-34], Status Resolution Rule 4
states:
> *"None of the above → the writer never committed. Dead."*

To reclaim space in `CF:txn`, [DESIGN.md §8.3][des-83]
specifies that `COMMITTED` records can only be dropped
once every surviving version has been stamped with
`commit_ts` by the compaction filter. To achieve this,
the design prescribes:
> *"Record $W_0 = \text{gc\_watermark}$, then run a full
> compaction pass over every relation CF, flushing
> memtables first... drop the `COMMITTED` records with
> $\text{commit\_ts} \le W_0$."*

#### The Protocol Itself Is Sound
An earlier draft of this review argued that cold SST
files escape the pass, leaving versions unstamped and
resolving as DEAD under Rule 4. That was a misreading and
is withdrawn. §8.3 does not sweep `CF:txn` on a timer: it
sweeps only after the full pass has completed and every
iterator older than the pass has been released, and it
uses the watermark from the pass's *start* precisely so
that a version kept unstamped cannot have its record
dropped. The failure described was a violation of the
protocol, not the protocol.

Two RocksDB behaviours are load-bearing for that
argument, and §8.3 assumes both without saying so:

- `CompactRangeOptions::bottommost_level_compaction`
  defaults to `kIfHaveCompactionFilter`, so the pass does
  rewrite bottommost files — but only because wisr
  installs a compaction filter. Anything that later makes
  the filter conditional breaks the sweep silently.
- `CompactionFilter::IgnoreSnapshots()` is fixed at
  `true` (the overridable version is deprecated), so no
  entry is skipped because an open snapshot covers it.
  Every version in the compaction input is visited.

#### What Remains a Real Cost
**Excessive write amplification.** A full compaction pass
calls `CompactRange(..., nullptr, nullptr)` across all
column families. On large databases (e.g. 500GB+), this
forces a complete rewrite of levels L0 through L6,
causing I/O storms and write stalls — and its cost scales
with the number of relation CFs (§3.6), not only with
data volume.

#### Recommendation
Keep the protocol. Make it cheaper, and make its
dependencies explicit.
- State both RocksDB dependencies above in §8.3, and pass
  `bottommost_level_compaction` explicitly instead of
  relying on the default.
- Trigger the sweep on `CF:txn` growth rather than on a
  fixed period, so a full rewrite is paid for only when
  the space it reclaims justifies it.
- A denser in-memory commit map (a CLOG or 2-bit status
  bitset — 25 MB at 100M transactions) is worth sizing
  up, but note that it is what the commit cache of rule
  §3.4/2 already is. This is a capacity choice, not an
  architectural one.

One correction for the record: presuming that any version
with `xid < min_uncompacted_xid` and no entry in an
aborted set is committed — the fix an earlier draft
proposed — is **unsafe**. A crashed transaction leaves
versions with no status record anywhere, which is exactly
what Rule 4 exploits to make recovery undo-free.
Presuming them committed resurrects them unless recovery
first writes an `ABORTED` record for every crashed `xid`,
which reintroduces the undo pass the design set out to
avoid.

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
[`sm_read`][sm-read], whose loop in
[DESIGN.md §10.1][des-101] never leaves a single userkey
and so is exactly the case prefix mode is for.

`auto_prefix_mode = true` is the sharper instrument here.
Because `sm_scan` always sets `iterate_upper_bound`,
RocksDB can decide per seek: prefix mode when the bound
lies within the seek key's prefix, total order otherwise.
That keeps the bloom filter for single-key ranges. The
flaw documented on that option concerns keys shorter than
a full-length prefix, which cannot arise here — wisr's
extractor is variable-length, so `FullLengthEnabled()` is
false and the affected path is never taken.

---

### 3.3 Tombstone Purging via `DeleteRange` Overhead (§8.3)

#### The Problem
In [DESIGN.md §8.3][des-83], when the compaction filter
sees a tombstone with $\text{commit\_ts} \le
\text{gc\_watermark}$, it queues the key for background
deletion:
```cpp
db.DeleteRange(cf(relid), version_key, userkey | FF×12);
```
The document asserts that the filter cannot drop the
tombstone because:
> *"CompactionFilter::Context does not report [bottommost
> level] anyway"*

#### The API Assertion Is Correct
An earlier draft of this review claimed that
`CompactionFilter::Context` has carried a
`bool is_bottommost_level` since RocksDB 5.x. That is
wrong and is withdrawn. The struct carries
`is_full_compaction`, `is_manual_compaction`,
`input_start_level`, `column_family_id`, `reason`, and
`input_table_properties` — no bottommost flag. The
identifier `is_bottommost_level` exists only on
`rocksdb_compactionservice_jobinfo_t` in the C API, which
is unrelated. DESIGN.md's parenthetical stands.

The flag was never the design's real argument, either.
§8.3 rests on the fact that RocksDB picks compaction
inputs by full version key, so older versions of the same
userkey may sit in an adjacent SST file or another
subcompaction the filter never sees. "No older versions
appear in this compaction" does not establish "no older
versions exist", so dropping the tombstone in the filter
would resurrect them even with a bottommost flag in hand.

#### What Remains a Real Cost
**Range tombstone saturation.** Issuing thousands of
single-key `DeleteRange` calls inserts thousands of range
tombstones. During reads, RocksDB's `RangeDelAggregator`
must search all overlapping range tombstones, causing
read amplification and CPU overhead. This is upstream's
own warning, not a speculative one: `db.h` documents that
"accumulating too many range tombstones in the memtable
will degrade read performance", and recommends occasional
manual flushes and `max_open_files = -1` to contain it.

#### Recommendation
Keep the purge queue; reduce the number of tombstones it
produces, and record its constraints.
- Coalesce adjacent entries in the purge queue into a
  single `DeleteRange` before issuing it. A bulk delete
  over a key range is the shape `DeleteRange` is designed
  for; one call per deleted key is not.
- Give the purge task a flush cadence, per upstream's
  first caveat, so range tombstones do not accumulate in
  the memtable.
- Note in §12.2 that this path forecloses `row_cache`:
  `DeleteRange` returns `Status::NotSupported()` when a
  row cache is configured.

---

### 3.4 Savepoint Rollback Visibility Anomaly (§6.5 & §10.2)

#### The Problem
In [DESIGN.md §10.2][des-102]:
> *"The cursor also captures `T.seq_hwm` at `sm_scan` and
> applies §4.4 with that value, not the live one. Its view
> of the transaction's own writes is then fixed like the
> rest: a later `sm_rollback` neither hides rows the cursor
> could already see..."*

#### The Consequence
Consider this execution sequence:
1. Transaction begins, creates savepoint via
   [`sm_savepoint`][sm-savepoint].
2. Transaction inserts row `"k1"` (`seq = 1`, the first
   write of the transaction).
3. Transaction opens scan cursor `C` via
   [`sm_scan`][sm-scan]. `C` captures RocksDB snapshot
   $R_1$ and `seq_hwm = 1`.
4. Transaction calls [`sm_rollback`][sm-rollback] to undo
   `"k1"`.
5. Transaction calls [`sm_scan_next(C)`][sm-scan-next].

Because $R_1$ was taken prior to rollback, `"k1"` is
still in $R_1$. Because $C$ checks against its captured
`seq_hwm = 1`, **cursor $C$ yields `"k1"` to the user,
returning a row that the transaction itself explicitly
rolled back.**

This is a deliberate decision, not an oversight: the
§10.2 text quoted above states the behaviour outright,
and §6.5 reasons about this exact cursor when it explains
why `T.seq` is rewound. The issue is that the decision
looks accidental where it is visible to callers, and its
cost has not been weighed against the alternative.

#### Recommendation
Two defensible positions; pick one explicitly.
- **Follow PostgreSQL.** [`sm_rollback`][sm-rollback]
  closes scan cursors opened after the savepoint mark.
  With one savepoint per session this is cheap: record
  the set of open cursors at `sm_savepoint` and close
  everything not in it. This is what SQL callers expect.
- **Keep the current semantics** as a fixed-cursor-view
  rule consistent with the Halloween protection of §10.2.

Either way, [`API.md`][api]'s `sm_rollback` contract must
say what happens to open cursors. It currently documents
the SIREAD read-tracking caveat and says nothing about
cursors at all, so a caller cannot discover this
behaviour from the interface.

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

#### Already Documented — No Action Required
The justification is stronger than "key layout
aesthetics", and §6.2 states it: a read-committed
transaction with `T.xid = 50` updating a row against a
statement snapshot of 75 would place a version at `~50`
that sorts *behind* an already-committed `xid 60`.
Readers would return the older value and GC would drop
the newer commit as superseded. Checking writes against
the begin snapshot is what keeps §4.1's invariant at this
level.

The request an earlier draft made — that the divergence
be highlighted in user documentation — is already met.
[`API.md`][api] states it as divergence 2: "Writes can
fail with `SM_ERR_CONFLICT` at every level, including
READ COMMITTED", along with the note that the engine does
not silently re-read and retry. This item is closed;
it is recorded here only so the behaviour stays a
conscious choice.

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
$$\text{key} = \text{relid (8B)} \mid \text{userkey} \mid {\sim}\text{xid} \mid {\sim}\text{seq}$$
This makes relation creation/drop pure MVCC operations,
eliminates orphan CF sweeps, and scales to millions of
tables.

#### But the Trade Is Already Recorded
[DESIGN.md §13][des] lists a column family per relation
as a known trade-off and enumerates these same costs:
startup time growing with relation count, per-CF memtable
and LSM metadata, and CF create/drop sitting outside
MVCC. The alternative is not free either, and an earlier
draft of this review did not price it:
- `sm_rmrel` loses its O(1) `DropColumnFamily` and
  becomes a `DeleteRange` over the relation's entire
  keyspace — the same range-tombstone cost objected to in
  §3.3 above.
- Per-relation tuning, per-relation compaction statistics,
  and the ability to give one relation a different
  comparator or block cache all disappear.
- Every key grows by 8 bytes, and the `wisr.version.v1`
  comparator has to learn the prefix.

This is therefore a scale-threshold decision rather than
a defect. What the design is missing is a stated target:
how many relations must one database hold? At hundreds,
the current layout is the better trade. At tens of
thousands, it is not.

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
| **Environment & Root DB Lifecycle** | **PASS** | [DESIGN.md §3.5][des-35] PGDATA layout (`global/root`, `base/{dbid}`) clean & secure. |
| **Name Listing (`sm_strlist_t`)** | **PASS** | [PLAN.md][plan] specifies an opaque, safe, counted C ABI representation. |
| **Read Committed Level** | **PASS** | First-committer-wins at `sm_begin` causes aborts, but the divergence is documented (§3.5). |
| **Repeatable Read Level** | **PASS** | Snapshot isolation with first-committer-wins operates as intended. |
| **Serializable Level (SSI)** | **PASS** | Full Cahill/Ports SSI implementation without read locks. |
| **Savepoint & Rollback** | **WARN** | Cursor behaviour across savepoint rollback is decided but unstated in `API.md`. |
| **Two-Phase Commit (2PC)** | **PASS** | Durable `PREPARED` records and post-restart SSI rule meet [OBJECTIVE.md §Decisions][obj-dec]. |
| **Crash Recovery** | **PASS** | Status Rule 4 plus the §8.3 sweep ordering is sound; the cost is a full compaction pass (§3.1). |
| **Deployment Model** | **PASS** | Single-process multi-threading with pidlocks matches [OBJECTIVE.md §Decisions][obj-dec]. |

---

## 6. Conclusion

The addition of the PostgreSQL-style directory layout, the
root DB, and the [`sm_strlist_t`][strlist-type] abstraction
substantially improves wisr's architecture, providing robust
containment and clean lifecycle separation.

The remaining work lies squarely in the LSM-MVCC engine,
and on re-examination it is narrower than this review
first claimed. One live correctness bug stands:
**`total_order_seek` (or `auto_prefix_mode`) must be set
on scans**, or `sm_scan` returns nothing once a prefix
extractor is configured. Beyond it are refinements rather
than defects — bounding the cost of the `CF:txn` sweep
(§3.1), coalescing tombstone purges (§3.3), and settling
and documenting cursor behaviour across savepoint
rollback (§3.4).

Three claims in the earlier draft did not survive
checking against the RocksDB source and have been
withdrawn above: the cold-SST data loss under Rule 4
(§3.1), the existence of
`CompactionFilter::Context::is_bottommost_level` (§3.3),
and the charge that the Read Committed divergence is
undocumented (§3.5). In each case the design was right.
Addressing what remains will make wisr a truly
bulletproof, production-grade storage manager.

<!-- Reference Links -->
[des]: file:///home/cktan/p/wisr/DESIGN.md
[des-31]: file:///home/cktan/p/wisr/DESIGN.md#L93-L157
[des-34]: file:///home/cktan/p/wisr/DESIGN.md#L203-L244
[des-35]: file:///home/cktan/p/wisr/DESIGN.md#L245-L315
[des-43]: file:///home/cktan/p/wisr/DESIGN.md#L355-L403
[des-44]: file:///home/cktan/p/wisr/DESIGN.md#L404-L413
[des-5]: file:///home/cktan/p/wisr/DESIGN.md#L424-L440
[des-51]: file:///home/cktan/p/wisr/DESIGN.md#L454-L474
[des-62]: file:///home/cktan/p/wisr/DESIGN.md#L509-L532
[des-71]: file:///home/cktan/p/wisr/DESIGN.md#L617-L630
[des-83]: file:///home/cktan/p/wisr/DESIGN.md#L789-L891
[des-84]: file:///home/cktan/p/wisr/DESIGN.md#L961-L1009
[des-91]: file:///home/cktan/p/wisr/DESIGN.md#L1052-L1088
[des-101]: file:///home/cktan/p/wisr/DESIGN.md#L1126-L1145
[des-102]: file:///home/cktan/p/wisr/DESIGN.md#L1181-L1258
[des-122]: file:///home/cktan/p/wisr/DESIGN.md#L1457-L1461
[obj]: file:///home/cktan/p/wisr/OBJECTIVE.md
[obj-scope]: file:///home/cktan/p/wisr/OBJECTIVE.md#L6-L19
[obj-goals]: file:///home/cktan/p/wisr/OBJECTIVE.md#L20-L25
[obj-dec]: file:///home/cktan/p/wisr/OBJECTIVE.md#L44-L69
[api]: file:///home/cktan/p/wisr/API.md
[plan]: file:///home/cktan/p/wisr/PLAN.md
[sm-h]: file:///home/cktan/p/wisr/include/sm.h
[err-conflict]: file:///home/cktan/p/wisr/include/sm.h#L37
[err-version]: file:///home/cktan/p/wisr/include/sm.h#L47
[sm-init]: file:///home/cktan/p/wisr/include/sm.h#L73
[sm-attach]: file:///home/cktan/p/wisr/include/sm.h#L83
[sm-detach]: file:///home/cktan/p/wisr/include/sm.h#L93
[strlist-type]: file:///home/cktan/p/wisr/include/sm.h#L108
[strlist-free]: file:///home/cktan/p/wisr/include/sm.h#L120
[sm-read]: file:///home/cktan/p/wisr/API.md#L618-L632
[sm-scan]: file:///home/cktan/p/wisr/API.md#L633-L670
[sm-scan-next]: file:///home/cktan/p/wisr/API.md#L671-L685
[sm-savepoint]: file:///home/cktan/p/wisr/API.md#L415-L425
[sm-rollback]: file:///home/cktan/p/wisr/API.md#L426-L446
[sm-rmrel]: file:///home/cktan/p/wisr/API.md#L551-L564
[sm-scan-t]: file:///home/cktan/p/wisr/API.md#L165
