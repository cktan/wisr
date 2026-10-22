/*
 * This test is for collations and character operations when using the
 * builtin provider with the C.UTF-8 locale.
 */

/* skip test if not UTF8 server encoding */
SELECT getdatabaseencoding() <> 'UTF8' AS skip_test \gset
\if :skip_test
\quit
\endif

SET client_encoding TO UTF8;

--
-- Test builtin "C"
--
--dg-skip: relation pg_collation cannot be written: digres has no catalog tables
--dg CREATE COLLATION regress_builtin_c (
--dg   provider = builtin, locale = 'C');

-- non-ASCII characters are unchanged
--dg-cascade
--dg SELECT LOWER(U&'\00C1' COLLATE regress_builtin_c) = U&'\00C1';
--dg-cascade
--dg SELECT UPPER(U&'\00E1' COLLATE regress_builtin_c) = U&'\00E1';

-- non-ASCII characters are not alphabetic
--dg-cascade
--dg SELECT U&'\00C1\00E1' !~ '[[:alpha:]]' COLLATE regress_builtin_c;

--dg-cascade
--dg DROP COLLATION regress_builtin_c;

--
-- Test PG_C_UTF8
--

CREATE COLLATION regress_pg_c_utf8 (
  provider = builtin, locale = 'C_UTF8'); -- fails
--dg-skip: relation pg_collation cannot be written: digres has no catalog tables
--dg CREATE COLLATION regress_pg_c_utf8 (
--dg   provider = builtin, locale = 'C.UTF8');
--dg-cascade
--dg DROP COLLATION regress_pg_c_utf8;
--dg-skip: relation pg_collation cannot be written: digres has no catalog tables
--dg CREATE COLLATION regress_pg_c_utf8 (
--dg   provider = builtin, locale = 'C.UTF-8');

--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE test_pg_c_utf8 (
--dg   t TEXT COLLATE PG_C_UTF8
--dg );
--dg-cascade
--dg INSERT INTO test_pg_c_utf8 VALUES
--dg   ('abc DEF 123abc'),
--dg   ('ábc sßs ßss DÉF'),
--dg   ('ǄxxǄ ǆxxǅ ǅxxǆ'),
--dg   (U&'Λλ 1a \FF11a'),
--dg   ('ȺȺȺ'),
--dg   ('ⱥⱥⱥ'),
--dg   ('ⱥȺ');

--dg-cascade
--dg SELECT
--dg     t, lower(t), initcap(t), upper(t),
--dg     length(convert_to(t, 'UTF8')) AS t_bytes,
--dg     length(convert_to(lower(t), 'UTF8')) AS lower_t_bytes,
--dg     length(convert_to(initcap(t), 'UTF8')) AS initcap_t_bytes,
--dg     length(convert_to(upper(t), 'UTF8')) AS upper_t_bytes
--dg   FROM test_pg_c_utf8;

--dg-cascade
--dg DROP TABLE test_pg_c_utf8;

-- negative test: Final_Sigma not used for builtin locale C.UTF-8
SELECT lower('ΑΣ' COLLATE PG_C_UTF8);
SELECT lower('ΑͺΣͺ' COLLATE PG_C_UTF8);
SELECT lower('Α΄Σ΄' COLLATE PG_C_UTF8);

-- properties

SELECT 'xyz' ~ '[[:alnum:]]' COLLATE PG_C_UTF8;
SELECT 'xyz' !~ '[[:upper:]]' COLLATE PG_C_UTF8;
SELECT '@' !~ '[[:alnum:]]' COLLATE PG_C_UTF8;
SELECT '=' ~ '[[:punct:]]' COLLATE PG_C_UTF8; -- symbols are punctuation in posix
SELECT 'a8a' ~ '[[:digit:]]' COLLATE PG_C_UTF8;
SELECT '൧' !~ '\d' COLLATE PG_C_UTF8; -- only 0-9 considered digits in posix

-- case mapping

SELECT 'xYz' ~* 'XyZ' COLLATE PG_C_UTF8;
SELECT 'xAb' ~* '[W-Y]' COLLATE PG_C_UTF8;
SELECT 'xAb' !~* '[c-d]' COLLATE PG_C_UTF8;
SELECT 'Δ' ~* '[γ-λ]' COLLATE PG_C_UTF8;
SELECT 'δ' ~* '[Γ-Λ]' COLLATE PG_C_UTF8; -- same as above with cases reversed

-- case folding
select casefold('AbCd 123 #$% ıiIİ ẞ ß Ǆǅǆ Σσς' collate PG_C_UTF8);

--
-- Test PG_UNICODE_FAST
--

CREATE COLLATION regress_pg_unicode_fast (
  provider = builtin, locale = 'unicode'); -- fails
--dg-skip: relation pg_collation cannot be written: digres has no catalog tables
--dg CREATE COLLATION regress_pg_unicode_fast (
--dg   provider = builtin, locale = 'PG_UNICODE_FAST');

--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE test_pg_unicode_fast (
--dg   t TEXT COLLATE PG_UNICODE_FAST
--dg );
--dg-cascade
--dg INSERT INTO test_pg_unicode_fast VALUES
--dg   ('abc DEF 123abc'),
--dg   ('ábc sßs ßss DÉF'),
--dg   ('ǄxxǄ ǆxxǅ ǅxxǆ'),
--dg   (U&'Λλ 1a \FF11a'),
--dg   ('ȺȺȺ'),
--dg   ('ⱥⱥⱥ'),
--dg   ('ⱥȺ');

--dg-cascade
--dg SELECT
--dg     t, lower(t), initcap(t), upper(t),
--dg     length(convert_to(t, 'UTF8')) AS t_bytes,
--dg     length(convert_to(lower(t), 'UTF8')) AS lower_t_bytes,
--dg     length(convert_to(initcap(t), 'UTF8')) AS initcap_t_bytes,
--dg     length(convert_to(upper(t), 'UTF8')) AS upper_t_bytes
--dg   FROM test_pg_unicode_fast;

--dg-cascade
--dg DROP TABLE test_pg_unicode_fast;

-- test Final_Sigma
SELECT lower('ΑΣ' COLLATE PG_UNICODE_FAST); -- 0391 03A3
SELECT lower('ΑΣ0' COLLATE PG_UNICODE_FAST); -- 0391 03A3 0030
SELECT lower('ἈΣ̓' COLLATE PG_UNICODE_FAST); -- 0391 0343 03A3 0343
SELECT lower('ᾼΣͅ' COLLATE PG_UNICODE_FAST); -- 0391 0345 03A3 0345

-- test !Final_Sigma
SELECT lower('Σ' COLLATE PG_UNICODE_FAST); -- 03A3
SELECT lower('0Σ' COLLATE PG_UNICODE_FAST); -- 0030 03A3
SELECT lower('ΑΣΑ' COLLATE PG_UNICODE_FAST); -- 0391 03A3 0391
SELECT lower('ἈΣ̓Α' COLLATE PG_UNICODE_FAST); -- 0391 0343 03A3 0343 0391
SELECT lower('ᾼΣͅΑ' COLLATE PG_UNICODE_FAST); -- 0391 0345 03A3 0345 0391
SELECT lower(U&'\0300\03A3' COLLATE PG_UNICODE_FAST);

-- properties

SELECT 'xyz' ~ '[[:alnum:]]' COLLATE PG_UNICODE_FAST;
SELECT 'xyz' !~ '[[:upper:]]' COLLATE PG_UNICODE_FAST;
SELECT '@' !~ '[[:alnum:]]' COLLATE PG_UNICODE_FAST;
SELECT '=' !~ '[[:punct:]]' COLLATE PG_UNICODE_FAST; -- symbols are not punctuation
SELECT 'a8a' ~ '[[:digit:]]' COLLATE PG_UNICODE_FAST;
SELECT '൧' ~ '\d' COLLATE PG_UNICODE_FAST;

-- case mapping

SELECT 'xYz' ~* 'XyZ' COLLATE PG_UNICODE_FAST;
SELECT 'xAb' ~* '[W-Y]' COLLATE PG_UNICODE_FAST;
SELECT 'xAb' !~* '[c-d]' COLLATE PG_UNICODE_FAST;
SELECT 'Δ' ~* '[γ-λ]' COLLATE PG_UNICODE_FAST;
SELECT 'δ' ~* '[Γ-Λ]' COLLATE PG_UNICODE_FAST; -- same as above with cases reversed

-- case folding
select casefold('AbCd 123 #$% ıiIİ ẞ ß Ǆǅǆ Σσς' collate PG_UNICODE_FAST);
