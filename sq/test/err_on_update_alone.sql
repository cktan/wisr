-- Verify that ON UPDATE is taken only after a DEFAULT: alone, it is a
-- syntax error at ON.
-- 1. A column with ON UPDATE and no default.
CREATE TABLE a (m timestamptz ON UPDATE);
