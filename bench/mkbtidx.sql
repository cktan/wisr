-- mkbtidx.sql -- usertable's secondary indexes, btree: on lastname
-- and on state.
--
--   psql -d bench -f mkbtidx.sql
--
-- On digres a btree is WITH (tree = on) (../sm/BTREE.md). PostgreSQL
-- has no other kind, and makes its B-tree, as mkregidx.sql does.

drop index if exists usertable_lastname;
drop index if exists usertable_state;
drop index if exists usertable_lastname_tree;
drop index if exists usertable_state_tree;

-- digres has no wal_level: its log is RocksDB's.
select current_setting('wal_level', true) is null as digres \gset
\if :digres
create index usertable_lastname_tree on usertable (lastname)
  with (tree = on);
create index usertable_state_tree on usertable (state) with (tree = on);
\else
create index usertable_lastname_tree on usertable (lastname);
create index usertable_state_tree on usertable (state);
\endif

analyze usertable;
