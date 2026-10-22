--
-- expression evaluation tests that don't fit into a more specific file
--

--
-- Tests for SQLValueFunction
--


-- current_date  (always matches because of transactional behaviour)
SELECT date(now())::text = current_date::text;


-- current_time / localtime
SELECT now()::timetz::text = current_time::text;
SELECT now()::timetz(4)::text = current_time(4)::text;
SELECT now()::time::text = localtime::text;
SELECT now()::time(3)::text = localtime(3)::text;

-- current_time[stamp]/ localtime[stamp] (always matches because of transactional behaviour)
SELECT current_timestamp = NOW();
-- precision
SELECT length(current_timestamp::text) >= length(current_timestamp(0)::text);
-- localtimestamp
SELECT now()::timestamp::text = localtimestamp::text;
-- precision overflow
SELECT current_time = current_time(7);
SELECT current_timestamp = current_timestamp(7);
SELECT localtime = localtime(7);
SELECT localtimestamp = localtimestamp(7);

-- current_role/user/user is tested in rolenames.sql

-- current database / catalog
SELECT current_catalog = current_database();

-- current_schema
SELECT current_schema;
SET search_path = 'notme';
SELECT current_schema;
SET search_path = 'pg_catalog';
SELECT current_schema;
RESET search_path;


--
-- Test parsing of a no-op cast to a type with unspecified typmod
--
begin;

create table numeric_tbl (f1 numeric(18,3), f2 numeric);

create view numeric_view as
  select
    f1, f1::numeric(16,4) as f1164, f1::numeric as f1n,
    f2, f2::numeric(16,4) as f2164, f2::numeric as f2n
  from numeric_tbl;

--dg-differs: the result
--dg \d+ numeric_view

--dg-differs: the plan
--dg explain (verbose, costs off) select * from numeric_view;

-- bpchar, lacking planner support for its length coercion function,
-- could behave differently

create table bpchar_tbl (f1 character(16) unique, f2 bpchar);

create view bpchar_view as
  select
    f1, f1::character(14) as f114, f1::bpchar as f1n,
    f2, f2::character(14) as f214, f2::bpchar as f2n
  from bpchar_tbl;

\d+ bpchar_view

--dg-differs: the plan
--dg explain (verbose, costs off) select * from bpchar_view
--dg   where f1::bpchar = 'foo';

rollback;


--
-- Ordinarily, IN/NOT IN can be converted to a ScalarArrayOpExpr
-- with a suitably-chosen array type.
--
explain (verbose, costs off)
select random() IN (1, 4, 8.0);
explain (verbose, costs off)
select random()::int IN (1, 4, 8.0);
-- However, if there's not a common supertype for the IN elements,
-- we should instead try to produce "x = v1 OR x = v2 OR ...".
-- In most cases that'll fail for lack of all the requisite = operators,
-- but it can succeed sometimes.  So this should complain about lack of
-- an = operator, not about cast failure.
select '(0,0)'::point in ('(0,0,0,0)'::box, point(0,0));


--
-- Tests for ScalarArrayOpExpr with a hashfn
--

-- create a stable function so that the tests below are not
-- evaluated using the planner's constant folding.
begin;

--dg-skip: language "?" does not exist
--dg create function return_int_input(int) returns int as $$
--dg begin
--dg 	return $1;
--dg end;
--dg $$ language plpgsql stable;

--dg-skip: language "?" does not exist
--dg create function return_text_input(text) returns text as $$
--dg begin
--dg 	return $1;
--dg end;
--dg $$ language plpgsql stable;

--dg-cascade
--dg select return_int_input(1) in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1);
--dg-cascade
--dg select return_int_input(1) in (10, 9, 2, 8, 3, 7, 4, 6, 5, null);
--dg-cascade
--dg select return_int_input(1) in (null, null, null, null, null, null, null, null, null, null, null);
--dg-cascade
--dg select return_int_input(1) in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1, null);
--dg-cascade
--dg select return_int_input(null::int) in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1);
--dg-cascade
--dg select return_int_input(null::int) in (10, 9, 2, 8, 3, 7, 4, 6, 5, null);
--dg-cascade
--dg select return_text_input('a') in ('a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j');
-- NOT IN
--dg-cascade
--dg select return_int_input(1) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1);
--dg-cascade
--dg select return_int_input(1) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, 0);
--dg-cascade
--dg select return_int_input(1) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, 2, null);
--dg-cascade
--dg select return_int_input(1) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1, null);
--dg-cascade
--dg select return_int_input(1) not in (null, null, null, null, null, null, null, null, null, null, null);
--dg-cascade
--dg select return_int_input(null::int) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, 1);
--dg-cascade
--dg select return_int_input(null::int) not in (10, 9, 2, 8, 3, 7, 4, 6, 5, null);
--dg-cascade
--dg select return_text_input('a') not in ('a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j');

rollback;

-- Test with non-strict equality function.
-- We need to create our own type for this.

begin;

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type myint;
--dg-cascade
--dg create function myintin(cstring) returns myint strict immutable language
--dg   internal as 'int4in';
--dg-cascade
--dg create function myintout(myint) returns cstring strict immutable language
--dg   internal as 'int4out';
--dg-cascade
--dg create function myinthash(myint) returns integer strict immutable language
--dg   internal as 'hashint4';

--dg-cascade
--dg create type myint (input = myintin, output = myintout, like = int4);

--dg-cascade
--dg create cast (int4 as myint) without function;
--dg-cascade
--dg create cast (myint as int4) without function;

--dg-cascade
--dg create function myinteq(myint, myint) returns bool as $$
--dg begin
--dg   if $1 is null and $2 is null then
--dg     return true;
--dg   else
--dg     return $1::int = $2::int;
--dg   end if;
--dg end;
--dg $$ language plpgsql immutable;

--dg-cascade
--dg create function myintne(myint, myint) returns bool as $$
--dg begin
--dg   return not myinteq($1, $2);
--dg end;
--dg $$ language plpgsql immutable;

--dg-cascade
--dg create operator = (
--dg   leftarg    = myint,
--dg   rightarg   = myint,
--dg   commutator = =,
--dg   negator    = <>,
--dg   procedure  = myinteq,
--dg   restrict   = eqsel,
--dg   join       = eqjoinsel,
--dg   merges
--dg );

--dg-cascade
--dg create operator <> (
--dg   leftarg    = myint,
--dg   rightarg   = myint,
--dg   commutator = <>,
--dg   negator    = =,
--dg   procedure  = myintne,
--dg   restrict   = eqsel,
--dg   join       = eqjoinsel,
--dg   merges
--dg );

--dg-cascade
--dg create operator class myint_ops
--dg default for type myint using hash as
--dg   operator    1   =  (myint, myint),
--dg   function    1   myinthash(myint);

--dg-cascade
--dg create table inttest (a myint);
--dg-cascade
--dg insert into inttest values (null), (0::myint), (1::myint);

-- Test EEOP_HASHED_SCALARARRAYOP against EEOP_SCALARARRAYOP.  Ensure the
-- result of non-hashed vs hashed is the same.
--dg-cascade
--dg select
--dg   a,
--dg   a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
--dg   a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed
--dg from inttest;

--dg-cascade
--dg select
--dg   a,
--dg   a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
--dg   a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
--dg  from inttest;

--dg-cascade
--dg select
--dg   a,
--dg   a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
--dg   a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed
--dg from inttest;

--dg-cascade
--dg select
--dg   a,
--dg   a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
--dg   a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
--dg from inttest;

-- Now make the equal function return false when given two NULLs
--dg-cascade
--dg create or replace function myinteq(myint, myint) returns bool as $$
--dg begin
--dg   if $1 is null and $2 is null then
--dg     return false;
--dg   else
--dg     return $1::int = $2::int;
--dg   end if;
--dg end;
--dg $$ language plpgsql immutable;

-- And try the same again to ensure EEOP_HASHED_SCALARARRAYOP does the same
-- thing as EEOP_SCALARARRAYOP.
--dg-cascade
--dg select
--dg   a,
--dg   a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
--dg   a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed
--dg from inttest;

--dg-cascade
--dg select
--dg   a,
--dg   a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
--dg   a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
--dg  from inttest;

--dg-cascade
--dg select
--dg   a,
--dg   a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
--dg   a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed
--dg from inttest;

--dg-cascade
--dg select
--dg   a,
--dg   a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
--dg   a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
--dg from inttest;

-- Try again with an equality function that treats NULLs as equal to 0.
--dg-cascade
--dg create or replace function myinteq(myint, myint) returns bool as $$
--dg begin
--dg   if $1 is null and $2 is null then
--dg     return false;
--dg   else
--dg     return coalesce($1::int,0) = coalesce($2::int, 0);
--dg   end if;
--dg end;
--dg $$ language plpgsql immutable;

--dg-cascade
--dg select
--dg   a,
--dg   a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
--dg   a in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed,
--dg   a in (0::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed_zero,
--dg   a in (0::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed_zero
--dg from inttest;

--dg-cascade
--dg select
--dg   a,
--dg   a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
--dg   a in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
--dg  from inttest;

--dg-cascade
--dg select
--dg   a,
--dg   a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as not_hashed,
--dg   a not in (1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint,9::myint) as hashed,
--dg   a not in (0::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed_zero,
--dg   a not in (0::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed_zero
--dg from inttest;

--dg-cascade
--dg select
--dg   a,
--dg   a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint) as not_hashed,
--dg   a not in (null::myint,1::myint,2::myint,3::myint,4::myint,5::myint,6::myint,7::myint,8::myint) as hashed
--dg from inttest;

rollback;
