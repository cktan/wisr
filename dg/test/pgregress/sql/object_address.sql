--
-- Test for pg_get_object_address
--

-- Clean up in case a prior regression run failed
SET client_min_messages TO 'warning';
DROP ROLE IF EXISTS regress_addr_user;
RESET client_min_messages;

CREATE USER regress_addr_user;

-- Test generic object addressing/identification functions
CREATE SCHEMA addr_nsp;
SET search_path TO 'addr_nsp';
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg CREATE FOREIGN DATA WRAPPER addr_fdw;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER addr_fserv FOREIGN DATA WRAPPER addr_fdw;
--dg-skip: text search is not supported
--dg CREATE TEXT SEARCH DICTIONARY addr_ts_dict (template=simple);
--dg-skip: text search is not supported
--dg CREATE TEXT SEARCH CONFIGURATION addr_ts_conf (copy=english);
--dg-skip: text search is not supported
--dg CREATE TEXT SEARCH TEMPLATE addr_ts_temp (lexize=dsimple_lexize);
--dg-skip: text search is not supported
--dg CREATE TEXT SEARCH PARSER addr_ts_prs
--dg     (start = prsd_start, gettoken = prsd_nexttoken, end = prsd_end, lextypes = prsd_lextype);
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE addr_nsp.gentable (
--dg     a serial primary key CONSTRAINT a_chk CHECK (a > 0),
--dg     b text DEFAULT 'hello'
--dg );
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE addr_nsp.parttable (
--dg     a int PRIMARY KEY
--dg ) PARTITION BY RANGE (a);
--dg-cascade
--dg CREATE VIEW addr_nsp.genview AS SELECT * from addr_nsp.gentable;
--dg-cascade
--dg CREATE MATERIALIZED VIEW addr_nsp.genmatview AS SELECT * FROM addr_nsp.gentable;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE addr_nsp.gencomptype AS (a int);
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE TYPE addr_nsp.genenum AS ENUM ('one', 'two');
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE FOREIGN TABLE addr_nsp.genftable (a int) SERVER addr_fserv;
--dg-skip: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg CREATE AGGREGATE addr_nsp.genaggr(int4) (sfunc = int4pl, stype = int4);
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE DOMAIN addr_nsp.gendomain AS int4 CONSTRAINT domconstr CHECK (value > 0);
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION addr_nsp.trig() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN END; $$;
--dg-cascade
--dg CREATE TRIGGER t BEFORE INSERT ON addr_nsp.gentable FOR EACH ROW EXECUTE PROCEDURE addr_nsp.trig();
--dg-cascade
--dg CREATE POLICY genpol ON addr_nsp.gentable;
--dg-skip: language "?" does not exist
--dg CREATE PROCEDURE addr_nsp.proc(int4) LANGUAGE SQL AS $$ $$;
--dg-skip: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg CREATE SERVER "integer" FOREIGN DATA WRAPPER addr_fdw;
--dg-skip: relation pg_user_mapping cannot be opened: digres has no catalog tables
--dg CREATE USER MAPPING FOR regress_addr_user SERVER "integer";
--dg-skip: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg ALTER DEFAULT PRIVILEGES FOR ROLE regress_addr_user IN SCHEMA public GRANT ALL ON TABLES TO regress_addr_user;
--dg-skip: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg ALTER DEFAULT PRIVILEGES FOR ROLE regress_addr_user REVOKE DELETE ON TABLES FROM regress_addr_user;
-- this transform would be quite unsafe to leave lying around,
-- except that the SQL language pays no attention to transforms:
--dg-skip: language "?" does not exist
--dg CREATE TRANSFORM FOR int LANGUAGE SQL (
--dg     FROM SQL WITH FUNCTION prsd_lextype(internal),
--dg     TO SQL WITH FUNCTION int4recv(internal));
-- make a function that uses it too, mainly to exercise pg_dump
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION public.sql_func_with_transform(int) RETURNS int LANGUAGE sql
--dg AS 'select $1 + 1'
--dg TRANSFORM FOR TYPE int;
-- suppress warning that depends on wal_level
SET client_min_messages = 'ERROR';
--dg-skip: publications are not supported
--dg CREATE PUBLICATION addr_pub FOR TABLE addr_nsp.gentable;
--dg-skip: publications are not supported
--dg CREATE PUBLICATION addr_pub_schema FOR TABLES IN SCHEMA addr_nsp;
RESET client_min_messages;
--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_addr_sub CONNECTION '' PUBLICATION bar WITH (connect = false, slot_name = NONE);
--dg-cascade
--dg CREATE STATISTICS addr_nsp.gentable_stat ON a, b FROM addr_nsp.gentable;

-- test some error cases
SELECT pg_get_object_address('stone', '{}', '{}');
SELECT pg_get_object_address('table', '{}', '{}');
SELECT pg_get_object_address('table', '{NULL}', '{}');

-- unrecognized object types
--dg-skip: language "?" does not exist
--dg DO $$
--dg DECLARE
--dg     objtype text;
--dg BEGIN
--dg     FOR objtype IN VALUES ('toast table'), ('index column'), ('sequence column'),
--dg         ('toast table column'), ('view column'), ('materialized view column')
--dg     LOOP
--dg         BEGIN
--dg             PERFORM pg_get_object_address(objtype, '{one}', '{}');
--dg         EXCEPTION WHEN invalid_parameter_value THEN
--dg             RAISE WARNING 'error for %: %', objtype, sqlerrm;
--dg         END;
--dg     END LOOP;
--dg END;
--dg $$;

-- miscellaneous other errors
--dg-differs: another error: relation pg_opfamily cannot be opened: digres has no catalog tables
--dg select * from pg_get_object_address('operator of access method', '{btree,integer_ops,1}', '{int4,bool}');
--dg-differs: another error: relation pg_opfamily cannot be opened: digres has no catalog tables
--dg select * from pg_get_object_address('operator of access method', '{btree,integer_ops,99}', '{int4,int4}');
--dg-differs: another error: relation pg_opfamily cannot be opened: digres has no catalog tables
--dg select * from pg_get_object_address('function of access method', '{btree,integer_ops,1}', '{int4,bool}');
--dg-differs: another error: relation pg_opfamily cannot be opened: digres has no catalog tables
--dg select * from pg_get_object_address('function of access method', '{btree,integer_ops,99}', '{int4,int4}');

--dg-skip: language "?" does not exist
--dg DO $$
--dg DECLARE
--dg     objtype text;
--dg     names   text[];
--dg     args    text[];
--dg BEGIN
--dg     FOR objtype IN VALUES
--dg         ('table'), ('index'), ('sequence'), ('view'),
--dg         ('materialized view'), ('foreign table'),
--dg         ('table column'), ('foreign table column'),
--dg         ('aggregate'), ('function'), ('procedure'), ('type'), ('cast'),
--dg         ('table constraint'), ('domain constraint'), ('conversion'), ('default value'),
--dg         ('operator'), ('operator class'), ('operator family'), ('rule'), ('trigger'),
--dg         ('text search parser'), ('text search dictionary'),
--dg         ('text search template'), ('text search configuration'),
--dg         ('policy'), ('user mapping'), ('default acl'), ('transform'),
--dg         ('operator of access method'), ('function of access method'),
--dg         ('publication namespace'), ('publication relation')
--dg     LOOP
--dg         FOR names IN VALUES ('{eins}'), ('{addr_nsp, zwei}'), ('{eins, zwei, drei}')
--dg         LOOP
--dg             FOR args IN VALUES ('{}'), ('{integer}')
--dg             LOOP
--dg                 BEGIN
--dg                     PERFORM pg_get_object_address(objtype, names, args);
--dg                 EXCEPTION WHEN OTHERS THEN
--dg                     RAISE WARNING 'error for %,%,%: %', objtype, names, args, sqlerrm;
--dg                 END;
--dg             END LOOP;
--dg         END LOOP;
--dg     END LOOP;
--dg END;
--dg $$;

-- these object types cannot be qualified names
SELECT pg_get_object_address('language', '{one}', '{}');
SELECT pg_get_object_address('language', '{one,two}', '{}');
--dg-differs: another error: large objects are not supported
--dg SELECT pg_get_object_address('large object', '{123}', '{}');
SELECT pg_get_object_address('large object', '{123,456}', '{}');
--dg-differs: another error: large objects are not supported
--dg SELECT pg_get_object_address('large object', '{blargh}', '{}');
SELECT pg_get_object_address('schema', '{one}', '{}');
SELECT pg_get_object_address('schema', '{one,two}', '{}');
SELECT pg_get_object_address('role', '{one}', '{}');
SELECT pg_get_object_address('role', '{one,two}', '{}');
SELECT pg_get_object_address('database', '{one}', '{}');
SELECT pg_get_object_address('database', '{one,two}', '{}');
SELECT pg_get_object_address('tablespace', '{one}', '{}');
SELECT pg_get_object_address('tablespace', '{one,two}', '{}');
--dg-differs: another error: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg SELECT pg_get_object_address('foreign-data wrapper', '{one}', '{}');
SELECT pg_get_object_address('foreign-data wrapper', '{one,two}', '{}');
--dg-differs: another error: relation pg_foreign_server cannot be opened: digres has no catalog tables
--dg SELECT pg_get_object_address('server', '{one}', '{}');
SELECT pg_get_object_address('server', '{one,two}', '{}');
SELECT pg_get_object_address('extension', '{one}', '{}');
SELECT pg_get_object_address('extension', '{one,two}', '{}');
--dg-differs: another error: relation pg_event_trigger cannot be opened: digres has no catalog tables
--dg SELECT pg_get_object_address('event trigger', '{one}', '{}');
SELECT pg_get_object_address('event trigger', '{one,two}', '{}');
SELECT pg_get_object_address('access method', '{one}', '{}');
SELECT pg_get_object_address('access method', '{one,two}', '{}');
--dg-differs: another error: unrecognized object type "?"
--dg SELECT pg_get_object_address('publication', '{one}', '{}');
--dg-differs: another error: unrecognized object type "?"
--dg SELECT pg_get_object_address('publication', '{one,two}', '{}');
--dg-differs: another error: unrecognized object type "?"
--dg SELECT pg_get_object_address('subscription', '{one}', '{}');
--dg-differs: another error: unrecognized object type "?"
--dg SELECT pg_get_object_address('subscription', '{one,two}', '{}');

-- Make sure that NULL handling is correct.
\pset null 'NULL'

-- Temporarily disable fancy output, so as future additions never create
-- a large amount of diffs.
\a\t

-- test successful cases
--dg-cascade
--dg WITH objects (type, name, args) AS (VALUES
--dg     ('table', '{addr_nsp, gentable}'::text[], '{}'::text[]),
--dg     ('table', '{addr_nsp, parttable}'::text[], '{}'::text[]),
--dg     ('index', '{addr_nsp, gentable_pkey}', '{}'),
--dg     ('index', '{addr_nsp, parttable_pkey}', '{}'),
--dg     ('sequence', '{addr_nsp, gentable_a_seq}', '{}'),
--dg     -- toast table
--dg     ('view', '{addr_nsp, genview}', '{}'),
--dg     ('materialized view', '{addr_nsp, genmatview}', '{}'),
--dg     ('foreign table', '{addr_nsp, genftable}', '{}'),
--dg     ('table column', '{addr_nsp, gentable, b}', '{}'),
--dg     ('foreign table column', '{addr_nsp, genftable, a}', '{}'),
--dg     ('aggregate', '{addr_nsp, genaggr}', '{int4}'),
--dg     ('function', '{pg_catalog, pg_identify_object}', '{pg_catalog.oid, pg_catalog.oid, int4}'),
--dg     ('procedure', '{addr_nsp, proc}', '{int4}'),
--dg     ('type', '{pg_catalog._int4}', '{}'),
--dg     ('type', '{addr_nsp.gendomain}', '{}'),
--dg     ('type', '{addr_nsp.gencomptype}', '{}'),
--dg     ('type', '{addr_nsp.genenum}', '{}'),
--dg     ('cast', '{int8}', '{int4}'),
--dg     ('collation', '{default}', '{}'),
--dg     ('table constraint', '{addr_nsp, gentable, a_chk}', '{}'),
--dg     ('domain constraint', '{addr_nsp.gendomain}', '{domconstr}'),
--dg     ('conversion', '{pg_catalog, koi8_r_to_mic}', '{}'),
--dg     ('default value', '{addr_nsp, gentable, b}', '{}'),
--dg     ('language', '{plpgsql}', '{}'),
--dg     -- large object
--dg     ('operator', '{+}', '{int4, int4}'),
--dg     ('operator class', '{btree, int4_ops}', '{}'),
--dg     ('operator family', '{btree, integer_ops}', '{}'),
--dg     ('operator of access method', '{btree,integer_ops,1}', '{integer,integer}'),
--dg     ('function of access method', '{btree,integer_ops,2}', '{integer,integer}'),
--dg     ('rule', '{addr_nsp, genview, _RETURN}', '{}'),
--dg     ('trigger', '{addr_nsp, gentable, t}', '{}'),
--dg     ('schema', '{addr_nsp}', '{}'),
--dg     ('text search parser', '{addr_ts_prs}', '{}'),
--dg     ('text search dictionary', '{addr_ts_dict}', '{}'),
--dg     ('text search template', '{addr_ts_temp}', '{}'),
--dg     ('text search configuration', '{addr_ts_conf}', '{}'),
--dg     ('role', '{regress_addr_user}', '{}'),
--dg     -- database
--dg     -- tablespace
--dg     ('foreign-data wrapper', '{addr_fdw}', '{}'),
--dg     ('server', '{addr_fserv}', '{}'),
--dg     ('user mapping', '{regress_addr_user}', '{integer}'),
--dg     ('default acl', '{regress_addr_user,public}', '{r}'),
--dg     ('default acl', '{regress_addr_user}', '{r}'),
--dg     -- extension
--dg     -- event trigger
--dg     ('policy', '{addr_nsp, gentable, genpol}', '{}'),
--dg     ('transform', '{int}', '{sql}'),
--dg     ('access method', '{btree}', '{}'),
--dg     ('publication', '{addr_pub}', '{}'),
--dg     ('publication namespace', '{addr_nsp}', '{addr_pub_schema}'),
--dg     ('publication relation', '{addr_nsp, gentable}', '{addr_pub}'),
--dg     ('subscription', '{regress_addr_sub}', '{}'),
--dg     ('statistics object', '{addr_nsp, gentable_stat}', '{}')
--dg   )
--dg SELECT (pg_identify_object(addr1.classid, addr1.objid, addr1.objsubid)).*,
--dg         -- test roundtrip through pg_identify_object_as_address
--dg         ROW(pg_identify_object(addr1.classid, addr1.objid, addr1.objsubid)) =
--dg           ROW(pg_identify_object(addr2.classid, addr2.objid, addr2.objsubid)) AS roundtrip
--dg FROM objects,
--dg      pg_get_object_address(type, name, args) AS addr1,
--dg      pg_identify_object_as_address(classid, objid, objsubid) AS ioa (typ, nms, args),
--dg      pg_get_object_address(typ, nms, ioa.args) AS addr2
--dg ORDER BY addr1.classid, addr1.objid, addr1.objsubid;

---
--- Cleanup resources
---
--dg-skip: relation pg_foreign_data_wrapper cannot be opened: digres has no catalog tables
--dg DROP FOREIGN DATA WRAPPER addr_fdw CASCADE;
--dg-skip: publications are not supported
--dg DROP PUBLICATION addr_pub;
--dg-skip: publications are not supported
--dg DROP PUBLICATION addr_pub_schema;
--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_addr_sub;

--dg-cascade
--dg DROP SCHEMA addr_nsp CASCADE;

DROP OWNED BY regress_addr_user;
DROP USER regress_addr_user;

--
-- Checks for invalid objects
--

-- Keep this list in the same order as getObjectIdentityParts()
-- in objectaddress.c.
--dg-skip: relation "?" does not exist
--dg WITH objects (classid, objid, objsubid) AS (VALUES
--dg     ('pg_class'::regclass, 0, 0), -- no relation
--dg     ('pg_class'::regclass, 'pg_class'::regclass, 100), -- no column for relation
--dg     ('pg_proc'::regclass, 0, 0), -- no function
--dg     ('pg_type'::regclass, 0, 0), -- no type
--dg     ('pg_cast'::regclass, 0, 0), -- no cast
--dg     ('pg_collation'::regclass, 0, 0), -- no collation
--dg     ('pg_constraint'::regclass, 0, 0), -- no constraint
--dg     ('pg_conversion'::regclass, 0, 0), -- no conversion
--dg     ('pg_attrdef'::regclass, 0, 0), -- no default attribute
--dg     ('pg_language'::regclass, 0, 0), -- no language
--dg     ('pg_largeobject'::regclass, 0, 0), -- no large object, no error
--dg     ('pg_operator'::regclass, 0, 0), -- no operator
--dg     ('pg_opclass'::regclass, 0, 0), -- no opclass, no need to check for no access method
--dg     ('pg_opfamily'::regclass, 0, 0), -- no opfamily
--dg     ('pg_am'::regclass, 0, 0), -- no access method
--dg     ('pg_amop'::regclass, 0, 0), -- no AM operator
--dg     ('pg_amproc'::regclass, 0, 0), -- no AM proc
--dg     ('pg_rewrite'::regclass, 0, 0), -- no rewrite
--dg     ('pg_trigger'::regclass, 0, 0), -- no trigger
--dg     ('pg_namespace'::regclass, 0, 0), -- no schema
--dg     ('pg_statistic_ext'::regclass, 0, 0), -- no statistics
--dg     ('pg_ts_parser'::regclass, 0, 0), -- no TS parser
--dg     ('pg_ts_dict'::regclass, 0, 0), -- no TS dictionary
--dg     ('pg_ts_template'::regclass, 0, 0), -- no TS template
--dg     ('pg_ts_config'::regclass, 0, 0), -- no TS configuration
--dg     ('pg_authid'::regclass, 0, 0), -- no role
--dg     ('pg_auth_members'::regclass, 0, 0),  -- no role membership
--dg     ('pg_database'::regclass, 0, 0), -- no database
--dg     ('pg_tablespace'::regclass, 0, 0), -- no tablespace
--dg     ('pg_foreign_data_wrapper'::regclass, 0, 0), -- no FDW
--dg     ('pg_foreign_server'::regclass, 0, 0), -- no server
--dg     ('pg_user_mapping'::regclass, 0, 0), -- no user mapping
--dg     ('pg_default_acl'::regclass, 0, 0), -- no default ACL
--dg     ('pg_extension'::regclass, 0, 0), -- no extension
--dg     ('pg_event_trigger'::regclass, 0, 0), -- no event trigger
--dg     ('pg_parameter_acl'::regclass, 0, 0), -- no parameter ACL
--dg     ('pg_policy'::regclass, 0, 0), -- no policy
--dg     ('pg_publication'::regclass, 0, 0), -- no publication
--dg     ('pg_publication_namespace'::regclass, 0, 0), -- no publication namespace
--dg     ('pg_publication_rel'::regclass, 0, 0), -- no publication relation
--dg     ('pg_subscription'::regclass, 0, 0), -- no subscription
--dg     ('pg_transform'::regclass, 0, 0) -- no transformation
--dg   )
--dg SELECT ROW(pg_identify_object(objects.classid, objects.objid, objects.objsubid))
--dg          AS ident,
--dg        ROW(pg_identify_object_as_address(objects.classid, objects.objid, objects.objsubid))
--dg          AS addr,
--dg        pg_describe_object(objects.classid, objects.objid, objects.objsubid)
--dg          AS descr
--dg FROM objects
--dg ORDER BY objects.classid, objects.objid, objects.objsubid;

-- restore normal output mode
\a\t
