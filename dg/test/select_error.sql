-- Verify that an error raised deep in the executor reaches the top as
-- PostgreSQL's error, and that what ran before it has been printed: an
-- ERROR is a C++ exception here, where PostgreSQL jumps
-- (sm/PGFORK.md section 2).
-- 1. Run a statement that succeeds.
-- 2. Divide by zero: the function raises, some frames below psql's
--    statement.
-- 3. Follow with a statement that must not run.
SELECT 6 * 7;
SELECT 1 / 0;
SELECT 99;
