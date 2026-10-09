-- Verify that sq refuses this as PostgreSQL does, with its message:
-- CREATE SCHEMA IF NOT EXISTS cannot include schema elements
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE SCHEMA IF NOT EXISTS s CREATE TABLE t (a int);
