#!/bin/bash
# rowlocks.sh -- what a row lock costs digres (../sm/NEXT.md section 1,
# item 4): rows locked FOR UPDATE, one, ten and a hundred a statement,
# each against the same rows read without a lock, so the difference is
# the locks'; and rows inserted under a foreign key to a few parents,
# every client's check locking the same few rows shared, against rows
# inserted with none. Each client connects once and runs statements for
# the whole run, each statement its own transaction, its locks released
# at its commit. Each client reads and locks rows of its own, a
# sixteenth of r, so no client waits for another's lock: what is timed
# is taking and releasing them.
#
#   bench/rowlocks.sh
#
# The statements, on r, 100,000 rows, a key at random each time in the
# client's sixteenth:
#
#   read1    one row by its key
#   lock1    the same, FOR UPDATE: one row lock
#   read10   ten rows, a range of the key
#   lock10   the same, FOR UPDATE: ten
#   read100  a hundred rows, a range
#   lock100  the same, FOR UPDATE: a hundred
#   ins      a row into n, which has no foreign key
#   fk       a row into c, whose foreign key names one of p's 10 rows:
#            the check locks it FOR KEY SHARE, S in sm
#
# Each prepared, with 1 and 16 clients, SECONDS_PER_RUN (10) of pgbench
# a run, ROUNDS (2) rounds; the median of the rounds is reported, with
# what a lock adds to a statement, (lockN - readN) / N, and the check
# to an insert, fk - ins, in microseconds of a client's time. digres is
# ../dg/build/digres-release ("make -C dg release"), checked to be a
# release build (CODE.md); its data directory and log are under
# bench/build/rowlocks, the raw runs in bench/build/rowlocks/runs.
cd "$(dirname "$0")" || exit 1
set -u
work=$PWD/build/rowlocks
digres=$PWD/../dg/build/digres-release
psql=${PSQL:-psql}
pgbench=${PGBENCH:-pgbench}
secs=${SECONDS_PER_RUN:-10}
rounds=${ROUNDS:-2}
port=${PORT:-54482}

# Only optimized builds are measured (CODE.md): every part of digres
# without assertions, and RocksDB its own static build.
if [ "$("$digres" --build | grep -vc NDEBUG)" != 0 ] ||
  ! "$digres" --build | grep -q '^RocksDB .*, static,'; then
  echo "rowlocks: $digres is not a release build; see CODE.md"
  "$digres" --build
  exit 1
fi

rm -rf "$work"
mkdir -p "$work"
# A socket path over 107 bytes cannot be bound: a short directory for it.
sock=$(mktemp -d /tmp/rowlocks.XXXX)
"$digres" --init "$work/data" > "$work/initdb.out" 2>&1 || {
  echo "rowlocks: initdb failed; see $work/initdb.out"
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

"$psql" -X -q -h "$sock" -p "$port" -d postgres > "$work/load.out" 2>&1 << 'SQL'
create table r (id int8 primary key, v int8);
insert into r select g, g from generate_series(1, 100000) g;
create table p (id int8 primary key);
insert into p select g from generate_series(1, 10) g;
create table c (pid int8 not null references p);
create table n (pid int8 not null);
vacuum (freeze, analyze);
SQL

# A statement's script: its rows from a key at random in the client's
# sixteenth of r, $2 of them, the lock clause $3.
script() {
  {
    echo "\\set k :client_id * 6250 + random(1, $((6250 - $2 + 1)))"
    if [ "$2" = 1 ]; then
      echo "select v from r where id = :k $3;"
    else
      echo "select v from r where id between :k and :k + $(($2 - 1)) $3;"
    fi
  } > "$work/$1.sql"
}
script read1 1 ""
script lock1 1 "for update"
script read10 10 ""
script lock10 10 "for update"
script read100 100 ""
script lock100 100 "for update"
# An insert's script: a row into table $2, naming a parent at random.
insert() {
  printf '\\set k random(1, 10)\ninsert into %s values (:k);\n' "$2" \
    > "$work/$1.sql"
}
insert ins n
insert fk c

# The transactions a second of one run: $1 clients, statement $2.
run() {
  "$pgbench" -h "$sock" -p "$port" -n -M prepared -T "$secs" -c "$1" \
    -j "$(($1 < 4 ? $1 : 4))" -f "$work/$2.sql" postgres 2>&1 |
    sed -n 's/^tps = \([0-9.]*\).*/\1/p'
}

for round in $(seq "$rounds"); do
  for c in 1 16; do
    for st in read1 lock1 read10 lock10 read100 lock100 ins fk; do
      echo "$round $st $c $(run "$c" "$st")"
    done
  done
done > "$work/runs"
kill "$server"
wait "$server" 2> /dev/null
rm -rf "$sock"

# The report: tps by statement and clients, how 16 clients scale on 1,
# and on each lockN line what a lock adds over readN, on fk's what the
# check adds over ins, in microseconds of a client's time.
python3 - "$work/runs" << 'PY'
import statistics, sys
runs = {}
for line in open(sys.argv[1]):
    f = line.split()
    if len(f) == 4:
        runs.setdefault((f[1], f[2]), []).append(float(f[3]))
tps = {k: statistics.median(v) for k, v in runs.items()}
pairs = {'lock1': ('read1', 1), 'lock10': ('read10', 10),
         'lock100': ('read100', 100), 'fk': ('ins', 1)}
print('%-8s %9s %9s %7s %9s %9s' % (
    'stmt', 'tps c=1', 'tps c=16', '16/1', 'us/lock 1', 'us/lock 16'))
for st in ('read1', 'lock1', 'read10', 'lock10', 'read100', 'lock100', 'ins',
           'fk'):
    one, six = tps.get((st, '1')), tps.get((st, '16'))
    if not (one and six):
        continue
    line = '%-8s %9.0f %9.0f %7.2f' % (st, one, six, six / one)
    if st in pairs:
        base, n = pairs[st]
        # A client's time a statement: clients over tps.
        per = []
        for c in ('1', '16'):
            t, b = tps.get((st, c)), tps.get((base, c))
            per.append((int(c) * 1e6 / t - int(c) * 1e6 / b) / n
                       if t and b else float('nan'))
        line += ' %9.2f %9.2f' % tuple(per)
    print(line)
PY
