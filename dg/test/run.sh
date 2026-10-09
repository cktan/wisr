#!/bin/bash
# run.sh -- run each test/NAME.sql with "digres --parse --run", which
# starts no server, and compare what it prints with
# test/expected/NAME.out.
#
#   test/run.sh            run the tests
#   test/run.sh --accept   write what is printed now as expected
#
# What differs is left in build/test/NAME.out and NAME.diff.
cd "$(dirname "$0")/.." || exit 1
mkdir -p build/test
failed=0
count=0
for sql in test/*.sql; do
  name=$(basename "$sql" .sql)
  build/digres --parse --run < "$sql" > "build/test/$name.out" 2>&1
  count=$((count + 1))
  if [ "$1" = "--accept" ]; then
    cp "build/test/$name.out" "test/expected/$name.out"
  elif ! diff -u "test/expected/$name.out" "build/test/$name.out" \
      > "build/test/$name.diff" 2>&1; then
    echo "$name: FAILED (build/test/$name.diff)"
    failed=1
  fi
done
if [ "$1" = "--accept" ]; then
  echo "run: accepted $count files as expected"
elif [ $failed = 0 ]; then
  echo "run: PASSED ($count files)"
fi
exit $failed
