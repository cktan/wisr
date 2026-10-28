-- create a table to use as a basis for views and materialized views in various combinations
CREATE TABLE mvtest_t (id int NOT NULL PRIMARY KEY, type text NOT NULL, amt numeric NOT NULL);
INSERT INTO mvtest_t VALUES
  (1, 'x', 2),
  (2, 'x', 3),
  (3, 'y', 5),
  (4, 'y', 7),
  (5, 'z', 11);

-- we want a view based on the table, too, since views present additional challenges
CREATE VIEW mvtest_tv AS SELECT type, sum(amt) AS totamt FROM mvtest_t GROUP BY type;
--dg-differs: the result
--dg SELECT * FROM mvtest_tv ORDER BY type;

-- create a materialized view with no data, and confirm correct behavior
EXPLAIN (costs off)
  CREATE MATERIALIZED VIEW mvtest_tm AS SELECT type, sum(amt) AS totamt FROM mvtest_t GROUP BY type WITH NO DATA;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_tm AS SELECT type, sum(amt) AS totamt FROM mvtest_t GROUP BY type WITH NO DATA;
--dg-cascade
--dg SELECT relispopulated FROM pg_class WHERE oid = 'mvtest_tm'::regclass;
--dg-cascade
--dg SELECT * FROM mvtest_tm ORDER BY type;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_tm;
--dg-cascade
--dg SELECT relispopulated FROM pg_class WHERE oid = 'mvtest_tm'::regclass;
--dg-cascade
--dg CREATE UNIQUE INDEX mvtest_tm_type ON mvtest_tm (type);
--dg-cascade
--dg SELECT * FROM mvtest_tm ORDER BY type;

-- create various views
EXPLAIN (costs off)
  CREATE MATERIALIZED VIEW mvtest_tvm AS SELECT * FROM mvtest_tv ORDER BY type;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_tvm AS SELECT * FROM mvtest_tv ORDER BY type;
--dg-cascade
--dg SELECT * FROM mvtest_tvm;
--dg-cascade
--dg CREATE MATERIALIZED VIEW mvtest_tmm AS SELECT sum(totamt) AS grandtot FROM mvtest_tm;
--dg-cascade
--dg CREATE MATERIALIZED VIEW mvtest_tvmm AS SELECT sum(totamt) AS grandtot FROM mvtest_tvm;
--dg-cascade
--dg CREATE UNIQUE INDEX mvtest_tvmm_expr ON mvtest_tvmm ((grandtot > 0));
--dg-cascade
--dg CREATE UNIQUE INDEX mvtest_tvmm_pred ON mvtest_tvmm (grandtot) WHERE grandtot < 0;
CREATE VIEW mvtest_tvv AS SELECT sum(totamt) AS grandtot FROM mvtest_tv;
EXPLAIN (costs off)
  CREATE MATERIALIZED VIEW mvtest_tvvm AS SELECT * FROM mvtest_tvv;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_tvvm AS SELECT * FROM mvtest_tvv;
--dg-cascade
--dg CREATE VIEW mvtest_tvvmv AS SELECT * FROM mvtest_tvvm;
--dg-cascade
--dg CREATE MATERIALIZED VIEW mvtest_bb AS SELECT * FROM mvtest_tvvmv;
--dg-cascade
--dg CREATE INDEX mvtest_aa ON mvtest_bb (grandtot);

-- check that plans seem reasonable
--dg-cascade
--dg \d+ mvtest_tvm
--dg-cascade
--dg \d+ mvtest_tvm
--dg-cascade
--dg \d+ mvtest_tvvm
--dg-cascade
--dg \d+ mvtest_bb

-- test schema behavior
CREATE SCHEMA mvtest_mvschema;
--dg-skip: ALTER MATERIALIZED VIEW is not supported yet
--dg ALTER MATERIALIZED VIEW mvtest_tvm SET SCHEMA mvtest_mvschema;
\d+ mvtest_tvm
--dg-cascade
--dg \d+ mvtest_tvmm
SET search_path = mvtest_mvschema, public;
--dg-cascade
--dg \d+ mvtest_tvm

-- modify the underlying table data
INSERT INTO mvtest_t VALUES (6, 'z', 13);

-- confirm pre- and post-refresh contents of fairly simple materialized views
--dg-cascade
--dg SELECT * FROM mvtest_tm ORDER BY type;
--dg-cascade
--dg SELECT * FROM mvtest_tvm ORDER BY type;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW CONCURRENTLY mvtest_tm;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_tvm;
--dg-cascade
--dg SELECT * FROM mvtest_tm ORDER BY type;
--dg-cascade
--dg SELECT * FROM mvtest_tvm ORDER BY type;
RESET search_path;

-- confirm pre- and post-refresh contents of nested materialized views
--dg-cascade
--dg EXPLAIN (costs off)
--dg   SELECT * FROM mvtest_tmm;
--dg-cascade
--dg EXPLAIN (costs off)
--dg   SELECT * FROM mvtest_tvmm;
--dg-cascade
--dg EXPLAIN (costs off)
--dg   SELECT * FROM mvtest_tvvm;
--dg-cascade
--dg SELECT * FROM mvtest_tmm;
--dg-cascade
--dg SELECT * FROM mvtest_tvmm;
--dg-cascade
--dg SELECT * FROM mvtest_tvvm;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_tmm;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW CONCURRENTLY mvtest_tvmm;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_tvmm;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_tvvm;
--dg-cascade
--dg EXPLAIN (costs off)
--dg   SELECT * FROM mvtest_tmm;
--dg-cascade
--dg EXPLAIN (costs off)
--dg   SELECT * FROM mvtest_tvmm;
--dg-cascade
--dg EXPLAIN (costs off)
--dg   SELECT * FROM mvtest_tvvm;
--dg-cascade
--dg SELECT * FROM mvtest_tmm;
--dg-cascade
--dg SELECT * FROM mvtest_tvmm;
--dg-cascade
--dg SELECT * FROM mvtest_tvvm;

-- test diemv when the mv does not exist
DROP MATERIALIZED VIEW IF EXISTS no_such_mv;

-- make sure invalid combination of options is prohibited
--dg-cascade
--dg REFRESH MATERIALIZED VIEW CONCURRENTLY mvtest_tvmm WITH NO DATA;

-- no tuple locks on materialized views
--dg-cascade
--dg SELECT * FROM mvtest_tvvm FOR SHARE;

-- test join of mv and view
--dg-cascade
--dg SELECT type, m.totamt AS mtot, v.totamt AS vtot FROM mvtest_tm m LEFT JOIN mvtest_tv v USING (type) ORDER BY type;

-- make sure that dependencies are reported properly when they block the drop
--dg-cascade
--dg DROP TABLE mvtest_t;

-- make sure dependencies are dropped and reported
-- and make sure that transactional behavior is correct on rollback
-- incidentally leaving some interesting materialized views for pg_dump testing
BEGIN;
--dg-cascade
--dg DROP TABLE mvtest_t CASCADE;
ROLLBACK;

-- some additional tests not using base tables
CREATE VIEW mvtest_vt1 AS SELECT 1 moo;
CREATE VIEW mvtest_vt2 AS SELECT moo, 2*moo FROM mvtest_vt1 UNION ALL SELECT moo, 3*moo FROM mvtest_vt1;
\d+ mvtest_vt2
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mv_test2 AS SELECT moo, 2*moo FROM mvtest_vt2 UNION ALL SELECT moo, 3*moo FROM mvtest_vt2;
--dg-cascade
--dg \d+ mv_test2
--dg-cascade
--dg CREATE MATERIALIZED VIEW mv_test3 AS SELECT * FROM mv_test2 WHERE moo = 12345;
--dg-cascade
--dg SELECT relispopulated FROM pg_class WHERE oid = 'mv_test3'::regclass;

--dg-cascade
--dg DROP VIEW mvtest_vt1 CASCADE;

-- test that duplicate values on unique index prevent refresh
CREATE TABLE mvtest_foo(a, b) AS VALUES(1, 10);
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_mv AS SELECT * FROM mvtest_foo;
--dg-cascade
--dg CREATE UNIQUE INDEX ON mvtest_mv(a);
INSERT INTO mvtest_foo SELECT * FROM mvtest_foo;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_mv;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW CONCURRENTLY mvtest_mv;
--dg-cascade
--dg DROP TABLE mvtest_foo CASCADE;

-- make sure that all columns covered by unique indexes works
--dg-cascade
--dg CREATE TABLE mvtest_foo(a, b, c) AS VALUES(1, 2, 3);
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_mv AS SELECT * FROM mvtest_foo;
--dg-cascade
--dg CREATE UNIQUE INDEX ON mvtest_mv (a);
--dg-cascade
--dg CREATE UNIQUE INDEX ON mvtest_mv (b);
--dg-cascade
--dg CREATE UNIQUE INDEX on mvtest_mv (c);
--dg-cascade
--dg INSERT INTO mvtest_foo VALUES(2, 3, 4);
--dg-cascade
--dg INSERT INTO mvtest_foo VALUES(3, 4, 5);
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_mv;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW CONCURRENTLY mvtest_mv;
--dg-cascade
--dg DROP TABLE mvtest_foo CASCADE;

-- allow subquery to reference unpopulated matview if WITH NO DATA is specified
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_mv1 AS SELECT 1 AS col1 WITH NO DATA;
--dg-cascade
--dg CREATE MATERIALIZED VIEW mvtest_mv2 AS SELECT * FROM mvtest_mv1
--dg   WHERE col1 = (SELECT LEAST(col1) FROM mvtest_mv1) WITH NO DATA;
--dg-cascade
--dg DROP MATERIALIZED VIEW mvtest_mv1 CASCADE;

-- make sure that types with unusual equality tests work
--dg-skip: type ? is not supported yet
--dg CREATE TABLE mvtest_boxes (id serial primary key, b box);
--dg-cascade
--dg INSERT INTO mvtest_boxes (b) VALUES
--dg   ('(32,32),(31,31)'),
--dg   ('(2.0000004,2.0000004),(1,1)'),
--dg   ('(1.9999996,1.9999996),(1,1)');
--dg-cascade
--dg CREATE MATERIALIZED VIEW mvtest_boxmv AS SELECT * FROM mvtest_boxes;
--dg-cascade
--dg CREATE UNIQUE INDEX mvtest_boxmv_id ON mvtest_boxmv (id);
--dg-cascade
--dg UPDATE mvtest_boxes SET b = '(2,2),(1,1)' WHERE id = 2;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW CONCURRENTLY mvtest_boxmv;
--dg-cascade
--dg SELECT * FROM mvtest_boxmv ORDER BY id;
--dg-cascade
--dg DROP TABLE mvtest_boxes CASCADE;

-- make sure that column names are handled correctly
CREATE TABLE mvtest_v (i int, j int);
CREATE MATERIALIZED VIEW mvtest_mv_v (ii, jj, kk) AS SELECT i, j FROM mvtest_v; -- error
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_mv_v (ii, jj) AS SELECT i, j FROM mvtest_v; -- ok
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_mv_v_2 (ii) AS SELECT i, j FROM mvtest_v; -- ok
CREATE MATERIALIZED VIEW mvtest_mv_v_3 (ii, jj, kk) AS SELECT i, j FROM mvtest_v WITH NO DATA; -- error
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_mv_v_3 (ii, jj) AS SELECT i, j FROM mvtest_v WITH NO DATA; -- ok
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_mv_v_4 (ii) AS SELECT i, j FROM mvtest_v WITH NO DATA; -- ok
ALTER TABLE mvtest_v RENAME COLUMN i TO x;
INSERT INTO mvtest_v values (1, 2);
--dg-cascade
--dg CREATE UNIQUE INDEX mvtest_mv_v_ii ON mvtest_mv_v (ii);
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_mv_v;
UPDATE mvtest_v SET j = 3 WHERE x = 1;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW CONCURRENTLY mvtest_mv_v;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_mv_v_2;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_mv_v_3;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW mvtest_mv_v_4;
SELECT * FROM mvtest_v;
--dg-cascade
--dg SELECT * FROM mvtest_mv_v;
--dg-cascade
--dg SELECT * FROM mvtest_mv_v_2;
--dg-cascade
--dg SELECT * FROM mvtest_mv_v_3;
--dg-cascade
--dg SELECT * FROM mvtest_mv_v_4;
--dg-cascade
--dg DROP TABLE mvtest_v CASCADE;

-- Check that unknown literals are converted to "text" in CREATE MATVIEW,
-- so that we don't end up with unknown-type columns.
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mv_unspecified_types AS
--dg   SELECT 42 as i, 42.5 as num, 'foo' as u, 'foo'::unknown as u2, null as n;
--dg-cascade
--dg \d+ mv_unspecified_types
--dg-cascade
--dg SELECT * FROM mv_unspecified_types;
--dg-cascade
--dg DROP MATERIALIZED VIEW mv_unspecified_types;

-- make sure that create WITH NO DATA does not plan the query (bug #13907)
--dg-differs: another error: a relation's storage cannot be made: digres has no catalog tables
--dg create materialized view mvtest_error as select 1/0 as x;  -- fail
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg create materialized view mvtest_error as select 1/0 as x with no data;
--dg-cascade
--dg refresh materialized view mvtest_error;  -- fail here
--dg-cascade
--dg drop materialized view mvtest_error;

-- make sure that matview rows can be referenced as source rows (bug #9398)
--dg-cascade
--dg CREATE TABLE mvtest_v AS SELECT generate_series(1,10) AS a;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mvtest_mv_v AS SELECT a FROM mvtest_v WHERE a <= 5;
--dg-cascade
--dg DELETE FROM mvtest_v WHERE EXISTS ( SELECT * FROM mvtest_mv_v WHERE mvtest_mv_v.a = mvtest_v.a );
--dg-cascade
--dg SELECT * FROM mvtest_v;
--dg-cascade
--dg SELECT * FROM mvtest_mv_v;
--dg-cascade
--dg DROP TABLE mvtest_v CASCADE;

-- make sure running as superuser works when MV owned by another role (bug #11208)
CREATE ROLE regress_user_mvtest;
SET ROLE regress_user_mvtest;
-- this test case also checks for ambiguity in the queries issued by
-- refresh_by_match_merge(), by choosing column names that intentionally
-- duplicate all the aliases used in those queries
CREATE TABLE mvtest_foo_data AS SELECT i,
  i+1 AS tid,
  fipshash(random()::text) AS mv,
  fipshash(random()::text) AS newdata,
  fipshash(random()::text) AS newdata2,
  fipshash(random()::text) AS diff
  FROM generate_series(1, 10) i;
CREATE MATERIALIZED VIEW mvtest_mv_foo AS SELECT * FROM mvtest_foo_data;
CREATE MATERIALIZED VIEW mvtest_mv_foo AS SELECT * FROM mvtest_foo_data;
CREATE MATERIALIZED VIEW IF NOT EXISTS mvtest_mv_foo AS SELECT * FROM mvtest_foo_data;
CREATE UNIQUE INDEX ON mvtest_mv_foo (i);
RESET ROLE;
REFRESH MATERIALIZED VIEW mvtest_mv_foo;
REFRESH MATERIALIZED VIEW CONCURRENTLY mvtest_mv_foo;
DROP OWNED BY regress_user_mvtest CASCADE;
DROP ROLE regress_user_mvtest;

-- Concurrent refresh requires a unique index on the materialized
-- view. Test what happens if it's dropped during the refresh.
SET search_path = mvtest_mvschema, public;
--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION mvtest_drop_the_index()
--dg   RETURNS bool AS $$
--dg BEGIN
--dg   EXECUTE 'DROP INDEX IF EXISTS mvtest_mvschema.mvtest_drop_idx';
--dg   RETURN true;
--dg END;
--dg $$ LANGUAGE plpgsql;

--dg-cascade
--dg CREATE MATERIALIZED VIEW drop_idx_matview AS
--dg   SELECT 1 as i WHERE mvtest_drop_the_index();

--dg-cascade
--dg CREATE UNIQUE INDEX mvtest_drop_idx ON drop_idx_matview (i);
--dg-cascade
--dg REFRESH MATERIALIZED VIEW CONCURRENTLY drop_idx_matview;
--dg-cascade
--dg DROP MATERIALIZED VIEW drop_idx_matview; -- clean up
RESET search_path;

-- make sure that create WITH NO DATA works via SPI
BEGIN;
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION mvtest_func()
--dg   RETURNS void AS $$
--dg BEGIN
--dg   CREATE MATERIALIZED VIEW mvtest1 AS SELECT 1 AS x;
--dg   CREATE MATERIALIZED VIEW mvtest2 AS SELECT 1 AS x WITH NO DATA;
--dg END;
--dg $$ LANGUAGE plpgsql;
--dg-cascade
--dg SELECT mvtest_func();
--dg-cascade
--dg SELECT * FROM mvtest1;
--dg-cascade
--dg SELECT * FROM mvtest2;
ROLLBACK;

-- INSERT privileges if relation owner is not allowed to insert.
CREATE SCHEMA matview_schema;
CREATE USER regress_matview_user;
--dg-skip: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg ALTER DEFAULT PRIVILEGES FOR ROLE regress_matview_user
--dg   REVOKE INSERT ON TABLES FROM regress_matview_user;
GRANT ALL ON SCHEMA matview_schema TO public;

SET SESSION AUTHORIZATION regress_matview_user;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW matview_schema.mv_withdata1 (a) AS
--dg   SELECT generate_series(1, 10) WITH DATA;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
--dg   CREATE MATERIALIZED VIEW matview_schema.mv_withdata2 (a) AS
--dg   SELECT generate_series(1, 10) WITH DATA;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW matview_schema.mv_withdata2;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW matview_schema.mv_nodata1 (a) AS
--dg   SELECT generate_series(1, 10) WITH NO DATA;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
--dg   CREATE MATERIALIZED VIEW matview_schema.mv_nodata2 (a) AS
--dg   SELECT generate_series(1, 10) WITH NO DATA;
--dg-cascade
--dg REFRESH MATERIALIZED VIEW matview_schema.mv_nodata2;
RESET SESSION AUTHORIZATION;

--dg-skip: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg ALTER DEFAULT PRIVILEGES FOR ROLE regress_matview_user
--dg   GRANT INSERT ON TABLES TO regress_matview_user;

--dg-cascade
--dg DROP SCHEMA matview_schema CASCADE;
DROP USER regress_matview_user;

-- CREATE MATERIALIZED VIEW ... IF NOT EXISTS
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW matview_ine_tab AS SELECT 1;
--dg-cascade
--dg CREATE MATERIALIZED VIEW matview_ine_tab AS SELECT 1 / 0; -- error
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW IF NOT EXISTS matview_ine_tab AS
--dg   SELECT 1 / 0; -- ok
--dg-cascade
--dg CREATE MATERIALIZED VIEW matview_ine_tab AS
--dg   SELECT 1 / 0 WITH NO DATA; -- error
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW IF NOT EXISTS matview_ine_tab AS
--dg   SELECT 1 / 0 WITH NO DATA; -- ok
--dg-cascade
--dg EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
--dg   CREATE MATERIALIZED VIEW matview_ine_tab AS
--dg     SELECT 1 / 0; -- error
--dg-cascade
--dg EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
--dg   CREATE MATERIALIZED VIEW IF NOT EXISTS matview_ine_tab AS
--dg     SELECT 1 / 0; -- ok
--dg-cascade
--dg EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
--dg   CREATE MATERIALIZED VIEW matview_ine_tab AS
--dg     SELECT 1 / 0 WITH NO DATA; -- error
--dg-cascade
--dg EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
--dg   CREATE MATERIALIZED VIEW IF NOT EXISTS matview_ine_tab AS
--dg     SELECT 1 / 0 WITH NO DATA; -- ok
--dg-cascade
--dg DROP MATERIALIZED VIEW matview_ine_tab;
