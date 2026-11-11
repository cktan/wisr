-- ins1k.sql -- insert 1000 rows in one transaction, one statement:
-- the cost of a row, with the commit spread over 1000.
--
--   pgbench -n -M prepared -f ins1k.sql

insert into t (c1, c2, c3, c4, c5, c6, c7, c8,
               c9, c10, c11, c12, c13, c14, c15, c16)
select r, r, r, r, r, r, r, r, r, r, r, r, r, r, r, r
from (select (random() * 1e18)::int8 as r
      from generate_series(1, 1000)) s;
