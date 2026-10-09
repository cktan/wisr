#!/bin/bash
# run.sh -- run every test/*.sql through build/oracle and compare
# what it prints with test/expected/NAME.out.
#
#   test/run.sh            run the tests
#   test/run.sh --accept   write what is printed now as expected
#
# What differs is left in build/test/NAME.out and NAME.diff.
cd "$(dirname "$0")/.." || exit 1
mkdir -p build/test
failed=0
for sql in test/*.sql; do
  name=$(basename "$sql" .sql)
  build/oracle < "$sql" > "build/test/$name.out" 2>&1
  if [ "$1" = "--accept" ]; then
    cp "build/test/$name.out" "test/expected/$name.out"
    echo "$name: accepted"
  elif diff -u "test/expected/$name.out" "build/test/$name.out" \
      > "build/test/$name.diff" 2>&1; then
    echo "$name: PASSED"
  else
    echo "$name: FAILED (build/test/$name.diff)"
    failed=1
  fi
done
[ $failed = 0 ] && echo "All sq tests passed."
exit $failed
