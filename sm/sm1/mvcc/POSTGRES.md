# POSTGRES — PostgreSQL's isolation tests, beside these

PostgreSQL validates its concurrency control with an isolation
test suite: scenario files in `src/test/isolation/specs`, each
a schedule of steps across sessions with an expected outcome,
as the scenarios here are. This file keeps that list, for
porting cases that [`MVCC.md`](MVCC.md)'s scenarios do not yet
cover.

The list is the suite on PostgreSQL's `REL_17_STABLE` branch,
125 files, read through GitHub's API on 2026-10-06. The
comparison with wisr's scenarios is by name and purpose; the
scripts have not been compared line by line.

---

## 1. Where the serializable tests come from

"Serializable Snapshot Isolation in PostgreSQL", Dan R. K.
Ports and Kevin Grittner, VLDB 2012
(<https://arxiv.org/abs/1208.4179>), describes two examples:

- **Example 1, simple write skew** (its Figure 1): two
  transactions each count the doctors on call and, finding at
  least two, take one off. Under snapshot isolation both
  commit, and no one is on call.
- **Example 2, batch processing** (its Figure 2): `NEW-RECEIPT`
  reads the current batch number and inserts a receipt under
  it; `CLOSE-BATCH` increments the number; a read-only `REPORT`
  totals the previous batch. Snapshot isolation lets a report
  show a total that a later receipt changes, and the anomaly
  needs all three, the read-only one included.

Its evaluation uses benchmarks, not tests: SIBENCH, DBT-2++
(TPC-C-like, with Cahill's "credit check" transaction) and
RUBiS. The suite below is where the cases live.

---

## 2. The suite, by subject

**Serializable: the paper's examples**

- `simple-write-skew` — Example 1
- `receipt-report`, `read-only-anomaly`, `read-only-anomaly-2`,
  `read-only-anomaly-3` — Example 2 and its read-only variants

**Serializable: write skew and predicates**

- `classroom-scheduling`, `temporal-range-integrity`,
  `project-manager`, `total-cash`, `two-ids`,
  `multiple-row-versions`, `matview-write-skew`

**Serializable: through indexes**

- `partial-index`, `index-only-scan`, `predicate-gin`,
  `predicate-gist`, `predicate-hash`, `predicate-lock-hot-tuple`

**Serializable: unique keys**

- `read-write-unique`, `read-write-unique-2`,
  `read-write-unique-3`, `read-write-unique-4`,
  `insert-conflict-serializable`

**Serializable: integrity and transactions**

- `referential-integrity`, `ri-trigger`, `prepared-transactions`,
  `prepared-transactions-cic`, `update-conflict-out`,
  `serializable-parallel`, `serializable-parallel-2`,
  `serializable-parallel-3`

**READ COMMITTED re-check and row locks**

- `eval-plan-qual`, `eval-plan-qual-trigger`,
  `lock-update-delete`, `lock-update-traversal`,
  `lock-committed-update`, `lock-committed-keyupdate`,
  `update-locked-tuple`, `propagate-lock-delete`,
  `tuplelock-conflict`, `tuplelock-update`,
  `tuplelock-upgrade-no-deadlock`, `tuplelock-partition`,
  `multixact-no-deadlock`, `multixact-no-forget`,
  `aborted-keyrevoke`, `delete-abort-savept`,
  `delete-abort-savept-2`, `subxid-overflow`

**Waits and deadlocks**

- `deadlock-simple`, `deadlock-hard`, `deadlock-soft`,
  `deadlock-soft-2`, `deadlock-parallel`, `nowait`,
  `nowait-2` to `nowait-5`, `skip-locked`, `skip-locked-2` to
  `skip-locked-4`, `lock-nowait`, `timeouts`

**Foreign keys**

- `fk-contention`, `fk-deadlock`, `fk-deadlock2`,
  `fk-partitioned-1`, `fk-partitioned-2`, `fk-snapshot`

**INSERT ... ON CONFLICT and MERGE**

- `insert-conflict-do-nothing`, `insert-conflict-do-nothing-2`,
  `insert-conflict-do-update`, `insert-conflict-do-update-2` to
  `-4`, `insert-conflict-specconflict`, `merge-insert-update`,
  `merge-delete`, `merge-update`, `merge-match-recheck`,
  `merge-join`

**DDL and maintenance against DML**

- `alter-table-1` to `-4`, `create-trigger`,
  `ddl-dependency-locking`, `drop-index-concurrently-1`,
  `multiple-cic`, `reindex-concurrently`,
  `reindex-concurrently-toast`, `reindex-schema`,
  `truncate-conflict`, `cluster-conflict`,
  `cluster-conflict-partition`, `sequence-ddl`,
  `drop-owned-grant`, `tablespace-dependency-locking`,
  `detach-partition-concurrently-1` to `-4`,
  `partition-concurrent-attach`, `partition-drop-index-locking`,
  `partition-key-update-1` to `-4`, `inherit-temp`,
  `temp-schema-cleanup`, `pub-concurrent-drop`

**Vacuum, freezing and the rest**

- `freeze-the-dead`, `horizons`, `vacuum-concurrent-drop`,
  `vacuum-conflict`, `vacuum-no-cleanup-lock`,
  `vacuum-skip-locked`, `async-notify`, `stats`,
  `plpgsql-toast`, `inplace-inval`, `intra-grant-inplace`,
  `intra-grant-inplace-db`

---

## 3. Against wisr's scenarios

| PostgreSQL | wisr's nearest |
| --- | --- |
| `simple-write-skew` | `c02_write_skew_point_reads` |
| `receipt-report`, `read-only-anomaly*` | `c07_read_only_anomaly` |
| `classroom-scheduling`, `temporal-range-integrity`, `project-manager` | `c03`, `c04`, `c22`: write skew through predicates |
| `eval-plan-qual` | `f01` to `f11`: the READ COMMITTED re-read |
| `deadlock-simple`, `deadlock-hard` | `b16` to `b21` |
| `prepared-transactions` | `test/test_2pc.cc` |
| `truncate-conflict` | `rel/test/test_truncate.rel`, step 4 |

**Ported**, as permutation scripts
([`DRIVER.md`](DRIVER.md#6-permutation-scripts) §6), `in/p01`
to `in/p10`: `simple-write-skew`, `receipt-report`,
`read-only-anomaly`, `read-write-unique`, `total-cash`,
`two-ids`, `classroom-scheduling`, `temporal-range-integrity`,
`project-manager`, `multiple-row-versions`.
[`MVCC.md`](MVCC.md#7b-family-p--postgresqls-own-isolation-tests)
§7b has
how each compares.

**Worth porting next**, also serializable: `partial-index`,
`index-only-scan`, `read-write-unique-2` to `-4`,
`read-only-anomaly-2` and `-3`, `prepared-transactions`.

`read-write-unique` (p04) confirms the ordering no scenario
here had checked: two serializable inserts of one key end with
a serialization failure, not a unique violation, as in
PostgreSQL.

**Not for sm1 yet:**

- `nowait*`, `skip-locked*`, `lock-nowait`, `timeouts`: the wait
  policies are digres's, on sm1's `trylock` and a timer's
  cancel ([`../../NEXT.md`](../../NEXT.md#2-loose-ends) §2); no
  scenario here drives them.
- `fk-*`, `ri-trigger`, `referential-integrity`,
  `insert-conflict-*`, `merge-*`: the host's, above the
  stack.
- `tuplelock-*`, `multixact-*`, `*-keyupdate`: PostgreSQL's
  in-tuple row locks and its `FOR KEY SHARE` and `FOR NO KEY
  UPDATE` modes, which sm1 does not have.
- Vacuum and freezing: sm1 has neither; its garbage collection
  is compaction's.
