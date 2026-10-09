-- Verify that every kind of literal scans to PostgreSQL's token and value.
-- 1. Strings: plain, continued across a line, escape, Unicode, dollar-quoted.
-- 2. Bit and hexadecimal strings, and a national character string.
-- 3. Integers in each base, with underscores, and one too large for 32 bits.
-- 4. Numerics and reals, and the integer before a '..'.
-- 5. Parameters, quoted identifiers, and an identifier with upper case.
SELECT 'a', 'it''s', 'a'
  'b', E'tab\there\x41\101\u00e9\U0001F600', U&'d\0061t\+000061';
SELECT $$dollar$$, $t$a $b$ c$t$, $t$$u$$t$;
SELECT b'101', x'1F', n'nat';
SELECT 42, 0x2A, 0o52, 0b101010, 1_000_000, 0xFF_FF, 99999999999;
SELECT 1.5, .5, 5., 1e5, 1.5e-3, 1..2;
SELECT $1, $23, "Quoted Id", "with ""quote", U&"d\0061t", MixedCase;
