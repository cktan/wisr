-- Verify that sq refuses this as PostgreSQL does, with its message:
-- frame start cannot be UNBOUNDED FOLLOWING
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT sum(a) OVER (ROWS UNBOUNDED FOLLOWING) FROM t;
