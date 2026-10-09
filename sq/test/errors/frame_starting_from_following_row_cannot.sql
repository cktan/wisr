-- Verify that sq refuses this as PostgreSQL does, with its message:
-- frame starting from following row cannot have preceding rows
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT sum(a) OVER (ROWS BETWEEN 1 FOLLOWING AND CURRENT ROW) FROM t;
