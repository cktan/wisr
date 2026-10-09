-- Verify that sq refuses this as PostgreSQL does, with its message:
-- type modifier cannot have parameter name
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
SELECT foo(a => 1) 'x';
