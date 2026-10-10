#!/bin/bash
# run.sh -- run each test/NAME.sql with "digres --parse --run", which
# starts no server, and compare what it prints with
# test/expected/NAME.out. Each is run again in 8 threads at once, each
# thread a session, and must print the same.
#
#   test/run.sh            run the tests
#   test/run.sh --accept   write what is printed now as expected
#
# DIGRES names the program to run, build/digres if it is not set: "make
# tsan" sets it to the build under the thread sanitizer.
#
# What differs is left in build/test/NAME.out and NAME.diff, and in
# NAME.threads.out and NAME.threads.diff.
cd "$(dirname "$0")/.." || exit 1
digres=${DIGRES:-build/digres}
mkdir -p build/test
failed=0
count=0

# Report test NAME as failed if build/test/OUT.out, what it printed,
# differs from what it is expected to print. The arguments: NAME OUT.
check() {
  if ! diff -u "test/expected/$1.out" "build/test/$2.out" \
      > "build/test/$2.diff" 2>&1; then
    echo "$1: FAILED (build/test/$2.diff)"
    failed=1
  fi
}

for sql in test/*.sql; do
  name=$(basename "$sql" .sql)
  $digres --parse --run < "$sql" > "build/test/$name.out" 2>&1
  count=$((count + 1))
  if [ "$1" = "--accept" ]; then
    cp "build/test/$name.out" "test/expected/$name.out"
    continue
  fi
  check "$name" "$name"
  $digres --parse --run --threads 8 < "$sql" \
    > "build/test/$name.threads.out" 2>&1
  check "$name" "$name.threads"
done
# Verify that a FATAL ends its session's thread, not the process.
# 1. Run select_error.sql in 8 threads with exit_on_error set, which
#    makes its ERROR a FATAL in every session.
# 2. Confirm each session reported the FATAL, and nothing else was
#    reported.
# 3. Confirm the process lived to print what the sessions printed before
#    it, all the same, and exited with their code, 1.
if [ "$1" != "--accept" ]; then
  $digres --parse --run -c exit_on_error=on --threads 8 \
    < test/select_error.sql > build/test/fatal.out 2> build/test/fatal.err
  code=$?
  if [ $code != 1 ] ||
      [ "$(grep -c 'FATAL:  division by zero' build/test/fatal.err)" != 8 ] ||
      [ "$(grep -vc 'FATAL:  division by zero' build/test/fatal.err)" != 0 ] ||
      ! grep -q '"42"' build/test/fatal.out ||
      grep -q "printed otherwise" build/test/fatal.out; then
    echo "fatal: FAILED (build/test/fatal.out, fatal.err)"
    failed=1
  fi
fi

if [ "$1" = "--accept" ]; then
  echo "run: accepted $count files as expected"
elif [ $failed = 0 ]; then
  echo "run: PASSED ($count files, alone and in 8 threads)"
fi
exit $failed
