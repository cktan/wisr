-- Verify that sq refuses this as PostgreSQL does, with its message:
-- LIMIT #,# syntax is not supported
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT * FROM t LIMIT 1, 2;
