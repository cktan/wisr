-- A key moved (btree.sh): one row of wide_mixed by its c2, uniform, its
-- c2 negated, so the c2 index writes an entry at a new key; run with
-- q10.sql, 1 in 10.
\set k random(3, 1000002)
update wide_mixed set c2 = -c2 where c2 = :k;
