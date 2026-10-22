--
-- Tests for functions providing information about partitions
--
SELECT * FROM pg_partition_tree(NULL);
SELECT * FROM pg_partition_tree(0);
SELECT * FROM pg_partition_ancestors(NULL);
SELECT * FROM pg_partition_ancestors(0);
SELECT pg_partition_root(NULL);
SELECT pg_partition_root(0);

-- Test table partition trees
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE ptif_test (a int, b int) PARTITION BY range (a);
--dg-cascade
--dg CREATE TABLE ptif_test0 PARTITION OF ptif_test
--dg   FOR VALUES FROM (minvalue) TO (0) PARTITION BY list (b);
--dg-cascade
--dg CREATE TABLE ptif_test01 PARTITION OF ptif_test0 FOR VALUES IN (1);
--dg-cascade
--dg CREATE TABLE ptif_test1 PARTITION OF ptif_test
--dg   FOR VALUES FROM (0) TO (100) PARTITION BY list (b);
--dg-cascade
--dg CREATE TABLE ptif_test11 PARTITION OF ptif_test1 FOR VALUES IN (1);
--dg-cascade
--dg CREATE TABLE ptif_test2 PARTITION OF ptif_test
--dg   FOR VALUES FROM (100) TO (200);
-- This partitioned table should remain with no partitions.
--dg-cascade
--dg CREATE TABLE ptif_test3 PARTITION OF ptif_test
--dg   FOR VALUES FROM (200) TO (maxvalue) PARTITION BY list (b);

-- Test pg_partition_root for tables
--dg-cascade
--dg SELECT pg_partition_root('ptif_test');
--dg-cascade
--dg SELECT pg_partition_root('ptif_test0');
--dg-cascade
--dg SELECT pg_partition_root('ptif_test01');
--dg-cascade
--dg SELECT pg_partition_root('ptif_test3');

-- Test index partition tree
--dg-cascade
--dg CREATE INDEX ptif_test_index ON ONLY ptif_test (a);
--dg-cascade
--dg CREATE INDEX ptif_test0_index ON ONLY ptif_test0 (a);
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX ptif_test_index ATTACH PARTITION ptif_test0_index;
--dg-cascade
--dg CREATE INDEX ptif_test01_index ON ptif_test01 (a);
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX ptif_test0_index ATTACH PARTITION ptif_test01_index;
--dg-cascade
--dg CREATE INDEX ptif_test1_index ON ONLY ptif_test1 (a);
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX ptif_test_index ATTACH PARTITION ptif_test1_index;
--dg-cascade
--dg CREATE INDEX ptif_test11_index ON ptif_test11 (a);
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX ptif_test1_index ATTACH PARTITION ptif_test11_index;
--dg-cascade
--dg CREATE INDEX ptif_test2_index ON ptif_test2 (a);
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX ptif_test_index ATTACH PARTITION ptif_test2_index;
--dg-cascade
--dg CREATE INDEX ptif_test3_index ON ptif_test3 (a);
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX ptif_test_index ATTACH PARTITION ptif_test3_index;

-- Test pg_partition_root for indexes
--dg-cascade
--dg SELECT pg_partition_root('ptif_test_index');
--dg-cascade
--dg SELECT pg_partition_root('ptif_test0_index');
--dg-cascade
--dg SELECT pg_partition_root('ptif_test01_index');
--dg-cascade
--dg SELECT pg_partition_root('ptif_test3_index');

-- List all tables members of the tree
--dg-cascade
--dg SELECT relid, parentrelid, level, isleaf
--dg   FROM pg_partition_tree('ptif_test');
-- List tables from an intermediate level
--dg-cascade
--dg SELECT relid, parentrelid, level, isleaf
--dg   FROM pg_partition_tree('ptif_test0') p
--dg   JOIN pg_class c ON (p.relid = c.oid);
-- List from leaf table
--dg-cascade
--dg SELECT relid, parentrelid, level, isleaf
--dg   FROM pg_partition_tree('ptif_test01') p
--dg   JOIN pg_class c ON (p.relid = c.oid);
-- List from partitioned table with no partitions
--dg-cascade
--dg SELECT relid, parentrelid, level, isleaf
--dg   FROM pg_partition_tree('ptif_test3') p
--dg   JOIN pg_class c ON (p.relid = c.oid);
-- List all ancestors of root and leaf tables
--dg-cascade
--dg SELECT * FROM pg_partition_ancestors('ptif_test01');
--dg-cascade
--dg SELECT * FROM pg_partition_ancestors('ptif_test');
-- List all members using pg_partition_root with leaf table reference
--dg-cascade
--dg SELECT relid, parentrelid, level, isleaf
--dg   FROM pg_partition_tree(pg_partition_root('ptif_test01')) p
--dg   JOIN pg_class c ON (p.relid = c.oid);

-- List all indexes members of the tree
--dg-cascade
--dg SELECT relid, parentrelid, level, isleaf
--dg   FROM pg_partition_tree('ptif_test_index');
-- List indexes from an intermediate level
--dg-cascade
--dg SELECT relid, parentrelid, level, isleaf
--dg   FROM pg_partition_tree('ptif_test0_index') p
--dg   JOIN pg_class c ON (p.relid = c.oid);
-- List from leaf index
--dg-cascade
--dg SELECT relid, parentrelid, level, isleaf
--dg   FROM pg_partition_tree('ptif_test01_index') p
--dg   JOIN pg_class c ON (p.relid = c.oid);
-- List from partitioned index with no partitions
--dg-cascade
--dg SELECT relid, parentrelid, level, isleaf
--dg   FROM pg_partition_tree('ptif_test3_index') p
--dg   JOIN pg_class c ON (p.relid = c.oid);
-- List all members using pg_partition_root with leaf index reference
--dg-cascade
--dg SELECT relid, parentrelid, level, isleaf
--dg   FROM pg_partition_tree(pg_partition_root('ptif_test01_index')) p
--dg   JOIN pg_class c ON (p.relid = c.oid);
-- List all ancestors of root and leaf indexes
--dg-cascade
--dg SELECT * FROM pg_partition_ancestors('ptif_test01_index');
--dg-cascade
--dg SELECT * FROM pg_partition_ancestors('ptif_test_index');

--dg-cascade
--dg DROP TABLE ptif_test;

-- Table that is not part of any partition tree is not listed.
CREATE TABLE ptif_normal_table(a int);
SELECT relid, parentrelid, level, isleaf
  FROM pg_partition_tree('ptif_normal_table');
SELECT * FROM pg_partition_ancestors('ptif_normal_table');
SELECT pg_partition_root('ptif_normal_table');
DROP TABLE ptif_normal_table;

-- Various partitioning-related functions return empty/NULL if passed relations
-- of types that cannot be part of a partition tree; for example, views,
-- materialized views, legacy inheritance children or parents, etc.
CREATE VIEW ptif_test_view AS SELECT 1;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW ptif_test_matview AS SELECT 1;
CREATE TABLE ptif_li_parent ();
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE ptif_li_child () INHERITS (ptif_li_parent);
SELECT * FROM pg_partition_tree('ptif_test_view');
--dg-cascade
--dg SELECT * FROM pg_partition_tree('ptif_test_matview');
SELECT * FROM pg_partition_tree('ptif_li_parent');
--dg-cascade
--dg SELECT * FROM pg_partition_tree('ptif_li_child');
SELECT * FROM pg_partition_ancestors('ptif_test_view');
--dg-cascade
--dg SELECT * FROM pg_partition_ancestors('ptif_test_matview');
SELECT * FROM pg_partition_ancestors('ptif_li_parent');
--dg-cascade
--dg SELECT * FROM pg_partition_ancestors('ptif_li_child');
SELECT pg_partition_root('ptif_test_view');
--dg-cascade
--dg SELECT pg_partition_root('ptif_test_matview');
SELECT pg_partition_root('ptif_li_parent');
--dg-cascade
--dg SELECT pg_partition_root('ptif_li_child');
DROP VIEW ptif_test_view;
--dg-cascade
--dg DROP MATERIALIZED VIEW ptif_test_matview;
--dg-cascade
--dg DROP TABLE ptif_li_parent, ptif_li_child;
