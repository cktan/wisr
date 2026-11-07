# Parity with PostgreSQL on q1, q2 and q3 (btree indexes)

Goal: digres runs `bench/q1-sk10.sql`, `q2-sk180.sql` and
`q3-skfetchrow.sql` on btree
indexes as fast as PostgreSQL 18. This document gives the
profile, what was changed, and what is left. Written 2026-11-07
by a profile of `main` at 20f43e31.

## 1. Method

- Box: GCP aarch64, 4 CPUs. Server pinned to CPU 2, pgbench to
  CPU 3. Both servers have `shared_buffers = 2GB`.
- Data: `bench/load.sql`, 10,000 rows, then `mkbtidx.sql`, which
  vacuums. So q1 reads 8 entries, q2 reads 183, q3 reads 8 rows.
- Cost: user instructions a statement, from `perf stat
  --per-thread` on the server, for pgbench runs of 5,000 and
  25,000 prepared statements. The slope of the two cancels the
  connection and the first plan. The busiest thread is the
  backend: digres is one process, so a count of the process
  holds the cleaner and RocksDB threads too.
- Profile: `perf record -e instructions:u -c 10007`. The period
  is prime, so it cannot alias with a loop.
- Both systems use the same plan for q1 and q2: an index-only
  scan and a count. For q3 digres uses an index scan and
  PostgreSQL a bitmap heap scan.

**Warning: the count depends on the state of the data.** A
restart moved q2 by up to 47% in one run (374k, then 550k) when
the server was started in a different way. Compare two builds on
the same data directory, with the same restart, as `ISSUE.md`
§2.3 says. The A/B numbers below were taken that way.

## 2. Where the instructions go

Instructions a statement, `main` at 20f43e31:

| Query | digres | PostgreSQL | digres / PostgreSQL |
| --- | --- | --- | --- |
| q1, 8 entries | 71,447 | 60,170 | 1.19 |
| q2, 183 entries | 373,663 | 148,062 | 2.52 |
| q3, 8 rows read | 177,064 | 86,009 | 2.06 |
| q0, no entry (`WHERE` matches nothing) | 48,229 | 50,597 | 0.95 |

- **The fixed cost is at parity.** q0 is a count over an index
  that matches no entry. digres costs 2,400 less than
  PostgreSQL. So the open of the table, the plan, the commit and
  the rest cost no more than PostgreSQL's, since `wip-tblpark`.
- **q1 and q2 differ by the cost of an entry.** From q0, q1 and
  q2: digres pays 1,060 instructions an entry, PostgreSQL 530.
  q1 reads 8 entries, so its gap is 8 × 530 plus the descent to
  the first leaf.
- **q3 differs by the cost of a row.** digres pays about 13,200
  instructions a row beyond q1, PostgreSQL 3,200. In q3's
  profile RocksDB takes 40% (about 9,000 a row, in `MultiGet`),
  sm and wisr 22%, the rest is the executor.

What an entry cost digres in q2 on `main`, by share of the
profile:

| Part | Share |
| --- | --- |
| tree cursor: `tbl_scannxt`, `cursor_next`, `idx_scan_entry`, `page_view_t` | about 33% |
| decode the key: `idx_entry_cols`, `decode_key_front`, `get_escaped`, `key_decodable`, `bytes_unkey` | about 20% |
| recheck the equality: `keys_met`, `texteq`, `FunctionCall2Coll` | about 8% |
| PostgreSQL's executor: `ExecInterpExpr`, `ExecScan`, `IndexOnlyNext`, `ExecAgg` | the rest |

## 3. What was done

Two branches, not merged. `RESULT.md` in the home directory has
the numbers of each step.

### 3.1 `wip-parity-ios`: read no column when none is wanted

`count(*)` reads no column of an entry, yet digres decoded every
key and rechecked the equality on it. Now:

1. The planner sets `IndexOnlyScan.nocols` when nothing above
   reads a column and the scan checks none
   (`createplan.c`).
2. `sm_rel_scan_plain` tells, with no decoding, that a tree
   entry's key is whole: not cut, and it gives its columns back.
3. `smstore_index_getentry` returns such an entry at once if
   every scan key is an exact bound, or an equality on a text
   value of 64 bytes or less in a column of a deterministic
   collation. The bound then holds the key for every whole
   entry. A cut entry takes the old path and its row.

### 3.2 `wip-parity-tree-entry`: on top of that branch

1. `idx_decodable` asks `key_decodable` once for an index, not
   once for each entry. This was 6% of q2.
2. A leaf fill finds the first key past the high bound from the
   leaf's last key, or by binary search, not by comparing every
   key.
3. `smstore_index_beginscan` makes the places for array keys only
   when a key is an array.

Instructions a statement. The first three rows are on one data
directory, restarted for each; "off" is the same binary with the
new path turned off by a switch that is not in the commit.

| Build | q1 | q2 | q3 |
| --- | --- | --- | --- |
| `main`, before | 71,447 | 373,663 | 177,064 |
| first branch, off | 71,509 | 374,256 | 176,880 |
| first branch, on | 63,755 | 260,063 | 176,781 |
| first branch, final | 63,742 | 260,053 | 176,438 |
| second branch, decodable and fill | 63,725 | 241,893 | 176,696 |
| second branch, final | 63,071 | 241,411 | 176,393 |
| PostgreSQL 18 | 60,170 | 148,062 | 86,009 |
| second branch / PostgreSQL | 1.05 | 1.63 | 2.05 |

The tests of the first branch passed in the debug build: sm
(unit, rel, isolation) and dg (231 pgregress files, `run.sh`,
`server.sh`). The test `dg/test/index_count_text.sql` fails when
the skip is made unsound (a bound of any length, a cut entry
called whole): it then counts 30 where 10 is right.

## 4. Prognosis

**q1: parity is near.** The gap is 2,900 instructions, 5%.
Fixed cost is already lower than PostgreSQL's, so the gap is 8
entries and one descent. A cut of 360 instructions an entry
closes it. The next steps below do that.

**q2: parity needs the entry to cost 530 or less, now 1,000.**
The decode and the recheck are gone. What is left is the tree
cursor, about 46% of q2, and the executor. Ranked by the saving
I expect, not measured:

1. **Copy an entry once.** `fill` copies a leaf's entries into
   the cursor, and `tree_fill` copies them again into the
   scan's `bytes`, then builds nine parallel arrays an entry
   (`key_at`, `row_at`, `cuts`, `unread`, `tuples`, `rcs`, `ids`
   and the views). A scan with no row to read needs few of
   them. About 10% of q2.
2. **Hand out a leaf's entries without the copy,** from the
   pinned leaf, as PostgreSQL walks its page. About 5%.
3. **Count in sm.** For a scan with `nocols` and bounds that
   hold every key, sm can count the visible whole entries of a
   leaf at once and give the executor one tuple a leaf. It
   needs a batch call and a change to the executor's loop. It
   could remove most of the per-entry cost of q2, but only for
   `count(*)`.

With the first two, q2 reaches about 1.4 times PostgreSQL.
Parity on q2 needs the third.

**q3: parity is not reachable by tuning.** A row of q3 costs
digres 13,200 instructions beyond the entry, and 9,000 of them
are RocksDB's `MultiGet`. The settings are already tuned (16 KB
blocks, restart interval 4, a hash index in the block, a
prefix bloom filter). If RocksDB's whole cost went to zero, q3
would still be 105,000, 1.2 times PostgreSQL. Two things could
close it, and both are large:

- A row cache in sm, in its buffer pool, keyed by the row's key
  and version, as the tree pages are. A hit would cost 1,000 to
  2,000 a row. q3 would be about 115,000, 1.35 times
  PostgreSQL. It needs a design for the MVCC version in the key
  and for invalidation.
- Keep a table's rows in sm's own pages, as the tree does for
  an index. This is the change that gives parity, and it is a
  redesign of the row store.

## 5. Not done, and not known

- Throughput. Instruction counts are the measure here. Section
  6 has the throughput of `bench/run.sh` for the final branch.
- The data is 10,000 rows. A table that does not fit in the
  block cache moves q3 most.
- A restart changes the count (section 1). The cause is not
  known; `ISSUE.md` §2.3 asks for a load, then a compare of the
  counts before and after a flush and a compaction.
- Debug-build tests ran on the first branch alone, then on the
  second. See `RESULT.md` for the status of each.

## 6. Throughput of `bench/run.sh`

The final branch (`wip-parity-tree-entry`), the same box and the
same servers, 10 seconds a query, one client, prepared. The
earlier run is `main` before any change, the same day.

| query | index | dg tps | pg tps | dg / pg latency | before |
| --- | --- | --: | --: | --: | --: |
| q1 | btree | 49,521 | 57,884 | 1.18 | 1.24 |
| q2 | btree | 30,846 | 42,194 | 1.33 | 1.78 |
| q3 | btree | 25,276 | 47,614 | 1.90 | 1.95 |

- Each cell is one run of 10 seconds, not repeated. I did not
  measure the spread of tps. Read a change under 10% as noise:
  the q1 and q3 changes are. The q2 change, 27% more tps, is
  far above it, and the instruction counts agree with it.
- The instruction ratio of q2 is 1.63, but the latency ratio is
  1.33. digres runs q2's instructions at a higher IPC (3.3, as
  PostgreSQL's 3.4), because they are a tight loop.
- q1 is 1.05 in user instructions and 1.12 in cycles, user and
  kernel together: digres 50,500 cycles, PostgreSQL 45,200.
  digres runs q1 at an IPC of 1.9, PostgreSQL's at 2.0, and
  the rest of the gap is in the time the client waits. So when
  q1 reaches 1.00 in instructions, it will still be about 1.07
  in time. A profile by cycles, not by instructions, is the next
  step for q1.
