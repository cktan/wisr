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

# Wait for the server PID to log to LOG that it accepts connections: up to
# thirty seconds, as a loaded machine (make -j4 tsan) has taken thirteen.
server_ready() {
  for _ in $(seq 300); do
    grep -q "ready to accept connections" "$2" && return 0
    kill -0 "$1" 2> /dev/null || return 1
    sleep 0.1
  done
  return 1
}

# Run psql on the local socket with the arguments given, bare output.
local_psql() {
  psql -X -A -t -h "$work" -p "$port" -d postgres "$@"
}

# Run psql over TCP with the password and the arguments given.
tcp_psql() {
  PGPASSWORD=$1 psql -X -A -t -h 127.0.0.1 -p "$port" -d postgres "${@:2}"
}

# A held session: a psql reading statements from a fifo, so a transaction
# stays open across steps; its output is in $work/NAME.out.
declare -A held_pid held_fd

# Start the held session NAME.
held_start() {
  local fd
  mkfifo "$work/$1.in"
  # Without the other held sessions' fifos, whose end it would keep open.
  (
    for fd in "${held_fd[@]}"; do exec {fd}>&-; done
    exec psql -X -A -t -h "$work" -p "$port" -d postgres
  ) < "$work/$1.in" > "$work/$1.out" 2>&1 &
  held_pid[$1]=$!
  exec {fd}> "$work/$1.in"
  held_fd[$1]=$fd
}

# Send the statements SQL to the held session NAME.
held_send() {
  echo "$2" >&"${held_fd[$1]}"
}

# Wait, five seconds at most, for the held session NAME to print LINE.
held_wait() {
  for _ in $(seq 50); do
    grep -qxF -- "$2" "$work/$1.out" && return 0
    sleep 0.1
  done
  return 1
}

# End the held session NAME, once it has run what it was sent.
held_end() {
  local fd=${held_fd[$1]}
  exec {fd}>&-
  wait "${held_pid[$1]}"
}

# A statement in the background, in a session of its own; its output is in
# $work/NAME.out.
declare -A bg_pid

# Start psql with the arguments given as the background statement NAME.
bg_start() {
  local name=$1
  shift
  local_psql "$@" > "$work/$name.out" 2>&1 &
  bg_pid[$name]=$!
}

# Whether the background statement NAME is still waiting after a second.
bg_waiting() {
  sleep 1
  kill -0 "${bg_pid[$1]}" 2> /dev/null
}

# Wait, ten seconds at most, for the background statement NAME to end;
# false, and it is killed, if it does not.
bg_end() {
  for _ in $(seq 100); do
    kill -0 "${bg_pid[$1]}" 2> /dev/null || break
    sleep 0.1
  done
  if kill -0 "${bg_pid[$1]}" 2> /dev/null; then
    kill -KILL "${bg_pid[$1]}"
    wait "${bg_pid[$1]}"
    return 1
  fi
  wait "${bg_pid[$1]}"
}

# Whether the psql OTHER, whose first statement printed its session's pid
# ("pid N") into FILE, waits behind another session or runs to its end:
# "waited" once pg_blocking_pids() names a blocker of it, "passed" if it ends
# first, "stuck" if neither within ten seconds.  Decided by what the server
# says, not by how long the statement takes, which a slow build stretches.
wait_state() {
  local other=$1 file=$2 pid=
  for _ in $(seq 100); do
    pid=$(sed -n 's/^pid \([0-9]*\)$/\1/p' "$file")
    [ -n "$pid" ] || ! kill -0 "$other" 2> /dev/null && break
    sleep 0.1
  done
  for _ in $(seq 100); do
    if ! kill -0 "$other" 2> /dev/null; then
      echo passed
      return
    fi
    if [ -n "$pid" ] && [ "$(local_psql -c \
      "select pg_blocking_pids($pid) <> '{}'")" = t ]; then
      echo waited
      return
    fi
    sleep 0.1
  done
  echo stuck
}

# Verify that the server starts on what initdb made, and takes connections.
# 1. Make a data directory whose superuser has a password.
# 2. Start the server on it.
# 3. Wait until it says it is ready, ten seconds at most.
echo "sesame" > "$work/pw"
build/initdb --pwfile="$work/pw" "$work/data" > "$work/initdb.out" 2>&1 ||
  fail "initdb"
# A temporary file a prior run left, for startup to remove (checked below).
mkdir -p "$work/data/base/pgsql_tmp"
echo leftover > "$work/data/base/pgsql_tmp/pgsql_tmp99.0"
# The soft limit on open files is lowered, as a service's often is, for
# the server to raise (checked below). A session's share of real fds is
# max_files_per_process, made small for a join to need more (below).
(ulimit -Sn 1024 && exec $digres -D "$work/data" -k "$work" -p "$port" \
  -c authentication_timeout=1 -c max_files_per_process=64) \
  > "$work/log" 2>&1 &
server=$!
server_ready $server "$work/log" || fail "start"

# Verify that startup removes the temporary files a prior run left, in
# base/pgsql_tmp alone: there is no other tablespace to look in.
# 1. Before the start above, a leftover temporary file was planted.
# 2. Confirm it is gone, and the log names no pg_tblspc.
[ -e "$work/data/base/pgsql_tmp/pgsql_tmp99.0" ] &&
  fail "a leftover temporary file is removed"
grep -q pg_tblspc "$work/log" && fail "startup looks for no tablespace"

# Verify that the server raises its soft limit on open files to the hard
# limit, since every session is a thread under the one limit.
# 1. The server was started above with the soft limit at 1024.
# 2. Read its soft and hard limits from /proc.
# 3. Confirm the soft limit is now the hard one.
read -r soft hard < <(awk '/^Max open files/ { print $4, $5 }' \
  "/proc/$server/limits")
[ "$soft" = "$hard" ] || fail "open files limit ($soft, not $hard)"

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

# Verify that VACUUM succeeds, with options sm has nothing more to do for,
# and that it refuses a table that is not there, as PostgreSQL does.
# 1. Run VACUUM, with and without options, then a statement after them.
# 2. Confirm psql exited with success and reported no error.
# 3. Confirm the statement after them answered.
# 4. VACUUM of a table that is not there is an error.
local_psql -c 'vacuum' -c 'vacuum (full, freeze, analyze)' \
  -c 'select 4 + 4' > "$work/vacuum.out" 2>&1 || fail "vacuum succeeds"
grep -q "ERROR" "$work/vacuum.out" && fail "vacuum reported an error"
local_psql -c 'vacuum no_such_table' 2>&1 |
  grep -q 'ERROR:  relation "no_such_table" does not exist' ||
  fail "vacuum of no such table"
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

# Verify that what needs PostgreSQL's xid or oid counter is refused, since
# digres assigns no transaction ids and sm gives the oids.
# 1. Each of pg_current_snapshot(), txid_current_snapshot(),
#    pg_xact_status(), age(xid), pg_current_xact_id() and txid_current()
#    fails: digres assigns no transaction ids. pg_current_xact_id_if_assigned()
#    and txid_current_if_assigned() are null, as none is ever assigned.
# 2. pg_stop_making_pinned_objects(), and CREATE DOMAIN, TYPE and CAST,
#    which draw an oid for their catalog row, fail: digres has no catalog
#    tables.
# 3. Confirm a statement after them answers.
for sql in "select pg_current_snapshot()" "select txid_current_snapshot()" \
  "select pg_xact_status('5'::xid8)" "select age('5'::xid)" \
  "select pg_current_xact_id()" "select txid_current()"; do
  local_psql -c "$sql" 2>&1 | grep -q 'digres assigns no transaction ids' ||
    fail "refused: $sql"
done
[ "$(local_psql -c 'select pg_current_xact_id_if_assigned() is null,
                           txid_current_if_assigned() is null')" = "t|t" ] ||
  fail "no xid is assigned"
for sql in "select pg_stop_making_pinned_objects()" \
  "create domain d1 as int4" "create type e1 as enum ('a')" \
  "create cast (int4 as text) with inout"; do
  local_psql -c "$sql" > "$work/counters.out" 2>&1
  grep -q 'digres has no catalog tables' "$work/counters.out" ||
    fail "refused: $sql ($(head -1 "$work/counters.out"))"
done
[ "$(local_psql -c 'select 6 + 6' 2> "$work/counters.err")" = 12 ] ||
  fail "statement after the counters' refusals"

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
local_psql -c 'merge into t1 using (select 1 as x) s on t1.a = s.x
  when not matched then insert values (1, 1, 1)' >> "$work/rows.out" 2>&1
grep -q 'MERGE does not write to table "t1" yet' "$work/rows.out" ||
  fail "merge refused"
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

# Verify that a cursor declared before a savepoint reads on after a rollback
# to it, and reads the rows there were when it was declared.
# 1. Fill cr1 with 1 to 100, and cr2, indexed on a, with the same.
# 2. Over cr1, fetch 2 inside a savepoint, roll back to it, fetch 2 more and
#    close the cursor: 1 to 4.
# 3. The same over cr2 by its index, from 50, the cursor closed by the
#    commit: 51 to 54.
# 4. Declare a cursor of cr1's rows past 98, insert 101, fetch them all: 99
#    and 100, not 101.
# 5. A nested loop whose inner scan of cr1 begins again inside a savepoint
#    fails at the cursor's next fetch after a rollback to it, and the server
#    answers after.
local_psql > /dev/null 2>&1 <<'SQL'
create table cr1 (a int4);
insert into cr1 select generate_series(1, 100);
create table cr2 (a int4, b int4);
create index cr2_a on cr2 (a);
insert into cr2 select g, g from generate_series(1, 100) g;
SQL
[ "$(local_psql 2>&1 <<'SQL' | grep -v '^[A-Z]' | tr '\n' ' '
begin;
declare c cursor for select a from cr1;
savepoint s;
fetch 2 from c;
rollback to s;
fetch 2 from c;
close c;
commit;
SQL
)" = "1 2 3 4 " ] || fail "cursor over a rollback to a savepoint"
[ "$(local_psql 2>&1 <<'SQL' | grep -v '^[A-Z]' | tr '\n' ' '
set enable_seqscan = off;
begin;
declare c cursor for select b from cr2 where a > 50 order by a;
savepoint s;
fetch 2 from c;
rollback to s;
fetch 2 from c;
commit;
SQL
)" = "51 52 53 54 " ] || fail "index cursor over a rollback to a savepoint"
[ "$(local_psql 2>&1 <<'SQL' | grep -v '^[A-Z]' | tr '\n' ' '
begin;
declare c cursor for select a from cr1 where a > 98;
insert into cr1 values (101);
fetch all from c;
rollback;
SQL
)" = "99 100 " ] || fail "cursor reads as declared"
local_psql > "$work/cursor_rescan.out" 2>&1 <<'SQL'
set enable_hashjoin = off;
set enable_mergejoin = off;
set enable_material = off;
begin;
declare c cursor for select x.a from cr1 x join cr1 y on x.a = y.a
  where x.a <= 3;
fetch 1 from c;
savepoint s;
fetch 1 from c;
rollback to s;
fetch 1 from c;
SQL
grep -q 'closed by a rollback' "$work/cursor_rescan.out" &&
  [ "$(local_psql -c 'select count(*) from cr1' 2>&1)" = 100 ] ||
  fail "cursor rescanned in a rolled back savepoint"

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
  "comment on publication p is 'x'" "alter publication p rename to q" \
  "alter publication p owner to current_user"; do
  local_psql -c "$sql" 2>&1 | grep -q 'publications are not supported' ||
    fail "publication refused: $sql"
done
for sql in "create subscription s connection 'x' publication p" \
  "alter subscription s disable" "drop subscription s" \
  "alter subscription s rename to q"; do
  local_psql -c "$sql" 2>&1 | grep -q 'subscriptions are not supported' ||
    fail "subscription refused: $sql"
done

# Verify that a role cannot be given REPLICATION, as digres has no
# replication, while NOREPLICATION, which pg_dumpall writes, passes it by.
# 1. CREATE ROLE and ALTER ROLE with REPLICATION each fail, saying so.
# 2. With NOREPLICATION neither fails on the attribute (it fails later, as
#    digres has no roles to make yet).
# 3. ALTER TABLE ... REPLICA IDENTITY on a table made for it is refused.
for sql in "create role r replication" "alter role current_user replication"; do
  local_psql -c "$sql" 2>&1 |
    grep -q 'the REPLICATION attribute is not supported' ||
    fail "REPLICATION refused: $sql"
done
for sql in "create role r noreplication" \
  "alter role current_user noreplication"; do
  local_psql -c "$sql" 2>&1 | grep -q 'REPLICATION attribute' &&
    fail "NOREPLICATION passes: $sql"
done
local_psql -c "create table ri (a int4 primary key)" \
  -c "alter table ri replica identity full" -c "drop table ri" 2>&1 |
  grep -q 'ERROR:  ALTER TABLE supports only' || fail "REPLICA IDENTITY refused"

# Verify that LOCK TABLE takes sm's table lock: SHARE lets another session
# read and keeps it from writing; ACCESS EXCLUSIVE keeps it from reading;
# and NOWAIT fails at once on a lock held in a mode that conflicts.
# 1. In a session of its own, lock t2 in SHARE mode and hold it.
# 2. From another, a scan of t2 answers; an insert with statement_timeout =
#    500 times out. LOCK ... ACCESS SHARE NOWAIT is granted, and ROW
#    EXCLUSIVE NOWAIT fails with 55P03.
# 3. Commit, lock t2 in ACCESS EXCLUSIVE mode, and hold it: a scan times out,
#    and LOCK ... ACCESS SHARE NOWAIT fails with 55P03, without waiting.
#    ANALYZE (SKIP_LOCKED) t2 passes t2 over with a warning, and with no
#    table named passes it over in silence; neither waits.
# 4. Commit: LOCK t2 NOWAIT, ACCESS EXCLUSIVE, is granted.
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
[ "$(timeout 10 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -c 'begin' -c 'lock table t2 in access share mode nowait' \
  -c "select 'granted'" -c 'commit' 2>&1 | grep -c granted)" = 1 ] ||
  fail "nowait under a share lock, granted"
timeout 10 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -v VERBOSITY=sqlstate -c 'begin' \
  -c 'lock table t2 in row exclusive mode nowait' > "$work/locker5.out" 2>&1
grep -qx 'ERROR:  55P03' "$work/locker5.out" ||
  fail "nowait under a share lock, refused"
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
timeout 10 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -c 'begin' -c 'lock table t2 in access share mode nowait' \
  > "$work/locker4.out" 2>&1
grep -qx 'ERROR:  could not obtain lock on relation "t2"' \
  "$work/locker4.out" || fail "nowait under an exclusive lock"
timeout 10 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -c 'analyze (skip_locked) t2' > "$work/locker6.out" 2>&1
grep -qx 'WARNING:  skipping analyze of "t2" --- lock not available' \
  "$work/locker6.out" || fail "analyze skip_locked of a held table"
[ "$(timeout 10 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -c 'analyze (skip_locked)' 2>&1)" = ANALYZE ] ||
  fail "analyze skip_locked of every table"
echo "commit;" >&4
exec 4>&-
wait $locker
[ "$(local_psql -c 'begin' -c 'lock table t2 nowait' -c "select 'granted'" \
  -c 'commit' 2>&1 | grep -c granted)" = 1 ] || fail "nowait, granted"

# Verify that ANALYZE (SKIP_LOCKED) passes over a table another
# transaction is analyzing, where a plain ANALYZE waits for it, as each
# does in PostgreSQL.
# 1. In a held session, analyze t2 and keep the transaction open.
# 2. ANALYZE (SKIP_LOCKED) t2 warns that it skips t2, without waiting.
# 3. A plain ANALYZE t2 with statement_timeout = 500 times out.
# 4. End the held session.
held_start analyzer
held_send analyzer "begin; analyze t2; select 'analyzed';"
held_wait analyzer analyzed || fail "analyze held"
[ "$(timeout 10 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -c 'analyze (skip_locked) t2' 2>&1 | head -1)" = \
  'WARNING:  skipping analyze of "t2" --- lock not available' ] ||
  fail "analyze skip_locked of a table being analyzed"
timeout 10 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -c 'set statement_timeout = 500' -c 'analyze t2' > "$work/analyzer2.out" 2>&1
grep -q 'canceling statement due to statement timeout' "$work/analyzer2.out" ||
  fail "analyze of a table being analyzed waits"
held_send analyzer "commit;"
held_end analyzer

# Verify that an index's definition is read under its table's lock in sm:
# a session that has the index's descriptor cached waits out a DROP INDEX
# in flight, and then finds no index, as pg_get_indexdef() in PostgreSQL
# finds none once it is dropped.
# 1. Make ix1 (a int) with index ix1_a; a held session reads its
#    definition, caching its descriptor.
# 2. Another held session drops ix1_a and holds its transaction open.
# 3. The first reads the definition again: it waits, printing nothing.
# 4. The drop commits: the first prints null.
local_psql -c 'create table ix1(a int)' -c 'create index ix1_a on ix1(a)' \
  > /dev/null
held_start idxreader
held_send idxreader "select pg_get_indexdef('ix1_a'::regclass);"
held_wait idxreader 'CREATE INDEX ix1_a ON public.ix1 USING btree (a)' ||
  fail "index definition read"
held_start idxdropper
held_send idxdropper "begin; drop index ix1_a; select 'dropped';"
held_wait idxdropper dropped || fail "index dropped, held"
held_send idxreader "select pg_get_indexdef('ix1_a'::regclass) is null;"
sleep 1
[ "$(grep -c . "$work/idxreader.out")" = 1 ] ||
  fail "index definition waits out its drop"
held_send idxdropper "commit;"
held_wait idxreader t || fail "index definition after its drop is null"
held_end idxdropper
held_end idxreader

# Verify that another session's DDL reaches what a session caches with no
# message sent: a table's definition by its version, checked under sm's
# lock, and a schema by sm's namespaces' epoch (SHMEM.md section 5.1).
# 1. A held session reads vt (a int), caching its descriptor.
# 2. Another session adds column b to vt and commits: the held session's
#    next read of vt, in a transaction of its own, has b.
# 3. The held session sets its search path to vs1, public, with no schema
#    vs1: vt is public's.
# 4. Another session makes vs1 and vs1.vt (c int): the held session's next
#    read of vt finds vs1's, with column c.
local_psql -c 'create table vt(a int)' -c 'insert into vt values (1)' \
  > /dev/null
held_start vreader
held_send vreader "select * from vt;"
held_wait vreader 1 || fail "version: vt read"
local_psql -c "alter table vt add column b text default 'x'" > /dev/null
held_send vreader "select a || '/' || b from vt;"
held_wait vreader 1/x || fail "version: another session's ADD COLUMN seen"
held_send vreader "set search_path = vs1, public; select 'path ' || a from vt;"
held_wait vreader 'path 1' || fail "version: vt is public's"
local_psql -c 'create schema vs1' -c 'create table vs1.vt(c int)' \
  -c 'insert into vs1.vt values (7)' > /dev/null
held_send vreader "select 'schema ' || c from vt;"
held_wait vreader 'schema 7' || fail "version: another session's schema seen"
held_end vreader

# Verify that pg_log_backend_memory_contexts() reaches another session by an
# interrupt bit of its own, INTERRUPT_LOG_MEMORY, which was a ProcSignal.
# 1. A held session gives its pid.
# 2. Another session asks it to log its memory contexts: true.
# 3. The held session runs a statement, and the server's log says it
#    logged them.
held_start memlog
held_send memlog "select 'pid ' || pg_backend_pid();"
for _ in $(seq 50); do
  memlog_pid=$(sed -n 's/^pid //p' "$work/memlog.out")
  [ -n "$memlog_pid" ] && break
  sleep 0.1
done
[ "$(local_psql -c "select pg_log_backend_memory_contexts($memlog_pid)")" = t ] ||
  fail "memory contexts: asked"
held_send memlog "select 'logged';"
held_wait memlog logged || fail "memory contexts: the session goes on"
for _ in $(seq 50); do
  grep -q "logging memory contexts of PID $memlog_pid" "$work/log" && break
  sleep 0.1
done
grep -q "logging memory contexts of PID $memlog_pid" "$work/log" ||
  fail "memory contexts: logged"
held_end memlog

# Verify that a session is found by its record in the session registry
# (SHMEM.md section 5.2): pg_blocking_pids() names the session blocking
# another, and pg_terminate_backend() ends a session, in its own thread.
# Advisory locks are in digres's own table (storage/advisory_lock.h), whose
# waits pg_blocking_pids() reads.
# 1. A held session takes advisory lock 42, and gives its pid.
# 2. A second held session asks for it, and waits; it gives its pid first.
# 3. pg_blocking_pids() of the second is the first's pid.
# 4. pg_terminate_backend() of the second is true, and it ends.
held_start blocker
held_send blocker "select pg_advisory_lock(42); select 'pid ' || pg_backend_pid();"
# The blocker gives its pid once it holds the lock: only then does the
# second ask for it, or it might take the lock first.
for _ in $(seq 50); do
  blocker_pid=$(sed -n 's/^pid //p' "$work/blocker.out" 2> /dev/null)
  [ -n "$blocker_pid" ] && break
  sleep 0.1
done
[ -n "$blocker_pid" ] || fail "the blocker takes the lock"
held_start blocked
held_send blocked "select 'pid ' || pg_backend_pid(); select pg_advisory_lock(42);"
for _ in $(seq 50); do
  blocked_pid=$(sed -n 's/^pid //p' "$work/blocked.out" 2> /dev/null)
  [ -n "$blocked_pid" ] && break
  sleep 0.1
done
for _ in $(seq 50); do
  blockers=$(local_psql -c "select pg_blocking_pids($blocked_pid)")
  [ "$blockers" = "{$blocker_pid}" ] && break
  sleep 0.1
done
[ "$blockers" = "{$blocker_pid}" ] || fail "pg_blocking_pids names the blocker"
[ "$(local_psql -c "select pg_terminate_backend($blocked_pid, 0)")" = t ] ||
  fail "pg_terminate_backend"
for _ in $(seq 100); do
  kill -0 "${held_pid[blocked]}" 2> /dev/null || break
  sleep 0.1
done
if kill -0 "${held_pid[blocked]}" 2> /dev/null; then
  fail "pg_terminate_backend ends the session promptly"
  kill "${held_pid[blocked]}"
fi
held_end blocked
grep -q 'terminating connection due to administrator command' \
  "$work/blocked.out" || fail "pg_terminate_backend ends the session"
# Verify that pg_cancel_backend() ends a session's wait for a lock, and
# leaves the session: the cancel reaches its thread as an interrupt bit,
# no signal.
# 1. A third held session asks for advisory lock 42, still held, and waits.
# 2. pg_cancel_backend() of it is true, and its statement fails, cancelled.
# 3. The session still runs a statement.
held_start cancelled
held_send cancelled "select 'pid ' || pg_backend_pid(); select pg_advisory_lock(42);"
for _ in $(seq 50); do
  cancelled_pid=$(sed -n 's/^pid //p' "$work/cancelled.out" 2> /dev/null)
  [ -n "$cancelled_pid" ] && break
  sleep 0.1
done
for _ in $(seq 50); do
  blockers=$(local_psql -c "select pg_blocking_pids($cancelled_pid)")
  [ "$blockers" = "{$blocker_pid}" ] && break
  sleep 0.1
done
[ "$blockers" = "{$blocker_pid}" ] || fail "pg_cancel_backend: the session waits"
[ "$(local_psql -c "select pg_cancel_backend($cancelled_pid)")" = t ] ||
  fail "pg_cancel_backend"
for _ in $(seq 50); do
  grep -q 'canceling statement due to user request' "$work/cancelled.out" &&
    break
  sleep 0.1
done
grep -q 'canceling statement due to user request' "$work/cancelled.out" ||
  fail "pg_cancel_backend ends a lock wait"
held_send cancelled "select 'alive';"
held_wait cancelled "alive" || fail "pg_cancel_backend leaves the session"
held_end cancelled
held_send blocker "select pg_advisory_unlock(42);"
held_end blocker

# Verify that pg_blocking_pids() names whom a session waits for in sm: the
# writer of a row it would write, and the holder of a table's lock, which
# isolationtester asks through pg_isolation_test_session_is_blocked().
# 1. A held session updates a row of bp1 in an open transaction, and gives
#    its pid.
# 2. A second held session updates the same row, and waits behind it.
# 3. pg_blocking_pids() of the second is the first's pid, and
#    pg_isolation_test_session_is_blocked() is true of it.
# 4. The first commits; the second's update goes through, and it is
#    blocked by no one.
# 5. The first locks bp1 ACCESS EXCLUSIVE; the second's LOCK TABLE waits,
#    and pg_blocking_pids() of it is the first's pid.
local_psql -c 'create table bp1 (k int4, v int4)' \
  -c 'insert into bp1 values (1, 1)' > /dev/null
held_start bp_a
held_send bp_a "begin; update bp1 set v = 2 where k = 1; select 'pid ' || pg_backend_pid();"
for _ in $(seq 50); do
  bp_a_pid=$(sed -n 's/^pid //p' "$work/bp_a.out" 2> /dev/null)
  [ -n "$bp_a_pid" ] && break
  sleep 0.1
done
held_start bp_b
held_send bp_b "select 'pid ' || pg_backend_pid(); update bp1 set v = 3 where k = 1; select 'updated';"
for _ in $(seq 50); do
  bp_b_pid=$(sed -n 's/^pid //p' "$work/bp_b.out" 2> /dev/null)
  [ -n "$bp_b_pid" ] && break
  sleep 0.1
done
for _ in $(seq 50); do
  blockers=$(local_psql -c "select pg_blocking_pids($bp_b_pid)")
  [ "$blockers" = "{$bp_a_pid}" ] && break
  sleep 0.1
done
[ "$blockers" = "{$bp_a_pid}" ] || fail "pg_blocking_pids names a row's writer"
[ "$(local_psql -c "select pg_isolation_test_session_is_blocked($bp_b_pid, '{$bp_a_pid}')")" = t ] ||
  fail "pg_isolation_test_session_is_blocked by a row's writer"
held_send bp_a "commit;"
held_wait bp_b updated || fail "the waiting update goes through"
[ "$(local_psql -c "select pg_blocking_pids($bp_b_pid)")" = "{}" ] ||
  fail "pg_blocking_pids of a session no longer waiting"
held_send bp_a "begin; lock table bp1 in access exclusive mode; select 'locked';"
held_wait bp_a locked || fail "the first locks bp1"
held_send bp_b "begin; lock table bp1 in access share mode; select 'shared'; commit;"
for _ in $(seq 50); do
  blockers=$(local_psql -c "select pg_blocking_pids($bp_b_pid)")
  [ "$blockers" = "{$bp_a_pid}" ] && break
  sleep 0.1
done
[ "$blockers" = "{$bp_a_pid}" ] || fail "pg_blocking_pids names a table lock's holder"
held_send bp_a "commit;"
held_wait bp_b shared || fail "the waiting LOCK TABLE goes through"
held_end bp_a
held_end bp_b

# Verify that a deadlock among sm's locks is reported as PostgreSQL reports
# one: "deadlock detected", SQLSTATE 40P01, which clients retry on.
# 1. Session A locks dl1 and session B dl2, each in a transaction.
# 2. A asks for dl2 and waits; B asks for dl1, which closes the cycle.
# 3. One of them fails with "deadlock detected" and 40P01; the other's
#    lock is then granted.
local_psql -c 'create table dl1 (k int4)' -c 'create table dl2 (k int4)' \
  > /dev/null
held_start dl_a
held_start dl_b
held_send dl_a "\\set VERBOSITY verbose
begin; lock table dl1; select 'a has dl1';"
held_wait dl_a 'a has dl1' || fail "A locks dl1"
held_send dl_b "\\set VERBOSITY verbose
begin; lock table dl2; select 'b has dl2';"
held_wait dl_b 'b has dl2' || fail "B locks dl2"
held_send dl_a "lock table dl2; select 'a has dl2';"
sleep 0.5
held_send dl_b "lock table dl1; select 'b has dl1';"
for _ in $(seq 100); do
  grep -q 'deadlock detected' "$work/dl_a.out" "$work/dl_b.out" && break
  sleep 0.1
done
grep -q 'ERROR:  40P01: deadlock detected' "$work/dl_a.out" "$work/dl_b.out" ||
  fail "a deadlock in sm is deadlock detected, 40P01"
held_send dl_a "rollback;"
held_send dl_b "rollback;"
held_end dl_a
held_end dl_b

# Verify that lock_timeout ends a wait in sm for a row's writer or a table's
# lock, as PostgreSQL's ends a wait for a lock, and no statement that does
# not wait.
# 1. A held session updates a row of lt1 and locks lt1, in a transaction.
# 2. With lock_timeout 200ms, an update of that row and a LOCK TABLE each
#    fail with 55P03, "canceling statement due to lock timeout", promptly.
# 3. With lock_timeout 1ms, an insert of 100,000 rows into another table,
#    which waits for nothing, completes.
local_psql -c 'create table lt1 (k int4, v int4)' \
  -c 'insert into lt1 values (1, 1)' -c 'create table lt2 (i int4)' > /dev/null
held_start lt_a
held_send lt_a "begin; update lt1 set v = 2 where k = 1; lock table lt1 in row exclusive mode; select 'lt held';"
held_wait lt_a 'lt held' || fail "the holder of lt1"
start=$(date +%s%N)
local_psql -c '\set VERBOSITY verbose' -c "set lock_timeout = '200ms'" \
  -c 'update lt1 set v = 3 where k = 1' 2>&1 |
  grep -q 'ERROR:  55P03: canceling statement due to lock timeout' ||
  fail "lock_timeout ends a wait for a row's writer"
[ $((($(date +%s%N) - start) / 1000000)) -lt 5000 ] ||
  fail "lock_timeout ends a wait promptly"
local_psql -c '\set VERBOSITY verbose' -c "set lock_timeout = '200ms'" \
  -c 'begin' -c 'lock table lt1 in access exclusive mode' 2>&1 |
  grep -q 'ERROR:  55P03: canceling statement due to lock timeout' ||
  fail "lock_timeout ends a wait for a table's lock"
[ "$(local_psql -c "set lock_timeout = '1ms'" \
  -c 'insert into lt2 select generate_series(1, 100000)' 2>&1 | tail -1)" = \
  "INSERT 0 100000" ] || fail "lock_timeout leaves a statement that does not wait"
held_send lt_a "rollback;"
held_end lt_a

# Verify that advisory locks keep PostgreSQL's semantics in digres's own
# table: holds are counted, a transaction's go as it or a savepoint ends,
# a wait ends on lock_timeout, on a cancel, and on a deadlock among them.
# 1. A session locks key 50 twice and unlocks it three times: the third
#    unlock is false, with a warning.
# 2. Held session A takes transaction lock 51, lock 52 in a savepoint it
#    rolls back, session lock 53, and unlocks all its session locks.
# 3. Another session can have 52 and 53, and not 51.
# 4. Another waits for 51 under lock_timeout, and fails on it.
# 5. Another waits for 51 until pg_cancel_backend() ends its wait.
# 6. A commits and stays connected, and 51 can be had.
# 7. Held sessions C and D take 54 and 55, then each waits for the
#    other's: D, the second to wait, fails with a deadlock, and once it
#    unlocks 55, which the error left it holding, C gets it.
local_psql -c 'select pg_advisory_lock(50)' -c 'select pg_advisory_lock(50)' \
  -c 'select pg_advisory_unlock(50)' -c 'select pg_advisory_unlock(50)' \
  -c 'select pg_advisory_unlock(50)' > "$work/adv_count.out" 2>&1
[ "$(grep -cx t "$work/adv_count.out")" = 2 ] &&
  grep -qx f "$work/adv_count.out" &&
  grep -q "you don't own a lock of type ExclusiveLock" \
    "$work/adv_count.out" || fail "advisory holds are counted"
held_start adv_a
held_send adv_a "begin; select pg_advisory_xact_lock(51); savepoint s;
select pg_advisory_xact_lock(52); rollback to s; select pg_advisory_lock(53);
select pg_advisory_unlock_all(); select 'pid ' || pg_backend_pid();"
for _ in $(seq 50); do
  adv_a_pid=$(sed -n 's/^pid //p' "$work/adv_a.out" 2> /dev/null)
  [ -n "$adv_a_pid" ] && break
  sleep 0.1
done
[ "$(local_psql -c 'select pg_try_advisory_lock(51), pg_try_advisory_lock(52),
  pg_try_advisory_lock(53)')" = "f|t|t" ] || fail "advisory scopes"
local_psql -c 'set lock_timeout = 200' -c 'select pg_advisory_lock(51)' \
  > "$work/adv_timeout.out" 2>&1
grep -q 'canceling statement due to lock timeout' "$work/adv_timeout.out" ||
  fail "advisory wait ends on lock_timeout"
held_start adv_w
held_send adv_w "select 'pid ' || pg_backend_pid(); select pg_advisory_lock(51);"
for _ in $(seq 50); do
  adv_w_pid=$(sed -n 's/^pid //p' "$work/adv_w.out" 2> /dev/null)
  [ -n "$adv_w_pid" ] &&
    [ "$(local_psql -c "select pg_blocking_pids($adv_w_pid)")" = "{$adv_a_pid}" ] &&
    break
  sleep 0.1
done
local_psql -c "select pg_cancel_backend($adv_w_pid)" > /dev/null
held_wait adv_w "ERROR:  canceling statement due to user request" ||
  fail "advisory wait ends on a cancel"
held_end adv_w
held_send adv_a "commit; select 'committed';"
held_wait adv_a committed || fail "advisory session commits"
[ "$(local_psql -c 'select pg_try_advisory_lock(51)')" = t ] ||
  fail "advisory transaction lock goes at commit"
held_end adv_a
held_start adv_c
held_start adv_d
# Under lock_timeout, so that a deadlock not found fails the check, not
# the suite.
held_send adv_c "set lock_timeout = '10s'; select pg_advisory_lock(54);
select 'pid ' || pg_backend_pid();"
held_send adv_d "set lock_timeout = '10s'; select pg_advisory_lock(55);
select 'pid ' || pg_backend_pid();"
for _ in $(seq 50); do
  adv_c_pid=$(sed -n 's/^pid //p' "$work/adv_c.out" 2> /dev/null)
  adv_d_pid=$(sed -n 's/^pid //p' "$work/adv_d.out" 2> /dev/null)
  [ -n "$adv_c_pid" ] && [ -n "$adv_d_pid" ] && break
  sleep 0.1
done
held_send adv_c "select pg_advisory_lock(55); select 'c got 55';"
for _ in $(seq 50); do
  [ "$(local_psql -c "select pg_blocking_pids($adv_c_pid)")" = "{$adv_d_pid}" ] &&
    break
  sleep 0.1
done
held_send adv_d "select pg_advisory_lock(54);"
held_wait adv_d "ERROR:  deadlock detected" || fail "advisory deadlock detected"
held_send adv_d "select pg_advisory_unlock(55);"
held_wait adv_c "c got 55" || fail "advisory deadlock: the other gets its lock"
held_end adv_c
held_end adv_d

# Verify that pg_lock_status() lists what sm's lock table and the advisory
# table hold and are waited for, in sm's modes, each with its session's pid
# and its database.
# 1. A, in a transaction, locks lkst's row 1 for update and takes advisory
#    lock 77; B waits to lock row 1 for share; C waits for advisory lock 77.
# 2. pg_lock_status() lists A's IX on lkst, its X on the row, at no tid,
#    as a row of a table with a primary key is locked by its key
#    (../sm/CLUSTER.md section 10), and its X on 77, granted; B's IS on lkst,
#    granted, and its S on the row, waited for; C's X on 77, waited for; each
#    in this database.
# 3. A commits: B's S on the row is held, and A's advisory lock, a
#    session's, still is.
# 4. A unlocks 77: C holds it.
local_psql -c 'create table lkst (a int4 primary key, b int4)' \
  -c 'insert into lkst values (1, 10), (2, 20)' > /dev/null 2>&1
held_start lk_a
held_start lk_b
held_start lk_c
held_send lk_a "begin; select 'pid ' || pg_backend_pid();
select a from lkst where a = 1 for update; select pg_advisory_lock(77);
select 'a locked';"
held_wait lk_a "a locked" || fail "lock status: A locks"
held_send lk_b "select 'pid ' || pg_backend_pid(); begin;
select a from lkst where a = 1 for share; select 'b got';"
held_send lk_c "select 'pid ' || pg_backend_pid();
select pg_advisory_lock(77); select 'c got';"
for _ in $(seq 50); do
  lk_b_pid=$(sed -n 's/^pid //p' "$work/lk_b.out" 2> /dev/null)
  lk_c_pid=$(sed -n 's/^pid //p' "$work/lk_c.out" 2> /dev/null)
  [ -n "$lk_b_pid" ] && [ -n "$lk_c_pid" ] && break
  sleep 0.1
done
lk_a_pid=$(sed -n 's/^pid //p' "$work/lk_a.out")
# Each row as who, what, where and how, over the three sessions' pids.
lock_rows() {
  local_psql -c "select case pid when $lk_a_pid then 'a' when $lk_b_pid
  then 'b' when $lk_c_pid then 'c' end, locktype, relation::regclass,
  '(' || page || ',' || tuple || ')', classid, objid, objsubid,
  mode, granted, database = (select oid from pg_database
  where datname = current_database())
  from pg_lock_status() where pid in ($lk_a_pid, $lk_b_pid, $lk_c_pid)
  order by 1, 2, mode"
}
for _ in $(seq 50); do
  [ "$(lock_rows | grep -c '|f|t$')" = 2 ] && break
  sleep 0.1
done
lock_rows > "$work/lock_status1.out" 2>&1
cat > "$work/lock_status1.want" <<'ROWS'
a|advisory|||0|77|1|X|t|t
a|relation|lkst|||||IX|t|t
a|tuple|lkst|||||X|t|t
b|relation|lkst|||||IS|t|t
b|tuple|lkst|||||S|f|t
c|advisory|||0|77|1|X|f|t
ROWS
cmp -s "$work/lock_status1.out" "$work/lock_status1.want" ||
  fail "lock status lists what is held and waited for"
held_send lk_a "commit; select 'a committed';"
held_wait lk_b "b got" || fail "lock status: B gets the row"
lock_rows > "$work/lock_status2.out" 2>&1
cat > "$work/lock_status2.want" <<'ROWS'
a|advisory|||0|77|1|X|t|t
b|relation|lkst|||||IS|t|t
b|tuple|lkst|||||S|t|t
c|advisory|||0|77|1|X|f|t
ROWS
cmp -s "$work/lock_status2.out" "$work/lock_status2.want" ||
  fail "lock status after a commit"
held_send lk_a "select pg_advisory_unlock(77);"
held_wait lk_c "c got" || fail "lock status: C gets 77"
[ "$(lock_rows | grep '^c|advisory')" = 'c|advisory|||0|77|1|X|t|t' ] ||
  fail "lock status: C holds 77"
held_send lk_b "commit;"
held_end lk_a
held_end lk_b
held_end lk_c

# Verify that reading the catalogs takes no lock: a catalog's rows are
# compiled in and never change, so no lock would guard anything of them,
# and nothing waits on a virtual xid's lock.
# 1. In a transaction, read pg_class and pg_namespace.
# 2. pg_lock_status() lists no lock of the session's.
[ "$(local_psql -c 'begin' -c 'select count(*) > 0 from pg_class' \
  -c 'select count(*) > 0 from pg_namespace' \
  -c 'select count(*) from pg_lock_status() where pid = pg_backend_pid()' \
  -c 'commit' | sed -n 4p)" = 0 ] ||
  fail "reading the catalogs takes no lock"

# Verify that a snapshot is neither exported nor imported: what a session
# sees is sm's snapshot, which PostgreSQL's export does not carry, so an
# importing session would not see what the exporter does.
# 1. pg_export_snapshot() fails, not supported yet.
# 2. SET TRANSACTION SNAPSHOT fails the same way.
local_psql -c 'begin isolation level repeatable read' \
  -c 'select pg_export_snapshot()' > "$work/export.out" 2>&1
grep -q 'exporting a snapshot is not supported yet' "$work/export.out" ||
  fail "snapshot export refused"
local_psql -c 'begin isolation level repeatable read' \
  -c "set transaction snapshot '00000003-00000002-1'" > "$work/import.out" 2>&1
grep -q 'SET TRANSACTION SNAPSHOT is not supported yet' "$work/import.out" ||
  fail "snapshot import refused"

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
grep -q 'duplicate key value violates unique constraint "t3_b"' \
  "$work/index.out" &&
  grep -qx 'DETAIL:  Key (b)=(10) already exists.' "$work/index.out" ||
  fail "unique index"
local_psql -c 'drop index t3_b' -c 'insert into t3 values (2, 10)' \
  -c 'drop index t3_b' > "$work/index.out" 2>&1
grep -qx 'INSERT 0 1' "$work/index.out" &&
  grep -q 'index "t3_b" does not exist' "$work/index.out" ||
  fail "drop index"

# Verify that DDL on a relation whose descriptor this session cached in an
# earlier transaction takes sm's lock before it reads the descriptor: each
# step stopped an assertion build at relation_open's NoLock check.
# 1. Insert into a table with an index, caching it; drop the index.
# 2. Insert into a temporary table; DISCARD TEMP drops it.
# 3. Insert into an ON COMMIT DELETE ROWS table and commit; a later
#    transaction that reads another temporary table truncates it.
# 4. Insert into a table; make a sequence owned by its column.
# 5. Analyze the table and read it; pg_stats() reads its statistics.
# 6. Confirm no statement failed and the session ran to its last.
local_psql > "$work/cached.out" 2>&1 <<'SQL'
create table cd1 (k int4);
create index cd1_k on cd1 (k);
insert into cd1 values (1);
drop index cd1_k;
create temp table cd2 (a int4);
insert into cd2 values (1);
discard temp;
create temp table cd3 (a int4) on commit delete rows;
create temp table cd4 (a int4);
begin;
insert into cd3 values (1);
commit;
select count(*) from cd4;
create sequence cd1_s;
alter sequence cd1_s owned by cd1.k;
insert into cd1 select generate_series(2, 100);
analyze cd1;
select count(*) from cd1;
select count(*) > 0 from pg_stats();
select 'cached done';
SQL
! grep -q ERROR "$work/cached.out" &&
  grep -qx 'cached done' "$work/cached.out" ||
  fail "DDL on a cached descriptor takes sm's lock"

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

# Verify that the planner plans no bitmap scan of a table in sm, which is
# read by its key, not in the order of a bitmap (sm/CLUSTER.md section 6).
# 1. Fill t7 with a = 1..1000 and b = a % 10, indexed on a and on b.
# 2. With sequential and index scans off, plan an OR of both columns, which
#    PostgreSQL plans as a BitmapOr: confirm no bitmap node.
# 3. Confirm the OR still finds its 121 rows.
nobitmap_psql() {
  local_psql -c 'set enable_seqscan = off' -c 'set enable_indexscan = off' \
    -c "$1" 2>&1 | grep -v '^SET$'
}
local_psql -c 'create table t7 (a int4, b int4)' -c 'create index t7_a on t7 (a)' \
  -c 'create index t7_b on t7 (b)' \
  -c "insert into t7 values $(seq 1000 | awk '{printf "%s(%d,%d)",
    (NR > 1 ? "," : ""), $1, $1 % 10}')" > "$work/bitmap.out" 2>&1
nobitmap_psql 'explain (costs off) select a from t7 where a < 25 or b = 3' |
  grep -q 'Bitmap' && fail "bitmap plan"
[ "$(nobitmap_psql 'select a from t7 where a < 25 or b = 3' | wc -l)" = 121 ] ||
  fail "or without a bitmap"

# Verify that an index-only scan of an index in sm gives the index's
# columns from its entries, reading no row: key columns decoded from the
# key, INCLUDE columns from the entry's value.
# 1. Create t8 with an index on (a, b DESC) including c, and a unique
#    index on d; insert rows with a negative int2, nulls, and a null d.
# 2. With sequential and bitmap scans off, a range on a plans an
#    index-only scan, and returns a, b and c, nulls and the sign kept.
# 3. A range on d, by the unique index, returns its values; d IS NULL,
#    whose entry carries its row's key, returns its one row.
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

# Verify that a session keeps more temporary files open than its share of
# real fds: sm0's cache closes the one used least recently, and opens it
# again at its next use.
# 1. The server was started with max_files_per_process at 64, the most
#    real fds a session holds.
# 2. With work_mem at 64kB, a function scan of 20000 rows spills to a
#    temporary file.
# 3. In one transaction, open 100 cursors on such a scan, each fetching
#    a row: 100 temporary files open at once.
# 4. Fetch a row from each again: every one reads its file, reopened.
spill_sql="select * from generate_series(1, 20000)"
local_psql -c "set work_mem = '64kB'" -c "explain (analyze, buffers,
  costs off, timing off, summary off) $spill_sql" 2>&1 |
  grep -q 'temp read=[0-9]* written=' || fail "function scan spill"
{
  echo "set work_mem = '64kB';"
  echo "begin;"
  for i in $(seq 100); do
    echo "declare c$i cursor for $spill_sql;"
    echo "fetch 1 from c$i;"
  done
  for i in $(seq 100); do echo "fetch 1 from c$i;"; done
  echo "commit;"
} > "$work/vfd_cursors.sql"
local_psql -f "$work/vfd_cursors.sql" > "$work/vfd_cursors.out" 2>&1
[ "$(grep -c '^1$' "$work/vfd_cursors.out")" = 100 ] &&
  [ "$(grep -c '^2$' "$work/vfd_cursors.out")" = 100 ] ||
  fail "cursors over temporary files"

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

# Verify that a view's lock and what it reads hold across sessions
# (../sm/VIEW.md section 7): a view's drop waits for a transaction reading
# it, a replace is seen by another session's prepared query, and a table's
# drop waits for a view being made on it, then refuses.
# 1. Make svt (a) with rows 1 and 2, and views sv1 and sv2 over it.
# 2. A reads sv1 in a transaction; B's DROP VIEW sv1 waits until A commits,
#    then drops it.
# 3. A prepares a query over sv2 and runs it; B replaces sv2; A's prepared
#    query reads the new definition.
# 4. psql's \dv lists sv2, and \d+ gives its definition, as replaced.
# 5. A makes sv3 over svt in a transaction; B's DROP TABLE svt waits until A
#    commits, then is refused; with CASCADE it tells of the views it drops.
local_psql -c 'create table svt (a int4)' -c 'insert into svt values (1), (2)' \
  -c 'create view sv1 as select a from svt' \
  -c 'create view sv2 as select a from svt' > "$work/view.out" 2>&1
held_start va
held_send va "begin; select 'count ' || count(*) from sv1;"
held_wait va "count 2" || fail "view: a reader"
bg_start vdrop -c 'drop view sv1'
bg_waiting vdrop || fail "view: a drop waits for its reader"
held_send va "commit;"
bg_end vdrop || fail "view: the drop ends"
grep -qx "DROP VIEW" "$work/vdrop.out" || fail "view: dropped after its reader"
held_send va "prepare vq as select 'got ' || sum(a) from sv2; execute vq;"
held_wait va "got 3" || fail "view: a prepared query"
local_psql -c 'create or replace view sv2 as select a * 10 as a from svt' \
  >> "$work/view.out" 2>&1
held_send va "execute vq;"
held_wait va "got 30" || fail "view: a prepared query sees the replace"
local_psql -c '\dv' 2>&1 | grep -q '^public|sv2|view|' || fail "view: \\dv"
# Its footer, the definition, is left out of a listing of tuples only.
local_psql -P tuples_only=off -c '\d+ sv2' 2>&1 |
  grep -q 'SELECT a \* 10 AS a' ||
  fail "view: \\d+ gives its definition"
held_send va "begin; create view sv3 as select a from svt; select 'made';"
held_wait va "made" || fail "view: made in a transaction"
bg_start vtab -c 'drop table svt'
bg_waiting vtab || fail "view: a table's drop waits for a view's create"
held_send va "commit;"
held_end va
# Its psql fails, as it should: the message below says it ended.
bg_end vtab
grep -q 'cannot drop table svt because other objects depend on it' \
  "$work/vtab.out" || fail "view: a table's drop is refused"
local_psql -c 'drop table svt cascade' 2>&1 |
  grep -q 'drop cascades to view sv' || fail "view: drop table cascade"

# Verify that another session's temporary views hold what they read, and go
# with it as PostgreSQL's DROP ... CASCADE takes them (../sm/VIEW.md section
# 12).
# 1. Make tvt (a) and tpv over it; A makes temporary ttv over tpv, a
#    permanent view, and ttw over ttv, a temporary one, and reads ttw.
# 2. B's DROP TABLE tvt is refused, naming A's views in pg_temp.
# 3. A begins a transaction and reads tvo; B's DROP TABLE tvt CASCADE drops
#    tpv and A's views: A's next statement, in that transaction, finds no
#    ttw, and A makes ttv again.
local_psql -c 'create table tvt (a int4)' -c 'insert into tvt values (1), (2)' \
  -c 'create view tpv as select a from tvt' -c 'create table tvo (a int4)' \
  > "$work/tview.out" 2>&1
held_start ta
held_send ta "create temp view ttv as select a from tpv;
  create temp view ttw as select a * 10 as a from ttv;
  select 'sum ' || sum(a) from ttw;"
held_wait ta "sum 30" || fail "temporary view: made and read"
local_psql -c 'drop table tvt' > "$work/tdrop.out" 2>&1
grep -q 'view pg_temp.ttw depends on view pg_temp.ttv' "$work/tdrop.out" ||
  fail "temporary view: another session's drop is refused"
held_send ta "begin; select 'tvo ' || count(*) from tvo;"
held_wait ta "tvo 0" || fail "temporary view: a transaction begun"
local_psql -c 'drop table tvt cascade' > "$work/tdrop.out" 2>&1
grep -q 'drop cascades to view pg_temp.ttv' "$work/tdrop.out" ||
  fail "temporary view: another session's drop cascades"
held_send ta "select * from ttw; rollback;
  create temp view ttv as select 1 as a; select 'again ' || a from ttv;"
held_wait ta "again 1" || fail "temporary view: made again"
held_end ta
grep -q 'relation "ttw" does not exist' "$work/ta.out" ||
  fail "temporary view: dropped by another session"

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

# Verify that EXPLAIN VERBOSE prints function calls and a sequence's regclass
# from the rows compiled in and from sm, with no pg_proc or pg_class.
# 1. Function calls print by name.
# 2. A serial's nextval() prints its sequence as a regclass, qualified only
#    off the search path; so does a sequence cast to regclass.
local_psql -c 'create table ev1 (id serial, v text)' -c 'create schema ev' \
  -c 'create table ev.t (id serial)' > /dev/null 2>&1
local_psql -c 'explain verbose select upper(v), length(v) from ev1' 2>&1 |
  grep -q 'Output: upper(v), length(v)' || fail "explain verbose, functions"
local_psql -c "explain verbose insert into ev1 (v) values ('q')" 2>&1 |
  grep -q "Output: nextval('ev1_id_seq'::regclass), 'q'::text" ||
  fail "explain verbose, a serial"
local_psql -c 'explain verbose insert into ev.t default values' 2>&1 |
  grep -q "Output: nextval('ev.t_id_seq'::regclass)" ||
  fail "explain verbose, a serial off the search path"
[ "$(local_psql -c "select 'ev1_id_seq'::regclass, 'ev.t_id_seq'::regclass" \
  2>&1)" = "ev1_id_seq|ev.t_id_seq" ] || fail "a sequence's regclass"
local_psql -c 'drop table ev1' -c 'drop schema ev cascade' > /dev/null 2>&1

# Verify that an lc_* setting is checked without setlocale(), which would
# change every session's locale: a valid name is taken, another refused.
# 1. SET lc_numeric to C is taken and shown.
# 2. SET lc_time to a locale that does not exist is refused.
[ "$(local_psql -c "set lc_numeric = 'C'" -c 'show lc_numeric' 2>&1 |
  tr '\n' ,)" = "SET,C," ] || fail "lc_numeric set to C"
local_psql -c "set lc_time = 'xx_YY'" 2>&1 |
  grep -q 'invalid value for parameter "lc_time": "xx_YY"' ||
  fail "lc_time set to no locale"

# Verify that an identity column draws from a sequence it owns for good,
# and that ALWAYS refuses a value given unless overridden.
# 1. id1's ALWAYS and id2's BY DEFAULT (START 10, INCREMENT 5) number
#    their rows; id1 refuses an id given, or set by UPDATE, but takes one
#    OVERRIDING SYSTEM VALUE; id2 takes one, and ignores it OVERRIDING
#    USER VALUE.
# 2. Its sequence is neither dropped alone nor given another owner.
# 3. A text identity column is refused; a smallint one past 32767 is out
#    of range rather than wrapping.
# 4. EXPLAIN VERBOSE names the sequence; DROP TABLE drops it.
local_psql -c 'create table id1 (id int generated always as identity, v text)' \
  -c 'create table id2 (id bigint generated by default as identity
        (start with 10 increment by 5), v text)' > /dev/null 2>&1
[ "$(local_psql -c "insert into id1 (v) values ('a'), ('b') returning id" \
  -c "insert into id1 overriding system value values (100, 'c') returning id" \
  -c "insert into id2 (v) values ('x') returning id" \
  -c "insert into id2 values (1, 'y') returning id" \
  -c "insert into id2 overriding user value values (1, 'z') returning id" \
  2>&1 | tr '\n' ,)" = \
  "1,2,INSERT 0 2,100,INSERT 0 1,10,INSERT 0 1,1,INSERT 0 1,15,INSERT 0 1," ] ||
  fail "identity columns"
local_psql -c "insert into id1 values (7, 'd')" 2>&1 |
  grep -q 'cannot insert a non-DEFAULT value into column "id"' ||
  fail "an ALWAYS identity given a value"
local_psql -c "update id1 set id = 7" 2>&1 |
  grep -q 'column "id" can only be updated to DEFAULT' ||
  fail "an ALWAYS identity updated"
local_psql -c 'drop sequence id1_id_seq' 2>&1 |
  grep -q 'cannot drop sequence id1_id_seq because other objects depend' ||
  fail "an identity sequence dropped alone"
local_psql -c 'alter sequence id1_id_seq owned by none' 2>&1 |
  grep -q 'cannot change ownership of identity sequence' ||
  fail "an identity sequence disowned"
local_psql -c 'create table id3 (id text generated always as identity)' 2>&1 |
  grep -q 'identity column type must be smallint, integer, or bigint' ||
  fail "a text identity column"
[ "$(local_psql -c 'create table id3 (id smallint generated by default as
        identity (start with 32767))' \
  -c 'insert into id3 default values' -c 'insert into id3 default values' \
  -c 'drop table id3' 2>&1 | tr '\n' ,)" = \
  "CREATE TABLE,INSERT 0 1,ERROR:  smallint out of range,DROP TABLE," ] ||
  fail "a smallint identity past its range"
local_psql -c "explain verbose insert into id2 (v) values ('q')" 2>&1 |
  grep -q "Output: nextval('id2_id_seq'), 'q'::text" ||
  fail "explain verbose of an identity"
local_psql -c 'drop table id1' -c "select nextval('id1_id_seq')" 2>&1 |
  grep -q 'relation "id1_id_seq" does not exist' ||
  fail "a table's drop takes its identity sequence"

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

# Verify that a session's estimates follow a new ANALYZE, though it keeps
# the statistics rows it made.
# 1. Make t31, 10,000 rows with b = 0 in a tenth, and analyze it.
# 2. In one session, estimate b = 0: 1,000 within 10%.
# 3. In the same session, make b = 0 in half the rows, analyze again, and
#    estimate b = 0 again: 5,000 within 10%.
local_psql -c 'create table t31 (a int4, b int8)' \
  -c 'insert into t31 select g, g % 10 from generate_series(1, 10000) g' \
  -c 'analyze t31' >> "$work/stats.out" 2>&1
local_psql -c 'explain select * from t31 where b = 0' \
  -c 'update t31 set b = 0 where a % 2 = 0' -c 'analyze t31' \
  -c 'explain select * from t31 where b = 0' > "$work/stats2.out" 2>&1
n=$(grep -o 'rows=[0-9]*' "$work/stats2.out" | sed -n 1p | cut -d= -f2)
within "$n" 1000 10 || fail "b = 0 before the second analyze: $n"
n=$(grep -o 'rows=[0-9]*' "$work/stats2.out" | sed -n 2p | cut -d= -f2)
within "$n" 5000 10 || fail "b = 0 after the second analyze: $n"

# Verify that a session keeps a table's size for its plans for 10 s, and
# sizes it again after a DDL or an ANALYZE.
# 1. Make t33, a table with a primary key, of 20,000 rows, and analyze it.
# 2. In one session, estimate a scan, insert 20,000 rows and estimate
#    again: 20,000 both times, by the size kept.
# 3. Add a column: the estimate is 40,000 within 10%, sized again.
# 4. Insert 20,000 more and estimate: still 40,000; after 10 s, 60,000
#    within 10%.
# 5. Make t34 of 20,000 rows, and analyze it. In one session, estimate a
#    scan, keeping its size; insert 60,000 more, VACUUM, analyze and
#    estimate, in well under 10 s: 80,000 within 40%, by its size now.
#    By the size kept, it would be 20,000. The VACUUM puts every row in
#    sorted files first, so the analyze counts them from sizes RocksDB
#    estimates well, not memtables it estimates up to 7x over
#    (../sm/STATISTICS.md section 6).
# 6. In a transaction, analyze t34 and estimate: 80,000. Insert 80,000
#    more from another session, and estimate again in the transaction:
#    160,000 within 40%, sized again. The table's version stays the same
#    while the transaction has changed it, so a size kept would say
#    80,000.
local_psql -c 'create table t33 (a int4 primary key, b int8)' \
  -c 'insert into t33 select g, g from generate_series(1, 20000) g' \
  -c 'analyze t33' >> "$work/stats.out" 2>&1
local_psql -c 'explain select * from t33' \
  -c 'insert into t33 select g, g from generate_series(20001, 40000) g' \
  -c 'explain select * from t33' \
  -c 'alter table t33 add column c int4' -c 'explain select * from t33' \
  -c 'insert into t33 select g, g, 0 from generate_series(40001, 60000) g' \
  -c 'explain select * from t33' -c 'select pg_sleep(10.5)' \
  -c 'explain select * from t33' > "$work/sizes.out" 2>&1
grep -o 'rows=[0-9]*' "$work/sizes.out" | cut -d= -f2 > "$work/sizes.rows"
n=$(sed -n 1p "$work/sizes.rows")
[ "$n" = 20000 ] || fail "a size kept, before an insert: $n"
n=$(sed -n 2p "$work/sizes.rows")
[ "$n" = 20000 ] || fail "a size kept, after an insert: $n"
n=$(sed -n 3p "$work/sizes.rows")
within "$n" 40000 10 || fail "a size taken again after a DDL: $n"
n=$(sed -n 4p "$work/sizes.rows")
within "$n" 40000 10 || fail "a size kept for 10 s: $n"
n=$(sed -n 5p "$work/sizes.rows")
within "$n" 60000 10 || fail "a size taken again after 10 s: $n"
local_psql -c 'create table t34 (a int4 primary key, b int8)' \
  -c 'insert into t34 select g, g from generate_series(1, 20000) g' \
  -c 'analyze t34' >> "$work/stats.out" 2>&1
local_psql -c 'explain select * from t34' \
  -c 'insert into t34 select g, g from generate_series(20001, 80000) g' \
  -c 'vacuum t34' -c 'analyze t34' -c 'explain select * from t34' \
  > "$work/sizes2.out" 2>&1
n=$(grep -o 'rows=[0-9]*' "$work/sizes2.out" | sed -n 2p | cut -d= -f2)
within "$n" 80000 40 || fail "a size taken again after an analyze: $n"
held_start own
held_send own "begin; analyze t34; explain select * from t34; select 'one';"
held_wait own one || fail "a transaction's own analyze"
local_psql -c 'insert into t34 select g, g from generate_series(80001, 160000) g' \
  >> "$work/stats.out" 2>&1
held_send own "explain select * from t34; select 'two'; commit;"
held_end own
n=$(grep -o 'rows=[0-9]*' "$work/own.out" | sed -n 1p | cut -d= -f2)
[ "$n" = 80000 ] || fail "a transaction's own analyze, estimated: $n"
n=$(grep -o 'rows=[0-9]*' "$work/own.out" | sed -n 2p | cut -d= -f2)
within "$n" 160000 40 || fail "a size not kept in a transaction that changed it: $n"

# Verify that a session keeps what a name means while the catalog's
# epoch holds, and looks it up again once a DDL moves it
# (SCHEMACACHE.md section 4.1).
# 1. A held session reads n1, which does not exist: refused, and the
#    miss is kept.
# 2. Another session makes n1 with one row: the held session reads it.
# 3. Another session renames n1 and makes a new n1 of two rows: the
#    held session reads the new one.
# 4. The held session makes a temporary n1 of three rows and reads its
#    own; dropped, it reads public's two again.
# 5. In a transaction it makes n2 and reads it; rolled back, n2 is
#    refused again.
held_start names
held_send names "select count(*) from n1; select 'one';"
held_wait names one || fail "a name's miss"
local_psql -c 'create table n1 (a int4)' -c 'insert into n1 values (1)' \
  >> "$work/names.log" 2>&1
held_send names "select count(*) from n1; select 'two';"
held_wait names two || fail "a name made since"
local_psql -c 'alter table n1 rename to n1old' \
  -c 'create table n1 (a int4)' -c 'insert into n1 values (1), (2)' \
  >> "$work/names.log" 2>&1
held_send names "select count(*) from n1; select 'three';"
held_wait names three || fail "a name made again"
held_send names "create temporary table n1 (a int4);
  insert into n1 values (1), (2), (3); select count(*) from n1;
  drop table n1; select count(*) from n1; select 'four';"
held_wait names four || fail "a temporary name"
held_send names "begin; create table n2 (a int4); select count(*) from n2;
  rollback; select count(*) from n2; select 'five';"
held_wait names five || fail "a name of the transaction's own"
held_end names
tr '\n' ' ' < "$work/names.out" > "$work/names.flat"
grep -q 'relation "n1" does not exist.*one 1 two 2 three' \
  "$work/names.flat" || fail "names after another session's DDL"
grep -q 'three .*3 .*2 four' "$work/names.flat" ||
  fail "a temporary table's name"
grep -q 'four .*0 .*relation "n2" does not exist.*five' \
  "$work/names.flat" || fail "a name of a rolled back transaction"

# Verify that what a session caches of a table's definition follows
# another session's DDL, though its version is not checked again
# while the catalog's epoch holds (SCHEMACACHE.md section 4.2).
# 1. A held session reads v1, one column: its row, twice, in two
#    transactions, the second checking nothing.
# 2. Another session adds column b and sets it: the held session reads
#    b.
# 3. Another session drops b: the held session's read of b is refused.
local_psql -c 'create table v1 (a int4)' -c 'insert into v1 values (7)' \
  >> "$work/defs.log" 2>&1
held_start defs
held_send defs "select a from v1; select a from v1; select 'one';"
held_wait defs one || fail "a definition read"
local_psql -c 'alter table v1 add column b int4' -c 'update v1 set b = 8' \
  >> "$work/defs.log" 2>&1
held_send defs "select a, b from v1; select 'two';"
held_wait defs two || fail "a column added since"
local_psql -c 'alter table v1 drop column b' >> "$work/defs.log" 2>&1
held_send defs "select b from v1; select 'three';"
held_wait defs three || fail "a column dropped since"
held_end defs
tr '\n' ' ' < "$work/defs.out" > "$work/defs.flat"
grep -q '^7 7 one 7|8 two .*column "b" does not exist.*three' \
  "$work/defs.flat" || fail "a definition after another session's DDL"

# Verify that a session's statistics rows follow another session's
# ANALYZE, though kept while the catalog's epoch holds
# (SCHEMACACHE.md section 4.3).
# 1. Make t36, 10,000 rows with b = 0 in a tenth, and analyze it. A
#    held session estimates b = 0 twice: 1,000 within 10%.
# 2. Another session makes b = 0 in half the rows and analyzes: the
#    held session estimates 5,000 within 10%.
local_psql -c 'create table t36 (a int4, b int8)' \
  -c 'insert into t36 select g, g % 10 from generate_series(1, 10000) g' \
  -c 'analyze t36' >> "$work/stats.out" 2>&1
held_start st36
held_send st36 "explain select * from t36 where b = 0;
  explain select * from t36 where b = 0; select 'one';"
held_wait st36 one || fail "statistics read"
local_psql -c 'update t36 set b = 0 where a % 2 = 0' -c 'analyze t36' \
  >> "$work/stats.out" 2>&1
held_send st36 "explain select * from t36 where b = 0; select 'two';"
held_wait st36 two || fail "statistics read after an analyze"
held_end st36
grep -o 'rows=[0-9]*' "$work/st36.out" | cut -d= -f2 > "$work/st36.rows"
n=$(sed -n 2p "$work/st36.rows")
within "$n" 1000 10 || fail "statistics kept: $n"
n=$(sed -n 3p "$work/st36.rows")
within "$n" 5000 10 || fail "statistics after another session's analyze: $n"

# Verify that VACUUM compacts a table's store in sm, and that VACUUM
# ANALYZE also analyzes it.
# 1. Make t32 with 20,000 rows of 200 bytes, and VACUUM ANALYZE it: a
#    scan is estimated at 20,000 rows exactly, and it takes over 2 MB.
# 2. Delete every row: it still takes over 2 MB.
# 3. VACUUM it: the versions no reader needs are gone, and it takes
#    under a third of what it did -- the rows' tombstones, which sm's
#    purge takes after (../sm/sm1/SM1_DESIGN.md section 8.3).
# 4. VACUUM of a view is passed over with a warning; a bare VACUUM
#    succeeds.
local_psql -c 'create table t32 (a int4, b text)' \
  -c "insert into t32 select g, repeat('x', 200) from generate_series(1, 20000) g" \
  -c 'vacuum analyze t32' > "$work/vacuum2.out" 2>&1
[ "$(rows_of 'select * from t32')" = 20000 ] || fail "vacuum analyze"
full=$(local_psql -c "select pg_relation_size('t32')" 2>&1)
[ "$full" -gt 2000000 ] || fail "a table's size before its rows go: $full"
local_psql -c 'delete from t32' >> "$work/vacuum2.out" 2>&1
n=$(local_psql -c "select pg_relation_size('t32')" 2>&1)
[ "$n" -gt 2000000 ] || fail "a table's size once deleted: $n"
local_psql -c 'vacuum t32' >> "$work/vacuum2.out" 2>&1
n=$(local_psql -c "select pg_relation_size('t32')" 2>&1)
[ "$n" -lt $((full / 3)) ] || fail "vacuum drops dead versions: $n of $full"
local_psql -c 'create view v32 as select a from t32' -c 'vacuum v32' 2>&1 |
  grep -q 'WARNING:  skipping "v32" --- cannot vacuum non-tables' ||
  fail "vacuum a view"
[ "$(local_psql -c 'vacuum' 2>&1)" = "VACUUM" ] || fail "vacuum every table"

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
#    numeric give their values, numeric's at the scale it is written with.
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

# Verify that ALTER TABLE adds, drops and renames columns, and renames a
# table, in sm; and refuses the rest plainly, rather than failing on
# pg_class.
# 1. Add to al1, which has rows, c with a constant DEFAULT and d with an
#    expression one: the rows read them, and a new row takes them.
# 2. A NOT NULL column with no DEFAULT is refused on al1, which has rows,
#    and taken by the empty ale1; a volatile DEFAULT gives each row a
#    value of its own, and a serial numbers the rows, each column dropped
#    again; a taken name is an error, or with IF NOT EXISTS a notice.
# 3. Drop b: al1 reads without it. One an index uses is refused.
# 4. Rename c to cc and al1 to al2: al1 is gone, al2 has cc.
# 5. A session that read al1 before an ADD COLUMN reads the new column
#    after it commits; one rolled back leaves al2 as it was.
# 6. While that session reads the empty ale2 in a transaction, a NOT NULL
#    column with no DEFAULT is refused at once, ale2 being in use.
# 7. ALTER COLUMN, SET SCHEMA and ALTER INDEX are refused, each naming
#    what it is.
local_psql -c 'create table al1 (a int4, b text)' \
  -c "insert into al1 values (1, 'x'), (2, 'y')" \
  -c 'create index al1a on al1 (a)' \
  -c "alter table al1 add column c int4 default 7,
        add column d text default 'h' || 'i'" \
  -c 'insert into al1 (a) values (3)' -c 'select * from al1 order by a' \
  -c 'alter table al1 add column e int4 not null' \
  -c 'create table ale1 (x int4)' \
  -c 'alter table ale1 add column y int4 not null' \
  -c 'insert into ale1 values (1, null)' \
  -c 'alter table al1 add column f float8 default random()' \
  -c 'select count(distinct f) from al1' -c 'alter table al1 drop column f' \
  -c 'alter table al1 add column f serial' \
  -c 'select f from al1 order by f' -c 'alter table al1 drop column f' \
  -c 'alter table al1 add column a int4' \
  -c 'alter table al1 add column if not exists a int4' \
  -c 'alter table al1 drop column b' -c 'select * from al1 order by a' \
  -c 'alter table al1 drop column a' \
  -c 'alter table al1 rename column c to cc' \
  -c 'alter table al1 rename to al2' -c 'select cc from al2 order by 1' \
  -c 'select * from al1' > "$work/alter.out" 2>&1
cat > "$work/alter.expected" << 'OUT'
CREATE TABLE
INSERT 0 2
CREATE INDEX
ALTER TABLE
INSERT 0 1
1|x|7|hi
2|y|7|hi
3||7|hi
ERROR:  column "e" of relation "al1" contains null values
CREATE TABLE
ALTER TABLE
ERROR:  null value in column "y" of relation "ale1" violates not-null constraint
DETAIL:  Failing row contains (1, null).
ALTER TABLE
3
ALTER TABLE
ALTER TABLE
1
2
3
ALTER TABLE
ERROR:  column "a" of relation "al1" already exists
NOTICE:  column "a" of relation "al1" already exists, skipping
ALTER TABLE
ALTER TABLE
1|7|hi
2|7|hi
3|7|hi
ERROR:  cannot drop column a of table al1 because other objects depend on it
DETAIL:  drop column: index al1a uses a
HINT:  Drop the index first.
ALTER TABLE
ALTER TABLE
7
7
7
ERROR:  relation "al1" does not exist
LINE 1: select * from al1
                      ^
OUT
cmp -s "$work/alter.out" "$work/alter.expected" || fail "ALTER TABLE"

# A unique index over a duplicated key fails with the key in PostgreSQL's
# words, read from a row sm names by its key (../../sm/ROWKEY.md section
# 2.5).
# 1. On a table with rows, the build after the commit meets it.
# 2. A key of two columns, one quoted, says both.
# 3. On a table made in the transaction, the build at once meets it.
local_psql -c 'create table dk (a int4, "B c" text, v text)' \
  -c "insert into dk values (1, 'x', 'p'), (2, 'y', 'q'), (1, 'x', 'r')" \
  -c 'create unique index dka on dk (a)' \
  -c 'create unique index dkab on dk (a, "B c")' \
  -c 'begin' -c 'create table dk2 (k text)' \
  -c "insert into dk2 values ('o''k'), ('o''k')" \
  -c 'create unique index dk2k on dk2 (k)' -c 'rollback' \
  > "$work/dupkey.out" 2>&1
cat > "$work/dupkey.expected" << 'OUT'
CREATE TABLE
INSERT 0 3
ERROR:  could not create unique index "dka"
DETAIL:  Key (a)=(1) is duplicated.
ERROR:  could not create unique index "dkab"
DETAIL:  Key (a, "B c")=(1, x) is duplicated.
BEGIN
CREATE TABLE
INSERT 0 2
ERROR:  could not create unique index "dk2k"
DETAIL:  Key (k)=(o'k) is duplicated.
ROLLBACK
OUT
cmp -s "$work/dupkey.out" "$work/dupkey.expected" ||
  fail "a duplicated key's build"

# A write of a key a unique index holds fails with PostgreSQL's words: the
# index, and the row's key in it.
# 1. An insert, and an update to a taken key.
# 2. ON CONFLICT on one index, meeting the key in another.
# 3. COPY into a table with rows, a row at a time.
# 4. A key of two columns, one quoted, in a table whose name is.
# 5. COPY's error names the line of the row, not the batch's end.
local_psql -c 'create table dw (a int4 primary key, b text unique)' \
  -c "insert into dw values (1, 'x'), (2, 'y')" \
  -c "insert into dw values (1, 'z')" \
  -c 'update dw set a = 2 where a = 1' \
  -c "insert into dw values (1, 'y') on conflict (a) do nothing" \
  -c "insert into dw values (3, 'y') on conflict (a) do nothing" \
  -c "copy dw from program 'printf \"4\\tw\\n2\\tv\\n\"'" \
  -c 'create table "D w" ("K 2" int4, k text, unique ("K 2", k))' \
  -c "insert into \"D w\" values (5, 'a''b'), (5, 'a''b')" \
  > "$work/dupwrite.out" 2>&1
cat > "$work/dupwrite.expected" << 'OUT'
CREATE TABLE
INSERT 0 2
ERROR:  duplicate key value violates unique constraint "dw_pkey"
DETAIL:  Key (a)=(1) already exists.
ERROR:  duplicate key value violates unique constraint "dw_pkey"
DETAIL:  Key (a)=(2) already exists.
INSERT 0 0
ERROR:  duplicate key value violates unique constraint "dw_b_key"
DETAIL:  Key (b)=(y) already exists.
ERROR:  duplicate key value violates unique constraint "dw_pkey"
DETAIL:  Key (a)=(2) already exists.
CONTEXT:  COPY dw, line 2
CREATE TABLE
ERROR:  duplicate key value violates unique constraint "D w_K 2_k_key"
DETAIL:  Key ("K 2", k)=(5, a'b) already exists.
OUT
cmp -s "$work/dupwrite.out" "$work/dupwrite.expected" ||
  fail "a write of a taken key"
mkfifo "$work/alter.in"
local_psql < "$work/alter.in" > "$work/alter2.out" 2>&1 &
alter=$!
exec 4> "$work/alter.in"
echo "select count(*) from al2;" >&4
for _ in $(seq 50); do
  grep -qx 3 "$work/alter2.out" && break
  sleep 0.1
done
local_psql -c 'alter table al2 add column z int4 default 4' \
  -c 'begin' -c 'alter table al2 add column w int4' -c 'rollback' \
  -c 'create table ale2 (x int4)' > /dev/null 2>&1
echo "begin; select count(*) from ale2;" >&4
for _ in $(seq 50); do
  grep -qx 0 "$work/alter2.out" && break
  sleep 0.1
done
local_psql -c 'alter table ale2 add column y int4 not null' 2>&1 |
  grep -q 'could not obtain lock on relation "ale2"' ||
  fail "NOT NULL column refused on a table in use"
echo "commit; select * from al2 where a = 1;" >&4
exec 4>&-
wait $alter
[ "$(tr '\n' ' ' < "$work/alter2.out")" = "3 BEGIN 0 COMMIT 1|7|hi|4 " ] ||
  fail "ALTER TABLE seen by another session"
local_psql -c 'alter table al2 alter column a set not null' \
  -c 'alter table al2 set schema s7' \
  -c 'alter index al1a rename to al1b' > "$work/alter3.out" 2>&1
grep -q "ERROR:  ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet" \
  "$work/alter3.out" &&
  grep -q 'ERROR:  ALTER TABLE is not supported yet' "$work/alter3.out" &&
  grep -q 'ERROR:  ALTER INDEX is not supported yet' "$work/alter3.out" ||
  fail "ALTER refused"

# Verify that ALTER TABLE and the statements around it wait for each other
# in sm, and that a statement that waited reads the new definition, as
# PostgreSQL's relation locks make it (lmgr.c, smstore_lock_relation()).
# 1. A reads rc in a transaction: B's ADD COLUMN waits until A commits.
# 2. C has read rc. A adds d DEFAULT 5 and has not committed: C's INSERT and
#    SELECT wait, and once A commits, the INSERT takes d's default and the
#    SELECT reads d.
# 3. A drops b and has not committed: C's INSERT of four values waits, and
#    once A commits fails as rc has three columns; one of three goes in.
# 4. C reads rc in a transaction: RENAME COLUMN and RENAME TO wait until C
#    commits, and C then reads rc2 and aa.
local_psql -c 'create table rc (a int4, b int4)' \
  -c 'insert into rc values (1, 1)' > /dev/null 2>&1
held_start rca
held_start rcc
held_send rca "begin; select 'step1', count(*) from rc;"
held_wait rca 'step1|1' || fail "rc step 1 read"
bg_start rcb -c 'alter table rc add column c int4'
bg_waiting rcb || fail "ADD COLUMN waits for a reader"
held_send rca "commit;"
bg_end rcb && [ "$(cat "$work/rcb.out")" = "ALTER TABLE" ] ||
  fail "ADD COLUMN after a reader"

held_send rcc "select 'step2', count(*) from rc;"
held_wait rcc 'step2|1' || fail "rc step 2 read"
held_send rca "begin; alter table rc add column d int4 default 5;
  select 'altered';"
held_wait rca altered || fail "rc step 2 alter"
held_send rcc "insert into rc (a) values (2); select * from rc order by a;"
sleep 1
grep -q INSERT "$work/rcc.out" && fail "INSERT waits for ADD COLUMN"
held_send rca "commit;"
held_wait rcc '2|||5' && grep -qxF '1|1||5' "$work/rcc.out" ||
  fail "INSERT and SELECT after ADD COLUMN"

held_send rca "begin; alter table rc drop column b; select 'dropped';"
held_wait rca dropped || fail "rc step 3 alter"
held_send rcc "insert into rc values (3, 3, 3, 3);"
sleep 1
grep -q 'more expressions' "$work/rcc.out" && fail "INSERT waits for DROP COLUMN"
held_send rca "commit;"
held_send rcc "insert into rc values (3, 3, 3); select * from rc where a = 3;"
held_wait rcc '3|3|3' &&
  grep -q 'ERROR:  INSERT has more expressions than target columns' \
    "$work/rcc.out" || fail "INSERT after DROP COLUMN"

held_send rcc "begin; select 'step4', count(*) from rc;"
held_wait rcc 'step4|3' || fail "rc step 4 read"
bg_start rcr -c 'alter table rc rename column a to aa' \
  -c 'alter table rc rename to rc2'
bg_waiting rcr || fail "RENAME waits for a reader"
held_send rcc "commit;"
bg_end rcr || fail "RENAME after a reader"
held_send rcc "select 'renamed', sum(aa) from rc2;"
held_wait rcc 'renamed|6' || fail "RENAME read after"
held_end rca
held_end rcc

# Verify that pg_namespace is a catalog relation a query reads, made from
# the rows compiled in and sm's schemas, so psql's \dn runs; and that it
# is never written.
# 1. Make schema dn1: \dn dn* lists it, owned by the superuser.
# 2. pg_namespace has pg_catalog, pg_toast, public and dn1, by their oids;
#    a schema made in an open transaction is seen there alone.
# 3. INSERT, DELETE, UPDATE, TRUNCATE, FOR UPDATE and DROP TABLE of it are
#    refused, as of a catalog.
me=$(local_psql -c 'select current_user')
local_psql -c 'create schema dn1' -c '\dn dn*' \
  -c "select oid, nspname from pg_namespace
        where nspname in ('pg_catalog', 'pg_toast', 'public') order by oid" \
  -c "select count(*) from pg_namespace where nspname = 'dn1'" \
  -c 'begin' -c 'create schema dn2' \
  -c "select count(*) from pg_namespace where nspname = 'dn2'" -c 'rollback' \
  -c "select count(*) from pg_namespace where nspname = 'dn2'" \
  -c "insert into pg_namespace values (1, 'x', 10, null)" \
  -c 'delete from pg_namespace' -c 'update pg_namespace set nspacl = null' \
  -c 'truncate pg_namespace' -c 'select * from pg_namespace for update' \
  -c 'drop table pg_namespace' > "$work/dn.out" 2>&1
cat > "$work/dn.expected" << OUT
CREATE SCHEMA
dn1|$me
11|pg_catalog
99|pg_toast
2200|public
1
BEGIN
CREATE SCHEMA
1
ROLLBACK
0
ERROR:  permission denied for table pg_namespace
ERROR:  permission denied for table pg_namespace
ERROR:  permission denied for table pg_namespace
ERROR:  permission denied: "pg_namespace" is a system catalog
ERROR:  permission denied for table pg_namespace
ERROR:  permission denied: "pg_namespace" is a system catalog
OUT
cmp -s "$work/dn.out" "$work/dn.expected" || fail "pg_namespace and \\dn"

# Verify that pg_class and pg_am are catalog relations a query reads, so
# psql's \dt and \ds run: pg_class lists the catalog relations, and sm's
# tables, indexes, sequences and the session's temporary tables.
# 1. In schema dt1 make table a, its serial's sequence and its primary key,
#    sequence q, and a temporary table tt.
# 2. \dt and \ds list them; \dtS lists the catalog relations.
# 3. pg_class has each with its kind, columns and whether it has an index;
#    pg_am has btree and heap.
# 4. A write to pg_class is refused.
local_psql -c 'create schema dt1' \
  -c 'create table dt1.a (x serial primary key, y text)' \
  -c 'create sequence dt1.q' -c 'create temp table tt (z int4)' \
  -c '\dt dt1.*' -c '\ds dt1.*' -c '\dt pg_temp.*' -c '\dtS pg_cl*' \
  -c "select relname, relkind, relnatts, relhasindex from pg_class
        where relnamespace = 'dt1'::regnamespace order by relname" \
  -c "select amname from pg_am where amname in ('btree', 'heap') order by 1" \
  -c "delete from pg_class" > "$work/dt.out" 2>&1
cat > "$work/dt.expected" << OUT
CREATE SCHEMA
CREATE TABLE
CREATE SEQUENCE
CREATE TABLE
dt1|a|table|$me
dt1|a_x_seq|sequence|$me
dt1|q|sequence|$me
pg_temp|tt|table|$me
pg_catalog|pg_class|table|$me
a|r|2|t
a_pkey|i|1|f
a_x_seq|S|3|f
q|S|3|f
btree
heap
ERROR:  permission denied for table pg_class
OUT
cmp -s "$work/dt.out" "$work/dt.expected" || fail "pg_class, pg_am and \\dt"

# Verify that pg_index is a catalog relation a query reads, so psql's \di
# runs, and that pg_get_indexdef() prints an index of sm's as PostgreSQL
# does.
# 1. In schema di1 make table a, its primary key, a unique index with
#    DESC, NULLS FIRST and INCLUDE, and one on a quoted column.
# 2. \di lists them with their table.
# 3. pg_index has each index's columns, operator classes and options.
# 4. pg_get_indexdef() prints each, and one column of one.
local_psql -c 'create schema di1' \
  -c 'create table di1.a (x int4 primary key, y text, "Z z" int8)' \
  -c 'create unique index a_yz on di1.a (y desc, "Z z" nulls first)
        include (x)' -c 'create index a_z on di1.a ("Z z")' \
  -c '\di di1.*' \
  -c "select indexrelid::regclass, indnatts, indnkeyatts, indisunique, indkey,
        indoption from pg_index where indrelid = 'di1.a'::regclass
        order by indexrelid" \
  -c "select pg_get_indexdef(indexrelid) from pg_index
        where indrelid = 'di1.a'::regclass order by indexrelid" \
  -c "select pg_get_indexdef('di1.a_yz'::regclass, 2, true)" \
  > "$work/di.out" 2>&1
cat > "$work/di.expected" << OUT
CREATE SCHEMA
CREATE TABLE
CREATE INDEX
CREATE INDEX
di1|a_pkey|index|$me|a
di1|a_yz|index|$me|a
di1|a_z|index|$me|a
di1.a_pkey|1|1|t|1|0
di1.a_yz|3|2|t|2 3 1|3 2
di1.a_z|1|1|f|3|0
CREATE UNIQUE INDEX a_pkey ON di1.a USING btree (x)
CREATE UNIQUE INDEX a_yz ON di1.a USING btree (y DESC, "Z z" NULLS FIRST) INCLUDE (x)
CREATE INDEX a_z ON di1.a USING btree ("Z z")
"Z z"
OUT
cmp -s "$work/di.out" "$work/di.expected" || fail "pg_index, pg_get_indexdef and \\di"

# Verify that psql's \d describes a table and an index of sm's, and \du the
# superuser: pg_attribute, pg_attrdef, pg_type, pg_collation and pg_roles
# are read, and the catalogs digres has nothing of are read empty.
# 1. In schema dd1 make table t with a default, NOT NULL, a numeric's
#    typmod, a serial and a primary key; an index with DESC and INCLUDE;
#    and a column added and dropped.
# 2. \d dd1.t lists its columns, but not the dropped one, and its indexes.
# 3. \d dd1.t_cb lists the index's columns and its table; \d dd1.t_pkey
#    calls it the primary key.
# 4. \du lists the superuser.
local_psql -c 'create schema dd1' \
  -c "create table dd1.t (a int4 primary key, b text default 'x' not null,
        c numeric(10,2), d serial)" \
  -c 'create unique index t_cb on dd1.t (c desc) include (b)' \
  -c 'alter table dd1.t add column e int4' \
  -c 'alter table dd1.t drop column e' -P format=aligned \
  -P tuples_only=off -c '\d dd1.t' -c '\d dd1.t_cb' -c '\d dd1.t_pkey' 2>&1 |
  sed 's/ *$//' > "$work/dd.out"
cat > "$work/dd.expected" << OUT
CREATE SCHEMA
CREATE TABLE
CREATE INDEX
ALTER TABLE
ALTER TABLE
                                  Table "dd1.t"
 Column |     Type      | Collation | Nullable |             Default
--------+---------------+-----------+----------+----------------------------------
 a      | integer       |           | not null |
 b      | text          |           | not null | 'x'::text
 c      | numeric(10,2) |           |          |
 d      | integer       |           | not null | nextval('dd1.t_d_seq'::regclass)
Indexes:
    "t_pkey" PRIMARY KEY, btree (a)
    "t_cb" UNIQUE, btree (c DESC) INCLUDE (b)

              Index "dd1.t_cb"
 Column |     Type      | Key? | Definition
--------+---------------+------+------------
 c      | numeric(10,2) | yes  | c
 b      | text          | no   | b
unique, btree, for table "dd1.t"

          Index "dd1.t_pkey"
 Column |  Type   | Key? | Definition
--------+---------+------+------------
 a      | integer | yes  | a
primary key, btree, for table "dd1.t"

OUT
cmp -s "$work/dd.out" "$work/dd.expected" || fail "\\d of a table and an index"

# Verify that psql's \d shows a foreign key, on the table that has it and on
# the one it references, from pg_constraint's rows (WORKNOTE.md step 246).
# 1. In dd1 make table p with a primary key, and c with a foreign key to it.
# 2. \d dd1.c lists the key under Foreign-key constraints; \d dd1.p under
#    Referenced by.
local_psql -c 'create table dd1.p (a int4 primary key)' \
  -c 'create table dd1.c (x int4 references dd1.p on delete cascade)' \
  -P format=aligned -P tuples_only=off -c '\d dd1.c' -c '\d dd1.p' 2>&1 |
  sed 's/ *$//' > "$work/fk.out"
cat > "$work/fk.expected" << OUT
CREATE TABLE
CREATE TABLE
                   Table "dd1.c"
 Column |  Type   | Collation | Nullable | Default
--------+---------+-----------+----------+---------
 x      | integer |           |          |
Foreign-key constraints:
    "c_x_fkey" FOREIGN KEY (x) REFERENCES dd1.p(a) ON DELETE CASCADE

                   Table "dd1.p"
 Column |  Type   | Collation | Nullable | Default
--------+---------+-----------+----------+---------
 a      | integer |           | not null |
Indexes:
    "p_pkey" PRIMARY KEY, btree (a)
Referenced by:
    TABLE "dd1.c" CONSTRAINT "c_x_fkey" FOREIGN KEY (x) REFERENCES dd1.p(a) ON DELETE CASCADE

OUT
cmp -s "$work/fk.out" "$work/fk.expected" || fail "\\d of a foreign key"

# Verify that the partition functions answer for a table of sm's, which no
# partition tree holds, as PostgreSQL does for a table not partitioned:
# psql 18's \d calls pg_partition_ancestors(), and only its plan kept the
# call from failing.
# 1. pg_partition_ancestors() and pg_partition_tree() of dd1.t: no rows.
# 2. pg_partition_root() of dd1.t: null.
# 3. pg_partition_tree() of oid 0, no relation: no rows.
local_psql -c "select count(*) from pg_partition_ancestors('dd1.t')" \
  -c "select count(*) from pg_partition_tree('dd1.t')" \
  -c "select pg_partition_root('dd1.t') is null" \
  -c "select count(*) from pg_partition_tree(0)" > "$work/part.out" 2>&1
[ "$(tr '\n' ' ' < "$work/part.out")" = "0 0 t 0 " ] ||
  fail "partition functions of a table of sm's"
[ "$(local_psql -c '\du')" = \
  "$me|Superuser, Create role, Create DB, Bypass RLS" ] ||
  fail "\\du"
# Verify that a scan of pg_attribute for one table gives that table's rows
# alone (catalog/catrel.c): its key passes dd1.t's five columns and none of
# its indexes', so the filter has nothing left to remove.
local_psql -c "explain (analyze, costs off, timing off, summary off)
                 select * from pg_attribute where attrelid = 'dd1.t'::regclass" \
  > "$work/narrow.out"
grep -qx 'Seq Scan on pg_attribute (actual rows=5.00 loops=1)' \
  "$work/narrow.out" && ! grep -q 'Rows Removed' "$work/narrow.out" ||
  fail "pg_attribute narrowed"

# Verify that pg_database is a catalog relation a query reads, a row a
# database of sm's. psql 16's \l asks for daticulocale, which PostgreSQL 17
# renamed, so its query is not sent here.
# 1. postgres is there, owned by the superuser, in UTF8, with the C locale.
# 2. Its oid is above the built-in ones: FirstNormalObjectId and sm's id.
local_psql -c "select datname, pg_get_userbyid(datdba) = current_user,
                 pg_encoding_to_char(encoding), datlocprovider, datcollate,
                 datctype, datallowconn from pg_database
               where datname = 'postgres'" \
  -c "select count(*) from pg_database
        where datname = current_database() and oid >= 16384" \
  > "$work/db.out" 2>&1
cat > "$work/db.expected" << OUT
postgres|t|UTF8|c|C|C|t
1
OUT
cmp -s "$work/db.out" "$work/db.expected" || fail "pg_database"

# Verify that psql's \d describes a sequence of sm's: pg_sequence has its
# START and INCREMENT, and pg_depend the column that owns it.
# 1. In schema sq1 make sequence q, START 5 INCREMENT 3, and table t with a
#    serial a and an identity column b.
# 2. \d of q gives its START and INCREMENT, and of t's sequences says the
#    serial's is owned by t.a, the identity's for t.b.
local_psql -c 'create schema sq1' -c 'create sequence sq1.q start 5 increment 3' \
  -c 'create table sq1.t (a serial, b int4 generated always as identity)' \
  -P format=aligned -P tuples_only=off \
  -c '\d sq1.q' -c '\d sq1.t_a_seq' -c '\d sq1.t_b_seq' 2>&1 |
  sed 's/ *$//' > "$work/sq.out"
cat > "$work/sq.expected" << OUT
CREATE SCHEMA
CREATE SEQUENCE
CREATE TABLE
                               Sequence "sq1.q"
  Type  | Start | Minimum |       Maximum       | Increment | Cycles? | Cache
--------+-------+---------+---------------------+-----------+---------+-------
 bigint |     5 |       1 | 9223372036854775807 |         3 | no      |     1

                            Sequence "sq1.t_a_seq"
  Type  | Start | Minimum |       Maximum       | Increment | Cycles? | Cache
--------+-------+---------+---------------------+-----------+---------+-------
 bigint |     1 |       1 | 9223372036854775807 |         1 | no      |     1
Owned by: sq1.t.a

                            Sequence "sq1.t_b_seq"
  Type  | Start | Minimum |       Maximum       | Increment | Cycles? | Cache
--------+-------+---------+---------------------+-----------+---------+-------
 bigint |     1 |       1 | 9223372036854775807 |         1 | no      |     1
Sequence for identity column: sq1.t.b

OUT
cmp -s "$work/sq.out" "$work/sq.expected" || fail "\\d of a sequence"

# Verify that psql's \d+, \dt+ and \ds+ run: the comment functions answer
# null, as digres keeps no comments, and a sequence's size is null; an empty
# table's is 0.
# 1. In schema dp1 make table t, with a primary key and a default, and
#    sequence q.
# 2. \d+ dp1.t lists each column's storage, and its access method.
# 3. \dt+ and \ds+ list t and q with their persistence and no comment.
local_psql -c 'create schema dp1' \
  -c "create table dp1.t (a int4 primary key, b text default 'x')" \
  -c 'create sequence dp1.q' -P format=aligned -P tuples_only=off \
  -c '\d+ dp1.t' 2>&1 | sed 's/ *$//' > "$work/dp.out"
local_psql -c '\dt+ dp1.*' -c '\ds+ dp1.*' >> "$work/dp.out" 2>&1
cat > "$work/dp.expected" << OUT
CREATE SCHEMA
CREATE TABLE
CREATE SEQUENCE
                                               Table "dp1.t"
 Column |  Type   | Collation | Nullable |  Default  | Storage  | Compression | Stats target | Description
--------+---------+-----------+----------+-----------+----------+-------------+--------------+-------------
 a      | integer |           | not null |           | plain    |             |              |
 b      | text    |           |          | 'x'::text | extended |             |              |
Indexes:
    "t_pkey" PRIMARY KEY, btree (a)
Access method: heap

dp1|t|table|$me|permanent|heap|0 bytes|
dp1|q|sequence|$me|permanent||
OUT
cmp -s "$work/dp.out" "$work/dp.expected" || fail "\\d+, \\dt+ and \\ds+"
# Verify that the size functions give a table's and its indexes' bytes in
# sm, which are 0 while empty (above) and grow with their rows. sm counts
# its bytes in memtables, so a secondary index's are never 0 here; what is
# in files is an estimate, so no more than that is checked.
# 1. Put a thousand rows in dp1.t, and index b.
# 2. The table and its index on b have bytes; its primary key has none of
#    its own, its entries being the rows (../sm/ROWKEY.md section 2.1);
#    the total is the table and its indexes; a fork other than main has
#    none.
[ "$(local_psql -c "insert into dp1.t select g, repeat('x', 100)
                      from generate_series(1, 1000) g" \
      -c 'create index t_b on dp1.t (b)' \
      -c "select pg_relation_size('dp1.t') > 0,
                 pg_relation_size('dp1.t_pkey') = 0,
                 pg_relation_size('dp1.t_b') > 0,
                 pg_total_relation_size('dp1.t') =
                   pg_table_size('dp1.t') + pg_indexes_size('dp1.t'),
                 pg_relation_size('dp1.t', 'fsm')")" = "INSERT 0 1000
CREATE INDEX
t|t|t|t|0" ] || fail "sizes of a table in sm"
# Verify that COMMENT is refused plainly, as digres keeps no comments,
# rather than failing on pg_description.
local_psql -c "comment on table dp1.t is 'x'" 2>&1 |
  grep -qx 'ERROR:  COMMENT is not supported yet' || fail "COMMENT refused"

# Verify that psql's \df, \do, \dC, \dT and \dx run: pg_proc, pg_operator,
# pg_cast and pg_type are the rows compiled in, pg_extension empty.
# 1. \df lists abs's seven functions, numeric's twice as both decimals
#    are named numeric, and pg_get_keywords's OUT arguments.
# 2. \do lists the operators named ->; \dC the casts to and from inet;
#    \dT the type int2vector; \dx no extension.
local_psql -c '\df abs' -c '\df pg_get_keywords' -c '\do -> ' \
  -c '\dC inet' -c '\dT int2vector' -c '\dx' > "$work/df.out" 2>&1
cat > "$work/df.expected" << 'OUT'
pg_catalog|abs|bigint|bigint|func
pg_catalog|abs|double precision|double precision|func
pg_catalog|abs|integer|integer|func
pg_catalog|abs|numeric|numeric|func
pg_catalog|abs|numeric|numeric|func
pg_catalog|abs|real|real|func
pg_catalog|abs|smallint|smallint|func
pg_catalog|pg_get_keywords|SETOF record|OUT word text, OUT catcode "char", OUT barelabel boolean, OUT catdesc text, OUT baredesc text|func
pg_catalog|->|json|integer|json|
pg_catalog|->|json|text|json|
pg_catalog|->|jsonb|integer|jsonb|
pg_catalog|->|jsonb|text|jsonb|
cidr|inet|(binary coercible)|yes
inet|character|text|in assignment
inet|character varying|text|in assignment
inet|cidr|cidr|in assignment
inet|text|text|in assignment
pg_catalog|int2vector|
OUT
cmp -s "$work/df.out" "$work/df.expected" || fail "\\df, \\do, \\dC, \\dT and \\dx"

# Verify that a syscache miss in a catalog of compiled-in rows alone is
# looked up in its catalog relation (catrel_is_builtin()), so the server's
# own lookups of functions and languages succeed.
# 1. \sf prints abs(integer)'s definition: pg_proc's and pg_language's rows.
# 2. A function no oid names gives NULL, as a lookup that misses does.
# 3. pg_language lists the languages compiled in, c and internal.
local_psql -c '\sf abs(integer)' \
  -c 'select pg_get_functiondef(1) is null' \
  -c 'select lanname from pg_language order by 1' > "$work/sf.out" 2>&1
cat > "$work/sf.expected" << 'OUT'
CREATE OR REPLACE FUNCTION pg_catalog.abs(integer)
 RETURNS integer
 LANGUAGE internal
 IMMUTABLE PARALLEL SAFE STRICT
AS $function$int4abs$function$
t
c
internal
OUT
cmp -s "$work/sf.out" "$work/sf.expected" ||
  fail "a syscache miss reads the compiled-in rows"

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

# Verify that sm stores a long value out of line, and that digres reads it
# wherever a value is read and writes it back wherever a value is written.
# 1. Make toast1, toast2, and toast3 with an index; insert a row past
#    sm's 65535 bytes, which only TOAST stores, and values of 3000 bytes.
# 2. Read the lengths, sizes, ends and a slice of the values back.
# 3. An update leaving the value alone keeps it, and one setting another
#    column to it, the row's own pointer for another column, copies it.
# 4. Insert the values into another table and into their own.
# 5. An update of the value, rolled back, leaves the old one.
# 6. A join keys an index scan of toast3 by a value out of line, which
#    the executor reads before it keys the scan.
# 7. DELETE ... RETURNING reads the value it deleted.
local_psql -c 'create table toast1 (id int4 primary key, a text, b text,
      n int4)' \
  -c 'create table toast2 (id int4, a text)' -c 'create table toast3 (k text)' \
  -c 'create index toast3k on toast3 (k)' \
  -c "insert into toast1 values (1, repeat('a', 100000) || 'z', 'short', 0),
      (2, repeat('c', 3000), null, 0)" \
  -c "insert into toast3 values (repeat('c', 3000)), ('d')" \
  > "$work/toast.out" 2>&1
[ "$(local_psql -c 'select id, length(a), octet_length(a), right(a, 1),
      substr(a, 99999, 3) from toast1 order by id' 2>&1 | tr '\n' ,)" = \
  "1|100001|100001|z|aaz,2|3000|3000|c|," ] || fail "a value out of line read"
[ "$(local_psql -c 'update toast1 set n = n + 1' \
  -c 'update toast1 set b = a where id = 1' \
  -c 'select id, n, length(b), b = a from toast1 order by id' 2>&1 |
  tr '\n' ,)" = "UPDATE 2,UPDATE 1,1|1|100001|t,2|1||," ] ||
  fail "an update keeps and copies a value out of line"
[ "$(local_psql -c 'insert into toast2 select id, a from toast1' \
  -c 'insert into toast1 select id + 10, a, b, n from toast1' \
  -c 'select (select sum(length(a)) from toast2),
      (select count(*) from toast1 where a = (select a from toast2 where id = 1))' \
  2>&1 | tr '\n' ,)" = "INSERT 0 2,INSERT 0 2,103001|2," ] ||
  fail "an insert copies a value out of line"
[ "$(local_psql -c 'begin' -c "update toast1 set a = a || 'q' where id = 1" \
  -c 'select length(a), right(a, 2) from toast1 where id = 1' -c 'rollback' \
  -c 'select length(a), right(a, 1) from toast1 where id = 1' 2>&1 |
  tr '\n' ,)" = "BEGIN,UPDATE 1,100002|zq,ROLLBACK,100001|z," ] ||
  fail "a value out of line rolled back"
[ "$(local_psql -c 'set enable_hashjoin = off' -c 'set enable_mergejoin = off' \
  -c 'set enable_seqscan = off' -c 'set enable_bitmapscan = off' \
  -c 'explain (costs off) select count(*) from toast1 join toast3 on toast3.k = toast1.a' \
  -c 'select count(*) from toast1 join toast3 on toast3.k = toast1.a' 2>&1 |
  grep -cxE 'SET|.*Index (Only )?Scan using toast3k on toast3|2')" = 6 ] ||
  fail "an index scan keyed by a value out of line"
[ "$(local_psql -c 'delete from toast1 where id = 11 returning length(a)' 2>&1 |
  tr '\n' ,)" = "100001,DELETE 1," ] ||
  fail "DELETE ... RETURNING reads a value out of line"

# Verify that ALTER SYSTEM rewrites postgresql.auto.conf one session at a
# time (guc.c's pcb->auto_file_mutex).
# 1. In 8 sessions at once, each sets its own setting with ALTER SYSTEM.
# 2. The file holds all 8, each once: none was lost to another's rewrite.
# 3. ALTER SYSTEM RESET ALL leaves none.
pids=()
for i in $(seq 8); do
  local_psql -c "alter system set application_name = 'as$i'" \
    -c "alter system set statement_timeout = '${i}s'" \
    -c "alter system set \"dg.as$i\" = 'v$i'" > "$work/as$i.out" 2>&1 &
  pids+=($!)
done
wait "${pids[@]}"
[ "$(grep -c "^dg\.as[1-8] = 'v[1-8]'$" "$work/data/postgresql.auto.conf")" \
  = 8 ] || fail "ALTER SYSTEM from 8 sessions"
local_psql -c 'alter system reset all' > "$work/asreset.out" 2>&1
grep -q "^dg\.as" "$work/data/postgresql.auto.conf" &&
  fail "ALTER SYSTEM RESET ALL"

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

# Verify that numeric refuses what a decimal of 36 digits has no value for,
# as PostgreSQL words each error.
# 1. NaN and infinity are not values; text past 36 digits is refused.
# 2. Division by zero, the root of a negative, the logarithm of zero.
# 3. A precision past 36, a field overflow, a result past 36 digits, and
#    a round or trunc of a numeric of at most 17 digits to more of them.
# 4. An int8 and a decimal64 have no common type, and decimal64 holds at
#    most 17 digits.
for c in "'NaN'::numeric|invalid input syntax for type numeric" \
  "'Infinity'::numeric|invalid input syntax for type numeric" \
  "'0.123456789012345678901234567890123456789'::numeric|value overflows numeric format" \
  "1 / 0::numeric|division by zero" \
  "1.5 % 0.0|division by zero" \
  "sqrt(-1::numeric)|cannot take square root of a negative number" \
  "ln(0::numeric)|cannot take logarithm of zero" \
  "1::numeric(50,2)|NUMERIC precision 50 must be between 1 and 36" \
  "12345.6::numeric(5,2)|numeric field overflow" \
  "round(99999999999999999::numeric(17,0), -1)|value overflows numeric format" \
  "round(1.5, 20)|value overflows numeric format" \
  "trunc(1.5, 20)|value overflows numeric format" \
  "coalesce(1::int8, 1.5)|COALESCE could not convert type numeric to bigint" \
  "1::decimal64(20,2)|NUMERIC precision 20 must be between 1 and 17" \
  "999999999999999999999999999999999999::numeric * 10|value overflows numeric format"; do
  local_psql -c "select ${c%%|*}" 2>&1 | grep -q "${c#*|}" ||
    fail "numeric refuses ${c%%|*}"
done

# Verify that numeric columns are stored in sm as decimals at their scale,
# indexed, keyed and analyzed.
# 1. Make t20 with numeric(10,2), numeric(30,5) and a bare numeric, which is
#    numeric(17,3), and indexes; values are rounded to the column's scale.
# 2. A value past the precision is refused, as an assignment is.
# 3. Index scans find = and ranges whose keys have more places than the
#    column, and keys past its range.
# 4. A unique numeric(30,5) index refuses a value that rounds to a taken one.
# 5. ANALYZE gives common values at the column's scale, and = is estimated.
# 6. numeric(17,17) and numeric(36,0), the widest of sm's decimal64 and
#    decimal128, keep their largest values; numeric(37,0) is refused.
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
local_psql -c 'create table t24 (a numeric(17,17), b numeric(36,0))' \
  -c 'insert into t24 values (0.99999999999999999, 999999999999999999999999999999999999),
      (-0.99999999999999999, -999999999999999999999999999999999999)' >> "$work/numeric.out" 2>&1
[ "$(local_psql -c 'select * from t24 order by a' 2>&1 | tr '\n' ,)" = \
  "-0.99999999999999999|-999999999999999999999999999999999999,0.99999999999999999|999999999999999999999999999999999999," ] ||
  fail "numeric columns at sm's widest"
local_psql -c 'create table t25 (a numeric(37,0))' 2>&1 |
  grep -q 'must be between 1 and 36' || fail "numeric(37,0) refused"

# Verify that a column neither grouped nor aggregated is refused as
# PostgreSQL refuses it, and that one a grouped primary key determines is
# not: the key is the table's primary index, as digres has no pg_constraint.
# 1. A column beside an aggregate, with no GROUP BY, of a table, a temporary
#    table and a compiled-in catalog, is refused, named as PostgreSQL names
#    it.
# 2. One beside a GROUP BY of another column that is no key is refused.
# 3. Grouping by the primary key lets the table's other columns be selected,
#    through a join too; a catalog has no key here, and is refused.
# 4. Of a table with a primary key of two columns, a unique index and
#    another, grouping by the whole key lets the rest be selected, by part of
#    it or by the unique index's column does not.
local_psql -c 'create table t28 (id int4 primary key, v text, n int4)' \
  -c "insert into t28 values (1, 'a', 10), (2, 'b', 20), (3, 'b', 30)" \
  -c 'create table t30 (id int4, k int4, u int4, v text, primary key (id, k))' \
  -c 'create unique index t30u on t30 (u)' -c 'create index t30v on t30 (v)' \
  -c "insert into t30 values (1, 1, 10, 'a'), (1, 2, 20, 'b')" \
  > "$work/groupby.out" 2>&1
for c in "select v, sum(n) from t28|column \"t28.v\" must appear in the GROUP BY clause or be used in an aggregate function" \
  "create temp table t29 (a int4); select a, count(*) from t29|column \"t29.a\" must appear in the GROUP BY clause" \
  "select relname, count(*) from pg_class|column \"pg_class.relname\" must appear in the GROUP BY clause" \
  "select v, sum(n) from t28 group by n|column \"t28.v\" must appear in the GROUP BY clause" \
  "select relname from pg_class group by oid|column \"pg_class.relname\" must appear in the GROUP BY clause" \
  "select id, v from t30 group by id|column \"t30.v\" must appear in the GROUP BY clause" \
  "select u, v from t30 group by u|column \"t30.v\" must appear in the GROUP BY clause"; do
  local_psql -c "${c%%|*}" 2>&1 | grep -qF "${c#*|}" ||
    fail "an ungrouped column refused: ${c%%|*}"
done
[ "$(local_psql -c 'select id, v, sum(n) from t28 group by id order by id' \
  -c 'select x.id, x.v, count(*) from t28 x join t28 y on x.id = y.id
      group by x.id order by 1' \
  -c 'select id, k, u, v from t30 group by id, k order by k' 2>&1 |
  tr '\n' ,)" = "1|a|10,2|b|20,3|b|30,1|a|1,2|b|1,3|b|1,1|1|10|a,1|2|20|b," ] ||
  fail "a column the grouped primary key determines"

# Verify that numerics bound and fetched in binary, as a driver does, are
# PostgreSQL's numeric to the client, both decimals of digres alike.
# 1. build/numbin binds values typed 1700 in numeric's binary format into
#    numeric(10,2) and numeric(30,5): rounded to each, and one of 21 digits
#    refused by the first; it compares with them and computes.
# 2. It fetches in binary: oid 1700 and numeric's digits, weight, sign and
#    scale for both columns and what is computed of them.
# 3. COPY in binary format writes both columns and reads them back.
build/numbin "host=$work port=$port dbname=postgres" > "$work/numbin.out" 2>&1
cat > "$work/numbin.expected" <<'OUT'
insert into nb values ($1, $2): INSERT 0 1
insert into nb (b) values ($1): INSERT 0 1
insert into nb (a) values ($1): value overflows numeric format
delete from nb where b = $1 * 2 - $1: DELETE 1
select a, b, a * b, sum(a) over (), -a from nb:
  a: oid 1700 nd=3 w=1 pos dscale=2 digits=1,2345,6800
  b: oid 1700 nd=3 w=1 pos dscale=5 digits=1,2345,6780
  ?column?: oid 1700 nd=5 w=2 pos dscale=7 digits=1,5241,5789,9710,4000
  sum: oid 1700 nd=3 w=1 pos dscale=2 digits=1,2345,6800
  ?column?: oid 1700 nd=3 w=1 neg dscale=2 digits=1,2345,6800
OUT
cmp -s "$work/numbin.out" "$work/numbin.expected" ||
  fail "numerics bound and fetched in binary"
[ "$(local_psql -c 'create table t26 (a numeric(10,2), b numeric(30,5))' \
  -c 'create table t27 (a numeric(10,2), b numeric(30,5))' \
  -c "insert into t26 values (-12345.67, 123456789012345678901234.12345),
      (0.01, null)" \
  -c "copy t26 to '$work/numeric.bin' with (format binary)" \
  -c "copy t27 from '$work/numeric.bin' with (format binary)" \
  -c 'select * from t27 order by a' 2>&1 | tail -2 | tr '\n' ,)" = \
  "-12345.67|123456789012345678901234.12345,0.01|," ] ||
  fail "numerics copied in binary"

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
# 4. Confirm max_pred_locks_per_transaction is not a setting, and the data
#    directory has no pg_serial.
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
for guc in max_pred_locks_per_transaction; do
  local_psql -c "show $guc" 2>&1 |
    grep -q 'unrecognized configuration parameter' || fail "setting $guc"
done
[ ! -e "$work/data/pg_serial" ] || fail "no pg_serial"

# Verify that an index scan keyed by a constant of another integer type, an
# int4 on an int8 column, as most are, reads only the key's range: two
# SERIALIZABLE transactions that each read one key by the index and insert
# one far from the other's do not conflict.  Read without a bound, each
# range would be the whole index and hold the other's insert, and the
# second would fail with SQLSTATE 40001.
# 1. Create tb1 (a int8 primary key) with rows 1 to 100.
# 2. In two sessions, with only index scans, begin SERIALIZABLE, read a = 5
#    and a = 50, and insert 200 and 300; commit the first, then the second.
# 3. Confirm both commit.
local_psql -c 'create table tb1 (a int8 primary key)' \
  -c 'insert into tb1 select generate_series(1, 100)' > "$work/bound.out" 2>&1
for n in 1 2; do
  mkfifo "$work/bound$n.in"
  psql -X -A -t -v VERBOSITY=verbose -h "$work" -p "$port" -d postgres \
    < "$work/bound$n.in" > "$work/bound$n.out" 2>&1 &
  eval "bound$n=\$!"
done
exec 5> "$work/bound1.in" 6> "$work/bound2.in"
for n in 5 6; do
  echo "set enable_seqscan = off; set enable_bitmapscan = off;
    begin isolation level serializable;" >&$n
done
echo "select a from tb1 where a = 5; insert into tb1 values (200);
  select 'inserted';" >&5
echo "select a from tb1 where a = 50; insert into tb1 values (300);
  select 'inserted';" >&6
for _ in $(seq 50); do
  grep -q '^inserted$' "$work/bound1.out" &&
    grep -q '^inserted$' "$work/bound2.out" && break
  sleep 0.1
done
echo "commit;" >&5
exec 5>&-
wait $bound1
echo "commit;" >&6
exec 6>&-
wait $bound2
grep -q '^COMMIT$' "$work/bound1.out" && grep -q '^COMMIT$' "$work/bound2.out" ||
  fail "an index scan of an int4 key on an int8 column is bounded"

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
# 8. The second's UPDATE joins tj: the re-check uses the tj row it joined,
#    not a newer one, as PostgreSQL's EvalPlanQual fetches it again
#    (ROW_MARK_REFERENCE), and digres keeps a copy of (ROW_MARK_COPY).  Joined on b, which the first changed, it no
#    longer matches, and updates none; joined on a, it still does, and
#    updates the first's row, though the first also moved the tj row it
#    joined out of 'EU'.
# 9. Joined to tw1 itself, the copy's re-check reads the version it joined,
#    as PostgreSQL's does, so the second updates the row.
# 10. The first changes row 3's key: the second's update of it, found by
#    another column, fails with a serialization failure, as PostgreSQL's
#    does of a row moved to another partition (../sm/ROWKEY.md section 2.3);
#    so does a FOR UPDATE of it.
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
  grep -q 'duplicate key value violates unique constraint "tw1_pkey"' ||
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

# Run the writes $1 in a session of its own and hold its transaction, run
# the statements given after them in another, which waits, commit the
# first, and print what the other said, on one line.
after_held() {
  local held=$1
  shift
  mkfifo "$work/write.in"
  local_psql < "$work/write.in" > "$work/write_held.out" 2>&1 &
  local holder=$!
  exec 4> "$work/write.in"
  echo "begin; $held select 'held';" >&4
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

# after_held, holding row 3 of tw1 updated (b + 1000).
after_update() {
  after_held 'update tw1 set b = b + 1000 where a = 3;' "$@"
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
local_psql -c 'insert into tw1 values (3, 30)' \
  -c 'create table tj (k int4 primary key, region text)' \
  -c "insert into tj values (3, 'EU'), (30, 'EU')" > /dev/null 2>&1
[ "$(after_update -c "update tw1 set b = b + 1 from tj where tj.k = tw1.b
  and tj.region = 'EU' and tw1.a = 3")" = "UPDATE 0 " ] ||
  fail "a join's re-check uses the row it joined"
[ "$(after_held "update tw1 set b = b + 1000 where a = 3;
  update tj set region = 'US' where k = 3;" \
  -c "update tw1 set b = b + 1 from tj where tj.k = tw1.a
  and tj.region = 'EU' returning tw1.b")" = "2031 UPDATE 1 " ] ||
  fail "a join's re-check that still holds, on the row it joined"
[ "$(after_update -c 'update tw1 set b = 0 from tw1 t2 where t2.a = tw1.a
  and tw1.a = 3 and t2.b < 3000')" = "UPDATE 1 " ] ||
  fail "a self-join's re-check reads the row it joined"
after_held 'update tw1 set a = 33 where a = 3;' -v VERBOSITY=verbose \
  -c 'update tw1 set b = 1 where b = 0' |
  grep -q 'ERROR:  40001: could not serialize access due to concurrent update' ||
  fail "update of a row whose key changed meanwhile"
after_held 'update tw1 set a = 3 where a = 33;' -v VERBOSITY=verbose \
  -c 'select a from tw1 where b = 0 for update' |
  grep -q 'ERROR:  40001' || fail "for update of a row whose key changed"
[ "$(local_psql -c 'select a, b from tw1 where b = 0')" = "3|0" ] ||
  fail "the row moved twice"
local_psql -c 'drop table tj' > /dev/null 2>&1
local_psql -c 'drop table tw1' > /dev/null 2>&1

# Verify that SELECT ... FOR UPDATE and FOR SHARE lock rows in sm, until the
# transaction ends, and read the newest version of a row changed meanwhile.
# 1. Each of the four strengths returns the rows a plain SELECT would, and a
#    row the transaction itself updated is returned as updated.
# 2. While one session holds row 3 updated, another's FOR UPDATE of it waits,
#    and then returns the first's row; with a WHERE the first's row no longer
#    meets, it returns none; of a row the first deleted, none.
# 3. While one session holds row 2 FOR UPDATE, another's UPDATE of it waits,
#    and a plain SELECT of it does not.
# 4. While one holds row 2 FOR SHARE, another's FOR SHARE passes, and
#    another's UPDATE waits.
# 5. At REPEATABLE READ, FOR UPDATE of a row changed since the snapshot fails
#    with a serialization failure; of a row unchanged, it and FOR SHARE lock
#    the row and return it, as at SERIALIZABLE.
# 6. NOWAIT: a row that is free is locked; one another session holds FOR
#    UPDATE, or has updated, fails at once with lock_not_available; a FOR
#    SHARE NOWAIT passes a FOR SHARE holder.
# 7. SKIP LOCKED: with one session holding the first ready row, another's
#    "first ready row" is the second, as two workers on a queue find; and a
#    scan of every row leaves out the one held, and one updated and not yet
#    committed. Neither waits.
local_psql -c 'create table tk1 (a int4 primary key, b int4)' \
  -c 'insert into tk1 values (1, 10), (2, 20), (3, 30), (4, 40)' \
  > "$work/rowlock.out" 2>&1
[ "$(local_psql -c 'select b from tk1 where a = 1 for update' \
  -c 'select b from tk1 where a = 1 for no key update' \
  -c 'select b from tk1 where a = 1 for share' \
  -c 'select b from tk1 where a = 1 for key share' \
  -c 'select count(*) from (select a from tk1 for update) s' 2>&1 |
  tr '\n' ' ')" = "10 10 10 10 4 " ] || fail "for update"
[ "$(local_psql -c 'begin' -c 'update tk1 set b = 11 where a = 1' \
  -c 'select b from tk1 where a = 1 for update' -c 'rollback' 2>&1 |
  grep -v '^[A-Z]' | tr '\n' ' ')" = "11 " ] || fail "for update of own row"

# Run the statement given first in a session that then holds its transaction
# open, run psql with the rest of the arguments in another, commit the first,
# and print "waited" or "passed", for whether the other waited behind the
# first (wait_state), then what the other said, on one line.
against() {
  mkfifo "$work/rowlock.in"
  local_psql < "$work/rowlock.in" > "$work/rowlock_held.out" 2>&1 &
  local holder=$!
  exec 4> "$work/rowlock.in"
  echo "begin; $1; select 'held';" >&4
  for _ in $(seq 50); do
    grep -qx held "$work/rowlock_held.out" && break
    sleep 0.1
  done
  local_psql -c "select 'pid ' || pg_backend_pid()" "${@:2}" \
    > "$work/rowlock_other.out" 2>&1 &
  local other=$!
  local state
  state=$(wait_state $other "$work/rowlock_other.out")
  echo "commit;" >&4
  exec 4>&-
  wait $holder
  wait $other
  rm "$work/rowlock.in"
  echo "$state $(grep -v '^pid [0-9]*$' "$work/rowlock_other.out" |
    tr '\n' ' ')"
}
[ "$(against 'update tk1 set b = b + 100 where a = 3' \
  -c 'select b from tk1 where a = 3 for update')" = "waited 130 " ] ||
  fail "for update after a concurrent update"
[ "$(against 'update tk1 set b = 0 where a = 3' \
  -c 'select b from tk1 where a = 3 and b > 100 for update')" = "waited " ] ||
  fail "for update rechecks its WHERE"
[ "$(against 'delete from tk1 where a = 4' \
  -c 'select b from tk1 where a = 4 for update')" = "waited " ] ||
  fail "for update of a row deleted meanwhile"
[ "$(against 'select b from tk1 where a = 2 for update' \
  -c 'update tk1 set b = b + 1 where a = 2 returning b')" = \
  "waited 21 UPDATE 1 " ] || fail "update waits for a for update"
[ "$(against 'select b from tk1 where a = 2 for update' \
  -c 'select b from tk1 where a = 2')" = "passed 21 " ] ||
  fail "select passes a for update"
[ "$(against 'select b from tk1 where a = 2 for share' \
  -c 'select b from tk1 where a = 2 for share')" = "passed 21 " ] ||
  fail "for share passes a for share"
[ "$(against 'select b from tk1 where a = 2 for share' \
  -c 'update tk1 set b = b + 1 where a = 2 returning b')" = \
  "waited 22 UPDATE 1 " ] || fail "update waits for a for share"
against 'update tk1 set b = b + 1 where a = 1' -v VERBOSITY=verbose \
  -c 'begin isolation level repeatable read' -c 'select count(*) from tk1' \
  -c 'select b from tk1 where a = 1 for update' |
  grep -q 'ERROR:  40001' || fail "for update at repeatable read"
[ "$(local_psql -c 'begin isolation level repeatable read' \
  -c 'select a from tk1 where a = 2 for update' \
  -c 'select a from tk1 where a = 2 for share' -c 'commit' \
  -c 'begin isolation level serializable' \
  -c 'select a from tk1 where a = 2 for update' -c 'commit' 2>&1 |
  grep -v '^[A-Z]' | tr '\n' ' ')" = "2 2 2 " ] ||
  fail "for update of an unchanged row above read committed"
[ "$(local_psql -c 'select b from tk1 where a = 1 for update nowait' 2>&1 |
  grep -c .)" = 1 ] || fail "for update nowait of a free row"
against 'select b from tk1 where a = 2 for update' -v VERBOSITY=verbose \
  -c 'select b from tk1 where a = 2 for update nowait' |
  grep -q 'passed ERROR:  55P03' || fail "for update nowait of a held row"
against 'update tk1 set b = b + 1 where a = 2' -v VERBOSITY=verbose \
  -c 'select b from tk1 where a = 2 for share nowait' |
  grep -q 'passed ERROR:  55P03' || fail "for share nowait of a written row"
[ "$(against 'select b from tk1 where a = 2 for share' \
  -c 'select a from tk1 where a = 2 for share nowait')" = "passed 2 " ] ||
  fail "for share nowait passes a for share"
[ "$(against 'select a from tk1 order by a limit 1 for update skip locked' \
  -c 'select a from tk1 order by a limit 1 for update skip locked')" = \
  "passed 2 " ] || fail "skip locked takes the next row"
[ "$(against 'select a from tk1 where a = 2 for update' \
  -c 'select a from tk1 order by a for update skip locked')" = \
  "passed 1 3 " ] || fail "skip locked leaves out a held row"
[ "$(against 'update tk1 set b = 0 where a = 3' \
  -c 'select a from tk1 order by a for share skip locked')" = \
  "passed 1 2 " ] || fail "skip locked leaves out a written row"
local_psql -c 'drop table tk1' > /dev/null 2>&1

# Verify that the parser refuses what digres has no part of, a publication
# or a subscription, by its statement or by a statement naming one, before
# it runs anything of the text; and refuses nothing else.
# 1. A publication's and a subscription's own statements are refused with
#    0A000 and digres's message.
# 2. Statements naming one as an object -- DROP, COMMENT ON, ALTER ... OWNER
#    -- are refused alike, and so is one in a SQL function's BEGIN ATOMIC
#    body.
# 3. A text whose second statement is refused runs neither: the table its
#    first would make is not there after.
# 4. The same kinds of statement naming a table run.
refused() {
  local_psql -v VERBOSITY=verbose -c "$1" 2>&1 |
    grep -q "ERROR:  0A000: $2 are not supported"
}
refused 'create publication p for all tables' publications ||
  fail "create publication is refused"
refused 'alter publication p add table t' publications ||
  fail "alter publication is refused"
refused "create subscription s connection 'x' publication p" subscriptions ||
  fail "create subscription is refused"
refused 'drop subscription s' subscriptions ||
  fail "drop subscription is refused"
refused 'drop publication p' publications ||
  fail "drop of a publication is refused"
refused "comment on publication p is 'x'" publications ||
  fail "comment on a publication is refused"
refused 'alter subscription s owner to nobody' subscriptions ||
  fail "alter owner of a subscription is refused"
refused 'create function fpub() returns int language sql begin atomic
  create publication p for all tables; select 1; end' publications ||
  fail "a publication in a function's body is refused"
refused 'create table tpub (a int); create publication p for all tables' \
  publications || fail "a text with a refused statement is refused"
local_psql -c 'select 1 from tpub' > /dev/null 2>&1 &&
  fail "a refused text runs none of its statements"
local_psql -c 'create table tpub (a int)' -c "comment on table tpub is 'x'" \
  -c 'alter table tpub rename to tpub2' -c 'drop table tpub2' \
  > "$work/pub.out" 2>&1 || fail "statements naming a table are not refused"

# Verify that INSERT ... ON CONFLICT finds the row that has a key before it
# writes, in sm, and does nothing or updates that row.
# 1. tu1 has a key, id, and a unique name. DO NOTHING on a taken id inserts
#    nothing, and on a free one inserts; with no column named, a taken name
#    is a conflict too.
# 2. DO UPDATE on a taken id updates that row from EXCLUDED, and RETURNING
#    gives the row as updated; on a free id it inserts; by name, it updates
#    the row that has the name; with a WHERE the row does not meet, it does
#    nothing.
# 3. With id the arbiter, a row whose id is free and whose name is taken
#    fails as an insert does, with a unique violation.
# 4. Several rows in one statement: the free are inserted and the taken
#    updated.
# 5. While a session has inserted an id and not committed, another's upsert
#    of it waits: after a commit it updates that row, and after a rollback
#    it inserts its own.
# 6. An upsert of a row another session is updating waits, and updates the
#    row as that session left it; of a row another deleted, it inserts.
# 7. At REPEATABLE READ, an id taken since the snapshot fails the upsert
#    with a serialization failure, and a row there before it is updated.
# 8. An upsert finds its row, and waits to lock it behind a session that has
#    updated it; that session then deletes the row and commits: the upsert
#    looks again, finds none, and inserts.
local_psql -c 'create table tu1 (id int4 primary key, name text unique, n int4)' \
  -c "insert into tu1 values (1, 'one', 1), (2, 'two', 1)" \
  > "$work/upsert.out" 2>&1
[ "$(local_psql -c "insert into tu1 values (1, 'uno', 9) on conflict (id) do nothing" \
  -c "insert into tu1 values (3, 'three', 1) on conflict (id) do nothing" \
  -c "insert into tu1 values (8, 'one', 9) on conflict do nothing" \
  -c 'select count(*), sum(n) from tu1' 2>&1 | tr '\n' ' ')" = \
  "INSERT 0 0 INSERT 0 1 INSERT 0 0 3|3 " ] || fail "on conflict do nothing"
[ "$(local_psql -c "insert into tu1 values (1, 'x', 5) on conflict (id)
    do update set n = tu1.n + excluded.n returning id, name, n" \
  -c "insert into tu1 values (4, 'four', 1) on conflict (id)
    do update set n = 0 returning id, name, n" \
  -c "insert into tu1 values (9, 'two', 7) on conflict (name)
    do update set n = excluded.n returning id, name, n" \
  -c "insert into tu1 values (3, 'z', 50) on conflict (id)
    do update set n = excluded.n where tu1.n > 10 returning n" 2>&1 |
  tr '\n' ' ')" = "1|one|6 INSERT 0 1 4|four|1 INSERT 0 1 2|two|7 INSERT 0 1 INSERT 0 0 " ] ||
  fail "on conflict do update"
local_psql -c "insert into tu1 values (9, 'one', 7) on conflict (id)
    do update set n = 0" 2>&1 |
  grep -q 'duplicate key value violates unique constraint "tu1_name_key"' ||
  fail "on conflict with another unique index"
[ "$(local_psql -c "insert into tu1 values (5, 'five', 1), (1, 'q', 100),
    (6, 'six', 1) on conflict (id) do update set n = excluded.n" \
  -c 'select id, n from tu1 order by id' 2>&1 | tr '\n' ' ')" = \
  "INSERT 0 3 1|100 2|7 3|1 4|1 5|1 6|1 " ] || fail "on conflict over several rows"

# Run the statement given first in a session, end its transaction with the
# word given second, run psql with the rest of the arguments in another
# session meanwhile, and print "waited" or "passed" (wait_state) and what the
# other said.
upsert_against() {
  mkfifo "$work/upsert.in"
  local_psql < "$work/upsert.in" > "$work/upsert_held.out" 2>&1 &
  local holder=$!
  exec 4> "$work/upsert.in"
  echo "begin; $1; select 'held';" >&4
  for _ in $(seq 50); do
    grep -qx held "$work/upsert_held.out" && break
    sleep 0.1
  done
  local_psql -c "select 'pid ' || pg_backend_pid()" "${@:3}" \
    > "$work/upsert_other.out" 2>&1 &
  local other=$!
  local state
  state=$(wait_state $other "$work/upsert_other.out")
  echo "$2;" >&4
  exec 4>&-
  wait $holder
  wait $other
  rm "$work/upsert.in"
  echo "$state $(grep -v '^pid [0-9]*$' "$work/upsert_other.out" |
    tr '\n' ' ')"
}
[ "$(upsert_against "insert into tu1 values (20, 'twenty', 1)" commit \
  -c "insert into tu1 values (20, 'vingt', 5) on conflict (id)
    do update set n = tu1.n + excluded.n returning name, n")" = \
  "waited twenty|6 INSERT 0 1 " ] || fail "upsert after a committed insert"
[ "$(upsert_against "insert into tu1 values (21, 'x21', 1)" rollback \
  -c "insert into tu1 values (21, 'y21', 5) on conflict (id)
    do update set n = 0 returning name, n")" = \
  "waited y21|5 INSERT 0 1 " ] || fail "upsert after a rolled back insert"
[ "$(upsert_against 'update tu1 set n = n + 1000 where id = 1' commit \
  -c "insert into tu1 values (1, 'z', 1) on conflict (id)
    do update set n = tu1.n + 1 returning n")" = \
  "waited 1101 INSERT 0 1 " ] || fail "upsert of a row updated meanwhile"
[ "$(upsert_against 'delete from tu1 where id = 2' commit \
  -c "insert into tu1 values (2, 'deux', 7) on conflict (id)
    do update set n = 0 returning name, n")" = \
  "waited deux|7 INSERT 0 1 " ] || fail "upsert of a row deleted meanwhile"
upsert_against "insert into tu1 values (30, 'thirty', 1)" commit \
  -v VERBOSITY=verbose -c 'begin isolation level repeatable read' \
  -c 'select count(*) from tu1' \
  -c "insert into tu1 values (30, 'trente', 1) on conflict do nothing" |
  grep -q 'ERROR:  40001' || fail "upsert at repeatable read"
[ "$(local_psql -c 'begin isolation level repeatable read' \
  -c "insert into tu1 values (3, 'z', 4) on conflict (id)
    do update set n = tu1.n + excluded.n returning n" -c 'commit' 2>&1 |
  tr '\n' ' ')" = "BEGIN 5 INSERT 0 1 COMMIT " ] ||
  fail "upsert of an old row at repeatable read"
[ "$(upsert_against 'update tu1 set n = n + 1 where id = 3' \
  'delete from tu1 where id = 3; commit' \
  -c "insert into tu1 values (3, 'trois', 7) on conflict (id)
    do update set n = 0 returning name, n")" = \
  "waited trois|7 INSERT 0 1 " ] || fail "upsert of a row deleted once found"
[ "$(local_psql -c 'select name, n from tu1 where id = 3')" = "trois|7" ] ||
  fail "the row an upsert inserted after its conflict went"
local_psql -c 'drop table tu1' > /dev/null 2>&1

# Verify that every statement that writes a table in sm runs above READ
# COMMITTED as it does at it, where no other session is in the way: sm reads
# and locks by other rules there, and a path taken only there is easily
# missed.
# 1. At REPEATABLE READ, and again at SERIALIZABLE, in one transaction over
#    a table of three rows: UPDATE of one row and of all, DELETE, INSERT, ON
#    CONFLICT DO UPDATE and DO NOTHING, FOR UPDATE, FOR SHARE NOWAIT and
#    SKIP LOCKED, COPY into the table and into an empty one, a temporary
#    table, TRUNCATE, and a ROLLBACK TO a savepoint.
# 2. Each statement answers as expected, and the table is as expected after
#    the commit.
printf '10\tten\t1\n11\televen\t1\n' > "$work/level.tsv"
for level in 'repeatable read' 'serializable'; do
  local_psql -c 'drop table if exists tv1' -c 'drop table if exists tv2' \
    -c 'create table tv1 (id int4 primary key, name text unique, n int4)' \
    -c "insert into tv1 values (1, 'one', 1), (2, 'two', 2), (3, 'three', 3)" \
    -c 'create table tv2 (id int4 primary key, name text, n int4)' \
    > "$work/level.out" 2>&1
  [ "$(local_psql -c "begin isolation level $level" \
    -c 'update tv1 set n = n + 10 where id = 1 returning n' \
    -c 'update tv1 set n = n + 1' \
    -c 'delete from tv1 where id = 3 returning id' \
    -c "insert into tv1 values (4, 'four', 4) returning id" \
    -c "insert into tv1 values (4, 'x', 9) on conflict (id)
      do update set n = tv1.n + excluded.n returning n" \
    -c "insert into tv1 values (1, 'x', 9) on conflict (id) do nothing" \
    -c 'select id from tv1 where id = 2 for update' \
    -c 'select id from tv1 where id = 2 for share nowait' \
    -c 'select count(*) from (select id from tv1 for update skip locked) s' \
    -c "copy tv1 from '$work/level.tsv'" -c "copy tv2 from '$work/level.tsv'" \
    -c 'select count(*) from tv2' \
    -c 'create temp table tv3 (x int4)' -c 'insert into tv3 values (1), (2)' \
    -c 'update tv3 set x = x + 1' -c 'delete from tv3 where x = 2' \
    -c 'select x from tv3' -c 'truncate tv2' -c 'select count(*) from tv2' \
    -c 'savepoint s' -c 'update tv1 set n = 0' -c 'rollback to s' \
    -c 'select sum(n) from tv1' -c 'commit' \
    -c 'select id, n from tv1 order by id' 2>&1 | grep -v '^[A-Z]' |
    tr '\n' ' ')" = \
    "11 3 4 13 2 2 3 2 3 0 30 1|12 2|3 4|13 10|1 11|1 " ] ||
    fail "the writes at $level"
done
local_psql -c 'drop table tv1' -c 'drop table tv2' > /dev/null 2>&1

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
# 8. The file COPY TO wrote is its owner's alone, and the server's umask,
#    every thread's, is as it was.
umask_before=$(grep '^Umask:' "/proc/$server/status")
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
  grep -q 'duplicate key value violates unique constraint "tc1_pkey"' ||
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
[ "$(stat -c %a "$work/copy.bin")" = 600 ] &&
  [ "$(grep '^Umask:' "/proc/$server/status")" = "$umask_before" ] ||
  fail "copy to a file leaves the umask"
local_psql -c 'drop table tc1' -c 'drop table tc2' > /dev/null 2>&1

# Verify that COPY into an empty table is a bulk load, from sorted files, and
# into a table with rows an insert a row (../sm/BULKLOAD.md).
# 1. Make tl1, keyed by a, with an index on b, and COPY 100000 rows into it,
#    enough to pass a small load: every row is counted, and both indexes
#    find theirs.
# 2. Into tl2, empty and keyed, COPY the same file with one key repeated at
#    its end: the COPY fails, and tl2 is left empty; the file without the
#    repeat then loads.
# 3. While a session has a COPY into empty tl3 uncommitted, another's read of
#    tl3 waits for it; a read begun after the commit sees every row.
# 4. While a session that read empty tl4 stays in its transaction, a COPY
#    into tl4 does not wait for it: its rows are inserted, and are there.
# 5. A COPY into tl1, which has rows, with a key that is taken fails at that
#    row and leaves tl1 as it was.
seq 1 100000 | awk '{ print $1 "\t" $1 % 1000 "\trow" $1 }' > "$work/load.tsv"
cp "$work/load.tsv" "$work/load_dup.tsv"
printf '77\t7\tagain\n' >> "$work/load_dup.tsv"
local_psql -c 'create table tl1 (a int4 primary key, b int4, c text)' \
  -c 'create index tl1_b on tl1 (b)' \
  -c 'create table tl2 (a int4 primary key, b int4, c text)' \
  -c 'create table tl3 (a int4 primary key, b int4, c text)' \
  -c 'create table tl4 (a int4 primary key, b int4, c text)' \
  > "$work/load.out" 2>&1
[ "$(local_psql -c "copy tl1 from '$work/load.tsv'" \
  -c 'select count(*), sum(b) from tl1' -c 'select c from tl1 where a = 54321' \
  -c 'select count(*) from tl1 where b = 999' 2>&1 | tr '\n' ' ')" = \
  "COPY 100000 100000|49950000 row54321 100 " ] || fail "bulk load"
local_psql -c "copy tl2 from '$work/load_dup.tsv'" 2>&1 |
  grep -q 'duplicate key value violates a unique index on "tl2"' ||
  fail "bulk load of a repeated key"
[ "$(local_psql -c 'select count(*) from tl2' \
  -c "copy tl2 from '$work/load.tsv'" -c 'select count(*) from tl2' 2>&1 |
  tr '\n' ' ')" = "0 COPY 100000 100000 " ] || fail "bulk load after a failed one"
mkfifo "$work/load.in"
local_psql < "$work/load.in" > "$work/load_held.out" 2>&1 &
loader=$!
exec 4> "$work/load.in"
echo "begin; copy tl3 from '$work/load.tsv'; select 'held';" >&4
for _ in $(seq 100); do
  grep -qx held "$work/load_held.out" && break
  sleep 0.1
done
local_psql -c 'select count(*) from tl3' > "$work/load_reader.out" 2>&1 &
reader=$!
sleep 1
kill -0 $reader 2> /dev/null || fail "a reader passed a bulk load"
echo "commit;" >&4
exec 4>&-
wait $loader
wait $reader
[ "$(local_psql -c 'select count(*) from tl3')" = 100000 ] ||
  fail "a reader after a bulk load"
mkfifo "$work/load_r.in"
local_psql < "$work/load_r.in" > "$work/load_r.out" 2>&1 &
reader=$!
exec 4> "$work/load_r.in"
echo "begin; select count(*) from tl4; select 'held';" >&4
for _ in $(seq 100); do
  grep -qx held "$work/load_r.out" && break
  sleep 0.1
done
[ "$(timeout 60 psql -X -A -t -h "$work" -p "$port" -d postgres \
  -c "copy tl4 from '$work/load.tsv'" -c 'select count(*) from tl4' 2>&1 |
  tr '\n' ' ')" = "COPY 100000 100000 " ] ||
  fail "a bulk load waited for a reader"
echo "commit;" >&4
exec 4>&-
wait $reader
printf '100001\t1\tnew\n77\t7\tagain\n' > "$work/load_more.tsv"
local_psql -c "copy tl1 from '$work/load_more.tsv'" 2>&1 |
  grep -q 'duplicate key value violates unique constraint "tl1_pkey"' ||
  fail "copy of a taken key into a table with rows"
[ "$(local_psql -c 'select count(*) from tl1')" = 100000 ] ||
  fail "a failed copy into a table with rows"
local_psql -c 'drop table tl1' -c 'drop table tl2' -c 'drop table tl3' \
  -c 'drop table tl4' \
  > /dev/null 2>&1

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

# Verify that a relation's name, looked up by oid with no lock, as regclass's
# output does, is what another session's DDL has left it: renamed, or gone.
# 1. A held session reads table rn1, which caches it and its index rn1_b, and
#    prints both by oid as regclass.
# 2. Another session renames rn1 to rn2 and drops rn1_b: the held session
#    prints rn2 and the index's bare oid.
# 3. Another session drops rn2: the held session prints its bare oid.
local_psql -c 'create table rn1 (a int4 primary key, b int4)' \
  -c 'create index rn1_b on rn1 (b)' > "$work/relname.out" 2>&1
rn_t=$(local_psql -c "select 'rn1'::regclass::oid")
rn_i=$(local_psql -c "select 'rn1_b'::regclass::oid")
held_start rn
held_send rn "select count(*) from rn1 where b = 1;"
held_send rn "select 'r1 ' || $rn_t::regclass || ' ' || $rn_i::regclass;"
held_wait rn "r1 rn1 rn1_b" || fail "relation's name"
local_psql -c 'alter table rn1 rename to rn2' -c 'drop index rn1_b' \
  >> "$work/relname.out" 2>&1
held_send rn "select 'r2 ' || $rn_t::regclass || ' ' || $rn_i::regclass;"
held_wait rn "r2 rn2 $rn_i" || fail "renamed relation's name"
local_psql -c 'drop table rn2' >> "$work/relname.out" 2>&1
held_send rn "select 'r3 ' || $rn_t::regclass;"
held_wait rn "r3 $rn_t" || fail "dropped relation's name"
held_end rn

# Verify that no thread takes the signals the postmaster reads from its
# signalfd: every thread, the postmaster's, sessions, sm's, RocksDB's and
# sm_sys.h's, blocks them, so no handler runs and no system call is
# interrupted.
# 1. With a session idle (opened above), list the server's threads.
# 2. Confirm each blocks SIGHUP, SIGINT, SIGQUIT and SIGTERM.
python3 - "$server" > "$work/sigblk.out" 2>&1 <<'PY' || fail "only the signalfd takes signals"
import os, sys
pid = sys.argv[1]
want = 0
for sig in (1, 2, 3, 15):
    want |= 1 << (sig - 1)
bad = []
for tid in os.listdir(f"/proc/{pid}/task"):
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

# Verify that pg_reload_conf() reloads the configuration: a session sends
# the postmaster SIGHUP, which the postmaster reads from its signalfd.
# 1. Call pg_reload_conf(): the postmaster logs the reload.
# 2. Confirm it took the signalfd for no listen socket, and still runs a
#    statement.
[ "$(local_psql -c 'select pg_reload_conf()')" = t ] || fail "pg_reload_conf"
for _ in $(seq 50); do
  grep -q "received SIGHUP, reloading configuration files" "$work/log" && break
  sleep 0.1
done
grep -q "received SIGHUP, reloading configuration files" "$work/log" ||
  fail "SIGHUP read"
grep -q "could not accept new connection" "$work/log" &&
  fail "the signalfd is no listen socket"
[ "$(local_psql -c 'select 3 + 3')" = 6 ] || fail "a statement after SIGHUP"

# Verify that pg_conf_load_time() is when the main thread last applied the
# configuration file, which every session reads (pcb->PgReloadTime): a
# session's own reload leaves it be.
# 1. Hold a session open, and note its pg_conf_load_time().
# 2. Give the data directory a configuration file, and call
#    pg_reload_conf(): a new session's pg_conf_load_time() is later.
# 3. The held session reports the same time as the new one.
# 4. Remove the file.
held_start loadtime
held_send loadtime "select 'lt0 ' || pg_conf_load_time();"
for _ in $(seq 50); do
  lt0=$(sed -n 's/^lt0 //p' "$work/loadtime.out")
  [ -n "$lt0" ] && break
  sleep 0.1
done
[ -n "$lt0" ] || fail "pg_conf_load_time in a held session"
: > "$work/data/postgresql.conf"
[ "$(local_psql -c 'select pg_reload_conf()')" = t ] ||
  fail "pg_reload_conf with a configuration file"
for _ in $(seq 50); do
  [ "$(local_psql -c "select pg_conf_load_time() > '$lt0'")" = t ] && break
  sleep 0.1
done
lt1=$(local_psql -c 'select pg_conf_load_time()')
[ "$(local_psql -c "select '$lt1'::timestamptz > '$lt0'")" = t ] ||
  fail "pg_conf_load_time after a reload"
held_send loadtime "select 'lt1 ' || (pg_conf_load_time() = '$lt1');"
held_wait loadtime "lt1 true" ||
  fail "a held session's pg_conf_load_time is the main thread's"
held_end loadtime
rm "$work/data/postgresql.conf"

# Verify that a session reads the program's name the main thread set
# (pcb->progname), as a new thread is no longer given a copy of it.
# 1. Connect with a switch the server does not know in PGOPTIONS.
# 2. Confirm the session refuses it, and its hint names the program.
PGOPTIONS="-Q" local_psql -c 'select 1' > "$work/progname.out" 2>&1 &&
  fail "a bad startup option is refused"
grep -q 'invalid command-line argument for server process: -Q' \
  "$work/progname.out" || fail "a bad startup option's error"
grep -q 'Try "digres[^"]* --help"' "$work/progname.out" ||
  fail "a session's hint names the program"

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
# and ends a session left open: the postmaster's SIGTERM reaches it as an
# interrupt bit, through its record in the session registry
# (session_interrupt()). Each side wakes the other's latch from another
# thread (sm_interrupt()), so a lost wake-up is a stall.
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

# Verify that max_connections holds as many sessions as the session registry
# takes, which PGPROC's free list did before (WORKNOTE.md step 159).
# 1. Start the server again with max_connections = 3, none reserved.
# 2. Hold three sessions open.
# 3. A fourth is refused: too many clients.
# 4. End the three, and a new session connects.
# 5. Stop the server.
$digres -D "$work/data" -k "$work" -p "$port" -c max_connections=3 \
  -c superuser_reserved_connections=0 -c reserved_connections=0 \
  > "$work/log2" 2>&1 &
server=$!
server_ready $server "$work/log2" || fail "max_connections: start"
for n in 1 2 3; do
  held_start "conn$n"
  held_send "conn$n" "select 'up $n';"
done
for n in 1 2 3; do
  held_wait "conn$n" "up $n" || fail "max_connections: session $n"
done
local_psql -c 'select 1' > "$work/conn4.out" 2>&1 &&
  fail "max_connections: a fourth session is refused"
grep -q 'sorry, too many clients already' "$work/conn4.out" ||
  fail "max_connections: too many clients"
for n in 1 2 3; do
  held_end "conn$n"
done
# A session leaves the registry as its thread ends, a moment after its psql.
for _ in $(seq 50); do
  after=$(local_psql -c 'select 5 + 5' 2> "$work/conn5.err")
  [ "$after" = 10 ] && break
  sleep 0.1
done
[ "$after" = 10 ] || fail "max_connections: a session after the three"
kill -INT $server
wait $server || fail "max_connections: shutdown status"

# Verify that an immediate shutdown (SIGQUIT) tells every client why its
# session ends: each session ends its own thread, not the server, so none
# ends the others before they have told theirs.
# 1. Start the server again.
# 2. Open five sessions: three run a long statement, two sit idle.
# 3. Send the server SIGQUIT: it stops within ten seconds, with status 0.
# 4. Each busy client got the warning; each idle one gets it as it sends
#    its next statement.
$digres -D "$work/data" -k "$work" -p "$port" > "$work/log3" 2>&1 &
server=$!
server_ready $server "$work/log3" || fail "immediate shutdown: start"
for n in 1 2 3; do
  bg_start "imm_busy$n" -c "select 'busy'" -c 'select pg_sleep(60)'
done
for n in 1 2; do
  held_start "imm_idle$n"
  held_send "imm_idle$n" "select 'up $n';"
  held_wait "imm_idle$n" "up $n" || fail "immediate shutdown: idle session $n"
done
# Each busy session is in its long statement once it has said so.
for _ in $(seq 50); do
  [ "$(cat "$work"/imm_busy?.out | grep -cx busy)" = 3 ] && break
  sleep 0.1
done
kill -QUIT $server
for _ in $(seq 100); do
  kill -0 $server 2> /dev/null || break
  sleep 0.1
done
if kill -0 $server 2> /dev/null; then
  fail "immediate shutdown: the server stops"
  kill -KILL $server
fi
wait $server || fail "immediate shutdown: status"
for n in 1 2 3; do
  bg_end "imm_busy$n"
  grep -q 'terminating connection due to immediate shutdown command' \
    "$work/imm_busy$n.out" || fail "immediate shutdown: busy client $n told"
done
for n in 1 2; do
  held_send "imm_idle$n" "select 'after';"
  held_end "imm_idle$n"
  grep -q 'terminating connection due to immediate shutdown command' \
    "$work/imm_idle$n.out" || fail "immediate shutdown: idle client $n told"
done

# Verify that a smart shutdown (SIGTERM) waits for its sessions: the
# postmaster reads it from its signalfd, refuses new sessions, and stops
# once the last has ended.
# 1. Start the server again, and hold a session open.
# 2. Send the server SIGTERM: it logs the request and refuses a new session,
#    but keeps running.
# 3. End the held session: the server stops within ten seconds, status 0.
$digres -D "$work/data" -k "$work" -p "$port" > "$work/log4" 2>&1 &
server=$!
server_ready $server "$work/log4" || fail "smart shutdown: start"
held_start smart
held_send smart "select 'up';"
held_wait smart "up" || fail "smart shutdown: session"
kill -TERM $server
for _ in $(seq 50); do
  grep -q "received smart shutdown request" "$work/log4" && break
  sleep 0.1
done
grep -q "received smart shutdown request" "$work/log4" ||
  fail "smart shutdown: request read"
local_psql -c 'select 1' > "$work/smart.out" 2>&1 &&
  fail "smart shutdown: a new session is refused"
kill -0 $server 2> /dev/null || fail "smart shutdown: waits for its session"
held_end smart
for _ in $(seq 100); do
  kill -0 $server 2> /dev/null || break
  sleep 0.1
done
if kill -0 $server 2> /dev/null; then
  fail "smart shutdown: the server stops"
  kill -KILL $server
fi
wait $server || fail "smart shutdown: status"

grep -q "terminated by signal" "$work/log" "$work/log2" "$work/log3" \
  "$work/log4" && fail "a process crashed"
grep -q "ThreadSanitizer" "$work/log" "$work/log2" "$work/log3" \
  "$work/log4" && fail "a thread sanitizer report"

[ $failed = 0 ] && echo "server: PASSED"
exit $failed
