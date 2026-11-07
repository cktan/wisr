#!/bin/bash
# idxopen.sh -- what a table's secondary indexes cost a statement that
# uses none of them, in instructions, on digres and PostgreSQL 18.
#
#   bench/idxopen.sh
#
# sm's table open builds every index of the table, used or not. Two
# statements, on tables of 0, 1, 4 and 16 secondary indexes:
#
#   t      select c1 ... WHERE false: reads nothing
#   pt     a point read by the primary key
#
# Each table has 100,000 rows and the same columns, so only the index
# count differs. digres has each table twice, with lsm indexes (its
# default) and with btree ones. PostgreSQL has btree alone.
#
# 1. Start each server pinned to a CPU, its data under build/idxopen.
# 2. Make the tables, and analyze them.
# 3. Refuse an assert build, and a pt whose plan is not the primary key.
# 4. Run a round that is not reported: the load's compactions and
#    autovacuum count as the server's instructions while they run.
# 5. Count the server's user instructions for runs of N1 and N2
#    statements, prepared, by perf stat.
# 6. Take the slope of the two: connection and first plans cancel.
# 7. Report the median of ROUNDS (5) slopes, their spread, and what an
#    index adds, from the tables of 0 and 16.
#
# digres is ../dg/build/digres-release ("make -C dg release"). PGBIN is
# PostgreSQL 18's bin (~/p/postgres18/install/bin). SRV_CPU and CLI_CPU
# are the CPUs of the server and of pgbench.
cd "$(dirname "$0")" || exit 1
set -u
root=$(cd .. && pwd)
work=$PWD/build/idxopen
digres=$root/dg/build/digres-release
pgbin=${PGBIN:-$HOME/p/postgres18/install/bin}
n1=${N1:-10000}
n2=${N2:-60000}
rounds=${ROUNDS:-5}
srv_cpu=${SRV_CPU:-2}
cli_cpu=${CLI_CPU:-4}
counts="0 1 4 16"

rm -rf "$work"
mkdir -p "$work"
# A socket path over 107 bytes cannot be bound: a short directory for it.
sock=$(mktemp -d /tmp/idxopen.XXXX)
trap 'rm -rf "$sock"' EXIT

# Write the statements of table $1.
statements() {
  echo "select c1 from $1 where false;" > "$work/t-$1.sql"
  printf '\\set k random(1, 100000)\nselect c1 from %s where id = :k;\n' "$1" \
    > "$work/pt-$1.sql"
}

# Print the SQL that makes table $1 with $2 indexes, of USING clause $3.
table_sql() {
  local name=$1 count=$2 clause=$3 i cols='' vals=''
  for i in $(seq 16); do
    cols+=", c$i int8"
    vals+=", g"
  done
  echo "create table $name (id int8 primary key$cols);"
  echo "insert into $name select g$vals from generate_series(1, 100000) g;"
  for i in $(seq "$count"); do
    echo "create index on $name $clause (c$i);"
  done
  echo "analyze $name;"
}

# psql as the superuser, on port $1, stopping at the first error.
sql() {
  local port=$1
  shift
  "$pgbin/psql" -X -q -v ON_ERROR_STOP=1 -h "$sock" -p "$port" -d postgres "$@"
}

# Wait for the server logging to $1 to accept connections.
wait_ready() {
  for _ in $(seq 600); do
    grep -q 'ready to accept' "$1" && return 0
    sleep 0.1
  done
  return 1
}

# Make the tables on port $1, those named after it. Refuse an assert
# build, and a point read that does not go by the primary key.
prepare() {
  local port=$1 name plan
  shift
  sql "$port" < "$work/load-$port.sql" || return 1
  [ "$(sql "$port" -Atc 'show debug_assertions')" = off ] || return 1
  for name in "$@"; do
    plan=$(sql "$port" -Atc "explain select c1 from $name where id = 1")
    grep -q "using ${name}_pkey" <<< "$plan" || {
      echo "idxopen: not the primary key: $name: $plan" >&2
      return 1
    }
  done
}

# Count system $1, port $2, server pid $3, on the tables after them.
# Round 0 is the warm-up, which the report skips.
measure() {
  local sys=$1 port=$2 pid=$3 round st name n
  shift 3
  for round in $(seq 0 "$rounds"); do
    for st in t pt; do
      for name in "$@"; do
        for n in "$n1" "$n2"; do
          # The sleep lets a session's thread end and fold its count
          # into its parent's before perf reads it.
          perf stat -x, -e instructions:u -p "$pid" \
            -o "$work/perf.out" -- taskset -c "$cli_cpu" sh -c \
            "'$pgbin/pgbench' -h '$sock' -p $port -n -M prepared -c 1 \
             -t $n -f '$work/$st-$name.sql' postgres \
             > '$work/pgbench.out' 2>&1; sleep 0.3"
          grep -q 'number of failed transactions: 0' "$work/pgbench.out" || {
            echo "idxopen: pgbench failed: $sys $st $name" >&2
            cat "$work/pgbench.out" >&2
            return 1
          }
          echo "$sys $round $st $name $n" \
            "$(awk -F, '/instructions/{print $1}' "$work/perf.out")" \
            >> "$work/runs"
        done
      done
    done
  done
}

dg_tables=''
pg_tables=''
: > "$work/load-54485.sql"
: > "$work/load-54486.sql"
for c in $counts; do
  dg_tables+=" l$c b$c"
  pg_tables+=" b$c"
  statements "l$c"
  statements "b$c"
  table_sql "l$c" "$c" '' >> "$work/load-54485.sql"
  table_sql "b$c" "$c" 'using btree' >> "$work/load-54485.sql"
  table_sql "b$c" "$c" 'using btree' >> "$work/load-54486.sql"
done

# digres, end to end.
"$digres" --init "$work/dg-data" > "$work/dg-init.out" 2>&1 || {
  echo "idxopen: digres init failed; see $work/dg-init.out" >&2
  exit 1
}
printf "shared_buffers = 1GB\nlisten_addresses = ''\n" \
  >> "$work/dg-data/postgresql.conf"
# The server under test, pinned so a count is not spread over CPUs.
taskset -c "$srv_cpu" "$digres" -D "$work/dg-data" -k "$sock" -p 54485 \
  > "$work/dg.log" 2>&1 &
server=$!
# shellcheck disable=SC2086
wait_ready "$work/dg.log" && prepare 54485 $dg_tables > "$work/dg-load.out" 2>&1 &&
  measure dg 54485 "$server" $dg_tables
rc=$?
kill "$server"
wait "$server" 2> /dev/null
[ "$rc" = 0 ] || {
  echo "idxopen: digres failed; see $work/dg-load.out" >&2
  exit 1
}

# PostgreSQL 18, the same way.
"$pgbin/initdb" -D "$work/pg-data" -A trust > "$work/pg-init.out" 2>&1 || {
  echo "idxopen: initdb failed; see $work/pg-init.out" >&2
  exit 1
}
printf "shared_buffers = 1GB\nlisten_addresses = ''\n" \
  >> "$work/pg-data/postgresql.conf"
# The reference server, pinned as digres was.
taskset -c "$srv_cpu" "$pgbin/postgres" -D "$work/pg-data" -k "$sock" \
  -p 54486 > "$work/pg.log" 2>&1 &
server=$!
# shellcheck disable=SC2086
wait_ready "$work/pg.log" && prepare 54486 $pg_tables > "$work/pg-load.out" 2>&1 &&
  measure pg 54486 "$server" $pg_tables
rc=$?
kill "$server"
wait "$server" 2> /dev/null
[ "$rc" = 0 ] || {
  echo "idxopen: PostgreSQL failed; see $work/pg-load.out" >&2
  exit 1
}

# The report: instructions a statement, and what an index adds.
python3 - "$work/runs" << 'PY'
import statistics, sys
raw = {}
for line in open(sys.argv[1]):
    f = line.split()
    if len(f) == 6 and f[5].isdigit() and f[1] != '0':
        raw.setdefault((f[0], f[1], f[2], f[3]), {})[int(f[4])] = int(f[5])
slopes = {}
for (system, _, st, name), v in raw.items():
    (a, ia), (b, ib) = sorted(v.items())
    slopes.setdefault((system, st, name), []).append((ib - ia) / (b - a))
med = {k: statistics.median(v) for k, v in slopes.items()}
print('%-3s %-5s %-5s %9s %9s %9s %9s %8s %8s' % (
    'sys', 'stmt', 'kind', '0', '1', '4', '16', 'an index', 'spread%'))
for system, kind in (('dg', 'l'), ('dg', 'b'), ('pg', 'b')):
    for st in ('t', 'pt'):
        keys = [(system, st, '%s%d' % (kind, c)) for c in (0, 1, 4, 16)]
        v = [med[k] for k in keys]
        spread = max((max(slopes[k]) - min(slopes[k])) / med[k] * 100
                     for k in keys)
        print('%-3s %-5s %-5s %9.0f %9.0f %9.0f %9.0f %8.0f %7.2f%%' % (
            system, st, {'l': 'lsm', 'b': 'btree'}[kind], *v,
            (v[3] - v[0]) / 16, spread))
PY
