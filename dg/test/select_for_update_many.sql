-- Verify that a transaction holding row locks by the hundred, more
-- than a block of the session's lock list (WORKNOTE.md step 342),
-- keeps exactly those past a savepoint: a rollback to it releases
-- the rows locked after, and the rows locked before stay locked.
-- A table of each session's own, named for its pid, as the 8
-- sessions run.sh starts share one database: a temporary table is
-- the session's alone, and takes no row lock.
-- 1. Lock 600 rows FOR UPDATE, then FOR SHARE on 300 of them, which
--    converts locks already held.
-- 2. Set a savepoint, and lock the other 400 rows FOR UPDATE.
-- 3. Roll back to the savepoint: this session lists the 600 rows
--    locked, not the 400.
-- 4. Lock the 400 again, and update one of the first 600: all 1000
--    are listed, and the commit ends every lock.
select 'fu_' || pg_backend_pid() as fu \gset
create table :fu (id int primary key, v int);
insert into :fu select g, 0 from generate_series(1, 1000) g;
-- The row locks this session holds, by mode; digres has
-- pg_lock_status() but no pg_locks.
create temp view my_rows as
  select mode, count(*) as rows from pg_lock_status() l
  where l.pid = pg_backend_pid() and l.locktype = 'tuple'
  group by mode order by mode;
begin;
select count(*) from (select id from :fu where id <= 600 for update) s;
select count(*) from (select id from :fu where id <= 300 for share) s;
select * from my_rows;
savepoint a;
select count(*) from (select id from :fu where id > 600 for update) s;
select * from my_rows;
rollback to savepoint a;
select * from my_rows;
select count(*) from (select id from :fu where id > 600 for update) s;
update :fu set v = 1 where id = 1;
select * from my_rows;
commit;
select * from my_rows;
select sum(v) from :fu;
drop table :fu;
