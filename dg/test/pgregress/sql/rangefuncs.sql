CREATE TABLE rngfunc2(rngfuncid int, f2 int);
INSERT INTO rngfunc2 VALUES(1, 11);
INSERT INTO rngfunc2 VALUES(2, 22);
INSERT INTO rngfunc2 VALUES(1, 111);

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION rngfunct(int) returns setof rngfunc2 as 'SELECT * FROM rngfunc2 WHERE rngfuncid = $1 ORDER BY f2;' LANGUAGE SQL;

-- function with ORDINALITY
--dg-cascade
--dg select * from rngfunct(1) with ordinality as z(a,b,ord);
--dg-cascade
--dg select * from rngfunct(1) with ordinality as z(a,b,ord) where b > 100;   -- ordinal 2, not 1
-- ordinality vs. column names and types
--dg-cascade
--dg select a,b,ord from rngfunct(1) with ordinality as z(a,b,ord);
select a,ord from unnest(array['a','b']) with ordinality as z(a,ord);
select * from unnest(array['a','b']) with ordinality as z(a,ord);
select a,ord from unnest(array[1.0::float8]) with ordinality as z(a,ord);
select * from unnest(array[1.0::float8]) with ordinality as z(a,ord);
select row_to_json(s.*) from generate_series(11,14) with ordinality s;
-- ordinality vs. views
--dg-cascade
--dg create temporary view vw_ord as select * from (values (1)) v(n) join rngfunct(1) with ordinality as z(a,b,ord) on (n=ord);
--dg-cascade
--dg select * from vw_ord;
--dg-cascade
--dg select definition from pg_views where viewname='vw_ord';
--dg-cascade
--dg drop view vw_ord;

-- multiple functions
--dg-cascade
--dg select * from rows from(rngfunct(1),rngfunct(2)) with ordinality as z(a,b,c,d,ord);
--dg-cascade
--dg create temporary view vw_ord as select * from (values (1)) v(n) join rows from(rngfunct(1),rngfunct(2)) with ordinality as z(a,b,c,d,ord) on (n=ord);
--dg-cascade
--dg select * from vw_ord;
--dg-cascade
--dg select definition from pg_views where viewname='vw_ord';
--dg-cascade
--dg drop view vw_ord;

-- expansions of unnest()
select * from unnest(array[10,20],array['foo','bar'],array[1.0]);
select * from unnest(array[10,20],array['foo','bar'],array[1.0]) with ordinality as z(a,b,c,ord);
select * from rows from(unnest(array[10,20],array['foo','bar'],array[1.0])) with ordinality as z(a,b,c,ord);
select * from rows from(unnest(array[10,20],array['foo','bar']), generate_series(101,102)) with ordinality as z(a,b,c,ord);
create temporary view vw_ord as select * from unnest(array[10,20],array['foo','bar'],array[1.0]) as z(a,b,c);
select * from vw_ord;
select definition from pg_views where viewname='vw_ord';
drop view vw_ord;
create temporary view vw_ord as select * from rows from(unnest(array[10,20],array['foo','bar'],array[1.0])) as z(a,b,c);
select * from vw_ord;
select definition from pg_views where viewname='vw_ord';
drop view vw_ord;
create temporary view vw_ord as select * from rows from(unnest(array[10,20],array['foo','bar']), generate_series(1,2)) as z(a,b,c);
select * from vw_ord;
select definition from pg_views where viewname='vw_ord';
drop view vw_ord;

-- ordinality and multiple functions vs. rewind and reverse scan
begin;
declare rf_cur scroll cursor for select * from rows from(generate_series(1,5),generate_series(1,2)) with ordinality as g(i,j,o);
fetch all from rf_cur;
fetch backward all from rf_cur;
fetch all from rf_cur;
fetch next from rf_cur;
fetch next from rf_cur;
fetch prior from rf_cur;
fetch absolute 1 from rf_cur;
fetch next from rf_cur;
fetch next from rf_cur;
fetch next from rf_cur;
fetch prior from rf_cur;
fetch prior from rf_cur;
fetch prior from rf_cur;
commit;

-- function with implicit LATERAL
--dg-cascade
--dg select * from rngfunc2, rngfunct(rngfunc2.rngfuncid) z where rngfunc2.f2 = z.f2;

-- function with implicit LATERAL and explicit ORDINALITY
--dg-cascade
--dg select * from rngfunc2, rngfunct(rngfunc2.rngfuncid) with ordinality as z(rngfuncid,f2,ord) where rngfunc2.f2 = z.f2;

-- function in subselect
--dg-cascade
--dg select * from rngfunc2 where f2 in (select f2 from rngfunct(rngfunc2.rngfuncid) z where z.rngfuncid = rngfunc2.rngfuncid) ORDER BY 1,2;

-- function in subselect
--dg-cascade
--dg select * from rngfunc2 where f2 in (select f2 from rngfunct(1) z where z.rngfuncid = rngfunc2.rngfuncid) ORDER BY 1,2;

-- function in subselect
--dg-cascade
--dg select * from rngfunc2 where f2 in (select f2 from rngfunct(rngfunc2.rngfuncid) z where z.rngfuncid = 1) ORDER BY 1,2;

-- nested functions
--dg-cascade
--dg select rngfunct.rngfuncid, rngfunct.f2 from rngfunct(sin(pi()/2)::int) ORDER BY 1,2;

CREATE TABLE rngfunc (rngfuncid int, rngfuncsubid int, rngfuncname text, primary key(rngfuncid,rngfuncsubid));
INSERT INTO rngfunc VALUES(1,1,'Joe');
INSERT INTO rngfunc VALUES(1,2,'Ed');
INSERT INTO rngfunc VALUES(2,1,'Mary');

-- sql, proretset = f, prorettype = b
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION getrngfunc1(int) RETURNS int AS 'SELECT $1;' LANGUAGE SQL;
--dg-cascade
--dg SELECT * FROM getrngfunc1(1) AS t1;
--dg-cascade
--dg SELECT * FROM getrngfunc1(1) WITH ORDINALITY AS t1(v,o);
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc1(1);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc1(1) WITH ORDINALITY as t1(v,o);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;

-- sql, proretset = t, prorettype = b
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION getrngfunc2(int) RETURNS setof int AS 'SELECT rngfuncid FROM rngfunc WHERE rngfuncid = $1;' LANGUAGE SQL;
--dg-cascade
--dg SELECT * FROM getrngfunc2(1) AS t1;
--dg-cascade
--dg SELECT * FROM getrngfunc2(1) WITH ORDINALITY AS t1(v,o);
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc2(1);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc2(1) WITH ORDINALITY AS t1(v,o);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;

-- sql, proretset = t, prorettype = b
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION getrngfunc3(int) RETURNS setof text AS 'SELECT rngfuncname FROM rngfunc WHERE rngfuncid = $1;' LANGUAGE SQL;
--dg-cascade
--dg SELECT * FROM getrngfunc3(1) AS t1;
--dg-cascade
--dg SELECT * FROM getrngfunc3(1) WITH ORDINALITY AS t1(v,o);
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc3(1);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc3(1) WITH ORDINALITY AS t1(v,o);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;

-- sql, proretset = f, prorettype = c
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION getrngfunc4(int) RETURNS rngfunc AS 'SELECT * FROM rngfunc WHERE rngfuncid = $1;' LANGUAGE SQL;
--dg-cascade
--dg SELECT * FROM getrngfunc4(1) AS t1;
--dg-cascade
--dg SELECT * FROM getrngfunc4(1) WITH ORDINALITY AS t1(a,b,c,o);
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc4(1);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc4(1) WITH ORDINALITY AS t1(a,b,c,o);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;

-- sql, proretset = t, prorettype = c
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION getrngfunc5(int) RETURNS setof rngfunc AS 'SELECT * FROM rngfunc WHERE rngfuncid = $1;' LANGUAGE SQL;
--dg-cascade
--dg SELECT * FROM getrngfunc5(1) AS t1;
--dg-cascade
--dg SELECT * FROM getrngfunc5(1) WITH ORDINALITY AS t1(a,b,c,o);
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc5(1);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc5(1) WITH ORDINALITY AS t1(a,b,c,o);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;

-- sql, proretset = f, prorettype = record
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION getrngfunc6(int) RETURNS RECORD AS 'SELECT * FROM rngfunc WHERE rngfuncid = $1;' LANGUAGE SQL;
--dg-cascade
--dg SELECT * FROM getrngfunc6(1) AS t1(rngfuncid int, rngfuncsubid int, rngfuncname text);
--dg-cascade
--dg SELECT * FROM ROWS FROM( getrngfunc6(1) AS (rngfuncid int, rngfuncsubid int, rngfuncname text) ) WITH ORDINALITY;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc6(1) AS
--dg (rngfuncid int, rngfuncsubid int, rngfuncname text);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS
--dg   SELECT * FROM ROWS FROM( getrngfunc6(1) AS (rngfuncid int, rngfuncsubid int, rngfuncname text) )
--dg                 WITH ORDINALITY;
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;

-- sql, proretset = t, prorettype = record
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION getrngfunc7(int) RETURNS setof record AS 'SELECT * FROM rngfunc WHERE rngfuncid = $1;' LANGUAGE SQL;
--dg-cascade
--dg SELECT * FROM getrngfunc7(1) AS t1(rngfuncid int, rngfuncsubid int, rngfuncname text);
--dg-cascade
--dg SELECT * FROM ROWS FROM( getrngfunc7(1) AS (rngfuncid int, rngfuncsubid int, rngfuncname text) ) WITH ORDINALITY;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc7(1) AS
--dg (rngfuncid int, rngfuncsubid int, rngfuncname text);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS
--dg   SELECT * FROM ROWS FROM( getrngfunc7(1) AS (rngfuncid int, rngfuncsubid int, rngfuncname text) )
--dg                 WITH ORDINALITY;
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;

-- plpgsql, proretset = f, prorettype = b
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION getrngfunc8(int) RETURNS int AS 'DECLARE rngfuncint int; BEGIN SELECT rngfuncid into rngfuncint FROM rngfunc WHERE rngfuncid = $1; RETURN rngfuncint; END;' LANGUAGE plpgsql;
--dg-cascade
--dg SELECT * FROM getrngfunc8(1) AS t1;
--dg-cascade
--dg SELECT * FROM getrngfunc8(1) WITH ORDINALITY AS t1(v,o);
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc8(1);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc8(1) WITH ORDINALITY AS t1(v,o);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;

-- plpgsql, proretset = f, prorettype = c
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION getrngfunc9(int) RETURNS rngfunc AS 'DECLARE rngfunctup rngfunc%ROWTYPE; BEGIN SELECT * into rngfunctup FROM rngfunc WHERE rngfuncid = $1; RETURN rngfunctup; END;' LANGUAGE plpgsql;
--dg-cascade
--dg SELECT * FROM getrngfunc9(1) AS t1;
--dg-cascade
--dg SELECT * FROM getrngfunc9(1) WITH ORDINALITY AS t1(a,b,c,o);
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc9(1);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;
--dg-cascade
--dg CREATE VIEW vw_getrngfunc AS SELECT * FROM getrngfunc9(1) WITH ORDINALITY AS t1(a,b,c,o);
--dg-cascade
--dg SELECT * FROM vw_getrngfunc;
--dg-cascade
--dg DROP VIEW vw_getrngfunc;

-- mix 'n match kinds, to exercise expandRTE and related logic

--dg-cascade
--dg select * from rows from(getrngfunc1(1),getrngfunc2(1),getrngfunc3(1),getrngfunc4(1),getrngfunc5(1),
--dg                     getrngfunc6(1) AS (rngfuncid int, rngfuncsubid int, rngfuncname text),
--dg                     getrngfunc7(1) AS (rngfuncid int, rngfuncsubid int, rngfuncname text),
--dg                     getrngfunc8(1),getrngfunc9(1))
--dg               with ordinality as t1(a,b,c,d,e,f,g,h,i,j,k,l,m,o,p,q,r,s,t,u);
--dg-cascade
--dg select * from rows from(getrngfunc9(1),getrngfunc8(1),
--dg                     getrngfunc7(1) AS (rngfuncid int, rngfuncsubid int, rngfuncname text),
--dg                     getrngfunc6(1) AS (rngfuncid int, rngfuncsubid int, rngfuncname text),
--dg                     getrngfunc5(1),getrngfunc4(1),getrngfunc3(1),getrngfunc2(1),getrngfunc1(1))
--dg               with ordinality as t1(a,b,c,d,e,f,g,h,i,j,k,l,m,o,p,q,r,s,t,u);

--dg-cascade
--dg create temporary view vw_rngfunc as
--dg   select * from rows from(getrngfunc9(1),
--dg                       getrngfunc7(1) AS (rngfuncid int, rngfuncsubid int, rngfuncname text),
--dg                       getrngfunc1(1))
--dg                 with ordinality as t1(a,b,c,d,e,f,g,n);
--dg-cascade
--dg select * from vw_rngfunc;
--dg-cascade
--dg select pg_get_viewdef('vw_rngfunc');
--dg-cascade
--dg drop view vw_rngfunc;

--dg-cascade
--dg DROP FUNCTION getrngfunc1(int);
--dg-cascade
--dg DROP FUNCTION getrngfunc2(int);
--dg-cascade
--dg DROP FUNCTION getrngfunc3(int);
--dg-cascade
--dg DROP FUNCTION getrngfunc4(int);
--dg-cascade
--dg DROP FUNCTION getrngfunc5(int);
--dg-cascade
--dg DROP FUNCTION getrngfunc6(int);
--dg-cascade
--dg DROP FUNCTION getrngfunc7(int);
--dg-cascade
--dg DROP FUNCTION getrngfunc8(int);
--dg-cascade
--dg DROP FUNCTION getrngfunc9(int);
--dg-cascade
--dg DROP FUNCTION rngfunct(int);
DROP TABLE rngfunc2;
DROP TABLE rngfunc;

-- Rescan tests --
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not TEMPORARY or UNLOGGED
--dg CREATE TEMPORARY SEQUENCE rngfunc_rescan_seq1;
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not TEMPORARY or UNLOGGED
--dg CREATE TEMPORARY SEQUENCE rngfunc_rescan_seq2;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE rngfunc_rescan_t AS (i integer, s bigint);

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION rngfunc_sql(int,int) RETURNS setof rngfunc_rescan_t AS 'SELECT i, nextval(''rngfunc_rescan_seq1'') FROM generate_series($1,$2) i;' LANGUAGE SQL;
-- plpgsql functions use materialize mode
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION rngfunc_mat(int,int) RETURNS setof rngfunc_rescan_t AS 'begin for i in $1..$2 loop return next (i, nextval(''rngfunc_rescan_seq2'')); end loop; end;' LANGUAGE plpgsql;

--invokes ExecReScanFunctionScan - all these cases should materialize the function only once
-- LEFT JOIN on a condition that the planner can't prove to be true is used to ensure the function
-- is on the inner path of a nestloop join

--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN rngfunc_sql(11,13) ON (r+i)<100;
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN rngfunc_sql(11,13) WITH ORDINALITY AS f(i,s,o) ON (r+i)<100;

--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN rngfunc_mat(11,13) ON (r+i)<100;
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN rngfunc_mat(11,13) WITH ORDINALITY AS f(i,s,o) ON (r+i)<100;
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN ROWS FROM( rngfunc_sql(11,13), rngfunc_mat(11,13) ) WITH ORDINALITY AS f(i1,s1,i2,s2,o) ON (r+i1+i2)<100;

SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN generate_series(11,13) f(i) ON (r+i)<100;
SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN generate_series(11,13) WITH ORDINALITY AS f(i,o) ON (r+i)<100;

SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN unnest(array[10,20,30]) f(i) ON (r+i)<100;
SELECT * FROM (VALUES (1),(2),(3)) v(r) LEFT JOIN unnest(array[10,20,30]) WITH ORDINALITY AS f(i,o) ON (r+i)<100;

--invokes ExecReScanFunctionScan with chgParam != NULL (using implied LATERAL)

--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), rngfunc_sql(10+r,13);
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), rngfunc_sql(10+r,13) WITH ORDINALITY AS f(i,s,o);
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), rngfunc_sql(11,10+r);
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), rngfunc_sql(11,10+r) WITH ORDINALITY AS f(i,s,o);
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (11,12),(13,15),(16,20)) v(r1,r2), rngfunc_sql(r1,r2);
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (11,12),(13,15),(16,20)) v(r1,r2), rngfunc_sql(r1,r2) WITH ORDINALITY AS f(i,s,o);

--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), rngfunc_mat(10+r,13);
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), rngfunc_mat(10+r,13) WITH ORDINALITY AS f(i,s,o);
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), rngfunc_mat(11,10+r);
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), rngfunc_mat(11,10+r) WITH ORDINALITY AS f(i,s,o);
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (11,12),(13,15),(16,20)) v(r1,r2), rngfunc_mat(r1,r2);
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (11,12),(13,15),(16,20)) v(r1,r2), rngfunc_mat(r1,r2) WITH ORDINALITY AS f(i,s,o);

-- selective rescan of multiple functions:

--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), ROWS FROM( rngfunc_sql(11,11), rngfunc_mat(10+r,13) );
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), ROWS FROM( rngfunc_sql(10+r,13), rngfunc_mat(11,11) );
--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM (VALUES (1),(2),(3)) v(r), ROWS FROM( rngfunc_sql(10+r,13), rngfunc_mat(10+r,13) );

--dg-cascade
--dg SELECT setval('rngfunc_rescan_seq1',1,false),setval('rngfunc_rescan_seq2',1,false);
--dg-cascade
--dg SELECT * FROM generate_series(1,2) r1, generate_series(r1,3) r2, ROWS FROM( rngfunc_sql(10+r1,13), rngfunc_mat(10+r2,13) );

SELECT * FROM (VALUES (1),(2),(3)) v(r), generate_series(10+r,20-r) f(i);
SELECT * FROM (VALUES (1),(2),(3)) v(r), generate_series(10+r,20-r) WITH ORDINALITY AS f(i,o);

SELECT * FROM (VALUES (1),(2),(3)) v(r), unnest(array[r*10,r*20,r*30]) f(i);
SELECT * FROM (VALUES (1),(2),(3)) v(r), unnest(array[r*10,r*20,r*30]) WITH ORDINALITY AS f(i,o);

-- deep nesting

SELECT * FROM (VALUES (1),(2),(3)) v1(r1),
              LATERAL (SELECT r1, * FROM (VALUES (10),(20),(30)) v2(r2)
                                         LEFT JOIN generate_series(21,23) f(i) ON ((r2+i)<100) OFFSET 0) s1;
SELECT * FROM (VALUES (1),(2),(3)) v1(r1),
              LATERAL (SELECT r1, * FROM (VALUES (10),(20),(30)) v2(r2)
                                         LEFT JOIN generate_series(20+r1,23) f(i) ON ((r2+i)<100) OFFSET 0) s1;
SELECT * FROM (VALUES (1),(2),(3)) v1(r1),
              LATERAL (SELECT r1, * FROM (VALUES (10),(20),(30)) v2(r2)
                                         LEFT JOIN generate_series(r2,r2+3) f(i) ON ((r2+i)<100) OFFSET 0) s1;
SELECT * FROM (VALUES (1),(2),(3)) v1(r1),
              LATERAL (SELECT r1, * FROM (VALUES (10),(20),(30)) v2(r2)
                                         LEFT JOIN generate_series(r1,2+r2/5) f(i) ON ((r2+i)<100) OFFSET 0) s1;

-- check handling of FULL JOIN with multiple lateral references (bug #15741)

SELECT *
FROM (VALUES (1),(2)) v1(r1)
    LEFT JOIN LATERAL (
        SELECT *
        FROM generate_series(1, v1.r1) AS gs1
        LEFT JOIN LATERAL (
            SELECT *
            FROM generate_series(1, gs1) AS gs2
            LEFT JOIN generate_series(1, gs2) AS gs3 ON TRUE
        ) AS ss1 ON TRUE
        FULL JOIN generate_series(1, v1.r1) AS gs4 ON FALSE
    ) AS ss0 ON TRUE;

--dg-cascade
--dg DROP FUNCTION rngfunc_sql(int,int);
--dg-cascade
--dg DROP FUNCTION rngfunc_mat(int,int);
--dg-cascade
--dg DROP SEQUENCE rngfunc_rescan_seq1;
--dg-cascade
--dg DROP SEQUENCE rngfunc_rescan_seq2;

--
-- Test cases involving OUT parameters
--

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION rngfunc(in f1 int, out f2 int)
--dg AS 'select $1+1' LANGUAGE sql;
--dg-cascade
--dg SELECT rngfunc(42);
--dg-cascade
--dg SELECT * FROM rngfunc(42);
--dg-cascade
--dg SELECT * FROM rngfunc(42) AS p(x);

-- explicit spec of return type is OK
--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION rngfunc(in f1 int, out f2 int) RETURNS int
--dg AS 'select $1+1' LANGUAGE sql;
-- error, wrong result type
--dg-differs: another error: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION rngfunc(in f1 int, out f2 int) RETURNS float
--dg AS 'select $1+1' LANGUAGE sql;
-- with multiple OUT params you must get a RECORD result
--dg-differs: another error: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION rngfunc(in f1 int, out f2 int, out f3 text) RETURNS int
--dg AS 'select $1+1' LANGUAGE sql;
--dg-cascade
--dg CREATE OR REPLACE FUNCTION rngfunc(in f1 int, out f2 int, out f3 text)
--dg RETURNS record
--dg AS 'select $1+1' LANGUAGE sql;

--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION rngfuncr(in f1 int, out f2 int, out text)
--dg AS $$select $1-1, $1::text || 'z'$$ LANGUAGE sql;
--dg-cascade
--dg SELECT f1, rngfuncr(f1) FROM int4_tbl;
--dg-cascade
--dg SELECT * FROM rngfuncr(42);
--dg-cascade
--dg SELECT * FROM rngfuncr(42) AS p(a,b);

--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION rngfuncb(in f1 int, inout f2 int, out text)
--dg AS $$select $2-1, $1::text || 'z'$$ LANGUAGE sql;
--dg-cascade
--dg SELECT f1, rngfuncb(f1, f1/2) FROM int4_tbl;
--dg-cascade
--dg SELECT * FROM rngfuncb(42, 99);
--dg-cascade
--dg SELECT * FROM rngfuncb(42, 99) AS p(a,b);

-- Can reference function with or without OUT params for DROP, etc
--dg-cascade
--dg DROP FUNCTION rngfunc(int);
--dg-cascade
--dg DROP FUNCTION rngfuncr(in f2 int, out f1 int, out text);
--dg-cascade
--dg DROP FUNCTION rngfuncb(in f1 int, inout f2 int);

--
-- For my next trick, polymorphic OUT parameters
--

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION dup (f1 anyelement, f2 out anyelement, f3 out anyarray)
--dg AS 'select $1, array[$1,$1]' LANGUAGE sql;
--dg-cascade
--dg SELECT dup(22);
--dg-cascade
--dg SELECT dup('xyz');	-- fails
--dg-cascade
--dg SELECT dup('xyz'::text);
--dg-cascade
--dg SELECT * FROM dup('xyz'::text);

-- fails, as we are attempting to rename first argument
--dg-cascade
--dg CREATE OR REPLACE FUNCTION dup (inout f2 anyelement, out f3 anyarray)
--dg AS 'select $1, array[$1,$1]' LANGUAGE sql;

--dg-cascade
--dg DROP FUNCTION dup(anyelement);

-- equivalent behavior, though different name exposed for input arg
--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION dup (inout f2 anyelement, out f3 anyarray)
--dg AS 'select $1, array[$1,$1]' LANGUAGE sql;
--dg-cascade
--dg SELECT dup(22);

--dg-cascade
--dg DROP FUNCTION dup(anyelement);

-- fails, no way to deduce outputs
--dg-differs: another error: language "?" does not exist
--dg CREATE FUNCTION bad (f1 int, out f2 anyelement, out f3 anyarray)
--dg AS 'select $1, array[$1,$1]' LANGUAGE sql;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION dup (f1 anycompatible, f2 anycompatiblearray, f3 out anycompatible, f4 out anycompatiblearray)
--dg AS 'select $1, $2' LANGUAGE sql;
--dg-cascade
--dg SELECT dup(22, array[44]);
--dg-cascade
--dg SELECT dup(4.5, array[44]);
--dg-cascade
--dg SELECT dup(22, array[44::bigint]);
--dg-cascade
--dg SELECT *, pg_typeof(f3), pg_typeof(f4) FROM dup(22, array[44::bigint]);

--dg-cascade
--dg DROP FUNCTION dup(f1 anycompatible, f2 anycompatiblearray);

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION dup (f1 anycompatiblerange, f2 out anycompatible, f3 out anycompatiblearray, f4 out anycompatiblerange)
--dg AS 'select lower($1), array[lower($1), upper($1)], $1' LANGUAGE sql;
--dg-cascade
--dg SELECT dup(int4range(4,7));
--dg-cascade
--dg SELECT dup(numrange(4,7));
SELECT dup(textrange('aaa', 'bbb'));

--dg-cascade
--dg DROP FUNCTION dup(f1 anycompatiblerange);

-- fails, no way to deduce outputs
--dg-differs: another error: language "?" does not exist
--dg CREATE FUNCTION bad (f1 anyarray, out f2 anycompatible, out f3 anycompatiblearray)
--dg AS 'select $1, array[$1,$1]' LANGUAGE sql;

--
-- table functions
--

--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION rngfunc()
--dg RETURNS TABLE(a int)
--dg AS $$ SELECT a FROM generate_series(1,5) a(a) $$ LANGUAGE sql;
--dg-cascade
--dg SELECT * FROM rngfunc();
--dg-cascade
--dg DROP FUNCTION rngfunc();

--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION rngfunc(int)
--dg RETURNS TABLE(a int, b int)
--dg AS $$ SELECT a, b
--dg          FROM generate_series(1,$1) a(a),
--dg               generate_series(1,$1) b(b) $$ LANGUAGE sql;
--dg-cascade
--dg SELECT * FROM rngfunc(3);
--dg-cascade
--dg DROP FUNCTION rngfunc(int);

-- case that causes change of typmod knowledge during inlining
--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION rngfunc()
--dg RETURNS TABLE(a varchar(5))
--dg AS $$ SELECT 'hello'::varchar(5) $$ LANGUAGE sql STABLE;
--dg-cascade
--dg SELECT * FROM rngfunc() GROUP BY 1;
--dg-cascade
--dg DROP FUNCTION rngfunc();

--
-- some tests on SQL functions with RETURNING
--

--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not TEMPORARY or UNLOGGED
--dg create temp table tt(f1 serial, data text);

--dg-skip: language "?" does not exist
--dg create function insert_tt(text) returns int as
--dg $$ insert into tt(data) values($1) returning f1 $$
--dg language sql;

--dg-cascade
--dg select insert_tt('foo');
--dg-cascade
--dg select insert_tt('bar');
--dg-cascade
--dg select * from tt;

-- insert will execute to completion even if function needs just 1 row
--dg-skip: language "?" does not exist
--dg create or replace function insert_tt(text) returns int as
--dg $$ insert into tt(data) values($1),($1||$1) returning f1 $$
--dg language sql;

--dg-cascade
--dg select insert_tt('fool');
--dg-cascade
--dg select * from tt;

-- setof does what's expected
--dg-skip: language "?" does not exist
--dg create or replace function insert_tt2(text,text) returns setof int as
--dg $$ insert into tt(data) values($1),($2) returning f1 $$
--dg language sql;

--dg-cascade
--dg select insert_tt2('foolish','barrish');
--dg-cascade
--dg select * from insert_tt2('baz','quux');
--dg-cascade
--dg select * from tt;

-- limit doesn't prevent execution to completion
--dg-cascade
--dg select insert_tt2('foolish','barrish') limit 1;
--dg-cascade
--dg select * from tt;

-- triggers will fire, too
--dg-skip: language "?" does not exist
--dg create function noticetrigger() returns trigger as $$
--dg begin
--dg   raise notice 'noticetrigger % %', new.f1, new.data;
--dg   return null;
--dg end $$ language plpgsql;
--dg-cascade
--dg create trigger tnoticetrigger after insert on tt for each row
--dg execute procedure noticetrigger();

--dg-cascade
--dg select insert_tt2('foolme','barme') limit 1;
--dg-cascade
--dg select * from tt;

-- and rules work
create temp table tt_log(f1 int, data text);

--dg-cascade
--dg create rule insert_tt_rule as on insert to tt do also
--dg   insert into tt_log values(new.*);

--dg-cascade
--dg select insert_tt2('foollog','barlog') limit 1;
--dg-cascade
--dg select * from tt;
-- note that nextval() gets executed a second time in the rule expansion,
-- which is expected.
--dg-cascade
--dg select * from tt_log;

-- test case for a whole-row-variable bug
--dg-skip: language "?" does not exist
--dg create function rngfunc1(n integer, out a text, out b text)
--dg   returns setof record
--dg   language sql
--dg   as $$ select 'foo ' || i, 'bar ' || i from generate_series(1,$1) i $$;

set work_mem='64kB';
--dg-cascade
--dg select t.a, t, t.a from rngfunc1(10000) t limit 1;
reset work_mem;
--dg-cascade
--dg select t.a, t, t.a from rngfunc1(10000) t limit 1;

--dg-cascade
--dg drop function rngfunc1(n integer);

-- test use of SQL functions returning record
-- this is supported in some cases where the query doesn't specify
-- the actual record type ...

--dg-skip: language "?" does not exist
--dg create function array_to_set(anyarray) returns setof record as $$
--dg   select i AS "index", $1[i] AS "value" from generate_subscripts($1, 1) i
--dg $$ language sql strict immutable;

--dg-cascade
--dg select array_to_set(array['one', 'two']);
--dg-cascade
--dg select * from array_to_set(array['one', 'two']) as t(f1 int,f2 text);
--dg-cascade
--dg select * from array_to_set(array['one', 'two']); -- fail
-- after-the-fact coercion of the columns is now possible, too
--dg-cascade
--dg select * from array_to_set(array['one', 'two']) as t(f1 numeric(4,2),f2 text);
-- and if it doesn't work, you get a compile-time not run-time error
--dg-cascade
--dg select * from array_to_set(array['one', 'two']) as t(f1 point,f2 text);

-- with "strict", this function can't be inlined in FROM
--dg-cascade
--dg explain (verbose, costs off)
--dg   select * from array_to_set(array['one', 'two']) as t(f1 numeric(4,2),f2 text);

-- but without, it can be:

--dg-skip: language "?" does not exist
--dg create or replace function array_to_set(anyarray) returns setof record as $$
--dg   select i AS "index", $1[i] AS "value" from generate_subscripts($1, 1) i
--dg $$ language sql immutable;

--dg-cascade
--dg select array_to_set(array['one', 'two']);
--dg-cascade
--dg select * from array_to_set(array['one', 'two']) as t(f1 int,f2 text);
--dg-cascade
--dg select * from array_to_set(array['one', 'two']) as t(f1 numeric(4,2),f2 text);
--dg-cascade
--dg select * from array_to_set(array['one', 'two']) as t(f1 point,f2 text);
--dg-cascade
--dg explain (verbose, costs off)
--dg   select * from array_to_set(array['one', 'two']) as t(f1 numeric(4,2),f2 text);

create temp table rngfunc(f1 int8, f2 int8);

--dg-skip: language "?" does not exist
--dg create function testrngfunc() returns record as $$
--dg   insert into rngfunc values (1,2) returning *;
--dg $$ language sql;

--dg-cascade
--dg select testrngfunc();
--dg-cascade
--dg select * from testrngfunc() as t(f1 int8,f2 int8);
--dg-cascade
--dg select * from testrngfunc(); -- fail

--dg-cascade
--dg drop function testrngfunc();

--dg-skip: language "?" does not exist
--dg create function testrngfunc() returns setof record as $$
--dg   insert into rngfunc values (1,2), (3,4) returning *;
--dg $$ language sql;

--dg-cascade
--dg select testrngfunc();
--dg-cascade
--dg select * from testrngfunc() as t(f1 int8,f2 int8);
--dg-cascade
--dg select * from testrngfunc(); -- fail

--dg-cascade
--dg drop function testrngfunc();

-- Check that typmod imposed by a composite type is honored
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg create type rngfunc_type as (f1 numeric(35,6), f2 numeric(35,2));

--dg-skip: language "?" does not exist
--dg create function testrngfunc() returns rngfunc_type as $$
--dg   select 7.136178319899999964, 7.136178319899999964;
--dg $$ language sql immutable;

--dg-cascade
--dg explain (verbose, costs off)
--dg select testrngfunc();
--dg-cascade
--dg select testrngfunc();
--dg-cascade
--dg explain (verbose, costs off)
--dg select * from testrngfunc();
--dg-cascade
--dg select * from testrngfunc();

--dg-skip: language "?" does not exist
--dg create or replace function testrngfunc() returns rngfunc_type as $$
--dg   select 7.136178319899999964, 7.136178319899999964;
--dg $$ language sql volatile;

--dg-cascade
--dg explain (verbose, costs off)
--dg select testrngfunc();
--dg-cascade
--dg select testrngfunc();
--dg-cascade
--dg explain (verbose, costs off)
--dg select * from testrngfunc();
--dg-cascade
--dg select * from testrngfunc();

--dg-cascade
--dg drop function testrngfunc();

--dg-skip: language "?" does not exist
--dg create function testrngfunc() returns setof rngfunc_type as $$
--dg   select 7.136178319899999964, 7.136178319899999964;
--dg $$ language sql immutable;

--dg-cascade
--dg explain (verbose, costs off)
--dg select testrngfunc();
--dg-cascade
--dg select testrngfunc();
--dg-cascade
--dg explain (verbose, costs off)
--dg select * from testrngfunc();
--dg-cascade
--dg select * from testrngfunc();

--dg-skip: language "?" does not exist
--dg create or replace function testrngfunc() returns setof rngfunc_type as $$
--dg   select 7.136178319899999964, 7.136178319899999964;
--dg $$ language sql volatile;

--dg-cascade
--dg explain (verbose, costs off)
--dg select testrngfunc();
--dg-cascade
--dg select testrngfunc();
--dg-cascade
--dg explain (verbose, costs off)
--dg select * from testrngfunc();
--dg-cascade
--dg select * from testrngfunc();

--dg-skip: language "?" does not exist
--dg create or replace function testrngfunc() returns setof rngfunc_type as $$
--dg   select 1, 2 union select 3, 4 order by 1;
--dg $$ language sql immutable;

--dg-cascade
--dg explain (verbose, costs off)
--dg select testrngfunc();
--dg-cascade
--dg select testrngfunc();
--dg-cascade
--dg explain (verbose, costs off)
--dg select * from testrngfunc();
--dg-cascade
--dg select * from testrngfunc();

-- Check a couple of error cases while we're here
--dg-cascade
--dg select * from testrngfunc() as t(f1 int8,f2 int8);  -- fail, composite result
select * from pg_get_keywords() as t(f1 int8,f2 int8);  -- fail, OUT params
select * from sin(3) as t(f1 int8,f2 int8);  -- fail, scalar result type

--dg-cascade
--dg drop type rngfunc_type cascade;

--
-- Check some cases involving added/dropped columns in a rowtype result
--

create temp table users (userid text, seq int, email text, todrop bool, moredrop int, enabled bool);
insert into users values ('id',1,'email',true,11,true);
insert into users values ('id2',2,'email2',true,12,true);
alter table users drop column todrop;

--dg-skip: language "?" does not exist
--dg create or replace function get_first_user() returns users as
--dg $$ SELECT * FROM users ORDER BY userid LIMIT 1; $$
--dg language sql stable;

--dg-cascade
--dg SELECT get_first_user();
--dg-cascade
--dg SELECT * FROM get_first_user();

--dg-skip: language "?" does not exist
--dg create or replace function get_users() returns setof users as
--dg $$ SELECT * FROM users ORDER BY userid; $$
--dg language sql stable;

--dg-cascade
--dg SELECT get_users();
--dg-cascade
--dg SELECT * FROM get_users();
--dg-cascade
--dg SELECT * FROM get_users() WITH ORDINALITY;   -- make sure ordinality copes

-- multiple functions vs. dropped columns
--dg-cascade
--dg SELECT * FROM ROWS FROM(generate_series(10,11), get_users()) WITH ORDINALITY;
--dg-cascade
--dg SELECT * FROM ROWS FROM(get_users(), generate_series(10,11)) WITH ORDINALITY;

-- check that we can cope with post-parsing changes in rowtypes
--dg-cascade
--dg create temp view usersview as
--dg SELECT * FROM ROWS FROM(get_users(), generate_series(10,11)) WITH ORDINALITY;

--dg-cascade
--dg select * from usersview;
alter table users add column junk text;
--dg-cascade
--dg select * from usersview;

--dg-cascade
--dg alter table users drop column moredrop;  -- fail, view has reference

-- We used to have a bug that would allow the above to succeed, posing
-- hazards for later execution of the view.  Check that the internal
-- defenses for those hazards haven't bit-rotted, in case some other
-- bug with similar symptoms emerges.
begin;

-- destroy the dependency entry that prevents the DROP:
--dg-cascade
--dg delete from pg_depend where
--dg   objid = (select oid from pg_rewrite
--dg            where ev_class = 'usersview'::regclass and rulename = '_RETURN')
--dg   and refobjsubid = 5
--dg returning pg_describe_object(classid, objid, objsubid) as obj,
--dg           pg_describe_object(refclassid, refobjid, refobjsubid) as ref,
--dg           deptype;

--dg-cascade
--dg alter table users drop column moredrop;
--dg-cascade
--dg select * from usersview;  -- expect clean failure
rollback;

--dg-cascade
--dg alter table users alter column seq type numeric;  -- fail, view has reference

-- likewise, check we don't crash if the dependency goes wrong
begin;

-- destroy the dependency entry that prevents the ALTER:
--dg-cascade
--dg delete from pg_depend where
--dg   objid = (select oid from pg_rewrite
--dg            where ev_class = 'usersview'::regclass and rulename = '_RETURN')
--dg   and refobjsubid = 2
--dg returning pg_describe_object(classid, objid, objsubid) as obj,
--dg           pg_describe_object(refclassid, refobjid, refobjsubid) as ref,
--dg           deptype;

--dg-cascade
--dg alter table users alter column seq type numeric;
--dg-cascade
--dg select * from usersview;  -- expect clean failure
rollback;

--dg-cascade
--dg drop view usersview;
--dg-cascade
--dg drop function get_first_user();
--dg-cascade
--dg drop function get_users();
drop table users;

-- check behavior with type coercion required for a set-op

--dg-skip: language "?" does not exist
--dg create or replace function rngfuncbar() returns setof text as
--dg $$ select 'foo'::varchar union all select 'bar'::varchar ; $$
--dg language sql stable;

--dg-cascade
--dg select rngfuncbar();
--dg-cascade
--dg select * from rngfuncbar();
-- this function is now inlinable, too:
--dg-cascade
--dg explain (verbose, costs off) select * from rngfuncbar();

--dg-cascade
--dg drop function rngfuncbar();

-- check handling of a SQL function with multiple OUT params (bug #5777)

--dg-skip: language "?" does not exist
--dg create or replace function rngfuncbar(out integer, out numeric) as
--dg $$ select (1, 2.1) $$ language sql;

--dg-cascade
--dg select * from rngfuncbar();

--dg-skip: language "?" does not exist
--dg create or replace function rngfuncbar(out integer, out numeric) as
--dg $$ select (1, 2) $$ language sql;

--dg-cascade
--dg select * from rngfuncbar();  -- fail

--dg-skip: language "?" does not exist
--dg create or replace function rngfuncbar(out integer, out numeric) as
--dg $$ select (1, 2.1, 3) $$ language sql;

--dg-cascade
--dg select * from rngfuncbar();  -- fail

--dg-cascade
--dg drop function rngfuncbar();

-- check whole-row-Var handling in nested lateral functions (bug #11703)

--dg-skip: language "?" does not exist
--dg create function extractq2(t int8_tbl) returns int8 as $$
--dg   select t.q2
--dg $$ language sql immutable;

--dg-cascade
--dg explain (verbose, costs off)
--dg select x from int8_tbl, extractq2(int8_tbl) f(x);

--dg-cascade
--dg select x from int8_tbl, extractq2(int8_tbl) f(x);

--dg-skip: language "?" does not exist
--dg create function extractq2_2(t int8_tbl) returns table(ret1 int8) as $$
--dg   select extractq2(t) offset 0
--dg $$ language sql immutable;

--dg-cascade
--dg explain (verbose, costs off)
--dg select x from int8_tbl, extractq2_2(int8_tbl) f(x);

--dg-cascade
--dg select x from int8_tbl, extractq2_2(int8_tbl) f(x);

-- without the "offset 0", this function gets optimized quite differently

--dg-skip: language "?" does not exist
--dg create function extractq2_2_opt(t int8_tbl) returns table(ret1 int8) as $$
--dg   select extractq2(t)
--dg $$ language sql immutable;

--dg-cascade
--dg explain (verbose, costs off)
--dg select x from int8_tbl, extractq2_2_opt(int8_tbl) f(x);

--dg-cascade
--dg select x from int8_tbl, extractq2_2_opt(int8_tbl) f(x);

-- check handling of nulls in SRF results (bug #7808)

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg create type rngfunc2 as (a integer, b text);

--dg-cascade
--dg select *, row_to_json(u) from unnest(array[(1,'foo')::rngfunc2, null::rngfunc2]) u;
--dg-cascade
--dg select *, row_to_json(u) from unnest(array[null::rngfunc2, null::rngfunc2]) u;
--dg-cascade
--dg select *, row_to_json(u) from unnest(array[null::rngfunc2, (1,'foo')::rngfunc2, null::rngfunc2]) u;
--dg-cascade
--dg select *, row_to_json(u) from unnest(array[]::rngfunc2[]) u;

--dg-cascade
--dg drop type rngfunc2;

-- check handling of functions pulled up into function RTEs (bug #17227)

--dg-skip: function ?(...) does not exist
--dg explain (verbose, costs off)
--dg select * from
--dg   (select jsonb_path_query_array(module->'lectures', '$[*]') as lecture
--dg    from unnest(array['{"lectures": [{"id": "1"}]}'::jsonb])
--dg         as unnested_modules(module)) as ss,
--dg   jsonb_to_recordset(ss.lecture) as j (id text);

--dg-skip: function ?(...) does not exist
--dg select * from
--dg   (select jsonb_path_query_array(module->'lectures', '$[*]') as lecture
--dg    from unnest(array['{"lectures": [{"id": "1"}]}'::jsonb])
--dg         as unnested_modules(module)) as ss,
--dg   jsonb_to_recordset(ss.lecture) as j (id text);

-- check detection of mismatching record types with a const-folded expression

with a(b) as (values (row(1,2,3)))
select * from a, coalesce(b) as c(d int, e int);  -- fail
with a(b) as (values (row(1,2,3)))
select * from a, coalesce(b) as c(d int, e int, f int, g int);  -- fail
with a(b) as (values (row(1,2,3)))
select * from a, coalesce(b) as c(d int, e int, f float);  -- fail
select * from int8_tbl, coalesce(row(1)) as (a int, b int);  -- fail
