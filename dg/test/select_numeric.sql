-- Verify that numeric, a decimal of at most 36 digits (DECIMAL.md), gives
-- PostgreSQL's results: its text, its scales, its errors.
-- 1. Read literals, text with spaces, base 16 and underscores; a literal
--    past 36 digits is a float8.
-- 2. Add, subtract, multiply and divide, at PostgreSQL's scales.
-- 3. Take remainders and integer quotients, compare across scales, and
--    round, truncate, take ceilings and floors.
-- 4. Take square roots, exponentials, logarithms and powers.
-- 5. Keep 36 digits, round to a typmod, and cast to and from the integers
--    and floats.
-- 6. Take scales, gcd, lcm and a factorial.
-- 7. Sum, average and take deviations of numerics and integers.
-- 8. Make a series and find a bucket.
-- 9. Use numeric in jsonb, jsonpath, extract, to_char, money and pg_lsn.
-- 10. Send values in PostgreSQL's binary format.
-- 11. Add, subtract, multiply, divide, sum, average, round, truncate,
--     take ceilings and floors of, and format numerics of at most 17
--     digits by their own paths, and get what decimal128's give, a
--     product past 17 digits and sums across scales and windows among
--     them.
-- (Errors are server.sh's: a run stops at its first.)
SELECT 1.5, -0.001, 1e3, 1.50, '  12.340  '::numeric, '0x1F'::numeric, '1_000.5'::numeric;
SELECT 1e300, 3.4e38, 1e35;
SELECT 1.5 + 2.25, 1.5 - 2.25, 1.5 * 2.25, 10 / 3.0, 1 / 3::numeric, 2.0 / 3;
SELECT 7.5 % 2, -7.5 % 2, div(7.5, 2), 1.5 = 1.50, 1.5 < 1.51, abs(-2.5), -(2.5);
SELECT round(2.5), round(-2.5), round(1.2345, 2), round(1234.5, -2), trunc(1.999, 2), trunc(-1.999), ceil(1.1), floor(-1.1);
SELECT sqrt(2::numeric), sqrt(16::numeric), exp(1::numeric), ln(10::numeric), log(100::numeric), log(2, 8::numeric);
SELECT power(2::numeric, 10), power(2::numeric, 0.5), power(2.0, -2), 2::numeric ^ 3;
SELECT 999999999999999999999999999999999999::numeric(36,0) + 0, '1.000000000000000000000000000000000000000000'::numeric(36,35);
SELECT 1.23::numeric(5,1), 123.45::numeric(5,2), 0.1::float8::numeric, 1.5::numeric::float8, 2.5::numeric::int4, 3.5::numeric::int8;
SELECT scale(1.230), min_scale(1.230), trim_scale(1.230), gcd(12.0, 18), lcm(1.5, 2.25), factorial(20);
SELECT sum(x), avg(x), min(x), max(x) FROM (VALUES (1.5), (2.25), (3)) v(x);
SELECT sum(x::int8), avg(x::int4), stddev(x::int4), variance(x::numeric) FROM generate_series(1, 10) x;
SELECT x FROM generate_series(0.5, 2, 0.5) x;
SELECT width_bucket(5.35, 0.024, 10.06, 5), sign(-3.2);
SELECT '{"a": 1.5, "b": [1, 2.25, -3]}'::jsonb, '{"a": 1}'::jsonb = '{"a": 1.0}'::jsonb;
SELECT jsonb_path_query('{"x": 2.5}'::jsonb, '$.x * 2 + 1'::jsonpath, '{}'::jsonb, false);
SELECT extract(epoch FROM timestamp '2024-01-01 00:00:01.5'), to_char(1234.5::numeric, '9999.99'), to_char(0.000123::numeric(17,6), '9.99EEEE'), to_number('1,234.5', '9,999.9');
SELECT 12.34::numeric::money, '0/10'::pg_lsn + 5::numeric, '0/20'::pg_lsn - '0/10'::pg_lsn;
SELECT numeric_send(1.5), numeric_send(-0.0001), numeric_send(0::numeric), numeric_send(12345678.9);
SELECT a * b, bool_and((a + b)::text = (a::decimal128 + b::decimal128)::text AND (a - b)::text = (a::decimal128 - b::decimal128)::text AND (a * b)::text = (a::decimal128 * b::decimal128)::text AND (a / b)::text = (a::decimal128 / b::decimal128)::text AND (-a)::text = (-(a::decimal128))::text AND abs(a)::text = abs(a::decimal128)::text) OVER () FROM (VALUES (1.5, 2.25), (-99999999999999.999, 0.001), (12345678901234567::numeric(17,0), -3), (0.00000000000000001, 7), (99999999999999999::numeric(17,0), 99999999999999999::numeric(17,0))) v(a, b);
SELECT sum(x), avg(x), sum(x)::text = sum(x::decimal128)::text, avg(x)::text = avg(x::decimal128)::text FROM (VALUES (1.5), (2.25), (-3), (0.001), (99999999999999999::numeric(17,0))) v(x);
SELECT i, sum(x) OVER w, avg(x) OVER w FROM (VALUES (1, 1.5), (2, 2.25), (3, -3), (4, 0.001)) v(i, x) WINDOW w AS (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW);
SELECT x, round(x, 1), round(x), round(x, -2), trunc(x, 1), trunc(x), trunc(x, -2), ceil(x), floor(x), to_char(x, '999,999.99'), bool_and(ceil(x)::text = ceil(x::decimal128)::text AND ceiling(x)::text = ceiling(x::decimal128)::text AND floor(x)::text = floor(x::decimal128)::text AND round(x, 1)::text = round(x::decimal128, 1)::text AND round(x)::text = round(x::decimal128)::text AND round(x, -2)::text = round(x::decimal128, -2)::text AND trunc(x, 1)::text = trunc(x::decimal128, 1)::text AND trunc(x)::text = trunc(x::decimal128)::text AND trunc(x, -2)::text = trunc(x::decimal128, -2)::text AND to_char(x, '999,999.99') = to_char(x::decimal128, '999,999.99')) OVER () FROM (VALUES (1234.567), (-0.05), (99.95), (-151.25), (-149.5), (149.5), (-7), (9999999999999999.9)) v(x);
