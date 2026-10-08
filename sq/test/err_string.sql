-- Verify that the scanner reports a string that is never closed.
-- 1. A quoted string with no closing quote.
SELECT 'never closed;
