--dg-skip: CREATE DATABASE takes no options yet
--dg CREATE DATABASE regression_tbd
--dg 	ENCODING utf8 LC_COLLATE "C" LC_CTYPE "C" TEMPLATE template0;
--dg-skip: ALTER DATABASE is not supported
--dg ALTER DATABASE regression_tbd RENAME TO regression_utf8;
--dg-cascade
--dg ALTER DATABASE regression_utf8 SET TABLESPACE regress_tblspace;
--dg-skip: ALTER DATABASE is not supported
--dg ALTER DATABASE regression_utf8 RESET TABLESPACE;
--dg-skip: ALTER DATABASE is not supported
--dg ALTER DATABASE regression_utf8 CONNECTION_LIMIT 123;

-- Test PgDatabaseToastTable.  Doing this with GRANT would be slow.
BEGIN;
--dg-skip: permission denied for table pg_database
--dg UPDATE pg_database
--dg SET datacl = array_fill(makeaclitem(10, 10, 'USAGE', false), ARRAY[5e5::int])
--dg WHERE datname = 'regression_utf8';
-- load catcache entry, if nothing else does
--dg-cascade
--dg ALTER DATABASE regression_utf8 RESET TABLESPACE;
ROLLBACK;

--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_datdba_before;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_datdba_after;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER DATABASE regression_utf8 OWNER TO regress_datdba_before;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg REASSIGN OWNED BY regress_datdba_before TO regress_datdba_after;

--dg-cascade
--dg DROP DATABASE regression_utf8;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_datdba_before;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_datdba_after;
