-- Verify that sq refuses this as PostgreSQL does, with its message:
-- time zone interval must be HOUR or HOUR TO MINUTE
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SET TIME ZONE INTERVAL '1' YEAR;
