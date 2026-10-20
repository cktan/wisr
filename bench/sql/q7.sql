-- Q7 (DESIGN.md section 5): one row of wide_mixed by its key.
\set k random(1, 1000000)
select c1 from wide_mixed where id = :k;
