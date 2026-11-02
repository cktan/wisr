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

analyze usertable;
