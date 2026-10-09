-- Verify that sq refuses this as PostgreSQL does, with its message:
-- WITH TIES cannot be specified without ORDER BY clause
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT * FROM t FETCH FIRST 1 ROW WITH TIES;
