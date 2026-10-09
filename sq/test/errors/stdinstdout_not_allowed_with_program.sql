-- Verify that sq refuses this as PostgreSQL does, with its message:
-- STDIN/STDOUT not allowed with PROGRAM
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
COPY t FROM PROGRAM STDIN;
