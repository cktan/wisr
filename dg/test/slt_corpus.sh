#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# slt_corpus.sh -- run SQLite's whole sqllogictest corpus, or part of it,
# on PostgreSQL 18 and on digres, and report the records digres fails that
# PostgreSQL 18 passes (test/slt/README.md).
#
#   test/slt_corpus.sh                 the sample: the first set of files in
#                                      each directory of index/ and random/,
#                                      and evidence/
#   test/slt_corpus.sh PATTERN...      the files under the corpus's test/
#                                      that these sparse-checkout patterns
#                                      name, such as '/test/random/expr/'
#
# The corpus is about 1.2 GB, too much to keep in the repository: it is
# fetched once, from a mirror on GitHub at the commit named below, into
# build/slt-corpus, and only the files the patterns name. A record either
# engine skips, by "skipif" or "onlyif", is not run on it; one PostgreSQL 18
# fails is SQLite's or MySQL's dialect, and is not held against digres.
# What digres fails alone, file:line, is in build/test/slt-corpus/digres-only,
# and the failures behind it in build/test/slt-corpus/digres.failures.
cd "$(dirname "$0")/.." || exit 1
mirror=https://github.com/gregrahn/sqllogictest.git
commit=c93ac416f05635ff5cc490009add936d9753d01d
corpus=$PWD/build/slt-corpus
work=$PWD/build/test/slt-corpus
patterns=("$@")
[ ${#patterns[@]} -gt 0 ] || patterns=('/test/evidence/' '/test/index/*/10/'
  '/test/random/*/slt_good_[0-2].test')

# Fetch the corpus once, with no file's contents until a pattern names it.
if [ ! -d "$corpus/.git" ]; then
  git clone -q --filter=blob:none --no-checkout "$mirror" "$corpus" || {
    echo "slt_corpus: FAILED (could not fetch $mirror)"
    exit 2
  }
fi
git -C "$corpus" sparse-checkout set --no-cone "${patterns[@]}" &&
  git -C "$corpus" checkout -q "$commit" || {
  echo "slt_corpus: FAILED (could not check out $commit)"
  exit 2
}
# The files checked out: those the patterns name, and no others.
mapfile -t files < <(find "$corpus/test" -name '*.test' 2> /dev/null | sort)
if [ ${#files[@]} = 0 ]; then
  echo "slt_corpus: FAILED (no files match ${patterns[*]})"
  exit 2
fi
rm -rf "$work"
mkdir -p "$work"

# Run the files on ENGINE (pg18 or digres), keeping where each failure is,
# in a directory of their own, which no other run of slt.sh clears; a run
# that could not be made, or ran nothing, fails.
run_on() {
  local flag= status
  [ "$1" = pg18 ] && flag=--pg18
  SLT_WORK="$work/$1" test/slt.sh $flag "${files[@]}" > "$work/$1.log" 2>&1
  status=$?
  cp "$work/$1/failures" "$work/$1.failures" 2> /dev/null
  cp "$work/$1/tally" "$work/$1.tally" 2> /dev/null
  if [ $status -gt 1 ] || ! grep -q ' run, ' "$work/$1.tally" 2> /dev/null ||
      [ "$(awk '{ r += $2 } END { print r + 0 }' "$work/$1.tally")" = 0 ]; then
    tail -3 "$work/$1.log"
    echo "slt_corpus: FAILED (the run on $1 did not run; see $work/$1)"
    exit 2
  fi
  grep -oE '^/[^ ]*\.test:[0-9]+: ' "$work/$1.failures" |
    sed "s|^$corpus/||; s/: $//" | sort > "$work/$1.locations"
  tail -1 "$work/$1.log"
}
run_on pg18
run_on digres

comm -13 "$work/pg18.locations" "$work/digres.locations" > "$work/digres-only"
n=$(wc -l < "$work/digres-only")
if [ "$n" = 0 ]; then
  echo "slt_corpus: PASSED (${#files[@]} files; digres fails nothing PostgreSQL 18 passes)"
  exit 0
fi
cut -d: -f1 "$work/digres-only" | sort | uniq -c | sort -rn | head -20
echo "slt_corpus: FAILED ($n records digres fails and PostgreSQL 18 passes; see $work/digres-only)"
exit 1
