-- pg_regress should ensure that this default value applies; however
-- we can't rely on any specific default value of vacuum_cost_delay
SHOW datestyle;

-- Check output style of CamelCase enum options
SET intervalstyle to 'asd';

-- SET to some nondefault value
SET vacuum_cost_delay TO 40;
SET datestyle = 'ISO, YMD';
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET LOCAL has no effect outside of a transaction
SET LOCAL vacuum_cost_delay TO 50;
SHOW vacuum_cost_delay;
SET LOCAL datestyle = 'SQL';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET LOCAL within a transaction that commits
BEGIN;
SET LOCAL vacuum_cost_delay TO 50;
SHOW vacuum_cost_delay;
SET LOCAL datestyle = 'SQL';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
COMMIT;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET should be reverted after ROLLBACK
BEGIN;
SET vacuum_cost_delay TO 60;
SHOW vacuum_cost_delay;
SET datestyle = 'German';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- Some tests with subtransactions
BEGIN;
SET vacuum_cost_delay TO 70;
SET datestyle = 'MDY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
SAVEPOINT first_sp;
SET vacuum_cost_delay TO 80.1;
SHOW vacuum_cost_delay;
SET datestyle = 'German, DMY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK TO first_sp;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
SAVEPOINT second_sp;
SET vacuum_cost_delay TO '900us';
SET datestyle = 'SQL, YMD';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
SAVEPOINT third_sp;
SET vacuum_cost_delay TO 100;
SHOW vacuum_cost_delay;
SET datestyle = 'Postgres, MDY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK TO third_sp;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK TO second_sp;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET LOCAL with Savepoints
BEGIN;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
SAVEPOINT sp;
SET LOCAL vacuum_cost_delay TO 30;
SHOW vacuum_cost_delay;
SET LOCAL datestyle = 'Postgres, MDY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK TO sp;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET LOCAL persists through RELEASE (which was not true in 8.0-8.2)
BEGIN;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
SAVEPOINT sp;
SET LOCAL vacuum_cost_delay TO 30;
SHOW vacuum_cost_delay;
SET LOCAL datestyle = 'Postgres, MDY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
RELEASE SAVEPOINT sp;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
ROLLBACK;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- SET followed by SET LOCAL
BEGIN;
SET vacuum_cost_delay TO 40;
SET LOCAL vacuum_cost_delay TO 50;
SHOW vacuum_cost_delay;
SET datestyle = 'ISO, DMY';
SET LOCAL datestyle = 'Postgres, MDY';
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
COMMIT;
SHOW vacuum_cost_delay;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

--
-- Test RESET.  We use datestyle because the reset value is forced by
-- pg_regress, so it doesn't depend on the installation's configuration.
--
SET datestyle = iso, ymd;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;
RESET datestyle;
SHOW datestyle;
SELECT '2006-08-13 12:34:56'::timestamptz;

-- Test some simple error cases
SET seq_page_cost TO 'NaN';
SET vacuum_cost_delay TO '10s';
SET no_such_variable TO 42;

-- Test "custom" GUCs created on the fly (which aren't really an
-- intended feature, but many people use them).
SHOW custom.my_guc;  -- error, not known yet
SET custom.my_guc = 42;
SHOW custom.my_guc;
RESET custom.my_guc;  -- this makes it go to empty, not become unknown again
SHOW custom.my_guc;
SET custom.my.qualified.guc = 'foo';
SHOW custom.my.qualified.guc;
SET custom."bad-guc" = 42;  -- disallowed because -c cannot set this name
SHOW custom."bad-guc";
SET special."weird name" = 'foo';  -- could be allowed, but we choose not to
SHOW special."weird name";

-- Check what happens when you try to set a "custom" GUC within the
-- namespace of an extension.
SET plpgsql.extra_foo_warnings = true;  -- allowed if plpgsql is not loaded yet
--dg-skip: loading library "?" is not supported yet
--dg LOAD 'plpgsql';  -- this will throw a warning and delete the variable
--dg-cascade
--dg SET plpgsql.extra_foo_warnings = true;  -- now, it's an error
--dg-cascade
--dg SHOW plpgsql.extra_foo_warnings;

--
-- Test DISCARD TEMP
--
CREATE TEMP TABLE reset_test ( data text ) ON COMMIT DELETE ROWS;
SELECT relname FROM pg_class WHERE relname = 'reset_test';
DISCARD TEMP;
SELECT relname FROM pg_class WHERE relname = 'reset_test';

--
-- Test DISCARD ALL
--

-- do changes
DECLARE foo CURSOR WITH HOLD FOR SELECT 1;
PREPARE foo AS SELECT 1;
--dg-skip: LISTEN is not supported
--dg LISTEN foo_event;
SET vacuum_cost_delay = 13;
CREATE TEMP TABLE tmp_foo (data text) ON COMMIT DELETE ROWS;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_guc_user;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_guc_user;
-- look changes
--dg-cascade
--dg SELECT pg_listening_channels();
--dg-skip: relation "?" does not exist
--dg SELECT name FROM pg_prepared_statements;
--dg-skip: relation "?" does not exist
--dg SELECT name FROM pg_cursors;
SHOW vacuum_cost_delay;
SELECT relname from pg_class where relname = 'tmp_foo';
--dg-cascade
--dg SELECT current_user = 'regress_guc_user';
-- discard everything
DISCARD ALL;
-- look again
--dg-skip: function ?(...) does not exist
--dg SELECT pg_listening_channels();
--dg-skip: relation "?" does not exist
--dg SELECT name FROM pg_prepared_statements;
--dg-skip: relation "?" does not exist
--dg SELECT name FROM pg_cursors;
SHOW vacuum_cost_delay;
SELECT relname from pg_class where relname = 'tmp_foo';
SELECT current_user = 'regress_guc_user';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_guc_user;

--
-- search_path should react to changes in pg_namespace
--

set search_path = foo, public, not_there_initially;
select current_schemas(false);
create schema not_there_initially;
select current_schemas(false);
drop schema not_there_initially;
select current_schemas(false);
reset search_path;

--
-- Tests for function-local GUC settings
--

set work_mem = '3MB';

--dg-skip: language "?" does not exist
--dg create function report_guc(text) returns text as
--dg $$ select current_setting($1) $$ language sql
--dg set work_mem = '1MB';

--dg-cascade
--dg select report_guc('work_mem'), current_setting('work_mem');

--dg-cascade
--dg alter function report_guc(text) set work_mem = '2MB';

--dg-cascade
--dg select report_guc('work_mem'), current_setting('work_mem');

--dg-cascade
--dg alter function report_guc(text) reset all;

--dg-cascade
--dg select report_guc('work_mem'), current_setting('work_mem');

-- SET LOCAL is restricted by a function SET option
--dg-skip: language "?" does not exist
--dg create or replace function myfunc(int) returns text as $$
--dg begin
--dg   set local work_mem = '2MB';
--dg   return current_setting('work_mem');
--dg end $$
--dg language plpgsql
--dg set work_mem = '1MB';

--dg-cascade
--dg select myfunc(0), current_setting('work_mem');

--dg-cascade
--dg alter function myfunc(int) reset all;

--dg-cascade
--dg select myfunc(0), current_setting('work_mem');

set work_mem = '3MB';

-- but SET isn't
--dg-skip: language "?" does not exist
--dg create or replace function myfunc(int) returns text as $$
--dg begin
--dg   set work_mem = '2MB';
--dg   return current_setting('work_mem');
--dg end $$
--dg language plpgsql
--dg set work_mem = '1MB';

--dg-cascade
--dg select myfunc(0), current_setting('work_mem');

set work_mem = '3MB';

-- it should roll back on error, though
--dg-skip: language "?" does not exist
--dg create or replace function myfunc(int) returns text as $$
--dg begin
--dg   set work_mem = '2MB';
--dg   perform 1/$1;
--dg   return current_setting('work_mem');
--dg end $$
--dg language plpgsql
--dg set work_mem = '1MB';

--dg-cascade
--dg select myfunc(0);
select current_setting('work_mem');
--dg-cascade
--dg select myfunc(1), current_setting('work_mem');

-- check current_setting()'s behavior with invalid setting name

select current_setting('nosuch.setting');  -- FAIL
select current_setting('nosuch.setting', false);  -- FAIL
select current_setting('nosuch.setting', true) is null;

-- after this, all three cases should yield 'nada'
set nosuch.setting = 'nada';

select current_setting('nosuch.setting');
select current_setting('nosuch.setting', false);
select current_setting('nosuch.setting', true);

-- Normally, CREATE FUNCTION should complain about invalid values in
-- function SET options; but not if check_function_bodies is off,
-- because that creates ordering hazards for pg_dump

--dg-differs: another error: unrecognized configuration parameter "?"
--dg create function func_with_bad_set() returns int as $$ select 1 $$
--dg language sql
--dg set default_text_search_config = no_such_config;

set check_function_bodies = off;

--dg-skip: unrecognized configuration parameter "?"
--dg create function func_with_bad_set() returns int as $$ select 1 $$
--dg language sql
--dg set default_text_search_config = no_such_config;

--dg-cascade
--dg select func_with_bad_set();

reset check_function_bodies;

set default_with_oids to f;
-- Should not allow to set it to true.
set default_with_oids to t;

-- Test that disabling track_activities disables query ID reporting in
-- pg_stat_activity.
SET compute_query_id = on;
--dg-skip: unrecognized configuration parameter "?"
--dg SET track_activities = on;
--dg-skip: relation "?" does not exist
--dg SELECT query_id IS NOT NULL AS qid_set FROM pg_stat_activity
--dg   WHERE pid = pg_backend_pid();
--dg-skip: unrecognized configuration parameter "?"
--dg SET track_activities = off;
--dg-cascade
--dg SELECT query_id IS NOT NULL AS qid_set FROM pg_stat_activity
--dg   WHERE pid = pg_backend_pid();
--dg-skip: unrecognized configuration parameter "?"
--dg RESET track_activities;
RESET compute_query_id;

-- Test GUC categories and flag patterns
SELECT pg_settings_get_flags(NULL);
SELECT pg_settings_get_flags('does_not_exist');
CREATE TABLE tab_settings_flags AS SELECT name, category,
    'EXPLAIN'          = ANY(flags) AS explain,
    'NO_RESET'         = ANY(flags) AS no_reset,
    'NO_RESET_ALL'     = ANY(flags) AS no_reset_all,
    'NOT_IN_SAMPLE'    = ANY(flags) AS not_in_sample,
    'RUNTIME_COMPUTED' = ANY(flags) AS runtime_computed
  FROM pg_show_all_settings() AS psas,
    pg_settings_get_flags(psas.name) AS flags;

-- Developer GUCs should be flagged with GUC_NOT_IN_SAMPLE:
SELECT name FROM tab_settings_flags
  WHERE category = 'Developer Options' AND NOT not_in_sample
  ORDER BY 1;
-- Most query-tuning GUCs are flagged as valid for EXPLAIN.
-- default_statistics_target is an exception.
SELECT name FROM tab_settings_flags
  WHERE category ~ '^Query Tuning' AND NOT explain
  ORDER BY 1;
-- Runtime-computed GUCs should be part of the preset category.
SELECT name FROM tab_settings_flags
  WHERE NOT category = 'Preset Options' AND runtime_computed
  ORDER BY 1;
-- Preset GUCs are flagged as NOT_IN_SAMPLE.
SELECT name FROM tab_settings_flags
  WHERE category = 'Preset Options' AND NOT not_in_sample
  ORDER BY 1;
-- NO_RESET implies NO_RESET_ALL.
SELECT name FROM tab_settings_flags
  WHERE no_reset AND NOT no_reset_all
  ORDER BY 1;
DROP TABLE tab_settings_flags;
