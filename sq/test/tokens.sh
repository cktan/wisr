#!/bin/bash
# tokens.sh -- verify that sq's scanner and PostgreSQL's find the same
# tokens in the same text, and refuse the same text the same way.
#
#   test/tokens.sh DIR     DIR holds *.sql
#
# 1. Scan each file with build/sq_tokens and with `build/oracle --tokens`.
# 2. Confirm the two print the same: every token's number, where it
#    begins and its value, or the error, its position, detail and hint.
#
# A file need not parse: the scanner reads any text until it meets
# something that is no token.
cd "$(dirname "$0")/.." || exit 1
src=${1:?usage: test/tokens.sh DIR}
out=build/test/tokens
rm -rf $out && mkdir -p $out
files=0
failed=0
for sql in "$src"/*.sql; do
  name=$(basename "$sql" .sql)
  build/oracle --tokens < "$sql" > "$out/$name.want" 2> /dev/null
  build/sq_tokens < "$sql" > "$out/$name.got" 2>&1
  files=$((files + 1))
  if ! cmp -s "$out/$name.want" "$out/$name.got"; then
    echo "tokens: $name differs ($out/$name.want, $out/$name.got)"
    failed=$((failed + 1))
  fi
done
tokens=$(cat $out/*.got | wc -l)
if [ $failed != 0 ]; then
  echo "tokens: FAILED ($failed of $files files differ)"
  exit 1
fi
echo "tokens: PASSED ($files files, $tokens lines)"
