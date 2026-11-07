-- Verify that scans of a btree whose entries span many leaves give what
-- a sequential scan gives: a batch of rows read through the tree views
-- the cursor's copy of one leaf, and ends where that leaf does. A
-- temporary table, as the 8 sessions run.sh starts share one database.
-- 1. Make a table of 3,000 rows of distinct short keys, inserted out of
--    key order, and a btree on the key.
-- 2. Read the rows by an index scan, from the first key and from s1500,
--    a key inside a leaf.
-- 3. Read the keys by an index-only scan, decoded and counted, before
--    and after a vacuum sets the leaves all visible.
-- 4. Read the same by a sequential scan: the sets must agree.
create temp table tb (id int, k text);
insert into tb select i * 7919 % 3000, 's' || lpad((i * 7919 % 3000)::text, 4, '0')
  from generate_series(0, 2999) i;
create index tb_k on tb using btree (k);
set enable_seqscan = off;
set enable_bitmapscan = off;
explain (costs off) select count(*), sum(id) from tb where k >= 's0000';
select count(*), sum(id) from tb where k >= 's0000';
select count(*), sum(id) from tb where k >= 's1500';
explain (costs off) select sum(substr(k, 2)::int) from tb where k >= 's1500';
select sum(substr(k, 2)::int) from tb where k >= 's1500';
select count(*) from tb where k >= 's1500';
vacuum tb;
select sum(substr(k, 2)::int) from tb where k >= 's1500';
select count(*) from tb where k >= 's1500';
reset enable_seqscan;
set enable_indexscan = off;
set enable_indexonlyscan = off;
explain (costs off) select count(*), sum(id) from tb where k >= 's1500';
select count(*), sum(id) from tb where k >= 's0000';
select count(*), sum(id) from tb where k >= 's1500';
select sum(substr(k, 2)::int) from tb where k >= 's1500';
reset enable_indexscan;
reset enable_indexonlyscan;
reset enable_bitmapscan;
drop table tb;
