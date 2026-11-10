#!/bin/bash
# leakupd.sh -- does an UPDATE grow digres's memory with the number of
# updates, whatever the size of the data? The soak (SOAK.md) grows on
# updates alone; this narrows it with a table of 1000 rows, which fits
# any cache, so a buffer pool filling cannot explain a growth.
#
#   bench/leakupd.sh [seconds]
#
# seconds (default 60) is the length of each phase.
#
# 1. Start digres, a release build, with 256 MB of shared buffers.
# 2. Make a table of 1000 rows, and run a warm-up that is not counted.
# 3. Run five phases, one client each, for the given time:
#    read     a select by key: the control, which should not grow
#    fixed    an update of one row, over and over
#    spread   an update of a random row of the 1000
#    spread-c the same, and a new connection for every update
#    batch    ten updates of random rows in one transaction
# 4. Per phase, print the transactions, the PSS before and after
#    (/proc/PID/smaps_rollup, in kB), and the kB gained a 1000
#    transactions.
set -uo pipefail
cd "$(dirname "$0")"

secs=${1:-60}
digres=${DIGRES:-$PWD/../dg/build/digres-release}
pgbin=${PGBIN:-$HOME/p/postgres18/install/bin}
work=$PWD/build/leakupd
port=5551

rm -rf "$work"
mkdir -p "$work" "$PWD/build/tmp"
export TMPDIR=$PWD/build/tmp
sock=$(mktemp -d /tmp/leakupd.XXXX)
export PGHOST=$sock
trap 'kill -INT "$server" 2> /dev/null; wait "$server" 2> /dev/null' EXIT

# The server's PSS in kB: sm's sessions are threads, so one process.
pss() {
  awk '/^Pss:/{print $2}' "/proc/$server/smaps_rollup"
}

# Print the 5 mappings of the server with the most PSS, as "kB range
# name", under the label $1. A big anonymous range that grows and then
# shrinks is a buffer or a memtable; a growing heap is many allocations.
maps() {
  echo "  $1:"
  awk '/^[0-9a-f]+-[0-9a-f]+ /{r=$1; n=($6 == "" ? "[anon]" : $6)}
       /^Pss:/{print $2, r, n}' "/proc/$server/smaps" |
    sort -rn | head -5 | sed 's/^/    /'
}

# 1. Start digres with 256 MB of shared buffers.
"$digres" --init "$work/data" > "$work/init.out" 2>&1 || exit 1
printf "shared_buffers = 256MB\nlisten_addresses = ''\n" \
  >> "$work/data/postgresql.conf"
"$digres" -D "$work/data" -k "$sock" -p "$port" > "$work/server.log" 2>&1 &
server=$!
for _ in $(seq 600); do
  grep -q 'ready to accept' "$work/server.log" && break
  sleep 0.1
done

# 2. Make the table.
"$pgbin/psql" -X -q -v ON_ERROR_STOP=1 -p "$port" -d postgres << 'EOF' || exit 1
create table t (id int8 primary key, v int8 not null);
insert into t select g, 0 from generate_series(1, 1000) g;
analyze t;
EOF

cat > "$work/read.sql" << 'EOF'
\set k random(1, 1000)
select v from t where id = :k;
EOF
cat > "$work/fixed.sql" << 'EOF'
update t set v = v + 1 where id = 1;
EOF
cat > "$work/spread.sql" << 'EOF'
\set k random(1, 1000)
update t set v = v + 1 where id = :k;
EOF
cat > "$work/batch.sql" << 'EOF'
begin;
\set a random(1, 1000)
\set b random(1, 1000)
\set c random(1, 1000)
\set d random(1, 1000)
\set e random(1, 1000)
update t set v = v + 1 where id = :a;
update t set v = v + 1 where id = :b;
update t set v = v + 1 where id = :c;
update t set v = v + 1 where id = :d;
update t set v = v + 1 where id = :e;
update t set v = v + 1 where id = :a + 1;
update t set v = v + 1 where id = :b + 1;
update t set v = v + 1 where id = :c + 1;
update t set v = v + 1 where id = :d + 1;
update t set v = v + 1 where id = :e + 1;
commit;
EOF

# Run script $2 for $secs seconds with pgbench options $3, and print a
# row for phase $1.
phase() {
  local name=$1 script=$2 opts=$3 before after n
  before=$(pss)
  # shellcheck disable=SC2086
  "$pgbin/pgbench" -p "$port" -d postgres -n -M prepared -c 1 -T "$secs" \
    $opts -f "$work/$script.sql" > "$work/$name.log" 2>&1
  after=$(pss)
  n=$(sed -n 's/^number of transactions actually processed: \([0-9]*\).*/\1/p' \
    "$work/$name.log")
  awk -v p="$name" -v n="$n" -v b="$before" -v a="$after" 'BEGIN {
    printf "%-9s %10d %10d %10d %12.2f\n", p, n, b, a,
      (a - b) * 1000.0 / (n ? n : 1) }'
  maps "after $name" >> "$work/maps.txt"
}

# The warm-up fills the caches and the first-use structures.
"$pgbin/pgbench" -p "$port" -d postgres -n -M prepared -c 1 -T "$secs" \
  -f "$work/spread.sql" > "$work/warmup.log" 2>&1

# 3 and 4. The phases, and the table of them.
maps "before the phases" > "$work/maps.txt"
printf "%-9s %10s %10s %10s %12s\n" phase txns pss_before pss_after kb_per_1000
phase read read ""
phase fixed fixed ""
phase spread spread ""
phase spread-c spread "-C"
phase batch batch ""

# 5. Idle: no client. If the reapers catch up, PSS falls; if it stays,
#    the memory is not garbage waiting to be reaped. PSS every 5 s.
echo "idle, PSS in kB after the phases:"
for t in $(seq 0 5 180); do
  [ "$t" -gt 0 ] && sleep 5
  printf "  +%3ds %d\n" "$t" "$(pss)"
  [ "$t" = 0 ] || [ "$t" = 60 ] || [ "$t" = 180 ] && maps "idle +${t}s" >> "$work/maps.txt"
done
echo "the biggest mappings (kB range name):"
cat "$work/maps.txt"
