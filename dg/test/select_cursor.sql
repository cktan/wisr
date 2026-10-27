-- Verify that a cursor reads as its DECLARE did on each FETCH: no write
-- its transaction makes after the DECLARE is seen, however the plan
-- reads again (WORKNOTE.md step 304); as PostgreSQL's cursor reads at
-- its query's snapshot. Temporary tables, as the 8 sessions run.sh
-- starts share one database.
-- 1. A cursor fetches a row, the row is deleted by WHERE CURRENT OF,
--    and the cursor rewound: it reads all three rows again.
-- 2. A row inserted after the DECLARE is not read after a rewind.
-- 3. A nested loop's inner scan, begun again for each outer row in a
--    later FETCH, still reads an inner row deleted after the DECLARE.
-- 4. An index scan's row, read by its rowno in a later FETCH, is read
--    though deleted after the DECLARE, and as it was though updated.
-- 5. A SCROLL cursor WITH HOLD, read again from its start at its commit
--    to be kept, keeps a row deleted after its DECLARE.
create temp table uc (f1 int, f2 text);
insert into uc values (1, 'one'), (2, 'two'), (3, 'three');
begin;
declare c1 cursor for select * from uc;
fetch 2 from c1;
delete from uc where current of c1;
move backward all in c1;
fetch all from c1;
rollback;
begin;
declare c1 cursor for select * from uc;
fetch 1 from c1;
insert into uc values (4, 'four');
move backward all in c1;
fetch all from c1;
rollback;
create temp table uo (a int);
insert into uo values (1), (2), (3);
set enable_hashjoin = off;
set enable_mergejoin = off;
set enable_material = off;
explain (costs off) select a, f1 from uo, uc where a = f1;
begin;
declare c2 cursor for select a, f1 from uo, uc where a = f1;
fetch 1 from c2;
delete from uo where a = 3;
fetch all from c2;
rollback;
reset enable_hashjoin;
reset enable_mergejoin;
reset enable_material;
create index uc_f1 on uc (f1);
set enable_seqscan = off;
set enable_bitmapscan = off;
explain (costs off) select * from uc where f1 > 0;
begin;
declare c3 cursor for select * from uc where f1 > 0;
fetch 1 from c3;
delete from uc where f1 = 3;
update uc set f2 = 'TWO' where f1 = 2;
fetch all from c3;
rollback;
reset enable_seqscan;
reset enable_bitmapscan;
begin;
declare c4 scroll cursor with hold for select * from uc;
fetch 1 from c4;
delete from uc where f1 = 3;
commit;
fetch all from c4;
close c4;
select * from uc order by f1;
