-- Verify that sq refuses this as PostgreSQL does, with its message:
-- UNIQUE constraints cannot be marked NOT VALID
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE TABLE t (a int, UNIQUE (a) NOT VALID);
