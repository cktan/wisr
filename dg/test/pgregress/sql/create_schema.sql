--
-- CREATE_SCHEMA
--

-- Schema creation with elements.

--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_create_schema_role SUPERUSER;

-- Cases where schema creation fails as objects are qualified with a schema
-- that does not match with what's expected.
-- This checks all the object types that include schema qualifications.
--dg-cascade
--dg CREATE SCHEMA AUTHORIZATION regress_create_schema_role
--dg   CREATE SEQUENCE schema_not_existing.seq;
--dg-cascade
--dg CREATE SCHEMA AUTHORIZATION regress_create_schema_role
--dg   CREATE TABLE schema_not_existing.tab (id int);
--dg-cascade
--dg CREATE SCHEMA AUTHORIZATION regress_create_schema_role
--dg   CREATE VIEW schema_not_existing.view AS SELECT 1;
--dg-cascade
--dg CREATE SCHEMA AUTHORIZATION regress_create_schema_role
--dg   CREATE INDEX ON schema_not_existing.tab (id);
--dg-cascade
--dg CREATE SCHEMA AUTHORIZATION regress_create_schema_role
--dg   CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
--dg   EXECUTE FUNCTION schema_trig.no_func();
-- Again, with a role specification and no schema names.
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET ROLE regress_create_schema_role;
--dg-cascade
--dg CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
--dg   CREATE SEQUENCE schema_not_existing.seq;
--dg-cascade
--dg CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
--dg   CREATE TABLE schema_not_existing.tab (id int);
--dg-cascade
--dg CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
--dg   CREATE VIEW schema_not_existing.view AS SELECT 1;
--dg-cascade
--dg CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
--dg   CREATE INDEX ON schema_not_existing.tab (id);
--dg-cascade
--dg CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
--dg   CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
--dg   EXECUTE FUNCTION schema_trig.no_func();
-- Again, with a schema name and a role specification.
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
RESET ROLE;

-- Cases where the schema creation succeeds.
-- The schema created matches the role name.
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE SCHEMA AUTHORIZATION regress_create_schema_role
--dg   CREATE TABLE regress_create_schema_role.tab (id int);
--dg-cascade
--dg \d regress_create_schema_role.tab
--dg-cascade
--dg DROP SCHEMA regress_create_schema_role CASCADE;
-- Again, with a different role specification and no schema names.
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET ROLE regress_create_schema_role;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
--dg   CREATE TABLE regress_create_schema_role.tab (id int);
--dg-cascade
--dg \d regress_create_schema_role.tab
--dg-cascade
--dg DROP SCHEMA regress_create_schema_role CASCADE;
-- Again, with a schema name and a role specification.
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TABLE regress_schema_1.tab (id int);
\d regress_schema_1.tab
DROP SCHEMA regress_schema_1 CASCADE;
RESET ROLE;

-- Clean up
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_create_schema_role;
