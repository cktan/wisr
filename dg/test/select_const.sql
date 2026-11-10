-- Verify that a statement over constants runs with no catalog tables:
-- the operators, their functions and the types are found compiled in
-- (sm/PGFORK.md section 10, the second thing to run).
-- 1. Add two integers: the first statement digres ran.
-- 2. Multiply, divide and subtract integers in one target list.
-- 3. Compare integers, which yields booleans.
-- 4. Name the columns, and filter on a constant condition.
SELECT 1 + 1;
SELECT 2 * 3, 10 / 3, 7 - 9;
SELECT 1 < 2, 3 = 3, 4 > 5;
SELECT 1 + 1 AS two, 4 AS four WHERE 1 = 1;
