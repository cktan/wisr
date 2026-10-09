-- Verify that a string never closed is refused where it opens.
-- 1. A quoted string with no closing quote.
SELECT 'never closed;
