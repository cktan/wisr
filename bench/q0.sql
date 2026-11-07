-- q0.sql -- the zip of one user, by id, 1 to 10000: a lookup of one
-- row by the primary key. zip is in no index, so the row is read.
--
--   pgbench -d bench -n -f q0.sql

\set n random(1, 10000)
select zip from usertable where id = :n;
