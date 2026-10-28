CALL nonexistent();  -- error
CALL random();  -- error

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION cp_testfunc1(a int) RETURNS int LANGUAGE SQL AS $$ SELECT a $$;

CREATE TABLE cp_test (a int, b text);

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest1(x text)
--dg LANGUAGE SQL
--dg AS $$
--dg INSERT INTO cp_test VALUES (1, x);
--dg $$;

--dg-cascade
--dg \df ptest1
--dg-cascade
--dg SELECT pg_get_functiondef('ptest1'::regproc);

-- show only normal functions
--dg-cascade
--dg \dfn public.*test*1

-- show only procedures
--dg-cascade
--dg \dfp public.*test*1

--dg-cascade
--dg SELECT ptest1('x');  -- error
--dg-cascade
--dg CALL ptest1('a');  -- ok
--dg-cascade
--dg CALL ptest1('xy' || 'zzy');  -- ok, constant-folded arg
--dg-cascade
--dg CALL ptest1(substring(random()::numeric(20,15)::text, 1, 1));  -- ok, volatile arg

--dg-cascade
--dg SELECT * FROM cp_test ORDER BY b COLLATE "C";


-- SQL-standard body
--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest1s(x text)
--dg LANGUAGE SQL
--dg BEGIN ATOMIC
--dg   INSERT INTO cp_test VALUES (1, x);
--dg END;

--dg-cascade
--dg \df ptest1s
--dg-cascade
--dg SELECT pg_get_functiondef('ptest1s'::regproc);

--dg-cascade
--dg CALL ptest1s('b');

--dg-cascade
--dg SELECT * FROM cp_test ORDER BY b COLLATE "C";

-- utility functions currently not supported here
--dg-differs: another error: language "?" does not exist
--dg CREATE PROCEDURE ptestx()
--dg LANGUAGE SQL
--dg BEGIN ATOMIC
--dg   CREATE TABLE x (a int);
--dg END;


--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest2()
--dg LANGUAGE SQL
--dg AS $$
--dg SELECT 5;
--dg $$;

--dg-cascade
--dg CALL ptest2();


-- nested CALL
TRUNCATE cp_test;

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest3(y text)
--dg LANGUAGE SQL
--dg AS $$
--dg CALL ptest1(y);
--dg CALL ptest1($1);
--dg $$;

--dg-cascade
--dg CALL ptest3('b');

--dg-cascade
--dg SELECT * FROM cp_test;


-- output arguments

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest4a(INOUT a int, INOUT b int)
--dg LANGUAGE SQL
--dg AS $$
--dg SELECT 1, 2;
--dg $$;

--dg-cascade
--dg CALL ptest4a(NULL, NULL);

--dg-cascade
--dg CREATE PROCEDURE ptest4b(INOUT b int, INOUT a int)
--dg LANGUAGE SQL
--dg AS $$
--dg CALL ptest4a(a, b);  -- error, not supported
--dg $$;

-- we used to get confused by a single output argument that is composite
--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest4c(INOUT comp int8_tbl)
--dg LANGUAGE SQL
--dg AS $$
--dg SELECT ROW(1, 2);
--dg $$;

--dg-cascade
--dg CALL ptest4c(NULL);

--dg-cascade
--dg DROP PROCEDURE ptest4a, ptest4c;


-- named and default parameters

--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE PROCEDURE ptest5(a int, b text, c int default 100)
--dg LANGUAGE SQL
--dg AS $$
--dg INSERT INTO cp_test VALUES(a, b);
--dg INSERT INTO cp_test VALUES(c, b);
--dg $$;

TRUNCATE cp_test;

--dg-cascade
--dg CALL ptest5(10, 'Hello', 20);
--dg-cascade
--dg CALL ptest5(10, 'Hello');
--dg-cascade
--dg CALL ptest5(10, b => 'Hello');
--dg-cascade
--dg CALL ptest5(b => 'Hello', a => 10);

--dg-cascade
--dg SELECT * FROM cp_test;


-- polymorphic types

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest6(a int, b anyelement)
--dg LANGUAGE SQL
--dg AS $$
--dg SELECT NULL::int;
--dg $$;

--dg-cascade
--dg CALL ptest6(1, 2);

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest6a(inout a anyelement, out b anyelement)
--dg LANGUAGE SQL
--dg AS $$
--dg SELECT $1, $1;
--dg $$;

--dg-cascade
--dg CALL ptest6a(1, null);
--dg-cascade
--dg CALL ptest6a(1.1, null);

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest6b(a anyelement, out b anyelement, out c anyarray)
--dg LANGUAGE SQL
--dg AS $$
--dg SELECT $1, array[$1];
--dg $$;

--dg-cascade
--dg CALL ptest6b(1, null, null);
--dg-cascade
--dg CALL ptest6b(1.1, null, null);

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest6c(inout a anyelement, inout b anyelement)
--dg LANGUAGE SQL
--dg AS $$
--dg SELECT $1, 1;
--dg $$;

--dg-cascade
--dg CALL ptest6c(1, null);
--dg-cascade
--dg CALL ptest6c(1.1, null);  -- fails before v13


-- collation assignment

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest7(a text, b text)
--dg LANGUAGE SQL
--dg AS $$
--dg SELECT a = b;
--dg $$;

--dg-cascade
--dg CALL ptest7(least('a', 'b'), 'a');


-- empty body
--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest8(x text)
--dg BEGIN ATOMIC
--dg END;

--dg-cascade
--dg \df ptest8
--dg-cascade
--dg SELECT pg_get_functiondef('ptest8'::regproc);
--dg-cascade
--dg CALL ptest8('');


-- OUT parameters

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest9(OUT a int)
--dg LANGUAGE SQL
--dg AS $$
--dg INSERT INTO cp_test VALUES (1, 'a');
--dg SELECT 1;
--dg $$;

-- standard way to do a call:
--dg-cascade
--dg CALL ptest9(NULL);
-- you can write an expression, but it's not evaluated
--dg-cascade
--dg CALL ptest9(1/0);  -- no error
-- ... and it had better match the type of the parameter
CALL ptest9(1./0.);  -- error

-- check named-parameter matching
--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest10(OUT a int, IN b int, IN c int)
--dg LANGUAGE SQL AS $$ SELECT b - c $$;

--dg-cascade
--dg CALL ptest10(null, 7, 4);
--dg-cascade
--dg CALL ptest10(a => null, b => 8, c => 2);
--dg-cascade
--dg CALL ptest10(null, 7, c => 2);
--dg-cascade
--dg CALL ptest10(null, c => 4, b => 11);
--dg-cascade
--dg CALL ptest10(b => 8, c => 2, a => 0);

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest11(a OUT int, VARIADIC b int[]) LANGUAGE SQL
--dg   AS $$ SELECT b[1] + b[2] $$;

--dg-cascade
--dg CALL ptest11(null, 11, 12, 13);

-- check resolution of ambiguous DROP commands

--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE ptest10(IN a int, IN b int, IN c int)
--dg LANGUAGE SQL AS $$ SELECT a + b - c $$;

--dg-cascade
--dg \df ptest10

--dg-cascade
--dg drop procedure ptest10;  -- fail
--dg-cascade
--dg drop procedure ptest10(int, int, int);  -- fail
begin;
--dg-cascade
--dg drop procedure ptest10(out int, int, int);
--dg-cascade
--dg \df ptest10
--dg-cascade
--dg drop procedure ptest10(int, int, int);  -- now this would work
rollback;
begin;
--dg-cascade
--dg drop procedure ptest10(in int, int, int);
--dg-cascade
--dg \df ptest10
--dg-cascade
--dg drop procedure ptest10(int, int, int);  -- now this would work
rollback;

-- various error cases

CALL version();  -- error: not a procedure
CALL sum(1);  -- error: not a procedure

CREATE PROCEDURE ptestx() LANGUAGE SQL WINDOW AS $$ INSERT INTO cp_test VALUES (1, 'a') $$;
CREATE PROCEDURE ptestx() LANGUAGE SQL STRICT AS $$ INSERT INTO cp_test VALUES (1, 'a') $$;
--dg-differs: another error: language "?" does not exist
--dg CREATE PROCEDURE ptestx(a VARIADIC int[], b OUT int) LANGUAGE SQL
--dg   AS $$ SELECT a[1] $$;
--dg-differs: another error: language "?" does not exist
--dg CREATE PROCEDURE ptestx(a int DEFAULT 42, b OUT int) LANGUAGE SQL
--dg   AS $$ SELECT a $$;

--dg-cascade
--dg ALTER PROCEDURE ptest1(text) STRICT;
--dg-cascade
--dg ALTER FUNCTION ptest1(text) VOLATILE;  -- error: not a function
--dg-cascade
--dg ALTER PROCEDURE cp_testfunc1(int) VOLATILE;  -- error: not a procedure
ALTER PROCEDURE nonexistent() VOLATILE;

--dg-cascade
--dg DROP FUNCTION ptest1(text);  -- error: not a function
--dg-cascade
--dg DROP PROCEDURE cp_testfunc1(int);  -- error: not a procedure
DROP PROCEDURE nonexistent();


-- privileges

CREATE USER regress_cp_user1;
--dg-skip: relation pg_class cannot be written: digres has no catalog tables
--dg GRANT INSERT ON cp_test TO regress_cp_user1;
--dg-cascade
--dg REVOKE EXECUTE ON PROCEDURE ptest1(text) FROM PUBLIC;
SET ROLE regress_cp_user1;
--dg-cascade
--dg CALL ptest1('a');  -- error
RESET ROLE;
--dg-cascade
--dg GRANT EXECUTE ON PROCEDURE ptest1(text) TO regress_cp_user1;
SET ROLE regress_cp_user1;
--dg-cascade
--dg CALL ptest1('a');  -- ok
RESET ROLE;


-- ROUTINE syntax

--dg-cascade
--dg ALTER ROUTINE cp_testfunc1(int) RENAME TO cp_testfunc1a;
--dg-cascade
--dg ALTER ROUTINE cp_testfunc1a RENAME TO cp_testfunc1;

--dg-cascade
--dg ALTER ROUTINE ptest1(text) RENAME TO ptest1a;
--dg-cascade
--dg ALTER ROUTINE ptest1a RENAME TO ptest1;

--dg-cascade
--dg DROP ROUTINE cp_testfunc1(int);


-- cleanup

--dg-cascade
--dg DROP PROCEDURE ptest1;
--dg-cascade
--dg DROP PROCEDURE ptest1s;
--dg-cascade
--dg DROP PROCEDURE ptest2;

DROP TABLE cp_test;

DROP USER regress_cp_user1;
