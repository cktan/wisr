-- Verify that sq refuses this as PostgreSQL does, with its message:
-- an ordered-set aggregate with a VARIADIC direct argument must have one
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE AGGREGATE a(VARIADIC int[] ORDER BY int) (sfunc = f, stype = int);
