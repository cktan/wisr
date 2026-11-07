# Verify a database owner change cannot restore a concurrent ACL revoke.
# 1. Make a database with an explicit CONNECT grant and two roles.
# 2. Hold an owner change in one session.
# 3. Commit a revoke in another session.
# 4. Require the owner change to conflict at commit.
# 5. Confirm that CONNECT stays revoked.
# 6. Retry the owner change.
# 7. Confirm that CONNECT stays revoked after the retry.
# Sourced by server.sh, using its held-session helpers.
local_psql -v ON_ERROR_STOP=1 -c 'create role aoc_owner' \
  -c 'create role aoc_reader' -c 'create database aoc_db' \
  -c 'revoke all on database aoc_db from public' \
  -c 'grant connect on database aoc_db to aoc_reader' \
  > "$work/aoc.log" 2>&1 || fail "owner ACL: setup"
held_start aoc
held_send aoc "begin;
  alter database aoc_db owner to aoc_owner;
  select 'owner pending';"
held_wait aoc 'owner pending' || fail "owner ACL: pending change"
local_psql -v ON_ERROR_STOP=1 \
  -c 'revoke connect on database aoc_db from aoc_reader' \
  >> "$work/aoc.log" 2>&1 || fail "owner ACL: revoke"
held_send aoc "commit; select 'owner done';"
held_wait aoc 'owner done' || fail "owner ACL: commit"
grep -q 'could not serialize access' "$work/aoc.out" ||
  fail "owner ACL: stale owner change committed"
held_end aoc
[ "$(local_psql -c \
  "select has_database_privilege('aoc_reader','aoc_db','connect')")" = f ] ||
  fail "owner ACL: CONNECT restored"
local_psql -v ON_ERROR_STOP=1 \
  -c 'alter database aoc_db owner to aoc_owner' \
  >> "$work/aoc.log" 2>&1 || fail "owner ACL: retry"
[ "$(local_psql -c \
  "select has_database_privilege('aoc_reader','aoc_db','connect')")" = f ] ||
  fail "owner ACL: CONNECT restored by retry"
local_psql -v ON_ERROR_STOP=1 -c 'drop database aoc_db' \
  -c 'drop role aoc_owner' -c 'drop role aoc_reader' \
  >> "$work/aoc.log" 2>&1 || fail "owner ACL: cleanup"
