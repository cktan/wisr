# Review of the past 18 hours

The window starts at 2026-11-07 01:46:08 UTC and ends at
19:46:08 UTC. The target is `dac3c73d^..268fb234`.
The window contains 22 commits, including merge commits.

The review covers the modified code in aa, sm and dg, and the
benchmark scripts. It excludes unchanged PostgreSQL code.
The report was written before any source fix.

## P1: A bulk B-tree load omits SSI write notifications

Location: `sm/sm2/src/rel/index.cc`, lines 724 to 735.
Commit: `268fb234`.

Status: fixed on this branch.

The new tree path calls `load_put()` on a drained stream.
That stream deliberately skips `ssi_on_write()` in sm1.
The old path called `idx_insert()`, which reports the tree
entry through `ssi_write_key()`. The heap write cannot replace
that report: an index scan registers a different key range.

The exclusive table lock does not remove a committed reader's
SIREAD range while another transaction overlaps that reader.
This schedule commits a serialization cycle:

1. Start transaction B at SERIALIZABLE.
2. Read empty table Y in B.
3. Start transaction A at SERIALIZABLE.
4. Scan empty X's tree in A.
5. Insert into Y in A.
6. Commit A.
7. Bulk load a row into X in B under its exclusive table lock.
8. Commit B. Both transactions commit despite the cycle.

The control uses an ordinary insert in B. Its commit fails
with `SM_ERR_SERIALIZATION`. The bulk load's commit succeeds.

Report each complete tree entry key through `ssi_write_key()`
before a unique load removes the row key for sorting. Add the
schedule to the sm3 and rel tests. The sm1 isolation driver
cannot express a tree index. A SQL COPY test can cover dg.

Artifacts:

- `bench/build/review-18h/ssi_probe.cc`: the standalone
  reproduction and control.
- `bench/build/review-18h/ssi-insert.log`: the ordinary insert
  fails at commit.
- `bench/build/review-18h/ssi-bulk.log`: the bulk load commits.

## P1: A nested settle can let an owner change restore a revoke

Location: `aa/src/aa.cc`, lines 542 to 545.
Commit: `dac3c73d`.

Status: fixed on this branch.

`aa_object_settled()` replaces `settled_epoch` on every call.
The new conflict check therefore loses the epoch of an earlier
ACL read if a nested setter settles before the first change.

An existing caller has this exact nesting:
`pg_global_change_owner()` settles and reads the old ACL, then
calls `pg_global_set_owner()`, which settles again. It finally
passes the saved ACL through `aclnewowner()` to the ACL setter.
`ALTER DATABASE ... OWNER TO` reaches this path without a
table lock that excludes a concurrent database ACL change.

The following call sequence reproduces the lost revoke:

1. Read an ACL that grants CONNECT to PUBLIC in session A.
2. Revoke that grant in session B.
3. Commit B.
4. Repeat the settle inside A's owner setter.
5. Set the owner in A.
6. Apply the saved ACL with the new owner in A.
7. Commit A. The commit succeeds and restores PUBLIC CONNECT.

The control omits the nested settle. Its commit conflicts and
the grant stays revoked. This is an incomplete fix for the
previously reported lost ACL update, rather than a new grant
path introduced by the commit.

Keep the epoch of the ACL read through the complete owner
change. A nested setter must not replace that epoch before
the change is recorded. Add the nesting to the aa conflict
tests and cover the database owner path in dg.

Artifacts:

- `bench/build/review-18h/aa_probe.cc`: the standalone
  reproduction and control.
- `bench/build/review-18h/aa-single.log`: the owner change
  conflicts.
- `bench/build/review-18h/aa-nested.log`: the commit succeeds and
  restores CONNECT.

## Implemented fixes

The bulk load now reports every full tree entry key to SSI
before it shortens a unique key for sorting. Tests cover sm2,
sm3, rel, and SQL COPY. The sm1 isolation driver cannot express
a tree index. AA and sq do not implement this path.

The owner change now records its rewritten ACL before the
nested owner setter settles again. AA retains the entry's
first change epoch. This avoids a global change to AA's epoch
rules for later statements. Tests cover AA's epoch retention
and a database owner change beside a revoke in dg. The storage
levels and rel do not implement owners or ACLs.

## Validation

- Debug builds of sm and dg completed.
- AA: all 44 tests passed, with 659 assertions.
- sm0: all 85 tests passed, with 3,297,655 assertions.
- All 124 isolation scripts matched in both store formats.
- dg SQL tests: all 13 files passed, alone and in eight
  concurrent sessions.
- All 124 parser test files passed.
- The dg initialization and server integration tests passed.
- All 65 logic test files passed: 11,073 records and 19 skips.
- All 231 ported PostgreSQL regression files passed.
- The full sm suite passed at every level, including all rel
  tests. sm1 passed 352 unit tests and 5 stress tests. sm2
  passed 150 tests. sm3 passed 269 tests.

The probes use debug libraries. Their data and logs are under
`bench/build/review-18h`, with TMPDIR in that directory.
