#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# roles.sh -- what checking privileges costs a role that is not a
# superuser (../sm/NEXT.md section 1, item 2): statements that read no
# row, so what is timed is the statement's checks, not its data, run by
# the superuser and by two roles, and the time each role adds to a
# statement and to each object a statement checks. Each client
# authenticates once and then runs statements for the whole run, as an
# application's connection does.
#
#   bench/roles.sh
#
# The users: the superuser; direct, granted SELECT on each table; and
# deep, which has SELECT through a group two grants up, so its checks
# follow role memberships. The statements:
#
#   one   select 1: no table, no check
#   t1    one table, WHERE ... AND false: one check
#   t16   sixteen tables, UNION ALL, each WHERE ... AND false: sixteen
#         checks, so a check's cost shows above the runs' noise
#   fn    one table, a function called: a check of EXECUTE too
#
# Each by the simple and the prepared protocol, with 1 and 16 clients,
# SECONDS (10) of pgbench a run, ROUNDS (2) rounds; the median of the
# rounds is reported. digres is ../dg/build/digres-release ("make -C dg
# release"); its data directory and log are under bench/build/roles,
# the raw runs in bench/build/roles/runs.
cd "$(dirname "$0")" || exit 1
set -u
work=$PWD/build/roles
digres=$PWD/../dg/build/digres-release
psql=${PSQL:-psql}
pgbench=${PGBENCH:-pgbench}
secs=${SECONDS_PER_RUN:-10}
rounds=${ROUNDS:-2}
port=${PORT:-54480}
me=$(id -un)

rm -rf "$work"
mkdir -p "$work"
# A socket path over 107 bytes cannot be bound: a short directory for it.
sock=$(mktemp -d /tmp/roles.XXXX)
"$digres" --init "$work/data" > "$work/initdb.out" 2>&1 || {
  echo "roles: initdb failed; see $work/initdb.out"
  exit 1
}
printf "shared_buffers = 1GB\nlisten_addresses = ''\n" \
  >> "$work/data/postgresql.conf"
# The server for the runs, stopped at the end.
"$digres" -D "$work/data" -k "$sock" -p "$port" > "$work/log" 2>&1 &
server=$!
for _ in $(seq 600); do
  grep -q "ready to accept" "$work/log" && break
  sleep 0.1
done

# Sixteen small tables, each with a key; direct granted SELECT on each,
# deep through g1, in g2, which is granted it.
tables=$(seq -s ', ' -f 't%g' 1 16)
{
  for i in $(seq 16); do
    echo "create table t$i (id int8 primary key, a int8, c text);"
    echo "insert into t$i select g, g, 'x' || g from generate_series(1, 1000) g;"
  done
  echo "create role direct login password 'p';"
  echo "create role deep login password 'p';"
  echo "create role g1;"
  echo "create role g2;"
  echo "grant g2 to g1;"
  echo "grant g1 to deep;"
  echo "grant select on $tables to direct, g2;"
} | "$psql" -X -q -h "$sock" -p "$port" -d postgres > "$work/load.out" 2>&1

cat > "$work/one.sql" << 'EOF'
select 1;
EOF
cat > "$work/t1.sql" << 'EOF'
\set k random(1, 1000)
select a from t1 where id = :k and a > 0 and false;
EOF
{
  echo '\set k random(1, 1000)'
  for i in $(seq 16); do
    echo "select a from t$i where id = :k and false"
    [ "$i" -lt 16 ] && echo "union all"
  done
  echo ";"
} > "$work/t16.sql"
cat > "$work/fn.sql" << 'EOF'
\set k random(1, 1000)
select length(c) from t1 where id = :k and false;
EOF

# The transactions a second of one run: user $1, protocol $2, $3 clients,
# statement $4.
run() {
  PGPASSWORD=p "$pgbench" -h "$sock" -p "$port" -U "$1" -n -M "$2" \
    -T "$secs" -c "$3" -j "$(($3 < 4 ? $3 : 4))" -f "$work/$4.sql" \
    postgres 2>&1 | sed -n 's/^tps = \([0-9.]*\).*/\1/p'
}

for round in $(seq "$rounds"); do
  for mode in simple prepared; do
    for c in 1 16; do
      for st in one t1 t16 fn; do
        for user in "$me" direct deep; do
          echo "$round $user $st $mode $c $(run "$user" "$mode" "$c" "$st")"
        done
      done
    done
  done
done > "$work/runs"
kill "$server"
wait "$server" 2> /dev/null
rm -rf "$sock"

# The report: tps by user; what each role adds to a statement, in
# microseconds of a client's time; and, on the t16 line, what it adds
# to each object checked, (t16 - t1) / 15.
python3 - "$work/runs" "$me" << 'PY'
import statistics, sys
runs, me = {}, sys.argv[2]
for line in open(sys.argv[1]):
    f = line.split()
    if len(f) == 6 and f[5]:
        runs.setdefault(tuple(f[1:5]), []).append(float(f[5]))
tps = {k: statistics.median(v) for k, v in runs.items()}
print('%-4s %-8s %3s %9s %9s %9s %9s %9s %11s %11s' % (
    'stmt', 'protocol', 'c', 'super', 'direct', 'deep', 'direct+us',
    'deep+us', 'direct+us/o', 'deep+us/o'))
for mode in ('simple', 'prepared'):
    for c in ('1', '16'):
        # A client's time a statement: clients over tps.
        us = lambda t: int(c) * 1e6 / t
        added = {}
        for st in ('one', 't1', 't16', 'fn'):
            s, d, p = (tps.get((u, st, mode, c)) for u in (me, 'direct', 'deep'))
            if not (s and d and p):
                continue
            added[st] = (us(d) - us(s), us(p) - us(s))
            line = '%-4s %-8s %3s %9.0f %9.0f %9.0f %9.1f %9.1f' % (
                st, mode, c, s, d, p, added[st][0], added[st][1])
            if 't16' == st and 't1' in added:
                line += ' %11.2f %11.2f' % tuple(
                    (added['t16'][i] - added['t1'][i]) / 15 for i in (0, 1))
            print(line)
PY
