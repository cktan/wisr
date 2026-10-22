--
-- CREATE_FUNCTION_C
--
-- This script used to create C functions for other scripts to use.
-- But to get rid of the ordering dependencies that caused, such
-- functions are now made either in test_setup.sql or in the specific
-- test script that needs them.  All that remains here is error cases.

-- directory path and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

--
-- Check LOAD command.  (The alternative of implicitly loading the library
-- is checked in many other test scripts.)
--
--dg-skip: loading library "?" is not supported yet
--dg LOAD :'regresslib';

-- Things that shouldn't work:

--dg-differs: another error: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION test1 (int) RETURNS int LANGUAGE C
--dg     AS 'nosuchfile';

-- To produce stable regression test output, we have to filter the name
-- of the regresslib file out of the error message in this test.
\set VERBOSITY sqlstate
--dg-differs: another error: 0A000
--dg CREATE FUNCTION test1 (int) RETURNS int LANGUAGE C
--dg     AS :'regresslib', 'nosuchsymbol';
\set VERBOSITY default
--dg-cascade
--dg SELECT regexp_replace(:'LAST_ERROR_MESSAGE', 'file ".*"', 'file "..."');

--dg-differs: another error: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION test1 (int) RETURNS int LANGUAGE internal
--dg     AS 'nosuch';
