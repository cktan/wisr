-- Verify generated columns as PostgreSQL 18 has them (../sm/GENERATED.md):
-- a stored one computed by the executor on every write, a virtual one by
-- the rewriter on every read, sm3 keeping the kind, the expression and
-- the columns it reads.  Temporary tables, as the 8 sessions run.sh
-- starts share one database.
-- 1. A table with a stored and a virtual column: INSERT, UPDATE and COPY
--    give the stored one its value; a read computes the virtual one.
-- 2. A value given for either is refused; an index on the stored one is
--    used, one on the virtual one refused.
-- 3. ALTER COLUMN TYPE of a column an expression reads, and of a
--    generated column, SET DEFAULT of one, and a NOT NULL virtual column
--    are refused.
-- 4. DROP COLUMN of a column an expression reads is refused; with
--    CASCADE the generated columns go too, once no index is on them.
-- 5. pg_attribute and pg_attrdef show them.
-- 6. A table whose stored column is dropped takes rows; LIKE ... INCLUDING
--    GENERATED is refused.
-- 7. An expression reading its own column or another generated one is
--    refused; one reading tableoid is taken; a check on a virtual column
--    reads its expression when it checks the rows there.
-- The refusals are what is tested, so an error goes on to the next
-- statement.
\set ON_ERROR_STOP off
create temp table item (id int primary key, price int8, qty int4,
  total int8 generated always as (price * qty) stored,
  label text generated always as ('#' || qty::text));
insert into item (id, price, qty) values (1, 25, 4);
update item set qty = 5 where id = 1;
copy item (id, price, qty) from stdin;
2	10	2
\.
select * from item order by id;
select id, label from item where total > 50 order by id;
insert into item (id, total) values (3, 1);
update item set label = 'x';
create index item_label on item (label);
create index item_total on item (total);
set enable_seqscan = off;
select id from item where total = 20;
reset enable_seqscan;
alter table item alter column qty type int8;
alter table item alter column total type int4;
alter table item alter column total set default 1;
alter table item alter column label drop default;
create temp table bad (a int, b int generated always as (a * 2) virtual not null);
alter table item drop column qty;
select attname, attgenerated from pg_attribute
  where attrelid = 'item'::regclass and attnum > 0 order by attnum;
select pg_get_expr(adbin, adrelid) from pg_attrdef
  where adrelid = 'item'::regclass order by adnum;
drop index item_total;
alter table item drop column qty cascade;
select * from item order by id;
drop table item;
create temp table gd (a int primary key, b int generated always as (a * 2) stored);
alter table gd drop column b;
insert into gd values (1);
select * from gd;
drop table gd;
create temp table gs (a int, b int generated always as (a * 2) stored);
create temp table gl (like gs including generated);
create temp table gp (like gs);
select attname, attgenerated from pg_attribute
  where attrelid = 'gp'::regclass and attnum > 0 order by attnum;
drop table gs;
drop table gp;
create temp table e2 (a int, b int generated always as (b * 2) stored);
create temp table e3 (a int, b int generated always as (c * 2) stored,
                      c int generated always as (a * 2));
create temp table gto (a int, b bool generated always as
                       (tableoid = 'gto'::regclass) stored);
insert into gto values (1);
select * from gto;
drop table gto;
create temp table g20 (a int, b int generated always as (a * 2) virtual);
insert into g20 (a) values (10), (30);
alter table g20 add check (b < 50);
alter table g20 add constraint chk check (b < 70) not valid;
alter table g20 validate constraint chk;
insert into g20 (a) values (40);
drop table g20;
