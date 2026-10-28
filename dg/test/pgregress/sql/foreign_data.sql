--
-- Test foreign-data wrapper and server management.
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

--dg-skip: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION test_fdw_handler()
--dg     RETURNS fdw_handler
--dg     AS :'regresslib', 'test_fdw_handler'
--dg     LANGUAGE C;

-- Clean up in case a prior regression run failed

-- Suppress NOTICE messages when roles don't exist
SET client_min_messages TO 'warning';

DROP ROLE IF EXISTS regress_foreign_data_user, regress_test_role, regress_test_role2, regress_test_role_super, regress_test_indirect, regress_unprivileged_role;

RESET client_min_messages;

CREATE ROLE regress_foreign_data_user LOGIN SUPERUSER;
SET SESSION AUTHORIZATION 'regress_foreign_data_user';

CREATE ROLE regress_test_role;
CREATE ROLE regress_test_role2;
CREATE ROLE regress_test_role_super SUPERUSER;
CREATE ROLE regress_test_indirect;
CREATE ROLE regress_unprivileged_role;

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER dummy;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg COMMENT ON FOREIGN DATA WRAPPER dummy IS 'useless';
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER postgresql VALIDATOR postgresql_fdw_validator;

-- At this point we should have 2 built-in wrappers and no servers.
--dg-cascade
--dg SELECT fdwname, fdwhandler::regproc, fdwvalidator::regproc, fdwoptions FROM pg_foreign_data_wrapper ORDER BY 1, 2, 3;
--dg-skip: relation "?" does not exist
--dg SELECT srvname, srvoptions FROM pg_foreign_server;
--dg-skip: relation "?" does not exist
--dg SELECT * FROM pg_user_mapping;

-- CREATE FOREIGN DATA WRAPPER
--dg-differs: another error: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER foo VALIDATOR bar;            -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER foo;
--dg-cascade
--dg \dew

--dg-cascade
--dg CREATE FOREIGN DATA WRAPPER foo; -- duplicate
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER foo;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER foo OPTIONS (testing '1');
--dg-cascade
--dg \dew+

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER foo;
--dg-cascade
--dg CREATE FOREIGN DATA WRAPPER foo OPTIONS (testing '1', testing '2');   -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER foo OPTIONS (testing '1', another '2');
--dg-cascade
--dg \dew+

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER foo;
SET ROLE regress_test_role;
--dg-differs: another error: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER foo; -- ERROR
RESET ROLE;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER foo VALIDATOR postgresql_fdw_validator;
--dg-cascade
--dg \dew+

-- HANDLER related checks
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION invalid_fdw_handler() RETURNS int LANGUAGE SQL AS 'SELECT 1;';
--dg-cascade
--dg CREATE FOREIGN DATA WRAPPER test_fdw HANDLER invalid_fdw_handler;  -- ERROR
--dg-cascade
--dg CREATE FOREIGN DATA WRAPPER test_fdw HANDLER test_fdw_handler HANDLER invalid_fdw_handler;  -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER test_fdw HANDLER test_fdw_handler;

-- should preserve dependency on test_fdw_handler
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER test_fdw VALIDATOR postgresql_fdw_validator;
--dg-cascade
--dg DROP FUNCTION test_fdw_handler(); -- ERROR

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER test_fdw;

-- ALTER FOREIGN DATA WRAPPER
--dg-cascade
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (nonexistent 'fdw');         -- ERROR

ALTER FOREIGN DATA WRAPPER foo;                             -- ERROR
--dg-cascade
--dg ALTER FOREIGN DATA WRAPPER foo VALIDATOR bar;               -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo NO VALIDATOR;
--dg-cascade
--dg \dew+

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (a '1', b '2');
--dg-cascade
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (SET c '4');         -- ERROR
--dg-cascade
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (DROP c);            -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD x '1', DROP x);
--dg-cascade
--dg \dew+

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (DROP a, SET b '3', ADD c '4');
--dg-cascade
--dg \dew+

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (a '2');
--dg-cascade
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (b '4');             -- ERROR
--dg-cascade
--dg \dew+

SET ROLE regress_test_role;
--dg-differs: another error: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD d '5');         -- ERROR
SET ROLE regress_test_role_super;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD d '5');
--dg-cascade
--dg \dew+

--dg-cascade
--dg ALTER FOREIGN DATA WRAPPER foo OWNER TO regress_test_role;  -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo OWNER TO regress_test_role_super;
ALTER ROLE regress_test_role_super NOSUPERUSER;
SET ROLE regress_test_role_super;
--dg-differs: another error: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD e '6');         -- ERROR
RESET ROLE;
--dg-cascade
--dg \dew+

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo RENAME TO foo1;
--dg-cascade
--dg \dew+
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo1 RENAME TO foo;

-- HANDLER related checks
--dg-cascade
--dg ALTER FOREIGN DATA WRAPPER foo HANDLER invalid_fdw_handler;  -- ERROR
--dg-cascade
--dg ALTER FOREIGN DATA WRAPPER foo HANDLER test_fdw_handler HANDLER anything;  -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo HANDLER test_fdw_handler;
--dg-cascade
--dg DROP FUNCTION invalid_fdw_handler();

-- DROP FOREIGN DATA WRAPPER
--dg-differs: another error: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER nonexistent;                      -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER IF EXISTS nonexistent;
--dg-cascade
--dg \dew+

--dg-cascade
--dg DROP ROLE regress_test_role_super;                          -- ERROR
--dg-cascade
--dg SET ROLE regress_test_role_super;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER foo;
RESET ROLE;
--dg-cascade
--dg DROP ROLE regress_test_role_super;
--dg-cascade
--dg \dew+

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER foo;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s1 FOREIGN DATA WRAPPER foo;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg COMMENT ON SERVER s1 IS 'foreign server';
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR current_user SERVER s1;
--dg-cascade
--dg CREATE USER MAPPING FOR current_user SERVER s1;				-- ERROR
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING IF NOT EXISTS FOR current_user SERVER s1; -- NOTICE
--dg-cascade
--dg \dew+
--dg-cascade
--dg \des+
--dg-cascade
--dg \deu+
--dg-cascade
--dg DROP FOREIGN DATA WRAPPER foo;                              -- ERROR
SET ROLE regress_test_role;
--dg-cascade
--dg DROP FOREIGN DATA WRAPPER foo CASCADE;                      -- ERROR
RESET ROLE;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER foo CASCADE;
--dg-cascade
--dg \dew+
--dg-skip: relation "?" does not exist
--dg \des+
--dg-skip: relation "?" does not exist
--dg \deu+

-- exercise CREATE SERVER
--dg-differs: another error: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s1 FOREIGN DATA WRAPPER foo;                  -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER foo OPTIONS ("test wrapper" 'true');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s1 FOREIGN DATA WRAPPER foo;
--dg-cascade
--dg CREATE SERVER s1 FOREIGN DATA WRAPPER foo;                  -- ERROR
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER IF NOT EXISTS s1 FOREIGN DATA WRAPPER foo;	-- No ERROR, just NOTICE
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s2 FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s3 TYPE 'oracle' FOREIGN DATA WRAPPER foo;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s4 TYPE 'oracle' FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s5 VERSION '15.0' FOREIGN DATA WRAPPER foo;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s6 VERSION '16.0' FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s7 TYPE 'oracle' VERSION '17.0' FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');
--dg-cascade
--dg CREATE SERVER s8 FOREIGN DATA WRAPPER postgresql OPTIONS (foo '1'); -- ERROR
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s8 FOREIGN DATA WRAPPER postgresql OPTIONS (host 'localhost', dbname 's8db');
--dg-cascade
--dg \des+
SET ROLE regress_test_role;
--dg-cascade
--dg CREATE SERVER t1 FOREIGN DATA WRAPPER foo;                 -- ERROR: no usage on FDW
RESET ROLE;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_role;
SET ROLE regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER t1 FOREIGN DATA WRAPPER foo;
RESET ROLE;
--dg-cascade
--dg \des+

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg REVOKE USAGE ON FOREIGN DATA WRAPPER foo FROM regress_test_role;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_indirect;
SET ROLE regress_test_role;
--dg-cascade
--dg CREATE SERVER t2 FOREIGN DATA WRAPPER foo;                 -- ERROR
RESET ROLE;
GRANT regress_test_indirect TO regress_test_role;
SET ROLE regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER t2 FOREIGN DATA WRAPPER foo;
--dg-cascade
--dg \des+
RESET ROLE;
REVOKE regress_test_indirect FROM regress_test_role;

-- ALTER SERVER
ALTER SERVER s0;                                            -- ERROR
--dg-differs: another error: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s0 OPTIONS (a '1');                            -- ERROR
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s1 VERSION '1.0' OPTIONS (servername 's1');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s2 VERSION '1.1';
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s3 OPTIONS ("tns name" 'orcl', port '1521');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN SERVER s1 TO regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN SERVER s6 TO regress_test_role2 WITH GRANT OPTION;
--dg-cascade
--dg \des+
SET ROLE regress_test_role;
--dg-cascade
--dg ALTER SERVER s1 VERSION '1.1';                              -- ERROR
--dg-cascade
--dg ALTER SERVER s1 OWNER TO regress_test_role;                 -- ERROR
RESET ROLE;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s1 OWNER TO regress_test_role;
GRANT regress_test_role2 TO regress_test_role;
SET ROLE regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s1 VERSION '1.1';
--dg-cascade
--dg ALTER SERVER s1 OWNER TO regress_test_role2;                -- ERROR
RESET ROLE;
--dg-cascade
--dg ALTER SERVER s8 OPTIONS (foo '1');                          -- ERROR option validation
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s8 OPTIONS (connect_timeout '30', SET dbname 'db1', DROP host);
SET ROLE regress_test_role;
--dg-cascade
--dg ALTER SERVER s1 OWNER TO regress_test_indirect;             -- ERROR
RESET ROLE;
GRANT regress_test_indirect TO regress_test_role;
SET ROLE regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s1 OWNER TO regress_test_indirect;
RESET ROLE;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_indirect;
SET ROLE regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s1 OWNER TO regress_test_indirect;
RESET ROLE;
--dg-cascade
--dg DROP ROLE regress_test_indirect;                            -- ERROR
--dg-cascade
--dg \des+

--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s8 RENAME to s8new;
--dg-cascade
--dg \des+
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s8new RENAME to s8;

-- DROP SERVER
--dg-differs: another error: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP SERVER nonexistent;                                    -- ERROR
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP SERVER IF EXISTS nonexistent;
--dg-cascade
--dg \des
SET ROLE regress_test_role;
--dg-cascade
--dg DROP SERVER s2;                                             -- ERROR
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP SERVER s1;
RESET ROLE;
--dg-cascade
--dg \des
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s2 OWNER TO regress_test_role;
SET ROLE regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP SERVER s2;
RESET ROLE;
--dg-cascade
--dg \des
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR current_user SERVER s3;
--dg-cascade
--dg \deu
--dg-cascade
--dg DROP SERVER s3;                                             -- ERROR
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP SERVER s3 CASCADE;
--dg-cascade
--dg \des
--dg-skip: relation "?" does not exist
--dg \deu

-- CREATE USER MAPPING
--dg-differs: another error: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR regress_test_missing_role SERVER s1;  -- ERROR
--dg-differs: another error: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR current_user SERVER s1;             -- ERROR
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR current_user SERVER s4;
--dg-cascade
--dg CREATE USER MAPPING FOR user SERVER s4;                     -- ERROR duplicate
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR public SERVER s4 OPTIONS ("this mapping" 'is public');
--dg-cascade
--dg CREATE USER MAPPING FOR user SERVER s8 OPTIONS (username 'test', password 'secret');    -- ERROR
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR user SERVER s8 OPTIONS (user 'test', password 'secret');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s5 OWNER TO regress_test_role;
--dg-cascade
--dg ALTER SERVER s6 OWNER TO regress_test_indirect;
SET ROLE regress_test_role;
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR current_user SERVER s5;
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR current_user SERVER s6 OPTIONS (username 'test');
--dg-cascade
--dg CREATE USER MAPPING FOR current_user SERVER s7;             -- ERROR
--dg-cascade
--dg CREATE USER MAPPING FOR public SERVER s8;                   -- ERROR
RESET ROLE;

--dg-cascade
--dg ALTER SERVER t1 OWNER TO regress_test_indirect;
SET ROLE regress_test_role;
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR current_user SERVER t1 OPTIONS (username 'bob', password 'boo');
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR public SERVER t1;
RESET ROLE;
--dg-cascade
--dg \deu

-- ALTER USER MAPPING
--dg-differs: another error: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg ALTER USER MAPPING FOR regress_test_missing_role SERVER s4 OPTIONS (gotcha 'true'); -- ERROR
--dg-differs: another error: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg ALTER USER MAPPING FOR user SERVER ss4 OPTIONS (gotcha 'true'); -- ERROR
--dg-cascade
--dg ALTER USER MAPPING FOR public SERVER s5 OPTIONS (gotcha 'true');            -- ERROR
--dg-cascade
--dg ALTER USER MAPPING FOR current_user SERVER s8 OPTIONS (username 'test');    -- ERROR
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg ALTER USER MAPPING FOR current_user SERVER s8 OPTIONS (DROP user, SET password 'public');
SET ROLE regress_test_role;
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg ALTER USER MAPPING FOR current_user SERVER s5 OPTIONS (ADD modified '1');
--dg-cascade
--dg ALTER USER MAPPING FOR public SERVER s4 OPTIONS (ADD modified '1'); -- ERROR
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg ALTER USER MAPPING FOR public SERVER t1 OPTIONS (ADD modified '1');
RESET ROLE;
--dg-cascade
--dg \deu+

-- DROP USER MAPPING
DROP USER MAPPING FOR regress_test_missing_role SERVER s4;  -- ERROR
--dg-differs: another error: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP USER MAPPING FOR user SERVER ss4;
--dg-cascade
--dg DROP USER MAPPING FOR public SERVER s7;                     -- ERROR
DROP USER MAPPING IF EXISTS FOR regress_test_missing_role SERVER s4;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP USER MAPPING IF EXISTS FOR user SERVER ss4;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP USER MAPPING IF EXISTS FOR public SERVER s7;
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR public SERVER s8;
SET ROLE regress_test_role;
--dg-cascade
--dg DROP USER MAPPING FOR public SERVER s8;                     -- ERROR
RESET ROLE;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP SERVER s7;
--dg-cascade
--dg \deu

-- CREATE FOREIGN TABLE
CREATE SCHEMA foreign_schema;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s0 FOREIGN DATA WRAPPER dummy;
CREATE FOREIGN TABLE ft1 ();                                    -- ERROR
--dg-differs: another error: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE ft1 () SERVER no_server;                   -- ERROR
CREATE FOREIGN TABLE ft1 (
	c1 integer OPTIONS ("param 1" 'val1') PRIMARY KEY,
	c2 text OPTIONS (param2 'val2', param3 'val3'),
	c3 date
) SERVER s0 OPTIONS (delimiter ',', quote '"', "be quoted" 'value'); -- ERROR
CREATE TABLE ref_table (id integer PRIMARY KEY);
CREATE FOREIGN TABLE ft1 (
	c1 integer OPTIONS ("param 1" 'val1') REFERENCES ref_table (id),
	c2 text OPTIONS (param2 'val2', param3 'val3'),
	c3 date
) SERVER s0 OPTIONS (delimiter ',', quote '"', "be quoted" 'value'); -- ERROR
DROP TABLE ref_table;
CREATE FOREIGN TABLE ft1 (
	c1 integer OPTIONS ("param 1" 'val1') NOT NULL,
	c2 text OPTIONS (param2 'val2', param3 'val3'),
	c3 date,
	UNIQUE (c3)
) SERVER s0 OPTIONS (delimiter ',', quote '"', "be quoted" 'value'); -- ERROR
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE ft1 (
--dg 	c1 integer OPTIONS ("param 1" 'val1') NOT NULL,
--dg 	c2 text OPTIONS (param2 'val2', param3 'val3') CHECK (c2 <> ''),
--dg 	c3 date,
--dg 	CHECK (c3 BETWEEN '1994-01-01'::date AND '1994-01-31'::date)
--dg ) SERVER s0 OPTIONS (delimiter ',', quote '"', "be quoted" 'value');
--dg-cascade
--dg COMMENT ON FOREIGN TABLE ft1 IS 'ft1';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN ft1.c1 IS 'ft1.c1';
--dg-cascade
--dg \d+ ft1
--dg-cascade
--dg \det+
--dg-cascade
--dg CREATE INDEX id_ft1_c2 ON ft1 (c2);                             -- ERROR
--dg-cascade
--dg SELECT * FROM ft1;                                              -- ERROR
--dg-cascade
--dg EXPLAIN SELECT * FROM ft1;                                      -- ERROR

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE lt1 (a INT) PARTITION BY RANGE (a);
--dg-cascade
--dg CREATE FOREIGN TABLE ft_part1
--dg   PARTITION OF lt1 FOR VALUES FROM (0) TO (1000) SERVER s0;
--dg-cascade
--dg CREATE INDEX ON lt1 (a);                              -- skips partition
--dg-cascade
--dg CREATE UNIQUE INDEX ON lt1 (a);                                 -- ERROR
--dg-cascade
--dg ALTER TABLE lt1 ADD PRIMARY KEY (a);                            -- ERROR
--dg-cascade
--dg DROP TABLE lt1;

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE lt1 (a INT) PARTITION BY RANGE (a);
--dg-cascade
--dg CREATE INDEX ON lt1 (a);
--dg-cascade
--dg CREATE FOREIGN TABLE ft_part1
--dg   PARTITION OF lt1 FOR VALUES FROM (0) TO (1000) SERVER s0;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE ft_part2 (a INT) SERVER s0;
--dg-cascade
--dg ALTER TABLE lt1 ATTACH PARTITION ft_part2 FOR VALUES FROM (1000) TO (2000);
--dg-cascade
--dg DROP FOREIGN TABLE ft_part1, ft_part2;
--dg-cascade
--dg CREATE UNIQUE INDEX ON lt1 (a);
--dg-cascade
--dg ALTER TABLE lt1 ADD PRIMARY KEY (a);
--dg-cascade
--dg CREATE FOREIGN TABLE ft_part1
--dg   PARTITION OF lt1 FOR VALUES FROM (0) TO (1000) SERVER s0;     -- ERROR
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE ft_part2 (a INT NOT NULL) SERVER s0;
--dg-cascade
--dg ALTER TABLE lt1 ATTACH PARTITION ft_part2
--dg   FOR VALUES FROM (1000) TO (2000);                             -- ERROR
--dg-cascade
--dg DROP TABLE lt1;
--dg-cascade
--dg DROP FOREIGN TABLE ft_part2;

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE lt1 (a INT) PARTITION BY RANGE (a);
--dg-cascade
--dg CREATE INDEX ON lt1 (a);
--dg-cascade
--dg CREATE TABLE lt1_part1
--dg   PARTITION OF lt1 FOR VALUES FROM (0) TO (1000)
--dg   PARTITION BY RANGE (a);
--dg-cascade
--dg CREATE FOREIGN TABLE ft_part_1_1
--dg   PARTITION OF lt1_part1 FOR VALUES FROM (0) TO (100) SERVER s0;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE ft_part_1_2 (a INT) SERVER s0;
--dg-cascade
--dg ALTER TABLE lt1_part1 ATTACH PARTITION ft_part_1_2 FOR VALUES FROM (100) TO (200);
--dg-cascade
--dg CREATE UNIQUE INDEX ON lt1 (a);
--dg-cascade
--dg ALTER TABLE lt1 ADD PRIMARY KEY (a);
--dg-cascade
--dg DROP FOREIGN TABLE ft_part_1_1, ft_part_1_2;
--dg-cascade
--dg CREATE UNIQUE INDEX ON lt1 (a);
--dg-cascade
--dg ALTER TABLE lt1 ADD PRIMARY KEY (a);
--dg-cascade
--dg CREATE FOREIGN TABLE ft_part_1_1
--dg   PARTITION OF lt1_part1 FOR VALUES FROM (0) TO (100) SERVER s0;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE ft_part_1_2 (a INT NOT NULL) SERVER s0;
--dg-cascade
--dg ALTER TABLE lt1_part1 ATTACH PARTITION ft_part_1_2 FOR VALUES FROM (100) TO (200);
--dg-cascade
--dg DROP TABLE lt1;
--dg-cascade
--dg DROP FOREIGN TABLE ft_part_1_2;

-- ALTER FOREIGN TABLE
--dg-cascade
--dg COMMENT ON FOREIGN TABLE ft1 IS 'foreign table';
--dg-cascade
--dg COMMENT ON FOREIGN TABLE ft1 IS NULL;
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN ft1.c1 IS 'foreign column';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN ft1.c1 IS NULL;

--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD COLUMN c4 integer;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD COLUMN c5 integer DEFAULT 0;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD COLUMN c6 integer;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD COLUMN IF NOT EXISTS c6 integer;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD COLUMN c7 integer NOT NULL;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD COLUMN c8 integer;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD COLUMN c9 integer;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD COLUMN c10 integer OPTIONS (p1 'v1');
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD c11 integer;

--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c4 SET DEFAULT 0;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c5 DROP DEFAULT;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c6 SET NOT NULL;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c7 DROP NOT NULL;
--dg-cascade
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 TYPE char(10) USING '0'; -- ERROR
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 TYPE char(10);
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 SET DATA TYPE text;
--dg-cascade
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN xmin OPTIONS (ADD p1 'v1'); -- ERROR
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c7 OPTIONS (ADD p1 'v1', ADD p2 'v2'),
--dg                         ALTER COLUMN c8 OPTIONS (ADD p1 'v1', ADD p2 'v2');
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 OPTIONS (SET p2 'V2', DROP p1);
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c1 SET STATISTICS 10000;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c1 SET (n_distinct = 100);
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 SET STATISTICS -1;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 SET STORAGE PLAIN;
--dg-cascade
--dg \d+ ft1
-- can't change the column type if it's used elsewhere
--dg-cascade
--dg CREATE TABLE use_ft1_column_type (x ft1);
--dg-cascade
--dg ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 SET DATA TYPE integer;	-- ERROR
--dg-cascade
--dg DROP TABLE use_ft1_column_type;
--dg-cascade
--dg ALTER FOREIGN TABLE ft1 ADD PRIMARY KEY (c7);                   -- ERROR
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD CONSTRAINT ft1_c9_check CHECK (c9 < 0) NOT VALID;
--dg-cascade
--dg ALTER FOREIGN TABLE ft1 ALTER CONSTRAINT ft1_c9_check DEFERRABLE; -- ERROR
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 DROP CONSTRAINT ft1_c9_check;
--dg-cascade
--dg ALTER FOREIGN TABLE ft1 DROP CONSTRAINT no_const;               -- ERROR
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 DROP CONSTRAINT IF EXISTS no_const;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 OWNER TO regress_test_role;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 OPTIONS (DROP delimiter, SET quote '~', ADD escape '@');
--dg-cascade
--dg ALTER FOREIGN TABLE ft1 DROP COLUMN no_column;                  -- ERROR
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 DROP COLUMN IF EXISTS no_column;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 DROP COLUMN c9;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 DROP c11;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 ADD COLUMN c11 serial;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 SET SCHEMA foreign_schema;
--dg-differs: another error: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft1 SET TABLESPACE ts;                      -- ERROR
--dg-cascade
--dg ALTER SEQUENCE foreign_schema.ft1_c11_seq SET SCHEMA public;    -- ERROR
--dg-skip: ALTER TABLE ... RENAME COLUMN is not supported yet
--dg ALTER FOREIGN TABLE foreign_schema.ft1 RENAME c1 TO foreign_column_1;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE foreign_schema.ft1 RENAME TO foreign_table_1;
--dg-cascade
--dg \d foreign_schema.foreign_table_1

-- alter noexisting table
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ADD COLUMN c4 integer;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ADD COLUMN c6 integer;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ADD COLUMN IF NOT EXISTS c6 integer;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ADD COLUMN c7 integer NOT NULL;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ADD COLUMN c8 integer;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ADD COLUMN c9 integer;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ADD COLUMN c10 integer OPTIONS (p1 'v1');
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ADD c11 integer;

--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ALTER COLUMN c6 SET NOT NULL;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ALTER COLUMN c7 DROP NOT NULL;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ALTER COLUMN c8 TYPE char(10);
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ALTER COLUMN c8 SET DATA TYPE text;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ALTER COLUMN c7 OPTIONS (ADD p1 'v1', ADD p2 'v2'),
--dg                         ALTER COLUMN c8 OPTIONS (ADD p1 'v1', ADD p2 'v2');
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 ALTER COLUMN c8 OPTIONS (SET p2 'V2', DROP p1);

--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 DROP CONSTRAINT IF EXISTS no_const;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 DROP CONSTRAINT ft1_c1_check;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 OWNER TO regress_test_role;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 OPTIONS (DROP delimiter, SET quote '~', ADD escape '@');
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 DROP COLUMN no_column;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 DROP COLUMN IF EXISTS no_column;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 DROP COLUMN c9;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 DROP c11;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 SET SCHEMA foreign_schema;
--dg-skip: ALTER TABLE ... RENAME COLUMN is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 RENAME c1 TO foreign_column_1;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE IF EXISTS doesnt_exist_ft1 RENAME TO foreign_table_1;

-- Information schema

--dg-cascade
--dg SELECT * FROM information_schema.foreign_data_wrappers ORDER BY 1, 2;
--dg-cascade
--dg SELECT * FROM information_schema.foreign_data_wrapper_options ORDER BY 1, 2, 3;
--dg-cascade
--dg SELECT * FROM information_schema.foreign_servers ORDER BY 1, 2;
--dg-cascade
--dg SELECT * FROM information_schema.foreign_server_options ORDER BY 1, 2, 3;
--dg-cascade
--dg SELECT * FROM information_schema.user_mappings ORDER BY lower(authorization_identifier), 2, 3;
--dg-cascade
--dg SELECT * FROM information_schema.user_mapping_options ORDER BY lower(authorization_identifier), 2, 3, 4;
--dg-cascade
--dg SELECT * FROM information_schema.usage_privileges WHERE object_type LIKE 'FOREIGN%' AND object_name IN ('s6', 'foo') ORDER BY 1, 2, 3, 4, 5;
--dg-cascade
--dg SELECT * FROM information_schema.role_usage_grants WHERE object_type LIKE 'FOREIGN%' AND object_name IN ('s6', 'foo') ORDER BY 1, 2, 3, 4, 5;
--dg-cascade
--dg SELECT * FROM information_schema.foreign_tables ORDER BY 1, 2, 3;
--dg-cascade
--dg SELECT * FROM information_schema.foreign_table_options ORDER BY 1, 2, 3, 4;
SET ROLE regress_test_role;
--dg-cascade
--dg SELECT * FROM information_schema.user_mapping_options ORDER BY 1, 2, 3, 4;
--dg-cascade
--dg SELECT * FROM information_schema.usage_privileges WHERE object_type LIKE 'FOREIGN%' AND object_name IN ('s6', 'foo') ORDER BY 1, 2, 3, 4, 5;
--dg-cascade
--dg SELECT * FROM information_schema.role_usage_grants WHERE object_type LIKE 'FOREIGN%' AND object_name IN ('s6', 'foo') ORDER BY 1, 2, 3, 4, 5;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP USER MAPPING FOR current_user SERVER t1;
SET ROLE regress_test_role2;
--dg-cascade
--dg SELECT * FROM information_schema.user_mapping_options ORDER BY 1, 2, 3, 4;
RESET ROLE;


-- has_foreign_data_wrapper_privilege
--dg-cascade
--dg SELECT has_foreign_data_wrapper_privilege('regress_test_role',
--dg     (SELECT oid FROM pg_foreign_data_wrapper WHERE fdwname='foo'), 'USAGE');
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg SELECT has_foreign_data_wrapper_privilege('regress_test_role', 'foo', 'USAGE');
--dg-cascade
--dg SELECT has_foreign_data_wrapper_privilege(
--dg     (SELECT oid FROM pg_roles WHERE rolname='regress_test_role'),
--dg     (SELECT oid FROM pg_foreign_data_wrapper WHERE fdwname='foo'), 'USAGE');
--dg-cascade
--dg SELECT has_foreign_data_wrapper_privilege(
--dg     (SELECT oid FROM pg_foreign_data_wrapper WHERE fdwname='foo'), 'USAGE');
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg SELECT has_foreign_data_wrapper_privilege(
--dg     (SELECT oid FROM pg_roles WHERE rolname='regress_test_role'), 'foo', 'USAGE');
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg SELECT has_foreign_data_wrapper_privilege('foo', 'USAGE');
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_role;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg SELECT has_foreign_data_wrapper_privilege('regress_test_role', 'foo', 'USAGE');

-- has_server_privilege
--dg-cascade
--dg SELECT has_server_privilege('regress_test_role',
--dg     (SELECT oid FROM pg_foreign_server WHERE srvname='s8'), 'USAGE');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg SELECT has_server_privilege('regress_test_role', 's8', 'USAGE');
--dg-cascade
--dg SELECT has_server_privilege(
--dg     (SELECT oid FROM pg_roles WHERE rolname='regress_test_role'),
--dg     (SELECT oid FROM pg_foreign_server WHERE srvname='s8'), 'USAGE');
--dg-cascade
--dg SELECT has_server_privilege(
--dg     (SELECT oid FROM pg_foreign_server WHERE srvname='s8'), 'USAGE');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg SELECT has_server_privilege(
--dg     (SELECT oid FROM pg_roles WHERE rolname='regress_test_role'), 's8', 'USAGE');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg SELECT has_server_privilege('s8', 'USAGE');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN SERVER s8 TO regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg SELECT has_server_privilege('regress_test_role', 's8', 'USAGE');
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg REVOKE USAGE ON FOREIGN SERVER s8 FROM regress_test_role;

--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN SERVER s4 TO regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP USER MAPPING FOR public SERVER s4;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s6 OPTIONS (DROP host, DROP dbname);
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg ALTER USER MAPPING FOR regress_test_role SERVER s6 OPTIONS (DROP username);
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo VALIDATOR postgresql_fdw_validator;

-- Privileges
SET ROLE regress_unprivileged_role;
--dg-differs: another error: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER foobar;                             -- ERROR
--dg-differs: another error: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (gotcha 'true');         -- ERROR
--dg-cascade
--dg ALTER FOREIGN DATA WRAPPER foo OWNER TO regress_unprivileged_role; -- ERROR
--dg-cascade
--dg DROP FOREIGN DATA WRAPPER foo;                                  -- ERROR
--dg-cascade
--dg GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_role;   -- ERROR
--dg-cascade
--dg CREATE SERVER s9 FOREIGN DATA WRAPPER foo;                      -- ERROR
--dg-cascade
--dg ALTER SERVER s4 VERSION '0.5';                                  -- ERROR
--dg-cascade
--dg ALTER SERVER s4 OWNER TO regress_unprivileged_role;             -- ERROR
--dg-cascade
--dg DROP SERVER s4;                                                 -- ERROR
--dg-cascade
--dg GRANT USAGE ON FOREIGN SERVER s4 TO regress_test_role;          -- ERROR
--dg-cascade
--dg CREATE USER MAPPING FOR public SERVER s4;                       -- ERROR
--dg-cascade
--dg ALTER USER MAPPING FOR regress_test_role SERVER s6 OPTIONS (gotcha 'true'); -- ERROR
--dg-cascade
--dg DROP USER MAPPING FOR regress_test_role SERVER s6;              -- ERROR
RESET ROLE;

--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN DATA WRAPPER postgresql TO regress_unprivileged_role;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_unprivileged_role WITH GRANT OPTION;
SET ROLE regress_unprivileged_role;
--dg-differs: another error: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER foobar;                             -- ERROR
--dg-differs: another error: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg ALTER FOREIGN DATA WRAPPER foo OPTIONS (gotcha 'true');         -- ERROR
--dg-cascade
--dg DROP FOREIGN DATA WRAPPER foo;                                  -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN DATA WRAPPER postgresql TO regress_test_role; -- WARNING
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s9 FOREIGN DATA WRAPPER postgresql;
--dg-cascade
--dg ALTER SERVER s6 VERSION '0.5';                                  -- ERROR
--dg-cascade
--dg DROP SERVER s6;                                                 -- ERROR
--dg-cascade
--dg GRANT USAGE ON FOREIGN SERVER s6 TO regress_test_role;          -- ERROR
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN SERVER s9 TO regress_test_role;
--dg-cascade
--dg CREATE USER MAPPING FOR public SERVER s6;                       -- ERROR
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR public SERVER s9;
--dg-cascade
--dg ALTER USER MAPPING FOR regress_test_role SERVER s6 OPTIONS (gotcha 'true'); -- ERROR
--dg-cascade
--dg DROP USER MAPPING FOR regress_test_role SERVER s6;              -- ERROR
RESET ROLE;

--dg-cascade
--dg REVOKE USAGE ON FOREIGN DATA WRAPPER foo FROM regress_unprivileged_role; -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg REVOKE USAGE ON FOREIGN DATA WRAPPER foo FROM regress_unprivileged_role CASCADE;
SET ROLE regress_unprivileged_role;
--dg-cascade
--dg GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_role;   -- ERROR
--dg-cascade
--dg CREATE SERVER s10 FOREIGN DATA WRAPPER foo;                     -- ERROR
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg ALTER SERVER s9 VERSION '1.1';
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN SERVER s9 TO regress_test_role;
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR current_user SERVER s9;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP SERVER s9 CASCADE;
RESET ROLE;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s9 FOREIGN DATA WRAPPER foo;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN SERVER s9 TO regress_unprivileged_role;
SET ROLE regress_unprivileged_role;
--dg-cascade
--dg ALTER SERVER s9 VERSION '1.2';                                  -- ERROR
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg GRANT USAGE ON FOREIGN SERVER s9 TO regress_test_role;          -- WARNING
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR current_user SERVER s9;
--dg-cascade
--dg DROP SERVER s9 CASCADE;                                         -- ERROR

-- Check visibility of user mapping data
SET ROLE regress_test_role;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER s10 FOREIGN DATA WRAPPER foo;
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR public SERVER s10 OPTIONS (user 'secret');
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR regress_unprivileged_role SERVER s10 OPTIONS (user 'secret');
-- owner of server can see some option fields
--dg-cascade
--dg \deu+
RESET ROLE;
-- superuser can see all option fields
--dg-cascade
--dg \deu+
-- unprivileged user cannot see any option field
SET ROLE regress_unprivileged_role;
--dg-cascade
--dg \deu+
RESET ROLE;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP SERVER s10 CASCADE;

-- Triggers
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION dummy_trigger() RETURNS TRIGGER AS $$
--dg   BEGIN
--dg     RETURN NULL;
--dg   END
--dg $$ language plpgsql;

--dg-cascade
--dg CREATE TRIGGER trigtest_before_stmt BEFORE INSERT OR UPDATE OR DELETE
--dg ON foreign_schema.foreign_table_1
--dg FOR EACH STATEMENT
--dg EXECUTE PROCEDURE dummy_trigger();

--dg-cascade
--dg CREATE TRIGGER trigtest_after_stmt AFTER INSERT OR UPDATE OR DELETE
--dg ON foreign_schema.foreign_table_1
--dg FOR EACH STATEMENT
--dg EXECUTE PROCEDURE dummy_trigger();

--dg-cascade
--dg CREATE TRIGGER trigtest_after_stmt_tt AFTER INSERT OR UPDATE OR DELETE -- ERROR
--dg ON foreign_schema.foreign_table_1
--dg REFERENCING NEW TABLE AS new_table
--dg FOR EACH STATEMENT
--dg EXECUTE PROCEDURE dummy_trigger();

--dg-cascade
--dg CREATE TRIGGER trigtest_before_row BEFORE INSERT OR UPDATE OR DELETE
--dg ON foreign_schema.foreign_table_1
--dg FOR EACH ROW
--dg EXECUTE PROCEDURE dummy_trigger();

--dg-cascade
--dg CREATE TRIGGER trigtest_after_row AFTER INSERT OR UPDATE OR DELETE
--dg ON foreign_schema.foreign_table_1
--dg FOR EACH ROW
--dg EXECUTE PROCEDURE dummy_trigger();

--dg-cascade
--dg CREATE CONSTRAINT TRIGGER trigtest_constraint AFTER INSERT OR UPDATE OR DELETE
--dg ON foreign_schema.foreign_table_1
--dg FOR EACH ROW
--dg EXECUTE PROCEDURE dummy_trigger();

--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE foreign_schema.foreign_table_1
--dg 	DISABLE TRIGGER trigtest_before_stmt;
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE foreign_schema.foreign_table_1
--dg 	ENABLE TRIGGER trigtest_before_stmt;

--dg-cascade
--dg DROP TRIGGER trigtest_before_stmt ON foreign_schema.foreign_table_1;
--dg-cascade
--dg DROP TRIGGER trigtest_before_row ON foreign_schema.foreign_table_1;
--dg-cascade
--dg DROP TRIGGER trigtest_after_stmt ON foreign_schema.foreign_table_1;
--dg-cascade
--dg DROP TRIGGER trigtest_after_row ON foreign_schema.foreign_table_1;

--dg-cascade
--dg DROP FUNCTION dummy_trigger();

-- Table inheritance
CREATE TABLE fd_pt1 (
	c1 integer NOT NULL,
	c2 text,
	c3 date
);
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE ft2 () INHERITS (fd_pt1)
--dg   SERVER s0 OPTIONS (delimiter ',', quote '"', "be quoted" 'value');
--dg-cascade
--dg \d+ fd_pt1
--dg-cascade
--dg \d+ ft2
--dg-cascade
--dg DROP FOREIGN TABLE ft2;
--dg-differs: the result
--dg \d+ fd_pt1
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE ft2 (
--dg 	c1 integer NOT NULL,
--dg 	c2 text,
--dg 	c3 date
--dg ) SERVER s0 OPTIONS (delimiter ',', quote '"', "be quoted" 'value');
--dg-cascade
--dg \d+ ft2
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft2 INHERIT fd_pt1;
--dg-cascade
--dg \d+ fd_pt1
--dg-cascade
--dg \d+ ft2
--dg-cascade
--dg CREATE TABLE ct3() INHERITS(ft2);
--dg-cascade
--dg CREATE FOREIGN TABLE ft3 (
--dg 	c1 integer NOT NULL,
--dg 	c2 text,
--dg 	c3 date
--dg ) INHERITS(ft2)
--dg   SERVER s0;
--dg-cascade
--dg \d+ ft2
--dg-cascade
--dg \d+ ct3
--dg-cascade
--dg \d+ ft3

-- add attributes recursively
ALTER TABLE fd_pt1 ADD COLUMN c4 integer;
ALTER TABLE fd_pt1 ADD COLUMN c5 integer DEFAULT 0;
ALTER TABLE fd_pt1 ADD COLUMN c6 integer;
ALTER TABLE fd_pt1 ADD COLUMN c7 integer NOT NULL;
ALTER TABLE fd_pt1 ADD COLUMN c8 integer;
--dg-cascade
--dg \d+ fd_pt1
--dg-cascade
--dg \d+ ft2
--dg-cascade
--dg \d+ ct3
--dg-cascade
--dg \d+ ft3

-- alter attributes recursively
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE fd_pt1 ALTER COLUMN c4 SET DEFAULT 0;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE fd_pt1 ALTER COLUMN c5 DROP DEFAULT;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE fd_pt1 ALTER COLUMN c6 SET NOT NULL;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE fd_pt1 ALTER COLUMN c7 DROP NOT NULL;
--dg-cascade
--dg ALTER TABLE fd_pt1 ALTER COLUMN c8 TYPE char(10) USING '0';        -- ERROR
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE fd_pt1 ALTER COLUMN c8 TYPE char(10);
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE fd_pt1 ALTER COLUMN c8 SET DATA TYPE text;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE fd_pt1 ALTER COLUMN c1 SET STATISTICS 10000;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE fd_pt1 ALTER COLUMN c1 SET (n_distinct = 100);
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE fd_pt1 ALTER COLUMN c8 SET STATISTICS -1;
--dg-cascade
--dg ALTER TABLE fd_pt1 ALTER COLUMN c8 SET STORAGE EXTERNAL;
--dg-cascade
--dg \d+ fd_pt1
--dg-cascade
--dg \d+ ft2

-- drop attributes recursively
ALTER TABLE fd_pt1 DROP COLUMN c4;
ALTER TABLE fd_pt1 DROP COLUMN c5;
ALTER TABLE fd_pt1 DROP COLUMN c6;
ALTER TABLE fd_pt1 DROP COLUMN c7;
ALTER TABLE fd_pt1 DROP COLUMN c8;
--dg-cascade
--dg \d+ fd_pt1
--dg-cascade
--dg \d+ ft2

-- add constraints recursively
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY yet
--dg ALTER TABLE fd_pt1 ADD CONSTRAINT fd_pt1chk1 CHECK (c1 > 0) NO INHERIT;
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY yet
--dg ALTER TABLE fd_pt1 ADD CONSTRAINT fd_pt1chk2 CHECK (c2 <> '');
-- connoinherit should be true for NO INHERIT constraint
--dg-cascade
--dg SELECT relname, conname, contype, conislocal, coninhcount, connoinherit
--dg   FROM pg_class AS pc JOIN pg_constraint AS pgc ON (conrelid = pc.oid)
--dg   WHERE pc.relname = 'fd_pt1'
--dg   ORDER BY 1,2;
-- child does not inherit NO INHERIT constraints
--dg-cascade
--dg \d+ fd_pt1
--dg-cascade
--dg \d+ ft2
--dg-cascade
--dg DROP FOREIGN TABLE ft2; -- ERROR
--dg-cascade
--dg DROP FOREIGN TABLE ft2 CASCADE;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE ft2 (
--dg 	c1 integer NOT NULL,
--dg 	c2 text,
--dg 	c3 date
--dg ) SERVER s0 OPTIONS (delimiter ',', quote '"', "be quoted" 'value');
-- child must have parent's INHERIT constraints
--dg-cascade
--dg ALTER FOREIGN TABLE ft2 INHERIT fd_pt1;                            -- ERROR
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft2 ADD CONSTRAINT fd_pt1chk2 CHECK (c2 <> '');
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE ft2 INHERIT fd_pt1;
-- child does not inherit NO INHERIT constraints
--dg-cascade
--dg \d+ fd_pt1
--dg-cascade
--dg \d+ ft2

-- drop constraints recursively
--dg-cascade
--dg ALTER TABLE fd_pt1 DROP CONSTRAINT fd_pt1chk1 CASCADE;
--dg-cascade
--dg ALTER TABLE fd_pt1 DROP CONSTRAINT fd_pt1chk2 CASCADE;

-- NOT VALID case
INSERT INTO fd_pt1 VALUES (1, 'fd_pt1'::text, '1994-01-01'::date);
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY yet
--dg ALTER TABLE fd_pt1 ADD CONSTRAINT fd_pt1chk3 CHECK (c2 <> '') NOT VALID;
--dg-cascade
--dg \d+ fd_pt1
--dg-cascade
--dg \d+ ft2
-- VALIDATE CONSTRAINT need do nothing on foreign tables
--dg-cascade
--dg ALTER TABLE fd_pt1 VALIDATE CONSTRAINT fd_pt1chk3;
--dg-cascade
--dg \d+ fd_pt1
--dg-cascade
--dg \d+ ft2

-- changes name of an attribute recursively
ALTER TABLE fd_pt1 RENAME COLUMN c1 TO f1;
ALTER TABLE fd_pt1 RENAME COLUMN c2 TO f2;
ALTER TABLE fd_pt1 RENAME COLUMN c3 TO f3;
-- changes name of a constraint recursively
--dg-cascade
--dg ALTER TABLE fd_pt1 RENAME CONSTRAINT fd_pt1chk3 TO f2_check;
--dg-cascade
--dg \d+ fd_pt1
--dg-cascade
--dg \d+ ft2

--dg-cascade
--dg DROP TABLE fd_pt1 CASCADE;

-- IMPORT FOREIGN SCHEMA
--dg-cascade
--dg IMPORT FOREIGN SCHEMA s1 FROM SERVER s9 INTO public; -- ERROR
--dg-cascade
--dg IMPORT FOREIGN SCHEMA s1 LIMIT TO (t1) FROM SERVER s9 INTO public; --ERROR
--dg-cascade
--dg IMPORT FOREIGN SCHEMA s1 EXCEPT (t1) FROM SERVER s9 INTO public; -- ERROR
--dg-cascade
--dg IMPORT FOREIGN SCHEMA s1 EXCEPT (t1, t2) FROM SERVER s9 INTO public
--dg OPTIONS (option1 'value1', option2 'value2'); -- ERROR

-- DROP FOREIGN TABLE
DROP FOREIGN TABLE no_table;                                    -- ERROR
DROP FOREIGN TABLE IF EXISTS no_table;
--dg-cascade
--dg DROP FOREIGN TABLE foreign_schema.foreign_table_1;

-- REASSIGN OWNED/DROP OWNED of foreign objects
REASSIGN OWNED BY regress_test_role TO regress_test_role2;
--dg-cascade
--dg DROP OWNED BY regress_test_role2;
--dg-cascade
--dg DROP OWNED BY regress_test_role2 CASCADE;

-- Foreign partition DDL stuff
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE fd_pt2 (
--dg 	c1 integer NOT NULL,
--dg 	c2 text,
--dg 	c3 date
--dg ) PARTITION BY LIST (c1);
--dg-cascade
--dg CREATE FOREIGN TABLE fd_pt2_1 PARTITION OF fd_pt2 FOR VALUES IN (1)
--dg   SERVER s0 OPTIONS (delimiter ',', quote '"', "be quoted" 'value');
--dg-cascade
--dg \d+ fd_pt2
--dg-cascade
--dg \d+ fd_pt2_1

-- partition cannot have additional columns
--dg-cascade
--dg DROP FOREIGN TABLE fd_pt2_1;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE fd_pt2_1 (
--dg 	c1 integer NOT NULL,
--dg 	c2 text,
--dg 	c3 date,
--dg 	c4 char
--dg ) SERVER s0 OPTIONS (delimiter ',', quote '"', "be quoted" 'value');
--dg-cascade
--dg \d+ fd_pt2_1
--dg-cascade
--dg ALTER TABLE fd_pt2 ATTACH PARTITION fd_pt2_1 FOR VALUES IN (1);       -- ERROR

--dg-cascade
--dg DROP FOREIGN TABLE fd_pt2_1;
--dg-cascade
--dg \d+ fd_pt2
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE fd_pt2_1 (
--dg 	c1 integer NOT NULL,
--dg 	c2 text,
--dg 	c3 date
--dg ) SERVER s0 OPTIONS (delimiter ',', quote '"', "be quoted" 'value');
--dg-cascade
--dg \d+ fd_pt2_1
-- no attach partition validation occurs for foreign tables
--dg-cascade
--dg ALTER TABLE fd_pt2 ATTACH PARTITION fd_pt2_1 FOR VALUES IN (1);
--dg-cascade
--dg \d+ fd_pt2
--dg-cascade
--dg \d+ fd_pt2_1

-- cannot add column to a partition
--dg-cascade
--dg ALTER TABLE fd_pt2_1 ADD c4 char;

-- ok to have a partition's own constraints though
--dg-cascade
--dg ALTER TABLE fd_pt2_1 ALTER c3 SET NOT NULL;
--dg-cascade
--dg ALTER TABLE fd_pt2_1 ADD CONSTRAINT p21chk CHECK (c2 <> '');
--dg-cascade
--dg \d+ fd_pt2
--dg-cascade
--dg \d+ fd_pt2_1

-- cannot drop inherited NOT NULL constraint from a partition
--dg-cascade
--dg ALTER TABLE fd_pt2_1 ALTER c1 DROP NOT NULL;

-- partition must have parent's constraints
--dg-cascade
--dg ALTER TABLE fd_pt2 DETACH PARTITION fd_pt2_1;
--dg-cascade
--dg ALTER TABLE fd_pt2 ALTER c2 SET NOT NULL;
--dg-cascade
--dg \d+ fd_pt2
--dg-cascade
--dg \d+ fd_pt2_1
--dg-cascade
--dg ALTER TABLE fd_pt2 ATTACH PARTITION fd_pt2_1 FOR VALUES IN (1);       -- ERROR
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE fd_pt2_1 ALTER c2 SET NOT NULL;
--dg-cascade
--dg ALTER TABLE fd_pt2 ATTACH PARTITION fd_pt2_1 FOR VALUES IN (1);

--dg-cascade
--dg ALTER TABLE fd_pt2 DETACH PARTITION fd_pt2_1;
--dg-cascade
--dg ALTER TABLE fd_pt2 ADD CONSTRAINT fd_pt2chk1 CHECK (c1 > 0);
--dg-cascade
--dg \d+ fd_pt2
--dg-cascade
--dg \d+ fd_pt2_1
--dg-cascade
--dg ALTER TABLE fd_pt2 ATTACH PARTITION fd_pt2_1 FOR VALUES IN (1);       -- ERROR
--dg-skip: ALTER FOREIGN TABLE is not supported yet
--dg ALTER FOREIGN TABLE fd_pt2_1 ADD CONSTRAINT fd_pt2chk1 CHECK (c1 > 0);
--dg-cascade
--dg ALTER TABLE fd_pt2 ATTACH PARTITION fd_pt2_1 FOR VALUES IN (1);

--dg-cascade
--dg DROP FOREIGN TABLE fd_pt2_1;
--dg-cascade
--dg DROP TABLE fd_pt2;

-- foreign table cannot be part of partition tree made of temporary
-- relations.
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TEMP TABLE temp_parted (a int) PARTITION BY LIST (a);
--dg-cascade
--dg CREATE FOREIGN TABLE foreign_part PARTITION OF temp_parted DEFAULT
--dg   SERVER s0;  -- ERROR
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE foreign_part (a int) SERVER s0;
--dg-cascade
--dg ALTER TABLE temp_parted ATTACH PARTITION foreign_part DEFAULT;  -- ERROR
--dg-cascade
--dg DROP FOREIGN TABLE foreign_part;
--dg-cascade
--dg DROP TABLE temp_parted;

-- Cleanup
DROP SCHEMA foreign_schema CASCADE;
--dg-cascade
--dg DROP ROLE regress_test_role;                                -- ERROR
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP SERVER t1 CASCADE;
--dg-cascade
--dg DROP USER MAPPING FOR regress_test_role SERVER s6;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER foo CASCADE;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg DROP SERVER s8 CASCADE;
--dg-cascade
--dg DROP ROLE regress_test_indirect;
--dg-cascade
--dg DROP ROLE regress_test_role;
--dg-cascade
--dg DROP ROLE regress_unprivileged_role;                        -- ERROR
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg REVOKE ALL ON FOREIGN DATA WRAPPER postgresql FROM regress_unprivileged_role;
--dg-cascade
--dg DROP ROLE regress_unprivileged_role;
DROP ROLE regress_test_role2;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER postgresql CASCADE;
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER dummy CASCADE;
\c
--dg-cascade
--dg DROP ROLE regress_foreign_data_user;

-- At this point we should have no wrappers, no servers, and no mappings.
--dg-skip: relation "?" does not exist
--dg SELECT fdwname, fdwhandler, fdwvalidator, fdwoptions FROM pg_foreign_data_wrapper;
--dg-skip: relation "?" does not exist
--dg SELECT srvname, srvoptions FROM pg_foreign_server;
--dg-skip: relation "?" does not exist
--dg SELECT * FROM pg_user_mapping;
