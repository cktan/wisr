-- Verify that sq refuses this as PostgreSQL does, with its message:
-- conflicting or redundant NULL / NOT NULL declarations for column "a"
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT * FROM XMLTABLE('/a' PASSING x COLUMNS a int NULL NOT NULL);
