--
-- ARRAYS
--

-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR

--dg-skip: type ? is not supported yet
--dg CREATE TABLE arrtest (
--dg 	a 			int2[],
--dg 	b 			int4[][][],
--dg 	c 			name[],
--dg 	d			text[][],
--dg 	e 			float8[],
--dg 	f			char(5)[],
--dg 	g			varchar(5)[]
--dg );

--dg-skip: type ? is not supported yet
--dg CREATE TABLE array_op_test (
--dg 	seqno		int4,
--dg 	i			int4[],
--dg 	t			text[]
--dg );

\set filename :abs_srcdir '/data/array.data'
--dg-cascade
--dg COPY array_op_test FROM :'filename';
--dg-cascade
--dg ANALYZE array_op_test;

--
-- only the 'e' array is 0-based, the others are 1-based.
--

--dg-cascade
--dg INSERT INTO arrtest (a[1:5], b[1:1][1:2][1:2], c, d, f, g)
--dg    VALUES ('{1,2,3,4,5}', '{{{0,0},{1,2}}}', '{}', '{}', '{}', '{}');

--dg-cascade
--dg UPDATE arrtest SET e[0] = '1.1';

--dg-cascade
--dg UPDATE arrtest SET e[1] = '2.2';

--dg-cascade
--dg INSERT INTO arrtest (f)
--dg    VALUES ('{"too long"}');

--dg-cascade
--dg INSERT INTO arrtest (a, b[1:2][1:2], c, d, e, f, g)
--dg    VALUES ('{11,12,23}', '{{3,4},{4,5}}', '{"foobar"}',
--dg            '{{"elt1", "elt2"}}', '{"3.4", "6.7"}',
--dg            '{"abc","abcde"}', '{"abc","abcde"}');

--dg-cascade
--dg INSERT INTO arrtest (a, b[1:2], c, d[1:2])
--dg    VALUES ('{}', '{3,4}', '{foo,bar}', '{bar,foo}');

--dg-cascade
--dg INSERT INTO arrtest (b[2]) VALUES(now());  -- error, type mismatch

--dg-cascade
--dg INSERT INTO arrtest (b[1:2]) VALUES(now());  -- error, type mismatch

--dg-cascade
--dg SELECT * FROM arrtest;

--dg-cascade
--dg SELECT arrtest.a[1],
--dg           arrtest.b[1][1][1],
--dg           arrtest.c[1],
--dg           arrtest.d[1][1],
--dg           arrtest.e[0]
--dg    FROM arrtest;

--dg-cascade
--dg SELECT a[1], b[1][1][1], c[1], d[1][1], e[0]
--dg    FROM arrtest;

--dg-cascade
--dg SELECT a[1:3],
--dg           b[1:1][1:2][1:2],
--dg           c[1:2],
--dg           d[1:1][1:2]
--dg    FROM arrtest;

--dg-cascade
--dg SELECT array_ndims(a) AS a,array_ndims(b) AS b,array_ndims(c) AS c
--dg    FROM arrtest;

--dg-cascade
--dg SELECT array_dims(a) AS a,array_dims(b) AS b,array_dims(c) AS c
--dg    FROM arrtest;

-- returns nothing
--dg-cascade
--dg SELECT *
--dg    FROM arrtest
--dg    WHERE a[1] < 5 and
--dg          c = '{"foobar"}'::_name;

--dg-cascade
--dg UPDATE arrtest
--dg   SET a[1:2] = '{16,25}'
--dg   WHERE NOT a = '{}'::_int2;

--dg-cascade
--dg UPDATE arrtest
--dg   SET b[1:1][1:1][1:2] = '{113, 117}',
--dg       b[1:1][1:2][2:2] = '{142, 147}'
--dg   WHERE array_dims(b) = '[1:1][1:2][1:2]';

--dg-cascade
--dg UPDATE arrtest
--dg   SET c[2:2] = '{"new_word"}'
--dg   WHERE array_dims(c) is not null;

--dg-cascade
--dg SELECT a,b,c FROM arrtest;

--dg-cascade
--dg SELECT a[1:3],
--dg           b[1:1][1:2][1:2],
--dg           c[1:2],
--dg           d[1:1][2:2]
--dg    FROM arrtest;

--dg-cascade
--dg SELECT b[1:1][2][2],
--dg        d[1:1][2]
--dg    FROM arrtest;

--dg-cascade
--dg INSERT INTO arrtest(a) VALUES('{1,null,3}');
--dg-cascade
--dg SELECT a FROM arrtest;
--dg-cascade
--dg UPDATE arrtest SET a[4] = NULL WHERE a[2] IS NULL;
--dg-cascade
--dg SELECT a FROM arrtest WHERE a[2] IS NULL;
--dg-cascade
--dg DELETE FROM arrtest WHERE a[2] IS NULL AND b IS NULL;
--dg-cascade
--dg SELECT a,b,c FROM arrtest;

-- test non-error-throwing API
SELECT pg_input_is_valid('{1,2,3}', 'integer[]');
SELECT pg_input_is_valid('{1,2', 'integer[]');
SELECT pg_input_is_valid('{1,zed}', 'integer[]');
SELECT * FROM pg_input_error_info('{1,zed}', 'integer[]');

-- test mixed slice/scalar subscripting
select '{{1,2,3},{4,5,6},{7,8,9}}'::int[];
select ('{{1,2,3},{4,5,6},{7,8,9}}'::int[])[1:2][2];
select '[0:2][0:2]={{1,2,3},{4,5,6},{7,8,9}}'::int[];
select ('[0:2][0:2]={{1,2,3},{4,5,6},{7,8,9}}'::int[])[1:2][2];

--
-- check subscription corner cases
--
-- More subscripts than MAXDIM (6)
SELECT ('{}'::int[])[1][2][3][4][5][6][7];
-- NULL index yields NULL when selecting
SELECT ('{{{1},{2},{3}},{{4},{5},{6}}}'::int[])[1][NULL][1];
SELECT ('{{{1},{2},{3}},{{4},{5},{6}}}'::int[])[1][NULL:1][1];
SELECT ('{{{1},{2},{3}},{{4},{5},{6}}}'::int[])[1][1:NULL][1];
-- NULL index in assignment is an error
--dg-cascade
--dg UPDATE arrtest
--dg   SET c[NULL] = '{"can''t assign"}'
--dg   WHERE array_dims(c) is not null;
--dg-cascade
--dg UPDATE arrtest
--dg   SET c[NULL:1] = '{"can''t assign"}'
--dg   WHERE array_dims(c) is not null;
--dg-cascade
--dg UPDATE arrtest
--dg   SET c[1:NULL] = '{"can''t assign"}'
--dg   WHERE array_dims(c) is not null;
-- Un-subscriptable type
SELECT (now())[1];

-- test slices with empty lower and/or upper index
--dg-skip: type ? is not supported yet
--dg CREATE TEMP TABLE arrtest_s (
--dg   a       int2[],
--dg   b       int2[][]
--dg );
--dg-cascade
--dg INSERT INTO arrtest_s VALUES ('{1,2,3,4,5}', '{{1,2,3}, {4,5,6}, {7,8,9}}');
--dg-cascade
--dg INSERT INTO arrtest_s VALUES ('[0:4]={1,2,3,4,5}', '[0:2][0:2]={{1,2,3}, {4,5,6}, {7,8,9}}');

--dg-cascade
--dg SELECT * FROM arrtest_s;
--dg-cascade
--dg SELECT a[:3], b[:2][:2] FROM arrtest_s;
--dg-cascade
--dg SELECT a[2:], b[2:][2:] FROM arrtest_s;
--dg-cascade
--dg SELECT a[:], b[:] FROM arrtest_s;

-- updates
--dg-cascade
--dg UPDATE arrtest_s SET a[:3] = '{11, 12, 13}', b[:2][:2] = '{{11,12}, {14,15}}'
--dg   WHERE array_lower(a,1) = 1;
--dg-cascade
--dg SELECT * FROM arrtest_s;
--dg-cascade
--dg UPDATE arrtest_s SET a[3:] = '{23, 24, 25}', b[2:][2:] = '{{25,26}, {28,29}}';
--dg-cascade
--dg SELECT * FROM arrtest_s;
--dg-cascade
--dg UPDATE arrtest_s SET a[:] = '{11, 12, 13, 14, 15}';
--dg-cascade
--dg SELECT * FROM arrtest_s;
--dg-cascade
--dg UPDATE arrtest_s SET a[:] = '{23, 24, 25}';  -- fail, too small
--dg-cascade
--dg INSERT INTO arrtest_s VALUES(NULL, NULL);
--dg-cascade
--dg UPDATE arrtest_s SET a[:] = '{11, 12, 13, 14, 15}';  -- fail, no good with null

-- we want to work with a point_tbl that includes a null
CREATE TEMP TABLE point_tbl AS SELECT * FROM public.point_tbl;
INSERT INTO POINT_TBL(f1) VALUES (NULL);

-- check with fixed-length-array type, such as point
SELECT f1[0:1] FROM POINT_TBL;
SELECT f1[0:] FROM POINT_TBL;
SELECT f1[:1] FROM POINT_TBL;
SELECT f1[:] FROM POINT_TBL;

-- subscript assignments to fixed-width result in NULL if previous value is NULL
UPDATE point_tbl SET f1[0] = 10 WHERE f1 IS NULL RETURNING *;
INSERT INTO point_tbl(f1[0]) VALUES(0) RETURNING *;
-- NULL assignments get ignored
UPDATE point_tbl SET f1[0] = NULL WHERE f1::text = '(10,10)'::point::text RETURNING *;
-- but non-NULL subscript assignments work
UPDATE point_tbl SET f1[0] = -10, f1[1] = -10 WHERE f1::text = '(10,10)'::point::text RETURNING *;
-- but not to expand the range
UPDATE point_tbl SET f1[3] = 10 WHERE f1::text = '(-10,-10)'::point::text RETURNING *;

--
-- test array extension
--
--dg-skip: type ? is not supported yet
--dg CREATE TEMP TABLE arrtest1 (i int[], t text[]);
--dg-cascade
--dg insert into arrtest1 values(array[1,2,null,4], array['one','two',null,'four']);
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[2] = 22, t[2] = 'twenty-two';
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[5] = 5, t[5] = 'five';
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[8] = 8, t[8] = 'eight';
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[0] = 0, t[0] = 'zero';
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[-3] = -3, t[-3] = 'minus-three';
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[0:2] = array[10,11,12], t[0:2] = array['ten','eleven','twelve'];
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[8:10] = array[18,null,20], t[8:10] = array['p18',null,'p20'];
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[11:12] = array[null,22], t[11:12] = array[null,'p22'];
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[15:16] = array[null,26], t[15:16] = array[null,'p26'];
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[-5:-3] = array[-15,-14,-13], t[-5:-3] = array['m15','m14','m13'];
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[-7:-6] = array[-17,null], t[-7:-6] = array['m17',null];
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[-12:-10] = array[-22,null,-20], t[-12:-10] = array['m22',null,'m20'];
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg delete from arrtest1;
--dg-cascade
--dg insert into arrtest1 values(array[1,2,null,4], array['one','two',null,'four']);
--dg-cascade
--dg select * from arrtest1;
--dg-cascade
--dg update arrtest1 set i[0:5] = array[0,1,2,null,4,5], t[0:5] = array['z','p1','p2',null,'p4','p5'];
--dg-cascade
--dg select * from arrtest1;

--
-- array expressions and operators
--

-- table creation and INSERTs
--dg-skip: type ? is not supported yet
--dg CREATE TEMP TABLE arrtest2 (i integer ARRAY[4], f float8[], n numeric[], t text[], d timestamp[]);
--dg-cascade
--dg INSERT INTO arrtest2 VALUES(
--dg   ARRAY[[[113,142],[1,147]]],
--dg   ARRAY[1.1,1.2,1.3]::float8[],
--dg   ARRAY[1.1,1.2,1.3],
--dg   ARRAY[[['aaa','aab'],['aba','abb'],['aca','acb']],[['baa','bab'],['bba','bbb'],['bca','bcb']]],
--dg   ARRAY['19620326','19931223','19970117']::timestamp[]
--dg );

-- some more test data
CREATE TEMP TABLE arrtest_f (f0 int, f1 text, f2 float8);
insert into arrtest_f values(1,'cat1',1.21);
insert into arrtest_f values(2,'cat1',1.24);
insert into arrtest_f values(3,'cat1',1.18);
insert into arrtest_f values(4,'cat1',1.26);
insert into arrtest_f values(5,'cat1',1.15);
insert into arrtest_f values(6,'cat2',1.15);
insert into arrtest_f values(7,'cat2',1.26);
insert into arrtest_f values(8,'cat2',1.32);
insert into arrtest_f values(9,'cat2',1.30);

CREATE TEMP TABLE arrtest_i (f0 int, f1 text, f2 int);
insert into arrtest_i values(1,'cat1',21);
insert into arrtest_i values(2,'cat1',24);
insert into arrtest_i values(3,'cat1',18);
insert into arrtest_i values(4,'cat1',26);
insert into arrtest_i values(5,'cat1',15);
insert into arrtest_i values(6,'cat2',15);
insert into arrtest_i values(7,'cat2',26);
insert into arrtest_i values(8,'cat2',32);
insert into arrtest_i values(9,'cat2',30);

-- expressions
SELECT t.f[1][3][1] AS "131", t.f[2][2][1] AS "221" FROM (
  SELECT ARRAY[[[111,112],[121,122],[131,132]],[[211,212],[221,122],[231,232]]] AS f
) AS t;
SELECT ARRAY[[[[[['hello'],['world']]]]]];
SELECT ARRAY[ARRAY['hello'],ARRAY['world']];
SELECT ARRAY(select f2 from arrtest_f order by f2) AS "ARRAY";

-- with nulls
SELECT '{1,null,3}'::int[];
SELECT ARRAY[1,NULL,3];

-- functions
SELECT array_append(array[42], 6) AS "{42,6}";
SELECT array_prepend(6, array[42]) AS "{6,42}";
SELECT array_cat(ARRAY[1,2], ARRAY[3,4]) AS "{1,2,3,4}";
SELECT array_cat(ARRAY[1,2], ARRAY[[3,4],[5,6]]) AS "{{1,2},{3,4},{5,6}}";
SELECT array_cat(ARRAY[[3,4],[5,6]], ARRAY[1,2]) AS "{{3,4},{5,6},{1,2}}";

SELECT array_position(ARRAY[1,2,3,4,5], 4);
SELECT array_position(ARRAY[5,3,4,2,1], 4);
SELECT array_position(ARRAY[[1,2],[3,4]], 3);
SELECT array_position(ARRAY['sun','mon','tue','wed','thu','fri','sat'], 'mon');
SELECT array_position(ARRAY['sun','mon','tue','wed','thu','fri','sat'], 'sat');
SELECT array_position(ARRAY['sun','mon','tue','wed','thu','fri','sat'], NULL);
SELECT array_position(ARRAY['sun','mon','tue','wed','thu',NULL,'fri','sat'], NULL);
SELECT array_position(ARRAY['sun','mon','tue','wed','thu',NULL,'fri','sat'], 'sat');

SELECT array_positions(NULL, 10);
SELECT array_positions(NULL, NULL::int);
SELECT array_positions(ARRAY[1,2,3,4,5,6,1,2,3,4,5,6], 4);
SELECT array_positions(ARRAY[[1,2],[3,4]], 4);
SELECT array_positions(ARRAY[1,2,3,4,5,6,1,2,3,4,5,6], NULL);
SELECT array_positions(ARRAY[1,2,3,NULL,5,6,1,2,3,NULL,5,6], NULL);
SELECT array_length(array_positions(ARRAY(SELECT 'AAAAAAAAAAAAAAAAAAAAAAAAA'::text || i % 10
                                          FROM generate_series(1,100) g(i)),
                                  'AAAAAAAAAAAAAAAAAAAAAAAAA5'), 1);

--dg-skip: language "?" does not exist
--dg DO $$
--dg DECLARE
--dg   o int;
--dg   a int[] := ARRAY[1,2,3,2,3,1,2];
--dg BEGIN
--dg   o := array_position(a, 2);
--dg   WHILE o IS NOT NULL
--dg   LOOP
--dg     RAISE NOTICE '%', o;
--dg     o := array_position(a, 2, o + 1);
--dg   END LOOP;
--dg END
--dg $$ LANGUAGE plpgsql;

SELECT array_position('[2:4]={1,2,3}'::int[], 1);
SELECT array_positions('[2:4]={1,2,3}'::int[], 1);

SELECT
    array_position(ids, (1, 1)),
    array_positions(ids, (1, 1))
        FROM
(VALUES
    (ARRAY[(0, 0), (1, 1)]),
    (ARRAY[(1, 1)])
) AS f (ids);

-- operators
--dg-cascade
--dg SELECT a FROM arrtest WHERE b = ARRAY[[[113,142],[1,147]]];
SELECT NOT ARRAY[1.1,1.2,1.3] = ARRAY[1.1,1.2,1.3] AS "FALSE";
SELECT ARRAY[1,2] || 3 AS "{1,2,3}";
SELECT 0 || ARRAY[1,2] AS "{0,1,2}";
SELECT ARRAY[1,2] || ARRAY[3,4] AS "{1,2,3,4}";
SELECT ARRAY[[['hello','world']]] || ARRAY[[['happy','birthday']]] AS "ARRAY";
SELECT ARRAY[[1,2],[3,4]] || ARRAY[5,6] AS "{{1,2},{3,4},{5,6}}";
SELECT ARRAY[0,0] || ARRAY[1,1] || ARRAY[2,2] AS "{0,0,1,1,2,2}";
SELECT 0 || ARRAY[1,2] || 3 AS "{0,1,2,3}";
SELECT ARRAY[1.1] || ARRAY[2,3,4];
SELECT array_agg(x) || array_agg(x) FROM (VALUES (ROW(1,2)), (ROW(3,4))) v(x);
SELECT ROW(1,2) || array_agg(x) FROM (VALUES (ROW(3,4)), (ROW(5,6))) v(x);

--dg-cascade
--dg SELECT * FROM array_op_test WHERE i @> '{32}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i && '{32}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i @> '{17}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i && '{17}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i @> '{32,17}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i && '{32,17}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i <@ '{38,34,32,89}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i = '{}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i @> '{}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i && '{}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i <@ '{}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i = '{NULL}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i @> '{NULL}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i && '{NULL}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE i <@ '{NULL}' ORDER BY seqno;

--dg-cascade
--dg SELECT * FROM array_op_test WHERE t @> '{AAAAAAAA72908}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE t && '{AAAAAAAA72908}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE t @> '{AAAAAAAAAA646}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE t && '{AAAAAAAAAA646}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE t @> '{AAAAAAAA72908,AAAAAAAAAA646}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE t && '{AAAAAAAA72908,AAAAAAAAAA646}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE t <@ '{AAAAAAAA72908,AAAAAAAAAAAAAAAAAAA17075,AA88409,AAAAAAAAAAAAAAAAAA36842,AAAAAAA48038,AAAAAAAAAAAAAA10611}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE t = '{}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE t @> '{}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE t && '{}' ORDER BY seqno;
--dg-cascade
--dg SELECT * FROM array_op_test WHERE t <@ '{}' ORDER BY seqno;

-- array casts
SELECT ARRAY[1,2,3]::text[]::int[]::float8[] AS "{1,2,3}";
SELECT pg_typeof(ARRAY[1,2,3]::text[]::int[]::float8[]) AS "double precision[]";
SELECT ARRAY[['a','bc'],['def','hijk']]::text[]::varchar[] AS "{{a,bc},{def,hijk}}";
SELECT pg_typeof(ARRAY[['a','bc'],['def','hijk']]::text[]::varchar[]) AS "character varying[]";
SELECT CAST(ARRAY[[[[[['a','bb','ccc']]]]]] as text[]) as "{{{{{{a,bb,ccc}}}}}}";
SELECT NULL::text[]::int[] AS "NULL";

-- scalar op any/all (array)
select 33 = any ('{1,2,3}');
select 33 = any ('{1,2,33}');
select 33 = all ('{1,2,33}');
select 33 >= all ('{1,2,33}');
-- boundary cases
select null::int >= all ('{1,2,33}');
select null::int >= all ('{}');
select null::int >= any ('{}');
-- cross-datatype
select 33.4 = any (array[1,2,3]);
select 33.4 > all (array[1,2,3]);
-- errors
select 33 * any ('{1,2,3}');
select 33 * any (44);
-- nulls
select 33 = any (null::int[]);
select null::int = any ('{1,2,3}');
select 33 = any ('{1,null,3}');
select 33 = any ('{1,null,33}');
select 33 = all (null::int[]);
select null::int = all ('{1,2,3}');
select 33 = all ('{1,null,3}');
select 33 = all ('{33,null,33}');
-- nulls later in the bitmap
SELECT -1 != ALL(ARRAY(SELECT NULLIF(g.i, 900) FROM generate_series(1,1000) g(i)));

-- test indexes on arrays
--dg-skip: type ? is not supported yet
--dg create temp table arr_tbl (f1 int[] unique);
--dg-cascade
--dg insert into arr_tbl values ('{1,2,3}');
--dg-cascade
--dg insert into arr_tbl values ('{1,2}');
-- failure expected:
--dg-cascade
--dg insert into arr_tbl values ('{1,2,3}');
--dg-cascade
--dg insert into arr_tbl values ('{2,3,4}');
--dg-cascade
--dg insert into arr_tbl values ('{1,5,3}');
--dg-cascade
--dg insert into arr_tbl values ('{1,2,10}');

set enable_seqscan to off;
set enable_bitmapscan to off;
--dg-cascade
--dg select * from arr_tbl where f1 > '{1,2,3}' and f1 <= '{1,5,3}';
--dg-cascade
--dg select * from arr_tbl where f1 >= '{1,2,3}' and f1 < '{1,5,3}';

-- test ON CONFLICT DO UPDATE with arrays
--dg-skip: type ? is not supported yet
--dg create temp table arr_pk_tbl (pk int4 primary key, f1 int[]);
--dg-cascade
--dg insert into arr_pk_tbl values (1, '{1,2,3}');
--dg-cascade
--dg insert into arr_pk_tbl values (1, '{3,4,5}') on conflict (pk)
--dg   do update set f1[1] = excluded.f1[1], f1[3] = excluded.f1[3]
--dg   returning pk, f1;
--dg-cascade
--dg insert into arr_pk_tbl(pk, f1[1:2]) values (1, '{6,7,8}') on conflict (pk)
--dg   do update set f1[1] = excluded.f1[1],
--dg     f1[2] = excluded.f1[2],
--dg     f1[3] = excluded.f1[3]
--dg   returning pk, f1;

-- note: if above selects don't produce the expected tuple order,
-- then you didn't get an indexscan plan, and something is busted.
reset enable_seqscan;
reset enable_bitmapscan;

-- test subscript overflow detection

-- The normal error message includes a platform-dependent limit,
-- so suppress it to avoid needing multiple expected-files.
\set VERBOSITY sqlstate

--dg-cascade
--dg insert into arr_pk_tbl values(10, '[-2147483648:-2147483647]={1,2}');
--dg-cascade
--dg update arr_pk_tbl set f1[2147483647] = 42 where pk = 10;
--dg-cascade
--dg update arr_pk_tbl set f1[2147483646:2147483647] = array[4,2] where pk = 10;
--dg-cascade
--dg insert into arr_pk_tbl(pk, f1[0:2147483647]) values (2, '{}');
--dg-cascade
--dg insert into arr_pk_tbl(pk, f1[-2147483648:2147483647]) values (2, '{}');

-- also exercise the expanded-array case
--dg-differs: another error: N
--dg do $$ declare a int[];
--dg begin
--dg   a := '[-2147483648:-2147483647]={1,2}'::int[];
--dg   a[2147483647] := 42;
--dg end $$;

\set VERBOSITY default

-- test [not] (like|ilike) (any|all) (...)
select 'foo' like any (array['%a', '%o']); -- t
select 'foo' like any (array['%a', '%b']); -- f
select 'foo' like all (array['f%', '%o']); -- t
select 'foo' like all (array['f%', '%b']); -- f
select 'foo' not like any (array['%a', '%b']); -- t
select 'foo' not like all (array['%a', '%o']); -- f
select 'foo' ilike any (array['%A', '%O']); -- t
select 'foo' ilike all (array['F%', '%O']); -- t

--
-- General array parser tests
--

-- none of the following should be accepted
select '{{1,{2}},{2,3}}'::text[];
select E'{{1,2},\\{2,3}}'::text[];
select '{"a"b}'::text[];
select '{a"b"}'::text[];
select '{"a""b"}'::text[];
select '{{"1 2" x},{3}}'::text[];
select '{{"1 2"} x,{3}}'::text[];
select '{}}'::text[];
select '{ }}'::text[];
select '}{'::text[];
select '{foo{}}'::text[];
select '{"foo"{}}'::text[];
select '{foo,,bar}'::text[];
select '{{1},{{2}}}'::text[];
select '{{{1}},{2}}'::text[];
select '{{},{{}}}'::text[];
select '{{{}},{}}'::text[];
select '{{1},{}}'::text[];
select '{{},{1}}'::text[];
select '[1:0]={}'::int[];
select '[2147483646:2147483647]={1,2}'::int[];
select '[1:-1]={}'::int[];
select '[2]={1}'::int[];
select '[1:]={1}'::int[];
select '[:1]={1}'::int[];
select array[];
select '{{1,},{1},}'::text[];
select '{{1,},{1}}'::text[];
select '{{1,}}'::text[];
select '{1,}'::text[];
select '[21474836488:21474836489]={1,2}'::int[];
select '[-2147483649:-2147483648]={1,2}'::int[];
-- none of the above should be accepted

-- all of the following should be accepted
select '{}'::text[];
select '{{},{}}'::text[];
select '{{{1,2,3,4},{2,3,4,5}},{{3,4,5,6},{4,5,6,7}}}'::text[];
select '{null,n\ull,"null"}'::text[];
select '{ ab\c , "ab\"c" }'::text[];
select '{0 second  ,0 second}'::interval[];
select '{ { "," } , { 3 } }'::text[];
select '  {   {  "  0 second  "   ,  0 second  }   }'::text[];
select '{
           0 second,
           @ 1 hour @ 42 minutes @ 20 seconds
         }'::interval[];
select array[]::text[];
select '[2]={1,7}'::int[];
select '[0:1]={1.1,2.2}'::float8[];
select '[2147483646:2147483646]={1}'::int[];
select '[-2147483648:-2147483647]={1,2}'::int[];
-- all of the above should be accepted

-- some day we might allow these cases, but for now they're errors:
select array[]::oidvector;
select array[]::int2vector;

-- tests for array aggregates
--dg-skip: type ? is not supported yet
--dg CREATE TEMP TABLE arraggtest ( f1 INT[], f2 TEXT[][], f3 FLOAT[]);

--dg-cascade
--dg INSERT INTO arraggtest (f1, f2, f3) VALUES
--dg ('{1,2,3,4}','{{grey,red},{blue,blue}}','{1.6, 0.0}');
--dg-cascade
--dg INSERT INTO arraggtest (f1, f2, f3) VALUES
--dg ('{1,2,3}','{{grey,red},{grey,blue}}','{1.6}');
--dg-cascade
--dg SELECT max(f1), min(f1), max(f2), min(f2), max(f3), min(f3) FROM arraggtest;

--dg-cascade
--dg INSERT INTO arraggtest (f1, f2, f3) VALUES
--dg ('{3,3,2,4,5,6}','{{white,yellow},{pink,orange}}','{2.1,3.3,1.8,1.7,1.6}');
--dg-cascade
--dg SELECT max(f1), min(f1), max(f2), min(f2), max(f3), min(f3) FROM arraggtest;

--dg-cascade
--dg INSERT INTO arraggtest (f1, f2, f3) VALUES
--dg ('{2}','{{black,red},{green,orange}}','{1.6,2.2,2.6,0.4}');
--dg-cascade
--dg SELECT max(f1), min(f1), max(f2), min(f2), max(f3), min(f3) FROM arraggtest;

--dg-cascade
--dg INSERT INTO arraggtest (f1, f2, f3) VALUES
--dg ('{4,2,6,7,8,1}','{{red},{black},{purple},{blue},{blue}}',NULL);
--dg-cascade
--dg SELECT max(f1), min(f1), max(f2), min(f2), max(f3), min(f3) FROM arraggtest;

--dg-cascade
--dg INSERT INTO arraggtest (f1, f2, f3) VALUES
--dg ('{}','{{pink,white,blue,red,grey,orange}}','{2.1,1.87,1.4,2.2}');
--dg-cascade
--dg SELECT max(f1), min(f1), max(f2), min(f2), max(f3), min(f3) FROM arraggtest;

-- A few simple tests for arrays of composite types

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg create type comptype as (f1 int, f2 text);

--dg-cascade
--dg create table comptable (c1 comptype, c2 comptype[]);

-- XXX would like to not have to specify row() construct types here ...
--dg-cascade
--dg insert into comptable
--dg   values (row(1,'foo'), array[row(2,'bar')::comptype, row(3,'baz')::comptype]);

-- check that implicitly named array type _comptype isn't a problem
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type _comptype as enum('fooey');

--dg-cascade
--dg select * from comptable;
--dg-cascade
--dg select c2[2].f2 from comptable;

--dg-cascade
--dg drop type _comptype;
--dg-cascade
--dg drop table comptable;
--dg-cascade
--dg drop type comptype;

--dg-skip: language "?" does not exist
--dg create or replace function unnest1(anyarray)
--dg returns setof anyelement as $$
--dg select $1[s] from generate_subscripts($1,1) g(s);
--dg $$ language sql immutable;

--dg-skip: language "?" does not exist
--dg create or replace function unnest2(anyarray)
--dg returns setof anyelement as $$
--dg select $1[s1][s2] from generate_subscripts($1,1) g1(s1),
--dg                    generate_subscripts($1,2) g2(s2);
--dg $$ language sql immutable;

--dg-cascade
--dg select * from unnest1(array[1,2,3]);
--dg-cascade
--dg select * from unnest2(array[[1,2,3],[4,5,6]]);

--dg-cascade
--dg drop function unnest1(anyarray);
--dg-cascade
--dg drop function unnest2(anyarray);

select array_fill(null::integer, array[3,3],array[2,2]);
select array_fill(null::integer, array[3,3]);
select array_fill(null::text, array[3,3],array[2,2]);
select array_fill(null::text, array[3,3]);
select array_fill(7, array[3,3],array[2,2]);
select array_fill(7, array[3,3]);
select array_fill('juhu'::text, array[3,3],array[2,2]);
select array_fill('juhu'::text, array[3,3]);
select a, a = '{}' as is_eq, array_dims(a)
  from (select array_fill(42, array[0]) as a) ss;
select a, a = '{}' as is_eq, array_dims(a)
  from (select array_fill(42, '{}') as a) ss;
select a, a = '{}' as is_eq, array_dims(a)
  from (select array_fill(42, '{}', '{}') as a) ss;
-- raise exception
select array_fill(1, null, array[2,2]);
select array_fill(1, array[2,2], null);
select array_fill(1, array[2,2], '{}');
select array_fill(1, array[3,3], array[1,1,1]);
select array_fill(1, array[1,2,null]);
select array_fill(1, array[[1,2],[3,4]]);

select string_to_array('1|2|3', '|');
select string_to_array('1|2|3|', '|');
select string_to_array('1||2|3||', '||');
select string_to_array('1|2|3', '');
select string_to_array('', '|');
select string_to_array('1|2|3', NULL);
select string_to_array(NULL, '|') IS NULL;
select string_to_array('abc', '');
select string_to_array('abc', '', 'abc');
select string_to_array('abc', ',');
select string_to_array('abc', ',', 'abc');
select string_to_array('1,2,3,4,,6', ',');
select string_to_array('1,2,3,4,,6', ',', '');
select string_to_array('1,2,3,4,*,6', ',', '*');

select v, v is null as "is null" from string_to_table('1|2|3', '|') g(v);
select v, v is null as "is null" from string_to_table('1|2|3|', '|') g(v);
select v, v is null as "is null" from string_to_table('1||2|3||', '||') g(v);
select v, v is null as "is null" from string_to_table('1|2|3', '') g(v);
select v, v is null as "is null" from string_to_table('', '|') g(v);
select v, v is null as "is null" from string_to_table('1|2|3', NULL) g(v);
select v, v is null as "is null" from string_to_table(NULL, '|') g(v);
select v, v is null as "is null" from string_to_table('abc', '') g(v);
select v, v is null as "is null" from string_to_table('abc', '', 'abc') g(v);
select v, v is null as "is null" from string_to_table('abc', ',') g(v);
select v, v is null as "is null" from string_to_table('abc', ',', 'abc') g(v);
select v, v is null as "is null" from string_to_table('1,2,3,4,,6', ',') g(v);
select v, v is null as "is null" from string_to_table('1,2,3,4,,6', ',', '') g(v);
select v, v is null as "is null" from string_to_table('1,2,3,4,*,6', ',', '*') g(v);

select array_to_string(NULL::int4[], ',') IS NULL;
select array_to_string('{}'::int4[], ',');
select array_to_string(array[1,2,3,4,NULL,6], ',');
select array_to_string(array[1,2,3,4,NULL,6], ',', '*');
select array_to_string(array[1,2,3,4,NULL,6], NULL);
select array_to_string(array[1,2,3,4,NULL,6], ',', NULL);

select array_to_string(string_to_array('1|2|3', '|'), '|');

select array_length(array[1,2,3], 1);
select array_length(array[[1,2,3], [4,5,6]], 0);
select array_length(array[[1,2,3], [4,5,6]], 1);
select array_length(array[[1,2,3], [4,5,6]], 2);
select array_length(array[[1,2,3], [4,5,6]], 3);

select cardinality(NULL::int[]);
select cardinality('{}'::int[]);
select cardinality(array[1,2,3]);
select cardinality('[2:4]={5,6,7}'::int[]);
select cardinality('{{1,2}}'::int[]);
select cardinality('{{1,2},{3,4},{5,6}}'::int[]);
select cardinality('{{{1,9},{5,6}},{{2,3},{3,4}}}'::int[]);

-- array_agg(anynonarray)
select array_agg(unique1) from (select unique1 from tenk1 where unique1 < 15 order by unique1) ss;
select array_agg(ten) from (select ten from tenk1 where unique1 < 15 order by unique1) ss;
select array_agg(nullif(ten, 4)) from (select ten from tenk1 where unique1 < 15 order by unique1) ss;
select array_agg(unique1) from tenk1 where unique1 < -15;

-- array_agg(anyarray)
select array_agg(ar)
  from (values ('{1,2}'::int[]), ('{3,4}'::int[])) v(ar);
select array_agg(distinct ar order by ar desc)
  from (select array[i / 2] from generate_series(1,10) a(i)) b(ar);
select array_agg(ar)
  from (select array_agg(array[i, i+1, i-1])
        from generate_series(1,2) a(i)) b(ar);
select array_agg(array[i+1.2, i+1.3, i+1.4]) from generate_series(1,3) g(i);
select array_agg(array['Hello', i::text]) from generate_series(9,11) g(i);
select array_agg(array[i, nullif(i, 3), i+1]) from generate_series(1,4) g(i);
-- errors
select array_agg('{}'::int[]) from generate_series(1,2);
select array_agg(null::int[]) from generate_series(1,2);
select array_agg(ar)
  from (values ('{1,2}'::int[]), ('{3}'::int[])) v(ar);

select unnest(array[1,2,3]);
select * from unnest(array[1,2,3]);
select unnest(array[1,2,3,4.5]::float8[]);
select unnest(array[1,2,3,4.5]::numeric[]);
select unnest(array[1,2,3,null,4,null,null,5,6]);
select unnest(array[1,2,3,null,4,null,null,5,6]::text[]);
select abs(unnest(array[1,2,null,-3]));
select array_remove(array[1,2,2,3], 2);
select array_remove(array[1,2,2,3], 5);
select array_remove(array[1,NULL,NULL,3], NULL);
select array_remove(array['A','CC','D','C','RR'], 'RR');
select array_remove(array[1.0, 2.1, 3.3], 1);
select array_remove('{{1,2,2},{1,4,3}}', 2); -- not allowed
select array_remove(array['X','X','X'], 'X') = '{}';
select array_replace(array[1,2,5,4],5,3);
select array_replace(array[1,2,5,4],5,NULL);
select array_replace(array[1,2,NULL,4,NULL],NULL,5);
select array_replace(array['A','B','DD','B'],'B','CC');
select array_replace(array[1,NULL,3],NULL,NULL);
select array_replace(array['AB',NULL,'CDE'],NULL,'12');

-- array(select array-value ...)
select array(select array[i,i/2] from generate_series(1,5) i);
select array(select array['Hello', i::text] from generate_series(9,11) i);

-- int2vector and oidvector should be treated as scalar types for this purpose
select pg_typeof(array(select '11 22 33'::int2vector from generate_series(1,5)));
select array(select '11 22 33'::int2vector from generate_series(1,5));
select unnest(array(select '11 22 33'::int2vector from generate_series(1,5)));
select pg_typeof(array(select '11 22 33'::oidvector from generate_series(1,5)));
select array(select '11 22 33'::oidvector from generate_series(1,5));
select unnest(array(select '11 22 33'::oidvector from generate_series(1,5)));

-- array[] should do the same
select pg_typeof(array['11 22 33'::int2vector]);
select array['11 22 33'::int2vector];
select pg_typeof(unnest(array['11 22 33'::int2vector]));
select unnest(array['11 22 33'::int2vector]);
select pg_typeof(unnest('11 22 33'::int2vector));
select unnest('11 22 33'::int2vector);
select pg_typeof(array['11 22 33'::oidvector]);
select array['11 22 33'::oidvector];
select pg_typeof(unnest(array['11 22 33'::oidvector]));
select unnest(array['11 22 33'::oidvector]);
select pg_typeof(unnest('11 22 33'::oidvector));
select unnest('11 22 33'::oidvector);

-- Insert/update on a column that is array of composite

--dg-skip: type "?" does not exist
--dg create temp table t1 (f1 int8_tbl[]);
--dg-cascade
--dg insert into t1 (f1[5].q1) values(42);
--dg-cascade
--dg select * from t1;
--dg-cascade
--dg update t1 set f1[5].q2 = 43;
--dg-cascade
--dg select * from t1;

-- Check that arrays of composites are safely detoasted when needed

create temp table src (f1 text);
insert into src
  select string_agg(random()::text,'') from generate_series(1,10000);
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg create type textandtext as (c1 text, c2 text);
--dg-cascade
--dg create temp table dest (f1 textandtext[]);
--dg-cascade
--dg insert into dest select array[row(f1,f1)::textandtext] from src;
--dg-cascade
--dg select length(fipshash((f1[1]).c2)) from dest;
delete from src;
--dg-cascade
--dg select length(fipshash((f1[1]).c2)) from dest;
truncate table src;
drop table src;
--dg-cascade
--dg select length(fipshash((f1[1]).c2)) from dest;
--dg-cascade
--dg drop table dest;
--dg-cascade
--dg drop type textandtext;

-- Tests for polymorphic-array form of width_bucket()

-- this exercises the varwidth and float8 code paths
SELECT
    op,
    width_bucket(op::numeric, ARRAY[1, 3, 5, 10.0]::numeric[]) AS wb_n1,
    width_bucket(op::numeric, ARRAY[0, 5.5, 9.99]::numeric[]) AS wb_n2,
    width_bucket(op::numeric, ARRAY[-6, -5, 2.0]::numeric[]) AS wb_n3,
    width_bucket(op::float8, ARRAY[1, 3, 5, 10.0]::float8[]) AS wb_f1,
    width_bucket(op::float8, ARRAY[0, 5.5, 9.99]::float8[]) AS wb_f2,
    width_bucket(op::float8, ARRAY[-6, -5, 2.0]::float8[]) AS wb_f3
FROM (VALUES
  (-5.2),
  (-0.0000000001),
  (0.000000000001),
  (1),
  (1.99999999999999),
  (2),
  (2.00000000000001),
  (3),
  (4),
  (4.5),
  (5),
  (5.5),
  (6),
  (7),
  (8),
  (9),
  (9.99999999999999),
  (10),
  (10.0000000000001)
) v(op);

-- ensure float8 path handles NaN properly
SELECT
    op,
    width_bucket(op, ARRAY[1, 3, 9, 'NaN', 'NaN']::float8[]) AS wb
FROM (VALUES
  (-5.2::float8),
  (4::float8),
  (77::float8),
  ('NaN'::float8)
) v(op);

-- these exercise the generic fixed-width code path
SELECT
    op,
    width_bucket(op, ARRAY[1, 3, 5, 10]) AS wb_1
FROM generate_series(0,11) as op;

SELECT width_bucket(now(),
                    array['yesterday', 'today', 'tomorrow']::timestamptz[]);

-- corner cases
SELECT width_bucket(5, ARRAY[3]);
SELECT width_bucket(5, '{}');

-- error cases
SELECT width_bucket('5'::text, ARRAY[3, 4]::integer[]);
SELECT width_bucket(5, ARRAY[3, 4, NULL]);
SELECT width_bucket(5, ARRAY[ARRAY[1, 2], ARRAY[3, 4]]);

-- trim_array

SELECT arr, trim_array(arr, 2)
FROM
(VALUES ('{1,2,3,4,5,6}'::bigint[]),
        ('{1,2}'),
        ('[10:16]={1,2,3,4,5,6,7}'),
        ('[-15:-10]={1,2,3,4,5,6}'),
        ('{{1,10},{2,20},{3,30},{4,40}}')) v(arr);

SELECT trim_array(ARRAY[1, 2, 3], -1); -- fail
SELECT trim_array(ARRAY[1, 2, 3], 10); -- fail
SELECT trim_array(ARRAY[]::int[], 1); -- fail

-- array_shuffle
SELECT array_shuffle('{1,2,3,4,5,6}'::int[]) <@ '{1,2,3,4,5,6}';
SELECT array_shuffle('{1,2,3,4,5,6}'::int[]) @> '{1,2,3,4,5,6}';
SELECT array_dims(array_shuffle('[-1:2][2:3]={{1,2},{3,NULL},{5,6},{7,8}}'::int[]));
SELECT array_dims(array_shuffle('{{{1,2},{3,NULL}},{{5,6},{7,8}},{{9,10},{11,12}}}'::int[]));

-- array_sample
SELECT array_sample('{1,2,3,4,5,6}'::int[], 3) <@ '{1,2,3,4,5,6}';
SELECT array_length(array_sample('{1,2,3,4,5,6}'::int[], 3), 1);
SELECT array_dims(array_sample('[-1:2][2:3]={{1,2},{3,NULL},{5,6},{7,8}}'::int[], 3));
SELECT array_dims(array_sample('{{{1,2},{3,NULL}},{{5,6},{7,8}},{{9,10},{11,12}}}'::int[], 2));
SELECT array_sample('{1,2,3,4,5,6}'::int[], -1); -- fail
SELECT array_sample('{1,2,3,4,5,6}'::int[], 7); --fail

-- array_reverse
SELECT array_reverse('{}'::int[]);
SELECT array_reverse('{1}'::int[]);
SELECT array_reverse('{1,2}'::int[]);
SELECT array_reverse('{1,2,3,NULL,4,5,6}'::int[]);
SELECT array_reverse('{{1,2},{3,4},{5,6},{7,8}}'::int[]);

-- array_sort
SELECT array_sort('{}'::int[]);
SELECT array_sort('{1}'::int[]);
SELECT array_sort('{1,3,5,2,4,6}'::int[]);
SELECT array_sort('{1.1,3.3,5.5,2.2,4.4,6.6}'::numeric[]);
SELECT array_sort('{foo,bar,CCC,Abc,bbc}'::text[] COLLATE "C");
SELECT array_sort('{foo,bar,null,CCC,Abc,bbc}'::text[] COLLATE "C");
SELECT array_sort(ARRAY(SELECT '1 4'::int2vector UNION ALL SELECT '1 2'::int2vector));

-- array_sort with order specified
SELECT array_sort('{1.1,3.3,5.5,2.2,null,4.4,6.6}'::float8[], true);
SELECT array_sort('{1.1,3.3,5.5,2.2,null,4.4,6.6}'::float8[], false);

-- array_sort with order and nullsfirst flag specified
SELECT array_sort('{1.1,3.3,5.5,2.2,null,4.4,6.6}'::float8[], true, true);
SELECT array_sort('{1.1,3.3,5.5,2.2,null,4.4,6.6}'::float8[], true, false);
SELECT array_sort('{1.1,3.3,5.5,2.2,null,4.4,6.6}'::float8[], false, true);
SELECT array_sort('{1.1,3.3,5.5,2.2,null,4.4,6.6}'::float8[], false, false);

-- multidimensional array tests
SELECT array_sort('{{1}}'::int[]);
SELECT array_sort(ARRAY[[2,4],[2,1],[6,5]]);
SELECT array_sort('{{"1 2","3 4"}, {"1 -2","-1 4"}}'::int2vector[]);

-- no ordering operator tests
SELECT array_sort('{1}'::xid[]);  -- no error because no sort is required
SELECT array_sort('{1,2,3}'::xid[]);
SELECT array_sort('{{1,2,3},{2,3,4}}'::xid[]);

-- bounds preservation tests
SELECT array_sort(a) FROM (VALUES ('[10:12][20:21]={{1,2},{10,20},{3,4}}'::int[])) v(a);
SELECT array_sort(a) FROM (VALUES ('[-1:0]={7,1}'::int[])) v(a);
SELECT array_sort(a) FROM (VALUES ('[-2:0][20:21]={{1,2},{10,20},{1,-4}}'::int[])) v(a);
SELECT array_sort(a [-1:0]) FROM (VALUES ('[-2:0][20:21]={{1,2},{10,20},{1,-4}}'::int[])) v(a);
SELECT array_sort(a [-1:0][20:20]) FROM (VALUES ('[-2:0][20:21]={{1,2},{10,20},{1,-4}}'::int[])) v(a);
