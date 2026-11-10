# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# Verify DDL checks and volatile defaults include later committed rows.
# 1. Establish an old snapshot, then commit rows from another session.
# 2. Reject foreign keys with orphan rows, including explicit validation.
# 3. Accept valid references and restore the old view after each check.
# 4. Fill nullable, non-null, and identity columns in all current rows.
# 5. Preserve later updates, indexes, and rollback of a failed rewrite.
# Sourced by server.sh, using its server and held-session helpers.
for level in 'repeatable read' 'serializable'; do
  tag=${level// /_}
  local_psql -v ON_ERROR_STOP=1 -c "
    create table dc_parent(id int primary key);
    create table dc_child(id int primary key, pid int);
    create table dc_witness(id int primary key);
    insert into dc_parent values(1);
    insert into dc_child values(1,1);" > "$work/dc-$tag.log" 2>&1 ||
    fail "current DDL setup at $level"
  held_start "dc_$tag"
  held_send "dc_$tag" "begin isolation level $level;
    select count(*) from dc_child; select 'snapshot';"
  held_wait "dc_$tag" snapshot || fail "current DDL snapshot at $level"
  local_psql -v ON_ERROR_STOP=1 -c "
    insert into dc_child values(2,999);
    insert into dc_witness values(1);" >> "$work/dc-$tag.log" 2>&1
  held_send "dc_$tag" "savepoint bad;
    alter table dc_child add constraint dc_fk
      foreign key(pid) references dc_parent(id);
    rollback to bad;
    select 'old view ' || count(*) from dc_witness;
    select 'rejected';"
  held_wait "dc_$tag" rejected || fail "foreign key rejection at $level"
  grep -q 'violates foreign key constraint "dc_fk"' \
    "$work/dc_$tag.out" || fail "orphan hidden at $level"
  grep -qx 'old view 0' "$work/dc_$tag.out" ||
    fail "view after failed foreign key at $level"
  held_send "dc_$tag" "alter table dc_child add constraint dc_fk
      foreign key(pid) references dc_parent(id) not valid;
    savepoint invalid;
    alter table dc_child validate constraint dc_fk;
    rollback to invalid;
    select 'still invalid ' || convalidated from pg_constraint
      where conname = 'dc_fk'; rollback; select 'validated';"
  held_wait "dc_$tag" validated || fail "explicit validation at $level"
  [ "$(grep -c 'violates foreign key constraint "dc_fk"' \
    "$work/dc_$tag.out")" = 2 ] || fail "validation missed orphan at $level"
  grep -qx 'still invalid false' "$work/dc_$tag.out" ||
    fail "validation state after rejection at $level"
  held_end "dc_$tag"

  local_psql -v ON_ERROR_STOP=1 -c 'delete from dc_child where id=2' \
    >> "$work/dc-$tag.log" 2>&1
  held_start "dgood_$tag"
  held_send "dgood_$tag" "begin isolation level $level;
    select count(*) from dc_child; select 'snapshot';"
  held_wait "dgood_$tag" snapshot || fail "valid FK snapshot at $level"
  local_psql -v ON_ERROR_STOP=1 -c '
    insert into dc_parent values(2);
    insert into dc_child values(2,2);
    insert into dc_witness values(2);' >> "$work/dc-$tag.log" 2>&1
  held_send "dgood_$tag" "alter table dc_child add constraint dc_fk
      foreign key(pid) references dc_parent(id);
    select 'old view ' || count(*) from dc_witness;
    commit; select 'done';"
  held_wait "dgood_$tag" done || fail "valid foreign key at $level"
  grep -q 'ERROR:' "$work/dgood_$tag.out" &&
    fail "current valid references at $level"
  grep -qx 'old view 1' "$work/dgood_$tag.out" ||
    fail "view after successful foreign key at $level"
  held_end "dgood_$tag"

  # A validation does not wait for a child writer in flight.
  # 1. Re-add the key NOT VALID, and hold a writer of dc_child open.
  # 2. Validate from another session with a lock_timeout: it ends.
  # 3. The writer commits, and the key reads validated.
  held_start "dw_$tag"
  local_psql -v ON_ERROR_STOP=1 -c "
    alter table dc_child drop constraint dc_fk;
    alter table dc_child add constraint dc_fk
      foreign key(pid) references dc_parent(id) not valid;" \
    >> "$work/dc-$tag.log" 2>&1 || fail "validation setup at $level"
  held_send "dw_$tag" "begin; insert into dc_child values(3,1);
    select 'writing';"
  held_wait "dw_$tag" writing || fail "child writer at $level"
  local_psql -v ON_ERROR_STOP=1 -c "set lock_timeout = '10s';
    alter table dc_child validate constraint dc_fk;" \
    >> "$work/dc-$tag.log" 2>&1 ||
    fail "validation waited for a child writer at $level"
  held_send "dw_$tag" "commit; select 'committed';"
  held_wait "dw_$tag" committed || fail "child writer commit at $level"
  held_end "dw_$tag"
  [ "$(local_psql -At -c "select convalidated from pg_constraint
      where conname = 'dc_fk'")" = t ] ||
    fail "key not validated beside a writer at $level"
  local_psql -c 'drop table dc_child, dc_parent, dc_witness' \
    >> "$work/dc-$tag.log" 2>&1

  for kind in nullable notnull identity; do
    local_psql -v ON_ERROR_STOP=1 -c "
      create table dc_fill(id int primary key, payload text);
      insert into dc_fill values(1,'old');
      create sequence dc_seq;
      create index dc_payload on dc_fill(payload);" \
      >> "$work/dc-$tag.log" 2>&1
    held_start "df_${tag}_$kind"
    held_send "df_${tag}_$kind" "begin isolation level $level;
      select count(*) from dc_fill; select 'snapshot';"
    held_wait "df_${tag}_$kind" snapshot || fail "fill snapshot at $level"
    local_psql -v ON_ERROR_STOP=1 -c "
      update dc_fill set payload='new' where id=1;
      insert into dc_fill values(2,'inserted');" \
      >> "$work/dc-$tag.log" 2>&1
    case $kind in
      nullable) column="bigint default nextval('dc_seq')" ;;
      notnull) column="bigint not null default nextval('dc_seq')" ;;
      identity) column='bigint generated always as identity' ;;
    esac
    held_send "df_${tag}_$kind" "alter table dc_fill add column v $column;
      commit; select 'done';"
    held_wait "df_${tag}_$kind" done || fail "fill $kind at $level"
    grep -q 'ERROR:' "$work/df_${tag}_$kind.out" &&
      fail "fill $kind error at $level"
    held_end "df_${tag}_$kind"
    [ "$(local_psql -c 'select id,payload,v from dc_fill order by id')" = \
      $'1|new|1\n2|inserted|2' ] || fail "fill $kind values at $level"
    [ "$(local_psql -c 'set enable_seqscan=off' \
      -c "select id from dc_fill where payload='inserted'" | tail -1)" = 2 ] ||
      fail "fill $kind index at $level"
    [ "$(local_psql -c "insert into dc_fill(id,payload)
      values(3,'default') returning v" | head -1)" = 3 ] ||
      fail "fill $kind keeps its default at $level"
    local_psql -c 'drop table dc_fill; drop sequence dc_seq' \
      >> "$work/dc-$tag.log" 2>&1
  done
done

# Verify a failing default rewrite leaves the table and indexes intact.
# 1. Fail on the second row after writing the first row to the new store.
# 2. Roll back to the savepoint, then read and write the original table.
local_psql -v ON_ERROR_STOP=1 -c "
  create table dc_error(id int primary key, payload text);
  insert into dc_error values(1,'one'),(2,'two');
  create index dc_error_payload on dc_error(payload);
  create sequence dc_error_seq;" > "$work/dc-error.log" 2>&1
held_start dc_error
held_send dc_error "begin; savepoint rewrite;
  alter table dc_error add column v bigint
    default 1 / (nextval('dc_error_seq') - 2);
  rollback to rewrite;
  set enable_seqscan=off;
  select 'kept ' || id from dc_error where payload='two';
  insert into dc_error values(3,'three'); commit; select 'done';"
held_wait dc_error done || fail "default rewrite rollback"
grep -q 'division by zero' "$work/dc_error.out" &&
  grep -qx 'kept 2' "$work/dc_error.out" || fail "failed default rewrite"
held_end dc_error
[ "$(local_psql -c 'select count(*) from dc_error')" = 3 ] ||
  fail "table after failed default rewrite"
local_psql -c 'drop table dc_error; drop sequence dc_error_seq' \
  >> "$work/dc-error.log" 2>&1
