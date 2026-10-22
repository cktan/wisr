--
-- COPY
--

-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR
\getenv abs_builddir PG_ABS_BUILDDIR

--- test copying in CSV mode with various styles
--- of embedded line ending characters

create temp table copytest (
	style	text,
	test 	text,
	filler	int);

insert into copytest values('DOS',E'abc\r\ndef',1);
insert into copytest values('Unix',E'abc\ndef',2);
insert into copytest values('Mac',E'abc\rdef',3);
insert into copytest values(E'esc\\ape',E'a\\r\\\r\\\n\\nb',4);

\set filename :abs_builddir '/results/copytest.csv'
copy copytest to :'filename' csv;

create temp table copytest2 (like copytest);

copy copytest2 from :'filename' csv;

select * from copytest except select * from copytest2;

truncate copytest2;

--- same test but with an escape char different from quote char

copy copytest to :'filename' csv quote '''' escape E'\\';

copy copytest2 from :'filename' csv quote '''' escape E'\\';

select * from copytest except select * from copytest2;

--- test unquoted \. as data inside CSV
-- do not use copy out to export the data, as it would quote \.
\o :filename
\qecho line1
\qecho '\\.'
\qecho line2
\o
-- get the data back in with copy
truncate copytest2;
copy copytest2(test) from :'filename' csv;
select test from copytest2 order by test collate "C";

-- in text mode, \. must be alone on its line
truncate copytest2;
copy copytest2(test) from stdin;
line1
line2
foo\.
line3
\.
copy copytest2(test) from stdin;
line4
line5
\.foo
line6
\.
select test from copytest2;


-- test header line feature

create temp table copytest3 (
	c1 int,
	"col with , comma" text,
	"col with "" quote"  int);

copy copytest3 from stdin csv header;
this is just a line full of junk that would error out if parsed
1,a,1
2,b,2
\.

copy copytest3 to stdout csv header;

create temp table copytest4 (
	c1 int,
	"colname with tab: 	" text);

copy copytest4 from stdin (header);
this is just a line full of junk that would error out if parsed
1	a
2	b
\.

copy copytest4 to stdout (header);

-- test copy from with a partitioned table
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_copytest (
--dg 	a int,
--dg 	b int,
--dg 	c text
--dg ) partition by list (b);

create table parted_copytest_a1 (c text, b int, a int);
create table parted_copytest_a2 (a int, c text, b int);

--dg-cascade
--dg alter table parted_copytest attach partition parted_copytest_a1 for values in(1);
--dg-cascade
--dg alter table parted_copytest attach partition parted_copytest_a2 for values in(2);

-- We must insert enough rows to trigger multi-inserts.  These are only
-- enabled adaptively when there are few enough partition changes.
--dg-cascade
--dg insert into parted_copytest select x,1,'One' from generate_series(1,1000) x;
--dg-cascade
--dg insert into parted_copytest select x,2,'Two' from generate_series(1001,1010) x;
--dg-cascade
--dg insert into parted_copytest select x,1,'One' from generate_series(1011,1020) x;

\set filename :abs_builddir '/results/parted_copytest.csv'
--dg-cascade
--dg copy (select * from parted_copytest order by a) to :'filename';

--dg-cascade
--dg truncate parted_copytest;

--dg-cascade
--dg copy parted_copytest from :'filename';

-- Ensure COPY FREEZE errors for partitioned tables.
begin;
--dg-cascade
--dg truncate parted_copytest;
--dg-cascade
--dg copy parted_copytest from :'filename' (freeze);
rollback;

--dg-cascade
--dg select tableoid::regclass,count(*),sum(a) from parted_copytest
--dg group by tableoid order by tableoid::regclass::name;

--dg-cascade
--dg truncate parted_copytest;

-- create before insert row trigger on parted_copytest_a2
--dg-skip: language "?" does not exist
--dg create function part_ins_func() returns trigger language plpgsql as $$
--dg begin
--dg   return new;
--dg end;
--dg $$;

--dg-cascade
--dg create trigger part_ins_trig
--dg 	before insert on parted_copytest_a2
--dg 	for each row
--dg 	execute procedure part_ins_func();

--dg-cascade
--dg copy parted_copytest from :'filename';

--dg-cascade
--dg select tableoid::regclass,count(*),sum(a) from parted_copytest
--dg group by tableoid order by tableoid::regclass::name;

--dg-cascade
--dg truncate table parted_copytest;
--dg-cascade
--dg create index on parted_copytest (b);
--dg-skip: relation pg_trigger cannot be opened: digres has no catalog tables
--dg drop trigger part_ins_trig on parted_copytest_a2;

--dg-cascade
--dg copy parted_copytest from stdin;
--dg 1	1	str1
--dg 2	2	str2
--dg \.

-- Ensure index entries were properly added during the copy.
--dg-cascade
--dg select * from parted_copytest where b = 1;
--dg-cascade
--dg select * from parted_copytest where b = 2;

--dg-cascade
--dg drop table parted_copytest;

--
-- Progress reporting for COPY
--
--dg-skip: type ? is not supported yet
--dg create table tab_progress_reporting (
--dg 	name text,
--dg 	age int4,
--dg 	location point,
--dg 	salary int4,
--dg 	manager name
--dg );

-- Add a trigger to catch and print the contents of the catalog view
-- pg_stat_progress_copy during data insertion.  This allows to test
-- the validation of some progress reports for COPY FROM where the trigger
-- would fire.
--dg-skip: language "?" does not exist
--dg create function notice_after_tab_progress_reporting() returns trigger AS
--dg $$
--dg declare report record;
--dg begin
--dg   -- The fields ignored here are the ones that may not remain
--dg   -- consistent across multiple runs.  The sizes reported may differ
--dg   -- across platforms, so just check if these are strictly positive.
--dg   with progress_data as (
--dg     select
--dg        relid::regclass::text as relname,
--dg        command,
--dg        type,
--dg        bytes_processed > 0 as has_bytes_processed,
--dg        bytes_total > 0 as has_bytes_total,
--dg        tuples_processed,
--dg        tuples_excluded,
--dg        tuples_skipped
--dg       from pg_stat_progress_copy
--dg       where pid = pg_backend_pid())
--dg   select into report (to_jsonb(r)) as value
--dg     from progress_data r;
--dg
--dg   raise info 'progress: %', report.value::text;
--dg   return new;
--dg end;
--dg $$ language plpgsql;

--dg-cascade
--dg create trigger check_after_tab_progress_reporting
--dg 	after insert on tab_progress_reporting
--dg 	for each statement
--dg 	execute function notice_after_tab_progress_reporting();

-- Generate COPY FROM report with PIPE.
--dg-cascade
--dg copy tab_progress_reporting from stdin;
--dg sharon	25	(15,12)	1000	sam
--dg sam	30	(10,5)	2000	bill
--dg bill	20	(11,10)	1000	sharon
--dg \.

-- Generate COPY FROM report with FILE, with some excluded tuples.
--dg-cascade
--dg truncate tab_progress_reporting;
\set filename :abs_srcdir '/data/emp.data'
--dg-cascade
--dg copy tab_progress_reporting from :'filename'
--dg 	where (salary < 2000);

-- Generate COPY FROM report with PIPE, with some skipped tuples.
--dg-cascade
--dg copy tab_progress_reporting from stdin(on_error ignore);
--dg sharon	x	(15,12)	x	sam
--dg sharon	25	(15,12)	1000	sam
--dg sharon	y	(15,12)	x	sam
--dg \.

--dg-cascade
--dg drop trigger check_after_tab_progress_reporting on tab_progress_reporting;
--dg-cascade
--dg drop function notice_after_tab_progress_reporting();
--dg-cascade
--dg drop table tab_progress_reporting;

-- Test header matching feature
create table header_copytest (
	a int,
	b int,
	c text
);
-- Make sure it works with dropped columns
alter table header_copytest drop column c;
alter table header_copytest add column c text;
copy header_copytest to stdout with (header match);
copy header_copytest from stdin with (header wrong_choice);
\.
-- works
copy header_copytest from stdin with (header match);
a	b	c
1	2	foo
\.
copy header_copytest (c, a, b) from stdin with (header match);
c	a	b
bar	3	4
\.
copy header_copytest from stdin with (header match, format csv);
a,b,c
5,6,baz
\.
-- errors
copy header_copytest (c, b, a) from stdin with (header match);
a	b	c
1	2	foo
\.
copy header_copytest from stdin with (header match);
a	b	\N
1	2	foo
\.
copy header_copytest from stdin with (header match);
a	b
1	2
\.
copy header_copytest from stdin with (header match);
a	b	c	d
1	2	foo	bar
\.
copy header_copytest from stdin with (header match);
a	b	d
1	2	foo
\.
SELECT * FROM header_copytest ORDER BY a;

-- Drop an extra column, in the middle of the existing set.
alter table header_copytest drop column b;
-- works
copy header_copytest (c, a) from stdin with (header match);
c	a
foo	7
\.
copy header_copytest (a, c) from stdin with (header match);
a	c
8	foo
\.
-- errors
copy header_copytest from stdin with (header match);
a	........pg.dropped.2........	c
1	2	foo
\.
copy header_copytest (a, c) from stdin with (header match);
a	c	b
1	foo	2
\.

SELECT * FROM header_copytest ORDER BY a;
drop table header_copytest;

-- test COPY with overlong column defaults
create temp table oversized_column_default (
    col1 varchar(5) DEFAULT 'more than 5 chars',
    col2 varchar(5));
-- normal COPY should work
copy oversized_column_default from stdin;
\.
-- error if the column is excluded
copy oversized_column_default (col2) from stdin;
\.
-- error if the DEFAULT option is given
copy oversized_column_default from stdin (default '');
\.
drop table oversized_column_default;


--
-- Create partitioned table that does not allow bulk insertions, to test bugs
-- related to the reuse of BulkInsertState across partitions (only done when
-- not using bulk insert).  Switching between partitions often makes it more
-- likely to encounter these bugs, so we just switch on roughly every insert
-- by having an even/odd number partition and inserting evenly distributed
-- data.
--
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE parted_si (
--dg   id int not null,
--dg   data text not null,
--dg   -- prevent use of bulk insert by having a volatile function
--dg   rand float8 not null default random()
--dg )
--dg PARTITION BY LIST((id % 2));

--dg-cascade
--dg CREATE TABLE parted_si_p_even PARTITION OF parted_si FOR VALUES IN (0);
--dg-cascade
--dg CREATE TABLE parted_si_p_odd PARTITION OF parted_si FOR VALUES IN (1);

-- Test that bulk relation extension handles reusing a single BulkInsertState
-- across partitions.  Without the fix applied, this reliably reproduces
-- #18130 unless shared_buffers is extremely small (preventing any use of bulk
-- relation extension). See
-- https://postgr.es/m/18130-7a86a7356a75209d%40postgresql.org
-- https://postgr.es/m/257696.1695670946%40sss.pgh.pa.us
\set filename :abs_srcdir '/data/desc.data'
--dg-cascade
--dg COPY parted_si(id, data) FROM :'filename';

-- An earlier bug (see commit b1ecb9b3fcf) could end up using a buffer from
-- the wrong partition. This test is *not* guaranteed to trigger that bug, but
-- does so when shared_buffers is small enough.  To test if we encountered the
-- bug, check that the partition condition isn't violated.
--dg-cascade
--dg SELECT tableoid::regclass, id % 2 = 0 is_even, count(*) from parted_si GROUP BY 1, 2 ORDER BY 1;

--dg-cascade
--dg DROP TABLE parted_si;

-- ensure COPY FREEZE errors for foreign tables
begin;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg create foreign data wrapper copytest_wrapper;
--dg-cascade
--dg create server copytest_server foreign data wrapper copytest_wrapper;
--dg-cascade
--dg create foreign table copytest_foreign_table (a int) server copytest_server;
--dg-cascade
--dg copy copytest_foreign_table from stdin (freeze);
--dg 1
--dg \.
rollback;

-- Tests for COPY TO with materialized views.
-- COPY TO should fail for an unpopulated materialized view
-- but succeed for a populated one.
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW copytest_mv AS SELECT 1 AS id WITH NO DATA;
--dg-cascade
--dg COPY copytest_mv(id) TO stdout WITH (header);
--dg-cascade
--dg REFRESH MATERIALIZED VIEW copytest_mv;
--dg-cascade
--dg COPY copytest_mv(id) TO stdout WITH (header);
--dg-cascade
--dg DROP MATERIALIZED VIEW copytest_mv;
