-- Verify that sq refuses this as PostgreSQL does, with its message:
-- CURRENT_USER cannot be used as a role name here
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE ROLE current_user;
