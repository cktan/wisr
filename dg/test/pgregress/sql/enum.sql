--
-- Enum tests
--

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE TYPE rainbow AS ENUM ('red', 'orange', 'yellow', 'green', 'blue', 'purple');

--
-- Did it create the right number of rows?
--
--dg-cascade
--dg SELECT COUNT(*) FROM pg_enum WHERE enumtypid = 'rainbow'::regtype;

--
-- I/O functions
--
--dg-cascade
--dg SELECT 'red'::rainbow;
--dg-cascade
--dg SELECT 'mauve'::rainbow;

-- Also try it with non-error-throwing API
--dg-cascade
--dg SELECT pg_input_is_valid('red', 'rainbow');
--dg-cascade
--dg SELECT pg_input_is_valid('mauve', 'rainbow');
--dg-cascade
--dg SELECT * FROM pg_input_error_info('mauve', 'rainbow');
\x
--dg-cascade
--dg SELECT * FROM pg_input_error_info(repeat('too_long', 32), 'rainbow');
\x

--
-- adding new values
--

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE TYPE planets AS ENUM ( 'venus', 'earth', 'mars' );

--dg-cascade
--dg SELECT enumlabel, enumsortorder
--dg FROM pg_enum
--dg WHERE enumtypid = 'planets'::regtype
--dg ORDER BY 2;

--dg-cascade
--dg ALTER TYPE planets ADD VALUE 'uranus';

--dg-cascade
--dg SELECT enumlabel, enumsortorder
--dg FROM pg_enum
--dg WHERE enumtypid = 'planets'::regtype
--dg ORDER BY 2;

--dg-cascade
--dg ALTER TYPE planets ADD VALUE 'mercury' BEFORE 'venus';
--dg-cascade
--dg ALTER TYPE planets ADD VALUE 'saturn' BEFORE 'uranus';
--dg-cascade
--dg ALTER TYPE planets ADD VALUE 'jupiter' AFTER 'mars';
--dg-cascade
--dg ALTER TYPE planets ADD VALUE 'neptune' AFTER 'uranus';

--dg-cascade
--dg SELECT enumlabel, enumsortorder
--dg FROM pg_enum
--dg WHERE enumtypid = 'planets'::regtype
--dg ORDER BY 2;

--dg-cascade
--dg SELECT enumlabel, enumsortorder
--dg FROM pg_enum
--dg WHERE enumtypid = 'planets'::regtype
--dg ORDER BY enumlabel::planets;

-- errors for adding labels
--dg-cascade
--dg ALTER TYPE planets ADD VALUE
--dg   'plutoplutoplutoplutoplutoplutoplutoplutoplutoplutoplutoplutoplutopluto';

--dg-cascade
--dg ALTER TYPE planets ADD VALUE 'pluto' AFTER 'zeus';

-- if not exists tests

--  existing value gives error
--dg-cascade
--dg ALTER TYPE planets ADD VALUE 'mercury';

-- unless IF NOT EXISTS is specified
--dg-cascade
--dg ALTER TYPE planets ADD VALUE IF NOT EXISTS 'mercury';

-- should be neptune, not mercury
--dg-cascade
--dg SELECT enum_last(NULL::planets);

--dg-cascade
--dg ALTER TYPE planets ADD VALUE IF NOT EXISTS 'pluto';

-- should be pluto, i.e. the new value
--dg-cascade
--dg SELECT enum_last(NULL::planets);

--
-- Test inserting so many values that we have to renumber
--

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type insenum as enum ('L1', 'L2');

--dg-cascade
--dg alter type insenum add value 'i1' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i2' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i3' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i4' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i5' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i6' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i7' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i8' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i9' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i10' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i11' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i12' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i13' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i14' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i15' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i16' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i17' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i18' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i19' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i20' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i21' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i22' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i23' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i24' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i25' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i26' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i27' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i28' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i29' before 'L2';
--dg-cascade
--dg alter type insenum add value 'i30' before 'L2';

-- The exact values of enumsortorder will now depend on the local properties
-- of float4, but in any reasonable implementation we should get at least
-- 20 splits before having to renumber; so only hide values > 20.

--dg-cascade
--dg SELECT enumlabel,
--dg        case when enumsortorder > 20 then null else enumsortorder end as so
--dg FROM pg_enum
--dg WHERE enumtypid = 'insenum'::regtype
--dg ORDER BY enumsortorder;

--
-- Basic table creation, row selection
--
--dg-cascade
--dg CREATE TABLE enumtest (col rainbow);
--dg-cascade
--dg INSERT INTO enumtest values ('red'), ('orange'), ('yellow'), ('green');
--dg-cascade
--dg COPY enumtest FROM stdin;
--dg blue
--dg purple
--dg \.
--dg-cascade
--dg SELECT * FROM enumtest;

--
-- Operators, no index
--
--dg-cascade
--dg SELECT * FROM enumtest WHERE col = 'orange';
--dg-cascade
--dg SELECT * FROM enumtest WHERE col <> 'orange' ORDER BY col;
--dg-cascade
--dg SELECT * FROM enumtest WHERE col > 'yellow' ORDER BY col;
--dg-cascade
--dg SELECT * FROM enumtest WHERE col >= 'yellow' ORDER BY col;
--dg-cascade
--dg SELECT * FROM enumtest WHERE col < 'green' ORDER BY col;
--dg-cascade
--dg SELECT * FROM enumtest WHERE col <= 'green' ORDER BY col;

--
-- Cast to/from text
--
--dg-cascade
--dg SELECT 'red'::rainbow::text || 'hithere';
--dg-cascade
--dg SELECT 'red'::text::rainbow = 'red'::rainbow;

--
-- Aggregates
--
--dg-cascade
--dg SELECT min(col) FROM enumtest;
--dg-cascade
--dg SELECT max(col) FROM enumtest;
--dg-cascade
--dg SELECT max(col) FROM enumtest WHERE col < 'green';

--
-- Index tests, force use of index
--
SET enable_seqscan = off;
SET enable_bitmapscan = off;

--
-- Btree index / opclass with the various operators
--
--dg-cascade
--dg CREATE UNIQUE INDEX enumtest_btree ON enumtest USING btree (col);
--dg-cascade
--dg SELECT * FROM enumtest WHERE col = 'orange';
--dg-cascade
--dg SELECT * FROM enumtest WHERE col <> 'orange' ORDER BY col;
--dg-cascade
--dg SELECT * FROM enumtest WHERE col > 'yellow' ORDER BY col;
--dg-cascade
--dg SELECT * FROM enumtest WHERE col >= 'yellow' ORDER BY col;
--dg-cascade
--dg SELECT * FROM enumtest WHERE col < 'green' ORDER BY col;
--dg-cascade
--dg SELECT * FROM enumtest WHERE col <= 'green' ORDER BY col;
--dg-cascade
--dg SELECT min(col) FROM enumtest;
--dg-cascade
--dg SELECT max(col) FROM enumtest;
--dg-cascade
--dg SELECT max(col) FROM enumtest WHERE col < 'green';
--dg-cascade
--dg DROP INDEX enumtest_btree;

--
-- Hash index / opclass with the = operator
--
--dg-cascade
--dg CREATE INDEX enumtest_hash ON enumtest USING hash (col);
--dg-cascade
--dg SELECT * FROM enumtest WHERE col = 'orange';
--dg-cascade
--dg DROP INDEX enumtest_hash;

--
-- End index tests
--
RESET enable_seqscan;
RESET enable_bitmapscan;

--
-- Domains over enums
--
--dg-cascade
--dg CREATE DOMAIN rgb AS rainbow CHECK (VALUE IN ('red', 'green', 'blue'));
--dg-cascade
--dg SELECT 'red'::rgb;
--dg-cascade
--dg SELECT 'purple'::rgb;
--dg-cascade
--dg SELECT 'purple'::rainbow::rgb;
--dg-cascade
--dg DROP DOMAIN rgb;

--
-- Arrays
--
--dg-cascade
--dg SELECT '{red,green,blue}'::rainbow[];
--dg-cascade
--dg SELECT ('{red,green,blue}'::rainbow[])[2];
--dg-cascade
--dg SELECT 'red' = ANY ('{red,green,blue}'::rainbow[]);
--dg-cascade
--dg SELECT 'yellow' = ANY ('{red,green,blue}'::rainbow[]);
--dg-cascade
--dg SELECT 'red' = ALL ('{red,green,blue}'::rainbow[]);
--dg-cascade
--dg SELECT 'red' = ALL ('{red,red}'::rainbow[]);

--
-- Support functions
--
--dg-cascade
--dg SELECT enum_first(NULL::rainbow);
--dg-cascade
--dg SELECT enum_last('green'::rainbow);
--dg-cascade
--dg SELECT enum_range(NULL::rainbow);
--dg-cascade
--dg SELECT enum_range('orange'::rainbow, 'green'::rainbow);
--dg-cascade
--dg SELECT enum_range(NULL, 'green'::rainbow);
--dg-cascade
--dg SELECT enum_range('orange'::rainbow, NULL);
--dg-cascade
--dg SELECT enum_range(NULL::rainbow, NULL);

--
-- User functions, can't test perl/python etc here since may not be compiled.
--
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION echo_me(anyenum) RETURNS text AS $$
--dg BEGIN
--dg RETURN $1::text || 'omg';
--dg END
--dg $$ LANGUAGE plpgsql;
--dg-cascade
--dg SELECT echo_me('red'::rainbow);
--
-- Concrete function should override generic one
--
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION echo_me(rainbow) RETURNS text AS $$
--dg BEGIN
--dg RETURN $1::text || 'wtf';
--dg END
--dg $$ LANGUAGE plpgsql;
--dg-cascade
--dg SELECT echo_me('red'::rainbow);
--
-- If we drop the original generic one, we don't have to qualify the type
-- anymore, since there's only one match
--
--dg-cascade
--dg DROP FUNCTION echo_me(anyenum);
--dg-cascade
--dg SELECT echo_me('red');
--dg-cascade
--dg DROP FUNCTION echo_me(rainbow);

--
-- RI triggers on enum types
--
--dg-cascade
--dg CREATE TABLE enumtest_parent (id rainbow PRIMARY KEY);
--dg-cascade
--dg CREATE TABLE enumtest_child (parent rainbow REFERENCES enumtest_parent);
--dg-cascade
--dg INSERT INTO enumtest_parent VALUES ('red');
--dg-cascade
--dg INSERT INTO enumtest_child VALUES ('red');
--dg-cascade
--dg INSERT INTO enumtest_child VALUES ('blue');  -- fail
--dg-cascade
--dg DELETE FROM enumtest_parent;  -- fail
--
-- cross-type RI should fail
--
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE TYPE bogus AS ENUM('good', 'bad', 'ugly');
--dg-cascade
--dg CREATE TABLE enumtest_bogus_child(parent bogus REFERENCES enumtest_parent);
--dg-cascade
--dg DROP TYPE bogus;

-- check renaming a value
--dg-cascade
--dg ALTER TYPE rainbow RENAME VALUE 'red' TO 'crimson';
--dg-cascade
--dg SELECT enumlabel, enumsortorder
--dg FROM pg_enum
--dg WHERE enumtypid = 'rainbow'::regtype
--dg ORDER BY 2;
-- check that renaming a non-existent value fails
--dg-cascade
--dg ALTER TYPE rainbow RENAME VALUE 'red' TO 'crimson';
-- check that renaming to an existent value fails
--dg-cascade
--dg ALTER TYPE rainbow RENAME VALUE 'blue' TO 'green';

--
-- check transactional behaviour of ALTER TYPE ... ADD VALUE
--
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE TYPE bogus AS ENUM('good');

-- check that we can add new values to existing enums in a transaction
-- but we can't use them
BEGIN;
--dg-cascade
--dg ALTER TYPE bogus ADD VALUE 'new';
--dg-cascade
--dg SAVEPOINT x;
--dg-cascade
--dg SELECT 'new'::bogus;  -- unsafe
--dg-cascade
--dg ROLLBACK TO x;
--dg-cascade
--dg SELECT enum_first(null::bogus);  -- safe
--dg-cascade
--dg SELECT enum_last(null::bogus);  -- unsafe
--dg-cascade
--dg ROLLBACK TO x;
--dg-cascade
--dg SELECT enum_range(null::bogus);  -- unsafe
--dg-cascade
--dg ROLLBACK TO x;
COMMIT;
--dg-cascade
--dg SELECT 'new'::bogus;  -- now safe
--dg-cascade
--dg SELECT enumlabel, enumsortorder
--dg FROM pg_enum
--dg WHERE enumtypid = 'bogus'::regtype
--dg ORDER BY 2;

-- check that we recognize the case where the enum already existed but was
-- modified in the current txn; this should not be considered safe
BEGIN;
--dg-cascade
--dg ALTER TYPE bogus RENAME TO bogon;
--dg-cascade
--dg ALTER TYPE bogon ADD VALUE 'bad';
--dg-cascade
--dg SELECT 'bad'::bogon;
ROLLBACK;

-- but a renamed value is safe to use later in same transaction
BEGIN;
--dg-cascade
--dg ALTER TYPE bogus RENAME VALUE 'good' to 'bad';
--dg-cascade
--dg SELECT 'bad'::bogus;
ROLLBACK;

--dg-cascade
--dg DROP TYPE bogus;

-- check that values created during CREATE TYPE can be used in any case
BEGIN;
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE TYPE bogus AS ENUM('good','bad','ugly');
--dg-cascade
--dg ALTER TYPE bogus RENAME TO bogon;
--dg-cascade
--dg select enum_range(null::bogon);
ROLLBACK;

-- we must allow this usage to support pg_dump in binary upgrade mode
BEGIN;
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE TYPE bogus AS ENUM('good');
--dg-cascade
--dg ALTER TYPE bogus RENAME TO bogon;
--dg-cascade
--dg ALTER TYPE bogon ADD VALUE 'bad';
--dg-cascade
--dg ALTER TYPE bogon ADD VALUE 'ugly';
--dg-cascade
--dg select enum_range(null::bogon);
ROLLBACK;

--
-- Cleanup
--
--dg-cascade
--dg DROP TABLE enumtest_child;
--dg-cascade
--dg DROP TABLE enumtest_parent;
--dg-cascade
--dg DROP TABLE enumtest;
--dg-cascade
--dg DROP TYPE rainbow;

--
-- Verify properly cleaned up
--
SELECT COUNT(*) FROM pg_type WHERE typname = 'rainbow';
--dg-skip: relation "?" does not exist
--dg SELECT * FROM pg_enum WHERE NOT EXISTS
--dg   (SELECT 1 FROM pg_type WHERE pg_type.oid = enumtypid);
