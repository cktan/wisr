-- upd.sql -- update a column with no index, in one row found by the
-- primary key. rows is the number of rows in t (pgbench -D).
--
--   pgbench -n -M prepared -D rows=100000 -f upd.sql

\set k random(1, :rows)
update t set c16 = c16 + 1 where id = :k;
