#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# isolation.sh -- run PostgreSQL 18's isolation specs on digres, with its
# isolationtester, and compare each outcome with test/isolation_specs.txt.
#
#   test/isolation.sh           every spec the list names
#   test/isolation.sh SPEC...   these
#
# The specs, their expected output and pg_isolation_regress are those of
# the PostgreSQL 18 source tree built in $PG18_SRC, ~/p/postgres18 if that
# is not set. Each spec runs in a database of its own; a step that waits
# longer than 20 seconds is cancelled, a spec that runs past 180 seconds
# stopped. The server runs on a data directory initdb makes in
# build/test/isolation, at port DG_TEST_PORT or one the kernel finds free;
# DIGRES names the server, build/digres if it is not set. What each spec
# printed and its diff are in build/test/isolation/SPEC/.
cd "$(dirname "$0")/.." || exit 1
src=${PG18_SRC:-$HOME/p/postgres18}/src/test/isolation
work=$PWD/build/test/isolation
port=${DG_TEST_PORT:-$(python3 -c 'import socket; s = socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')}
digres=${DIGRES:-build/digres}
if [ ! -x "$src/pg_isolation_regress" ]; then
  echo "isolation: SKIPPED (no PostgreSQL 18 build in $src; set PG18_SRC)"
  exit 0
fi
# The tree's own libpq, for isolationtester: a system libpq of an older
# PostgreSQL lacks what it calls, and it then fails to start.
export LD_LIBRARY_PATH=$src/../../interfaces/libpq${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
declare -A want
while read -r spec outcome _; do
  want[$spec]=$outcome
done < <(grep -v '^#' test/isolation_specs.txt | grep -v '^$')
specs=("$@")
[ ${#specs[@]} -gt 0 ] || specs=($(grep -v '^#' test/isolation_specs.txt |
  awk 'NF { print $1 }'))
rm -rf "$work"
mkdir -p "$work"

# Start the server on a data directory initdb makes, and wait until it
# says it is ready, ten seconds at most.
build/initdb "$work/data" > "$work/initdb.out" 2>&1 || {
  echo "isolation: FAILED (initdb; see $work)"
  exit 1
}
$digres -D "$work/data" -k "$work" -p "$port" > "$work/log" 2>&1 &
server=$!
for _ in $(seq 100); do
  grep -q "ready to accept connections" "$work/log" && break
  sleep 0.1
done
if ! grep -q "ready to accept connections" "$work/log"; then
  echo "isolation: FAILED (the server did not start; see $work/log)"
  kill -KILL $server 2> /dev/null
  exit 1
fi

failed=0
passed=0
for spec in "${specs[@]}"; do
  # A database of its own: a spec that failed may leave its tables, and
  # its sessions, which FORCE ends.
  psql -X -q -h "$work" -p "$port" -d postgres \
    -c 'drop database if exists isolation_regression with (force)' \
    -c 'create database isolation_regression' > "$work/$spec.db" 2>&1
  # pg_isolation_regress finds isolationtester beside it, and the specs
  # under its own directory.
  (cd "$src" && PGISOLATIONTIMEOUT=20 timeout 180 ./pg_isolation_regress \
    --use-existing --host="$work" --port="$port" --inputdir="$src" \
    --outputdir="$work/$spec" --dbname=isolation_regression "$spec") \
    > "$work/$spec.log" 2>&1
  case $? in
  0) got=pass ;;
  124) got=fail; pkill -f "isolationtester.*$spec" 2> /dev/null ;;
  *) got=fail ;;
  esac
  [ "$got" = pass ] && passed=$((passed + 1))
  if [ "$got" != "${want[$spec]:-pass}" ]; then
    if [ "$got" = fail ]; then
      echo "$spec: FAILED ($work/$spec/regression.diffs)"
      failed=1
    else
      echo "$spec: passes now; mark it pass in test/isolation_specs.txt"
    fi
  fi
done

# Stop the server, and confirm its log tells of no crash.
kill -INT $server
wait $server
if grep -qE "terminated by signal|ThreadSanitizer|TRAP:" "$work/log"; then
  echo "isolation: FAILED (a crash or a report in $work/log)"
  failed=1
fi
[ $failed = 0 ] &&
  echo "isolation: PASSED ($passed of ${#specs[@]} specs pass, as listed)"
exit $failed
