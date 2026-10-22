--
-- Tests for password types
--

-- Tests for GUC password_encryption
SET password_encryption = 'novalue'; -- error
SET password_encryption = true; -- error
SET password_encryption = 'md5'; -- ok
SET password_encryption = 'scram-sha-256'; -- ok

-- consistency of password entries
SET password_encryption = 'md5';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd1;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_passwd1 PASSWORD 'role_pwd1';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd2;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_passwd2 PASSWORD 'role_pwd2';
SET password_encryption = 'scram-sha-256';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd3 PASSWORD 'role_pwd3';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd4 PASSWORD NULL;

-- check list of created entries
--
-- The scram secret will look something like:
-- SCRAM-SHA-256$4096:E4HxLGtnRzsYwg==$6YtlR4t69SguDiwFvbVgVZtuz6gpJQQqUMZ7IQJK5yI=:ps75jrHeYU4lXCcXI4O8oIdJ3eO8o2jirjruw9phBTo=
--
-- Since the salt is random, the exact value stored will be different on every test
-- run. Use a regular expression to mask the changing parts.
--dg-cascade
--dg SELECT rolname, regexp_replace(rolpassword, '(SCRAM-SHA-256)\$(\d+):([a-zA-Z0-9+/=]+)\$([a-zA-Z0-9+=/]+):([a-zA-Z0-9+/=]+)', '\1$\2:<salt>$<storedkey>:<serverkey>') as rolpassword_masked
--dg     FROM pg_authid
--dg     WHERE rolname LIKE 'regress_passwd%'
--dg     ORDER BY rolname, rolpassword;

-- Rename a role
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_passwd2 RENAME TO regress_passwd2_new;
-- md5 entry should have been removed
--dg-cascade
--dg SELECT rolname, rolpassword
--dg     FROM pg_authid
--dg     WHERE rolname LIKE 'regress_passwd2_new'
--dg     ORDER BY rolname, rolpassword;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_passwd2_new RENAME TO regress_passwd2;

-- Change passwords with ALTER USER. With plaintext or already-encrypted
-- passwords.
SET password_encryption = 'md5';

-- encrypt with MD5
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_passwd2 PASSWORD 'foo';
-- already encrypted, use as they are
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_passwd1 PASSWORD 'md5cd3578025fe2c3d7ed1b9a9b26238b70';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_passwd3 PASSWORD 'SCRAM-SHA-256$4096:VLK4RMaQLCvNtQ==$6YtlR4t69SguDiwFvbVgVZtuz6gpJQQqUMZ7IQJK5yI=:ps75jrHeYU4lXCcXI4O8oIdJ3eO8o2jirjruw9phBTo=';

SET password_encryption = 'scram-sha-256';
-- create SCRAM secret
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE  regress_passwd4 PASSWORD 'foo';
-- already encrypted with MD5, use as it is
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd5 PASSWORD 'md5e73a4b11df52a6068f8b39f90be36023';

-- This looks like a valid SCRAM-SHA-256 secret, but it is not
-- so it should be hashed with SCRAM-SHA-256.
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd6 PASSWORD 'SCRAM-SHA-256$1234';
-- These may look like valid MD5 secrets, but they are not, so they
-- should be hashed with SCRAM-SHA-256.
-- trailing garbage at the end
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd7 PASSWORD 'md5012345678901234567890123456789zz';
-- invalid length
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd8 PASSWORD 'md501234567890123456789012345678901zz';

-- Changing the SCRAM iteration count
SET scram_iterations = 1024;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd9 PASSWORD 'alterediterationcount';

--dg-cascade
--dg SELECT rolname, regexp_replace(rolpassword, '(SCRAM-SHA-256)\$(\d+):([a-zA-Z0-9+/=]+)\$([a-zA-Z0-9+=/]+):([a-zA-Z0-9+/=]+)', '\1$\2:<salt>$<storedkey>:<serverkey>') as rolpassword_masked
--dg     FROM pg_authid
--dg     WHERE rolname LIKE 'regress_passwd%'
--dg     ORDER BY rolname, rolpassword;

-- An empty password is not allowed, in any form
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd_empty PASSWORD '';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_passwd_empty PASSWORD 'md585939a5ce845f1a1b620742e3c659e0a';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_passwd_empty PASSWORD 'SCRAM-SHA-256$4096:hpFyHTUsSWcR7O9P$LgZFIt6Oqdo27ZFKbZ2nV+vtnYM995pDh9ca6WSi120=:qVV5NeluNfUPkwm7Vqat25RjSPLkGeoZBQs6wVv+um4=';
--dg-cascade
--dg SELECT rolpassword FROM pg_authid WHERE rolname='regress_passwd_empty';

-- Test with invalid stored and server keys.
--
-- The first is valid, to act as a control. The others have too long
-- stored/server keys. They will be re-hashed.
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd_sha_len0 PASSWORD 'SCRAM-SHA-256$4096:A6xHKoH/494E941doaPOYg==$Ky+A30sewHIH3VHQLRN9vYsuzlgNyGNKCh37dy96Rqw=:COPdlNiIkrsacU5QoxydEuOH6e/KfiipeETb/bPw8ZI=';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd_sha_len1 PASSWORD 'SCRAM-SHA-256$4096:A6xHKoH/494E941doaPOYg==$Ky+A30sewHIH3VHQLRN9vYsuzlgNyGNKCh37dy96RqwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=:COPdlNiIkrsacU5QoxydEuOH6e/KfiipeETb/bPw8ZI=';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd_sha_len2 PASSWORD 'SCRAM-SHA-256$4096:A6xHKoH/494E941doaPOYg==$Ky+A30sewHIH3VHQLRN9vYsuzlgNyGNKCh37dy96Rqw=:COPdlNiIkrsacU5QoxydEuOH6e/KfiipeETb/bPw8ZIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=';

-- Check that the invalid secrets were re-hashed. A re-hashed secret
-- should not contain the original salt.
--dg-cascade
--dg SELECT rolname, rolpassword not like '%A6xHKoH/494E941doaPOYg==%' as is_rolpassword_rehashed
--dg     FROM pg_authid
--dg     WHERE rolname LIKE 'regress_passwd_sha_len%'
--dg     ORDER BY rolname;

-- Test that valid hashes that are too long are rejected
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_passwd10 PASSWORD 'SCRAM-SHA-256$000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004096:wNFxNSk1hAXBkgub8py3bg==$65zC6E+R0U7tiYTC9+Wtq4Thw6gUDj3eDCINij8TflU=:rC1I7tcVugrHEY2DT0iPjGyjM4aJxkMM9n8WBxtUtHU=';
--dg-cascade
--dg ALTER ROLE regress_passwd9 PASSWORD 'SCRAM-SHA-256$000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004096:wNFxNSk1hAXBkgub8py3bg==$65zC6E+R0U7tiYTC9+Wtq4Thw6gUDj3eDCINij8TflU=:rC1I7tcVugrHEY2DT0iPjGyjM4aJxkMM9n8WBxtUtHU=';

--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd1;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd2;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd3;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd4;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd5;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd6;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd7;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd8;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd9;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd_empty;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd_sha_len0;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd_sha_len1;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_passwd_sha_len2;

-- all entries should have been removed
--dg-skip: relation "?" does not exist
--dg SELECT rolname, rolpassword
--dg     FROM pg_authid
--dg     WHERE rolname LIKE 'regress_passwd%'
--dg     ORDER BY rolname, rolpassword;
