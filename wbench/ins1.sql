-- ins1.sql -- insert one row, in its own transaction. The value is
-- random in a range of 9e18, so a unique index sees no duplicate.
--
--   pgbench -n -M prepared -f ins1.sql

\set r random(1, 9000000000000000000)
insert into t (c1, c2, c3, c4, c5, c6, c7, c8,
               c9, c10, c11, c12, c13, c14, c15, c16)
values (:r, :r, :r, :r, :r, :r, :r, :r,
        :r, :r, :r, :r, :r, :r, :r, :r);
