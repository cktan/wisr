#!/bin/bash
# privcheck.sh -- what a privilege check costs a role that is not a
# superuser, in instructions, on digres and PostgreSQL 18.
#
#   bench/privcheck.sh
#
# Statements that read no row leave the checks most of what is counted:
#
#   t      select a ... WHERE false: the table's check, no function
#   fn     select length(c) ... WHERE false: the table's, and one EXECUTE
#   fn16   sixteen length(c): the table's, and sixteen EXECUTE
#   pt     a point read of narrow, a million rows: the gap a client sees
#
# An EXECUTE check runs at executor start, for each function in the
# plan, so it runs under WHERE false. Timing would not show these gaps:
# a run's count swings about 2%. A role's check is taken as the role's
# count over the superuser's, as the superuser skips it.
#
# 1. Start each server pinned to a CPU, its data under build/privcheck.
# 2. Make the tables, and a role granted SELECT on them.
# 3. Run a round that is not reported: the load's compactions and
#    autovacuum count as the server's instructions while they run.
# 4. Count the server's user instructions for runs of N1 and N2
#    statements, prepared, by perf stat.
# 5. Take the slope of the two: connection and first plans cancel.
# 6. Report the median of ROUNDS (5) slopes, and their spread.
#
# The slope leaves out a session's first check of a function, which
# misses what the session keeps: a check here is a kept one. digres is
# ../dg/build/digres-release ("make -C dg release"). PGBIN is PostgreSQL
# 18's bin (~/p/postgres18/install/bin). SRV_CPU and CLI_CPU are the
# CPUs of the server and of pgbench.
cd "$(dirname "$0")" || exit 1
set -u
root=$(cd .. && pwd)
work=$PWD/build/privcheck
digres=$root/dg/build/digres-release
pgbin=${PGBIN:-$HOME/p/postgres18/install/bin}
n1=${N1:-10000}
n2=${N2:-60000}
rounds=${ROUNDS:-5}
srv_cpu=${SRV_CPU:-2}
cli_cpu=${CLI_CPU:-4}
me=$(id -un)

rm -rf "$work"
mkdir -p "$work"
# A socket path over 107 bytes cannot be bound: a short directory for it.
sock=$(mktemp -d /tmp/privcheck.XXXX)
trap 'rm -rf "$sock"' EXIT

cat > "$work/t.sql" << 'EOF'
select a from e where false;
EOF
cat > "$work/fn.sql" << 'EOF'
select length(c) from e where false;
EOF
{
  printf 'select length(c)'
  for _ in $(seq 15); do printf ', length(c)'; done
  echo ' from e where false;'
} > "$work/fn16.sql"
cat > "$work/pt.sql" << 'EOF'
\set k random(1, 1000000)
select c1 from narrow where id = :k;
EOF

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

# Make the tables and the role on port $1, and refuse an assert build.
prepare() {
  sql "$1" << 'EOF' || return 1
create table e (id int8 primary key, a int8, c text);
create table narrow (id int8 primary key, c1 int8, c2 int8, c3 text, c4 int8);
insert into narrow select g, g, g, 'x' || g, g
  from generate_series(1, 1000000) g;
create role r login password 'p';
grant select on e, narrow to r;
EOF
  [ "$(sql "$1" -Atc 'show debug_assertions')" = off ]
}

# Count system $1, port $2, server pid $3: every statement, each user.
# Round 0 is the warm-up, which the report skips.
measure() {
  local sys=$1 port=$2 pid=$3 round st user n
  for round in $(seq 0 "$rounds"); do
    for st in t fn fn16 pt; do
      for user in "$me" r; do
        for n in "$n1" "$n2"; do
          # The sleep lets a session's thread end and fold its count
          # into its parent's before perf reads it.
          PGPASSWORD=p perf stat -x, -e instructions:u -p "$pid" \
            -o "$work/perf.out" -- taskset -c "$cli_cpu" sh -c \
            "'$pgbin/pgbench' -h '$sock' -p $port -U $user -n -M prepared \
             -c 1 -t $n -f '$work/$st.sql' postgres \
             > '$work/pgbench.out' 2>&1; sleep 0.3"
          grep -q 'number of failed transactions: 0' "$work/pgbench.out" || {
            echo "privcheck: pgbench failed: $sys $user $st" >&2
            cat "$work/pgbench.out" >&2
            return 1
          }
          echo "$sys $round $user $st $n" \
            "$(awk -F, '/instructions/{print $1}' "$work/perf.out")" \
            >> "$work/runs"
        done
      done
    done
  done
}

# digres, end to end.
"$digres" --init "$work/dg-data" > "$work/dg-init.out" 2>&1 || {
  echo "privcheck: digres init failed; see $work/dg-init.out" >&2
  exit 1
}
printf "shared_buffers = 1GB\nlisten_addresses = ''\n" \
  >> "$work/dg-data/postgresql.conf"
# The server under test, pinned so a count is not spread over CPUs.
taskset -c "$srv_cpu" "$digres" -D "$work/dg-data" -k "$sock" -p 54481 \
  > "$work/dg.log" 2>&1 &
server=$!
wait_ready "$work/dg.log" && prepare 54481 > "$work/dg-load.out" 2>&1 &&
  measure dg 54481 "$server"
rc=$?
kill "$server"
wait "$server" 2> /dev/null
[ "$rc" = 0 ] || exit 1

# PostgreSQL 18, the same way.
"$pgbin/initdb" -D "$work/pg-data" -A trust > "$work/pg-init.out" 2>&1 || {
  echo "privcheck: initdb failed; see $work/pg-init.out" >&2
  exit 1
}
printf "shared_buffers = 1GB\nlisten_addresses = ''\n" \
  >> "$work/pg-data/postgresql.conf"
# The reference server, pinned as digres was.
taskset -c "$srv_cpu" "$pgbin/postgres" -D "$work/pg-data" -k "$sock" \
  -p 54482 > "$work/pg.log" 2>&1 &
server=$!
wait_ready "$work/pg.log" && prepare 54482 > "$work/pg-load.out" 2>&1 &&
  measure pg 54482 "$server"
rc=$?
kill "$server"
wait "$server" 2> /dev/null
[ "$rc" = 0 ] || exit 1

# The report: instructions a statement, and what a role adds.
python3 - "$work/runs" "$me" << 'PY'
import statistics, sys
raw = {}
for line in open(sys.argv[1]):
    f = line.split()
    if len(f) == 6 and f[5].isdigit() and f[1] != '0':
        raw.setdefault(tuple(f[:4]), {})[int(f[4])] = int(f[5])
slopes = {}
for (system, _, user, st), v in raw.items():
    (a, ia), (b, ib) = sorted(v.items())
    key = (system, 'super' if user == sys.argv[2] else 'role', st)
    slopes.setdefault(key, []).append((ib - ia) / (b - a))
med = {k: statistics.median(v) for k, v in slopes.items()}
spread = lambda k: (max(slopes[k]) - min(slopes[k])) / med[k] * 100
print('%-3s %-5s %9s %9s %7s %6s %8s' % (
    'sys', 'stmt', 'super', 'role', 'gap', 'gap%', 'spread%'))
for system in ('dg', 'pg'):
    gap = {}
    for st in ('t', 'fn', 'fn16', 'pt'):
        s, r = med[(system, 'super', st)], med[(system, 'role', st)]
        gap[st] = r - s
        print('%-3s %-5s %9.0f %9.0f %7.0f %5.1f%% %7.2f%%' % (
            system, st, s, r, r - s, (r - s) / s * 100,
            max(spread((system, u, st)) for u in ('super', 'role'))))
    print('%s: an EXECUTE check: %.0f by fn, %.0f by fn16' % (
        system, gap['fn'] - gap['t'], (gap['fn16'] - gap['t']) / 16))
PY
