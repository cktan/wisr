#!/bin/bash
# run.sh -- verify that sq scans and parses the SQL files here as
# PostgreSQL does.
#
#   test/run.sh            run the tests
#   test/run.sh --accept   write what is printed now as expected
#
# 1. Parse each DIR/NAME.sql with build/sq_parse, and confirm it prints
#    DIR/expected/NAME.out: one tree a statement in PostgreSQL's
#    nodeToString() text, or the error with its position, detail and
#    hint. The files of test/types are each parsed as one type name.
# 2. Parse each file of test and test/scan again and print each
#    statement whole, with every location shown, and confirm it prints
#    DIR/expected/NAME.loc.
# 3. Scan each file of test and test/scan with build/sq_tokens, and
#    confirm it prints DIR/expected/NAME.tok: every token's number,
#    where it begins and its value, or the error.
#
# The expected files are PostgreSQL's own answers: each was written by
# its parser and scanner, before sq stopped building them beside its
# own (PLAN.md step 6); the .loc files by its parser as it is in this
# repository's history, built again for the purpose. --accept replaces them with sq's, which is to
# say with nothing to check sq against: read the difference first.
#
# SQ_PARSE names another program to parse with, in place of
# build/sq_parse: one that prints what it does, and takes --type-name.
# digres checks its own parser this way, which is sq, building
# digres's nodes with nothing converted.
# Step 3 is then left out: it is sq's scanner alone, and needs sq's
# own tools built.
#
# What differs is left in build/test/DIR/NAME.out or .tok, and .diff.
cd "$(dirname "$0")/.." || exit 1
parse=${SQ_PARSE:-build/sq_parse}
failed=0
count=0

# check DIR EXT TOOL...: run TOOL over DIR/*.sql against expected/*.EXT
check() {
  local dir=$1 ext=$2
  shift 2
  mkdir -p "build/$dir"
  for sql in "$dir"/*.sql; do
    local name
    name=$(basename "$sql" .sql)
    local got="build/$dir/$name.$ext" want="$dir/expected/$name.$ext"
    "$@" < "$sql" > "$got" 2>&1
    count=$((count + 1))
    if [ "$accept" = 1 ]; then
      cp "$got" "$want"
    elif ! diff -u "$want" "$got" > "$got.diff" 2>&1; then
      echo "$dir/$name.$ext: FAILED ($got.diff)"
      failed=1
    fi
  done
}

[ "$1" = "--accept" ] && accept=1
check test out $parse
check test/scan out $parse
check test/errors out $parse
check test/types out $parse --type-name
check test loc $parse --locations
check test/scan loc $parse --locations
if [ -z "$SQ_PARSE" ]; then
  check test tok build/sq_tokens
  check test/scan tok build/sq_tokens
fi

if [ "$accept" = 1 ]; then
  echo "run: accepted $count files as expected"
elif [ $failed = 0 ]; then
  echo "run: PASSED ($count files)"
fi
exit $failed
