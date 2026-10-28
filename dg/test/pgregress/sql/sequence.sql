--
-- CREATE SEQUENCE
--

-- various error cases
--dg-differs: another error: a sequence counts up from N or more, by N or more, yet
--dg CREATE SEQUENCE sequence_testx INCREMENT BY 0;
--dg-differs: another error: CREATE SEQUENCE supports only START and INCREMENT yet, not minvalue
--dg CREATE SEQUENCE sequence_testx INCREMENT BY -1 MINVALUE 20;
--dg-differs: another error: CREATE SEQUENCE supports only START and INCREMENT yet, not maxvalue
--dg CREATE SEQUENCE sequence_testx INCREMENT BY 1 MAXVALUE -20;
--dg-differs: another error: a sequence counts up from N or more, by N or more, yet
--dg CREATE SEQUENCE sequence_testx INCREMENT BY -1 START 10;
--dg-differs: another error: a sequence counts up from N or more, by N or more, yet
--dg CREATE SEQUENCE sequence_testx INCREMENT BY 1 START -10;
--dg-differs: another error: CREATE SEQUENCE supports only START and INCREMENT yet, not cache
--dg CREATE SEQUENCE sequence_testx CACHE 0;

-- OWNED BY errors
--dg-differs: another error: CREATE SEQUENCE supports only START and INCREMENT yet, not owned_by
--dg CREATE SEQUENCE sequence_testx OWNED BY nobody;  -- nonsense word
--dg-differs: another error: CREATE SEQUENCE supports only START and INCREMENT yet, not owned_by
--dg CREATE SEQUENCE sequence_testx OWNED BY pg_class_oid_index.oid;  -- not a table
--dg-differs: another error: CREATE SEQUENCE supports only START and INCREMENT yet, not owned_by
--dg CREATE SEQUENCE sequence_testx OWNED BY pg_class.relname;  -- not same schema
CREATE TABLE sequence_test_table (a int);
--dg-differs: another error: CREATE SEQUENCE supports only START and INCREMENT yet, not owned_by
--dg CREATE SEQUENCE sequence_testx OWNED BY sequence_test_table.b;  -- wrong column
DROP TABLE sequence_test_table;

-- sequence data types
CREATE SEQUENCE sequence_test5 AS integer;
CREATE SEQUENCE sequence_test6 AS smallint;
CREATE SEQUENCE sequence_test7 AS bigint;
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not maxvalue
--dg CREATE SEQUENCE sequence_test8 AS integer MAXVALUE 100000;
--dg-skip: a sequence counts up from N or more, by N or more, yet
--dg CREATE SEQUENCE sequence_test9 AS integer INCREMENT BY -1;
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not minvalue
--dg CREATE SEQUENCE sequence_test10 AS integer MINVALUE -100000 START 1;
CREATE SEQUENCE sequence_test11 AS smallint;
--dg-skip: a sequence counts up from N or more, by N or more, yet
--dg CREATE SEQUENCE sequence_test12 AS smallint INCREMENT -1;
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not minvalue
--dg CREATE SEQUENCE sequence_test13 AS smallint MINVALUE -32768;
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not maxvalue
--dg CREATE SEQUENCE sequence_test14 AS smallint MAXVALUE 32767 INCREMENT -1;
CREATE SEQUENCE sequence_testx AS text;
--dg-differs: another error: type "?" does not exist
--dg CREATE SEQUENCE sequence_testx AS nosuchtype;

--dg-differs: another error: CREATE SEQUENCE supports only START and INCREMENT yet, not maxvalue
--dg CREATE SEQUENCE sequence_testx AS smallint MAXVALUE 100000;
--dg-differs: another error: CREATE SEQUENCE supports only START and INCREMENT yet, not minvalue
--dg CREATE SEQUENCE sequence_testx AS smallint MINVALUE -100000;

--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test5 AS smallint;  -- success, max will be adjusted
--dg-cascade
--dg ALTER SEQUENCE sequence_test8 AS smallint;  -- fail, max has to be adjusted
--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test8 AS smallint MAXVALUE 20000;  -- ok now
--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test9 AS smallint;  -- success, min will be adjusted
--dg-cascade
--dg ALTER SEQUENCE sequence_test10 AS smallint;  -- fail, min has to be adjusted
--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test10 AS smallint MINVALUE -20000;  -- ok now

--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test11 AS int;  -- max will be adjusted
--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test12 AS int;  -- min will be adjusted
--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test13 AS int;  -- min and max will be adjusted
--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test14 AS int;  -- min and max will be adjusted

---
--- test creation of SERIAL column
---

CREATE TABLE serialTest1 (f1 text, f2 serial);

INSERT INTO serialTest1 VALUES ('foo');
INSERT INTO serialTest1 VALUES ('bar');
INSERT INTO serialTest1 VALUES ('force', 100);
INSERT INTO serialTest1 VALUES ('wrong', NULL);

SELECT * FROM serialTest1;

SELECT pg_get_serial_sequence('serialTest1', 'f2');

-- test smallserial / bigserial
CREATE TABLE serialTest2 (f1 text, f2 serial, f3 smallserial, f4 serial2,
  f5 bigserial, f6 serial8);

INSERT INTO serialTest2 (f1)
  VALUES ('test_defaults');

INSERT INTO serialTest2 (f1, f2, f3, f4, f5, f6)
  VALUES ('test_max_vals', 2147483647, 32767, 32767, 9223372036854775807,
          9223372036854775807),
         ('test_min_vals', -2147483648, -32768, -32768, -9223372036854775808,
          -9223372036854775808);

-- All these INSERTs should fail:
INSERT INTO serialTest2 (f1, f3)
  VALUES ('bogus', -32769);

INSERT INTO serialTest2 (f1, f4)
  VALUES ('bogus', -32769);

INSERT INTO serialTest2 (f1, f3)
  VALUES ('bogus', 32768);

INSERT INTO serialTest2 (f1, f4)
  VALUES ('bogus', 32768);

INSERT INTO serialTest2 (f1, f5)
  VALUES ('bogus', -9223372036854775809);

INSERT INTO serialTest2 (f1, f6)
  VALUES ('bogus', -9223372036854775809);

INSERT INTO serialTest2 (f1, f5)
  VALUES ('bogus', 9223372036854775808);

INSERT INTO serialTest2 (f1, f6)
  VALUES ('bogus', 9223372036854775808);

SELECT * FROM serialTest2 ORDER BY f2 ASC;

SELECT nextval('serialTest2_f2_seq');
SELECT nextval('serialTest2_f3_seq');
SELECT nextval('serialTest2_f4_seq');
SELECT nextval('serialTest2_f5_seq');
SELECT nextval('serialTest2_f6_seq');

-- basic sequence operations using both text and oid references
CREATE SEQUENCE sequence_test;
CREATE SEQUENCE IF NOT EXISTS sequence_test;

SELECT nextval('sequence_test'::text);
SELECT nextval('sequence_test'::regclass);
SELECT currval('sequence_test'::text);
SELECT currval('sequence_test'::regclass);
--dg-skip: setval() is not supported yet
--dg SELECT setval('sequence_test'::text, 32);
--dg-cascade
--dg SELECT nextval('sequence_test'::regclass);
--dg-skip: setval() is not supported yet
--dg SELECT setval('sequence_test'::text, 99, false);
--dg-cascade
--dg SELECT nextval('sequence_test'::regclass);
--dg-skip: setval() is not supported yet
--dg SELECT setval('sequence_test'::regclass, 32);
--dg-cascade
--dg SELECT nextval('sequence_test'::text);
--dg-skip: setval() is not supported yet
--dg SELECT setval('sequence_test'::regclass, 99, false);
--dg-cascade
--dg SELECT nextval('sequence_test'::text);
DISCARD SEQUENCES;
SELECT currval('sequence_test'::regclass);

DROP SEQUENCE sequence_test;

-- renaming sequences
CREATE SEQUENCE foo_seq;
--dg-skip: "?" is a sequence, which cannot be used as a relation yet
--dg ALTER TABLE foo_seq RENAME TO foo_seq_new;
--dg-cascade
--dg SELECT * FROM foo_seq_new;
--dg-cascade
--dg SELECT nextval('foo_seq_new');
--dg-cascade
--dg SELECT nextval('foo_seq_new');
-- log_cnt can be higher if there is a checkpoint just at the right
-- time, so just test for the expected range
--dg-cascade
--dg SELECT last_value, log_cnt IN (31, 32) AS log_cnt_ok, is_called FROM foo_seq_new;
--dg-cascade
--dg DROP SEQUENCE foo_seq_new;

-- renaming serial sequences
--dg-skip: "?" is a sequence, which cannot be used as a relation yet
--dg ALTER TABLE serialtest1_f2_seq RENAME TO serialtest1_f2_foo;
INSERT INTO serialTest1 VALUES ('more');
SELECT * FROM serialTest1;

--
-- Check dependencies of serial and ordinary sequences
--
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not TEMPORARY or UNLOGGED
--dg CREATE TEMP SEQUENCE myseq2;
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not TEMPORARY or UNLOGGED
--dg CREATE TEMP SEQUENCE myseq3;
--dg-cascade
--dg CREATE TEMP TABLE t1 (
--dg   f1 serial,
--dg   f2 int DEFAULT nextval('myseq2'),
--dg   f3 int DEFAULT nextval('myseq3'::text)
--dg );
-- Both drops should fail, but with different error messages:
--dg-cascade
--dg DROP SEQUENCE t1_f1_seq;
--dg-cascade
--dg DROP SEQUENCE myseq2;
-- This however will work:
--dg-cascade
--dg DROP SEQUENCE myseq3;
--dg-cascade
--dg DROP TABLE t1;
-- Fails because no longer existent:
DROP SEQUENCE t1_f1_seq;
-- Now OK:
--dg-cascade
--dg DROP SEQUENCE myseq2;

--
-- Alter sequence
--

--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE IF EXISTS sequence_test2 RESTART WITH 24
--dg   INCREMENT BY 4 MAXVALUE 36 MINVALUE 5 CYCLE;

--dg-differs: another error: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE serialTest1 CYCLE;  -- error, not a sequence

CREATE SEQUENCE sequence_test2 START WITH 32;
--dg-skip: a sequence counts up from N or more, by N or more, yet
--dg CREATE SEQUENCE sequence_test4 INCREMENT BY -1;

SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test4');

--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test2 RESTART;
--dg-cascade
--dg SELECT nextval('sequence_test2');

--dg-differs: another error: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test2 RESTART WITH 0;  -- error
--dg-cascade
--dg ALTER SEQUENCE sequence_test4 RESTART WITH 40;  -- error

-- test CYCLE and NO CYCLE
--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test2 RESTART WITH 24
--dg   INCREMENT BY 4 MAXVALUE 36 MINVALUE 5 CYCLE;
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');  -- cycled

--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test2 RESTART WITH 24
--dg   NO CYCLE;
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');  -- error

--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test2 RESTART WITH -24 START WITH -24
--dg   INCREMENT BY -4 MINVALUE -36 MAXVALUE -5 CYCLE;
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');  -- cycled

--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test2 RESTART WITH -24
--dg   NO CYCLE;
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');
--dg-cascade
--dg SELECT nextval('sequence_test2');  -- error

-- reset
--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE IF EXISTS sequence_test2 RESTART WITH 32 START WITH 32
--dg   INCREMENT BY 4 MAXVALUE 36 MINVALUE 5 CYCLE;

--dg-cascade
--dg SELECT setval('sequence_test2', -100);  -- error
--dg-cascade
--dg SELECT setval('sequence_test2', 100);  -- error
--dg-skip: setval() is not supported yet
--dg SELECT setval('sequence_test2', 5);

CREATE SEQUENCE sequence_test3;  -- not read from, to test is_called


-- Information schema
--dg-cascade
--dg SELECT * FROM information_schema.sequences
--dg   WHERE sequence_name ~ ANY(ARRAY['sequence_test', 'serialtest'])
--dg   ORDER BY sequence_name ASC;

--dg-cascade
--dg SELECT schemaname, sequencename, start_value, min_value, max_value, increment_by, cycle, cache_size, last_value
--dg FROM pg_sequences
--dg WHERE sequencename ~ ANY(ARRAY['sequence_test', 'serialtest'])
--dg   ORDER BY sequencename ASC;


--dg-cascade
--dg SELECT * FROM pg_sequence_parameters('sequence_test4'::regclass);


--dg-cascade
--dg \d sequence_test4
--dg-differs: the result
--dg \d serialtest2_f2_seq


-- Test comments
COMMENT ON SEQUENCE asdf IS 'won''t work';
--dg-skip: "?" is a sequence, which cannot be used as a relation yet
--dg COMMENT ON SEQUENCE sequence_test2 IS 'will work';
--dg-skip: "?" is a sequence, which cannot be used as a relation yet
--dg COMMENT ON SEQUENCE sequence_test2 IS NULL;

-- Test lastval()
CREATE SEQUENCE seq;
SELECT nextval('seq');
SELECT lastval();
--dg-skip: setval() is not supported yet
--dg SELECT setval('seq', 99);
--dg-cascade
--dg SELECT lastval();
DISCARD SEQUENCES;
SELECT lastval();

CREATE SEQUENCE seq2;
SELECT nextval('seq2');
SELECT lastval();

DROP SEQUENCE seq2;
-- should fail
SELECT lastval();

-- unlogged sequences
-- (more tests in src/test/recovery/)
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not TEMPORARY or UNLOGGED
--dg CREATE UNLOGGED SEQUENCE sequence_test_unlogged;
--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test_unlogged SET LOGGED;
--dg-cascade
--dg \d sequence_test_unlogged
--dg-skip: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test_unlogged SET UNLOGGED;
--dg-cascade
--dg \d sequence_test_unlogged
--dg-cascade
--dg DROP SEQUENCE sequence_test_unlogged;

-- Test sequences in read-only transactions
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not TEMPORARY or UNLOGGED
--dg CREATE TEMPORARY SEQUENCE sequence_test_temp1;
START TRANSACTION READ ONLY;
--dg-cascade
--dg SELECT nextval('sequence_test_temp1');  -- ok
--dg-cascade
--dg SELECT nextval('sequence_test2');  -- error
ROLLBACK;
START TRANSACTION READ ONLY;
--dg-cascade
--dg SELECT setval('sequence_test_temp1', 1);  -- ok
--dg-cascade
--dg SELECT setval('sequence_test2', 1);  -- error
ROLLBACK;

-- privileges tests

CREATE USER regress_seq_user;

-- nextval
BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: no error where PostgreSQL has: permission denied for schema public
--dg CREATE SEQUENCE seq3;
--dg-cascade
--dg REVOKE ALL ON seq3 FROM regress_seq_user;
--dg-cascade
--dg GRANT SELECT ON seq3 TO regress_seq_user;
--dg-cascade
--dg SELECT nextval('seq3');
ROLLBACK;

BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: no error where PostgreSQL has: permission denied for schema public
--dg CREATE SEQUENCE seq3;
--dg-cascade
--dg REVOKE ALL ON seq3 FROM regress_seq_user;
--dg-cascade
--dg GRANT UPDATE ON seq3 TO regress_seq_user;
--dg-cascade
--dg SELECT nextval('seq3');
ROLLBACK;

BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: no error where PostgreSQL has: permission denied for schema public
--dg CREATE SEQUENCE seq3;
--dg-cascade
--dg REVOKE ALL ON seq3 FROM regress_seq_user;
--dg-cascade
--dg GRANT USAGE ON seq3 TO regress_seq_user;
--dg-cascade
--dg SELECT nextval('seq3');
ROLLBACK;

-- currval
BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: no error where PostgreSQL has: permission denied for schema public
--dg CREATE SEQUENCE seq3;
--dg-cascade
--dg SELECT nextval('seq3');
--dg-cascade
--dg REVOKE ALL ON seq3 FROM regress_seq_user;
--dg-cascade
--dg GRANT SELECT ON seq3 TO regress_seq_user;
--dg-cascade
--dg SELECT currval('seq3');
ROLLBACK;

BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: no error where PostgreSQL has: permission denied for schema public
--dg CREATE SEQUENCE seq3;
--dg-cascade
--dg SELECT nextval('seq3');
--dg-cascade
--dg REVOKE ALL ON seq3 FROM regress_seq_user;
--dg-cascade
--dg GRANT UPDATE ON seq3 TO regress_seq_user;
--dg-cascade
--dg SELECT currval('seq3');
ROLLBACK;

BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: no error where PostgreSQL has: permission denied for schema public
--dg CREATE SEQUENCE seq3;
--dg-cascade
--dg SELECT nextval('seq3');
--dg-cascade
--dg REVOKE ALL ON seq3 FROM regress_seq_user;
--dg-cascade
--dg GRANT USAGE ON seq3 TO regress_seq_user;
--dg-cascade
--dg SELECT currval('seq3');
ROLLBACK;

-- lastval
BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: no error where PostgreSQL has: permission denied for schema public
--dg CREATE SEQUENCE seq3;
--dg-cascade
--dg SELECT nextval('seq3');
--dg-cascade
--dg REVOKE ALL ON seq3 FROM regress_seq_user;
--dg-cascade
--dg GRANT SELECT ON seq3 TO regress_seq_user;
--dg-cascade
--dg SELECT lastval();
ROLLBACK;

BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: no error where PostgreSQL has: permission denied for schema public
--dg CREATE SEQUENCE seq3;
--dg-cascade
--dg SELECT nextval('seq3');
--dg-cascade
--dg REVOKE ALL ON seq3 FROM regress_seq_user;
--dg-cascade
--dg GRANT UPDATE ON seq3 TO regress_seq_user;
--dg-cascade
--dg SELECT lastval();
ROLLBACK;

BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: no error where PostgreSQL has: permission denied for schema public
--dg CREATE SEQUENCE seq3;
--dg-cascade
--dg SELECT nextval('seq3');
--dg-cascade
--dg REVOKE ALL ON seq3 FROM regress_seq_user;
--dg-cascade
--dg GRANT USAGE ON seq3 TO regress_seq_user;
--dg-cascade
--dg SELECT lastval();
ROLLBACK;

-- setval
BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: no error where PostgreSQL has: permission denied for schema public
--dg CREATE SEQUENCE seq3;
--dg-cascade
--dg REVOKE ALL ON seq3 FROM regress_seq_user;
--dg-cascade
--dg SAVEPOINT save;
--dg-cascade
--dg SELECT setval('seq3', 5);
--dg-cascade
--dg ROLLBACK TO save;
--dg-cascade
--dg GRANT UPDATE ON seq3 TO regress_seq_user;
--dg-cascade
--dg SELECT setval('seq3', 5);
--dg-cascade
--dg SELECT nextval('seq3');
ROLLBACK;

-- ALTER SEQUENCE
BEGIN;
SET LOCAL SESSION AUTHORIZATION regress_seq_user;
--dg-differs: another error: ALTER SEQUENCE is not supported yet
--dg ALTER SEQUENCE sequence_test2 START WITH 1;
ROLLBACK;

-- Sequences should get wiped out as well:
DROP TABLE serialTest1, serialTest2;

-- Make sure sequences are gone:
--dg-cascade
--dg SELECT * FROM information_schema.sequences WHERE sequence_name IN
--dg   ('sequence_test2', 'serialtest2_f2_seq', 'serialtest2_f3_seq',
--dg    'serialtest2_f4_seq', 'serialtest2_f5_seq', 'serialtest2_f6_seq')
--dg   ORDER BY sequence_name ASC;

DROP USER regress_seq_user;
DROP SEQUENCE seq;

-- cache tests
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not cache
--dg CREATE SEQUENCE test_seq1 CACHE 10;
--dg-cascade
--dg SELECT nextval('test_seq1');
--dg-cascade
--dg SELECT nextval('test_seq1');
--dg-cascade
--dg SELECT nextval('test_seq1');

-- pg_get_sequence_data
--dg-cascade
--dg SELECT * FROM pg_get_sequence_data('test_seq1');

--dg-cascade
--dg DROP SEQUENCE test_seq1;
