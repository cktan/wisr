# RCOMPACT — compaction on another machine

Garbage collection in wisr rides on RocksDB compaction: the
filter in `src/gc.cc` judges every version a compaction passes
over, and §8.3 of `SM1_DESIGN.md` is the argument for why that is
where the work belongs. Compaction is also the most expensive
background work the engine does.

So: can it be moved to another machine, shipping the fragments
it needs?

**Yes in principle, and RocksDB supports it directly. But the
filter as written cannot be shipped, for one specific reason,
and the fix is a contained change to how its inputs are
supplied.** Nothing here is implemented.

## 1. What RocksDB provides

`CompactionService` (`rocksdb/options.h`, present in the
RocksDB 8.9 this builds against):

- the primary serializes a compaction job and hands it to
  `CompactionService::StartV2()`;
- the remote side runs `DB::OpenAndCompact()`, documented as
  *"a read-only operation, the result won't be installed to
  the DB, it will be output to the `output_directory`"*;
- the primary installs the resulting SST files.

The inputs are immutable SST files. If they sit on shared
storage the worker opens them in place and no fragment is
copied at all; otherwise they are copied to the worker and the
outputs come back.

## 2. Why this filter cannot be shipped as it stands

The worker supplies its **own** filter:
`CompactionServiceOptionsOverride` carries
`compaction_filter_factory`, with the note that *"pointer
configurations are not passed to compaction service compaction
so the user needs to set it"*. That is fine for a filter that
is a pure function of the bytes it is given. `gc_filter_t` is
not. Every `Filter()` call reaches into the primary's memory:

| what it needs | where that lives |
| --- | --- |
| `xct_status(db, xid, &commit_ts)` (`src/gc.cc:76`) | the in-memory active table, the commit cache, and `CF:txn` |
| the watermark it judges against | `min` over live snapshots, open cursors and prepared transactions (§8.3) |
| `db->purges` and `gc_cv` (`src/gc.cc:110-114`) | the in-process purge queue the tombstone rule pushes onto |
| `db->gc_filter_errors` (`src/gc.cc:68`, `:82`) | the counter the `CF:txn` sweep consults before trusting a pass |

The first two are inputs the worker does not have. The last
two are **side effects travelling outward**, which a remote
worker cannot perform and would have to return instead.

## 3. Why it is nonetheless tractable

The filter's inputs are **monotone**, which is what makes them
safe to precompute and ship:

- a transaction's status only ever moves one way: once
  `COMMITTED`, always `COMMITTED`, and `commit_ts` never
  changes afterwards;
- judging against a watermark *lower* than the true one is
  always safe — it collects less, never wrongly;
- keeping a version is always safe. Only dropping and stamping
  have to be right.

So a job can carry its own answers:

1. **Resolve the xids up front.** Collect the distinct xids in
   the job's input files, resolve each on the primary, and send
   an `xid → (status, commit_ts)` map with the job. The map is
   small next to the data it describes, and it turns
   `xct_status()` into a lookup — which also removes the
   awkward requirement that a worker read a *moving* `CF:txn`
   consistently.
2. **Freeze the watermark into the job.** It is already a
   constructor argument (`gc_filter_t(db, storeid, watermark)`),
   so this is nearly free.
3. **Return the side effects rather than performing them.**
   The queued tombstone purges and the doubt count become part
   of the job's result, and the primary applies them when it
   installs the output. The `DeleteRange` purge itself stays on
   the primary in any case: it is a write.

Step 1 is worth doing even with no remote worker in sight. It
replaces a per-key status resolution with one batch resolution
per compaction, which is the same work the local filter does
today, only amortized.

## 4. What does not fit

**The `CF:txn` sweep, which is the pass you would most want to
offload.** §8.3's sweep is a forced full compaction across
every store column family, and its correctness rests on
state a worker does not have: the watermark recorded at the
pass's *start*, the two read-epoch counters that decide when
the records may go, and the "did any version stay unstamped"
verdict that decides whether the pass may be trusted at all.
Ordinary background compaction is the easy case; the sweep
needs its own argument before it could be moved.

## 5. When it would pay

Offloading trades local CPU and I/O for network I/O and
coordination. It pays when the primary is CPU-bound on
compaction and the storage is shared, so no fragment is copied.
It is a loss when fragments cross the network to relieve a
machine that was not the bottleneck.

Which of those describes wisr is **not known**: there is no
benchmark yet, and the same measurement gap that leaves §13's
other trade-offs unquantified leaves this one unquantified too.
No work here should start before that number exists.

## 6. One thing this does not cost

Remote compaction distributes a **stateless computation over
immutable inputs**. That is a different problem from letting
several processes use one database, which is what
`OBJECTIVE.md`'s single-process decision rules out and what
`../PART4.md` §2 shows to be the hard blocker for PostgreSQL.

So unlike almost every other distribution question about this
engine, remote compaction needs no part of that decision
reopened.
