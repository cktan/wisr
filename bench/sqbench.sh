#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# sqbench.sh -- time digres parsing a large SELECT whose WHERE clause
# ends with AND false, so the planner folds the qualification to a
# one-time false filter and neither sm nor a table scan runs.
#
#   bench/sqbench.sh [terms] [seconds]
#
# terms (default 1000) is the number of boolean conjuncts before
# AND false; seconds (default 10) is the pgbench run time. pgbench runs
# in simple query mode (-M simple) so every iteration parses the SQL.
# Set DIGRES to the server binary (default: the optimized build,
# ../dg/build/digres-release). The instance goes under build/, on a
# real disk.
set -euo pipefail
cd "$(dirname "$0")"

terms=${1:-1000}
seconds=${2:-10}
digres=${DIGRES:-../dg/build/digres-release}
pgbin=${PGBIN:-$HOME/p/postgres18/install/bin}
if [ ! -x "$pgbin/pgbench" ]; then
  pgbin=$(dirname "$(command -v pgbench)")
fi

mkdir -p build
work=$PWD/$(mktemp -d build/sqbench.XXXXXX)
trap '
  if [ -n "${server:-}" ]; then
    kill -INT "$server" 2>/dev/null || true
    wait "$server" 2>/dev/null || true
  fi
  rm -rf "$work"
' EXIT

port=$(python3 -c 'import socket; s = socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')

# Generate the large SELECT over a subselect (...) AS t with AND false.
python3 - "$terms" > "$work/sqbench.sql" <<'PY'
import sys
n = int(sys.argv[1])
cols = ["a", "b", "c", "d", "e", "f", "g", "h"]
sub = ", ".join(f"'{c}' AS {c}" for c in cols)
where = " AND ".join(f"{cols[i % len(cols)]} = '{cols[i % len(cols)]}'" for i in range(n)) + " AND false"
print(f"SELECT a, b, c, d, e, f, g, h FROM (SELECT {sub}) AS t WHERE {where};")
PY

"$digres" --init "$work/data" > "$work/initdb.log" 2>&1
"$digres" -D "$work/data" -k "$work" -p "$port" > "$work/server.log" 2>&1 &
server=$!

for _ in $(seq 100); do
  grep -q "ready to accept connections" "$work/server.log" && break
  sleep 0.1
done
if ! grep -q "ready to accept connections" "$work/server.log"; then
  echo "sqbench: server did not start; see $work/server.log" >&2
  exit 1
fi

sql_bytes=$(wc -c < "$work/sqbench.sql")
echo "sqbench: $terms terms ($sql_bytes bytes), ${seconds}s, server=$digres"
"$pgbin/pgbench" -h "$work" -p "$port" -d postgres -n -M simple \
  -c 1 -T "$seconds" -f "$work/sqbench.sql"
