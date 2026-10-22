--
-- TEST_SETUP --- prepare environment expected by regression test scripts
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

--
-- synchronous_commit=off delays when hint bits may be set. Some plans change
-- depending on the number of all-visible pages, which in turn can be
-- influenced by the delayed hint bits. Force synchronous_commit=on to avoid
-- that source of variability.
--
--dg-skip: unrecognized configuration parameter "?"
--dg SET synchronous_commit = on;

--
-- Postgres formerly made the public schema read/write by default,
-- and most of the core regression tests still expect that.
--
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg GRANT ALL ON SCHEMA public TO public;

-- Create a tablespace we can use in tests.
--dg-skip: unrecognized configuration parameter "?"
--dg SET allow_in_place_tablespaces = true;
--dg-skip: tablespaces are not supported
--dg CREATE TABLESPACE regress_tblspace LOCATION '';

--
-- These tables have traditionally been referenced by many tests,
-- so create and populate them.  Insert only non-error values here.
-- (Some subsequent tests try to insert erroneous values.  That's okay
-- because the table won't actually change.  Do not change the contents
-- of these tables in later tests, as it may affect other tests.)
--

CREATE TABLE CHAR_TBL(f1 char(4));

INSERT INTO CHAR_TBL (f1) VALUES
  ('a'),
  ('ab'),
  ('abcd'),
  ('abcd    ');
VACUUM CHAR_TBL;

CREATE TABLE FLOAT8_TBL(f1 float8);

INSERT INTO FLOAT8_TBL(f1) VALUES
  ('0.0'),
  ('-34.84'),
  ('-1004.30'),
  ('-1.2345678901234e+200'),
  ('-1.2345678901234e-200');
VACUUM FLOAT8_TBL;

CREATE TABLE INT2_TBL(f1 int2);

INSERT INTO INT2_TBL(f1) VALUES
  ('0   '),
  ('  1234 '),
  ('    -1234'),
  ('32767'),  -- largest and smallest values
  ('-32767');
VACUUM INT2_TBL;

CREATE TABLE INT4_TBL(f1 int4);

INSERT INTO INT4_TBL(f1) VALUES
  ('   0  '),
  ('123456     '),
  ('    -123456'),
  ('2147483647'),  -- largest and smallest values
  ('-2147483647');
VACUUM INT4_TBL;

CREATE TABLE INT8_TBL(q1 int8, q2 int8);

INSERT INTO INT8_TBL VALUES
  ('  123   ','  456'),
  ('123   ','4567890123456789'),
  ('4567890123456789','123'),
  (+4567890123456789,'4567890123456789'),
  ('+4567890123456789','-4567890123456789');
VACUUM INT8_TBL;

--dg-skip: type ? is not supported yet
--dg CREATE TABLE POINT_TBL(f1 point);

--dg-cascade
--dg INSERT INTO POINT_TBL(f1) VALUES
--dg   ('(0.0,0.0)'),
--dg   ('(-10.0,0.0)'),
--dg   ('(-3.0,4.0)'),
--dg   ('(5.1, 34.5)'),
--dg   ('(-5.0,-12.0)'),
--dg   ('(1e-300,-1e-300)'),  -- To underflow
--dg   ('(1e+300,Inf)'),  -- To overflow
--dg   ('(Inf,1e+300)'),  -- Transposed
--dg   (' ( Nan , NaN ) '),
--dg   ('10.0,10.0');
-- We intentionally don't vacuum point_tbl here; geometry depends on that

CREATE TABLE TEXT_TBL (f1 text);

INSERT INTO TEXT_TBL VALUES
  ('doh!'),
  ('hi de ho neighbor');
VACUUM TEXT_TBL;

CREATE TABLE VARCHAR_TBL(f1 varchar(4));

INSERT INTO VARCHAR_TBL (f1) VALUES
  ('a'),
  ('ab'),
  ('abcd'),
  ('abcd    ');
VACUUM VARCHAR_TBL;

CREATE TABLE onek (
	unique1		int4,
	unique2		int4,
	two			int4,
	four		int4,
	ten			int4,
	twenty		int4,
	hundred		int4,
	thousand	int4,
	twothousand	int4,
	fivethous	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
);

\set filename :abs_srcdir '/data/onek.data'
COPY onek FROM :'filename';
VACUUM ANALYZE onek;

CREATE TABLE onek2 AS SELECT * FROM onek;
VACUUM ANALYZE onek2;

CREATE TABLE tenk1 (
	unique1		int4,
	unique2		int4,
	two			int4,
	four		int4,
	ten			int4,
	twenty		int4,
	hundred		int4,
	thousand	int4,
	twothousand	int4,
	fivethous	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
);

\set filename :abs_srcdir '/data/tenk.data'
COPY tenk1 FROM :'filename';
VACUUM ANALYZE tenk1;

CREATE TABLE tenk2 AS SELECT * FROM tenk1;
VACUUM ANALYZE tenk2;

--dg-skip: type ? is not supported yet
--dg CREATE TABLE person (
--dg 	name 		text,
--dg 	age			int4,
--dg 	location 	point
--dg );

\set filename :abs_srcdir '/data/person.data'
--dg-cascade
--dg COPY person FROM :'filename';
--dg-cascade
--dg VACUUM ANALYZE person;

--dg-cascade
--dg CREATE TABLE emp (
--dg 	salary 		int4,
--dg 	manager 	name
--dg ) INHERITS (person);

\set filename :abs_srcdir '/data/emp.data'
--dg-cascade
--dg COPY emp FROM :'filename';
--dg-cascade
--dg VACUUM ANALYZE emp;

--dg-cascade
--dg CREATE TABLE student (
--dg 	gpa 		float8
--dg ) INHERITS (person);

\set filename :abs_srcdir '/data/student.data'
--dg-cascade
--dg COPY student FROM :'filename';
--dg-cascade
--dg VACUUM ANALYZE student;

--dg-cascade
--dg CREATE TABLE stud_emp (
--dg 	percent 	int4
--dg ) INHERITS (emp, student);

\set filename :abs_srcdir '/data/stud_emp.data'
--dg-cascade
--dg COPY stud_emp FROM :'filename';
--dg-cascade
--dg VACUUM ANALYZE stud_emp;

--dg-skip: type ? is not supported yet
--dg CREATE TABLE road (
--dg 	name		text,
--dg 	thepath 	path
--dg );

\set filename :abs_srcdir '/data/streets.data'
--dg-cascade
--dg COPY road FROM :'filename';
--dg-cascade
--dg VACUUM ANALYZE road;

--dg-cascade
--dg CREATE TABLE ihighway () INHERITS (road);

--dg-cascade
--dg INSERT INTO ihighway
--dg    SELECT *
--dg    FROM ONLY road
--dg    WHERE name ~ 'I- .*';
--dg-cascade
--dg VACUUM ANALYZE ihighway;

--dg-cascade
--dg CREATE TABLE shighway (
--dg 	surface		text
--dg ) INHERITS (road);

--dg-cascade
--dg INSERT INTO shighway
--dg    SELECT *, 'asphalt'
--dg    FROM ONLY road
--dg    WHERE name ~ 'State Hwy.*';
--dg-cascade
--dg VACUUM ANALYZE shighway;

--
-- We must have some enum type in the database for opr_sanity and type_sanity.
--

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type stoplight as enum ('red', 'yellow', 'green');

--
-- Also create some non-built-in range types.
--

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type float8range as range (subtype = float8, subtype_diff = float8mi);

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type textrange as range (subtype = text, collation = "C");

--
-- Create some C functions that will be used by various tests.
--

--dg-skip: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION binary_coercible(oid, oid)
--dg     RETURNS bool
--dg     AS :'regresslib', 'binary_coercible'
--dg     LANGUAGE C STRICT STABLE PARALLEL SAFE;

-- Use hand-rolled hash functions and operator classes to get predictable
-- result on different machines.  The hash function for int4 simply returns
-- the sum of the values passed to it and the one for text returns the length
-- of the non-empty string value passed to it or 0.

--dg-skip: language "?" does not exist
--dg create function part_hashint4_noop(value int4, seed int8)
--dg     returns int8 as $$
--dg     select value + seed;
--dg     $$ language sql strict immutable parallel safe;

--dg-skip: relation pg_opfamily cannot be opened: digres has no catalog tables
--dg create operator class part_test_int4_ops for type int4 using hash as
--dg     operator 1 =,
--dg     function 2 part_hashint4_noop(int4, int8);

--dg-skip: language "?" does not exist
--dg create function part_hashtext_length(value text, seed int8)
--dg     returns int8 as $$
--dg     select length(coalesce(value, ''))::int8
--dg     $$ language sql strict immutable parallel safe;

--dg-skip: relation pg_opfamily cannot be opened: digres has no catalog tables
--dg create operator class part_test_text_ops for type text using hash as
--dg     operator 1 =,
--dg     function 2 part_hashtext_length(text, int8);

--
-- These functions are used in tests that used to use md5(), which we now
-- mostly avoid so that the tests will pass in FIPS mode.
--

--dg-skip: language "?" does not exist
--dg create function fipshash(bytea)
--dg     returns text
--dg     strict immutable parallel safe leakproof
--dg     return substr(encode(sha256($1), 'hex'), 1, 32);

--dg-skip: language "?" does not exist
--dg create function fipshash(text)
--dg     returns text
--dg     strict immutable parallel safe leakproof
--dg     return substr(encode(sha256($1::bytea), 'hex'), 1, 32);
