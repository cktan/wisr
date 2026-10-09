-- Verify that sq refuses this as PostgreSQL does, with its message:
-- OUT and INOUT arguments aren't allowed in TABLE functions
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE FUNCTION f(OUT a int) RETURNS TABLE (b int) AS 'x' LANGUAGE sql;
