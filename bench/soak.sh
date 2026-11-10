#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# soak.sh -- run the school workload of SOAK.md against one server
# for hours, sample its memory, and check the data at the end.
#
#   bench/soak.sh dg|pg
#
# dg is digres, a release build (DIGRES, default
# ../dg/build/digres-release); pg is PostgreSQL 18 (PGBIN, default
# ~/p/postgres18/install/bin). Environment:
#
#   DURATION  seconds of the run, warm-up included (14400)
#   WARMUP    seconds at the start that the report does not score (900)
#   SRV_CPUS  CPUs of the server (0-1)
#   CLI_CPUS  CPUs of the clients (2-3)
#
# The data, the logs and the samples go to build/soak/SYS, on a real
# disk; soak/report.py reads them. The exit status is 1 when a check
# at the end fails.
#
# 1. Start the server pinned to its CPUs, with data under build/.
# 2. Load soak/schema.sql, the school, and count its rows.
# 3. Start the sampler: a row a minute of the server's memory,
#    threads and files, and of the data directory's size.
# 4. Start four pgbench processes, two roles by two ways to connect:
#    "reconnecting" connects for every transaction (-C), "persistent"
#    keeps one connection. Each has the transactions of its role.
# 5. Wait for them to end, and stop the sampler.
# 6. Check the data: capacity, orphan rows, table against indexes.
# 7. Stop the server, start it again, and check the data once more.
set -uo pipefail
cd "$(dirname "$0")"

sys=${1:?usage: soak.sh dg|pg}
duration=${DURATION:-14400}
srv_cpus=${SRV_CPUS:-0-1}
cli_cpus=${CLI_CPUS:-2-3}
digres=${DIGRES:-$PWD/../dg/build/digres-release}
pgbin=${PGBIN:-$HOME/p/postgres18/install/bin}
me=$(id -un)
work=$PWD/build/soak/$sys
port=$([ "$sys" = dg ] && echo 5541 || echo 5542)

rm -rf "$work"
mkdir -p "$work" "$PWD/build/tmp"
export TMPDIR=$PWD/build/tmp
# A socket path over 107 bytes cannot be bound: a short directory.
sock=$(mktemp -d /tmp/soak.XXXX)
export PGHOST=$sock

# psql on the server, as the superuser, stopping at the first error.
sql() {
  "$pgbin/psql" -X -q -v ON_ERROR_STOP=1 -p "$port" "$@"
}

# Wait until the server logging to $1 accepts connections.
wait_ready() {
  for _ in $(seq 600); do
    grep -q 'ready to accept' "$1" && return 0
    sleep 0.1
  done
  return 1
}

# Start the server on the data directory, pinned, and wait for it.
start_server() {
  nstart=$((nstart + 1))
  log=$work/server-$nstart.log
  if [ "$sys" = dg ]; then
    taskset -c "$srv_cpus" "$digres" -D "$work/data" -k "$sock" \
      -p "$port" > "$log" 2>&1 &
  else
    taskset -c "$srv_cpus" "$pgbin/postgres" -D "$work/data" \
      -k "$sock" -p "$port" > "$log" 2>&1 &
  fi
  server=$!
  wait_ready "$log"
}

# Stop the server with SIGINT, as the plan says, and wait for it.
stop_server() {
  kill -INT "$server" 2> /dev/null
  wait "$server" 2> /dev/null
}

# Print the pids of the server and its children (PostgreSQL's backends).
server_pids() {
  echo "$server"
  pgrep -P "$server" || true
}

# Append a row to $work/samples.csv a minute until the file stop appears.
# PSS, not RSS, is the memory: PostgreSQL's backends share pages.
sampler() {
  echo "t,pss_kb,rss_kb,threads,fds,procs,data_kb,files,alive" \
    > "$work/samples.csv"
  local t0=$SECONDS p pss rss thr fds procs data files alive
  while [ ! -e "$work/stop" ]; do
    pss=0 rss=0 thr=0 fds=0 procs=0 alive=1
    kill -0 "$server" 2> /dev/null || alive=0
    for p in $(server_pids); do
      [ -r "/proc/$p/smaps_rollup" ] || continue
      pss=$((pss + $(awk '/^Pss:/{print $2}' "/proc/$p/smaps_rollup")))
      rss=$((rss + $(awk '/^Rss:/{print $2}' "/proc/$p/smaps_rollup")))
      thr=$((thr + $(awk '/^Threads:/{print $2}' "/proc/$p/status")))
      fds=$((fds + $(ls "/proc/$p/fd" | wc -l)))
      procs=$((procs + 1))
    done
    data=$(du -sk "$work/data" | cut -f1)
    files=$(find "$work/data" -type f | wc -l)
    echo "$((SECONDS - t0)),$pss,$rss,$thr,$fds,$procs,$data,$files,$alive" \
      >> "$work/samples.csv"
    sleep 60
  done
}

# Run pgbench for role $2 with the scripts in $3 ("name@weight ...");
# $1 names the group, $4 is the client count, $5 is -C or empty.
client() {
  local group=$1 role=$2 scripts=$3 n=$4 reconnect=$5 args=() s
  for s in $scripts; do args+=(-f "soak/${s%@*}.sql@${s#*@}"); done
  # Log 2% of the transactions, for the percentiles of the report.
  PGPASSWORD=p taskset -c "$cli_cpus" "$pgbin/pgbench" -h "$sock" \
    -p "$port" -U "$role" -d soak -n -M prepared -c "$n" -j 1 \
    -T "$duration" --progress=60 -r --max-tries=10 $reconnect \
    -l --sampling-rate=0.02 --log-prefix="$work/tx-$group" \
    "${args[@]}" > "$work/pgbench-$group.log" 2>&1
}

# Print the counts that the checks compare: rows by a table scan, by
# the primary key and by the secondary index.
counts() {
  sql -d soak -At << 'EOF'
set enable_indexscan = off;
set enable_indexonlyscan = off;
set enable_bitmapscan = off;
select 'scan', count(*) from enrollment;
reset enable_indexscan;
reset enable_indexonlyscan;
reset enable_bitmapscan;
set enable_seqscan = off;
select 'pkey', count(*) from enrollment where student_id >= 0;
select 'class_idx', count(*) from enrollment where class_id >= 0;
EOF
}

# Print "check: NAME ok" or "check: NAME FAIL (detail)" for the data.
check_data() {
  local label=$1 over orphans c
  over=$(sql -d soak -Atc "select count(*) from class c where
    (select count(*) from enrollment e where e.class_id = c.id)
    > c.capacity")
  [ "$over" = 0 ] && echo "check: $label capacity ok" ||
    echo "check: $label capacity FAIL ($over classes over capacity)"
  orphans=$(sql -d soak -Atc "select count(*) from enrollment e where
    not exists (select 1 from student s where s.id = e.student_id)
    or not exists (select 1 from class c where c.id = e.class_id)")
  [ "$orphans" = 0 ] && echo "check: $label orphans ok" ||
    echo "check: $label orphans FAIL ($orphans rows)"
  c=$(counts | cut -d'|' -f2 | sort -u | wc -l)
  [ "$c" = 1 ] && echo "check: $label indexes ok ($(counts | head -1))" ||
    echo "check: $label indexes FAIL ($(counts | tr '\n' ' '))"
}

# 1. Start the server pinned to its CPUs, with data under build/.
if [ "$sys" = dg ]; then
  "$digres" --init "$work/data" > "$work/init.out" 2>&1 || exit 1
else
  "$pgbin/initdb" -D "$work/data" -A trust > "$work/init.out" 2>&1 ||
    exit 1
  # Passwords for the roles, as digres asks: the superuser stays trust.
  printf 'local all %s trust\nlocal all all scram-sha-256\n' "$me" \
    > "$work/data/pg_hba.conf"
  printf 'jit = off\nmax_parallel_workers_per_gather = 0\n' \
    >> "$work/data/postgresql.conf"
fi
printf "shared_buffers = 2GB\nlisten_addresses = ''\n" \
  >> "$work/data/postgresql.conf"
nstart=0
start_server || { echo "soak: server did not start" >&2; exit 1; }
trap 'touch "$work/stop"; kill -INT "$server" 2> /dev/null; rm -rf "$sock"' EXIT

# 2. Load soak/schema.sql, the school, and count its rows.
sql -d postgres -f soak/schema.sql > "$work/load.out" 2>&1 || {
  echo "soak: schema failed; see $work/load.out" >&2
  exit 1
}
if [ "$(sql -d postgres -Atc 'show debug_assertions')" != off ]; then
  echo "soak: the server has assertions on" >&2
  exit 1
fi
{ echo "start:"; counts; check_data start; } > "$work/checks.out"

# 3. Start the sampler.
sampler &
sampler_pid=$!

# 4. Start four pgbench processes. Student reads are 55% of a
#    student's mix, so the overall mix is near 80 read, 10 update,
#    5 delete and 5 insert: report.py prints the real one.
student="student_sched@40 student_grades@15 add_class@5 drop_class@5"
prof="prof_sched@12 roster@13 set_grade@10"
echo "soak: $sys: $duration seconds from $(date)"
clients=()
client rs student_app "$student" 3 -C &
clients+=($!)
client rp prof_app "$prof" 2 -C &
clients+=($!)
client ps student_app "$student" 3 "" &
clients+=($!)
client pp prof_app "$prof" 1 "" &
clients+=($!)

# 5. Wait for them to end, and stop the sampler.
wait "${clients[@]}"
touch "$work/stop"
wait "$sampler_pid" 2> /dev/null
echo "soak: clients ended $(date)"

# 6. Check the data.
{ echo "end:"; counts; check_data end; } >> "$work/checks.out"

# 7. Stop the server, start it again, and check the data once more.
stop_server
start_server || echo "check: restart FAIL (no start)" >> "$work/checks.out"
{ echo "restart:"; counts; check_data restart; } >> "$work/checks.out"
echo "errors in server log: $(cat "$work"/server-*.log |
  grep -cE 'ERROR|FATAL|PANIC')" >> "$work/checks.out"
stop_server
cat "$work/checks.out"
! grep -q FAIL "$work/checks.out"
