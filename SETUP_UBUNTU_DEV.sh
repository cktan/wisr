#!/bin/bash
# SETUP_UBUNTU_DEV.sh -- make a minimal Ubuntu machine ready to build,
# test and benchmark wisr: README.md's "Prerequisites", as a script.
#
#   ./SETUP_UBUNTU_DEV.sh
#
# Each step does nothing once its result is there, so the script can
# be run again after a failure. What it installs:
#
#   - the apt packages
#   - Janet, commit 86ca958e, into /usr/local, for sm/rel
#   - PostgreSQL 18 (REL_18_STABLE) in $SRC/postgres18, installed into
#     its install/, for the tests that compare with it, and its bin/
#     put first on PATH in ~/.profile, for psql and pgbench
#   - PostgreSQL 18.6 (724edf9), optimized, into bench/build/pg18, for
#     the benchmark
#   - on Ubuntu newer than 24.04, the release build's RocksDB, which
#     GCC 15 compiles only with <cstdint> given to it
#
# SRC, where Janet and PostgreSQL are cloned, is ~/p by default: the
# tests look for PostgreSQL 18 in ~/p/postgres18 unless PG18_SRC says
# otherwise.
set -euo pipefail

wisr=$(cd "$(dirname "$0")" && pwd)
src=${SRC:-$HOME/p}
pg18=$src/postgres18

# Root in a container has no sudo, and needs none.
sudo=sudo
[ "$(id -u)" -eq 0 ] && sudo=

step() { echo "== $*"; }

# 1. The packages: the compilers and generators, the libraries sm and
#    digres link, PostgreSQL's build dependencies, and the clients.
step "apt packages"
$sudo apt-get update
DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y \
  build-essential cmake git ca-certificates flex bison python3 \
  clang-format librocksdb-dev libicu-dev \
  libsnappy-dev liblz4-dev libzstd-dev zlib1g-dev libbz2-dev \
  libreadline-dev libpq-dev libssl-dev pkg-config bc \
  postgresql-client linux-tools-generic

mkdir -p "$src"

# 2. Janet, which sm/rel embeds, at the commit its tests run on.
if [ ! -f /usr/local/include/janet.h ]; then
  step "janet"
  [ -d "$src/janet" ] ||
    git clone https://github.com/janet-lang/janet.git "$src/janet"
  git -C "$src/janet" checkout 86ca958e
  make -C "$src/janet" -j4
  $sudo make -C "$src/janet" install
  # Refresh the loader's cache, so libjanet.so is found at run time.
  $sudo ldconfig
fi

# 3. PostgreSQL 18, whole, as 724edf9 below is in its history; its
#    psql, pg_regress and isolation tester run pgregress and the specs.
if [ ! -x "$pg18/install/bin/psql" ]; then
  step "postgres18"
  [ -d "$pg18" ] ||
    git clone --branch REL_18_STABLE --single-branch \
      https://github.com/postgres/postgres.git "$pg18"
  (cd "$pg18" && ./configure --prefix="$pg18/install")
  make -C "$pg18" -j4
  make -C "$pg18" install
  make -C "$pg18/src/test/isolation" -j4
fi

# 4. PostgreSQL 18's psql and pgbench before Ubuntu's.
line="export PATH=$pg18/install/bin:\$PATH"
if ! grep -qxF "$line" "$HOME/.profile" 2>/dev/null; then
  step "PATH in ~/.profile"
  echo "$line" >> "$HOME/.profile"
fi

# 5. PostgreSQL 18.6 for the benchmark, optimized, with no assertions,
#    from a worktree of the clone above (bench/DESIGN.md section 2).
if [ ! -x "$wisr/bench/build/pg18/bin/postgres" ]; then
  step "bench/build/pg18"
  wt=$src/postgres18-724edf9
  [ -d "$wt" ] || git -C "$pg18" worktree add --detach "$wt" 724edf9
  (cd "$wt" && ./configure --prefix="$wisr/bench/build/pg18")
  make -C "$wt" -j4
  make -C "$wt" install
fi

# 6. Newer than 24.04: GCC 15 builds RocksDB 8.9.1 only with <cstdint>
#    given to it, so build the release build's before make does.
. /etc/os-release
if dpkg --compare-versions "$VERSION_ID" gt 24.04 &&
  [ ! -f "$wisr/rocksdb/release/librocksdb.a" ]; then
  step "rocksdb release"
  CXX="g++ -include cstdint" "$wisr/dg/test/rocksdb.sh" release
fi

step "done"
echo "Log in again, or run: . ~/.profile"
if dpkg --compare-versions "$VERSION_ID" gt 24.04; then
  echo "Release builds here need: export CPATH=$wisr/rocksdb/src/include"
  echo "For make tsan, first: CXX=\"g++ -include cstdint\" dg/test/rocksdb.sh tsan"
fi
echo "Then: DEBUG=1 make -j4 && make -j4 test"
