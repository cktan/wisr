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
#   bench/rowlocks.sh          PostgreSQL 18, then digres, and the ratio
#   bench/rowlocks.sh pg|dg    one, for a look
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
# to an insert, fk - ins, in microseconds of a client's time; with both
# systems, digres's tps over PostgreSQL's for each. PostgreSQL is
# bench/build/pg18 (DESIGN.md section 2), checked to run without
# assertions, and digres ../dg/build/digres-release ("make -C dg
# release"), checked to be a release build (CODE.md). Each system's
# data directory and log are under bench/build/rowlocks, its raw runs
# in bench/build/rowlocks/SYS.runs.
cd "$(dirname "$0")" || exit 1
set -u
work=$PWD/build/rowlocks
digres=$PWD/../dg/build/digres-release
psql=${PSQL:-psql}
pgbench=${PGBENCH:-pgbench}
secs=${SECONDS_PER_RUN:-10}
rounds=${ROUNDS:-2}
port=${PORT:-54482}
pg18=$PWD/build/pg18
which=${1:-both}

# Only optimized builds are measured (CODE.md): every part of digres
# without assertions, and RocksDB its own static build.
if [ "$which" != pg ] &&
  { [ "$("$digres" --build | grep -vc NDEBUG)" != 0 ] ||
    ! "$digres" --build | grep -q '^RocksDB .*, static,'; }; then
  echo "rowlocks: $digres is not a release build; see CODE.md"
  "$digres" --build
  exit 1
fi

rm -rf "$work"
mkdir -p "$work"
# A socket path over 107 bytes cannot be bound: a short directory for it.
sock=$(mktemp -d /tmp/rowlocks.XXXX)

# Make system $1's data directory, start its server and wait until it
# is ready; the settings of DESIGN.md section 3.
start() {
  local sys=$1 data=$work/$1
  if [ "$sys" = pg ]; then
    "$pg18/bin/initdb" -D "$data" -A trust --no-sync > "$work/$sys.initdb" \
      2>&1 || { echo "rowlocks: initdb failed; see $work/$sys.initdb"; exit 1; }
    printf "shared_buffers = 1GB\njit = off\nautovacuum = off\n%s\n" \
      "max_parallel_workers_per_gather = 0" >> "$data/postgresql.conf"
    "$pg18/bin/postgres" -D "$data" -k "$sock" -p "$port" -c \
      listen_addresses= > "$work/$sys.log" 2>&1 &
  else
    "$digres" --init "$data" > "$work/$sys.initdb" 2>&1 ||
      { echo "rowlocks: initdb failed; see $work/$sys.initdb"; exit 1; }
    printf "shared_buffers = 1GB\nlisten_addresses = ''\n" \
      >> "$data/postgresql.conf"
    "$digres" -D "$data" -k "$sock" -p "$port" > "$work/$sys.log" 2>&1 &
  fi
  server=$!
  for _ in $(seq 600); do
    grep -q "ready to accept" "$work/$sys.log" && return
    sleep 0.1
  done
  echo "rowlocks: the $sys server did not start; see $work/$sys.log"
  exit 1
}

# Stop the server start() started, and wait for it.
stop() {
  kill -INT "$server"
  wait "$server" 2> /dev/null
}

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

# Load system $1 and run every statement on it, into $work/$1.runs.
measure() {
  start "$1"
  # PostgreSQL measured without its assertions (CODE.md).
  if [ "$1" = pg ] && [ "$("$psql" -X -A -t -h "$sock" -p "$port" \
    -d postgres -c 'show debug_assertions')" != off ]; then
    echo "rowlocks: PostgreSQL runs with assertions; see CODE.md"
    stop
    exit 1
  fi
  "$psql" -X -q -h "$sock" -p "$port" -d postgres > "$work/$1.load" 2>&1 << 'SQL'
create table r (id int8 primary key, v int8);
insert into r select g, g from generate_series(1, 100000) g;
create table p (id int8 primary key);
insert into p select g from generate_series(1, 10) g;
create table c (pid int8 not null references p);
create table n (pid int8 not null);
vacuum (freeze, analyze);
SQL
  for round in $(seq "$rounds"); do
    for c in 1 16; do
      for st in read1 lock1 read10 lock10 read100 lock100 ins fk; do
        echo "$round $st $c $(run "$c" "$st")"
      done
    done
  done > "$work/$1.runs"
  stop
}

case $which in
both) systems="pg dg" ;;
pg | dg) systems=$which ;;
*)
  echo "usage: bench/rowlocks.sh [pg|dg]" >&2
  exit 2
  ;;
esac
for sys in $systems; do
  measure "$sys"
done
rm -rf "$sock"

# The report, for each system: tps by statement and clients, how 16
# clients scale on 1, and on each lockN line what a lock adds over
# readN, on fk's what the check adds over ins, in microseconds of a
# client's time. With both, digres's tps over PostgreSQL's.
python3 - "$work" $systems << 'PY'
import statistics, sys
work, systems = sys.argv[1], sys.argv[2:]
stmts = ('read1', 'lock1', 'read10', 'lock10', 'read100', 'lock100', 'ins',
         'fk')
pairs = {'lock1': ('read1', 1), 'lock10': ('read10', 10),
         'lock100': ('read100', 100), 'fk': ('ins', 1)}
tps = {}
for sys_ in systems:
    runs = {}
    for line in open('%s/%s.runs' % (work, sys_)):
        f = line.split()
        if len(f) == 4:
            runs.setdefault((f[1], f[2]), []).append(float(f[3]))
    tps[sys_] = {k: statistics.median(v) for k, v in runs.items()}
for sys_ in systems:
    t = tps[sys_]
    print('%s\n%-8s %9s %9s %7s %9s %9s' % (
        {'pg': 'PostgreSQL 18', 'dg': 'digres'}[sys_], 'stmt', 'tps c=1',
        'tps c=16', '16/1', 'us/lock 1', 'us/lock 16'))
    for st in stmts:
        one, six = t.get((st, '1')), t.get((st, '16'))
        if not (one and six):
            continue
        line = '%-8s %9.0f %9.0f %7.2f' % (st, one, six, six / one)
        if st in pairs:
            base, n = pairs[st]
            # A client's time a statement: clients over tps.
            per = []
            for c in ('1', '16'):
                a, b = t.get((st, c)), t.get((base, c))
                per.append((int(c) * 1e6 / a - int(c) * 1e6 / b) / n
                           if a and b else float('nan'))
            line += ' %9.2f %9.2f' % tuple(per)
        print(line)
    print()
if len(systems) == 2:
    print('digres / PostgreSQL, tps\n%-8s %7s %7s' % ('stmt', 'c=1', 'c=16'))
    for st in stmts:
        r = []
        for c in ('1', '16'):
            d, p = tps['dg'].get((st, c)), tps['pg'].get((st, c))
            r.append(d / p if d and p else float('nan'))
        print('%-8s %7.2f %7.2f' % ((st,) + tuple(r)))
PY
