--
-- regproc
--

/* If objects exist, return oids */

--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_regrole_test;

-- without schemaname

SELECT regoper('||/');
SELECT regoperator('+(int4,int4)');
SELECT regproc('now');
SELECT regprocedure('abs(numeric)');
SELECT regclass('pg_class');
SELECT regtype('int4');
SELECT regcollation('"POSIX"');

SELECT to_regoper('||/');
SELECT to_regoperator('+(int4,int4)');
SELECT to_regproc('now');
SELECT to_regprocedure('abs(numeric)');
SELECT to_regclass('pg_class');
SELECT to_regtype('int4');
SELECT to_regcollation('"POSIX"');

-- with schemaname

SELECT regoper('pg_catalog.||/');
SELECT regoperator('pg_catalog.+(int4,int4)');
SELECT regproc('pg_catalog.now');
SELECT regprocedure('pg_catalog.abs(numeric)');
SELECT regclass('pg_catalog.pg_class');
SELECT regtype('pg_catalog.int4');
SELECT regcollation('pg_catalog."POSIX"');

SELECT to_regoper('pg_catalog.||/');
SELECT to_regproc('pg_catalog.now');
SELECT to_regprocedure('pg_catalog.abs(numeric)');
SELECT to_regclass('pg_catalog.pg_class');
SELECT to_regtype('pg_catalog.int4');
SELECT to_regcollation('pg_catalog."POSIX"');

-- schemaname not applicable

--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT regrole('regress_regrole_test');
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT regrole('"regress_regrole_test"');
SELECT regnamespace('pg_catalog');
SELECT regnamespace('"pg_catalog"');

--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT to_regrole('regress_regrole_test');
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT to_regrole('"regress_regrole_test"');
SELECT to_regnamespace('pg_catalog');
SELECT to_regnamespace('"pg_catalog"');

/* If objects don't exist, raise errors. */

--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_regrole_test;

-- without schemaname

--dg-cascade
--dg SELECT regoper('||//');
--dg-cascade
--dg SELECT regoperator('++(int4,int4)');
--dg-cascade
--dg SELECT regproc('know');
--dg-cascade
--dg SELECT regprocedure('absinthe(numeric)');
--dg-cascade
--dg SELECT regclass('pg_classes');
--dg-cascade
--dg SELECT regtype('int3');

-- with schemaname

--dg-cascade
--dg SELECT regoper('ng_catalog.||/');
--dg-cascade
--dg SELECT regoperator('ng_catalog.+(int4,int4)');
--dg-cascade
--dg SELECT regproc('ng_catalog.now');
--dg-cascade
--dg SELECT regprocedure('ng_catalog.abs(numeric)');
--dg-cascade
--dg SELECT regclass('ng_catalog.pg_class');
--dg-cascade
--dg SELECT regtype('ng_catalog.int4');
\set VERBOSITY sqlstate \\ -- error message is encoding-dependent
SELECT regcollation('ng_catalog."POSIX"');
\set VERBOSITY default

-- schemaname not applicable

--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT regrole('regress_regrole_test');
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT regrole('"regress_regrole_test"');
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT regrole('Nonexistent');
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT regrole('"Nonexistent"');
SELECT regrole('foo.bar');
SELECT regnamespace('Nonexistent');
SELECT regnamespace('"Nonexistent"');
SELECT regnamespace('foo.bar');

/* If objects don't exist, return NULL with no error. */

-- without schemaname

SELECT to_regoper('||//');
SELECT to_regoperator('++(int4,int4)');
SELECT to_regproc('know');
SELECT to_regprocedure('absinthe(numeric)');
SELECT to_regclass('pg_classes');
SELECT to_regtype('int3');
SELECT to_regcollation('notacollation');

-- with schemaname

SELECT to_regoper('ng_catalog.||/');
SELECT to_regoperator('ng_catalog.+(int4,int4)');
SELECT to_regproc('ng_catalog.now');
SELECT to_regprocedure('ng_catalog.abs(numeric)');
SELECT to_regclass('ng_catalog.pg_class');
SELECT to_regtype('ng_catalog.int4');
SELECT to_regcollation('ng_catalog."POSIX"');

-- schemaname not applicable

--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT to_regrole('regress_regrole_test');
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT to_regrole('"regress_regrole_test"');
SELECT to_regrole('foo.bar');
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT to_regrole('Nonexistent');
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT to_regrole('"Nonexistent"');
SELECT to_regrole('foo.bar');
SELECT to_regnamespace('Nonexistent');
SELECT to_regnamespace('"Nonexistent"');
SELECT to_regnamespace('foo.bar');

-- Test to_regtypemod
SELECT to_regtypemod('text');
SELECT to_regtypemod('timestamp(4)');
SELECT to_regtypemod('no_such_type(4)');
SELECT format_type(to_regtype('varchar(32)'), to_regtypemod('varchar(32)'));
SELECT format_type(to_regtype('bit'), to_regtypemod('bit'));
SELECT format_type(to_regtype('"bit"'), to_regtypemod('"bit"'));

-- Test soft-error API

SELECT * FROM pg_input_error_info('ng_catalog.pg_class', 'regclass');
SELECT pg_input_is_valid('ng_catalog."POSIX"', 'regcollation');
--dg-skip: relation pg_ts_config cannot be opened: digres has no catalog tables
--dg SELECT * FROM pg_input_error_info('no_such_config', 'regconfig');
--dg-skip: relation pg_ts_dict cannot be opened: digres has no catalog tables
--dg SELECT * FROM pg_input_error_info('no_such_dictionary', 'regdictionary');
SELECT * FROM pg_input_error_info('Nonexistent', 'regnamespace');
SELECT * FROM pg_input_error_info('ng_catalog.||/', 'regoper');
SELECT * FROM pg_input_error_info('-', 'regoper');
SELECT * FROM pg_input_error_info('ng_catalog.+(int4,int4)', 'regoperator');
SELECT * FROM pg_input_error_info('-', 'regoperator');
SELECT * FROM pg_input_error_info('ng_catalog.now', 'regproc');
SELECT * FROM pg_input_error_info('ng_catalog.abs(numeric)', 'regprocedure');
SELECT * FROM pg_input_error_info('ng_catalog.abs(numeric', 'regprocedure');
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT * FROM pg_input_error_info('regress_regrole_test', 'regrole');
SELECT * FROM pg_input_error_info('no_such_type', 'regtype');

-- Some cases that should be soft errors, but are not yet
SELECT * FROM pg_input_error_info('incorrect type name syntax', 'regtype');
SELECT * FROM pg_input_error_info('numeric(1,2,3)', 'regtype');  -- bogus typmod
SELECT * FROM pg_input_error_info('way.too.many.names', 'regtype');
SELECT * FROM pg_input_error_info('no_such_catalog.schema.name', 'regtype');
