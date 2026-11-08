--
-- TRIGGERS
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

--dg-skip: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION trigger_return_old ()
--dg         RETURNS trigger
--dg         AS :'regresslib'
--dg         LANGUAGE C;


-- Check behavior when trigger returns unmodified trigtuple
create table trigtest (f1 int, f2 text);

--dg-cascade
--dg create trigger trigger_return_old
--dg 	before insert or delete or update on trigtest
--dg 	for each row execute procedure trigger_return_old();

insert into trigtest values(1, 'foo');
select * from trigtest;
update trigtest set f2 = f2 || 'bar';
--dg-cascade
--dg select * from trigtest;
delete from trigtest;
select * from trigtest;

-- Also check what happens when such a trigger runs before or after others
--dg-skip: language "?" does not exist
--dg create function f1_times_10() returns trigger as
--dg $$ begin new.f1 := new.f1 * 10; return new; end $$ language plpgsql;

--dg-cascade
--dg create trigger trigger_alpha
--dg 	before insert or update on trigtest
--dg 	for each row execute procedure f1_times_10();

insert into trigtest values(1, 'foo');
--dg-cascade
--dg select * from trigtest;
update trigtest set f2 = f2 || 'bar';
--dg-cascade
--dg select * from trigtest;
delete from trigtest;
select * from trigtest;

--dg-cascade
--dg create trigger trigger_zed
--dg 	before insert or update on trigtest
--dg 	for each row execute procedure f1_times_10();

insert into trigtest values(1, 'foo');
--dg-cascade
--dg select * from trigtest;
update trigtest set f2 = f2 || 'bar';
--dg-cascade
--dg select * from trigtest;
delete from trigtest;
select * from trigtest;

--dg-cascade
--dg drop trigger trigger_alpha on trigtest;

insert into trigtest values(1, 'foo');
--dg-cascade
--dg select * from trigtest;
update trigtest set f2 = f2 || 'bar';
--dg-cascade
--dg select * from trigtest;
delete from trigtest;
select * from trigtest;

drop table trigtest;

-- Check behavior with an implicit column default, too (bug #16644)
create table trigtest (
  a integer,
  b bool default true not null,
  c text default 'xyzzy' not null);

--dg-cascade
--dg create trigger trigger_return_old
--dg 	before insert or delete or update on trigtest
--dg 	for each row execute procedure trigger_return_old();

insert into trigtest values(1);
select * from trigtest;

alter table trigtest add column d integer default 42 not null;

select * from trigtest;
--dg-cascade
--dg update trigtest set a = 2 where a = 1 returning *;
--dg-cascade
--dg select * from trigtest;

alter table trigtest drop column b;

--dg-cascade
--dg select * from trigtest;
--dg-cascade
--dg update trigtest set a = 2 where a = 1 returning *;
--dg-cascade
--dg select * from trigtest;

drop table trigtest;

--
-- tests for per-statement triggers
--

CREATE TABLE log_table (tstamp timestamp default timeofday()::timestamp);

CREATE TABLE main_table (a int unique, b int);

COPY main_table (a,b) FROM stdin;
5	10
20	20
30	10
50	35
80	15
\.

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION trigger_func() RETURNS trigger LANGUAGE plpgsql AS '
--dg BEGIN
--dg 	RAISE NOTICE ''trigger_func(%) called: action = %, when = %, level = %'', TG_ARGV[0], TG_OP, TG_WHEN, TG_LEVEL;
--dg 	RETURN NULL;
--dg END;';

--dg-cascade
--dg CREATE TRIGGER before_ins_stmt_trig BEFORE INSERT ON main_table
--dg FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func('before_ins_stmt');

--dg-cascade
--dg CREATE TRIGGER after_ins_stmt_trig AFTER INSERT ON main_table
--dg FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func('after_ins_stmt');

--
-- if neither 'FOR EACH ROW' nor 'FOR EACH STATEMENT' was specified,
-- CREATE TRIGGER should default to 'FOR EACH STATEMENT'
--
--dg-cascade
--dg CREATE TRIGGER after_upd_stmt_trig AFTER UPDATE ON main_table
--dg EXECUTE PROCEDURE trigger_func('after_upd_stmt');

-- Both insert and update statement level triggers (before and after) should
-- fire.  Doesn't fire UPDATE before trigger, but only because one isn't
-- defined.
--dg-cascade
--dg INSERT INTO main_table (a, b) VALUES (5, 10) ON CONFLICT (a)
--dg   DO UPDATE SET b = EXCLUDED.b;

--dg-cascade
--dg CREATE TRIGGER after_upd_row_trig AFTER UPDATE ON main_table
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('after_upd_row');

--dg-cascade
--dg INSERT INTO main_table DEFAULT VALUES;

--dg-cascade
--dg UPDATE main_table SET a = a + 1 WHERE b < 30;
-- UPDATE that effects zero rows should still call per-statement trigger
--dg-cascade
--dg UPDATE main_table SET a = a + 2 WHERE b > 100;

-- constraint now unneeded
ALTER TABLE main_table DROP CONSTRAINT main_table_a_key;

-- COPY should fire per-row and per-statement INSERT triggers
--dg-cascade
--dg COPY main_table (a, b) FROM stdin;
--dg 30	40
--dg 50	60
--dg \.

--dg-cascade
--dg SELECT * FROM main_table ORDER BY a, b;

-- Test comments
COMMENT ON TRIGGER no_such_trigger ON main_table IS 'wrong';
--dg-cascade
--dg COMMENT ON TRIGGER before_ins_stmt_trig ON main_table IS 'right';
--dg-cascade
--dg COMMENT ON TRIGGER before_ins_stmt_trig ON main_table IS NULL;

--
-- test triggers with WHEN clause
--

--dg-cascade
--dg CREATE TRIGGER modified_a BEFORE UPDATE OF a ON main_table
--dg FOR EACH ROW WHEN (OLD.a <> NEW.a) EXECUTE PROCEDURE trigger_func('modified_a');
--dg-cascade
--dg CREATE TRIGGER modified_any BEFORE UPDATE OF a ON main_table
--dg FOR EACH ROW WHEN (OLD.* IS DISTINCT FROM NEW.*) EXECUTE PROCEDURE trigger_func('modified_any');
--dg-cascade
--dg CREATE TRIGGER insert_a AFTER INSERT ON main_table
--dg FOR EACH ROW WHEN (NEW.a = 123) EXECUTE PROCEDURE trigger_func('insert_a');
--dg-cascade
--dg CREATE TRIGGER delete_a AFTER DELETE ON main_table
--dg FOR EACH ROW WHEN (OLD.a = 123) EXECUTE PROCEDURE trigger_func('delete_a');
--dg-cascade
--dg CREATE TRIGGER insert_when BEFORE INSERT ON main_table
--dg FOR EACH STATEMENT WHEN (true) EXECUTE PROCEDURE trigger_func('insert_when');
--dg-cascade
--dg CREATE TRIGGER delete_when AFTER DELETE ON main_table
--dg FOR EACH STATEMENT WHEN (true) EXECUTE PROCEDURE trigger_func('delete_when');
--dg-cascade
--dg SELECT trigger_name, event_manipulation, event_object_schema, event_object_table,
--dg        action_order, action_condition, action_orientation, action_timing,
--dg        action_reference_old_table, action_reference_new_table
--dg   FROM information_schema.triggers
--dg   WHERE event_object_table IN ('main_table')
--dg   ORDER BY trigger_name COLLATE "C", 2;
--dg-cascade
--dg INSERT INTO main_table (a) VALUES (123), (456);
--dg-cascade
--dg COPY main_table FROM stdin;
--dg 123	999
--dg 456	999
--dg \.
--dg-cascade
--dg DELETE FROM main_table WHERE a IN (123, 456);
--dg-cascade
--dg UPDATE main_table SET a = 50, b = 60;
--dg-cascade
--dg SELECT * FROM main_table ORDER BY a, b;
--dg-cascade
--dg SELECT pg_get_triggerdef(oid, true) FROM pg_trigger WHERE tgrelid = 'main_table'::regclass AND tgname = 'modified_a';
--dg-cascade
--dg SELECT pg_get_triggerdef(oid, false) FROM pg_trigger WHERE tgrelid = 'main_table'::regclass AND tgname = 'modified_a';
--dg-cascade
--dg SELECT pg_get_triggerdef(oid, true) FROM pg_trigger WHERE tgrelid = 'main_table'::regclass AND tgname = 'modified_any';

-- Test RENAME TRIGGER
--dg-cascade
--dg ALTER TRIGGER modified_a ON main_table RENAME TO modified_modified_a;
SELECT count(*) FROM pg_trigger WHERE tgrelid = 'main_table'::regclass AND tgname = 'modified_a';
--dg-cascade
--dg SELECT count(*) FROM pg_trigger WHERE tgrelid = 'main_table'::regclass AND tgname = 'modified_modified_a';

--dg-cascade
--dg DROP TRIGGER modified_modified_a ON main_table;
--dg-cascade
--dg DROP TRIGGER modified_any ON main_table;
--dg-cascade
--dg DROP TRIGGER insert_a ON main_table;
--dg-cascade
--dg DROP TRIGGER delete_a ON main_table;
--dg-cascade
--dg DROP TRIGGER insert_when ON main_table;
--dg-cascade
--dg DROP TRIGGER delete_when ON main_table;

-- Test WHEN condition accessing system columns.
create table table_with_oids(a int);
insert into table_with_oids values (1);
--dg-cascade
--dg create trigger oid_unchanged_trig after update on table_with_oids
--dg 	for each row
--dg 	when (new.tableoid = old.tableoid AND new.tableoid <> 0)
--dg 	execute procedure trigger_func('after_upd_oid_unchanged');
--dg-cascade
--dg update table_with_oids set a = a + 1;
drop table table_with_oids;

-- Test column-level triggers
--dg-cascade
--dg DROP TRIGGER after_upd_row_trig ON main_table;

--dg-cascade
--dg CREATE TRIGGER before_upd_a_row_trig BEFORE UPDATE OF a ON main_table
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('before_upd_a_row');
--dg-cascade
--dg CREATE TRIGGER after_upd_b_row_trig AFTER UPDATE OF b ON main_table
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('after_upd_b_row');
--dg-cascade
--dg CREATE TRIGGER after_upd_a_b_row_trig AFTER UPDATE OF a, b ON main_table
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('after_upd_a_b_row');

--dg-cascade
--dg CREATE TRIGGER before_upd_a_stmt_trig BEFORE UPDATE OF a ON main_table
--dg FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func('before_upd_a_stmt');
--dg-cascade
--dg CREATE TRIGGER after_upd_b_stmt_trig AFTER UPDATE OF b ON main_table
--dg FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func('after_upd_b_stmt');

--dg-cascade
--dg SELECT pg_get_triggerdef(oid) FROM pg_trigger WHERE tgrelid = 'main_table'::regclass AND tgname = 'after_upd_a_b_row_trig';

--dg-cascade
--dg UPDATE main_table SET a = 50;
--dg-cascade
--dg UPDATE main_table SET b = 10;

--
-- Test case for bug with BEFORE trigger followed by AFTER trigger with WHEN
--

CREATE TABLE some_t (some_col boolean NOT NULL);
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION dummy_update_func() RETURNS trigger AS $$
--dg BEGIN
--dg   RAISE NOTICE 'dummy_update_func(%) called: action = %, old = %, new = %',
--dg     TG_ARGV[0], TG_OP, OLD, NEW;
--dg   RETURN NEW;
--dg END;
--dg $$ LANGUAGE plpgsql;
--dg-cascade
--dg CREATE TRIGGER some_trig_before BEFORE UPDATE ON some_t FOR EACH ROW
--dg   EXECUTE PROCEDURE dummy_update_func('before');
--dg-cascade
--dg CREATE TRIGGER some_trig_aftera AFTER UPDATE ON some_t FOR EACH ROW
--dg   WHEN (NOT OLD.some_col AND NEW.some_col)
--dg   EXECUTE PROCEDURE dummy_update_func('aftera');
--dg-cascade
--dg CREATE TRIGGER some_trig_afterb AFTER UPDATE ON some_t FOR EACH ROW
--dg   WHEN (NOT NEW.some_col)
--dg   EXECUTE PROCEDURE dummy_update_func('afterb');
INSERT INTO some_t VALUES (TRUE);
--dg-cascade
--dg UPDATE some_t SET some_col = TRUE;
--dg-cascade
--dg UPDATE some_t SET some_col = FALSE;
--dg-cascade
--dg UPDATE some_t SET some_col = TRUE;
DROP TABLE some_t;

-- bogus cases
CREATE TRIGGER error_upd_and_col BEFORE UPDATE OR UPDATE OF a ON main_table
FOR EACH ROW EXECUTE PROCEDURE trigger_func('error_upd_and_col');
--dg-cascade
--dg CREATE TRIGGER error_upd_a_a BEFORE UPDATE OF a, a ON main_table
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('error_upd_a_a');
CREATE TRIGGER error_ins_a BEFORE INSERT OF a ON main_table
FOR EACH ROW EXECUTE PROCEDURE trigger_func('error_ins_a');
CREATE TRIGGER error_ins_when BEFORE INSERT OR UPDATE ON main_table
FOR EACH ROW WHEN (OLD.a <> NEW.a)
EXECUTE PROCEDURE trigger_func('error_ins_old');
CREATE TRIGGER error_del_when BEFORE DELETE OR UPDATE ON main_table
FOR EACH ROW WHEN (OLD.a <> NEW.a)
EXECUTE PROCEDURE trigger_func('error_del_new');
CREATE TRIGGER error_del_when BEFORE INSERT OR UPDATE ON main_table
FOR EACH ROW WHEN (NEW.tableoid <> 0)
EXECUTE PROCEDURE trigger_func('error_when_sys_column');
--dg-differs: another error: relation "?" does not have a composite type
--dg CREATE TRIGGER error_stmt_when BEFORE UPDATE OF a ON main_table
--dg FOR EACH STATEMENT WHEN (OLD.* IS DISTINCT FROM NEW.*)
--dg EXECUTE PROCEDURE trigger_func('error_stmt_when');

-- check dependency restrictions
--dg-cascade
--dg ALTER TABLE main_table DROP COLUMN b;
-- this should succeed, but we'll roll it back to keep the triggers around
begin;
--dg-cascade
--dg DROP TRIGGER after_upd_a_b_row_trig ON main_table;
--dg-cascade
--dg DROP TRIGGER after_upd_b_row_trig ON main_table;
--dg-cascade
--dg DROP TRIGGER after_upd_b_stmt_trig ON main_table;
--dg-cascade
--dg ALTER TABLE main_table DROP COLUMN b;
rollback;

-- Test enable/disable triggers

create table trigtest (i serial primary key);
-- test that disabling RI triggers works
create table trigtest2 (i int references trigtest(i) on delete cascade);

--dg-skip: language "?" does not exist
--dg create function trigtest() returns trigger as $$
--dg begin
--dg 	raise notice '% % % %', TG_TABLE_NAME, TG_OP, TG_WHEN, TG_LEVEL;
--dg 	return new;
--dg end;$$ language plpgsql;

--dg-cascade
--dg create trigger trigtest_b_row_tg before insert or update or delete on trigtest
--dg for each row execute procedure trigtest();
--dg-cascade
--dg create trigger trigtest_a_row_tg after insert or update or delete on trigtest
--dg for each row execute procedure trigtest();
--dg-cascade
--dg create trigger trigtest_b_stmt_tg before insert or update or delete on trigtest
--dg for each statement execute procedure trigtest();
--dg-cascade
--dg create trigger trigtest_a_stmt_tg after insert or update or delete on trigtest
--dg for each statement execute procedure trigtest();

--dg-cascade
--dg insert into trigtest default values;
--dg-cascade
--dg alter table trigtest disable trigger trigtest_b_row_tg;
--dg-cascade
--dg insert into trigtest default values;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table trigtest disable trigger user;
insert into trigtest default values;
--dg-cascade
--dg alter table trigtest enable trigger trigtest_a_stmt_tg;
--dg-cascade
--dg insert into trigtest default values;
--dg-skip: unrecognized configuration parameter "?"
--dg set session_replication_role = replica;
insert into trigtest default values;  -- does not trigger
--dg-cascade
--dg alter table trigtest enable always trigger trigtest_a_stmt_tg;
--dg-cascade
--dg insert into trigtest default values;  -- now it does
--dg-skip: unrecognized configuration parameter "?"
--dg reset session_replication_role;
insert into trigtest2 values(1);
insert into trigtest2 values(2);
--dg-cascade
--dg delete from trigtest where i=2;
--dg-cascade
--dg select * from trigtest2;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table trigtest disable trigger all;
delete from trigtest where i=1;
--dg-cascade
--dg select * from trigtest2;
-- ensure we still insert, even when all triggers are disabled
insert into trigtest default values;
--dg-cascade
--dg select *  from trigtest;
drop table trigtest2;
drop table trigtest;


-- dump trigger data
CREATE TABLE trigger_test (
        i int,
        v varchar
);

--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION trigger_data()  RETURNS trigger
--dg LANGUAGE plpgsql AS $$
--dg
--dg declare
--dg
--dg 	argstr text;
--dg 	relid text;
--dg
--dg begin
--dg
--dg 	relid := TG_relid::regclass;
--dg
--dg 	-- plpgsql can't discover its trigger data in a hash like perl and python
--dg 	-- can, or by a sort of reflection like tcl can,
--dg 	-- so we have to hard code the names.
--dg 	raise NOTICE 'TG_NAME: %', TG_name;
--dg 	raise NOTICE 'TG_WHEN: %', TG_when;
--dg 	raise NOTICE 'TG_LEVEL: %', TG_level;
--dg 	raise NOTICE 'TG_OP: %', TG_op;
--dg 	raise NOTICE 'TG_RELID::regclass: %', relid;
--dg 	raise NOTICE 'TG_RELNAME: %', TG_relname;
--dg 	raise NOTICE 'TG_TABLE_NAME: %', TG_table_name;
--dg 	raise NOTICE 'TG_TABLE_SCHEMA: %', TG_table_schema;
--dg 	raise NOTICE 'TG_NARGS: %', TG_nargs;
--dg
--dg 	argstr := '[';
--dg 	for i in 0 .. TG_nargs - 1 loop
--dg 		if i > 0 then
--dg 			argstr := argstr || ', ';
--dg 		end if;
--dg 		argstr := argstr || TG_argv[i];
--dg 	end loop;
--dg 	argstr := argstr || ']';
--dg 	raise NOTICE 'TG_ARGV: %', argstr;
--dg
--dg 	if TG_OP != 'INSERT' then
--dg 		raise NOTICE 'OLD: %', OLD;
--dg 	end if;
--dg
--dg 	if TG_OP != 'DELETE' then
--dg 		raise NOTICE 'NEW: %', NEW;
--dg 	end if;
--dg
--dg 	if TG_OP = 'DELETE' then
--dg 		return OLD;
--dg 	else
--dg 		return NEW;
--dg 	end if;
--dg
--dg end;
--dg $$;

--dg-cascade
--dg CREATE TRIGGER show_trigger_data_trig
--dg BEFORE INSERT OR UPDATE OR DELETE ON trigger_test
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

--dg-cascade
--dg insert into trigger_test values(1,'insert');
--dg-cascade
--dg update trigger_test set v = 'update' where i = 1;
--dg-cascade
--dg delete from trigger_test;

--dg-cascade
--dg DROP TRIGGER show_trigger_data_trig on trigger_test;

--dg-cascade
--dg DROP FUNCTION trigger_data();

DROP TABLE trigger_test;

--
-- Test use of row comparisons on OLD/NEW
--

CREATE TABLE trigger_test (f1 int, f2 text, f3 text);

-- this is the obvious (and wrong...) way to compare rows
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION mytrigger() RETURNS trigger LANGUAGE plpgsql as $$
--dg begin
--dg 	if row(old.*) = row(new.*) then
--dg 		raise notice 'row % not changed', new.f1;
--dg 	else
--dg 		raise notice 'row % changed', new.f1;
--dg 	end if;
--dg 	return new;
--dg end$$;

--dg-cascade
--dg CREATE TRIGGER t
--dg BEFORE UPDATE ON trigger_test
--dg FOR EACH ROW EXECUTE PROCEDURE mytrigger();

INSERT INTO trigger_test VALUES(1, 'foo', 'bar');
INSERT INTO trigger_test VALUES(2, 'baz', 'quux');

--dg-cascade
--dg UPDATE trigger_test SET f3 = 'bar';
--dg-cascade
--dg UPDATE trigger_test SET f3 = NULL;
-- this demonstrates that the above isn't really working as desired:
--dg-cascade
--dg UPDATE trigger_test SET f3 = NULL;

-- the right way when considering nulls is
--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION mytrigger() RETURNS trigger LANGUAGE plpgsql as $$
--dg begin
--dg 	if row(old.*) is distinct from row(new.*) then
--dg 		raise notice 'row % changed', new.f1;
--dg 	else
--dg 		raise notice 'row % not changed', new.f1;
--dg 	end if;
--dg 	return new;
--dg end$$;

--dg-cascade
--dg UPDATE trigger_test SET f3 = 'bar';
--dg-cascade
--dg UPDATE trigger_test SET f3 = NULL;
--dg-cascade
--dg UPDATE trigger_test SET f3 = NULL;

DROP TABLE trigger_test;

--dg-cascade
--dg DROP FUNCTION mytrigger();

-- Test snapshot management in serializable transactions involving triggers
-- per bug report in 6bc73d4c0910042358k3d1adff3qa36f8df75198ecea@mail.gmail.com
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION serializable_update_trig() RETURNS trigger LANGUAGE plpgsql AS
--dg $$
--dg declare
--dg 	rec record;
--dg begin
--dg 	new.description = 'updated in trigger';
--dg 	return new;
--dg end;
--dg $$;

CREATE TABLE serializable_update_tab (
	id int,
	filler  text,
	description text
);

--dg-cascade
--dg CREATE TRIGGER serializable_update_trig BEFORE UPDATE ON serializable_update_tab
--dg 	FOR EACH ROW EXECUTE PROCEDURE serializable_update_trig();

INSERT INTO serializable_update_tab SELECT a, repeat('xyzxz', 100), 'new'
	FROM generate_series(1, 50) a;

BEGIN;
SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;
UPDATE serializable_update_tab SET description = 'no no', id = 1 WHERE id = 1;
COMMIT;
--dg-cascade
--dg SELECT description FROM serializable_update_tab WHERE id = 1;
DROP TABLE serializable_update_tab;

-- minimal update trigger

CREATE TABLE min_updates_test (
	f1	text,
	f2 int,
	f3 int);

INSERT INTO min_updates_test VALUES ('a',1,2),('b','2',null);

--dg-skip: relation pg_trigger cannot be written: digres has no catalog tables
--dg CREATE TRIGGER z_min_update
--dg BEFORE UPDATE ON min_updates_test
--dg FOR EACH ROW EXECUTE PROCEDURE suppress_redundant_updates_trigger();

\set QUIET false

--dg-cascade
--dg UPDATE min_updates_test SET f1 = f1;

UPDATE min_updates_test SET f2 = f2 + 1;

UPDATE min_updates_test SET f3 = 2 WHERE f3 is null;

\set QUIET true

SELECT * FROM min_updates_test;

DROP TABLE min_updates_test;

--
-- Test triggers on views
--

--dg-cascade
--dg CREATE VIEW main_view AS SELECT a, b FROM main_table;

-- VIEW trigger function
--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION view_trigger() RETURNS trigger
--dg LANGUAGE plpgsql AS $$
--dg declare
--dg     argstr text := '';
--dg begin
--dg     for i in 0 .. TG_nargs - 1 loop
--dg         if i > 0 then
--dg             argstr := argstr || ', ';
--dg         end if;
--dg         argstr := argstr || TG_argv[i];
--dg     end loop;
--dg
--dg     raise notice '% % % % (%)', TG_TABLE_NAME, TG_WHEN, TG_OP, TG_LEVEL, argstr;
--dg
--dg     if TG_LEVEL = 'ROW' then
--dg         if TG_OP = 'INSERT' then
--dg             raise NOTICE 'NEW: %', NEW;
--dg             INSERT INTO main_table VALUES (NEW.a, NEW.b);
--dg             RETURN NEW;
--dg         end if;
--dg
--dg         if TG_OP = 'UPDATE' then
--dg             raise NOTICE 'OLD: %, NEW: %', OLD, NEW;
--dg             UPDATE main_table SET a = NEW.a, b = NEW.b WHERE a = OLD.a AND b = OLD.b;
--dg             if NOT FOUND then RETURN NULL; end if;
--dg             RETURN NEW;
--dg         end if;
--dg
--dg         if TG_OP = 'DELETE' then
--dg             raise NOTICE 'OLD: %', OLD;
--dg             DELETE FROM main_table WHERE a = OLD.a AND b = OLD.b;
--dg             if NOT FOUND then RETURN NULL; end if;
--dg             RETURN OLD;
--dg         end if;
--dg     end if;
--dg
--dg     RETURN NULL;
--dg end;
--dg $$;

-- Before row triggers aren't allowed on views
--dg-cascade
--dg CREATE TRIGGER invalid_trig BEFORE INSERT ON main_view
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('before_ins_row');

--dg-cascade
--dg CREATE TRIGGER invalid_trig BEFORE UPDATE ON main_view
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('before_upd_row');

--dg-cascade
--dg CREATE TRIGGER invalid_trig BEFORE DELETE ON main_view
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('before_del_row');

-- After row triggers aren't allowed on views
--dg-cascade
--dg CREATE TRIGGER invalid_trig AFTER INSERT ON main_view
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('before_ins_row');

--dg-cascade
--dg CREATE TRIGGER invalid_trig AFTER UPDATE ON main_view
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('before_upd_row');

--dg-cascade
--dg CREATE TRIGGER invalid_trig AFTER DELETE ON main_view
--dg FOR EACH ROW EXECUTE PROCEDURE trigger_func('before_del_row');

-- Truncate triggers aren't allowed on views
--dg-cascade
--dg CREATE TRIGGER invalid_trig BEFORE TRUNCATE ON main_view
--dg EXECUTE PROCEDURE trigger_func('before_tru_row');

--dg-cascade
--dg CREATE TRIGGER invalid_trig AFTER TRUNCATE ON main_view
--dg EXECUTE PROCEDURE trigger_func('before_tru_row');

-- INSTEAD OF triggers aren't allowed on tables
CREATE TRIGGER invalid_trig INSTEAD OF INSERT ON main_table
FOR EACH ROW EXECUTE PROCEDURE view_trigger('instead_of_ins');

CREATE TRIGGER invalid_trig INSTEAD OF UPDATE ON main_table
FOR EACH ROW EXECUTE PROCEDURE view_trigger('instead_of_upd');

CREATE TRIGGER invalid_trig INSTEAD OF DELETE ON main_table
FOR EACH ROW EXECUTE PROCEDURE view_trigger('instead_of_del');

-- Don't support WHEN clauses with INSTEAD OF triggers
--dg-cascade
--dg CREATE TRIGGER invalid_trig INSTEAD OF UPDATE ON main_view
--dg FOR EACH ROW WHEN (OLD.a <> NEW.a) EXECUTE PROCEDURE view_trigger('instead_of_upd');

-- Don't support column-level INSTEAD OF triggers
--dg-cascade
--dg CREATE TRIGGER invalid_trig INSTEAD OF UPDATE OF a ON main_view
--dg FOR EACH ROW EXECUTE PROCEDURE view_trigger('instead_of_upd');

-- Don't support statement-level INSTEAD OF triggers
--dg-cascade
--dg CREATE TRIGGER invalid_trig INSTEAD OF UPDATE ON main_view
--dg EXECUTE PROCEDURE view_trigger('instead_of_upd');

-- Valid INSTEAD OF triggers
--dg-cascade
--dg CREATE TRIGGER instead_of_insert_trig INSTEAD OF INSERT ON main_view
--dg FOR EACH ROW EXECUTE PROCEDURE view_trigger('instead_of_ins');

--dg-cascade
--dg CREATE TRIGGER instead_of_update_trig INSTEAD OF UPDATE ON main_view
--dg FOR EACH ROW EXECUTE PROCEDURE view_trigger('instead_of_upd');

--dg-cascade
--dg CREATE TRIGGER instead_of_delete_trig INSTEAD OF DELETE ON main_view
--dg FOR EACH ROW EXECUTE PROCEDURE view_trigger('instead_of_del');

-- Valid BEFORE statement VIEW triggers
--dg-cascade
--dg CREATE TRIGGER before_ins_stmt_trig BEFORE INSERT ON main_view
--dg FOR EACH STATEMENT EXECUTE PROCEDURE view_trigger('before_view_ins_stmt');

--dg-cascade
--dg CREATE TRIGGER before_upd_stmt_trig BEFORE UPDATE ON main_view
--dg FOR EACH STATEMENT EXECUTE PROCEDURE view_trigger('before_view_upd_stmt');

--dg-cascade
--dg CREATE TRIGGER before_del_stmt_trig BEFORE DELETE ON main_view
--dg FOR EACH STATEMENT EXECUTE PROCEDURE view_trigger('before_view_del_stmt');

-- Valid AFTER statement VIEW triggers
--dg-cascade
--dg CREATE TRIGGER after_ins_stmt_trig AFTER INSERT ON main_view
--dg FOR EACH STATEMENT EXECUTE PROCEDURE view_trigger('after_view_ins_stmt');

--dg-cascade
--dg CREATE TRIGGER after_upd_stmt_trig AFTER UPDATE ON main_view
--dg FOR EACH STATEMENT EXECUTE PROCEDURE view_trigger('after_view_upd_stmt');

--dg-cascade
--dg CREATE TRIGGER after_del_stmt_trig AFTER DELETE ON main_view
--dg FOR EACH STATEMENT EXECUTE PROCEDURE view_trigger('after_view_del_stmt');

\set QUIET false

-- Insert into view using trigger
--dg-cascade
--dg INSERT INTO main_view VALUES (20, 30);
--dg-cascade
--dg INSERT INTO main_view VALUES (21, 31) RETURNING a, b;

-- Table trigger will prevent updates
--dg-cascade
--dg UPDATE main_view SET b = 31 WHERE a = 20;
--dg-cascade
--dg UPDATE main_view SET b = 32 WHERE a = 21 AND b = 31 RETURNING a, b;

-- Remove table trigger to allow updates
--dg-cascade
--dg DROP TRIGGER before_upd_a_row_trig ON main_table;
--dg-cascade
--dg UPDATE main_view SET b = 31 WHERE a = 20;
--dg-cascade
--dg UPDATE main_view SET b = 32 WHERE a = 21 AND b = 31 RETURNING a, b;

-- Before and after stmt triggers should fire even when no rows are affected
--dg-cascade
--dg UPDATE main_view SET b = 0 WHERE false;

-- Delete from view using trigger
--dg-cascade
--dg DELETE FROM main_view WHERE a IN (20,21);
--dg-cascade
--dg DELETE FROM main_view WHERE a = 31 RETURNING a, b;

\set QUIET true

-- Describe view should list triggers
--dg-cascade
--dg \d main_view

-- Test dropping view triggers
--dg-cascade
--dg DROP TRIGGER instead_of_insert_trig ON main_view;
--dg-cascade
--dg DROP TRIGGER instead_of_delete_trig ON main_view;
--dg-cascade
--dg \d+ main_view
--dg-cascade
--dg DROP VIEW main_view;

--
-- Test triggers on a join view
--
CREATE TABLE country_table (
    country_id        serial primary key,
    country_name    text unique not null,
    continent        text not null
);

INSERT INTO country_table (country_name, continent)
    VALUES ('Japan', 'Asia'),
           ('UK', 'Europe'),
           ('USA', 'North America')
    RETURNING *;

CREATE TABLE city_table (
    city_id        serial primary key,
    city_name    text not null,
    population    bigint,
    country_id    int references country_table
);

CREATE VIEW city_view AS
    SELECT city_id, city_name, population, country_name, continent
    FROM city_table ci
    LEFT JOIN country_table co ON co.country_id = ci.country_id;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION city_insert() RETURNS trigger LANGUAGE plpgsql AS $$
--dg declare
--dg     ctry_id int;
--dg begin
--dg     if NEW.country_name IS NOT NULL then
--dg         SELECT country_id, continent INTO ctry_id, NEW.continent
--dg             FROM country_table WHERE country_name = NEW.country_name;
--dg         if NOT FOUND then
--dg             raise exception 'No such country: "%"', NEW.country_name;
--dg         end if;
--dg     else
--dg         NEW.continent := NULL;
--dg     end if;
--dg
--dg     if NEW.city_id IS NOT NULL then
--dg         INSERT INTO city_table
--dg             VALUES(NEW.city_id, NEW.city_name, NEW.population, ctry_id);
--dg     else
--dg         INSERT INTO city_table(city_name, population, country_id)
--dg             VALUES(NEW.city_name, NEW.population, ctry_id)
--dg             RETURNING city_id INTO NEW.city_id;
--dg     end if;
--dg
--dg     RETURN NEW;
--dg end;
--dg $$;

--dg-cascade
--dg CREATE TRIGGER city_insert_trig INSTEAD OF INSERT ON city_view
--dg FOR EACH ROW EXECUTE PROCEDURE city_insert();

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION city_delete() RETURNS trigger LANGUAGE plpgsql AS $$
--dg begin
--dg     DELETE FROM city_table WHERE city_id = OLD.city_id;
--dg     if NOT FOUND then RETURN NULL; end if;
--dg     RETURN OLD;
--dg end;
--dg $$;

--dg-cascade
--dg CREATE TRIGGER city_delete_trig INSTEAD OF DELETE ON city_view
--dg FOR EACH ROW EXECUTE PROCEDURE city_delete();

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION city_update() RETURNS trigger LANGUAGE plpgsql AS $$
--dg declare
--dg     ctry_id int;
--dg begin
--dg     if NEW.country_name IS DISTINCT FROM OLD.country_name then
--dg         SELECT country_id, continent INTO ctry_id, NEW.continent
--dg             FROM country_table WHERE country_name = NEW.country_name;
--dg         if NOT FOUND then
--dg             raise exception 'No such country: "%"', NEW.country_name;
--dg         end if;
--dg
--dg         UPDATE city_table SET city_name = NEW.city_name,
--dg                               population = NEW.population,
--dg                               country_id = ctry_id
--dg             WHERE city_id = OLD.city_id;
--dg     else
--dg         UPDATE city_table SET city_name = NEW.city_name,
--dg                               population = NEW.population
--dg             WHERE city_id = OLD.city_id;
--dg         NEW.continent := OLD.continent;
--dg     end if;
--dg
--dg     if NOT FOUND then RETURN NULL; end if;
--dg     RETURN NEW;
--dg end;
--dg $$;

--dg-cascade
--dg CREATE TRIGGER city_update_trig INSTEAD OF UPDATE ON city_view
--dg FOR EACH ROW EXECUTE PROCEDURE city_update();

\set QUIET false

-- INSERT .. RETURNING
--dg-cascade
--dg INSERT INTO city_view(city_name) VALUES('Tokyo') RETURNING *;
--dg-cascade
--dg INSERT INTO city_view(city_name, population) VALUES('London', 7556900) RETURNING *;
--dg-cascade
--dg INSERT INTO city_view(city_name, country_name) VALUES('Washington DC', 'USA') RETURNING *;
--dg-cascade
--dg INSERT INTO city_view(city_id, city_name) VALUES(123456, 'New York') RETURNING *;
--dg-cascade
--dg INSERT INTO city_view VALUES(234567, 'Birmingham', 1016800, 'UK', 'EU') RETURNING *;

-- UPDATE .. RETURNING
--dg-cascade
--dg UPDATE city_view SET country_name = 'Japon' WHERE city_name = 'Tokyo'; -- error
--dg-cascade
--dg UPDATE city_view SET country_name = 'Japan' WHERE city_name = 'Takyo'; -- no match
--dg-cascade
--dg UPDATE city_view SET country_name = 'Japan' WHERE city_name = 'Tokyo' RETURNING *; -- OK

--dg-cascade
--dg UPDATE city_view SET population = 13010279 WHERE city_name = 'Tokyo' RETURNING *;
--dg-cascade
--dg UPDATE city_view SET country_name = 'UK' WHERE city_name = 'New York' RETURNING *;
--dg-cascade
--dg UPDATE city_view SET country_name = 'USA', population = 8391881 WHERE city_name = 'New York' RETURNING *;
--dg-cascade
--dg UPDATE city_view SET continent = 'EU' WHERE continent = 'Europe' RETURNING *;
--dg-cascade
--dg UPDATE city_view v1 SET country_name = v2.country_name FROM city_view v2
--dg     WHERE v2.city_name = 'Birmingham' AND v1.city_name = 'London' RETURNING *;

-- DELETE .. RETURNING
--dg-cascade
--dg DELETE FROM city_view WHERE city_name = 'Birmingham' RETURNING *;

\set QUIET true

-- read-only view with WHERE clause
CREATE VIEW european_city_view AS
    SELECT * FROM city_view WHERE continent = 'Europe';
--dg-cascade
--dg SELECT count(*) FROM european_city_view;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION no_op_trig_fn() RETURNS trigger LANGUAGE plpgsql
--dg AS 'begin RETURN NULL; end';

--dg-cascade
--dg CREATE TRIGGER no_op_trig INSTEAD OF INSERT OR UPDATE OR DELETE
--dg ON european_city_view FOR EACH ROW EXECUTE PROCEDURE no_op_trig_fn();

\set QUIET false

--dg-cascade
--dg INSERT INTO european_city_view VALUES (0, 'x', 10000, 'y', 'z');
--dg-cascade
--dg UPDATE european_city_view SET population = 10000;
--dg-cascade
--dg DELETE FROM european_city_view;

\set QUIET true

-- rules bypassing no-op triggers
--dg-skip: relation pg_rewrite cannot be written: digres has no catalog tables
--dg CREATE RULE european_city_insert_rule AS ON INSERT TO european_city_view
--dg DO INSTEAD INSERT INTO city_view
--dg VALUES (NEW.city_id, NEW.city_name, NEW.population, NEW.country_name, NEW.continent)
--dg RETURNING *;

--dg-skip: relation pg_rewrite cannot be written: digres has no catalog tables
--dg CREATE RULE european_city_update_rule AS ON UPDATE TO european_city_view
--dg DO INSTEAD UPDATE city_view SET
--dg     city_name = NEW.city_name,
--dg     population = NEW.population,
--dg     country_name = NEW.country_name
--dg WHERE city_id = OLD.city_id
--dg RETURNING NEW.*;

--dg-skip: relation pg_rewrite cannot be written: digres has no catalog tables
--dg CREATE RULE european_city_delete_rule AS ON DELETE TO european_city_view
--dg DO INSTEAD DELETE FROM city_view WHERE city_id = OLD.city_id RETURNING *;

\set QUIET false

-- INSERT not limited by view's WHERE clause, but UPDATE AND DELETE are
--dg-cascade
--dg INSERT INTO european_city_view(city_name, country_name)
--dg     VALUES ('Cambridge', 'USA') RETURNING *;
--dg-cascade
--dg UPDATE european_city_view SET country_name = 'UK'
--dg     WHERE city_name = 'Cambridge';
--dg-cascade
--dg DELETE FROM european_city_view WHERE city_name = 'Cambridge';

-- UPDATE and DELETE via rule and trigger
--dg-cascade
--dg UPDATE city_view SET country_name = 'UK'
--dg     WHERE city_name = 'Cambridge' RETURNING *;
--dg-cascade
--dg UPDATE european_city_view SET population = 122800
--dg     WHERE city_name = 'Cambridge' RETURNING *;
--dg-cascade
--dg DELETE FROM european_city_view WHERE city_name = 'Cambridge' RETURNING *;

-- join UPDATE test
--dg-cascade
--dg UPDATE city_view v SET population = 599657
--dg     FROM city_table ci, country_table co
--dg     WHERE ci.city_name = 'Washington DC' and co.country_name = 'USA'
--dg     AND v.city_id = ci.city_id AND v.country_name = co.country_name
--dg     RETURNING co.country_id, v.country_name,
--dg               v.city_id, v.city_name, v.population;

\set QUIET true

--dg-cascade
--dg SELECT * FROM city_view;

DROP TABLE city_table CASCADE;
DROP TABLE country_table;


-- Test pg_trigger_depth()

create table depth_a (id int not null primary key);
create table depth_b (id int not null primary key);
create table depth_c (id int not null primary key);

--dg-skip: language "?" does not exist
--dg create function depth_a_tf() returns trigger
--dg   language plpgsql as $$
--dg begin
--dg   raise notice '%: depth = %', tg_name, pg_trigger_depth();
--dg   insert into depth_b values (new.id);
--dg   raise notice '%: depth = %', tg_name, pg_trigger_depth();
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger depth_a_tr before insert on depth_a
--dg   for each row execute procedure depth_a_tf();

--dg-skip: language "?" does not exist
--dg create function depth_b_tf() returns trigger
--dg   language plpgsql as $$
--dg begin
--dg   raise notice '%: depth = %', tg_name, pg_trigger_depth();
--dg   begin
--dg     execute 'insert into depth_c values (' || new.id::text || ')';
--dg   exception
--dg     when sqlstate 'U9999' then
--dg       raise notice 'SQLSTATE = U9999: depth = %', pg_trigger_depth();
--dg   end;
--dg   raise notice '%: depth = %', tg_name, pg_trigger_depth();
--dg   if new.id = 1 then
--dg     execute 'insert into depth_c values (' || new.id::text || ')';
--dg   end if;
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger depth_b_tr before insert on depth_b
--dg   for each row execute procedure depth_b_tf();

--dg-skip: language "?" does not exist
--dg create function depth_c_tf() returns trigger
--dg   language plpgsql as $$
--dg begin
--dg   raise notice '%: depth = %', tg_name, pg_trigger_depth();
--dg   if new.id = 1 then
--dg     raise exception sqlstate 'U9999';
--dg   end if;
--dg   raise notice '%: depth = %', tg_name, pg_trigger_depth();
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger depth_c_tr before insert on depth_c
--dg   for each row execute procedure depth_c_tf();

select pg_trigger_depth();
--dg-cascade
--dg insert into depth_a values (1);
select pg_trigger_depth();
--dg-cascade
--dg insert into depth_a values (2);
select pg_trigger_depth();

drop table depth_a, depth_b, depth_c;
--dg-cascade
--dg drop function depth_a_tf();
--dg-cascade
--dg drop function depth_b_tf();
--dg-cascade
--dg drop function depth_c_tf();

--
-- Test updates to rows during firing of BEFORE ROW triggers.
-- As of 9.2, such cases should be rejected (see bug #6123).
--

create temp table parent (
    aid int not null primary key,
    val1 text,
    val2 text,
    val3 text,
    val4 text,
    bcnt int not null default 0);
create temp table child (
    bid int not null primary key,
    aid int not null,
    val1 text);

--dg-skip: language "?" does not exist
--dg create function parent_upd_func()
--dg   returns trigger language plpgsql as
--dg $$
--dg begin
--dg   if old.val1 <> new.val1 then
--dg     new.val2 = new.val1;
--dg     delete from child where child.aid = new.aid and child.val1 = new.val1;
--dg   end if;
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger parent_upd_trig before update on parent
--dg   for each row execute procedure parent_upd_func();

--dg-skip: language "?" does not exist
--dg create function parent_del_func()
--dg   returns trigger language plpgsql as
--dg $$
--dg begin
--dg   delete from child where aid = old.aid;
--dg   return old;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger parent_del_trig before delete on parent
--dg   for each row execute procedure parent_del_func();

--dg-skip: language "?" does not exist
--dg create function child_ins_func()
--dg   returns trigger language plpgsql as
--dg $$
--dg begin
--dg   update parent set bcnt = bcnt + 1 where aid = new.aid;
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger child_ins_trig after insert on child
--dg   for each row execute procedure child_ins_func();

--dg-skip: language "?" does not exist
--dg create function child_del_func()
--dg   returns trigger language plpgsql as
--dg $$
--dg begin
--dg   update parent set bcnt = bcnt - 1 where aid = old.aid;
--dg   return old;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger child_del_trig after delete on child
--dg   for each row execute procedure child_del_func();

insert into parent values (1, 'a', 'a', 'a', 'a', 0);
insert into child values (10, 1, 'b');
--dg-cascade
--dg select * from parent;
 select * from child;

--dg-cascade
--dg update parent set val1 = 'b' where aid = 1; -- should fail
--dg-cascade
--dg merge into parent p using (values (1)) as v(id) on p.aid = v.id
--dg   when matched then update set val1 = 'b'; -- should fail
--dg-cascade
--dg select * from parent;
 select * from child;

--dg-cascade
--dg delete from parent where aid = 1; -- should fail
--dg-cascade
--dg merge into parent p using (values (1)) as v(id) on p.aid = v.id
--dg   when matched then delete; -- should fail
--dg-cascade
--dg select * from parent;
 select * from child;

-- replace the trigger function with one that restarts the deletion after
-- having modified a child
--dg-skip: language "?" does not exist
--dg create or replace function parent_del_func()
--dg   returns trigger language plpgsql as
--dg $$
--dg begin
--dg   delete from child where aid = old.aid;
--dg   if found then
--dg     delete from parent where aid = old.aid;
--dg     return null; -- cancel outer deletion
--dg   end if;
--dg   return old;
--dg end;
--dg $$;

delete from parent where aid = 1;
select * from parent; 
--dg-cascade
--dg select * from child;

drop table parent, child;

--dg-cascade
--dg drop function parent_upd_func();
--dg-cascade
--dg drop function parent_del_func();
--dg-cascade
--dg drop function child_ins_func();
--dg-cascade
--dg drop function child_del_func();

-- similar case, but with a self-referencing FK so that parent and child
-- rows can be affected by a single operation

create temp table self_ref_trigger (
    id int primary key,
    parent int references self_ref_trigger,
    data text,
    nchildren int not null default 0
);

--dg-skip: language "?" does not exist
--dg create function self_ref_trigger_ins_func()
--dg   returns trigger language plpgsql as
--dg $$
--dg begin
--dg   if new.parent is not null then
--dg     update self_ref_trigger set nchildren = nchildren + 1
--dg       where id = new.parent;
--dg   end if;
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger self_ref_trigger_ins_trig before insert on self_ref_trigger
--dg   for each row execute procedure self_ref_trigger_ins_func();

--dg-skip: language "?" does not exist
--dg create function self_ref_trigger_del_func()
--dg   returns trigger language plpgsql as
--dg $$
--dg begin
--dg   if old.parent is not null then
--dg     update self_ref_trigger set nchildren = nchildren - 1
--dg       where id = old.parent;
--dg   end if;
--dg   return old;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger self_ref_trigger_del_trig before delete on self_ref_trigger
--dg   for each row execute procedure self_ref_trigger_del_func();

insert into self_ref_trigger values (1, null, 'root');
insert into self_ref_trigger values (2, 1, 'root child A');
insert into self_ref_trigger values (3, 1, 'root child B');
insert into self_ref_trigger values (4, 2, 'grandchild 1');
insert into self_ref_trigger values (5, 3, 'grandchild 2');

update self_ref_trigger set data = 'root!' where id = 1;

--dg-cascade
--dg select * from self_ref_trigger;

--dg-cascade
--dg delete from self_ref_trigger;

--dg-cascade
--dg select * from self_ref_trigger;

drop table self_ref_trigger;
--dg-cascade
--dg drop function self_ref_trigger_ins_func();
--dg-cascade
--dg drop function self_ref_trigger_del_func();

--
-- Check that statement triggers work correctly even with all children excluded
--

create table stmt_trig_on_empty_upd (a int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table stmt_trig_on_empty_upd1 () inherits (stmt_trig_on_empty_upd);
--dg-skip: language "?" does not exist
--dg create function update_stmt_notice() returns trigger as $$
--dg begin
--dg 	raise notice 'updating %', TG_TABLE_NAME;
--dg 	return null;
--dg end;
--dg $$ language plpgsql;
--dg-cascade
--dg create trigger before_stmt_trigger
--dg 	before update on stmt_trig_on_empty_upd
--dg 	execute procedure update_stmt_notice();
--dg-cascade
--dg create trigger before_stmt_trigger
--dg 	before update on stmt_trig_on_empty_upd1
--dg 	execute procedure update_stmt_notice();

-- inherited no-op update
--dg-cascade
--dg update stmt_trig_on_empty_upd set a = a where false returning a+1 as aa;
-- simple no-op update
--dg-cascade
--dg update stmt_trig_on_empty_upd1 set a = a where false returning a+1 as aa;

--dg-cascade
--dg drop table stmt_trig_on_empty_upd cascade;
--dg-cascade
--dg drop function update_stmt_notice();

--
-- Check that index creation (or DDL in general) is prohibited in a trigger
--

create table trigger_ddl_table (
   col1 integer,
   col2 integer
);

--dg-skip: language "?" does not exist
--dg create function trigger_ddl_func() returns trigger as $$
--dg begin
--dg   alter table trigger_ddl_table add primary key (col1);
--dg   return new;
--dg end$$ language plpgsql;

--dg-cascade
--dg create trigger trigger_ddl_func before insert on trigger_ddl_table for each row
--dg   execute procedure trigger_ddl_func();

--dg-cascade
--dg insert into trigger_ddl_table values (1, 42);  -- fail

--dg-skip: language "?" does not exist
--dg create or replace function trigger_ddl_func() returns trigger as $$
--dg begin
--dg   create index on trigger_ddl_table (col2);
--dg   return new;
--dg end$$ language plpgsql;

--dg-cascade
--dg insert into trigger_ddl_table values (1, 42);  -- fail

drop table trigger_ddl_table;
--dg-cascade
--dg drop function trigger_ddl_func();

--
-- Verify behavior of before and after triggers with INSERT...ON CONFLICT
-- DO UPDATE
--
create table upsert (key int4 primary key, color text);

--dg-skip: language "?" does not exist
--dg create function upsert_before_func()
--dg   returns trigger language plpgsql as
--dg $$
--dg begin
--dg   if (TG_OP = 'UPDATE') then
--dg     raise warning 'before update (old): %', old.*::text;
--dg     raise warning 'before update (new): %', new.*::text;
--dg   elsif (TG_OP = 'INSERT') then
--dg     raise warning 'before insert (new): %', new.*::text;
--dg     if new.key % 2 = 0 then
--dg       new.key := new.key + 1;
--dg       new.color := new.color || ' trig modified';
--dg       raise warning 'before insert (new, modified): %', new.*::text;
--dg     end if;
--dg   end if;
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger upsert_before_trig before insert or update on upsert
--dg   for each row execute procedure upsert_before_func();

--dg-skip: language "?" does not exist
--dg create function upsert_after_func()
--dg   returns trigger language plpgsql as
--dg $$
--dg begin
--dg   if (TG_OP = 'UPDATE') then
--dg     raise warning 'after update (old): %', old.*::text;
--dg     raise warning 'after update (new): %', new.*::text;
--dg   elsif (TG_OP = 'INSERT') then
--dg     raise warning 'after insert (new): %', new.*::text;
--dg   end if;
--dg   return null;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger upsert_after_trig after insert or update on upsert
--dg   for each row execute procedure upsert_after_func();

--dg-cascade
--dg insert into upsert values(1, 'black') on conflict (key) do update set color = 'updated ' || upsert.color;
--dg-cascade
--dg insert into upsert values(2, 'red') on conflict (key) do update set color = 'updated ' || upsert.color;
--dg-cascade
--dg insert into upsert values(3, 'orange') on conflict (key) do update set color = 'updated ' || upsert.color;
--dg-cascade
--dg insert into upsert values(4, 'green') on conflict (key) do update set color = 'updated ' || upsert.color;
--dg-cascade
--dg insert into upsert values(5, 'purple') on conflict (key) do update set color = 'updated ' || upsert.color;
--dg-cascade
--dg insert into upsert values(6, 'white') on conflict (key) do update set color = 'updated ' || upsert.color;
--dg-cascade
--dg insert into upsert values(7, 'pink') on conflict (key) do update set color = 'updated ' || upsert.color;
--dg-cascade
--dg insert into upsert values(8, 'yellow') on conflict (key) do update set color = 'updated ' || upsert.color;

--dg-cascade
--dg select * from upsert;

drop table upsert;
--dg-cascade
--dg drop function upsert_before_func();
--dg-cascade
--dg drop function upsert_after_func();

--
-- Verify that triggers with transition tables are not allowed on
-- views
--

create table my_table (i int);
create view my_view as select * from my_table;
--dg-skip: language "?" does not exist
--dg create function my_trigger_function() returns trigger as $$ begin end; $$ language plpgsql;
create trigger my_trigger after update on my_view referencing old table as old_table
   for each statement execute procedure my_trigger_function();
--dg-cascade
--dg drop function my_trigger_function();
drop view my_view;
drop table my_table;

--
-- Verify cases that are unsupported with partitioned tables
--
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_trig (a int) partition by list (a);
--dg-skip: language "?" does not exist
--dg create function trigger_nothing() returns trigger
--dg   language plpgsql as $$ begin end; $$;
--dg-cascade
--dg create trigger failed instead of update on parted_trig
--dg   for each row execute procedure trigger_nothing();
--dg-cascade
--dg create trigger failed after update on parted_trig
--dg   referencing old table as old_table
--dg   for each row execute procedure trigger_nothing();
--dg-cascade
--dg drop table parted_trig;

--
-- Verify trigger creation for partitioned tables, and drop behavior
--
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table trigpart (a int, b int) partition by range (a);
--dg-cascade
--dg create table trigpart1 partition of trigpart for values from (0) to (1000);
--dg-cascade
--dg create trigger trg1 after insert on trigpart for each row execute procedure trigger_nothing();
--dg-cascade
--dg create table trigpart2 partition of trigpart for values from (1000) to (2000);
--dg-cascade
--dg create table trigpart3 (like trigpart);
--dg-cascade
--dg alter table trigpart attach partition trigpart3 for values from (2000) to (3000);
--dg-cascade
--dg create table trigpart4 partition of trigpart for values from (3000) to (4000) partition by range (a);
--dg-cascade
--dg create table trigpart41 partition of trigpart4 for values from (3000) to (3500);
--dg-cascade
--dg create table trigpart42 (like trigpart);
--dg-cascade
--dg alter table trigpart4 attach partition trigpart42 for values from (3500) to (4000);
--dg-cascade
--dg select tgrelid::regclass, tgname, tgfoid::regproc from pg_trigger
--dg   where tgrelid::regclass::text like 'trigpart%' order by tgrelid::regclass::text;
--dg-cascade
--dg drop trigger trg1 on trigpart1;	-- fail
--dg-cascade
--dg drop trigger trg1 on trigpart2;	-- fail
--dg-cascade
--dg drop trigger trg1 on trigpart3;	-- fail
--dg-cascade
--dg drop table trigpart2;			-- ok, trigger should be gone in that partition
--dg-cascade
--dg select tgrelid::regclass, tgname, tgfoid::regproc from pg_trigger
--dg   where tgrelid::regclass::text like 'trigpart%' order by tgrelid::regclass::text;
--dg-cascade
--dg drop trigger trg1 on trigpart;		-- ok, all gone
select tgrelid::regclass, tgname, tgfoid::regproc from pg_trigger
  where tgrelid::regclass::text like 'trigpart%' order by tgrelid::regclass::text;

-- check detach behavior
--dg-cascade
--dg create trigger trg1 after insert on trigpart for each row execute procedure trigger_nothing();
--dg-cascade
--dg \d trigpart3
--dg-cascade
--dg alter table trigpart detach partition trigpart3;
--dg-cascade
--dg drop trigger trg1 on trigpart3; -- fail due to "does not exist"
--dg-cascade
--dg alter table trigpart detach partition trigpart4;
--dg-cascade
--dg drop trigger trg1 on trigpart41; -- fail due to "does not exist"
--dg-cascade
--dg drop table trigpart4;
--dg-cascade
--dg alter table trigpart attach partition trigpart3 for values from (2000) to (3000);
--dg-cascade
--dg alter table trigpart detach partition trigpart3;
--dg-cascade
--dg alter table trigpart attach partition trigpart3 for values from (2000) to (3000);
--dg-cascade
--dg drop table trigpart3;

--dg-cascade
--dg select tgrelid::regclass::text, tgname, tgfoid::regproc, tgenabled, tgisinternal from pg_trigger
--dg   where tgname ~ '^trg1' order by 1;
--dg-cascade
--dg create table trigpart3 (like trigpart);
--dg-cascade
--dg create trigger trg1 after insert on trigpart3 for each row execute procedure trigger_nothing();
--dg-cascade
--dg \d trigpart3
--dg-cascade
--dg alter table trigpart attach partition trigpart3 FOR VALUES FROM (2000) to (3000); -- fail
--dg-cascade
--dg drop table trigpart3;

-- check display of unrelated triggers
--dg-cascade
--dg create trigger samename after delete on trigpart execute function trigger_nothing();
--dg-cascade
--dg create trigger samename after delete on trigpart1 execute function trigger_nothing();
--dg-cascade
--dg \d trigpart1

--dg-cascade
--dg drop table trigpart;
--dg-cascade
--dg drop function trigger_nothing();

--
-- Verify that triggers are fired for partitioned tables
--
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_stmt_trig (a int) partition by list (a);
--dg-cascade
--dg create table parted_stmt_trig1 partition of parted_stmt_trig for values in (1);
--dg-cascade
--dg create table parted_stmt_trig2 partition of parted_stmt_trig for values in (2);

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted2_stmt_trig (a int) partition by list (a);
--dg-cascade
--dg create table parted2_stmt_trig1 partition of parted2_stmt_trig for values in (1);
--dg-cascade
--dg create table parted2_stmt_trig2 partition of parted2_stmt_trig for values in (2);

--dg-skip: language "?" does not exist
--dg create or replace function trigger_notice() returns trigger as $$
--dg   begin
--dg     raise notice 'trigger % on % % % for %', TG_NAME, TG_TABLE_NAME, TG_WHEN, TG_OP, TG_LEVEL;
--dg     if TG_LEVEL = 'ROW' then
--dg        return NEW;
--dg     end if;
--dg     return null;
--dg   end;
--dg   $$ language plpgsql;

-- insert/update/delete statement-level triggers on the parent
--dg-cascade
--dg create trigger trig_ins_before before insert on parted_stmt_trig
--dg   for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_ins_after after insert on parted_stmt_trig
--dg   for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_upd_before before update on parted_stmt_trig
--dg   for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_upd_after after update on parted_stmt_trig
--dg   for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_del_before before delete on parted_stmt_trig
--dg   for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_del_after after delete on parted_stmt_trig
--dg   for each statement execute procedure trigger_notice();

-- insert/update/delete row-level triggers on the parent
--dg-cascade
--dg create trigger trig_ins_after_parent after insert on parted_stmt_trig
--dg   for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_upd_after_parent after update on parted_stmt_trig
--dg   for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_del_after_parent after delete on parted_stmt_trig
--dg   for each row execute procedure trigger_notice();

-- insert/update/delete row-level triggers on the first partition
--dg-cascade
--dg create trigger trig_ins_before_child before insert on parted_stmt_trig1
--dg   for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_ins_after_child after insert on parted_stmt_trig1
--dg   for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_upd_before_child before update on parted_stmt_trig1
--dg   for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_upd_after_child after update on parted_stmt_trig1
--dg   for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_del_before_child before delete on parted_stmt_trig1
--dg   for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_del_after_child after delete on parted_stmt_trig1
--dg   for each row execute procedure trigger_notice();

-- insert/update/delete statement-level triggers on the parent
--dg-cascade
--dg create trigger trig_ins_before_3 before insert on parted2_stmt_trig
--dg   for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_ins_after_3 after insert on parted2_stmt_trig
--dg   for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_upd_before_3 before update on parted2_stmt_trig
--dg   for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_upd_after_3 after update on parted2_stmt_trig
--dg   for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_del_before_3 before delete on parted2_stmt_trig
--dg   for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger trig_del_after_3 after delete on parted2_stmt_trig
--dg   for each statement execute procedure trigger_notice();

--dg-cascade
--dg with ins (a) as (
--dg   insert into parted2_stmt_trig values (1), (2) returning a
--dg ) insert into parted_stmt_trig select a from ins returning tableoid::regclass, a;

--dg-cascade
--dg with upd as (
--dg   update parted2_stmt_trig set a = a
--dg ) update parted_stmt_trig  set a = a;

--dg-cascade
--dg delete from parted_stmt_trig;

-- insert via copy on the parent
--dg-cascade
--dg copy parted_stmt_trig(a) from stdin;
--dg 1
--dg 2
--dg \.

-- insert via copy on the first partition
--dg-cascade
--dg copy parted_stmt_trig1(a) from stdin;
--dg 1
--dg \.

-- Disabling a trigger in the parent table should disable children triggers too
--dg-cascade
--dg alter table parted_stmt_trig disable trigger trig_ins_after_parent;
--dg-cascade
--dg insert into parted_stmt_trig values (1);
--dg-cascade
--dg alter table parted_stmt_trig enable trigger trig_ins_after_parent;
--dg-cascade
--dg insert into parted_stmt_trig values (1);

--dg-cascade
--dg drop table parted_stmt_trig, parted2_stmt_trig;

-- Verify that triggers fire in alphabetical order
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_trig (a int) partition by range (a);
--dg-cascade
--dg create table parted_trig_1 partition of parted_trig for values from (0) to (1000)
--dg    partition by range (a);
--dg-cascade
--dg create table parted_trig_1_1 partition of parted_trig_1 for values from (0) to (100);
--dg-cascade
--dg create table parted_trig_2 partition of parted_trig for values from (1000) to (2000);
--dg-cascade
--dg create trigger zzz after insert on parted_trig for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger mmm after insert on parted_trig_1_1 for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger aaa after insert on parted_trig_1 for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger bbb after insert on parted_trig for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger qqq after insert on parted_trig_1_1 for each row execute procedure trigger_notice();
--dg-cascade
--dg insert into parted_trig values (50), (1500);
--dg-cascade
--dg drop table parted_trig;

-- Verify that the correct triggers fire for cross-partition updates
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_trig (a int) partition by list (a);
--dg-cascade
--dg create table parted_trig1 partition of parted_trig for values in (1);
--dg-cascade
--dg create table parted_trig2 partition of parted_trig for values in (2);
--dg-cascade
--dg insert into parted_trig values (1);

--dg-skip: language "?" does not exist
--dg create or replace function trigger_notice() returns trigger as $$
--dg   begin
--dg     raise notice 'trigger % on % % % for %', TG_NAME, TG_TABLE_NAME, TG_WHEN, TG_OP, TG_LEVEL;
--dg     if TG_LEVEL = 'ROW' then
--dg       if TG_OP = 'DELETE' then
--dg         return OLD;
--dg       else
--dg         return NEW;
--dg       end if;
--dg     end if;
--dg     return null;
--dg   end;
--dg   $$ language plpgsql;
--dg-cascade
--dg create trigger parted_trig_before_stmt before insert or update or delete on parted_trig
--dg    for each statement execute procedure trigger_notice();
--dg-cascade
--dg create trigger parted_trig_before_row before insert or update or delete on parted_trig
--dg    for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger parted_trig_after_row after insert or update or delete on parted_trig
--dg    for each row execute procedure trigger_notice();
--dg-cascade
--dg create trigger parted_trig_after_stmt after insert or update or delete on parted_trig
--dg    for each statement execute procedure trigger_notice();

--dg-cascade
--dg update parted_trig set a = 2 where a = 1;

-- update action in merge should behave the same
--dg-cascade
--dg merge into parted_trig using (select 1) as ss on true
--dg   when matched and a = 2 then update set a = 1;

--dg-cascade
--dg drop table parted_trig;

-- Verify propagation of trigger arguments to partitions
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_trig (a int) partition by list (a);
--dg-cascade
--dg create table parted_trig1 partition of parted_trig for values in (1);
--dg-skip: language "?" does not exist
--dg create or replace function trigger_notice() returns trigger as $$
--dg   declare
--dg     arg1 text = TG_ARGV[0];
--dg     arg2 integer = TG_ARGV[1];
--dg   begin
--dg     raise notice 'trigger % on % % % for % args % %',
--dg 		TG_NAME, TG_TABLE_NAME, TG_WHEN, TG_OP, TG_LEVEL, arg1, arg2;
--dg     return null;
--dg   end;
--dg   $$ language plpgsql;
--dg-cascade
--dg create trigger aaa after insert on parted_trig
--dg    for each row execute procedure trigger_notice('quirky', 1);

-- Verify propagation of trigger arguments to partitions attached after creating trigger
--dg-cascade
--dg create table parted_trig2 partition of parted_trig for values in (2);
--dg-cascade
--dg create table parted_trig3 (like parted_trig);
--dg-cascade
--dg alter table parted_trig attach partition parted_trig3 for values in (3);
--dg-cascade
--dg insert into parted_trig values (1), (2), (3);
--dg-cascade
--dg drop table parted_trig;

-- test irregular partitions (i.e., different column definitions),
-- including that the WHEN clause works
--dg-skip: language "?" does not exist
--dg create function bark(text) returns bool language plpgsql immutable
--dg   as $$ begin raise notice '% <- woof!', $1; return true; end; $$;
--dg-skip: language "?" does not exist
--dg create or replace function trigger_notice_ab() returns trigger as $$
--dg   begin
--dg     raise notice 'trigger % on % % % for %: (a,b)=(%,%)',
--dg 		TG_NAME, TG_TABLE_NAME, TG_WHEN, TG_OP, TG_LEVEL,
--dg 		NEW.a, NEW.b;
--dg     if TG_LEVEL = 'ROW' then
--dg        return NEW;
--dg     end if;
--dg     return null;
--dg   end;
--dg   $$ language plpgsql;
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_irreg_ancestor (fd text, b text, fd2 int, fd3 int, a int)
--dg   partition by range (b);
--dg-cascade
--dg alter table parted_irreg_ancestor drop column fd,
--dg   drop column fd2, drop column fd3;
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_irreg (fd int, a int, fd2 int, b text)
--dg   partition by range (b);
--dg-cascade
--dg alter table parted_irreg drop column fd, drop column fd2;
--dg-cascade
--dg alter table parted_irreg_ancestor attach partition parted_irreg
--dg   for values from ('aaaa') to ('zzzz');
create table parted1_irreg (b text, fd int, a int);
alter table parted1_irreg drop column fd;
--dg-cascade
--dg alter table parted_irreg attach partition parted1_irreg
--dg   for values from ('aaaa') to ('bbbb');
--dg-cascade
--dg create trigger parted_trig after insert on parted_irreg
--dg   for each row execute procedure trigger_notice_ab();
--dg-cascade
--dg create trigger parted_trig_odd after insert on parted_irreg for each row
--dg   when (bark(new.b) AND new.a % 2 = 1) execute procedure trigger_notice_ab();
-- we should hear barking for every insert, but parted_trig_odd only emits
-- noise for odd values of a. parted_trig does it for all inserts.
--dg-cascade
--dg insert into parted_irreg values (1, 'aardvark'), (2, 'aanimals');
--dg-cascade
--dg insert into parted1_irreg values ('aardwolf', 2);
--dg-cascade
--dg insert into parted_irreg_ancestor values ('aasvogel', 3);
--dg-cascade
--dg drop table parted_irreg_ancestor;

-- Before triggers and partitions
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted (a int, b int, c text) partition by list (a);
--dg-cascade
--dg create table parted_1 partition of parted for values in (1)
--dg   partition by list (b);
--dg-cascade
--dg create table parted_1_1 partition of parted_1 for values in (1);
--dg-skip: language "?" does not exist
--dg create function parted_trigfunc() returns trigger language plpgsql as $$
--dg begin
--dg   new.a = new.a + 1;
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg insert into parted values (1, 1, 'uno uno v1');    -- works
--dg-cascade
--dg create trigger t before insert or update or delete on parted
--dg   for each row execute function parted_trigfunc();
--dg-cascade
--dg insert into parted values (1, 1, 'uno uno v2');    -- fail
--dg-cascade
--dg update parted set c = c || 'v3';                   -- fail
--dg-skip: language "?" does not exist
--dg create or replace function parted_trigfunc() returns trigger language plpgsql as $$
--dg begin
--dg   new.b = new.b + 1;
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg insert into parted values (1, 1, 'uno uno v4');    -- fail
--dg-cascade
--dg update parted set c = c || 'v5';                   -- fail
--dg-skip: language "?" does not exist
--dg create or replace function parted_trigfunc() returns trigger language plpgsql as $$
--dg begin
--dg   new.c = new.c || ' did '|| TG_OP;
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg insert into parted values (1, 1, 'uno uno');       -- works
--dg-cascade
--dg update parted set c = c || ' v6';                   -- works
--dg-cascade
--dg select tableoid::regclass, * from parted;

-- update itself moves tuple to new partition; trigger still works
--dg-cascade
--dg truncate table parted;
--dg-cascade
--dg create table parted_2 partition of parted for values in (2);
--dg-cascade
--dg insert into parted values (1, 1, 'uno uno v5');
--dg-cascade
--dg update parted set a = 2;
--dg-cascade
--dg select tableoid::regclass, * from parted;

-- both trigger and update change the partition
--dg-skip: language "?" does not exist
--dg create or replace function parted_trigfunc2() returns trigger language plpgsql as $$
--dg begin
--dg   new.a = new.a + 1;
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg create trigger t2 before update on parted
--dg   for each row execute function parted_trigfunc2();
--dg-cascade
--dg truncate table parted;
--dg-cascade
--dg insert into parted values (1, 1, 'uno uno v6');
--dg-cascade
--dg create table parted_3 partition of parted for values in (3);
--dg-cascade
--dg update parted set a = a + 1;
--dg-cascade
--dg select tableoid::regclass, * from parted;
-- there's no partition for a=0, but this update works anyway because
-- the trigger causes the tuple to be routed to another partition
--dg-cascade
--dg update parted set a = 0;
--dg-cascade
--dg select tableoid::regclass, * from parted;

--dg-cascade
--dg drop table parted;
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted (a int, b int, c text) partition by list ((a + b));
--dg-skip: language "?" does not exist
--dg create or replace function parted_trigfunc() returns trigger language plpgsql as $$
--dg begin
--dg   new.a = new.a + new.b;
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg create table parted_1 partition of parted for values in (1, 2);
--dg-cascade
--dg create table parted_2 partition of parted for values in (3, 4);
--dg-cascade
--dg create trigger t before insert or update on parted
--dg   for each row execute function parted_trigfunc();
--dg-cascade
--dg insert into parted values (0, 1, 'zero win');
--dg-cascade
--dg insert into parted values (1, 1, 'one fail');
--dg-cascade
--dg insert into parted values (1, 2, 'two fail');
--dg-cascade
--dg select * from parted;
--dg-cascade
--dg drop table parted;
--dg-cascade
--dg drop function parted_trigfunc();

-- Test exception handling in a deferred constraint trigger at COMMIT.
create table deferred_trigger_test (a int);
--dg-skip: language "?" does not exist
--dg create function deferred_trigger_func() returns trigger
--dg   language plpgsql as $$
--dg begin
--dg   perform 1 / 0;
--dg   return new;
--dg exception when division_by_zero then
--dg   raise notice 'caught division_by_zero';
--dg   return new;
--dg end;
--dg $$;
--dg-cascade
--dg create constraint trigger deferred_trigger
--dg   after insert on deferred_trigger_test
--dg   deferrable initially deferred
--dg   for each row execute function deferred_trigger_func();

begin;
insert into deferred_trigger_test values (1);
--dg-cascade
--dg commit;
select * from deferred_trigger_test;

drop table deferred_trigger_test;
--dg-cascade
--dg drop function deferred_trigger_func();

--
-- Constraint triggers and partitioned tables
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_constr_ancestor (a int, b text)
--dg   partition by range (b);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_constr (a int, b text)
--dg   partition by range (b);
--dg-cascade
--dg alter table parted_constr_ancestor attach partition parted_constr
--dg   for values from ('aaaa') to ('zzzz');
--dg-cascade
--dg create table parted1_constr (a int, b text);
--dg-cascade
--dg alter table parted_constr attach partition parted1_constr
--dg   for values from ('aaaa') to ('bbbb');
--dg-cascade
--dg create constraint trigger parted_trig after insert on parted_constr_ancestor
--dg   deferrable
--dg   for each row execute procedure trigger_notice_ab();
--dg-cascade
--dg create constraint trigger parted_trig_two after insert on parted_constr
--dg   deferrable initially deferred
--dg   for each row when (bark(new.b) AND new.a % 2 = 1)
--dg   execute procedure trigger_notice_ab();

-- The immediate constraint is fired immediately; the WHEN clause of the
-- deferred constraint is also called immediately.  The deferred constraint
-- is fired at commit time.
--dg-cascade
--dg begin;
--dg-cascade
--dg insert into parted_constr values (1, 'aardvark');
--dg-cascade
--dg insert into parted1_constr values (2, 'aardwolf');
--dg-cascade
--dg insert into parted_constr_ancestor values (3, 'aasvogel');
--dg-cascade
--dg commit;

-- The WHEN clause is immediate, and both constraint triggers are fired at
-- commit time.
--dg-cascade
--dg begin;
--dg-cascade
--dg set constraints parted_trig deferred;
--dg-cascade
--dg insert into parted_constr values (1, 'aardvark');
--dg-cascade
--dg insert into parted1_constr values (2, 'aardwolf'), (3, 'aasvogel');
--dg-cascade
--dg commit;
--dg-cascade
--dg drop table parted_constr_ancestor;
--dg-cascade
--dg drop function bark(text);

-- Test that the WHEN clause is set properly to partitions
--dg-cascade
--dg create table parted_trigger (a int, b text) partition by range (a);
--dg-cascade
--dg create table parted_trigger_1 partition of parted_trigger for values from (0) to (1000);
--dg-cascade
--dg create table parted_trigger_2 (drp int, a int, b text);
--dg-cascade
--dg alter table parted_trigger_2 drop column drp;
--dg-cascade
--dg alter table parted_trigger attach partition parted_trigger_2 for values from (1000) to (2000);
--dg-cascade
--dg create trigger parted_trigger after update on parted_trigger
--dg   for each row when (new.a % 2 = 1 and length(old.b) >= 2) execute procedure trigger_notice_ab();
--dg-cascade
--dg create table parted_trigger_3 (b text, a int) partition by range (length(b));
--dg-cascade
--dg create table parted_trigger_3_1 partition of parted_trigger_3 for values from (1) to (3);
--dg-cascade
--dg create table parted_trigger_3_2 partition of parted_trigger_3 for values from (3) to (5);
--dg-cascade
--dg alter table parted_trigger attach partition parted_trigger_3 for values from (2000) to (3000);
--dg-cascade
--dg insert into parted_trigger values
--dg     (0, 'a'), (1, 'bbb'), (2, 'bcd'), (3, 'c'),
--dg 	(1000, 'c'), (1001, 'ddd'), (1002, 'efg'), (1003, 'f'),
--dg 	(2000, 'e'), (2001, 'fff'), (2002, 'ghi'), (2003, 'h');
--dg-cascade
--dg update parted_trigger set a = a + 2; -- notice for odd 'a' values, long 'b' values
--dg-cascade
--dg drop table parted_trigger;

-- try a constraint trigger, also
--dg-cascade
--dg create table parted_referenced (a int);
--dg-cascade
--dg create table unparted_trigger (a int, b text);	-- for comparison purposes
--dg-cascade
--dg create table parted_trigger (a int, b text) partition by range (a);
--dg-cascade
--dg create table parted_trigger_1 partition of parted_trigger for values from (0) to (1000);
--dg-cascade
--dg create table parted_trigger_2 (drp int, a int, b text);
--dg-cascade
--dg alter table parted_trigger_2 drop column drp;
--dg-cascade
--dg alter table parted_trigger attach partition parted_trigger_2 for values from (1000) to (2000);
--dg-cascade
--dg create constraint trigger parted_trigger after update on parted_trigger
--dg   from parted_referenced
--dg   for each row execute procedure trigger_notice_ab();
--dg-cascade
--dg create constraint trigger parted_trigger after update on unparted_trigger
--dg   from parted_referenced
--dg   for each row execute procedure trigger_notice_ab();
--dg-cascade
--dg create table parted_trigger_3 (b text, a int) partition by range (length(b));
--dg-cascade
--dg create table parted_trigger_3_1 partition of parted_trigger_3 for values from (1) to (3);
--dg-cascade
--dg create table parted_trigger_3_2 partition of parted_trigger_3 for values from (3) to (5);
--dg-cascade
--dg alter table parted_trigger attach partition parted_trigger_3 for values from (2000) to (3000);
--dg-cascade
--dg select tgname, conname, t.tgrelid::regclass, t.tgconstrrelid::regclass,
--dg   c.conrelid::regclass, c.confrelid::regclass
--dg   from pg_trigger t join pg_constraint c on (t.tgconstraint = c.oid)
--dg   where tgname = 'parted_trigger'
--dg   order by t.tgrelid::regclass::text;
--dg-cascade
--dg drop table parted_referenced, parted_trigger, unparted_trigger;

-- verify that the "AFTER UPDATE OF columns" event is propagated correctly
--dg-cascade
--dg create table parted_trigger (a int, b text) partition by range (a);
--dg-cascade
--dg create table parted_trigger_1 partition of parted_trigger for values from (0) to (1000);
--dg-cascade
--dg create table parted_trigger_2 (drp int, a int, b text);
--dg-cascade
--dg alter table parted_trigger_2 drop column drp;
--dg-cascade
--dg alter table parted_trigger attach partition parted_trigger_2 for values from (1000) to (2000);
--dg-cascade
--dg create trigger parted_trigger after update of b on parted_trigger
--dg   for each row execute procedure trigger_notice_ab();
--dg-cascade
--dg create table parted_trigger_3 (b text, a int) partition by range (length(b));
--dg-cascade
--dg create table parted_trigger_3_1 partition of parted_trigger_3 for values from (1) to (4);
--dg-cascade
--dg create table parted_trigger_3_2 partition of parted_trigger_3 for values from (4) to (8);
--dg-cascade
--dg alter table parted_trigger attach partition parted_trigger_3 for values from (2000) to (3000);
--dg-cascade
--dg insert into parted_trigger values (0, 'a'), (1000, 'c'), (2000, 'e'), (2001, 'eeee');
--dg-cascade
--dg update parted_trigger set a = a + 2;	-- no notices here
--dg-cascade
--dg update parted_trigger set b = b || 'b';	-- all triggers should fire
--dg-cascade
--dg drop table parted_trigger;

--dg-cascade
--dg drop function trigger_notice_ab();

-- Make sure we don't end up with unnecessary copies of triggers, when
-- cloning them.
--dg-cascade
--dg create table trg_clone (a int) partition by range (a);
--dg-cascade
--dg create table trg_clone1 partition of trg_clone for values from (0) to (1000);
--dg-cascade
--dg alter table trg_clone add constraint uniq unique (a) deferrable;
--dg-cascade
--dg create table trg_clone2 partition of trg_clone for values from (1000) to (2000);
--dg-cascade
--dg create table trg_clone3 partition of trg_clone for values from (2000) to (3000)
--dg   partition by range (a);
--dg-cascade
--dg create table trg_clone_3_3 partition of trg_clone3 for values from (2000) to (2100);
--dg-cascade
--dg select tgrelid::regclass, count(*) from pg_trigger
--dg   where tgrelid::regclass in ('trg_clone', 'trg_clone1', 'trg_clone2',
--dg 	'trg_clone3', 'trg_clone_3_3')
--dg   group by tgrelid::regclass order by tgrelid::regclass;
--dg-cascade
--dg drop table trg_clone;

-- Test the interaction between ALTER TABLE .. DISABLE TRIGGER and
-- both kinds of inheritance.  Historically, legacy inheritance has
-- not recursed to children, so that behavior is preserved.
--dg-cascade
--dg create table parent (a int);
--dg-cascade
--dg create table child1 () inherits (parent);
--dg-skip: language "?" does not exist
--dg create function trig_nothing() returns trigger language plpgsql
--dg   as $$ begin return null; end $$;
--dg-cascade
--dg create trigger tg after insert on parent
--dg   for each row execute function trig_nothing();
--dg-cascade
--dg create trigger tg after insert on child1
--dg   for each row execute function trig_nothing();
--dg-cascade
--dg alter table parent disable trigger tg;
--dg-cascade
--dg select tgrelid::regclass, tgname, tgenabled from pg_trigger
--dg   where tgrelid in ('parent'::regclass, 'child1'::regclass)
--dg   order by tgrelid::regclass::text;
--dg-cascade
--dg alter table only parent enable always trigger tg;
--dg-cascade
--dg select tgrelid::regclass, tgname, tgenabled from pg_trigger
--dg   where tgrelid in ('parent'::regclass, 'child1'::regclass)
--dg   order by tgrelid::regclass::text;
--dg-cascade
--dg drop table parent, child1;

--dg-cascade
--dg create table parent (a int) partition by list (a);
--dg-cascade
--dg create table child1 partition of parent for values in (1);
--dg-cascade
--dg create trigger tg after insert on parent
--dg   for each row execute procedure trig_nothing();
--dg-cascade
--dg create trigger tg_stmt after insert on parent
--dg   for statement execute procedure trig_nothing();
--dg-cascade
--dg select tgrelid::regclass, tgname, tgenabled from pg_trigger
--dg   where tgrelid in ('parent'::regclass, 'child1'::regclass)
--dg   order by tgrelid::regclass::text, tgname;
--dg-cascade
--dg alter table only parent enable always trigger tg;	-- no recursion because ONLY
--dg-cascade
--dg alter table parent enable always trigger tg_stmt;	-- no recursion because statement trigger
--dg-cascade
--dg select tgrelid::regclass, tgname, tgenabled from pg_trigger
--dg   where tgrelid in ('parent'::regclass, 'child1'::regclass)
--dg   order by tgrelid::regclass::text, tgname;
-- The following is a no-op for the parent trigger but not so
-- for the child trigger, so recursion should be applied.
--dg-cascade
--dg alter table parent enable always trigger tg;
--dg-cascade
--dg select tgrelid::regclass, tgname, tgenabled from pg_trigger
--dg   where tgrelid in ('parent'::regclass, 'child1'::regclass)
--dg   order by tgrelid::regclass::text, tgname;
-- This variant malfunctioned in some releases.
--dg-cascade
--dg alter table parent disable trigger user;
--dg-cascade
--dg select tgrelid::regclass, tgname, tgenabled from pg_trigger
--dg   where tgrelid in ('parent'::regclass, 'child1'::regclass)
--dg   order by tgrelid::regclass::text, tgname;
--dg-cascade
--dg drop table parent, child1;

-- Check processing of foreign key triggers
--dg-cascade
--dg create table parent (a int primary key, f int references parent)
--dg   partition by list (a);
--dg-cascade
--dg create table child1 partition of parent for values in (1);
--dg-cascade
--dg select tgrelid::regclass, rtrim(tgname, '0123456789') as tgname,
--dg   tgfoid::regproc, tgenabled
--dg   from pg_trigger where tgrelid in ('parent'::regclass, 'child1'::regclass)
--dg   order by tgrelid::regclass::text, tgfoid;
--dg-cascade
--dg alter table parent disable trigger all;
--dg-cascade
--dg select tgrelid::regclass, rtrim(tgname, '0123456789') as tgname,
--dg   tgfoid::regproc, tgenabled
--dg   from pg_trigger where tgrelid in ('parent'::regclass, 'child1'::regclass)
--dg   order by tgrelid::regclass::text, tgfoid;
--dg-cascade
--dg drop table parent, child1;

-- Verify that firing state propagates correctly on creation, too
--dg-cascade
--dg CREATE TABLE trgfire (i int) PARTITION BY RANGE (i);
--dg-cascade
--dg CREATE TABLE trgfire1 PARTITION OF trgfire FOR VALUES FROM (1) TO (10);
--dg-skip: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION tgf() RETURNS trigger LANGUAGE plpgsql
--dg   AS $$ begin raise exception 'except'; end $$;
--dg-cascade
--dg CREATE TRIGGER tg AFTER INSERT ON trgfire FOR EACH ROW EXECUTE FUNCTION tgf();
--dg-cascade
--dg INSERT INTO trgfire VALUES (1);
--dg-cascade
--dg ALTER TABLE trgfire DISABLE TRIGGER tg;
--dg-cascade
--dg INSERT INTO trgfire VALUES (1);
--dg-cascade
--dg CREATE TABLE trgfire2 PARTITION OF trgfire FOR VALUES FROM (10) TO (20);
--dg-cascade
--dg INSERT INTO trgfire VALUES (11);
--dg-cascade
--dg CREATE TABLE trgfire3 (LIKE trgfire);
--dg-cascade
--dg ALTER TABLE trgfire ATTACH PARTITION trgfire3 FOR VALUES FROM (20) TO (30);
--dg-cascade
--dg INSERT INTO trgfire VALUES (21);
--dg-cascade
--dg CREATE TABLE trgfire4 PARTITION OF trgfire FOR VALUES FROM (30) TO (40) PARTITION BY LIST (i);
--dg-cascade
--dg CREATE TABLE trgfire4_30 PARTITION OF trgfire4 FOR VALUES IN (30);
--dg-cascade
--dg INSERT INTO trgfire VALUES (30);
--dg-cascade
--dg CREATE TABLE trgfire5 (LIKE trgfire) PARTITION BY LIST (i);
--dg-cascade
--dg CREATE TABLE trgfire5_40 PARTITION OF trgfire5 FOR VALUES IN (40);
--dg-cascade
--dg ALTER TABLE trgfire ATTACH PARTITION trgfire5 FOR VALUES FROM (40) TO (50);
--dg-cascade
--dg INSERT INTO trgfire VALUES (40);
--dg-cascade
--dg SELECT tgrelid::regclass, tgenabled FROM pg_trigger
--dg   WHERE tgrelid::regclass IN (SELECT oid from pg_class where relname LIKE 'trgfire%')
--dg   ORDER BY tgrelid::regclass::text;
--dg-cascade
--dg ALTER TABLE trgfire ENABLE TRIGGER tg;
--dg-cascade
--dg INSERT INTO trgfire VALUES (1);
--dg-cascade
--dg INSERT INTO trgfire VALUES (11);
--dg-cascade
--dg INSERT INTO trgfire VALUES (21);
--dg-cascade
--dg INSERT INTO trgfire VALUES (30);
--dg-cascade
--dg INSERT INTO trgfire VALUES (40);
--dg-cascade
--dg DROP TABLE trgfire;
--dg-cascade
--dg DROP FUNCTION tgf();

--
-- Test the interaction between transition tables and both kinds of
-- inheritance.  We'll dump the contents of the transition tables in a
-- format that shows the attribute order, so that we can distinguish
-- tuple formats (though not dropped attributes).
--

--dg-skip: language "?" does not exist
--dg create or replace function dump_insert() returns trigger language plpgsql as
--dg $$
--dg   begin
--dg     raise notice 'trigger = %, new table = %',
--dg                  TG_NAME,
--dg                  (select string_agg(new_table::text, ', ' order by a) from new_table);
--dg     return null;
--dg   end;
--dg $$;

--dg-skip: language "?" does not exist
--dg create or replace function dump_update() returns trigger language plpgsql as
--dg $$
--dg   begin
--dg     raise notice 'trigger = %, old table = %, new table = %',
--dg                  TG_NAME,
--dg                  (select string_agg(old_table::text, ', ' order by a) from old_table),
--dg                  (select string_agg(new_table::text, ', ' order by a) from new_table);
--dg     return null;
--dg   end;
--dg $$;

--dg-skip: language "?" does not exist
--dg create or replace function dump_delete() returns trigger language plpgsql as
--dg $$
--dg   begin
--dg     raise notice 'trigger = %, old table = %',
--dg                  TG_NAME,
--dg                  (select string_agg(old_table::text, ', ' order by a) from old_table);
--dg     return null;
--dg   end;
--dg $$;

--
-- Verify behavior of statement triggers on partition hierarchy with
-- transition tables.  Tuples should appear to each trigger in the
-- format of the relation the trigger is attached to.
--

-- set up a partition hierarchy with some different TupleDescriptors
--dg-cascade
--dg create table parent (a text, b int) partition by list (a);

-- a child matching parent
--dg-cascade
--dg create table child1 partition of parent for values in ('AAA');

-- a child with a dropped column
--dg-cascade
--dg create table child2 (x int, a text, b int);
--dg-cascade
--dg alter table child2 drop column x;
--dg-cascade
--dg alter table parent attach partition child2 for values in ('BBB');

-- a child with a different column order
--dg-cascade
--dg create table child3 (b int, a text);
--dg-cascade
--dg alter table parent attach partition child3 for values in ('CCC');

--dg-cascade
--dg create trigger parent_insert_trig
--dg   after insert on parent referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger parent_update_trig
--dg   after update on parent referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();
--dg-cascade
--dg create trigger parent_delete_trig
--dg   after delete on parent referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

--dg-cascade
--dg create trigger child1_insert_trig
--dg   after insert on child1 referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger child1_update_trig
--dg   after update on child1 referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();
--dg-cascade
--dg create trigger child1_delete_trig
--dg   after delete on child1 referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

--dg-cascade
--dg create trigger child2_insert_trig
--dg   after insert on child2 referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger child2_update_trig
--dg   after update on child2 referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();
--dg-cascade
--dg create trigger child2_delete_trig
--dg   after delete on child2 referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

--dg-cascade
--dg create trigger child3_insert_trig
--dg   after insert on child3 referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger child3_update_trig
--dg   after update on child3 referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();
--dg-cascade
--dg create trigger child3_delete_trig
--dg   after delete on child3 referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

--dg-cascade
--dg SELECT trigger_name, event_manipulation, event_object_schema, event_object_table,
--dg        action_order, action_condition, action_orientation, action_timing,
--dg        action_reference_old_table, action_reference_new_table
--dg   FROM information_schema.triggers
--dg   WHERE event_object_table IN ('parent', 'child1', 'child2', 'child3')
--dg   ORDER BY trigger_name COLLATE "C", 2;

-- insert directly into children sees respective child-format tuples
--dg-cascade
--dg insert into child1 values ('AAA', 42);
--dg-cascade
--dg insert into child2 values ('BBB', 42);
--dg-cascade
--dg insert into child3 values (42, 'CCC');

-- update via parent sees parent-format tuples
--dg-cascade
--dg update parent set b = b + 1;

-- delete via parent sees parent-format tuples
--dg-cascade
--dg delete from parent;

-- insert into parent sees parent-format tuples
--dg-cascade
--dg insert into parent values ('AAA', 42);
--dg-cascade
--dg insert into parent values ('BBB', 42);
--dg-cascade
--dg insert into parent values ('CCC', 42);

-- delete from children sees respective child-format tuples
--dg-cascade
--dg delete from child1;
--dg-cascade
--dg delete from child2;
--dg-cascade
--dg delete from child3;

-- copy into parent sees parent-format tuples
--dg-cascade
--dg copy parent (a, b) from stdin;
--dg AAA	42
--dg BBB	42
--dg CCC	42
--dg \.

-- check detach/reattach behavior; statement triggers with transition tables
-- should not prevent a table from becoming a partition again
--dg-cascade
--dg alter table parent detach partition child1;
--dg-cascade
--dg alter table parent attach partition child1 for values in ('AAA');

-- DML affecting parent sees tuples collected from children even if
-- there is no transition table trigger on the children
--dg-cascade
--dg drop trigger child1_insert_trig on child1;
--dg-cascade
--dg drop trigger child1_update_trig on child1;
--dg-cascade
--dg drop trigger child1_delete_trig on child1;
--dg-cascade
--dg drop trigger child2_insert_trig on child2;
--dg-cascade
--dg drop trigger child2_update_trig on child2;
--dg-cascade
--dg drop trigger child2_delete_trig on child2;
--dg-cascade
--dg drop trigger child3_insert_trig on child3;
--dg-cascade
--dg drop trigger child3_update_trig on child3;
--dg-cascade
--dg drop trigger child3_delete_trig on child3;
--dg-cascade
--dg delete from parent;

-- copy into parent sees tuples collected from children even if there
-- is no transition-table trigger on the children
--dg-cascade
--dg copy parent (a, b) from stdin;
--dg AAA	42
--dg BBB	42
--dg CCC	42
--dg \.

-- insert into parent with a before trigger on a child tuple before
-- insertion, and we capture the newly modified row in parent format
--dg-skip: language "?" does not exist
--dg create or replace function intercept_insert() returns trigger language plpgsql as
--dg $$
--dg   begin
--dg     new.b = new.b + 1000;
--dg     return new;
--dg   end;
--dg $$;

--dg-cascade
--dg create trigger intercept_insert_child3
--dg   before insert on child3
--dg   for each row execute procedure intercept_insert();


-- insert, parent trigger sees post-modification parent-format tuple
--dg-cascade
--dg insert into parent values ('AAA', 42), ('BBB', 42), ('CCC', 66);

-- copy, parent trigger sees post-modification parent-format tuple
--dg-cascade
--dg copy parent (a, b) from stdin;
--dg AAA	42
--dg BBB	42
--dg CCC	234
--dg \.

--dg-cascade
--dg drop table child1, child2, child3, parent;
--dg-cascade
--dg drop function intercept_insert();

--
-- Verify prohibition of row triggers with transition triggers on
-- partitions
--
--dg-cascade
--dg create table parent (a text, b int) partition by list (a);
--dg-cascade
--dg create table child partition of parent for values in ('AAA');

-- adding row trigger with transition table fails
--dg-cascade
--dg create trigger child_row_trig
--dg   after insert on child referencing new table as new_table
--dg   for each row execute procedure dump_insert();

-- detaching it first works
--dg-cascade
--dg alter table parent detach partition child;

--dg-cascade
--dg create trigger child_row_trig
--dg   after insert on child referencing new table as new_table
--dg   for each row execute procedure dump_insert();

-- but now we're not allowed to reattach it
--dg-cascade
--dg alter table parent attach partition child for values in ('AAA');

-- drop the trigger, and now we're allowed to attach it again
--dg-cascade
--dg drop trigger child_row_trig on child;
--dg-cascade
--dg alter table parent attach partition child for values in ('AAA');

--dg-cascade
--dg drop table child, parent;

--
-- Verify access of transition tables with UPDATE triggers and tuples
-- moved across partitions.
--
--dg-skip: language "?" does not exist
--dg create or replace function dump_update_new() returns trigger language plpgsql as
--dg $$
--dg   begin
--dg     raise notice 'trigger = %, new table = %', TG_NAME,
--dg                  (select string_agg(new_table::text, ', ' order by a) from new_table);
--dg     return null;
--dg   end;
--dg $$;
--dg-skip: language "?" does not exist
--dg create or replace function dump_update_old() returns trigger language plpgsql as
--dg $$
--dg   begin
--dg     raise notice 'trigger = %, old table = %', TG_NAME,
--dg                  (select string_agg(old_table::text, ', ' order by a) from old_table);
--dg     return null;
--dg   end;
--dg $$;
--dg-cascade
--dg create table trans_tab_parent (a text) partition by list (a);
--dg-cascade
--dg create table trans_tab_child1 partition of trans_tab_parent for values in ('AAA1', 'AAA2');
--dg-cascade
--dg create table trans_tab_child2 partition of trans_tab_parent for values in ('BBB1', 'BBB2');
--dg-cascade
--dg create trigger trans_tab_parent_update_trig
--dg   after update on trans_tab_parent referencing old table as old_table
--dg   for each statement execute procedure dump_update_old();
--dg-cascade
--dg create trigger trans_tab_parent_insert_trig
--dg   after insert on trans_tab_parent referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger trans_tab_parent_delete_trig
--dg   after delete on trans_tab_parent referencing old table as old_table
--dg   for each statement execute procedure dump_delete();
--dg-cascade
--dg insert into trans_tab_parent values ('AAA1'), ('BBB1');
-- should not trigger access to new table when moving across partitions.
--dg-cascade
--dg update trans_tab_parent set a = 'BBB2' where a = 'AAA1';
--dg-cascade
--dg drop trigger trans_tab_parent_update_trig on trans_tab_parent;
--dg-cascade
--dg create trigger trans_tab_parent_update_trig
--dg   after update on trans_tab_parent referencing new table as new_table
--dg   for each statement execute procedure dump_update_new();
-- should not trigger access to old table when moving across partitions.
--dg-cascade
--dg update trans_tab_parent set a = 'AAA2' where a = 'BBB1';
--dg-cascade
--dg delete from trans_tab_parent;
-- clean up
--dg-cascade
--dg drop table trans_tab_parent, trans_tab_child1, trans_tab_child2;
--dg-cascade
--dg drop function dump_update_new, dump_update_old;

--
-- Verify behavior of statement triggers on (non-partition)
-- inheritance hierarchy with transition tables; similar to the
-- partition case, except there is no rerouting on insertion and child
-- tables can have extra columns
--

-- set up inheritance hierarchy with different TupleDescriptors
--dg-cascade
--dg create table parent (a text, b int);

-- a child matching parent
--dg-cascade
--dg create table child1 () inherits (parent);

-- a child with a different column order
--dg-cascade
--dg create table child2 (b int, a text);
--dg-cascade
--dg alter table child2 inherit parent;

-- a child with an extra column
--dg-cascade
--dg create table child3 (c text) inherits (parent);

--dg-cascade
--dg create trigger parent_insert_trig
--dg   after insert on parent referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger parent_update_trig
--dg   after update on parent referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();
--dg-cascade
--dg create trigger parent_delete_trig
--dg   after delete on parent referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

--dg-cascade
--dg create trigger child1_insert_trig
--dg   after insert on child1 referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger child1_update_trig
--dg   after update on child1 referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();
--dg-cascade
--dg create trigger child1_delete_trig
--dg   after delete on child1 referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

--dg-cascade
--dg create trigger child2_insert_trig
--dg   after insert on child2 referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger child2_update_trig
--dg   after update on child2 referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();
--dg-cascade
--dg create trigger child2_delete_trig
--dg   after delete on child2 referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

--dg-cascade
--dg create trigger child3_insert_trig
--dg   after insert on child3 referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger child3_update_trig
--dg   after update on child3 referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();
--dg-cascade
--dg create trigger child3_delete_trig
--dg   after delete on child3 referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

-- insert directly into children sees respective child-format tuples
--dg-cascade
--dg insert into child1 values ('AAA', 42);
--dg-cascade
--dg insert into child2 values (42, 'BBB');
--dg-cascade
--dg insert into child3 values ('CCC', 42, 'foo');

-- update via parent sees parent-format tuples
--dg-cascade
--dg update parent set b = b + 1;

-- delete via parent sees parent-format tuples
--dg-cascade
--dg delete from parent;

-- reinsert values into children for next test...
--dg-cascade
--dg insert into child1 values ('AAA', 42);
--dg-cascade
--dg insert into child2 values (42, 'BBB');
--dg-cascade
--dg insert into child3 values ('CCC', 42, 'foo');

-- delete from children sees respective child-format tuples
--dg-cascade
--dg delete from child1;
--dg-cascade
--dg delete from child2;
--dg-cascade
--dg delete from child3;

-- copy into parent sees parent-format tuples (no rerouting, so these
-- are really inserted into the parent)
--dg-cascade
--dg copy parent (a, b) from stdin;
--dg AAA	42
--dg BBB	42
--dg CCC	42
--dg \.

-- same behavior for copy if there is an index (interesting because rows are
-- captured by a different code path in copyfrom.c if there are indexes)
--dg-cascade
--dg create index on parent(b);
--dg-cascade
--dg copy parent (a, b) from stdin;
--dg DDD	42
--dg \.

-- check disinherit/reinherit behavior; statement triggers with transition
-- tables should not prevent a table from becoming an inheritance child again
--dg-cascade
--dg alter table child1 no inherit parent;
--dg-cascade
--dg alter table child1 inherit parent;

-- DML affecting parent sees tuples collected from children even if
-- there is no transition table trigger on the children
--dg-cascade
--dg drop trigger child1_insert_trig on child1;
--dg-cascade
--dg drop trigger child1_update_trig on child1;
--dg-cascade
--dg drop trigger child1_delete_trig on child1;
--dg-cascade
--dg drop trigger child2_insert_trig on child2;
--dg-cascade
--dg drop trigger child2_update_trig on child2;
--dg-cascade
--dg drop trigger child2_delete_trig on child2;
--dg-cascade
--dg drop trigger child3_insert_trig on child3;
--dg-cascade
--dg drop trigger child3_update_trig on child3;
--dg-cascade
--dg drop trigger child3_delete_trig on child3;
--dg-cascade
--dg delete from parent;

--dg-cascade
--dg drop table child1, child2, child3, parent;

--
-- Verify prohibition of row triggers with transition triggers on
-- inheritance children
--
--dg-cascade
--dg create table parent (a text, b int);
--dg-cascade
--dg create table child () inherits (parent);

-- adding row trigger with transition table fails
--dg-cascade
--dg create trigger child_row_trig
--dg   after insert on child referencing new table as new_table
--dg   for each row execute procedure dump_insert();

-- disinheriting it first works
--dg-cascade
--dg alter table child no inherit parent;

--dg-cascade
--dg create trigger child_row_trig
--dg   after insert on child referencing new table as new_table
--dg   for each row execute procedure dump_insert();

-- but now we're not allowed to make it inherit anymore
--dg-cascade
--dg alter table child inherit parent;

-- drop the trigger, and now we're allowed to make it inherit again
--dg-cascade
--dg drop trigger child_row_trig on child;
--dg-cascade
--dg alter table child inherit parent;

--dg-cascade
--dg drop table child, parent;

--
-- Verify behavior of queries with wCTEs, where multiple transition
-- tuplestores can be active at the same time because there are
-- multiple DML statements that might fire triggers with transition
-- tables
--
--dg-cascade
--dg create table table1 (a int);
--dg-cascade
--dg create table table2 (a text);
--dg-cascade
--dg create trigger table1_trig
--dg   after insert on table1 referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger table2_trig
--dg   after insert on table2 referencing new table as new_table
--dg   for each statement execute procedure dump_insert();

--dg-cascade
--dg with wcte as (insert into table1 values (42))
--dg   insert into table2 values ('hello world');

--dg-cascade
--dg with wcte as (insert into table1 values (43))
--dg   insert into table1 values (44);

--dg-cascade
--dg with wcte as (insert into table1 values (45))
--dg   merge into table1 using (values (46)) as v(a) on table1.a = v.a
--dg     when not matched then insert values (v.a);

--dg-cascade
--dg select * from table1;
--dg-cascade
--dg select * from table2;

--dg-cascade
--dg drop table table1;
--dg-cascade
--dg drop table table2;

--
-- Verify behavior of INSERT ... ON CONFLICT DO UPDATE ... with
-- transition tables.
--

--dg-cascade
--dg create table my_table (a int primary key, b text);
--dg-cascade
--dg create trigger my_table_insert_trig
--dg   after insert on my_table referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger my_table_update_trig
--dg   after update on my_table referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();

-- inserts only
--dg-cascade
--dg insert into my_table values (1, 'AAA'), (2, 'BBB')
--dg   on conflict (a) do
--dg   update set b = my_table.b || ':' || excluded.b;

-- mixture of inserts and updates
--dg-cascade
--dg insert into my_table values (1, 'AAA'), (2, 'BBB'), (3, 'CCC'), (4, 'DDD')
--dg   on conflict (a) do
--dg   update set b = my_table.b || ':' || excluded.b;

-- updates only
--dg-cascade
--dg insert into my_table values (3, 'CCC'), (4, 'DDD')
--dg   on conflict (a) do
--dg   update set b = my_table.b || ':' || excluded.b;

--
-- now using a partitioned table
--

--dg-cascade
--dg create table iocdu_tt_parted (a int primary key, b text) partition by list (a);
--dg-cascade
--dg create table iocdu_tt_parted1 partition of iocdu_tt_parted for values in (1);
--dg-cascade
--dg create table iocdu_tt_parted2 partition of iocdu_tt_parted for values in (2);
--dg-cascade
--dg create table iocdu_tt_parted3 partition of iocdu_tt_parted for values in (3);
--dg-cascade
--dg create table iocdu_tt_parted4 partition of iocdu_tt_parted for values in (4);
--dg-cascade
--dg create trigger iocdu_tt_parted_insert_trig
--dg   after insert on iocdu_tt_parted referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger iocdu_tt_parted_update_trig
--dg   after update on iocdu_tt_parted referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();

-- inserts only
--dg-cascade
--dg insert into iocdu_tt_parted values (1, 'AAA'), (2, 'BBB')
--dg   on conflict (a) do
--dg   update set b = iocdu_tt_parted.b || ':' || excluded.b;

-- mixture of inserts and updates
--dg-cascade
--dg insert into iocdu_tt_parted values (1, 'AAA'), (2, 'BBB'), (3, 'CCC'), (4, 'DDD')
--dg   on conflict (a) do
--dg   update set b = iocdu_tt_parted.b || ':' || excluded.b;

-- updates only
--dg-cascade
--dg insert into iocdu_tt_parted values (3, 'CCC'), (4, 'DDD')
--dg   on conflict (a) do
--dg   update set b = iocdu_tt_parted.b || ':' || excluded.b;

--dg-cascade
--dg drop table iocdu_tt_parted;

--
-- Verify that you can't create a trigger with transition tables for
-- more than one event.
--

--dg-cascade
--dg create trigger my_table_multievent_trig
--dg   after insert or update on my_table referencing new table as new_table
--dg   for each statement execute procedure dump_insert();

--
-- Verify that you can't create a trigger with transition tables with
-- a column list.
--

--dg-cascade
--dg create trigger my_table_col_update_trig
--dg   after update of b on my_table referencing new table as new_table
--dg   for each statement execute procedure dump_insert();

--dg-cascade
--dg drop table my_table;

--
-- Verify that transition tables can't be used in, eg, a view.
--

--dg-cascade
--dg create table my_table (a int);
--dg-skip: language "?" does not exist
--dg create function make_bogus_matview() returns trigger as
--dg $$ begin
--dg   create materialized view transition_test_mv as select * from new_table;
--dg   return new;
--dg end $$
--dg language plpgsql;
--dg-cascade
--dg create trigger make_bogus_matview
--dg   after insert on my_table
--dg   referencing new table as new_table
--dg   for each statement execute function make_bogus_matview();
--dg-cascade
--dg insert into my_table values (42);  -- error
--dg-cascade
--dg drop table my_table;
--dg-cascade
--dg drop function make_bogus_matview();

--
-- Test firing of triggers with transition tables by foreign key cascades
--

--dg-cascade
--dg create table refd_table (a int primary key, b text);
--dg-cascade
--dg create table trig_table (a int, b text,
--dg   foreign key (a) references refd_table on update cascade on delete cascade
--dg );

--dg-cascade
--dg create trigger trig_table_before_trig
--dg   before insert or update or delete on trig_table
--dg   for each statement execute procedure trigger_func('trig_table');
--dg-cascade
--dg create trigger trig_table_insert_trig
--dg   after insert on trig_table referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger trig_table_update_trig
--dg   after update on trig_table referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();
--dg-cascade
--dg create trigger trig_table_delete_trig
--dg   after delete on trig_table referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

--dg-cascade
--dg insert into refd_table values
--dg   (1, 'one'),
--dg   (2, 'two'),
--dg   (3, 'three');
--dg-cascade
--dg insert into trig_table values
--dg   (1, 'one a'),
--dg   (1, 'one b'),
--dg   (2, 'two a'),
--dg   (2, 'two b'),
--dg   (3, 'three a'),
--dg   (3, 'three b');

--dg-cascade
--dg update refd_table set a = 11 where b = 'one';

--dg-cascade
--dg select * from trig_table;

--dg-cascade
--dg delete from refd_table where length(b) = 3;

--dg-cascade
--dg select * from trig_table;

--dg-cascade
--dg drop table refd_table, trig_table;

--
-- Test that we can drop a not-yet-fired deferred trigger
--

--dg-cascade
--dg create table refd_table (id int primary key);
--dg-cascade
--dg create table trig_table (fk int references refd_table initially deferred);

--dg-cascade
--dg begin;
--dg-cascade
--dg insert into trig_table values (1);
--dg-cascade
--dg drop table refd_table cascade;
commit;

--dg-cascade
--dg drop table trig_table;

--
-- self-referential FKs are even more fun
--

create table self_ref (a int primary key,
                       b int references self_ref(a) on delete cascade);

--dg-cascade
--dg create trigger self_ref_before_trig
--dg   before delete on self_ref
--dg   for each statement execute procedure trigger_func('self_ref');
--dg-cascade
--dg create trigger self_ref_r_trig
--dg   after delete on self_ref referencing old table as old_table
--dg   for each row execute procedure dump_delete();
--dg-cascade
--dg create trigger self_ref_s_trig
--dg   after delete on self_ref referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

insert into self_ref values (1, null), (2, 1), (3, 2);

--dg-cascade
--dg delete from self_ref where a = 1;

-- without AR trigger, cascaded deletes all end up in one transition table
--dg-cascade
--dg drop trigger self_ref_r_trig on self_ref;

--dg-cascade
--dg insert into self_ref values (1, null), (2, 1), (3, 2), (4, 3);

--dg-cascade
--dg delete from self_ref where a = 1;

drop table self_ref;

--
-- test transition tables with MERGE
--
create table merge_target_table (a int primary key, b text);
--dg-cascade
--dg create trigger merge_target_table_insert_trig
--dg   after insert on merge_target_table referencing new table as new_table
--dg   for each statement execute procedure dump_insert();
--dg-cascade
--dg create trigger merge_target_table_update_trig
--dg   after update on merge_target_table referencing old table as old_table new table as new_table
--dg   for each statement execute procedure dump_update();
--dg-cascade
--dg create trigger merge_target_table_delete_trig
--dg   after delete on merge_target_table referencing old table as old_table
--dg   for each statement execute procedure dump_delete();

create table merge_source_table (a int, b text);
insert into merge_source_table
  values (1, 'initial1'), (2, 'initial2'),
		 (3, 'initial3'), (4, 'initial4');

--dg-cascade
--dg merge into merge_target_table t
--dg using merge_source_table s
--dg on t.a = s.a
--dg when not matched then
--dg   insert values (a, b);

--dg-cascade
--dg merge into merge_target_table t
--dg using merge_source_table s
--dg on t.a = s.a
--dg when matched and s.a <= 2 then
--dg 	update set b = t.b || ' updated by merge'
--dg when matched and s.a > 2 then
--dg 	delete
--dg when not matched then
--dg   insert values (a, b);

--dg-cascade
--dg merge into merge_target_table t
--dg using merge_source_table s
--dg on t.a = s.a
--dg when matched and s.a <= 2 then
--dg 	update set b = t.b || ' updated again by merge'
--dg when matched and s.a > 2 then
--dg 	delete
--dg when not matched then
--dg   insert values (a, b);

drop table merge_source_table, merge_target_table;

-- cleanup
--dg-cascade
--dg drop function dump_insert();
--dg-cascade
--dg drop function dump_update();
--dg-cascade
--dg drop function dump_delete();

--
-- Tests for CREATE OR REPLACE TRIGGER
--
create table my_table (id integer);

--dg-skip: language "?" does not exist
--dg create function funcA() returns trigger as $$
--dg begin
--dg   raise notice 'hello from funcA';
--dg   return null;
--dg end; $$ language plpgsql;

--dg-skip: language "?" does not exist
--dg create function funcB() returns trigger as $$
--dg begin
--dg   raise notice 'hello from funcB';
--dg   return null;
--dg end; $$ language plpgsql;

--dg-cascade
--dg create trigger my_trig
--dg   after insert on my_table
--dg   for each row execute procedure funcA();

--dg-cascade
--dg create trigger my_trig
--dg   before insert on my_table
--dg   for each row execute procedure funcB();  -- should fail

--dg-cascade
--dg insert into my_table values (1);

--dg-cascade
--dg create or replace trigger my_trig
--dg   before insert on my_table
--dg   for each row execute procedure funcB();  -- OK

--dg-cascade
--dg insert into my_table values (2);  -- this insert should become a no-op

--dg-cascade
--dg table my_table;

drop table my_table;

-- test CREATE OR REPLACE TRIGGER on partition table
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_trig (a int) partition by range (a);
--dg-cascade
--dg create table parted_trig_1 partition of parted_trig
--dg        for values from (0) to (1000) partition by range (a);
--dg-cascade
--dg create table parted_trig_1_1 partition of parted_trig_1 for values from (0) to (100);
--dg-cascade
--dg create table parted_trig_2 partition of parted_trig for values from (1000) to (2000);
--dg-cascade
--dg create table default_parted_trig partition of parted_trig default;

-- test that trigger can be replaced by another one
-- at the same level of partition table
--dg-cascade
--dg create or replace trigger my_trig
--dg   after insert on parted_trig
--dg   for each row execute procedure funcA();
--dg-cascade
--dg insert into parted_trig (a) values (50);
--dg-cascade
--dg create or replace trigger my_trig
--dg   after insert on parted_trig
--dg   for each row execute procedure funcB();
--dg-cascade
--dg insert into parted_trig (a) values (50);

-- test that child trigger cannot be replaced directly
--dg-cascade
--dg create or replace trigger my_trig
--dg   after insert on parted_trig
--dg   for each row execute procedure funcA();
--dg-cascade
--dg insert into parted_trig (a) values (50);
--dg-cascade
--dg create or replace trigger my_trig
--dg   after insert on parted_trig_1
--dg   for each row execute procedure funcB();  -- should fail
--dg-cascade
--dg insert into parted_trig (a) values (50);
--dg-cascade
--dg drop trigger my_trig on parted_trig;
--dg-cascade
--dg insert into parted_trig (a) values (50);

-- test that user trigger can be overwritten by one defined at upper level
--dg-cascade
--dg create trigger my_trig
--dg   after insert on parted_trig_1
--dg   for each row execute procedure funcA();
--dg-cascade
--dg insert into parted_trig (a) values (50);
--dg-cascade
--dg create trigger my_trig
--dg   after insert on parted_trig
--dg   for each row execute procedure funcB();  -- should fail
--dg-cascade
--dg insert into parted_trig (a) values (50);
--dg-cascade
--dg create or replace trigger my_trig
--dg   after insert on parted_trig
--dg   for each row execute procedure funcB();
--dg-cascade
--dg insert into parted_trig (a) values (50);

-- cleanup
--dg-cascade
--dg drop table parted_trig;
--dg-cascade
--dg drop function funcA();
--dg-cascade
--dg drop function funcB();

-- Leave around some objects for other tests
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table trigger_parted (a int primary key) partition by list (a);
--dg-skip: language "?" does not exist
--dg create function trigger_parted_trigfunc() returns trigger language plpgsql as
--dg   $$ begin end; $$;
--dg-cascade
--dg create trigger aft_row after insert or update on trigger_parted
--dg   for each row execute function trigger_parted_trigfunc();
--dg-cascade
--dg create table trigger_parted_p1 partition of trigger_parted for values in (1)
--dg   partition by list (a);
--dg-cascade
--dg create table trigger_parted_p1_1 partition of trigger_parted_p1 for values in (1);
--dg-cascade
--dg create table trigger_parted_p2 partition of trigger_parted for values in (2)
--dg   partition by list (a);
--dg-cascade
--dg create table trigger_parted_p2_2 partition of trigger_parted_p2 for values in (2);
--dg-cascade
--dg alter table only trigger_parted_p2 disable trigger aft_row;
--dg-cascade
--dg alter table trigger_parted_p2_2 enable always trigger aft_row;

-- verify transition table conversion slot's lifetime
-- https://postgr.es/m/39a71864-b120-5a5c-8cc5-c632b6f16761@amazon.com
create table convslot_test_parent (col1 text primary key);
create table convslot_test_child (col1 text primary key,
	foreign key (col1) references convslot_test_parent(col1) on delete cascade on update cascade
);

alter table convslot_test_child add column col2 text not null default 'tutu';
insert into convslot_test_parent(col1) values ('1');
insert into convslot_test_child(col1) values ('1');
insert into convslot_test_parent(col1) values ('3');
insert into convslot_test_child(col1) values ('3');

--dg-skip: language "?" does not exist
--dg create function convslot_trig1()
--dg returns trigger
--dg language plpgsql
--dg AS $$
--dg begin
--dg raise notice 'trigger = %, old_table = %',
--dg           TG_NAME,
--dg           (select string_agg(old_table::text, ', ' order by col1) from old_table);
--dg return null;
--dg end; $$;

--dg-skip: language "?" does not exist
--dg create function convslot_trig2()
--dg returns trigger
--dg language plpgsql
--dg AS $$
--dg begin
--dg raise notice 'trigger = %, new table = %',
--dg           TG_NAME,
--dg           (select string_agg(new_table::text, ', ' order by col1) from new_table);
--dg return null;
--dg end; $$;

--dg-cascade
--dg create trigger but_trigger after update on convslot_test_child
--dg referencing new table as new_table
--dg for each statement execute function convslot_trig2();

--dg-cascade
--dg update convslot_test_parent set col1 = col1 || '1';

--dg-skip: language "?" does not exist
--dg create function convslot_trig3()
--dg returns trigger
--dg language plpgsql
--dg AS $$
--dg begin
--dg raise notice 'trigger = %, old_table = %, new table = %',
--dg           TG_NAME,
--dg           (select string_agg(old_table::text, ', ' order by col1) from old_table),
--dg           (select string_agg(new_table::text, ', ' order by col1) from new_table);
--dg return null;
--dg end; $$;

--dg-cascade
--dg create trigger but_trigger2 after update on convslot_test_child
--dg referencing old table as old_table new table as new_table
--dg for each statement execute function convslot_trig3();
--dg-cascade
--dg update convslot_test_parent set col1 = col1 || '1';

--dg-cascade
--dg create trigger bdt_trigger after delete on convslot_test_child
--dg referencing old table as old_table
--dg for each statement execute function convslot_trig1();
--dg-cascade
--dg delete from convslot_test_parent;

drop table convslot_test_child, convslot_test_parent;
--dg-cascade
--dg drop function convslot_trig1();
--dg-cascade
--dg drop function convslot_trig2();
--dg-cascade
--dg drop function convslot_trig3();

-- Bug #17607: variant of above in which trigger function raises an error;
-- we don't see any ill effects unless trigger tuple requires mapping

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table convslot_test_parent (id int primary key, val int)
--dg partition by range (id);

create table convslot_test_part (val int, id int not null);

--dg-cascade
--dg alter table convslot_test_parent
--dg   attach partition convslot_test_part for values from (1) to (1000);

--dg-skip: language "?" does not exist
--dg create function convslot_trig4() returns trigger as
--dg $$begin raise exception 'BOOM!'; end$$ language plpgsql;

--dg-cascade
--dg create trigger convslot_test_parent_update
--dg     after update on convslot_test_parent
--dg     referencing old table as old_rows new table as new_rows
--dg     for each statement execute procedure convslot_trig4();

--dg-cascade
--dg insert into convslot_test_parent (id, val) values (1, 2);

begin;
savepoint svp;
--dg-cascade
--dg update convslot_test_parent set val = 3;  -- error expected
rollback to savepoint svp;
rollback;

--dg-cascade
--dg drop table convslot_test_parent;
--dg-cascade
--dg drop function convslot_trig4();

-- Test trigger renaming on partitioned tables
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table grandparent (id int, primary key (id)) partition by range (id);
--dg-cascade
--dg create table middle partition of grandparent for values from (1) to (10)
--dg partition by range (id);
--dg-cascade
--dg create table chi partition of middle for values from (1) to (5);
--dg-cascade
--dg create table cho partition of middle for values from (6) to (10);
--dg-skip: language "?" does not exist
--dg create function f () returns trigger as
--dg $$ begin return new; end; $$
--dg language plpgsql;
--dg-cascade
--dg create trigger a after insert on grandparent
--dg for each row execute procedure f();

--dg-cascade
--dg alter trigger a on grandparent rename to b;
--dg-cascade
--dg select tgrelid::regclass, tgname,
--dg (select tgname from pg_trigger tr where tr.oid = pg_trigger.tgparentid) parent_tgname
--dg from pg_trigger where tgrelid in (select relid from pg_partition_tree('grandparent'))
--dg order by tgname, tgrelid::regclass::text COLLATE "C";
alter trigger a on only grandparent rename to b;	-- ONLY not supported
--dg-cascade
--dg alter trigger b on middle rename to c;	-- can't rename trigger on partition
--dg-cascade
--dg create trigger c after insert on middle
--dg for each row execute procedure f();
--dg-cascade
--dg alter trigger b on grandparent rename to c;

-- Rename cascading does not affect statement triggers
--dg-cascade
--dg create trigger p after insert on grandparent for each statement execute function f();
--dg-cascade
--dg create trigger p after insert on middle for each statement execute function f();
--dg-cascade
--dg alter trigger p on grandparent rename to q;
--dg-cascade
--dg select tgrelid::regclass, tgname,
--dg (select tgname from pg_trigger tr where tr.oid = pg_trigger.tgparentid) parent_tgname
--dg from pg_trigger where tgrelid in (select relid from pg_partition_tree('grandparent'))
--dg order by tgname, tgrelid::regclass::text COLLATE "C";

--dg-cascade
--dg drop table grandparent;

-- Trigger renaming does not recurse on legacy inheritance
create table parent (a int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table child () inherits (parent);
--dg-cascade
--dg create trigger parenttrig after insert on parent
--dg for each row execute procedure f();
--dg-cascade
--dg create trigger parenttrig after insert on child
--dg for each row execute procedure f();
--dg-cascade
--dg alter trigger parenttrig on parent rename to anothertrig;
--dg-cascade
--dg \d+ child

--dg-cascade
--dg drop table parent, child;
--dg-cascade
--dg drop function f();

-- Test who runs deferred trigger functions

-- setup
create role regress_caller;
create role regress_fn_owner;
--dg-skip: language "?" does not exist
--dg create function whoami() returns trigger language plpgsql
--dg as $$
--dg begin
--dg   raise notice 'I am %', current_user;
--dg   return null;
--dg end;
--dg $$;
--dg-cascade
--dg alter function whoami() owner to regress_fn_owner;

create table defer_trig (id integer);
grant insert on defer_trig to public;
--dg-cascade
--dg create constraint trigger whoami after insert on defer_trig
--dg   deferrable initially deferred
--dg   for each row
--dg   execute function whoami();

-- deferred triggers must run as the user that queued the trigger
begin;
set role regress_caller;
insert into defer_trig values (1);
reset role;
set role regress_fn_owner;
insert into defer_trig values (2);
reset role;
--dg-cascade
--dg commit;

-- security definer functions override the user who queued the trigger
--dg-cascade
--dg alter function whoami() security definer;
--dg-cascade
--dg begin;
set role regress_caller;
insert into defer_trig values (3);
reset role;
--dg-cascade
--dg commit;
--dg-cascade
--dg alter function whoami() security invoker;

-- make sure the current user is restored after error
--dg-skip: language "?" does not exist
--dg create or replace function whoami() returns trigger language plpgsql
--dg as $$
--dg begin
--dg   raise notice 'I am %', current_user;
--dg   perform 1 / 0;
--dg   return null;
--dg end;
--dg $$;

--dg-cascade
--dg begin;
set role regress_caller;
insert into defer_trig values (4);
reset role;
--dg-cascade
--dg commit;  -- error expected
select current_user = session_user;

-- clean up
drop table defer_trig;
--dg-cascade
--dg drop function whoami();
--dg-skip: DROP ROLE cannot run inside a transaction block
--dg drop role regress_fn_owner;
--dg-skip: DROP ROLE cannot run inside a transaction block
--dg drop role regress_caller;
