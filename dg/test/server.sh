#!/bin/bash
# server.sh -- check that the server starts on a data directory initdb
# made, and that PostgreSQL's psql can connect to it and run a statement.
#
#   test/server.sh
#
# The server listens on 127.0.0.1 and on a socket in build/test/server,
# at port DG_TEST_PORT, 54329 if that is not set. Its log and each
# check's output are left in build/test/server/. DIGRES names the server
# to run, build/digres if it is not set: "make tsan" sets it to the build
# under the thread sanitizer, and then a report in the log fails.
cd "$(dirname "$0")/.." || exit 1
if ! command -v psql > /dev/null; then
  echo "server: SKIPPED (no psql to connect with)"
  exit 0
fi
work=$PWD/build/test/server
port=${DG_TEST_PORT:-54329}
digres=${DIGRES:-build/digres}
rm -rf "$work"
mkdir -p "$work"
failed=0

# Report the check NAME as failed.
fail() {
  echo "server: FAILED ($1; see $work)"
  failed=1
}

# Run psql on the local socket with the arguments given, bare output.
local_psql() {
  psql -X -A -t -h "$work" -p "$port" -d postgres "$@"
}

# Run psql over TCP with the password and the arguments given.
tcp_psql() {
  PGPASSWORD=$1 psql -X -A -t -h 127.0.0.1 -p "$port" -d postgres "${@:2}"
}

# Verify that the server starts on what initdb made, and takes connections.
# 1. Make a data directory whose superuser has a password.
# 2. Start the server on it.
# 3. Wait until it says it is ready, ten seconds at most.
echo "sesame" > "$work/pw"
build/initdb --pwfile="$work/pw" "$work/data" > "$work/initdb.out" 2>&1 ||
  fail "initdb"
$digres -D "$work/data" -k "$work" -p "$port" \
  -c authentication_timeout=1 > "$work/log" 2>&1 &
server=$!
for _ in $(seq 100); do
  grep -q "ready to accept connections" "$work/log" && break
  sleep 0.1
done
grep -q "ready to accept connections" "$work/log" || fail "start"

# Verify that a statement runs over the local socket with no password.
# 1. Connect as the OS user, which peer authentication allows.
# 2. Confirm SELECT 1 + 1 answers 2.
# 3. Confirm the session's user is the superuser initdb named.
[ "$(local_psql -c 'select 1 + 1' 2> "$work/sum.err")" = 2 ] || fail "1 + 1"
[ "$(local_psql -c 'select current_user' 2> "$work/user.err")" = \
  "$(id -un)" ] || fail "current_user"

# Verify that a session carries on after an error.
# 1. Run a statement that fails, and one after it, in one session.
# 2. Confirm the error was reported and the second statement answered.
local_psql -c 'select 1/0' -c 'select 2 + 3' > "$work/error.out" 2>&1
grep -q "division by zero" "$work/error.out" || fail "error reported"
grep -qx 5 "$work/error.out" || fail "statement after an error"

# Verify that the local socket admits only the superuser, to a database
# that exists.
# 1. Connect as another role, and confirm peer authentication refuses it.
# 2. Connect to a database that is not there, and confirm it is refused.
psql -X -h "$work" -p "$port" -U nobody_here -d postgres -c 'select 1' \
  > "$work/peer.out" 2>&1 && fail "peer admitted another role"
grep -q "Peer authentication failed" "$work/peer.out" || fail "peer message"
psql -X -h "$work" -p "$port" -d no_such_db -c 'select 1' \
  > "$work/nodb.out" 2>&1 && fail "unknown database admitted"
grep -q 'database "no_such_db" does not exist' "$work/nodb.out" ||
  fail "unknown database message"

# Verify that a TCP connection is authenticated by password.
# 1. Connect with the password initdb was given, and run a statement.
# 2. Connect with another password, and confirm it is refused.
[ "$(tcp_psql sesame -c 'select 6 * 7' 2> "$work/tcp.err")" = 42 ] ||
  fail "password accepted"
tcp_psql wrong -c 'select 1' > "$work/badpw.out" 2>&1 &&
  fail "wrong password admitted"
grep -q "password authentication failed" "$work/badpw.out" ||
  fail "wrong password message"

# Verify that a statement the server cannot run yet fails without taking
# the server down.
# 1. Read a catalog table, which digres does not have.
# 2. Confirm the statement after it, in a new session, still answers.
local_psql -c 'select * from pg_class' > "$work/catalog.out" 2>&1
[ "$(local_psql -c 'select 3 - 1' 2> "$work/after.err")" = 2 ] ||
  fail "server after a failed lookup"

# Verify that VACUUM succeeds and does nothing, since sm collects its own
# garbage.
# 1. Run VACUUM, with and without options, then a statement after them.
# 2. Confirm psql exited with success and reported no error.
# 3. Confirm the statement after them answered.
local_psql -c 'vacuum' -c 'vacuum (full, analyze) no_such_table' \
  -c 'select 4 + 4' > "$work/vacuum.out" 2>&1 || fail "vacuum succeeds"
grep -q "ERROR" "$work/vacuum.out" && fail "vacuum reported an error"
grep -qx 8 "$work/vacuum.out" || fail "statement after vacuum"

# Verify that CHECKPOINT succeeds and does nothing, since sm keeps what is
# durable.
# 1. Run CHECKPOINT, then a statement after it.
# 2. Confirm psql exited with success and reported no error.
# 3. Confirm the statement after it answered.
local_psql -c 'checkpoint' -c 'select 9 + 9' > "$work/checkpoint.out" 2>&1 ||
  fail "checkpoint succeeds"
grep -q "ERROR" "$work/checkpoint.out" && fail "checkpoint reported an error"
grep -qx 18 "$work/checkpoint.out" || fail "statement after checkpoint"

# Verify that PREPARE TRANSACTION is refused, since a prepared transaction's
# state was kept in WAL and there is none.
# 1. Prepare a transaction, roll back the block the refusal aborted, then
#    run a statement, in one session.
# 2. Confirm the refusal was reported and the last statement answered.
# 3. Confirm COMMIT PREPARED finds no prepared transaction.
local_psql -c "begin; prepare transaction 'x'" -c 'rollback' \
  -c 'select 5 + 5' > "$work/prepare.out" 2>&1
grep -q "PREPARE TRANSACTION is not supported" "$work/prepare.out" ||
  fail "prepare refused"
grep -qx 10 "$work/prepare.out" || fail "statement after prepare"
local_psql -c "commit prepared 'x'" > "$work/prepare2.out" 2>&1
grep -q 'prepared transaction with identifier "x" does not exist' \
  "$work/prepare2.out" || fail "commit prepared"

# Verify that a session waits for its own popen() child, which nothing else
# in the process reaps first.
# 1. Run COPY TO PROGRAM twenty times, each writing its number to a file.
# 2. Confirm each succeeded and its program wrote what it was given.
for i in $(seq 20); do
  local_psql -c "copy (select $i) to program 'cat > $work/program.$i'" \
    > "$work/program.out" 2>&1 || { fail "copy to program"; break; }
  [ "$(cat "$work/program.$i" 2> /dev/null)" = "$i" ] ||
    { fail "copy to program output"; break; }
done

# Verify that sessions run at once, each a thread of the one server process.
# 1. Open 8 sessions, and run a statement in each while all are open.
# 2. Confirm each answered.
# 3. Confirm the server has a thread for each, and no child process.
# 4. Close them.
for i in $(seq 8); do
  mkfifo "$work/many$i.in"
  local_psql < "$work/many$i.in" > "$work/many$i.out" 2>&1 &
  eval "exec $((10 + i))> \"$work/many$i.in\""
  echo "select $i * 100;" >&$((10 + i))
done
for i in $(seq 8); do
  for _ in $(seq 50); do
    grep -qx $((i * 100)) "$work/many$i.out" && break
    sleep 0.1
  done
  grep -qx $((i * 100)) "$work/many$i.out" || fail "session $i of 8"
done
[ "$(ls /proc/$server/task | wc -l)" -gt 8 ] || fail "a thread a session"
[ "$(pgrep -P $server | wc -l)" = 0 ] || fail "no child process"
for i in $(seq 8); do
  eval "exec $((10 + i))>&-"
done
wait $(jobs -p | grep -v "^$server$")

# Verify that a session that never answers the password request is ended
# when authentication_timeout passes: the timeout signals its own session
# (SignalSelf()).
# 1. Open a TCP connection, send a startup packet, and say nothing more.
# 2. Confirm the server closes it within a few seconds.
# 3. Confirm the log tells why.
python3 - "$port" > "$work/authtimeout.out" 2>&1 <<'PY' || fail "auth timeout"
import socket, struct, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
body = struct.pack("!i", 196608) + b"user\0nobody\0database\0postgres\0\0"
s.sendall(struct.pack("!i", len(body) + 4) + body)
s.settimeout(10)
start = time.time()
while s.recv(4096):
    pass
sys.exit(0 if time.time() - start < 5 else 1)
PY
grep -q "canceling authentication due to timeout" "$work/log" ||
  fail "auth timeout message"

# Verify that CREATE TABLE makes a table in sm, for good.
# 1. Create a table of integer columns, one NOT NULL, in its own session.
# 2. Confirm another session cannot create it again, and IF NOT EXISTS
#    passes over it.
# 3. Confirm a column type sm's tables do not take yet is refused.
[ "$(local_psql -c 'create table t1 (a int2, b int4, c int8 not null)' \
  2>&1)" = "CREATE TABLE" ] || fail "create table"
local_psql -c 'create table t1 (a int4)' > "$work/table.out" 2>&1
grep -q 'relation "t1" already exists' "$work/table.out" ||
  fail "create table twice"
local_psql -c 'create table if not exists t1 (a int4)' >> "$work/table.out" 2>&1
grep -q 'relation "t1" already exists, skipping' "$work/table.out" ||
  fail "create table if not exists"
local_psql -c 'create table t2 (a text)' >> "$work/table.out" 2>&1
grep -q 'type text is not supported yet' "$work/table.out" ||
  fail "create table, a type not taken"

# Verify that INSERT writes rows to sm and a scan reads them back, in the
# transaction's isolation.
# 1. Insert rows into t1, one statement of two, and confirm a scan with and
#    without a WHERE returns them.
# 2. Confirm a NULL in the NOT NULL column is refused, and a rolled back
#    insert leaves nothing.
# 3. Confirm UPDATE is refused, not done on a heap that is not there.
# 4. In a session at READ COMMITTED, then at REPEATABLE READ, scan; insert
#    from another session; scan again: the first sees the new row, the
#    second does not.
local_psql -c 'insert into t1 values (1, 10, 100)' > /dev/null 2>&1
local_psql -c 'insert into t1 values (2, 20, 200), (3, 30, 300)' > /dev/null 2>&1
[ "$(local_psql -c 'select * from t1' 2>&1 | tr '\n' ' ')" = \
  "1|10|100 2|20|200 3|30|300 " ] || fail "insert and scan"
[ "$(local_psql -c 'select a + b from t1 where c > 150' 2>&1 | tr '\n' ' ')" \
  = "22 33 " ] || fail "scan with a WHERE"
local_psql -c 'insert into t1 values (4, 40, null)' > "$work/rows.out" 2>&1
grep -q 'violates not-null constraint' "$work/rows.out" || fail "not null"
local_psql > /dev/null 2>&1 <<'SQL'
begin;
insert into t1 values (5, 50, 500);
rollback;
SQL
[ "$(local_psql -c 'select a from t1 where a > 3' 2>&1)" = "" ] ||
  fail "rollback"
local_psql -c 'update t1 set a = 0' >> "$work/rows.out" 2>&1
grep -q 'only a plain INSERT writes to table "t1" yet' "$work/rows.out" ||
  fail "update refused"
mkfifo "$work/iso.in"
local_psql < "$work/iso.in" > "$work/iso.out" 2>&1 &
iso=$!
exec 4> "$work/iso.in"
row=6
for level in "read committed" "repeatable read"; do
  echo "begin isolation level $level; select count_me from (select 1 as count_me) s;" >&4
  echo "select a from t1 where a > 5;" >&4
  for _ in $(seq 50); do
    [ "$(grep -c '^1$' "$work/iso.out")" -ge $((row - 5)) ] && break
    sleep 0.1
  done
  local_psql -c "insert into t1 values ($row, 0, 0)" > /dev/null 2>&1
  echo "select 'after'; select a from t1 where a > 5; commit;" >&4
  row=$((row + 1))
done
exec 4>&-
wait $iso
[ "$(tr '\n' ' ' < "$work/iso.out")" = \
  "BEGIN 1 after 6 COMMIT BEGIN 1 6 after 6 COMMIT " ] ||
  fail "isolation levels"

# Verify that a statement waiting in sm for another session's lock is ended
# by a statement timeout, from its own sm_sys.h timer, and by a cancel request
# from its client, which another thread turns into an interrupt bit in it:
# sm's wait ends, and CHECK_FOR_INTERRUPTS() raises the error.
# 1. In a session of its own, begin, and drop t1 without committing: it
#    holds t1's lock.
# 2. From a session with statement_timeout = 500, scan t1: it waits, and
#    fails within seconds, its statement timed out.
# 3. Scan t1 from another session, and interrupt its psql, which sends a
#    cancel request: the scan fails, cancelled.
# 4. Roll the drop back: t1 is still there.
mkfifo "$work/lock.in"
local_psql < "$work/lock.in" > "$work/lock.out" 2>&1 &
holder=$!
exec 4> "$work/lock.in"
echo "begin; drop table t1; select 'dropped';" >&4
for _ in $(seq 50); do
  grep -qx dropped "$work/lock.out" && break
  sleep 0.1
done
timeout 10 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -c 'set statement_timeout = 500' -c 'select * from t1' \
  > "$work/timeout.out" 2>&1
grep -q 'canceling statement due to statement timeout' "$work/timeout.out" ||
  fail "statement timeout on a lock wait"
psql -X -A -t -h "$work" -p "$port" -d postgres -c 'select * from t1' \
  > "$work/cancel.out" 2>&1 &
waiter=$!
sleep 1
kill -INT $waiter
for _ in $(seq 50); do
  kill -0 $waiter 2> /dev/null || break
  sleep 0.1
done
if kill -0 $waiter 2> /dev/null; then
  fail "cancel ends a lock wait"
  kill -KILL $waiter
fi
wait $waiter
grep -q 'canceling statement due to user request' "$work/cancel.out" ||
  fail "cancel message"
echo "rollback;" >&4
exec 4>&-
wait $holder
[ "$(local_psql -c 'select a from t1 where a = 1' 2>&1)" = 1 ] ||
  fail "drop rolled back"

# Verify that a savepoint rolled back undoes its rows in sm, and one
# released keeps them.
# 1. In t2, insert, set a savepoint, insert, roll back to it, insert, and
#    commit: the middle row is gone.
# 2. Roll back a savepoint set before any row, and one whose insert failed.
# 3. Release an inner savepoint, roll back the outer: neither's rows stay;
#    roll back an inner one, release the outer: the outer's row stays.
local_psql > /dev/null 2>&1 <<'SQL'
create table t2 (a int4 not null);
begin; insert into t2 values (1); savepoint s; insert into t2 values (2);
rollback to s; insert into t2 values (3); commit;
begin; savepoint s; insert into t2 values (4); rollback to s; commit;
begin; insert into t2 values (5); savepoint s; insert into t2 values (null);
rollback to s; insert into t2 values (6); commit;
begin; savepoint a; insert into t2 values (7); savepoint b;
insert into t2 values (8); release b; rollback to a; commit;
begin; savepoint a; insert into t2 values (9); savepoint b;
insert into t2 values (10); rollback to b; release a; commit;
SQL
[ "$(local_psql -c 'select a from t2' 2>&1 | tr '\n' ' ')" = "1 3 5 6 9 " ] ||
  fail "savepoints"

# Verify that LISTEN and NOTIFY are refused, and that UNLISTEN and DISCARD
# ALL, which a pooler sends, still run.
# 1. Confirm LISTEN and NOTIFY fail, each with its name.
# 2. Confirm UNLISTEN * and DISCARD ALL succeed.
local_psql -c 'listen ch' > "$work/notify.out" 2>&1
local_psql -c 'notify ch' >> "$work/notify.out" 2>&1
grep -q 'LISTEN is not supported' "$work/notify.out" &&
  grep -q 'NOTIFY is not supported' "$work/notify.out" ||
  fail "listen and notify refused"
[ "$(local_psql -c 'unlisten *' -c 'discard all' 2>&1 | tr '\n' ' ')" = \
  "UNLISTEN DISCARD ALL " ] || fail "unlisten and discard all"

# Verify that large objects are refused: GRANT, default privileges and
# COMMENT on one each fail, naming large objects.
# 1. Run each statement in a session of its own.
# 2. Confirm each fails with the one message.
for sql in "grant select on large object 123 to public" \
  "alter default privileges grant select on large objects to public" \
  "comment on large object 123 is 'x'"; do
  local_psql -c "$sql" 2>&1 | grep -q 'large objects are not supported' ||
    fail "large object refused: $sql"
done

# Verify that publications and subscriptions are refused: creating,
# altering, dropping or commenting on one fails, naming which.
# 1. Run each statement in a session of its own.
# 2. Confirm each fails saying publications, or subscriptions, are not
#    supported.
for sql in "create publication p for all tables" \
  "alter publication p add table t" "drop publication p" \
  "comment on publication p is 'x'"; do
  local_psql -c "$sql" 2>&1 | grep -q 'publications are not supported' ||
    fail "publication refused: $sql"
done
for sql in "create subscription s connection 'x' publication p" \
  "drop subscription s"; do
  local_psql -c "$sql" 2>&1 | grep -q 'subscriptions are not supported' ||
    fail "subscription refused: $sql"
done

# Verify that LOCK TABLE takes sm's table lock: SHARE lets another session
# read and keeps it from writing; ACCESS EXCLUSIVE keeps it from reading.
# 1. In a session of its own, lock t2 in SHARE mode and hold it.
# 2. From another, a scan of t2 answers; an insert with statement_timeout =
#    500 times out.
# 3. Commit, lock t2 in ACCESS EXCLUSIVE mode, and hold it: a scan times out.
# 4. Confirm NOWAIT is refused, as sm cannot ask for it yet.
mkfifo "$work/locker.in"
local_psql < "$work/locker.in" > "$work/locker.out" 2>&1 &
locker=$!
exec 4> "$work/locker.in"
echo "begin; lock table t2 in share mode; select 'locked';" >&4
for _ in $(seq 50); do
  grep -qx locked "$work/locker.out" && break
  sleep 0.1
done
[ "$(local_psql -c 'select a from t2 where a = 1' 2>&1)" = 1 ] ||
  fail "read under a share lock"
timeout 10 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -c 'set statement_timeout = 500' -c 'insert into t2 values (100)' \
  > "$work/locker2.out" 2>&1
grep -q 'canceling statement due to statement timeout' "$work/locker2.out" ||
  fail "write under a share lock"
echo "commit; begin; lock table t2 in access exclusive mode; select 'excl';" >&4
for _ in $(seq 50); do
  grep -qx excl "$work/locker.out" && break
  sleep 0.1
done
timeout 10 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -c 'set statement_timeout = 500' -c 'select * from t2' \
  > "$work/locker3.out" 2>&1
grep -q 'canceling statement due to statement timeout' "$work/locker3.out" ||
  fail "read under an exclusive lock"
echo "commit;" >&4
exec 4>&-
wait $locker
local_psql -c 'begin' -c 'lock table t2 nowait' > "$work/locker4.out" 2>&1
grep -q 'NOWAIT is not supported yet' "$work/locker4.out" || fail "nowait"

# Verify that CREATE INDEX and DROP INDEX are sm's: a unique index refuses a
# duplicate, a dropped one no longer does, and names default as PostgreSQL's.
# 1. Create t3, a row, a plain index with no name and a unique one.
# 2. Confirm the second plain index gets the name t3_a_idx1, and a duplicate
#    of the unique key is refused.
# 3. Drop the unique index: the duplicate goes in; drop it again: it is gone.
local_psql > /dev/null 2>&1 <<'SQL'
create table t3 (a int4, b int8);
insert into t3 values (1, 10);
create index on t3 (a);
create index on t3 (a);
create unique index t3_b on t3 (b desc) include (a);
SQL
local_psql -c 'drop index t3_a_idx1' > "$work/index.out" 2>&1
grep -qx 'DROP INDEX' "$work/index.out" || fail "index default name"
local_psql -c 'insert into t3 values (2, 10)' > "$work/index.out" 2>&1
grep -q 'duplicate key value violates a unique index on "t3"' \
  "$work/index.out" || fail "unique index"
local_psql -c 'drop index t3_b' -c 'insert into t3 values (2, 10)' \
  -c 'drop index t3_b' > "$work/index.out" 2>&1
grep -qx 'INSERT 0 1' "$work/index.out" &&
  grep -q 'index "t3_b" does not exist' "$work/index.out" ||
  fail "drop index"

# Verify that an index scan reads a table in sm through its index: equal
# keys, ranges, a key on a second column, and IS NULL find the rows they
# should, with the plan an index scan.
# 1. Create t4 and a two-column index in one transaction, so sm fills the
#    index at once, then insert rows, one with a null key.
# 2. Confirm the plan of a range on the first column is an index scan,
#    with bitmap scans, which the planner prefers here, off.
# 3. Confirm a range, an equal key with a range on the second column, IS
#    NULL and an empty range return the right rows.
local_psql > /dev/null 2>&1 <<'SQL'
begin;
create table t4 (a int4, b int8, c int2);
create index t4_ac on t4 (a, c);
commit;
insert into t4 values (1, 10, 1), (2, 20, 2), (3, 30, 3), (4, 40, 4),
  (3, 31, 6), (null, 60, 7);
SQL
local_psql -c 'set enable_bitmapscan = off' \
  -c 'explain (costs off) select * from t4 where a >= 2 and a < 4' \
  > "$work/iscan.out" 2>&1
grep -q 'Index Scan using t4_ac on t4' "$work/iscan.out" || fail "index plan"
[ "$(local_psql -c 'select b from t4 where a >= 2 and a < 4' 2>&1 |
  tr '\n' ' ')" = "20 30 31 " ] || fail "index range"
[ "$(local_psql -c 'select b from t4 where a = 3 and c > 3' 2>&1)" = 31 ] ||
  fail "index key and range"
[ "$(local_psql -c 'select b from t4 where a is null' 2>&1)" = 60 ] ||
  fail "index is null"
[ "$(local_psql -c 'select b from t4 where a > 2 and a < 2' 2>&1)" = "" ] ||
  fail "index empty range"

# Verify that PRIMARY KEY and UNIQUE in CREATE TABLE make sm's unique
# indexes, and that a primary key's columns are NOT NULL.
# 1. Create t5 with an inline PRIMARY KEY and UNIQUE, and t6 with a table
#    PRIMARY KEY of two columns; insert a row in each.
# 2. Confirm a duplicate of each key is refused, and a NULL key.
# 3. Confirm a lookup by the primary key is an index scan on t5_pkey.
local_psql > /dev/null 2>&1 <<'SQL'
create table t5 (a int4 primary key, b int8 unique);
create table t6 (a int4, b int8, primary key (a, b));
insert into t5 values (1, 10);
insert into t6 values (1, 1), (1, 2);
SQL
for sql in "insert into t5 values (1, 20)" "insert into t5 values (2, 10)" \
  "insert into t6 values (1, 2)"; do
  local_psql -c "$sql" 2>&1 | grep -q 'duplicate key value' ||
    fail "duplicate refused: $sql"
done
local_psql -c 'insert into t5 values (null, 30)' 2>&1 |
  grep -q 'violates not-null constraint' || fail "primary key not null"
local_psql -c 'explain (costs off) select * from t5 where a = 1' 2>&1 |
  grep -q 'Index Scan using t5_pkey on t5' || fail "primary key plan"

# Verify that an index in sm gives ORDER BY its rows in order, forwards: a
# descending order the index does not have is sorted.
# 1. Order t4 by a, its index's order: an index scan, nulls last.
# 2. Order it by a DESC: a sort, nulls first, as PostgreSQL's DESC orders.
# 3. A LIMIT with a range stops early, in order.
local_psql -c 'explain (costs off) select b from t4 order by a' 2>&1 |
  grep -q 'Index Scan using t4_ac on t4' || fail "ordered index plan"
[ "$(local_psql -c 'select b from t4 order by a, c' 2>&1 | tr '\n' ' ')" = \
  "10 20 30 31 40 60 " ] || fail "ordered index scan"
[ "$(local_psql -c 'select b from t4 order by a desc, c desc' 2>&1 |
  tr '\n' ' ')" = "60 40 31 30 20 10 " ] || fail "descending order"
[ "$(local_psql -c 'select b from t4 where a > 1 order by a, c limit 2' 2>&1 |
  tr '\n' ' ')" = "20 30 " ] || fail "ordered limit"

# Verify that a scroll cursor over an index in sm, which scans only forwards,
# moves back over what it read.
# 1. Declare a scroll cursor ordered by t4's index, with sorts off.
# 2. Fetch two rows forwards, then one back: the first again.
local_psql -c 'set enable_sort = off' -c 'begin' \
  -c 'declare c scroll cursor for select b from t4 order by a, c' \
  -c 'fetch 2 from c' -c 'fetch prior from c' -c 'commit' \
  > "$work/scroll.out" 2>&1
[ "$(grep -v '^[A-Z]' "$work/scroll.out" | tr '\n' ' ')" = "10 20 10 " ] ||
  fail "scroll cursor"

# Verify that bitmap scans of indexes in sm find the rows their quals pick:
# bitmaps of rownos ORed and ANDed, an IN list, and no row the scan's
# transaction does not see.
# 1. Fill t7 with a = 1..1000 and b = a % 10, indexed on a and on b.
# 2. With sequential and index scans off, an OR of both columns plans a
#    BitmapOr, and an AND of a range and an equality a BitmapAnd.
# 3. Confirm the OR, the AND, an OR of two ANDs, which unions bitmaps
#    rather than adding to one, and an IN list each find just their rows, and
#    a key on t4's second index column, which bounds no scan, its one row.
# 4. In a session of its own, insert a row with b = 3 and hold it
#    uncommitted; confirm a bitmap scan for b = 3 does not see it.
bitmap_psql() {
  local_psql -c 'set enable_seqscan = off' -c 'set enable_indexscan = off' \
    -c "$1" 2>&1 | grep -v '^SET$'
}
local_psql -c 'create table t7 (a int4, b int4)' -c 'create index t7_a on t7 (a)' \
  -c 'create index t7_b on t7 (b)' \
  -c "insert into t7 values $(seq 1000 | awk '{printf "%s(%d,%d)",
    (NR > 1 ? "," : ""), $1, $1 % 10}')" > "$work/bitmap.out" 2>&1
bitmap_psql 'explain (costs off) select a from t7 where a < 25 or b = 3' |
  grep -q 'BitmapOr' || fail "bitmap or plan"
bitmap_psql 'explain (costs off) select a from t7
  where a between 100 and 140 and b = 3' | grep -q 'BitmapAnd' ||
  fail "bitmap and plan"
[ "$(bitmap_psql 'select a from t7 where a < 25 or b = 3' | wc -l)" = 121 ] ||
  fail "bitmap or"
[ "$(bitmap_psql 'select a from t7 where a between 100 and 140 and b = 3' |
  tr '\n' ' ')" = "103 113 123 133 " ] || fail "bitmap and"
[ "$(bitmap_psql 'select a from t7 where (a between 100 and 140 and b = 3)
  or (a between 500 and 530 and b = 4)' | tr '\n' ' ')" = \
  "103 113 123 133 504 514 524 " ] || fail "bitmap or of ands"
[ "$(bitmap_psql 'select a from t7 where a in (900, 5, 77)' |
  tr '\n' ' ')" = "5 77 900 " ] || fail "bitmap in list"
[ "$(bitmap_psql 'select b from t4 where c = 2')" = 20 ] ||
  fail "bitmap recheck"
mkfifo "$work/held.in"
local_psql < "$work/held.in" > "$work/held.out" 2>&1 &
held=$!
exec 4> "$work/held.in"
echo "begin; insert into t7 values (5000, 3); select 'held';" >&4
for _ in $(seq 50); do
  grep -q '^held$' "$work/held.out" && break
  sleep 0.1
done
[ "$(bitmap_psql 'select a from t7 where b = 3' | wc -l)" = 100 ] ||
  fail "bitmap scan sees an uncommitted row"
echo "rollback;" >&4
exec 4>&-
wait $held

# Verify that an index-only scan of an index in sm gives the index's
# columns from its entries, reading no row: key columns decoded from the
# key, INCLUDE columns from the entry's value.
# 1. Create t8 with an index on (a, b DESC) including c, and a unique
#    index on d; insert rows with a negative int2, nulls, and a null d.
# 2. With sequential and bitmap scans off, a range on a plans an
#    index-only scan, and returns a, b and c, nulls and the sign kept.
# 3. A range on d, by the unique index, returns its values; d IS NULL,
#    whose entry carries its rowno, returns its one row.
only_psql() {
  local_psql -c 'set enable_seqscan = off' -c 'set enable_bitmapscan = off' \
    -c "$1" 2>&1 | grep -v '^SET$'
}
local_psql -c 'create table t8 (a int4, b int2, c int8, d int4)' \
  -c 'create index t8_ab on t8 (a, b desc) include (c)' \
  -c 'create unique index t8_d on t8 (d)' \
  -c 'insert into t8 values (1, -1, 10, 100), (2, 5, 20, null),
    (2, -3, 30, 300), (null, 4, 40, 400), (5, null, null, 500)' \
  > "$work/only.out" 2>&1
only_psql 'explain (costs off) select a, b, c from t8 where a >= 2' |
  grep -q 'Index Only Scan using t8_ab on t8' || fail "index-only plan"
[ "$(only_psql 'select a, b, c from t8 where a >= 2' | tr '\n' ' ')" = \
  "2|5|20 2|-3|30 5|| " ] || fail "index-only scan"
[ "$(only_psql 'select d from t8 where d > 100' | tr '\n' ' ')" = \
  "300 400 500 " ] || fail "index-only unique"
[ "$(only_psql 'select d is null from t8 where d is null')" = t ] ||
  fail "index-only null key"

# Verify that tablespaces are refused, their settings gone, and that a
# sort too big for work_mem still spills to temporary files, which went
# to the temporary tablespaces.
# 1. Confirm CREATE, ALTER, rename and DROP TABLESPACE each fail.
# 2. Confirm SET temp_tablespaces fails: there is no such setting.
# 3. Fill t9 with a = 1..1000 and b = a % 10, four times over; with
#    work_mem at 64kB, sort it by b: an external merge, in order.
for sql in "create tablespace x location '/tmp'" "drop tablespace x" \
  "alter tablespace pg_default set (seq_page_cost = 1)" \
  "alter tablespace pg_default rename to y"; do
  local_psql -c "$sql" 2>&1 | grep -q 'tablespaces are not supported' ||
    fail "tablespace refused: $sql"
done
local_psql -c "set temp_tablespaces = ''" 2>&1 |
  grep -q 'unrecognized configuration parameter' || fail "temp_tablespaces"
rows=$(seq 1000 | awk '{printf "%s(%d,%d)", (NR > 1 ? "," : ""), $1, $1 % 10}')
local_psql -c 'create table t9 (a int4, b int4)' \
  -c "insert into t9 values $rows" -c "insert into t9 values $rows" \
  -c "insert into t9 values $rows" -c "insert into t9 values $rows" \
  > "$work/sort.out" 2>&1
local_psql -c "set work_mem = '64kB'" -c 'explain (analyze, costs off,
  timing off, summary off) select a from t9 order by b, a' 2>&1 |
  grep -q 'external merge' || fail "external sort plan"
[ "$(local_psql -c "set work_mem = '64kB'" \
  -c 'select a from t9 order by b, a limit 5' 2>&1 | grep -v '^SET$' |
  tr '\n' ' ')" = "10 10 10 10 20 " ] || fail "external sort"

# Verify that CREATE and DROP DATABASE are sm's: a new database is a
# database of its own, a database in use is not dropped unless forced, and
# ALTER DATABASE is refused.
# 1. Create d1; creating it again, or d2 with an option, fails.
# 2. Make a table in d1 with a row: d1 sees it, postgres does not.
# 3. Dropping d1 from d1 fails: it is the session's own.
# 4. With a session held open on d1, DROP DATABASE d1 fails as in use;
#    WITH (FORCE) drops d1, and the held session's next statement finds
#    it ended.
# 5. Dropping d1 again fails, IF EXISTS notices, and connecting to it fails.
# 6. Create d1 again: it has no table t. ALTER DATABASE fails.
db_psql() {
  psql -X -A -t -h "$work" -p "$port" -d "$1" -c "$2" 2>&1
}
[ "$(db_psql postgres 'create database d1')" = "CREATE DATABASE" ] ||
  fail "create database"
db_psql postgres 'create database d1' | grep -q 'database "d1" already exists' ||
  fail "create database twice"
db_psql postgres 'create database d2 with owner = x' |
  grep -q 'CREATE DATABASE takes no options yet' || fail "create db options"
db_psql d1 'create table t (a int4)' > "$work/db.out"
db_psql d1 'insert into t values (1)' >> "$work/db.out"
[ "$(db_psql d1 'select a from t')" = 1 ] || fail "table in d1"
db_psql postgres 'select a from t' | grep -q 'relation "t" does not exist' ||
  fail "table not in postgres"
db_psql d1 'drop database d1' |
  grep -q 'cannot drop the currently open database' || fail "drop own db"
mkfifo "$work/dbheld.in"
psql -X -A -t -h "$work" -p "$port" -d d1 < "$work/dbheld.in" \
  > "$work/dbheld.out" 2>&1 &
dbheld=$!
exec 4> "$work/dbheld.in"
echo "select 'held';" >&4
for _ in $(seq 50); do
  grep -q '^held$' "$work/dbheld.out" && break
  sleep 0.1
done
db_psql postgres 'drop database d1' |
  grep -q 'database "d1" is being accessed by other users' ||
  fail "drop database in use"
[ "$(db_psql postgres 'drop database d1 with (force)')" = "DROP DATABASE" ] ||
  fail "drop database force"
echo "select 2;" >&4 2> /dev/null
exec 4>&-
wait $dbheld
grep -q 'terminating connection due to administrator command' \
  "$work/dbheld.out" || fail "force ends the session"
db_psql postgres 'drop database d1' | grep -q 'database "d1" does not exist' ||
  fail "drop database twice"
db_psql postgres 'drop database if exists d1' |
  grep -q 'does not exist, skipping' || fail "drop database if exists"
db_psql d1 'select 1' | grep -q 'database "d1" does not exist' ||
  fail "connect to a dropped database"
db_psql postgres 'create database d1' > "$work/db.out"
db_psql d1 'select a from t' | grep -q 'relation "t" does not exist' ||
  fail "a new database is empty"
db_psql postgres 'alter database d1 rename to d3' |
  grep -q 'ALTER DATABASE is not supported' || fail "alter database"

# Verify that CLUSTER and REINDEX succeed, doing nothing, each with a
# warning saying why: sm keeps no table in an index's order, and keeps an
# index's entries with its table's rows.
# 1. Run CLUSTER on t4 by its index, and CLUSTER of every table.
# 2. Run REINDEX of t4, of its index, and of the database.
# 3. Confirm each warns and reports its command.
for sql in "cluster t4 using t4_ac" "cluster"; do
  out=$(local_psql -c "$sql" 2>&1)
  echo "$out" | grep -q 'WARNING:  CLUSTER does nothing' &&
    echo "$out" | grep -q '^CLUSTER$' || fail "cluster: $sql"
done
for sql in "reindex table t4" "reindex index t4_ac" \
  "reindex database postgres"; do
  out=$(local_psql -c "$sql" 2>&1)
  echo "$out" | grep -q 'WARNING:  REINDEX does nothing' &&
    echo "$out" | grep -q '^REINDEX$' || fail "reindex: $sql"
done

# Verify that a schema is sm's: CREATE SCHEMA makes one a table can be made
# in, for good or not at all, a session whose search path named it before it
# was made finds it, and DROP SCHEMA drops it, with its tables only when told.
# 1. Create s1; creating it again fails, and IF NOT EXISTS notices. Make
#    s1.t with a row, and read it back.
# 2. Hold a session with search_path s3, public that has read public.nsp0
#    by its bare name; create s3 from another; the held session's unqualified
#    CREATE TABLE w lands in s3.
# 3. Create s4 in a transaction and roll back: s4 is not there.
# 4. CREATE SCHEMA s5 CREATE TABLE v makes v in s5.
# 5. DROP SCHEMA s1 fails while it holds t; CASCADE drops it, telling of t,
#    and s1.t is gone; dropping s1 again fails, and IF EXISTS notices.
# 6. Dropping public is refused.
[ "$(local_psql -c 'create schema s1' 2>&1)" = "CREATE SCHEMA" ] ||
  fail "create schema"
local_psql -c 'create schema s1' 2>&1 |
  grep -q 'schema "s1" already exists' || fail "create schema twice"
local_psql -c 'create schema if not exists s1' 2>&1 |
  grep -q 'schema "s1" already exists, skipping' ||
  fail "create schema if not exists"
local_psql -c 'create table s1.t (a int4)' -c 'insert into s1.t values (3)' \
  > "$work/schema.out" 2>&1
[ "$(local_psql -c 'select a from s1.t' 2>&1)" = 3 ] || fail "table in s1"
local_psql -c 'create table nsp0 (a int4)' >> "$work/schema.out" 2>&1
mkfifo "$work/nsp.in"
local_psql < "$work/nsp.in" > "$work/nsp.out" 2>&1 &
nsp=$!
exec 4> "$work/nsp.in"
echo "set search_path = s3, public; select count(*) from nsp0;" >&4
echo "select 'held';" >&4
for _ in $(seq 50); do
  grep -q '^held$' "$work/nsp.out" && break
  sleep 0.1
done
local_psql -c 'create schema s3' >> "$work/schema.out" 2>&1
echo "create table w (a int4); select 'made';" >&4
for _ in $(seq 50); do
  grep -q '^made$' "$work/nsp.out" && break
  sleep 0.1
done
exec 4>&-
wait $nsp
local_psql -c 'select count(*) from s3.w' 2>&1 | grep -qx 0 ||
  fail "search path finds a schema made later"
local_psql -c 'begin' -c 'create schema s4' -c 'rollback' \
  >> "$work/schema.out" 2>&1
local_psql -c 'create table s4.x (a int4)' 2>&1 |
  grep -q 'schema "s4" does not exist' || fail "create schema rolled back"
local_psql -c 'create schema s5 create table v (b int8)' \
  >> "$work/schema.out" 2>&1
local_psql -c 'select count(*) from s5.v' 2>&1 | grep -qx 0 ||
  fail "create schema with a table"
local_psql -c 'drop schema s1' 2>&1 |
  grep -q 'cannot drop schema s1 because other objects depend on it' ||
  fail "drop schema restrict"
local_psql -c 'drop schema s1 cascade' > "$work/nspdrop.out" 2>&1
grep -q 'drop cascades to table s1.t' "$work/nspdrop.out" &&
  grep -q '^DROP SCHEMA$' "$work/nspdrop.out" || fail "drop schema cascade"
local_psql -c 'select a from s1.t' 2>&1 |
  grep -q 'relation "s1.t" does not exist' || fail "dropped schema's table"
local_psql -c 'drop schema s1' 2>&1 |
  grep -q 'schema "s1" does not exist' || fail "drop schema twice"
local_psql -c 'drop schema if exists s1' 2>&1 |
  grep -q 'schema "s1" does not exist, skipping' ||
  fail "drop schema if exists"
local_psql -c 'drop schema public' 2>&1 |
  grep -q 'schema "public" cannot be dropped' || fail "drop public"

# Verify that a sequence is sm's: CREATE SEQUENCE makes one, by a name a
# table cannot also take, nextval counts from START by INCREMENT and never
# gives a value back or out twice, currval and lastval report the session's
# own draws, a schema's drop takes its sequences only when told, and DROP
# SEQUENCE drops one.
# 1. Create q from 10 by 5; again fails, IF NOT EXISTS notices, and a table
#    named q fails.
# 2. In one session draw 10 and 15, then currval and lastval give 15; in a
#    new session both are not yet defined.
# 3. Draw in a transaction and roll back: the next draw is higher still.
# 4. Four sessions draw 25 each at once: 100 values, no two the same.
# 5. Make s6.r: it gives 1. DROP SCHEMA s6 fails; CASCADE drops it, telling
#    of r.
# 6. CYCLE, a negative increment, ALTER SEQUENCE and setval() are refused,
#    and reading q as a table is too.
# 7. Drop q: nextval('q') then fails, as does dropping it again; IF EXISTS
#    notices, and DROP SEQUENCE of a table fails.
# 8. DISCARD SEQUENCES runs, and forgets the session's draws.
[ "$(local_psql -c 'create sequence q start 10 increment 5' 2>&1)" = \
  "CREATE SEQUENCE" ] || fail "create sequence"
local_psql -c 'create sequence q' 2>&1 |
  grep -q 'relation "q" already exists' || fail "create sequence twice"
local_psql -c 'create sequence if not exists q' 2>&1 |
  grep -q 'relation "q" already exists, skipping' ||
  fail "create sequence if not exists"
local_psql -c 'create table q (a int4)' 2>&1 |
  grep -q 'relation "q" already exists' || fail "a table takes a sequence's name"
[ "$(local_psql -c "select nextval('q')" -c "select nextval('q')" \
  -c "select currval('q')" -c 'select lastval()' 2>&1 | tr '\n' ' ')" = \
  "10 15 15 15 " ] || fail "nextval, currval and lastval"
local_psql -c "select currval('q')" 2>&1 |
  grep -q 'currval of sequence "q" is not yet defined in this session' ||
  fail "currval in a new session"
local_psql -c 'select lastval()' 2>&1 |
  grep -q 'lastval is not yet defined in this session' ||
  fail "lastval in a new session"
local_psql -c 'begin' -c "select nextval('q')" -c 'rollback' \
  > "$work/seq.out" 2>&1
[ "$(local_psql -c "select nextval('q') > 20" 2>&1)" = t ] ||
  fail "a rolled back draw is not given back"
draws=""
for i in 1 2 3 4; do
  local_psql -c "select nextval('q') from generate_series(1, 25)" \
    > "$work/draw$i.out" 2>&1 &
  draws="$draws $!"
done
for pid in $draws; do
  wait "$pid"
done
[ "$(cat "$work"/draw?.out | grep -cx '[0-9]*')" = 100 ] &&
  [ "$(cat "$work"/draw?.out | sort -u | wc -l)" = 100 ] ||
  fail "concurrent draws are distinct"
local_psql -c 'create schema s6' -c 'create sequence s6.r' \
  >> "$work/seq.out" 2>&1
[ "$(local_psql -c "select nextval('s6.r')" 2>&1)" = 1 ] ||
  fail "a sequence in a schema"
local_psql -c 'drop schema s6' 2>&1 |
  grep -q 'cannot drop schema s6 because other objects depend on it' ||
  fail "drop schema holding a sequence"
local_psql -c 'drop schema s6 cascade' 2>&1 |
  grep -q 'drop cascades to sequence s6.r' || fail "drop schema cascade, sequence"
local_psql -c 'create sequence z cycle' 2>&1 |
  grep -q 'CREATE SEQUENCE supports only START and INCREMENT yet, not cycle' ||
  fail "refused: cycle"
local_psql -c 'create sequence z increment -1' 2>&1 |
  grep -q 'a sequence counts up from 1 or more' ||
  fail "refused: a negative increment"
for sql in "alter sequence q restart" "select setval('q', 5)"; do
  local_psql -c "$sql" 2>&1 | grep -q 'not supported yet' ||
    fail "refused: $sql"
done
local_psql -c 'select * from q' 2>&1 |
  grep -q '"q" is a sequence, which cannot be used as a relation yet' ||
  fail "reading a sequence as a table"
[ "$(local_psql -c 'drop sequence q' 2>&1)" = "DROP SEQUENCE" ] ||
  fail "drop sequence"
local_psql -c "select nextval('q')" 2>&1 |
  grep -q 'relation "q" does not exist' || fail "nextval of a dropped sequence"
local_psql -c 'drop sequence q' 2>&1 |
  grep -q 'sequence "q" does not exist' || fail "drop sequence twice"
local_psql -c 'drop sequence if exists q' 2>&1 |
  grep -q 'sequence "q" does not exist, skipping' ||
  fail "drop sequence if exists"
local_psql -c 'create table qt (a int4)' -c 'drop sequence qt' 2>&1 |
  grep -q '"qt" is not a sequence' || fail "drop sequence of a table"
local_psql -c 'create sequence q2' -c "select nextval('q2')" \
  -c 'discard sequences' -c "select currval('q2')" 2>&1 |
  grep -q 'currval of sequence "q2" is not yet defined' ||
  fail "discard sequences forgets the draws"

# Verify that ANALYZE is sm's, and that the planner sizes a table by it:
# never analyzed, ten pages; analyzed, its live rows, growing with it.
# 1. Make t13 with 20,000 rows: a scan is estimated under 5,000 rows.
# 2. Analyze it in a transaction rolled back: still under 5,000.
# 3. Analyze it: a scan is estimated at 20,000 rows exactly.
# 4. Insert 20,000 more: the estimate is 40,000 within 10%, with no
#    analyze since.
# 5. A bare ANALYZE VERBOSE reports t13 and a table in another schema;
#    a sequence is passed over with a warning; a column list is refused.
rows_of() {
  local_psql -c "explain $1" 2>&1 | grep -o 'rows=[0-9]*' | head -1 |
    cut -d= -f2
}
local_psql -c 'create table t13 (a int4, b int8)' \
  -c 'insert into t13 select g, g % 10 from generate_series(1, 20000) g' \
  > "$work/analyze.out" 2>&1
[ "$(rows_of 'select * from t13')" -lt 5000 ] || fail "never analyzed"
local_psql -c 'begin' -c 'analyze t13' -c 'rollback' >> "$work/analyze.out" 2>&1
[ "$(rows_of 'select * from t13')" -lt 5000 ] || fail "analyze rolled back"
[ "$(local_psql -c 'analyze t13' 2>&1)" = "ANALYZE" ] || fail "analyze"
[ "$(rows_of 'select * from t13')" = 20000 ] || fail "analyzed rows"
local_psql -c 'insert into t13 select g, g from generate_series(1, 20000) g' \
  >> "$work/analyze.out" 2>&1
n=$(rows_of 'select * from t13')
[ "$n" -gt 36000 ] && [ "$n" -lt 44000 ] || fail "rows grow with the table: $n"
local_psql -c 'create schema s7' -c 'create table s7.u (a int4)' \
  -c 'insert into s7.u values (1), (2)' >> "$work/analyze.out" 2>&1
local_psql -c 'analyze verbose' > "$work/analyze_all.out" 2>&1
grep -q 'INFO:  "t13": ' "$work/analyze_all.out" &&
  grep -q 'INFO:  "u": 2 live rows' "$work/analyze_all.out" ||
  fail "analyze every table"
local_psql -c 'create sequence q3' -c 'analyze q3' 2>&1 |
  grep -q 'WARNING:  skipping "q3"' || fail "analyze a sequence"
local_psql -c 'analyze t13 (a)' 2>&1 |
  grep -q 'ANALYZE of chosen columns is not supported yet' ||
  fail "analyze a column list"

# Verify that the planner estimates a filter by sm's column statistics.
# 1. Make t14 with 50,000 rows: b one of ten values, c null in a third and
#    one of 1,000 values otherwise; b = 0 is estimated at the default.
# 2. Analyze it: b = 0 and b <> 0 are estimated by the common values,
#    5,000 and 45,000 within 10%.
# 3. c IS NULL is estimated by the null fraction, 16,667 within 10%;
#    c < 100 by the histogram, 3,333 within 15%.
# 4. DISTINCT b is estimated at its 10 distinct values.
local_psql -c 'create table t14 (a int4, b int8, c int2)' \
  -c "insert into t14 select g, g % 10, case when g % 3 = 0 then null
      else g % 1000 end from generate_series(1, 50000) g" \
  > "$work/stats.out" 2>&1
[ "$(rows_of 'select * from t14 where b = 0')" -lt 100 ] ||
  fail "no statistics before analyze"
local_psql -c 'analyze t14' >> "$work/stats.out" 2>&1
within() {
  [ "$1" -ge $(($2 - $2 * $3 / 100)) ] && [ "$1" -le $(($2 + $2 * $3 / 100)) ]
}
n=$(rows_of 'select * from t14 where b = 0')
within "$n" 5000 10 || fail "b = 0 by the common values: $n"
n=$(rows_of 'select * from t14 where b <> 0')
within "$n" 45000 10 || fail "b <> 0 by the common values: $n"
n=$(rows_of 'select * from t14 where c is null')
within "$n" 16667 10 || fail "c is null by the null fraction: $n"
n=$(rows_of 'select * from t14 where c < 100')
within "$n" 3333 15 || fail "c < 100 by the histogram: $n"
n=$(local_psql -c 'explain select distinct b from t14' 2>&1 |
  grep -o 'rows=[0-9]*' | head -1 | cut -d= -f2)
[ "$n" = 10 ] || fail "distinct b by its distinct values: $n"

# Verify that a GROUP BY and a join of tables in sm plan and run, and that
# a join is estimated by the distinct values on each side.
# 1. GROUP BY b on t14 gives each of b's values, 0 with 5,000 rows.
# 2. Make u14 with 2,000 rows, b one of 100 values, and analyze it.
# 3. t14 join u14 on b is estimated at 10 x 5,000 x 20 = 1,000,000
#    within 10%.
[ "$(local_psql -c 'select b, count(*) from t14 group by b
      order by b limit 1' 2>&1)" = "0|5000" ] || fail "group by"
local_psql -c 'create table u14 (a int4, b int8)' \
  -c 'insert into u14 select g, g % 100 from generate_series(1, 2000) g' \
  -c 'analyze u14' >> "$work/stats.out" 2>&1
n=$(rows_of 'select * from t14 join u14 on t14.b = u14.b')
within "$n" 1000000 10 || fail "join by distinct values: $n"

# Verify that pg_stats() shows sm's statistics as the pg_stats view would.
# 1. A table never analyzed, t15, has no rows in pg_stats('t15').
# 2. t14 has a row a column in pg_stats('t14'), and pg_stats() has them
#    with u14's.
# 3. t14's b has its ten values as common values and no histogram; c has
#    a third null, and 101 histogram bounds.
local_psql -c 'create table t15 (a int4)' >> "$work/stats.out" 2>&1
[ "$(local_psql -c "select count(*) from pg_stats('t15')" 2>&1)" = 0 ] ||
  fail "pg_stats of a table never analyzed"
[ "$(local_psql -c "select string_agg(attname, ',' order by attname)
      from pg_stats('t14')" 2>&1)" = "a,b,c" ] || fail "pg_stats a column"
[ "$(local_psql -c "select count(*) from pg_stats()
      where tablename in ('t14', 'u14')" 2>&1)" = 5 ] ||
  fail "pg_stats of every table"
[ "$(local_psql -c "select array_length(most_common_vals, 1),
      array_length(most_common_freqs, 1), histogram_bounds is null
      from pg_stats('t14') where attname = 'b'" 2>&1)" = "10|10|t" ] ||
  fail "pg_stats common values"
[ "$(local_psql -c "select null_frac between '0.3' and '0.37',
      array_length(histogram_bounds, 1)
      from pg_stats('t14') where attname = 'c'" 2>&1)" = "t|101" ] ||
  fail "pg_stats nulls and bounds"

# Verify that the buffer manager's settings are gone, and DISCARD SEQUENCES
# still runs.
# 1. Confirm DISCARD SEQUENCES succeeds.
# 2. Confirm temp_buffers, effective_io_concurrency, io_combine_limit,
#    zero_damaged_pages and multixact_offset_buffers are not settings any
#    more, and the data directory has no pg_multixact.
[ "$(local_psql -c 'discard sequences' 2>&1)" = "DISCARD SEQUENCES" ] ||
  fail "discard sequences"
for guc in temp_buffers effective_io_concurrency io_combine_limit \
  zero_damaged_pages multixact_offset_buffers; do
  local_psql -c "show $guc" 2>&1 |
    grep -q 'unrecognized configuration parameter' || fail "setting $guc"
done
[ ! -e "$work/data/pg_multixact" ] || fail "no pg_multixact"

# Verify that SERIALIZABLE is sm's: a write skew between two serializable
# transactions fails one of them with a serialization failure, though
# PostgreSQL's predicate locks are gone, with their settings and pg_serial.
# 1. Create t10 with rows 1 and 2.
# 2. In two sessions, begin SERIALIZABLE, read a > 10 (nothing), and insert
#    a row of a > 10; commit the first, then the second.
# 3. Confirm the first commits, and the second fails with SQLSTATE 40001.
# 4. Confirm max_pred_locks_per_transaction and shared_buffers are not
#    settings, and the data directory has no pg_serial.
local_psql -c 'create table t10 (a int4)' -c 'insert into t10 values (1), (2)' \
  > "$work/skew.out" 2>&1
for n in 1 2; do
  mkfifo "$work/skew$n.in"
  psql -X -A -t -v VERBOSITY=verbose -h "$work" -p "$port" -d postgres \
    < "$work/skew$n.in" > "$work/skew$n.out" 2>&1 &
  eval "skew$n=\$!"
done
exec 5> "$work/skew1.in" 6> "$work/skew2.in"
echo "begin isolation level serializable; select a from t10 where a > 10;" >&5
echo "begin isolation level serializable; select a from t10 where a > 10;" >&6
echo "insert into t10 values (11); select 'inserted';" >&5
echo "insert into t10 values (12); select 'inserted';" >&6
for _ in $(seq 50); do
  grep -q '^inserted$' "$work/skew1.out" &&
    grep -q '^inserted$' "$work/skew2.out" && break
  sleep 0.1
done
echo "commit;" >&5
exec 5>&-
wait $skew1
echo "commit;" >&6
exec 6>&-
wait $skew2
grep -q '^COMMIT$' "$work/skew1.out" || fail "serializable first commit"
grep -q 'ERROR:  40001: could not commit: not serializable' \
  "$work/skew2.out" || fail "serializable write skew"
for guc in max_pred_locks_per_transaction shared_buffers; do
  local_psql -c "show $guc" 2>&1 |
    grep -q 'unrecognized configuration parameter' || fail "setting $guc"
done
[ ! -e "$work/data/pg_serial" ] || fail "no pg_serial"

# Verify that TRUNCATE empties a table in sm, in its transaction: a rollback,
# or a rollback to a savepoint taken before it, brings the rows back, and the
# table and its index take new rows after.
# 1. Fill t11, keyed by a, with rows 1 and 2.
# 2. Truncate in a transaction, read t11 there (empty), insert 3 and find it
#    by its key, then roll back: rows 1 and 2 again.
# 3. Truncate inside a savepoint and roll back to it: rows 1 and 2.
# 4. Truncate, commit, insert 5: t11 holds 5 alone, found by its key.
# 5. TRUNCATE ... RESTART IDENTITY, and TRUNCATE under an open cursor of t11,
#    are refused.
local_psql -c 'create table t11 (a int4 primary key, b int4)' \
  -c 'insert into t11 values (1, 10), (2, 20)' > "$work/truncate.out" 2>&1
[ "$(local_psql -c 'begin' -c 'truncate t11' -c 'select a from t11' \
  -c 'insert into t11 values (3, 30)' -c 'select b from t11 where a = 3' \
  -c 'rollback' 2>&1 | tr '\n' ' ')" = \
  "BEGIN TRUNCATE TABLE INSERT 0 1 30 ROLLBACK " ] || fail "truncate in xact"
[ "$(local_psql -c 'select a from t11' | tr '\n' ' ')" = "1 2 " ] ||
  fail "truncate rolled back"
[ "$(local_psql -c 'begin' -c 'savepoint s' -c 'truncate t11' \
  -c 'rollback to s' -c 'select a from t11' -c 'commit' 2>&1 |
  grep -v '^[A-Z]' | tr '\n' ' ')" = "1 2 " ] || fail "truncate savepoint"
local_psql -c 'truncate t11' -c 'insert into t11 values (5, 50)' \
  >> "$work/truncate.out" 2>&1
[ "$(local_psql -c 'select a from t11' -c 'select b from t11 where a = 5' |
  tr '\n' ' ')" = "5 50 " ] || fail "truncate committed"
local_psql -c 'truncate t11 restart identity' 2>&1 |
  grep -q 'sequences are not supported yet' || fail "truncate restart identity"
local_psql -c 'begin' -c 'declare c cursor for select a from t11' \
  -c 'truncate t11' 2>&1 | grep -q 'being used by active queries' ||
  fail "truncate under a cursor"

# Verify that functions are found by name among the built-in ones: a plain
# call, a call by argument names, a set-returning function,
# aggregates, and a window function, the last two over a table in sm.
# 1. Call abs(-3), parse_ident(strict => true, str => 'a.b') and
#    generate_series(1, 3).
# 2. Fill t12; count, sum, average and distinct-count it.
# 3. Run a running sum over it as a window function.
[ "$(local_psql -c 'select abs(-3)' 2>&1)" = 3 ] || fail "function call"
[ "$(local_psql -c "select parse_ident(strict => true, str => 'a.b')" 2>&1)" \
  = "{a,b}" ] || fail "function call by argument name"
[ "$(local_psql -c 'select generate_series(1, 3)' 2>&1 | tr '\n' ' ')" = \
  "1 2 3 " ] || fail "set-returning function"
local_psql -c 'create table t12 (a int4, b int4)' \
  -c 'insert into t12 values (1, 10), (2, 20), (2, 30)' > "$work/agg.out" 2>&1
[ "$(local_psql -c 'select count(*), sum(b), avg(b), count(distinct a)
  from t12' 2>&1)" = "3|60|20.0000000000000000|2" ] || fail "aggregates"
[ "$(local_psql -c 'select sum(b) over (order by b) from t12' 2>&1 |
  tr '\n' ' ')" = "10 30 60 " ] || fail "window function"

# Verify that = finds rows, and that DROP TABLE removes a table for good,
# even from a session that had used it.
# 1. Confirm WHERE a = 2 finds the one row.
# 2. In a session of its own, scan t1; drop t1 from another, and create it
#    again with other columns and a row; scan again in the first session:
#    it sees the new table.
# 3. Confirm dropping a table that is gone fails, and IF EXISTS passes.
[ "$(local_psql -c 'select b from t1 where a = 2' 2>&1)" = 20 ] ||
  fail "where a = 2"
mkfifo "$work/drop.in"
local_psql < "$work/drop.in" > "$work/drop.out" 2>&1 &
dropper=$!
exec 4> "$work/drop.in"
echo "select a from t1 where a = 1;" >&4
for _ in $(seq 50); do
  grep -qx 1 "$work/drop.out" && break
  sleep 0.1
done
local_psql -c 'drop table t1' > /dev/null 2>&1
local_psql -c 'create table t1 (x int8)' > /dev/null 2>&1
local_psql -c 'insert into t1 values (77)' > /dev/null 2>&1
echo "select * from t1;" >&4
exec 4>&-
wait $dropper
[ "$(tr '\n' ' ' < "$work/drop.out")" = "1 77 " ] || fail "drop table"
local_psql -c 'drop table t1' > /dev/null 2>&1
local_psql -c 'drop table t1' > "$work/drop2.out" 2>&1
grep -q 'table "t1" does not exist' "$work/drop2.out" || fail "drop twice"
local_psql -c 'drop table if exists t1' > "$work/drop2.out" 2>&1
grep -q 'does not exist, skipping' "$work/drop2.out" || fail "drop if exists"

# Verify that a signal sent to the server can land only in the postmaster's
# thread: every other thread, sessions, sm's, RocksDB's and sm_sys.h's,
# blocks the signals the postmaster handles, whose handlers set the
# postmaster's latch.
# 1. With a session idle (opened above), list the server's threads.
# 2. Confirm each but the main one blocks SIGHUP, SIGINT, SIGQUIT, SIGTERM,
#    SIGUSR1, SIGUSR2 and SIGALRM.
python3 - "$server" > "$work/sigblk.out" 2>&1 <<'PY' || fail "only the postmaster takes signals"
import os, sys
pid = sys.argv[1]
want = 0
for sig in (1, 2, 3, 10, 12, 14, 15):
    want |= 1 << (sig - 1)
bad = []
for tid in os.listdir(f"/proc/{pid}/task"):
    if tid == pid:
        continue
    try:
        with open(f"/proc/{pid}/task/{tid}/status") as f:
            status = dict(l.split(":", 1) for l in f if ":" in l)
    except FileNotFoundError:
        continue
    if int(status["SigBlk"], 16) & want != want:
        bad.append(f"{tid} {status['Name'].strip()} {status['SigBlk'].strip()}")
print("\n".join(bad))
sys.exit(1 if bad else 0)
PY

# Verify that SIGUSR1 and SIGUSR2 are ignored: there is no log rotation, nor
# pmsignal, and no child to reserve SIGUSR2 for.
# 1. Send the server SIGUSR1 and SIGUSR2.
# 2. Confirm it still runs a statement.
kill -USR1 $server
kill -USR2 $server
sleep 0.2
[ "$(local_psql -c 'select 2 + 2' 2> "$work/usr1.err")" = 4 ] ||
  fail "SIGUSR1 and SIGUSR2 ignored"

# Verify that the server stops when asked, with no process lost on the way,
# and ends a session left open: the postmaster's SIGTERM reaches it as a flag
# in its PGPROC (SignalProc()). Each side wakes the other's latch from
# another thread (sm_interrupt()), so a lost wake-up is a stall.
# 1. Open a session, run a statement in it, and leave it idle.
# 2. Interrupt the server, and confirm it stops within ten seconds.
# 3. Confirm it exited with status 0, and ended the idle session.
# 4. Confirm its log tells of no process ended by a signal.
mkfifo "$work/idle.in"
local_psql < "$work/idle.in" > "$work/idle.out" 2>&1 &
idle=$!
exec 3> "$work/idle.in"
echo "select 7 * 7;" >&3
for _ in $(seq 50); do
  grep -qx 49 "$work/idle.out" && break
  sleep 0.1
done
grep -qx 49 "$work/idle.out" || fail "idle session"
kill -INT $server
for _ in $(seq 100); do
  kill -0 $server 2> /dev/null || break
  sleep 0.1
done
if kill -0 $server 2> /dev/null; then
  fail "shutdown is prompt"
  kill -KILL $server
fi
wait $server || fail "shutdown status"
grep -q "terminating connection due to administrator command" "$work/log" ||
  fail "idle session ended"
exec 3>&-
wait $idle
grep -q "terminated by signal" "$work/log" && fail "a process crashed"
grep -q "ThreadSanitizer" "$work/log" && fail "a thread sanitizer report"

[ $failed = 0 ] && echo "server: PASSED"
exit $failed
