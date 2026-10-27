-- Verify that a foreign key's ON DELETE SET DEFAULT or SET NULL column
-- list keeps a column named twice once, as PostgreSQL's does
-- (WORKNOTE.md step 308): the action's UPDATE sets each column once, and
-- the parent's row is deleted. Temporary tables, as the 8 sessions run.sh
-- starts share one database.
-- 1. A key on (tid, d) with ON DELETE SET DEFAULT (d, d): its definition
--    names d once.
-- 2. Deleting the referenced parent row sets the child's d to its
--    default.
-- 3. ON DELETE SET NULL (d, d, d), added by ALTER TABLE: named once, and
--    the child's d set to null.
create temp table pk (tid int, id int, primary key (tid, id));
create temp table fk (tid int, d int default 0,
  foreign key (tid, d) references pk on delete set default (d, d));
select pg_get_constraintdef(oid) from pg_constraint
  where conrelid = 'fk'::regclass and contype = 'f';
insert into pk values (1, 0), (1, 5), (1, 6);
insert into fk values (1, 5);
delete from pk where id = 5;
select * from fk;
create temp table fn (tid int, d int);
alter table fn add foreign key (tid, d) references pk
  on delete set null (d, d, d);
select pg_get_constraintdef(oid) from pg_constraint
  where conrelid = 'fn'::regclass and contype = 'f';
insert into fn values (1, 6);
delete from pk where id = 6;
select * from fn;
