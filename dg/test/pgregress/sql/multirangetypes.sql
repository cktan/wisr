-- Tests for multirange data types.

--
-- test input parser
--

-- negative tests; should fail
select ''::textmultirange;
select '{,}'::textmultirange;
select '{(,)}.'::textmultirange;
select '{[a,c),}'::textmultirange;
select '{,[a,c)}'::textmultirange;
select '{-[a,z)}'::textmultirange;
select '{[a,z) - }'::textmultirange;
select '{(",a)}'::textmultirange;
select '{(,,a)}'::textmultirange;
select '{(),a)}'::textmultirange;
select '{(a,))}'::textmultirange;
select '{(],a)}'::textmultirange;
select '{(a,])}'::textmultirange;
select '{[z,a]}'::textmultirange;

-- should succeed
select '{}'::textmultirange;
select '  {}  '::textmultirange;
select ' { empty, empty }  '::textmultirange;
select ' {( " a " " a ", " z " " z " )  }'::textmultirange;
select textrange('\\\\', repeat('a', 200))::textmultirange;
select '{(,z)}'::textmultirange;
select '{(a,)}'::textmultirange;
select '{[,z]}'::textmultirange;
select '{[a,]}'::textmultirange;
select '{(,)}'::textmultirange;
select '{[ , ]}'::textmultirange;
select '{["",""]}'::textmultirange;
select '{[",",","]}'::textmultirange;
select '{["\\","\\"]}'::textmultirange;
select '{["""","\""]}'::textmultirange;
select '{(\\,a)}'::textmultirange;
select '{((,z)}'::textmultirange;
select '{([,z)}'::textmultirange;
select '{(!,()}'::textmultirange;
select '{(!,[)}'::textmultirange;
select '{[a,a]}'::textmultirange;
select '{[a,a],[a,b]}'::textmultirange;
select '{[a,b), [b,e]}'::textmultirange;
select '{[a,d), [b,f]}'::textmultirange;
select '{[a,a],[b,b]}'::textmultirange;
-- without canonicalization, we can't join these:
select '{[a,a], [b,b]}'::textmultirange;
-- with canonicalization, we can join these:
select '{[1,2], [3,4]}'::int4multirange;
select '{[a,a], [b,b], [c,c]}'::textmultirange;
select '{[a,d], [b,e]}'::textmultirange;
select '{[a,d), [d,e)}'::textmultirange;
-- these are allowed but normalize to empty:
select '{[a,a)}'::textmultirange;
select '{(a,a]}'::textmultirange;
select '{(a,a)}'::textmultirange;

-- Also try it with non-error-throwing API
select pg_input_is_valid('{[1,2], [4,5]}', 'int4multirange');
select pg_input_is_valid('{[1,2], [4,5]', 'int4multirange');
select * from pg_input_error_info('{[1,2], [4,5]', 'int4multirange');
select pg_input_is_valid('{[1,2], [4,zed]}', 'int4multirange');
select * from pg_input_error_info('{[1,2], [4,zed]}', 'int4multirange');

--
-- test the constructor
---
select textmultirange();
select textmultirange(textrange('a', 'c'));
select textmultirange(textrange('a', 'c'), textrange('f', 'g'));
select textmultirange(textrange('\\\\', repeat('a', 200)), textrange('c', 'd'));

--
-- test casts, both a built-in range type and a user-defined one:
--
select 'empty'::int4range::int4multirange;
select int4range(1, 3)::int4multirange;
select int4range(1, null)::int4multirange;
select int4range(null, null)::int4multirange;
select 'empty'::textrange::textmultirange;
select textrange('a', 'c')::textmultirange;
select textrange('a', null)::textmultirange;
select textrange(null, null)::textmultirange;

--
-- test unnest(multirange) function
--
select unnest(int4multirange(int4range('5', '6'), int4range('1', '2')));
select unnest(textmultirange(textrange('a', 'b'), textrange('d', 'e')));
select unnest(textmultirange(textrange('\\\\', repeat('a', 200)), textrange('c', 'd')));

--
-- create some test data and test the operators
--

--dg-skip: type ? is not supported yet
--dg CREATE TABLE nummultirange_test (nmr NUMMULTIRANGE);
--dg-cascade
--dg CREATE INDEX nummultirange_test_btree ON nummultirange_test(nmr);

--dg-cascade
--dg INSERT INTO nummultirange_test VALUES('{}');
--dg-cascade
--dg INSERT INTO nummultirange_test VALUES('{[,)}');
--dg-cascade
--dg INSERT INTO nummultirange_test VALUES('{[3,]}');
--dg-cascade
--dg INSERT INTO nummultirange_test VALUES('{[,), [3,]}');
--dg-cascade
--dg INSERT INTO nummultirange_test VALUES('{[, 5)}');
--dg-cascade
--dg INSERT INTO nummultirange_test VALUES(nummultirange());
--dg-cascade
--dg INSERT INTO nummultirange_test VALUES(nummultirange(variadic '{}'::numrange[]));
--dg-cascade
--dg INSERT INTO nummultirange_test VALUES(nummultirange(numrange(1.1, 2.2)));
--dg-cascade
--dg INSERT INTO nummultirange_test VALUES('{empty}');
--dg-cascade
--dg INSERT INTO nummultirange_test VALUES(nummultirange(numrange(1.7, 1.7, '[]'), numrange(1.7, 1.9)));
--dg-cascade
--dg INSERT INTO nummultirange_test VALUES(nummultirange(numrange(1.7, 1.7, '[]'), numrange(1.9, 2.1)));

--dg-cascade
--dg SELECT nmr, isempty(nmr), lower(nmr), upper(nmr) FROM nummultirange_test ORDER BY nmr;
--dg-cascade
--dg SELECT nmr, lower_inc(nmr), lower_inf(nmr), upper_inc(nmr), upper_inf(nmr) FROM nummultirange_test ORDER BY nmr;

--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr = '{}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr = '{(,5)}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr = '{[3,)}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr = '{[1.7,1.7]}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr = '{[1.7,1.7],[1.9,2.1)}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr < '{}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr < '{[-1000.0, -1000.0]}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr < '{[0.0, 1.0]}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr < '{[1000.0, 1001.0]}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr <= '{}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr <= '{[3,)}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr >= '{}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr >= '{[3,)}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr > '{}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr > '{[-1000.0, -1000.0]}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr > '{[0.0, 1.0]}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr > '{[1000.0, 1001.0]}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr <> '{}';
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr <> '{(,5)}';

select nummultirange(numrange(2.0, 1.0));
select nummultirange(numrange(5.0, 6.0), numrange(1.0, 2.0));

--dg-cascade
--dg analyze nummultirange_test;

-- overlaps
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE range_overlaps_multirange(numrange(4.0, 4.2), nmr);
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE numrange(4.0, 4.2) && nmr;
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE multirange_overlaps_range(nmr, numrange(4.0, 4.2));
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr && numrange(4.0, 4.2);
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE multirange_overlaps_multirange(nmr, nummultirange(numrange(4.0, 4.2), numrange(6.0, 7.0)));
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr && nummultirange(numrange(4.0, 4.2), numrange(6.0, 7.0));
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr && nummultirange(numrange(6.0, 7.0));
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr && nummultirange(numrange(6.0, 7.0), numrange(8.0, 9.0));

-- mr contains x
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE multirange_contains_elem(nmr, 4.0);
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr @> 4.0;
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE multirange_contains_range(nmr, numrange(4.0, 4.2));
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr @> numrange(4.0, 4.2);
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE multirange_contains_multirange(nmr, '{[4.0,4.2), [6.0, 8.0)}');
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE nmr @> '{[4.0,4.2), [6.0, 8.0)}'::nummultirange;

-- x is contained by mr
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE elem_contained_by_multirange(4.0, nmr);
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE 4.0 <@ nmr;
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE range_contained_by_multirange(numrange(4.0, 4.2), nmr);
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE numrange(4.0, 4.2) <@ nmr;
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE multirange_contained_by_multirange('{[4.0,4.2), [6.0, 8.0)}', nmr);
--dg-cascade
--dg SELECT * FROM nummultirange_test WHERE '{[4.0,4.2), [6.0, 8.0)}'::nummultirange <@ nmr;

-- overlaps
SELECT 'empty'::numrange && nummultirange();
SELECT 'empty'::numrange && nummultirange(numrange(1,2));
SELECT nummultirange() && 'empty'::numrange;
SELECT nummultirange(numrange(1,2)) && 'empty'::numrange;
SELECT nummultirange() && nummultirange();
SELECT nummultirange() && nummultirange(numrange(1,2));
SELECT nummultirange(numrange(1,2)) && nummultirange();
SELECT nummultirange(numrange(3,4)) && nummultirange(numrange(1,2), numrange(7,8));
SELECT nummultirange(numrange(1,2), numrange(7,8)) && nummultirange(numrange(3,4));
SELECT nummultirange(numrange(3,4)) && nummultirange(numrange(1,2), numrange(3.5,8));
SELECT nummultirange(numrange(1,2), numrange(3.5,8)) && numrange(3,4);
SELECT nummultirange(numrange(1,2), numrange(3.5,8)) && nummultirange(numrange(3,4));
select '{(10,20),(30,40),(50,60)}'::nummultirange && '(42,92)'::numrange;

-- contains
SELECT nummultirange() @> nummultirange();
SELECT nummultirange() @> 'empty'::numrange;
SELECT nummultirange(numrange(null,null)) @> numrange(1,2);
SELECT nummultirange(numrange(null,null)) @> numrange(null,2);
SELECT nummultirange(numrange(null,null)) @> numrange(2,null);
SELECT nummultirange(numrange(null,5)) @> numrange(null,3);
SELECT nummultirange(numrange(null,5)) @> numrange(null,8);
SELECT nummultirange(numrange(5,null)) @> numrange(8,null);
SELECT nummultirange(numrange(5,null)) @> numrange(3,null);
SELECT nummultirange(numrange(1,5)) @> numrange(8,9);
SELECT nummultirange(numrange(1,5)) @> numrange(3,9);
SELECT nummultirange(numrange(1,5)) @> numrange(1,4);
SELECT nummultirange(numrange(1,5)) @> numrange(1,5);
SELECT nummultirange(numrange(-4,-2), numrange(1,5)) @> numrange(1,5);
SELECT nummultirange(numrange(1,5), numrange(8,9)) @> numrange(1,5);
SELECT nummultirange(numrange(1,5), numrange(8,9)) @> numrange(6,7);
SELECT nummultirange(numrange(1,5), numrange(6,9)) @> numrange(6,7);
SELECT '{[1,5)}'::nummultirange @> '{[1,5)}';
SELECT '{[-4,-2), [1,5)}'::nummultirange @> '{[1,5)}';
SELECT '{[1,5), [8,9)}'::nummultirange @> '{[1,5)}';
SELECT '{[1,5), [8,9)}'::nummultirange @> '{[6,7)}';
SELECT '{[1,5), [6,9)}'::nummultirange @> '{[6,7)}';
select '{(10,20),(30,40),(50,60)}'::nummultirange @> '(52,56)'::numrange;
SELECT numrange(null,null) @> nummultirange(numrange(1,2));
SELECT numrange(null,null) @> nummultirange(numrange(null,2));
SELECT numrange(null,null) @> nummultirange(numrange(2,null));
SELECT numrange(null,5) @> nummultirange(numrange(null,3));
SELECT numrange(null,5) @> nummultirange(numrange(null,8));
SELECT numrange(5,null) @> nummultirange(numrange(8,null));
SELECT numrange(5,null) @> nummultirange(numrange(3,null));
SELECT numrange(1,5) @> nummultirange(numrange(8,9));
SELECT numrange(1,5) @> nummultirange(numrange(3,9));
SELECT numrange(1,5) @> nummultirange(numrange(1,4));
SELECT numrange(1,5) @> nummultirange(numrange(1,5));
SELECT numrange(1,9) @> nummultirange(numrange(-4,-2), numrange(1,5));
SELECT numrange(1,9) @> nummultirange(numrange(1,5), numrange(8,9));
SELECT numrange(1,9) @> nummultirange(numrange(1,5), numrange(6,9));
SELECT numrange(1,9) @> nummultirange(numrange(1,5), numrange(6,10));
SELECT '{[1,9)}' @> '{[1,5)}'::nummultirange;
SELECT '{[1,9)}' @> '{[-4,-2), [1,5)}'::nummultirange;
SELECT '{[1,9)}' @> '{[1,5), [8,9)}'::nummultirange;
SELECT '{[1,9)}' @> '{[1,5), [6,9)}'::nummultirange;
SELECT '{[1,9)}' @> '{[1,5), [6,10)}'::nummultirange;

-- is contained by
SELECT nummultirange() <@ nummultirange();
SELECT 'empty'::numrange <@ nummultirange();
SELECT numrange(1,2) <@ nummultirange(numrange(null,null));
SELECT numrange(null,2) <@ nummultirange(numrange(null,null));
SELECT numrange(2,null) <@ nummultirange(numrange(null,null));
SELECT numrange(null,3) <@ nummultirange(numrange(null,5));
SELECT numrange(null,8) <@ nummultirange(numrange(null,5));
SELECT numrange(8,null) <@ nummultirange(numrange(5,null));
SELECT numrange(3,null) <@ nummultirange(numrange(5,null));
SELECT numrange(8,9) <@ nummultirange(numrange(1,5));
SELECT numrange(3,9) <@ nummultirange(numrange(1,5));
SELECT numrange(1,4) <@ nummultirange(numrange(1,5));
SELECT numrange(1,5) <@ nummultirange(numrange(1,5));
SELECT numrange(1,5) <@ nummultirange(numrange(-4,-2), numrange(1,5));
SELECT numrange(1,5) <@ nummultirange(numrange(1,5), numrange(8,9));
SELECT numrange(6,7) <@ nummultirange(numrange(1,5), numrange(8,9));
SELECT numrange(6,7) <@ nummultirange(numrange(1,5), numrange(6,9));
SELECT '{[1,5)}' <@ '{[1,5)}'::nummultirange;
SELECT '{[1,5)}' <@ '{[-4,-2), [1,5)}'::nummultirange;
SELECT '{[1,5)}' <@ '{[1,5), [8,9)}'::nummultirange;
SELECT '{[6,7)}' <@ '{[1,5), [8,9)}'::nummultirange;
SELECT '{[6,7)}' <@ '{[1,5), [6,9)}'::nummultirange;
SELECT nummultirange(numrange(1,2)) <@ numrange(null,null);
SELECT nummultirange(numrange(null,2)) <@ numrange(null,null);
SELECT nummultirange(numrange(2,null)) <@ numrange(null,null);
SELECT nummultirange(numrange(null,3)) <@ numrange(null,5);
SELECT nummultirange(numrange(null,8)) <@ numrange(null,5);
SELECT nummultirange(numrange(8,null)) <@ numrange(5,null);
SELECT nummultirange(numrange(3,null)) <@ numrange(5,null);
SELECT nummultirange(numrange(8,9)) <@ numrange(1,5);
SELECT nummultirange(numrange(3,9)) <@ numrange(1,5);
SELECT nummultirange(numrange(1,4)) <@ numrange(1,5);
SELECT nummultirange(numrange(1,5)) <@ numrange(1,5);
SELECT nummultirange(numrange(-4,-2), numrange(1,5)) <@ numrange(1,9);
SELECT nummultirange(numrange(1,5), numrange(8,9)) <@ numrange(1,9);
SELECT nummultirange(numrange(1,5), numrange(6,9)) <@ numrange(1,9);
SELECT nummultirange(numrange(1,5), numrange(6,10)) <@ numrange(1,9);
SELECT '{[1,5)}'::nummultirange <@ '{[1,9)}';
SELECT '{[-4,-2), [1,5)}'::nummultirange <@ '{[1,9)}';
SELECT '{[1,5), [8,9)}'::nummultirange <@ '{[1,9)}';
SELECT '{[1,5), [6,9)}'::nummultirange <@ '{[1,9)}';
SELECT '{[1,5), [6,10)}'::nummultirange <@ '{[1,9)}';

-- overleft
SELECT 'empty'::numrange &< nummultirange();
SELECT 'empty'::numrange &< nummultirange(numrange(1,2));
SELECT nummultirange() &< 'empty'::numrange;
SELECT nummultirange(numrange(1,2)) &< 'empty'::numrange;
SELECT nummultirange() &< nummultirange();
SELECT nummultirange(numrange(1,2)) &< nummultirange();
SELECT nummultirange() &< nummultirange(numrange(1,2));
SELECT numrange(6,7) &< nummultirange(numrange(3,4));
SELECT numrange(1,2) &< nummultirange(numrange(3,4));
SELECT numrange(1,4) &< nummultirange(numrange(3,4));
SELECT numrange(1,6) &< nummultirange(numrange(3,4));
SELECT numrange(3.5,6) &< nummultirange(numrange(3,4));
SELECT nummultirange(numrange(6,7)) &< numrange(3,4);
SELECT nummultirange(numrange(1,2)) &< numrange(3,4);
SELECT nummultirange(numrange(1,4)) &< numrange(3,4);
SELECT nummultirange(numrange(1,6)) &< numrange(3,4);
SELECT nummultirange(numrange(3.5,6)) &< numrange(3,4);
SELECT nummultirange(numrange(6,7)) &< nummultirange(numrange(3,4));
SELECT nummultirange(numrange(1,2)) &< nummultirange(numrange(3,4));
SELECT nummultirange(numrange(1,4)) &< nummultirange(numrange(3,4));
SELECT nummultirange(numrange(1,6)) &< nummultirange(numrange(3,4));
SELECT nummultirange(numrange(3.5,6)) &< nummultirange(numrange(3,4));

-- overright
SELECT nummultirange() &> 'empty'::numrange;
SELECT nummultirange(numrange(1,2)) &> 'empty'::numrange;
SELECT 'empty'::numrange &> nummultirange();
SELECT 'empty'::numrange &> nummultirange(numrange(1,2));
SELECT nummultirange() &> nummultirange();
SELECT nummultirange() &> nummultirange(numrange(1,2));
SELECT nummultirange(numrange(1,2)) &> nummultirange();
SELECT nummultirange(numrange(3,4)) &> numrange(6,7);
SELECT nummultirange(numrange(3,4)) &> numrange(1,2);
SELECT nummultirange(numrange(3,4)) &> numrange(1,4);
SELECT nummultirange(numrange(3,4)) &> numrange(1,6);
SELECT nummultirange(numrange(3,4)) &> numrange(3.5,6);
SELECT numrange(3,4) &> nummultirange(numrange(6,7));
SELECT numrange(3,4) &> nummultirange(numrange(1,2));
SELECT numrange(3,4) &> nummultirange(numrange(1,4));
SELECT numrange(3,4) &> nummultirange(numrange(1,6));
SELECT numrange(3,4) &> nummultirange(numrange(3.5,6));
SELECT nummultirange(numrange(3,4)) &> nummultirange(numrange(6,7));
SELECT nummultirange(numrange(3,4)) &> nummultirange(numrange(1,2));
SELECT nummultirange(numrange(3,4)) &> nummultirange(numrange(1,4));
SELECT nummultirange(numrange(3,4)) &> nummultirange(numrange(1,6));
SELECT nummultirange(numrange(3,4)) &> nummultirange(numrange(3.5,6));

-- meets
SELECT 'empty'::numrange -|- nummultirange();
SELECT 'empty'::numrange -|- nummultirange(numrange(1,2));
SELECT nummultirange() -|- 'empty'::numrange;
SELECT nummultirange(numrange(1,2)) -|- 'empty'::numrange;
SELECT nummultirange() -|- nummultirange();
SELECT nummultirange(numrange(1,2)) -|- nummultirange();
SELECT nummultirange() -|- nummultirange(numrange(1,2));
SELECT numrange(1,2) -|- nummultirange(numrange(2,4));
SELECT numrange(1,2) -|- nummultirange(numrange(3,4));
SELECT nummultirange(numrange(1,2)) -|- numrange(2,4);
SELECT nummultirange(numrange(1,2)) -|- numrange(3,4);
SELECT nummultirange(numrange(1,2)) -|- nummultirange(numrange(2,4));
SELECT nummultirange(numrange(1,2)) -|- nummultirange(numrange(3,4));
SELECT nummultirange(numrange(1,2), numrange(5,6)) -|- nummultirange(numrange(3,4));
SELECT nummultirange(numrange(1,2), numrange(5,6)) -|- nummultirange(numrange(6,7));
SELECT nummultirange(numrange(1,2), numrange(5,6)) -|- nummultirange(numrange(8,9));
SELECT nummultirange(numrange(1,2)) -|- nummultirange(numrange(2,4), numrange(6,7));

-- strictly left
select 'empty'::numrange << nummultirange();
select numrange(1,2) << nummultirange();
select numrange(1,2) << nummultirange(numrange(3,4));
select numrange(1,2) << nummultirange(numrange(0,4));
select numrange(1,2) << nummultirange(numrange(0,4), numrange(7,8));
select nummultirange() << 'empty'::numrange;
select nummultirange() << numrange(1,2);
select nummultirange(numrange(3,4)) << numrange(3,6);
select nummultirange(numrange(0,2)) << numrange(3,6);
select nummultirange(numrange(0,2), numrange(7,8)) << numrange(3,6);
select nummultirange(numrange(-4,-2), numrange(0,2)) << numrange(3,6);
select nummultirange() << nummultirange();
select nummultirange() << nummultirange(numrange(1,2));
select nummultirange(numrange(1,2)) << nummultirange();
select nummultirange(numrange(1,2)) << nummultirange(numrange(1,2));
select nummultirange(numrange(1,2)) << nummultirange(numrange(3,4));
select nummultirange(numrange(1,2)) << nummultirange(numrange(3,4), numrange(7,8));
select nummultirange(numrange(1,2), numrange(4,5)) << nummultirange(numrange(3,4), numrange(7,8));

-- strictly right
select nummultirange() >> 'empty'::numrange;
select nummultirange() >> numrange(1,2);
select nummultirange(numrange(3,4)) >> numrange(1,2);
select nummultirange(numrange(0,4)) >> numrange(1,2);
select nummultirange(numrange(0,4), numrange(7,8)) >> numrange(1,2);
select 'empty'::numrange >> nummultirange();
select numrange(1,2) >> nummultirange();
select numrange(3,6) >> nummultirange(numrange(3,4));
select numrange(3,6) >> nummultirange(numrange(0,2));
select numrange(3,6) >> nummultirange(numrange(0,2), numrange(7,8));
select numrange(3,6) >> nummultirange(numrange(-4,-2), numrange(0,2));
select nummultirange() >> nummultirange();
select nummultirange(numrange(1,2)) >> nummultirange();
select nummultirange() >> nummultirange(numrange(1,2));
select nummultirange(numrange(1,2)) >> nummultirange(numrange(1,2));
select nummultirange(numrange(3,4)) >> nummultirange(numrange(1,2));
select nummultirange(numrange(3,4), numrange(7,8)) >> nummultirange(numrange(1,2));
select nummultirange(numrange(3,4), numrange(7,8)) >> nummultirange(numrange(1,2), numrange(4,5));

-- union
SELECT nummultirange() + nummultirange();
SELECT nummultirange() + nummultirange(numrange(1,2));
SELECT nummultirange(numrange(1,2)) + nummultirange();
SELECT nummultirange(numrange(1,2)) + nummultirange(numrange(1,2));
SELECT nummultirange(numrange(1,2)) + nummultirange(numrange(2,4));
SELECT nummultirange(numrange(1,2)) + nummultirange(numrange(3,4));
SELECT nummultirange(numrange(1,2), numrange(4,5)) + nummultirange(numrange(2,4));
SELECT nummultirange(numrange(1,2), numrange(4,5)) + nummultirange(numrange(3,4));
SELECT nummultirange(numrange(1,2), numrange(4,5)) + nummultirange(numrange(0,9));

-- merge
SELECT range_merge(nummultirange());
SELECT range_merge(nummultirange(numrange(1,2)));
SELECT range_merge(nummultirange(numrange(1,2), numrange(7,8)));

-- minus
SELECT nummultirange() - nummultirange();
SELECT nummultirange() - nummultirange(numrange(1,2));
SELECT nummultirange(numrange(1,2)) - nummultirange();
SELECT nummultirange(numrange(1,2), numrange(3,4)) - nummultirange();
SELECT nummultirange(numrange(1,2)) - nummultirange(numrange(1,2));
SELECT nummultirange(numrange(1,2)) - nummultirange(numrange(2,4));
SELECT nummultirange(numrange(1,2)) - nummultirange(numrange(3,4));
SELECT nummultirange(numrange(1,4)) - nummultirange(numrange(1,2));
SELECT nummultirange(numrange(1,4)) - nummultirange(numrange(2,3));
SELECT nummultirange(numrange(1,4)) - nummultirange(numrange(0,8));
SELECT nummultirange(numrange(1,4)) - nummultirange(numrange(0,2));
SELECT nummultirange(numrange(1,8)) - nummultirange(numrange(0,2), numrange(3,4));
SELECT nummultirange(numrange(1,8)) - nummultirange(numrange(2,3), numrange(5,null));
SELECT nummultirange(numrange(1,2), numrange(4,5)) - nummultirange(numrange(-2,0));
SELECT nummultirange(numrange(1,2), numrange(4,5)) - nummultirange(numrange(2,4));
SELECT nummultirange(numrange(1,2), numrange(4,5)) - nummultirange(numrange(3,5));
SELECT nummultirange(numrange(1,2), numrange(4,5)) - nummultirange(numrange(0,9));
SELECT nummultirange(numrange(1,3), numrange(4,5)) - nummultirange(numrange(2,9));
SELECT nummultirange(numrange(1,2), numrange(4,5)) - nummultirange(numrange(8,9));
SELECT nummultirange(numrange(1,2), numrange(4,5)) - nummultirange(numrange(-2,0), numrange(8,9));

-- intersection
SELECT nummultirange() * nummultirange();
SELECT nummultirange() * nummultirange(numrange(1,2));
SELECT nummultirange(numrange(1,2)) * nummultirange();
SELECT '{[1,3)}'::nummultirange * '{[1,5)}'::nummultirange;
SELECT '{[1,3)}'::nummultirange * '{[0,5)}'::nummultirange;
SELECT '{[1,3)}'::nummultirange * '{[0,2)}'::nummultirange;
SELECT '{[1,3)}'::nummultirange * '{[2,5)}'::nummultirange;
SELECT '{[1,4)}'::nummultirange * '{[2,3)}'::nummultirange;
SELECT '{[1,4)}'::nummultirange * '{[0,2), [3,5)}'::nummultirange;
SELECT '{[1,4), [7,10)}'::nummultirange * '{[0,8), [9,12)}'::nummultirange;
SELECT '{[1,4), [7,10)}'::nummultirange * '{[9,12)}'::nummultirange;
SELECT '{[1,4), [7,10)}'::nummultirange * '{[-5,-4), [5,6), [9,12)}'::nummultirange;
SELECT '{[1,4), [7,10)}'::nummultirange * '{[0,2), [3,8), [9,12)}'::nummultirange;
SELECT '{[1,4), [7,10)}'::nummultirange * '{[0,2), [3,8), [9,12)}'::nummultirange;

-- test GiST index
--dg-skip: type ? is not supported yet
--dg create table test_multirange_gist(mr int4multirange);
--dg-cascade
--dg insert into test_multirange_gist select int4multirange(int4range(g, g+10),int4range(g+20, g+30),int4range(g+40, g+50)) from generate_series(1,2000) g;
--dg-cascade
--dg insert into test_multirange_gist select '{}'::int4multirange from generate_series(1,500) g;
--dg-cascade
--dg insert into test_multirange_gist select int4multirange(int4range(g, g+10000)) from generate_series(1,1000) g;
--dg-cascade
--dg insert into test_multirange_gist select int4multirange(int4range(NULL, g*10, '(]'), int4range(g*10, g*20, '(]')) from generate_series(1,100) g;
--dg-cascade
--dg insert into test_multirange_gist select int4multirange(int4range(g*10, g*20, '(]'), int4range(g*20, NULL, '(]')) from generate_series(1,100) g;
--dg-cascade
--dg create index test_mulrirange_gist_idx on test_multirange_gist using gist (mr);

-- test statistics and selectivity estimation as well
--
-- We don't check the accuracy of selectivity estimation, but at least check
-- it doesn't fall.
--dg-cascade
--dg analyze test_multirange_gist;

-- first, verify non-indexed results
SET enable_seqscan    = t;
SET enable_indexscan  = f;
SET enable_bitmapscan = f;

--dg-cascade
--dg select count(*) from test_multirange_gist where mr = '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr && 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr <@ 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr << 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr >> 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &< 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &> 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr -|- 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr && '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr <@ '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr << '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr >> '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &< '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &> '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr -|- '{}'::int4multirange;

--dg-cascade
--dg select count(*) from test_multirange_gist where mr = int4multirange(int4range(10,20), int4range(30,40), int4range(50,60));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> 10;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> int4range(10,20);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr && int4range(10,20);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr <@ int4range(10,50);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr << int4range(100,500);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr >> int4range(100,500);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &< int4range(100,500);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &> int4range(100,500);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr -|- int4range(100,500);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> int4multirange(int4range(10,20), int4range(30,40));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr && '{(10,20),(30,40),(50,60)}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr <@ '{(10,30),(40,60),(70,90)}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr << int4multirange(int4range(100,200), int4range(400,500));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr >> int4multirange(int4range(100,200), int4range(400,500));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &< int4multirange(int4range(100,200), int4range(400,500));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &> int4multirange(int4range(100,200), int4range(400,500));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr -|- int4multirange(int4range(100,200), int4range(400,500));

-- now check same queries using index
SET enable_seqscan    = f;
SET enable_indexscan  = t;
SET enable_bitmapscan = f;

--dg-cascade
--dg select count(*) from test_multirange_gist where mr = '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr && 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr <@ 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr << 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr >> 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &< 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &> 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr -|- 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr && '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr <@ '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr << '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr >> '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &< '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &> '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr -|- '{}'::int4multirange;

--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> 'empty'::int4range;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr = int4multirange(int4range(10,20), int4range(30,40), int4range(50,60));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> 10;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> int4range(10,20);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr && int4range(10,20);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr <@ int4range(10,50);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr << int4range(100,500);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr >> int4range(100,500);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &< int4range(100,500);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &> int4range(100,500);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr -|- int4range(100,500);
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> '{}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr @> int4multirange(int4range(10,20), int4range(30,40));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr && '{(10,20),(30,40),(50,60)}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr <@ '{(10,30),(40,60),(70,90)}'::int4multirange;
--dg-cascade
--dg select count(*) from test_multirange_gist where mr << int4multirange(int4range(100,200), int4range(400,500));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr >> int4multirange(int4range(100,200), int4range(400,500));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &< int4multirange(int4range(100,200), int4range(400,500));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr &> int4multirange(int4range(100,200), int4range(400,500));
--dg-cascade
--dg select count(*) from test_multirange_gist where mr -|- int4multirange(int4range(100,200), int4range(400,500));

--dg-cascade
--dg drop table test_multirange_gist;

--
-- range_agg function
--
--dg-skip: type ? is not supported yet
--dg create table reservations ( room_id integer not null, booked_during daterange );
--dg-cascade
--dg insert into reservations values
--dg -- 1: has a meets and a gap
--dg (1, daterange('2018-07-01', '2018-07-07')),
--dg (1, daterange('2018-07-07', '2018-07-14')),
--dg (1, daterange('2018-07-20', '2018-07-22')),
--dg -- 2: just a single row
--dg (2, daterange('2018-07-01', '2018-07-03')),
--dg -- 3: one null range
--dg (3, NULL),
--dg -- 4: two null ranges
--dg (4, NULL),
--dg (4, NULL),
--dg -- 5: a null range and a non-null range
--dg (5, NULL),
--dg (5, daterange('2018-07-01', '2018-07-03')),
--dg -- 6: has overlap
--dg (6, daterange('2018-07-01', '2018-07-07')),
--dg (6, daterange('2018-07-05', '2018-07-10')),
--dg -- 7: two ranges that meet: no gap or overlap
--dg (7, daterange('2018-07-01', '2018-07-07')),
--dg (7, daterange('2018-07-07', '2018-07-14')),
--dg -- 8: an empty range
--dg (8, 'empty'::daterange)
--dg ;
--dg-cascade
--dg SELECT   room_id, range_agg(booked_during)
--dg FROM     reservations
--dg GROUP BY room_id
--dg ORDER BY room_id;

-- range_agg on a custom range type too
SELECT  range_agg(r)
FROM    (VALUES
          ('[a,c]'::textrange),
          ('[b,b]'::textrange),
          ('[c,f]'::textrange),
          ('[g,h)'::textrange),
          ('[h,j)'::textrange)
        ) t(r);

-- range_agg with multirange inputs
--dg-cascade
--dg select range_agg(nmr) from nummultirange_test;
--dg-cascade
--dg select range_agg(nmr) from nummultirange_test where false;
--dg-cascade
--dg select range_agg(null::nummultirange) from nummultirange_test;
select range_agg(nmr) from (values ('{}'::nummultirange)) t(nmr);
select range_agg(nmr) from (values ('{}'::nummultirange), ('{}'::nummultirange)) t(nmr);
select range_agg(nmr) from (values ('{[1,2]}'::nummultirange)) t(nmr);
select range_agg(nmr) from (values ('{[1,2], [5,6]}'::nummultirange)) t(nmr);
select range_agg(nmr) from (values ('{[1,2], [2,3]}'::nummultirange)) t(nmr);
select range_agg(nmr) from (values ('{[1,2]}'::nummultirange), ('{[5,6]}'::nummultirange)) t(nmr);
select range_agg(nmr) from (values ('{[1,2]}'::nummultirange), ('{[2,3]}'::nummultirange)) t(nmr);

--
-- range_intersect_agg function
--
--dg-cascade
--dg select range_intersect_agg(nmr) from nummultirange_test;
--dg-cascade
--dg select range_intersect_agg(nmr) from nummultirange_test where false;
--dg-cascade
--dg select range_intersect_agg(null::nummultirange) from nummultirange_test;
select range_intersect_agg(nmr) from (values ('{[1,3]}'::nummultirange), ('{[6,12]}'::nummultirange)) t(nmr);
select range_intersect_agg(nmr) from (values ('{[1,6]}'::nummultirange), ('{[3,12]}'::nummultirange)) t(nmr);
select range_intersect_agg(nmr) from (values ('{[1,6], [10,12]}'::nummultirange), ('{[4,14]}'::nummultirange)) t(nmr);
-- test with just one input:
select range_intersect_agg(nmr) from (values ('{}'::nummultirange)) t(nmr);
select range_intersect_agg(nmr) from (values ('{[1,2]}'::nummultirange)) t(nmr);
select range_intersect_agg(nmr) from (values ('{[1,6], [10,12]}'::nummultirange)) t(nmr);
--dg-cascade
--dg select range_intersect_agg(nmr) from nummultirange_test where nmr @> 4.0;

--dg-skip: type ? is not supported yet
--dg create table nummultirange_test2(nmr nummultirange);
--dg-cascade
--dg create index nummultirange_test2_hash_idx on nummultirange_test2 using hash (nmr);

--dg-cascade
--dg INSERT INTO nummultirange_test2 VALUES('{[, 5)}');
--dg-cascade
--dg INSERT INTO nummultirange_test2 VALUES(nummultirange(numrange(1.1, 2.2)));
--dg-cascade
--dg INSERT INTO nummultirange_test2 VALUES(nummultirange(numrange(1.1, 2.2)));
--dg-cascade
--dg INSERT INTO nummultirange_test2 VALUES(nummultirange(numrange(1.1, 2.2,'()')));
--dg-cascade
--dg INSERT INTO nummultirange_test2 VALUES('{}');

--dg-cascade
--dg select * from nummultirange_test2 where nmr = '{}';
--dg-cascade
--dg select * from nummultirange_test2 where nmr = nummultirange(numrange(1.1, 2.2));
--dg-cascade
--dg select * from nummultirange_test2 where nmr = nummultirange(numrange(1.1, 2.3));

set enable_nestloop=t;
set enable_hashjoin=f;
set enable_mergejoin=f;
--dg-cascade
--dg select * from nummultirange_test natural join nummultirange_test2 order by nmr;
set enable_nestloop=f;
set enable_hashjoin=t;
set enable_mergejoin=f;
--dg-cascade
--dg select * from nummultirange_test natural join nummultirange_test2 order by nmr;
set enable_nestloop=f;
set enable_hashjoin=f;
set enable_mergejoin=t;
--dg-cascade
--dg select * from nummultirange_test natural join nummultirange_test2 order by nmr;

set enable_nestloop to default;
set enable_hashjoin to default;
set enable_mergejoin to default;

--dg-cascade
--dg DROP TABLE nummultirange_test2;

--
-- Test user-defined multirange of floats
--

select '{[123.001, 5.e9)}'::float8multirange @> 888.882::float8;
create table float8multirange_test(f8mr float8multirange, i int);
insert into float8multirange_test values(float8multirange(float8range(-100.00007, '1.111113e9')), 42);
select * from float8multirange_test;
drop table float8multirange_test;

--
-- Test multirange types over domains
--

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create domain mydomain as int4;
--dg-cascade
--dg create type mydomainrange as range(subtype=mydomain);
--dg-cascade
--dg select '{[4,50)}'::mydomainmultirange @> 7::mydomain;
--dg-cascade
--dg drop domain mydomain cascade;

--
-- Test domains over multirange types
--

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create domain restrictedmultirange as int4multirange check (upper(value) < 10);
--dg-cascade
--dg select '{[4,5)}'::restrictedmultirange @> 7;
--dg-cascade
--dg select '{[4,50)}'::restrictedmultirange @> 7; -- should fail
--dg-cascade
--dg drop domain restrictedmultirange;

---
-- Check automatic naming of multiranges
---

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type intr as range(subtype=int);
--dg-cascade
--dg select intr_multirange(intr(1,10));
--dg-cascade
--dg drop type intr;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg create type intmultirange as (x int, y int);
--dg-cascade
--dg create type intrange as range(subtype=int); -- should fail
--dg-cascade
--dg drop type intmultirange;
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg create type intr_multirange as (x int, y int);
--dg-cascade
--dg create type intr as range(subtype=int); -- should fail
--dg-cascade
--dg drop type intr_multirange;

--
-- Test multiple multirange types over the same subtype and manual naming of
-- the multirange type.
--

-- should fail
--dg-differs: another error: relation pg_type cannot be written: digres has no catalog tables
--dg create type textrange1 as range(subtype=text, multirange_type_name=int, collation="C");
-- should pass
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type textrange1 as range(subtype=text, multirange_type_name=multirange_of_text, collation="C");
-- should pass, because existing _textrange1 is automatically renamed
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type textrange2 as range(subtype=text, multirange_type_name=_textrange1, collation="C");

--dg-cascade
--dg select multirange_of_text(textrange2('a','Z'));  -- should fail
--dg-cascade
--dg select multirange_of_text(textrange1('a','Z')) @> 'b'::text;
--dg-cascade
--dg select unnest(multirange_of_text(textrange1('a','b'), textrange1('d','e')));
--dg-cascade
--dg select _textrange1(textrange2('a','z')) @> 'b'::text;

--dg-cascade
--dg drop type textrange1;
--dg-cascade
--dg drop type textrange2;

--
-- Multiranges don't have their own ownership or permissions.
--
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type textrange1 as range(subtype=text, multirange_type_name=multitextrange1, collation="C");
create role regress_multirange_owner;

--dg-cascade
--dg alter type multitextrange1 owner to regress_multirange_owner;  -- fail
--dg-cascade
--dg alter type textrange1 owner to regress_multirange_owner;
set role regress_multirange_owner;
--dg-cascade
--dg revoke usage on type multitextrange1 from public;  -- fail
--dg-cascade
--dg revoke usage on type textrange1 from public;
--dg-cascade
--dg \dT+ *textrange1*
--dg-cascade
--dg create temp table test1(f1 multitextrange1[]);
--dg-cascade
--dg revoke usage on type textrange1 from regress_multirange_owner;
--dg-cascade
--dg create temp table test2(f1 multitextrange1[]);  -- fail

--dg-cascade
--dg drop table test1;
--dg-cascade
--dg drop type textrange1;
reset role;
drop role regress_multirange_owner;

--
-- CREATE TYPE checks for CREATE on multirange schema
--
create role regress_mr;
create schema mr_sch;
set role regress_mr;
create type mytype as range (subtype=int4, multirange_type_name=mr_sch.mr_type);
reset role;
--dg-skip: relation pg_namespace cannot be written: digres has no catalog tables
--dg grant create on schema mr_sch to regress_mr;
set role regress_mr;
create type mytype as range (subtype=int4, multirange_type_name=mr_sch.mr_type);
reset role;
drop type mytype;
drop schema mr_sch;
drop role regress_mr;

--
-- Test polymorphic type system
--

--dg-skip: language "?" does not exist
--dg create function anyarray_anymultirange_func(a anyarray, r anymultirange)
--dg   returns anyelement as 'select $1[1] + lower($2);' language sql;

--dg-cascade
--dg select anyarray_anymultirange_func(ARRAY[1,2], int4multirange(int4range(10,20)));

-- should fail
select anyarray_anymultirange_func(ARRAY[1,2], nummultirange(numrange(10,20)));

--dg-cascade
--dg drop function anyarray_anymultirange_func(anyarray, anymultirange);

-- should fail
--dg-differs: another error: language "?" does not exist
--dg create function bogus_func(anyelement)
--dg   returns anymultirange as 'select int4multirange(int4range(1,10))' language sql;

-- should fail
--dg-differs: another error: language "?" does not exist
--dg create function bogus_func(int)
--dg   returns anymultirange as 'select int4multirange(int4range(1,10))' language sql;

--dg-skip: language "?" does not exist
--dg create function range_add_bounds(anymultirange)
--dg   returns anyelement as 'select lower($1) + upper($1)' language sql;

--dg-cascade
--dg select range_add_bounds(int4multirange(int4range(1, 17)));
--dg-cascade
--dg select range_add_bounds(nummultirange(numrange(1.0001, 123.123)));

--dg-skip: language "?" does not exist
--dg create function multirangetypes_sql(q anymultirange, b anyarray, out c anyelement)
--dg   as $$ select upper($1) + $2[1] $$
--dg   language sql;

--dg-cascade
--dg select multirangetypes_sql(int4multirange(int4range(1,10)), ARRAY[2,20]);
select multirangetypes_sql(nummultirange(numrange(1,10)), ARRAY[2,20]);  -- match failure

--dg-skip: language "?" does not exist
--dg create function anycompatiblearray_anycompatiblemultirange_func(a anycompatiblearray, mr anycompatiblemultirange)
--dg   returns anycompatible as 'select $1[1] + lower($2);' language sql;

--dg-cascade
--dg select anycompatiblearray_anycompatiblemultirange_func(ARRAY[1,2], multirange(int4range(10,20)));

--dg-cascade
--dg select anycompatiblearray_anycompatiblemultirange_func(ARRAY[1,2], multirange(numrange(10,20)));

-- should fail
select anycompatiblearray_anycompatiblemultirange_func(ARRAY[1.1,2], multirange(int4range(10,20)));

--dg-cascade
--dg drop function anycompatiblearray_anycompatiblemultirange_func(anycompatiblearray, anycompatiblemultirange);

--dg-skip: language "?" does not exist
--dg create function anycompatiblerange_anycompatiblemultirange_func(r anycompatiblerange, mr anycompatiblemultirange)
--dg   returns anycompatible as 'select lower($1) + lower($2);' language sql;

--dg-cascade
--dg select anycompatiblerange_anycompatiblemultirange_func(int4range(1,2), multirange(int4range(10,20)));

-- should fail
select anycompatiblerange_anycompatiblemultirange_func(numrange(1,2), multirange(int4range(10,20)));

--dg-cascade
--dg drop function anycompatiblerange_anycompatiblemultirange_func(anycompatiblerange, anycompatiblemultirange);

-- should fail
--dg-differs: another error: language "?" does not exist
--dg create function bogus_func(anycompatible)
--dg   returns anycompatiblerange as 'select int4range(1,10)' language sql;

--
-- Arrays of multiranges
--

select ARRAY[nummultirange(numrange(1.1, 1.2)), nummultirange(numrange(12.3, 155.5))];

--dg-skip: type ? is not supported yet
--dg create table i8mr_array (f1 int, f2 int8multirange[]);
--dg-cascade
--dg insert into i8mr_array values (42, array[int8multirange(int8range(1,10)), int8multirange(int8range(2,20))]);
--dg-cascade
--dg select * from i8mr_array;
--dg-cascade
--dg drop table i8mr_array;

--
-- Multiranges of arrays
--

select arraymultirange(arrayrange(ARRAY[1,2], ARRAY[2,1]));
select arraymultirange(arrayrange(ARRAY[2,1], ARRAY[1,2]));  -- fail

select array[1,1] <@ arraymultirange(arrayrange(array[1,2], array[2,1]));
select array[1,3] <@ arraymultirange(arrayrange(array[1,2], array[2,1]));

--
-- Ranges of composites
--

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg create type two_ints as (a int, b int);
--dg-cascade
--dg create type two_ints_range as range (subtype = two_ints);

-- with debug_parallel_query on, this exercises tqueue.c's range remapping
--dg-cascade
--dg select *, row_to_json(upper(t)) as u from
--dg   (values (two_ints_multirange(two_ints_range(row(1,2), row(3,4)))),
--dg           (two_ints_multirange(two_ints_range(row(5,6), row(7,8))))) v(t);

-- this must be rejected to avoid self-inclusion issues:
--dg-cascade
--dg alter type two_ints add attribute c two_ints_multirange;

--dg-cascade
--dg drop type two_ints cascade;

--
-- Check behavior when subtype lacks a hash function
--

set enable_sort = off;  -- try to make it pick a hash setop implementation

select '{(01,10)}'::varbitmultirange except select '{(10,11)}'::varbitmultirange;

reset enable_sort;

--
-- OUT/INOUT/TABLE functions
--

-- infer anymultirange from anymultirange
--dg-skip: language "?" does not exist
--dg create function mr_outparam_succeed(i anymultirange, out r anymultirange, out t text)
--dg   as $$ select $1, 'foo'::text $$ language sql;

--dg-cascade
--dg select * from mr_outparam_succeed(int4multirange(int4range(1,2)));

-- infer anyarray from anymultirange
--dg-skip: language "?" does not exist
--dg create function mr_outparam_succeed2(i anymultirange, out r anyarray, out t text)
--dg   as $$ select ARRAY[upper($1)], 'foo'::text $$ language sql;

--dg-cascade
--dg select * from mr_outparam_succeed2(int4multirange(int4range(1,2)));

-- infer anyrange from anymultirange
--dg-skip: language "?" does not exist
--dg create function mr_outparam_succeed3(i anymultirange, out r anyrange, out t text)
--dg   as $$ select range_merge($1), 'foo'::text $$ language sql;
--dg-cascade
--dg select * from mr_outparam_succeed3(int4multirange(int4range(1,2)));

-- infer anymultirange from anyrange
--dg-skip: language "?" does not exist
--dg create function mr_outparam_succeed4(i anyrange, out r anymultirange, out t text)
--dg   as $$ select multirange($1), 'foo'::text $$ language sql;

--dg-cascade
--dg select * from mr_outparam_succeed4(int4range(1,2));

-- infer anyelement from anymultirange
--dg-skip: language "?" does not exist
--dg create function mr_inoutparam_succeed(out i anyelement, inout r anymultirange)
--dg   as $$ select upper($1), $1 $$ language sql;

--dg-cascade
--dg select * from mr_inoutparam_succeed(int4multirange(int4range(1,2)));

-- infer anyelement+anymultirange from anyelement+anymultirange
--dg-skip: language "?" does not exist
--dg create function mr_table_succeed(i anyelement, r anymultirange) returns table(i anyelement, r anymultirange)
--dg   as $$ select $1, $2 $$ language sql;

--dg-cascade
--dg select * from mr_table_succeed(123, int4multirange(int4range(1,11)));

-- use anymultirange in plpgsql
--dg-skip: language "?" does not exist
--dg create function mr_polymorphic(i anyrange) returns anymultirange
--dg   as $$ begin return multirange($1); end; $$ language plpgsql;
--dg-cascade
--dg select mr_polymorphic(int4range(1, 4));

-- should fail
--dg-differs: another error: language "?" does not exist
--dg create function mr_outparam_fail(i anyelement, out r anymultirange, out t text)
--dg   as $$ select '[1,10]', 'foo' $$ language sql;

--should fail
--dg-differs: another error: language "?" does not exist
--dg create function mr_inoutparam_fail(inout i anyelement, out r anymultirange)
--dg   as $$ select $1, '[1,10]' $$ language sql;

--should fail
--dg-differs: another error: language "?" does not exist
--dg create function mr_table_fail(i anyelement) returns table(i anyelement, r anymultirange)
--dg   as $$ select $1, '[1,10]' $$ language sql;
