-- Verify that sq refuses this as PostgreSQL does, with its message:
-- only string constants are supported in JSON_TABLE path specification
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT * FROM JSON_TABLE(x, a COLUMNS (b int));
