--dg-skip: unrecognized configuration parameter "?"
--dg SELECT current_setting('max_prepared_transactions')::integer < 2 AS skip_test \gset
\if :skip_test
--dg-cascade
--dg \quit
\endif

--
-- PREPARED TRANSACTIONS (two-phase commit)
--
-- We can't readily test persistence of prepared xacts within the
-- regression script framework, unfortunately.  Note that a crash
-- isn't really needed ... stopping and starting the postmaster would
-- be enough, but we can't even do that here.


-- create a simple table that we'll use in the tests
--dg-cascade
--dg CREATE TABLE pxtest1 (foobar VARCHAR(10));

--dg-cascade
--dg INSERT INTO pxtest1 VALUES ('aaa');


-- Test PREPARE TRANSACTION
--dg-cascade
--dg BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
--dg-cascade
--dg UPDATE pxtest1 SET foobar = 'bbb' WHERE foobar = 'aaa';
--dg-cascade
--dg SELECT * FROM pxtest1;
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_foo1';

--dg-cascade
--dg SELECT * FROM pxtest1;

-- Test pg_prepared_xacts system view
--dg-cascade
--dg SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

-- Test ROLLBACK PREPARED
--dg-cascade
--dg ROLLBACK PREPARED 'regress_foo1';

--dg-cascade
--dg SELECT * FROM pxtest1;

--dg-cascade
--dg SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;


-- Test COMMIT PREPARED
--dg-cascade
--dg BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
--dg-cascade
--dg INSERT INTO pxtest1 VALUES ('ddd');
--dg-cascade
--dg SELECT * FROM pxtest1;
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_foo2';

--dg-cascade
--dg SELECT * FROM pxtest1;

--dg-cascade
--dg COMMIT PREPARED 'regress_foo2';

--dg-cascade
--dg SELECT * FROM pxtest1;

-- Test duplicate gids
--dg-cascade
--dg BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
--dg-cascade
--dg UPDATE pxtest1 SET foobar = 'eee' WHERE foobar = 'ddd';
--dg-cascade
--dg SELECT * FROM pxtest1;
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_foo3';

--dg-cascade
--dg SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

--dg-cascade
--dg BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
--dg-cascade
--dg INSERT INTO pxtest1 VALUES ('fff');

-- This should fail, because the gid foo3 is already in use
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_foo3';

--dg-cascade
--dg SELECT * FROM pxtest1;

--dg-cascade
--dg ROLLBACK PREPARED 'regress_foo3';

--dg-cascade
--dg SELECT * FROM pxtest1;

-- Test serialization failure (SSI)
--dg-cascade
--dg BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
--dg-cascade
--dg UPDATE pxtest1 SET foobar = 'eee' WHERE foobar = 'ddd';
--dg-cascade
--dg SELECT * FROM pxtest1;
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_foo4';

--dg-cascade
--dg SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

--dg-cascade
--dg BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
--dg-cascade
--dg SELECT * FROM pxtest1;

-- This should fail, because the two transactions have a write-skew anomaly
--dg-cascade
--dg INSERT INTO pxtest1 VALUES ('fff');
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_foo5';

--dg-cascade
--dg SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

--dg-cascade
--dg ROLLBACK PREPARED 'regress_foo4';

--dg-cascade
--dg SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

-- Clean up
--dg-cascade
--dg DROP TABLE pxtest1;

-- Test detection of session-level and xact-level locks on same object
--dg-cascade
--dg BEGIN;
--dg-cascade
--dg SELECT pg_advisory_lock(1);
--dg-cascade
--dg SELECT pg_advisory_xact_lock_shared(1);
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_foo6';  -- fails

-- Test subtransactions
--dg-cascade
--dg BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
--dg-cascade
--dg   CREATE TABLE pxtest2 (a int);
--dg-cascade
--dg   INSERT INTO pxtest2 VALUES (1);
--dg-cascade
--dg   SAVEPOINT a;
--dg-cascade
--dg     INSERT INTO pxtest2 VALUES (2);
--dg-cascade
--dg   ROLLBACK TO a;
--dg-cascade
--dg   SAVEPOINT b;
--dg-cascade
--dg   INSERT INTO pxtest2 VALUES (3);
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_sub1';

--dg-cascade
--dg CREATE TABLE pxtest3(fff int);

-- Test shared invalidation
--dg-cascade
--dg BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
--dg-cascade
--dg   DROP TABLE pxtest3;
--dg-cascade
--dg   CREATE TABLE pxtest4 (a int);
--dg-cascade
--dg   INSERT INTO pxtest4 VALUES (1);
--dg-cascade
--dg   INSERT INTO pxtest4 VALUES (2);
--dg-cascade
--dg   DECLARE foo CURSOR FOR SELECT * FROM pxtest4;
  -- Fetch 1 tuple, keeping the cursor open
--dg-cascade
--dg   FETCH 1 FROM foo;
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_sub2';

-- No such cursor
--dg-cascade
--dg FETCH 1 FROM foo;

-- Table doesn't exist, the creation hasn't been committed yet
--dg-cascade
--dg SELECT * FROM pxtest2;

-- There should be two prepared transactions
--dg-cascade
--dg SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

-- pxtest3 should be locked because of the pending DROP
--dg-cascade
--dg begin;
--dg-cascade
--dg lock table pxtest3 in access share mode nowait;
--dg-cascade
--dg rollback;

-- Disconnect, we will continue testing in a different backend
--dg-cascade
--dg \c -

-- There should still be two prepared transactions
--dg-cascade
--dg SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

-- pxtest3 should still be locked because of the pending DROP
--dg-cascade
--dg begin;
--dg-cascade
--dg lock table pxtest3 in access share mode nowait;
--dg-cascade
--dg rollback;

-- Commit table creation
--dg-cascade
--dg COMMIT PREPARED 'regress_sub1';
--dg-cascade
--dg \d pxtest2
--dg-cascade
--dg SELECT * FROM pxtest2;

-- There should be one prepared transaction
--dg-cascade
--dg SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;

-- Commit table drop
--dg-cascade
--dg COMMIT PREPARED 'regress_sub2';
--dg-cascade
--dg SELECT * FROM pxtest3;

-- There should be no prepared transactions
--dg-cascade
--dg SELECT gid FROM pg_prepared_xacts WHERE gid ~ '^regress_' ORDER BY gid;


-- Test row-level locks held by prepared transactions
--dg-cascade
--dg CREATE TABLE pxtest_rowlock (id int PRIMARY KEY, data text);
--dg-cascade
--dg INSERT INTO pxtest_rowlock VALUES (1, 'test data');

--dg-cascade
--dg BEGIN;
--dg-cascade
--dg SELECT * FROM pxtest_rowlock WHERE id = 1 FOR SHARE;
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_p1';

-- Should fail because the row is locked
--dg-cascade
--dg SELECT * FROM pxtest_rowlock WHERE id = 1 FOR UPDATE NOWAIT;

-- Test prepared transactions that participate in multixacts. For
-- that, lock the same row again, creating a multixid.
--dg-cascade
--dg BEGIN;
--dg-cascade
--dg SELECT * FROM pxtest_rowlock WHERE id = 1 FOR SHARE;
--dg-skip: PREPARE TRANSACTION is not supported
--dg PREPARE TRANSACTION 'regress_p2';

-- Should fail because the row is locked
--dg-cascade
--dg SELECT * FROM pxtest_rowlock WHERE id = 1 FOR UPDATE NOWAIT;

--dg-cascade
--dg ROLLBACK PREPARED 'regress_p1';
--dg-cascade
--dg ROLLBACK PREPARED 'regress_p2';

-- Clean up
--dg-cascade
--dg DROP TABLE pxtest2;
-- pxtest3 was already dropped
--dg-cascade
--dg DROP TABLE pxtest4;
--dg-cascade
--dg DROP TABLE pxtest_rowlock;
