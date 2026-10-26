-- Verify that a numrange keeps its bounds wherever an array or a
-- multirange puts it, as its alignment is its decimal128 bounds'
-- (ISSUE.md issue 3).
-- 1. An array of three numranges: the second starts 4 bytes past an
--    8-byte boundary, where a range aligned to 4 is put.
-- 2. An array of two with bounds of several digits.
-- 3. A multirange given its ranges out of order, which it sorts.
-- 4. numrange, nummultirange and their arrays align as decimal128 does.
select ARRAY[numrange(1, 2), numrange(3, 4), numrange(5, 6)];
select ARRAY[numrange(1.1, 1.2), numrange(12.3, 155.5)];
select nummultirange(numrange(5.0, 6.0), numrange(1.0, 2.0));
select typname, typalign from pg_type
  where typname in ('decimal128', 'numrange', '_numrange', 'nummultirange',
                    '_nummultirange')
  order by typname;
