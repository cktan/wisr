#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# run.sh -- run each test/NAME.sql in a session of a server, with psql,
# and compare what it prints with test/expected/NAME.out. Each is run
# again in 8 sessions at once, and each must print the same.
#
#   test/run.sh            run the tests
#   test/run.sh --accept   write what is printed now as expected
#
# psql stops at a file's first error, and prints each statement before
# what it gave. The server runs on a data directory initdb makes in
# build/test/run, at port DG_TEST_PORT, or if that is not set one the
# kernel finds free, as server.sh's does. DIGRES names the server to run,
# build/digres if it is not set: "make tsan" sets it to the build under
# the thread sanitizer, and then a report in the log fails.
#
# What differs is left in build/test/NAME.out and NAME.diff, and in
# NAME.threadN.out and NAME.threadN.diff for each of the 8 sessions.
cd "$(dirname "$0")/.." || exit 1
if ! command -v psql > /dev/null; then
  echo "run: SKIPPED (no psql to connect with)"
  exit 0
fi
work=$PWD/build/test/run
# A free port: the kernel's pick for a socket bound to port 0, released
# before the server binds it.
port=${DG_TEST_PORT:-$(python3 -c 'import socket; s = socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')}
digres=${DIGRES:-build/digres}
rm -rf "$work"
mkdir -p "$work"
failed=0
count=0

# Run the file FILE in a session of the server, with psql, what it prints
# and its errors on standard output; more arguments go to psql.
run_file() {
  local file=$1
  shift
  psql -X -a -q -v ON_ERROR_STOP=1 -h "$work" -p "$port" -d postgres \
    -f "$file" "$@" 2>&1
}

# Report test NAME as failed if build/test/OUT.out, what it printed,
# differs from what it is expected to print. The arguments: NAME OUT.
check() {
  if ! diff -u "test/expected/$1.out" "build/test/$2.out" \
      > "build/test/$2.diff" 2>&1; then
    echo "$1: FAILED (build/test/$2.diff)"
    failed=1
  fi
}

# Start the server on a data directory initdb makes, and wait until it
# says it is ready, ten seconds at most.
build/initdb "$work/data" > "$work/initdb.out" 2>&1 || {
  echo "run: FAILED (initdb; see $work)"
  exit 1
}
$digres -D "$work/data" -k "$work" -p "$port" > "$work/log" 2>&1 &
server=$!
for _ in $(seq 100); do
  grep -q "ready to accept connections" "$work/log" && break
  sleep 0.1
done
if ! grep -q "ready to accept connections" "$work/log"; then
  echo "run: FAILED (the server did not start; see $work/log)"
  kill -KILL $server 2> /dev/null
  exit 1
fi

for sql in test/*.sql; do
  name=$(basename "$sql" .sql)
  run_file "$sql" > "build/test/$name.out"
  count=$((count + 1))
  if [ "$1" = "--accept" ]; then
    cp "build/test/$name.out" "test/expected/$name.out"
    continue
  fi
  check "$name" "$name"
  # The same in 8 sessions at once, each of them a thread of the server;
  # waiting for those alone, not the server.
  pids=()
  for i in $(seq 8); do
    run_file "$sql" > "build/test/$name.thread$i.out" &
    pids+=($!)
  done
  wait "${pids[@]}"
  for i in $(seq 8); do
    check "$name" "$name.thread$i"
  done
done

# Verify that a FATAL ends its session's thread, not the server.
# 1. Run select_error.sql in 8 sessions at once with exit_on_error set,
#    which makes its ERROR a FATAL in every session.
# 2. Confirm each session printed what ran before the error, then the
#    FATAL, and no other error.
# 3. Confirm the server still runs a statement after them.
if [ "$1" != "--accept" ]; then
  pids=()
  for i in $(seq 8); do
    PGOPTIONS="-c exit_on_error=on" run_file test/select_error.sql \
      > "build/test/fatal$i.out" &
    pids+=($!)
  done
  wait "${pids[@]}"
  for i in $(seq 8); do
    out="build/test/fatal$i.out"
    if ! grep -q "FATAL:  division by zero" "$out" ||
        ! grep -qx " *42" "$out" ||
        grep -E "ERROR:|FATAL:" "$out" | grep -qv "FATAL:  division by zero"; then
      echo "fatal: FAILED ($out)"
      failed=1
    fi
  done
  [ "$(psql -X -A -t -h "$work" -p "$port" -d postgres -c 'select 6 * 7' \
    2>&1)" = 42 ] || {
    echo "fatal: FAILED (the server after the FATALs; see $work/log)"
    failed=1
  }
fi

# Stop the server, promptly, and confirm its log tells of no crash.
kill -INT $server
for _ in $(seq 100); do
  kill -0 $server 2> /dev/null || break
  sleep 0.1
done
if kill -0 $server 2> /dev/null; then
  echo "run: FAILED (the server did not stop; see $work/log)"
  kill -KILL $server
  failed=1
fi
wait $server || {
  echo "run: FAILED (the server's exit status; see $work/log)"
  failed=1
}
if grep -qE "terminated by signal|ThreadSanitizer|TRAP:" "$work/log"; then
  echo "run: FAILED (a crash or a report in $work/log)"
  failed=1
fi

if [ "$1" = "--accept" ]; then
  echo "run: accepted $count files as expected"
elif [ $failed = 0 ]; then
  echo "run: PASSED ($count files, alone and in 8 sessions)"
fi
exit $failed
