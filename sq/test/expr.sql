-- Verify that expressions parse with PostgreSQL's precedence and its
-- special forms.
-- 1. Arithmetic precedence and parentheses.
-- 2. Boolean operators, IS NULL, BETWEEN, IN, LIKE.
-- 3. A cast in each of its three spellings.
-- 4. CASE, COALESCE and NULLIF.
-- 5. An array constructor, a subscript and a row.
-- 6. Constants: integer, numeric, string, escape string, bit, boolean.
-- 7. A string and an identifier with characters outside ASCII.
-- 8. A user-defined operator, and a parameter.
SELECT 1 + 2 * 3 - (4 - 5) / 6 % 7, -a ^ 2;
SELECT a AND NOT b OR c, x IS NULL, y IS NOT TRUE, z BETWEEN 1 AND 2, w IN (1, 2), s LIKE 'a%' ESCAPE '\';
SELECT CAST(a AS int), b::numeric(10, 2), int8 '5';
SELECT CASE WHEN a > 0 THEN 'pos' WHEN a < 0 THEN 'neg' ELSE 'zero' END, COALESCE(a, b, 0), NULLIF(a, 0);
SELECT ARRAY[1, 2, 3], arr[1], arr[2:3], ROW(1, 'a'), (c).f;
SELECT 42, 3.14, 1e10, 'it''s', E'tab\there', B'1010', X'1F', TRUE, NULL;
SELECT 'héllo wörld €', "naïve" FROM "tàble";
SELECT a <-> b, a @> b, $1 + $2;
