# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# Verify that a string setting one session sets does not change another's.
# The settings are tcb->guc fields, each session's own std::string. SHOW
# reads guc.c's copy of a value; a table name is found by the field.
# 1. Make schemas guc_a_s and guc_b_s, each with a table guc_t of one row
#    that names its schema.
# 2. Sessions a and b set application_name, search_path, DateStyle and
#    TimeZone, each to its own values, before either reads guc_t.
# 3. Each session shows its own values, and reads its own schema's guc_t.
# 4. Session b sets them again; session a still shows its values.
# 5. Session a sets application_name with SET LOCAL in a transaction;
#    session b still shows its value.
# 6. Session a rolls back, and shows its value from before the transaction.
# 7. A placeholder session a sets is unknown in session b.
# 8. A new session shows the defaults.
# Sourced by server.sh, using its held-session helpers.

# Print a select that shows the four settings, and the schema whose guc_t
# the session reads, as one line, after TAG.
guc_show() {
  echo "select '$1:' || current_setting('application_name') || '|' ||
    current_setting('search_path') || '|' || current_setting('DateStyle') ||
    '|' || current_setting('TimeZone') || '|' || (select v from guc_t);"
}

a_vals='guc_a|guc_a_s, public|SQL, DMY|Europe/Paris|a'
b_vals='guc_b|guc_b_s|German, DMY|UTC|b'
b_vals2='guc_b2|guc_b2_s, guc_b_s|Postgres, MDY|Asia/Tokyo|b'

# Step 1: a table of each schema.
local_psql -v ON_ERROR_STOP=1 -c "
  create schema guc_a_s; create table guc_a_s.guc_t(v text);
  insert into guc_a_s.guc_t values ('a');
  create schema guc_b_s; create table guc_b_s.guc_t(v text);
  insert into guc_b_s.guc_t values ('b');" > "$work/guc_local.log" 2>&1 ||
  fail "guc local: setup"
held_start guc_a
held_start guc_b
# Step 2: each session sets its own values, before either reads guc_t.
held_send guc_a "set application_name = 'guc_a';
  set search_path = guc_a_s, public; set DateStyle = 'SQL, DMY';
  set TimeZone = 'Europe/Paris';"
held_send guc_b "set application_name = 'guc_b'; set search_path = guc_b_s;
  set DateStyle = 'German'; set TimeZone = 'UTC';"
held_send guc_a "select 'a set';"
held_wait guc_a "a set" || fail "guc local: session a, its SETs"
held_send guc_b "select 'b set';"
held_wait guc_b "b set" || fail "guc local: session b, its SETs"
# Step 3: each shows its own values, and reads its own guc_t.
held_send guc_a "$(guc_show a1)"
held_wait guc_a "a1:$a_vals" || fail "guc local: session a, its values"
held_send guc_b "$(guc_show b1)"
held_wait guc_b "b1:$b_vals" || fail "guc local: session b, its values"
# Step 4: b sets them again; a keeps its own.
held_send guc_b "set application_name = 'guc_b2';
  set search_path = guc_b2_s, guc_b_s; set DateStyle = 'Postgres, MDY';
  set TimeZone = 'Asia/Tokyo';"
held_send guc_b "$(guc_show b2)"
held_wait guc_b "b2:$b_vals2" || fail "guc local: session b, its new values"
held_send guc_a "$(guc_show a2)"
held_wait guc_a "a2:$a_vals" ||
  fail "guc local: session b's new values reached session a"
# Step 5: SET LOCAL in a's transaction does not reach b.
held_send guc_a "begin; set local application_name = 'guc_a_local';"
held_send guc_a "$(guc_show a3)"
held_wait guc_a "a3:guc_a_local|guc_a_s, public|SQL, DMY|Europe/Paris|a" ||
  fail "guc local: session a, its SET LOCAL value"
held_send guc_b "$(guc_show b3)"
held_wait guc_b "b3:$b_vals2" ||
  fail "guc local: session a's SET LOCAL reached session b"
# Step 6: the rollback restores a's value.
held_send guc_a "rollback;"
held_send guc_a "$(guc_show a4)"
held_wait guc_a "a4:$a_vals" ||
  fail "guc local: session a, its value after the rollback"
# Step 7: a's placeholder is its own.
held_send guc_a "set guc_local.x = 'a'; select 'a ph:' || current_setting('guc_local.x');"
held_wait guc_a "a ph:a" || fail "guc local: session a, its placeholder"
held_send guc_b "select 'b ph:' || coalesce(current_setting('guc_local.x', true), 'none');"
held_wait guc_b "b ph:none" ||
  fail "guc local: session a's placeholder reached session b"
held_end guc_a
held_end guc_b
! grep -q ERROR "$work/guc_a.out" "$work/guc_b.out" ||
  fail "guc local: a SET failed"
# Step 8: a new session has the defaults.
[ "$(local_psql -c "select current_setting('search_path') || '|' ||
    current_setting('DateStyle')")" = '"$user", public|ISO, MDY' ] ||
  fail "guc local: a new session does not have the defaults"
local_psql -c 'drop schema guc_a_s, guc_b_s cascade' >> "$work/guc_local.log" 2>&1
