CREATE SCHEMA testxmlschema;

CREATE TABLE testxmlschema.test1 (a int, b text);
INSERT INTO testxmlschema.test1 VALUES (1, 'one'), (2, 'two'), (-1, null);
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE DOMAIN testxmldomain AS varchar;
--dg-cascade
--dg CREATE TABLE testxmlschema.test2 (z int, y varchar(500), x char(6),
--dg     w numeric(9,2), v smallint, u bigint, t real,
--dg     s time, stz timetz, r timestamp, rtz timestamptz, q date,
--dg     p xml, o testxmldomain, n bool, m bytea, aaa text);
--dg-cascade
--dg ALTER TABLE testxmlschema.test2 DROP COLUMN aaa;
--dg-cascade
--dg INSERT INTO testxmlschema.test2 VALUES (55, 'abc', 'def',
--dg     98.6, 2, 999, 0,
--dg     '21:07', '21:11 +05', '2009-06-08 21:07:30', '2009-06-08 21:07:30 -07', '2009-06-08',
--dg     NULL, 'ABC', true, 'XYZ');

SELECT table_to_xml('testxmlschema.test1', false, false, '');
SELECT table_to_xml('testxmlschema.test1', true, false, 'foo');
SELECT table_to_xml('testxmlschema.test1', false, true, '');
SELECT table_to_xml('testxmlschema.test1', true, true, '');
--dg-cascade
--dg SELECT table_to_xml('testxmlschema.test2', false, false, '');

--dg-differs: another error: relation pg_class cannot be opened: digres has no catalog tables
--dg SELECT table_to_xmlschema('testxmlschema.test1', false, false, '');
--dg-differs: another error: relation pg_class cannot be opened: digres has no catalog tables
--dg SELECT table_to_xmlschema('testxmlschema.test1', true, false, '');
--dg-differs: another error: relation pg_class cannot be opened: digres has no catalog tables
--dg SELECT table_to_xmlschema('testxmlschema.test1', false, true, 'foo');
--dg-differs: another error: relation pg_class cannot be opened: digres has no catalog tables
--dg SELECT table_to_xmlschema('testxmlschema.test1', true, true, '');
--dg-cascade
--dg SELECT table_to_xmlschema('testxmlschema.test2', false, false, '');

--dg-differs: another error: relation pg_class cannot be opened: digres has no catalog tables
--dg SELECT table_to_xml_and_xmlschema('testxmlschema.test1', false, false, '');
--dg-differs: another error: relation pg_class cannot be opened: digres has no catalog tables
--dg SELECT table_to_xml_and_xmlschema('testxmlschema.test1', true, false, '');
--dg-differs: another error: relation pg_class cannot be opened: digres has no catalog tables
--dg SELECT table_to_xml_and_xmlschema('testxmlschema.test1', false, true, '');
--dg-differs: another error: relation pg_class cannot be opened: digres has no catalog tables
--dg SELECT table_to_xml_and_xmlschema('testxmlschema.test1', true, true, 'foo');

SELECT query_to_xml('SELECT * FROM testxmlschema.test1', false, false, '');
SELECT query_to_xmlschema('SELECT * FROM testxmlschema.test1', false, false, '');
SELECT query_to_xml_and_xmlschema('SELECT * FROM testxmlschema.test1', true, true, '');

DECLARE xc CURSOR WITH HOLD FOR SELECT * FROM testxmlschema.test1 ORDER BY 1, 2;
SELECT cursor_to_xml('xc'::refcursor, 5, false, true, '');
SELECT cursor_to_xmlschema('xc'::refcursor, false, true, '');
MOVE BACKWARD ALL IN xc;
SELECT cursor_to_xml('xc'::refcursor, 5, true, false, '');
SELECT cursor_to_xmlschema('xc'::refcursor, true, false, '');

SELECT schema_to_xml('testxmlschema', false, true, '');
SELECT schema_to_xml('testxmlschema', true, false, '');
SELECT schema_to_xmlschema('testxmlschema', false, true, '');
SELECT schema_to_xmlschema('testxmlschema', true, false, '');
SELECT schema_to_xml_and_xmlschema('testxmlschema', true, true, 'foo');


-- test that domains are transformed like their base types

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE DOMAIN testboolxmldomain AS bool;
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE DOMAIN testdatexmldomain AS date;

--dg-cascade
--dg CREATE TABLE testxmlschema.test3
--dg     AS SELECT true c1,
--dg               true::testboolxmldomain c2,
--dg               '2013-02-21'::date c3,
--dg               '2013-02-21'::testdatexmldomain c4;

--dg-cascade
--dg SELECT xmlforest(c1, c2, c3, c4) FROM testxmlschema.test3;
--dg-cascade
--dg SELECT table_to_xml('testxmlschema.test3', true, true, '');
