-- Verify that sq refuses this as PostgreSQL does, with its message:
-- multiple COLLATE clauses not allowed
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE TABLE t (a text COLLATE "C" COLLATE "C");
