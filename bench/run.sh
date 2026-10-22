#!/bin/bash
# run.sh -- run the benchmark of DESIGN.md: load PostgreSQL 18 and digres,
# one at a time, time the queries on each, and write a report.
#
#   bench/run.sh             both systems
#   bench/run.sh pg|dg       one, for a look; no report is written
#
# RUNS (10) timed runs of each query after WARM (2), and Q7_SECONDS (30)
# of pgbench. PostgreSQL is bench/build/pg18 (DESIGN.md section 2) and
# digres ../dg/build/digres-release ("make -C dg release"). Each system's
# data directory and log are under bench/build/run; the report goes to
# bench/results.
cd "$(dirname "$0")" || exit 1
set -u
here=$PWD
work=$here/build/run
pg18=$here/build/pg18
digres=$here/../dg/build/digres-release
psql=${PSQL:-psql}
pgbench=${PGBENCH:-pgbench}
runs=${RUNS:-10}
warm=${WARM:-2}
q7_seconds=${Q7_SECONDS:-30}
which=${1:-both}
mkdir -p "$work"

# The server's settings of DESIGN.md section 3, for PostgreSQL, and for
# digres, which has no JIT, parallel query or autovacuum to turn off.
pg_settings="shared_buffers = 2GB
jit = off
max_parallel_workers_per_gather = 0
autovacuum = off
listen_addresses = ''"
dg_settings="shared_buffers = 2GB
listen_addresses = ''"

# Make system $1's data directory and start its server on port $2; wait
# until it is ready, a minute at most.
start() {
  local sys=$1 port=$2 data=$work/$1
  rm -rf "$data"
  if [ "$sys" = pg ]; then
    "$pg18/bin/initdb" -D "$data" -A trust --no-sync > "$work/$sys.initdb" \
      2>&1 || { echo "run: initdb failed; see $work/$sys.initdb"; exit 1; }
    echo "$pg_settings" >> "$data/postgresql.conf"
    "$pg18/bin/postgres" -D "$data" -k "$work" -p "$port" \
      > "$work/$sys.log" 2>&1 &
  else
    "$digres" --init "$data" > "$work/$sys.initdb" 2>&1 ||
      { echo "run: initdb failed; see $work/$sys.initdb"; exit 1; }
    echo "$dg_settings" >> "$data/postgresql.conf"
    "$digres" -D "$data" -k "$work" -p "$port" > "$work/$sys.log" 2>&1 &
  fi
  server=$!
  for _ in $(seq 600); do
    grep -q "ready to accept connections" "$work/$sys.log" && return
    sleep 0.1
  done
  echo "run: the $sys server did not start; see $work/$sys.log"
  exit 1
}

# Stop the server start() started, and wait for it.
stop() {
  kill -INT "$server"
  wait "$server" 2> /dev/null
}

# Run SQL from the arguments on the running server, quietly, rows unaligned.
sql() {
  "$psql" -h "$work" -p "$port" -d postgres -X -q -A -t -v ON_ERROR_STOP=1 "$@"
}

# The median, least and greatest of the numbers on stdin, as "M (L-G)".
summary() {
  sort -g | awk '{ v[NR] = $1 } END {
    m = NR % 2 ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2
    printf "%.1f (%.1f-%.1f)", m, v[1], v[NR] }'
}

# Load system $1, check each query's plan, time it, and write
# $work/$1.results: NAME|TABLE|SUMMARY lines, then sizes and Q7.
measure() {
  local sys=$1 out=$work/$1.results
  port=$2
  # Only optimized builds are measured (CODE.md): every part of digres
  # without assertions, and RocksDB its own static build, as the system's
  # keeps its assert()s.
  if [ "$sys" = dg ] && { [ "$("$digres" --build | grep -vc NDEBUG)" != 0 ] ||
    ! "$digres" --build | grep -q '^RocksDB .*, static,'; }; then
    echo "run: $digres is not a release build; see CODE.md"
    "$digres" --build
    exit 1
  fi
  start "$sys" "$port"
  if [ "$(sql -c 'show debug_assertions')" != off ]; then
    echo "run: the $sys server is a debug build; see CODE.md"
    stop
    exit 1
  fi
  echo "run: $sys: loading"
  sql -f sql/schema.sql || exit 1
  if [ "$sys" = pg ]; then
    sql -f sql/pg_only.sql || exit 1
  fi
  sql -f sql/load.sql || exit 1
  # Each side settled: PostgreSQL's heap, so no scan sets hint bits, and
  # digres's stores, compacted (section 3).
  sql -c 'vacuum (freeze, analyze)' || exit 1
  : > "$out"
  grep -v '^#' sql/queries.txt | while IFS='|' read -r name table scan query; do
    local set a=1 times=()
    # The plan the query is to have: only sequential scans, or only index.
    if [ "$scan" = seq ]; then
      set="set enable_indexscan = off; set enable_indexonlyscan = off;
        set enable_bitmapscan = off;"
    else
      set="set enable_seqscan = off; set enable_bitmapscan = off;"
    fi
    # The plan's scan node, for the report to show it is the one meant.
    local plan
    plan=$(sql -c "$set explain (costs off) ${query//:a/1}" | grep -m1 -o \
      '[A-Z][A-Za-z ]*Scan\( using [a-z_]*\)\? on [a-z_]*')
    for r in $(seq $((warm + runs))); do
      a=$(((r * 90001) % 990000 + 1))
      local t
      t=$(sql -c "$set explain (analyze, timing off) ${query//:a/$a}" |
        sed -n 's/^Execution Time: \([0-9.]*\) ms$/\1/p')
      [ "$r" -gt "$warm" ] && times+=("$t")
    done
    echo "$name|$table|$(printf '%s\n' "${times[@]}" | summary)|$plan" >> "$out"
    echo "run: $sys: $name on $table"
  done
  # Each table's size as its system reports it.
  for t in narrow wide_fixed wide_mixed toasted; do
    echo "size|$t|$(sql -c "select pg_size_pretty(pg_total_relation_size('$t'))" \
      2> /dev/null || echo '?')" >> "$out"
  done
  echo "size|datadir|$(du -sh "$work/$sys" | cut -f1)" >> "$out"
  echo "run: $sys: q7, $q7_seconds seconds"
  echo "q7|wide_mixed|$("$pgbench" -h "$work" -p "$port" -n -c 1 \
    -T "$q7_seconds" -f sql/q7.sql postgres 2> /dev/null |
    sed -n 's/^tps = \([0-9.]*\) .*/\1/p') tps|" >> "$out"
  stop
}

# The value of field 3 for NAME|TABLE in a results file.
field() {
  awk -F'|' -v n="$2" -v t="$3" '$1 == n && $2 == t { print $'"$4"' }' "$1"
}

# Write the report of DESIGN.md section 7 from both systems' results.
report() {
  local stamp file commit cpu
  stamp=$(date +%Y%m%d-%H%M)
  file=$here/results/$stamp.md
  commit=$(git -C "$here/.." rev-parse --short HEAD)
  cpu=$(lscpu | sed -n 's/^Model name: *//p' | head -1)
  [ -n "$cpu" ] && [ "$cpu" != - ] ||
    cpu="$(uname -m), $(lscpu | sed -n 's/^Vendor ID: *//p' | head -1)"
  mkdir -p "$here/results"
  {
    echo "# bench run $stamp"
    echo
    echo "- Machine: $cpu," \
      "$(nproc) cores, $(free -g | awk '/^Mem:/ { print $2 }') GB," \
      "$(uname -sr)"
    echo "- digres: $commit," \
      "make release (DESIGN.md section 2); digres --build says:"
    echo
    "$digres" --build | sed 's/^/      /'
    echo
    echo "- PostgreSQL: $("$pg18/bin/postgres" --version), at 724edf9"
    echo "- $runs runs a query after $warm, Q7 for $q7_seconds seconds"
    echo
    echo "Execution time in ms, median (least-greatest); Q7 in" \
      "transactions a second."
    echo
    echo "| Query | Table | PostgreSQL | digres | digres / PostgreSQL |" \
      "Plan, PostgreSQL | Plan, digres |"
    echo "| --- | --- | --- | --- | --- | --- | --- |"
    grep -v '^size' "$work/pg.results" | while IFS='|' read -r n t p plan; do
      local d dplan ratio
      d=$(field "$work/dg.results" "$n" "$t" 3)
      dplan=$(field "$work/dg.results" "$n" "$t" 4)
      ratio=$(awk -v p="${p%% *}" -v d="${d%% *}" \
        'BEGIN { if (p > 0) printf "%.2f", d / p }')
      echo "| $n | $t | $p | $d | $ratio | $plan | $dplan |"
    done
    echo
    echo "Sizes as each system reports them; digres's leaves out values" \
      "stored out of line, which the data directory's size holds."
    echo
    echo "| Size | PostgreSQL | digres |"
    echo "| --- | --- | --- |"
    grep '^size' "$work/pg.results" | while IFS='|' read -r _ t p; do
      echo "| $t | $p | $(field "$work/dg.results" size "$t" 3) |"
    done
  } > "$file"
  # Every report has a row in the log, with its date and commit (CODE.md).
  echo "| ${stamp:0:4}-${stamp:4:2}-${stamp:6:2} ${stamp:9:2}:${stamp:11:2}" \
    "| $commit | $cpu | [$stamp]($stamp.md) |" >> "$here/results/LOG.md"
  echo "run: report in $file"
}

case $which in
pg) measure pg 54318 ;;
dg) measure dg 54319 ;;
both)
  measure pg 54318
  measure dg 54319
  report
  ;;
*)
  echo "usage: bench/run.sh [pg|dg|both]" >&2
  exit 2
  ;;
esac
