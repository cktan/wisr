\set HIDE_TOAST_COMPRESSION false

-- ensure we get stable results regardless of installation's default
SET default_toast_compression = 'pglz';

-- test creating table with compression method
CREATE TABLE cmdata(f1 text COMPRESSION pglz);
CREATE INDEX idx ON cmdata(f1);
INSERT INTO cmdata VALUES(repeat('1234567890', 1000));
--dg-differs: the result
--dg \d+ cmdata
--dg-differs: no error where PostgreSQL has: compression method lz4 not supported
--dg CREATE TABLE cmdata1(f1 TEXT COMPRESSION lz4);
INSERT INTO cmdata1 VALUES(repeat('1234567890', 1004));
\d+ cmdata1

-- verify stored compression method in the data
--dg-differs: the result
--dg SELECT pg_column_compression(f1) FROM cmdata;
SELECT pg_column_compression(f1) FROM cmdata1;

-- decompress data slice
SELECT SUBSTR(f1, 200, 5) FROM cmdata;
SELECT SUBSTR(f1, 2000, 50) FROM cmdata1;

-- copy with table creation
SELECT * INTO cmmove1 FROM cmdata;
\d+ cmmove1
--dg-differs: the result
--dg SELECT pg_column_compression(f1) FROM cmmove1;

-- copy to existing table
CREATE TABLE cmmove3(f1 text COMPRESSION pglz);
INSERT INTO cmmove3 SELECT * FROM cmdata;
INSERT INTO cmmove3 SELECT * FROM cmdata1;
--dg-differs: the result
--dg SELECT pg_column_compression(f1) FROM cmmove3;

-- test LIKE INCLUDING COMPRESSION
CREATE TABLE cmdata2 (LIKE cmdata1 INCLUDING COMPRESSION);
\d+ cmdata2
DROP TABLE cmdata2;

-- try setting compression for incompressible data type
--dg-differs: no error where PostgreSQL has: column data type integer does not support compression
--dg CREATE TABLE cmdata2 (f1 int COMPRESSION pglz);

-- update using datum from different table
CREATE TABLE cmmove2(f1 text COMPRESSION pglz);
INSERT INTO cmmove2 VALUES (repeat('1234567890', 1004));
--dg-differs: the result
--dg SELECT pg_column_compression(f1) FROM cmmove2;
UPDATE cmmove2 SET f1 = cmdata1.f1 FROM cmdata1;
--dg-differs: the result
--dg SELECT pg_column_compression(f1) FROM cmmove2;

-- test externally stored compressed data
--dg-differs: another error: language "?" does not exist
--dg CREATE OR REPLACE FUNCTION large_val() RETURNS TEXT LANGUAGE SQL AS
--dg 'select array_agg(fipshash(g::text))::text from generate_series(1, 256) g';
CREATE TABLE cmdata2 (f1 text COMPRESSION pglz);
INSERT INTO cmdata2 SELECT large_val() || repeat('a', 4000);
SELECT pg_column_compression(f1) FROM cmdata2;
INSERT INTO cmdata1 SELECT large_val() || repeat('a', 4000);
SELECT pg_column_compression(f1) FROM cmdata1;
SELECT SUBSTR(f1, 200, 5) FROM cmdata1;
SELECT SUBSTR(f1, 200, 5) FROM cmdata2;
DROP TABLE cmdata2;

--test column type update varlena/non-varlena
CREATE TABLE cmdata2 (f1 int);
\d+ cmdata2
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE cmdata2 ALTER COLUMN f1 TYPE varchar;
--dg-cascade
--dg \d+ cmdata2
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE cmdata2 ALTER COLUMN f1 TYPE int USING f1::integer;
\d+ cmdata2

--changing column storage should not impact the compression method
--but the data should not be compressed
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE cmdata2 ALTER COLUMN f1 TYPE varchar;
--dg-cascade
--dg ALTER TABLE cmdata2 ALTER COLUMN f1 SET COMPRESSION pglz;
--dg-cascade
--dg \d+ cmdata2
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE cmdata2 ALTER COLUMN f1 SET STORAGE plain;
--dg-cascade
--dg \d+ cmdata2
--dg-cascade
--dg INSERT INTO cmdata2 VALUES (repeat('123456789', 800));
--dg-cascade
--dg SELECT pg_column_compression(f1) FROM cmdata2;

-- test compression with materialized view
CREATE MATERIALIZED VIEW compressmv(x) AS SELECT * FROM cmdata1;
\d+ compressmv
SELECT pg_column_compression(f1) FROM cmdata1;
SELECT pg_column_compression(x) FROM compressmv;

-- test compression with partition
--dg-differs: another error: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE cmpart(f1 text COMPRESSION lz4) PARTITION BY HASH(f1);
--dg-differs: another error: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE cmpart1 PARTITION OF cmpart FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE cmpart2(f1 text COMPRESSION pglz);

ALTER TABLE cmpart ATTACH PARTITION cmpart2 FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO cmpart VALUES (repeat('123456789', 1004));
INSERT INTO cmpart VALUES (repeat('123456789', 4004));
SELECT pg_column_compression(f1) FROM cmpart1;
SELECT pg_column_compression(f1) FROM cmpart2;

-- test compression with inheritance
--dg-differs: another error: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE cminh() INHERITS(cmdata, cmdata1); -- error
--dg-differs: another error: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE cminh(f1 TEXT COMPRESSION lz4) INHERITS(cmdata); -- error
CREATE TABLE cmdata3(f1 text);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE cminh() INHERITS (cmdata, cmdata3);

-- test default_toast_compression GUC
SET default_toast_compression = '';
SET default_toast_compression = 'I do not exist compression';
SET default_toast_compression = 'lz4';
SET default_toast_compression = 'pglz';

-- test alter compression method
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE cmdata ALTER COLUMN f1 SET COMPRESSION lz4;
INSERT INTO cmdata VALUES (repeat('123456789', 4004));
--dg-cascade
--dg \d+ cmdata
--dg-differs: the result
--dg SELECT pg_column_compression(f1) FROM cmdata;

--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE cmdata2 ALTER COLUMN f1 SET COMPRESSION default;
--dg-cascade
--dg \d+ cmdata2

-- test alter compression method for materialized views
--dg-differs: another error: ALTER MATERIALIZED VIEW is not supported yet
--dg ALTER MATERIALIZED VIEW compressmv ALTER COLUMN x SET COMPRESSION lz4;
\d+ compressmv

-- test alter compression method for partitioned tables
ALTER TABLE cmpart1 ALTER COLUMN f1 SET COMPRESSION pglz;
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE cmpart2 ALTER COLUMN f1 SET COMPRESSION lz4;

-- new data should be compressed with the current compression method
INSERT INTO cmpart VALUES (repeat('123456789', 1004));
INSERT INTO cmpart VALUES (repeat('123456789', 4004));
SELECT pg_column_compression(f1) FROM cmpart1;
SELECT pg_column_compression(f1) FROM cmpart2;

-- VACUUM FULL does not recompress
--dg-differs: the result
--dg SELECT pg_column_compression(f1) FROM cmdata;
VACUUM FULL cmdata;
--dg-differs: the result
--dg SELECT pg_column_compression(f1) FROM cmdata;

-- test expression index
DROP TABLE cmdata2;
--dg-differs: no error where PostgreSQL has: compression method lz4 not supported
--dg CREATE TABLE cmdata2 (f1 TEXT COMPRESSION pglz, f2 TEXT COMPRESSION lz4);
CREATE UNIQUE INDEX idx1 ON cmdata2 ((f1 || f2));
INSERT INTO cmdata2 VALUES((SELECT array_agg(fipshash(g::TEXT))::TEXT FROM
generate_series(1, 50) g), VERSION());

-- check data is ok
SELECT length(f1) FROM cmdata;
SELECT length(f1) FROM cmdata1;
SELECT length(f1) FROM cmmove1;
SELECT length(f1) FROM cmmove2;
SELECT length(f1) FROM cmmove3;

--dg-differs: no error where PostgreSQL has: invalid compression method "?"
--dg CREATE TABLE badcompresstbl (a text COMPRESSION I_Do_Not_Exist_Compression); -- fails
CREATE TABLE badcompresstbl (a text);
--dg-differs: another error: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE badcompresstbl ALTER a SET COMPRESSION I_Do_Not_Exist_Compression; -- fails
DROP TABLE badcompresstbl;

\set HIDE_TOAST_COMPRESSION true
