-- Verify that sq refuses this as PostgreSQL does, with its message:
-- precision for type float must be at least 1 bit
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT 1::float(0);
