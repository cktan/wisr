#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# rocksdb.sh -- build RocksDB for "make tsan" or "make release".
#
#   test/rocksdb.sh tsan       ../rocksdb/tsan/librocksdb.a
#   test/rocksdb.sh release    ../rocksdb/release/librocksdb.a
#
# The system's RocksDB is not instrumented: the sanitizer cannot see the
# atomics its threads hand work over with, and reports races in RocksDB's
# memory that are none. Nor is it a release build: Ubuntu's keeps its
# assert()s. This builds the system's version, from its source, with
# -fsanitize=thread, or optimized with no assertions, as a static
# library with the system's compression libraries: sm compiles against
# the system's headers, so another version does not link. It clones the
# source into ../rocksdb/src the first time, and does nothing once the
# library of that version is there; one of another version is built
# again. ../rocksdb is the repository's, beside dg/ and bench.old/, which
# link it, and outside every build/: "make clean" leaves it, as it
# changes only with RocksDB's version.
cd "$(dirname "$0")/.." || exit 1
kind=$1
case $kind in
tsan) flags="-O1 -g -fsanitize=thread" ;;
release) flags="-O3 -DNDEBUG" ;;
*)
  echo "usage: test/rocksdb.sh tsan|release" >&2
  exit 2
  ;;
esac
# The version of the system's headers, which sm is compiled against.
h=/usr/include/rocksdb/version.h
version=$(awk '/define ROCKSDB_(MAJOR|MINOR|PATCH) /{v = v (v ? "." : "") $3}
  END {print v}' "$h")
[ -n "$version" ] || {
  echo "rocksdb: no version in $h" >&2
  exit 1
}
src=$PWD/../rocksdb/src
out=$PWD/../rocksdb/$kind
# A source or a library of another version is put aside, to build anew.
for d in "$src" "$out"; do
  if [ -d "$d" ] && [ "$(cat "$d.version" 2> /dev/null)" != "$version" ]; then
    rm -rf "$d.old"
    mv "$d" "$d.old"
  fi
done
# The flags beside the library, for "digres --build" to name.
mkdir -p "$out"
echo "$flags" > "$out/flags"
echo "$version" > "$out.version"
[ -f "$out/librocksdb.a" ] && exit 0
if [ ! -d "$src" ]; then
  git clone --depth 1 --branch "v$version" \
    https://github.com/facebook/rocksdb.git "$src" || exit 1
  echo "$version" > "$src.version"
fi
# GCC 15 no longer includes <cstdint> by way of <string>; 9.11.2's
# blob_file_meta.h names uint64_t without it.
cmake -S "$src" -B "$out" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags -include cstdint" \
  -DCMAKE_C_FLAGS_RELEASE="" -DCMAKE_CXX_FLAGS_RELEASE="" \
  -DPORTABLE=1 -DFAIL_ON_WARNINGS=OFF -DUSE_RTTI=1 \
  -DWITH_SNAPPY=ON -DWITH_LZ4=ON -DWITH_ZSTD=ON -DWITH_ZLIB=ON -DWITH_BZ2=ON \
  -DWITH_GFLAGS=OFF -DWITH_TESTS=OFF -DWITH_TOOLS=OFF \
  -DWITH_BENCHMARK_TOOLS=OFF -DWITH_CORE_TOOLS=OFF -DWITH_TRACE_TOOLS=OFF \
  -DROCKSDB_BUILD_SHARED=OFF > "$out.cmake.log" 2>&1 || {
  echo "rocksdb: cmake failed; see $out.cmake.log"
  exit 1
}
cmake --build "$out" --target rocksdb -j4 > "$out.build.log" 2>&1 || {
  echo "rocksdb: build failed; see $out.build.log"
  exit 1
}
