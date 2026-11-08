-- Test basic TRUNCATE functionality.
CREATE TABLE truncate_a (col1 integer primary key);
INSERT INTO truncate_a VALUES (1);
INSERT INTO truncate_a VALUES (2);
SELECT * FROM truncate_a;
-- Roll truncate back
BEGIN;
TRUNCATE truncate_a;
ROLLBACK;
SELECT * FROM truncate_a;
-- Commit the truncate this time
BEGIN;
TRUNCATE truncate_a;
COMMIT;
SELECT * FROM truncate_a;

-- Test foreign-key checks
CREATE TABLE trunc_b (a int REFERENCES truncate_a);
CREATE TABLE trunc_c (a serial PRIMARY KEY);
CREATE TABLE trunc_d (a int REFERENCES trunc_c);
CREATE TABLE trunc_e (a int REFERENCES truncate_a, b int REFERENCES trunc_c);

TRUNCATE TABLE truncate_a;		-- fail
TRUNCATE TABLE truncate_a,trunc_b;		-- fail
TRUNCATE TABLE truncate_a,trunc_b,trunc_e;	-- ok
TRUNCATE TABLE truncate_a,trunc_e;		-- fail
TRUNCATE TABLE trunc_c;		-- fail
TRUNCATE TABLE trunc_c,trunc_d;		-- fail
TRUNCATE TABLE trunc_c,trunc_d,trunc_e;	-- ok
TRUNCATE TABLE trunc_c,trunc_d,trunc_e,truncate_a;	-- fail
TRUNCATE TABLE trunc_c,trunc_d,trunc_e,truncate_a,trunc_b;	-- ok

TRUNCATE TABLE truncate_a RESTRICT; -- fail
TRUNCATE TABLE truncate_a CASCADE;  -- ok

-- circular references
ALTER TABLE truncate_a ADD FOREIGN KEY (col1) REFERENCES trunc_c;

-- Add some data to verify that truncating actually works ...
INSERT INTO trunc_c VALUES (1);
INSERT INTO truncate_a VALUES (1);
INSERT INTO trunc_b VALUES (1);
INSERT INTO trunc_d VALUES (1);
INSERT INTO trunc_e VALUES (1,1);
--dg-differs: another error: cannot truncate a table referenced in a foreign key constraint
--dg TRUNCATE TABLE trunc_c;
TRUNCATE TABLE trunc_c,truncate_a;
TRUNCATE TABLE trunc_c,truncate_a,trunc_d;
TRUNCATE TABLE trunc_c,truncate_a,trunc_d,trunc_e;
TRUNCATE TABLE trunc_c,truncate_a,trunc_d,trunc_e,trunc_b;

-- Verify that truncating did actually work
SELECT * FROM truncate_a
   UNION ALL
 SELECT * FROM trunc_c
   UNION ALL
 SELECT * FROM trunc_b
   UNION ALL
 SELECT * FROM trunc_d;
SELECT * FROM trunc_e;

-- Add data again to test TRUNCATE ... CASCADE
INSERT INTO trunc_c VALUES (1);
INSERT INTO truncate_a VALUES (1);
INSERT INTO trunc_b VALUES (1);
INSERT INTO trunc_d VALUES (1);
INSERT INTO trunc_e VALUES (1,1);

--dg-differs: the result
--dg TRUNCATE TABLE trunc_c CASCADE;  -- ok

--dg-cascade
--dg SELECT * FROM truncate_a
--dg    UNION ALL
--dg  SELECT * FROM trunc_c
--dg    UNION ALL
--dg  SELECT * FROM trunc_b
--dg    UNION ALL
--dg  SELECT * FROM trunc_d;
--dg-cascade
--dg SELECT * FROM trunc_e;

DROP TABLE truncate_a,trunc_c,trunc_b,trunc_d,trunc_e CASCADE;

-- Test TRUNCATE with inheritance

CREATE TABLE trunc_f (col1 integer primary key);
INSERT INTO trunc_f VALUES (1);
INSERT INTO trunc_f VALUES (2);

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE trunc_fa (col2a text) INHERITS (trunc_f);
--dg-cascade
--dg INSERT INTO trunc_fa VALUES (3, 'three');

--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE trunc_fb (col2b int) INHERITS (trunc_f);
--dg-cascade
--dg INSERT INTO trunc_fb VALUES (4, 444);

--dg-cascade
--dg CREATE TABLE trunc_faa (col3 text) INHERITS (trunc_fa);
--dg-cascade
--dg INSERT INTO trunc_faa VALUES (5, 'five', 'FIVE');

BEGIN;
--dg-cascade
--dg SELECT * FROM trunc_f;
TRUNCATE trunc_f;
SELECT * FROM trunc_f;
ROLLBACK;

BEGIN;
--dg-cascade
--dg SELECT * FROM trunc_f;
TRUNCATE ONLY trunc_f;
--dg-cascade
--dg SELECT * FROM trunc_f;
ROLLBACK;

BEGIN;
--dg-cascade
--dg SELECT * FROM trunc_f;
--dg-cascade
--dg SELECT * FROM trunc_fa;
--dg-cascade
--dg SELECT * FROM trunc_faa;
--dg-cascade
--dg TRUNCATE ONLY trunc_fb, ONLY trunc_fa;
--dg-cascade
--dg SELECT * FROM trunc_f;
--dg-cascade
--dg SELECT * FROM trunc_fa;
--dg-cascade
--dg SELECT * FROM trunc_faa;
ROLLBACK;

BEGIN;
--dg-cascade
--dg SELECT * FROM trunc_f;
--dg-cascade
--dg SELECT * FROM trunc_fa;
--dg-cascade
--dg SELECT * FROM trunc_faa;
--dg-cascade
--dg TRUNCATE ONLY trunc_fb, trunc_fa;
--dg-cascade
--dg SELECT * FROM trunc_f;
--dg-cascade
--dg SELECT * FROM trunc_fa;
--dg-cascade
--dg SELECT * FROM trunc_faa;
ROLLBACK;

--dg-cascade
--dg DROP TABLE trunc_f CASCADE;

-- Test ON TRUNCATE triggers

CREATE TABLE trunc_trigger_test (f1 int, f2 text, f3 text);
CREATE TABLE trunc_trigger_log (tgop text, tglevel text, tgwhen text,
        tgargv text, tgtable name, rowcount bigint);

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION trunctrigger() RETURNS trigger as $$
--dg declare c bigint;
--dg begin
--dg     execute 'select count(*) from ' || quote_ident(tg_table_name) into c;
--dg     insert into trunc_trigger_log values
--dg       (TG_OP, TG_LEVEL, TG_WHEN, TG_ARGV[0], tg_table_name, c);
--dg     return null;
--dg end;
--dg $$ LANGUAGE plpgsql;

-- basic before trigger
INSERT INTO trunc_trigger_test VALUES(1, 'foo', 'bar'), (2, 'baz', 'quux');

--dg-cascade
--dg CREATE TRIGGER t
--dg BEFORE TRUNCATE ON trunc_trigger_test
--dg FOR EACH STATEMENT
--dg EXECUTE PROCEDURE trunctrigger('before trigger truncate');

SELECT count(*) as "Row count in test table" FROM trunc_trigger_test;
SELECT * FROM trunc_trigger_log;
TRUNCATE trunc_trigger_test;
SELECT count(*) as "Row count in test table" FROM trunc_trigger_test;
--dg-cascade
--dg SELECT * FROM trunc_trigger_log;

--dg-cascade
--dg DROP TRIGGER t ON trunc_trigger_test;

truncate trunc_trigger_log;

-- same test with an after trigger
INSERT INTO trunc_trigger_test VALUES(1, 'foo', 'bar'), (2, 'baz', 'quux');

--dg-cascade
--dg CREATE TRIGGER tt
--dg AFTER TRUNCATE ON trunc_trigger_test
--dg FOR EACH STATEMENT
--dg EXECUTE PROCEDURE trunctrigger('after trigger truncate');

SELECT count(*) as "Row count in test table" FROM trunc_trigger_test;
SELECT * FROM trunc_trigger_log;
TRUNCATE trunc_trigger_test;
SELECT count(*) as "Row count in test table" FROM trunc_trigger_test;
--dg-cascade
--dg SELECT * FROM trunc_trigger_log;

DROP TABLE trunc_trigger_test;
DROP TABLE trunc_trigger_log;

--dg-cascade
--dg DROP FUNCTION trunctrigger();

-- test TRUNCATE ... RESTART IDENTITY
CREATE SEQUENCE truncate_a_id1 START WITH 33;
CREATE TABLE truncate_a (id serial,
                         id1 integer default nextval('truncate_a_id1'));
ALTER SEQUENCE truncate_a_id1 OWNED BY truncate_a.id1;

INSERT INTO truncate_a DEFAULT VALUES;
INSERT INTO truncate_a DEFAULT VALUES;
SELECT * FROM truncate_a;

TRUNCATE truncate_a;

INSERT INTO truncate_a DEFAULT VALUES;
INSERT INTO truncate_a DEFAULT VALUES;
SELECT * FROM truncate_a;

--dg-skip: sequences are not supported yet
--dg TRUNCATE truncate_a RESTART IDENTITY;

INSERT INTO truncate_a DEFAULT VALUES;
INSERT INTO truncate_a DEFAULT VALUES;
--dg-cascade
--dg SELECT * FROM truncate_a;

CREATE TABLE truncate_b (id int GENERATED ALWAYS AS IDENTITY (START WITH 44));

INSERT INTO truncate_b DEFAULT VALUES;
INSERT INTO truncate_b DEFAULT VALUES;
SELECT * FROM truncate_b;

TRUNCATE truncate_b;

INSERT INTO truncate_b DEFAULT VALUES;
INSERT INTO truncate_b DEFAULT VALUES;
SELECT * FROM truncate_b;

--dg-skip: sequences are not supported yet
--dg TRUNCATE truncate_b RESTART IDENTITY;

INSERT INTO truncate_b DEFAULT VALUES;
INSERT INTO truncate_b DEFAULT VALUES;
--dg-cascade
--dg SELECT * FROM truncate_b;

-- check rollback of a RESTART IDENTITY operation
BEGIN;
--dg-skip: sequences are not supported yet
--dg TRUNCATE truncate_a RESTART IDENTITY;
INSERT INTO truncate_a DEFAULT VALUES;
--dg-cascade
--dg SELECT * FROM truncate_a;
ROLLBACK;
INSERT INTO truncate_a DEFAULT VALUES;
INSERT INTO truncate_a DEFAULT VALUES;
--dg-cascade
--dg SELECT * FROM truncate_a;

DROP TABLE truncate_a;

SELECT nextval('truncate_a_id1'); -- fail, seq should have been dropped

-- partitioned table
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE truncparted (a int, b char) PARTITION BY LIST (a);
-- error, can't truncate a partitioned table
--dg-cascade
--dg TRUNCATE ONLY truncparted;
--dg-cascade
--dg CREATE TABLE truncparted1 PARTITION OF truncparted FOR VALUES IN (1);
--dg-cascade
--dg INSERT INTO truncparted VALUES (1, 'a');
-- error, must truncate partitions
--dg-cascade
--dg TRUNCATE ONLY truncparted;
--dg-cascade
--dg TRUNCATE truncparted;
--dg-cascade
--dg DROP TABLE truncparted;

-- foreign key on partitioned table: partition key is referencing column.
-- Make sure truncate did execute on all tables
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION tp_ins_data() RETURNS void LANGUAGE plpgsql AS $$
--dg   BEGIN
--dg 	INSERT INTO truncprim VALUES (1), (100), (150);
--dg 	INSERT INTO truncpart VALUES (1), (100), (150);
--dg   END
--dg $$;
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION tp_chk_data(OUT pktb regclass, OUT pkval int, OUT fktb regclass, OUT fkval int)
--dg   RETURNS SETOF record LANGUAGE plpgsql AS $$
--dg   BEGIN
--dg     RETURN QUERY SELECT
--dg       pk.tableoid::regclass, pk.a, fk.tableoid::regclass, fk.a
--dg     FROM truncprim pk FULL JOIN truncpart fk USING (a)
--dg     ORDER BY 2, 4;
--dg   END
--dg $$;
CREATE TABLE truncprim (a int PRIMARY KEY);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE truncpart (a int REFERENCES truncprim)
--dg   PARTITION BY RANGE (a);
--dg-cascade
--dg CREATE TABLE truncpart_1 PARTITION OF truncpart FOR VALUES FROM (0) TO (100);
--dg-cascade
--dg CREATE TABLE truncpart_2 PARTITION OF truncpart FOR VALUES FROM (100) TO (200)
--dg   PARTITION BY RANGE (a);
--dg-cascade
--dg CREATE TABLE truncpart_2_1 PARTITION OF truncpart_2 FOR VALUES FROM (100) TO (150);
--dg-cascade
--dg CREATE TABLE truncpart_2_d PARTITION OF truncpart_2 DEFAULT;

--dg-cascade
--dg TRUNCATE TABLE truncprim;	-- should fail

--dg-cascade
--dg select tp_ins_data();
-- should truncate everything
--dg-cascade
--dg TRUNCATE TABLE truncprim, truncpart;
--dg-cascade
--dg select * from tp_chk_data();

--dg-cascade
--dg select tp_ins_data();
-- should truncate everything
--dg-cascade
--dg TRUNCATE TABLE truncprim CASCADE;
--dg-cascade
--dg SELECT * FROM tp_chk_data();

--dg-cascade
--dg SELECT tp_ins_data();
-- should truncate all partitions
--dg-cascade
--dg TRUNCATE TABLE truncpart;
--dg-cascade
--dg SELECT * FROM tp_chk_data();
--dg-cascade
--dg DROP TABLE truncprim, truncpart;
--dg-cascade
--dg DROP FUNCTION tp_ins_data(), tp_chk_data();

-- test cascade when referencing a partitioned table
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE trunc_a (a INT PRIMARY KEY) PARTITION BY RANGE (a);
--dg-cascade
--dg CREATE TABLE trunc_a1 PARTITION OF trunc_a FOR VALUES FROM (0) TO (10);
--dg-cascade
--dg CREATE TABLE trunc_a2 PARTITION OF trunc_a FOR VALUES FROM (10) TO (20)
--dg   PARTITION BY RANGE (a);
--dg-cascade
--dg CREATE TABLE trunc_a21 PARTITION OF trunc_a2 FOR VALUES FROM (10) TO (12);
--dg-cascade
--dg CREATE TABLE trunc_a22 PARTITION OF trunc_a2 FOR VALUES FROM (12) TO (16);
--dg-cascade
--dg CREATE TABLE trunc_a2d PARTITION OF trunc_a2 DEFAULT;
--dg-cascade
--dg CREATE TABLE trunc_a3 PARTITION OF trunc_a FOR VALUES FROM (20) TO (30);
--dg-cascade
--dg INSERT INTO trunc_a VALUES (0), (5), (10), (15), (20), (25);

-- truncate a partition cascading to a table
--dg-cascade
--dg CREATE TABLE ref_b (
--dg     b INT PRIMARY KEY,
--dg     a INT REFERENCES trunc_a(a) ON DELETE CASCADE
--dg );
--dg-cascade
--dg INSERT INTO ref_b VALUES (10, 0), (50, 5), (100, 10), (150, 15);

--dg-cascade
--dg TRUNCATE TABLE trunc_a1 CASCADE;
--dg-cascade
--dg SELECT a FROM ref_b;

--dg-cascade
--dg DROP TABLE ref_b;

-- truncate a partition cascading to a partitioned table
--dg-cascade
--dg CREATE TABLE ref_c (
--dg     c INT PRIMARY KEY,
--dg     a INT REFERENCES trunc_a(a) ON DELETE CASCADE
--dg ) PARTITION BY RANGE (c);
--dg-cascade
--dg CREATE TABLE ref_c1 PARTITION OF ref_c FOR VALUES FROM (100) TO (200);
--dg-cascade
--dg CREATE TABLE ref_c2 PARTITION OF ref_c FOR VALUES FROM (200) TO (300);
--dg-cascade
--dg INSERT INTO ref_c VALUES (100, 10), (150, 15), (200, 20), (250, 25);

--dg-cascade
--dg TRUNCATE TABLE trunc_a21 CASCADE;
--dg-cascade
--dg SELECT a as "from table ref_c" FROM ref_c;
--dg-cascade
--dg SELECT a as "from table trunc_a" FROM trunc_a ORDER BY a;

--dg-cascade
--dg DROP TABLE trunc_a, ref_c;
