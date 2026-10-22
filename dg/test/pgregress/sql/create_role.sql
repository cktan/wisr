-- ok, superuser can create users with any set of privileges
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_role_super SUPERUSER;
--dg-skip: the REPLICATION attribute is not supported
--dg CREATE ROLE regress_role_admin CREATEDB CREATEROLE REPLICATION BYPASSRLS;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg GRANT CREATE ON DATABASE regression TO regress_role_admin WITH GRANT OPTION;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_role_limited_admin CREATEROLE;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_role_normal;

-- fail, CREATEROLE user can't give away role attributes without having them
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_role_limited_admin;
--dg-cascade
--dg CREATE ROLE regress_nosuch_superuser SUPERUSER;
--dg-cascade
--dg CREATE ROLE regress_nosuch_replication_bypassrls REPLICATION BYPASSRLS;
--dg-cascade
--dg CREATE ROLE regress_nosuch_replication REPLICATION;
--dg-cascade
--dg CREATE ROLE regress_nosuch_bypassrls BYPASSRLS;
--dg-cascade
--dg CREATE ROLE regress_nosuch_createdb CREATEDB;

-- ok, can create a role without any special attributes
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_role_limited;

-- fail, can't give it in any of the restricted attributes
--dg-cascade
--dg ALTER ROLE regress_role_limited SUPERUSER;
--dg-cascade
--dg ALTER ROLE regress_role_limited REPLICATION;
--dg-cascade
--dg ALTER ROLE regress_role_limited CREATEDB;
--dg-cascade
--dg ALTER ROLE regress_role_limited BYPASSRLS;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_role_limited;

-- ok, can give away these role attributes if you have them
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_role_admin;
--dg-skip: the REPLICATION attribute is not supported
--dg CREATE ROLE regress_replication_bypassrls REPLICATION BYPASSRLS;
--dg-skip: the REPLICATION attribute is not supported
--dg CREATE ROLE regress_replication REPLICATION;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_bypassrls BYPASSRLS;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_createdb CREATEDB;

-- ok, can toggle these role attributes off and on if you have them
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_replication NOREPLICATION;
--dg-skip: the REPLICATION attribute is not supported
--dg ALTER ROLE regress_replication REPLICATION;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_bypassrls NOBYPASSRLS;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_bypassrls BYPASSRLS;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_createdb NOCREATEDB;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_createdb CREATEDB;

-- fail, can't toggle SUPERUSER
--dg-cascade
--dg ALTER ROLE regress_createdb SUPERUSER;
--dg-cascade
--dg ALTER ROLE regress_createdb NOSUPERUSER;

-- ok, having CREATEROLE is enough to create users with these privileges
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_createrole CREATEROLE NOINHERIT;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg GRANT CREATE ON DATABASE regression TO regress_createrole WITH GRANT OPTION;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_login LOGIN;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_inherit INHERIT;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_connection_limit CONNECTION LIMIT 5;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_encrypted_password ENCRYPTED PASSWORD 'foo';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_password_null PASSWORD NULL;

-- ok, backwards compatible noise words should be ignored
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_noiseword SYSID 12345;

-- fail, cannot grant membership in superuser role
--dg-cascade
--dg CREATE ROLE regress_nosuch_super IN ROLE regress_role_super;

-- fail, database owner cannot have members
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_nosuch_dbowner IN ROLE pg_database_owner;

-- ok, can grant other users into a role
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_inroles ROLE
--dg 	regress_role_super, regress_createdb, regress_createrole, regress_login,
--dg 	regress_inherit, regress_connection_limit, regress_encrypted_password, regress_password_null;

-- fail, cannot grant a role into itself
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_nosuch_recursive ROLE regress_nosuch_recursive;

-- ok, can grant other users into a role with admin option
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_adminroles ADMIN
--dg 	regress_role_super, regress_createdb, regress_createrole, regress_login,
--dg 	regress_inherit, regress_connection_limit, regress_encrypted_password, regress_password_null;

-- fail, cannot grant a role into itself with admin option
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_nosuch_admin_recursive ADMIN regress_nosuch_admin_recursive;

-- fail, regress_createrole does not have CREATEDB privilege
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_createrole;
--dg-cascade
--dg CREATE DATABASE regress_nosuch_db;

-- ok, regress_createrole can create new roles
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_plainrole;

-- ok, roles with CREATEROLE can create new roles with it
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_rolecreator CREATEROLE;

-- ok, roles with CREATEROLE can create new roles with different role
-- attributes, including CREATEROLE
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_hasprivs CREATEROLE LOGIN INHERIT CONNECTION LIMIT 5;

-- ok, we should be able to modify a role we created
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg COMMENT ON ROLE regress_hasprivs IS 'some comment';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT shobj_description('regress_hasprivs'::regrole, 'pg_authid') IS NOT NULL AS has_comment;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg COMMENT ON ROLE regress_hasprivs IS NULL;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT shobj_description('regress_hasprivs'::regrole, 'pg_authid') IS NULL AS no_comment;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg COMMENT ON ROLE regress_hasprivs IS 'add the comment back';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT shobj_description('regress_hasprivs'::regrole, 'pg_authid') IS NOT NULL AS has_comment;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg COMMENT ON ROLE regress_hasprivs IS ''; -- empty string removes the comment, same as NULL
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SELECT shobj_description('regress_hasprivs'::regrole, 'pg_authid') IS NULL AS no_comment;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_hasprivs RENAME TO regress_tenant;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER ROLE regress_tenant NOINHERIT NOLOGIN CONNECTION LIMIT 7;

-- fail, we should be unable to modify a role we did not create
--dg-cascade
--dg COMMENT ON ROLE regress_role_normal IS 'some comment';
--dg-cascade
--dg ALTER ROLE regress_role_normal RENAME TO regress_role_abnormal;
--dg-cascade
--dg ALTER ROLE regress_role_normal NOINHERIT NOLOGIN CONNECTION LIMIT 7;

-- ok, regress_tenant can create objects within the database
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_tenant;
--dg-cascade
--dg CREATE TABLE tenant_table (i integer);
--dg-cascade
--dg CREATE INDEX tenant_idx ON tenant_table(i);
--dg-cascade
--dg CREATE VIEW tenant_view AS SELECT * FROM pg_catalog.pg_class;
--dg-cascade
--dg REVOKE ALL PRIVILEGES ON tenant_table FROM PUBLIC;

-- fail, these objects belonging to regress_tenant
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_createrole;
--dg-cascade
--dg DROP INDEX tenant_idx;
--dg-cascade
--dg ALTER TABLE tenant_table ADD COLUMN t text;
--dg-cascade
--dg DROP TABLE tenant_table;
--dg-cascade
--dg ALTER VIEW tenant_view OWNER TO regress_role_admin;
--dg-cascade
--dg DROP VIEW tenant_view;

-- fail, can't create objects owned as regress_tenant
--dg-cascade
--dg CREATE SCHEMA regress_tenant_schema AUTHORIZATION regress_tenant;

-- fail, we don't inherit permissions from regress_tenant
--dg-cascade
--dg REASSIGN OWNED BY regress_tenant TO regress_createrole;

-- ok, create a role with a value for createrole_self_grant
SET createrole_self_grant = 'set, inherit';
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE ROLE regress_tenant2;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg GRANT CREATE ON DATABASE regression TO regress_tenant2;

-- ok, regress_tenant2 can create objects within the database
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_tenant2;
--dg-cascade
--dg CREATE TABLE tenant2_table (i integer);
--dg-cascade
--dg REVOKE ALL PRIVILEGES ON tenant2_table FROM PUBLIC;

-- ok, because we have SET and INHERIT on regress_tenant2
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_createrole;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg CREATE SCHEMA regress_tenant2_schema AUTHORIZATION regress_tenant2;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER SCHEMA regress_tenant2_schema OWNER TO regress_createrole;
--dg-cascade
--dg ALTER TABLE tenant2_table OWNER TO regress_createrole;
--dg-cascade
--dg ALTER TABLE tenant2_table OWNER TO regress_tenant2;

-- with SET but not INHERIT, we can give away objects but not take them
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg REVOKE INHERIT OPTION FOR regress_tenant2 FROM regress_createrole;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg ALTER SCHEMA regress_tenant2_schema OWNER TO regress_tenant2;
--dg-cascade
--dg ALTER TABLE tenant2_table OWNER TO regress_createrole;

-- with INHERIT but not SET, we can take objects but not give them away
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg GRANT regress_tenant2 TO regress_createrole WITH INHERIT TRUE, SET FALSE;
--dg-cascade
--dg ALTER TABLE tenant2_table OWNER TO regress_createrole;
--dg-cascade
--dg ALTER TABLE tenant2_table OWNER TO regress_tenant2;
--dg-cascade
--dg DROP TABLE tenant2_table;

-- fail, CREATEROLE is not enough to create roles in privileged roles
--dg-cascade
--dg CREATE ROLE regress_read_all_data IN ROLE pg_read_all_data;
--dg-cascade
--dg CREATE ROLE regress_write_all_data IN ROLE pg_write_all_data;
--dg-cascade
--dg CREATE ROLE regress_monitor IN ROLE pg_monitor;
--dg-cascade
--dg CREATE ROLE regress_read_all_settings IN ROLE pg_read_all_settings;
--dg-cascade
--dg CREATE ROLE regress_read_all_stats IN ROLE pg_read_all_stats;
--dg-cascade
--dg CREATE ROLE regress_stat_scan_tables IN ROLE pg_stat_scan_tables;
--dg-cascade
--dg CREATE ROLE regress_read_server_files IN ROLE pg_read_server_files;
--dg-cascade
--dg CREATE ROLE regress_write_server_files IN ROLE pg_write_server_files;
--dg-cascade
--dg CREATE ROLE regress_execute_server_program IN ROLE pg_execute_server_program;
--dg-cascade
--dg CREATE ROLE regress_signal_backend IN ROLE pg_signal_backend;

-- fail, role still owns database objects
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_tenant;

-- fail, creation of these roles failed above so they do not now exist
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg SET SESSION AUTHORIZATION regress_role_admin;
--dg-cascade
--dg DROP ROLE regress_nosuch_superuser;
--dg-cascade
--dg DROP ROLE regress_nosuch_replication_bypassrls;
--dg-cascade
--dg DROP ROLE regress_nosuch_replication;
--dg-cascade
--dg DROP ROLE regress_nosuch_bypassrls;
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_nosuch_super;
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_nosuch_dbowner;
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_nosuch_recursive;
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_nosuch_admin_recursive;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_plainrole;

-- must revoke privileges before dropping role
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg REVOKE CREATE ON DATABASE regression FROM regress_createrole CASCADE;

-- ok, should be able to drop non-superuser roles we created
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_replication_bypassrls;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_replication;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_bypassrls;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_createdb;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_createrole;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_login;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_inherit;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_connection_limit;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_encrypted_password;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_password_null;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_noiseword;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_inroles;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_adminroles;

-- fail, cannot drop ourself, nor superusers or roles we lack ADMIN for
--dg-cascade
--dg DROP ROLE regress_role_super;
--dg-cascade
--dg DROP ROLE regress_role_admin;
--dg-cascade
--dg DROP ROLE regress_rolecreator;

-- ok
RESET SESSION AUTHORIZATION;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg REVOKE CREATE ON DATABASE regression FROM regress_role_admin CASCADE;
DROP INDEX tenant_idx;
DROP TABLE tenant_table;
DROP VIEW tenant_view;
--dg-cascade
--dg DROP SCHEMA regress_tenant2_schema;
-- check for duplicated drop
--dg-differs: another error: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_tenant, regress_tenant;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_tenant2;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_rolecreator;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_role_admin;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_role_limited_admin;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_role_super;
--dg-skip: relation pg_authid cannot be opened: digres has no catalog tables
--dg DROP ROLE regress_role_normal;
