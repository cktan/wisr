-- Verify that a setting is set, shown and reset by its utility statements
-- with no server, each session its own, as in 8 threads at once.
-- 1. SET extra_float_digits to 0 rounds a float8 to 15 digits.
-- 2. SHOW gives the setting.
-- 3. RESET puts it back: the shortest exact digits again.
-- 4. SET lc_numeric to C is taken; to a locale that does not exist, refused.
SET extra_float_digits = 0;
SELECT 1.0::float8 / 3;
SHOW extra_float_digits;
RESET extra_float_digits;
SELECT 1.0::float8 / 3;
SET lc_numeric = 'C';
SHOW lc_numeric;
SET lc_numeric = 'xx_YY';
