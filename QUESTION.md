# Questions

Decisions taken while the user was away, each with the choice
made. Branch `wip-check`. Say if one should change.

## ALTER COLUMN SET/DROP NOT NULL and SET/DROP DEFAULT

Example:

```sql
create table job (id int8 primary key, state text,
                  mtime timestamptz default current_timestamp on update);
alter table job alter column state set not null;
alter table job alter column state drop not null;
alter table job alter column state set default 'free';
alter table job alter column mtime drop default;
```

1. **DROP NOT NULL of a primary key column is refused**, with
   PostgreSQL's error, `column "id" is in a primary key`. sm3
   refuses it too (`SM_ERR_INUSE`): a primary key's columns are
   NOT NULL in sm3's catalog (`create_index`). Recommended, as
   PostgreSQL does it.
2. **DROP NOT NULL, SET DEFAULT and DROP DEFAULT of an identity
   column are refused**, with PostgreSQL's error, `column "g" of
   relation "t" is an identity column`. sm3 refuses them too
   (`SM_ERR_INVALID`): its identity column is NOT NULL with no
   default (`check_columns`). Recommended, as PostgreSQL does it.
3. **DROP DEFAULT of a column with an update default** leaves the
   update default as the insert default:
   [`dg/ONUPDATE.md`](dg/ONUPDATE.md#2-what-it-means) §2 says
   the insert default is the plain `DEFAULT`, or, without one,
   the update default. So `mtime` above takes
   `CURRENT_TIMESTAMP` on INSERT still. The other choice, no
   insert default at all, breaks that rule. Recommended.
4. **SET NOT NULL reads every row at sm's latest view**, under
   the table's lock X (`sm3::set_not_null`, already built for
   ADD COLUMN). A row committed after a REPEATABLE READ
   transaction's snapshot is checked too.
5. **No `pg_constraint` row of type `n`**: PostgreSQL 18 names a
   NOT NULL constraint and lists it in `pg_constraint`; digres
   lists none, before or after this change. `ADD CONSTRAINT ...
   NOT NULL` stays refused.
6. **A row's missing value does not change**: a row written
   before an ADD COLUMN keeps the value the column had then, as
   PostgreSQL's `attmissingval` does.
7. **SET DEFAULT of a column with an update default** takes only
   what a default beside `ON UPDATE` takes, a constant or one of
   `CURRENT_TIMESTAMP`, `LOCALTIMESTAMP` and `CURRENT_DATE`
   ([`dg/ONUPDATE.md`](dg/ONUPDATE.md#2-what-it-means) §2), as
   CREATE TABLE does. Recommended: the rule stays one rule.

## NOT NULL constraints by name

[`sm/NOTNULL.md`](sm/NOTNULL.md): PostgreSQL 18 lists each NOT
NULL column as a constraint in `pg_constraint`; digres lists
none, so `CREATE TABLE ... (LIKE t INCLUDING ALL)` fails on any
table with a NOT NULL column. This replaces decision 5 above.

1. **sm3 keeps an oid and a name per NOT NULL column.** The other
   choice, rows made up in `catrel.c` with no oid of sm's, is
   less code, but a made-up oid could be another object's, and a
   name the user gives, `CONSTRAINT note_set NOT NULL`, would be
   lost. Recommended.
2. **`NOT VALID` of a NOT NULL constraint is refused; `NO
   INHERIT` is accepted and changes nothing**, as digres accepts
   it today. Recommended.

## Generated columns

[`sm/GENERATED.md`](sm/GENERATED.md): STORED and VIRTUAL, the
host computing the values with PostgreSQL's own code, sm3
keeping the kind, the expression and the columns it reads.

1. **Both kinds**, as VIRTUAL is PostgreSQL 18's default.
   Recommended.
2. **ALTER COLUMN TYPE of a generated column is refused**, as for
   a check: sm's rewrite cannot run the host's expression.
   PostgreSQL computes the column again. Recommended.
3. **SET EXPRESSION, DROP EXPRESSION and ADD COLUMN ... GENERATED
   are refused** for now: each would compute every row's value.
4. **NOT NULL on a virtual column is refused**: sm would see a
   null in its place. PostgreSQL 18 takes it. Recommended, until
   NOT NULL can be the host's check for such a column.
