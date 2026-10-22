--
-- CREATE_MISC
--

--
-- a is the type root
-- b and c inherit from a (one-level single inheritance)
-- d inherits from b and c (two-level multiple inheritance)
-- e inherits from c (two-level single inheritance)
-- f inherits from e (three-level single inheritance)
--
CREATE TABLE a_star (
	class		char,
	a 			int4
);

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE b_star (
--dg 	b 			text
--dg ) INHERITS (a_star);

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE c_star (
--dg 	c 			name
--dg ) INHERITS (a_star);

--dg-cascade
--dg CREATE TABLE d_star (
--dg 	d 			float8
--dg ) INHERITS (b_star, c_star);

--dg-cascade
--dg CREATE TABLE e_star (
--dg 	e 			int2
--dg ) INHERITS (c_star);

--dg-cascade
--dg CREATE TABLE f_star (
--dg 	f 			polygon
--dg ) INHERITS (e_star);

INSERT INTO a_star (class, a) VALUES ('a', 1);

INSERT INTO a_star (class, a) VALUES ('a', 2);

INSERT INTO a_star (class) VALUES ('a');

--dg-cascade
--dg INSERT INTO b_star (class, a, b) VALUES ('b', 3, 'mumble'::text);

--dg-cascade
--dg INSERT INTO b_star (class, a) VALUES ('b', 4);

--dg-cascade
--dg INSERT INTO b_star (class, b) VALUES ('b', 'bumble'::text);

--dg-cascade
--dg INSERT INTO b_star (class) VALUES ('b');

--dg-cascade
--dg INSERT INTO c_star (class, a, c) VALUES ('c', 5, 'hi mom'::name);

--dg-cascade
--dg INSERT INTO c_star (class, a) VALUES ('c', 6);

--dg-cascade
--dg INSERT INTO c_star (class, c) VALUES ('c', 'hi paul'::name);

--dg-cascade
--dg INSERT INTO c_star (class) VALUES ('c');

--dg-cascade
--dg INSERT INTO d_star (class, a, b, c, d)
--dg    VALUES ('d', 7, 'grumble'::text, 'hi sunita'::name, '0.0'::float8);

--dg-cascade
--dg INSERT INTO d_star (class, a, b, c)
--dg    VALUES ('d', 8, 'stumble'::text, 'hi koko'::name);

--dg-cascade
--dg INSERT INTO d_star (class, a, b, d)
--dg    VALUES ('d', 9, 'rumble'::text, '1.1'::float8);

--dg-cascade
--dg INSERT INTO d_star (class, a, c, d)
--dg    VALUES ('d', 10, 'hi kristin'::name, '10.01'::float8);

--dg-cascade
--dg INSERT INTO d_star (class, b, c, d)
--dg    VALUES ('d', 'crumble'::text, 'hi boris'::name, '100.001'::float8);

--dg-cascade
--dg INSERT INTO d_star (class, a, b)
--dg    VALUES ('d', 11, 'fumble'::text);

--dg-cascade
--dg INSERT INTO d_star (class, a, c)
--dg    VALUES ('d', 12, 'hi avi'::name);

--dg-cascade
--dg INSERT INTO d_star (class, a, d)
--dg    VALUES ('d', 13, '1000.0001'::float8);

--dg-cascade
--dg INSERT INTO d_star (class, b, c)
--dg    VALUES ('d', 'tumble'::text, 'hi andrew'::name);

--dg-cascade
--dg INSERT INTO d_star (class, b, d)
--dg    VALUES ('d', 'humble'::text, '10000.00001'::float8);

--dg-cascade
--dg INSERT INTO d_star (class, c, d)
--dg    VALUES ('d', 'hi ginger'::name, '100000.000001'::float8);

--dg-cascade
--dg INSERT INTO d_star (class, a) VALUES ('d', 14);

--dg-cascade
--dg INSERT INTO d_star (class, b) VALUES ('d', 'jumble'::text);

--dg-cascade
--dg INSERT INTO d_star (class, c) VALUES ('d', 'hi jolly'::name);

--dg-cascade
--dg INSERT INTO d_star (class, d) VALUES ('d', '1000000.0000001'::float8);

--dg-cascade
--dg INSERT INTO d_star (class) VALUES ('d');

--dg-cascade
--dg INSERT INTO e_star (class, a, c, e)
--dg    VALUES ('e', 15, 'hi carol'::name, '-1'::int2);

--dg-cascade
--dg INSERT INTO e_star (class, a, c)
--dg    VALUES ('e', 16, 'hi bob'::name);

--dg-cascade
--dg INSERT INTO e_star (class, a, e)
--dg    VALUES ('e', 17, '-2'::int2);

--dg-cascade
--dg INSERT INTO e_star (class, c, e)
--dg    VALUES ('e', 'hi michelle'::name, '-3'::int2);

--dg-cascade
--dg INSERT INTO e_star (class, a)
--dg    VALUES ('e', 18);

--dg-cascade
--dg INSERT INTO e_star (class, c)
--dg    VALUES ('e', 'hi elisa'::name);

--dg-cascade
--dg INSERT INTO e_star (class, e)
--dg    VALUES ('e', '-4'::int2);

--dg-cascade
--dg INSERT INTO f_star (class, a, c, e, f)
--dg    VALUES ('f', 19, 'hi claire'::name, '-5'::int2, '(1,3),(2,4)'::polygon);

--dg-cascade
--dg INSERT INTO f_star (class, a, c, e)
--dg    VALUES ('f', 20, 'hi mike'::name, '-6'::int2);

--dg-cascade
--dg INSERT INTO f_star (class, a, c, f)
--dg    VALUES ('f', 21, 'hi marcel'::name, '(11,44),(22,55),(33,66)'::polygon);

--dg-cascade
--dg INSERT INTO f_star (class, a, e, f)
--dg    VALUES ('f', 22, '-7'::int2, '(111,555),(222,666),(333,777),(444,888)'::polygon);

--dg-cascade
--dg INSERT INTO f_star (class, c, e, f)
--dg    VALUES ('f', 'hi keith'::name, '-8'::int2,
--dg 	   '(1111,3333),(2222,4444)'::polygon);

--dg-cascade
--dg INSERT INTO f_star (class, a, c)
--dg    VALUES ('f', 24, 'hi marc'::name);

--dg-cascade
--dg INSERT INTO f_star (class, a, e)
--dg    VALUES ('f', 25, '-9'::int2);

--dg-cascade
--dg INSERT INTO f_star (class, a, f)
--dg    VALUES ('f', 26, '(11111,33333),(22222,44444)'::polygon);

--dg-cascade
--dg INSERT INTO f_star (class, c, e)
--dg    VALUES ('f', 'hi allison'::name, '-10'::int2);

--dg-cascade
--dg INSERT INTO f_star (class, c, f)
--dg    VALUES ('f', 'hi jeff'::name,
--dg            '(111111,333333),(222222,444444)'::polygon);

--dg-cascade
--dg INSERT INTO f_star (class, e, f)
--dg    VALUES ('f', '-11'::int2, '(1111111,3333333),(2222222,4444444)'::polygon);

--dg-cascade
--dg INSERT INTO f_star (class, a) VALUES ('f', 27);

--dg-cascade
--dg INSERT INTO f_star (class, c) VALUES ('f', 'hi carl'::name);

--dg-cascade
--dg INSERT INTO f_star (class, e) VALUES ('f', '-12'::int2);

--dg-cascade
--dg INSERT INTO f_star (class, f)
--dg    VALUES ('f', '(11111111,33333333),(22222222,44444444)'::polygon);

--dg-cascade
--dg INSERT INTO f_star (class) VALUES ('f');

-- Analyze the X_star tables for better plan stability in later tests
ANALYZE a_star;
--dg-cascade
--dg ANALYZE b_star;
--dg-cascade
--dg ANALYZE c_star;
--dg-cascade
--dg ANALYZE d_star;
--dg-cascade
--dg ANALYZE e_star;
--dg-cascade
--dg ANALYZE f_star;

--
-- inheritance stress test
--
--dg-cascade
--dg SELECT * FROM a_star*;

--dg-cascade
--dg SELECT *
--dg    FROM b_star* x
--dg    WHERE x.b = text 'bumble' or x.a < 3;

--dg-cascade
--dg SELECT class, a
--dg    FROM c_star* x
--dg    WHERE x.c ~ text 'hi';

--dg-cascade
--dg SELECT class, b, c
--dg    FROM d_star* x
--dg    WHERE x.a < 100;

--dg-cascade
--dg SELECT class, c FROM e_star* x WHERE x.c NOTNULL;

--dg-cascade
--dg SELECT * FROM f_star* x WHERE x.c ISNULL;

-- grouping and aggregation on inherited sets have been busted in the past...

--dg-cascade
--dg SELECT sum(a) FROM a_star*;

--dg-cascade
--dg SELECT class, sum(a) FROM a_star* GROUP BY class ORDER BY class;


--dg-cascade
--dg ALTER TABLE f_star RENAME COLUMN f TO ff;

--dg-cascade
--dg ALTER TABLE e_star* RENAME COLUMN e TO ee;

--dg-cascade
--dg ALTER TABLE d_star* RENAME COLUMN d TO dd;

--dg-cascade
--dg ALTER TABLE c_star* RENAME COLUMN c TO cc;

--dg-cascade
--dg ALTER TABLE b_star* RENAME COLUMN b TO bb;

ALTER TABLE a_star* RENAME COLUMN a TO aa;

--dg-cascade
--dg SELECT class, aa
--dg    FROM a_star* x
--dg    WHERE aa ISNULL;

-- As of Postgres 7.1, ALTER implicitly recurses,
-- so this should be same as ALTER a_star*

ALTER TABLE a_star RENAME COLUMN aa TO foo;

--dg-cascade
--dg SELECT class, foo
--dg    FROM a_star* x
--dg    WHERE x.foo >= 2;

ALTER TABLE a_star RENAME COLUMN foo TO aa;

--dg-cascade
--dg SELECT *
--dg    from a_star*
--dg    WHERE aa < 1000;

--dg-cascade
--dg ALTER TABLE f_star ADD COLUMN f int4;

--dg-cascade
--dg UPDATE f_star SET f = 10;

--dg-cascade
--dg ALTER TABLE e_star* ADD COLUMN e int4;

--UPDATE e_star* SET e = 42;

--dg-cascade
--dg SELECT * FROM e_star*;

--dg-cascade
--dg ALTER TABLE a_star* ADD COLUMN a text;

-- That ALTER TABLE should have added TOAST tables.
--dg-cascade
--dg SELECT relname, reltoastrelid <> 0 AS has_toast_table
--dg    FROM pg_class
--dg    WHERE oid::regclass IN ('a_star', 'c_star')
--dg    ORDER BY 1;

--UPDATE b_star*
--   SET a = text 'gazpacho'
--   WHERE aa > 4;

--dg-cascade
--dg SELECT class, aa, a FROM a_star*;
