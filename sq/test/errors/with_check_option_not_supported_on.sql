-- Verify that sq refuses this as PostgreSQL does, with its message:
-- WITH CHECK OPTION not supported on recursive views
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE RECURSIVE VIEW v (a) AS SELECT 1 WITH CHECK OPTION;
