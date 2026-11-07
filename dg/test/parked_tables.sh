# Verify a session's next transaction, which reuses the table handle of
# its last, sees every change made to the table since.
# 1. A held session reads and writes a table, in transactions of its own.
# 2. Another session makes an index: the held session's insert reaches it.
# 3. Another session truncates: the held session reads none, and its
#    insert reaches the new store.
# 4. Another session changes a column's type: the held session reads the
#    new type.
# 5. Another session drops the index: the held session still writes.
# 6. Another session drops the table and makes it again: the held session
#    reads the new one.
# 7. The held session's own truncate, rolled back, leaves the rows.
# Sourced by server.sh, using its server and held-session helpers.
local_psql -v ON_ERROR_STOP=1 -c "
  create table pk_t(id int primary key, v int, s text);
  insert into pk_t values(1,10,'30');" > "$work/pk.log" 2>&1 ||
  fail "parked tables setup"
held_start pk
held_send pk "select 'rows ' || count(*) from pk_t;
  insert into pk_t values(2,20,'40');
  select 'rows ' || count(*) from pk_t; select 'step1';"
held_wait pk step1 || fail "parked tables: first transactions"
grep -qx 'rows 2' "$work/pk.out" || fail "parked tables: write and read"

local_psql -v ON_ERROR_STOP=1 -c "create index pk_t_v on pk_t(v)" \
  >> "$work/pk.log" 2>&1 || fail "parked tables: create index"
held_send pk "insert into pk_t values(3,30,'50'); select 'step2';"
held_wait pk step2 || fail "parked tables: insert after an index"
[ "$(local_psql -c 'set enable_seqscan=off; set enable_bitmapscan=off;
  select id from pk_t where v=30' | tail -1)" = 3 ] ||
  fail "parked tables: the index misses a reused handle's row"

local_psql -v ON_ERROR_STOP=1 -c "truncate pk_t" >> "$work/pk.log" 2>&1 ||
  fail "parked tables: truncate"
held_send pk "select 'after truncate ' || count(*) from pk_t;
  insert into pk_t values(4,40,'60'); select 'step3';"
held_wait pk step3 || fail "parked tables: insert after a truncate"
grep -qx 'after truncate 0' "$work/pk.out" ||
  fail "parked tables: rows after a truncate"
[ "$(local_psql -c 'select count(*) from pk_t')" = 1 ] ||
  fail "parked tables: the write missed the new store"

local_psql -v ON_ERROR_STOP=1 -c "
  alter table pk_t alter column s type int using s::int" \
  >> "$work/pk.log" 2>&1 || fail "parked tables: alter type"
held_send pk "select 'sum ' || s + 1 from pk_t; select 'step4';"
held_wait pk step4 || fail "parked tables: read after a type change"
grep -qx 'sum 61' "$work/pk.out" || fail "parked tables: the old type"

local_psql -v ON_ERROR_STOP=1 -c "drop index pk_t_v" \
  >> "$work/pk.log" 2>&1 || fail "parked tables: drop index"
held_send pk "insert into pk_t values(5,50,70);
  select 'after drop ' || count(*) from pk_t; select 'step5';"
held_wait pk step5 || fail "parked tables: insert after a drop index"
grep -qx 'after drop 2' "$work/pk.out" ||
  fail "parked tables: rows after a drop index"

local_psql -v ON_ERROR_STOP=1 -c "drop table pk_t;
  create table pk_t(id int primary key);
  insert into pk_t values(1),(2),(3);" >> "$work/pk.log" 2>&1 ||
  fail "parked tables: drop and make again"
held_send pk "select 'new table ' || count(*) from pk_t; select 'step6';"
held_wait pk step6 || fail "parked tables: read of a table made again"
grep -qx 'new table 3' "$work/pk.out" || fail "parked tables: the old table"

held_send pk "begin; truncate pk_t; rollback;
  select 'rolled back ' || count(*) from pk_t; select 'step7';"
held_wait pk step7 || fail "parked tables: own truncate"
grep -qx 'rolled back 3' "$work/pk.out" ||
  fail "parked tables: rows after a truncate rolled back"
grep -q 'ERROR:' "$work/pk.out" && fail "parked tables: an error"
held_end pk
local_psql -c 'drop table pk_t' >> "$work/pk.log" 2>&1
