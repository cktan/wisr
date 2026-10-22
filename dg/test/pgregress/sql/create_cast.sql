--
-- CREATE_CAST
--

-- Create some types to test with
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE TYPE casttesttype;

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE FUNCTION casttesttype_in(cstring)
--dg    RETURNS casttesttype
--dg    AS 'textin'
--dg    LANGUAGE internal STRICT IMMUTABLE;
--dg-cascade
--dg CREATE FUNCTION casttesttype_out(casttesttype)
--dg    RETURNS cstring
--dg    AS 'textout'
--dg    LANGUAGE internal STRICT IMMUTABLE;

--dg-cascade
--dg CREATE TYPE casttesttype (
--dg    internallength = variable,
--dg    input = casttesttype_in,
--dg    output = casttesttype_out,
--dg    alignment = int4
--dg );

-- a dummy function to test with
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION casttestfunc(casttesttype) RETURNS int4 LANGUAGE SQL AS
--dg $$ SELECT 1; $$;

SELECT casttestfunc('foo'::text); -- fails, as there's no cast

-- Try binary coercion cast
--dg-cascade
--dg CREATE CAST (text AS casttesttype) WITHOUT FUNCTION;
SELECT casttestfunc('foo'::text); -- doesn't work, as the cast is explicit
--dg-cascade
--dg SELECT casttestfunc('foo'::text::casttesttype); -- should work
--dg-cascade
--dg DROP CAST (text AS casttesttype); -- cleanup

-- Try IMPLICIT binary coercion cast
--dg-cascade
--dg CREATE CAST (text AS casttesttype) WITHOUT FUNCTION AS IMPLICIT;
--dg-cascade
--dg SELECT casttestfunc('foo'::text); -- Should work now

-- Try I/O conversion cast.
--dg-cascade
--dg SELECT 1234::int4::casttesttype; -- No cast yet, should fail

--dg-cascade
--dg CREATE CAST (int4 AS casttesttype) WITH INOUT;
--dg-cascade
--dg SELECT 1234::int4::casttesttype; -- Should work now

--dg-cascade
--dg DROP CAST (int4 AS casttesttype);

-- Try cast with a function

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION int4_casttesttype(int4) RETURNS casttesttype LANGUAGE SQL AS
--dg $$ SELECT ('foo'::text || $1::text)::casttesttype; $$;

--dg-cascade
--dg CREATE CAST (int4 AS casttesttype) WITH FUNCTION int4_casttesttype(int4) AS IMPLICIT;
--dg-cascade
--dg SELECT 1234::int4::casttesttype; -- Should work now

--dg-cascade
--dg DROP FUNCTION int4_casttesttype(int4) CASCADE;

-- Try it with a function that requires an implicit cast

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION bar_int4_text(int4) RETURNS text LANGUAGE SQL AS
--dg $$ SELECT ('bar'::text || $1::text); $$;

--dg-cascade
--dg CREATE CAST (int4 AS casttesttype) WITH FUNCTION bar_int4_text(int4) AS IMPLICIT;
--dg-cascade
--dg SELECT 1234::int4::casttesttype; -- Should work now

-- check dependencies generated for that
--dg-cascade
--dg SELECT pg_describe_object(classid, objid, objsubid) as obj,
--dg        pg_describe_object(refclassid, refobjid, refobjsubid) as objref,
--dg        deptype
--dg FROM pg_depend
--dg WHERE classid = 'pg_cast'::regclass AND
--dg       objid = (SELECT oid FROM pg_cast
--dg                WHERE castsource = 'int4'::regtype
--dg                  AND casttarget = 'casttesttype'::regtype)
--dg ORDER BY refclassid;
