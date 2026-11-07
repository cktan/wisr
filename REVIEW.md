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

The new tree path calls `load_put()` on a drained stream.
That stream deliberately skips `ssi_on_write()` in sm1.
The old path called `idx_insert()`, which reports the tree
entry through `ssi_write_key()`. The heap write cannot replace
that report: an index scan registers a different key range.

The exclusive table lock does not remove a committed reader's
SIREAD range while another transaction overlaps that reader.
This schedule commits a serialization cycle:

1. Start transaction B at SERIALIZABLE and read empty table Y.
2. Start transaction A at SERIALIZABLE and scan empty X's tree.
3. Insert into Y in A, then commit A.
4. Acquire X's exclusive lock in B and bulk load a row into X.
5. Commit B. Both transactions commit despite the cycle.

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
2. Revoke that grant in session B and commit B.
3. Repeat the settle inside A's owner setter.
4. Set the owner, then apply the saved ACL with the new owner.
5. Commit A. The commit succeeds and restores PUBLIC CONNECT.

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

## Validation

- Debug builds of sm and dg completed.
- AA: all 43 tests passed, with 643 assertions.
- sm0: all 85 tests passed, with 3,297,655 assertions.
- All 124 isolation scripts matched in both store formats.
- dg SQL tests: all 13 files passed, alone and in eight
  concurrent sessions.
- The dg server integration tests passed.
- The full sm suite was started. Its final status is reported
  in the review response.

The probes use debug libraries. Their data and logs are under
`bench/build/review-18h`, with TMPDIR in that directory.
