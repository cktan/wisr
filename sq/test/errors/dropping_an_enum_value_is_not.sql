-- Verify that sq refuses this as PostgreSQL does, with its message:
-- dropping an enum value is not implemented
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
ALTER TYPE e DROP VALUE 'a';
