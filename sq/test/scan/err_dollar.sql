-- Verify that a dollar quote never closed is refused where it opens.
-- 1. A dollar-quoted string whose closing tag differs.
SELECT $a$ text $b$;
