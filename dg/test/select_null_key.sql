-- Verify that an index scan whose key is null at run time reads nothing,
-- and that IS NULL and IS NOT NULL, keys of their own, still find theirs
-- (WORKNOTE.md step 429): the key's value from an InitPlan that returns no
-- row, as a comparison with null is never true. EXPLAIN ANALYZE's Index
-- Searches counts the probes a scan opens, so 0 says it read nothing.
-- Temporary tables, as the 8 sessions run.sh starts share one database.
-- 1. Make enroll, a btree index on (class_id, student_id), and rows with
--    class_id null among them; and keyed, with a primary key.
-- 2. With sequential scans off, the same scan keyed by a subquery that
--    returns a class: EXPLAIN ANALYZE shows its search and its row, so its
--    counts are read for a scan that runs.
-- 3. An index scan, an index-only scan and a primary key's scan keyed by a
--    subquery of no row: no search, no row.
--    A delete so keyed searches nothing, and deletes nothing.
-- 4. Scans with a key: one search, and one for each value of a list.
-- 5. The rows themselves: no row for a null key; IS NULL finds the rows
--    whose class_id is null, IS NOT NULL the rest, a null array none.
create temporary table enroll (student_id int4, class_id int4, grade int4);
insert into enroll select g % 10, g / 10, 0 from generate_series(0, 99) g;
insert into enroll values (1, null, 0), (2, null, 0), (3, null, 0);
create index enroll_class on enroll using btree (class_id, student_id);
create temporary table keyed (id int4 primary key, v int4);
insert into keyed select g, g from generate_series(1, 100) g;
analyze enroll;
analyze keyed;
set enable_seqscan = off;
set enable_bitmapscan = off;
explain (analyze, costs off, timing off, summary off, buffers off)
  select grade from enroll
  where student_id = 5
    and class_id = (select class_id from enroll where class_id = 3 limit 1);
explain (analyze, costs off, timing off, summary off, buffers off)
  select grade from enroll
  where student_id = 5 and class_id = (select class_id from enroll where false);
explain (analyze, costs off, timing off, summary off, buffers off)
  select student_id from enroll
  where class_id = (select class_id from enroll where false);
explain (analyze, costs off, timing off, summary off, buffers off)
  select v from keyed where id = (select id from keyed where false);
explain (analyze, costs off, timing off, summary off, buffers off)
  delete from enroll
  where student_id = 5 and class_id = (select class_id from enroll where false);
select count(*) from enroll;
explain (analyze, costs off, timing off, summary off, buffers off)
  select grade from enroll where class_id = 3;
explain (analyze, costs off, timing off, summary off, buffers off)
  select grade from enroll where class_id in (1, 3, 5);
explain (analyze, costs off, timing off, summary off, buffers off)
  select v from keyed where id in (1, 3, 5, 7);
select grade from enroll
  where student_id = 5 and class_id = (select class_id from enroll where false);
select student_id from enroll where class_id is null order by student_id;
select count(*) from enroll where class_id is not null;
select count(*) from enroll where class_id = any (null::int4[]);
reset enable_seqscan;
reset enable_bitmapscan;
drop table enroll;
drop table keyed;
