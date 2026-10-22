/*
 * 1.test CREATE INDEX
 *
 * Deliberately avoid dropping objects in this section, to get some pg_dump
 * coverage.
 */

-- Regular index with included columns
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl_include_reg (c1 int, c2 int, c3 int, c4 box);
--dg-cascade
--dg INSERT INTO tbl_include_reg SELECT x, 2*x, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg CREATE INDEX tbl_include_reg_idx ON tbl_include_reg (c1, c2) INCLUDE (c3, c4);
-- duplicate column is pretty pointless, but we allow it anyway
--dg-cascade
--dg CREATE INDEX ON tbl_include_reg (c1, c2) INCLUDE (c1, c3);
--dg-cascade
--dg SELECT pg_get_indexdef(i.indexrelid)
--dg FROM pg_index i JOIN pg_class c ON i.indexrelid = c.oid
--dg WHERE i.indrelid = 'tbl_include_reg'::regclass ORDER BY c.relname;
--dg-cascade
--dg \d tbl_include_reg_idx

-- Unique index and unique constraint
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl_include_unique1 (c1 int, c2 int, c3 int, c4 box);
--dg-cascade
--dg INSERT INTO tbl_include_unique1 SELECT x, 2*x, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg CREATE UNIQUE INDEX tbl_include_unique1_idx_unique ON tbl_include_unique1 using btree (c1, c2) INCLUDE (c3, c4);
--dg-cascade
--dg ALTER TABLE tbl_include_unique1 add UNIQUE USING INDEX tbl_include_unique1_idx_unique;
--dg-cascade
--dg ALTER TABLE tbl_include_unique1 add UNIQUE (c1, c2) INCLUDE (c3, c4);
--dg-cascade
--dg SELECT pg_get_indexdef(i.indexrelid)
--dg FROM pg_index i JOIN pg_class c ON i.indexrelid = c.oid
--dg WHERE i.indrelid = 'tbl_include_unique1'::regclass ORDER BY c.relname;

-- Unique index and unique constraint. Both must fail.
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl_include_unique2 (c1 int, c2 int, c3 int, c4 box);
--dg-cascade
--dg INSERT INTO tbl_include_unique2 SELECT 1, 2, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg CREATE UNIQUE INDEX tbl_include_unique2_idx_unique ON tbl_include_unique2 using btree (c1, c2) INCLUDE (c3, c4);
--dg-cascade
--dg ALTER TABLE tbl_include_unique2 add UNIQUE (c1, c2) INCLUDE (c3, c4);

-- PK constraint
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl_include_pk (c1 int, c2 int, c3 int, c4 box);
--dg-cascade
--dg INSERT INTO tbl_include_pk SELECT 1, 2*x, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg ALTER TABLE tbl_include_pk add PRIMARY KEY (c1, c2) INCLUDE (c3, c4);
--dg-cascade
--dg SELECT pg_get_indexdef(i.indexrelid)
--dg FROM pg_index i JOIN pg_class c ON i.indexrelid = c.oid
--dg WHERE i.indrelid = 'tbl_include_pk'::regclass ORDER BY c.relname;

--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl_include_box (c1 int, c2 int, c3 int, c4 box);
--dg-cascade
--dg INSERT INTO tbl_include_box SELECT 1, 2*x, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg CREATE UNIQUE INDEX tbl_include_box_idx_unique ON tbl_include_box using btree (c1, c2) INCLUDE (c3, c4);
--dg-cascade
--dg ALTER TABLE tbl_include_box add PRIMARY KEY USING INDEX tbl_include_box_idx_unique;
--dg-cascade
--dg SELECT pg_get_indexdef(i.indexrelid)
--dg FROM pg_index i JOIN pg_class c ON i.indexrelid = c.oid
--dg WHERE i.indrelid = 'tbl_include_box'::regclass ORDER BY c.relname;

-- PK constraint. Must fail.
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl_include_box_pk (c1 int, c2 int, c3 int, c4 box);
--dg-cascade
--dg INSERT INTO tbl_include_box_pk SELECT 1, 2, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg ALTER TABLE tbl_include_box_pk add PRIMARY KEY (c1, c2) INCLUDE (c3, c4);


/*
 * 2. Test CREATE TABLE with constraint
 */
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 box,
--dg 				CONSTRAINT covering UNIQUE(c1,c2) INCLUDE(c3,c4));
--dg-cascade
--dg SELECT indexrelid::regclass, indnatts, indnkeyatts, indisunique, indisprimary, indkey, indclass FROM pg_index WHERE indrelid = 'tbl'::regclass::oid;
--dg-cascade
--dg SELECT pg_get_constraintdef(oid), conname, conkey FROM pg_constraint WHERE conrelid = 'tbl'::regclass::oid;
-- ensure that constraint works
--dg-cascade
--dg INSERT INTO tbl SELECT 1, 2, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg DROP TABLE tbl;

--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 box,
--dg 				CONSTRAINT covering PRIMARY KEY(c1,c2) INCLUDE(c3,c4));
--dg-cascade
--dg SELECT indexrelid::regclass, indnatts, indnkeyatts, indisunique, indisprimary, indkey, indclass FROM pg_index WHERE indrelid = 'tbl'::regclass::oid;
--dg-cascade
--dg SELECT pg_get_constraintdef(oid), conname, conkey FROM pg_constraint WHERE conrelid = 'tbl'::regclass::oid AND contype = 'p';
-- ensure that constraint works
--dg-cascade
--dg INSERT INTO tbl SELECT 1, 2, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg INSERT INTO tbl SELECT 1, NULL, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg INSERT INTO tbl SELECT x, 2*x, NULL, NULL FROM generate_series(1,300) AS x;
--dg-cascade
--dg explain (costs off)
--dg select * from tbl where (c1,c2,c3) < (2,5,1);
--dg-cascade
--dg select * from tbl where (c1,c2,c3) < (2,5,1);
-- row comparison that compares high key at page boundary
SET enable_seqscan = off;
--dg-cascade
--dg explain (costs off)
--dg select * from tbl where (c1,c2,c3) < (262,1,1) limit 1;
--dg-cascade
--dg select * from tbl where (c1,c2,c3) < (262,1,1) limit 1;
--dg-cascade
--dg DROP TABLE tbl;
RESET enable_seqscan;

--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 box,
--dg 				UNIQUE(c1,c2) INCLUDE(c3,c4));
--dg-cascade
--dg SELECT indexrelid::regclass, indnatts, indnkeyatts, indisunique, indisprimary, indkey, indclass FROM pg_index WHERE indrelid = 'tbl'::regclass::oid;
--dg-cascade
--dg SELECT pg_get_constraintdef(oid), conname, conkey FROM pg_constraint WHERE conrelid = 'tbl'::regclass::oid;
-- ensure that constraint works
--dg-cascade
--dg INSERT INTO tbl SELECT 1, 2, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg DROP TABLE tbl;

--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 box,
--dg 				PRIMARY KEY(c1,c2) INCLUDE(c3,c4));
--dg-cascade
--dg SELECT indexrelid::regclass, indnatts, indnkeyatts, indisunique, indisprimary, indkey, indclass FROM pg_index WHERE indrelid = 'tbl'::regclass::oid;
--dg-cascade
--dg SELECT pg_get_constraintdef(oid), conname, conkey FROM pg_constraint WHERE conrelid = 'tbl'::regclass::oid AND contype = 'p';
-- ensure that constraint works
--dg-cascade
--dg INSERT INTO tbl SELECT 1, 2, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg INSERT INTO tbl SELECT 1, NULL, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg INSERT INTO tbl SELECT x, 2*x, NULL, NULL FROM generate_series(1,10) AS x;
--dg-cascade
--dg DROP TABLE tbl;

--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 box,
--dg 				EXCLUDE USING btree (c1 WITH =) INCLUDE(c3,c4));
--dg-cascade
--dg SELECT indexrelid::regclass, indnatts, indnkeyatts, indisunique, indisprimary, indkey, indclass FROM pg_index WHERE indrelid = 'tbl'::regclass::oid;
--dg-cascade
--dg SELECT pg_get_constraintdef(oid), conname, conkey FROM pg_constraint WHERE conrelid = 'tbl'::regclass::oid;
-- ensure that constraint works
--dg-cascade
--dg INSERT INTO tbl SELECT 1, 2, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg INSERT INTO tbl SELECT x, 2*x, NULL, NULL FROM generate_series(1,10) AS x;
--dg-cascade
--dg DROP TABLE tbl;

/*
 * 3.0 Test ALTER TABLE DROP COLUMN.
 * Any column deletion leads to index deletion.
 */
CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 int);
CREATE UNIQUE INDEX tbl_idx ON tbl using btree(c1, c2, c3, c4);
--dg-skip: relation "?" does not exist
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-skip: cannot drop column c3 of table tbl because other objects depend on it
--dg ALTER TABLE tbl DROP COLUMN c3;
--dg-cascade
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
DROP TABLE tbl;

/*
 * 3.1 Test ALTER TABLE DROP COLUMN.
 * Included column deletion leads to the index deletion,
 * AS well AS key columns deletion. It's explained in documentation.
 */
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 box);
--dg-cascade
--dg CREATE UNIQUE INDEX tbl_idx ON tbl using btree(c1, c2) INCLUDE(c3,c4);
--dg-cascade
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-cascade
--dg ALTER TABLE tbl DROP COLUMN c3;
--dg-skip: relation "?" does not exist
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-cascade
--dg DROP TABLE tbl;

/*
 * 3.2 Test ALTER TABLE DROP COLUMN.
 * Included column deletion leads to the index deletion.
 * AS well AS key columns deletion. It's explained in documentation.
 */
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 box, UNIQUE(c1, c2) INCLUDE(c3,c4));
--dg-cascade
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-cascade
--dg ALTER TABLE tbl DROP COLUMN c3;
--dg-skip: relation "?" does not exist
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-cascade
--dg ALTER TABLE tbl DROP COLUMN c1;
--dg-skip: relation "?" does not exist
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-cascade
--dg DROP TABLE tbl;

/*
 * 3.3 Test ALTER TABLE SET STATISTICS
 */
CREATE TABLE tbl (c1 int, c2 int);
--dg-skip: an index key is a column, not an expression, yet
--dg CREATE INDEX tbl_idx ON tbl (c1, (c1+0)) INCLUDE (c2);
--dg-cascade
--dg ALTER INDEX tbl_idx ALTER COLUMN 1 SET STATISTICS 1000;
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX tbl_idx ALTER COLUMN 2 SET STATISTICS 1000;
--dg-cascade
--dg ALTER INDEX tbl_idx ALTER COLUMN 3 SET STATISTICS 1000;
--dg-cascade
--dg ALTER INDEX tbl_idx ALTER COLUMN 4 SET STATISTICS 1000;
DROP TABLE tbl;

/*
 * 4. CREATE INDEX CONCURRENTLY
 */
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 box, UNIQUE(c1, c2) INCLUDE(c3,c4));
--dg-cascade
--dg INSERT INTO tbl SELECT x, 2*x, 3*x, box('4,4,4,4') FROM generate_series(1,1000) AS x;
--dg-cascade
--dg CREATE UNIQUE INDEX CONCURRENTLY on tbl (c1, c2) INCLUDE (c3, c4);
--dg-cascade
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-cascade
--dg DROP TABLE tbl;


/*
 * 5. REINDEX
 */
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 box, UNIQUE(c1, c2) INCLUDE(c3,c4));
--dg-cascade
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-cascade
--dg ALTER TABLE tbl DROP COLUMN c3;
--dg-skip: relation "?" does not exist
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-differs: no error where PostgreSQL has: relation "?" does not exist
--dg REINDEX INDEX tbl_c1_c2_c3_c4_key;
--dg-skip: relation "?" does not exist
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-cascade
--dg ALTER TABLE tbl DROP COLUMN c1;
--dg-skip: relation "?" does not exist
--dg SELECT indexdef FROM pg_indexes WHERE tablename = 'tbl' ORDER BY indexname;
--dg-cascade
--dg DROP TABLE tbl;

/*
 * 7. Check various AMs. All but btree, gist and spgist must fail.
 */
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 box, c4 box);
--dg-cascade
--dg CREATE INDEX on tbl USING brin(c1, c2) INCLUDE (c3, c4);
--dg-cascade
--dg CREATE INDEX on tbl USING gist(c3) INCLUDE (c1, c4);
--dg-cascade
--dg CREATE INDEX on tbl USING spgist(c3) INCLUDE (c4);
--dg-cascade
--dg CREATE INDEX on tbl USING gin(c1, c2) INCLUDE (c3, c4);
--dg-cascade
--dg CREATE INDEX on tbl USING hash(c1, c2) INCLUDE (c3, c4);
--dg-cascade
--dg CREATE INDEX on tbl USING rtree(c3) INCLUDE (c1, c4);
--dg-cascade
--dg CREATE INDEX on tbl USING btree(c1, c2) INCLUDE (c3, c4);
--dg-cascade
--dg DROP TABLE tbl;

/*
 * 8. Update, delete values in indexed table.
 */
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int, c2 int, c3 int, c4 box);
--dg-cascade
--dg INSERT INTO tbl SELECT x, 2*x, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg CREATE UNIQUE INDEX tbl_idx_unique ON tbl using btree(c1, c2) INCLUDE (c3,c4);
--dg-cascade
--dg UPDATE tbl SET c1 = 100 WHERE c1 = 2;
--dg-cascade
--dg UPDATE tbl SET c1 = 1 WHERE c1 = 3;
-- should fail
--dg-cascade
--dg UPDATE tbl SET c2 = 2 WHERE c1 = 1;
--dg-cascade
--dg UPDATE tbl SET c3 = 1;
--dg-cascade
--dg DELETE FROM tbl WHERE c1 = 5 OR c3 = 12;
--dg-cascade
--dg DROP TABLE tbl;

/*
 * 9. Alter column type.
 */
--dg-skip: type ? is not supported yet
--dg CREATE TABLE tbl (c1 int,c2 int, c3 int, c4 box, UNIQUE(c1, c2) INCLUDE(c3,c4));
--dg-cascade
--dg INSERT INTO tbl SELECT x, 2*x, 3*x, box('4,4,4,4') FROM generate_series(1,10) AS x;
--dg-cascade
--dg ALTER TABLE tbl ALTER c1 TYPE bigint;
--dg-cascade
--dg ALTER TABLE tbl ALTER c3 TYPE bigint;
--dg-cascade
--dg \d tbl
--dg-cascade
--dg DROP TABLE tbl;

/*
 * 10. Test coverage for names stored as cstrings in indexes
 */
CREATE TABLE nametbl (c1 int, c2 name, c3 float);
CREATE INDEX nametbl_c1_c2_idx ON nametbl (c2, c1) INCLUDE (c3);
INSERT INTO nametbl VALUES(1, 'two', 3.0);
VACUUM nametbl;
SET enable_seqscan = 0;

-- Ensure we get an index only scan plan
EXPLAIN (COSTS OFF) SELECT c2, c1, c3 FROM nametbl WHERE c2 = 'two' AND c1 = 1;

-- Validate the results look sane
SELECT c2, c1, c3 FROM nametbl WHERE c2 = 'two' AND c1 = 1;

RESET enable_seqscan;

DROP TABLE nametbl;
