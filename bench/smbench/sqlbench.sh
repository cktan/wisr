#!/bin/bash
# sqlbench.sh -- smbench's rows through SQL, in digres and in
# PostgreSQL: t (key int8 primary key, x int8, pad text) of ROWS rows,
# x = 3 key and PAD bytes of pad, loaded and vacuumed. Then, each on a
# warm session:
#
#   count, sum   select count(*) / sum(x) from t, a sequential scan:
#                ns a row
#   exec         select sum(x) from t where key = k for a random k,
#                the server's Execution Time of it, the mean of 200
#                keys on one session: ns a statement
#   statement    the same as pgbench's prepared statements send it,
#                k drawn by \set random(1, rows) before each: ns a
#                statement, the client's round trip included
#
# The key is drawn before the statement: random() in the WHERE
# clause is volatile, evaluated once a row, and no index serves it.
#
#   sqlbench.sh [rows] [pad] [churn]
#
# churn, as "20,10,5", is smbench's: a session holds a REPEATABLE
# READ snapshot while three UPDATEs give those shares of rows one,
# two and three older versions, picked by the same hash of key; then
# VACUUM, which keeps what the snapshot needs, and the timings, at new
# snapshots. Data directories and logs go under $WORK (default: a new
# temporary directory); the servers are bench/build/pg18 and
# digres-release.
set -u
cd "$(dirname "$0")" || exit 1
rows=${1:-1000000}
pad=${2:-60}
churn=${3:-0,0,0}
work=${WORK:-$(mktemp -d)}
# The servers' sockets, in a short directory of their own: a socket's
# path may hold no more than 107 bytes, and its directory no comma.
sock=$(mktemp -d /tmp/sqlb.XXXXXX)
trap 'rm -rf "$sock"' EXIT
pg18=../build/pg18
digres=../../dg/build/digres-release
settings="shared_buffers = 2GB
listen_addresses = ''"

# Start system $1 on port $2 over a new data directory; wait for it.
start() {
  local sys=$1 port=$2 data=$work/$1
  rm -rf "$data"
  if [ "$sys" = pg ]; then
    "$pg18/bin/initdb" -D "$data" -A trust --no-sync > "$work/$sys.initdb" 2>&1
    printf '%s\njit = off\nmax_parallel_workers_per_gather = 0\nautovacuum = off\n' \
      "$settings" >> "$data/postgresql.conf"
    "$pg18/bin/postgres" -D "$data" -k "$sock" -p "$port" > "$work/$sys.log" 2>&1 &
  else
    "$digres" --init "$data" > "$work/$sys.initdb" 2>&1
    echo "$settings" >> "$data/postgresql.conf"
    "$digres" -D "$data" -k "$sock" -p "$port" > "$work/$sys.log" 2>&1 &
  fi
  server=$!
  for _ in $(seq 600); do
    grep -q "ready to accept connections" "$work/$sys.log" && return
    sleep 0.1
  done
  echo "sqlbench: $sys did not start; see $work/$sys.log"
  exit 1
}

# SQL from the arguments, quietly, rows unaligned.
sql() {
  psql -h "$sock" -p "$port" -d postgres -X -q -A -t -v ON_ERROR_STOP=1 "$@"
}

# The median of the numbers on stdin.
median() {
  sort -g | awk '{a[NR] = $1} END {print a[int((NR + 1) / 2)]}'
}

# The median Execution Time, in ms, of EXPLAIN ANALYZE of $2 after the
# settings $1, seven runs on one session after two to warm it.
exec_ms() {
  local q="$1 explain (analyze, timing off) $2;"
  local script="" i
  for i in $(seq 9); do
    script="$script $q"
  done
  sql -c "$script" | sed -n 's/.*Execution Time: \([0-9.]*\) ms.*/\1/p' |
    tail -7 | median
}

# Load t, churn it if asked, then print one system's line.
measure() {
  local sys=$1 c1 c2 c3 holder=""
  sql -c "create table t (key int8 primary key, x int8, pad text)" || exit 1
  sql -c "insert into t select g, 3 * g, repeat('p', $pad)
          from generate_series(1, $rows) g" || exit 1
  sql -c "vacuum (freeze, analyze) t" || exit 1
  # The held snapshot, then the churn: pass n updates every row with
  # at least n older versions to come.
  IFS=, read -r c1 c2 c3 <<< "$churn"
  if [ $((c1 + c2 + c3)) -gt 0 ]; then
    psql -h "$sock" -p "$port" -d postgres -X -q -A -t \
      -c "begin isolation level repeatable read" \
      -c "select count(*) from t" -c "select pg_sleep(100000)" > /dev/null 2>&1 &
    holder=$!
    sleep 2
    local b="((key * 2654435761) % 100)"
    sql -c "update t set pad = repeat('u', $pad) where $b < $((c1 + c2 + c3))" || exit 1
    sql -c "update t set pad = repeat('u', $pad) where $b < $((c2 + c3))" || exit 1
    sql -c "update t set pad = repeat('u', $pad) where $b < $c3" || exit 1
    sql -c "vacuum t" || exit 1
  fi
  local seq="set enable_indexscan = off; set enable_indexonlyscan = off;
             set enable_bitmapscan = off;"
  local count sum point stmt i script=""
  count=$(exec_ms "$seq" "select count(*) from t")
  sum=$(exec_ms "$seq" "select sum(x) from t")
  # 200 random keys on one warm session, after 20 to warm it.
  for i in $(seq 220); do
    script="$script explain (analyze, timing off)
            select sum(x) from t where key = $(((RANDOM * 32768 + RANDOM) % rows + 1));"
  done
  point=$(sql -c "$script" |
          sed -n 's/.*Execution Time: \([0-9.]*\) ms.*/\1/p' | tail -200 |
          awk '{s += $1} END {print s / NR}')
  printf '\\set k random(1, %d)\nselect sum(x) from t where key = :k;\n' \
    "$rows" > "$work/point.sql"
  stmt=$(pgbench -h "$sock" -p "$port" -n -M prepared -c 1 -T 15 \
           -f "$work/point.sql" postgres 2> /dev/null |
         sed -n 's/^tps = \([0-9.]*\) .*/\1/p')
  awk -v sys="$sys" -v c="$count" -v s="$sum" -v p="$point" -v t="$stmt" \
      -v rows="$rows" 'BEGIN {
    printf "%-10s %10.1f %10.1f %12.1f %12.1f\n", sys,
      c * 1e6 / rows, s * 1e6 / rows, p * 1e6, 1e9 / t
  }'
  [ -n "$holder" ] && kill "$holder" 2> /dev/null
}

echo "$rows rows, pad $pad bytes, churn $churn; in $work"
printf '%-10s %10s %10s %12s %12s\n' system "count ns" "sum ns" \
  "exec ns" "statement ns"
port=54331
start dg $port
measure digres
kill -INT "$server"; wait "$server" 2> /dev/null
port=54332
start pg $port
measure postgres
kill -INT "$server"; wait "$server" 2> /dev/null
