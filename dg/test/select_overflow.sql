-- Verify that an error raised while planning, when a constant
-- expression is folded, is reported the same way.
-- 1. Add past the largest integer, which the planner evaluates.
SELECT 2147483647 + 1;
