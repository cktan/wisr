-- Verify ALTER COLUMN's SET and DROP NOT NULL and SET and DROP DEFAULT
-- as PostgreSQL does them, a column's update default kept (ONUPDATE.md
-- §2).  Temporary tables, as the 8 sessions run.sh starts share one
-- database.
-- 1. SET NOT NULL fails while a row holds a null, and succeeds once none
--    does; then a null is refused.
-- 2. DROP NOT NULL lets a null in; a primary key's column keeps NOT NULL.
-- 3. SET DEFAULT gives new rows the default; a row already there, one
--    written before the column was added among them, keeps its value.
--    DROP DEFAULT leaves new rows null.  A statement prepared before
--    reads the new default.
-- 4. A column with an update default: DROP DEFAULT leaves the update
--    default as the insert default; SET DEFAULT takes only its kinds.
-- 5. A column not there fails.  An identity column needs a sequence, which
--    no temporary table has: test/alter_column.sh shows its refusals.
-- The refusals are what is tested, so an error goes on to the next
-- statement.
\set ON_ERROR_STOP off
create temp table ac (id int primary key, v int, w text);
insert into ac values (1, 10, 'a'), (2, null, 'b');
alter table ac alter column v set not null;
update ac set v = 20 where id = 2;
alter table ac alter column v set not null;
insert into ac values (3, null, 'c');
\d ac
alter table ac alter column v drop not null;
insert into ac values (3, null, 'c');
alter table ac alter column id drop not null;
alter table ac alter column w set default 'none';
prepare ins as insert into ac (id, v) values ($1, 1);
execute ins(4);
alter table ac add column m int default 5;
alter table ac alter column m set default 6;
alter table ac alter column w set default 'other';
execute ins(5);
insert into ac (id) values (6);
alter table ac alter column w drop default;
alter table ac alter column m drop default;
insert into ac (id) values (7);
select id, v, w, m from ac order by id;
\d ac
create temp table job (id int primary key, n int,
  mtime timestamptz default '2020-01-01' default current_timestamp on update);
insert into job (id) values (1);
alter table job alter column mtime drop default;
insert into job (id) values (2);
select id, mtime > '2021-01-01' as now from job order by id;
alter table job alter column mtime set default now();
alter table job alter column mtime set default localtimestamp;
alter table ac alter column nope set default 1;
alter table ac alter column nope set not null;
drop table ac;
drop table job;
