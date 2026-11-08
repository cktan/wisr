-- Verify that CHECK constraints are kept in sm and enforced as PostgreSQL
-- enforces them (../sm/CHECK.md): every write path refuses a row for which
-- one is false, a null passes, ALTER TABLE adds, validates, renames and
-- drops them, and pg_constraint shows them.  Temporary tables, as the 8
-- sessions run.sh starts share one database.
-- 1. Make a table with a column's check, a table's check over two columns,
--    and a named one; show how they are named, read and listed, and that
--    LIKE ... INCLUDING CONSTRAINTS copies them, valid.
-- 2. Insert, update, upsert, COPY and INSERT ... SELECT: a row that fails
--    one is refused, naming it; a null passes.
-- 3. ADD CONSTRAINT checks the rows there; NOT VALID does not, but checks
--    new rows; VALIDATE checks them, and passes once they hold.
-- 4. RENAME CONSTRAINT, and a name already taken; a UNIQUE constraint may
--    not take a check's name, and the name chosen for one skips a check's.
-- 5. ADD COLUMN with a check: the rows there are checked with its value.
-- 6. ALTER COLUMN TYPE of a column a check reads is refused; NOT ENFORCED
--    is kept, and not checked, and VALIDATE of it is refused.
-- 7. DROP COLUMN takes the checks that read it; DROP CONSTRAINT drops one.
-- The refusals are what is tested, so an error goes on to the next
-- statement.
\set ON_ERROR_STOP off
create temp table ord (id int8 primary key, qty int4 check (qty > 0),
  price numeric, disc numeric, check (disc <= price),
  constraint id_small check (id < 1000000));
select conname, contype, convalidated, conkey, pg_get_constraintdef(oid)
  from pg_constraint where conrelid = 'ord'::regclass and contype = 'c'
  order by conname;
select relchecks from pg_class where oid = 'ord'::regclass;
create temp table ord_copy (like ord including constraints);
select conname, convalidated, pg_get_constraintdef(oid) from pg_constraint
  where conrelid = 'ord_copy'::regclass order by conname;
drop table ord_copy;
insert into ord values (1, 2, 10, 1);
insert into ord values (2, 0, 10, 1);
insert into ord values (3, 1, 10, 20);
insert into ord values (4, null, 10, 1);
update ord set qty = -1 where id = 1;
insert into ord values (1, 5, 10, 1) on conflict (id) do update set qty = 0;
copy ord from stdin;
6	5	10	2
7	-5	10	2
\.
insert into ord select g, g, 10, 1 from generate_series(10, 12) g;
insert into ord select g, g - 21, 10, 1 from generate_series(20, 22) g;
select * from ord order by id;
create temp table chk (x int, y int);
insert into chk values (1, 10), (2, -5), (3, null);
alter table chk add constraint y_pos check (y > 0);
alter table chk add constraint y_pos check (y > 0) not valid;
insert into chk values (4, -1);
alter table chk validate constraint y_pos;
update chk set y = 5 where y < 0;
alter table chk validate constraint y_pos;
alter table chk add check (x < 100);
select conname, convalidated from pg_constraint
  where conrelid = 'chk'::regclass order by conname;
alter table chk rename constraint y_pos to y_positive;
alter table chk rename constraint chk_x_check to y_positive;
alter table chk add constraint y_positive unique (x);
alter table chk add constraint chk_x_key check (x > -1);
alter table chk add unique (x);
select conname, contype from pg_constraint
  where conrelid = 'chk'::regclass and conname like 'chk_x_key%'
  order by conname;
alter table chk drop constraint chk_x_key1;
alter table chk drop constraint chk_x_key;
alter table chk add column w int check (w <> 0) default 7;
alter table chk add column v int default 0 check (v <> 0);
alter table chk alter column y type bigint;
alter table chk add constraint not_nine check (x <> 9) not enforced;
insert into chk values (9, 1, 1);
alter table chk validate constraint not_nine;
select conname, conenforced, convalidated from pg_constraint
  where conrelid = 'chk'::regclass order by conname;
alter table chk drop column y;
alter table chk drop constraint chk_x_check;
alter table chk drop constraint no_such;
select conname from pg_constraint where conrelid = 'chk'::regclass
  order by conname;
insert into chk values (200, 1);
insert into chk values (201, 0);
select * from chk order by x;
drop table ord;
drop table chk;
