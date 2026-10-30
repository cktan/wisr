#!/bin/bash
# btree.sh -- a secondary index as a tree against today's entries
# (../sm/BTREE.md, ../dg/WORKNOTE.md steps 347 to 353): wide_mixed's
# million rows loaded with its c2 index of one kind, then Q9 and Q10
# counted in user-mode instructions a statement of the whole server,
# and Q11 and a write that moves c2's key timed under 16 clients.
#
#   bench/btree.sh dg plain    digres, today's entries
#   bench/btree.sh dg tree     digres, CREATE INDEX ... WITH (tree = on)
#   bench/btree.sh pg plain    PostgreSQL 18
#
# What it prints, a line each:
#
#   load_s                the load's seconds, the index filled with it
#   q9, q10               instructions a statement, prepared: Q9 a
#                         range of 100 c2 values, Q10 100 Zipfian
#                         values in an IN list, N (300) of each after
#                         WARM (60), as perf stat counts the server's
#                         threads, unpinned
#   q11, q12              pgbench tps, 16 clients, CONC_S (20) s:
#                         Q10 9 in 10 with Q11's update, or with
#                         sql/q12.sql's, which moves a row's c2
#
# PROFILE=1 records Q9's and Q10's instructions with perf record into
# the work directory instead of counting them. Builds as CODE.md says:
# ../dg/build/digres-release and build/pg18. PERF names perf, the one
# for the running kernel when perf alone does not run (WSL).
cd "$(dirname "$0")" || exit 1
set -u
sys=${1:?dg or pg} kind=${2:?plain or tree}
here=$PWD
work=$here/build/btree/$sys-$kind
digres=$here/../dg/build/digres-release
pg18=$here/build/pg18
psql=${PSQL:-psql}
pgbench=${PGBENCH:-pgbench}
perf=${PERF:-perf}
n=${N:-300}
warm=${WARM:-60}
conc_s=${CONC_S:-20}
port=$(python3 -c 'import socket; s = socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
# The socket's directory: a socket path is at most 107 bytes.
sock=$(mktemp -d /tmp/btree.XXXXXX)
rm -rf "$work"
mkdir -p "$work"

# The server, on a data directory its initdb makes; ready in a minute.
if [ "$sys" = pg ]; then
  "$pg18/bin/initdb" -D "$work/data" -A trust --no-sync > "$work/initdb" 2>&1
  printf "shared_buffers = 2GB\njit = off\nmax_parallel_workers_per_gather = 0\nautovacuum = off\nlisten_addresses = ''\n" \
    >> "$work/data/postgresql.conf"
  "$pg18/bin/postgres" -D "$work/data" -k "$sock" -p "$port" \
    > "$work/log" 2>&1 &
else
  "$digres" --init "$work/data" > "$work/initdb" 2>&1
  printf "shared_buffers = 2GB\nlisten_addresses = ''\n" \
    >> "$work/data/postgresql.conf"
  "$digres" -D "$work/data" -k "$sock" -p "$port" > "$work/log" 2>&1 &
fi
server=$!
for _ in $(seq 600); do
  grep -q "ready to accept" "$work/log" && break
  sleep 0.1
done
sql() {
  "$psql" -h "$sock" -p "$port" -d postgres -X -q -A -t -v ON_ERROR_STOP=1 \
    "$@"
}

# wide_mixed and its c2 index, of the kind asked for, then its rows.
with=""
[ "$kind" = tree ] && with="with (tree = on)"
sed -n '/^create table wide_mixed/,/^);/p' sql/schema.sql > "$work/schema.sql"
echo "create index wide_mixed_c2 on wide_mixed (c2) $with;" >> "$work/schema.sql"
sql -f "$work/schema.sql" || exit 1
awk '/^insert into wide_mixed/,/;$/' sql/load.sql > "$work/load.sql"
t0=$(date +%s.%N)
sql -f "$work/load.sql" || exit 1
t1=$(date +%s.%N)
echo "$sys $kind load_s $(echo "$t1 - $t0" | bc)"
sql -c "vacuum wide_mixed" -c "analyze wide_mixed" || exit 1
sleep 5

# Q9's and Q10's executions, warm-up first: btree.py writes them.
(cd "$work" && python3 "$here/btree.py" $((n + warm)))

# Count, or record, the server's instructions over q's last n
# executions, through one session made first, its warm-up before: a
# thread of digres, or a process of PostgreSQL's.
measure() {
  local q=$1 prepare=$2 fifo=$work/fifo
  rm -f "$fifo"
  mkfifo "$fifo"
  "$psql" -h "$sock" -p "$port" -d postgres -X -q -A -t < "$fifo" \
    > "$work/out.$q" 2>&1 &
  local client=$!
  exec 3> "$fifo"
  echo "set enable_seqscan = off; set enable_bitmapscan = off;" >&3
  echo "$prepare" >&3
  echo "select 'pid', pg_backend_pid();" >&3
  head -n "$warm" "$work/$q.sql" >&3
  echo "select 'warm';" >&3
  for _ in $(seq 600); do
    grep -q "^warm" "$work/out.$q" && break
    sleep 0.1
  done
  local pid=$server
  [ "$sys" = pg ] && pid=$(grep '^pid|' "$work/out.$q" | cut -d'|' -f2)
  if [ -n "${PROFILE:-}" ]; then
    "$perf" record -e instructions:u -c 20000 -o "$work/perf.data.$q" \
      -p "$pid" 2> /dev/null &
  else
    "$perf" stat -e instructions:u -x, -o "$work/perf.$q" -p "$pid" &
  fi
  local counter=$!
  sleep 0.5
  tail -n "$n" "$work/$q.sql" >&3
  echo "select 'done';" >&3
  for _ in $(seq 6000); do
    grep -q "^done" "$work/out.$q" && break
    sleep 0.1
  done
  kill -INT $counter
  wait $counter
  exec 3>&-
  wait $client
  grep -i error "$work/out.$q" | head -3
  [ -n "${PROFILE:-}" ] && return
  local ins
  ins=$(grep instructions "$work/perf.$q" | cut -d, -f1)
  echo "$sys $kind $q instructions_per_stmt $((ins / n))"
}
measure q9 "prepare q9(int8) as select sum(c1) from wide_mixed where c2 between \$1 and \$1 + 99;"
measure q10 "prepare q10(int8[]) as select c1 from wide_mixed where c2 = any(\$1);"

# Q10's reads with a write one in ten, 16 clients.
for mix in q11 q12; do
  out=$("$pgbench" -h "$sock" -p "$port" -n -M prepared -T "$conc_s" -c 16 \
    -j 4 -f sql/q10.sql@9 -f "sql/$mix.sql@1" postgres 2>&1 |
    grep -E "^tps|rror" | head -3)
  echo "$sys $kind $mix 16 clients: $out"
done

kill -INT $server
wait $server
rm -rf "$sock"
