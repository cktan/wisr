-- Perform tests on the Memoize node.

-- The cache hits/misses/evictions from the Memoize node can vary between
-- machines.  Let's just replace the number with an 'N'.  In order to allow us
-- to perform validation when the measure was zero, we replace a zero value
-- with "Zero".  All other numbers are replaced with 'N'.
--dg-skip: language "?" does not exist
--dg create function explain_memoize(query text, hide_hitmiss bool) returns setof text
--dg language plpgsql as
--dg $$
--dg declare
--dg     ln text;
--dg begin
--dg     for ln in
--dg         execute format('explain (analyze, costs off, summary off, timing off, buffers off) %s',
--dg             query)
--dg     loop
--dg         if hide_hitmiss = true then
--dg                 ln := regexp_replace(ln, 'Hits: 0', 'Hits: Zero');
--dg                 ln := regexp_replace(ln, 'Hits: \d+', 'Hits: N');
--dg                 ln := regexp_replace(ln, 'Misses: 0', 'Misses: Zero');
--dg                 ln := regexp_replace(ln, 'Misses: \d+', 'Misses: N');
--dg         end if;
--dg         ln := regexp_replace(ln, 'Evictions: 0', 'Evictions: Zero');
--dg         ln := regexp_replace(ln, 'Evictions: \d+', 'Evictions: N');
--dg         ln := regexp_replace(ln, 'Memory Usage: \d+', 'Memory Usage: N');
--dg         ln := regexp_replace(ln, 'Heap Fetches: \d+', 'Heap Fetches: N');
--dg         ln := regexp_replace(ln, 'loops=\d+', 'loops=N');
--dg         ln := regexp_replace(ln, 'Index Searches: \d+', 'Index Searches: N');
--dg         return next ln;
--dg     end loop;
--dg end;
--dg $$;

-- Ensure we get a memoize node on the inner side of the nested loop
SET enable_hashjoin TO off;
SET enable_bitmapscan TO off;

--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT COUNT(*),AVG(t1.unique1) FROM tenk1 t1
--dg INNER JOIN tenk1 t2 ON t1.unique1 = t2.twenty
--dg WHERE t2.unique1 < 1000;', false);

-- And check we get the expected results.
SELECT COUNT(*),AVG(t1.unique1) FROM tenk1 t1
INNER JOIN tenk1 t2 ON t1.unique1 = t2.twenty
WHERE t2.unique1 < 1000;

-- Try with LATERAL joins
--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT COUNT(*),AVG(t2.unique1) FROM tenk1 t1,
--dg LATERAL (SELECT t2.unique1 FROM tenk1 t2
--dg          WHERE t1.twenty = t2.unique1 OFFSET 0) t2
--dg WHERE t1.unique1 < 1000;', false);

-- And check we get the expected results.
SELECT COUNT(*),AVG(t2.unique1) FROM tenk1 t1,
LATERAL (SELECT t2.unique1 FROM tenk1 t2
         WHERE t1.twenty = t2.unique1 OFFSET 0) t2
WHERE t1.unique1 < 1000;

-- Try with LATERAL joins
--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT COUNT(*),AVG(t2.t1two) FROM tenk1 t1 LEFT JOIN
--dg LATERAL (
--dg     SELECT t1.two as t1two, * FROM tenk1 t2 WHERE t2.unique1 < 4 OFFSET 0
--dg ) t2
--dg ON t1.two = t2.two
--dg WHERE t1.unique1 < 10;', false);

-- And check we get the expected results.
SELECT COUNT(*),AVG(t2.t1two) FROM tenk1 t1 LEFT JOIN
LATERAL (
    SELECT t1.two as t1two, * FROM tenk1 t2 WHERE t2.unique1 < 4 OFFSET 0
) t2
ON t1.two = t2.two
WHERE t1.unique1 < 10;

-- Try with LATERAL references within PlaceHolderVars
--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT COUNT(*), AVG(t1.twenty) FROM tenk1 t1 LEFT JOIN
--dg LATERAL (SELECT t1.two+1 AS c1, t2.unique1 AS c2 FROM tenk1 t2) s ON TRUE
--dg WHERE s.c1 = s.c2 AND t1.unique1 < 1000;', false);

-- And check we get the expected results.
SELECT COUNT(*), AVG(t1.twenty) FROM tenk1 t1 LEFT JOIN
LATERAL (SELECT t1.two+1 AS c1, t2.unique1 AS c2 FROM tenk1 t2) s ON TRUE
WHERE s.c1 = s.c2 AND t1.unique1 < 1000;

-- Ensure we do not omit the cache keys from PlaceHolderVars
--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT COUNT(*), AVG(t1.twenty) FROM tenk1 t1 LEFT JOIN
--dg LATERAL (SELECT t1.twenty AS c1, t2.unique1 AS c2, t2.two FROM tenk1 t2) s
--dg ON t1.two = s.two
--dg WHERE s.c1 = s.c2 AND t1.unique1 < 1000;', false);

-- And check we get the expected results.
SELECT COUNT(*), AVG(t1.twenty) FROM tenk1 t1 LEFT JOIN
LATERAL (SELECT t1.twenty AS c1, t2.unique1 AS c2, t2.two FROM tenk1 t2) s
ON t1.two = s.two
WHERE s.c1 = s.c2 AND t1.unique1 < 1000;

SET enable_mergejoin TO off;

-- Test for varlena datatype with expr evaluation
CREATE TABLE expr_key (x numeric, t text);
INSERT INTO expr_key (x, t)
SELECT d1::numeric, d1::text FROM (
    SELECT round((d / pi())::numeric, 7) AS d1 FROM generate_series(1, 20) AS d
) t;

-- duplicate rows so we get some cache hits
INSERT INTO expr_key SELECT * FROM expr_key;

CREATE INDEX expr_key_idx_x_t ON expr_key (x, t);
VACUUM ANALYZE expr_key;

-- Ensure we get we get a cache miss and hit for each of the 20 distinct values
--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT * FROM expr_key t1 INNER JOIN expr_key t2
--dg ON t1.x = t2.t::numeric AND t1.t::numeric = t2.x;', false);

DROP TABLE expr_key;

-- Reduce work_mem and hash_mem_multiplier so that we see some cache evictions
SET work_mem TO '64kB';
SET hash_mem_multiplier TO 1.0;
-- Ensure we get some evictions.  We're unable to validate the hits and misses
-- here as the number of entries that fit in the cache at once will vary
-- between different machines.
--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT COUNT(*),AVG(t1.unique1) FROM tenk1 t1
--dg INNER JOIN tenk1 t2 ON t1.unique1 = t2.thousand
--dg WHERE t2.unique1 < 1200;', true);

CREATE TABLE flt (f float);
CREATE INDEX flt_f_idx ON flt (f);
INSERT INTO flt VALUES('-0.0'::float),('+0.0'::float);
ANALYZE flt;

SET enable_seqscan TO off;

-- Ensure memoize operates in logical mode
--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT * FROM flt f1 INNER JOIN flt f2 ON f1.f = f2.f;', false);

-- Ensure memoize operates in binary mode
--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT * FROM flt f1 INNER JOIN flt f2 ON f1.f >= f2.f;', false);

DROP TABLE flt;

-- Exercise Memoize in binary mode with a large fixed width type and a
-- varlena type.
CREATE TABLE strtest (n name, t text);
CREATE INDEX strtest_n_idx ON strtest (n);
CREATE INDEX strtest_t_idx ON strtest (t);
INSERT INTO strtest VALUES('one','one'),('two','two'),('three',repeat(fipshash('three'),100));
-- duplicate rows so we get some cache hits
INSERT INTO strtest SELECT * FROM strtest;
ANALYZE strtest;

-- Ensure we get 3 hits and 3 misses
--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT * FROM strtest s1 INNER JOIN strtest s2 ON s1.n >= s2.n;', false);

-- Ensure we get 3 hits and 3 misses
--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT * FROM strtest s1 INNER JOIN strtest s2 ON s1.t >= s2.t;', false);

DROP TABLE strtest;

-- Ensure memoize works with partitionwise join
SET enable_partitionwise_join TO on;

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE prt (a int) PARTITION BY RANGE(a);
--dg-cascade
--dg CREATE TABLE prt_p1 PARTITION OF prt FOR VALUES FROM (0) TO (10);
--dg-cascade
--dg CREATE TABLE prt_p2 PARTITION OF prt FOR VALUES FROM (10) TO (20);
--dg-cascade
--dg INSERT INTO prt VALUES (0), (0), (0), (0);
--dg-cascade
--dg INSERT INTO prt VALUES (10), (10), (10), (10);
--dg-cascade
--dg CREATE INDEX iprt_p1_a ON prt_p1 (a);
--dg-cascade
--dg CREATE INDEX iprt_p2_a ON prt_p2 (a);
--dg-cascade
--dg ANALYZE prt;

--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT * FROM prt t1 INNER JOIN prt t2 ON t1.a = t2.a;', false);

-- Ensure memoize works with parameterized union-all Append path
SET enable_partitionwise_join TO off;

--dg-cascade
--dg SELECT explain_memoize('
--dg SELECT * FROM prt_p1 t1 INNER JOIN
--dg (SELECT * FROM prt_p1 UNION ALL SELECT * FROM prt_p2) t2
--dg ON t1.a = t2.a;', false);

--dg-cascade
--dg DROP TABLE prt;

RESET enable_partitionwise_join;

-- Exercise Memoize code that flushes the cache when a parameter changes which
-- is not part of the cache key.

-- Ensure we get a Memoize plan
EXPLAIN (COSTS OFF)
SELECT unique1 FROM tenk1 t0
WHERE unique1 < 3
  AND EXISTS (
	SELECT 1 FROM tenk1 t1
	INNER JOIN tenk1 t2 ON t1.unique1 = t2.hundred
	WHERE t0.ten = t1.twenty AND t0.two <> t2.four OFFSET 0);

-- Ensure the above query returns the correct result
SELECT unique1 FROM tenk1 t0
WHERE unique1 < 3
  AND EXISTS (
	SELECT 1 FROM tenk1 t1
	INNER JOIN tenk1 t2 ON t1.unique1 = t2.hundred
	WHERE t0.ten = t1.twenty AND t0.two <> t2.four OFFSET 0);

RESET enable_seqscan;
RESET enable_mergejoin;
RESET work_mem;
RESET hash_mem_multiplier;
RESET enable_bitmapscan;
RESET enable_hashjoin;

-- Test parallel plans with Memoize
--dg-skip: unrecognized configuration parameter "?"
--dg SET min_parallel_table_scan_size TO 0;
--dg-skip: unrecognized configuration parameter "?"
--dg SET parallel_setup_cost TO 0;
--dg-skip: unrecognized configuration parameter "?"
--dg SET parallel_tuple_cost TO 0;
--dg-skip: unrecognized configuration parameter "?"
--dg SET max_parallel_workers_per_gather TO 2;

-- Ensure we get a parallel plan.
--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT COUNT(*),AVG(t2.unique1) FROM tenk1 t1,
--dg LATERAL (SELECT t2.unique1 FROM tenk1 t2 WHERE t1.twenty = t2.unique1) t2
--dg WHERE t1.unique1 < 1000;

-- And ensure the parallel plan gives us the correct results.
SELECT COUNT(*),AVG(t2.unique1) FROM tenk1 t1,
LATERAL (SELECT t2.unique1 FROM tenk1 t2 WHERE t1.twenty = t2.unique1) t2
WHERE t1.unique1 < 1000;

--dg-skip: unrecognized configuration parameter "?"
--dg RESET max_parallel_workers_per_gather;
--dg-skip: unrecognized configuration parameter "?"
--dg RESET parallel_tuple_cost;
--dg-skip: unrecognized configuration parameter "?"
--dg RESET parallel_setup_cost;
--dg-skip: unrecognized configuration parameter "?"
--dg RESET min_parallel_table_scan_size;
