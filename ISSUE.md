# Issues

Open questions found in the reviews of 2026-11-04, kept for a
decision. Each states what the code does now, why it is a
question, and the choices.

## 1. An md5 hash is taken as a password over TCP

`CREATE ROLE` and `ALTER ROLE ... PASSWORD` over TCP without TLS
refuse a password in cleartext (`aa/ROLE.md` §5.1, and
`check_cleartext_password()` in
`dg/src/backend/commands/user.c`). They take any password that
is not cleartext, and an md5 hash is not cleartext, so it is
taken.

- A role that holds an md5 hash cannot log in: a login is by
  SCRAM alone (`aa/ROLE.md` §5).
- The hash still crossed the network in the clear. An md5 hash
  of a password and a role name is weak: a guess at the password
  can be checked against it offline.

Choices:

- **Keep it**, as now. The docs say an md5 hash is taken and no
  login accepts it.
- **Refuse an md5 hash**, over TCP or everywhere, since no login
  can use it.

## 2. A password in cleartext can still be seen

The log masks each string after the keyword `PASSWORD`
(`query_for_log()` in `dg/src/backend/utils/error/elog.c`). Two
places still show a password:

- `pg_stat_activity.query` shows the statement's text while it
  runs, to its own role and to a role with `pg_read_all_stats`,
  as PostgreSQL does.
- A statement whose keyword is misspelled, such as
  `ALTER ROLE r PASWORD 'x'`, is a syntax error. Its
  `STATEMENT:` line is logged as it is: no keyword `PASSWORD`
  precedes the string, so nothing is masked.

Choices:

- **Keep both**, as PostgreSQL does.
- **Mask the activity's text too**, with `query_for_log()` where
  the backend reports its query.
- **Mask more of a statement that failed to parse**: every
  string of it, for example.

## 3. sm/CODE.md and sm/DESIGN.md disagree on std::bad_alloc

- `sm/CODE.md` §5: "Do not catch `std::bad_alloc`, and do not
  report a failed allocation as `SM_ERR_NOMEM`."
- `sm/DESIGN.md` §5: the C wrapper is the one catch-all, and
  `std::bad_alloc` becomes `NOMEM` there. It is possible there
  only before `sm1::attach` installs the new-handler, or under a
  host's own handler.

The code does what `DESIGN.md` says (`guard()` in
`sm/src/sm.cc`, and `sm.h`'s conventions).

Choices:

- **Name the exception in `CODE.md`**: its rule is for sm0 to
  sm3, and the C wrapper maps `bad_alloc` to `NOMEM`.
- **Drop the mapping**: a `bad_alloc` in the wrapper becomes
  `SM_ERR_EXCEPTION`, as every other exception does, and
  `NOMEM` leaves `sm.h`.

## 4. A decimal key refuses a datum at another scale

`decimal_key` in `sm/sm2/src/type/decimal.cc` now checks a
datum's scale bits against its column's typmod, as
`decimal_out` does, and fails with `SM_ERR_INVALID` on a
mismatch. Before, it made a key that sorted in the wrong place
among the column's keys.

- dg's suites pass with the check.
- A path that encodes a key from a value at another scale, such
  as a scan bound from a constant of another `numeric(p, s)`,
  now fails where it gave a wrong order before. None is known.

Choices:

- **Keep the check**, and fix a caller that fails, by casting
  the value to the column's scale first.
- **Rescale in `decimal_key`**: a key at the column's scale,
  where the value fits it exactly; refuse it otherwise.
