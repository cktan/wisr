#!/bin/bash
# spec.sh -- verify that spec/ and the generator agree with PostgreSQL
# as it is in ../dg (gen/check_pg.py).
# 1. Confirm spec/'s headers are ../dg's.
# 2. Confirm spec/nodes.list is what the grammar can make.
# 3. Confirm the generated printer's fields are PostgreSQL's.
cd "$(dirname "$0")/.." || exit 1
if ! python3 gen/check_pg.py ../dg/src; then
  echo "spec: FAILED"
  exit 1
fi
echo "spec: PASSED"
