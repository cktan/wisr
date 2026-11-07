#!/bin/bash
# sortload.sh -- a COPY into an empty table with indexes, on digres and
# PostgreSQL 18: what a load's sort costs, in instructions and time.
#
#   bench/sortload.sh
#
# The table has a primary key and two secondary indexes, so a digres
# load sorts three streams. ROWS (5,000,000) rows in no key order pass
# the sort budget, 256 MB by default, so the sort spills and merges.
#
# 1. Make a CSV of ROWS rows under build/sortload.
# 2. Start each server pinned to a CPU, and make the table.
# 3. Count the server's user instructions, and the time, of one COPY,
#    by perf stat, ROUNDS (3) times on a table made anew.
# 4. Report the median of each.
#
# Instructions include the server's background threads, so compactions
# after an ingest count. DIGRES is the binary, by default
# ../dg/build/digres-release ("make -C dg release"). PGBIN is PostgreSQL
# 18's bin (~/p/postgres18/install/bin). SRV_CPU and CLI_CPU are the
# CPUs of the server and of psql.
cd "$(dirname "$0")" || exit 1
set -u
root=$(cd .. && pwd)
work=$PWD/build/sortload
digres=${DIGRES:-$root/dg/build/digres-release}
pgbin=${PGBIN:-$HOME/p/postgres18/install/bin}
rows=${ROWS:-5000000}
rounds=${ROUNDS:-3}
srv_cpu=${SRV_CPU:-2}
cli_cpu=${CLI_CPU:-4}

mkdir -p "$work"
rm -rf "$work/dg-data" "$work/pg-data" "$work/runs"
# A socket path over 107 bytes cannot be bound: a short directory for it.
sock=$(mktemp -d /tmp/sortload.XXXX)
trap 'rm -rf "$sock"' EXIT

# The rows: a key, an email and a number, each in no order.
[ -f "$work/rows-$rows.csv" ] || python3 - "$rows" "$work/rows-$rows.csv" << 'PY'
import random, sys
n = int(sys.argv[1])
ids = list(range(n))
random.seed(1)
random.shuffle(ids)
with open(sys.argv[2], 'w') as f:
    for i in ids:
        f.write('%d,user%d@example.com,%d\n' % (i, (i * 2654435761) % n, i ^ 0x5555))
PY

# Wait for the server logging to $1 to accept connections.
wait_ready() {
  for _ in $(seq 600); do
    grep -q 'ready to accept' "$1" && return 0
    sleep 0.1
  done
  return 1
}

# psql on port $1, stopping at the first error.
sql() {
  local port=$1
  shift
  "$pgbin/psql" -X -q -v ON_ERROR_STOP=1 -h "$sock" -p "$port" -d postgres "$@"
}

# Run the COPY ROUNDS times on system $1, port $2, server pid $3.
measure() {
  local sys=$1 port=$2 pid=$3 round start end
  for round in $(seq "$rounds"); do
    sql "$port" << 'SQL' || return 1
drop table if exists t;
create table t (id int8 primary key, email text, n int8);
create index t_email on t (email);
create index t_n on t (n);
SQL
    start=$(date +%s%N)
    perf stat -x, -e instructions:u -p "$pid" -o "$work/perf.out" -- \
      taskset -c "$cli_cpu" "$pgbin/psql" -X -q -v ON_ERROR_STOP=1 \
      -h "$sock" -p "$port" -d postgres \
      -c "\\copy t from '$work/rows-$rows.csv' csv" || return 1
    end=$(date +%s%N)
    [ "$(sql "$port" -Atc 'select count(*) from t')" = "$rows" ] || {
      echo "sortload: $sys lost rows" >&2
      return 1
    }
    echo "$sys $round $(awk -F, '/instructions/{print $1}' "$work/perf.out")" \
      "$(((end - start) / 1000000))" >> "$work/runs"
  done
}

"$digres" --init "$work/dg-data" > "$work/dg-init.out" 2>&1 || exit 1
printf "shared_buffers = 1GB\nlisten_addresses = ''\n" \
  >> "$work/dg-data/postgresql.conf"
# The server under test, pinned so a count is not spread over CPUs.
taskset -c "$srv_cpu" "$digres" -D "$work/dg-data" -k "$sock" -p 54491 \
  > "$work/dg.log" 2>&1 &
server=$!
wait_ready "$work/dg.log" && measure dg 54491 "$server"
rc=$?
kill "$server"
wait "$server" 2> /dev/null
[ "$rc" = 0 ] || exit 1

"$pgbin/initdb" -D "$work/pg-data" -A trust > "$work/pg-init.out" 2>&1 || exit 1
printf "shared_buffers = 1GB\nlisten_addresses = ''\n" \
  >> "$work/pg-data/postgresql.conf"
# The reference server, pinned as digres was.
taskset -c "$srv_cpu" "$pgbin/postgres" -D "$work/pg-data" -k "$sock" \
  -p 54492 > "$work/pg.log" 2>&1 &
server=$!
wait_ready "$work/pg.log" && measure pg 54492 "$server"
rc=$?
kill "$server"
wait "$server" 2> /dev/null
[ "$rc" = 0 ] || exit 1

python3 - "$work/runs" "$rows" << 'PY'
import statistics, sys
runs = {}
for line in open(sys.argv[1]):
    sysname, _, ins, ms = line.split()
    runs.setdefault(sysname, []).append((int(ins), int(ms)))
print('%-3s %14s %12s %10s' % ('sys', 'instructions', 'a row', 'ms'))
for sysname in ('dg', 'pg'):
    i = statistics.median(r[0] for r in runs[sysname])
    m = statistics.median(r[1] for r in runs[sysname])
    print('%-3s %14d %12.0f %10d' % (sysname, i, i / int(sys.argv[2]), m))
PY
