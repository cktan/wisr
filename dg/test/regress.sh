#!/bin/bash
# regress.sh -- run PostgreSQL 18's regression suite on digres, and report
# which missing features cost the most files. It only measures: it exits 0
# whatever it finds.
#
#   test/regress.sh           every file of parallel_schedule
#   test/regress.sh FILE...   these
#
# The files, their expected output and pg_regress are those of the
# PostgreSQL 18 source tree built in $PG18_SRC, ~/p/postgres18 if that is
# not set. Each file runs, one at a time, in a database of its own, after
# test_setup, which makes the tables most files read; a file that runs
# past 180 seconds is stopped. The server runs on a data directory initdb
# makes in build/test/regress, at port DG_TEST_PORT or one the kernel finds
# free; DIGRES names the server, build/digres if it is not set. A crash
# stops the server, and it is started again for the next file.
#
# build/test/regress/ holds, per file, pg_regress's output, its diff and
# the server's log while it ran (FILE/); and, made by test/regress/report.py
# from them: summary, causes, crashes and setup (test/regress/README.md).
cd "$(dirname "$0")/.." || exit 1
pg=${PG18_SRC:-$HOME/p/postgres18}
src=$pg/src/test/regress
work=$PWD/build/test/regress
port=${DG_TEST_PORT:-$(python3 -c 'import socket; s = socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')}
digres=${DIGRES:-build/digres}
if [ ! -x "$src/pg_regress" ]; then
  echo "regress: SKIPPED (no PostgreSQL 18 build in $src; set PG18_SRC)"
  exit 0
fi
# PostgreSQL 18's psql, the one pg_regress is told to run too.
bindir=$pg/src/bin/psql
files=("$@")
[ ${#files[@]} -gt 0 ] || files=($(sed -n 's/^test: //p' \
  "$src/parallel_schedule" | tr ' ' '\n' | grep -vx 'test_setup'))
rm -rf "$work"
mkdir -p "$work"
# A socket path over 107 bytes cannot be bound: the socket goes in a short
# directory of its own, removed at the end.
sock=$(mktemp -d "${TMPDIR:-/tmp}/dgr.XXXXXX")
server=

# Run PostgreSQL 18's psql on the socket, in the database postgres, with
# the arguments given; a server that does not answer in 60 seconds fails.
admin_psql() {
  timeout -k 5 60 "$bindir/psql" -X -q -h "$sock" -p "$port" -d postgres "$@"
}

# Start the server on $work/data, making it with initdb if it is not
# there, and wait until it says it is ready, ten seconds at most.
start_server() {
  if [ ! -d "$work/data" ]; then
    build/initdb "$work/data" >> "$work/initdb.out" 2>&1 || return 1
  fi
  local from
  from=$(stat -c %s "$work/log" 2> /dev/null || echo 0)
  $digres -D "$work/data" -k "$sock" -p "$port" >> "$work/log" 2>&1 &
  server=$!
  for _ in $(seq 100); do
    tail -c +$((from + 1)) "$work/log" |
      grep -q "ready to accept connections" && return 0
    kill -0 $server 2> /dev/null || return 1
    sleep 0.1
  done
  return 1
}

# Start the server again after a crash or a hang: on the same data
# directory, or if it will not start there, on a new one.
restart_server() {
  kill -KILL $server 2> /dev/null
  wait $server 2> /dev/null
  start_server && return 0
  echo "regress: the server would not start again on its data; a new one" \
    >> "$work/log"
  kill -KILL $server 2> /dev/null
  wait $server 2> /dev/null
  rm -rf "$work/data"
  start_server
}

# Make the database regression afresh, with nothing left of the file
# before: its database, the roles and databases it made, its sessions.
fresh_database() {
  admin_psql -c 'drop database if exists regression with (force)' &&
    admin_psql -At -c "select format('drop database if exists %I with
      (force)', datname) from pg_database where datname like 'regression%'
      union all select format('drop role if exists %I', rolname) from
      pg_roles where rolname like 'regress%'" |
    admin_psql &&
    admin_psql -c 'create database regression'
}

if ! start_server; then
  echo "regress: the server did not start (see $work/log)"
  kill -KILL $server 2> /dev/null
  rm -rf "$sock"
  exit 0
fi

n=0
for t in "${files[@]}"; do
  n=$((n + 1))
  mkdir -p "$work/$t"
  if ! fresh_database > "$work/$t/db" 2>&1; then
    # The server is gone or does not answer: the file before left it so.
    echo "regress: no fresh database for $t; the server is restarted" \
      >> "$work/log"
    restart_server && fresh_database >> "$work/$t/db" 2>&1
  fi
  from=$(stat -c %s "$work/log")
  # test_setup first, then the file, in the same database. pg_regress
  # finds the .sql and expected files under --inputdir; timeout stops
  # its whole process group, the psql it runs with it.
  (cd "$src" && timeout -k 10 180 ./pg_regress --use-existing \
    --host="$sock" --port="$port" --bindir="$bindir" --inputdir="$src" \
    --outputdir="$work/$t" --dlpath="$src" --dbname=regression \
    test_setup "$t") > "$work/$t/out" 2>&1
  rc=$?
  tail -c +$((from + 1)) "$work/log" > "$work/$t/server.log"
  if ! kill -0 $server 2> /dev/null ||
    grep -qE "TRAP:|terminated by signal|PANIC:|Sanitizer" \
      "$work/$t/server.log"; then
    got=crash
  elif [ $rc = 124 ] || [ $rc = 137 ]; then
    got=timeout
  elif grep -qE "^ok [0-9]+ +[-+] $t " "$work/$t/out"; then
    got=pass
  else
    got=fail
  fi
  echo "$got" > "$work/$t/status"
  printf '%3d/%d %-28s %s\n' $n ${#files[@]} "$t" "$got"
  [ $got = crash ] || [ $got = timeout ] && ! kill -0 $server 2> /dev/null &&
    restart_server
done

# Stop the server: a fast shutdown, then a kill if it will not go.
kill -INT $server 2> /dev/null
for _ in $(seq 100); do
  kill -0 $server 2> /dev/null || break
  sleep 0.1
done
kill -KILL $server 2> /dev/null
wait $server 2> /dev/null
rm -rf "$sock"

python3 test/regress/report.py "$work" "${files[@]}"
exit 0
