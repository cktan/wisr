# Verify that a check added or validated at REPEATABLE READ checks a row
# committed after the transaction's snapshot (../sm/CHECK.md §5.2).
# 1. Make a table with a row that holds the check to come.
# 2. A REPEATABLE READ transaction reads the table: its snapshot is taken.
# 3. Another session commits a row that breaks the check.
# 4. The first transaction's ADD CONSTRAINT must fail on that row.
# 5. Again, the first transaction adding it NOT VALID, then VALIDATE
#    CONSTRAINT: the VALIDATE fails. NOT VALID checks a new row, so the
#    row is committed before the check is added.
# Sourced by server.sh, using its held-session helpers.
for how in add validate; do
  local_psql -v ON_ERROR_STOP=1 -c "
    create table crv(v int);
    insert into crv values (1);" > "$work/crv-$how.log" 2>&1 ||
    fail "check at repeatable read: $how setup"
  held_start "crv_$how"
  held_send "crv_$how" "begin isolation level repeatable read;
    select count(*) from crv; select 'reader ready';"
  held_wait "crv_$how" 'reader ready' ||
    fail "check at repeatable read: $how read"
  local_psql -v ON_ERROR_STOP=1 -c "insert into crv values (-1);" \
    >> "$work/crv-$how.log" 2>&1 ||
    fail "check at repeatable read: $how insert"
  if [ "$how" = validate ]; then
    held_send "crv_$how" "alter table crv add constraint crv_pos
      check (v > 0) not valid;
      alter table crv validate constraint crv_pos; select 'checked';"
  else
    held_send "crv_$how" "alter table crv add constraint crv_pos
      check (v > 0); select 'checked';"
  fi
  held_send "crv_$how" "rollback; select 'reader done';"
  held_wait "crv_$how" 'reader done' ||
    fail "check at repeatable read: $how end"
  grep -q 'is violated by some row' "$work/crv_$how.out" ||
    fail "check at repeatable read: $how missed a row committed since"
  held_end "crv_$how"
  local_psql -c 'drop table crv' >> "$work/crv-$how.log" 2>&1
done
