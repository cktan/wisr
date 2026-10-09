-- Verify that sq refuses this as PostgreSQL does, with its message:
-- cannot use multiple ORDER BY clauses with WITHIN GROUP
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT f(a ORDER BY a) WITHIN GROUP (ORDER BY b);
