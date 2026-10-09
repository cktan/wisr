#!/bin/bash
# trees.sh -- verify that sq parses each statement to the tree
# PostgreSQL's parser builds for it, or refuses it the same way.
#
#   test/trees.sh DIR          DIR holds *.sql: compare with the oracle
#   test/trees.sh --corpus     every statement of build/corpus, against
#                              the tree `make corpus` wrote beside it
#   test/trees.sh --chunks     every piece of build/chunks, which
#                              test/chunks.py cut: compare with the oracle
#
# 1. Parse each file with build/sq_parse.
# 2. Confirm it prints what the oracle prints: one tree a statement in
#    PostgreSQL's nodeToString() text, or the error with its position,
#    detail and hint.
cd "$(dirname "$0")/.." || exit 1
out=build/test/trees
rm -rf $out && mkdir -p $out
files=0
failed=0
if [ "$1" = "--corpus" ]; then
  for sql in build/corpus/*/*.sql; do
    files=$((files + 1))
    if ! build/sq_parse < "$sql" 2>&1 | cmp -s - "${sql%.sql}.out"; then
      failed=$((failed + 1))
      echo "$sql" >> $out/failed
    fi
  done
elif [ "$1" = "--chunks" ]; then
  for sql in build/chunks/*/*.sql; do
    files=$((files + 1))
    if ! build/sq_parse < "$sql" 2>&1 |
        cmp -s - <(build/oracle < "$sql" 2> /dev/null); then
      failed=$((failed + 1))
      echo "$sql" >> $out/failed
    fi
  done
else
  src=${1:?usage: test/trees.sh DIR | --corpus | --chunks}
  for sql in "$src"/*.sql; do
    name=$(basename "$sql" .sql)
    files=$((files + 1))
    build/oracle < "$sql" > "$out/$name.want" 2> /dev/null
    build/sq_parse < "$sql" > "$out/$name.got" 2>&1
    if ! cmp -s "$out/$name.want" "$out/$name.got"; then
      echo "trees: $name differs ($out/$name.want, $out/$name.got)"
      failed=$((failed + 1))
    fi
  done
fi
if [ $failed != 0 ]; then
  echo "trees: FAILED ($failed of $files differ; see $out)"
  exit 1
fi
echo "trees: PASSED ($files files)"
