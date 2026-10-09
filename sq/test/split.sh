#!/bin/bash
# split.sh -- verify that the oracle's --split writes each statement
# as a file that parses, alone, to that statement's tree.
# 1. Split test/select.sql into build/test/split.
# 2. Confirm there is one pair of files a statement.
# 3. Confirm the trees, in order, are the expected output.
# 4. Confirm each statement's file parses to its own tree.
cd "$(dirname "$0")/.." || exit 1
dir=build/test/split
rm -rf $dir && mkdir -p $dir

build/oracle --split $dir < test/select.sql > /dev/null || exit 1

want=$(wc -l < test/expected/select.out)
if [ "$(ls $dir/*.sql | wc -l)" != "$want" ] ||
   [ "$(ls $dir/*.out | wc -l)" != "$want" ]; then
  echo "split: FAILED (expected $want statements in $dir)"
  exit 1
fi

if ! cat $dir/*.out | diff -u test/expected/select.out - > $dir/diff; then
  echo "split: FAILED ($dir/diff)"
  exit 1
fi

for sql in $dir/*.sql; do
  if ! build/oracle < "$sql" | diff -u "${sql%.sql}.out" - > $dir/diff; then
    echo "split: FAILED ($sql, $dir/diff)"
    exit 1
  fi
done
echo "split: PASSED"
