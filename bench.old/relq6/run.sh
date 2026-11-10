#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# run.sh -- time q6.rel: the bench's q6 as the calls it makes through
# sm to RocksDB, run by rel. rel is built optimized, against the sm
# and the RocksDB the bench's digres links (../CODE.md): sm in
# sm/build/release, RocksDB from dg/test/rocksdb.sh release, both made
# by make -C dg release. The rows are loaded once, into Q6_HOME, and
# kept; RELOAD=1 loads them again.
#
#   run.sh [queries]     500 queries if not given
#
# It prints q6.rel's time a query and a row, then the user
# instructions and cycles a row of rel's own thread in the timed
# queries, by perf record: not of its background threads, sm1's GC
# worker and RocksDB's, nor of attaching, opening and reading every
# row once to warm the cache. Pinned to cores 0-5, as ../run.sh pins
# the servers.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
export Q6_HOME=${Q6_HOME:-$top/bench/build/relq6}
export Q6_QUERIES=${1:-500}
rel=$top/sm/build/release/rel

# rel, optimized, linking the release sm and RocksDB.
[ -f "$top/sm/build/release/libsm.a" ] &&
  [ -f "$top/rocksdb/release/librocksdb.a" ] || {
  echo "run.sh: no release sm or RocksDB: make -C dg release first" >&2
  exit 1
}
make -s -C "$top/sm/rel" -j4 BUILD=../build/release \
  LDLIBS="-L/usr/local/lib -ljanet $top/rocksdb/release/librocksdb.a \
-lsnappy -llz4 -lzstd -lz -lbz2 -licui18n -licuuc -licudata -lpthread -ldl -lm"
if ldd "$rel" | grep -q librocksdb; then
  echo "run.sh: $rel links a shared RocksDB, not the release one" >&2
  exit 1
fi

if [ "${RELOAD:-0}" = 1 ] || [ ! -d "$Q6_HOME" ]; then
  Q6_PHASE=load "$rel" "$here/q6.rel"
fi

# The scan run, sampled every 10 million user instructions and cycles
# (under the kernel's cap on the sample rate, which a shorter period
# passes, so samples are dropped),
# on the monotonic clock q6.rel reads.
Q6_PHASE=scan perf record -q -k CLOCK_MONOTONIC -c 10000000 \
  -e instructions:u -e cycles:u -o "$Q6_HOME.perf" \
  taskset -c 0-5 "$rel" "$here/q6.rel" > "$Q6_HOME.out"
grep -v '^window' "$Q6_HOME.out"
# The samples of rel's own thread, its pid, within the timed loop:
# each 10 million of its event.
perf script -i "$Q6_HOME.perf" -F pid,tid,time,event 2>/dev/null |
  awk -v rows=$((Q6_QUERIES * 10000)) "$(awk '/^window/ {
    printf "BEGIN {t0 = %s; t1 = %s}", $2, $3}' "$Q6_HOME.out")"'
  { split($1, p, "/"); t = $2 + 0 }
  p[1] == p[2] && t >= t0 && t <= t1 {
    if (index($3, "/instructions")) i++; else if (index($3, "/cycles")) c++ }
  END { printf "rel: %.0f instructions a row, %.0f cycles a row\n",
        i * 1e7 / rows, c * 1e7 / rows }'
