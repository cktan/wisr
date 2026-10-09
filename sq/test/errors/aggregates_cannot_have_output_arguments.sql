-- Verify that sq refuses this as PostgreSQL does, with its message:
-- aggregates cannot have output arguments
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE AGGREGATE a(OUT int) (sfunc = f, stype = int);
