-- Verify that ON UPDATE after a column's DEFAULT parses as that
-- default, marked by generated_when 'u', digres's one addition to
-- PostgreSQL's grammar (../../dg/ONUPDATE.md section 4), and that a
-- foreign key's ON UPDATE parses as before.
-- 1. CURRENT_TIMESTAMP, LOCALTIMESTAMP and CURRENT_DATE defaults with
--    ON UPDATE, one followed by NOT NULL: each a DEFAULT constraint
--    with generated_when 'u'.
-- 2. The same default without ON UPDATE: generated_when unset.
-- 3. ADD COLUMN with any expression before ON UPDATE: parsed; which
--    defaults are taken is digres's to say, not the grammar's.
-- 4. A default before a foreign key's ON UPDATE CASCADE, and a foreign
--    key's ON UPDATE and ON DELETE: the foreign key's actions, as in
--    PostgreSQL.
-- 5. A plain default then an update default on one column: two DEFAULT
--    constraints, the first unmarked, the second marked 'u'.
CREATE TABLE a (m timestamptz DEFAULT CURRENT_TIMESTAMP ON UPDATE,
                l timestamp DEFAULT LOCALTIMESTAMP ON UPDATE,
                d date DEFAULT CURRENT_DATE ON UPDATE NOT NULL);
CREATE TABLE b (m timestamptz DEFAULT CURRENT_TIMESTAMP);
ALTER TABLE a ADD COLUMN n timestamptz DEFAULT now() ON UPDATE;
CREATE TABLE c (p int DEFAULT 1 REFERENCES q ON UPDATE CASCADE);
CREATE TABLE e (p int REFERENCES q ON UPDATE CASCADE ON DELETE SET NULL);
CREATE TABLE f (s int NOT NULL DEFAULT 0 DEFAULT 1 ON UPDATE);
