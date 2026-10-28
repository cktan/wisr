--
-- Test large object support
--

-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR
\getenv abs_builddir PG_ABS_BUILDDIR

-- ensure consistent test output regardless of the default bytea format
SET bytea_output TO escape;

-- Test ALTER LARGE OBJECT OWNER
CREATE ROLE regress_lo_user;
--dg-skip: function ?(...) does not exist
--dg SELECT lo_create(42);
--dg-skip: large objects are not supported
--dg ALTER LARGE OBJECT 42 OWNER TO regress_lo_user;

-- Test GRANT, COMMENT as non-superuser
SET SESSION AUTHORIZATION regress_lo_user;

--dg-skip: large objects are not supported
--dg GRANT SELECT ON LARGE OBJECT 42 TO public;
--dg-skip: large objects are not supported
--dg COMMENT ON LARGE OBJECT 42 IS 'the ultimate answer';

RESET SESSION AUTHORIZATION;

-- Test psql's \lo_list et al (we assume no other LOs exist yet)
--dg-cascade
--dg \lo_list
--dg-cascade
--dg \lo_list+
--dg-cascade
--dg \lo_unlink 42
--dg-skip: relation "?" does not exist
--dg \dl

-- Load a file
--dg-skip: type ? is not supported yet
--dg CREATE TABLE lotest_stash_values (loid oid, fd integer);
-- lo_creat(mode integer) returns oid
-- The mode arg to lo_creat is unused, some vestigal holdover from ancient times
-- returns the large object id
--dg-cascade
--dg INSERT INTO lotest_stash_values (loid) SELECT lo_creat(42);

-- NOTE: large objects require transactions
BEGIN;

-- lo_open(lobjId oid, mode integer) returns integer
-- The mode parameter to lo_open uses two constants:
--   INV_WRITE = 0x20000
--   INV_READ  = 0x40000
-- The return value is a file descriptor-like value which remains valid for the
-- transaction.
--dg-cascade
--dg UPDATE lotest_stash_values SET fd = lo_open(loid, CAST(x'20000' | x'40000' AS integer));

-- loread/lowrite names are wonky, different from other functions which are lo_*
-- lowrite(fd integer, data bytea) returns integer
-- the integer is the number of bytes written
--dg-cascade
--dg SELECT lowrite(fd, '
--dg I wandered lonely as a cloud
--dg That floats on high o''er vales and hills,
--dg When all at once I saw a crowd,
--dg A host, of golden daffodils;
--dg Beside the lake, beneath the trees,
--dg Fluttering and dancing in the breeze.
--dg
--dg Continuous as the stars that shine
--dg And twinkle on the milky way,
--dg They stretched in never-ending line
--dg Along the margin of a bay:
--dg Ten thousand saw I at a glance,
--dg Tossing their heads in sprightly dance.
--dg
--dg The waves beside them danced; but they
--dg Out-did the sparkling waves in glee:
--dg A poet could not but be gay,
--dg In such a jocund company:
--dg I gazed--and gazed--but little thought
--dg What wealth the show to me had brought:
--dg
--dg For oft, when on my couch I lie
--dg In vacant or in pensive mood,
--dg They flash upon that inward eye
--dg Which is the bliss of solitude;
--dg And then my heart with pleasure fills,
--dg And dances with the daffodils.
--dg
--dg          -- William Wordsworth
--dg ') FROM lotest_stash_values;

-- lo_close(fd integer) returns integer
-- return value is 0 for success, or <0 for error (actually only -1, but...)
--dg-cascade
--dg SELECT lo_close(fd) FROM lotest_stash_values;

END;

-- Copy to another large object.
-- Note: we intentionally don't remove the object created here;
-- it's left behind to help test pg_dump.

--dg-cascade
--dg SELECT lo_from_bytea(0, lo_get(loid)) AS newloid FROM lotest_stash_values
--dg \gset

-- Add a comment to it, as well, for pg_dump/pg_upgrade testing.
--dg-cascade
--dg COMMENT ON LARGE OBJECT :newloid IS 'I Wandered Lonely as a Cloud';

-- Read out a portion
BEGIN;
--dg-cascade
--dg UPDATE lotest_stash_values SET fd=lo_open(loid, CAST(x'20000' | x'40000' AS integer));

-- lo_lseek(fd integer, offset integer, whence integer) returns integer
-- offset is in bytes, whence is one of three values:
--  SEEK_SET (= 0) meaning relative to beginning
--  SEEK_CUR (= 1) meaning relative to current position
--  SEEK_END (= 2) meaning relative to end (offset better be negative)
-- returns current position in file
--dg-cascade
--dg SELECT lo_lseek(fd, 104, 0) FROM lotest_stash_values;

-- loread/lowrite names are wonky, different from other functions which are lo_*
-- loread(fd integer, len integer) returns bytea
--dg-cascade
--dg SELECT loread(fd, 28) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_lseek(fd, -19, 1) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lowrite(fd, 'n') FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_tell(fd) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_lseek(fd, -744, 2) FROM lotest_stash_values;

--dg-cascade
--dg SELECT loread(fd, 28) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_close(fd) FROM lotest_stash_values;

END;

-- Test resource management
BEGIN;
--dg-cascade
--dg SELECT lo_open(loid, x'40000'::int) from lotest_stash_values;
ABORT;

\set filename :abs_builddir '/results/invalid/path'
\set dobody 'DECLARE loid oid; BEGIN '
\set dobody :dobody 'SELECT tbl.loid INTO loid FROM lotest_stash_values tbl; '
\set dobody :dobody 'PERFORM lo_export(loid, ' :'filename' '); '
\set dobody :dobody 'EXCEPTION WHEN UNDEFINED_FILE THEN '
\set dobody :dobody 'RAISE NOTICE ''could not open file, as expected''; END'
--dg-skip: language "?" does not exist
--dg DO :'dobody';

-- Test truncation.
BEGIN;
--dg-cascade
--dg UPDATE lotest_stash_values SET fd=lo_open(loid, CAST(x'20000' | x'40000' AS integer));

--dg-cascade
--dg SELECT lo_truncate(fd, 11) FROM lotest_stash_values;
--dg-cascade
--dg SELECT loread(fd, 15) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_truncate(fd, 10000) FROM lotest_stash_values;
--dg-cascade
--dg SELECT loread(fd, 10) FROM lotest_stash_values;
--dg-cascade
--dg SELECT lo_lseek(fd, 0, 2) FROM lotest_stash_values;
--dg-cascade
--dg SELECT lo_tell(fd) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_truncate(fd, 5000) FROM lotest_stash_values;
--dg-cascade
--dg SELECT lo_lseek(fd, 0, 2) FROM lotest_stash_values;
--dg-cascade
--dg SELECT lo_tell(fd) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_close(fd) FROM lotest_stash_values;
END;

-- Test 64-bit large object functions.
BEGIN;
--dg-cascade
--dg UPDATE lotest_stash_values SET fd = lo_open(loid, CAST(x'20000' | x'40000' AS integer));

--dg-cascade
--dg SELECT lo_lseek64(fd, 4294967296, 0) FROM lotest_stash_values;
--dg-cascade
--dg SELECT lowrite(fd, 'offset:4GB') FROM lotest_stash_values;
--dg-cascade
--dg SELECT lo_tell64(fd) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_lseek64(fd, -10, 1) FROM lotest_stash_values;
--dg-cascade
--dg SELECT lo_tell64(fd) FROM lotest_stash_values;
--dg-cascade
--dg SELECT loread(fd, 10) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_truncate64(fd, 5000000000) FROM lotest_stash_values;
--dg-cascade
--dg SELECT lo_lseek64(fd, 0, 2) FROM lotest_stash_values;
--dg-cascade
--dg SELECT lo_tell64(fd) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_truncate64(fd, 3000000000) FROM lotest_stash_values;
--dg-cascade
--dg SELECT lo_lseek64(fd, 0, 2) FROM lotest_stash_values;
--dg-cascade
--dg SELECT lo_tell64(fd) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_close(fd) FROM lotest_stash_values;
END;

-- lo_unlink(lobjId oid) returns integer
-- return value appears to always be 1
--dg-cascade
--dg SELECT lo_unlink(loid) from lotest_stash_values;

--dg-cascade
--dg TRUNCATE lotest_stash_values;

\set filename :abs_srcdir '/data/tenk.data'
--dg-cascade
--dg INSERT INTO lotest_stash_values (loid) SELECT lo_import(:'filename');

BEGIN;
--dg-cascade
--dg UPDATE lotest_stash_values SET fd=lo_open(loid, CAST(x'20000' | x'40000' AS integer));

-- verify length of large object
--dg-cascade
--dg SELECT lo_lseek(fd, 0, 2) FROM lotest_stash_values;

-- with the default BLCKSZ, LOBLKSIZE = 2048, so this positions us for a block
-- edge case
--dg-cascade
--dg SELECT lo_lseek(fd, 2030, 0) FROM lotest_stash_values;

-- this should get half of the value from page 0 and half from page 1 of the
-- large object
--dg-cascade
--dg SELECT loread(fd, 36) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_tell(fd) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_lseek(fd, -26, 1) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lowrite(fd, 'abcdefghijklmnop') FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_lseek(fd, 2030, 0) FROM lotest_stash_values;

--dg-cascade
--dg SELECT loread(fd, 36) FROM lotest_stash_values;

--dg-cascade
--dg SELECT lo_close(fd) FROM lotest_stash_values;
END;

\set filename :abs_builddir '/results/lotest.txt'
--dg-cascade
--dg SELECT lo_export(loid, :'filename') FROM lotest_stash_values;

--dg-cascade
--dg \lo_import :filename

\set newloid :LASTOID

-- just make sure \lo_export does not barf
\set filename :abs_builddir '/results/lotest2.txt'
--dg-cascade
--dg \lo_export :newloid :filename

-- This is a hack to test that export/import are reversible
-- This uses knowledge about the inner workings of large object mechanism
-- which should not be used outside it.  This makes it a HACK
--dg-cascade
--dg SELECT pageno, data FROM pg_largeobject WHERE loid = (SELECT loid from lotest_stash_values)
--dg EXCEPT
--dg SELECT pageno, data FROM pg_largeobject WHERE loid = :newloid;

--dg-cascade
--dg SELECT lo_unlink(loid) FROM lotest_stash_values;

--dg-cascade
--dg TRUNCATE lotest_stash_values;

--dg-cascade
--dg \lo_unlink :newloid

\set filename :abs_builddir '/results/lotest.txt'
--dg-cascade
--dg \lo_import :filename

\set newloid_1 :LASTOID

--dg-cascade
--dg SELECT lo_from_bytea(0, lo_get(:newloid_1)) AS newloid_2
--dg \gset

--dg-unstable
--dg SELECT fipshash(lo_get(:newloid_1)) = fipshash(lo_get(:newloid_2));

--dg-cascade
--dg SELECT lo_get(:newloid_1, 0, 20);
--dg-cascade
--dg SELECT lo_get(:newloid_1, 10, 20);
--dg-cascade
--dg SELECT lo_put(:newloid_1, 5, decode('afafafaf', 'hex'));
--dg-cascade
--dg SELECT lo_get(:newloid_1, 0, 20);

--dg-cascade
--dg SELECT lo_put(:newloid_1, 4294967310, 'foo');
--dg-cascade
--dg SELECT lo_get(:newloid_1);
--dg-cascade
--dg SELECT lo_get(:newloid_1, 4294967294, 100);

--dg-cascade
--dg \lo_unlink :newloid_1
--dg-cascade
--dg \lo_unlink :newloid_2

-- This object is left in the database for pg_dump test purposes
--dg-skip: function ?(...) does not exist
--dg SELECT lo_from_bytea(0, E'\\xdeadbeef') AS newloid
--dg \gset

SET bytea_output TO hex;
--dg-cascade
--dg SELECT lo_get(:newloid);

-- Create one more object that we leave behind for testing pg_dump/pg_upgrade;
-- this one intentionally has an OID in the system range
--dg-skip: function ?(...) does not exist
--dg SELECT lo_create(2121);

--dg-skip: large objects are not supported
--dg COMMENT ON LARGE OBJECT 2121 IS 'testing comments';

-- Test writes on large objects in read-only transactions
START TRANSACTION READ ONLY;
-- INV_READ ... ok
--dg-cascade
--dg SELECT lo_open(2121, x'40000'::int);
-- INV_WRITE ... error
--dg-cascade
--dg SELECT lo_open(2121, x'20000'::int);
ROLLBACK;

START TRANSACTION READ ONLY;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT lo_create(42);
ROLLBACK;

START TRANSACTION READ ONLY;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT lo_creat(42);
ROLLBACK;

START TRANSACTION READ ONLY;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT lo_unlink(42);
ROLLBACK;

START TRANSACTION READ ONLY;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT lowrite(42, 'x');
ROLLBACK;

START TRANSACTION READ ONLY;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT lo_import(:'filename');
ROLLBACK;

START TRANSACTION READ ONLY;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT lo_truncate(42, 0);
ROLLBACK;

START TRANSACTION READ ONLY;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT lo_truncate64(42, 0);
ROLLBACK;

START TRANSACTION READ ONLY;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT lo_from_bytea(0, 'x');
ROLLBACK;

START TRANSACTION READ ONLY;
--dg-differs: another error: function ?(...) does not exist
--dg SELECT lo_put(42, 0, 'x');
ROLLBACK;

-- Clean up
--dg-cascade
--dg DROP TABLE lotest_stash_values;

DROP ROLE regress_lo_user;
