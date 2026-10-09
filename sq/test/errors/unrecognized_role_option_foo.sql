-- Verify that sq refuses this as PostgreSQL does, with its message:
-- unrecognized role option "foo"
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE ROLE r FOO;
