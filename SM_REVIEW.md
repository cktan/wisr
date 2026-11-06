# Storage manager code review

Reviewed on 2026-11-06 at commit
`2cd5805d751d92849200b7b09bb4c7c22871f980`.

## Findings

Four correctness issues were confirmed with focused probes.
Three are P1 because they can return invalid data or repeat
identifiers. One is P2 because repeated conflict checks exhaust
an internal resource during normal use.

| Priority | Finding | Evidence |
| --- | --- | --- |
| P1 | B-tree batches survive rollback and abort | Two failing probes |
| P1 | A tree arbiter returns a row whose key changed | Coordinated two-session probe |
| P1 | A failed sequence lease read restarts the sequence | Injected malformed lease |
| P2 | Conflict checks retain internal savepoints | Failure on conflict call 257 |

### P1: Invalidate B-tree read-ahead when transaction state changes

Primary location:
[table.cc:1861](sm/sm3/src/table/table.cc#L1861).
Related cleanup:
[pin.cc:138](sm/sm3/src/table/pin.cc#L138) and
[pin.cc:175](sm/sm3/src/table/pin.cc#L175).

`tree_next()` consumes prefetched tuples without checking
whether rollback or transaction end invalidated the batch.
`pins_rolled_back()` skips tree cursors because they have no
`rel` cursor. `release_all_pins()` drops the cursor's current
pin, but leaves its batch positions, tuple views, and other
raw pins intact.

The check in `still_has()` does not repair this. It compares
the cached tuple with the cached index key. It does not read
the row again to establish visibility after rollback.

Reproduction:

1. Begin a transaction and set a savepoint.
2. Insert four rows covered by a B-tree index.
3. Open a tree row scan and fetch two rows. The first batch
   holds one row; the second holds two, leaving one unread.
4. Roll back to the savepoint, undoing all four inserts.
5. Fetch again from the cursor.

The next fetch returns success and a 14-byte tuple from an
undone insert. A separate probe commits the initial rows,
opens another scan, fetches twice, aborts, and fetches again.
That fetch also returns success and a 14-byte tuple.

**Impact:** a caller can observe a row after its insertion
was undone. Transaction end can also revive a cursor pin
with data from the ended transaction. The behavior depends
on the batch position, making it difficult to reproduce
with tests that fetch only one row.

**Suggested fix:** track tree cursors through rollback and
transaction end. Discard all prefetched rows and release all
batch pins. Close cursors created after a rolled-back mark,
or explicitly rebuild their read state if continuation is
part of the contract. Checking only `in_txn` is insufficient:
rollback leaves the transaction active, and a later begin
must not revive a cursor from an earlier transaction.

**Regression coverage:** exercise partially consumed batches
at rollback, abort, commit, and a subsequent begin. Cover
both ordinary and index-only tree scans. The probes here
confirmed rollback and abort for ordinary tree scans.

### P1: Recheck a tree arbiter's key after acquiring its row lock

Primary location:
[table.cc:2022](sm/sm3/src/table/table.cc#L2022).
Caller:
[table.cc:691](sm/sm3/src/table/table.cc#L691).

`tree_find(..., now=true)` reads a row's newest version and
checks its index key before taking the row's shared lock.
After taking that lock, it returns without repeating the
key check. An explicit row lock can block this acquisition
without leaving an uncommitted row version for the earlier
`rel_read_newest()` call to detect.

When the caller requests the conflict tuple, `find_now()`
reads the row again under the lock. It still does not check
whether the new tuple has the requested unique key.

The coordinated probe used this interleaving:

1. Commit a row with `owner = 'x'` in a unique tree index.
2. Session B takes an exclusive row lock without updating it.
3. Session A tries an arbiter insert with `owner = 'x'`.
4. A reads the committed row, then waits for B's row lock.
5. B updates the row to `owner = 'y'` and commits.
6. A acquires its lock and returns the conflict.

The probe waits until `waits_for()` reports A blocked by B
before performing the update. A returns 1 and the tuple with
`owner = 'y'`, although B has freed the key. The expected
result is an insertion of the new row with `owner = 'x'`.

**Impact:** `ON CONFLICT DO NOTHING` can skip an insertion
after the conflicting key has disappeared. A caller using
the returned tuple for `DO UPDATE` receives a row whose key
no longer matches the arbiter key.

**Suggested fix:** after the row lock is acquired, read the
newest row again and evaluate its index key and membership.
Restart the lookup if it was deleted, moved, or no longer
matches. Preserve the behavior that releases temporary
conflict-check locks before returning to the caller.

**Regression coverage:** keep the explicit-lock interleaving
above, with and without a requested conflict tuple. Include
deletion and changes to a partial index's predicate.

### P1: Distinguish a missing sequence lease from a failed read

Primary location:
[db.cc:947](sm/sm1/src/db.cc#L947).
Read helper:
[db.cc:413](sm/sm1/src/db.cc#L413).

`read_meta()` returns false for every unsuccessful RocksDB
read and for a value whose length is not eight bytes.
`db_next_seq()` interprets false as an unused sequence and
sets its next value to `start`. It then persists a new lease
and issues values without reporting the read failure.

Reproduction with a malformed persisted lease:

1. Create a sequence starting at 1, with increment 1.
2. Commit it and draw its first value: 1.
3. Replace its lease in `CF:meta` with the bytes `bad`.
4. Remove its cached sequence state to force lease recovery.
5. Draw again by the public sequence API.

The second draw returns success and issues 1 again. This is
a fault-injection probe: the malformed lease and cache
eviction use internal access. The same branch handles an
I/O or checksum error from RocksDB, as shown by inspection;
those error statuses were not separately injected.

**Impact:** a lease read error can become silent identifier
reuse. Row identifiers, TOAST identifiers, and tree page
identifiers depend on sequence allocation. Their callers
rely on those identifiers being fresh, so repeated values
can cause failed writes or overwrite existing objects.

**Suggested fix:** return distinct results for found,
not found, and error. Start a sequence only on a confirmed
`IsNotFound()`. Report malformed leases as `SM_ERR_CORRUPT`
and propagate other RocksDB errors. Do not cache a fresh
sequence state or replace its lease after a failed read.
Apply the same distinction to `db_last_seq()`.

**Regression coverage:** inject malformed leases and failed
reads after a prior successful draw. Verify no value is
issued and no lease is overwritten. Verify a truly absent
lease still starts a new sequence normally.

### P2: Release each conflict check's internal savepoint

Primary location:
[table.cc:699](sm/sm3/src/table/table.cc#L699).
Savepoint creation:
[table.cc:679](sm/sm3/src/table/table.cc#L679).

`find_now()` creates a savepoint to release the locks used
by its conflict check. It rolls back to that savepoint, but
never calls `forget()` to remove it. Rollback deliberately
preserves its target mark. Each successful conflict check
therefore retains another nested savepoint.

Reproduction:

1. Insert a row covered by a unique index and commit.
2. Begin one READ COMMITTED transaction.
3. Call `tbl_insert_arbiter()` with the same key 257 times.

The first 256 calls return 1 for the existing row. Call 257
returns -1 because the savepoint stack is full. The caller
has not created any savepoints. Existing user savepoints
reduce the number of successful conflict checks further.

**Impact:** a bulk insert or repeated upsert can fail solely
because it encountered enough existing keys. Retained
marks also cause later writes to accumulate undo state
that the caller did not request.

**Suggested fix:** after successful rollback, forget the
internal mark before returning. Define cleanup for error
paths while preserving the original error and accounting
for errors that already aborted the transaction.

**Regression coverage:** repeat more than 256 conflicts in
one transaction, then confirm a caller savepoint still
works. Cover LSM and tree arbiters and both tuple-output
variants. The confirmed limit probe used an LSM arbiter.

## Scope and method

This was a correctness review of the current tree, not a
review of one commit. The working tree was clean at start.
No production source or existing test was changed.

The review read the stack overview, coding rules, and each
level's design overview. Source inspection covered sm0
resource handling, sm1 transactions and recovery, version
visibility, GC, locking, sequences and bulk loading, sm2
tuple and key codecs and relations, sm3 catalog changes,
index maintenance, tree scans, builders, and the C wrapper.
Inspection depth varied by module; this is not a proof that
unlisted paths are correct. Vendored PostgreSQL float
printing code was excluded.

Focused probes reused the existing sm3 test fixtures and
linked the storage libraries. Probe sources, binaries, and
logs are local artifacts under `bench/build/sm-review/`;
that directory is ignored by Git. The five probes assert
the expected correct behavior and fail on this revision.

| Artifact | Purpose |
| --- | --- |
| `probes.cc`, `probes.log` | Savepoint limit and malformed lease |
| `tree-probes.cc`, `tree-probes.log` | Rollback, abort, and arbiter race |
| `probe-build.log`, `tree-probe-build.log` | Probe compilation |
| `full-test.log` | Full debug suite output |

Run the focused binaries from the repository root:

```sh
export TMPDIR="$PWD/bench/build/sm-review/tmp"
bench/build/sm-review/probes --test-case='review:*'
bench/build/sm-review/tree-probes --test-case='review:*'
```

The baseline suite was launched with:

```sh
TMPDIR="$PWD/bench/build/sm-review/tmp" \
  DEBUG=1 make -C sm -j4 test \
  > bench/build/sm-review/full-test.log 2>&1
```

Validation results:

- Both probe programs compiled with `-O0 -g` and linked
  debug storage libraries.
- `probes`: two cases failed at the expected assertions;
  287 of 289 assertions passed.
- `tree-probes`: three cases failed at the expected
  assertions; 90 of 93 assertions passed.
- The baseline sm0 suite passed all 61 cases and all
  1,157,454 assertions.
- The full baseline run was interrupted during the sm1
  scenarios. No scenario difference had been reported.
  The sm1 unit and stress suites, sm2, sm3, and rel were
  not reached by that run. A full-suite pass is not claimed.
- ASan, TSan, and crash-recovery fault injection were not run.

## Fixes on wip-codex-fix

All four findings are addressed on this branch:

- Tree cursors retain their entry position across rollback,
  discard unread cached rows, and read those rows again.
  Commit, abort, and prepare close tree cursors permanently.
  Releasing pins alone still allows the cursor to continue.
- A tree arbiter reads and validates the candidate again
  after acquiring its row lock. A changed key, deletion, or
  predicate change restarts the lookup.
- Metadata reads distinguish missing values from errors.
  Sequence recovery fails on malformed or unreadable leases
  without issuing a value or changing the lease.
- A successful conflict check forgets its internal savepoint
  after rollback. Errors retain the existing contract that
  the caller aborts or rolls back the failed operation.

Permanent regression tests are in:

- `sm/sm1/test/test_seq.cc`: malformed leases and injected
  read errors, including unchanged outputs and lease state.
- `sm/sm3/test/test_arbiter.cc`: more than 256 conflicts,
  caller savepoints, both index layouts, and tuple outputs.
- `sm/sm3/test/test_tree.cc`: rollback of buffered inserts
  and updates; transaction end and a later begin; ordinary
  and index-only scans; key, deletion, and predicate races.
- `dg/test/slt/on_conflict_many.test`: 300 SQL conflicts,
  DO NOTHING, DO UPDATE, and rollback to a caller savepoint.

The SQL regression fails on the original digres binary with
the savepoint-limit error. Its expected results pass on
PostgreSQL 18. The sequence and sm3 regressions pass on the
fixed debug libraries.

The rel driver has no arbiter verb and no persistent cursor
over tree rows. Its index cursor exposes entries instead.
The cache and arbiter cases therefore use sm3's direct API.
The lease fault injection requires sm1's internal metadata
access. No isolation scenario was changed: these fixes do
not change sm1's visibility or lock compatibility rules.

Post-fix validation logs are under `bench/build/codex-fix/`.
The full digres debug target passed its statement, initdb,
server, and SQL tests: 65 SQL files, 11,073 records, with
19 existing skips. The target's pgregress step could not find
clients under the source tree's `install/bin` directory.
Running the same runner with the installed PostgreSQL 18
clients passed all 231 files. No test expectation was changed.

The five new sm3 regression cases also passed under TSan:
1,756 assertions, with no sanitizer report. A broader TSan
run over all tree cases was stopped because of its runtime;
it is not reported as a pass. The full debug tree run passed
all 41 cases before the full-suite run.

The full `DEBUG=1 make -C sm -j4 test` run passed, including
all 124 isolation scripts in both storage layouts, every
level's unit tests, sm1's stress tests, and all rel tests.
Formatting and `git diff --check` also passed.
