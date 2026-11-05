-- mkbtidx.sql -- usertable's secondary indexes, btree: on lastname
-- and on state.
--
--   psql -d bench -f mkbtidx.sql
--
-- On digres USING btree makes a tree (../sm/BTREEINDEX.md); its default
-- kind is lsm. PostgreSQL makes its B-tree, as mkregidx.sql does.

drop index if exists usertable_lastname;
drop index if exists usertable_state;
drop index if exists usertable_lastname_tree;
drop index if exists usertable_state_tree;

create index usertable_lastname_tree on usertable using btree (lastname);
create index usertable_state_tree on usertable using btree (state);

-- Vacuumed, as a server's autovacuum would have: PostgreSQL's
-- visibility map, and digres's trees' all-visible leaves
-- (../sm/BTREEINDEX.md section 8), let an index-only scan skip the rows.
vacuum analyze usertable;
