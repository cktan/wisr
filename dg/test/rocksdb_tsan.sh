#!/bin/bash
# rocksdb_tsan.sh -- build RocksDB for the thread sanitizer, for "make tsan".
#
#   test/rocksdb_tsan.sh
#
# The system's RocksDB is not instrumented: the sanitizer cannot see the
# atomics its threads hand work over with, and reports races in RocksDB's
# memory that are none. This builds the same version, 8.9.1, from its
# source, with -fsanitize=thread, as a static library with the system's
# compression libraries: build/rocksdb-tsan/librocksdb.a. It clones the
# source into build/rocksdb-src the first time, and does nothing once the
# library is there.
cd "$(dirname "$0")/.." || exit 1
src=$PWD/build/rocksdb-src
out=$PWD/build/rocksdb-tsan
[ -f "$out/librocksdb.a" ] && exit 0
if [ ! -d "$src" ]; then
  git clone --depth 1 --branch v8.9.1 https://github.com/facebook/rocksdb.git \
    "$src" || exit 1
fi
flags="-O1 -g -fsanitize=thread"
cmake -S "$src" -B "$out" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
  -DCMAKE_C_FLAGS_RELEASE="" -DCMAKE_CXX_FLAGS_RELEASE="" \
  -DPORTABLE=1 -DFAIL_ON_WARNINGS=OFF -DUSE_RTTI=1 \
  -DWITH_SNAPPY=ON -DWITH_LZ4=ON -DWITH_ZSTD=ON -DWITH_ZLIB=ON -DWITH_BZ2=ON \
  -DWITH_GFLAGS=OFF -DWITH_TESTS=OFF -DWITH_TOOLS=OFF \
  -DWITH_BENCHMARK_TOOLS=OFF -DWITH_CORE_TOOLS=OFF -DWITH_TRACE_TOOLS=OFF \
  -DROCKSDB_BUILD_SHARED=OFF > "$out.cmake.log" 2>&1 || {
  echo "rocksdb_tsan: cmake failed; see $out.cmake.log"
  exit 1
}
cmake --build "$out" --target rocksdb -j4 > "$out.build.log" 2>&1 || {
  echo "rocksdb_tsan: build failed; see $out.build.log"
  exit 1
}
