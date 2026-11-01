-- Q8's write (DESIGN.md section 5): one row of wide_mixed by its key,
-- Zipfian, updated; run with q7.sql, 1 in 10.
\set k random_zipfian(1, 1000000, 1.1)
update wide_mixed set c1 = c1 + 1 where id = :k;
