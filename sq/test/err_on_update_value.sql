-- Verify that ON UPDATE takes no value of its own: MySQL's form, which
-- names one, is a syntax error at the value.
-- 1. DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP.
CREATE TABLE a (m timestamptz DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP);
