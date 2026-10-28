-- Q11's write (DESIGN.md section 5): one row of wide_mixed by its c2,
-- Zipfian over c2's values, updated; run with q10.sql, 1 in 10.
\set k random_zipfian(3, 1000002, 1.1)
update wide_mixed set c1 = c1 + 1 where c2 = :k;
