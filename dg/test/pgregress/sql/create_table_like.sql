/* Test inheritance of structure (LIKE) */
CREATE TABLE inhx (xx text DEFAULT 'text');

/*
 * Test double inheritance
 *
 * Ensure that defaults are NOT included unless
 * INCLUDING DEFAULTS is specified
 */
CREATE TABLE ctla (aa TEXT);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE ctlb (bb TEXT) INHERITS (ctla);

CREATE TABLE foo (LIKE nonexistent);

--dg-cascade
--dg CREATE TABLE inhe (ee text, LIKE inhx) inherits (ctlb);
--dg-cascade
--dg INSERT INTO inhe VALUES ('ee-col1', 'ee-col2', DEFAULT, 'ee-col4');
--dg-cascade
--dg SELECT * FROM inhe;
 /* Columns aa, bb, xx value NULL, ee */
SELECT * FROM inhx; /* Empty set since LIKE inherits structure only */
--dg-cascade
--dg SELECT * FROM ctlb;
 /* Has ee entry */
--dg-cascade
--dg SELECT * FROM ctla;
 /* Has ee entry */

CREATE TABLE inhf (LIKE inhx, LIKE inhx); /* Throw error */

--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg CREATE TABLE inhf (LIKE inhx INCLUDING DEFAULTS INCLUDING CONSTRAINTS);
--dg-cascade
--dg INSERT INTO inhf DEFAULT VALUES;
--dg-cascade
--dg SELECT * FROM inhf;
 /* Single entry with value 'text' */

--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY yet
--dg ALTER TABLE inhx add constraint foo CHECK (xx = 'text');
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY yet
--dg ALTER TABLE inhx ADD PRIMARY KEY (xx);
CREATE TABLE inhg (LIKE inhx); /* Doesn't copy constraint */
INSERT INTO inhg VALUES ('foo');
DROP TABLE inhg;
CREATE TABLE inhg (x text, LIKE inhx INCLUDING CONSTRAINTS, y text); /* Copies constraints */
INSERT INTO inhg VALUES ('x', 'text', 'y'); /* Succeeds */
INSERT INTO inhg VALUES ('x', 'text', 'y'); /* Succeeds -- Unique constraints not copied */
--dg-cascade
--dg INSERT INTO inhg VALUES ('x', 'foo',  'y');
  /* fails due to constraint */
--dg-cascade
--dg SELECT * FROM inhg;
 /* Two records with three columns in order x=x, xx=text, y=y */
DROP TABLE inhg;

CREATE TABLE test_like_id_1 (a bigint GENERATED ALWAYS AS IDENTITY, b text);
\d test_like_id_1
INSERT INTO test_like_id_1 (b) VALUES ('b1');
SELECT * FROM test_like_id_1;
--dg-skip: relation pg_constraint cannot be opened: digres has no catalog tables
--dg CREATE TABLE test_like_id_2 (LIKE test_like_id_1);
--dg-cascade
--dg \d test_like_id_2
--dg-cascade
--dg INSERT INTO test_like_id_2 (b) VALUES ('b2');
--dg-cascade
--dg SELECT * FROM test_like_id_2;  -- identity was not copied
--dg-skip: copying a sequence's options is not supported yet
--dg CREATE TABLE test_like_id_3 (LIKE test_like_id_1 INCLUDING IDENTITY);
--dg-cascade
--dg \d test_like_id_3
--dg-cascade
--dg INSERT INTO test_like_id_3 (b) VALUES ('b3');
--dg-cascade
--dg SELECT * FROM test_like_id_3;  -- identity was copied and applied
--dg-cascade
--dg DROP TABLE test_like_id_1, test_like_id_2, test_like_id_3;

--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE test_like_gen_1 (a int, b int GENERATED ALWAYS AS (a * 2) STORED, c int GENERATED ALWAYS AS (a * 3) VIRTUAL);
--dg-cascade
--dg \d test_like_gen_1
--dg-cascade
--dg INSERT INTO test_like_gen_1 (a) VALUES (1);
--dg-cascade
--dg SELECT * FROM test_like_gen_1;
--dg-cascade
--dg CREATE TABLE test_like_gen_2 (LIKE test_like_gen_1);
--dg-cascade
--dg \d test_like_gen_2
--dg-cascade
--dg INSERT INTO test_like_gen_2 (a) VALUES (1);
--dg-cascade
--dg SELECT * FROM test_like_gen_2;
--dg-cascade
--dg CREATE TABLE test_like_gen_3 (LIKE test_like_gen_1 INCLUDING GENERATED);
--dg-cascade
--dg \d test_like_gen_3
--dg-cascade
--dg INSERT INTO test_like_gen_3 (a) VALUES (1);
--dg-cascade
--dg SELECT * FROM test_like_gen_3;
--dg-cascade
--dg DROP TABLE test_like_gen_1, test_like_gen_2, test_like_gen_3;

-- also test generated column with a "forward" reference (bug #16342)
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE test_like_4 (b int DEFAULT 42,
--dg   c int GENERATED ALWAYS AS (a * 2) STORED,
--dg   a int CHECK (a > 0));
--dg-cascade
--dg \d test_like_4
--dg-cascade
--dg CREATE TABLE test_like_4a (LIKE test_like_4);
--dg-cascade
--dg CREATE TABLE test_like_4b (LIKE test_like_4 INCLUDING DEFAULTS);
--dg-cascade
--dg CREATE TABLE test_like_4c (LIKE test_like_4 INCLUDING GENERATED);
--dg-cascade
--dg CREATE TABLE test_like_4d (LIKE test_like_4 INCLUDING DEFAULTS INCLUDING GENERATED);
--dg-cascade
--dg \d test_like_4a
--dg-cascade
--dg INSERT INTO test_like_4a (a) VALUES(11);
--dg-cascade
--dg SELECT a, b, c FROM test_like_4a;
--dg-cascade
--dg \d test_like_4b
--dg-cascade
--dg INSERT INTO test_like_4b (a) VALUES(11);
--dg-cascade
--dg SELECT a, b, c FROM test_like_4b;
--dg-cascade
--dg \d test_like_4c
--dg-cascade
--dg INSERT INTO test_like_4c (a) VALUES(11);
--dg-cascade
--dg SELECT a, b, c FROM test_like_4c;
--dg-cascade
--dg \d test_like_4d
--dg-cascade
--dg INSERT INTO test_like_4d (a) VALUES(11);
--dg-cascade
--dg SELECT a, b, c FROM test_like_4d;

-- Test renumbering of Vars when combining LIKE with inheritance
--dg-skip: type ? is not supported yet
--dg CREATE TABLE test_like_5 (x point, y point, z point);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE test_like_5x (p int CHECK (p > 0),
--dg    q int GENERATED ALWAYS AS (p * 2) STORED);
--dg-cascade
--dg CREATE TABLE test_like_5c (LIKE test_like_4 INCLUDING ALL)
--dg   INHERITS (test_like_5, test_like_5x);
--dg-cascade
--dg \d test_like_5c

-- Test updating of column numbers in statistics expressions (bug #18468)
CREATE TABLE test_like_6 (a int, c text, b text);
--dg-skip: relation pg_statistic_ext cannot be opened: digres has no catalog tables
--dg CREATE STATISTICS ext_stat ON (a || b) FROM test_like_6;
ALTER TABLE test_like_6 DROP COLUMN c;
--dg-skip: relation pg_description cannot be opened: digres has no catalog tables
--dg CREATE TABLE test_like_6c (LIKE test_like_6 INCLUDING ALL);
--dg-cascade
--dg \d+ test_like_6c

--dg-cascade
--dg DROP TABLE test_like_4, test_like_4a, test_like_4b, test_like_4c, test_like_4d;
--dg-cascade
--dg DROP TABLE test_like_5, test_like_5x, test_like_5c;
--dg-cascade
--dg DROP TABLE test_like_6, test_like_6c;

CREATE TABLE inhg (x text, LIKE inhx INCLUDING INDEXES, y text); /* copies indexes */
INSERT INTO inhg VALUES (5, 10);
--dg-cascade
--dg INSERT INTO inhg VALUES (20, 10); -- should fail
DROP TABLE inhg;
/* Multiple primary keys creation should fail */
--dg-cascade
--dg CREATE TABLE inhg (x text, LIKE inhx INCLUDING INDEXES, PRIMARY KEY(x));
 /* fails */
CREATE TABLE inhz (xx text DEFAULT 'text', yy int UNIQUE);
CREATE UNIQUE INDEX inhz_xx_idx on inhz (xx) WHERE xx <> 'test';
/* Ok to create multiple unique indexes */
--dg-cascade
--dg CREATE TABLE inhg (x text UNIQUE, LIKE inhz INCLUDING INDEXES);
--dg-cascade
--dg INSERT INTO inhg (xx, yy, x) VALUES ('test', 5, 10);
--dg-cascade
--dg INSERT INTO inhg (xx, yy, x) VALUES ('test', 10, 15);
--dg-cascade
--dg INSERT INTO inhg (xx, yy, x) VALUES ('foo', 10, 15); -- should fail
--dg-cascade
--dg DROP TABLE inhg;
DROP TABLE inhz;

/* Use primary key imported by LIKE for self-referential FK constraint */
--dg-cascade
--dg CREATE TABLE inhz (x text REFERENCES inhz, LIKE inhx INCLUDING INDEXES);
--dg-cascade
--dg \d inhz
--dg-cascade
--dg DROP TABLE inhz;

-- including storage and comments
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE ctlt1 (a text CHECK (length(a) > 2) ENFORCED PRIMARY KEY,
--dg 	b text CHECK (length(b) > 100) NOT ENFORCED);
--dg-cascade
--dg ALTER TABLE ctlt1 ADD CONSTRAINT cc CHECK (length(b) > 100) NOT VALID;
--dg-cascade
--dg CREATE INDEX ctlt1_b_key ON ctlt1 (b);
--dg-cascade
--dg CREATE INDEX ctlt1_fnidx ON ctlt1 ((a || b));
--dg-cascade
--dg CREATE STATISTICS ctlt1_a_b_stat ON a,b FROM ctlt1;
--dg-cascade
--dg CREATE STATISTICS ctlt1_expr_stat ON (a || b) FROM ctlt1;
--dg-skip: relation pg_statistic_ext cannot be opened: digres has no catalog tables
--dg COMMENT ON STATISTICS ctlt1_a_b_stat IS 'ab stats';
--dg-skip: relation pg_statistic_ext cannot be opened: digres has no catalog tables
--dg COMMENT ON STATISTICS ctlt1_expr_stat IS 'ab expr stats';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN ctlt1.a IS 'A';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN ctlt1.b IS 'B';
--dg-cascade
--dg COMMENT ON CONSTRAINT ctlt1_a_check ON ctlt1 IS 't1_a_check';
--dg-cascade
--dg COMMENT ON INDEX ctlt1_pkey IS 'index pkey';
--dg-cascade
--dg COMMENT ON INDEX ctlt1_b_key IS 'index b_key';
--dg-cascade
--dg ALTER TABLE ctlt1 ALTER COLUMN a SET STORAGE MAIN;

CREATE TABLE ctlt2 (c text NOT NULL);
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE ctlt2 ALTER COLUMN c SET STORAGE EXTERNAL;
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN ctlt2.c IS 'C';
--dg-skip: relation pg_constraint cannot be opened: digres has no catalog tables
--dg COMMENT ON CONSTRAINT ctlt2_c_not_null ON ctlt2 IS 't2_c_not_null';

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE ctlt3 (a text CHECK (length(a) < 5), c text CHECK (length(c) < 7));
--dg-cascade
--dg ALTER TABLE ctlt3 ALTER COLUMN c SET STORAGE EXTERNAL;
--dg-cascade
--dg ALTER TABLE ctlt3 ALTER COLUMN a SET STORAGE MAIN;
--dg-cascade
--dg CREATE INDEX ctlt3_fnidx ON ctlt3 ((a || c));
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN ctlt3.a IS 'A3';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN ctlt3.c IS 'C';
--dg-cascade
--dg COMMENT ON CONSTRAINT ctlt3_a_check ON ctlt3 IS 't3_a_check';

CREATE TABLE ctlt4 (a text, c text);
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE ctlt4 ALTER COLUMN c SET STORAGE EXTERNAL;

--dg-cascade
--dg CREATE TABLE ctlt12_storage (LIKE ctlt1 INCLUDING STORAGE, LIKE ctlt2 INCLUDING STORAGE);
--dg-cascade
--dg \d+ ctlt12_storage
--dg-cascade
--dg CREATE TABLE ctlt12_comments (LIKE ctlt1 INCLUDING COMMENTS, LIKE ctlt2 INCLUDING COMMENTS);
--dg-cascade
--dg \d+ ctlt12_comments
--dg-cascade
--dg SELECT conname, description FROM pg_description, pg_constraint c WHERE classoid = 'pg_constraint'::regclass AND objoid = c.oid AND c.conrelid = 'ctlt12_comments'::regclass;
--dg-cascade
--dg CREATE TABLE ctlt1_inh (LIKE ctlt1 INCLUDING CONSTRAINTS INCLUDING COMMENTS) INHERITS (ctlt1);
--dg-cascade
--dg \d+ ctlt1_inh
--dg-cascade
--dg SELECT description FROM pg_description, pg_constraint c WHERE classoid = 'pg_constraint'::regclass AND objoid = c.oid AND c.conrelid = 'ctlt1_inh'::regclass;
--dg-cascade
--dg CREATE TABLE ctlt13_inh () INHERITS (ctlt1, ctlt3);
--dg-cascade
--dg \d+ ctlt13_inh
--dg-cascade
--dg CREATE TABLE ctlt13_like (LIKE ctlt3 INCLUDING CONSTRAINTS INCLUDING INDEXES INCLUDING COMMENTS INCLUDING STORAGE) INHERITS (ctlt1);
--dg-cascade
--dg \d+ ctlt13_like
--dg-cascade
--dg SELECT description FROM pg_description, pg_constraint c WHERE classoid = 'pg_constraint'::regclass AND objoid = c.oid AND c.conrelid = 'ctlt13_like'::regclass;

--dg-cascade
--dg CREATE TABLE ctlt_all (LIKE ctlt1 INCLUDING ALL);
--dg-cascade
--dg \d+ ctlt_all
--dg-cascade
--dg SELECT c.relname, objsubid, description FROM pg_description, pg_index i, pg_class c WHERE classoid = 'pg_class'::regclass AND objoid = i.indexrelid AND c.oid = i.indexrelid AND i.indrelid = 'ctlt_all'::regclass ORDER BY c.relname, objsubid;
--dg-cascade
--dg SELECT s.stxname, objsubid, description FROM pg_description, pg_statistic_ext s WHERE classoid = 'pg_statistic_ext'::regclass AND objoid = s.oid AND s.stxrelid = 'ctlt_all'::regclass ORDER BY s.stxname, objsubid;

--dg-cascade
--dg CREATE TABLE inh_error1 () INHERITS (ctlt1, ctlt4);
--dg-cascade
--dg CREATE TABLE inh_error2 (LIKE ctlt4 INCLUDING STORAGE) INHERITS (ctlt1);

-- Check that LIKE isn't confused by a system catalog of the same name
--dg-cascade
--dg CREATE TABLE pg_attrdef (LIKE ctlt1 INCLUDING ALL);
--dg-cascade
--dg \d+ public.pg_attrdef
--dg-cascade
--dg DROP TABLE public.pg_attrdef;

-- Check that LIKE isn't confused when new table masks the old, either
BEGIN;
CREATE SCHEMA ctl_schema;
SET LOCAL search_path = ctl_schema, public;
--dg-cascade
--dg CREATE TABLE ctlt1 (LIKE ctlt1 INCLUDING ALL);
--dg-cascade
--dg \d+ ctlt1
ROLLBACK;

--dg-cascade
--dg DROP TABLE ctlt1, ctlt2, ctlt3, ctlt4, ctlt12_storage, ctlt12_comments, ctlt1_inh, ctlt13_inh, ctlt13_like, ctlt_all, ctla, ctlb CASCADE;

-- LIKE must respect NO INHERIT property of constraints
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE noinh_con_copy (a int CHECK (a > 0) NO INHERIT, b int not null,
--dg 	c int not null no inherit);

--dg-cascade
--dg COMMENT ON CONSTRAINT noinh_con_copy_b_not_null ON noinh_con_copy IS 'not null b';
--dg-cascade
--dg COMMENT ON CONSTRAINT noinh_con_copy_c_not_null ON noinh_con_copy IS 'not null c no inherit';

--dg-cascade
--dg CREATE TABLE noinh_con_copy1 (LIKE noinh_con_copy INCLUDING CONSTRAINTS INCLUDING COMMENTS);
--dg-cascade
--dg \d+ noinh_con_copy1

--dg-cascade
--dg SELECT conname, description
--dg FROM  pg_description, pg_constraint c
--dg WHERE classoid = 'pg_constraint'::regclass
--dg AND   objoid = c.oid AND c.conrelid = 'noinh_con_copy1'::regclass
--dg ORDER BY conname COLLATE "C";

-- fail, as partitioned tables don't allow NO INHERIT constraints
--dg-cascade
--dg CREATE TABLE noinh_con_copy1_parted (LIKE noinh_con_copy INCLUDING ALL)
--dg   PARTITION BY LIST (a);

--dg-cascade
--dg DROP TABLE noinh_con_copy, noinh_con_copy1;


/* LIKE with other relation kinds */

--dg-cascade
--dg CREATE TABLE ctlt4 (a int, b text);

CREATE SEQUENCE ctlseq1;
--dg-differs: another error: "?" is a sequence, which cannot be used as a relation yet
--dg CREATE TABLE ctlt10 (LIKE ctlseq1);  -- fail

CREATE VIEW ctlv1 AS SELECT * FROM ctlt4;
CREATE TABLE ctlt11 (LIKE ctlv1);
--dg-skip: relation pg_description cannot be opened: digres has no catalog tables
--dg CREATE TABLE ctlt11a (LIKE ctlv1 INCLUDING ALL);

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE ctlty1 AS (a int, b text);
--dg-cascade
--dg CREATE TABLE ctlt12 (LIKE ctlty1);

DROP SEQUENCE ctlseq1;
--dg-cascade
--dg DROP TYPE ctlty1;
DROP VIEW ctlv1;
--dg-cascade
--dg DROP TABLE IF EXISTS ctlt4, ctlt10, ctlt11, ctlt11a, ctlt12;

--
-- CREATE FOREIGN TABLE LIKE
--
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER ctl_dummy;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER ctl_s0 FOREIGN DATA WRAPPER ctl_dummy;

--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE ctl_table(a int PRIMARY KEY,
--dg   b varchar COMPRESSION pglz,
--dg   c int GENERATED ALWAYS AS (a * 2) STORED,
--dg   d bigint GENERATED ALWAYS AS IDENTITY,
--dg   e int DEFAULT 1);

--dg-cascade
--dg CREATE INDEX ctl_table_a_key ON ctl_table(a);
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN ctl_table.b IS 'Column b';
--dg-cascade
--dg CREATE STATISTICS ctl_table_stat ON a,b FROM ctl_table;
--dg-cascade
--dg ALTER TABLE ctl_table ADD CONSTRAINT foo CHECK (b = 'text');
--dg-cascade
--dg ALTER TABLE ctl_table ALTER COLUMN b SET STORAGE MAIN;

--dg-cascade
--dg \d+ ctl_table

-- Test EXCLUDING ALL
--dg-cascade
--dg CREATE FOREIGN TABLE ctl_foreign_table1(LIKE ctl_table EXCLUDING ALL) SERVER ctl_s0;
--dg-cascade
--dg \d+ ctl_foreign_table1
-- \d+ does not report the value of attcompression for a foreign table, so
-- check separately.
--dg-cascade
--dg SELECT attname, attcompression FROM pg_attribute
--dg   WHERE attrelid = 'ctl_foreign_table1'::regclass and attnum > 0 ORDER BY attnum;

-- Test INCLUDING ALL
-- INDEXES, IDENTITY, COMPRESSION, STORAGE are not copied.
--dg-cascade
--dg CREATE FOREIGN TABLE ctl_foreign_table2(LIKE ctl_table INCLUDING ALL) SERVER ctl_s0;
--dg-cascade
--dg \d+ ctl_foreign_table2
-- \d+ does not report the value of attcompression for a foreign table, so
-- check separately.
--dg-cascade
--dg SELECT attname, attcompression FROM pg_attribute
--dg   WHERE attrelid = 'ctl_foreign_table2'::regclass and attnum > 0 ORDER BY attnum;

-- LIKE ... INCLUDING STATISTICS with dropped columns in the parent,
-- so stxkeys attnums are not contiguous.
CREATE TABLE ctl_stats3_parent (a int, b int, c int);
ALTER TABLE ctl_stats3_parent DROP COLUMN b;
--dg-skip: relation pg_statistic_ext cannot be opened: digres has no catalog tables
--dg CREATE STATISTICS ctl_stats3_stat ON a, c FROM ctl_stats3_parent;
CREATE TABLE ctl_stats3_child (LIKE ctl_stats3_parent INCLUDING STATISTICS);
CREATE TABLE ctl_stats4_parent (a int, b int, c int, d int);
ALTER TABLE ctl_stats4_parent DROP COLUMN b;
--dg-skip: relation pg_statistic_ext cannot be opened: digres has no catalog tables
--dg CREATE STATISTICS ctl_stats4_stat ON a, c FROM ctl_stats4_parent;
CREATE TABLE ctl_stats4_child (LIKE ctl_stats4_parent INCLUDING STATISTICS);
--dg-cascade
--dg SELECT s.stxrelid::regclass AS relation,
--dg        array_agg(a.attname ORDER BY u.ord) AS stats_columns
--dg FROM pg_statistic_ext s
--dg CROSS JOIN LATERAL
--dg   unnest(s.stxkeys::int2[]) WITH ORDINALITY AS u(attnum, ord)
--dg JOIN pg_attribute a
--dg   ON a.attrelid = s.stxrelid AND a.attnum = u.attnum
--dg WHERE s.stxrelid IN ('ctl_stats3_child'::regclass,
--dg                      'ctl_stats4_child'::regclass)
--dg GROUP BY s.stxrelid
--dg ORDER BY s.stxrelid::regclass::text;
DROP TABLE ctl_stats3_parent;
DROP TABLE ctl_stats3_child;
DROP TABLE ctl_stats4_parent;
DROP TABLE ctl_stats4_child;

--dg-cascade
--dg DROP TABLE ctl_table;
--dg-cascade
--dg DROP FOREIGN TABLE ctl_foreign_table1;
--dg-cascade
--dg DROP FOREIGN TABLE ctl_foreign_table2;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER ctl_dummy CASCADE;
