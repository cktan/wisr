-- Verify that sq refuses this as PostgreSQL does, with its message:
-- SKIP LOCKED and WITH TIES options cannot be used together
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT * FROM t ORDER BY 1 FETCH FIRST 1 ROW WITH TIES FOR UPDATE SKIP LOCKED;
