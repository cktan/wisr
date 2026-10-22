
-- Simple create
--dg-skip: storage parameter "?" is not supported
--dg CREATE TABLE reloptions_test(i INT) WITH (FiLLFaCToR=30,
--dg 	autovacuum_enabled = false, autovacuum_analyze_scale_factor = 0.2);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test'::regclass;

-- Fail min/max values check
CREATE TABLE reloptions_test2(i INT) WITH (fillfactor=2);
CREATE TABLE reloptions_test2(i INT) WITH (fillfactor=110);
--dg-differs: another error: storage parameter "?" is not supported
--dg CREATE TABLE reloptions_test2(i INT) WITH (autovacuum_analyze_scale_factor = -10.0);
--dg-differs: another error: storage parameter "?" is not supported
--dg CREATE TABLE reloptions_test2(i INT) WITH (autovacuum_analyze_scale_factor = 110.0);

-- Fail when option and namespace do not exist
--dg-differs: another error: storage parameter "?" is not supported
--dg CREATE TABLE reloptions_test2(i INT) WITH (not_existing_option=2);
--dg-differs: another error: storage parameter "?" is not supported
--dg CREATE TABLE reloptions_test2(i INT) WITH (not_existing_namespace.fillfactor=2);

-- Fail while setting improper values
CREATE TABLE reloptions_test2(i INT) WITH (fillfactor=-30.1);
CREATE TABLE reloptions_test2(i INT) WITH (fillfactor='string');
CREATE TABLE reloptions_test2(i INT) WITH (fillfactor=true);
CREATE TABLE reloptions_test2(i INT) WITH (autovacuum_enabled=12);
CREATE TABLE reloptions_test2(i INT) WITH (autovacuum_enabled=30.5);
CREATE TABLE reloptions_test2(i INT) WITH (autovacuum_enabled='string');
--dg-differs: another error: storage parameter "?" is not supported
--dg CREATE TABLE reloptions_test2(i INT) WITH (autovacuum_analyze_scale_factor='string');
--dg-differs: another error: storage parameter "?" is not supported
--dg CREATE TABLE reloptions_test2(i INT) WITH (autovacuum_analyze_scale_factor=true);

-- Fail if option is specified twice
CREATE TABLE reloptions_test2(i INT) WITH (fillfactor=30, fillfactor=40);

-- Specifying name only for a non-Boolean option should fail
CREATE TABLE reloptions_test2(i INT) WITH (fillfactor);

-- Simple ALTER TABLE
--dg-cascade
--dg ALTER TABLE reloptions_test SET (fillfactor=31,
--dg 	autovacuum_analyze_scale_factor = 0.3);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test'::regclass;

-- Set boolean option to true without specifying value
--dg-cascade
--dg ALTER TABLE reloptions_test SET (autovacuum_enabled, fillfactor=32);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test'::regclass;

-- Check that RESET works well
--dg-cascade
--dg ALTER TABLE reloptions_test RESET (fillfactor);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test'::regclass;

-- Resetting all values causes the column to become null
--dg-cascade
--dg ALTER TABLE reloptions_test RESET (autovacuum_enabled,
--dg 	autovacuum_analyze_scale_factor);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test'::regclass AND
--dg        reloptions IS NULL;

-- RESET fails if a value is specified
--dg-cascade
--dg ALTER TABLE reloptions_test RESET (fillfactor=12);

-- We can RESET an invalid option which for some reason is already set
--dg-cascade
--dg UPDATE pg_class
--dg 	SET reloptions = '{fillfactor=13,autovacuum_enabled=false,illegal_option=4}'
--dg 	WHERE oid = 'reloptions_test'::regclass;
--dg-cascade
--dg ALTER TABLE reloptions_test RESET (illegal_option);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test'::regclass;

-- Test vacuum_truncate option
--dg-cascade
--dg DROP TABLE reloptions_test;

--dg-skip: storage parameter "?" is not supported
--dg CREATE TEMP TABLE reloptions_test(i INT NOT NULL, j text)
--dg 	WITH (vacuum_truncate=false,
--dg 	toast.vacuum_truncate=false,
--dg 	autovacuum_enabled=false);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test'::regclass;
--dg-cascade
--dg INSERT INTO reloptions_test VALUES (1, NULL), (NULL, NULL);
-- Do an aggressive vacuum to prevent page-skipping.
--dg-cascade
--dg VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) reloptions_test;
--dg-cascade
--dg SELECT pg_relation_size('reloptions_test') > 0;

--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid =
--dg 	(SELECT reltoastrelid FROM pg_class
--dg 	WHERE oid = 'reloptions_test'::regclass);

--dg-cascade
--dg ALTER TABLE reloptions_test RESET (vacuum_truncate);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test'::regclass;
--dg-cascade
--dg INSERT INTO reloptions_test VALUES (1, NULL), (NULL, NULL);
-- Do an aggressive vacuum to prevent page-skipping.
--dg-cascade
--dg VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) reloptions_test;
--dg-cascade
--dg SELECT pg_relation_size('reloptions_test') = 0;

-- Test toast.* options
--dg-cascade
--dg DROP TABLE reloptions_test;

--dg-skip: storage parameter "?" is not supported
--dg CREATE TABLE reloptions_test (s VARCHAR)
--dg 	WITH (toast.autovacuum_vacuum_cost_delay = 23);
--dg-cascade
--dg SELECT reltoastrelid as toast_oid
--dg 	FROM pg_class WHERE oid = 'reloptions_test'::regclass \gset
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = :toast_oid;

--dg-cascade
--dg ALTER TABLE reloptions_test SET (toast.autovacuum_vacuum_cost_delay = 24);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = :toast_oid;

--dg-cascade
--dg ALTER TABLE reloptions_test RESET (toast.autovacuum_vacuum_cost_delay);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = :toast_oid;

-- Fail on non-existent options in toast namespace
--dg-differs: another error: storage parameter "?" is not supported
--dg CREATE TABLE reloptions_test2 (i int) WITH (toast.not_existing_option = 42);

-- Mix TOAST & heap
--dg-cascade
--dg DROP TABLE reloptions_test;

--dg-skip: storage parameter "?" is not supported
--dg CREATE TABLE reloptions_test (s VARCHAR) WITH
--dg 	(toast.autovacuum_vacuum_cost_delay = 23,
--dg 	autovacuum_vacuum_cost_delay = 24, fillfactor = 40);

--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test'::regclass;
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = (
--dg 	SELECT reltoastrelid FROM pg_class WHERE oid = 'reloptions_test'::regclass);

--
-- CREATE INDEX, ALTER INDEX for btrees
--

--dg-cascade
--dg CREATE INDEX reloptions_test_idx ON reloptions_test (s) WITH (fillfactor=30);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test_idx'::regclass;

-- Fail when option and namespace do not exist
--dg-cascade
--dg CREATE INDEX reloptions_test_idx ON reloptions_test (s)
--dg 	WITH (not_existing_option=2);
--dg-cascade
--dg CREATE INDEX reloptions_test_idx ON reloptions_test (s)
--dg 	WITH (not_existing_ns.fillfactor=2);

-- Check allowed ranges
--dg-cascade
--dg CREATE INDEX reloptions_test_idx2 ON reloptions_test (s) WITH (fillfactor=1);
--dg-cascade
--dg CREATE INDEX reloptions_test_idx2 ON reloptions_test (s) WITH (fillfactor=130);

-- Check ALTER
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX reloptions_test_idx SET (fillfactor=40);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test_idx'::regclass;

-- Check ALTER on empty reloption list
--dg-cascade
--dg CREATE INDEX reloptions_test_idx3 ON reloptions_test (s);
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX reloptions_test_idx3 SET (fillfactor=40);
--dg-cascade
--dg SELECT reloptions FROM pg_class WHERE oid = 'reloptions_test_idx3'::regclass;
