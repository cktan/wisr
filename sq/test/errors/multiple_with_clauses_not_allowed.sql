-- Verify that sq refuses this as PostgreSQL does, with its message:
-- multiple WITH clauses not allowed
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
WITH a AS (SELECT 1) (WITH b AS (SELECT 2) SELECT 1);
