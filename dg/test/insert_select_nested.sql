-- Verify that an insert can consume another insert's RETURNING rows.
-- 1. Create an empty table with a primary key.
-- 2. Insert query rows and consume them in an insert into the same table.
-- 3. Confirm that both inserts stored their rows.
-- 4. Repeat without ON CONFLICT to check two eligible bulk inserts.
create temp table isn (id int primary key);
with r as (
  insert into isn select i from generate_series(1, 3) i returning id
)
insert into isn select id + 10 from r on conflict do nothing;
select id from isn order by id;
truncate isn;
with r as (
  insert into isn select i from generate_series(1, 3) i returning id
)
insert into isn select id + 10 from r;
select id from isn order by id;
