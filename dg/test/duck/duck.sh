#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# duck.sh -- run DuckDB's SQL tests on PostgreSQL 18 and on digres, side
# by side, and report each place where digres differs (README.md here).
#
#   test/duck/duck.sh                       the default directories
#   test/duck/duck.sh test/sql/upsert ...   these directories or files
#   test/duck/duck.sh --tree ...            digres indexes are btree
#   test/duck/duck.sh --isolation LEVEL ... sessions run at LEVEL
#   test/duck/duck.sh --selftest            prove the runner works
#
# No make target or other test runs this: a person starts it by hand.
# DuckDB's files are fetched once, at a pinned commit, into
# build/duck-corpus. The servers, their data and logs, and the report are
# in build/dk. PostgreSQL 18 is the one in $PG18, ../bench/build/pg18 if
# that is not set. The exit status is 0 with no finding, 1 with one, 2 if
# the run could not start.
cd "$(dirname "$0")/../.." || exit 2
repo=https://github.com/duckdb/duckdb.git
commit=b2207b7290887503b6280ee41f3c8c597a884141
corpus=$PWD/build/duck-corpus
dk=$PWD/build/dk
report=$dk/report
pg18=${PG18:-$PWD/../bench/build/pg18}
defaults=(test/sql/transactions test/sql/update test/sql/upsert
  test/sql/constraints test/sql/delete test/sql/index test/sql/alter
  test/sql/types/decimal)
flags=()
selftest=0
paths=()
while [ $# -gt 0 ]; do
  case $1 in
    --tree) flags+=(--tree) ;;
    --isolation) flags+=(--isolation "$2"); shift ;;
    --selftest) selftest=1 ;;
    *) paths+=("${1%/}") ;;
  esac
  shift
done
[ ${#paths[@]} -gt 0 ] || paths=("${defaults[@]}")

# Fail with what is missing, before anything starts.
die() {
  echo "duck: FAILED ($1)"
  exit 2
}
[ -x "$pg18/bin/postgres" ] || die "no PostgreSQL 18 in $pg18; set PG18"
[ -x build/digres ] ||
  die "no build/digres; run DEBUG=1 make -C sm -j4 && DEBUG=1 make -C dg -j4"

# The runner, built as dg/Makefile builds build/slt, less -lcrypto.
g++ -std=c++20 -O2 -Wall $(pkg-config --cflags libpq) -o build/duckrun \
  test/duck/duckrun.cc -lpq || die "could not build build/duckrun"

# The files to run: the self-test's, or DuckDB's, fetched once with no
# file's contents until sparse-checkout names it.
if [ $selftest = 1 ]; then
  root=$PWD/test/duck
  paths=(selftest)
else
  root=$corpus
  if [ ! -d "$corpus/.git" ]; then
    git clone -q --filter=blob:none --no-checkout "$repo" "$corpus" ||
      die "could not fetch $repo"
  fi
  patterns=()
  for p in "${paths[@]}"; do
    case $p in
      *.test | *.test_slow) patterns+=("/$p") ;;
      *) patterns+=("/$p/") ;;
    esac
  done
  git -C "$corpus" sparse-checkout set --no-cone "${patterns[@]}" &&
    git -C "$corpus" checkout -q "$commit" || die "could not check out $commit"
fi
files=()
for p in "${paths[@]}"; do
  if [ -f "$root/$p" ]; then
    files+=("$root/$p")
  else
    mapfile -t -O ${#files[@]} files < <(find "$root/$p" \
      \( -name '*.test' -o -name '*.test_slow' \) 2> /dev/null | sort)
  fi
done
[ ${#files[@]} -gt 0 ] || die "no test files in ${paths[*]}"

# A fresh build/dk: a directory for each server's data, and for the
# files each writes for {TEST_DIR}.
rm -rf "$dk"
mkdir -p "$report" "$dk/tmp/pg" "$dk/tmp/dg"
free_port() {
  python3 -c 'import socket; s = socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}
pg_port=$(free_port)
dg_port=$(free_port)
"$pg18/bin/initdb" -D "$dk/pg" -A trust > "$dk/pg.initdb" 2>&1 ||
  die "PostgreSQL 18's initdb; see $dk/pg.initdb"
build/digres --init "$dk/dg" > "$dk/dg.initdb" 2>&1 ||
  die "digres's initdb; see $dk/dg.initdb"

# Start server NAME (pg or dg), and wait until its log says it is
# ready, 30 seconds at most; the log of each start is kept.
start() {
  local log=$dk/$1.log
  if [ "$1" = pg ]; then
    "$pg18/bin/postgres" -D "$dk/pg" -k "$dk" -p $pg_port \
      -c listen_addresses= >> "$log" 2>&1 &
    pg_pid=$!
  else
    build/digres -D "$dk/dg" -k "$dk" -p $dg_port >> "$log" 2>&1 &
    dg_pid=$!
  fi
  for _ in $(seq 300); do
    [ "$(grep -c "ready to accept connections" "$log")" -gt "${2:-0}" ] &&
      return 0
    sleep 0.1
  done
  return 1
}

# Stop the server whose pid is $1, promptly, and by force if it must.
stop() {
  kill -INT "$1" 2> /dev/null
  for _ in $(seq 100); do
    kill -0 "$1" 2> /dev/null || break
    sleep 0.1
  done
  kill -KILL "$1" 2> /dev/null
  wait "$1" 2> /dev/null
}

# Stop both servers when the script ends, however it ends.
trap 'stop $pg_pid; stop $dg_pid' EXIT
start pg || die "PostgreSQL 18 did not start; see $dk/pg.log"
start dg || die "digres did not start; see $dk/dg.log"

# Run the files files.tsv does not list yet; after a crash or a hang,
# keep digres's log, restart both servers, and go on with the next file.
printf '%s\n' "${files[@]}" > "$dk/all"
known=$PWD/test/duck/known.txt
crashes=0
while :; do
  cut -f1 "$report/files.tsv" 2> /dev/null | sed "s|^|$root/|" |
    sort > "$dk/done"
  mapfile -t left < <(sort "$dk/all" | comm -23 - "$dk/done")
  [ ${#left[@]} -gt 0 ] || break
  build/duckrun "${flags[@]}" --known "$known" --root "$root" \
    --report "$report" --tmp "$dk/tmp" \
    "host=$dk port=$pg_port dbname=postgres" \
    "host=$dk port=$dg_port dbname=postgres" "${left[@]}" >> "$dk/run.log"
  status=$?
  [ $status = 0 ] && break
  [ $status = 3 ] || die "the runner could not start; see $dk/run.log"
  crashes=$((crashes + 1))
  cp "$dk/dg.log" "$report/digres-$crashes.log"
  echo "duck: restarting both servers after $(tail -1 "$report/files.tsv" |
    cut -f1,3)"
  for s in pg dg; do
    n=$(grep -c "ready to accept connections" "$dk/$s.log")
    if [ $s = pg ]; then stop $pg_pid; else stop $dg_pid; fi
    start $s "$n" || die "$s did not restart; see $dk/$s.log"
  done
done

# The report: the files of each directory asked for, and their
# findings by kind.
group() {
  local p
  for p in "${paths[@]}"; do
    case $1 in "$p" | "$p"/*) echo "$p"; return ;; esac
  done
  echo other
}
while IFS=$'\t' read -r f st detail run notrun both; do
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(group "$f")" "$f" "$st" \
    "$detail" "$run" "$notrun" "$both"
done < "$report/files.tsv" > "$dk/grouped"
{
  printf '%-24s %6s %8s %6s %8s  %s\n' directory files skipped clean records \
    findings
  awk -F'\t' '{ n[$1]++; r[$1] += $5; if ($3 == "skipped") s[$1]++
    if ($3 == "clean" || $3 == "known") c[$1]++
    if ($3 == "finding" || $3 == "crash") {
      split($4, k, " "); f[$1] = f[$1] " " k[1] }
    if ($3 == "error") f[$1] = f[$1] " error" }
    END { for (d in n) printf "%-24s %6d %8d %6d %8d %s\n", d, n[d], s[d],
      c[d], r[d], f[d] }' "$dk/grouped" | sort |
    awk '{ out = ""; delete k
      for (i = 6; i <= NF; i++) k[$i]++
      for (w in k) out = out " " w "=" k[w]
      printf "%-24s %6d %8d %6d %8d %s\n", $1, $2, $3, $4, $5, out }'
} > "$report/summary"
awk -F'\t' '$3 == "skipped" { print $2 "\t" $4 }' "$dk/grouped" \
  > "$report/skipped"
sort "$report/dialect.raw" 2> /dev/null | uniq -c | sort -rn | head -30 \
  > "$report/dialect"
awk -F'\t' '$5 > 0 && $7 * 2 > $5 { print $2 "\t" $7 " of " $5 " failed on both" }' \
  "$dk/grouped" > "$report/dead"
touch "$report/findings"

# The self-test: each file says on a line "# expect: STATUS DETAIL RUN
# NOTRUN BOTH" what files.tsv must say of it, with digres's indexes as
# written and again with --tree, so both rewrites of CREATE INDEX run.
if [ $selftest = 1 ]; then
  mkdir -p "$dk/report-tree"
  build/duckrun --tree --root "$root" --report "$dk/report-tree" \
    --tmp "$dk/tmp" "host=$dk port=$pg_port dbname=postgres" \
    "host=$dk port=$dg_port dbname=postgres" "${files[@]}" \
    >> "$dk/run.log" || die "the runner failed with --tree; see $dk/run.log"
  bad=0
  for f in "${files[@]}"; do
    rel=${f#$root/}
    want=$(sed -n 's/^# expect: //p' "$f")
    for r in "$report" "$dk/report-tree"; do
      got=$(awk -F'\t' -v f="$rel" '$1 == f { print $2, $3, $4, $5, $6 }' \
        "$r/files.tsv")
      mode=$([ "$r" = "$report" ] && echo "     " || echo "tree ")
      if [ "$want" = "$got" ]; then
        echo "duck: ok   $mode$rel"
      else
        echo "duck: FAIL $mode$rel: expected '$want', got '$got'"
        bad=$((bad + 1))
      fi
    done
  done
  [ $bad = 0 ] && { echo "duck: PASSED (self-test)"; exit 0; }
  echo "duck: FAILED ($bad self-test files)"
  exit 1
fi

cat "$report/summary"
n=$(grep -cE '^(crash|hang|digres-only-error|pg-only-error|sqlstate-differs|result-differs|order-only) ' \
  "$report/findings")
e=$(awk -F'\t' '$2 == "error"' "$report/files.tsv" | wc -l)
if grep -qE "TRAP:|terminated by signal" "$dk/dg.log" "$report"/digres-*.log 2> /dev/null; then
  echo "duck: digres's log tells of a crash; see $dk/dg.log"
fi
if [ "$n" = 0 ] && [ "$e" = 0 ]; then
  echo "duck: PASSED"
  exit 0
fi
echo "duck: FAILED ($n findings, $e files that could not run; see $report)"
exit 1
