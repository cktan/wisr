-- Verify that a numeric of at most 17 digits is a decimal64, by value, and
-- that a client sees both decimals as PostgreSQL's numeric
-- (DG_DECIMAL_PLAN.md).
-- 1. A literal is a decimal64 to 17 digits, a decimal128 past them, and a
--    float8 with an exponent; a bare numeric is decimal64(17,3).
-- 2. numeric and decimal name decimal64 to 17 digits, decimal128 past
--    them, and each is named numeric(p,s) again.
-- 3. A client is told oid 1700, and 1231 for an array, for either decimal.
-- 4. A decimal64 hashes as a decimal128 of its value does, at any scale;
--    a hash join of a decimal64 and a decimal128 key, a GROUP BY, a UNION
--    and min and max of decimal64s treat values equal at any scale alike.
-- 5. decimal64's binary form is numeric's.
-- (Errors are server.sh's: a run stops at its first.)
SELECT pg_typeof(1.5)::oid, pg_typeof(0.00000000000000001)::oid, pg_typeof(123456789012345678.5)::oid, pg_typeof(1e3)::oid, 1.23456::numeric, pg_typeof(1.5::numeric(18,2))::oid;
SELECT 'numeric'::regtype::oid, 'decimal(5)'::regtype::oid, 'numeric(17,2)'::regtype::oid, 'numeric(18,2)'::regtype::oid, format_type(1700, ((10 << 16) | 2) + 4), format_type(8383, ((30 << 16) | 5) + 4), format_type(1700, -1);
SELECT 1.5::numeric(5,2) AS a, 1.5::numeric(30,2) AS b, 1.5 * 2 AS c, ARRAY[1.5] AS d, ARRAY[1.5::numeric(30,2)] AS e \gdesc
SELECT hash_decimal64(1.5) = hash_numeric(1.50::decimal128), hash_decimal64(-0.0) = hash_numeric(0::decimal128), hash_decimal64_extended(12.300, 7) = hash_numeric_extended(12.3::decimal128, 7), hash_decimal64(1.5) <> hash_decimal64(1.6);
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM (VALUES (1.5), (3.0), (4.50)) a(x) JOIN (VALUES (1.50::numeric(30,5)), (4.5::numeric(30,5)), (7::numeric(30,5))) b(y) ON a.x = b.y;
SELECT count(*) FROM (VALUES (1.5), (3.0), (4.50)) a(x) JOIN (VALUES (1.50::numeric(30,5)), (4.5::numeric(30,5)), (7::numeric(30,5))) b(y) ON a.x = b.y;
RESET enable_mergejoin;
RESET enable_nestloop;
SELECT x, count(*) FROM (VALUES (1.5), (1.50), (2.25), (1.500)) v(x) GROUP BY x ORDER BY x;
SELECT x FROM (VALUES (1.5), (2.25)) a(x) UNION SELECT y FROM (VALUES (1.50), (3.0)) b(y) ORDER BY 1;
SELECT min(x), max(x), min(x)::text = min(x::decimal128)::text, max(x)::text = max(x::decimal128)::text FROM (VALUES (1.5), (-2.25), (3)) v(x);
SELECT decimal64_send(1.5), decimal64_send(1.5) = numeric_send(1.5::decimal128), decimal64_send(-0.0001) = numeric_send((-0.0001)::decimal128), decimal64_send(0) = numeric_send(0::decimal128), decimal64_send(99999999999999999::numeric(17,0)) = numeric_send(99999999999999999::decimal128);
