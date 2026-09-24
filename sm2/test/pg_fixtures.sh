#!/bin/bash
#
# pg_fixtures.sh -- regenerate test/pg_fixtures.h from PostgreSQL
# (PLAN.md, "Testing").
#
# For each type, a one-column table holds the fixture values, and
# pageinspect reads back the bytes PostgreSQL stored for each. The
# header pairs every value's text with those bytes in hex. It needs
# a running PostgreSQL server with pageinspect and superuser
# rights; the build never runs it.
#
#   test/pg_fixtures.sh > test/pg_fixtures.h
#
# PGHOST, PGPORT and PGUSER pick the server; the defaults are the
# development server described in PLAN.md.
set -euo pipefail

export PGHOST=${PGHOST:-$HOME/pg18}
export PGPORT=${PGPORT:-5418}
export PGUSER=${PGUSER:-postgres}
# The canonical text: ISO dates, timestamptz in UTC, ISO 8601
# intervals (sm2.h).
export PGOPTIONS="-c DateStyle=ISO -c TimeZone=UTC -c IntervalStyle=iso_8601"
PSQL=${PSQL:-$HOME/pg18/bin/psql}
DB=wisr_fixtures

# Each type: its frozen code (TYPE.md §3), its PostgreSQL name,
# and its fixture values as SQL literals.
TYPES=(
  "1|bool|true, false"
  "2|int2|-32768, -32767, -256, -1, 0, 1, 255, 256, 32766, 32767"
  "3|int4|-2147483648, -2147483647, -65536, -1, 0, 1, 65535, 65536, 2147483646, 2147483647"
  "4|int8|-9223372036854775808, -9223372036854775807, -4294967296, -1, 0, 1, 4294967295, 4294967296, 9223372036854775806, 9223372036854775807"
  "5|float4|'NaN', 'Infinity', '-Infinity', '0', '-0', 1, -1, 0.1, 0.5, 1.5, 100, 3.1415927, 0.001, 0.0001, 1e-05, 123456, 1234567, 1e+06, 1.1754944e-38, 1e-45, 3.4028235e+38, -3.4028235e+38"
  "6|float8|'NaN', 'Infinity', '-Infinity', '0', '-0', 1, -1, 0.1, 0.5, 1.5, 100, 3.141592653589793, 0.30000000000000004, 0.001, 0.0001, 1e-05, 123456789012345, 1234567890123456, 100000000000000, 1e+15, 1e+100, 1e-100, 2.2250738585072014e-308, 5e-324, 1.7976931348623157e+308, -1.7976931348623157e+308"
)

# random_floats BITS: seeded random float4 (32) or float8 (64)
# values as SQL literals, appended to that type's fixtures so that
# PostgreSQL's float printing is checked well beyond the chosen
# values. Two thousand are any finite bit pattern, and five
# hundred are integers just past the width's exact range, where
# PostgreSQL does not print the shortest decimal. Python's repr of
# the exact value reads back as the same float.
random_floats() {
  python3 - "$1" <<'PY'
import math, random, struct, sys
bits = int(sys.argv[1])
fmt, top = ("<f", 24) if bits == 32 else ("<d", 53)
rng = random.Random(20260924)
out = []
while len(out) < 2000:
    v = struct.unpack(fmt, rng.getrandbits(bits).to_bytes(bits // 8, "little"))[0]
    if math.isfinite(v):
        out.append(repr(v))
for _ in range(500):
    v = rng.randrange(2**(top + 1), 2**(top + 4)) * rng.choice((1, -1))
    out.append(repr(struct.unpack(fmt, struct.pack(fmt, v))[0]))
print(", ".join(out))
PY
}
TYPES[4]+=", $(random_floats 32)"
TYPES[5]+=", $(random_floats 64)"

TYPES+=(
  "11|date|'-infinity', 'infinity', '2000-01-01', '1999-12-31', '2000-01-02', '1970-01-01', '2024-02-29', '0001-01-01', '0001-12-31 BC', '4713-11-24 BC', '4714-11-24 BC', '0999-12-31', '9999-12-31', '10000-01-01', '5874897-12-31'"
  "12|time|'00:00:00', '24:00:00', '23:59:59.999999', '00:00:00.000001', '12:34:56.5', '12:00:00.1', '01:02:03.04'"
  "13|timestamp|'-infinity', 'infinity', '2000-01-01 00:00:00', '1999-12-31 23:59:59.999999', '1970-01-01 00:00:00', '2024-02-29 12:34:56.789', '0001-01-01 00:00:00', '0001-12-31 23:59:59 BC', '4713-11-24 00:00:00 BC', '4714-11-24 00:00:00 BC', '294276-12-31 23:59:59.999999'"
  "14|timestamptz|'-infinity', 'infinity', '2000-01-01 00:00:00+00', '1999-12-31 23:59:59.999999+00', '1970-01-01 00:00:00+00', '2024-02-29 12:34:56.789+00', '0001-01-01 00:00:00+00', '0001-12-31 23:59:59+00 BC', '4713-11-24 00:00:00+00 BC', '4714-11-24 00:00:00+00 BC', '294276-12-31 23:59:59.999999+00'"
)

# random_intervals: 500 seeded random intervals as SQL literals
# in PostgreSQL's verbose input form, so the ISO 8601 text in the
# header is PostgreSQL's own printing: half small, half anywhere
# in the components' ranges.
random_intervals() {
  python3 - <<'PY'
import random
rng = random.Random(20260924)
out = []
for i in range(500):
    if i % 2 == 0:
        m, d = rng.randint(-1200, 1200), rng.randint(-1000, 1000)
        h, us = rng.randint(-100000, 100000), rng.randint(-3599999999, 3599999999)
    else:
        m, d = rng.randint(-2**31 + 1, 2**31 - 1), rng.randint(-2**31 + 1, 2**31 - 1)
        h, us = rng.randint(-2562047787, 2562047787), rng.randint(-3599999999, 3599999999)
    out.append("'%d months %d days %d hours %d microseconds'" % (m, d, h, us))
print(", ".join(out))
PY
}
TYPES+=(
  "15|interval|'infinity', '-infinity', 'PT0S', '1 mon', '30 days', '1 year 2 months 3 days 04:05:06.789', 'PT-0.5S', 'P-1Y2M', '-1 day +02:00', '-00:00:01.000001', 'P178956970Y7M', 'P-178956970Y-8M', 'P2147483647D', 'P-2147483648D', 'PT2562047788H54.775807S', 'PT-2562047788H-54.775808S', 'P178956970Y7M2147483647DT2562047788H54.775806S', $(random_intervals)"
)

# Seeded random values PostgreSQL itself makes, 500 per type: half
# within a few thousand years of 2000, half anywhere in range.
us="' microseconds')::interval"
declare -A RANDOM_ROWS=(
  [date]="SELECT DATE '2000-01-01' + CASE WHEN g % 2 = 0
            THEN floor(random() * 4903090) - 2451545
            ELSE floor(random() * 2147483493) - 2451545 END::int
          FROM generate_series(1, 500) g"
  [time]="SELECT TIME '00:00' + (floor(random() * 86400000000)::bigint || $us
          FROM generate_series(1, 500)"
  [timestamp]="SELECT TIMESTAMP '2000-01-01' + (CASE WHEN g % 2 = 0
            THEN floor((random() * 2 - 1) * 1e17)
            ELSE floor(random() * 9.4e18) - 2.1e17 END::bigint || $us
          FROM generate_series(1, 500) g"
  [timestamptz]="SELECT TIMESTAMPTZ '2000-01-01 00:00:00+00' + (CASE WHEN g % 2 = 0
            THEN floor((random() * 2 - 1) * 1e17)
            ELSE floor(random() * 9.4e18) - 2.1e17 END::bigint || $us
          FROM generate_series(1, 500) g"
)

# A scratch database, made fresh for this run.
"$PSQL" -q -d postgres -c "SET client_min_messages = warning" \
  -c "DROP DATABASE IF EXISTS $DB" \
  -c "CREATE DATABASE $DB"
q() { "$PSQL" -q -X -At -F '|' -d $DB -v ON_ERROR_STOP=1 "$@"; }
q -c "CREATE EXTENSION pageinspect"

version=$(q -c "SHOW server_version")
cat <<HDR
/*
 * pg_fixtures.h -- generated by test/pg_fixtures.sh; do not edit.
 *
 * PostgreSQL $version, $(date +%Y-%m-%d). Each fixture is a
 * value's canonical text and the bytes PostgreSQL stored for it,
 * in hex. The bytes hold on little-endian hosts only.
 */
#ifndef SM2_TEST_PG_FIXTURES_H
#define SM2_TEST_PG_FIXTURES_H

#include <cstdint>

struct pg_fixture_t {
  uint8_t code;
  const char *text;
  const char *hex;
};

inline const pg_fixture_t pg_fixtures[] = {
HDR

for t in "${TYPES[@]}"; do
  IFS='|' read -r code name values <<<"$t"
  # One table per type, one row per value, in insertion order.
  q -c "CREATE TABLE fx_$name (c $name)" \
    -c "INSERT INTO fx_$name VALUES ($(sed 's/, /), (/g' <<<"$values"))"
  # Then the random ones, if the type has any.
  if [[ -n "${RANDOM_ROWS[$name]:-}" ]]; then
    q -c "SELECT setseed(0.20260924)" \
      -c "INSERT INTO fx_$name ${RANDOM_ROWS[$name]}"
  fi
  # Pair each row's text with its tuple's data bytes. The column
  # itself, not c::text, so that the text is the type's output
  # function's: a bool prints "t", but its cast to text is "true".
  q -c "SELECT f.c, encode(h.t_data, 'hex')
          FROM generate_series(0, pg_relation_size('fx_$name') / 8192 - 1) p
         CROSS JOIN heap_page_items(get_raw_page('fx_$name', p::int)) h
          JOIN fx_$name f
            ON f.ctid = ('(' || p || ',' || h.lp || ')')::tid
         ORDER BY p, h.lp" |
    while IFS='|' read -r text hex; do
      printf '    {%s, "%s", "%s"},\n' "$code" "$text" "$hex"
    done
done

cat <<'TAIL'
};

#endif /* SM2_TEST_PG_FIXTURES_H */
TAIL

# Leave nothing behind.
"$PSQL" -q -d postgres -c "DROP DATABASE $DB"
