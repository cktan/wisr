-- Verify that SETOF and %TYPE parse as they do in a function signature.
-- 1. A set of the type of a column.
setof s.t.c%TYPE
