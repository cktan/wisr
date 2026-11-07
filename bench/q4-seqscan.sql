-- q4-seqscan.sql -- the sum of one unindexed column over the whole table:
-- a sequential scan on both systems, with no index and no lookup.
-- zip is text of digits, so the cast makes it a number.
--
--   pgbench -d bench -n -f q4-seqscan.sql

select sum(zip::int) from usertable;
