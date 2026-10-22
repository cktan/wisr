--
-- RULES
-- From Jan's original setup_ruletest.sql and run_ruletest.sql
-- - thomas 1998-09-13
--

--
-- Tables and rules for the view test
--
create table rtest_t1 (a int4, b int4);
create table rtest_t2 (a int4, b int4);
create table rtest_t3 (a int4, b int4);

create view rtest_v1 as select * from rtest_t1;
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_v1_ins as on insert to rtest_v1 do instead
--dg 	insert into rtest_t1 values (new.a, new.b);
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_v1_upd as on update to rtest_v1 do instead
--dg 	update rtest_t1 set a = new.a, b = new.b
--dg 	where a = old.a;
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_v1_del as on delete to rtest_v1 do instead
--dg 	delete from rtest_t1 where a = old.a;
-- Test comments
--dg-differs: another error: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg COMMENT ON RULE rtest_v1_bad ON rtest_v1 IS 'bad rule';
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg COMMENT ON RULE rtest_v1_del ON rtest_v1 IS 'delete rule';
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg COMMENT ON RULE rtest_v1_del ON rtest_v1 IS NULL;
--
-- Tables and rules for the constraint update/delete test
--
-- Note:
-- 	Now that we have multiple action rule support, we check
-- 	both possible syntaxes to define them (The last action
--  can but must not have a semicolon at the end).
--
create table rtest_system (sysname text, sysdesc text);
create table rtest_interface (sysname text, ifname text);
create table rtest_person (pname text, pdesc text);
create table rtest_admin (pname text, sysname text);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_sys_upd as on update to rtest_system do also (
--dg 	update rtest_interface set sysname = new.sysname
--dg 		where sysname = old.sysname;
--dg 	update rtest_admin set sysname = new.sysname
--dg 		where sysname = old.sysname
--dg 	);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_sys_del as on delete to rtest_system do also (
--dg 	delete from rtest_interface where sysname = old.sysname;
--dg 	delete from rtest_admin where sysname = old.sysname;
--dg 	);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_pers_upd as on update to rtest_person do also
--dg 	update rtest_admin set pname = new.pname where pname = old.pname;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_pers_del as on delete to rtest_person do also
--dg 	delete from rtest_admin where pname = old.pname;

--
-- Tables and rules for the logging test
--
create table rtest_emp (ename char(20), salary numeric);
create table rtest_emplog (ename char(20), who name, action char(10), newsal numeric, oldsal numeric);
create table rtest_empmass (ename char(20), salary numeric);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_emp_ins as on insert to rtest_emp do
--dg 	insert into rtest_emplog values (new.ename, current_user,
--dg 			'hired', new.salary, '0.00');

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_emp_upd as on update to rtest_emp where new.salary != old.salary do
--dg 	insert into rtest_emplog values (new.ename, current_user,
--dg 			'honored', new.salary, old.salary);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_emp_del as on delete to rtest_emp do
--dg 	insert into rtest_emplog values (old.ename, current_user,
--dg 			'fired', '0.00', old.salary);

--
-- Tables and rules for the multiple cascaded qualified instead
-- rule test
--
create table rtest_t4 (a int4, b text);
create table rtest_t5 (a int4, b text);
create table rtest_t6 (a int4, b text);
create table rtest_t7 (a int4, b text);
create table rtest_t8 (a int4, b text);
create table rtest_t9 (a int4, b text);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_t4_ins1 as on insert to rtest_t4
--dg 		where new.a >= 10 and new.a < 20 do instead
--dg 	insert into rtest_t5 values (new.a, new.b);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_t4_ins2 as on insert to rtest_t4
--dg 		where new.a >= 20 and new.a < 30 do
--dg 	insert into rtest_t6 values (new.a, new.b);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_t5_ins as on insert to rtest_t5
--dg 		where new.a > 15 do
--dg 	insert into rtest_t7 values (new.a, new.b);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_t6_ins as on insert to rtest_t6
--dg 		where new.a > 25 do instead
--dg 	insert into rtest_t8 values (new.a, new.b);

--
-- Tables and rules for the rule fire order test
--
-- As of PG 7.3, the rules should fire in order by name, regardless
-- of INSTEAD attributes or creation order.
--
create table rtest_order1 (a int4);
create table rtest_order2 (a int4, b int4, c text);

create sequence rtest_seq;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_order_r3 as on insert to rtest_order1 do instead
--dg 	insert into rtest_order2 values (new.a, nextval('rtest_seq'),
--dg 		'rule 3 - this should run 3rd');

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_order_r4 as on insert to rtest_order1
--dg 		where a < 100 do instead
--dg 	insert into rtest_order2 values (new.a, nextval('rtest_seq'),
--dg 		'rule 4 - this should run 4th');

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_order_r2 as on insert to rtest_order1 do
--dg 	insert into rtest_order2 values (new.a, nextval('rtest_seq'),
--dg 		'rule 2 - this should run 2nd');

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_order_r1 as on insert to rtest_order1 do instead
--dg 	insert into rtest_order2 values (new.a, nextval('rtest_seq'),
--dg 		'rule 1 - this should run 1st');

--
-- Tables and rules for the instead nothing test
--
create table rtest_nothn1 (a int4, b text);
create table rtest_nothn2 (a int4, b text);
create table rtest_nothn3 (a int4, b text);
create table rtest_nothn4 (a int4, b text);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_nothn_r1 as on insert to rtest_nothn1
--dg 	where new.a >= 10 and new.a < 20 do instead nothing;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_nothn_r2 as on insert to rtest_nothn1
--dg 	where new.a >= 30 and new.a < 40 do instead nothing;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_nothn_r3 as on insert to rtest_nothn2
--dg 	where new.a >= 100 do instead
--dg 	insert into rtest_nothn3 values (new.a, new.b);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rtest_nothn_r4 as on insert to rtest_nothn2
--dg 	do instead nothing;

--
-- Tests on a view that is select * of a table
-- and has insert/update/delete instead rules to
-- behave close like the real table.
--

--
-- We need test date later
--
insert into rtest_t2 values (1, 21);
insert into rtest_t2 values (2, 22);
insert into rtest_t2 values (3, 23);

insert into rtest_t3 values (1, 31);
insert into rtest_t3 values (2, 32);
insert into rtest_t3 values (3, 33);
insert into rtest_t3 values (4, 34);
insert into rtest_t3 values (5, 35);

-- insert values
insert into rtest_v1 values (1, 11);
insert into rtest_v1 values (2, 12);
select * from rtest_v1;

-- delete with constant expression
delete from rtest_v1 where a = 1;
select * from rtest_v1;
insert into rtest_v1 values (1, 11);
delete from rtest_v1 where b = 12;
select * from rtest_v1;
insert into rtest_v1 values (2, 12);
insert into rtest_v1 values (2, 13);
select * from rtest_v1;
** Remember the delete rule on rtest_v1: It says
** DO INSTEAD DELETE FROM rtest_t1 WHERE a = old.a
** So this time both rows with a = 2 must get deleted
\p
\r
delete from rtest_v1 where b = 12;
--dg-cascade
--dg select * from rtest_v1;
delete from rtest_v1;

-- insert select
insert into rtest_v1 select * from rtest_t2;
select * from rtest_v1;
delete from rtest_v1;

-- same with swapped targetlist
insert into rtest_v1 (b, a) select b, a from rtest_t2;
select * from rtest_v1;

-- now with only one target attribute
insert into rtest_v1 (a) select a from rtest_t3;
select * from rtest_v1;
select * from rtest_v1 where b isnull;

-- let attribute a differ (must be done on rtest_t1 - see above)
update rtest_t1 set a = a + 10 where b isnull;
delete from rtest_v1 where b isnull;
select * from rtest_v1;

-- now updates with constant expression
update rtest_v1 set b = 42 where a = 2;
--dg-differs: the result
--dg select * from rtest_v1;
update rtest_v1 set b = 99 where b = 42;
--dg-differs: the result
--dg select * from rtest_v1;
update rtest_v1 set b = 88 where b < 50;
--dg-differs: the result
--dg select * from rtest_v1;
delete from rtest_v1;
insert into rtest_v1 select rtest_t2.a, rtest_t3.b
    from rtest_t2, rtest_t3
    where rtest_t2.a = rtest_t3.a;
select * from rtest_v1;

-- updates in a mergejoin
update rtest_v1 set b = rtest_t2.b from rtest_t2 where rtest_v1.a = rtest_t2.a;
select * from rtest_v1;
insert into rtest_v1 select * from rtest_t3;
select * from rtest_v1;
update rtest_t1 set a = a + 10 where b > 30;
select * from rtest_v1;
update rtest_v1 set a = rtest_t3.a + 20 from rtest_t3 where rtest_v1.b = rtest_t3.b;
select * from rtest_v1;

--
-- Test for constraint updates/deletes
--
insert into rtest_system values ('orion', 'Linux Jan Wieck');
insert into rtest_system values ('notjw', 'WinNT Jan Wieck (notebook)');
insert into rtest_system values ('neptun', 'Fileserver');

insert into rtest_interface values ('orion', 'eth0');
insert into rtest_interface values ('orion', 'eth1');
insert into rtest_interface values ('notjw', 'eth0');
insert into rtest_interface values ('neptun', 'eth0');

insert into rtest_person values ('jw', 'Jan Wieck');
insert into rtest_person values ('bm', 'Bruce Momjian');

insert into rtest_admin values ('jw', 'orion');
insert into rtest_admin values ('jw', 'notjw');
insert into rtest_admin values ('bm', 'neptun');

update rtest_system set sysname = 'pluto' where sysname = 'neptun';

--dg-cascade
--dg select * from rtest_interface;
--dg-cascade
--dg select * from rtest_admin;

update rtest_person set pname = 'jwieck' where pdesc = 'Jan Wieck';

-- Note: use ORDER BY here to ensure consistent output across all systems.
-- The above UPDATE affects two rows with equal keys, so they could be
-- updated in either order depending on the whim of the local qsort().

--dg-cascade
--dg select * from rtest_admin order by pname, sysname;

delete from rtest_system where sysname = 'orion';

--dg-cascade
--dg select * from rtest_interface;
--dg-cascade
--dg select * from rtest_admin;

--
-- Rule qualification test
--
insert into rtest_emp values ('wiecc', '5000.00');
insert into rtest_emp values ('gates', '80000.00');
update rtest_emp set ename = 'wiecx' where ename = 'wiecc';
update rtest_emp set ename = 'wieck', salary = '6000.00' where ename = 'wiecx';
update rtest_emp set salary = '7000.00' where ename = 'wieck';
delete from rtest_emp where ename = 'gates';

--dg-cascade
--dg select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog order by ename, action, newsal;
insert into rtest_empmass values ('meyer', '4000.00');
insert into rtest_empmass values ('maier', '5000.00');
insert into rtest_empmass values ('mayr', '6000.00');
insert into rtest_emp select * from rtest_empmass;
--dg-cascade
--dg select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog order by ename, action, newsal;
update rtest_empmass set salary = salary + '1000.00';
update rtest_emp set salary = rtest_empmass.salary from rtest_empmass where rtest_emp.ename = rtest_empmass.ename;
--dg-cascade
--dg select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog order by ename, action, newsal;
delete from rtest_emp using rtest_empmass where rtest_emp.ename = rtest_empmass.ename;
--dg-cascade
--dg select ename, who = current_user as "matches user", action, newsal, oldsal from rtest_emplog order by ename, action, newsal;

--
-- Multiple cascaded qualified instead rule test
--
insert into rtest_t4 values (1, 'Record should go to rtest_t4');
insert into rtest_t4 values (2, 'Record should go to rtest_t4');
insert into rtest_t4 values (10, 'Record should go to rtest_t5');
insert into rtest_t4 values (15, 'Record should go to rtest_t5');
insert into rtest_t4 values (19, 'Record should go to rtest_t5 and t7');
insert into rtest_t4 values (20, 'Record should go to rtest_t4 and t6');
insert into rtest_t4 values (26, 'Record should go to rtest_t4 and t8');
insert into rtest_t4 values (28, 'Record should go to rtest_t4 and t8');
insert into rtest_t4 values (30, 'Record should go to rtest_t4');
insert into rtest_t4 values (40, 'Record should go to rtest_t4');

--dg-cascade
--dg select * from rtest_t4;
--dg-cascade
--dg select * from rtest_t5;
--dg-cascade
--dg select * from rtest_t6;
--dg-cascade
--dg select * from rtest_t7;
--dg-cascade
--dg select * from rtest_t8;

delete from rtest_t4;
delete from rtest_t5;
delete from rtest_t6;
delete from rtest_t7;
delete from rtest_t8;

insert into rtest_t9 values (1, 'Record should go to rtest_t4');
insert into rtest_t9 values (2, 'Record should go to rtest_t4');
insert into rtest_t9 values (10, 'Record should go to rtest_t5');
insert into rtest_t9 values (15, 'Record should go to rtest_t5');
insert into rtest_t9 values (19, 'Record should go to rtest_t5 and t7');
insert into rtest_t9 values (20, 'Record should go to rtest_t4 and t6');
insert into rtest_t9 values (26, 'Record should go to rtest_t4 and t8');
insert into rtest_t9 values (28, 'Record should go to rtest_t4 and t8');
insert into rtest_t9 values (30, 'Record should go to rtest_t4');
insert into rtest_t9 values (40, 'Record should go to rtest_t4');

insert into rtest_t4 select * from rtest_t9 where a < 20;

--dg-cascade
--dg select * from rtest_t4;
--dg-cascade
--dg select * from rtest_t5;
select * from rtest_t6;
--dg-cascade
--dg select * from rtest_t7;
select * from rtest_t8;

insert into rtest_t4 select * from rtest_t9 where b ~ 'and t8';

--dg-cascade
--dg select * from rtest_t4;
--dg-cascade
--dg select * from rtest_t5;
select * from rtest_t6;
--dg-cascade
--dg select * from rtest_t7;
--dg-cascade
--dg select * from rtest_t8;

insert into rtest_t4 select a + 1, b from rtest_t9 where a in (20, 30, 40);

--dg-cascade
--dg select * from rtest_t4;
--dg-cascade
--dg select * from rtest_t5;
--dg-cascade
--dg select * from rtest_t6;
--dg-cascade
--dg select * from rtest_t7;
--dg-cascade
--dg select * from rtest_t8;

--
-- Check that the ordering of rules fired is correct
--
insert into rtest_order1 values (1);
--dg-cascade
--dg select * from rtest_order2;

--
-- Check if instead nothing w/without qualification works
--
insert into rtest_nothn1 values (1, 'want this');
insert into rtest_nothn1 values (2, 'want this');
insert into rtest_nothn1 values (10, 'don''t want this');
insert into rtest_nothn1 values (19, 'don''t want this');
insert into rtest_nothn1 values (20, 'want this');
insert into rtest_nothn1 values (29, 'want this');
insert into rtest_nothn1 values (30, 'don''t want this');
insert into rtest_nothn1 values (39, 'don''t want this');
insert into rtest_nothn1 values (40, 'want this');
insert into rtest_nothn1 values (50, 'want this');
insert into rtest_nothn1 values (60, 'want this');

--dg-cascade
--dg select * from rtest_nothn1;

insert into rtest_nothn2 values (10, 'too small');
insert into rtest_nothn2 values (50, 'too small');
insert into rtest_nothn2 values (100, 'OK');
insert into rtest_nothn2 values (200, 'OK');

--dg-cascade
--dg select * from rtest_nothn2;
--dg-cascade
--dg select * from rtest_nothn3;

delete from rtest_nothn1;
delete from rtest_nothn2;
delete from rtest_nothn3;

insert into rtest_nothn4 values (1, 'want this');
insert into rtest_nothn4 values (2, 'want this');
insert into rtest_nothn4 values (10, 'don''t want this');
insert into rtest_nothn4 values (19, 'don''t want this');
insert into rtest_nothn4 values (20, 'want this');
insert into rtest_nothn4 values (29, 'want this');
insert into rtest_nothn4 values (30, 'don''t want this');
insert into rtest_nothn4 values (39, 'don''t want this');
insert into rtest_nothn4 values (40, 'want this');
insert into rtest_nothn4 values (50, 'want this');
insert into rtest_nothn4 values (60, 'want this');

insert into rtest_nothn1 select * from rtest_nothn4;

--dg-cascade
--dg select * from rtest_nothn1;

delete from rtest_nothn4;

insert into rtest_nothn4 values (10, 'too small');
insert into rtest_nothn4 values (50, 'too small');
insert into rtest_nothn4 values (100, 'OK');
insert into rtest_nothn4 values (200, 'OK');

insert into rtest_nothn2 select * from rtest_nothn4;

--dg-cascade
--dg select * from rtest_nothn2;
--dg-cascade
--dg select * from rtest_nothn3;

create table rtest_view1 (a int4, b text, v bool);
create table rtest_view2 (a int4);
create table rtest_view3 (a int4, b text);
create table rtest_view4 (a int4, b text, c int4);
create view rtest_vview1 as select a, b from rtest_view1 X
	where 0 < (select count(*) from rtest_view2 Y where Y.a = X.a);
create view rtest_vview2 as select a, b from rtest_view1 where v;
create view rtest_vview3 as select a, b from rtest_vview2 X
	where 0 < (select count(*) from rtest_view2 Y where Y.a = X.a);
create view rtest_vview4 as select X.a, X.b, count(Y.a) as refcount
	from rtest_view1 X, rtest_view2 Y
	where X.a = Y.a
	group by X.a, X.b;
--dg-skip: language "?" does not exist
--dg create function rtest_viewfunc1(int4) returns int4 as
--dg 	'select count(*)::int4 from rtest_view2 where a = $1'
--dg 	language sql;
--dg-cascade
--dg create view rtest_vview5 as select a, b, rtest_viewfunc1(a) as refcount
--dg 	from rtest_view1;

insert into rtest_view1 values (1, 'item 1', 't');
insert into rtest_view1 values (2, 'item 2', 't');
insert into rtest_view1 values (3, 'item 3', 't');
insert into rtest_view1 values (4, 'item 4', 'f');
insert into rtest_view1 values (5, 'item 5', 't');
insert into rtest_view1 values (6, 'item 6', 'f');
insert into rtest_view1 values (7, 'item 7', 't');
insert into rtest_view1 values (8, 'item 8', 't');

insert into rtest_view2 values (2);
insert into rtest_view2 values (2);
insert into rtest_view2 values (4);
insert into rtest_view2 values (5);
insert into rtest_view2 values (7);
insert into rtest_view2 values (7);
insert into rtest_view2 values (7);
insert into rtest_view2 values (7);

select * from rtest_vview1;
select * from rtest_vview2;
select * from rtest_vview3;
select * from rtest_vview4 order by a, b;
--dg-cascade
--dg select * from rtest_vview5;

insert into rtest_view3 select * from rtest_vview1 where a < 7;
select * from rtest_view3;
delete from rtest_view3;

insert into rtest_view3 select * from rtest_vview2 where a != 5 and b !~ '2';
select * from rtest_view3;
delete from rtest_view3;

insert into rtest_view3 select * from rtest_vview3;
select * from rtest_view3;
delete from rtest_view3;

insert into rtest_view4 select * from rtest_vview4 where 3 > refcount;
select * from rtest_view4 order by a, b;
delete from rtest_view4;

--dg-cascade
--dg insert into rtest_view4 select * from rtest_vview5 where a > 2 and refcount = 0;
--dg-cascade
--dg select * from rtest_view4;
delete from rtest_view4;
--
-- Test for computations in views
--
create table rtest_comp (
	part	text,
	unit	char(4),
	size	float
);


create table rtest_unitfact (
	unit	char(4),
	factor	float
);

create view rtest_vcomp as
	select X.part, (X.size * Y.factor) as size_in_cm
			from rtest_comp X, rtest_unitfact Y
			where X.unit = Y.unit;


insert into rtest_unitfact values ('m', 100.0);
insert into rtest_unitfact values ('cm', 1.0);
insert into rtest_unitfact values ('inch', 2.54);

insert into rtest_comp values ('p1', 'm', 5.0);
insert into rtest_comp values ('p2', 'm', 3.0);
insert into rtest_comp values ('p3', 'cm', 5.0);
insert into rtest_comp values ('p4', 'cm', 15.0);
insert into rtest_comp values ('p5', 'inch', 7.0);
insert into rtest_comp values ('p6', 'inch', 4.4);

select * from rtest_vcomp order by part;

select * from rtest_vcomp where size_in_cm > 10.0 order by size_in_cm using >;

--
-- In addition run the (slightly modified) queries from the
-- programmers manual section on the rule system.
--
CREATE TABLE shoe_data (
	shoename   char(10),      -- primary key
	sh_avail   integer,       -- available # of pairs
	slcolor    char(10),      -- preferred shoelace color
	slminlen   float,         -- minimum shoelace length
	slmaxlen   float,         -- maximum shoelace length
	slunit     char(8)        -- length unit
);

CREATE TABLE shoelace_data (
	sl_name    char(10),      -- primary key
	sl_avail   integer,       -- available # of pairs
	sl_color   char(10),      -- shoelace color
	sl_len     float,         -- shoelace length
	sl_unit    char(8)        -- length unit
);

CREATE TABLE unit (
	un_name    char(8),       -- the primary key
	un_fact    float          -- factor to transform to cm
);

CREATE VIEW shoe AS
	SELECT sh.shoename,
		   sh.sh_avail,
		   sh.slcolor,
		   sh.slminlen,
		   sh.slminlen * un.un_fact AS slminlen_cm,
		   sh.slmaxlen,
		   sh.slmaxlen * un.un_fact AS slmaxlen_cm,
		   sh.slunit
	  FROM shoe_data sh, unit un
	 WHERE sh.slunit = un.un_name;

CREATE VIEW shoelace AS
	SELECT s.sl_name,
		   s.sl_avail,
		   s.sl_color,
		   s.sl_len,
		   s.sl_unit,
		   s.sl_len * u.un_fact AS sl_len_cm
	  FROM shoelace_data s, unit u
	 WHERE s.sl_unit = u.un_name;

CREATE VIEW shoe_ready AS
	SELECT rsh.shoename,
		   rsh.sh_avail,
		   rsl.sl_name,
		   rsl.sl_avail,
		   int4smaller(rsh.sh_avail, rsl.sl_avail) AS total_avail
	  FROM shoe rsh, shoelace rsl
	 WHERE rsl.sl_color = rsh.slcolor
	   AND rsl.sl_len_cm >= rsh.slminlen_cm
	   AND rsl.sl_len_cm <= rsh.slmaxlen_cm;

INSERT INTO unit VALUES ('cm', 1.0);
INSERT INTO unit VALUES ('m', 100.0);
INSERT INTO unit VALUES ('inch', 2.54);

INSERT INTO shoe_data VALUES ('sh1', 2, 'black', 70.0, 90.0, 'cm');
INSERT INTO shoe_data VALUES ('sh2', 0, 'black', 30.0, 40.0, 'inch');
INSERT INTO shoe_data VALUES ('sh3', 4, 'brown', 50.0, 65.0, 'cm');
INSERT INTO shoe_data VALUES ('sh4', 3, 'brown', 40.0, 50.0, 'inch');

INSERT INTO shoelace_data VALUES ('sl1', 5, 'black', 80.0, 'cm');
INSERT INTO shoelace_data VALUES ('sl2', 6, 'black', 100.0, 'cm');
INSERT INTO shoelace_data VALUES ('sl3', 0, 'black', 35.0 , 'inch');
INSERT INTO shoelace_data VALUES ('sl4', 8, 'black', 40.0 , 'inch');
INSERT INTO shoelace_data VALUES ('sl5', 4, 'brown', 1.0 , 'm');
INSERT INTO shoelace_data VALUES ('sl6', 0, 'brown', 0.9 , 'm');
INSERT INTO shoelace_data VALUES ('sl7', 7, 'brown', 60 , 'cm');
INSERT INTO shoelace_data VALUES ('sl8', 1, 'brown', 40 , 'inch');

-- SELECTs in doc
SELECT * FROM shoelace ORDER BY sl_name;
SELECT * FROM shoe_ready WHERE total_avail >= 2 ORDER BY 1;

    CREATE TABLE shoelace_log (
        sl_name    char(10),      -- shoelace changed
        sl_avail   integer,       -- new available value
        log_who    name,          -- who did it
        log_when   timestamp      -- when
    );

-- Want "log_who" to be CURRENT_USER,
-- but that is non-portable for the regression test
-- - thomas 1999-02-21

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg     CREATE RULE log_shoelace AS ON UPDATE TO shoelace_data
--dg         WHERE NEW.sl_avail != OLD.sl_avail
--dg         DO INSERT INTO shoelace_log VALUES (
--dg                                         NEW.sl_name,
--dg                                         NEW.sl_avail,
--dg                                         'Al Bundy',
--dg                                         'epoch'
--dg                                     );

UPDATE shoelace_data SET sl_avail = 6 WHERE  sl_name = 'sl7';

--dg-cascade
--dg SELECT * FROM shoelace_log;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg     CREATE RULE shoelace_ins AS ON INSERT TO shoelace
--dg         DO INSTEAD
--dg         INSERT INTO shoelace_data VALUES (
--dg                NEW.sl_name,
--dg                NEW.sl_avail,
--dg                NEW.sl_color,
--dg                NEW.sl_len,
--dg                NEW.sl_unit);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg     CREATE RULE shoelace_upd AS ON UPDATE TO shoelace
--dg         DO INSTEAD
--dg         UPDATE shoelace_data SET
--dg                sl_name = NEW.sl_name,
--dg                sl_avail = NEW.sl_avail,
--dg                sl_color = NEW.sl_color,
--dg                sl_len = NEW.sl_len,
--dg                sl_unit = NEW.sl_unit
--dg          WHERE sl_name = OLD.sl_name;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg     CREATE RULE shoelace_del AS ON DELETE TO shoelace
--dg         DO INSTEAD
--dg         DELETE FROM shoelace_data
--dg          WHERE sl_name = OLD.sl_name;

    CREATE TABLE shoelace_arrive (
        arr_name    char(10),
        arr_quant   integer
    );

    CREATE TABLE shoelace_ok (
        ok_name     char(10),
        ok_quant    integer
    );

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg     CREATE RULE shoelace_ok_ins AS ON INSERT TO shoelace_ok
--dg         DO INSTEAD
--dg         UPDATE shoelace SET
--dg                sl_avail = sl_avail + NEW.ok_quant
--dg          WHERE sl_name = NEW.ok_name;

INSERT INTO shoelace_arrive VALUES ('sl3', 10);
INSERT INTO shoelace_arrive VALUES ('sl6', 20);
INSERT INTO shoelace_arrive VALUES ('sl8', 20);

SELECT * FROM shoelace ORDER BY sl_name;

insert into shoelace_ok select * from shoelace_arrive;

--dg-cascade
--dg SELECT * FROM shoelace ORDER BY sl_name;

--dg-cascade
--dg SELECT * FROM shoelace_log ORDER BY sl_name;

    CREATE VIEW shoelace_obsolete AS
	SELECT * FROM shoelace WHERE NOT EXISTS
	    (SELECT shoename FROM shoe WHERE slcolor = sl_color);

    CREATE VIEW shoelace_candelete AS
	SELECT * FROM shoelace_obsolete WHERE sl_avail = 0;

--dg-cascade
--dg insert into shoelace values ('sl9', 0, 'pink', 35.0, 'inch', 0.0);
--dg-cascade
--dg insert into shoelace values ('sl10', 1000, 'magenta', 40.0, 'inch', 0.0);
-- Unsupported (even though a similar updatable view construct is)
--dg-cascade
--dg insert into shoelace values ('sl10', 1000, 'magenta', 40.0, 'inch', 0.0)
--dg   on conflict do nothing;

--dg-cascade
--dg SELECT * FROM shoelace_obsolete ORDER BY sl_len_cm;
--dg-cascade
--dg SELECT * FROM shoelace_candelete;

--dg-cascade
--dg DELETE FROM shoelace WHERE EXISTS
--dg     (SELECT * FROM shoelace_candelete
--dg              WHERE sl_name = shoelace.sl_name);

--dg-cascade
--dg SELECT * FROM shoelace ORDER BY sl_name;

SELECT * FROM shoe ORDER BY shoename;
SELECT count(*) FROM shoe;


--
-- Simple test of qualified ON INSERT ... this did not work in 7.0 ...
--
create table rules_foo (f1 int);
create table rules_foo2 (f1 int);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rules_foorule as on insert to rules_foo where f1 < 100
--dg do instead nothing;

insert into rules_foo values(1);
insert into rules_foo values(1001);
--dg-cascade
--dg select * from rules_foo;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg drop rule rules_foorule on rules_foo;

-- this should fail because f1 is not exposed for unqualified reference:
create rule rules_foorule as on insert to rules_foo where f1 < 100
do instead insert into rules_foo2 values (f1);
-- this is the correct way:
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rules_foorule as on insert to rules_foo where f1 < 100
--dg do instead insert into rules_foo2 values (new.f1);

insert into rules_foo values(2);
insert into rules_foo values(100);

--dg-cascade
--dg select * from rules_foo;
--dg-cascade
--dg select * from rules_foo2;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg drop rule rules_foorule on rules_foo;
drop table rules_foo;
drop table rules_foo2;


--
-- Test rules containing INSERT ... SELECT, which is a very ugly special
-- case as of 7.1.  Example is based on bug report from Joel Burton.
--
create table pparent (pid int, txt text);
insert into pparent values (1,'parent1');
insert into pparent values (2,'parent2');

create table cchild (pid int, descrip text);
insert into cchild values (1,'descrip1');

create view vview as
  select pparent.pid, txt, descrip from
    pparent left join cchild using (pid);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rrule as
--dg   on update to vview do instead
--dg (
--dg   insert into cchild (pid, descrip)
--dg     select old.pid, new.descrip where old.descrip isnull;
--dg   update cchild set descrip = new.descrip where cchild.pid = old.pid;
--dg );

select * from vview;
--dg-cascade
--dg update vview set descrip='test1' where pid=1;
--dg-cascade
--dg select * from vview;
--dg-cascade
--dg update vview set descrip='test2' where pid=2;
--dg-cascade
--dg select * from vview;
--dg-cascade
--dg update vview set descrip='test3' where pid=3;
--dg-cascade
--dg select * from vview;
--dg-cascade
--dg select * from cchild;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg drop rule rrule on vview;
drop view vview;
drop table pparent;
drop table cchild;


--
-- Check that ruleutils are working
--

-- temporarily disable fancy output, so view changes create less diff noise
\a\t

--dg-differs: the result
--dg SELECT viewname, definition FROM pg_views
--dg WHERE schemaname = 'pg_catalog'
--dg ORDER BY viewname;

--dg-skip: relation "?" does not exist
--dg SELECT tablename, rulename, definition FROM pg_rules
--dg WHERE schemaname = 'pg_catalog'
--dg ORDER BY tablename, rulename;

-- restore normal output mode
\a\t

--
-- CREATE OR REPLACE RULE
--

CREATE TABLE ruletest_tbl (a int, b int);
CREATE TABLE ruletest_tbl2 (a int, b int);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg CREATE OR REPLACE RULE myrule AS ON INSERT TO ruletest_tbl
--dg 	DO INSTEAD INSERT INTO ruletest_tbl2 VALUES (10, 10);

INSERT INTO ruletest_tbl VALUES (99, 99);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg CREATE OR REPLACE RULE myrule AS ON INSERT TO ruletest_tbl
--dg 	DO INSTEAD INSERT INTO ruletest_tbl2 VALUES (1000, 1000);

INSERT INTO ruletest_tbl VALUES (99, 99);

--dg-cascade
--dg SELECT * FROM ruletest_tbl2;

-- Check that rewrite rules splitting one INSERT into multiple
-- conditional statements does not disable FK checking.
create table rule_and_refint_t1 (
	id1a integer,
	id1b integer,

	primary key (id1a, id1b)
);

create table rule_and_refint_t2 (
	id2a integer,
	id2c integer,

	primary key (id2a, id2c)
);

create table rule_and_refint_t3 (
	id3a integer,
	id3b integer,
	id3c integer,
	data text,

	primary key (id3a, id3b, id3c),

	foreign key (id3a, id3b) references rule_and_refint_t1 (id1a, id1b),
	foreign key (id3a, id3c) references rule_and_refint_t2 (id2a, id2c)
);


insert into rule_and_refint_t1 values (1, 11);
insert into rule_and_refint_t1 values (1, 12);
insert into rule_and_refint_t1 values (2, 21);
insert into rule_and_refint_t1 values (2, 22);

insert into rule_and_refint_t2 values (1, 11);
insert into rule_and_refint_t2 values (1, 12);
insert into rule_and_refint_t2 values (2, 21);
insert into rule_and_refint_t2 values (2, 22);

insert into rule_and_refint_t3 values (1, 11, 11, 'row1');
insert into rule_and_refint_t3 values (1, 11, 12, 'row2');
insert into rule_and_refint_t3 values (1, 12, 11, 'row3');
insert into rule_and_refint_t3 values (1, 12, 12, 'row4');
insert into rule_and_refint_t3 values (1, 11, 13, 'row5');
insert into rule_and_refint_t3 values (1, 13, 11, 'row6');
-- Ordinary table
insert into rule_and_refint_t3 values (1, 13, 11, 'row6')
  on conflict do nothing;
-- rule not fired, so fk violation
insert into rule_and_refint_t3 values (1, 13, 11, 'row6')
  on conflict (id3a, id3b, id3c) do update
  set id3b = excluded.id3b;
-- rule fired, so unsupported
--dg-cascade
--dg insert into shoelace values ('sl9', 0, 'pink', 35.0, 'inch', 0.0)
--dg   on conflict (sl_name) do update
--dg   set sl_avail = excluded.sl_avail;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule rule_and_refint_t3_ins as on insert to rule_and_refint_t3
--dg 	where (exists (select 1 from rule_and_refint_t3
--dg 			where (((rule_and_refint_t3.id3a = new.id3a)
--dg 			and (rule_and_refint_t3.id3b = new.id3b))
--dg 			and (rule_and_refint_t3.id3c = new.id3c))))
--dg 	do instead update rule_and_refint_t3 set data = new.data
--dg 	where (((rule_and_refint_t3.id3a = new.id3a)
--dg 	and (rule_and_refint_t3.id3b = new.id3b))
--dg 	and (rule_and_refint_t3.id3c = new.id3c));

insert into rule_and_refint_t3 values (1, 11, 13, 'row7');
insert into rule_and_refint_t3 values (1, 13, 11, 'row8');

--
-- disallow dropping a view's rule (bug #5072)
--

create view rules_fooview as select 'rules_foo'::text;
--dg-differs: another error: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg drop rule "_RETURN" on rules_fooview;
drop view rules_fooview;

--
-- We used to allow converting a table to a view by creating a "_RETURN"
-- rule for it, but no more.
--

create table rules_fooview (x int, y text);
create rule "_RETURN" as on select to rules_fooview do instead
  select 1 as x, 'aaa'::text as y;
drop table rules_fooview;

-- likewise, converting a partitioned table or partition to view is not allowed
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table rules_fooview (x int, y text) partition by list (x);
--dg-cascade
--dg create rule "_RETURN" as on select to rules_fooview do instead
--dg   select 1 as x, 'aaa'::text as y;

--dg-cascade
--dg create table rules_fooview_part partition of rules_fooview for values in (1);
--dg-cascade
--dg create rule "_RETURN" as on select to rules_fooview_part do instead
--dg   select 1 as x, 'aaa'::text as y;

--dg-cascade
--dg drop table rules_fooview;

--
-- check for planner problems with complex inherited UPDATES
--

create table id (id serial primary key, name text);
-- currently, must respecify PKEY for each inherited subtable
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table test_1 (id integer primary key) inherits (id);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table test_2 (id integer primary key) inherits (id);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table test_3 (id integer primary key) inherits (id);

--dg-cascade
--dg insert into test_1 (name) values ('Test 1');
--dg-cascade
--dg insert into test_1 (name) values ('Test 2');
--dg-cascade
--dg insert into test_2 (name) values ('Test 3');
--dg-cascade
--dg insert into test_2 (name) values ('Test 4');
--dg-cascade
--dg insert into test_3 (name) values ('Test 5');
--dg-cascade
--dg insert into test_3 (name) values ('Test 6');

create view id_ordered as select * from id order by id;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule update_id_ordered as on update to id_ordered
--dg 	do instead update id set name = new.name where id = old.id;

--dg-cascade
--dg select * from id_ordered;
update id_ordered set name = 'update 2' where id = 2;
update id_ordered set name = 'update 4' where id = 4;
update id_ordered set name = 'update 5' where id = 5;
--dg-cascade
--dg select * from id_ordered;

--dg-cascade
--dg drop table id cascade;

--
-- check corner case where an entirely-dummy subplan is created by
-- constraint exclusion
--

create temp table t1 (a integer primary key);

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create temp table t1_1 (check (a >= 0 and a < 10)) inherits (t1);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create temp table t1_2 (check (a >= 10 and a < 20)) inherits (t1);

--dg-cascade
--dg create rule t1_ins_1 as on insert to t1
--dg 	where new.a >= 0 and new.a < 10
--dg 	do instead
--dg 	insert into t1_1 values (new.a);
--dg-cascade
--dg create rule t1_ins_2 as on insert to t1
--dg 	where new.a >= 10 and new.a < 20
--dg 	do instead
--dg 	insert into t1_2 values (new.a);

--dg-cascade
--dg create rule t1_upd_1 as on update to t1
--dg 	where old.a >= 0 and old.a < 10
--dg 	do instead
--dg 	update t1_1 set a = new.a where a = old.a;
--dg-cascade
--dg create rule t1_upd_2 as on update to t1
--dg 	where old.a >= 10 and old.a < 20
--dg 	do instead
--dg 	update t1_2 set a = new.a where a = old.a;

set constraint_exclusion = on;

insert into t1 select * from generate_series(5,19,1) g;
update t1 set a = 4 where a = 5;

--dg-cascade
--dg select * from only t1;
--dg-cascade
--dg select * from only t1_1;
--dg-cascade
--dg select * from only t1_2;

reset constraint_exclusion;

-- test FOR UPDATE in rules

create table rules_base(f1 int, f2 int);
insert into rules_base values(1,2), (11,12);
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule r1 as on update to rules_base do instead
--dg   select * from rules_base where f1 = 1 for update;
--dg-cascade
--dg update rules_base set f2 = f2 + 1;
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create or replace rule r1 as on update to rules_base do instead
--dg   select * from rules_base where f1 = 11 for update of rules_base;
--dg-cascade
--dg update rules_base set f2 = f2 + 1;
create or replace rule r1 as on update to rules_base do instead
  select * from rules_base where f1 = 11 for update of old; -- error
drop table rules_base;

-- test various flavors of pg_get_viewdef()

select pg_get_viewdef('shoe'::regclass) as unpretty;
select pg_get_viewdef('shoe'::regclass,true) as pretty;
select pg_get_viewdef('shoe'::regclass,0) as prettier;

--
-- check multi-row VALUES in rules
--

create table rules_src(f1 int, f2 int default 0);
create table rules_log(f1 int, f2 int, tag text, id serial);
insert into rules_src values(1,2), (11,12);
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule r1 as on update to rules_src do also
--dg   insert into rules_log values(old.*, 'old', default), (new.*, 'new', default);
update rules_src set f2 = f2 + 1;
update rules_src set f2 = f2 * 10;
select * from rules_src;
--dg-cascade
--dg select * from rules_log;
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule r2 as on update to rules_src do also
--dg   values(old.*, 'old'), (new.*, 'new');
--dg-cascade
--dg update rules_src set f2 = f2 / 10;
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule r3 as on insert to rules_src do also
--dg   insert into rules_log values(null, null, '-', default), (new.*, 'new', default);
insert into rules_src values(22,23), (33,default);
--dg-cascade
--dg select * from rules_src;
--dg-cascade
--dg select * from rules_log;
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule r4 as on delete to rules_src do notify rules_src_deletion;

--
-- Ensure an aliased target relation for insert is correctly deparsed.
--
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule r5 as on insert to rules_src do instead insert into rules_log AS trgt SELECT NEW.* RETURNING trgt.f1, trgt.f2;
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule r6 as on update to rules_src do instead UPDATE rules_log AS trgt SET tag = 'updated' WHERE trgt.f1 = new.f1;

--
-- Check deparse disambiguation of INSERT/UPDATE/DELETE targets.
--
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg create rule r7 as on delete to rules_src do instead
--dg   with wins as (insert into int4_tbl as trgt values (0) returning *),
--dg        wupd as (update int4_tbl trgt set f1 = f1+1 returning *),
--dg        wdel as (delete from int4_tbl trgt where f1 = 0 returning *)
--dg   insert into rules_log AS trgt select old.* from wins, wupd, wdel
--dg   returning trgt.f1, trgt.f2;

-- check display of all rules added above
--dg-cascade
--dg \d+ rules_src

--
-- Also check multiassignment deparsing.
--
create table rule_t1(f1 int, f2 int);
--dg-skip: type ? is not supported yet
--dg create table rule_dest(f1 int, f2 int[], tag text);
--dg-cascade
--dg create rule rr as on update to rule_t1 do instead UPDATE rule_dest trgt
--dg   SET (f2[1], f1, tag) = (SELECT new.f2, new.f1, 'updated'::varchar)
--dg   WHERE trgt.f1 = new.f1 RETURNING new.*;
--dg-cascade
--dg \d+ rule_t1
--dg-cascade
--dg drop table rule_t1, rule_dest;

--
-- Test implicit LATERAL references to old/new in rules
--
--dg-cascade
--dg CREATE TABLE rule_t1(a int, b text DEFAULT 'xxx', c int);
CREATE VIEW rule_v1 AS SELECT * FROM rule_t1;
--dg-cascade
--dg CREATE RULE v1_ins AS ON INSERT TO rule_v1
--dg   DO ALSO INSERT INTO rule_t1
--dg   SELECT * FROM (SELECT a + 10 FROM rule_t1 WHERE a = NEW.a) tt;
--dg-cascade
--dg CREATE RULE v1_upd AS ON UPDATE TO rule_v1
--dg   DO ALSO UPDATE rule_t1 t
--dg   SET c = tt.a * 10
--dg   FROM (SELECT a FROM rule_t1 WHERE a = OLD.a) tt WHERE t.a = tt.a;
--dg-cascade
--dg INSERT INTO rule_v1 VALUES (1, 'a'), (2, 'b');
--dg-cascade
--dg UPDATE rule_v1 SET b = upper(b);
--dg-cascade
--dg SELECT * FROM rule_t1;
DROP TABLE rule_t1 CASCADE;

--
-- check alter rename rule
--
CREATE TABLE rule_t1 (a INT);
CREATE VIEW rule_v1 AS SELECT * FROM rule_t1;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg CREATE RULE InsertRule AS
--dg     ON INSERT TO rule_v1
--dg     DO INSTEAD
--dg         INSERT INTO rule_t1 VALUES(new.a);

--dg-skip: relation pg_class cannot be opened: digres has no catalog tables
--dg ALTER RULE InsertRule ON rule_v1 RENAME to NewInsertRule;

INSERT INTO rule_v1 VALUES(1);
SELECT * FROM rule_v1;

--dg-cascade
--dg \d+ rule_v1

--
-- error conditions for alter rename rule
--
--dg-differs: another error: relation pg_class cannot be opened: digres has no catalog tables
--dg ALTER RULE InsertRule ON rule_v1 RENAME TO NewInsertRule; -- doesn't exist
--dg-cascade
--dg ALTER RULE NewInsertRule ON rule_v1 RENAME TO "_RETURN"; -- already exists
--dg-differs: another error: relation pg_class cannot be opened: digres has no catalog tables
--dg ALTER RULE "_RETURN" ON rule_v1 RENAME TO abc; -- ON SELECT rule cannot be renamed
--dg-cascade
--dg ALTER RULE rtest_t4_ins1 ON rtest_t4 RENAME TO "_RETURN"; -- also disallowed

DROP VIEW rule_v1;
DROP TABLE rule_t1;

--
-- check display of VALUES in view definitions
--
create view rule_v1 as values(1,2);
\d+ rule_v1
alter table rule_v1 rename column column2 to q2;
\d+ rule_v1
drop view rule_v1;
create view rule_v1(x) as values(1,2);
\d+ rule_v1
drop view rule_v1;
create view rule_v1(x) as select * from (values(1,2)) v;
\d+ rule_v1
drop view rule_v1;
create view rule_v1(x) as select * from (values(1,2)) v(q,w);
\d+ rule_v1
drop view rule_v1;

--
-- Check DO INSTEAD rules with ON CONFLICT
--
CREATE TABLE hats (
	hat_name    char(10) primary key,
	hat_color   char(10)      -- hat color
);

CREATE TABLE hat_data (
	hat_name    char(10),
	hat_color   char(10)      -- hat color
);
--dg-skip: operator class bpchar_pattern_ops is not supported yet
--dg create unique index hat_data_unique_idx
--dg   on hat_data (hat_name COLLATE "C" bpchar_pattern_ops);

-- DO NOTHING with ON CONFLICT
--dg-skip: relation pg_opclass cannot be opened: digres has no catalog tables
--dg CREATE RULE hat_nosert AS ON INSERT TO hats
--dg     DO INSTEAD
--dg     INSERT INTO hat_data VALUES (
--dg            NEW.hat_name,
--dg            NEW.hat_color)
--dg         ON CONFLICT (hat_name COLLATE "C" bpchar_pattern_ops) WHERE hat_color = 'green'
--dg         DO NOTHING
--dg         RETURNING *;
--dg-cascade
--dg SELECT definition FROM pg_rules WHERE tablename = 'hats' ORDER BY rulename;

-- Works (projects row)
INSERT INTO hats VALUES ('h7', 'black') RETURNING *;
-- Works (does nothing)
--dg-cascade
--dg INSERT INTO hats VALUES ('h7', 'black') RETURNING *;
--dg-cascade
--dg SELECT tablename, rulename, definition FROM pg_rules
--dg 	WHERE tablename = 'hats';
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg DROP RULE hat_nosert ON hats;

-- DO NOTHING without ON CONFLICT
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg CREATE RULE hat_nosert_all AS ON INSERT TO hats
--dg     DO INSTEAD
--dg     INSERT INTO hat_data VALUES (
--dg            NEW.hat_name,
--dg            NEW.hat_color)
--dg         ON CONFLICT
--dg         DO NOTHING
--dg         RETURNING *;
--dg-cascade
--dg SELECT definition FROM pg_rules WHERE tablename = 'hats' ORDER BY rulename;
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg DROP RULE hat_nosert_all ON hats;

-- Works (does nothing)
--dg-cascade
--dg INSERT INTO hats VALUES ('h7', 'black') RETURNING *;

-- DO UPDATE with a WHERE clause
--dg-skip: relation "?" does not have a composite type
--dg CREATE RULE hat_upsert AS ON INSERT TO hats
--dg     DO INSTEAD
--dg     INSERT INTO hat_data VALUES (
--dg            NEW.hat_name,
--dg            NEW.hat_color)
--dg         ON CONFLICT (hat_name)
--dg         DO UPDATE
--dg            SET hat_name = hat_data.hat_name, hat_color = excluded.hat_color
--dg            WHERE excluded.hat_color <>  'forbidden' AND hat_data.* != excluded.*
--dg         RETURNING *;
--dg-cascade
--dg SELECT definition FROM pg_rules WHERE tablename = 'hats' ORDER BY rulename;

-- Works (does upsert)
--dg-cascade
--dg INSERT INTO hats VALUES ('h8', 'black') RETURNING *;
--dg-cascade
--dg SELECT * FROM hat_data WHERE hat_name = 'h8';
--dg-cascade
--dg INSERT INTO hats VALUES ('h8', 'white') RETURNING *;
--dg-cascade
--dg SELECT * FROM hat_data WHERE hat_name = 'h8';
--dg-cascade
--dg INSERT INTO hats VALUES ('h8', 'forbidden') RETURNING *;
--dg-cascade
--dg SELECT * FROM hat_data WHERE hat_name = 'h8';
--dg-cascade
--dg SELECT tablename, rulename, definition FROM pg_rules
--dg 	WHERE tablename = 'hats';
-- ensure explain works for on insert conflict rules
--dg-cascade
--dg explain (costs off) INSERT INTO hats VALUES ('h8', 'forbidden') RETURNING *;

-- ensure upserting into a rule, with a CTE (different offsets!) works
--dg-cascade
--dg WITH data(hat_name, hat_color) AS MATERIALIZED (
--dg     VALUES ('h8', 'green'),
--dg         ('h9', 'blue'),
--dg         ('h7', 'forbidden')
--dg )
--dg INSERT INTO hats
--dg     SELECT * FROM data
--dg RETURNING *;
--dg-cascade
--dg EXPLAIN (costs off)
--dg WITH data(hat_name, hat_color) AS MATERIALIZED (
--dg     VALUES ('h8', 'green'),
--dg         ('h9', 'blue'),
--dg         ('h7', 'forbidden')
--dg )
--dg INSERT INTO hats
--dg     SELECT * FROM data
--dg RETURNING *;
--dg-cascade
--dg SELECT * FROM hat_data WHERE hat_name IN ('h8', 'h9', 'h7') ORDER BY hat_name;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg DROP RULE hat_upsert ON hats;

drop table hats;
drop table hat_data;

-- test for pg_get_functiondef properly regurgitating SET parameters
-- Note that the function is kept around to stress pg_dump.
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION func_with_set_params() RETURNS integer
--dg     AS 'select 1;'
--dg     LANGUAGE SQL
--dg     SET search_path TO PG_CATALOG
--dg     SET extra_float_digits TO 2
--dg     SET work_mem TO '4MB'
--dg     SET datestyle to iso, mdy
--dg     SET local_preload_libraries TO "Mixed/Case", 'c:/''a"/path', '', '0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789'
--dg     IMMUTABLE STRICT;
--dg-cascade
--dg SELECT pg_get_functiondef('func_with_set_params()'::regprocedure);

-- tests for pg_get_*def with invalid objects
SELECT pg_get_constraintdef(0);
SELECT pg_get_functiondef(0);
SELECT pg_get_indexdef(0);
SELECT pg_get_ruledef(0);
--dg-skip: relation pg_statistic_ext cannot be opened: digres has no catalog tables
--dg SELECT pg_get_statisticsobjdef(0);
--dg-skip: relation pg_trigger cannot be opened: digres has no catalog tables
--dg SELECT pg_get_triggerdef(0);
SELECT pg_get_viewdef(0);
SELECT pg_get_function_arguments(0);
SELECT pg_get_function_identity_arguments(0);
SELECT pg_get_function_result(0);
SELECT pg_get_function_arg_default(0, 0);
SELECT pg_get_function_arg_default('pg_class'::regclass, 0);
--dg-skip: relation pg_partitioned_table cannot be opened: digres has no catalog tables
--dg SELECT pg_get_partkeydef(0);

-- test rename for a rule defined on a partitioned table
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE rules_parted_table (a int) PARTITION BY LIST (a);
--dg-cascade
--dg CREATE TABLE rules_parted_table_1 PARTITION OF rules_parted_table FOR VALUES IN (1);
--dg-cascade
--dg CREATE RULE rules_parted_table_insert AS ON INSERT to rules_parted_table
--dg     DO INSTEAD INSERT INTO rules_parted_table_1 VALUES (NEW.*);
--dg-skip: relation pg_class cannot be opened: digres has no catalog tables
--dg ALTER RULE rules_parted_table_insert ON rules_parted_table RENAME TO rules_parted_table_insert_redirect;
--dg-cascade
--dg DROP TABLE rules_parted_table;

--
-- test MERGE
--
CREATE TABLE rule_merge1 (a int, b text);
CREATE TABLE rule_merge2 (a int, b text);
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg CREATE RULE rule1 AS ON INSERT TO rule_merge1
--dg 	DO INSTEAD INSERT INTO rule_merge2 VALUES (NEW.*);
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg CREATE RULE rule2 AS ON UPDATE TO rule_merge1
--dg 	DO INSTEAD UPDATE rule_merge2 SET a = NEW.a, b = NEW.b
--dg 	WHERE a = OLD.a;
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg CREATE RULE rule3 AS ON DELETE TO rule_merge1
--dg 	DO INSTEAD DELETE FROM rule_merge2 WHERE a = OLD.a;

-- MERGE not supported for table with rules
--dg-cascade
--dg MERGE INTO rule_merge1 t USING (SELECT 1 AS a) s
--dg 	ON t.a = s.a
--dg 	WHEN MATCHED AND t.a < 2 THEN
--dg 		UPDATE SET b = b || ' updated by merge'
--dg 	WHEN MATCHED AND t.a > 2 THEN
--dg 		DELETE
--dg 	WHEN NOT MATCHED THEN
--dg 		INSERT VALUES (s.a, '');

-- should be ok with the other table though
--dg-skip: MERGE does not write to table "?" yet
--dg MERGE INTO rule_merge2 t USING (SELECT 1 AS a) s
--dg 	ON t.a = s.a
--dg 	WHEN MATCHED AND t.a < 2 THEN
--dg 		UPDATE SET b = b || ' updated by merge'
--dg 	WHEN MATCHED AND t.a > 2 THEN
--dg 		DELETE
--dg 	WHEN NOT MATCHED THEN
--dg 		INSERT VALUES (s.a, '');

-- also ok if the rules are disabled
--dg-cascade
--dg ALTER TABLE rule_merge1 DISABLE RULE rule1;
--dg-cascade
--dg ALTER TABLE rule_merge1 DISABLE RULE rule2;
--dg-cascade
--dg ALTER TABLE rule_merge1 DISABLE RULE rule3;
--dg-skip: MERGE does not write to table "?" yet
--dg MERGE INTO rule_merge1 t USING (SELECT 1 AS a) s
--dg 	ON t.a = s.a
--dg 	WHEN MATCHED AND t.a < 2 THEN
--dg 		UPDATE SET b = b || ' updated by merge'
--dg 	WHEN MATCHED AND t.a > 2 THEN
--dg 		DELETE
--dg 	WHEN NOT MATCHED THEN
--dg 		INSERT VALUES (s.a, '');

-- test deparsing
--dg-skip: type ? is not supported yet
--dg CREATE TABLE sf_target(id int, data text, filling int[]);

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION merge_sf_test()
--dg  RETURNS TABLE(action text, a int, b text,
--dg                id int, data text, filling int[],
--dg                old_id int, old_data text, old_filling int[],
--dg                new_id int, new_data text, new_filling int[])
--dg  LANGUAGE sql
--dg BEGIN ATOMIC
--dg  MERGE INTO sf_target t
--dg    USING rule_merge1 s
--dg    ON (s.a = t.id)
--dg WHEN MATCHED
--dg    AND (s.a + t.id) = 42
--dg    THEN UPDATE SET data = repeat(t.data, s.a) || s.b, id = length(s.b)
--dg WHEN NOT MATCHED
--dg    AND (s.b IS NOT NULL)
--dg    THEN INSERT (data, id)
--dg    VALUES (s.b, s.a)
--dg WHEN MATCHED
--dg    AND length(s.b || t.data) > 10
--dg    THEN UPDATE SET data = s.b
--dg WHEN MATCHED
--dg    AND s.a > 200
--dg    THEN UPDATE SET filling[s.a] = t.id
--dg WHEN MATCHED
--dg    AND s.a > 100
--dg    THEN DELETE
--dg WHEN MATCHED
--dg    THEN DO NOTHING
--dg WHEN NOT MATCHED
--dg    AND s.a > 200
--dg    THEN INSERT DEFAULT VALUES
--dg WHEN NOT MATCHED
--dg    AND s.a > 100
--dg    THEN INSERT (id, data) OVERRIDING USER VALUE
--dg    VALUES (s.a, DEFAULT)
--dg WHEN NOT MATCHED
--dg    AND s.a > 0
--dg    THEN INSERT
--dg    VALUES (s.a, s.b, DEFAULT)
--dg WHEN NOT MATCHED
--dg    THEN INSERT (filling[1], id)
--dg    VALUES (s.a, s.a)
--dg RETURNING
--dg    WITH (OLD AS o, NEW AS n)
--dg    merge_action() AS action, *, o.*, n.*;
--dg END;

--dg-cascade
--dg \sf merge_sf_test

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION merge_sf_test2()
--dg  RETURNS void
--dg  LANGUAGE sql
--dg BEGIN ATOMIC
--dg  MERGE INTO sf_target t
--dg    USING rule_merge1 s
--dg    ON (s.a = t.id)
--dg WHEN NOT MATCHED
--dg    THEN INSERT (data, id)
--dg    VALUES (s.a, s.a)
--dg WHEN MATCHED
--dg    THEN UPDATE SET data = s.b
--dg WHEN NOT MATCHED BY SOURCE
--dg    THEN DELETE;
--dg END;

--dg-cascade
--dg \sf merge_sf_test2

--dg-cascade
--dg DROP FUNCTION merge_sf_test;
--dg-cascade
--dg DROP FUNCTION merge_sf_test2;
--dg-cascade
--dg DROP TABLE sf_target;

--
-- Test enabling/disabling
--
CREATE TABLE ruletest1 (a int);
CREATE TABLE ruletest2 (b int);

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg CREATE RULE rule1 AS ON INSERT TO ruletest1
--dg     DO INSTEAD INSERT INTO ruletest2 VALUES (NEW.*);

INSERT INTO ruletest1 VALUES (1);
--dg-cascade
--dg ALTER TABLE ruletest1 DISABLE RULE rule1;
INSERT INTO ruletest1 VALUES (2);
--dg-cascade
--dg ALTER TABLE ruletest1 ENABLE RULE rule1;
SET session_replication_role = replica;
INSERT INTO ruletest1 VALUES (3);
--dg-cascade
--dg ALTER TABLE ruletest1 ENABLE REPLICA RULE rule1;
INSERT INTO ruletest1 VALUES (4);
RESET session_replication_role;
INSERT INTO ruletest1 VALUES (5);

--dg-cascade
--dg SELECT * FROM ruletest1;
--dg-cascade
--dg SELECT * FROM ruletest2;

DROP TABLE ruletest1;
DROP TABLE ruletest2;

--
-- Test non-SELECT rule on security invoker view.
-- Should use view owner's permissions.
--
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE USER regress_rule_user1;

CREATE TABLE ruletest_t1 (x int);
CREATE TABLE ruletest_t2 (x int);
CREATE VIEW ruletest_v1 WITH (security_invoker=true) AS
    SELECT * FROM ruletest_t1;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg GRANT INSERT ON ruletest_v1 TO regress_rule_user1;

--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg CREATE RULE rule1 AS ON INSERT TO ruletest_v1
--dg     DO INSTEAD INSERT INTO ruletest_t2 VALUES (NEW.*);

--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_rule_user1;
INSERT INTO ruletest_v1 VALUES (1);

RESET SESSION AUTHORIZATION;

-- Test that main query's relation's permissions are checked before
-- the rule action's relation's.
CREATE TABLE ruletest_t3 (x int);
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg CREATE RULE rule2 AS ON UPDATE TO ruletest_t1
--dg     DO INSTEAD INSERT INTO ruletest_t2 VALUES (OLD.*);
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg REVOKE ALL ON ruletest_t2 FROM regress_rule_user1;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg REVOKE ALL ON ruletest_t3 FROM regress_rule_user1;
--dg-cascade
--dg ALTER TABLE ruletest_t1 OWNER TO regress_rule_user1;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_rule_user1;
--dg-cascade
--dg UPDATE ruletest_t1 t1 SET x = 0 FROM ruletest_t3 t3 WHERE t1.x = t3.x;

RESET SESSION AUTHORIZATION;
--dg-cascade
--dg SELECT * FROM ruletest_t1;
--dg-cascade
--dg SELECT * FROM ruletest_t2;

DROP VIEW ruletest_v1;
--dg-skip: relation pg_rewrite cannot be opened: digres has no catalog tables
--dg DROP RULE rule2 ON ruletest_t1;
DROP TABLE ruletest_t3;
DROP TABLE ruletest_t2;
DROP TABLE ruletest_t1;

--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP USER regress_rule_user1;
