#!/bin/bash
# server.sh -- check that the server starts on a data directory initdb
# made, and that PostgreSQL's psql can connect to it and run a statement.
#
#   test/server.sh
#
# The server listens on 127.0.0.1 and on a socket in build/test/server,
# at port DG_TEST_PORT, or if that is not set one the kernel finds free,
# so runs side by side, by one user or two, do not meet. Its log and each
# check's output are left in build/test/server/. DIGRES names the server
# to run, build/digres if it is not set: "make tsan" sets it to the build
# under the thread sanitizer, and then a report in the log fails.
cd "$(dirname "$0")/.." || exit 1
if ! command -v psql > /dev/null; then
  echo "server: SKIPPED (no psql to connect with)"
  exit 0
fi
work=$PWD/build/test/server
# A free port: the kernel's pick for a socket bound to port 0, released
# before the server binds it.
port=${DG_TEST_PORT:-$(python3 -c 'import socket; s = socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')}
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
local_psql -c 'create table t2 (a point)' >> "$work/table.out" 2>&1
grep -q 'type point is not supported yet' "$work/table.out" ||
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
local_psql -c 'insert into t1 values (1, 1, 1) on conflict do nothing' \
  >> "$work/rows.out" 2>&1
grep -q 'do not write to table "t1" yet' "$work/rows.out" ||
  fail "on conflict refused"
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

# Verify that a SERIAL column draws from a sequence its column owns, which
# goes with the table.
# 1. sr1's serial, sr2's bigserial and smallserial number their rows from
#    1; sr1's sequence, its name taken, is sr1_id_seq1.
# 2. A sequence a column owns is not dropped alone; owned by none, it is.
# 3. OWNED BY a table of another schema, or AS text, is refused.
# 4. DROP TABLE drops the sequence sr2's columns own.
# 5. DROP SCHEMA ... CASCADE drops a serial's table, and its sequence with
#    it, unnamed.
local_psql -c 'create sequence sr1_id_seq' \
  -c 'create table sr1 (id serial primary key, v text)' \
  -c 'create table sr2 (id bigserial, w smallserial, v text)' > /dev/null 2>&1
[ "$(local_psql -c "insert into sr1 (v) values ('a'), ('b') returning id" \
  -c "insert into sr2 (v) values ('x') returning id, w" \
  -c "select nextval('sr1_id_seq1')" 2>&1 | tr '\n' ,)" = \
  "1,2,INSERT 0 2,1|1,INSERT 0 1,3," ] || fail "serial columns"
local_psql -c 'drop sequence sr1_id_seq1' 2>&1 |
  grep -q 'cannot drop sequence sr1_id_seq1 because other objects depend' ||
  fail "an owned sequence dropped alone"
[ "$(local_psql -c 'alter sequence sr1_id_seq owned by sr1.v' \
  -c 'alter sequence sr1_id_seq owned by none' \
  -c 'drop sequence sr1_id_seq' 2>&1 | tr '\n' ,)" = \
  "ALTER SEQUENCE,ALTER SEQUENCE,DROP SEQUENCE," ] ||
  fail "a sequence owned by none"
local_psql -c 'create schema s9' -c 'create table s9.u (id int4)' \
  -c 'create sequence q9' -c 'alter sequence q9 owned by s9.u.id' 2>&1 |
  grep -q 'sequence must be in same schema as table it is linked to' ||
  fail "a sequence owned across schemas"
local_psql -c 'create sequence q10 as text' 2>&1 |
  grep -q 'sequence type must be smallint, integer, or bigint' ||
  fail "a sequence AS text"
local_psql -c 'drop table sr2' -c "select nextval('sr2_id_seq')" 2>&1 |
  grep -q 'relation "sr2_id_seq" does not exist' ||
  fail "a table's drop takes its sequence"
local_psql -c 'create table s9.v (id serial)' > /dev/null 2>&1
[ "$(local_psql -c 'drop schema s9 cascade' -c 'create schema s9' 2>&1 |
  tr '\n' ,)" = "NOTICE:  drop cascades to 2 other objects,DETAIL:  drop cascades to table s9.u,drop cascades to table s9.v,DROP SCHEMA,CREATE SCHEMA," ] ||
  fail "drop schema cascade, serial"

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
# 4. s7.u, analyzed above, is shown in its schema, s7, by name: a schema
#    made by CREATE SCHEMA is named by sm.
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
[ "$(local_psql -c "select schemaname, tablename from pg_stats('s7.u')" \
  2>&1)" = "s7|u" ] || fail "pg_stats of a table in a schema made"

# Verify that a regclass prints as its relation's name, qualified only
# when the search path would not find it.
# 1. On the default path, t14 prints bare and s7.u qualified.
# 2. With s7 first on the path, s7.u prints bare; a t14 made in s7 then
#    hides public's, which prints qualified.
# 3. An oid that is no relation prints as the number.
[ "$(local_psql -c "select 't14'::regclass, 's7.u'::regclass" 2>&1)" = \
  "t14|s7.u" ] || fail "regclass on the default path"
[ "$(local_psql -c 'create table s7.t14 (a int4)' -c 'set search_path = s7, public' \
  -c "select 's7.u'::regclass, 'public.t14'::regclass, 's7.t14'::regclass" \
  2>&1 | tail -1)" = "u|public.t14|t14" ] || fail "regclass on a path"
[ "$(local_psql -c 'select 99999::regclass' 2>&1)" = 99999 ] ||
  fail "regclass of no relation"

# Verify that a type named by its schema, as the SQL standard's names are
# (integer is pg_catalog.int4), is found among the compiled-in types.
# 1. Casts to integer, bigint, real, double precision, varchar(3) and
#    numeric give their values.
# 2. pg_catalog.int4 is found; public.int4 is not a type, and nope.int4
#    names no schema.
[ "$(local_psql -c "select 1::integer, 2::bigint, 1.5::real,
      2.5::double precision, 'xyz1'::varchar(3), 1.25::numeric" 2>&1)" = \
  "1|2|1.5|2.5|xyz|1.25" ] || fail "casts to the standard's type names"
[ "$(local_psql -c "select '7'::pg_catalog.int4" 2>&1)" = 7 ] ||
  fail "a type qualified by its schema"
local_psql -c "select '1'::public.int4" 2>&1 |
  grep -q 'type "public.int4" does not exist' || fail "no type in a schema"
local_psql -c "select '1'::nope.int4" 2>&1 |
  grep -q 'schema "nope" does not exist' || fail "a type in no schema"

# Verify that an operator whose argument types match none exactly is
# chosen among the compiled-in operators of its name, as PostgreSQL's
# rules choose.
# 1. float4 > a numeric constant, numeric + integer, and int2 * int8 are
#    found by coercing an argument.
# 2. OPERATOR(pg_catalog.+) is found; OPERATOR(public.+) does not exist.
[ "$(local_psql -c "select '1'::float4 > 0.3, 1.5::numeric(5,2) + 1,
      2::int2 * 3::int8" 2>&1)" = "t|2.50|6" ] ||
  fail "operators chosen by coercion"
[ "$(local_psql -c 'select 3 operator(pg_catalog.+) 4' 2>&1)" = 7 ] ||
  fail "an operator qualified by its schema"
local_psql -c 'select 3 operator(public.+) 4' 2>&1 |
  grep -q 'operator does not exist: integer public.+ integer' ||
  fail "no operator in a schema"

# Verify that range and multirange types, and functions polymorphic over
# them, such as unnest's anymultirange form, are resolved from the
# compiled-in pg_range.
# 1. unnest of an array gives its elements, with ordinality too.
# 2. int4range contains 5; numeric ranges intersect.
# 3. An int4multirange is made of two ranges.
[ "$(local_psql -c 'select unnest(array[1,2])' 2>&1 | tr '\n' ,)" = "1,2," ] ||
  fail "unnest"
[ "$(local_psql -c "select * from unnest(array['a','b']) with ordinality" \
  2>&1 | tr '\n' ,)" = "a|1,b|2," ] || fail "unnest with ordinality"
[ "$(local_psql -c 'select int4range(1, 10) @> 5,
      numrange(1.5, 2.5) * numrange(2, 3)' 2>&1)" = "t|[2,2.5)" ] ||
  fail "ranges"
[ "$(local_psql -c 'select int4multirange(int4range(1, 3), int4range(5, 7))' \
  2>&1)" = "{[1,3),[5,7)}" ] || fail "a multirange"

# Verify that array_agg, whose arguments the planner checks for send and
# receive functions, plans and runs.
# 1. array_agg of an unnest gives the array back.
# 2. array_agg ordered, grouped over t14, gives each group's values.
[ "$(local_psql -c 'select array_agg(x) from unnest(array[3,1,2]) x' 2>&1)" = \
  "{3,1,2}" ] || fail "array_agg"
[ "$(local_psql -c 'select b, array_agg(a order by a) from t14
      where a <= 20 group by b order by b limit 1' 2>&1)" = "0|{10,20}" ] ||
  fail "array_agg grouped"

# Verify that the built-in functions PostgreSQL writes in SQL, written in C
# here (utils/adt/sql_bodies.c), and the operators and the cast they
# implement, give what PostgreSQL's SQL bodies give.
# 1. || of text and any other type casts the other to text, bool's as
#    'true'; quote_literal and quote_nullable take any type.
# 2. lpad and rpad pad with spaces; numeric round, trunc, log and log10
#    take their defaults; bit_length counts bits.
# 3. + of a date, time, timestamp or timetz with an interval, integer or
#    date on the left; inet, pg_lsn and path @> point; circle's polygon.
# 4. date_part of a date, age from today, timestamptz(date, time).
# 5. OVERLAPS with lengths, with ends, and with a null start.
# 6. pg_sleep_for and pg_sleep_until return; a strict function of a null
#    xml is null.
local_psql -c "set timezone = 'UTC'" \
  -c "select 'a' || 1, 1 || 'a', 'a' || true, 'x' || 1.5::numeric,
      quote_literal(42), quote_literal(true), quote_nullable(null::int),
      quote_nullable(7)" \
  -c "select lpad('ab', 5), rpad('ab', 5) || '|', round(2.5::numeric),
      trunc(2.7::numeric), log(100::numeric), log10(1000::numeric),
      bit_length('abc'), bit_length('\x0102'::bytea), bit_length(B'10101')" \
  -c "select 1 + date '2024-01-31', interval '1 day' + date '2024-01-01',
      time '10:00' + date '2024-01-01', interval '1 hour' + time '10:00',
      interval '1 day' + timestamp '2024-01-01 00:00',
      timetz '10:00+02' + date '2024-01-01', interval '1 hour' + timetz '10:00+02',
      interval '1 day' + timestamptz '2024-01-01 00:00+00'" \
  -c "select 10::int8 + inet '10.0.0.1', 1::numeric + '0/10'::pg_lsn,
      path '((0,0),(2,0))' @> point '(1,0)',
      npoints(circle '<(0,0),1>'::polygon)" \
  -c "select date_part('year', date '2024-05-01'),
      age(current_date::timestamp), age(current_date::timestamptz),
      timestamptz(date '2024-01-01', time '10:00')" \
  -c "select (date '2024-01-01', interval '2 days') overlaps
      (date '2024-01-02', interval '1 day'),
      (time '10:00', interval '1 hour') overlaps (time '10:30', time '12:00'),
      (timestamp '2024-01-01', timestamp '2024-01-02') overlaps
      (timestamp '2024-01-03', interval '1 day'),
      ((null::timestamp, interval '1 day') overlaps
      (timestamp '2024-01-01', interval '1 day')) is null" \
  -c "select pg_sleep_for('10 milliseconds'), pg_sleep_until(now()),
      xpath('/a', null::xml) is null" > "$work/sqlbodies.out" 2>&1
cat > "$work/sqlbodies.expected" <<'OUT'
SET
a1|1a|atrue|x1.5|'42'|'true'|NULL|'7'
   ab|ab   ||3|2|2.0000000000000000|3.0000000000000000|24|16|5
2024-02-01|2024-01-02 00:00:00|2024-01-01 10:00:00|11:00:00|2024-01-02 00:00:00|2024-01-01 08:00:00+00|11:00:00+02|2024-01-02 00:00:00+00
10.0.0.11|0/11|t|12
2024|00:00:00|00:00:00|2024-01-01 10:00:00+00
t|t|f|t
||t
OUT
cmp -s "$work/sqlbodies.out" "$work/sqlbodies.expected" ||
  fail "functions written in SQL by PostgreSQL"

# Verify that an ALTER of a table or index is refused plainly, as digres
# has no ALTER TABLE yet, rather than failing on pg_class.
# 1. ADD FOREIGN KEY, ADD COLUMN, RENAME, RENAME COLUMN and SET SCHEMA of
#    t14 are refused, each naming what it is.
# 2. ALTER INDEX is refused as ALTER INDEX.
local_psql -c 'alter table t14 add foreign key (a) references u14' \
  -c 'alter table t14 add column z int4' -c 'alter table t14 rename to t99' \
  -c 'alter table t14 rename column a to z' \
  -c 'alter table t14 set schema s7' -c 'create index t14a on t14 (a)' \
  -c 'alter index t14a rename to t14b' > "$work/alter.out" 2>&1
[ "$(grep -c 'ERROR:  ALTER TABLE is not supported yet' "$work/alter.out")" = 4 ] &&
  grep -q 'ERROR:  ALTER TABLE ... RENAME COLUMN is not supported yet' \
    "$work/alter.out" &&
  grep -q 'ERROR:  ALTER INDEX is not supported yet' "$work/alter.out" ||
  fail "ALTER refused"

# Verify that text compares, cases and matches, by the database's default
# collation, C.
# 1. 'a' < 'b', and ORDER BY sorts B before a, as C's byte order does.
# 2. upper and lower change ASCII only; LIKE and ILIKE match.
# 3. A regular expression, SIMILAR TO and SUBSTRING ... SIMILAR match.
[ "$(local_psql -c "select 'a' < 'b', upper('abc'), lower('ÀB'),
      'Foo' like 'F%', 'foo' ilike 'F%'" 2>&1)" = "t|ABC|Àb|t|t" ] ||
  fail "text compared and cased"
[ "$(local_psql -c "select x from (values ('b'), ('a'), ('B')) v(x)
      order by x" 2>&1 | tr '\n' ,)" = "B,a,b," ] || fail "text sorted"
[ "$(local_psql -c "select substring('foobar' from 'o.b'), 'foobar' ~ 'o.b',
      'foobar' similar to '%o_b%',
      substring('foobar' similar '%#\"o_b#\"%' escape '#')" 2>&1)" = \
  "oob|t|t|oob" ] || fail "text matched"

# Verify that the built-in collations, compiled in, are found by name and
# used, by each of their providers.
# 1. "C" and "POSIX" compare by byte: 'a' is not below 'B'.
# 2. "unicode", ICU's root, sorts a, ä, b, B.
# 3. pg_c_utf8 upper-cases ä, "C" does not, pg_unicode_fast makes ß SS.
# 4. A collation qualified by pg_catalog is found; an unknown one is not.
[ "$(local_psql -c "select 'a' collate \"C\" < 'B', 'a' collate \"POSIX\" < 'B',
      upper('ä' collate pg_c_utf8), upper('ä' collate \"C\"),
      upper('ß' collate pg_unicode_fast), 'a' collate pg_catalog.\"C\" < 'b'" \
  2>&1)" = "f|f|Ä|ä|SS|t" ] || fail "built-in collations"
[ "$(local_psql -c "select x from (values ('b'), ('a'), ('B'), ('ä')) v(x)
      order by x collate \"unicode\"" 2>&1 | tr '\n' ,)" = "a,ä,b,B," ] ||
  fail "ICU collation"
local_psql -c "select 'a' collate nope < 'b'" 2>&1 |
  grep -q 'collation "nope" for encoding "UTF8" does not exist' ||
  fail "an unknown collation"

# Verify that a collation's name is printed from the rows compiled in.
# 1. regcollation prints a name, quoted when it must be, and an unknown oid
#    as a number.
# 2. EXPLAIN VERBOSE of a scan prints a constant's collation.
[ "$(local_psql -c "select 'unicode'::regcollation, 950::regcollation,
      0::regcollation, 99999::regcollation" 2>&1)" = 'unicode|"C"|-|99999' ] ||
  fail "regcollation printed"
local_psql -c 'create table tcoll (x text)' \
  -c "explain verbose select x < 'y' collate \"unicode\" from tcoll" 2>&1 |
  grep -q "'y'::text COLLATE unicode" || fail "a collation explained"

# Verify that bool, float4 and float8 columns are stored in sm, and keyed
# in its indexes by PostgreSQL's order.
# 1. Make t16 with each and its indexes, made empty so that none is still
#    building when used, and insert NaN, Infinity, -1e300 and nulls.
# 2. Read them back as written.
# 3. An index on float8 orders NaN last and finds it with = 'NaN'; one on
#    (bool, float4) serves b AND r > 2.
# 4. A unique index on float4 calls -0 and 0 one key.
local_psql -c 'create table t16 (id int4 primary key, b bool, r float4,
      d float8)' \
  -c 'create index t16d on t16 (d)' -c 'create index t16b on t16 (b, r)' \
  -c 'create unique index t16r on t16 (r)' \
  -c "insert into t16 values (1, true, 1.5, 2.25), (2, false, 0, 'NaN'),
      (3, null, 'Infinity', -1e300), (4, true, 3.4e38, 0)" \
  > "$work/floats.out" 2>&1
[ "$(local_psql -c 'select * from t16 order by id' 2>&1 | tr '\n' ,)" = \
  "1|t|1.5|2.25,2|f|0|NaN,3||Infinity|-1e+300,4|t|3.4e+38|0," ] ||
  fail "bool and floats read back"
[ "$(local_psql -c 'set enable_seqscan = off' \
  -c 'select id from t16 order by d' \
  -c "select id from t16 where d = 'NaN'" \
  -c 'select id from t16 where b and r > 2' 2>&1 | tr '\n' ,)" = \
  "SET,3,4,1,2,2,4," ] || fail "bool and float keys"
local_psql -c "insert into t16 values (5, false, '-0', 1)" 2>&1 |
  grep -q 'duplicate key' || fail "-0 and 0 one float key"

# Verify that text, varchar and bytea columns are stored in sm, short and
# long, and keyed in its indexes in the default collation's byte order.
# 1. Make t17 with each and its indexes, made empty so that none is still
#    building when used; insert values empty, of 200 and 5000 bytes, null.
# 2. Read their lengths back; a varchar(5) refuses six characters.
# 3. Indexes on each: = on text and on varchar (by text's opclass) use
#    them; a range and ORDER BY on varchar, and ORDER BY on bytea, come
#    in byte order.
# 4. A unique index on varchar refuses a duplicate.
local_psql -c 'create table t17 (id int4 primary key, t text, v varchar(5),
      b bytea)' \
  -c 'create index t17t on t17 (t)' -c 'create unique index t17v on t17 (v)' \
  -c 'create index t17b on t17 (b)' \
  -c "insert into t17 values (1, 'hello', 'ab', '\x00ff'), (2, '', null, ''),
      (3, repeat('x', 200), 'abcde', '\xdeadbeef'),
      (4, repeat('y', 5000), 'z', null), (5, 'B', 'a', '\x01')" \
  > "$work/text.out" 2>&1
[ "$(local_psql -c 'select id, length(t), v, b from t17 order by id' 2>&1 |
  tr '\n' ,)" = "1|5|ab|\x00ff,2|0||\x,3|200|abcde|\xdeadbeef,4|5000|z|,5|1|a|\x01," ] ||
  fail "text, varchar and bytea read back"
local_psql -c "insert into t17 values (6, 'x', 'toolong', null)" 2>&1 |
  grep -q 'value too long for type character varying(5)' ||
  fail "varchar(5) refuses six"
[ "$(local_psql -c 'set enable_seqscan = off' \
  -c "explain (costs off) select id from t17 where t = 'hello'" \
  -c "explain (costs off) select id from t17 where v = 'ab'" 2>&1 |
  grep -cE 'Index Scan (on|using) t17[tv]')" = 2 ] || fail "text and varchar indexes"
[ "$(local_psql -c 'set enable_seqscan = off' \
  -c "select id from t17 where t = 'hello'" \
  -c "select id from t17 where v > 'a' order by v" \
  -c 'select id from t17 order by b nulls first' 2>&1 | tr '\n' ,)" = \
  "SET,1,1,3,4,4,2,1,5,3," ] || fail "text, varchar and bytea keys"
local_psql -c "insert into t17 values (7, 'q', 'ab', null)" 2>&1 |
  grep -q 'duplicate key' || fail "a unique varchar key"

# Verify that date, time, timestamp, timestamptz, interval and uuid columns
# are stored in sm and keyed in its indexes in PostgreSQL's order.
# 1. Make t18 with each and an index on each, made empty; insert values at
#    the types' ends, infinities and nulls among them.
# 2. Read them back as written, in UTC.
# 3. Each index orders as PostgreSQL does, uuid nulls first when asked;
#    30 days and 1 month are one interval, found together by =.
# 4. A unique uuid index refuses a duplicate.
# 5. An index keyed on interval is refused: sm's keys would tell 30 days
#    from 1 month.
local_psql -c 'create table t18 (id int4 primary key, dt date, tm time,
      ts timestamp, tz timestamptz, iv interval, u uuid)' \
  -c 'create index t18dt on t18 (dt)' -c 'create index t18tm on t18 (tm)' \
  -c 'create index t18ts on t18 (ts)' -c 'create index t18tz on t18 (tz)' \
  -c 'create unique index t18u on t18 (u)' \
  -c "insert into t18 values (1, '2024-01-31', '10:30',
      '2024-01-31 10:30:00.123456', '2024-01-31 10:30+02', '1 day 2 hours',
      'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'),
      (2, '1999-12-31', '00:00', '-infinity', 'infinity', '-1 month',
      '00000000-0000-0000-0000-000000000000'),
      (3, 'infinity', '23:59:59.999999', '2000-01-01', null, '30 days', null),
      (4, '4713-01-01 BC', null, '294276-12-31 23:59:59',
      '1970-01-01 00:00+00', '1 month', 'ffffffff-ffff-ffff-ffff-ffffffffffff')" \
  > "$work/dates.out" 2>&1
[ "$(local_psql -c "set timezone = 'UTC'" -c 'select * from t18 order by id' \
  2>&1 | tr '\n' ,)" = "SET,1|2024-01-31|10:30:00|2024-01-31 10:30:00.123456|2024-01-31 08:30:00+00|1 day 02:00:00|a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11,2|1999-12-31|00:00:00|-infinity|infinity|-1 mons|00000000-0000-0000-0000-000000000000,3|infinity|23:59:59.999999|2000-01-01 00:00:00||30 days|,4|4713-01-01 BC||294276-12-31 23:59:59|1970-01-01 00:00:00+00|1 mon|ffffffff-ffff-ffff-ffff-ffffffffffff," ] ||
  fail "dates, times, intervals and uuids read back"
[ "$(local_psql -c 'set enable_seqscan = off' \
  -c 'select array_agg(id) from (select id from t18 order by dt) x' \
  -c 'select array_agg(id) from (select id from t18 order by tm) x' \
  -c 'select array_agg(id) from (select id from t18 order by ts) x' \
  -c 'select array_agg(id) from (select id from t18 order by tz nulls first) x' \
  -c "select array_agg(id order by id) from t18
      where iv = interval '1 month'" \
  -c 'select array_agg(id) from (select id from t18 order by u nulls first) x' \
  2>&1 | tr '\n' ,)" = \
  "SET,{4,2,1,3},{2,1,3,4},{2,3,1,4},{3,4,1,2},{3,4},{3,2,1,4}," ] ||
  fail "date, time, interval and uuid keys"
local_psql -c "insert into t18 values (5, null, null, null, null, null,
    'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11')" 2>&1 |
  grep -q 'duplicate key' || fail "a unique uuid key"
local_psql -c 'create index t18iv on t18 (iv)' 2>&1 |
  grep -q 'an index key of type interval is not supported' ||
  fail "an interval key refused"

# Verify that numeric refuses what a decimal of 38 digits has no value for,
# as PostgreSQL words each error.
# 1. NaN and infinity are not values; text past 38 digits is refused.
# 2. Division by zero, the root of a negative, the logarithm of zero.
# 3. A precision past 38, a field overflow, a result past 38 digits.
for c in "'NaN'::numeric|invalid input syntax for type numeric" \
  "'Infinity'::numeric|invalid input syntax for type numeric" \
  "'0.123456789012345678901234567890123456789'::numeric|value overflows numeric format" \
  "1 / 0::numeric|division by zero" \
  "sqrt(-1::numeric)|cannot take square root of a negative number" \
  "ln(0::numeric)|cannot take logarithm of zero" \
  "1::numeric(50,2)|NUMERIC precision 50 must be between 1 and 38" \
  "12345.6::numeric(5,2)|numeric field overflow" \
  "99999999999999999999999999999999999999::numeric * 10|value overflows numeric format"; do
  local_psql -c "select ${c%%|*}" 2>&1 | grep -q "${c#*|}" ||
    fail "numeric refuses ${c%%|*}"
done

# Verify that numeric columns are stored in sm as decimals at their scale,
# indexed, keyed and analyzed.
# 1. Make t20 with numeric(10,2), numeric(30,5) and a bare numeric, which is
#    numeric(18,3), and indexes; values are rounded to the column's scale.
# 2. A value past the precision is refused, as an assignment is.
# 3. Index scans find = and ranges whose keys have more places than the
#    column, and keys past its range.
# 4. A unique numeric(30,5) index refuses a value that rounds to a taken one.
# 5. ANALYZE gives common values at the column's scale, and = is estimated.
local_psql -c 'create table t20 (id int4 primary key, a numeric(10,2),
      b numeric(30,5), c numeric)' \
  -c 'create index t20a on t20 (a)' -c 'create unique index t20b on t20 (b)' \
  -c 'insert into t20 values (1, 1.5, 123456789012345678901234.12345, 2.5),
      (2, -0.004, -1, 1.23456), (3, null, 0, null), (4, 1.51, 1.51, 0.0005)' \
  > "$work/numeric.out" 2>&1
[ "$(local_psql -c 'select * from t20 order by id' 2>&1 | tr '\n' ,)" = \
  "1|1.50|123456789012345678901234.12345|2.500,2|0.00|-1.00000|1.235,3||0.00000|,4|1.51|1.51000|0.001," ] ||
  fail "numeric columns read back"
local_psql -c 'insert into t20 values (5, 123456789.5, 5, 5)' 2>&1 |
  grep -q 'numeric field overflow' || fail "numeric column precision"
[ "$(local_psql -c 'set enable_seqscan = off' \
  -c 'select array_agg(id) from t20 where a = 1.5' \
  -c 'select array_agg(id) from t20 where a = 1.505' \
  -c 'select array_agg(id order by a) from t20 where a <= 1.505' \
  -c 'select array_agg(id order by a) from t20 where a > -1e30' \
  -c 'select array_agg(id order by b) from t20 where b >= 1.500001' 2>&1 |
  tr '\n' ,)" = "SET,{1},,{2,1},{2,1,4},{4,1}," ] || fail "numeric keys"
local_psql -c 'insert into t20 values (6, 0, 1.509999999, 0)' 2>&1 |
  grep -q 'duplicate key' || fail "a unique numeric key"
local_psql -c 'create table t21 (a numeric(10,2))' \
  -c 'insert into t21 select (g % 10) / 4.0 from generate_series(1, 5000) g' \
  -c 'analyze t21' >> "$work/numeric.out" 2>&1
[ "$(local_psql -c "select (most_common_vals::text[])[1] from pg_stats('t21')" \
  2>&1)" = "0.00" ] && [ "$(rows_of 'select * from t21 where a = 0.5')" = 500 ] ||
  fail "numeric statistics"

# Verify that a column's DEFAULT is kept in sm and fills what an INSERT or
# UPDATE leaves to it, in later sessions too.
# 1. Make t22 with constant, expression, now() and numeric defaults.
# 2. Insert naming one column, with DEFAULT, and DEFAULT VALUES, each in a
#    session of its own; read the defaults back.
# 3. UPDATE ... SET col = DEFAULT gives the default again.
# 4. A default of the wrong type, one naming a column, and one with a
#    subquery are refused, as PostgreSQL refuses them.
local_psql -c "create table t22 (id int4, n int4 default 7,
      t text default 'x' || 'y', ts timestamptz default now(),
      r numeric(10,2) default 1.555)" > "$work/default.out" 2>&1
local_psql -c 'insert into t22 (id) values (1)' >> "$work/default.out" 2>&1
local_psql -c "insert into t22 values (2, default, 'z', default, default)" \
  >> "$work/default.out" 2>&1
local_psql -c 'insert into t22 default values' >> "$work/default.out" 2>&1
[ "$(local_psql -c 'select id, n, t, ts is not null, r from t22
      order by id nulls first' 2>&1 | tr '\n' ,)" = \
  "|7|xy|t|1.56,1|7|xy|t|1.56,2|7|z|t|1.56," ] || fail "column defaults"
[ "$(local_psql -c 'update t22 set n = 0, t = default where id = 2' \
  -c 'update t22 set n = default where id = 2' \
  -c 'select n, t from t22 where id = 2' 2>&1 | tail -1)" = "7|xy" ] ||
  fail "UPDATE to DEFAULT"
for c in "default 'abc'|invalid input syntax for type integer" \
  "default b|cannot use column reference in DEFAULT expression" \
  "default (select 1)|cannot use subquery in DEFAULT expression"; do
  local_psql -c "create table t23 (a int4 ${c%%|*})" 2>&1 |
    grep -q "${c#*|}" || fail "a default refused: ${c%%|*}"
done

# Verify that an INSERT naming its columns fills the others with their
# types' defaults, null for every built-in type.
# 1. Insert naming two columns of t19, then three in another order, then
#    from a SELECT.
# 2. Read the rows back with nulls where nothing was named.
# 3. Leaving out a NOT NULL column, or giving DEFAULT VALUES, is refused.
local_psql -c 'create table t19 (a int4, b int4, t text, c int8 not null)' \
  -c 'insert into t19 (a, c) values (1, 10)' \
  -c "insert into t19 (c, a, t) values (20, 2, 'two')" \
  -c 'insert into t19 (a, c) select g, g * 10 from generate_series(4, 5) g' \
  > "$work/insertcols.out" 2>&1
[ "$(local_psql -c 'select * from t19 order by a' 2>&1 | tr '\n' ,)" = \
  "1|||10,2||two|20,4|||40,5|||50," ] || fail "an insert naming its columns"
[ "$(local_psql -c 'insert into t19 (a) values (3)' \
  -c 'insert into t19 default values' 2>&1 |
  grep -c 'null value in column "c"')" = 2 ] ||
  fail "a NOT NULL column left out"

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

# Verify that pg_hba.conf and pg_ident.conf are gone, with what read them.
# 1. Confirm hba_file and ident_file are not settings any more.
# 2. Confirm the functions that showed the files' rules are not there.
for guc in hba_file ident_file; do
  local_psql -c "show $guc" 2>&1 |
    grep -q 'unrecognized configuration parameter' || fail "setting $guc"
done
for f in pg_hba_file_rules pg_ident_file_mappings; do
  local_psql -c "select * from $f()" 2>&1 |
    grep -q "function $f() does not exist" || fail "function $f"
done

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

# Verify that UPDATE and DELETE write a table in sm, with its indexes, in
# their transaction; and that under READ COMMITTED a row another transaction
# changed first is read again and checked again, as PostgreSQL does.
# 1. Fill tw1, keyed by a, with four rows.  Update one by its key, then
#    every row; move two keys with RETURNING and find them by the index;
#    delete one by its key with RETURNING: the rows left are as expected.
# 2. An update to a key that is taken is refused, and one to NULL in a NOT
#    NULL column; neither changes a row.
# 3. A delete rolled back, and an update rolled back to a savepoint, leave
#    the rows as they were.
# 4. A temporary table takes an update and a delete.
# 5. One session updates row 3 and holds its transaction open.  A second, at
#    READ COMMITTED, updates the same row and waits; when the first commits,
#    the second's change is made to the first's row.  The same, the second
#    setting another column: the first's change to b is kept.
# 6. The same, the second's WHERE no longer true of the first's row: it
#    updates none.  And a DELETE ... RETURNING gives the first's row.
# 7. The same at REPEATABLE READ: the second fails, and can be retried.
# 8. SELECT ... FOR UPDATE is refused.
local_psql -c 'create table tw1 (a int4 primary key, b int4 not null)' \
  -c 'insert into tw1 values (1, 10), (2, 20), (3, 30), (4, 40)' \
  > "$work/write.out" 2>&1
[ "$(local_psql -c 'update tw1 set b = b + 1 where a = 2' \
  -c 'select b from tw1 where a = 2' -c 'update tw1 set b = b + 100' \
  -c 'select b from tw1 order by a' 2>&1 | tr '\n' ' ')" = \
  "UPDATE 1 21 UPDATE 4 110 121 130 140 " ] || fail "update"
[ "$(local_psql -c 'update tw1 set a = a + 10 where a < 3 returning a, b' \
  -c 'select b from tw1 where a = 12' -c 'select b from tw1 where a = 2' \
  -c 'delete from tw1 where a = 11 returning a, b' \
  -c 'select a from tw1 order by a' 2>&1 | tr '\n' ' ')" = \
  "11|110 12|121 UPDATE 2 121 11|110 DELETE 1 3 4 12 " ] ||
  fail "update a key, delete"
local_psql -c 'update tw1 set a = 4 where a = 3' 2>&1 |
  grep -q 'duplicate key value violates a unique index on "tw1"' ||
  fail "update to a taken key"
local_psql -c 'update tw1 set b = null where a = 3' 2>&1 |
  grep -q 'violates not-null constraint' || fail "update to null"
[ "$(local_psql -c 'begin' -c 'delete from tw1' -c 'select count(*) from tw1' \
  -c 'rollback' -c 'begin' -c 'savepoint s' -c 'update tw1 set b = 0' \
  -c 'rollback to s' -c 'commit' -c 'select a, b from tw1 order by a' 2>&1 |
  grep -v '^[A-Z]' | tr '\n' ' ')" = "0 3|130 4|140 12|121 " ] ||
  fail "update and delete rolled back"
[ "$(local_psql -c 'create temp table tw2 (a int4, b int4)' \
  -c 'insert into tw2 values (1, 1), (2, 2)' \
  -c 'update tw2 set b = 9 where a = 1' -c 'delete from tw2 where a = 2' \
  -c 'select a, b from tw2' 2>&1 | tail -1)" = "1|9" ] ||
  fail "update and delete of a temporary table"

# Hold row 3 of tw1 updated (b + 1000) in a session of its own, run the
# statements given in another, which waits, commit the first, and print what
# the other said, on one line.
after_update() {
  mkfifo "$work/write.in"
  local_psql < "$work/write.in" > "$work/write_held.out" 2>&1 &
  local holder=$!
  exec 4> "$work/write.in"
  echo "begin; update tw1 set b = b + 1000 where a = 3; select 'held';" >&4
  for _ in $(seq 50); do
    grep -qx held "$work/write_held.out" && break
    sleep 0.1
  done
  local_psql "$@" > "$work/write_other.out" 2>&1 &
  local other=$!
  sleep 1
  echo "commit;" >&4
  exec 4>&-
  wait $holder
  wait $other
  rm "$work/write.in"
  tr '\n' ' ' < "$work/write_other.out"
}
[ "$(after_update -c 'update tw1 set b = b + 1 where a = 3 returning b')" = \
  "1131 UPDATE 1 " ] || fail "update after a concurrent update"
[ "$(after_update -c 'update tw1 set a = a where a = 3 returning b')" = \
  "2131 UPDATE 1 " ] || fail "update keeps a concurrent update's column"
[ "$(after_update -c 'update tw1 set b = 0 where a = 3 and b < 2000')" = \
  "UPDATE 0 " ] || fail "update rechecks its WHERE"
[ "$(local_psql -c 'select b from tw1 where a = 3')" = 3131 ] ||
  fail "update that no longer matched"
[ "$(after_update -c 'delete from tw1 where a = 3 and b < 2000 returning b')" = \
  "DELETE 0 " ] || fail "delete rechecks its WHERE"
after_update -v VERBOSITY=verbose -c 'begin isolation level repeatable read' \
  -c 'select count(*) from tw1' -c 'update tw1 set b = b + 1 where a = 3' |
  grep -q 'ERROR:  40001' || fail "update at repeatable read"
[ "$(after_update -c 'delete from tw1 where a = 3 returning b')" = \
  "6131 DELETE 1 " ] || fail "delete after a concurrent update"
local_psql -c 'select a from tw1 for update' 2>&1 |
  grep -q 'locking a row of table "tw1" is not supported yet' ||
  fail "for update refused"
local_psql -c 'drop table tw1' > /dev/null 2>&1

# Verify that COPY reads a table in sm and writes one, its indexes with it.
# 1. Make tc1, keyed by a, with an index on b, and COPY three rows into it
#    from a file, one with a null: they are there, and both indexes find them.
# 2. COPY tc1 TO STDOUT gives the rows back as they came, and a column list
#    with CSV and a header gives those columns.
# 3. psql's \copy loads rows from its input, and writes the table to a file.
# 4. A file with a row whose key is taken, after a row that is new, fails,
#    and leaves neither row; so does one with a null in a NOT NULL column.
# 5. A COPY rolled back leaves no row.
# 6. The binary format written from tc1 loads into another table, and the
#    two agree.
# 7. A temporary table takes a COPY, and 5000 rows load and are all counted.
printf '1\t10\tone\n2\t20\t\\N\n3\t30\tthree\n' > "$work/copy.tsv"
printf '4\t40\tfour\n1\t99\tdup\n' > "$work/copy_dup.tsv"
printf '7\t\\N\tseven\n' > "$work/copy_null.tsv"
local_psql -c 'create table tc1 (a int4 primary key, b int4 not null, c text)' \
  -c 'create index tc1_b on tc1 (b)' > "$work/copy.out" 2>&1
[ "$(local_psql -c "copy tc1 from '$work/copy.tsv'" \
  -c 'select c from tc1 where a = 3' -c 'select a from tc1 where b = 20' \
  -c 'select count(*) from tc1 where c is null' 2>&1 | tr '\n' ' ')" = \
  "COPY 3 three 2 1 " ] || fail "copy from a file"
local_psql -c 'copy tc1 to stdout' 2> "$work/copy.err" |
  cmp -s - "$work/copy.tsv" || fail "copy to stdout"
[ "$(local_psql -c 'copy tc1 (a, c) to stdout with (format csv, header)' 2>&1 |
  tr '\n' ' ')" = "a,c 1,one 2, 3,three " ] || fail "copy columns as csv"
printf '5\t50\tfive\n6\t60\tsix\n' |
  local_psql -c '\copy tc1 from stdin' >> "$work/copy.out" 2>&1
local_psql -c "\\copy tc1 to '$work/copy_out.tsv'" >> "$work/copy.out" 2>&1
[ "$(wc -l < "$work/copy_out.tsv")" = 5 ] &&
  grep -qx "$(printf '6\t60\tsix')" "$work/copy_out.tsv" || fail "psql's copy"
local_psql -c "copy tc1 from '$work/copy_dup.tsv'" 2>&1 |
  grep -q 'duplicate key value violates a unique index on "tc1"' ||
  fail "copy of a taken key"
local_psql -c "copy tc1 from '$work/copy_null.tsv'" 2>&1 |
  grep -q 'violates not-null constraint' || fail "copy of a null"
[ "$(local_psql -c 'select count(*) from tc1' \
  -c 'select count(*) from tc1 where a in (4, 7)' | tr '\n' ' ')" = "5 0 " ] ||
  fail "a failed copy left rows"
printf '8\t80\teight\n' > "$work/copy_one.tsv"
[ "$(local_psql -c 'begin' -c "copy tc1 from '$work/copy_one.tsv'" \
  -c 'select count(*) from tc1' -c 'rollback' -c 'select count(*) from tc1' \
  2>&1 | grep -v '^[A-Z]' | tr '\n' ' ')" = "6 5 " ] || fail "copy rolled back"
[ "$(local_psql -c "copy tc1 to '$work/copy.bin' with (format binary)" \
  -c 'create table tc2 (a int4 primary key, b int4 not null, c text)' \
  -c "copy tc2 from '$work/copy.bin' with (format binary)" \
  -c 'select count(*), sum(b) from tc2' \
  -c 'select count(*) from tc1 join tc2 using (a, b, c)' 2>&1 |
  grep -v '^[A-Z]' | tr '\n' ' ')" = "5|170 4 " ] || fail "copy in binary"
seq 1 5000 | awk '{ print $1 "\t" $1 * 2 "\trow" $1 }' > "$work/copy_big.tsv"
[ "$(local_psql -c 'create temp table tc3 (a int4 primary key, b int4, c text)' \
  -c "copy tc3 from '$work/copy_big.tsv'" \
  -c 'select count(*), max(b) from tc3' -c 'select c from tc3 where a = 4321' \
  2>&1 | tr '\n' ' ')" = "CREATE TABLE COPY 5000 5000|10000 row4321 " ] ||
  fail "copy into a temporary table"
local_psql -c 'drop table tc1' -c 'drop table tc2' > /dev/null 2>&1

# Verify that a temporary table is its session's alone, in sm: it is found
# before a table of its name, follows the session's transactions, takes
# writes in a read-only one, and does what its ON COMMIT asks.
# 1. In one session create temporary tt1 with a key and two rows; read it by
#    name, through pg_temp, and joined with table tp1; then create a temporary
#    tp1, which hides the table, still read as public.tp1.
# 2. In one session roll back an insert into a temporary table, roll back
#    another to a savepoint, and commit a third in a read-only transaction:
#    it holds its first row and the third; an insert into a table in a
#    read-only transaction is refused.
# 3. ON COMMIT DROP: the table is there in its transaction and gone after.
#    ON COMMIT DELETE ROWS: its rows are there in a transaction, gone after.
# 4. While a session holds temporary tt2 with a row, another finds no tt2,
#    and makes its own, empty; the first still counts its row.
# 5. DISCARD TEMP drops a session's temporary tables; ANALYZE skips one with
#    a warning; UNLOGGED, ON COMMIT on a table, and a temporary table in
#    public are refused.
local_psql -c 'create table tp1 (a int4)' -c 'insert into tp1 values (1)' \
  > "$work/temp.out" 2>&1
[ "$(local_psql -c 'create temporary table tt1 (a int4 primary key, b int4)' \
  -c 'insert into tt1 values (1, 10), (2, 20)' \
  -c 'select b from tt1 where a = 2' -c 'select b from pg_temp.tt1 where a = 1' \
  -c 'select tt1.b from tt1, tp1 where tt1.a = tp1.a' \
  -c 'create temp table tp1 (z int4)' -c 'select count(*) from tp1' \
  -c 'select a from public.tp1' 2>&1 | grep -v '^[A-Z]' | tr '\n' ' ')" = \
  "20 10 10 0 1 " ] || fail "temporary table"
local_psql -c 'select 1 from tt1' 2>&1 | grep -q 'relation "tt1" does not exist' ||
  fail "temporary table outlived its session"
[ "$(local_psql -c 'create temp table tt1 (a int4)' \
  -c 'insert into tt1 values (1)' \
  -c 'begin' -c 'insert into tt1 values (2)' -c 'rollback' \
  -c 'begin' -c 'savepoint s' -c 'insert into tt1 values (3)' \
  -c 'rollback to s' -c 'commit' \
  -c 'begin read only' -c 'insert into tt1 values (4)' -c 'commit' \
  -c 'select a from tt1 order by a' 2>&1 | grep -v '^[A-Z]' |
  tr '\n' ' ')" = "1 4 " ] || fail "temporary table in transactions"
local_psql -c 'begin read only' -c 'insert into tp1 values (2)' 2>&1 |
  grep -q 'cannot execute INSERT in a read-only transaction' ||
  fail "read-only insert into a table"
local_psql -c 'begin' -c 'create temp table tt1 (a int4) on commit drop' \
  -c 'insert into tt1 values (7)' -c 'select a from tt1' -c 'commit' \
  -c 'select a from tt1' > "$work/temp_drop.out" 2>&1
grep -qx 7 "$work/temp_drop.out" &&
  grep -q 'relation "tt1" does not exist' "$work/temp_drop.out" ||
  fail "on commit drop"
[ "$(local_psql -c 'create temp table tt1 (a int4) on commit delete rows' \
  -c 'begin' -c 'insert into tt1 values (1), (2)' \
  -c 'select count(*) from tt1' -c 'commit' -c 'select count(*) from tt1' \
  2>&1 | grep -v '^[A-Z]' | tr '\n' ' ')" = "2 0 " ] ||
  fail "on commit delete rows"
mkfifo "$work/temp.in"
local_psql < "$work/temp.in" > "$work/temp_own.out" 2>&1 &
owner=$!
exec 4> "$work/temp.in"
echo "create temp table tt2 (a int4); insert into tt2 values (1);" >&4
echo "select 'made';" >&4
for _ in $(seq 50); do
  grep -qx made "$work/temp_own.out" && break
  sleep 0.1
done
local_psql -c 'select 1 from tt2' 2>&1 |
  grep -q 'relation "tt2" does not exist' || fail "temporary table seen"
[ "$(local_psql -c 'create temp table tt2 (a int4)' \
  -c 'select count(*) from tt2' 2>&1 | tail -1)" = 0 ] ||
  fail "temporary table of the same name"
echo "select count(*) + 40 from tt2;" >&4
exec 4>&-
wait $owner
grep -qx 41 "$work/temp_own.out" || fail "temporary table kept"
local_psql -c 'create temp table tt1 (a int4)' -c 'discard temp' \
  -c 'select 1 from tt1' 2>&1 | grep -q 'relation "tt1" does not exist' ||
  fail "discard temp"
local_psql -c 'create temp table tt1 (a int4)' -c 'analyze tt1' 2>&1 |
  grep -q 'statistics are not kept for a temporary table yet' ||
  fail "analyze a temporary table"
local_psql -c 'create unlogged table tu1 (a int4)' 2>&1 |
  grep -q 'supports only' || fail "unlogged table"
local_psql -c 'create table tu1 (a int4) on commit drop' 2>&1 |
  grep -q 'ON COMMIT can only be used on temporary tables' ||
  fail "on commit on a table"
local_psql -c 'create temp table public.tu1 (a int4)' 2>&1 |
  grep -q 'cannot create temporary relation in non-temporary schema' ||
  fail "temporary table in public"
local_psql -c 'drop table tp1' > /dev/null 2>&1

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
