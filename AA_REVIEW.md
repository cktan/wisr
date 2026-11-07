# aa code review

Reviewed commit: `062e0d6332dab049d3dfd20c20308795cabfd03a`.

Two confirmed P1 findings. Both can leave a role holding
privileges that a committed transaction revoked.
This review changes no production code.

## P1: Overlapping ACL changes can restore a committed revoke

Primary location:
[`aa_post_commit()`](aa/src/aa.cc#L707).
Related locations:
[`aa_object_settled()`](aa/src/aa.cc#L520),
[`aa_set_object()`](aa/src/aa.cc#L214), and
[`intermediates()`](aa/src/aa.cc#L414).

The commit mutex orders writes, but does not protect the read
that produced a pending ACL. Two transactions can both read the
same ACL before either starts its commit. The second commit
then replaces the first transaction's changes with its stale
copy. An unrelated GRANT can therefore restore a revoked right.

`aa_object_settled()` takes and releases the mutex immediately.
`aa_set_object()` saves the complete replacement entry without
its original value or version. At commit, `intermediates()`
compares that replacement with the latest shared entry. It
does not reject a change based on an older entry.
`aa_post_commit()` then writes the replacement unconditionally.
The role-write path has conflict detection; this path does not.

### SQL reproduction

Run each setup statement separately as the administrator:

```sql
CREATE ROLE review_reader;
CREATE ROLE review_writer;
CREATE TABLE review_t (id int);
GRANT SELECT ON review_t TO review_reader;
```

Session A:

```sql
BEGIN;
REVOKE SELECT ON review_t FROM review_reader;
```

Session B, before A commits:

```sql
BEGIN;
GRANT UPDATE ON review_t TO review_writer;
```

Then commit A, followed by B:

```sql
-- Session A
COMMIT;
-- Session B
COMMIT;
```

Check from another transaction:

```sql
SELECT has_table_privilege(
  'review_reader', 'review_t', 'SELECT');
```

Digres returns `true`. Both transactions report success, but B
restores the SELECT privilege that A revoked.

In the PostgreSQL 18.6 comparison, B's GRANT waits for A. After
A commits, B fails with `tuple concurrently updated`. The final
privilege check returns `false`.

An API-level probe confirms the same defect even when both
sessions call `aa_object_settled()` before reading their entry.
The reader's mask becomes zero after the revoke. After the
other transaction commits its stale ACL, SELECT and UPDATE
return. The second `aa_commit()` reports success.

### Suggested correction and regression coverage

Record the base entry or its version when a transaction first
changes an object. Under the commit mutex, reject a stale
replacement before writing any part of that transaction.
Alternatively, serialize changes from the initial read through
the final commit. Holding the global mutex for that whole
interval would block unrelated objects and long transactions.

Add two-session tests in aa and digres. Cover overlapping
GRANT/GRANT and REVOKE/GRANT operations whose statements both
finish before either commit starts. Include owner changes and
multiple objects in one transaction. Require either a conflict
or preservation of both changes; never restore a revoked right.

Evidence: `bench/build/aa-review/sql-probe.log` and
`bench/build/aa-review/probe.log`.

## P1: A failed final write leaves stale privileges live

Primary location:
[`aa_post_commit()`](aa/src/aa.cc#L718).
Related locations:
[`aa_commit()`](aa/src/aa.cc#L595),
[`write_objects()`](aa/src/aa.cc#L454), and
[`aa_object_mask()`](aa/src/aa_authz.cc#L365).

The first commit writes a restrictive intermediate ACL to
`pg_global`. It leaves the shared objects snapshot unchanged.
If the database commits but `aa_post_commit()` fails, the live
snapshot still grants the old rights. A fresh load from disk
denies those rights. The server can thus keep authorizing a
privilege that the committed REVOKE removed on disk.

`aa_post_commit()` publishes an objects snapshot only when its
final write succeeds. On failure it clears the pending changes
and recovery state, then releases the commit mutex. It does not
publish the intermediate ACL or advance the objects epoch.
Existing readers keep their cached masks. New readers also see
the old shared snapshot, so clearing caches alone cannot fix it.

Digres logs a warning for this failure after the database has
committed. It continues serving queries. The failure therefore
must leave authorization safe while the process remains alive,
not only after a restart.

### Fault-injection reproduction

The probe uses a persisted table owned by role 16500. Its ACL
grants role 16501 SELECT and UPDATE.

1. Warm a reader session's `aa_object_mask()` cache.
2. Replace the writer's ACL with one granting SELECT alone.
3. Call `aa_commit(writer)` successfully.
4. Treat the database commit as successful.
5. Inject `SM_ERR_IO` into the next `sm_begin()` call.
6. Call `aa_post_commit(writer)` and confirm it returns `-1`.
7. Check privileges through the live environment and a fresh
   `aa_load()` of the same `pg_global`.

Observed masks, requesting SELECT and UPDATE:

| Check | Expected after failure | Observed |
| --- | --- | --- |
| Existing reader, cached | SELECT | SELECT and UPDATE |
| Live environment, uncached | SELECT | SELECT and UPDATE |
| Fresh environment loaded from disk | SELECT | SELECT |

The numeric masks are 6 for the live checks and 2 for the fresh
load. The injected error is reported as a failed transaction
begin in `pg_global`.

The probe compiles the original `aa.cc` with a wrapper around
its calls to `sm_begin()`. Only the selected call returns an
injected error. No production source was edited. This tests the
failure branch directly; it does not simulate a physical disk
failure or a process crash.

### Suggested correction and regression coverage

Keep the shared snapshot consistent with the restrictive state
already committed to disk. One option is to publish that state
before the database commits, then publish and invalidate again
when an abort restores the prior state. Another is to reconcile
and publish the persisted state on final-write failure before
allowing further authorization checks.

If safe reconciliation fails, fail closed rather than continue
with the old grants. Retain enough state for recovery or retry;
clearing the changes currently loses that information.

Add fault-injection cases for beginning, writing, and committing
the final transaction. Test REVOKE, owner transfer, and default
database CONNECT. Compare cached readers, uncached readers, and
reloaded state. Also test a later unrelated commit, so recovery
cannot depend on changing the affected object again.

Evidence: `bench/build/aa-review/probe.cc` and its saved
`probe.log`.

## Scope and validation

The review covered every aa implementation file and its public
and internal headers. It traced authentication decisions,
role closures, ACL evaluation, privilege caches, persistence,
commit ordering, savepoints, and temporary-object state.
The review used `AA_DESIGN.md`, `ROLE.md`, `USAGE.md`, and the
existing tests to identify intended behavior.

Integration inspection covered the aa calls in digres's
`pg_global.c` and privilege checks. Original PostgreSQL code
was outside the findings scope. The installed PostgreSQL 18.6
server supplied the SQL comparison result.

The baseline debug suite passed all 38 tests and 571 assertions:

```sh
TMPDIR="$PWD/bench/build/aa-review/tmp" DEBUG=1 make -C aa -j4 test
```

A debug digres build also passed. The two API probe cases fail
three safety assertions, confirming the findings above. The SQL
probe independently confirms the first finding through digres.
Probe sources, binaries, data directories, and logs are under
`bench/build/aa-review/`, which is ignored by Git.

The review did not run the full digres regression suite,
sanitizers, or a physical crash/failure campaign. It does not
claim that these are the only defects. Documented feature gaps
and authentication policy choices are not findings here.
