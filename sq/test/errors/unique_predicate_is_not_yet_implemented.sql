-- Verify that sq refuses this as PostgreSQL does, with its message:
-- UNIQUE predicate is not yet implemented
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT UNIQUE (SELECT 1);
