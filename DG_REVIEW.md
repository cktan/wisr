# dg code review

Reviewed commit: `bc43915cbe8f20dc38d2f9655bb08d956233cd55`.

Two confirmed findings. Both can commit incorrect database state.
The review did not change production code.

## P1: Foreign-key validation misses committed orphan rows

Primary location:
[`check_fkey_rows()`](dg/src/backend/access/sm/smstore.c#L5240).
Related locations:
[`validate_constraint()`](dg/src/backend/access/sm/smstore.c#L5333)
and
[`GetLatestSnapshot()`](dg/src/backend/utils/time/snapmgr.c#L109).

A repeatable-read transaction can add a validated foreign key
while the table contains an orphan row. The row must have
committed after the transaction established its snapshot.
`ADD CONSTRAINT ... NOT VALID` followed by `VALIDATE CONSTRAINT`
has the same defect.

`check_fkey_rows()` delegates validation to `RI_Initial_Check()`.
That function requests `GetLatestSnapshot()` for its SPI query.
The replacement snapshot manager returns the transaction
snapshot token. It does not change the storage manager's read
view. The validation query therefore scans the older view.
Locks prevent further changes but do not make existing,
later-committed rows visible.

For ordinary foreign-key creation, the adapter has already set
`def.validated` before this check. For explicit validation,
`validate_constraint()` sets the validated state after the
incomplete check succeeds. Both paths can commit the false
claim that all existing rows meet the constraint.

### Reproduction

Run the setup in either session:

```sql
CREATE TABLE parent (id int PRIMARY KEY);
CREATE TABLE child (id int PRIMARY KEY, pid int);
INSERT INTO parent VALUES (1);
INSERT INTO child VALUES (1, 1);
```

Session A:

```sql
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT * FROM child;
```

Session B:

```sql
INSERT INTO child VALUES (2, 999);
```

Session A:

```sql
ALTER TABLE child ADD CONSTRAINT fk
  FOREIGN KEY (pid) REFERENCES parent(id);
COMMIT;
```

Digres commits. The orphan row remains, and `pg_constraint`
reports `convalidated = true`. PostgreSQL 18.6 rejects the
`ALTER TABLE` with a foreign-key violation for `pid = 999`.

The explicit validation variant also reproduced. Replace the
last `ALTER TABLE` with these statements:

```sql
ALTER TABLE child ADD CONSTRAINT fk
  FOREIGN KEY (pid) REFERENCES parent(id) NOT VALID;
ALTER TABLE child VALIDATE CONSTRAINT fk;
```

### Suggested correction and tests

Give the validation query a current storage read view while
holding the required locks on both tables. Restore the prior
view on success and error. Do not change ordinary
repeatable-read queries to use the current view.

Add two-session tests for both creation and validation. Include
an orphan inserted after the initial snapshot and a valid row
whose parent also committed after that snapshot. Check the
catalog state after a rejected validation.

Evidence: `bench/build/dg-review/fkey.log` and
`bench/build/dg-review/variants.log`.

## P1: Volatile ADD COLUMN defaults miss committed rows

Primary location:
[`fill_column()`](dg/src/backend/access/sm/smstore.c#L3974).
Caller:
[`add_column()`](dg/src/backend/access/sm/smstore.c#L4124).

Adding a column with a volatile default can commit `NULL`
values in rows that should receive the default. This occurs
when another session inserts and commits rows after the DDL
transaction establishes its repeatable-read snapshot.

`add_column()` creates the column, then calls `fill_column()`
to evaluate the default once for each existing row. Its call
to `sm_tbl_scan_rows()` uses the transaction's existing view.
The exclusive DDL lock does not advance that view. Rows that
committed after the snapshot never enter the filling loop.
They retain the new column's missing value, which is `NULL`.

When the column also has `NOT NULL`, the later null check sees
the omitted rows and rejects a valid schema change instead.
This variant was also reproduced. Serial and identity column
additions use the same filling path.

### Reproduction

Setup:

```sql
CREATE TABLE t (id int PRIMARY KEY);
INSERT INTO t VALUES (1);
CREATE SEQUENCE s;
```

Session A:

```sql
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT * FROM t;
```

Session B:

```sql
INSERT INTO t VALUES (2);
```

Session A:

```sql
ALTER TABLE t ADD COLUMN v bigint DEFAULT nextval('s');
COMMIT;
SELECT * FROM t ORDER BY id;
```

Digres returns `(1, 1)` and `(2, NULL)`. PostgreSQL 18.6 returns
`(1, 1)` and `(2, 2)`.

Adding `NOT NULL` to the column definition makes digres raise
`column "v" of relation "t" contains null values`.
PostgreSQL completes that schema change successfully.

### Suggested correction and tests

Fill all current rows under the exclusive DDL lock. Use the
storage manager's current-view mechanism for the full filling
operation, including its reads and writes. Restore the prior
view on every exit path. Starting another statement alone does
not advance a repeatable-read snapshot.

Add two-session tests for nullable and non-null volatile
defaults. Cover an identity column and a row updated after the
initial snapshot. Assert the final stored values after commit.

Evidence: `bench/build/dg-review/probe.log` and
`bench/build/dg-review/variants.log`.

## Review scope and validation

The review focused on the fork's storage adapter, DDL,
transaction and savepoint handling, index bounds and batched
reads, row locking, session lifecycle, and advisory locks.
The review used the design documents and local PostgreSQL
18.6 sources to distinguish fork changes from upstream code.
Unmodified PostgreSQL code was outside the review scope.

The debug build passed with `DEBUG=1 make -C dg -j4`.
Focused SQL probes ran against that build and PostgreSQL 18.6.
The build log and probe scripts are under
`bench/build/dg-review/`. This directory is ignored by Git.

The findings above are confirmed behavior differences, not
inferred failures. A separate cursor rollback probe did not
establish an additional defect and is not a finding.

This was a manual review, not an exhaustive audit of every
file in the fork. The full regression suite, sanitizers,
crash recovery tests, and stress tests were not run for this
review. Documented feature gaps, including backward scans,
parallel queries, and weaker foreign-key lock modes, are not
reported as defects here.
