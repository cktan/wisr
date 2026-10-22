--
-- PARALLEL
--

begin;

-- encourage use of parallel plans
--dg-skip: unrecognized configuration parameter "?"
--dg set parallel_setup_cost=0;
--dg-skip: unrecognized configuration parameter "?"
--dg set parallel_tuple_cost=0;
--dg-skip: unrecognized configuration parameter "?"
--dg set min_parallel_table_scan_size=0;
--dg-skip: unrecognized configuration parameter "?"
--dg set max_parallel_workers_per_gather=4;

--
-- Test write operations that has an underlying query that is eligible
-- for parallel plans
--
--dg-cascade
--dg explain (costs off) create table parallel_write as
--dg     select length(stringu1) from tenk1 group by length(stringu1);
create table parallel_write as
    select length(stringu1) from tenk1 group by length(stringu1);
drop table parallel_write;

--dg-cascade
--dg explain (costs off) select length(stringu1) into parallel_write
--dg     from tenk1 group by length(stringu1);
select length(stringu1) into parallel_write
    from tenk1 group by length(stringu1);
drop table parallel_write;

--dg-cascade
--dg explain (costs off) create materialized view parallel_mat_view as
--dg     select length(stringu1) from tenk1 group by length(stringu1);
--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg create materialized view parallel_mat_view as
--dg     select length(stringu1) from tenk1 group by length(stringu1);
--dg-cascade
--dg create unique index on parallel_mat_view(length);
--dg-cascade
--dg refresh materialized view parallel_mat_view;
--dg-cascade
--dg refresh materialized view concurrently parallel_mat_view;
--dg-cascade
--dg drop materialized view parallel_mat_view;

--dg-cascade
--dg prepare prep_stmt as select length(stringu1) from tenk1 group by length(stringu1);
--dg-cascade
--dg explain (costs off) create table parallel_write as execute prep_stmt;
--dg-cascade
--dg create table parallel_write as execute prep_stmt;
--dg-cascade
--dg drop table parallel_write;

rollback;
