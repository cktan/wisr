-- Verify that a syntax error is reported with the token and its
-- position, and nothing is printed for the statements around it.
-- 1. A valid statement.
-- 2. A SELECT with no target list before WHERE's operand.
SELECT 1;
SELECT FROM WHERE;
