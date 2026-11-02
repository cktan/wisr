-- q3.sql -- the phones of one lastname, Last1 to Last1000: phone is
-- in no index, so each user's row is read, by either kind of index.
--
--   pgbench -d bench -n -f q3.sql

\set n random(1, 1000)
select phone from usertable where lastname = 'Last' || (:n)::int;
