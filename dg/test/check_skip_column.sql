-- Verify that a skipped ADD COLUMN does not install or validate CHECKs.
-- 1. Skip plain, serial and identity additions with a failing CHECK.
-- 2. Skip an empty column and confirm that no check was installed.
-- 3. Add a new checked column, then skip that column in the same ALTER.
-- 4. Confirm the new check remains and rejects an invalid value.
\set ON_ERROR_STOP off
create temp table check_skip (a int);
insert into check_skip values (-1);
alter table check_skip add column if not exists a int check (a > 0);
alter table check_skip add column if not exists a serial check (a > 0);
alter table check_skip add column if not exists a int
  generated always as identity check (a > 0);
select count(*) from pg_constraint
  where conrelid = 'check_skip'::regclass and contype = 'c';
create temp table check_skip_empty (a int);
alter table check_skip_empty add column if not exists a int check (a > 0);
insert into check_skip_empty values (-1);
select count(*) from pg_constraint
  where conrelid = 'check_skip_empty'::regclass and contype = 'c';
alter table check_skip add column if not exists b int default 1 check (b > 0),
  add column if not exists b int check (b < 0);
select a, b from check_skip;
select count(*) from pg_constraint
  where conrelid = 'check_skip'::regclass and contype = 'c';
insert into check_skip values (2, -1);
