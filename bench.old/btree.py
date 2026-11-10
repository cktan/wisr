# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# btree.py N -- write q9.sql and q10.sql, N executions each of btree.sh's
# prepared Q9 and Q10: Q9 a range of 100 c2 values from a uniform start,
# Q10 100 c2 values each Zipfian (1.1) over 3 to 1000002, as
# sql/q10.sql's pgbench draws them. Seeded, so every run reads the same.
import bisect
import random
import sys

random.seed(1)
n = int(sys.argv[1])
# Zipf over 1..N by its cumulative weights, a value drawn by bisection.
N, s, lo = 1000000, 1.1, 3
cum, t = [], 0.0
for i in range(1, N + 1):
    t += 1.0 / i ** s
    cum.append(t)


def zipf():
    return lo + bisect.bisect_left(cum, random.random() * t)


with open('q9.sql', 'w') as f:
    for _ in range(n):
        f.write(f"execute q9({random.randint(3, 999900)});\n")
with open('q10.sql', 'w') as f:
    for _ in range(n):
        ks = ','.join(str(zipf()) for _ in range(100))
        f.write(f"execute q10('{{{ks}}}');\n")
