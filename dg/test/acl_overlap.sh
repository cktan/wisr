# Verify that overlapping GRANT and REVOKE on one table never restore a
# right a committed transaction revoked.
# 1. Make a table that grants a role SELECT, and two more roles.
# 2. Hold two transactions: one revokes the SELECT, one grants UPDATE to
#    another role, from the ACL it read before the revoke committed.
# 3. Commit the revoke, then the grant: the grant fails to serialize.
# 4. Confirm the SELECT stays revoked and the UPDATE was not granted.
# 5. Run the grant again, after the revoke: it commits, SELECT stays gone.
# Sourced by server.sh, using its server and held-session helpers.
# Separate -c options: digres refuses a role change in a transaction block.
local_psql -v ON_ERROR_STOP=1 -c "create role ao_reader" \
  -c "create role ao_writer" -c "create table ao_t(id int)" \
  -c "grant select on ao_t to ao_reader" > "$work/ao.log" 2>&1 ||
  fail "overlapping ACL: setup"
held_start ao_a
held_start ao_b
held_send ao_a "begin; revoke select on ao_t from ao_reader; select 'a open';"
held_send ao_b "begin; grant update on ao_t to ao_writer; select 'b open';"
held_wait ao_a "a open" || fail "overlapping ACL: revoke open"
held_wait ao_b "b open" || fail "overlapping ACL: grant open"
held_send ao_a "commit; select 'a done';"
held_wait ao_a "a done" || fail "overlapping ACL: revoke commit"
held_send ao_b "commit; select 'b done';"
held_wait ao_b "b done" || fail "overlapping ACL: grant commit"
grep -q 'could not serialize access' "$work/ao_b.out" ||
  fail "overlapping ACL: the stale grant committed"
held_end ao_a
held_end ao_b
[ "$(local_psql -c "select has_table_privilege('ao_reader','ao_t','select')")" \
  = f ] || fail "overlapping ACL: the revoked SELECT came back"
[ "$(local_psql -c "select has_table_privilege('ao_writer','ao_t','update')")" \
  = f ] || fail "overlapping ACL: the failed grant took effect"
local_psql -v ON_ERROR_STOP=1 -c "grant update on ao_t to ao_writer" \
  >> "$work/ao.log" 2>&1 || fail "overlapping ACL: grant after the revoke"
[ "$(local_psql -c "select has_table_privilege('ao_reader','ao_t','select')")" \
  = f ] || fail "overlapping ACL: SELECT back after a later grant"
[ "$(local_psql -c "select has_table_privilege('ao_writer','ao_t','update')")" \
  = t ] || fail "overlapping ACL: the later grant is lost"
