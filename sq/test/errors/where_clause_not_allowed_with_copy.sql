-- Verify that sq refuses this as PostgreSQL does, with its message:
-- WHERE clause not allowed with COPY TO
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
COPY t TO stdout WHERE a > 1;
