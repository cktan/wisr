--
-- Tests for some likely failure cases with combo cmin/cmax mechanism
--
CREATE TEMP TABLE combocidtest (foobar int);

BEGIN;

-- a few dummy ops to push up the CommandId counter
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;

INSERT INTO combocidtest VALUES (1);
INSERT INTO combocidtest VALUES (2);

--dg-skip: column "?" does not exist
--dg SELECT ctid,cmin,* FROM combocidtest;

SAVEPOINT s1;

UPDATE combocidtest SET foobar = foobar + 10;

-- here we should see only updated tuples
--dg-skip: column "?" does not exist
--dg SELECT ctid,cmin,* FROM combocidtest;

ROLLBACK TO s1;

-- now we should see old tuples, but with combo CIDs starting at 0
--dg-skip: column "?" does not exist
--dg SELECT ctid,cmin,* FROM combocidtest;

COMMIT;

-- combo data is not there anymore, but should still see tuples
--dg-skip: column "?" does not exist
--dg SELECT ctid,cmin,* FROM combocidtest;

-- Test combo CIDs with portals
BEGIN;

INSERT INTO combocidtest VALUES (333);

--dg-skip: column "?" does not exist
--dg DECLARE c CURSOR FOR SELECT ctid,cmin,* FROM combocidtest;

DELETE FROM combocidtest;

--dg-cascade
--dg FETCH ALL FROM c;

ROLLBACK;

--dg-skip: column "?" does not exist
--dg SELECT ctid,cmin,* FROM combocidtest;

-- check behavior with locked tuples
BEGIN;

-- a few dummy ops to push up the CommandId counter
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;
INSERT INTO combocidtest SELECT 1 LIMIT 0;

INSERT INTO combocidtest VALUES (444);

--dg-skip: column "?" does not exist
--dg SELECT ctid,cmin,* FROM combocidtest;

SAVEPOINT s1;

-- this doesn't affect cmin
--dg-skip: column "?" does not exist
--dg SELECT ctid,cmin,* FROM combocidtest FOR UPDATE;
--dg-skip: column "?" does not exist
--dg SELECT ctid,cmin,* FROM combocidtest;

-- but this does
UPDATE combocidtest SET foobar = foobar + 10;

--dg-cascade
--dg SELECT ctid,cmin,* FROM combocidtest;

ROLLBACK TO s1;

--dg-cascade
--dg SELECT ctid,cmin,* FROM combocidtest;

COMMIT;

--dg-cascade
--dg SELECT ctid,cmin,* FROM combocidtest;

-- test for bug reported in
-- CABRT9RC81YUf1=jsmWopcKJEro=VoeG2ou6sPwyOUTx_qteRsg@mail.gmail.com
CREATE TABLE IF NOT EXISTS testcase(
	id int PRIMARY KEY,
	balance numeric
);
INSERT INTO testcase VALUES (1, 0);
BEGIN;
--dg-differs: the result
--dg SELECT * FROM testcase WHERE testcase.id = 1 FOR UPDATE;
UPDATE testcase SET balance = balance + 400 WHERE id=1;
SAVEPOINT subxact;
UPDATE testcase SET balance = balance - 100 WHERE id=1;
ROLLBACK TO SAVEPOINT subxact;
-- should return one tuple
--dg-differs: the result
--dg SELECT * FROM testcase WHERE id = 1 FOR UPDATE;
ROLLBACK;
DROP TABLE testcase;
