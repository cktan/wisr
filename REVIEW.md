# Code Review: Scattered Keys

The review found no wrong-row, crash or memory-safety bug. The
top findings are performance and error-handling defects.

- **Target:** `HEAD~2..HEAD`, which is `bfe0128a` and `ef64048b`.
  These two commits are the scattered-keys feature.
- **Why this target:** `main` matches `origin/main`, and the tree
  has no change to tracked files. The review had no pending diff
  to read, so it took the latest feature. The two commits were
  the reviewer's choice, not a request.
- **Effort:** `high`.
- **Verification:** the findings come from reading the code. The
  review ran no tests and no benchmarks. Lines cited in findings
  1, 2, 3, 4, 7, 8 and 9 were checked against `ef64048b`.
- **Order:** the most severe finding is first.

## Findings

### 1. Correlation disables the iterator for clustered rows

- **Location:** `sm/sm3/src/table/table.cc:1687`
- **Defect:** correlation measures global order, not clustering.
  A low value disables the batch iterator for the whole cursor.
  This overrides the runtime check that would keep the iterator.
- **Scenario:**
  1. Load rows tenant by tenant, with hashed tenant ids.
  2. Each tenant's rows are adjacent, but |correlation| is near
     0.
  3. `tree_scattered` returns true.
  4. `read_chained_batch` does not try the steps.
  5. Each 64-row batch of a contiguous range goes to MultiGet.
- **Note:** step 4 of `test_tree.cc` asserts this behavior. Rows
  that are together with correlation -0.1 make no iterator. The
  runtime check of WORKNOTE step 372 alone would keep the
  iterator here.

### 2. A probe's gap carries over to later probes

- **Location:** `sm/sm3/src/table/table.cc:2077`
- **Defect:** `tbl_scan_rebound` resets the cursor's per-probe
  state (`at`, `n`, `batch`, `entries_done`). It does not reset
  the `missed` flag of the kept iterator.
- **Scenario:**
  1. A query with `k = ANY(array)` or an IN list reads a tree.
  2. The query reaches `open_probe` through `next_probe`, which
     aims the same cursor again.
  3. The two rows of one value are far apart, so `missed` is set.
  4. All later probes go to MultiGet, even with adjacent rows.
- **Note:** sm1 has no call that clears `missed` without freeing
  the iterator.

### 3. One pair of keys decides for the whole cursor

- **Location:** `sm/sm1/src/chain.cc:864`
- **Defect:** the fills of a tree cursor are 1, 2 and 4 rows. The
  second fill has `n` = 2, so a single miss makes `done` = 1.
- **Scenario:**
  1. A tree has a correlation of 0.9.
  2. One out-of-place row puts the second and third rows of a
     long range scan more than 4 live keys apart.
  3. `missed` is set.
  4. MultiGet reads all remaining rows, though the steps work.
- **Fix:** take a larger sample before giving up, or count the
  misses.

### 4. `tree_scattered` drops a statistics failure

- **Location:** `sm/sm3/src/table/table.cc:1675`
- **Defect:** `tree_scattered` drops a `cat_get_stats` failure
  and leaves the error in the thread's error state. The cache
  does not keep a failed load, so the load repeats on the read
  path.
- **Scenario:**
  1. A cold load runs `in_own_txn` inside `tree_fill`: opendb,
     begin, catalog scan, commit. The "in memory" note of
     WORKNOTE step 373 does not state this.
  2. The load fails, for example with `SM_ERR_CORRUPT` from
     `decode_stat`, or when opendb cannot open a session.
  3. `cat->stats` gets no entry.
  4. Each new cursor on the table repeats the full load.
  5. This path hides the corruption.
- **Note:** no test covers this path.

### 5. Tests skip the expression branch and ANALYZE

- **Location:** `sm/sm3/test/test_tree.cc:1131`
- **Defect:** the test injects statistics through `cat_put_stats`
  with a column position only. The expression branch at
  `table.cc:1685` and the real ANALYZE path do not run. The
  commits add no dg test.
- **Scenario:** `analyze.cc` writes an expression position that
  does not match `0 == st.position` in `tree_scattered`. All
  tests pass, and the feature does nothing for expression trees.
- **Note:** no tree test in `dg/test/slt` mentions ANALYZE.
  `CLAUDE.md` requires each layer that can express the scenario,
  and a statement of which layers cannot. WORKNOTE steps 372 and
  373 name neither the dg layer nor the rel layer.
  `BATCHREAD.md` §4 is unchanged.

### 6. Non-tree secondary indexes keep the old cost

- **Location:** `sm/sm3/src/table/table.cc:1320`
- **Defect:** `tbl_read_keys` passes no iterator, so the fix
  covers only tree cursors. A non-tree secondary index pays on
  each batch the cost that the two commits removed.
- **Scenario:**
  1. dg's read-ahead for a non-tree secondary index (`ra_rows`,
     through `sm_tbl_read_keys`) sends ascending row keys with
     `bi == nullptr`.
  2. Each call does NewIterator and Seek.
  3. The call misses the second key and falls to MultiGet.
  4. `if (bi && 1 == done)` cannot record the miss.
  5. Nothing calls `tree_scattered` on this path.
- **Note:** this path costs more than a tree cursor did, because
  it builds the iterator again for each batch.
- **Fix:** put the decision on a kept iterator that this path
  also uses, or in sm1.

### 7. MultiGet sorts keys that are already sorted

- **Location:** `sm/sm1/src/chain.cc:885`
- **Defect:** the hot MultiGet call passes `false` for
  `sorted_input`. This occurs when line 815 forces `ascending` to
  false after the keys were found sorted, and when MultiGet reads
  the sorted tail after a miss.
- **Scenario:** on the scattered path of q3,
  `PrepareMultiGetKeys` in RocksDB runs `std::sort` on keys that
  are in order. The commits optimize this exact path.
- **Fix:** keep a separate `sorted` flag, and pass it as
  `sorted_input`. Current keys compare bytewise under
  `version_comparator`, so the claim is valid.

### 8. The new code sits inside a dead condition

- **Location:** `sm/sm1/src/chain.cc:860`
- **Defect:** `++seeks == 1` is true on the first miss, so the
  loop always breaks and never seeks past a gap.
- **Scenario:** `placed && ++seeks == 1` reduces to
  `if (placed)`, so `int seeks` can go. Three comments are stale:
  - lines 804-806: "seeking only past a gap".
  - line 853: "then a seek".
  - `sm/sm3/src/table/pin.h:64`: "kept to the cursor's end",
    though the iterator can now hold none.

### 9. Two flags hold one outcome

- **Location:** `sm/sm1/src/session.h:68`
- **Defect:** `missed` and `scattered` record one outcome. The
  store-scoped flag serves a case that no production caller has.
- **Scenario:** the only production caller, `tree_fill`, keeps
  one iterator for each cursor on one table, which is one store.
  This leaves the `storeid` comparison at `chain.cc:814`, the
  reset at line 845 and a second field to maintain.
- **Fix:** use one flag, set by `batch_iter_new` or by a miss.
  Only sm1's tests can reach the difference.

### 10. A procedure comment breaks the steps rule

- **Location:** `sm/sm1/src/chain.cc:742`
- **Defect:** step 3 of the procedure comment holds several
  actions, and its continuation lines lost their indent.
- **Scenario:** step 3 checks the keys, picks or refreshes the
  iterator, seeks, steps, stops, marks `bi` missed and releases
  the iterator. `CLAUDE.md` requires one action for each step.
  Lines 744-747 sit at column 3, not under the step text.
- **Fix:** split it into numbered steps.
