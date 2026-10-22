--
-- SP-GiST index tests
--

--dg-skip: type ? is not supported yet
--dg CREATE TABLE quad_point_tbl AS
--dg     SELECT point(unique1,unique2) AS p FROM tenk1;

--dg-cascade
--dg INSERT INTO quad_point_tbl
--dg     SELECT '(333.0,400.0)'::point FROM generate_series(1,1000);

--dg-cascade
--dg INSERT INTO quad_point_tbl VALUES (NULL), (NULL), (NULL);

--dg-cascade
--dg CREATE INDEX sp_quad_ind ON quad_point_tbl USING spgist (p);

--dg-cascade
--dg CREATE TABLE kd_point_tbl AS SELECT * FROM quad_point_tbl;

--dg-cascade
--dg CREATE INDEX sp_kd_ind ON kd_point_tbl USING spgist (p kd_point_ops);

CREATE TABLE radix_text_tbl AS
    SELECT name AS t FROM road WHERE name !~ '^[0-9]';

INSERT INTO radix_text_tbl
    SELECT 'P0123456789abcdef' FROM generate_series(1,1000);
INSERT INTO radix_text_tbl VALUES ('P0123456789abcde');
INSERT INTO radix_text_tbl VALUES ('P0123456789abcdefF');

CREATE INDEX sp_radix_ind ON radix_text_tbl USING spgist (t);

-- get non-indexed results for comparison purposes

SET enable_seqscan = ON;
SET enable_indexscan = OFF;
SET enable_bitmapscan = OFF;

--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;

--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;

--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl;

--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;

--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';

--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';

--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p <<| '(5000, 4000)';

--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p |>> '(5000, 4000)';

--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';

--dg-cascade
--dg CREATE TEMP TABLE quad_point_tbl_ord_seq1 AS
--dg SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
--dg FROM quad_point_tbl;

--dg-cascade
--dg CREATE TEMP TABLE quad_point_tbl_ord_seq2 AS
--dg SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
--dg FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

--dg-cascade
--dg CREATE TEMP TABLE quad_point_tbl_ord_seq3 AS
--dg SELECT row_number() OVER (ORDER BY p <-> '333,400') n, p <-> '333,400' dist, p
--dg FROM quad_point_tbl WHERE p IS NOT NULL;

SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';

SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';

SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';

SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t ^@  'Worth';

-- Now check the results from plain indexscan
SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl;
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p <<| '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p <<| '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p |>> '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p |>> '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
--dg FROM quad_point_tbl;
--dg-cascade
--dg CREATE TEMP TABLE quad_point_tbl_ord_idx1 AS
--dg SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
--dg FROM quad_point_tbl;
--dg-cascade
--dg SELECT * FROM quad_point_tbl_ord_seq1 seq FULL JOIN quad_point_tbl_ord_idx1 idx
--dg ON seq.n = idx.n
--dg WHERE seq.dist IS DISTINCT FROM idx.dist;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
--dg FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';
--dg-cascade
--dg CREATE TEMP TABLE quad_point_tbl_ord_idx2 AS
--dg SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
--dg FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';
--dg-cascade
--dg SELECT * FROM quad_point_tbl_ord_seq2 seq FULL JOIN quad_point_tbl_ord_idx2 idx
--dg ON seq.n = idx.n
--dg WHERE seq.dist IS DISTINCT FROM idx.dist;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT row_number() OVER (ORDER BY p <-> '333,400') n, p <-> '333,400' dist, p
--dg FROM quad_point_tbl WHERE p IS NOT NULL;
--dg-cascade
--dg CREATE TEMP TABLE quad_point_tbl_ord_idx3 AS
--dg SELECT row_number() OVER (ORDER BY p <-> '333,400') n, p <-> '333,400' dist, p
--dg FROM quad_point_tbl WHERE p IS NOT NULL;
--dg-cascade
--dg SELECT * FROM quad_point_tbl_ord_seq3 seq FULL JOIN quad_point_tbl_ord_idx3 idx
--dg ON seq.n = idx.n
--dg WHERE seq.dist IS DISTINCT FROM idx.dist;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p <<| '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p <<| '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p |>> '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p |>> '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
--dg FROM kd_point_tbl;
--dg-cascade
--dg CREATE TEMP TABLE kd_point_tbl_ord_idx1 AS
--dg SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
--dg FROM kd_point_tbl;
--dg-cascade
--dg SELECT * FROM quad_point_tbl_ord_seq1 seq FULL JOIN kd_point_tbl_ord_idx1 idx
--dg ON seq.n = idx.n
--dg WHERE seq.dist IS DISTINCT FROM idx.dist;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
--dg FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';
--dg-cascade
--dg CREATE TEMP TABLE kd_point_tbl_ord_idx2 AS
--dg SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
--dg FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';
--dg-cascade
--dg SELECT * FROM quad_point_tbl_ord_seq2 seq FULL JOIN kd_point_tbl_ord_idx2 idx
--dg ON seq.n = idx.n
--dg WHERE seq.dist IS DISTINCT FROM idx.dist;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT row_number() OVER (ORDER BY p <-> '333,400') n, p <-> '333,400' dist, p
--dg FROM kd_point_tbl WHERE p IS NOT NULL;
--dg-cascade
--dg CREATE TEMP TABLE kd_point_tbl_ord_idx3 AS
--dg SELECT row_number() OVER (ORDER BY p <-> '333,400') n, p <-> '333,400' dist, p
--dg FROM kd_point_tbl WHERE p IS NOT NULL;
--dg-cascade
--dg SELECT * FROM quad_point_tbl_ord_seq3 seq FULL JOIN kd_point_tbl_ord_idx3 idx
--dg ON seq.n = idx.n
--dg WHERE seq.dist IS DISTINCT FROM idx.dist;

-- test KNN scan with included columns
-- the distance numbers are not exactly the same across platforms
SET extra_float_digits = 0;
--dg-cascade
--dg CREATE INDEX ON quad_point_tbl_ord_seq1 USING spgist(p) INCLUDE(dist);
--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT p, dist FROM quad_point_tbl_ord_seq1 ORDER BY p <-> '0,0' LIMIT 10;
--dg-cascade
--dg SELECT p, dist FROM quad_point_tbl_ord_seq1 ORDER BY p <-> '0,0' LIMIT 10;
RESET extra_float_digits;

-- check ORDER BY distance to NULL
--dg-cascade
--dg SELECT (SELECT p FROM kd_point_tbl ORDER BY p <-> pt, p <-> '0,0' LIMIT 1)
--dg FROM (VALUES (point '1,2'), (NULL), ('1234,5678')) pts(pt);


EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ^@	 'Worth';
SELECT count(*) FROM radix_text_tbl WHERE t ^@	 'Worth';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE starts_with(t, 'Worth');
SELECT count(*) FROM radix_text_tbl WHERE starts_with(t, 'Worth');

-- Now check the results from bitmap indexscan
SET enable_seqscan = OFF;
SET enable_indexscan = OFF;
SET enable_bitmapscan = ON;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl;
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p <<| '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p <<| '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p |>> '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p |>> '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';
--dg-cascade
--dg SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p <<| '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p <<| '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p |>> '(5000, 4000)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p |>> '(5000, 4000)';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';
--dg-cascade
--dg SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ^@	 'Worth';
SELECT count(*) FROM radix_text_tbl WHERE t ^@	 'Worth';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE starts_with(t, 'Worth');
SELECT count(*) FROM radix_text_tbl WHERE starts_with(t, 'Worth');

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
