-- Verify that sq refuses this as PostgreSQL does, with its message:
-- a column list with SET NULL is only supported for ON DELETE actions
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE TABLE t (a int, FOREIGN KEY (a) REFERENCES r ON UPDATE SET NULL (a));
