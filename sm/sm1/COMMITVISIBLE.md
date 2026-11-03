# COMMITVISIBLE — a commit returns once it is visible

Status: built (`../../dg/WORKNOTE.md` step 315).

## 1. What

```sql
-- session A                         -- session B, at once
create temp table uc (f1 int);       ...
insert into uc values (1), (2), (3); commit;   -- claims 99, syncing
select count(*) from uc;  -- 0, now and then; 3 is right
```

A commit publishes in `commit_ts` order (`SM1_DESIGN.md`
§4.3): `visible_ts` is one below the oldest commit still in
progress. A's INSERT commits at 100 while B's commit at 99
writes its record; A's `sm1::commit` returns, and `visible_ts`
is still 98. A's next statement takes its snapshot at 98, and
does not see the rows A just committed.

PostgreSQL makes a commit visible before COMMIT returns: any
snapshot taken after, in any session, sees it. digres's test
`select_cursor.sql`, eight sessions at once, failed so in 4
of 80 sessions, on main as on every branch; a SELECT right
after an INSERT counted 0 rows in 4 of 160
(`../../dg/WORKNOTE.md` step 315). Temporary tables only made
it likelier; a permanent table's commit is the same.

## 2. Design

- **`sm1::commit` waits for its commit to be visible**: after
  `sequencer.done(commit_ts)` (§8.2 step 5), it waits until
  `visible_ts >= commit_ts`, on a condition variable of the
  sequencer's mutex (`alloc_mu`), which `db_commit_done()`
  signals whenever it raises `visible_ts`.
- **It waits only behind older commits in progress**, each
  writing its status record; with group commit, the same
  sync. No lock is held while it waits: locks are released
  after it (§8.2 step 6), as now, so a waiter's own locks
  keep it from no one longer than the older commit's sync.
- **A failed record write or sync aborts the process**
  (§8.2): no older commit stays in the sequencer for a waiter
  to wait behind.
- **It is not cut short by an interrupt**: it is past the
  commit point, and bounded by an older commit's sync, which
  finishes or aborts the process. A cancel then would leave a
  committed transaction that its own next statement does not
  see.
- **COMMIT PREPARED** publishes through the same path, and
  waits alike. A read-only transaction publishes nothing, and
  does not wait.

## 3. Cases

| Case | What happens |
| --- | --- |
| no older commit in progress | `visible_ts` reaches commit_ts in `db_commit_done()`: no wait |
| an older commit syncing | waits for that sync, which group commit shares |
| an older commit's record write or sync fails | the process aborts (§8.2): no waiter returns |
| SERIALIZABLE validation fails after the claim | `done(commit_ts)` with nothing published, as now: it raises `visible_ts` for the waiters behind it |
| COMMIT PREPARED, a prepared transaction restored at open | the same publish, the same wait |
| read-only commit | no commit_ts: no wait |
| temporary-only transaction | the same as any commit |

## 4. Tests

- sm1 unit tests (`test/test_xct.cc`): A claims its
  commit_ts and stalls before its record
  (`test_before_commit`); B commits after: B's commit returns
  only once A's is done, and B's next transaction sees B's
  row. With the wait taken out, B's commit returns at once
  and its next transaction sees no row. A failed write of
  A's record ends B's wait; with no signal there, B waits
  for good.
- `mvcc`: an isolation scenario cannot stall a commit between
  its claim and its record; the unit test does.
- dg: `test/server.sh` runs eight sessions at once, each
  inserting a row and counting its table's rows 50 times:
  every count must be the rows inserted so far. With the wait
  taken out, 28 counts of 400 missed the session's own last
  row. `test/run.sh`'s `select_cursor.sql` in eight sessions,
  which failed so now and then, passes 160 runs of 160.
- Measured: Q8's commit rate before and after.
