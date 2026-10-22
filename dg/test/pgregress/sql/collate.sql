/*
 * This test is intended to pass on all platforms supported by Postgres.
 * We can therefore only assume that the default, C, and POSIX collations
 * are available --- and since the regression tests are often run in a
 * C-locale database, these may well all have the same behavior.  But
 * fortunately, the system doesn't know that and will treat them as
 * incompatible collations.  It is therefore at least possible to test
 * parser behaviors such as collation conflict resolution.  This test will,
 * however, be more revealing when run in a database with non-C locale,
 * since any departure from C sorting behavior will show as a failure.
 */

CREATE SCHEMA collate_tests;
SET search_path = collate_tests;

--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE collate_test1 (
--dg     a int,
--dg     b text COLLATE "C" NOT NULL
--dg );

--dg-cascade
--dg \d collate_test1

CREATE TABLE collate_test_fail (
    a int COLLATE "C",
    b text
);

--dg-cascade
--dg CREATE TABLE collate_test_like (
--dg     LIKE collate_test1
--dg );

--dg-cascade
--dg \d collate_test_like

--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE collate_test2 (
--dg     a int,
--dg     b text COLLATE "POSIX"
--dg );

--dg-cascade
--dg INSERT INTO collate_test1 VALUES (1, 'abc'), (2, 'Abc'), (3, 'bbc'), (4, 'ABD');
--dg-cascade
--dg INSERT INTO collate_test2 SELECT * FROM collate_test1;

--dg-cascade
--dg SELECT * FROM collate_test1 WHERE b COLLATE "C" >= 'abc';
--dg-cascade
--dg SELECT * FROM collate_test1 WHERE b >= 'abc' COLLATE "C";
--dg-cascade
--dg SELECT * FROM collate_test1 WHERE b COLLATE "C" >= 'abc' COLLATE "C";
--dg-cascade
--dg SELECT * FROM collate_test1 WHERE b COLLATE "C" >= 'bbc' COLLATE "POSIX"; -- fail

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE DOMAIN testdomain_p AS text COLLATE "POSIX";
CREATE DOMAIN testdomain_i AS int COLLATE "POSIX"; -- fail
--dg-cascade
--dg CREATE TABLE collate_test4 (
--dg     a int,
--dg     b testdomain_p
--dg );
--dg-cascade
--dg INSERT INTO collate_test4 SELECT * FROM collate_test1;
--dg-cascade
--dg SELECT a, b FROM collate_test4 ORDER BY b;

--dg-cascade
--dg CREATE TABLE collate_test5 (
--dg     a int,
--dg     b testdomain_p COLLATE "C"
--dg );
--dg-cascade
--dg INSERT INTO collate_test5 SELECT * FROM collate_test1;
--dg-cascade
--dg SELECT a, b FROM collate_test5 ORDER BY b;


--dg-cascade
--dg SELECT a, b FROM collate_test1 ORDER BY b;
--dg-cascade
--dg SELECT a, b FROM collate_test2 ORDER BY b;

--dg-cascade
--dg SELECT a, b FROM collate_test1 ORDER BY b COLLATE "C";

-- star expansion
--dg-cascade
--dg SELECT * FROM collate_test1 ORDER BY b;
--dg-cascade
--dg SELECT * FROM collate_test2 ORDER BY b;

-- constant expression folding
SELECT 'bbc' COLLATE "C" > 'Abc' COLLATE "C" AS "true";
SELECT 'bbc' COLLATE "POSIX" < 'Abc' COLLATE "POSIX" AS "false";

-- upper/lower

--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE collate_test10 (
--dg     a int,
--dg     x text COLLATE "C",
--dg     y text COLLATE "POSIX"
--dg );

--dg-cascade
--dg INSERT INTO collate_test10 VALUES (1, 'hij', 'hij'), (2, 'HIJ', 'HIJ');

--dg-cascade
--dg SELECT a, lower(x), lower(y), upper(x), upper(y), initcap(x), initcap(y) FROM collate_test10;
--dg-cascade
--dg SELECT a, lower(x COLLATE "C"), lower(y COLLATE "C") FROM collate_test10;

--dg-cascade
--dg SELECT a, x, y FROM collate_test10 ORDER BY lower(y), a;

-- backwards parsing

--dg-cascade
--dg CREATE VIEW collview1 AS SELECT * FROM collate_test1 WHERE b COLLATE "C" >= 'bbc';
--dg-cascade
--dg CREATE VIEW collview2 AS SELECT a, b FROM collate_test1 ORDER BY b COLLATE "C";
--dg-cascade
--dg CREATE VIEW collview3 AS SELECT a, lower((x || x) COLLATE "POSIX") FROM collate_test10;

--dg-cascade
--dg SELECT table_name, view_definition FROM information_schema.views
--dg   WHERE table_name LIKE 'collview%' ORDER BY 1;


-- collation propagation in various expression types

--dg-cascade
--dg SELECT a, coalesce(b, 'foo') FROM collate_test1 ORDER BY 2;
--dg-cascade
--dg SELECT a, coalesce(b, 'foo') FROM collate_test2 ORDER BY 2;
--dg-cascade
--dg SELECT a, lower(coalesce(x, 'foo')), lower(coalesce(y, 'foo')) FROM collate_test10;

--dg-cascade
--dg SELECT a, b, greatest(b, 'CCC') FROM collate_test1 ORDER BY 3;
--dg-cascade
--dg SELECT a, b, greatest(b, 'CCC') FROM collate_test2 ORDER BY 3;
--dg-cascade
--dg SELECT a, x, y, lower(greatest(x, 'foo')), lower(greatest(y, 'foo')) FROM collate_test10;

--dg-cascade
--dg SELECT a, nullif(b, 'abc') FROM collate_test1 ORDER BY 2;
--dg-cascade
--dg SELECT a, nullif(b, 'abc') FROM collate_test2 ORDER BY 2;
--dg-cascade
--dg SELECT a, lower(nullif(x, 'foo')), lower(nullif(y, 'foo')) FROM collate_test10;

--dg-cascade
--dg SELECT a, CASE b WHEN 'abc' THEN 'abcd' ELSE b END FROM collate_test1 ORDER BY 2;
--dg-cascade
--dg SELECT a, CASE b WHEN 'abc' THEN 'abcd' ELSE b END FROM collate_test2 ORDER BY 2;

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE DOMAIN testdomain AS text;
--dg-cascade
--dg SELECT a, b::testdomain FROM collate_test1 ORDER BY 2;
--dg-cascade
--dg SELECT a, b::testdomain FROM collate_test2 ORDER BY 2;
--dg-cascade
--dg SELECT a, b::testdomain_p FROM collate_test2 ORDER BY 2;
--dg-cascade
--dg SELECT a, lower(x::testdomain), lower(y::testdomain) FROM collate_test10;

--dg-cascade
--dg SELECT min(b), max(b) FROM collate_test1;
--dg-cascade
--dg SELECT min(b), max(b) FROM collate_test2;

--dg-cascade
--dg SELECT array_agg(b ORDER BY b) FROM collate_test1;
--dg-cascade
--dg SELECT array_agg(b ORDER BY b) FROM collate_test2;

-- In aggregates, ORDER BY expressions don't affect aggregate's collation
--dg-cascade
--dg SELECT string_agg(x COLLATE "C", y COLLATE "POSIX") FROM collate_test10;  -- fail
--dg-cascade
--dg SELECT array_agg(x COLLATE "C" ORDER BY y COLLATE "POSIX") FROM collate_test10;
--dg-cascade
--dg SELECT array_agg(a ORDER BY x COLLATE "C", y COLLATE "POSIX") FROM collate_test10;
--dg-cascade
--dg SELECT array_agg(a ORDER BY x||y) FROM collate_test10;  -- fail

--dg-cascade
--dg SELECT a, b FROM collate_test1 UNION ALL SELECT a, b FROM collate_test1 ORDER BY 2;
--dg-cascade
--dg SELECT a, b FROM collate_test2 UNION SELECT a, b FROM collate_test2 ORDER BY 2;
--dg-cascade
--dg SELECT a, b FROM collate_test2 WHERE a < 4 INTERSECT SELECT a, b FROM collate_test2 WHERE a > 1 ORDER BY 2;
--dg-cascade
--dg SELECT a, b FROM collate_test2 EXCEPT SELECT a, b FROM collate_test2 WHERE a < 2 ORDER BY 2;

--dg-cascade
--dg SELECT a, b FROM collate_test1 UNION ALL SELECT a, b FROM collate_test2 ORDER BY 2; -- fail
--dg-cascade
--dg SELECT a, b FROM collate_test1 UNION ALL SELECT a, b FROM collate_test2; -- ok
--dg-cascade
--dg SELECT a, b FROM collate_test1 UNION SELECT a, b FROM collate_test2 ORDER BY 2; -- fail
--dg-cascade
--dg SELECT a, b COLLATE "C" FROM collate_test1 UNION SELECT a, b FROM collate_test2 ORDER BY 2; -- ok
--dg-cascade
--dg SELECT a, b FROM collate_test1 INTERSECT SELECT a, b FROM collate_test2 ORDER BY 2; -- fail
--dg-cascade
--dg SELECT a, b FROM collate_test1 EXCEPT SELECT a, b FROM collate_test2 ORDER BY 2; -- fail

--dg-cascade
--dg CREATE TABLE test_u AS SELECT a, b FROM collate_test1 UNION ALL SELECT a, b FROM collate_test2; -- fail

-- ideally this would be a parse-time error, but for now it must be run-time:
--dg-cascade
--dg select x < y from collate_test10; -- fail
--dg-cascade
--dg select x || y from collate_test10; -- ok, because || is not collation aware
--dg-cascade
--dg select x, y from collate_test10 order by x || y; -- not so ok

-- collation mismatch between recursive and non-recursive term
WITH RECURSIVE foo(x) AS
   (SELECT x FROM (VALUES('a' COLLATE "C"),('b')) t(x)
   UNION ALL
   SELECT (x || 'c') COLLATE "POSIX" FROM foo WHERE length(x) < 10)
SELECT * FROM foo;

SELECT a, b, a < b as lt FROM
  (VALUES ('a', 'B'), ('A', 'b' COLLATE "C")) v(a,b);

-- collation mismatch in subselects
--dg-cascade
--dg SELECT * FROM collate_test10 WHERE (x, y) NOT IN (SELECT y, x FROM collate_test10);
-- now it works with overrides
--dg-cascade
--dg SELECT * FROM collate_test10 WHERE (x COLLATE "POSIX", y COLLATE "C") NOT IN (SELECT y, x FROM collate_test10);
--dg-cascade
--dg SELECT * FROM collate_test10 WHERE (x, y) NOT IN (SELECT y COLLATE "C", x COLLATE "POSIX" FROM collate_test10);

-- casting

SELECT CAST('42' AS text COLLATE "C");

--dg-cascade
--dg SELECT a, CAST(b AS varchar) FROM collate_test1 ORDER BY 2;
--dg-cascade
--dg SELECT a, CAST(b AS varchar) FROM collate_test2 ORDER BY 2;


-- result of a SQL function

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION vc (text) RETURNS text LANGUAGE sql
--dg     AS 'select $1::varchar';

--dg-cascade
--dg SELECT a, b FROM collate_test1 ORDER BY a, vc(b);


-- polymorphism

--dg-cascade
--dg SELECT * FROM unnest((SELECT array_agg(b ORDER BY b) FROM collate_test1)) ORDER BY 1;
--dg-cascade
--dg SELECT * FROM unnest((SELECT array_agg(b ORDER BY b) FROM collate_test2)) ORDER BY 1;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION dup (anyelement) RETURNS anyelement
--dg     AS 'select $1' LANGUAGE sql;

--dg-cascade
--dg SELECT a, dup(b) FROM collate_test1 ORDER BY 2;
--dg-cascade
--dg SELECT a, dup(b) FROM collate_test2 ORDER BY 2;


-- indexes

--dg-cascade
--dg CREATE INDEX collate_test1_idx1 ON collate_test1 (b);
--dg-cascade
--dg CREATE INDEX collate_test1_idx2 ON collate_test1 (b COLLATE "POSIX");
--dg-cascade
--dg CREATE INDEX collate_test1_idx3 ON collate_test1 ((b COLLATE "POSIX")); -- this is different grammatically
--dg-cascade
--dg CREATE INDEX collate_test1_idx4 ON collate_test1 (((b||'foo') COLLATE "POSIX"));

--dg-cascade
--dg CREATE INDEX collate_test1_idx5 ON collate_test1 (a COLLATE "POSIX"); -- fail
--dg-cascade
--dg CREATE INDEX collate_test1_idx6 ON collate_test1 ((a COLLATE "POSIX")); -- fail

--dg-cascade
--dg SELECT relname, pg_get_indexdef(oid) FROM pg_class WHERE relname LIKE 'collate_test%_idx%' ORDER BY 1;


-- foreign keys

-- force indexes and mergejoins to be used for FK checking queries,
-- else they might not exercise collation-dependent operators
SET enable_seqscan TO 0;
SET enable_hashjoin TO 0;
SET enable_nestloop TO 0;

--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE collate_test20 (f1 text COLLATE "C" PRIMARY KEY);
--dg-cascade
--dg INSERT INTO collate_test20 VALUES ('foo'), ('bar');
--dg-cascade
--dg CREATE TABLE collate_test21 (f2 text COLLATE "POSIX" REFERENCES collate_test20);
--dg-cascade
--dg INSERT INTO collate_test21 VALUES ('foo'), ('bar');
--dg-cascade
--dg INSERT INTO collate_test21 VALUES ('baz'); -- fail
--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE collate_test22 (f2 text COLLATE "POSIX");
--dg-cascade
--dg INSERT INTO collate_test22 VALUES ('foo'), ('bar'), ('baz');
--dg-cascade
--dg ALTER TABLE collate_test22 ADD FOREIGN KEY (f2) REFERENCES collate_test20; -- fail
--dg-cascade
--dg DELETE FROM collate_test22 WHERE f2 = 'baz';
--dg-cascade
--dg ALTER TABLE collate_test22 ADD FOREIGN KEY (f2) REFERENCES collate_test20;

RESET enable_seqscan;
RESET enable_hashjoin;
RESET enable_nestloop;


-- EXPLAIN

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg   SELECT * FROM collate_test10 ORDER BY x, y;
--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg   SELECT * FROM collate_test10 ORDER BY x DESC, y COLLATE "C" ASC NULLS FIRST;


-- CREATE/DROP COLLATION

--dg-skip: relation pg_collation cannot be written: digres has no catalog tables
--dg CREATE COLLATION builtin_c ( PROVIDER = builtin, LOCALE = "C" );

--dg-cascade
--dg SELECT b FROM collate_test1 ORDER BY b COLLATE builtin_c;

CREATE COLLATION builtin2 ( PROVIDER = builtin ); -- fails
CREATE COLLATION builtin2 ( PROVIDER = builtin, LOCALE = "en_US" ); -- fails
CREATE COLLATION builtin2 ( PROVIDER = builtin, LC_CTYPE = "C", LC_COLLATE = "C" ); -- fails

--dg-skip: relation pg_collation cannot be written: digres has no catalog tables
--dg CREATE COLLATION mycoll1 FROM "C";
--dg-skip: relation pg_collation cannot be written: digres has no catalog tables
--dg CREATE COLLATION mycoll2 ( LC_COLLATE = "POSIX", LC_CTYPE = "POSIX" );
CREATE COLLATION mycoll3 FROM "default";  -- intentionally unsupported

--dg-cascade
--dg DROP COLLATION mycoll1;
--dg-cascade
--dg CREATE TABLE collate_test23 (f1 text collate mycoll2);
--dg-cascade
--dg DROP COLLATION mycoll2;  -- fail

-- invalid: non-lowercase quoted identifiers
CREATE COLLATION case_coll ("Lc_Collate" = "POSIX", "Lc_Ctype" = "POSIX");

-- 9.1 bug with useless COLLATE in an expression subject to length coercion

CREATE TEMP TABLE vctable (f1 varchar(25));
INSERT INTO vctable VALUES ('foo' COLLATE "C");


SELECT collation for ('foo'); -- unknown type - null
SELECT collation for ('foo'::text);
--dg-cascade
--dg SELECT collation for ((SELECT a FROM collate_test1 LIMIT 1)); -- non-collatable type - error
--dg-cascade
--dg SELECT collation for ((SELECT b FROM collate_test1 LIMIT 1));

-- old bug with not dropping COLLATE when coercing to non-collatable type
CREATE VIEW collate_on_int AS
SELECT c1+1 AS c1p FROM
  (SELECT ('4' COLLATE "C")::INT AS c1) ss;
\d+ collate_on_int

-- Check conflicting or redundant options in CREATE COLLATION
-- LC_COLLATE
CREATE COLLATION coll_dup_chk (LC_COLLATE = "POSIX", LC_COLLATE = "NONSENSE", LC_CTYPE = "POSIX");
-- LC_CTYPE
CREATE COLLATION coll_dup_chk (LC_CTYPE = "POSIX", LC_CTYPE = "NONSENSE", LC_COLLATE = "POSIX");
-- PROVIDER
CREATE COLLATION coll_dup_chk (PROVIDER = icu, PROVIDER = NONSENSE, LC_COLLATE = "POSIX", LC_CTYPE = "POSIX");
-- LOCALE
CREATE COLLATION case_sensitive (LOCALE = '', LOCALE = "NONSENSE");
-- DETERMINISTIC
CREATE COLLATION coll_dup_chk (DETERMINISTIC = TRUE, DETERMINISTIC = NONSENSE, LOCALE = '');
-- VERSION
CREATE COLLATION coll_dup_chk (VERSION = '1', VERSION = "NONSENSE", LOCALE = '');
-- LOCALE conflicts with LC_COLLATE and LC_CTYPE
CREATE COLLATION coll_dup_chk (LC_COLLATE = "POSIX", LC_CTYPE = "POSIX", LOCALE = '');
-- LOCALE conflicts with LC_COLLATE
CREATE COLLATION coll_dup_chk (LC_COLLATE = "POSIX", LOCALE = '');
-- LOCALE conflicts with LC_CTYPE
CREATE COLLATION coll_dup_chk (LC_CTYPE = "POSIX", LOCALE = '');
-- FROM conflicts with any other option
CREATE COLLATION coll_dup_chk (FROM = "C", VERSION = "1");

-- Regex exact-match optimization should use an index even when the expression
-- and index have different collations, so long as the expression's collation
-- is deterministic.  This example tests what we want because the optimizer
-- does not perceive "C" collation (used by the system catalogs) as identical
-- to "POSIX" collation.
--dg-differs: the plan
--dg EXPLAIN (COSTS OFF)
--dg SELECT * FROM pg_class WHERE relname ~ '^pg_class$' COLLATE "POSIX";
--dg-differs: the plan
--dg EXPLAIN (COSTS OFF)
--dg SELECT * FROM pg_class WHERE relname LIKE 'pg\_class' COLLATE "POSIX";

--
-- Clean up.  Many of these table names will be re-used if the user is
-- trying to run any platform-specific collation tests later, so we
-- must get rid of them.
--
--dg-cascade
--dg DROP SCHEMA collate_tests CASCADE;
