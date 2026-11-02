#!/bin/bash
# run.sh -- time q1 and q2 on digres and PostgreSQL, with regular
# indexes and then btree ones, and print a table of the results.
#
#   DG_PORT=5432 PG_PORT=5433 bench/run.sh
#
# Both servers must be running on this machine: digres, a release
# build, and PostgreSQL 18, each with its socket in PGHOST (/tmp by
# default). On each, load.sql makes the database bench again, then
# mkregidx.sql, q1, q2, mkbtidx.sql, q1, q2. Each query is pgbench,
# prepared, one client, for Q_SECONDS (10). PGBIN is PostgreSQL 18's
# bin, for psql and pgbench (~/p/postgres18/install/bin). The logs go
# to build/run.
set -euo pipefail
cd "$(dirname "$0")"

: "${DG_PORT:?set DG_PORT, digres's port}"
: "${PG_PORT:?set PG_PORT, PostgreSQL's port}"
export PGHOST=${PGHOST:-/tmp}
pgbin=${PGBIN:-$HOME/p/postgres18/install/bin}
seconds=${Q_SECONDS:-10}
work=build/run
mkdir -p "$work"

# psql on port $1, stopping at the first error.
sql() {
  local port=$1
  shift
  "$pgbin/psql" -X -q -v ON_ERROR_STOP=1 -p "$port" "$@"
}

# Refuse a debug build of system $1 on port $2: numbers from one
# measure its assertions. digres's binary says how it was built.
check() {
  local sys=$1 port=$2
  if [ "$(sql "$port" -d postgres -Atc 'show debug_assertions')" != off ]; then
    echo "run: the $sys server on port $port has assertions on" >&2
    exit 1
  fi
  [ "$sys" = dg ] || return 0
  local dir pid build
  dir=$(sql "$port" -d postgres -Atc 'show data_directory')
  pid=$(head -1 "$dir/postmaster.pid")
  build=$("/proc/$pid/exe" --build)
  if [ "$(grep -vc NDEBUG <<< "$build")" != 0 ] ||
    ! grep -q '^RocksDB .*, static,' <<< "$build"; then
    echo "run: digres on port $port is not a release build:" >&2
    echo "$build" >&2
    exit 1
  fi
}

# Run query $3 (q1 or q2) on system $1, port $2, with index kind $4,
# and append "SYS KIND QUERY TPS MS" to $work/results.
measure() {
  local sys=$1 port=$2 q=$3 kind=$4 log=$work/$1-$4-$3.log
  echo "run: $sys: $q, $kind index"
  "$pgbin/pgbench" -p "$port" -d bench -n -M prepared -c 1 -T "$seconds" \
    -f "$q.sql" > "$log" 2>&1 || {
    echo "run: pgbench failed; see $log" >&2
    exit 1
  }
  local tps ms
  tps=$(sed -n 's/^tps = \([0-9.]*\) .*/\1/p' "$log")
  ms=$(sed -n 's/^latency average = \([0-9.]*\) ms/\1/p' "$log")
  echo "$sys $kind $q $tps $ms" >> "$work/results"
}

: > "$work/results"
for sys in dg pg; do
  if [ $sys = dg ]; then port=$DG_PORT; else port=$PG_PORT; fi
  check $sys "$port"
  echo "run: $sys: loading"
  sql "$port" -d postgres -f load.sql > "$work/$sys-load.log" 2>&1 || {
    echo "run: load.sql failed; see $work/$sys-load.log" >&2
    exit 1
  }
  for kind in regular btree; do
    if [ $kind = regular ]; then mk=mkregidx.sql; else mk=mkbtidx.sql; fi
    sql "$port" -d bench -f $mk > "$work/$sys-$kind.log" 2>&1 || {
      echo "run: $mk failed; see $work/$sys-$kind.log" >&2
      exit 1
    }
    measure $sys "$port" q1 $kind
    measure $sys "$port" q2 $kind
  done
done

# One row a query and index kind: each system's tps and latency, and
# digres's latency over PostgreSQL's.
echo
echo "| query | index | dg tps | dg ms | pg tps | pg ms | dg/pg |"
echo "| --- | --- | --: | --: | --: | --: | --: |"
awk '
  { tps[$1, $2, $3] = $4; ms[$1, $2, $3] = $5 }
  END {
    split("q1 q2", qs, " ")
    split("regular btree", ks, " ")
    for (i = 1; i <= 2; i++)
      for (j = 1; j <= 2; j++) {
        q = qs[i]; k = ks[j]
        printf "| %s | %s | %.0f | %.3f | %.0f | %.3f | %.2f |\n", q, k,
          tps["dg", k, q], ms["dg", k, q], tps["pg", k, q], ms["pg", k, q],
          ms["dg", k, q] / ms["pg", k, q]
      }
  }' "$work/results"
