#!/bin/bash
# alone.sh -- verify that the library and its tests are built from
# nothing outside sq: not digres's, and not sm's (PLAN.md step 1).
# 1. Read the compiler's dependency files for src/ and test/.
# 2. Confirm no file under ../ is named in them.
cd "$(dirname "$0")/.." || exit 1
if grep -l '\.\./' build/src/*.d build/test/*.d; then
  echo "alone: FAILED (the files above depend on files outside sq)"
  exit 1
fi
echo "alone: PASSED"
