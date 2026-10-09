-- Verify that sq refuses this as PostgreSQL does, with its message:
-- CREATE EXTENSION ... FROM is no longer supported
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE EXTENSION e FROM '1.0';
