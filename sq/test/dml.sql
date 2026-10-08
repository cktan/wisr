-- Verify that the statements that change rows parse.
-- 1. INSERT of two rows naming its columns.
-- 2. INSERT from a query, with ON CONFLICT and RETURNING.
-- 3. UPDATE of two columns with a WHERE.
-- 4. DELETE with USING.
-- 5. SELECT FOR UPDATE, which locks rows.
INSERT INTO t (a, b) VALUES (1, 'one'), (2, 'two');
INSERT INTO t SELECT a, b FROM u ON CONFLICT (a) DO UPDATE SET b = excluded.b RETURNING a;
UPDATE t SET a = a + 1, b = DEFAULT WHERE a < 10;
DELETE FROM t USING u WHERE t.a = u.a;
SELECT a FROM t WHERE a = 1 FOR UPDATE NOWAIT;
