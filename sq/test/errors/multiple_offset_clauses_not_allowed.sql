-- Verify that sq refuses this as PostgreSQL does, with its message:
-- multiple OFFSET clauses not allowed
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
(SELECT 1 OFFSET 1) OFFSET 2;
