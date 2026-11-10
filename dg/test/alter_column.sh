# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# Verify what ALTER COLUMN does that test/alter_column.sql cannot show in
# 8 sessions at once: SET NOT NULL at REPEATABLE READ checks a row
# committed after the transaction's snapshot (sm3::set_not_null reads at
# the latest view), and an identity column, which needs a sequence no
# temporary table has, keeps NOT NULL and takes no default.
# 1. Make a table with a row that holds a value.
# 2. A REPEATABLE READ transaction reads the table: its snapshot is taken.
# 3. Another session commits a row with a null.
# 4. The first transaction's SET NOT NULL must fail on that row.
# 5. DROP NOT NULL, SET DEFAULT and DROP DEFAULT of an identity column
#    fail with PostgreSQL's error, DROP DEFAULT with its hint.
# Sourced by server.sh, using its held-session helpers.
local_psql -v ON_ERROR_STOP=1 -c "
  create table nnr(v int);
  insert into nnr values (1);" > "$work/nnr.log" 2>&1 ||
  fail "not null at repeatable read: setup"
held_start nnr
held_send nnr "begin isolation level repeatable read;
  select count(*) from nnr; select 'reader ready';"
held_wait nnr 'reader ready' || fail "not null at repeatable read: read"
local_psql -v ON_ERROR_STOP=1 -c "insert into nnr values (null);" \
  >> "$work/nnr.log" 2>&1 || fail "not null at repeatable read: insert"
held_send nnr "alter table nnr alter column v set not null;
  select 'checked';"
held_send nnr "rollback; select 'reader done';"
held_wait nnr 'reader done' || fail "not null at repeatable read: end"
grep -q 'contains null values' "$work/nnr.out" ||
  fail "not null at repeatable read: missed a row committed since"
held_end nnr
local_psql -c 'drop table nnr' >> "$work/nnr.log" 2>&1

local_psql -c "create table nni(id int, g int generated always as identity)" \
  -c "alter table nni alter column g drop not null" \
  -c "alter table nni alter column g set default 1" \
  -c "alter table nni alter column g drop default" \
  -c "drop table nni" > "$work/nni.out" 2>&1
[ "$(grep -c 'column "g" of relation "nni" is an identity column' \
  "$work/nni.out")" = 3 ] || fail "identity column: ALTER COLUMN refused"
grep -q 'HINT:  Use ALTER TABLE ... ALTER COLUMN ... DROP IDENTITY instead.' \
  "$work/nni.out" || fail "identity column: DROP DEFAULT's hint"
