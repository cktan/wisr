-- Verify NOT NULL constraints by name as PostgreSQL 18 has them
-- (../sm/NOTNULL.md): each NOT NULL column is a constraint of
-- pg_constraint, named by PostgreSQL's rule or by the statement, and
-- DROP, RENAME and ADD CONSTRAINT take it.  Temporary tables, as the 8
-- sessions run.sh starts share one database.
-- 1. A table's NOT NULL columns, a primary key's among them, are rows
--    of type n, one named by the statement; \d+ lists them.
-- 2. LIKE ... INCLUDING ALL copies them.
-- 3. DROP CONSTRAINT of one is DROP NOT NULL; RENAME CONSTRAINT renames
--    one; SET NOT NULL and ADD CONSTRAINT ... NOT NULL make one.
-- 4. A primary key's keeps its column NOT NULL; a name given twice, or
--    taken by another constraint, is refused; a column's second name
--    and NOT VALID are refused.
-- The refusals are what is tested, so an error goes on to the next
-- statement.
\set ON_ERROR_STOP off
create temp table job (id int primary key, owner text not null,
                       note text constraint note_set not null, x int);
select conname, contype, conkey from pg_constraint
  where conrelid = 'job'::regclass order by conname;
\d+ job
create temp table job2 (like job including all);
select conname, contype, conkey from pg_constraint
  where conrelid = 'job2'::regclass order by conname;
alter table job drop constraint job_owner_not_null;
insert into job values (1, null, 'n', 1);
alter table job rename constraint note_set to note_given;
alter table job alter column x set not null;
alter table job add constraint x_set not null owner;
select conname, contype, conkey from pg_constraint
  where conrelid = 'job'::regclass order by conname;
delete from job;
alter table job add constraint x_set not null owner;
alter table job drop constraint job_id_not_null;
create temp table bad (a int constraint blah not null,
                       b int constraint blah not null);
alter table job add constraint note_given check (x > 0);
alter table job add constraint other not null x;
alter table job add constraint nv not null note not valid;
alter table job add column y int;
alter table job add constraint nv not null y not valid;
select conname, contype, conkey from pg_constraint
  where conrelid = 'job'::regclass order by conname;
drop table job;
drop table job2;
