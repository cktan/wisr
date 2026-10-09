-- Verify that sq refuses this as PostgreSQL does, with its message:
-- sequence option "owned_by" not supported here
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
ALTER TABLE t ALTER COLUMN a SET OWNED BY NONE;
