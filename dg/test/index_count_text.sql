-- Verify that count(*) over an index-only scan of a btree on text counts
-- the rows an equality names, with no column of an entry read: keys cut
-- for their length, a value that is another's prefix, and a bound of 64,
-- 65 and 66 bytes, around the length the scan trusts, all give the count
-- a sequential scan gives. Temporary
-- tables, as the 8 sessions run.sh starts share one database.
-- 1. Make a table of short, prefix, long and null values, with a btree on
--    the text column and one on the varchar column, then delete and
--    update some rows.
-- 2. Count each value by the text index and by the varchar index.
-- 3. Drop the text index for one on (text, int), and count again.
-- 4. Count again by a sequential scan: the three sets must agree.
create temp table ic (t text, v varchar(2000), n int);
insert into ic select 'k' || (i % 7), 'v' || (i % 5), i
  from generate_series(1, 700) i;
insert into ic values ('k1x', 'v1x', 0), ('k', 'v', 0), (null, null, 0);
insert into ic select repeat('p', 1400) || (i % 3), repeat('p', 1400) || (i % 3), i
  from generate_series(1, 30) i;
insert into ic select repeat('r', 64 + i % 3), 'v', i from generate_series(1, 30) i;
delete from ic where t = 'k3' and n < 100;
update ic set t = 'k9' where t = 'k2' and n < 50;
create index ic_t on ic using btree (t);
create index ic_v on ic using btree (v);
set enable_seqscan = off;
set enable_bitmapscan = off;
explain (costs off) select count(*) from ic where t = 'k3';
explain (costs off) select count(*) from ic where v = 'v2';
select count(*) from ic where t = 'k3';
select count(*) from ic where t = 'k1';
select count(*) from ic where t = 'k';
select count(*) from ic where t = 'k1x';
select count(*) from ic where t = 'k9';
select count(*) from ic where t = 'zz';
select count(*) from ic where t = repeat('p', 1400) || '0';
select count(*) from ic where t = repeat('p', 1400) || '1';
select count(*) from ic where t = repeat('p', 1400);
select count(*) from ic where t = repeat('q', 100);
select count(*) from ic where t = repeat('r', 64);
select count(*) from ic where t = repeat('r', 65);
select count(*) from ic where t = repeat('r', 66);
select count(*) from ic where t is null;
select count(*) from ic where t = 'k4' and n > 300;
select count(*) from ic where v = 'v2';
select count(*) from ic where v = 'v';
select count(*) from ic where v = repeat('p', 1400) || '2';
create index ic_tn on ic using btree (t, n);
drop index ic_t;
explain (costs off) select count(*) from ic where t = 'k3';
select count(*) from ic where t = 'k3';
select count(*) from ic where t = 'k1';
select count(*) from ic where t = 'k';
select count(*) from ic where t = 'k1x';
select count(*) from ic where t = 'k9';
select count(*) from ic where t = 'zz';
select count(*) from ic where t = repeat('p', 1400) || '0';
select count(*) from ic where t = repeat('p', 1400) || '1';
select count(*) from ic where t = repeat('p', 1400);
select count(*) from ic where t = repeat('q', 100);
select count(*) from ic where t = repeat('r', 64);
select count(*) from ic where t = repeat('r', 65);
select count(*) from ic where t = repeat('r', 66);
select count(*) from ic where t is null;
select count(*) from ic where t = 'k4' and n > 300;
set enable_indexonlyscan = off;
set enable_indexscan = off;
set enable_seqscan = on;
select count(*) from ic where t = 'k3';
select count(*) from ic where t = 'k1';
select count(*) from ic where t = 'k';
select count(*) from ic where t = 'k1x';
select count(*) from ic where t = 'k9';
select count(*) from ic where t = 'zz';
select count(*) from ic where t = repeat('p', 1400) || '0';
select count(*) from ic where t = repeat('p', 1400) || '1';
select count(*) from ic where t = repeat('p', 1400);
select count(*) from ic where t = repeat('q', 100);
select count(*) from ic where t = repeat('r', 64);
select count(*) from ic where t = repeat('r', 65);
select count(*) from ic where t = repeat('r', 66);
select count(*) from ic where t is null;
select count(*) from ic where t = 'k4' and n > 300;
select count(*) from ic where v = 'v2';
select count(*) from ic where v = 'v';
select count(*) from ic where v = repeat('p', 1400) || '2';
