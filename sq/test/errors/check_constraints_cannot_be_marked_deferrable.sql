-- Verify that sq refuses this as PostgreSQL does, with its message:
-- CHECK constraints cannot be marked DEFERRABLE
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE TABLE t (a int, CHECK (a > 0) DEFERRABLE);
