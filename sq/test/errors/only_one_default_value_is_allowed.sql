-- Verify that sq refuses this as PostgreSQL does, with its message:
-- only one DEFAULT value is allowed
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT * FROM XMLTABLE('/a' PASSING x COLUMNS a int DEFAULT 1 DEFAULT 2);
