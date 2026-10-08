-- Verify that schema statements parse, the ones the storage stack has
-- a counterpart for first.
-- 1. CREATE TABLE of integer columns, the spike's table.
-- 2. CREATE TABLE with constraints and defaults.
-- 3. CREATE INDEX, plain and unique with INCLUDE.
-- 4. ALTER TABLE adding and dropping a column.
-- 5. CREATE SCHEMA, CREATE SEQUENCE and CREATE VIEW.
-- 6. DROP of a table and of an index.
-- 7. Transaction control.
CREATE TABLE t (a int, b bigint, c smallint);
CREATE TABLE u (id int PRIMARY KEY, name text NOT NULL DEFAULT 'x', t_a int REFERENCES t (a), CHECK (id > 0));
CREATE INDEX t_a ON t (a);
CREATE UNIQUE INDEX u_name ON u (name DESC NULLS LAST) INCLUDE (t_a);
ALTER TABLE t ADD COLUMN d int, DROP COLUMN c;
CREATE SCHEMA s;
CREATE SEQUENCE seq START 10 INCREMENT 2;
CREATE VIEW v AS SELECT a FROM t;
DROP TABLE IF EXISTS t CASCADE;
DROP INDEX u_name;
BEGIN ISOLATION LEVEL SERIALIZABLE;
SAVEPOINT sp;
ROLLBACK TO sp;
COMMIT;
