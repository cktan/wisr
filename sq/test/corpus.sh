#!/bin/bash
# corpus.sh -- split a directory of SQL files into statements with the
# oracle, as build/corpus/FILE/NNNNN.sql and .out (PLAN.md step 0).
#
#   test/corpus.sh DIR     DIR holds *.sql, such as PostgreSQL's
#                          src/test/regress/sql
#
# A file the oracle cannot parse whole is left out: PostgreSQL's parser
# returns nothing for a text with an error in it, and so cannot say
# where that text's statements begin and end.
cd "$(dirname "$0")/.." || exit 1
src=${1:?usage: test/corpus.sh DIR}
rm -rf build/corpus && mkdir -p build/corpus
files=0
skipped=0
for sql in "$src"/*.sql; do
  dir=build/corpus/$(basename "$sql" .sql)
  mkdir "$dir"
  if build/oracle --split "$dir" < "$sql" > /dev/null 2>&1; then
    files=$((files + 1))
  else
    rm -rf "$dir"
    skipped=$((skipped + 1))
  fi
done
stmts=$(find build/corpus -name '*.sql' | wc -l)
echo "corpus: $stmts statements from $files files; $skipped files skipped"
