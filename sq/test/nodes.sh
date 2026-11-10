#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# nodes.sh -- verify that include/sq_nodes.h is what the generator
# writes from spec/ today.
# 1. Generate the header again.
# 2. Confirm it is the committed one, byte for byte.
cd "$(dirname "$0")/.." || exit 1
mkdir -p build/test
if ! python3 gen/gen_nodes.py header | diff -u include/sq_nodes.h - \
    > build/test/nodes.diff; then
  echo "nodes: FAILED (include/sq_nodes.h is stale: run make nodes;" \
       "build/test/nodes.diff)"
  exit 1
fi
echo "nodes: PASSED"
