--dg-skip: type ? is not supported yet
--dg CREATE TABLE brintest_multi (
--dg 	int8col bigint,
--dg 	int2col smallint,
--dg 	int4col integer,
--dg 	oidcol oid,
--dg 	tidcol tid,
--dg 	float4col real,
--dg 	float8col double precision,
--dg 	macaddrcol macaddr,
--dg 	macaddr8col macaddr8,
--dg 	inetcol inet,
--dg 	cidrcol cidr,
--dg 	datecol date,
--dg 	timecol time without time zone,
--dg 	timestampcol timestamp without time zone,
--dg 	timestamptzcol timestamp with time zone,
--dg 	intervalcol interval,
--dg 	timetzcol time with time zone,
--dg 	numericcol numeric,
--dg 	uuidcol uuid,
--dg 	lsncol pg_lsn
--dg ) WITH (fillfactor=10);

--dg-cascade
--dg INSERT INTO brintest_multi SELECT
--dg 	142857 * tenthous,
--dg 	thousand,
--dg 	twothousand,
--dg 	unique1::oid,
--dg 	format('(%s,%s)', tenthous, twenty)::tid,
--dg 	(four + 1.0)/(hundred+1),
--dg 	odd::float8 / (tenthous + 1),
--dg 	format('%s:00:%s:00:%s:00', to_hex(odd), to_hex(even), to_hex(hundred))::macaddr,
--dg 	substr(fipshash(unique1::text), 1, 16)::macaddr8,
--dg 	inet '10.2.3.4/24' + tenthous,
--dg 	cidr '10.2.3/24' + tenthous,
--dg 	date '1995-08-15' + tenthous,
--dg 	time '01:20:30' + thousand * interval '18.5 second',
--dg 	timestamp '1942-07-23 03:05:09' + tenthous * interval '36.38 hours',
--dg 	timestamptz '1972-10-10 03:00' + thousand * interval '1 hour',
--dg 	justify_days(justify_hours(tenthous * interval '12 minutes')),
--dg 	timetz '01:30:20+02' + hundred * interval '15 seconds',
--dg 	tenthous::numeric(36,30) * fivethous * even / (hundred + 1),
--dg 	format('%s%s-%s-%s-%s-%s%s%s', to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'))::uuid,
--dg 	format('%s/%s%s', odd, even, tenthous)::pg_lsn
--dg FROM tenk1 ORDER BY unique2 LIMIT 100;

-- throw in some NULL's and different values
--dg-cascade
--dg INSERT INTO brintest_multi (inetcol, cidrcol) SELECT
--dg 	inet 'fe80::6e40:8ff:fea9:8c46' + tenthous,
--dg 	cidr 'fe80::6e40:8ff:fea9:8c46' + tenthous
--dg FROM tenk1 ORDER BY thousand, tenthous LIMIT 25;

-- test minmax-multi specific index options
-- number of values must be >= 16
--dg-cascade
--dg CREATE INDEX brinidx_multi ON brintest_multi USING brin (
--dg 	int8col int8_minmax_multi_ops(values_per_range = 7)
--dg );
-- number of values must be <= 256
--dg-cascade
--dg CREATE INDEX brinidx_multi ON brintest_multi USING brin (
--dg 	int8col int8_minmax_multi_ops(values_per_range = 257)
--dg );

-- first create an index with a single page range, to force compaction
-- due to exceeding the number of values per summary
--dg-cascade
--dg CREATE INDEX brinidx_multi ON brintest_multi USING brin (
--dg 	int8col int8_minmax_multi_ops,
--dg 	int2col int2_minmax_multi_ops,
--dg 	int4col int4_minmax_multi_ops,
--dg 	oidcol oid_minmax_multi_ops,
--dg 	tidcol tid_minmax_multi_ops,
--dg 	float4col float4_minmax_multi_ops,
--dg 	float8col float8_minmax_multi_ops,
--dg 	macaddrcol macaddr_minmax_multi_ops,
--dg 	macaddr8col macaddr8_minmax_multi_ops,
--dg 	inetcol inet_minmax_multi_ops,
--dg 	cidrcol inet_minmax_multi_ops,
--dg 	datecol date_minmax_multi_ops,
--dg 	timecol time_minmax_multi_ops,
--dg 	timestampcol timestamp_minmax_multi_ops,
--dg 	timestamptzcol timestamptz_minmax_multi_ops,
--dg 	intervalcol interval_minmax_multi_ops,
--dg 	timetzcol timetz_minmax_multi_ops,
--dg 	numericcol numeric_minmax_multi_ops,
--dg 	uuidcol uuid_minmax_multi_ops,
--dg 	lsncol pg_lsn_minmax_multi_ops
--dg );

--dg-cascade
--dg DROP INDEX brinidx_multi;

--dg-cascade
--dg CREATE INDEX brinidx_multi ON brintest_multi USING brin (
--dg 	int8col int8_minmax_multi_ops,
--dg 	int2col int2_minmax_multi_ops,
--dg 	int4col int4_minmax_multi_ops,
--dg 	oidcol oid_minmax_multi_ops,
--dg 	tidcol tid_minmax_multi_ops,
--dg 	float4col float4_minmax_multi_ops,
--dg 	float8col float8_minmax_multi_ops,
--dg 	macaddrcol macaddr_minmax_multi_ops,
--dg 	macaddr8col macaddr8_minmax_multi_ops,
--dg 	inetcol inet_minmax_multi_ops,
--dg 	cidrcol inet_minmax_multi_ops,
--dg 	datecol date_minmax_multi_ops,
--dg 	timecol time_minmax_multi_ops,
--dg 	timestampcol timestamp_minmax_multi_ops,
--dg 	timestamptzcol timestamptz_minmax_multi_ops,
--dg 	intervalcol interval_minmax_multi_ops,
--dg 	timetzcol timetz_minmax_multi_ops,
--dg 	numericcol numeric_minmax_multi_ops,
--dg 	uuidcol uuid_minmax_multi_ops,
--dg 	lsncol pg_lsn_minmax_multi_ops
--dg ) with (pages_per_range = 1);

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE brinopers_multi (colname name, typ text,
--dg 	op text[], value text[], matches int[],
--dg 	check (cardinality(op) = cardinality(value)),
--dg 	check (cardinality(op) = cardinality(matches)));

--dg-cascade
--dg INSERT INTO brinopers_multi VALUES
--dg 	('int2col', 'int2',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0, 0, 800, 999, 999}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('int2col', 'int4',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0, 0, 800, 999, 1999}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('int2col', 'int8',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0, 0, 800, 999, 1428427143}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('int4col', 'int2',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0, 0, 800, 1999, 1999}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('int4col', 'int4',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0, 0, 800, 1999, 1999}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('int4col', 'int8',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0, 0, 800, 1999, 1428427143}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('int8col', 'int2',
--dg 	 '{>, >=}',
--dg 	 '{0, 0}',
--dg 	 '{100, 100}'),
--dg 	('int8col', 'int4',
--dg 	 '{>, >=}',
--dg 	 '{0, 0}',
--dg 	 '{100, 100}'),
--dg 	('int8col', 'int8',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0, 0, 1257141600, 1428427143, 1428427143}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('oidcol', 'oid',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0, 0, 8800, 9999, 9999}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('tidcol', 'tid',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{"(0,0)", "(0,0)", "(8800,0)", "(9999,19)", "(9999,19)"}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('float4col', 'float4',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0.0103093, 0.0103093, 1, 1, 1}',
--dg 	 '{100, 100, 4, 100, 96}'),
--dg 	('float4col', 'float8',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0.0103093, 0.0103093, 1, 1, 1}',
--dg 	 '{100, 100, 4, 100, 96}'),
--dg 	('float8col', 'float4',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0, 0, 0, 1.98, 1.98}',
--dg 	 '{99, 100, 1, 100, 100}'),
--dg 	('float8col', 'float8',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0, 0, 0, 1.98, 1.98}',
--dg 	 '{99, 100, 1, 100, 100}'),
--dg 	('macaddrcol', 'macaddr',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{00:00:01:00:00:00, 00:00:01:00:00:00, 2c:00:2d:00:16:00, ff:fe:00:00:00:00, ff:fe:00:00:00:00}',
--dg 	 '{99, 100, 2, 100, 100}'),
--dg 	('macaddr8col', 'macaddr8',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{b1:d1:0e:7b:af:a4:42:12, d9:35:91:bd:f7:86:0e:1e, 72:8f:20:6c:2a:01:bf:57, 23:e8:46:63:86:07:ad:cb, 13:16:8e:6a:2e:6c:84:b4}',
--dg 	 '{31, 17, 1, 11, 4}'),
--dg 	('inetcol', 'inet',
--dg 	 '{=, <, <=, >, >=}',
--dg 	 '{10.2.14.231/24, 255.255.255.255, 255.255.255.255, 0.0.0.0, 0.0.0.0}',
--dg 	 '{1, 100, 100, 125, 125}'),
--dg 	('inetcol', 'cidr',
--dg 	 '{<, <=, >, >=}',
--dg 	 '{255.255.255.255, 255.255.255.255, 0.0.0.0, 0.0.0.0}',
--dg 	 '{100, 100, 125, 125}'),
--dg 	('cidrcol', 'inet',
--dg 	 '{=, <, <=, >, >=}',
--dg 	 '{10.2.14/24, 255.255.255.255, 255.255.255.255, 0.0.0.0, 0.0.0.0}',
--dg 	 '{2, 100, 100, 125, 125}'),
--dg 	('cidrcol', 'cidr',
--dg 	 '{=, <, <=, >, >=}',
--dg 	 '{10.2.14/24, 255.255.255.255, 255.255.255.255, 0.0.0.0, 0.0.0.0}',
--dg 	 '{2, 100, 100, 125, 125}'),
--dg 	('datecol', 'date',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{1995-08-15, 1995-08-15, 2009-12-01, 2022-12-30, 2022-12-30}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('timecol', 'time',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{01:20:30, 01:20:30, 02:28:57, 06:28:31.5, 06:28:31.5}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('timestampcol', 'timestamp',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{1942-07-23 03:05:09, 1942-07-23 03:05:09, 1964-03-24 19:26:45, 1984-01-20 22:42:21, 1984-01-20 22:42:21}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('timestampcol', 'timestamptz',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{1942-07-23 03:05:09, 1942-07-23 03:05:09, 1964-03-24 19:26:45, 1984-01-20 22:42:21, 1984-01-20 22:42:21}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('timestamptzcol', 'timestamptz',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{1972-10-10 03:00:00-04, 1972-10-10 03:00:00-04, 1972-10-19 09:00:00-07, 1972-11-20 19:00:00-03, 1972-11-20 19:00:00-03}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('intervalcol', 'interval',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{00:00:00, 00:00:00, 1 mons 13 days 12:24, 2 mons 23 days 07:48:00, 1 year}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('timetzcol', 'timetz',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{01:30:20+02, 01:30:20+02, 01:35:50+02, 23:55:05+02, 23:55:05+02}',
--dg 	 '{99, 100, 2, 100, 100}'),
--dg 	('numericcol', 'numeric',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{0.00, 0.01, 2268164.347826086956521739130434782609, 99470151.9, 99470151.9}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('uuidcol', 'uuid',
--dg 	 '{>, >=, =, <=, <}',
--dg 	 '{00040004-0004-0004-0004-000400040004, 00040004-0004-0004-0004-000400040004, 52225222-5222-5222-5222-522252225222, 99989998-9998-9998-9998-999899989998, 99989998-9998-9998-9998-999899989998}',
--dg 	 '{100, 100, 1, 100, 100}'),
--dg 	('lsncol', 'pg_lsn',
--dg 	 '{>, >=, =, <=, <, IS, IS NOT}',
--dg 	 '{0/1200, 0/1200, 44/455222, 198/1999799, 198/1999799, NULL, NULL}',
--dg 	 '{100, 100, 1, 100, 100, 25, 100}');

--dg-skip: language "?" does not exist
--dg DO $x$
--dg DECLARE
--dg 	r record;
--dg 	r2 record;
--dg 	cond text;
--dg 	idx_ctids tid[];
--dg 	ss_ctids tid[];
--dg 	count int;
--dg 	plan_ok bool;
--dg 	plan_line text;
--dg BEGIN
--dg 	FOR r IN SELECT colname, oper, typ, value[ordinality], matches[ordinality] FROM brinopers_multi, unnest(op) WITH ORDINALITY AS oper LOOP
--dg
--dg 		-- prepare the condition
--dg 		IF r.value IS NULL THEN
--dg 			cond := format('%I %s %L', r.colname, r.oper, r.value);
--dg 		ELSE
--dg 			cond := format('%I %s %L::%s', r.colname, r.oper, r.value, r.typ);
--dg 		END IF;
--dg
--dg 		-- run the query using the brin index
--dg 		SET enable_seqscan = 0;
--dg 		SET enable_bitmapscan = 1;
--dg
--dg 		plan_ok := false;
--dg 		FOR plan_line IN EXECUTE format($y$EXPLAIN SELECT array_agg(ctid) FROM brintest_multi WHERE %s $y$, cond) LOOP
--dg 			IF plan_line LIKE '%Bitmap Heap Scan on brintest_multi%' THEN
--dg 				plan_ok := true;
--dg 			END IF;
--dg 		END LOOP;
--dg 		IF NOT plan_ok THEN
--dg 			RAISE WARNING 'did not get bitmap indexscan plan for %', r;
--dg 		END IF;
--dg
--dg 		EXECUTE format($y$SELECT array_agg(ctid) FROM brintest_multi WHERE %s $y$, cond)
--dg 			INTO idx_ctids;
--dg
--dg 		-- run the query using a seqscan
--dg 		SET enable_seqscan = 1;
--dg 		SET enable_bitmapscan = 0;
--dg
--dg 		plan_ok := false;
--dg 		FOR plan_line IN EXECUTE format($y$EXPLAIN SELECT array_agg(ctid) FROM brintest_multi WHERE %s $y$, cond) LOOP
--dg 			IF plan_line LIKE '%Seq Scan on brintest_multi%' THEN
--dg 				plan_ok := true;
--dg 			END IF;
--dg 		END LOOP;
--dg 		IF NOT plan_ok THEN
--dg 			RAISE WARNING 'did not get seqscan plan for %', r;
--dg 		END IF;
--dg
--dg 		EXECUTE format($y$SELECT array_agg(ctid) FROM brintest_multi WHERE %s $y$, cond)
--dg 			INTO ss_ctids;
--dg
--dg 		-- make sure both return the same results
--dg 		count := array_length(idx_ctids, 1);
--dg
--dg 		IF NOT (count = array_length(ss_ctids, 1) AND
--dg 				idx_ctids @> ss_ctids AND
--dg 				idx_ctids <@ ss_ctids) THEN
--dg 			-- report the results of each scan to make the differences obvious
--dg 			RAISE WARNING 'something not right in %: count %', r, count;
--dg 			SET enable_seqscan = 1;
--dg 			SET enable_bitmapscan = 0;
--dg 			FOR r2 IN EXECUTE 'SELECT ' || r.colname || ' FROM brintest_multi WHERE ' || cond LOOP
--dg 				RAISE NOTICE 'seqscan: %', r2;
--dg 			END LOOP;
--dg
--dg 			SET enable_seqscan = 0;
--dg 			SET enable_bitmapscan = 1;
--dg 			FOR r2 IN EXECUTE 'SELECT ' || r.colname || ' FROM brintest_multi WHERE ' || cond LOOP
--dg 				RAISE NOTICE 'bitmapscan: %', r2;
--dg 			END LOOP;
--dg 		END IF;
--dg
--dg 		-- make sure we found expected number of matches
--dg 		IF count != r.matches THEN RAISE WARNING 'unexpected number of results % for %', count, r; END IF;
--dg 	END LOOP;
--dg END;
--dg $x$;

RESET enable_seqscan;
RESET enable_bitmapscan;

--dg-cascade
--dg INSERT INTO brintest_multi SELECT
--dg 	142857 * tenthous,
--dg 	thousand,
--dg 	twothousand,
--dg 	unique1::oid,
--dg 	format('(%s,%s)', tenthous, twenty)::tid,
--dg 	(four + 1.0)/(hundred+1),
--dg 	odd::float8 / (tenthous + 1),
--dg 	format('%s:00:%s:00:%s:00', to_hex(odd), to_hex(even), to_hex(hundred))::macaddr,
--dg 	substr(fipshash(unique1::text), 1, 16)::macaddr8,
--dg 	inet '10.2.3.4' + tenthous,
--dg 	cidr '10.2.3/24' + tenthous,
--dg 	date '1995-08-15' + tenthous,
--dg 	time '01:20:30' + thousand * interval '18.5 second',
--dg 	timestamp '1942-07-23 03:05:09' + tenthous * interval '36.38 hours',
--dg 	timestamptz '1972-10-10 03:00' + thousand * interval '1 hour',
--dg 	justify_days(justify_hours(tenthous * interval '12 minutes')),
--dg 	timetz '01:30:20' + hundred * interval '15 seconds',
--dg 	tenthous::numeric(36,30) * fivethous * even / (hundred + 1),
--dg 	format('%s%s-%s-%s-%s-%s%s%s', to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'), to_char(tenthous, 'FM0000'))::uuid,
--dg 	format('%s/%s%s', odd, even, tenthous)::pg_lsn
--dg FROM tenk1 ORDER BY unique2 LIMIT 5 OFFSET 5;

--dg-cascade
--dg SELECT brin_desummarize_range('brinidx_multi', 0);
--dg-cascade
--dg VACUUM brintest_multi;  -- force a summarization cycle in brinidx

-- Try inserting a values with NaN, to test distance calculation.
--dg-cascade
--dg insert into public.brintest_multi (float4col) values (real 'nan');
--dg-cascade
--dg insert into public.brintest_multi (float8col) values (real 'nan');

--dg-cascade
--dg UPDATE brintest_multi SET int8col = int8col * int4col;

-- Test handling of inet netmasks with inet_minmax_multi_ops
--dg-skip: type ? is not supported yet
--dg CREATE TABLE brin_test_inet (a inet);
--dg-cascade
--dg CREATE INDEX ON brin_test_inet USING brin (a inet_minmax_multi_ops);
--dg-cascade
--dg INSERT INTO brin_test_inet VALUES ('127.0.0.1/0');
--dg-cascade
--dg INSERT INTO brin_test_inet VALUES ('0.0.0.0/12');
--dg-cascade
--dg DROP TABLE brin_test_inet;

-- Tests for brin_summarize_new_values
--dg-cascade
--dg SELECT brin_summarize_new_values('brintest_multi'); -- error, not an index
--dg-differs: another error: function ?(...) does not exist
--dg SELECT brin_summarize_new_values('tenk1_unique1'); -- error, not a BRIN index
--dg-cascade
--dg SELECT brin_summarize_new_values('brinidx_multi'); -- ok, no change expected

-- Tests for brin_desummarize_range
--dg-cascade
--dg SELECT brin_desummarize_range('brinidx_multi', -1); -- error, invalid range
--dg-cascade
--dg SELECT brin_desummarize_range('brinidx_multi', 0);
--dg-cascade
--dg SELECT brin_desummarize_range('brinidx_multi', 0);
--dg-cascade
--dg SELECT brin_desummarize_range('brinidx_multi', 100000000);

-- test building an index with many values, to force compaction of the buffer
CREATE TABLE brin_large_range (a int4);
INSERT INTO brin_large_range SELECT i FROM generate_series(1,10000) s(i);
--dg-skip: access method "?" is not supported
--dg CREATE INDEX brin_large_range_idx ON brin_large_range USING brin (a int4_minmax_multi_ops);
DROP TABLE brin_large_range;

-- Test brin_summarize_range
CREATE TABLE brin_summarize_multi (
    value int
) WITH (fillfactor=10, autovacuum_enabled=false);
--dg-skip: access method "?" is not supported
--dg CREATE INDEX brin_summarize_multi_idx ON brin_summarize_multi USING brin (value) WITH (pages_per_range=2);
-- Fill a few pages
--dg-skip: language "?" does not exist
--dg DO $$
--dg DECLARE curtid tid;
--dg BEGIN
--dg   LOOP
--dg     INSERT INTO brin_summarize_multi VALUES (1) RETURNING ctid INTO curtid;
--dg     EXIT WHEN curtid > tid '(2, 0)';
--dg   END LOOP;
--dg END;
--dg $$;

-- summarize one range
--dg-cascade
--dg SELECT brin_summarize_range('brin_summarize_multi_idx', 0);
-- nothing: already summarized
--dg-cascade
--dg SELECT brin_summarize_range('brin_summarize_multi_idx', 1);
-- summarize one range
--dg-cascade
--dg SELECT brin_summarize_range('brin_summarize_multi_idx', 2);
-- nothing: page doesn't exist in table
--dg-cascade
--dg SELECT brin_summarize_range('brin_summarize_multi_idx', 4294967295);
-- invalid block number values
--dg-cascade
--dg SELECT brin_summarize_range('brin_summarize_multi_idx', -1);
--dg-cascade
--dg SELECT brin_summarize_range('brin_summarize_multi_idx', 4294967296);


-- test brin cost estimates behave sanely based on correlation of values
CREATE TABLE brin_test_multi (a INT, b INT);
INSERT INTO brin_test_multi SELECT x/100,x%100 FROM generate_series(1,10000) x(x);
--dg-skip: access method "?" is not supported
--dg CREATE INDEX brin_test_multi_a_idx ON brin_test_multi USING brin (a) WITH (pages_per_range = 2);
--dg-skip: access method "?" is not supported
--dg CREATE INDEX brin_test_multi_b_idx ON brin_test_multi USING brin (b) WITH (pages_per_range = 2);
VACUUM ANALYZE brin_test_multi;

-- Ensure brin index is used when columns are perfectly correlated
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM brin_test_multi WHERE a = 1;
-- Ensure brin index is not used when values are not correlated
EXPLAIN (COSTS OFF) SELECT * FROM brin_test_multi WHERE b = 1;


-- do some inequality tests
CREATE TABLE brin_test_multi_1 (a INT, b BIGINT) WITH (fillfactor=10);
INSERT INTO brin_test_multi_1
SELECT i/5 + mod(911 * i + 483, 25),
       i/10 + mod(751 * i + 221, 41)
  FROM generate_series(1,1000) s(i);

--dg-skip: access method "?" is not supported
--dg CREATE INDEX brin_test_multi_1_idx_1 ON brin_test_multi_1 USING brin (a int4_minmax_multi_ops) WITH (pages_per_range=5);
--dg-skip: access method "?" is not supported
--dg CREATE INDEX brin_test_multi_1_idx_2 ON brin_test_multi_1 USING brin (b int8_minmax_multi_ops) WITH (pages_per_range=5);

SET enable_seqscan=off;

-- int: less than
SELECT COUNT(*) FROM brin_test_multi_1 WHERE a < 37;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a < 113;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a <= 177;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a <= 25;

-- int: greater than
SELECT COUNT(*) FROM brin_test_multi_1 WHERE a > 120;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a >= 180;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a > 71;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a >= 63;

-- int: equals
SELECT COUNT(*) FROM brin_test_multi_1 WHERE a = 207;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a = 177;

-- bigint: less than
SELECT COUNT(*) FROM brin_test_multi_1 WHERE b < 73;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b <= 47;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b < 199;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b <= 150;

-- bigint: greater than
SELECT COUNT(*) FROM brin_test_multi_1 WHERE b > 93;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b > 37;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b >= 215;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b > 201;

-- bigint: equals
SELECT COUNT(*) FROM brin_test_multi_1 WHERE b = 88;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b = 103;

-- now do the same, but insert the rows with the indexes already created
-- so that we don't use the "build callback" and instead use the regular
-- approach of adding rows into existing ranges
TRUNCATE brin_test_multi_1;

INSERT INTO brin_test_multi_1
SELECT i/5 + mod(911 * i + 483, 25),
       i/10 + mod(751 * i + 221, 41)
  FROM generate_series(1,1000) s(i);

-- int: less than
SELECT COUNT(*) FROM brin_test_multi_1 WHERE a < 37;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a < 113;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a <= 177;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a <= 25;

-- int: greater than
SELECT COUNT(*) FROM brin_test_multi_1 WHERE a > 120;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a >= 180;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a > 71;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a >= 63;

-- int: equals
SELECT COUNT(*) FROM brin_test_multi_1 WHERE a = 207;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE a = 177;

-- bigint: less than
SELECT COUNT(*) FROM brin_test_multi_1 WHERE b < 73;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b <= 47;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b < 199;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b <= 150;

-- bigint: greater than
SELECT COUNT(*) FROM brin_test_multi_1 WHERE b > 93;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b > 37;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b >= 215;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b > 201;

-- bigint: equals
SELECT COUNT(*) FROM brin_test_multi_1 WHERE b = 88;

SELECT COUNT(*) FROM brin_test_multi_1 WHERE b = 103;


DROP TABLE brin_test_multi_1;
RESET enable_seqscan;


-- do some inequality tests for varlena data types
CREATE TABLE brin_test_multi_2 (a UUID) WITH (fillfactor=10);
INSERT INTO brin_test_multi_2
SELECT v::uuid FROM (SELECT row_number() OVER (ORDER BY v) c, v FROM (SELECT fipshash((i/13)::text) AS v FROM generate_series(1,1000) s(i)) foo) bar ORDER BY c + 25 * random();

--dg-skip: access method "?" is not supported
--dg CREATE INDEX brin_test_multi_2_idx ON brin_test_multi_2 USING brin (a uuid_minmax_multi_ops) WITH (pages_per_range=5);

SET enable_seqscan=off;

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a < '3d914f93-48c9-cc0f-f8a7-9716700b9fcd';

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a > '3d914f93-48c9-cc0f-f8a7-9716700b9fcd';

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a <= 'f369cb89-fc62-7e66-8987-007d121ed1ea';

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a >= 'aea92132-c4cb-eb26-3e6a-c2bf6c183b5d';

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a = '5feceb66-ffc8-6f38-d952-786c6d696c79';

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a = '86e50149-6586-6131-2a9e-0b35558d84f6';


-- now do the same, but insert the rows with the indexes already created
-- so that we don't use the "build callback" and instead use the regular
-- approach of adding rows into existing ranges

TRUNCATE brin_test_multi_2;
INSERT INTO brin_test_multi_2
SELECT v::uuid FROM (SELECT row_number() OVER (ORDER BY v) c, v FROM (SELECT fipshash((i/13)::text) AS v FROM generate_series(1,1000) s(i)) foo) bar ORDER BY c + 25 * random();

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a < '3d914f93-48c9-cc0f-f8a7-9716700b9fcd';

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a > '3d914f93-48c9-cc0f-f8a7-9716700b9fcd';

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a <= 'f369cb89-fc62-7e66-8987-007d121ed1ea';

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a >= 'aea92132-c4cb-eb26-3e6a-c2bf6c183b5d';

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a = '5feceb66-ffc8-6f38-d952-786c6d696c79';

SELECT COUNT(*) FROM brin_test_multi_2 WHERE a = '86e50149-6586-6131-2a9e-0b35558d84f6';

DROP TABLE brin_test_multi_2;
RESET enable_seqscan;

-- test overflows during CREATE INDEX with extreme timestamp values
CREATE TABLE brin_timestamp_test(a TIMESTAMPTZ);

SET datestyle TO iso;

-- values close to timestamp minimum
INSERT INTO brin_timestamp_test
SELECT '4713-01-01 00:00:01 BC'::timestamptz + (i || ' seconds')::interval
  FROM generate_series(1,30) s(i);

-- values close to timestamp maximum
INSERT INTO brin_timestamp_test
SELECT '294276-12-01 00:00:01'::timestamptz + (i || ' seconds')::interval
  FROM generate_series(1,30) s(i);

--dg-skip: access method "?" is not supported
--dg CREATE INDEX ON brin_timestamp_test USING brin (a timestamptz_minmax_multi_ops) WITH (pages_per_range=1);
DROP TABLE brin_timestamp_test;

-- test overflows during CREATE INDEX with extreme date values
CREATE TABLE brin_date_test(a DATE);

-- insert values close to date minimum
INSERT INTO brin_date_test SELECT '4713-01-01 BC'::date + i FROM generate_series(1, 30) s(i);

-- insert values close to date minimum
INSERT INTO brin_date_test SELECT '5874897-12-01'::date + i FROM generate_series(1, 30) s(i);

--dg-skip: access method "?" is not supported
--dg CREATE INDEX ON brin_date_test USING brin (a date_minmax_multi_ops) WITH (pages_per_range=1);

SET enable_seqscan = off;

-- make sure the ranges were built correctly and 2023-01-01 eliminates all
--dg-cascade
--dg EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF)
--dg SELECT * FROM brin_date_test WHERE a = '2023-01-01'::date;

DROP TABLE brin_date_test;
RESET enable_seqscan;

-- test handling of infinite timestamp values
CREATE TABLE brin_timestamp_test(a TIMESTAMP);

INSERT INTO brin_timestamp_test VALUES ('-infinity'), ('infinity');
INSERT INTO brin_timestamp_test
SELECT i FROM generate_series('2000-01-01'::timestamp, '2000-02-09'::timestamp, '1 day'::interval) s(i);

--dg-skip: access method "?" is not supported
--dg CREATE INDEX ON brin_timestamp_test USING brin (a timestamp_minmax_multi_ops) WITH (pages_per_range=1);

SET enable_seqscan = off;

--dg-cascade
--dg EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF)
--dg SELECT * FROM brin_timestamp_test WHERE a = '2023-01-01'::timestamp;

--dg-cascade
--dg EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF)
--dg SELECT * FROM brin_timestamp_test WHERE a = '1900-01-01'::timestamp;

DROP TABLE brin_timestamp_test;
RESET enable_seqscan;

-- test handling of infinite date values
CREATE TABLE brin_date_test(a DATE);

INSERT INTO brin_date_test VALUES ('-infinity'), ('infinity');
INSERT INTO brin_date_test SELECT '2000-01-01'::date + i FROM generate_series(1, 40) s(i);

--dg-skip: access method "?" is not supported
--dg CREATE INDEX ON brin_date_test USING brin (a date_minmax_multi_ops) WITH (pages_per_range=1);

SET enable_seqscan = off;

--dg-cascade
--dg EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF)
--dg SELECT * FROM brin_date_test WHERE a = '2023-01-01'::date;

--dg-cascade
--dg EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF)
--dg SELECT * FROM brin_date_test WHERE a = '1900-01-01'::date;

DROP TABLE brin_date_test;
RESET enable_seqscan;
RESET datestyle;

-- test handling of overflow for interval values
CREATE TABLE brin_interval_test(a INTERVAL);

INSERT INTO brin_interval_test SELECT (i || ' years')::interval FROM generate_series(-178000000, -177999980) s(i);

INSERT INTO brin_interval_test SELECT (i || ' years')::interval FROM generate_series( 177999980,  178000000) s(i);

--dg-skip: access method "?" is not supported
--dg CREATE INDEX ON brin_interval_test USING brin (a interval_minmax_multi_ops) WITH (pages_per_range=1);

SET enable_seqscan = off;

--dg-cascade
--dg EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF)
--dg SELECT * FROM brin_interval_test WHERE a = '-30 years'::interval;

--dg-cascade
--dg EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF)
--dg SELECT * FROM brin_interval_test WHERE a = '30 years'::interval;

DROP TABLE brin_interval_test;
RESET enable_seqscan;

-- test handling of infinite interval values
CREATE TABLE brin_interval_test(a INTERVAL);

INSERT INTO brin_interval_test VALUES ('-infinity'), ('infinity');
INSERT INTO brin_interval_test SELECT (i || ' days')::interval FROM generate_series(100, 140) s(i);

--dg-skip: access method "?" is not supported
--dg CREATE INDEX ON brin_interval_test USING brin (a interval_minmax_multi_ops) WITH (pages_per_range=1);

SET enable_seqscan = off;

--dg-cascade
--dg EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF)
--dg SELECT * FROM brin_interval_test WHERE a = '-30 years'::interval;

--dg-cascade
--dg EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF)
--dg SELECT * FROM brin_interval_test WHERE a = '30 years'::interval;

DROP TABLE brin_interval_test;
RESET enable_seqscan;
RESET datestyle;
