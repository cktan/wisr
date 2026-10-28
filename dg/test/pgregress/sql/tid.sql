-- basic tests for the TID data type

SELECT
  '(0,0)'::tid as tid00,
  '(0,1)'::tid as tid01,
  '(-1,0)'::tid as tidm10,
  '(4294967295,65535)'::tid as tidmax;

SELECT '(4294967296,1)'::tid;  -- error
SELECT '(1,65536)'::tid;  -- error

-- Also try it with non-error-throwing API
SELECT pg_input_is_valid('(0)', 'tid');
SELECT * FROM pg_input_error_info('(0)', 'tid');
SELECT pg_input_is_valid('(0,-1)', 'tid');
SELECT * FROM pg_input_error_info('(0,-1)', 'tid');


-- tests for functions related to TID handling

CREATE TABLE tid_tab (a int);

-- min() and max() for TIDs
INSERT INTO tid_tab VALUES (1), (2);
--dg-skip: column "?" does not exist
--dg SELECT min(ctid) FROM tid_tab;
--dg-skip: column "?" does not exist
--dg SELECT max(ctid) FROM tid_tab;
TRUNCATE tid_tab;

-- Tests for currtid2() with various relation kinds

-- Materialized view
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW tid_matview AS SELECT a FROM tid_tab;
--dg-cascade
--dg SELECT currtid2('tid_matview'::text, '(0,1)'::tid); -- fails
INSERT INTO tid_tab VALUES (1);
--dg-cascade
--dg REFRESH MATERIALIZED VIEW tid_matview;
--dg-cascade
--dg SELECT currtid2('tid_matview'::text, '(0,1)'::tid); -- ok
--dg-cascade
--dg DROP MATERIALIZED VIEW tid_matview;
TRUNCATE tid_tab;

-- Sequence
CREATE SEQUENCE tid_seq;
--dg-skip: function ?(...) does not exist
--dg SELECT currtid2('tid_seq'::text, '(0,1)'::tid); -- ok
DROP SEQUENCE tid_seq;

-- Index, fails with incorrect relation type
CREATE INDEX tid_ind ON tid_tab(a);
--dg-differs: another error: function ?(...) does not exist
--dg SELECT currtid2('tid_ind'::text, '(0,1)'::tid); -- fails
DROP INDEX tid_ind;

-- Partitioned table, no storage
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE tid_part (a int) PARTITION BY RANGE (a);
--dg-cascade
--dg SELECT currtid2('tid_part'::text, '(0,1)'::tid); -- fails
--dg-cascade
--dg DROP TABLE tid_part;

-- Views
-- ctid not defined in the view
CREATE VIEW tid_view_no_ctid AS SELECT a FROM tid_tab;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT currtid2('tid_view_no_ctid'::text, '(0,1)'::tid); -- fails
DROP VIEW tid_view_no_ctid;
-- ctid fetched directly from the source table.
--dg-skip: column "?" does not exist
--dg CREATE VIEW tid_view_with_ctid AS SELECT ctid, a FROM tid_tab;
--dg-cascade
--dg SELECT currtid2('tid_view_with_ctid'::text, '(0,1)'::tid); -- fails
INSERT INTO tid_tab VALUES (1);
--dg-cascade
--dg SELECT currtid2('tid_view_with_ctid'::text, '(0,1)'::tid); -- ok
--dg-cascade
--dg DROP VIEW tid_view_with_ctid;
TRUNCATE tid_tab;
-- ctid attribute with incorrect data type
CREATE VIEW tid_view_fake_ctid AS SELECT 1 AS ctid, 2 AS a;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT currtid2('tid_view_fake_ctid'::text, '(0,1)'::tid); -- fails
DROP VIEW tid_view_fake_ctid;

DROP TABLE tid_tab CASCADE;
