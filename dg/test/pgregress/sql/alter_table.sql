--
-- ALTER_TABLE
--

-- Clean up in case a prior regression run failed
SET client_min_messages TO 'warning';
DROP ROLE IF EXISTS regress_alter_table_user1;
RESET client_min_messages;

CREATE USER regress_alter_table_user1;

--
-- add attribute
--

CREATE TABLE attmp (initial int4);

COMMENT ON TABLE attmp_wrong IS 'table comment';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON TABLE attmp IS 'table comment';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON TABLE attmp IS NULL;

ALTER TABLE attmp ADD COLUMN xmin integer; -- fails

ALTER TABLE attmp ADD COLUMN a int4 default 3;

ALTER TABLE attmp ADD COLUMN b name;

ALTER TABLE attmp ADD COLUMN c text;

ALTER TABLE attmp ADD COLUMN d float8;

ALTER TABLE attmp ADD COLUMN e float4;

ALTER TABLE attmp ADD COLUMN f int2;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN g polygon;

ALTER TABLE attmp ADD COLUMN i char;

ALTER TABLE attmp ADD COLUMN k int4;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN l tid;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN m xid;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN n oidvector;

--ALTER TABLE attmp ADD COLUMN o lock;
ALTER TABLE attmp ADD COLUMN p boolean;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN q point;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN r lseg;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN s path;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN t box;

ALTER TABLE attmp ADD COLUMN v timestamp;

ALTER TABLE attmp ADD COLUMN w interval;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN x float8[];

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN y float4[];

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN z int2[];

--dg-cascade
--dg INSERT INTO attmp (a, b, c, d, e, f, g,    i,    k, l, m, n, p, q, r, s, t,
--dg 	v, w, x, y, z)
--dg    VALUES (4, 'name', 'text', 4.1, 4.1, 2, '(4.1,4.1,3.1,3.1)',
--dg 	'c',
--dg 	314159, '(1,1)', '512',
--dg 	'1 2 3 4 5 6 7 8', true, '(1.1,1.1)', '(4.1,4.1,3.1,3.1)',
--dg 	'(0,2,4.1,4.1,3.1,3.1)', '(4.1,4.1,3.1,3.1)',
--dg 	'epoch', '01:00:10', '{1.0,2.0,3.0,4.0}', '{1.0,2.0,3.0,4.0}', '{1,2,3,4}');

--dg-cascade
--dg SELECT * FROM attmp;

DROP TABLE attmp;

-- the wolf bug - schema mods caused inconsistent row descriptors
CREATE TABLE attmp (
	initial 	int4
);

ALTER TABLE attmp ADD COLUMN a int4;

ALTER TABLE attmp ADD COLUMN b name;

ALTER TABLE attmp ADD COLUMN c text;

ALTER TABLE attmp ADD COLUMN d float8;

ALTER TABLE attmp ADD COLUMN e float4;

ALTER TABLE attmp ADD COLUMN f int2;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN g polygon;

ALTER TABLE attmp ADD COLUMN i char;

ALTER TABLE attmp ADD COLUMN k int4;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN l tid;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN m xid;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN n oidvector;

--ALTER TABLE attmp ADD COLUMN o lock;
ALTER TABLE attmp ADD COLUMN p boolean;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN q point;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN r lseg;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN s path;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN t box;

ALTER TABLE attmp ADD COLUMN v timestamp;

ALTER TABLE attmp ADD COLUMN w interval;

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN x float8[];

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN y float4[];

--dg-skip: type ? is not supported yet
--dg ALTER TABLE attmp ADD COLUMN z int2[];

--dg-cascade
--dg INSERT INTO attmp (a, b, c, d, e, f, g,    i,   k, l, m, n, p, q, r, s, t,
--dg 	v, w, x, y, z)
--dg    VALUES (4, 'name', 'text', 4.1, 4.1, 2, '(4.1,4.1,3.1,3.1)',
--dg         'c',
--dg 	314159, '(1,1)', '512',
--dg 	'1 2 3 4 5 6 7 8', true, '(1.1,1.1)', '(4.1,4.1,3.1,3.1)',
--dg 	'(0,2,4.1,4.1,3.1,3.1)', '(4.1,4.1,3.1,3.1)',
--dg 	'epoch', '01:00:10', '{1.0,2.0,3.0,4.0}', '{1.0,2.0,3.0,4.0}', '{1,2,3,4}');

--dg-cascade
--dg SELECT * FROM attmp;

CREATE INDEX attmp_idx ON attmp (a, (d + e), b);

ALTER INDEX attmp_idx ALTER COLUMN 0 SET STATISTICS 1000;

--dg-differs: another error: ALTER INDEX is not supported yet
--dg ALTER INDEX attmp_idx ALTER COLUMN 1 SET STATISTICS 1000;

--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX attmp_idx ALTER COLUMN 2 SET STATISTICS 1000;

--dg-cascade
--dg \d+ attmp_idx

--dg-differs: another error: ALTER INDEX is not supported yet
--dg ALTER INDEX attmp_idx ALTER COLUMN 3 SET STATISTICS 1000;

--dg-differs: another error: ALTER INDEX is not supported yet
--dg ALTER INDEX attmp_idx ALTER COLUMN 4 SET STATISTICS 1000;

--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX attmp_idx ALTER COLUMN 2 SET STATISTICS -1;

DROP TABLE attmp;


--
-- rename - check on both non-temp and temp tables
--
CREATE TABLE attmp (regtable int);
CREATE TEMP TABLE attmp (attmptable int);

ALTER TABLE attmp RENAME TO attmp_new;

SELECT * FROM attmp;
SELECT * FROM attmp_new;

ALTER TABLE attmp RENAME TO attmp_new2;

SELECT * FROM attmp;		-- should fail
SELECT * FROM attmp_new;
SELECT * FROM attmp_new2;

DROP TABLE attmp_new;
DROP TABLE attmp_new2;

-- check rename of partitioned tables and indexes also
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE part_attmp (a int primary key) partition by range (a);
--dg-cascade
--dg CREATE TABLE part_attmp1 PARTITION OF part_attmp FOR VALUES FROM (0) TO (100);
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX part_attmp_pkey RENAME TO part_attmp_index;
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX part_attmp1_pkey RENAME TO part_attmp1_index;
--dg-cascade
--dg ALTER TABLE part_attmp RENAME TO part_at2tmp;
--dg-cascade
--dg ALTER TABLE part_attmp1 RENAME TO part_at2tmp1;
SET ROLE regress_alter_table_user1;
--dg-cascade
--dg ALTER INDEX part_attmp_index RENAME TO fail;
--dg-cascade
--dg ALTER INDEX part_attmp1_index RENAME TO fail;
--dg-cascade
--dg ALTER TABLE part_at2tmp RENAME TO fail;
--dg-cascade
--dg ALTER TABLE part_at2tmp1 RENAME TO fail;
RESET ROLE;
--dg-cascade
--dg DROP TABLE part_at2tmp;

--
-- check renaming to a table's array type's autogenerated name
-- (the array type's name should get out of the way)
--
CREATE TABLE attmp_array (id int);
CREATE TABLE attmp_array2 (id int);
--dg-skip: type "?" does not exist
--dg SELECT typname FROM pg_type WHERE oid = 'attmp_array[]'::regtype;
--dg-skip: type "?" does not exist
--dg SELECT typname FROM pg_type WHERE oid = 'attmp_array2[]'::regtype;
ALTER TABLE attmp_array2 RENAME TO _attmp_array;
--dg-skip: type "?" does not exist
--dg SELECT typname FROM pg_type WHERE oid = 'attmp_array[]'::regtype;
--dg-skip: type "?" does not exist
--dg SELECT typname FROM pg_type WHERE oid = '_attmp_array[]'::regtype;
DROP TABLE _attmp_array;
DROP TABLE attmp_array;

-- renaming to table's own array type's name is an interesting corner case
CREATE TABLE attmp_array (id int);
--dg-skip: type "?" does not exist
--dg SELECT typname FROM pg_type WHERE oid = 'attmp_array[]'::regtype;
ALTER TABLE attmp_array RENAME TO _attmp_array;
--dg-skip: type "?" does not exist
--dg SELECT typname FROM pg_type WHERE oid = '_attmp_array[]'::regtype;
DROP TABLE _attmp_array;

-- ALTER TABLE ... RENAME on non-table relations
-- renaming indexes (FIXME: this should probably test the index's functionality)
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX IF EXISTS __onek_unique1 RENAME TO attmp_onek_unique1;
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX IF EXISTS __attmp_onek_unique1 RENAME TO onek_unique1;

--dg-differs: another error: ALTER INDEX is not supported yet
--dg ALTER INDEX onek_unique1 RENAME TO attmp_onek_unique1;
--dg-differs: another error: ALTER INDEX is not supported yet
--dg ALTER INDEX attmp_onek_unique1 RENAME TO onek_unique1;

SET ROLE regress_alter_table_user1;
--dg-differs: another error: ALTER INDEX is not supported yet
--dg ALTER INDEX onek_unique1 RENAME TO fail;  -- permission denied
RESET ROLE;

-- rename statements with mismatching statement and object types
CREATE TABLE alter_idx_rename_test (a INT);
CREATE INDEX alter_idx_rename_test_idx ON alter_idx_rename_test (a);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE alter_idx_rename_test_parted (a INT) PARTITION BY LIST (a);
--dg-cascade
--dg CREATE INDEX alter_idx_rename_test_parted_idx ON alter_idx_rename_test_parted (a);
BEGIN;
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX alter_idx_rename_test RENAME TO alter_idx_rename_test_2;
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX alter_idx_rename_test_parted RENAME TO alter_idx_rename_test_parted_2;
--dg-cascade
--dg SELECT relation::regclass, mode FROM pg_locks
--dg WHERE pid = pg_backend_pid() AND locktype = 'relation'
--dg   AND relation::regclass::text LIKE 'alter\_idx%'
--dg ORDER BY relation::regclass::text COLLATE "C";
COMMIT;
BEGIN;
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX alter_idx_rename_test_idx RENAME TO alter_idx_rename_test_idx_2;
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX alter_idx_rename_test_parted_idx RENAME TO alter_idx_rename_test_parted_idx_2;
--dg-cascade
--dg SELECT relation::regclass, mode FROM pg_locks
--dg WHERE pid = pg_backend_pid() AND locktype = 'relation'
--dg   AND relation::regclass::text LIKE 'alter\_idx%'
--dg ORDER BY relation::regclass::text COLLATE "C";
COMMIT;
BEGIN;
--dg-cascade
--dg ALTER TABLE alter_idx_rename_test_idx_2 RENAME TO alter_idx_rename_test_idx_3;
--dg-cascade
--dg ALTER TABLE alter_idx_rename_test_parted_idx_2 RENAME TO alter_idx_rename_test_parted_idx_3;
--dg-cascade
--dg SELECT relation::regclass, mode FROM pg_locks
--dg WHERE pid = pg_backend_pid() AND locktype = 'relation'
--dg   AND relation::regclass::text LIKE 'alter\_idx%'
--dg ORDER BY relation::regclass::text COLLATE "C";
COMMIT;
--dg-cascade
--dg DROP TABLE alter_idx_rename_test_2;

-- renaming views
CREATE VIEW attmp_view (unique1) AS SELECT unique1 FROM tenk1;
ALTER TABLE attmp_view RENAME TO attmp_view_new;

SET ROLE regress_alter_table_user1;
ALTER VIEW attmp_view_new RENAME TO fail;  -- permission denied
RESET ROLE;

-- hack to ensure we get an indexscan here
set enable_seqscan to off;
set enable_bitmapscan to off;
-- 5 values, sorted
SELECT unique1 FROM tenk1 WHERE unique1 < 5;
reset enable_seqscan;
reset enable_bitmapscan;

DROP VIEW attmp_view_new;
-- toast-like relation name
alter table stud_emp rename to pg_toast_stud_emp;
alter table pg_toast_stud_emp rename to stud_emp;

-- renaming index should rename constraint as well
ALTER TABLE onek ADD CONSTRAINT onek_unique1_constraint UNIQUE (unique1);
--dg-skip: ALTER INDEX is not supported yet
--dg ALTER INDEX onek_unique1_constraint RENAME TO onek_unique1_constraint_foo;
--dg-cascade
--dg ALTER TABLE onek DROP CONSTRAINT onek_unique1_constraint_foo;

-- renaming constraint
ALTER TABLE onek ADD CONSTRAINT onek_check_constraint CHECK (unique1 >= 0);
ALTER TABLE onek RENAME CONSTRAINT onek_check_constraint TO onek_check_constraint_foo;
ALTER TABLE onek DROP CONSTRAINT onek_check_constraint_foo;

-- renaming constraint should rename index as well
--dg-cascade
--dg ALTER TABLE onek ADD CONSTRAINT onek_unique1_constraint UNIQUE (unique1);
DROP INDEX onek_unique1_constraint;  -- to see whether it's there
--dg-skip: constraint "?" of relation "?" does not exist
--dg ALTER TABLE onek RENAME CONSTRAINT onek_unique1_constraint TO onek_unique1_constraint_foo;
--dg-cascade
--dg DROP INDEX onek_unique1_constraint_foo;  -- to see whether it's there
--dg-cascade
--dg ALTER TABLE onek DROP CONSTRAINT onek_unique1_constraint_foo;

-- renaming constraints vs. inheritance
CREATE TABLE constraint_rename_test (a int CONSTRAINT con1 CHECK (a > 0), b int, c int);
\d constraint_rename_test
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE constraint_rename_test2 (a int CONSTRAINT con1 CHECK (a > 0), d int) INHERITS (constraint_rename_test);
--dg-cascade
--dg \d constraint_rename_test2
--dg-cascade
--dg ALTER TABLE constraint_rename_test2 RENAME CONSTRAINT con1 TO con1foo; -- fail
--dg-cascade
--dg ALTER TABLE ONLY constraint_rename_test RENAME CONSTRAINT con1 TO con1foo; -- fail
--dg-cascade
--dg ALTER TABLE constraint_rename_test RENAME CONSTRAINT con1 TO con1foo; -- ok
--dg-cascade
--dg \d constraint_rename_test
--dg-cascade
--dg \d constraint_rename_test2
ALTER TABLE constraint_rename_test ADD CONSTRAINT con2 CHECK (b > 0) NO INHERIT;
ALTER TABLE ONLY constraint_rename_test RENAME CONSTRAINT con2 TO con2foo; -- ok
ALTER TABLE constraint_rename_test RENAME CONSTRAINT con2foo TO con2bar; -- ok
--dg-cascade
--dg \d constraint_rename_test
--dg-cascade
--dg \d constraint_rename_test2
ALTER TABLE constraint_rename_test ADD CONSTRAINT con3 PRIMARY KEY (a);
--dg-skip: constraint "?" of relation "?" does not exist
--dg ALTER TABLE constraint_rename_test RENAME CONSTRAINT con3 TO con3foo; -- ok
--dg-cascade
--dg \d constraint_rename_test
--dg-cascade
--dg \d constraint_rename_test2
--dg-cascade
--dg DROP TABLE constraint_rename_test2;
DROP TABLE constraint_rename_test;
ALTER TABLE IF EXISTS constraint_not_exist RENAME CONSTRAINT con3 TO con3foo; -- ok
ALTER TABLE IF EXISTS constraint_rename_test ADD CONSTRAINT con4 UNIQUE (a);

-- renaming constraints with cache reset of target relation
CREATE TABLE constraint_rename_cache (a int,
  CONSTRAINT chk_a CHECK (a > 0),
  PRIMARY KEY (a));
ALTER TABLE constraint_rename_cache
  RENAME CONSTRAINT chk_a TO chk_a_new;
--dg-skip: constraint "?" of relation "?" does not exist
--dg ALTER TABLE constraint_rename_cache
--dg   RENAME CONSTRAINT constraint_rename_cache_pkey TO constraint_rename_pkey_new;
--dg-skip: constraint "?" for table "?" does not exist
--dg CREATE TABLE like_constraint_rename_cache
--dg   (LIKE constraint_rename_cache INCLUDING ALL);
--dg-cascade
--dg \d like_constraint_rename_cache
DROP TABLE constraint_rename_cache;
--dg-cascade
--dg DROP TABLE like_constraint_rename_cache;

-- FOREIGN KEY CONSTRAINT adding TEST

CREATE TABLE attmp2 (a int primary key);

CREATE TABLE attmp3 (a int, b int);

CREATE TABLE attmp4 (a int, b int, unique(a,b));

CREATE TABLE attmp5 (a int, b int);

-- Insert rows into attmp2 (pktable)
INSERT INTO attmp2 values (1);
INSERT INTO attmp2 values (2);
INSERT INTO attmp2 values (3);
INSERT INTO attmp2 values (4);

-- Insert rows into attmp3
INSERT INTO attmp3 values (1,10);
INSERT INTO attmp3 values (1,20);
INSERT INTO attmp3 values (5,50);

-- Try (and fail) to add constraint due to invalid source columns
ALTER TABLE attmp3 add constraint attmpconstr foreign key(c) references attmp2 match full;

-- Try (and fail) to add constraint due to invalid destination columns explicitly given
ALTER TABLE attmp3 add constraint attmpconstr foreign key(a) references attmp2(b) match full;

-- Try (and fail) to add constraint due to invalid data
ALTER TABLE attmp3 add constraint attmpconstr foreign key (a) references attmp2 match full;

-- Delete failing row
DELETE FROM attmp3 where a=5;

-- Try (and succeed)
ALTER TABLE attmp3 add constraint attmpconstr foreign key (a) references attmp2 match full;
ALTER TABLE attmp3 drop constraint attmpconstr;

INSERT INTO attmp3 values (5,50);

-- Try NOT VALID and then VALIDATE CONSTRAINT, but fails. Delete failure then re-validate
ALTER TABLE attmp3 add constraint attmpconstr foreign key (a) references attmp2 match full NOT VALID;
ALTER TABLE attmp3 validate constraint attmpconstr;

-- Delete failing row
DELETE FROM attmp3 where a=5;

-- Try (and succeed) and repeat to show it works on already valid constraint
ALTER TABLE attmp3 validate constraint attmpconstr;
ALTER TABLE attmp3 validate constraint attmpconstr;

-- Try a non-verified CHECK constraint
ALTER TABLE attmp3 ADD CONSTRAINT b_greater_than_ten CHECK (b > 10); -- fail
ALTER TABLE attmp3 ADD CONSTRAINT b_greater_than_ten CHECK (b > 10) NOT VALID; -- succeeds
ALTER TABLE attmp3 ADD CONSTRAINT b_greater_than_ten_not_enforced CHECK (b > 10) NOT ENFORCED; -- succeeds
ALTER TABLE attmp3 VALIDATE CONSTRAINT b_greater_than_ten; -- fails
DELETE FROM attmp3 WHERE NOT b > 10;
ALTER TABLE attmp3 VALIDATE CONSTRAINT b_greater_than_ten; -- succeeds
ALTER TABLE attmp3 VALIDATE CONSTRAINT b_greater_than_ten; -- succeeds
ALTER TABLE attmp3 VALIDATE CONSTRAINT b_greater_than_ten_not_enforced; -- fail

-- Test inherited NOT VALID CHECK constraints
select * from attmp3;
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE attmp6 () INHERITS (attmp3);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE attmp7 () INHERITS (attmp3);

--dg-cascade
--dg INSERT INTO attmp6 VALUES (6, 30), (7, 16);
ALTER TABLE attmp3 ADD CONSTRAINT b_le_20 CHECK (b <= 20) NOT VALID;
--dg-cascade
--dg ALTER TABLE attmp3 VALIDATE CONSTRAINT b_le_20;	-- fails
--dg-cascade
--dg DELETE FROM attmp6 WHERE b > 20;
ALTER TABLE attmp3 VALIDATE CONSTRAINT b_le_20;	-- succeeds

-- An already validated constraint must not be revalidated
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION boo(int) RETURNS int IMMUTABLE STRICT LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'boo: %', $1; RETURN $1; END; $$;
--dg-cascade
--dg INSERT INTO attmp7 VALUES (8, 18);
--dg-cascade
--dg ALTER TABLE attmp7 ADD CONSTRAINT identity CHECK (b = boo(b));
--dg-cascade
--dg ALTER TABLE attmp3 ADD CONSTRAINT IDENTITY check (b = boo(b)) NOT VALID;
--dg-cascade
--dg ALTER TABLE attmp3 VALIDATE CONSTRAINT identity;

-- A NO INHERIT constraint should not be looked for in children during VALIDATE CONSTRAINT
create table parent_noinh_convalid (a int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table child_noinh_convalid () inherits (parent_noinh_convalid);
insert into parent_noinh_convalid values (1);
--dg-cascade
--dg insert into child_noinh_convalid values (1);
alter table parent_noinh_convalid add constraint check_a_is_2 check (a = 2) no inherit not valid;
-- fail, because of the row in parent
alter table parent_noinh_convalid validate constraint check_a_is_2;
delete from only parent_noinh_convalid;
-- ok (parent itself contains no violating rows)
alter table parent_noinh_convalid validate constraint check_a_is_2;
select convalidated from pg_constraint where conrelid = 'parent_noinh_convalid'::regclass and conname = 'check_a_is_2';
-- cleanup
--dg-cascade
--dg drop table parent_noinh_convalid, child_noinh_convalid;

-- Try (and fail) to create constraint from attmp5(a) to attmp4(a) - unique constraint on
-- attmp4 is a,b

ALTER TABLE attmp5 add constraint attmpconstr foreign key(a) references attmp4(a) match full;

--dg-cascade
--dg DROP TABLE attmp7;

--dg-cascade
--dg DROP TABLE attmp6;

DROP TABLE attmp5;

DROP TABLE attmp4;

DROP TABLE attmp3;

DROP TABLE attmp2;

-- NOT VALID with plan invalidation -- ensure we don't use a constraint for
-- exclusion until validated
set constraint_exclusion TO 'partition';
create table nv_parent (d date, check (false) no inherit not valid);
-- not valid constraint added at creation time should automatically become valid
\d nv_parent

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table nv_child_2010 () inherits (nv_parent);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table nv_child_2011 () inherits (nv_parent);
--dg-cascade
--dg alter table nv_child_2010 add check (d between '2010-01-01'::date and '2010-12-31'::date) not valid;
--dg-cascade
--dg alter table nv_child_2011 add check (d between '2011-01-01'::date and '2011-12-31'::date) not valid;
--dg-cascade
--dg explain (costs off) select * from nv_parent where d between '2011-08-01' and '2011-08-31';
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table nv_child_2009 (check (d between '2009-01-01'::date and '2009-12-31'::date)) inherits (nv_parent);
--dg-cascade
--dg explain (costs off) select * from nv_parent where d between '2011-08-01'::date and '2011-08-31'::date;
--dg-cascade
--dg explain (costs off) select * from nv_parent where d between '2009-08-01'::date and '2009-08-31'::date;
-- after validation, the constraint should be used
--dg-cascade
--dg alter table nv_child_2011 VALIDATE CONSTRAINT nv_child_2011_d_check;
--dg-cascade
--dg explain (costs off) select * from nv_parent where d between '2009-08-01'::date and '2009-08-31'::date;

-- add an inherited NOT VALID constraint
alter table nv_parent add check (d between '2001-01-01'::date and '2099-12-31'::date) not valid;
--dg-cascade
--dg \d nv_child_2009
-- we leave nv_parent and children around to help test pg_dump logic

-- Foreign key adding test with mixed types

-- Note: these tables are TEMP to avoid name conflicts when this test
-- is run in parallel with foreign_key.sql.

CREATE TEMP TABLE PKTABLE (ptest1 int PRIMARY KEY);
INSERT INTO PKTABLE VALUES(42);
--dg-skip: type ? is not supported yet
--dg CREATE TEMP TABLE FKTABLE (ftest1 inet);
-- This next should fail, because int=inet does not exist
--dg-cascade
--dg ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable;
-- This should also fail for the same reason, but here we
-- give the column name
--dg-cascade
--dg ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable(ptest1);
--dg-cascade
--dg DROP TABLE FKTABLE;
-- This should succeed, even though they are different types,
-- because int=int8 exists and is a member of the integer opfamily
CREATE TEMP TABLE FKTABLE (ftest1 int8);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable;
-- Check it actually works
INSERT INTO FKTABLE VALUES(42);		-- should succeed
INSERT INTO FKTABLE VALUES(43);		-- should fail
DROP TABLE FKTABLE;
-- This should fail, because we'd have to cast numeric to int which is
-- not an implicit coercion (or use numeric=numeric, but that's not part
-- of the integer opfamily)
CREATE TEMP TABLE FKTABLE (ftest1 numeric);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable;
DROP TABLE FKTABLE;
DROP TABLE PKTABLE;
-- On the other hand, this should work because int implicitly promotes to
-- numeric, and we allow promotion on the FK side
CREATE TEMP TABLE PKTABLE (ptest1 numeric PRIMARY KEY);
INSERT INTO PKTABLE VALUES(42);
CREATE TEMP TABLE FKTABLE (ftest1 int);
ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1) references pktable;
-- Check it actually works
INSERT INTO FKTABLE VALUES(42);		-- should succeed
INSERT INTO FKTABLE VALUES(43);		-- should fail
DROP TABLE FKTABLE;
DROP TABLE PKTABLE;

--dg-skip: type ? is not supported yet
--dg CREATE TEMP TABLE PKTABLE (ptest1 int, ptest2 inet,
--dg                            PRIMARY KEY(ptest1, ptest2));
-- This should fail, because we just chose really odd types
--dg-skip: type ? is not supported yet
--dg CREATE TEMP TABLE FKTABLE (ftest1 cidr, ftest2 timestamp);
--dg-cascade
--dg ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1, ftest2) references pktable;
--dg-cascade
--dg DROP TABLE FKTABLE;
-- Again, so should this...
--dg-skip: type ? is not supported yet
--dg CREATE TEMP TABLE FKTABLE (ftest1 cidr, ftest2 timestamp);
--dg-cascade
--dg ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1, ftest2)
--dg      references pktable(ptest1, ptest2);
--dg-cascade
--dg DROP TABLE FKTABLE;
-- This fails because we mixed up the column ordering
--dg-skip: type ? is not supported yet
--dg CREATE TEMP TABLE FKTABLE (ftest1 int, ftest2 inet);
--dg-cascade
--dg ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest1, ftest2)
--dg      references pktable(ptest2, ptest1);
-- As does this...
--dg-cascade
--dg ALTER TABLE FKTABLE ADD FOREIGN KEY(ftest2, ftest1)
--dg      references pktable(ptest1, ptest2);
--dg-cascade
--dg DROP TABLE FKTABLE;
--dg-cascade
--dg DROP TABLE PKTABLE;

-- Test that ALTER CONSTRAINT updates trigger deferrability properly

CREATE TEMP TABLE PKTABLE (ptest1 int primary key);
CREATE TEMP TABLE FKTABLE (ftest1 int);

ALTER TABLE FKTABLE ADD CONSTRAINT fknd FOREIGN KEY(ftest1) REFERENCES pktable
  ON DELETE CASCADE ON UPDATE NO ACTION NOT DEFERRABLE;
--dg-skip: a DEFERRABLE foreign key is not supported yet
--dg ALTER TABLE FKTABLE ADD CONSTRAINT fkdd FOREIGN KEY(ftest1) REFERENCES pktable
--dg   ON DELETE CASCADE ON UPDATE NO ACTION DEFERRABLE INITIALLY DEFERRED;
--dg-skip: a DEFERRABLE foreign key is not supported yet
--dg ALTER TABLE FKTABLE ADD CONSTRAINT fkdi FOREIGN KEY(ftest1) REFERENCES pktable
--dg   ON DELETE CASCADE ON UPDATE NO ACTION DEFERRABLE INITIALLY IMMEDIATE;

--dg-skip: a DEFERRABLE foreign key is not supported yet
--dg ALTER TABLE FKTABLE ADD CONSTRAINT fknd2 FOREIGN KEY(ftest1) REFERENCES pktable
--dg   ON DELETE CASCADE ON UPDATE NO ACTION DEFERRABLE INITIALLY DEFERRED;
--dg-cascade
--dg ALTER TABLE FKTABLE ALTER CONSTRAINT fknd2 NOT DEFERRABLE;
ALTER TABLE FKTABLE ADD CONSTRAINT fkdd2 FOREIGN KEY(ftest1) REFERENCES pktable
  ON DELETE CASCADE ON UPDATE NO ACTION NOT DEFERRABLE;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE FKTABLE ALTER CONSTRAINT fkdd2 DEFERRABLE INITIALLY DEFERRED;
ALTER TABLE FKTABLE ADD CONSTRAINT fkdi2 FOREIGN KEY(ftest1) REFERENCES pktable
  ON DELETE CASCADE ON UPDATE NO ACTION NOT DEFERRABLE;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE FKTABLE ALTER CONSTRAINT fkdi2 DEFERRABLE INITIALLY IMMEDIATE;

--dg-cascade
--dg SELECT conname, tgfoid::regproc, tgtype, tgdeferrable, tginitdeferred
--dg FROM pg_trigger JOIN pg_constraint con ON con.oid = tgconstraint
--dg WHERE tgrelid = 'pktable'::regclass
--dg ORDER BY 1,2,3;
--dg-cascade
--dg SELECT conname, tgfoid::regproc, tgtype, tgdeferrable, tginitdeferred
--dg FROM pg_trigger JOIN pg_constraint con ON con.oid = tgconstraint
--dg WHERE tgrelid = 'fktable'::regclass
--dg ORDER BY 1,2,3;

-- temp tables should go away by themselves, need not drop them.

-- test check constraint adding

create table atacc1 ( test int );
-- add a check constraint
alter table atacc1 add constraint atacc_test1 check (test>3);
-- should fail
insert into atacc1 (test) values (2);
-- should succeed
insert into atacc1 (test) values (4);
drop table atacc1;

-- let's do one where the check fails when added
create table atacc1 ( test int );
-- insert a soon to be failing row
insert into atacc1 (test) values (2);
-- add a check constraint (fails)
alter table atacc1 add constraint atacc_test1 check (test>3);
insert into atacc1 (test) values (4);
drop table atacc1;

-- let's do one where the check fails because the column doesn't exist
create table atacc1 ( test int );
-- add a check constraint (fails)
alter table atacc1 add constraint atacc_test1 check (test1>3);
drop table atacc1;

-- something a little more complicated
create table atacc1 ( test int, test2 int, test3 int);
-- add a check constraint (fails)
alter table atacc1 add constraint atacc_test1 check (test+test2<test3*4);
-- should fail
insert into atacc1 (test,test2,test3) values (4,4,2);
-- should succeed
insert into atacc1 (test,test2,test3) values (4,4,5);
drop table atacc1;

-- lets do some naming tests
create table atacc1 (test int check (test>3), test2 int);
alter table atacc1 add check (test2>test);
-- should fail for $2
insert into atacc1 (test2, test) values (3, 4);
drop table atacc1;

-- inheritance related tests
create table atacc1 (test int);
create table atacc2 (test2 int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table atacc3 (test3 int) inherits (atacc1, atacc2);
alter table atacc2 add constraint foo check (test2>0);
-- fail and then succeed on atacc2
insert into atacc2 (test2) values (-3);
insert into atacc2 (test2) values (3);
-- fail and then succeed on atacc3
--dg-cascade
--dg insert into atacc3 (test2) values (-3);
--dg-cascade
--dg insert into atacc3 (test2) values (3);
--dg-cascade
--dg drop table atacc3;
drop table atacc2;
drop table atacc1;

-- same things with one created with INHERIT
create table atacc1 (test int);
create table atacc2 (test2 int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table atacc3 (test3 int) inherits (atacc1, atacc2);
--dg-cascade
--dg alter table atacc3 no inherit atacc2;
-- fail
--dg-cascade
--dg alter table atacc3 no inherit atacc2;
-- make sure it really isn't a child
--dg-cascade
--dg insert into atacc3 (test2) values (3);
select test2 from atacc2;
-- fail due to missing constraint
alter table atacc2 add constraint foo check (test2>0);
--dg-cascade
--dg alter table atacc3 inherit atacc2;
-- fail due to missing column
--dg-cascade
--dg alter table atacc3 rename test2 to testx;
--dg-cascade
--dg alter table atacc3 inherit atacc2;
-- fail due to mismatched data type
--dg-cascade
--dg alter table atacc3 add test2 bool;
--dg-cascade
--dg alter table atacc3 inherit atacc2;
--dg-cascade
--dg alter table atacc3 drop test2;
-- succeed
--dg-cascade
--dg alter table atacc3 add test2 int;
--dg-cascade
--dg update atacc3 set test2 = 4 where test2 is null;
--dg-cascade
--dg alter table atacc3 add constraint foo check (test2>0);
--dg-cascade
--dg alter table atacc3 inherit atacc2;
-- fail due to duplicates and circular inheritance
--dg-cascade
--dg alter table atacc3 inherit atacc2;
--dg-cascade
--dg alter table atacc2 inherit atacc3;
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table atacc2 inherit atacc2;
-- test that we really are a child now (should see 4 not 3 and cascade should go through)
--dg-cascade
--dg select test2 from atacc2;
--dg-cascade
--dg drop table atacc2 cascade;
drop table atacc1;

-- adding only to a parent is allowed as of 9.2

create table atacc1 (test int);
--dg-cascade
--dg create table atacc2 (test2 int) inherits (atacc1);
-- ok:
alter table atacc1 add constraint foo check (test>0) no inherit;
-- check constraint is not there on child
--dg-cascade
--dg insert into atacc2 (test) values (-3);
-- check constraint is there on parent
insert into atacc1 (test) values (-3);
insert into atacc1 (test) values (3);
-- fail, violating row:
--dg-cascade
--dg alter table atacc2 add constraint foo check (test>0) no inherit;
drop table atacc2;
drop table atacc1;

-- test unique constraint adding

create table atacc1 ( test int ) ;
-- add a unique constraint
alter table atacc1 add constraint atacc_test1 unique (test);
-- insert first value
insert into atacc1 (test) values (2);
-- should fail
insert into atacc1 (test) values (2);
-- should succeed
insert into atacc1 (test) values (4);
-- try to create duplicates via alter table using - should fail
alter table atacc1 alter column test type integer using 0;
drop table atacc1;

-- let's do one where the unique constraint fails when added
create table atacc1 ( test int );
-- insert soon to be failing rows
insert into atacc1 (test) values (2);
insert into atacc1 (test) values (2);
-- add a unique constraint (fails)
alter table atacc1 add constraint atacc_test1 unique (test);
insert into atacc1 (test) values (3);
drop table atacc1;

-- let's do one where the unique constraint fails
-- because the column doesn't exist
create table atacc1 ( test int );
-- add a unique constraint (fails)
alter table atacc1 add constraint atacc_test1 unique (test1);
drop table atacc1;

-- something a little more complicated
create table atacc1 ( test int, test2 int);
-- add a unique constraint
alter table atacc1 add constraint atacc_test1 unique (test, test2);
-- insert initial value
insert into atacc1 (test,test2) values (4,4);
-- should fail
insert into atacc1 (test,test2) values (4,4);
-- should all succeed
insert into atacc1 (test,test2) values (4,5);
insert into atacc1 (test,test2) values (5,4);
insert into atacc1 (test,test2) values (5,5);
drop table atacc1;

-- lets do some naming tests
create table atacc1 (test int, test2 int, unique(test));
alter table atacc1 add unique (test2);
-- should fail for @@ second one @@
insert into atacc1 (test2, test) values (3, 3);
insert into atacc1 (test2, test) values (2, 3);
drop table atacc1;

-- test primary key constraint adding

create table atacc1 ( id serial, test int) ;
-- add a primary key constraint
alter table atacc1 add constraint atacc_test1 primary key (test);
-- insert first value
insert into atacc1 (test) values (2);
-- should fail
insert into atacc1 (test) values (2);
-- should succeed
insert into atacc1 (test) values (4);
-- inserting NULL should fail
insert into atacc1 (test) values(NULL);
-- try adding a second primary key (should fail)
alter table atacc1 add constraint atacc_oid1 primary key(id);
-- drop first primary key constraint
alter table atacc1 drop constraint atacc_test1 restrict;
-- try adding a primary key on oid (should succeed)
alter table atacc1 add constraint atacc_oid1 primary key(id);
drop table atacc1;

-- let's do one where the primary key constraint fails when added
create table atacc1 ( test int );
-- insert soon to be failing rows
insert into atacc1 (test) values (2);
insert into atacc1 (test) values (2);
-- add a primary key (fails)
alter table atacc1 add constraint atacc_test1 primary key (test);
insert into atacc1 (test) values (3);
drop table atacc1;

-- let's do another one where the primary key constraint fails when added
create table atacc1 ( test int );
-- insert soon to be failing row
insert into atacc1 (test) values (NULL);
-- add a primary key (fails)
alter table atacc1 add constraint atacc_test1 primary key (test);
insert into atacc1 (test) values (3);
drop table atacc1;

-- let's do one where the primary key constraint fails
-- because the column doesn't exist
create table atacc1 ( test int );
-- add a primary key constraint (fails)
alter table atacc1 add constraint atacc_test1 primary key (test1);
drop table atacc1;

-- adding a new column as primary key to a non-empty table.
-- should fail unless the column has a non-null default value.
create table atacc1 ( test int );
insert into atacc1 (test) values (0);
-- add a primary key column without a default (fails).
--dg-differs: another error: ALTER TABLE ... ADD COLUMN supports only NOT NULL, DEFAULT and CHECK yet
--dg alter table atacc1 add column test2 int primary key;
-- now add a primary key column with a default (succeeds).
--dg-skip: ALTER TABLE ... ADD COLUMN supports only NOT NULL, DEFAULT and CHECK yet
--dg alter table atacc1 add column test2 int default 0 primary key;
drop table atacc1;

-- Replacing a NOT NULL column must create a new constraint for the primary key.
create table atacc1 (id int not null);
alter table atacc1 drop column id, add column id int, add primary key (id);
drop table atacc1;
-- A NOT NULL constraint dropped in the same ALTER TABLE must be replaced.
create table atacc1 (id int not null);
alter table atacc1 alter column id drop not null, add primary key (id);
drop table atacc1;
create table atacc1 (id int constraint atacc1_id_nn not null);
--dg-skip: constraint "?" of relation "?" does not exist
--dg alter table atacc1 drop constraint atacc1_id_nn, add primary key (id);
drop table atacc1;
-- ADD PRIMARY KEY must prefer an explicitly named replacement NOT NULL.
create table atacc1 (id int not null);
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY, PRIMARY KEY, UNIQUE and CHECK yet
--dg alter table atacc1 alter column id drop not null,
--dg   add primary key (id), add constraint atacc1_id_nn not null id;
drop table atacc1;
create table atacc1 (id int not null);
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY, PRIMARY KEY, UNIQUE and CHECK yet
--dg alter table atacc1 alter column id drop not null,
--dg   add constraint atacc1_id_nn not null id, add primary key (id);
drop table atacc1;
-- The replacement NOT NULL constraint must propagate to the partitions too.
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table atacc1 (id int not null) partition by range (id);
--dg-cascade
--dg create table atacc1_1 partition of atacc1 for values from (0) to (10);
--dg-cascade
--dg alter table atacc1 alter column id drop not null, add primary key (id);
--dg-cascade
--dg drop table atacc1;

-- this combination used to have order-of-execution problems (bug #15580)
create table atacc1 (a int);
insert into atacc1 values(1);
alter table atacc1
  add column b float8 not null default random(),
  add primary key(a);
drop table atacc1;

-- additionally, we've seen issues with foreign key validation not being
-- properly delayed until after a table rewrite.  Check that works ok.
create table atacc1 (a int primary key);
alter table atacc1 add constraint atacc1_fkey foreign key (a) references atacc1 (a) not valid;
--dg-skip: ALTER COLUMN TYPE of a column a foreign key uses is not supported yet
--dg alter table atacc1 validate constraint atacc1_fkey, alter a type bigint;
drop table atacc1;

-- we've also seen issues with check constraints being validated at the wrong
-- time when there's a pending table rewrite.
create table atacc1 (a bigint, b int);
insert into atacc1 values(1,1);
alter table atacc1 add constraint atacc1_chk check(b = 1) not valid;
alter table atacc1 validate constraint atacc1_chk, alter a type int;
drop table atacc1;

-- same as above, but ensure the constraint violation is detected
create table atacc1 (a bigint, b int);
insert into atacc1 values(1,2);
alter table atacc1 add constraint atacc1_chk check(b = 1) not valid;
alter table atacc1 validate constraint atacc1_chk, alter a type int;
drop table atacc1;

-- something a little more complicated
create table atacc1 ( test int, test2 int);
-- add a primary key constraint
alter table atacc1 add constraint atacc_test1 primary key (test, test2);
-- try adding a second primary key - should fail
alter table atacc1 add constraint atacc_test2 primary key (test);
-- insert initial value
insert into atacc1 (test,test2) values (4,4);
-- should fail
insert into atacc1 (test,test2) values (4,4);
insert into atacc1 (test,test2) values (NULL,3);
insert into atacc1 (test,test2) values (3, NULL);
insert into atacc1 (test,test2) values (NULL,NULL);
-- should all succeed
insert into atacc1 (test,test2) values (4,5);
insert into atacc1 (test,test2) values (5,4);
insert into atacc1 (test,test2) values (5,5);
drop table atacc1;

-- lets do some naming tests
create table atacc1 (test int, test2 int, primary key(test));
-- only first should succeed
insert into atacc1 (test2, test) values (3, 3);
insert into atacc1 (test2, test) values (2, 3);
insert into atacc1 (test2, test) values (1, NULL);
drop table atacc1;

-- alter table / alter column [set/drop] not null tests
-- try altering system catalogs, should fail
alter table pg_class alter column relname drop not null;
alter table pg_class alter relname set not null;

-- try altering non-existent table, should fail
alter table non_existent alter column bar set not null;
alter table non_existent alter column bar drop not null;

-- test setting columns to null and not null and vice versa
-- test checking for null values and primary key
create table atacc1 (test int not null);
alter table atacc1 add constraint "atacc1_pkey" primary key (test);
--dg-differs: the result
--dg \d atacc1
alter table atacc1 alter column test drop not null;
--dg-differs: the result
--dg \d atacc1
alter table atacc1 drop constraint "atacc1_pkey";
alter table atacc1 alter column test drop not null;
\d atacc1
insert into atacc1 values (null);
alter table atacc1 alter test set not null;
delete from atacc1;
alter table atacc1 alter test set not null;

-- try altering a non-existent column, should fail
alter table atacc1 alter bar set not null;
alter table atacc1 alter bar drop not null;

-- try creating a view and altering that, should fail
create view myview as select * from atacc1;
--dg-differs: another error: ALTER VIEW supports only SET and RESET of options, and RENAME yet
--dg alter table myview alter column test drop not null;
--dg-differs: another error: ALTER VIEW supports only SET and RESET of options, and RENAME yet
--dg alter table myview alter column test set not null;
drop view myview;

drop table atacc1;

-- set not null verified by constraints
create table atacc1 (test_a int, test_b int);
insert into atacc1 values (null, 1);
-- constraint not cover all values, should fail
alter table atacc1 add constraint atacc1_constr_or check(test_a is not null or test_b < 10);
alter table atacc1 alter test_a set not null;
alter table atacc1 drop constraint atacc1_constr_or;
-- not valid constraint, should fail
alter table atacc1 add constraint atacc1_constr_invalid check(test_a is not null) not valid;
alter table atacc1 alter test_a set not null;
alter table atacc1 drop constraint atacc1_constr_invalid;
-- with valid constraint
update atacc1 set test_a = 1;
alter table atacc1 add constraint atacc1_constr_a_valid check(test_a is not null);
alter table atacc1 alter test_a set not null;
delete from atacc1;

insert into atacc1 values (2, null);
alter table atacc1 alter test_a drop not null;
-- test multiple set not null at same time
-- test_a checked by atacc1_constr_a_valid, test_b should fail by table scan
alter table atacc1 alter test_a set not null, alter test_b set not null;
-- commands order has no importance
alter table atacc1 alter test_b set not null, alter test_a set not null;

-- valid one by table scan, one by check constraints
update atacc1 set test_b = 1;
alter table atacc1 alter test_b set not null, alter test_a set not null;

alter table atacc1 alter test_a drop not null, alter test_b drop not null;
-- both column has check constraints
alter table atacc1 add constraint atacc1_constr_b_valid check(test_b is not null);
alter table atacc1 alter test_b set not null, alter test_a set not null;
drop table atacc1;

-- not null not valid with partitions
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE atnnparted (id int, col1 int) PARTITION BY LIST (id);
--dg-cascade
--dg ALTER TABLE atnnparted ADD CONSTRAINT dummy_constr NOT NULL id NOT VALID;
CREATE TABLE atnnpart1 (col1 int, id int);
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY, PRIMARY KEY, UNIQUE and CHECK yet
--dg ALTER TABLE atnnpart1 ADD CONSTRAINT another_constr NOT NULL id;
ALTER TABLE atnnpart1 ADD PRIMARY KEY (id);
--dg-cascade
--dg ALTER TABLE atnnparted ATTACH PARTITION atnnpart1 FOR VALUES IN ('1');
--dg-cascade
--dg \d+ atnnpart*
BEGIN;
--dg-cascade
--dg ALTER TABLE atnnparted VALIDATE CONSTRAINT dummy_constr;
--dg-cascade
--dg \d+ atnnpart*
ROLLBACK;
-- leave a table in this state for the pg_upgrade test

-- test inheritance
create table parent (a int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table child (b varchar(255)) inherits (parent);

alter table parent alter a set not null;
insert into parent values (NULL);
--dg-cascade
--dg insert into child (a, b) values (NULL, 'foo');
alter table parent alter a drop not null;
insert into parent values (NULL);
--dg-cascade
--dg insert into child (a, b) values (NULL, 'foo');
alter table only parent alter a set not null;
--dg-cascade
--dg alter table child alter a set not null;
--dg-cascade
--dg drop table child;
drop table parent;

-- test setting and removing default values
create table def_test (
	c1	int4 default 5,
	c2	text default 'initial_default'
);
insert into def_test default values;
alter table def_test alter column c1 drop default;
insert into def_test default values;
alter table def_test alter column c2 drop default;
insert into def_test default values;
alter table def_test alter column c1 set default 10;
alter table def_test alter column c2 set default 'new_default';
insert into def_test default values;
select * from def_test;

-- set defaults to an incorrect type: this should fail
alter table def_test alter column c1 set default 'wrong_datatype';
alter table def_test alter column c2 set default 20;

-- set defaults on a non-existent column: this should fail
alter table def_test alter column c3 set default 30;

-- set defaults on views: we need to create a view, add a rule
-- to allow insertions into it, and then alter the view to add
-- a default
create view def_view_test as select * from def_test;
--dg-skip: relation pg_rewrite cannot be written: digres has no catalog tables
--dg create rule def_view_test_ins as
--dg 	on insert to def_view_test
--dg 	do instead insert into def_test select new.*;
insert into def_view_test default values;
--dg-skip: ALTER VIEW supports only SET and RESET of options, and RENAME yet
--dg alter table def_view_test alter column c1 set default 45;
insert into def_view_test default values;
--dg-skip: ALTER VIEW supports only SET and RESET of options, and RENAME yet
--dg alter table def_view_test alter column c2 set default 'view_default';
insert into def_view_test default values;
--dg-cascade
--dg select * from def_view_test;

--dg-cascade
--dg drop rule def_view_test_ins on def_view_test;
drop view def_view_test;
drop table def_test;

-- alter table / drop column tests
-- try altering system catalogs, should fail
alter table pg_class drop column relname;

-- try altering non-existent table, should fail
alter table nosuchtable drop column bar;

-- test dropping columns
create table atacc1 (a int4 not null, b int4, c int4 not null, d int4);
insert into atacc1 values (1, 2, 3, 4);
alter table atacc1 drop a;
alter table atacc1 drop a;

-- SELECTs
select * from atacc1;
select * from atacc1 order by a;
select * from atacc1 order by "........pg.dropped.1........";
select * from atacc1 group by a;
select * from atacc1 group by "........pg.dropped.1........";
select atacc1.* from atacc1;
select a from atacc1;
select atacc1.a from atacc1;
select b,c,d from atacc1;
select a,b,c,d from atacc1;
select * from atacc1 where a = 1;
select "........pg.dropped.1........" from atacc1;
select atacc1."........pg.dropped.1........" from atacc1;
select "........pg.dropped.1........",b,c,d from atacc1;
select * from atacc1 where "........pg.dropped.1........" = 1;

-- UPDATEs
update atacc1 set a = 3;
update atacc1 set b = 2 where a = 3;
update atacc1 set "........pg.dropped.1........" = 3;
update atacc1 set b = 2 where "........pg.dropped.1........" = 3;

-- INSERTs
insert into atacc1 values (10, 11, 12, 13);
insert into atacc1 values (default, 11, 12, 13);
insert into atacc1 values (11, 12, 13);
insert into atacc1 (a) values (10);
insert into atacc1 (a) values (default);
insert into atacc1 (a,b,c,d) values (10,11,12,13);
insert into atacc1 (a,b,c,d) values (default,11,12,13);
insert into atacc1 (b,c,d) values (11,12,13);
insert into atacc1 ("........pg.dropped.1........") values (10);
insert into atacc1 ("........pg.dropped.1........") values (default);
insert into atacc1 ("........pg.dropped.1........",b,c,d) values (10,11,12,13);
insert into atacc1 ("........pg.dropped.1........",b,c,d) values (default,11,12,13);

-- DELETEs
delete from atacc1 where a = 3;
delete from atacc1 where "........pg.dropped.1........" = 3;
delete from atacc1;

-- try dropping a non-existent column, should fail
alter table atacc1 drop bar;

-- try removing an oid column, should succeed (as it's nonexistent)
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table atacc1 SET WITHOUT OIDS;

-- try adding an oid column, should fail (not supported)
alter table atacc1 SET WITH OIDS;

-- try dropping the xmin column, should fail
--dg-differs: another error: column "?" of relation "?" does not exist
--dg alter table atacc1 drop xmin;

-- try creating a view and altering that, should fail
create view myview as select * from atacc1;
select * from myview;
--dg-differs: another error: ALTER VIEW supports only SET and RESET of options, and RENAME yet
--dg alter table myview drop d;
drop view myview;

-- test some commands to make sure they fail on the dropped column
--dg-differs: another error: ANALYZE of chosen columns is not supported yet
--dg analyze atacc1(a);
--dg-differs: another error: ANALYZE of chosen columns is not supported yet
--dg analyze atacc1("........pg.dropped.1........");
--dg-differs: another error: ANALYZE of chosen columns is not supported yet
--dg vacuum analyze atacc1(a);
--dg-differs: another error: ANALYZE of chosen columns is not supported yet
--dg vacuum analyze atacc1("........pg.dropped.1........");
--dg-differs: another error: COMMENT is not supported yet
--dg comment on column atacc1.a is 'testing';
--dg-differs: another error: COMMENT is not supported yet
--dg comment on column atacc1."........pg.dropped.1........" is 'testing';
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table atacc1 alter a set storage plain;
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table atacc1 alter "........pg.dropped.1........" set storage plain;
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table atacc1 alter a set statistics 0;
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table atacc1 alter "........pg.dropped.1........" set statistics 0;
alter table atacc1 alter a set default 3;
alter table atacc1 alter "........pg.dropped.1........" set default 3;
alter table atacc1 alter a drop default;
alter table atacc1 alter "........pg.dropped.1........" drop default;
alter table atacc1 alter a set not null;
alter table atacc1 alter "........pg.dropped.1........" set not null;
alter table atacc1 alter a drop not null;
alter table atacc1 alter "........pg.dropped.1........" drop not null;
alter table atacc1 rename a to x;
alter table atacc1 rename "........pg.dropped.1........" to x;
alter table atacc1 add primary key(a);
alter table atacc1 add primary key("........pg.dropped.1........");
alter table atacc1 add unique(a);
alter table atacc1 add unique("........pg.dropped.1........");
alter table atacc1 add check (a > 3);
alter table atacc1 add check ("........pg.dropped.1........" > 3);
create table atacc2 (id int4 unique);
alter table atacc1 add foreign key (a) references atacc2(id);
alter table atacc1 add foreign key ("........pg.dropped.1........") references atacc2(id);
alter table atacc2 add foreign key (id) references atacc1(a);
alter table atacc2 add foreign key (id) references atacc1("........pg.dropped.1........");
drop table atacc2;
create index "testing_idx" on atacc1(a);
create index "testing_idx" on atacc1("........pg.dropped.1........");

-- test create as and select into
insert into atacc1 values (21, 22, 23);
create table attest1 as select * from atacc1;
select * from attest1;
drop table attest1;
select * into attest2 from atacc1;
select * from attest2;
drop table attest2;

-- try dropping all columns
alter table atacc1 drop c;
alter table atacc1 drop d;
alter table atacc1 drop b;
select * from atacc1;

drop table atacc1;

-- test constraint error reporting in presence of dropped columns
create table atacc1 (id serial primary key, value int check (value < 10));
insert into atacc1(value) values (100);
alter table atacc1 drop column value;
alter table atacc1 add column value int check (value < 10);
insert into atacc1(value) values (100);
insert into atacc1(id, value) values (null, 0);
drop table atacc1;

-- test inheritance
create table parent (a int, b int, c int);
insert into parent values (1, 2, 3);
alter table parent drop a;
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table child (d varchar(255)) inherits (parent);
--dg-cascade
--dg insert into child values (12, 13, 'testing');

--dg-cascade
--dg select * from parent;
--dg-cascade
--dg select * from child;
alter table parent drop c;
--dg-cascade
--dg select * from parent;
--dg-cascade
--dg select * from child;

--dg-cascade
--dg drop table child;
drop table parent;

-- check error cases for inheritance column merging
--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg create table parent (a float8, b numeric(10,4), c text collate "C");

--dg-cascade
--dg create table child (a float4) inherits (parent); -- fail
--dg-cascade
--dg create table child (b decimal(10,7)) inherits (parent); -- fail
--dg-cascade
--dg create table child (c text collate "POSIX") inherits (parent); -- fail
--dg-cascade
--dg create table child (a double precision, b decimal(10,4)) inherits (parent);

--dg-cascade
--dg drop table child;
--dg-cascade
--dg drop table parent;

-- test copy in/out
create table attest (a int4, b int4, c int4);
insert into attest values (1,2,3);
alter table attest drop a;
copy attest to stdout;
copy attest(a) to stdout;
copy attest("........pg.dropped.1........") to stdout;
copy attest from stdin;
10	11	12
\.
select * from attest;
copy attest from stdin;
21	22
\.
select * from attest;
copy attest(a) from stdin;
\.
copy attest("........pg.dropped.1........") from stdin;
\.
copy attest(b,c) from stdin;
31	32
\.
select * from attest;
drop table attest;

-- test inheritance

create table dropColumn (a int, b int, e int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table dropColumnChild (c int) inherits (dropColumn);
--dg-cascade
--dg create table dropColumnAnother (d int) inherits (dropColumnChild);

-- these two should fail
--dg-cascade
--dg alter table dropColumnchild drop column a;
--dg-cascade
--dg alter table only dropColumnChild drop column b;



-- these three should work
alter table only dropColumn drop column e;
--dg-cascade
--dg alter table dropColumnChild drop column c;
alter table dropColumn drop column a;

create table renameColumn (a int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table renameColumnChild (b int) inherits (renameColumn);
--dg-cascade
--dg create table renameColumnAnother (c int) inherits (renameColumnChild);

-- these three should fail
--dg-cascade
--dg alter table renameColumnChild rename column a to d;
--dg-cascade
--dg alter table only renameColumnChild rename column a to d;
--dg-cascade
--dg alter table only renameColumn rename column a to d;

-- these should work
--dg-cascade
--dg alter table renameColumn rename column a to d;
--dg-cascade
--dg alter table renameColumnChild rename column b to a;

-- these should work
alter table if exists doesnt_exist_tab rename column a to d;
alter table if exists doesnt_exist_tab rename column b to a;

-- this should work
alter table renameColumn add column w int;

-- this should fail
--dg-cascade
--dg alter table only renameColumn add column x int;

-- this should work
--dg-cascade
--dg alter table renameColumn add column x int check (x > 0) not enforced;

-- this should fail
--dg-differs: another error: ALTER TABLE ... ADD COLUMN supports only NOT NULL, DEFAULT and CHECK yet
--dg alter table renameColumn add column y int check (x > 0) not enforced enforced;

-- Test corner cases in dropping of inherited columns

create table p1 (f1 int, f2 int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table c1 (f1 int not null) inherits(p1);

-- should be rejected since c1.f1 is inherited
--dg-cascade
--dg alter table c1 drop column f1;
-- should work
alter table p1 drop column f1;
-- c1.f1 is still there, but no longer inherited
--dg-cascade
--dg select f1 from c1;
--dg-cascade
--dg alter table c1 drop column f1;
--dg-cascade
--dg select f1 from c1;

--dg-cascade
--dg drop table p1 cascade;

--dg-cascade
--dg create table p1 (f1 int, f2 int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table c1 () inherits(p1);

-- should be rejected since c1.f1 is inherited
--dg-cascade
--dg alter table c1 drop column f1;
--dg-cascade
--dg alter table p1 drop column f1;
-- c1.f1 is dropped now, since there is no local definition for it
--dg-cascade
--dg select f1 from c1;

--dg-cascade
--dg drop table p1 cascade;

--dg-cascade
--dg create table p1 (f1 int, f2 int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table c1 () inherits(p1);

-- should be rejected since c1.f1 is inherited
--dg-cascade
--dg alter table c1 drop column f1;
--dg-cascade
--dg alter table only p1 drop column f1;
-- c1.f1 is NOT dropped, but must now be considered non-inherited
--dg-cascade
--dg alter table c1 drop column f1;

--dg-cascade
--dg drop table p1 cascade;

--dg-cascade
--dg create table p1 (f1 int, f2 int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table c1 (f1 int not null) inherits(p1);

-- should be rejected since c1.f1 is inherited
--dg-cascade
--dg alter table c1 drop column f1;
--dg-cascade
--dg alter table only p1 drop column f1;
-- c1.f1 is still there, but no longer inherited
--dg-cascade
--dg alter table c1 drop column f1;

--dg-cascade
--dg drop table p1 cascade;

--dg-cascade
--dg create table p1(id int, name text);
create table p2(id2 int, name text, height int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table c1(age int) inherits(p1,p2);
--dg-cascade
--dg create table gc1() inherits (c1);

--dg-cascade
--dg select relname, attname, attinhcount, attislocal
--dg from pg_class join pg_attribute on (pg_class.oid = pg_attribute.attrelid)
--dg where relname in ('p1','p2','c1','gc1') and attnum > 0 and not attisdropped
--dg order by relname, attnum;

-- should work
--dg-cascade
--dg alter table only p1 drop column name;
-- should work. Now c1.name is local and inhcount is 0.
alter table p2 drop column name;
-- should be rejected since its inherited
--dg-cascade
--dg alter table gc1 drop column name;
-- should work, and drop gc1.name along
--dg-cascade
--dg alter table c1 drop column name;
-- should fail: column does not exist
--dg-cascade
--dg alter table gc1 drop column name;
-- should work and drop the attribute in all tables
alter table p2 drop column height;

-- IF EXISTS test
create table dropColumnExists ();
alter table dropColumnExists drop column non_existing; --fail
alter table dropColumnExists drop column if exists non_existing; --succeed

--dg-cascade
--dg select relname, attname, attinhcount, attislocal
--dg from pg_class join pg_attribute on (pg_class.oid = pg_attribute.attrelid)
--dg where relname in ('p1','p2','c1','gc1') and attnum > 0 and not attisdropped
--dg order by relname, attnum;

--dg-cascade
--dg drop table p1, p2 cascade;

-- test attinhcount tracking with merged columns

create table depth0();
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table depth1(c text) inherits (depth0);
--dg-cascade
--dg create table depth2() inherits (depth1);
--dg-cascade
--dg alter table depth0 add c text;

--dg-cascade
--dg select attrelid::regclass, attname, attinhcount, attislocal
--dg from pg_attribute
--dg where attnum > 0 and attrelid::regclass in ('depth0', 'depth1', 'depth2')
--dg order by attrelid::regclass::text, attnum;

-- test renumbering of child-table columns in inherited operations

--dg-cascade
--dg create table p1 (f1 int);
--dg-cascade
--dg create table c1 (f2 text, f3 int) inherits (p1);

alter table p1 add column a1 int check (a1 > 0);
--dg-cascade
--dg alter table p1 add column f2 text;

--dg-cascade
--dg insert into p1 values (1,2,'abc');
--dg-cascade
--dg insert into c1 values(11,'xyz',33,0); -- should fail
--dg-cascade
--dg insert into c1 values(11,'xyz',33,22);

--dg-cascade
--dg select * from p1;
--dg-cascade
--dg update p1 set a1 = a1 + 1, f2 = upper(f2);
--dg-cascade
--dg select * from p1;

--dg-cascade
--dg drop table p1 cascade;

-- test that operations with a dropped column do not try to reference
-- its datatype

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create domain mytype as text;
--dg-cascade
--dg create temp table foo (f1 text, f2 mytype, f3 text);

--dg-cascade
--dg insert into foo values('bb','cc','dd');
--dg-cascade
--dg select * from foo;

--dg-cascade
--dg drop domain mytype cascade;

--dg-cascade
--dg select * from foo;
--dg-cascade
--dg insert into foo values('qq','rr');
--dg-cascade
--dg select * from foo;
--dg-cascade
--dg update foo set f3 = 'zz';
--dg-cascade
--dg select * from foo;
--dg-cascade
--dg select f3,max(f1) from foo group by f3;

-- Simple tests for alter table column type
--dg-cascade
--dg alter table foo alter f1 TYPE integer; -- fails
--dg-cascade
--dg alter table foo alter f1 TYPE varchar(10);

create table anothertab (atcol1 serial8, atcol2 boolean,
	constraint anothertab_chk check (atcol1 <= 3));

insert into anothertab (atcol1, atcol2) values (default, true);
insert into anothertab (atcol1, atcol2) values (default, false);
select * from anothertab;

alter table anothertab alter column atcol1 type boolean; -- fails
alter table anothertab alter column atcol1 type boolean using atcol1::int; -- fails
--dg-skip: ALTER COLUMN TYPE of a column a check constraint reads is not supported yet
--dg alter table anothertab alter column atcol1 type integer;

select * from anothertab;

insert into anothertab (atcol1, atcol2) values (45, null); -- fails
insert into anothertab (atcol1, atcol2) values (default, null);

select * from anothertab;

alter table anothertab alter column atcol2 type text
      using case when atcol2 is true then 'IT WAS TRUE'
                 when atcol2 is false then 'IT WAS FALSE'
                 else 'IT WAS NULL!' end;

select * from anothertab;
alter table anothertab alter column atcol1 type boolean
        using case when atcol1 % 2 = 0 then true else false end; -- fails
alter table anothertab alter column atcol1 drop default;
--dg-differs: another error: ALTER COLUMN TYPE of a column a check constraint reads is not supported yet
--dg alter table anothertab alter column atcol1 type boolean
--dg         using case when atcol1 % 2 = 0 then true else false end; -- fails
alter table anothertab drop constraint anothertab_chk;
alter table anothertab drop constraint anothertab_chk; -- fails
alter table anothertab drop constraint IF EXISTS anothertab_chk; -- succeeds

alter table anothertab alter column atcol1 type boolean
        using case when atcol1 % 2 = 0 then true else false end;

select * from anothertab;

drop table anothertab;

-- Test index handling in alter table column type (cf. bugs #15835, #15865)
create table anothertab(f1 int primary key, f2 int unique,
                        f3 int, f4 int, f5 int);
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY, PRIMARY KEY, UNIQUE and CHECK yet
--dg alter table anothertab
--dg   add exclude using btree (f3 with =);
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY, PRIMARY KEY, UNIQUE and CHECK yet
--dg alter table anothertab
--dg   add exclude using btree (f4 with =) where (f4 is not null);
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY, PRIMARY KEY, UNIQUE and CHECK yet
--dg alter table anothertab
--dg   add exclude using btree (f4 with =) where (f5 > 0);
alter table anothertab
  add unique(f1,f4);
create index on anothertab(f2,f3);
create unique index on anothertab(f4);

--dg-cascade
--dg \d anothertab
alter table anothertab alter column f1 type bigint;
alter table anothertab
  alter column f2 type bigint,
  alter column f3 type bigint,
  alter column f4 type bigint;
alter table anothertab alter column f5 type bigint;
--dg-cascade
--dg \d anothertab

drop table anothertab;

-- test that USING expressions are parsed before column alter type / drop steps
create table another (f1 int, f2 text, f3 text);

insert into another values(1, 'one', 'uno');
insert into another values(2, 'two', 'due');
insert into another values(3, 'three', 'tre');

select * from another;

--dg-skip: a USING that reads a column the same ALTER TABLE changes the type of is not supported yet
--dg alter table another
--dg   alter f1 type text using f2 || ' and ' || f3 || ' more',
--dg   alter f2 type bigint using f1 * 10,
--dg   drop column f3;

--dg-cascade
--dg select * from another;

drop table another;

-- Create an index that skips WAL, then perform a SET DATA TYPE that skips
-- rewriting the index.
begin;
create table skip_wal_skip_rewrite_index (c varchar(10) primary key);
alter table skip_wal_skip_rewrite_index alter c type varchar(20);
commit;

-- We disallow changing table's row type if it's used for storage
create table at_tab1 (a int, b text);
--dg-skip: type "?" does not exist
--dg create table at_tab2 (x int, y at_tab1);
--dg-cascade
--dg alter table at_tab1 alter column b type varchar; -- fails
--dg-cascade
--dg drop table at_tab2;
-- Use of row type in an expression is defended differently
--dg-skip: type "?" does not exist
--dg create table at_tab2 (x int, y text, check((x,y)::at_tab1 = (1,'42')::at_tab1));
alter table at_tab1 alter column b type varchar; -- allowed, but ...
--dg-cascade
--dg insert into at_tab2 values(1,'42'); -- ... this will fail
--dg-cascade
--dg drop table at_tab1, at_tab2;
-- Check it for a partitioned table, too
--dg-cascade
--dg create table at_tab1 (a int, b text) partition by list(a);
--dg-skip: type "?" does not exist
--dg create table at_tab2 (x int, y at_tab1);
--dg-cascade
--dg alter table at_tab1 alter column b type varchar; -- fails
--dg-cascade
--dg drop table at_tab1, at_tab2;

-- Alter column type that's part of a partitioned index
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table at_partitioned (a int, b text) partition by range (a);
--dg-cascade
--dg create table at_part_1 partition of at_partitioned for values from (0) to (1000);
--dg-cascade
--dg insert into at_partitioned values (512, '0.123');
create table at_part_2 (b text, a int);
insert into at_part_2 values ('1.234', 1024);
--dg-cascade
--dg create index on at_partitioned (b);
--dg-cascade
--dg create index on at_partitioned (a);
--dg-cascade
--dg \d at_part_1
\d at_part_2
--dg-cascade
--dg alter table at_partitioned attach partition at_part_2 for values from (1000) to (2000);
--dg-cascade
--dg \d at_part_2
--dg-cascade
--dg alter table at_partitioned alter column b type numeric using b::numeric;
--dg-cascade
--dg \d at_part_1
--dg-cascade
--dg \d at_part_2
--dg-cascade
--dg drop table at_partitioned;

-- Alter column type when no table rewrite is required
-- Also check that comments are preserved
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table at_partitioned(id int, name varchar(64), unique (id, name))
--dg   partition by hash(id);
--dg-cascade
--dg comment on constraint at_partitioned_id_name_key on at_partitioned is 'parent constraint';
--dg-cascade
--dg comment on index at_partitioned_id_name_key is 'parent index';
--dg-cascade
--dg create table at_partitioned_0 partition of at_partitioned
--dg   for values with (modulus 2, remainder 0);
--dg-cascade
--dg comment on constraint at_partitioned_0_id_name_key on at_partitioned_0 is 'child 0 constraint';
--dg-cascade
--dg comment on index at_partitioned_0_id_name_key is 'child 0 index';
--dg-cascade
--dg create table at_partitioned_1 partition of at_partitioned
--dg   for values with (modulus 2, remainder 1);
--dg-cascade
--dg comment on constraint at_partitioned_1_id_name_key on at_partitioned_1 is 'child 1 constraint';
--dg-cascade
--dg comment on index at_partitioned_1_id_name_key is 'child 1 index';
--dg-cascade
--dg insert into at_partitioned values(1, 'foo');
--dg-cascade
--dg insert into at_partitioned values(3, 'bar');

--dg-skip: type ? is not supported yet
--dg create temp table old_oids as
--dg   select relname, oid as oldoid, relfilenode as oldfilenode
--dg   from pg_class where relname like 'at_partitioned%';

--dg-cascade
--dg select relname,
--dg   c.oid = oldoid as orig_oid,
--dg   case relfilenode
--dg     when 0 then 'none'
--dg     when c.oid then 'own'
--dg     when oldfilenode then 'orig'
--dg     else 'OTHER'
--dg     end as storage,
--dg   obj_description(c.oid, 'pg_class') as desc
--dg   from pg_class c left join old_oids using (relname)
--dg   where relname like 'at_partitioned%'
--dg   order by relname;

--dg-cascade
--dg select conname, obj_description(oid, 'pg_constraint') as desc
--dg   from pg_constraint where conname like 'at_partitioned%'
--dg   order by conname;

--dg-cascade
--dg alter table at_partitioned alter column name type varchar(127);

--dg-cascade
--dg select relname,
--dg   c.oid = oldoid as orig_oid,
--dg   case relfilenode
--dg     when 0 then 'none'
--dg     when c.oid then 'own'
--dg     when oldfilenode then 'orig'
--dg     else 'OTHER'
--dg     end as storage,
--dg   obj_description(c.oid, 'pg_class') as desc
--dg   from pg_class c left join old_oids using (relname)
--dg   where relname like 'at_partitioned%'
--dg   order by relname;

--dg-cascade
--dg select conname, obj_description(oid, 'pg_constraint') as desc
--dg   from pg_constraint where conname like 'at_partitioned%'
--dg   order by conname;

-- Don't remove this DROP, it exposes bug #15672
--dg-cascade
--dg drop table at_partitioned;

-- disallow recursive containment of row types
create temp table recur1 (f1 int);
--dg-differs: another error: type "?" does not exist
--dg alter table recur1 add column f2 recur1; -- fails
--dg-differs: another error: type "?" does not exist
--dg alter table recur1 add column f2 recur1[]; -- fails
--dg-skip: type "?" does not exist
--dg create domain array_of_recur1 as recur1[];
--dg-cascade
--dg alter table recur1 add column f2 array_of_recur1; -- fails
--dg-skip: type "?" does not exist
--dg create temp table recur2 (f1 int, f2 recur1);
--dg-cascade
--dg alter table recur1 add column f2 recur2; -- fails
alter table recur1 add column f2 int;
--dg-cascade
--dg alter table recur1 alter column f2 type recur2; -- fails

-- SET STORAGE may need to add a TOAST table
create table test_storage (a text, c text storage plain);
--dg-differs: the result
--dg select reltoastrelid <> 0 as has_toast_table
--dg   from pg_class where oid = 'test_storage'::regclass;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table test_storage alter a set storage plain;
-- rewrite table to remove its TOAST table; need a non-constant column default
alter table test_storage add b int default random()::int;
--dg-cascade
--dg select reltoastrelid <> 0 as has_toast_table
--dg   from pg_class where oid = 'test_storage'::regclass;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table test_storage alter a set storage default; -- re-add TOAST table
--dg-differs: the result
--dg select reltoastrelid <> 0 as has_toast_table
--dg   from pg_class where oid = 'test_storage'::regclass;

-- check STORAGE correctness
--dg-differs: no error where PostgreSQL has: column data type integer can only have storage PLAIN
--dg create table test_storage_failed (a text, b int storage extended);

-- test that SET STORAGE propagates to index correctly
create index test_storage_idx on test_storage (b, a);
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table test_storage alter column a set storage external;
--dg-cascade
--dg \d+ test_storage
--dg-cascade
--dg \d+ test_storage_idx

-- ALTER COLUMN TYPE with a check constraint and a child table (bug #13779)
CREATE TABLE test_inh_check (a float check (a > 10.2), b float);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE test_inh_check_child() INHERITS(test_inh_check);
--dg-cascade
--dg \d test_inh_check
--dg-cascade
--dg \d test_inh_check_child
--dg-cascade
--dg select relname, conname, coninhcount, conislocal, connoinherit
--dg   from pg_constraint c, pg_class r
--dg   where relname like 'test_inh_check%' and c.conrelid = r.oid
--dg   order by 1, 2;
--dg-skip: ALTER COLUMN TYPE of a column a check constraint reads is not supported yet
--dg ALTER TABLE test_inh_check ALTER COLUMN a TYPE numeric;
--dg-cascade
--dg \d test_inh_check
--dg-cascade
--dg \d test_inh_check_child
--dg-cascade
--dg select relname, conname, coninhcount, conislocal, connoinherit
--dg   from pg_constraint c, pg_class r
--dg   where relname like 'test_inh_check%' and c.conrelid = r.oid
--dg   order by 1, 2;
-- also try noinherit, local, and local+inherited cases
ALTER TABLE test_inh_check ADD CONSTRAINT bnoinherit CHECK (b > 100) NO INHERIT;
--dg-cascade
--dg ALTER TABLE test_inh_check_child ADD CONSTRAINT blocal CHECK (b < 1000);
--dg-cascade
--dg ALTER TABLE test_inh_check_child ADD CONSTRAINT bmerged CHECK (b > 1);
--dg-cascade
--dg ALTER TABLE test_inh_check ADD CONSTRAINT bmerged CHECK (b > 1);
--dg-cascade
--dg \d test_inh_check
--dg-cascade
--dg \d test_inh_check_child
--dg-cascade
--dg select relname, conname, coninhcount, conislocal, connoinherit
--dg   from pg_constraint c, pg_class r
--dg   where relname like 'test_inh_check%' and c.conrelid = r.oid
--dg   order by 1, 2;
--dg-skip: ALTER COLUMN TYPE of a column a check constraint reads is not supported yet
--dg ALTER TABLE test_inh_check ALTER COLUMN b TYPE numeric;
--dg-cascade
--dg \d test_inh_check
--dg-cascade
--dg \d test_inh_check_child
--dg-cascade
--dg select relname, conname, coninhcount, conislocal, connoinherit
--dg   from pg_constraint c, pg_class r
--dg   where relname like 'test_inh_check%' and c.conrelid = r.oid
--dg   order by 1, 2;

-- ALTER COLUMN TYPE with different schema in children
-- Bug at https://postgr.es/m/20170102225618.GA10071@telsasoft.com
CREATE TABLE test_type_diff (f1 int);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE test_type_diff_c (extra smallint) INHERITS (test_type_diff);
ALTER TABLE test_type_diff ADD COLUMN f2 int;
--dg-cascade
--dg INSERT INTO test_type_diff_c VALUES (1, 2, 3);
ALTER TABLE test_type_diff ALTER COLUMN f2 TYPE bigint USING f2::bigint;

CREATE TABLE test_type_diff2 (int_two int2, int_four int4, int_eight int8);
CREATE TABLE test_type_diff2_c1 (int_four int4, int_eight int8, int_two int2);
CREATE TABLE test_type_diff2_c2 (int_eight int8, int_two int2, int_four int4);
CREATE TABLE test_type_diff2_c3 (int_two int2, int_four int4, int_eight int8);
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE test_type_diff2_c1 INHERIT test_type_diff2;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE test_type_diff2_c2 INHERIT test_type_diff2;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE test_type_diff2_c3 INHERIT test_type_diff2;
INSERT INTO test_type_diff2_c1 VALUES (1, 2, 3);
INSERT INTO test_type_diff2_c2 VALUES (4, 5, 6);
INSERT INTO test_type_diff2_c3 VALUES (7, 8, 9);
ALTER TABLE test_type_diff2 ALTER COLUMN int_four TYPE int8 USING int_four::int8;
-- whole-row references are disallowed
--dg-cascade
--dg ALTER TABLE test_type_diff2 ALTER COLUMN int_four TYPE int4 USING (pg_column_size(test_type_diff2));

-- check for rollback of ANALYZE corrupting table property flags (bug #11638)
CREATE TABLE check_fk_presence_1 (id int PRIMARY KEY, t text);
CREATE TABLE check_fk_presence_2 (id int REFERENCES check_fk_presence_1, t text);
BEGIN;
ALTER TABLE check_fk_presence_2 DROP CONSTRAINT check_fk_presence_2_id_fkey;
ANALYZE check_fk_presence_2;
ROLLBACK;
\d check_fk_presence_2
DROP TABLE check_fk_presence_1, check_fk_presence_2;

-- check column addition within a view (bug #14876)
create table at_base_table(id int, stuff text);
insert into at_base_table values (23, 'skidoo');
create view at_view_1 as select * from at_base_table bt;
--dg-skip: relation "?" does not have a composite type
--dg create view at_view_2 as select *, to_json(v1) as j from at_view_1 v1;
\d+ at_view_1
--dg-cascade
--dg \d+ at_view_2
--dg-cascade
--dg explain (verbose, costs off) select * from at_view_2;
--dg-cascade
--dg select * from at_view_2;

create or replace view at_view_1 as select *, 2+2 as more from at_base_table bt;
\d+ at_view_1
--dg-cascade
--dg \d+ at_view_2
--dg-cascade
--dg explain (verbose, costs off) select * from at_view_2;
--dg-cascade
--dg select * from at_view_2;

--dg-cascade
--dg drop view at_view_2;
drop view at_view_1;
drop table at_base_table;

-- related case (bug #17811)
begin;
create temp table t1 as select * from int8_tbl;
create temp view v1 as select 1::int8 as q1;
create temp view v2 as select * from v1;
create or replace temp view v1 with (security_barrier = true)
  as select * from t1;

create temp table log (q1 int8, q2 int8);
--dg-skip: relation pg_rewrite cannot be written: digres has no catalog tables
--dg create rule v1_upd_rule as on update to v1
--dg   do also insert into log values (new.*);

update v2 set q1 = q1 + 1 where q1 = 123;

select * from t1;
--dg-cascade
--dg select * from log;
rollback;

-- check adding a column not itself requiring a rewrite, together with
-- a column requiring a default (bug #16038)

-- ensure that rewrites aren't silently optimized away, removing the
-- value of the test
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION check_ddl_rewrite(p_tablename regclass, p_ddl text)
--dg RETURNS boolean
--dg LANGUAGE plpgsql AS $$
--dg DECLARE
--dg     v_relfilenode oid;
--dg BEGIN
--dg     v_relfilenode := relfilenode FROM pg_class WHERE oid = p_tablename;
--dg
--dg     EXECUTE p_ddl;
--dg
--dg     RETURN v_relfilenode <> (SELECT relfilenode FROM pg_class WHERE oid = p_tablename);
--dg END;
--dg $$;

CREATE TABLE rewrite_test(col text);
INSERT INTO rewrite_test VALUES ('something');
INSERT INTO rewrite_test VALUES (NULL);

-- empty[12] don't need rewrite, but notempty[12]_rewrite will force one
--dg-cascade
--dg SELECT check_ddl_rewrite('rewrite_test', $$
--dg   ALTER TABLE rewrite_test
--dg       ADD COLUMN empty1 text,
--dg       ADD COLUMN notempty1_rewrite serial;
--dg $$);
--dg-cascade
--dg SELECT check_ddl_rewrite('rewrite_test', $$
--dg     ALTER TABLE rewrite_test
--dg         ADD COLUMN notempty2_rewrite serial,
--dg         ADD COLUMN empty2 text;
--dg $$);
-- also check that fast defaults cause no problem, first without rewrite
--dg-cascade
--dg SELECT check_ddl_rewrite('rewrite_test', $$
--dg     ALTER TABLE rewrite_test
--dg         ADD COLUMN empty3 text,
--dg         ADD COLUMN notempty3_norewrite int default 42;
--dg $$);
--dg-cascade
--dg SELECT check_ddl_rewrite('rewrite_test', $$
--dg     ALTER TABLE rewrite_test
--dg         ADD COLUMN notempty4_norewrite int default 42,
--dg         ADD COLUMN empty4 text;
--dg $$);
-- then with rewrite
--dg-cascade
--dg SELECT check_ddl_rewrite('rewrite_test', $$
--dg     ALTER TABLE rewrite_test
--dg         ADD COLUMN empty5 text,
--dg         ADD COLUMN notempty5_norewrite int default 42,
--dg         ADD COLUMN notempty5_rewrite serial;
--dg $$);
--dg-cascade
--dg SELECT check_ddl_rewrite('rewrite_test', $$
--dg     ALTER TABLE rewrite_test
--dg         ADD COLUMN notempty6_rewrite serial,
--dg         ADD COLUMN empty6 text,
--dg         ADD COLUMN notempty6_norewrite int default 42;
--dg $$);

-- cleanup
--dg-cascade
--dg DROP FUNCTION check_ddl_rewrite(regclass, text);
DROP TABLE rewrite_test;

--
-- lock levels
--
drop type lockmodes;
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type lockmodes as enum (
--dg  'SIReadLock'
--dg ,'AccessShareLock'
--dg ,'RowShareLock'
--dg ,'RowExclusiveLock'
--dg ,'ShareUpdateExclusiveLock'
--dg ,'ShareLock'
--dg ,'ShareRowExclusiveLock'
--dg ,'ExclusiveLock'
--dg ,'AccessExclusiveLock'
--dg );

drop view my_locks;
--dg-cascade
--dg create or replace view my_locks as
--dg select case when c.relname like 'pg_toast%' then 'pg_toast' else c.relname end, max(mode::lockmodes) as max_lockmode
--dg from pg_locks l join pg_class c on l.relation = c.oid
--dg where virtualtransaction = (
--dg         select virtualtransaction
--dg         from pg_locks
--dg         where transactionid = pg_current_xact_id()::xid)
--dg and locktype = 'relation'
--dg and relnamespace != (select oid from pg_namespace where nspname = 'pg_catalog')
--dg and c.relname != 'my_locks'
--dg group by c.relname;

create table alterlock (f1 int primary key, f2 text);
insert into alterlock values (1, 'foo');
create table alterlock2 (f3 int primary key, f1 int);
insert into alterlock2 values (1, 1);

begin; 
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alterlock alter column f2 set statistics 150;
--dg-cascade
--dg select * from my_locks order by 1;
rollback;

begin; 
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alterlock cluster on alterlock_pkey;
--dg-cascade
--dg select * from my_locks order by 1;
commit;

begin; 
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alterlock set without cluster;
--dg-cascade
--dg select * from my_locks order by 1;
commit;

begin; 
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alterlock set (fillfactor = 100);
--dg-cascade
--dg select * from my_locks order by 1;
commit;

begin; 
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alterlock reset (fillfactor);
--dg-cascade
--dg select * from my_locks order by 1;
commit;

begin; 
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alterlock set (toast.autovacuum_enabled = off);
--dg-cascade
--dg select * from my_locks order by 1;
commit;

begin; 
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alterlock set (autovacuum_enabled = off);
--dg-cascade
--dg select * from my_locks order by 1;
commit;

begin; 
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alterlock alter column f2 set (n_distinct = 1);
--dg-cascade
--dg select * from my_locks order by 1;
rollback;

-- test that mixing options with different lock levels works as expected
begin; 
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alterlock set (autovacuum_enabled = off, fillfactor = 80);
--dg-cascade
--dg select * from my_locks order by 1;
commit;

begin; 
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alterlock alter column f2 set storage extended;
--dg-cascade
--dg select * from my_locks order by 1;
rollback;

begin; alter table alterlock alter column f2 set default 'x';
--dg-cascade
--dg select * from my_locks order by 1;
rollback;

begin;
--dg-skip: language "?" does not exist
--dg create function ttdummy () returns trigger language plpgsql as
--dg $$ begin return new; end $$;
--dg-cascade
--dg create trigger ttdummy
--dg 	before delete or update on alterlock
--dg 	for each row
--dg 	execute procedure
--dg 	ttdummy (1, 1);
--dg-cascade
--dg select * from my_locks order by 1;
rollback;

begin;
--dg-cascade
--dg select * from my_locks order by 1;
--dg-cascade
--dg alter table alterlock2 add foreign key (f1) references alterlock (f1);
--dg-cascade
--dg select * from my_locks order by 1;
rollback;

begin;
alter table alterlock2
add constraint alterlock2nv foreign key (f1) references alterlock (f1) NOT VALID;
--dg-cascade
--dg select * from my_locks order by 1;
commit;
begin;
--dg-cascade
--dg alter table alterlock2 validate constraint alterlock2nv;
--dg-cascade
--dg select * from my_locks order by 1;
rollback;

--dg-cascade
--dg create or replace view my_locks as
--dg select case when c.relname like 'pg_toast%' then 'pg_toast' else c.relname end, max(mode::lockmodes) as max_lockmode
--dg from pg_locks l join pg_class c on l.relation = c.oid
--dg where virtualtransaction = (
--dg         select virtualtransaction
--dg         from pg_locks
--dg         where transactionid = pg_current_xact_id()::xid)
--dg and locktype = 'relation'
--dg and relnamespace != (select oid from pg_namespace where nspname = 'pg_catalog')
--dg and c.relname = 'my_locks'
--dg group by c.relname;

-- raise exception
--dg-cascade
--dg alter table my_locks set (autovacuum_enabled = false);
--dg-cascade
--dg alter view my_locks set (autovacuum_enabled = false);
--dg-cascade
--dg alter table my_locks reset (autovacuum_enabled);
--dg-cascade
--dg alter view my_locks reset (autovacuum_enabled);

begin;
--dg-cascade
--dg alter view my_locks set (security_barrier=off);
--dg-cascade
--dg select * from my_locks order by 1;
--dg-cascade
--dg alter view my_locks reset (security_barrier);
rollback;

-- this test intentionally applies the ALTER TABLE command against a view, but
-- uses a view option so we expect this to succeed. This form of SQL is
-- accepted for historical reasons, as shown in the docs for ALTER VIEW
begin;
--dg-cascade
--dg alter table my_locks set (security_barrier=off);
--dg-cascade
--dg select * from my_locks order by 1;
--dg-cascade
--dg alter table my_locks reset (security_barrier);
rollback;

-- cleanup
drop table alterlock2;
drop table alterlock;
--dg-cascade
--dg drop view my_locks;
--dg-cascade
--dg drop type lockmodes;

--
-- alter function
--
--dg-skip: language "?" does not exist
--dg create function test_strict(text) returns text as
--dg     'select coalesce($1, ''got passed a null'');'
--dg     language sql returns null on null input;
--dg-cascade
--dg select test_strict(NULL);
--dg-cascade
--dg alter function test_strict(text) called on null input;
--dg-cascade
--dg select test_strict(NULL);

--dg-skip: language "?" does not exist
--dg create function non_strict(text) returns text as
--dg     'select coalesce($1, ''got passed a null'');'
--dg     language sql called on null input;
--dg-cascade
--dg select non_strict(NULL);
--dg-cascade
--dg alter function non_strict(text) returns null on null input;
--dg-cascade
--dg select non_strict(NULL);

--
-- alter object set schema
--

create schema alter1;
create schema alter2;

create table alter1.t1(f1 serial primary key, f2 int check (f2 > 0));

create view alter1.v1 as select * from alter1.t1;

--dg-skip: language "?" does not exist
--dg create function alter1.plus1(int) returns int as 'select $1+1' language sql;

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create domain alter1.posint integer check (value > 0);

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg create type alter1.ctype as (f1 int, f2 text);

--dg-skip: language "?" does not exist
--dg create function alter1.same(alter1.ctype, alter1.ctype) returns boolean language sql
--dg as 'select $1.f1 is not distinct from $2.f1 and $1.f2 is not distinct from $2.f2';

--dg-cascade
--dg create operator alter1.=(procedure = alter1.same, leftarg  = alter1.ctype, rightarg = alter1.ctype);

--dg-cascade
--dg create operator class alter1.ctype_hash_ops default for type alter1.ctype using hash as
--dg   operator 1 alter1.=(alter1.ctype, alter1.ctype);

--dg-skip: loading library "?" is not supported yet
--dg create conversion alter1.latin1_to_utf8 for 'latin1' to 'utf8' from iso8859_1_to_utf8;

--dg-skip: text search is not supported
--dg create text search parser alter1.prs(start = prsd_start, gettoken = prsd_nexttoken, end = prsd_end, lextypes = prsd_lextype);
--dg-skip: text search is not supported
--dg create text search configuration alter1.cfg(parser = alter1.prs);
--dg-skip: text search is not supported
--dg create text search template alter1.tmpl(init = dsimple_init, lexize = dsimple_lexize);
--dg-skip: text search is not supported
--dg create text search dictionary alter1.dict(template = alter1.tmpl);

insert into alter1.t1(f2) values(11);
insert into alter1.t1(f2) values(12);

--dg-skip: ALTER TABLE is not supported yet
--dg alter table alter1.t1 set schema alter1; -- no-op, same schema
--dg-skip: ALTER TABLE is not supported yet
--dg alter table alter1.t1 set schema alter2;
--dg-skip: ALTER TABLE is not supported yet
--dg alter table alter1.v1 set schema alter2;
--dg-cascade
--dg alter function alter1.plus1(int) set schema alter2;
--dg-cascade
--dg alter domain alter1.posint set schema alter2;
--dg-skip: relation pg_opclass cannot be opened: digres has no catalog tables
--dg alter operator class alter1.ctype_hash_ops using hash set schema alter2;
--dg-skip: relation pg_opfamily cannot be opened: digres has no catalog tables
--dg alter operator family alter1.ctype_hash_ops using hash set schema alter2;
--dg-cascade
--dg alter operator alter1.=(alter1.ctype, alter1.ctype) set schema alter2;
--dg-cascade
--dg alter function alter1.same(alter1.ctype, alter1.ctype) set schema alter2;
--dg-cascade
--dg alter type alter1.ctype set schema alter1; -- no-op, same schema
--dg-cascade
--dg alter type alter1.ctype set schema alter2;
--dg-skip: relation pg_conversion cannot be opened: digres has no catalog tables
--dg alter conversion alter1.latin1_to_utf8 set schema alter2;
--dg-skip: relation pg_ts_parser cannot be opened: digres has no catalog tables
--dg alter text search parser alter1.prs set schema alter2;
--dg-skip: relation pg_ts_config cannot be opened: digres has no catalog tables
--dg alter text search configuration alter1.cfg set schema alter2;
--dg-skip: relation pg_ts_template cannot be opened: digres has no catalog tables
--dg alter text search template alter1.tmpl set schema alter2;
--dg-skip: relation pg_ts_dict cannot be opened: digres has no catalog tables
--dg alter text search dictionary alter1.dict set schema alter2;

-- this should succeed because nothing is left in alter1
--dg-cascade
--dg drop schema alter1;

--dg-cascade
--dg insert into alter2.t1(f2) values(13);
--dg-cascade
--dg insert into alter2.t1(f2) values(14);

--dg-cascade
--dg select * from alter2.t1;

--dg-cascade
--dg select * from alter2.v1;

--dg-cascade
--dg select alter2.plus1(41);

-- clean up
--dg-cascade
--dg drop schema alter2 cascade;

--
-- composite types
--

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE test_type AS (a int);
--dg-cascade
--dg \d test_type

--dg-differs: another error: ALTER TYPE is not supported yet
--dg ALTER TYPE nosuchtype ADD ATTRIBUTE b text; -- fails

--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_type ADD ATTRIBUTE b text;
--dg-cascade
--dg \d test_type

--dg-cascade
--dg ALTER TYPE test_type ADD ATTRIBUTE b text; -- fails

--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_type ALTER ATTRIBUTE b SET DATA TYPE varchar;
--dg-cascade
--dg \d test_type

--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_type ALTER ATTRIBUTE b SET DATA TYPE integer;
--dg-cascade
--dg \d test_type

--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_type DROP ATTRIBUTE b;
--dg-cascade
--dg \d test_type

--dg-cascade
--dg ALTER TYPE test_type DROP ATTRIBUTE c; -- fails

--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_type DROP ATTRIBUTE IF EXISTS c;

--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_type DROP ATTRIBUTE a, ADD ATTRIBUTE d boolean;
--dg-cascade
--dg \d test_type

--dg-cascade
--dg ALTER TYPE test_type RENAME ATTRIBUTE a TO aa;
--dg-skip: ALTER TABLE ... RENAME COLUMN is not supported yet
--dg ALTER TYPE test_type RENAME ATTRIBUTE d TO dd;
--dg-cascade
--dg \d test_type

--dg-cascade
--dg DROP TYPE test_type;

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE test_type1 AS (a int, b text);
--dg-cascade
--dg CREATE TABLE test_tbl1 (x int, y test_type1);
--dg-cascade
--dg ALTER TYPE test_type1 ALTER ATTRIBUTE b TYPE varchar; -- fails

--dg-cascade
--dg DROP TABLE test_tbl1;
CREATE TABLE test_tbl1 (x int, y text);
--dg-cascade
--dg CREATE INDEX test_tbl1_idx ON test_tbl1((row(x,y)::test_type1));
--dg-cascade
--dg ALTER TYPE test_type1 ALTER ATTRIBUTE b TYPE varchar; -- fails

DROP TABLE test_tbl1;
--dg-cascade
--dg DROP TYPE test_type1;

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE test_type2 AS (a int, b text);
--dg-cascade
--dg CREATE TABLE test_tbl2 OF test_type2;
--dg-cascade
--dg CREATE TABLE test_tbl2_subclass () INHERITS (test_tbl2);
--dg-cascade
--dg \d test_type2
--dg-cascade
--dg \d test_tbl2

--dg-cascade
--dg ALTER TYPE test_type2 ADD ATTRIBUTE c text; -- fails
--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_type2 ADD ATTRIBUTE c text CASCADE;
--dg-cascade
--dg \d test_type2
--dg-cascade
--dg \d test_tbl2

--dg-cascade
--dg ALTER TYPE test_type2 ALTER ATTRIBUTE b TYPE varchar; -- fails
--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_type2 ALTER ATTRIBUTE b TYPE varchar CASCADE;
--dg-cascade
--dg \d test_type2
--dg-cascade
--dg \d test_tbl2

--dg-cascade
--dg ALTER TYPE test_type2 DROP ATTRIBUTE b; -- fails
--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_type2 DROP ATTRIBUTE b CASCADE;
--dg-cascade
--dg \d test_type2
--dg-cascade
--dg \d test_tbl2

--dg-cascade
--dg ALTER TYPE test_type2 RENAME ATTRIBUTE a TO aa; -- fails
--dg-skip: ALTER TABLE ... RENAME COLUMN is not supported yet
--dg ALTER TYPE test_type2 RENAME ATTRIBUTE a TO aa CASCADE;
--dg-cascade
--dg \d test_type2
--dg-cascade
--dg \d test_tbl2
--dg-cascade
--dg \d test_tbl2_subclass

--dg-cascade
--dg DROP TABLE test_tbl2_subclass, test_tbl2;
--dg-cascade
--dg DROP TYPE test_type2;

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE test_typex AS (a int, b text);
--dg-cascade
--dg CREATE TABLE test_tblx (x int, y test_typex check ((y).a > 0));
--dg-cascade
--dg ALTER TYPE test_typex DROP ATTRIBUTE a; -- fails
--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_typex DROP ATTRIBUTE a CASCADE;
--dg-cascade
--dg \d test_tblx
--dg-cascade
--dg DROP TABLE test_tblx;
--dg-cascade
--dg DROP TYPE test_typex;

-- This test isn't that interesting on its own, but the purpose is to leave
-- behind a table to test pg_upgrade with. The table has a composite type
-- column in it, and the composite type has a dropped attribute.
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE test_type3 AS (a int);
--dg-cascade
--dg CREATE TABLE test_tbl3 (c) AS SELECT '(1)'::test_type3;
--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE test_type3 DROP ATTRIBUTE a, ADD ATTRIBUTE b int;

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE test_type_empty AS ();
--dg-cascade
--dg DROP TYPE test_type_empty;

--
-- typed tables: OF / NOT OF
--

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE tt_t0 AS (z inet, x int, y numeric(8,2));
--dg-skip: ALTER TYPE is not supported yet
--dg ALTER TYPE tt_t0 DROP ATTRIBUTE z;
CREATE TABLE tt0 (x int NOT NULL, y numeric(8,2));	-- OK
CREATE TABLE tt1 (x int, y bigint);					-- wrong base type
CREATE TABLE tt2 (x int, y numeric(9,2));			-- wrong typmod
CREATE TABLE tt3 (y numeric(8,2), x int);			-- wrong column order
CREATE TABLE tt4 (x int);							-- too few columns
CREATE TABLE tt5 (x int, y numeric(8,2), z int);	-- too few columns
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE tt6 () INHERITS (tt0);					-- can't have a parent
CREATE TABLE tt7 (x int, q text, y numeric(8,2));
ALTER TABLE tt7 DROP q;								-- OK

--dg-cascade
--dg ALTER TABLE tt0 OF tt_t0;
--dg-cascade
--dg ALTER TABLE tt1 OF tt_t0;
--dg-cascade
--dg ALTER TABLE tt2 OF tt_t0;
--dg-cascade
--dg ALTER TABLE tt3 OF tt_t0;
--dg-cascade
--dg ALTER TABLE tt4 OF tt_t0;
--dg-cascade
--dg ALTER TABLE tt5 OF tt_t0;
--dg-cascade
--dg ALTER TABLE tt6 OF tt_t0;
--dg-cascade
--dg ALTER TABLE tt7 OF tt_t0;

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE tt_t1 AS (x int, y numeric(8,2));
--dg-cascade
--dg ALTER TABLE tt7 OF tt_t1;			-- reassign an already-typed table
--dg-cascade
--dg ALTER TABLE tt7 NOT OF;
\d tt7

-- make sure we can drop a constraint on the parent but it remains on the child
CREATE TABLE test_drop_constr_parent (c text CHECK (c IS NOT NULL));
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE test_drop_constr_child () INHERITS (test_drop_constr_parent);
ALTER TABLE ONLY test_drop_constr_parent DROP CONSTRAINT "test_drop_constr_parent_c_check";
-- should fail
--dg-cascade
--dg INSERT INTO test_drop_constr_child (c) VALUES (NULL);
--dg-cascade
--dg DROP TABLE test_drop_constr_parent CASCADE;

--
-- IF EXISTS test
--
ALTER TABLE IF EXISTS tt8 ADD COLUMN f int;
ALTER TABLE IF EXISTS tt8 ADD CONSTRAINT xxx PRIMARY KEY(f);
ALTER TABLE IF EXISTS tt8 ADD CHECK (f BETWEEN 0 AND 10);
ALTER TABLE IF EXISTS tt8 ALTER COLUMN f SET DEFAULT 0;
ALTER TABLE IF EXISTS tt8 RENAME COLUMN f TO f1;
--dg-skip: ALTER TABLE is not supported yet
--dg ALTER TABLE IF EXISTS tt8 SET SCHEMA alter2;

CREATE TABLE tt8(a int);
--dg-cascade
--dg CREATE SCHEMA alter2;

ALTER TABLE IF EXISTS tt8 ADD COLUMN f int;
ALTER TABLE IF EXISTS tt8 ADD CONSTRAINT xxx PRIMARY KEY(f);
ALTER TABLE IF EXISTS tt8 ADD CHECK (f BETWEEN 0 AND 10);
ALTER TABLE IF EXISTS tt8 ALTER COLUMN f SET DEFAULT 0;
ALTER TABLE IF EXISTS tt8 RENAME COLUMN f TO f1;
--dg-skip: ALTER TABLE is not supported yet
--dg ALTER TABLE IF EXISTS tt8 SET SCHEMA alter2;

--dg-cascade
--dg \d alter2.tt8

--dg-cascade
--dg DROP TABLE alter2.tt8;
DROP SCHEMA alter2;

--
-- Check conflicts between index and CHECK constraint names
--
CREATE TABLE tt9(c integer);
ALTER TABLE tt9 ADD CHECK(c > 1);
ALTER TABLE tt9 ADD CHECK(c > 2);  -- picks nonconflicting name
ALTER TABLE tt9 ADD CONSTRAINT foo CHECK(c > 3);
ALTER TABLE tt9 ADD CONSTRAINT foo CHECK(c > 4);  -- fail, dup name
ALTER TABLE tt9 ADD UNIQUE(c);
ALTER TABLE tt9 ADD UNIQUE(c);  -- picks nonconflicting name
ALTER TABLE tt9 ADD CONSTRAINT tt9_c_key UNIQUE(c);  -- fail, dup name
ALTER TABLE tt9 ADD CONSTRAINT foo UNIQUE(c);  -- fail, dup name
ALTER TABLE tt9 ADD CONSTRAINT tt9_c_key CHECK(c > 5);  -- fail, dup name
ALTER TABLE tt9 ADD CONSTRAINT tt9_c_key2 CHECK(c > 6);
ALTER TABLE tt9 ADD UNIQUE(c);  -- picks nonconflicting name
--dg-differs: the result
--dg \d tt9
DROP TABLE tt9;


-- Check that comments on constraints and indexes are not lost at ALTER TABLE.
CREATE TABLE comment_test (
  id int,
  constraint id_notnull_constraint not null id,
  positive_col int CHECK (positive_col > 0),
  indexed_col int,
  CONSTRAINT comment_test_pk PRIMARY KEY (id));
CREATE INDEX comment_test_index ON comment_test(indexed_col);

--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN comment_test.id IS 'Column ''id'' on comment_test';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON INDEX comment_test_index IS 'Simple index on comment_test';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON CONSTRAINT comment_test_positive_col_check ON comment_test IS 'CHECK constraint on comment_test.positive_col';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON CONSTRAINT comment_test_pk ON comment_test IS 'PRIMARY KEY constraint of comment_test';
--dg-skip: constraint "?" for table "?" does not exist
--dg COMMENT ON CONSTRAINT id_notnull_constraint ON comment_test IS 'NOT NULL constraint of comment_test';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON INDEX comment_test_pk IS 'Index backing the PRIMARY KEY of comment_test';

--dg-cascade
--dg SELECT col_description('comment_test'::regclass, 1) as comment;
--dg-cascade
--dg SELECT indexrelid::regclass::text as index, obj_description(indexrelid, 'pg_class') as comment FROM pg_index where indrelid = 'comment_test'::regclass ORDER BY 1, 2;
--dg-cascade
--dg SELECT conname as constraint, obj_description(oid, 'pg_constraint') as comment FROM pg_constraint where conrelid = 'comment_test'::regclass ORDER BY 1, 2;

-- Change the datatype of all the columns. ALTER TABLE is optimized to not
-- rebuild an index if the new data type is binary compatible with the old
-- one. Check do a dummy ALTER TABLE that doesn't change the datatype
-- first, to test that no-op codepath, and another one that does.
ALTER TABLE comment_test ALTER COLUMN indexed_col SET DATA TYPE int;
ALTER TABLE comment_test ALTER COLUMN indexed_col SET DATA TYPE text;
ALTER TABLE comment_test ALTER COLUMN id SET DATA TYPE int;
ALTER TABLE comment_test ALTER COLUMN id SET DATA TYPE text;
--dg-skip: ALTER COLUMN TYPE of a column a check constraint reads is not supported yet
--dg ALTER TABLE comment_test ALTER COLUMN positive_col SET DATA TYPE int;
--dg-skip: ALTER COLUMN TYPE of a column a check constraint reads is not supported yet
--dg ALTER TABLE comment_test ALTER COLUMN positive_col SET DATA TYPE bigint;

-- Some error cases.
--dg-differs: another error: column "?" of relation "?" does not exist
--dg ALTER TABLE comment_test ALTER COLUMN xmin SET DATA TYPE x;
--dg-differs: another error: type "?" does not exist
--dg ALTER TABLE comment_test ALTER COLUMN id SET DATA TYPE x;
--dg-differs: another error: ALTER COLUMN TYPE ... COLLATE is not supported yet
--dg ALTER TABLE comment_test ALTER COLUMN id SET DATA TYPE int COLLATE "C";

-- Check that the comments are intact.
--dg-cascade
--dg SELECT col_description('comment_test'::regclass, 1) as comment;
--dg-cascade
--dg SELECT indexrelid::regclass::text as index, obj_description(indexrelid, 'pg_class') as comment FROM pg_index where indrelid = 'comment_test'::regclass ORDER BY 1, 2;
--dg-cascade
--dg SELECT conname as constraint, obj_description(oid, 'pg_constraint') as comment FROM pg_constraint where conrelid = 'comment_test'::regclass ORDER BY 1, 2;

-- Check compatibility for foreign keys and comments. This is done
-- separately as rebuilding the column type of the parent leads
-- to an error and would reduce the test scope.
CREATE TABLE comment_test_child (
  id text CONSTRAINT comment_test_child_fk REFERENCES comment_test);
CREATE INDEX comment_test_child_fk ON comment_test_child(id);
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON COLUMN comment_test_child.id IS 'Column ''id'' on comment_test_child';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON INDEX comment_test_child_fk IS 'Index backing the FOREIGN KEY of comment_test_child';
--dg-skip: COMMENT is not supported yet
--dg COMMENT ON CONSTRAINT comment_test_child_fk ON comment_test_child IS 'FOREIGN KEY constraint of comment_test_child';

-- Change column type of parent
--dg-skip: ALTER COLUMN TYPE of a column a foreign key references is not supported yet
--dg ALTER TABLE comment_test ALTER COLUMN id SET DATA TYPE text;
--dg-differs: another error: ALTER COLUMN TYPE of a column a foreign key references is not supported yet
--dg ALTER TABLE comment_test ALTER COLUMN id SET DATA TYPE int USING id::integer;

-- Comments should be intact
--dg-cascade
--dg SELECT col_description('comment_test_child'::regclass, 1) as comment;
--dg-cascade
--dg SELECT indexrelid::regclass::text as index, obj_description(indexrelid, 'pg_class') as comment FROM pg_index where indrelid = 'comment_test_child'::regclass ORDER BY 1, 2;
--dg-cascade
--dg SELECT conname as constraint, obj_description(oid, 'pg_constraint') as comment FROM pg_constraint where conrelid = 'comment_test_child'::regclass ORDER BY 1, 2;

-- Check that we map relation oids to filenodes and back correctly.  Only
-- display bad mappings so the test output doesn't change all the time.  A
-- filenode function call can return NULL for a relation dropped concurrently
-- with the call's surrounding query, so ignore a NULL mapped_oid for
-- relations that no longer exist after all calls finish.
-- Temporary relations are ignored, as not supported by pg_filenode_relation().
--dg-skip: type ? is not supported yet
--dg CREATE TEMP TABLE filenode_mapping AS
--dg SELECT
--dg     oid, mapped_oid, reltablespace, relfilenode, relname
--dg FROM pg_class,
--dg     pg_filenode_relation(reltablespace, pg_relation_filenode(oid)) AS mapped_oid
--dg WHERE relkind IN ('r', 'i', 'S', 't', 'm')
--dg   AND relpersistence != 't'
--dg   AND mapped_oid IS DISTINCT FROM oid;
--dg-cascade
--dg SELECT m.* FROM filenode_mapping m LEFT JOIN pg_class c ON c.oid = m.oid
--dg WHERE c.oid IS NOT NULL OR m.mapped_oid IS NOT NULL;

-- Checks on creating and manipulation of user defined relations in
-- pg_catalog.

SHOW allow_system_table_mods;
-- disallowed because of search_path issues with pg_dump
CREATE TABLE pg_catalog.new_system_table();
-- instead create in public first, move to catalog
CREATE TABLE new_system_table(id serial primary key, othercol text);
--dg-skip: ALTER TABLE is not supported yet
--dg ALTER TABLE new_system_table SET SCHEMA pg_catalog;
--dg-skip: ALTER TABLE is not supported yet
--dg ALTER TABLE new_system_table SET SCHEMA public;
--dg-skip: ALTER TABLE is not supported yet
--dg ALTER TABLE new_system_table SET SCHEMA pg_catalog;
-- will be ignored -- already there:
--dg-skip: ALTER TABLE is not supported yet
--dg ALTER TABLE new_system_table SET SCHEMA pg_catalog;
ALTER TABLE new_system_table RENAME TO old_system_table;
CREATE INDEX old_system_table__othercol ON old_system_table (othercol);
INSERT INTO old_system_table(othercol) VALUES ('somedata'), ('otherdata');
UPDATE old_system_table SET id = -id;
DELETE FROM old_system_table WHERE othercol = 'somedata';
TRUNCATE old_system_table;
ALTER TABLE old_system_table DROP CONSTRAINT new_system_table_pkey;
--dg-skip: cannot drop column othercol of table old_system_table because other objects depend on it
--dg ALTER TABLE old_system_table DROP COLUMN othercol;
DROP TABLE old_system_table;

-- set logged
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not TEMPORARY or UNLOGGED
--dg CREATE UNLOGGED TABLE unlogged1(f1 SERIAL PRIMARY KEY, f2 TEXT); -- has sequence, toast
-- check relpersistence of an unlogged table
--dg-cascade
--dg SELECT relname, relkind, relpersistence FROM pg_class WHERE relname ~ '^unlogged1'
--dg UNION ALL
--dg SELECT r.relname || ' toast table', t.relkind, t.relpersistence FROM pg_class r JOIN pg_class t ON t.oid = r.reltoastrelid WHERE r.relname ~ '^unlogged1'
--dg UNION ALL
--dg SELECT r.relname || ' toast index', ri.relkind, ri.relpersistence FROM pg_class r join pg_class t ON t.oid = r.reltoastrelid JOIN pg_index i ON i.indrelid = t.oid JOIN pg_class ri ON ri.oid = i.indexrelid WHERE r.relname ~ '^unlogged1'
--dg ORDER BY relname;
--dg-cascade
--dg CREATE UNLOGGED TABLE unlogged2(f1 SERIAL PRIMARY KEY, f2 INTEGER REFERENCES unlogged1); -- foreign key
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not TEMPORARY or UNLOGGED
--dg CREATE UNLOGGED TABLE unlogged3(f1 SERIAL PRIMARY KEY, f2 INTEGER REFERENCES unlogged3); -- self-referencing foreign key
--dg-cascade
--dg ALTER TABLE unlogged3 SET LOGGED; -- skip self-referencing foreign key
--dg-cascade
--dg ALTER TABLE unlogged2 SET LOGGED; -- fails because a foreign key to an unlogged table exists
--dg-cascade
--dg ALTER TABLE unlogged1 SET LOGGED;
-- check relpersistence of an unlogged table after changing to permanent
--dg-cascade
--dg SELECT relname, relkind, relpersistence FROM pg_class WHERE relname ~ '^unlogged1'
--dg UNION ALL
--dg SELECT r.relname || ' toast table', t.relkind, t.relpersistence FROM pg_class r JOIN pg_class t ON t.oid = r.reltoastrelid WHERE r.relname ~ '^unlogged1'
--dg UNION ALL
--dg SELECT r.relname || ' toast index', ri.relkind, ri.relpersistence FROM pg_class r join pg_class t ON t.oid = r.reltoastrelid JOIN pg_index i ON i.indrelid = t.oid JOIN pg_class ri ON ri.oid = i.indexrelid WHERE r.relname ~ '^unlogged1'
--dg ORDER BY relname;
--dg-cascade
--dg ALTER TABLE unlogged1 SET LOGGED; -- silently do nothing
--dg-cascade
--dg DROP TABLE unlogged3;
--dg-cascade
--dg DROP TABLE unlogged2;
--dg-cascade
--dg DROP TABLE unlogged1;

-- set unlogged
CREATE TABLE logged1(f1 SERIAL PRIMARY KEY, f2 TEXT); -- has sequence, toast
-- check relpersistence of a permanent table
--dg-differs: the result
--dg SELECT relname, relkind, relpersistence FROM pg_class WHERE relname ~ '^logged1'
--dg UNION ALL
--dg SELECT r.relname || ' toast table', t.relkind, t.relpersistence FROM pg_class r JOIN pg_class t ON t.oid = r.reltoastrelid WHERE r.relname ~ '^logged1'
--dg UNION ALL
--dg SELECT r.relname ||' toast index', ri.relkind, ri.relpersistence FROM pg_class r join pg_class t ON t.oid = r.reltoastrelid JOIN pg_index i ON i.indrelid = t.oid JOIN pg_class ri ON ri.oid = i.indexrelid WHERE r.relname ~ '^logged1'
--dg ORDER BY relname;
CREATE TABLE logged2(f1 SERIAL PRIMARY KEY, f2 INTEGER REFERENCES logged1); -- foreign key
CREATE TABLE logged3(f1 SERIAL PRIMARY KEY, f2 INTEGER REFERENCES logged3); -- self-referencing foreign key
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE logged1 SET UNLOGGED; -- fails because a foreign key from a permanent table exists
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE logged3 SET UNLOGGED; -- skip self-referencing foreign key
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE logged2 SET UNLOGGED;
--dg-cascade
--dg ALTER TABLE logged1 SET UNLOGGED;
-- check relpersistence of a permanent table after changing to unlogged
--dg-cascade
--dg SELECT relname, relkind, relpersistence FROM pg_class WHERE relname ~ '^logged1'
--dg UNION ALL
--dg SELECT r.relname || ' toast table', t.relkind, t.relpersistence FROM pg_class r JOIN pg_class t ON t.oid = r.reltoastrelid WHERE r.relname ~ '^logged1'
--dg UNION ALL
--dg SELECT r.relname || ' toast index', ri.relkind, ri.relpersistence FROM pg_class r join pg_class t ON t.oid = r.reltoastrelid JOIN pg_index i ON i.indrelid = t.oid JOIN pg_class ri ON ri.oid = i.indexrelid WHERE r.relname ~ '^logged1'
--dg ORDER BY relname;
--dg-cascade
--dg ALTER TABLE logged1 SET UNLOGGED; -- silently do nothing
DROP TABLE logged3;
DROP TABLE logged2;
DROP TABLE logged1;

-- test ADD COLUMN IF NOT EXISTS
CREATE TABLE test_add_column(c1 integer);
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN c2 integer;
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN c2 integer; -- fail because c2 already exists
ALTER TABLE ONLY test_add_column
	ADD COLUMN c2 integer; -- fail because c2 already exists
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN IF NOT EXISTS c2 integer; -- skipping because c2 already exists
ALTER TABLE ONLY test_add_column
	ADD COLUMN IF NOT EXISTS c2 integer; -- skipping because c2 already exists
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN c2 integer, -- fail because c2 already exists
	ADD COLUMN c3 integer primary key;
\d test_add_column
--dg-skip: ALTER TABLE ... ADD COLUMN supports only NOT NULL, DEFAULT and CHECK yet
--dg ALTER TABLE test_add_column
--dg 	ADD COLUMN IF NOT EXISTS c2 integer, -- skipping because c2 already exists
--dg 	ADD COLUMN c3 integer primary key;
--dg-cascade
--dg \d test_add_column
--dg-cascade
--dg ALTER TABLE test_add_column
--dg 	ADD COLUMN IF NOT EXISTS c2 integer, -- skipping because c2 already exists
--dg 	ADD COLUMN IF NOT EXISTS c3 integer primary key; -- skipping because c3 already exists
--dg-differs: the result
--dg \d test_add_column
--dg-cascade
--dg ALTER TABLE test_add_column
--dg 	ADD COLUMN IF NOT EXISTS c2 integer, -- skipping because c2 already exists
--dg 	ADD COLUMN IF NOT EXISTS c3 integer, -- skipping because c3 already exists
--dg 	ADD COLUMN c4 integer REFERENCES test_add_column;
--dg-cascade
--dg \d test_add_column
--dg-cascade
--dg ALTER TABLE test_add_column
--dg 	ADD COLUMN IF NOT EXISTS c4 integer REFERENCES test_add_column;
--dg-cascade
--dg \d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN IF NOT EXISTS c5 SERIAL CHECK (c5 > 8);
--dg-cascade
--dg \d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN IF NOT EXISTS c5 SERIAL CHECK (c5 > 10);
ALTER TABLE test_add_column
	ADD c6 integer; -- omit COLUMN
ALTER TABLE test_add_column
	ADD IF NOT EXISTS c6 integer;
ALTER TABLE test_add_column
	DROP c6; -- omit COLUMN
ALTER TABLE test_add_column
	DROP IF EXISTS c6;
--dg-cascade
--dg \d test_add_column*
DROP TABLE test_add_column;
\d test_add_column*

-- assorted cases with multiple ALTER TABLE steps
CREATE TABLE ataddindex(f1 INT);
INSERT INTO ataddindex VALUES (42), (43);
CREATE UNIQUE INDEX ataddindexi0 ON ataddindex(f1);
--dg-skip: ALTER TABLE ... ADD CONSTRAINT ... USING INDEX is not supported yet
--dg ALTER TABLE ataddindex
--dg   ADD PRIMARY KEY USING INDEX ataddindexi0,
--dg   ALTER f1 TYPE BIGINT;
--dg-cascade
--dg \d ataddindex
DROP TABLE ataddindex;

CREATE TABLE ataddindex(f1 VARCHAR(10));
INSERT INTO ataddindex(f1) VALUES ('foo'), ('a');
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY, PRIMARY KEY, UNIQUE and CHECK yet
--dg ALTER TABLE ataddindex
--dg   ALTER f1 SET DATA TYPE TEXT,
--dg   ADD EXCLUDE ((f1 LIKE 'a') WITH =);
--dg-cascade
--dg \d ataddindex
DROP TABLE ataddindex;

CREATE TABLE ataddindex(id int, ref_id int);
ALTER TABLE ataddindex
  ADD PRIMARY KEY (id),
  ADD FOREIGN KEY (ref_id) REFERENCES ataddindex;
--dg-differs: the result
--dg \d ataddindex
DROP TABLE ataddindex;

CREATE TABLE ataddindex(id int, ref_id int);
--dg-skip: there is no unique constraint matching given keys for referenced table "?"
--dg ALTER TABLE ataddindex
--dg   ADD UNIQUE (id),
--dg   ADD FOREIGN KEY (ref_id) REFERENCES ataddindex (id);
--dg-cascade
--dg \d ataddindex
DROP TABLE ataddindex;

CREATE TABLE atnotnull1 ();
ALTER TABLE atnotnull1
  ADD COLUMN a INT,
  ALTER a SET NOT NULL;
--dg-skip: ALTER TABLE ... ADD CONSTRAINT supports only FOREIGN KEY, PRIMARY KEY, UNIQUE and CHECK yet
--dg ALTER TABLE atnotnull1
--dg   ADD COLUMN b INT,
--dg   ADD NOT NULL b;
ALTER TABLE atnotnull1
  ADD COLUMN c INT,
  ADD PRIMARY KEY (c);
--dg-cascade
--dg \d+ atnotnull1

-- cannot drop column that is part of the partition key
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE partitioned (
--dg 	a int,
--dg 	b int
--dg ) PARTITION BY RANGE (a, (a+b+1));
--dg-cascade
--dg ALTER TABLE partitioned DROP COLUMN a;
--dg-cascade
--dg ALTER TABLE partitioned ALTER COLUMN a TYPE char(5);
--dg-cascade
--dg ALTER TABLE partitioned DROP COLUMN b;
--dg-cascade
--dg ALTER TABLE partitioned ALTER COLUMN b TYPE char(5);

-- specifying storage parameters for partitioned tables is not supported
--dg-cascade
--dg ALTER TABLE partitioned SET (fillfactor=100);

-- partitioned table cannot participate in regular inheritance
CREATE TABLE nonpartitioned (
	a int,
	b int
);
--dg-cascade
--dg ALTER TABLE partitioned INHERIT nonpartitioned;
--dg-cascade
--dg ALTER TABLE nonpartitioned INHERIT partitioned;

-- cannot add NO INHERIT constraint to partitioned tables
--dg-cascade
--dg ALTER TABLE partitioned ADD CONSTRAINT chk_a CHECK (a > 0) NO INHERIT;

--dg-cascade
--dg DROP TABLE partitioned, nonpartitioned;

--
-- ATTACH PARTITION
--

-- check that target table is partitioned
CREATE TABLE unparted (
	a int
);
CREATE TABLE fail_part (like unparted);
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE unparted ATTACH PARTITION fail_part FOR VALUES IN ('a');
DROP TABLE unparted, fail_part;

-- check that partition bound is compatible
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE list_parted (
--dg 	a int NOT NULL,
--dg 	b char(2) COLLATE "C",
--dg 	CONSTRAINT check_a CHECK (a > 0)
--dg ) PARTITION BY LIST (a);
--dg-cascade
--dg CREATE TABLE fail_part (LIKE list_parted);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION fail_part FOR VALUES FROM (1) TO (10);
--dg-cascade
--dg DROP TABLE fail_part;

-- check that the table being attached exists
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION nonexistent FOR VALUES IN (1);

-- check ownership of the source table
CREATE ROLE regress_test_me;
CREATE ROLE regress_test_not_me;
--dg-cascade
--dg CREATE TABLE not_owned_by_me (LIKE list_parted);
--dg-cascade
--dg ALTER TABLE not_owned_by_me OWNER TO regress_test_not_me;
SET SESSION AUTHORIZATION regress_test_me;
CREATE TABLE owned_by_me (
	a int
) PARTITION BY LIST (a);
ALTER TABLE owned_by_me ATTACH PARTITION not_owned_by_me FOR VALUES IN (1);
RESET SESSION AUTHORIZATION;
DROP TABLE owned_by_me, not_owned_by_me;
--dg-cascade
--dg DROP ROLE regress_test_not_me;
DROP ROLE regress_test_me;

-- check that the table being attached is not part of regular inheritance
--dg-cascade
--dg CREATE TABLE parent (LIKE list_parted);
--dg-cascade
--dg CREATE TABLE child () INHERITS (parent);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION child FOR VALUES IN (1);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION parent FOR VALUES IN (1);
--dg-cascade
--dg DROP TABLE child;
-- now it should work, with a little tweak
--dg-cascade
--dg ALTER TABLE parent ADD CONSTRAINT check_a CHECK (a > 0);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION parent FOR VALUES IN (1);
-- test insert/update, per bug #18550
--dg-cascade
--dg INSERT INTO parent VALUES (1);
--dg-cascade
--dg UPDATE parent SET a = 2 WHERE a = 1;
--dg-cascade
--dg DROP TABLE parent CASCADE;

-- check any TEMP-ness
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TEMP TABLE temp_parted (a int) PARTITION BY LIST (a);
CREATE TABLE perm_part (a int);
--dg-cascade
--dg ALTER TABLE temp_parted ATTACH PARTITION perm_part FOR VALUES IN (1);
--dg-cascade
--dg DROP TABLE temp_parted, perm_part;

-- check that the table being attached is not a typed table
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE mytype AS (a int);
--dg-cascade
--dg CREATE TABLE fail_part OF mytype;
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION fail_part FOR VALUES IN (1);
--dg-cascade
--dg DROP TYPE mytype CASCADE;

-- check that the table being attached has only columns present in the parent
--dg-cascade
--dg CREATE TABLE fail_part (like list_parted, c int);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION fail_part FOR VALUES IN (1);
--dg-cascade
--dg DROP TABLE fail_part;

-- check that the table being attached has every column of the parent
CREATE TABLE fail_part (a int NOT NULL);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION fail_part FOR VALUES IN (1);
DROP TABLE fail_part;

-- check that columns match in type, collation and NOT NULL status
CREATE TABLE fail_part (
	b char(3),
	a int NOT NULL
);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION fail_part FOR VALUES IN (1);
--dg-skip: ALTER COLUMN TYPE ... COLLATE is not supported yet
--dg ALTER TABLE fail_part ALTER b TYPE char (2) COLLATE "POSIX";
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION fail_part FOR VALUES IN (1);
DROP TABLE fail_part;

-- check that the table being attached has all constraints of the parent
--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE fail_part (
--dg 	b char(2) COLLATE "C",
--dg 	a int NOT NULL
--dg );
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION fail_part FOR VALUES IN (1);

-- check that the constraint matches in definition with parent's constraint
--dg-cascade
--dg ALTER TABLE fail_part ADD CONSTRAINT check_a CHECK (a >= 0);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION fail_part FOR VALUES IN (1);
--dg-cascade
--dg DROP TABLE fail_part;

-- check the attributes and constraints after partition is attached
--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE part_1 (
--dg 	a int NOT NULL,
--dg 	b char(2) COLLATE "C",
--dg 	CONSTRAINT check_a CHECK (a > 0)
--dg );
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION part_1 FOR VALUES IN (1);
-- attislocal and conislocal are always false for merged attributes and constraints respectively.
--dg-cascade
--dg SELECT attislocal, attinhcount FROM pg_attribute WHERE attrelid = 'part_1'::regclass AND attnum > 0;
--dg-cascade
--dg SELECT conislocal, coninhcount FROM pg_constraint WHERE conrelid = 'part_1'::regclass AND conname = 'check_a';

-- check that NOT NULL NO INHERIT cannot be merged to a normal NOT NULL
--dg-skip: column "?" asks for what CREATE TABLE does not support yet
--dg CREATE TABLE part_fail (a int NOT NULL NO INHERIT,
--dg 	b char(2) COLLATE "C",
--dg 	CONSTRAINT check_a CHECK (a > 0)
--dg );
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION part_fail FOR VALUES IN (2);
--dg-cascade
--dg DROP TABLE part_fail;

-- check that the new partition won't overlap with an existing partition
--dg-cascade
--dg CREATE TABLE fail_part (LIKE part_1 INCLUDING CONSTRAINTS);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION fail_part FOR VALUES IN (1);
--dg-cascade
--dg DROP TABLE fail_part;
-- check that an existing table can be attached as a default partition
--dg-cascade
--dg CREATE TABLE def_part (LIKE list_parted INCLUDING CONSTRAINTS);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION def_part DEFAULT;
-- check attaching default partition fails if a default partition already
-- exists
--dg-cascade
--dg CREATE TABLE fail_def_part (LIKE part_1 INCLUDING CONSTRAINTS);
--dg-cascade
--dg ALTER TABLE list_parted ATTACH PARTITION fail_def_part DEFAULT;

-- check validation when attaching list partitions
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE list_parted2 (
--dg 	a int,
--dg 	b char
--dg ) PARTITION BY LIST (a);

-- check that violating rows are correctly reported
--dg-cascade
--dg CREATE TABLE part_2 (LIKE list_parted2);
--dg-cascade
--dg INSERT INTO part_2 VALUES (3, 'a');
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_2 FOR VALUES IN (2);

-- should be ok after deleting the bad row
--dg-cascade
--dg DELETE FROM part_2;
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_2 FOR VALUES IN (2);

-- check partition cannot be attached if default has some row for its values
--dg-cascade
--dg CREATE TABLE list_parted2_def PARTITION OF list_parted2 DEFAULT;
--dg-cascade
--dg INSERT INTO list_parted2_def VALUES (11, 'z');
--dg-cascade
--dg CREATE TABLE part_3 (LIKE list_parted2);
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_3 FOR VALUES IN (11);
-- should be ok after deleting the bad row
--dg-cascade
--dg DELETE FROM list_parted2_def WHERE a = 11;
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_3 FOR VALUES IN (11);

-- adding constraints that describe the desired partition constraint
-- (or more restrictive) will help skip the validation scan
--dg-cascade
--dg CREATE TABLE part_3_4 (
--dg 	LIKE list_parted2,
--dg 	CONSTRAINT check_a CHECK (a IN (3))
--dg );

-- however, if a list partition does not accept nulls, there should be
-- an explicit NOT NULL constraint on the partition key column for the
-- validation scan to be skipped;
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_3_4 FOR VALUES IN (3, 4);

-- adding a NOT NULL constraint will cause the scan to be skipped
--dg-cascade
--dg ALTER TABLE list_parted2 DETACH PARTITION part_3_4;
--dg-cascade
--dg ALTER TABLE part_3_4 ALTER a SET NOT NULL;
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_3_4 FOR VALUES IN (3, 4);

-- check if default partition scan skipped
--dg-cascade
--dg ALTER TABLE list_parted2_def ADD CONSTRAINT check_a CHECK (a IN (5, 6));
--dg-cascade
--dg CREATE TABLE part_55_66 PARTITION OF list_parted2 FOR VALUES IN (55, 66);

-- check validation when attaching range partitions
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE range_parted (
--dg 	a int,
--dg 	b int
--dg ) PARTITION BY RANGE (a, b);

-- check that violating rows are correctly reported
CREATE TABLE part1 (
	a int NOT NULL CHECK (a = 1),
	b int NOT NULL CHECK (b >= 1 AND b <= 10)
);
INSERT INTO part1 VALUES (1, 10);
-- Remember the TO bound is exclusive
--dg-cascade
--dg ALTER TABLE range_parted ATTACH PARTITION part1 FOR VALUES FROM (1, 1) TO (1, 10);

-- should be ok after deleting the bad row
DELETE FROM part1;
--dg-cascade
--dg ALTER TABLE range_parted ATTACH PARTITION part1 FOR VALUES FROM (1, 1) TO (1, 10);

-- adding constraints that describe the desired partition constraint
-- (or more restrictive) will help skip the validation scan
CREATE TABLE part2 (
	a int NOT NULL CHECK (a = 1),
	b int NOT NULL CHECK (b >= 10 AND b < 18)
);
--dg-cascade
--dg ALTER TABLE range_parted ATTACH PARTITION part2 FOR VALUES FROM (1, 10) TO (1, 20);

-- Create default partition
--dg-cascade
--dg CREATE TABLE partr_def1 PARTITION OF range_parted DEFAULT;

-- Only one default partition is allowed, hence, following should give error
CREATE TABLE partr_def2 (LIKE part1 INCLUDING CONSTRAINTS);
--dg-cascade
--dg ALTER TABLE range_parted ATTACH PARTITION partr_def2 DEFAULT;

-- Overlapping partitions cannot be attached, hence, following should give error
--dg-cascade
--dg INSERT INTO partr_def1 VALUES (2, 10);
--dg-cascade
--dg CREATE TABLE part3 (LIKE range_parted);
--dg-cascade
--dg ALTER TABLE range_parted ATTACH partition part3 FOR VALUES FROM (2, 10) TO (2, 20);

-- Attaching partitions should be successful when there are no overlapping rows
--dg-cascade
--dg ALTER TABLE range_parted ATTACH partition part3 FOR VALUES FROM (3, 10) TO (3, 20);

-- check that leaf partitions are scanned when attaching a partitioned
-- table
--dg-cascade
--dg CREATE TABLE part_5 (
--dg 	LIKE list_parted2
--dg ) PARTITION BY LIST (b);

-- check that violating rows are correctly reported
--dg-cascade
--dg CREATE TABLE part_5_a PARTITION OF part_5 FOR VALUES IN ('a');
--dg-cascade
--dg INSERT INTO part_5_a (a, b) VALUES (6, 'a');
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_5 FOR VALUES IN (5);

-- delete the faulting row and also add a constraint to skip the scan
--dg-cascade
--dg DELETE FROM part_5_a WHERE a NOT IN (3);
--dg-cascade
--dg ALTER TABLE part_5 ADD CONSTRAINT check_a CHECK (a IS NOT NULL AND a = 5);
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_5 FOR VALUES IN (5);
--dg-cascade
--dg ALTER TABLE list_parted2 DETACH PARTITION part_5;
--dg-cascade
--dg ALTER TABLE part_5 DROP CONSTRAINT check_a;

-- scan should again be skipped, even though NOT NULL is now a column property
--dg-cascade
--dg ALTER TABLE part_5 ADD CONSTRAINT check_a CHECK (a IN (5)), ALTER a SET NOT NULL;
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_5 FOR VALUES IN (5);

-- Check the case where attnos of the partitioning columns in the table being
-- attached differs from the parent.  It should not affect the constraint-
-- checking logic that allows to skip the scan.
--dg-cascade
--dg CREATE TABLE part_6 (
--dg 	c int,
--dg 	LIKE list_parted2,
--dg 	CONSTRAINT check_a CHECK (a IS NOT NULL AND a = 6)
--dg );
--dg-cascade
--dg ALTER TABLE part_6 DROP c;
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_6 FOR VALUES IN (6);

-- Similar to above, but the table being attached is a partitioned table
-- whose partition has still different attnos for the root partitioning
-- columns.
--dg-cascade
--dg CREATE TABLE part_7 (
--dg 	LIKE list_parted2,
--dg 	CONSTRAINT check_a CHECK (a IS NOT NULL AND a = 7)
--dg ) PARTITION BY LIST (b);
--dg-cascade
--dg CREATE TABLE part_7_a_null (
--dg 	c int,
--dg 	d int,
--dg 	e int,
--dg 	LIKE list_parted2,  -- 'a' will have attnum = 4
--dg 	CONSTRAINT check_b CHECK (b IS NULL OR b = 'a'),
--dg 	CONSTRAINT check_a CHECK (a IS NOT NULL AND a = 7)
--dg );
--dg-cascade
--dg ALTER TABLE part_7_a_null DROP c, DROP d, DROP e;
--dg-cascade
--dg ALTER TABLE part_7 ATTACH PARTITION part_7_a_null FOR VALUES IN ('a', null);
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_7 FOR VALUES IN (7);

-- Same example, but check this time that the constraint correctly detects
-- violating rows
--dg-cascade
--dg ALTER TABLE list_parted2 DETACH PARTITION part_7;
--dg-cascade
--dg ALTER TABLE part_7 DROP CONSTRAINT check_a; -- thusly, scan won't be skipped
--dg-cascade
--dg INSERT INTO part_7 (a, b) VALUES (8, null), (9, 'a');
--dg-cascade
--dg SELECT tableoid::regclass, a, b FROM part_7 order by a;
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_7 FOR VALUES IN (7);

-- check that leaf partitions of default partition are scanned when
-- attaching a partitioned table.
--dg-cascade
--dg ALTER TABLE part_5 DROP CONSTRAINT check_a;
--dg-cascade
--dg CREATE TABLE part5_def PARTITION OF part_5 DEFAULT PARTITION BY LIST(a);
--dg-cascade
--dg CREATE TABLE part5_def_p1 PARTITION OF part5_def FOR VALUES IN (5);
--dg-cascade
--dg INSERT INTO part5_def_p1 VALUES (5, 'y');
--dg-cascade
--dg CREATE TABLE part5_p1 (LIKE part_5);
--dg-cascade
--dg ALTER TABLE part_5 ATTACH PARTITION part5_p1 FOR VALUES IN ('y');
-- should be ok after deleting the bad row
--dg-cascade
--dg DELETE FROM part5_def_p1 WHERE b = 'y';
--dg-cascade
--dg ALTER TABLE part_5 ATTACH PARTITION part5_p1 FOR VALUES IN ('y');

-- check that the table being attached is not already a partition
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION part_2 FOR VALUES IN (2);

-- check that circular inheritance is not allowed
--dg-cascade
--dg ALTER TABLE part_5 ATTACH PARTITION list_parted2 FOR VALUES IN ('b');
--dg-cascade
--dg ALTER TABLE list_parted2 ATTACH PARTITION list_parted2 FOR VALUES IN (0);

-- If a partitioned table being created or an existing table being attached
-- as a partition does not have a constraint that would allow validation scan
-- to be skipped, but an individual partition does, then the partition's
-- validation scan is skipped.
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE quuux (a int, b text) PARTITION BY LIST (a);
--dg-cascade
--dg CREATE TABLE quuux_default PARTITION OF quuux DEFAULT PARTITION BY LIST (b);
--dg-cascade
--dg CREATE TABLE quuux_default1 PARTITION OF quuux_default (
--dg 	CONSTRAINT check_1 CHECK (a IS NOT NULL AND a = 1)
--dg ) FOR VALUES IN ('b');
CREATE TABLE quuux1 (a int, b text);
--dg-cascade
--dg ALTER TABLE quuux ATTACH PARTITION quuux1 FOR VALUES IN (1); -- validate!
CREATE TABLE quuux2 (a int, b text);
--dg-cascade
--dg ALTER TABLE quuux ATTACH PARTITION quuux2 FOR VALUES IN (2); -- skip validation
DROP TABLE quuux1, quuux2;
-- should validate for quuux1, but not for quuux2
--dg-cascade
--dg CREATE TABLE quuux1 PARTITION OF quuux FOR VALUES IN (1);
--dg-cascade
--dg CREATE TABLE quuux2 PARTITION OF quuux FOR VALUES IN (2);
--dg-cascade
--dg DROP TABLE quuux;

-- check validation when attaching hash partitions

-- Use hand-rolled hash functions and operator class to get predictable result
-- on different machines. part_test_int4_ops is defined in test_setup.sql.

-- check that the new partition won't overlap with an existing partition
--dg-differs: another error: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE hash_parted (
--dg 	a int,
--dg 	b int
--dg ) PARTITION BY HASH (a part_test_int4_ops);
--dg-differs: another error: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE hpart_1 PARTITION OF hash_parted FOR VALUES WITH (MODULUS 4, REMAINDER 0);
CREATE TABLE fail_part (LIKE hpart_1);
ALTER TABLE hash_parted ATTACH PARTITION fail_part FOR VALUES WITH (MODULUS 8, REMAINDER 4);
ALTER TABLE hash_parted ATTACH PARTITION fail_part FOR VALUES WITH (MODULUS 8, REMAINDER 0);
DROP TABLE fail_part;

-- check validation when attaching hash partitions

-- check that violating rows are correctly reported
CREATE TABLE hpart_2 (LIKE hash_parted);
INSERT INTO hpart_2 VALUES (3, 0);
ALTER TABLE hash_parted ATTACH PARTITION hpart_2 FOR VALUES WITH (MODULUS 4, REMAINDER 1);

-- should be ok after deleting the bad row
DELETE FROM hpart_2;
ALTER TABLE hash_parted ATTACH PARTITION hpart_2 FOR VALUES WITH (MODULUS 4, REMAINDER 1);

-- check that leaf partitions are scanned when attaching a partitioned
-- table
CREATE TABLE hpart_5 (
	LIKE hash_parted
) PARTITION BY LIST (b);

-- check that violating rows are correctly reported
--dg-differs: another error: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE hpart_5_a PARTITION OF hpart_5 FOR VALUES IN ('1', '2', '3');
INSERT INTO hpart_5_a (a, b) VALUES (7, 1);
ALTER TABLE hash_parted ATTACH PARTITION hpart_5 FOR VALUES WITH (MODULUS 4, REMAINDER 2);

-- should be ok after deleting the bad row
DELETE FROM hpart_5_a;
ALTER TABLE hash_parted ATTACH PARTITION hpart_5 FOR VALUES WITH (MODULUS 4, REMAINDER 2);

-- check that the table being attach is with valid modulus and remainder value
CREATE TABLE fail_part(LIKE hash_parted);
ALTER TABLE hash_parted ATTACH PARTITION fail_part FOR VALUES WITH (MODULUS 0, REMAINDER 1);
ALTER TABLE hash_parted ATTACH PARTITION fail_part FOR VALUES WITH (MODULUS 8, REMAINDER 8);
ALTER TABLE hash_parted ATTACH PARTITION fail_part FOR VALUES WITH (MODULUS 3, REMAINDER 2);
DROP TABLE fail_part;

--
-- DETACH PARTITION
--

-- check that the table is partitioned at all
CREATE TABLE regular_table (a int);
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE regular_table DETACH PARTITION any_name;
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE regular_table DETACH PARTITION any_name CONCURRENTLY;
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE regular_table DETACH PARTITION any_name FINALIZE;
DROP TABLE regular_table;

-- check that the partition being detached exists at all
--dg-cascade
--dg ALTER TABLE list_parted2 DETACH PARTITION part_4;
ALTER TABLE hash_parted DETACH PARTITION hpart_4;

-- check that the partition being detached is actually a partition of the parent
CREATE TABLE not_a_part (a int);
--dg-cascade
--dg ALTER TABLE list_parted2 DETACH PARTITION not_a_part;
--dg-cascade
--dg ALTER TABLE list_parted2 DETACH PARTITION part_1;

ALTER TABLE hash_parted DETACH PARTITION not_a_part;
DROP TABLE not_a_part;

-- check that, after being detached, attinhcount/coninhcount is dropped to 0 and
-- attislocal/conislocal is set to true
--dg-cascade
--dg ALTER TABLE list_parted2 DETACH PARTITION part_3_4;
--dg-cascade
--dg SELECT attinhcount, attislocal FROM pg_attribute WHERE attrelid = 'part_3_4'::regclass AND attnum > 0;
--dg-cascade
--dg SELECT coninhcount, conislocal FROM pg_constraint WHERE conrelid = 'part_3_4'::regclass AND conname = 'check_a';
--dg-cascade
--dg DROP TABLE part_3_4;

-- check that a detached partition is not dropped on dropping a partitioned table
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE range_parted2 (
--dg     a int
--dg ) PARTITION BY RANGE(a);
--dg-cascade
--dg CREATE TABLE part_rp PARTITION OF range_parted2 FOR VALUES FROM (0) to (100);
--dg-cascade
--dg ALTER TABLE range_parted2 DETACH PARTITION part_rp;
--dg-cascade
--dg DROP TABLE range_parted2;
--dg-cascade
--dg SELECT * from part_rp;
--dg-cascade
--dg DROP TABLE part_rp;

-- concurrent detach
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE range_parted2 (
--dg 	a int
--dg ) PARTITION BY RANGE(a);
--dg-cascade
--dg CREATE TABLE part_rp PARTITION OF range_parted2 FOR VALUES FROM (0) to (100);
BEGIN;
-- doesn't work in a partition block
ALTER TABLE range_parted2 DETACH PARTITION part_rp CONCURRENTLY;
COMMIT;
--dg-cascade
--dg CREATE TABLE part_rpd PARTITION OF range_parted2 DEFAULT;
-- doesn't work if there's a default partition
--dg-cascade
--dg ALTER TABLE range_parted2 DETACH PARTITION part_rp CONCURRENTLY;
-- doesn't work for the default partition
--dg-cascade
--dg ALTER TABLE range_parted2 DETACH PARTITION part_rpd CONCURRENTLY;
--dg-cascade
--dg DROP TABLE part_rpd;
-- works fine
--dg-cascade
--dg ALTER TABLE range_parted2 DETACH PARTITION part_rp CONCURRENTLY;
--dg-cascade
--dg \d+ range_parted2
-- constraint should be created
--dg-cascade
--dg \d part_rp
--dg-cascade
--dg CREATE TABLE part_rp100 PARTITION OF range_parted2 (CHECK (a>=123 AND a<133 AND a IS NOT NULL)) FOR VALUES FROM (100) to (200);
--dg-cascade
--dg ALTER TABLE range_parted2 DETACH PARTITION part_rp100 CONCURRENTLY;
-- redundant constraint should not be created
--dg-cascade
--dg \d part_rp100
--dg-cascade
--dg DROP TABLE range_parted2;

-- Test that hash partitions continue to work after they're concurrently
-- detached (bugs #18371, #19070)
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE hash_parted2 (a int) PARTITION BY HASH(a);
--dg-cascade
--dg CREATE TABLE part_hp PARTITION OF hash_parted2 FOR VALUES WITH (MODULUS 2, REMAINDER 0);
--dg-cascade
--dg ALTER TABLE hash_parted2 DETACH PARTITION part_hp CONCURRENTLY;
--dg-cascade
--dg DROP TABLE hash_parted2;
--dg-cascade
--dg INSERT INTO part_hp VALUES (1);
--dg-cascade
--dg DROP TABLE part_hp;

-- Check ALTER TABLE commands for partitioned tables and partitions

-- cannot add/drop column to/from *only* the parent
--dg-cascade
--dg ALTER TABLE ONLY list_parted2 ADD COLUMN c int;
--dg-cascade
--dg ALTER TABLE ONLY list_parted2 DROP COLUMN b;

-- cannot add a column to partition or drop an inherited one
--dg-cascade
--dg ALTER TABLE part_2 ADD COLUMN c text;
--dg-cascade
--dg ALTER TABLE part_2 DROP COLUMN b;

-- Nor rename, alter type
--dg-cascade
--dg ALTER TABLE part_2 RENAME COLUMN b to c;
--dg-cascade
--dg ALTER TABLE part_2 ALTER COLUMN b TYPE text;

-- cannot add NOT NULL or check constraints to *only* the parent, when
-- partitions exist
--dg-cascade
--dg ALTER TABLE ONLY list_parted2 ALTER b SET NOT NULL;
--dg-cascade
--dg ALTER TABLE ONLY list_parted2 ADD CONSTRAINT check_b CHECK (b <> 'zz');

-- dropping them is ok though
--dg-cascade
--dg ALTER TABLE list_parted2 ALTER b SET NOT NULL;
--dg-cascade
--dg ALTER TABLE ONLY list_parted2 ALTER b DROP NOT NULL;
--dg-cascade
--dg ALTER TABLE list_parted2 ADD CONSTRAINT check_b CHECK (b <> 'zz');
--dg-cascade
--dg ALTER TABLE ONLY list_parted2 DROP CONSTRAINT check_b;
-- ... and the partitions should still have both
--dg-cascade
--dg \d+ part_2

-- It's alright though, if no partitions are yet created
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE parted_no_parts (a int) PARTITION BY LIST (a);
--dg-cascade
--dg ALTER TABLE ONLY parted_no_parts ALTER a SET NOT NULL;
--dg-cascade
--dg ALTER TABLE ONLY parted_no_parts ADD CONSTRAINT check_a CHECK (a > 0);
--dg-cascade
--dg DROP TABLE parted_no_parts;

-- cannot drop inherited NOT NULL or check constraints from partition
--dg-cascade
--dg ALTER TABLE list_parted2 ALTER b SET NOT NULL, ADD CONSTRAINT check_a2 CHECK (a > 0);
--dg-cascade
--dg ALTER TABLE part_2 ALTER b DROP NOT NULL;
--dg-cascade
--dg ALTER TABLE part_2 DROP CONSTRAINT check_a2;

-- can't drop NOT NULL from under an invalid PK
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE list_parted3 (a int NOT NULL) PARTITION BY LIST (a);
--dg-cascade
--dg CREATE TABLE list_parted3_1 PARTITION OF list_parted3 FOR VALUES IN (1);
--dg-cascade
--dg ALTER TABLE ONLY list_parted3 ADD PRIMARY KEY (a);
--dg-cascade
--dg ALTER TABLE ONLY list_parted3 DROP CONSTRAINT list_parted3_a_not_null;

-- Doesn't make sense to add NO INHERIT constraints on partitioned tables
--dg-cascade
--dg ALTER TABLE list_parted2 add constraint check_b2 check (b <> 'zz') NO INHERIT;

-- check that a partition cannot participate in regular inheritance
--dg-cascade
--dg CREATE TABLE inh_test () INHERITS (part_2);
--dg-cascade
--dg CREATE TABLE inh_test (LIKE part_2);
--dg-cascade
--dg ALTER TABLE inh_test INHERIT part_2;
--dg-cascade
--dg ALTER TABLE part_2 INHERIT inh_test;

-- cannot drop or alter type of partition key columns of lower level
-- partitioned tables; for example, part_5, which is list_parted2's
-- partition, is partitioned on b;
--dg-cascade
--dg ALTER TABLE list_parted2 DROP COLUMN b;
--dg-cascade
--dg ALTER TABLE list_parted2 ALTER COLUMN b TYPE text;

-- dropping non-partition key columns should be allowed on the parent table.
--dg-cascade
--dg ALTER TABLE list_parted DROP COLUMN b;
--dg-cascade
--dg SELECT * FROM list_parted;

-- cleanup
--dg-cascade
--dg DROP TABLE list_parted, list_parted2, range_parted, list_parted3;
--dg-cascade
--dg DROP TABLE fail_def_part;
DROP TABLE hash_parted;

-- more tests for certain multi-level partitioning scenarios
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table p (a int, b int) partition by range (a, b);
--dg-cascade
--dg create table p1 (b int, a int not null) partition by range (b);
create table p11 (like p1);
--dg-cascade
--dg alter table p11 drop a;
alter table p11 add a int;
alter table p11 drop a;
alter table p11 add a int not null;
-- attnum for key attribute 'a' is different in p, p1, and p11
--dg-cascade
--dg select attrelid::regclass, attname, attnum
--dg from pg_attribute
--dg where attname = 'a'
--dg  and (attrelid = 'p'::regclass
--dg    or attrelid = 'p1'::regclass
--dg    or attrelid = 'p11'::regclass)
--dg order by attrelid::regclass::text;

--dg-cascade
--dg alter table p1 attach partition p11 for values from (2) to (5);

--dg-cascade
--dg insert into p1 (a, b) values (2, 3);
-- check that partition validation scan correctly detects violating rows
--dg-cascade
--dg alter table p attach partition p1 for values from (1, 2) to (1, 10);

-- cleanup
--dg-cascade
--dg drop table p;
drop table p1;

-- validate constraint on partitioned tables should only scan leaf partitions
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table parted_validate_test (a int) partition by list (a);
--dg-cascade
--dg create table parted_validate_test_1 partition of parted_validate_test for values in (0, 1);
--dg-cascade
--dg alter table parted_validate_test add constraint parted_validate_test_chka check (a > 0) not valid;
--dg-cascade
--dg alter table parted_validate_test validate constraint parted_validate_test_chka;
--dg-cascade
--dg drop table parted_validate_test;
-- test alter column options
CREATE TABLE attmp(i integer);
INSERT INTO attmp VALUES (1);
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE attmp ALTER COLUMN i SET (n_distinct = 1, n_distinct_inherited = 2);
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE attmp ALTER COLUMN i RESET (n_distinct_inherited);
ANALYZE attmp;
DROP TABLE attmp;

DROP USER regress_alter_table_user1;

-- check that violating rows are correctly reported when attaching as the
-- default partition
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table defpart_attach_test (a int) partition by list (a);
--dg-cascade
--dg create table defpart_attach_test1 partition of defpart_attach_test for values in (1);
create table defpart_attach_test_d (b int, a int);
alter table defpart_attach_test_d drop b;
insert into defpart_attach_test_d values (1), (2);

-- error because its constraint as the default partition would be violated
-- by the row containing 1
--dg-cascade
--dg alter table defpart_attach_test attach partition defpart_attach_test_d default;
delete from defpart_attach_test_d where a = 1;
alter table defpart_attach_test_d add check (a > 1);

-- should be attached successfully and without needing to be scanned
--dg-cascade
--dg alter table defpart_attach_test attach partition defpart_attach_test_d default;

-- check that attaching a partition correctly reports any rows in the default
-- partition that should not be there for the new partition to be attached
-- successfully
create table defpart_attach_test_2 (like defpart_attach_test_d);
--dg-cascade
--dg alter table defpart_attach_test attach partition defpart_attach_test_2 for values in (2);

--dg-cascade
--dg drop table defpart_attach_test;

-- check combinations of temporary and permanent relations when attaching
-- partitions.
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table perm_part_parent (a int) partition by list (a);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create temp table temp_part_parent (a int) partition by list (a);
create table perm_part_child (a int);
create temp table temp_part_child (a int);
--dg-cascade
--dg alter table temp_part_parent attach partition perm_part_child default; -- error
--dg-cascade
--dg alter table perm_part_parent attach partition temp_part_child default; -- error
--dg-cascade
--dg alter table temp_part_parent attach partition temp_part_child default; -- ok
--dg-cascade
--dg drop table perm_part_parent cascade;
--dg-cascade
--dg drop table temp_part_parent cascade;

-- check that attaching partitions to a table while it is being used is
-- prevented
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table tab_part_attach (a int) partition by list (a);
--dg-skip: language "?" does not exist
--dg create or replace function func_part_attach() returns trigger
--dg   language plpgsql as $$
--dg   begin
--dg     execute 'create table tab_part_attach_1 (a int)';
--dg     execute 'alter table tab_part_attach attach partition tab_part_attach_1 for values in (1)';
--dg     return null;
--dg   end $$;
--dg-cascade
--dg create trigger trig_part_attach before insert on tab_part_attach
--dg   for each statement execute procedure func_part_attach();
--dg-cascade
--dg insert into tab_part_attach values (1);
--dg-cascade
--dg drop table tab_part_attach;
--dg-cascade
--dg drop function func_part_attach();

-- test case where the partitioning operator is a SQL function whose
-- evaluation results in the table's relcache being rebuilt partway through
-- the execution of an ATTACH PARTITION command
--dg-skip: language "?" does not exist
--dg create function at_test_sql_partop (int4, int4) returns int language sql
--dg as $$ select case when $1 = $2 then 0 when $1 > $2 then 1 else -1 end; $$;
--dg-skip: relation pg_opfamily cannot be opened: digres has no catalog tables
--dg create operator class at_test_sql_partop for type int4 using btree as
--dg     operator 1 < (int4, int4), operator 2 <= (int4, int4),
--dg     operator 3 = (int4, int4), operator 4 >= (int4, int4),
--dg     operator 5 > (int4, int4), function 1 at_test_sql_partop(int4, int4);
--dg-cascade
--dg create table at_test_sql_partop (a int) partition by range (a at_test_sql_partop);
create table at_test_sql_partop_1 (a int);
--dg-cascade
--dg alter table at_test_sql_partop attach partition at_test_sql_partop_1 for values from (0) to (10);
--dg-cascade
--dg drop table at_test_sql_partop;
--dg-cascade
--dg drop operator class at_test_sql_partop using btree;
--dg-cascade
--dg drop function at_test_sql_partop;


/* Test case for bug #16242 */

-- We create a parent and child where the child has missing
-- non-null attribute values, and arrange to pass them through
-- tuple conversion from the child to the parent tupdesc
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table bar1 (a integer, b integer not null default 1)
--dg   partition by range (a);
create table bar2 (a integer);
insert into bar2 values (1);
alter table bar2 add column b integer not null default 1;
-- (at this point bar2 contains tuple with natts=1)
--dg-cascade
--dg alter table bar1 attach partition bar2 default;

-- this works:
--dg-cascade
--dg select * from bar1;

-- this exercises tuple conversion:
--dg-skip: language "?" does not exist
--dg create function xtrig()
--dg   returns trigger language plpgsql
--dg as $$
--dg   declare
--dg     r record;
--dg   begin
--dg     for r in select * from old loop
--dg       raise info 'a=%, b=%', r.a, r.b;
--dg     end loop;
--dg     return NULL;
--dg   end;
--dg $$;
--dg-cascade
--dg create trigger xtrig
--dg   after update on bar1
--dg   referencing old table as old
--dg   for each statement execute procedure xtrig();

--dg-cascade
--dg update bar1 set a = a + 1;

/* End test case for bug #16242 */

/* Test case for bug #17409 */

create table attbl (p1 int constraint pk_attbl primary key);
create table atref (c1 int references attbl(p1));
--dg-differs: the result
--dg cluster attbl using pk_attbl;
--dg-skip: ALTER COLUMN TYPE of a column a foreign key references is not supported yet
--dg alter table attbl alter column p1 set data type bigint;
--dg-skip: ALTER COLUMN TYPE of a column a foreign key uses is not supported yet
--dg alter table atref alter column c1 set data type bigint;
drop table attbl, atref;

create table attbl (p1 int constraint pk_attbl primary key);
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table attbl replica identity using index pk_attbl;
create table atref (c1 int references attbl(p1));
--dg-skip: ALTER COLUMN TYPE of a column a foreign key references is not supported yet
--dg alter table attbl alter column p1 set data type bigint;
--dg-skip: ALTER COLUMN TYPE of a column a foreign key uses is not supported yet
--dg alter table atref alter column c1 set data type bigint;
drop table attbl, atref;

/* End test case for bug #17409 */

/* Test case for bug #18970 */

create table attbl(a int);
--dg-skip: type "?" does not exist
--dg create table atref(b attbl check ((b).a is not null));
--dg-cascade
--dg alter table attbl alter column a type numeric;  -- someday this should work
--dg-cascade
--dg alter table atref drop constraint atref_b_check;

--dg-cascade
--dg create statistics atref_stat on ((b).a is not null) from atref;
--dg-cascade
--dg alter table attbl alter column a type numeric;  -- someday this should work
--dg-cascade
--dg drop statistics atref_stat;

--dg-cascade
--dg create index atref_idx on atref (((b).a));
--dg-cascade
--dg alter table attbl alter column a type numeric;  -- someday this should work
--dg-cascade
--dg drop table attbl, atref;

/* End test case for bug #18970 */

-- Test that ALTER TABLE rewrite preserves a clustered index
-- for normal indexes and indexes on constraints.
create table alttype_cluster (a int);
alter table alttype_cluster add primary key (a);
create index alttype_cluster_ind on alttype_cluster (a);
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alttype_cluster cluster on alttype_cluster_ind;
-- Normal index remains clustered.
--dg-cascade
--dg select indexrelid::regclass, indisclustered from pg_index
--dg   where indrelid = 'alttype_cluster'::regclass
--dg   order by indexrelid::regclass::text;
alter table alttype_cluster alter a type bigint;
--dg-cascade
--dg select indexrelid::regclass, indisclustered from pg_index
--dg   where indrelid = 'alttype_cluster'::regclass
--dg   order by indexrelid::regclass::text;
-- Constraint index remains clustered.
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg alter table alttype_cluster cluster on alttype_cluster_pkey;
--dg-cascade
--dg select indexrelid::regclass, indisclustered from pg_index
--dg   where indrelid = 'alttype_cluster'::regclass
--dg   order by indexrelid::regclass::text;
alter table alttype_cluster alter a type int;
--dg-cascade
--dg select indexrelid::regclass, indisclustered from pg_index
--dg   where indrelid = 'alttype_cluster'::regclass
--dg   order by indexrelid::regclass::text;
drop table alttype_cluster;

--
-- Check that attaching or detaching a partitioned partition correctly leads
-- to its partitions' constraint being updated to reflect the parent's
-- newly added/removed constraint
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table target_parted (a int, b int) partition by list (a);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg create table attach_parted (a int, b int) partition by list (b);
--dg-cascade
--dg create table attach_parted_part1 partition of attach_parted for values in (1);
-- insert a row directly into the leaf partition so that its partition
-- constraint is built and stored in the relcache
--dg-cascade
--dg insert into attach_parted_part1 values (1, 1);
-- the following better invalidate the partition constraint of the leaf
-- partition too...
--dg-cascade
--dg alter table target_parted attach partition attach_parted for values in (1);
-- ...such that the following insert fails
--dg-cascade
--dg insert into attach_parted_part1 values (2, 1);
-- ...and doesn't when the partition is detached along with its own partition
--dg-cascade
--dg alter table target_parted detach partition attach_parted;
--dg-cascade
--dg insert into attach_parted_part1 values (2, 1);

-- Test altering table having publication
--dg-cascade
--dg create schema alter1;
create schema alter2;
--dg-cascade
--dg create table alter1.t1 (a int);
set client_min_messages = 'ERROR';
--dg-skip: publications are not supported
--dg create publication pub1 for table alter1.t1, tables in schema alter2;
reset client_min_messages;
--dg-skip: ALTER TABLE is not supported yet
--dg alter table alter1.t1 set schema alter2;
--dg-cascade
--dg \d+ alter2.t1
--dg-skip: publications are not supported
--dg drop publication pub1;
--dg-cascade
--dg drop schema alter1 cascade;
--dg-cascade
--dg drop schema alter2 cascade;
