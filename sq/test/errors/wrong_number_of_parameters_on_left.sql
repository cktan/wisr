-- Verify that sq refuses this as PostgreSQL does, with its message:
-- wrong number of parameters on left side of OVERLAPS expression
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT (1, 2, 3) OVERLAPS (1, 2);
