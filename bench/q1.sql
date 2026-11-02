-- q1.sql -- count the users of one lastname, Last1 to Last1000.
--
--   pgbench -d bench -n -f q1.sql

\set n random(1, 1000)
select count(*) from usertable where lastname = 'Last' || (:n)::int;
