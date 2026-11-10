-- Verify that a numrange keeps its bounds wherever an array or a
-- multirange puts it, as its alignment is its decimal128 bounds'
-- (ISSUE.md dg issue 3), that its bounds are decimal128 whichever decimal
-- they are made of, and that a decimal64 is an element of one.
-- 1. An array of three numranges: the second starts 4 bytes past an
--    8-byte boundary, where a range aligned to 4 is put.
-- 2. An array of two with bounds of several digits.
-- 3. A multirange given its ranges out of order, which it sorts.
-- 4. numrange, nummultirange and their arrays align as decimal128 does.
-- 5. A numrange of numeric(10,2) decimal64s holds them as decimal128
--    bounds, alone, in an array and in a multirange; no table, as the
--    8 sessions run.sh starts share one database.
-- 6. A decimal64 is found in a numrange or not, by @>, <@,
--    range_contains_elem and elem_contained_by_range, as a numeric is
--    in PostgreSQL.
select ARRAY[numrange(1, 2), numrange(3, 4), numrange(5, 6)];
select ARRAY[numrange(1.1, 1.2), numrange(12.3, 155.5)];
select nummultirange(numrange(5.0, 6.0), numrange(1.0, 2.0));
select typname, typalign from pg_type
  where typname in ('decimal128', 'numrange', '_numrange', 'nummultirange',
                    '_nummultirange')
  order by typname;
select numrange(x, y), lower(numrange(x, y)),
       ARRAY[numrange(1.1, 1.2), numrange(x, y)],
       nummultirange(numrange(x, y), numrange(0.5, 1.0))
  from (values (1.25::numeric(10,2), 3.75::numeric(10,2)),
               (10.5::numeric(10,2), 20.25::numeric(10,2))) d(x, y)
  order by x;
explain (verbose, costs off) select numrange(x, y)
  from (values (1.25::numeric(10,2), 3.75::numeric(10,2)),
               (10.5::numeric(10,2), 20.25::numeric(10,2))) d(x, y);
select numrange(1.5, 2.5) @> 2.0, numrange(1.5, 2.5) @> 3.0,
       2.0 <@ numrange(1.5, 2.5),
       range_contains_elem(numrange(1.5, 2.5), 2.0),
       elem_contained_by_range(2.0, numrange(1.5, 2.5));
select x, numrange(1, 5) @> x, x <@ numrange(1, 5)
  from (values (1.25::numeric(10,2)), (10.5::numeric(10,2))) d(x)
  order by x;
