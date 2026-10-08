-- Verify that a reserved word is refused as a table name, at its
-- position.
-- 1. CREATE TABLE named with the reserved word "select".
CREATE TABLE select (a int);
