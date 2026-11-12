#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# run.sh -- write workloads on digres and PostgreSQL 18, with each
# commit synced on both: throughput, latency, and bytes written.
#
#   wbench/run.sh
#
# The table t has id, an identity primary key, and c1 to c16, int8.
# The workloads (WORKLOADS, default all four):
#
#   ins1   insert one row a transaction (ins1.sql), with 0, 1, 4 or
#          16 secondary indexes (COUNTS)
#   ins1k  insert 1000 rows a transaction (ins1k.sql), no secondary
#          index
#   upd    update a column with no index, one row by key (upd.sql),
#          in a table of UPD_ROWS (100,000) rows
#   uniq   ins1.sql with one unique secondary index
#
# digres runs each workload with secondary indexes twice: lsm indexes
# (its default) and btree ones. PostgreSQL has btree alone.
#
# 1. Start each server, its data under build/run. PostgreSQL has fsync
#    and synchronous_commit on; digres syncs its WAL at each commit by
#    default (durability_sync, sm/include/sm_options.h).
# 2. For each workload, index kind and client count (CLIENTS, 1 and
#    16), make t again, and checkpoint.
# 3. Read the sectors written to the data's device.
# 4. Run pgbench, prepared, for W_TXNS (10,000) transactions shared by
#    the clients (ins1k: a twentieth of them, 1000 rows each), with a
#    log of each transaction. A count, not a time, caps the writes.
# 5. Read the sectors again: the difference is the run's bytes, WAL,
#    flushes and compactions included.
# 6. Report tps, latency p50 and p99, and KB written a transaction.
#
# Another process that writes to the device adds to its count. DIGRES
# is the binary, by default ../dg/build/digres-release ("make -C dg
# release"). PGBIN is PostgreSQL 18's bin (~/p/postgres18/install/bin).
cd "$(dirname "$0")" || exit 1
set -u
root=$(cd .. && pwd)
here=$PWD
work=$PWD/build/run
digres=${DIGRES:-$root/dg/build/digres-release}
pgbin=${PGBIN:-$HOME/p/postgres18/install/bin}
txns=${W_TXNS:-10000}
clients=${CLIENTS:-1 16}
counts=${COUNTS:-0 1 4 16}
workloads=${WORKLOADS:-ins1 ins1k upd uniq}
upd_rows=${UPD_ROWS:-100000}

# Refuse a digres with assertions: its numbers measure them.
if "$digres" --build | grep -qv NDEBUG; then
  echo "run: $digres is not a release build:" >&2
  "$digres" --build >&2
  exit 1
fi

rm -rf "$work"
mkdir -p "$work"
# A socket path over 107 bytes cannot be bound: a short directory for it.
sock=$(mktemp -d /tmp/wbench.XXXX)
trap 'rm -rf "$sock"' EXIT

# The device under the data: its sectors written are the bytes of a run.
# The source can be /dev/root, so find the device by its major:minor.
dev=$(mountpoint -d "$(df --output=target "$work" | tail -1)")
stat=/sys/dev/block/$dev/stat
[ -r "$stat" ] || {
  echo "run: no $stat for the device of $work" >&2
  exit 1
}

# Print the sectors written to the device, 512 bytes each.
sectors() {
  awk '{print $7}' "$stat"
}

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

# Print the SQL that makes t for workload $1, index kind $2 (lsm, btree
# or -), with $3 secondary indexes.
setup_sql() {
  local w=$1 kind=$2 n=$3 using="" i
  [ "$kind" = btree ] && using=" using btree"
  echo "drop table if exists t;"
  printf "create table t (id int8 generated always as identity primary key"
  for i in $(seq 16); do printf ", c%d int8" "$i"; done
  echo ");"
  for i in $(seq "$n"); do
    echo "create index t_c$i on t$using (c$i);"
  done
  [ "$w" = uniq ] && echo "create unique index t_u on t$using (c1);"
  if [ "$w" = upd ]; then
    printf "insert into t (c1"
    for i in $(seq 2 16); do printf ", c%d" "$i"; done
    printf ") select g"
    for i in $(seq 2 16); do printf ", g"; done
    echo " from generate_series(1, $upd_rows) g;"
  fi
  echo "analyze t;"
  # Write the setup's dirty pages now, not in the measured run.
  echo "checkpoint;"
}

# Print "workload kind count" for each run of system $1.
runs() {
  local kinds=btree w n k
  [ "$1" = dg ] && kinds="lsm btree"
  for w in $workloads; do
    case $w in
    ins1)
      for n in $counts; do
        if [ "$n" = 0 ]; then
          echo "ins1 - 0"
        else
          for k in $kinds; do echo "ins1 $k $n"; done
        fi
      done
      ;;
    ins1k | upd) echo "$w - 0" ;;
    uniq) for k in $kinds; do echo "uniq $k 0"; done ;;
    *) echo "run: no workload $w" >&2 && return 1 ;;
    esac
  done
}

# Run workload $3, index kind $4, $5 indexes, $6 clients on system
# $1, port $2, and append a line of results to $work/results.
measure() {
  local sys=$1 port=$2 w=$3 kind=$4 n=$5 c=$6
  local name=$sys-$w-$kind-$n-c$c script s0 s1 j t
  script=$here/$w.sql
  [ "$w" = uniq ] && script=$here/ins1.sql
  echo "run: $name"
  setup_sql "$w" "$kind" "$n" | sql "$port" > "$work/$name.setup" 2>&1 || {
    echo "run: setup failed; see $work/$name.setup" >&2
    return 1
  }
  rm -rf "$work/txlog" && mkdir "$work/txlog"
  j=$((c < 4 ? c : 4))
  # ins1k writes 1000 rows a transaction: a twentieth of the count.
  t=$txns
  [ "$w" = ins1k ] && t=$((txns / 20))
  t=$((t / c))
  s0=$(sectors)
  "$pgbin/pgbench" -h "$sock" -p "$port" -d postgres -n -M prepared \
    -c "$c" -j "$j" -t "$t" -D rows="$upd_rows" \
    -l --log-prefix="$work/txlog/tx" -f "$script" > "$work/$name.log" 2>&1 || {
    echo "run: pgbench failed; see $work/$name.log" >&2
    return 1
  }
  s1=$(sectors)
  python3 - "$work/$name.log" "$work/txlog" "$((s1 - s0))" \
    "$sys $w $kind $n $c" >> "$work/results" << 'PY'
import glob, re, sys
log, txdir, sectors, label = sys.argv[1:]
text = open(log).read()
tps = float(re.search(r'^tps = ([0-9.]+)', text, re.M).group(1))
txns = int(re.search(r'actually processed: (\d+)', text).group(1))
# The third field of a line of the transaction log is its latency, us.
lat = sorted(int(line.split()[2])
             for f in glob.glob(txdir + '/tx*') for line in open(f))
p50 = lat[len(lat) // 2] / 1000
p99 = lat[len(lat) * 99 // 100] / 1000
kb = int(sectors) * 512 / 1024 / txns
print(label, '%.0f %.3f %.3f %.1f' % (tps, p50, p99, kb))
PY
}

# Run every run of system $1, port $2.
measure_all() {
  local w kind n c
  runs "$1" > "$work/$1.runs" || return 1
  while read -r w kind n; do
    for c in $clients; do
      measure "$1" "$2" "$w" "$kind" "$n" "$c" || return 1
    done
  done < "$work/$1.runs"
}

: > "$work/results"

"$digres" --init "$work/dg-data" > "$work/dg-init.out" 2>&1 || exit 1
printf "shared_buffers = 1GB\nlisten_addresses = ''\n" \
  >> "$work/dg-data/postgresql.conf"
# The server under test.
"$digres" -D "$work/dg-data" -k "$sock" -p 54493 > "$work/dg.log" 2>&1 &
server=$!
wait_ready "$work/dg.log" && measure_all dg 54493
rc=$?
kill "$server"
wait "$server" 2> /dev/null
[ "$rc" = 0 ] || exit 1

"$pgbin/initdb" -D "$work/pg-data" -A trust > "$work/pg-init.out" 2>&1 || exit 1
printf "shared_buffers = 1GB\nlisten_addresses = ''\n%s\n" \
  "fsync = on
synchronous_commit = on
full_page_writes = on" >> "$work/pg-data/postgresql.conf"
# The reference server.
"$pgbin/postgres" -D "$work/pg-data" -k "$sock" -p 54494 \
  > "$work/pg.log" 2>&1 &
server=$!
wait_ready "$work/pg.log" && {
  # Refuse a PostgreSQL with assertions, as digres above.
  if [ "$(sql 54494 -Atc 'show debug_assertions')" != off ]; then
    echo "run: $pgbin/postgres has assertions on" >&2
    false
  else
    measure_all pg 54494
  fi
}
rc=$?
kill "$server"
wait "$server" 2> /dev/null
[ "$rc" = 0 ] || exit 1

# One row a run, in the order run: digres's, then PostgreSQL's.
echo
echo "| sys | workload | index | count | clients | tps | p50 ms | p99 ms | KB/txn |"
echo "| --- | --- | --- | --: | --: | --: | --: | --: | --: |"
awk '{ printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
       $1, $2, $3, $4, $5, $6, $7, $8, $9 }' "$work/results"
