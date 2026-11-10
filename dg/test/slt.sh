#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# slt.sh -- run the sqllogictest files under test/slt on a server, with
# build/slt, each in a database of its own (test/slt/README.md).
#
#   test/slt.sh                     run them all on digres
#   test/slt.sh FILE...             run only these
#   test/slt.sh --pg18 [FILE...]    run them on PostgreSQL 18 instead
#   test/slt.sh --rewrite FILE...   fill FILE's results from PostgreSQL 18
#
# PostgreSQL 18 is the one installed in $PG18, ~/pg18 if that is not
# set: its results are the ones digres must give. On digres, "skipif" and
# "onlyif" match both "digres" and "postgresql"; on PostgreSQL 18,
# "postgresql" alone. The server runs on a data directory its initdb
# makes in build/test/slt, at port DG_TEST_PORT, or if that is not set
# one the kernel finds free, as run.sh's does. DIGRES names the server to
# run, build/digres if it is not set, and INITDB its initdb, build/initdb.
cd "$(dirname "$0")/.." || exit 1
# Where the server, its log and what failed are kept: SLT_WORK, or
# build/test/slt.
work=${SLT_WORK:-$PWD/build/test/slt}
port=${DG_TEST_PORT:-$(python3 -c 'import socket; s = socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')}
digres=${DIGRES:-build/digres}
initdb=${INITDB:-build/initdb}
pg18=${PG18:-$HOME/pg18}
target=digres
args=()
case $1 in
  --pg18) target=pg18; shift ;;
  --rewrite) target=pg18; args=(--rewrite); shift
    [ $# -gt 0 ] || { echo "slt: --rewrite needs the files to fill"; exit 2; } ;;
esac
files=("$@")
[ ${#files[@]} -gt 0 ] || mapfile -t files < <(find test/slt -name '*.test' | sort)
if [ $target = pg18 ] && [ ! -x "$pg18/bin/postgres" ]; then
  echo "slt: FAILED (no PostgreSQL 18 in $pg18; set PG18)"
  exit 2
fi
rm -rf "$work"
mkdir -p "$work"

# Start the server on a data directory its initdb makes, and wait until
# it says it is ready, ten seconds at most.
if [ $target = pg18 ]; then
  args+=(--engine postgresql)
  "$pg18/bin/initdb" -D "$work/data" -A trust > "$work/initdb.out" 2>&1
  server_cmd=("$pg18/bin/postgres" -c listen_addresses=)
else
  args+=(--engine postgresql --engine digres)
  $initdb "$work/data" > "$work/initdb.out" 2>&1
  server_cmd=("$digres")
fi || {
  echo "slt: FAILED (initdb; see $work)"
  exit 2
}
"${server_cmd[@]}" -D "$work/data" -k "$work" -p "$port" > "$work/log" 2>&1 &
server=$!
for _ in $(seq 100); do
  grep -q "ready to accept connections" "$work/log" && break
  sleep 0.1
done
if ! grep -q "ready to accept connections" "$work/log"; then
  echo "slt: FAILED (the server did not start; see $work/log)"
  kill -KILL $server 2> /dev/null
  exit 2
fi

# Run the files; what failed is in build/test/slt/failures, and how long
# each record took, the slowest first, in build/test/slt/times.
build/slt "${args[@]}" --times "$work/times" \
  "host=$work port=$port dbname=postgres" "${files[@]}" \
  > "$work/tally" 2> "$work/failures"
status=$?

# Stop the server, promptly, and confirm its log tells of no crash.
kill -INT $server
for _ in $(seq 100); do
  kill -0 $server 2> /dev/null || break
  sleep 0.1
done
if kill -0 $server 2> /dev/null; then
  echo "slt: FAILED (the server did not stop; see $work/log)"
  kill -KILL $server
  status=2
fi
wait $server || status=2
if grep -qE "terminated by signal|ThreadSanitizer|TRAP:" "$work/log"; then
  echo "slt: FAILED (a crash or a report in $work/log)"
  status=2
fi

read -r run failed skipped secs < <(awk '{ r += $2; f += $4; s += $6;
  t += $9 } END { printf "%d %d %d %.1f\n", r, f, s, t }' "$work/tally")
echo "slt: $secs s on $target; the slowest record:"
head -1 "$work/times" | cut -c1-120
if [ $status = 0 ]; then
  echo "slt: PASSED (${#files[@]} files on $target, $run records, $skipped skipped)"
else
  grep -v ' 0 failed' "$work/tally"
  echo "slt: FAILED ($failed of $run records on $target; see $work/failures)"
fi
exit $status
