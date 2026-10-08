-- Verify that SELECT in its common shapes parses to the tree PostgreSQL
-- builds for it.
-- 1. A constant expression with no FROM.
-- 2. A filtered, sorted, limited query over one table.
-- 3. An inner and an outer join with aliases.
-- 4. Grouping with an aggregate and HAVING.
-- 5. A subquery in FROM and one in WHERE.
-- 6. A set operation.
-- 7. A common table expression.
-- 8. A window function.
SELECT 1 + 1;
SELECT a, b AS bee FROM t WHERE x = 3 AND y <> 'no' ORDER BY a DESC LIMIT 10 OFFSET 5;
SELECT t.a, u.b FROM t JOIN u ON t.id = u.id LEFT JOIN v USING (id);
SELECT k, count(*), sum(v) FROM t GROUP BY k HAVING count(*) > 1;
SELECT * FROM (SELECT a FROM t) AS s WHERE a IN (SELECT a FROM u);
SELECT a FROM t UNION ALL SELECT a FROM u;
WITH w AS (SELECT 1 AS n) SELECT n FROM w;
SELECT rank() OVER (PARTITION BY k ORDER BY v) FROM t;
