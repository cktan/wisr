-- Verify that sq refuses this as PostgreSQL does, with its message:
-- improper qualified name (too many dotted names): a.b.c.d
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT * FROM a.b.c.d;
