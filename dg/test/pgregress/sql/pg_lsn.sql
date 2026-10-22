--
-- PG_LSN
--

--dg-skip: type ? is not supported yet
--dg CREATE TABLE PG_LSN_TBL (f1 pg_lsn);

-- Largest and smallest input
--dg-cascade
--dg INSERT INTO PG_LSN_TBL VALUES ('0/0');
--dg-cascade
--dg INSERT INTO PG_LSN_TBL VALUES ('FFFFFFFF/FFFFFFFF');

-- Incorrect input
--dg-cascade
--dg INSERT INTO PG_LSN_TBL VALUES ('G/0');
--dg-cascade
--dg INSERT INTO PG_LSN_TBL VALUES ('-1/0');
--dg-cascade
--dg INSERT INTO PG_LSN_TBL VALUES (' 0/12345678');
--dg-cascade
--dg INSERT INTO PG_LSN_TBL VALUES ('ABCD/');
--dg-cascade
--dg INSERT INTO PG_LSN_TBL VALUES ('/ABCD');

-- Also try it with non-error-throwing API
SELECT pg_input_is_valid('16AE7F7', 'pg_lsn');
SELECT * FROM pg_input_error_info('16AE7F7', 'pg_lsn');

-- Min/Max aggregation
--dg-cascade
--dg SELECT MIN(f1), MAX(f1) FROM PG_LSN_TBL;

--dg-cascade
--dg DROP TABLE PG_LSN_TBL;

-- Operators
SELECT '0/16AE7F8' = '0/16AE7F8'::pg_lsn;
SELECT '0/16AE7F8'::pg_lsn != '0/16AE7F7';
SELECT '0/16AE7F7' < '0/16AE7F8'::pg_lsn;
SELECT '0/16AE7F8' > pg_lsn '0/16AE7F7';
SELECT '0/16AE7F7'::pg_lsn - '0/16AE7F8'::pg_lsn;
SELECT '0/16AE7F8'::pg_lsn - '0/16AE7F7'::pg_lsn;
SELECT '0/16AE7F7'::pg_lsn + 16::numeric;
SELECT 16::numeric + '0/16AE7F7'::pg_lsn;
SELECT '0/16AE7F7'::pg_lsn - 16::numeric;
SELECT 'FFFFFFFF/FFFFFFFE'::pg_lsn + 1::numeric;
SELECT 'FFFFFFFF/FFFFFFFE'::pg_lsn + 2::numeric; -- out of range error
SELECT '0/1'::pg_lsn - 1::numeric;
SELECT '0/1'::pg_lsn - 2::numeric; -- out of range error
SELECT '0/0'::pg_lsn + ('FFFFFFFF/FFFFFFFF'::pg_lsn - '0/0'::pg_lsn);
SELECT 'FFFFFFFF/FFFFFFFF'::pg_lsn - ('FFFFFFFF/FFFFFFFF'::pg_lsn - '0/0'::pg_lsn);
--dg-differs: another error: invalid input syntax for type ?
--dg SELECT '0/16AE7F7'::pg_lsn + 'NaN'::numeric;
--dg-differs: another error: invalid input syntax for type ?
--dg SELECT '0/16AE7F7'::pg_lsn - 'NaN'::numeric;

-- Check btree and hash opclasses
--dg-differs: the plan
--dg EXPLAIN (COSTS OFF)
--dg SELECT DISTINCT (i || '/' || j)::pg_lsn f
--dg   FROM generate_series(1, 10) i,
--dg        generate_series(1, 10) j,
--dg        generate_series(1, 5) k
--dg   WHERE i <= 10 AND j > 0 AND j <= 10
--dg   ORDER BY f;

SELECT DISTINCT (i || '/' || j)::pg_lsn f
  FROM generate_series(1, 10) i,
       generate_series(1, 10) j,
       generate_series(1, 5) k
  WHERE i <= 10 AND j > 0 AND j <= 10
  ORDER BY f;
