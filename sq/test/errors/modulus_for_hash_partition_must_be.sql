-- Verify that sq refuses this as PostgreSQL does, with its message:
-- modulus for hash partition must be specified
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE TABLE p PARTITION OF t FOR VALUES WITH (remainder 1);
