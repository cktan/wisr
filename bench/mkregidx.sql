-- mkregidx.sql -- usertable's secondary indexes, regular: on
-- lastname and on state.
--
--   psql -d bench -f mkregidx.sql

drop index if exists usertable_lastname;
drop index if exists usertable_state;
drop index if exists usertable_lastname_tree;
drop index if exists usertable_state_tree;

create index usertable_lastname on usertable (lastname);
create index usertable_state on usertable (state);

-- Vacuumed, as a server's autovacuum would have: PostgreSQL's
-- visibility map, and digres's trees' all-visible leaves
-- (../sm/BTREE.md section 4.1), let an index-only scan skip the rows.
vacuum analyze usertable;
