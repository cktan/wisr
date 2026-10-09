-- Verify that sq refuses this as PostgreSQL does, with its message:
-- sequence option "as" not supported here
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
ALTER TABLE t ALTER COLUMN a SET AS int;
