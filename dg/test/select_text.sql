-- Verify that text is compared, cased, matched and concatenated with no
-- server: names are looked up through the search path, which holds the
-- built-in schemas only, and the default collation is C.
-- 1. Compare two strings, and upper-case one.
-- 2. Match a regular expression and a LIKE pattern.
-- 3. Concatenate text and an integer, which casts the integer to text.
-- 4. Cast to a type named by the SQL standard, found in pg_catalog.
-- 5. Compare the current user with the session user: one role, named.
-- 6. Take what SUBSTRING ... SIMILAR matched, and its null where the
--    pattern does not match.
SELECT 'a' < 'b', 'b' < 'a', upper('abc');
SELECT 'foobar' ~ 'o.b', 'foobar' LIKE 'f%';
SELECT 'x' || 1;
SELECT 1.5::numeric, 'abc'::varchar(2);
SELECT current_user = session_user;
SELECT substring('foobar' SIMILAR '%#"o_b#"%' ESCAPE '#'),
  substring('foobar' SIMILAR '#"o_b#"%' ESCAPE '#') IS NULL;
