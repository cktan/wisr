-- Verify that INSERT ... SELECT into a table in sm, which hands its rows to
-- sm's bulk load, leaves the table as a row at a time insert would: rows
-- and indexes right, an abort an empty table.
-- Temporary tables, as the 8 sessions run.sh starts share one database.
-- 1. Insert into an empty table with a primary key and a second index,
--    and read the table by both indexes and by a scan.
-- 2. Insert again, into a table that now has rows.
-- 3. Insert into an empty table then abort: the table reads empty.
-- 4. Insert from the table into itself, and with RETURNING.
-- 5. Insert into a table with a foreign key, which has a row trigger.
-- 6. Insert in one transaction, then read and insert again.
create temp table il (id int primary key, v text, n int);
create index il_n on il (n);
insert into il select i, 'v' || i, i % 10 from generate_series(1, 5000) i;
select count(*), sum(id), min(id), max(id) from il;
set enable_seqscan = off;
select count(*) from il where n = 3;
select v from il where id = 4321;
reset enable_seqscan;
insert into il select i, 'w' || i, i % 10 from generate_series(5001, 6000) i;
select count(*), sum(id) from il;
create temp table ia (id int primary key, v text);
begin;
insert into ia select i, 'a' from generate_series(1, 100) i;
select count(*) from ia;
rollback;
select count(*) from ia;
insert into ia select i, 'a' from generate_series(1, 100) i;
insert into ia select id + 100, v from ia;
select count(*) from ia;
with r as (insert into ia select i + 5000, 'r' from generate_series(1, 3) i
           returning id)
select count(*) from r;
create temp table ip (id int primary key);
create temp table ic (id int references ip, v text);
insert into ip select i from generate_series(1, 5) i;
insert into ic select i % 5 + 1, 'c' from generate_series(1, 50) i;
select count(*), sum(id) from ic;
create temp table iu (id int, v text);
begin;
insert into iu select i, 'a' from generate_series(1, 10) i;
select count(*) from iu;
insert into iu select i + 10, 'b' from generate_series(1, 10) i;
select count(*) from iu;
commit;
select count(*) from iu;
