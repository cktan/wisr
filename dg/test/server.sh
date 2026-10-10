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
local_psql -c "begin; prepare transaction 'x'" -c 'rollback' \
  -c 'select 5 + 5' > "$work/prepare.out" 2>&1
grep -q "PREPARE TRANSACTION is not supported" "$work/prepare.out" ||
  fail "prepare refused"
grep -qx 10 "$work/prepare.out" || fail "statement after prepare"

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

# Verify that the server stops when asked, with no process lost on the way,
# and ends a session left open: the postmaster's SIGTERM reaches it as a flag
# in its PGPROC (SignalProc()). Each side wakes the other's latch from
# another thread (sys_interrupt()), so a lost wake-up is a stall.
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
