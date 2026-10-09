-- Verify that sq refuses this as PostgreSQL does, with its message:
-- constraint declared INITIALLY DEFERRED must be DEFERRABLE
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE TABLE t (a int, CHECK (a > 0) INITIALLY DEFERRED NOT DEFERRABLE);
