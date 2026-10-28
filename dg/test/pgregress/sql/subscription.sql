--
-- SUBSCRIPTION
--

CREATE ROLE regress_subscription_user LOGIN SUPERUSER;
CREATE ROLE regress_subscription_user2;
CREATE ROLE regress_subscription_user3 IN ROLE pg_create_subscription;
CREATE ROLE regress_subscription_user_dummy LOGIN NOSUPERUSER;
SET SESSION AUTHORIZATION 'regress_subscription_user';

-- fail - no publications
CREATE SUBSCRIPTION regress_testsub CONNECTION 'foo';

-- fail - no connection
CREATE SUBSCRIPTION regress_testsub PUBLICATION foo;

-- fail - cannot do CREATE SUBSCRIPTION CREATE SLOT inside transaction block
BEGIN;
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'testconn' PUBLICATION testpub WITH (create_slot);
COMMIT;

-- fail - invalid connection string
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'testconn' PUBLICATION testpub;

-- fail - duplicate publications
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION foo, testpub, foo WITH (connect = false);

-- ok
--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);

--dg-skip: subscriptions are not supported
--dg COMMENT ON SUBSCRIPTION regress_testsub IS 'test subscription';
--dg-cascade
--dg SELECT obj_description(s.oid, 'pg_subscription') FROM pg_subscription s;

-- Check if the subscription stats are created and stats_reset is updated
-- by pg_stat_reset_subscription_stats().
--dg-cascade
--dg SELECT subname, stats_reset IS NULL stats_reset_is_null FROM pg_stat_subscription_stats WHERE subname = 'regress_testsub';
--dg-cascade
--dg SELECT pg_stat_reset_subscription_stats(oid) FROM pg_subscription WHERE subname = 'regress_testsub';
--dg-cascade
--dg SELECT subname, stats_reset IS NULL stats_reset_is_null FROM pg_stat_subscription_stats WHERE subname = 'regress_testsub';

-- Reset the stats again and check if the new reset_stats is updated.
--dg-cascade
--dg SELECT stats_reset as prev_stats_reset FROM pg_stat_subscription_stats WHERE subname = 'regress_testsub' \gset
--dg-cascade
--dg SELECT pg_stat_reset_subscription_stats(oid) FROM pg_subscription WHERE subname = 'regress_testsub';
--dg-cascade
--dg SELECT :'prev_stats_reset' < stats_reset FROM pg_stat_subscription_stats WHERE subname = 'regress_testsub';

-- fail - name already exists
--dg-cascade
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);

-- fail - must be superuser
SET SESSION AUTHORIZATION 'regress_subscription_user2';
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION foo WITH (connect = false);
SET SESSION AUTHORIZATION 'regress_subscription_user';

-- fail - invalid option combinations
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, copy_data = true);
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, enabled = true);
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, create_slot = true);
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, enabled = true);
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, enabled = false, create_slot = true);
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE);
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, enabled = false);
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, create_slot = false);

-- ok - with slot_name = NONE
--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub3 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, connect = false);
-- fail
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub3 ENABLE;
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub3 REFRESH PUBLICATION;

-- fail - origin must be either none or any
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub4 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, connect = false, origin = foo);

-- now it works
--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub4 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, connect = false, origin = none);
--dg-cascade
--dg \dRs+ regress_testsub4
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub4 SET (origin = any);
--dg-cascade
--dg \dRs+ regress_testsub4

--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_testsub3;
--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_testsub4;

-- fail, connection string does not parse
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub5 CONNECTION 'i_dont_exist=param' PUBLICATION testpub;

-- fail, connection string parses, but doesn't work (and does so without
-- connecting, so this is reliable and safe)
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub5 CONNECTION 'port=-1' PUBLICATION testpub;

-- fail - invalid connection string during ALTER
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub CONNECTION 'foobar';

--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET PUBLICATION testpub2, testpub3 WITH (refresh = false);
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist2';
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (slot_name = 'newname');
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (password_required = false);
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (run_as_owner = true);
--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (password_required = true);
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (run_as_owner = false);

-- fail
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub SET (slot_name = '');

-- fail
--dg-differs: another error: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_doesnotexist CONNECTION 'dbname=regress_doesnotexist2';
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub SET (create_slot = false);

-- ok
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SKIP (lsn = '0/12345');

--dg-cascade
--dg \dRs+

-- ok - with lsn = NONE
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SKIP (lsn = NONE);

-- fail
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub SKIP (lsn = '0/0');

--dg-cascade
--dg \dRs+

BEGIN;
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub ENABLE;

--dg-cascade
--dg \dRs

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub DISABLE;

--dg-cascade
--dg \dRs

COMMIT;

-- fail - must be owner of subscription
SET ROLE regress_subscription_user_dummy;
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub RENAME TO regress_testsub_dummy;
RESET ROLE;

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub RENAME TO regress_testsub_foo;
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub_foo SET (synchronous_commit = local);
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub_foo SET (synchronous_commit = foobar);

--dg-cascade
--dg \dRs+

-- rename back to keep the rest simple
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub_foo RENAME TO regress_testsub;

-- ok, we're a superuser
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub OWNER TO regress_subscription_user2;

-- fail - cannot do DROP SUBSCRIPTION inside transaction block with slot name
BEGIN;
--dg-cascade
--dg DROP SUBSCRIPTION regress_testsub;
COMMIT;

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);

-- now it works
BEGIN;
--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_testsub;
COMMIT;

--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION IF EXISTS regress_testsub;
--dg-cascade
--dg DROP SUBSCRIPTION regress_testsub;  -- fail

-- fail - binary must be boolean
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, binary = foo);

-- now it works
--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, binary = true);

--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (binary = false);
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);

--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_testsub;

-- fail - streaming must be boolean or 'parallel'
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, streaming = foo);

-- now it works
--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, streaming = true);

--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (streaming = parallel);

--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (streaming = false);
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);

--dg-cascade
--dg \dRs+

-- fail - publication already exists
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub ADD PUBLICATION testpub WITH (refresh = false);

-- fail - publication used more than once
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub ADD PUBLICATION testpub1, testpub1 WITH (refresh = false);

-- ok - add two publications into subscription
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub ADD PUBLICATION testpub1, testpub2 WITH (refresh = false);

-- fail - publications already exist
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub ADD PUBLICATION testpub1, testpub2 WITH (refresh = false);

--dg-cascade
--dg \dRs+

-- fail - publication used more than once
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub DROP PUBLICATION testpub1, testpub1 WITH (refresh = false);

-- fail - all publications are deleted
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub DROP PUBLICATION testpub, testpub1, testpub2 WITH (refresh = false);

-- fail - publication does not exist in subscription
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub DROP PUBLICATION testpub3 WITH (refresh = false);

-- ok - delete publications
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub DROP PUBLICATION testpub1, testpub2 WITH (refresh = false);

--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_testsub;

--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION mypub
--dg        WITH (connect = false, create_slot = false, copy_data = false);

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub ENABLE;

-- fail - ALTER SUBSCRIPTION with refresh is not allowed in a transaction
-- block or function
BEGIN;
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub SET PUBLICATION mypub WITH (refresh = true);
END;

BEGIN;
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub REFRESH PUBLICATION;
END;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION func() RETURNS VOID AS
--dg $$ ALTER SUBSCRIPTION regress_testsub SET PUBLICATION mypub WITH (refresh = true) $$ LANGUAGE SQL;
--dg-cascade
--dg SELECT func();

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub DISABLE;
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_testsub;
--dg-cascade
--dg DROP FUNCTION func;

-- fail - two_phase must be boolean
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, two_phase = foo);

-- now it works
--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, two_phase = true);

--dg-cascade
--dg \dRs+
-- we can alter streaming when two_phase enabled
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (streaming = true);

--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_testsub;

-- two_phase and streaming are compatible.
--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, streaming = true, two_phase = true);

--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_testsub;

-- fail - disable_on_error must be boolean
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, disable_on_error = foo);

-- now it works
--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, disable_on_error = false);

--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (disable_on_error = true);

--dg-cascade
--dg \dRs+

--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_testsub;

-- let's do some tests with pg_create_subscription rather than superuser
SET SESSION AUTHORIZATION regress_subscription_user3;

-- fail, not enough privileges
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);

-- fail, must specify password
RESET SESSION AUTHORIZATION;
GRANT CREATE ON DATABASE REGRESSION TO regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;
--dg-cascade
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);

-- fail, can't set password_required=false
RESET SESSION AUTHORIZATION;
GRANT CREATE ON DATABASE REGRESSION TO regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;
--dg-differs: another error: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, password_required = false);

-- ok
RESET SESSION AUTHORIZATION;
GRANT CREATE ON DATABASE REGRESSION TO regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;
--dg-skip: subscriptions are not supported
--dg CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist password=regress_fakepassword' PUBLICATION testpub WITH (connect = false);

-- we cannot give the subscription away to some random user
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub OWNER TO regress_subscription_user;

-- but we can rename the subscription we just created
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub RENAME TO regress_testsub2;

-- ok, even after losing pg_create_subscription we can still rename it
RESET SESSION AUTHORIZATION;
REVOKE pg_create_subscription FROM regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub2 RENAME TO regress_testsub;

-- fail, after losing CREATE on the database we can't rename it any more
RESET SESSION AUTHORIZATION;
REVOKE CREATE ON DATABASE REGRESSION FROM regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub RENAME TO regress_testsub2;

-- fail - cannot do ALTER SUBSCRIPTION SET (failover) inside transaction block
BEGIN;
--dg-cascade
--dg ALTER SUBSCRIPTION regress_testsub SET (failover);
COMMIT;

-- ok, owning it is enough for this stuff
--dg-skip: subscriptions are not supported
--dg ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
--dg-skip: subscriptions are not supported
--dg DROP SUBSCRIPTION regress_testsub;

RESET SESSION AUTHORIZATION;
--dg-cascade
--dg DROP ROLE regress_subscription_user;
DROP ROLE regress_subscription_user2;
DROP ROLE regress_subscription_user3;
DROP ROLE regress_subscription_user_dummy;
