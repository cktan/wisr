-- q2-seckey180.sql -- count the users of one state, CA.
--
--   pgbench -d bench -n -f q2-seckey180.sql

select count(*) from usertable where state = 'CA';
