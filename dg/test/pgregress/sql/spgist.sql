--
-- Test SP-GiST indexes.
--
-- There are other tests to test different SP-GiST opclasses. This is for
-- testing SP-GiST code itself.

--dg-skip: type ? is not supported yet
--dg create table spgist_point_tbl(id int4, p point);
--dg-cascade
--dg create index spgist_point_idx on spgist_point_tbl using spgist(p) with (fillfactor = 75);

-- Test vacuum-root operation. It gets invoked when the root is also a leaf,
-- i.e. the index is very small.
--dg-cascade
--dg insert into spgist_point_tbl (id, p)
--dg select g, point(g*10, g*10) from generate_series(1, 10) g;
--dg-cascade
--dg delete from spgist_point_tbl where id < 5;
--dg-cascade
--dg vacuum spgist_point_tbl;

-- Insert more data, to make the index a few levels deep.
--dg-cascade
--dg insert into spgist_point_tbl (id, p)
--dg select g,      point(g*10, g*10) from generate_series(1, 10000) g;
--dg-cascade
--dg insert into spgist_point_tbl (id, p)
--dg select g+100000, point(g*10+1, g*10+1) from generate_series(1, 10000) g;

-- To test vacuum, delete some entries from all over the index.
--dg-cascade
--dg delete from spgist_point_tbl where id % 2 = 1;

-- And also delete some concentration of values. (SP-GiST doesn't currently
-- attempt to delete pages even when they become empty, but if it did, this
-- would exercise it)
--dg-cascade
--dg delete from spgist_point_tbl where id < 10000;

--dg-cascade
--dg vacuum spgist_point_tbl;

-- Test rescan paths (cf. bug #15378)
-- use box and && rather than point, so that rescan happens when the
-- traverse stack is non-empty

--dg-skip: type ? is not supported yet
--dg create table spgist_box_tbl(id serial, b box);
--dg-cascade
--dg insert into spgist_box_tbl(b)
--dg select box(point(i,j),point(i+s,j+s))
--dg   from generate_series(1,100,5) i,
--dg        generate_series(1,100,5) j,
--dg        generate_series(1,10) s;
--dg-cascade
--dg create index spgist_box_idx on spgist_box_tbl using spgist (b);

--dg-cascade
--dg select count(*)
--dg   from (values (point(5,5)),(point(8,8)),(point(12,12))) v(p)
--dg  where exists(select * from spgist_box_tbl b where b.b && box(v.p,v.p));

-- The point opclass's choose method only uses the spgMatchNode action,
-- so the other actions are not tested by the above. Create an index using
-- text opclass, which uses the others actions.

create table spgist_text_tbl(id int4, t text);
--dg-skip: access method "?" is not supported
--dg create index spgist_text_idx on spgist_text_tbl using spgist(t);

insert into spgist_text_tbl (id, t)
select g, 'f' || repeat('o', 100) || g from generate_series(1, 10000) g
union all
select g, 'baaaaaaaaaaaaaar' || g from generate_series(1, 1000) g;

-- Do a lot of insertions that have to split an existing node. Hopefully
-- one of these will cause the page to run out of space, causing the inner
-- tuple to be moved to another page.
insert into spgist_text_tbl (id, t)
select -g, 'f' || repeat('o', 100-g) || 'surprise' from generate_series(1, 100) g;

-- Test out-of-range fillfactor values
--dg-cascade
--dg create index spgist_point_idx2 on spgist_point_tbl using spgist(p) with (fillfactor = 9);
--dg-cascade
--dg create index spgist_point_idx2 on spgist_point_tbl using spgist(p) with (fillfactor = 101);

-- Modify fillfactor in existing index
--dg-skip: ALTER INDEX is not supported yet
--dg alter index spgist_point_idx set (fillfactor = 90);
--dg-cascade
--dg reindex index spgist_point_idx;

-- Test index over a domain
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create domain spgist_text as varchar;
--dg-cascade
--dg create table spgist_domain_tbl (f1 spgist_text);
--dg-cascade
--dg create index spgist_domain_idx on spgist_domain_tbl using spgist(f1);
--dg-cascade
--dg insert into spgist_domain_tbl values('fee'), ('fi'), ('fo'), ('fum');
--dg-cascade
--dg explain (costs off)
--dg select * from spgist_domain_tbl where f1 = 'fo';
--dg-cascade
--dg select * from spgist_domain_tbl where f1 = 'fo';

-- test an unlogged table, mostly to get coverage of spgistbuildempty
--dg-skip: CREATE SEQUENCE supports only START and INCREMENT yet, not TEMPORARY or UNLOGGED
--dg create unlogged table spgist_unlogged_tbl(id serial, b box);
--dg-cascade
--dg create index spgist_unlogged_idx on spgist_unlogged_tbl using spgist (b);
--dg-cascade
--dg insert into spgist_unlogged_tbl(b)
--dg select box(point(i,j))
--dg   from generate_series(1,100,5) i,
--dg        generate_series(1,10,5) j;
-- leave this table around, to help in testing dump/restore
