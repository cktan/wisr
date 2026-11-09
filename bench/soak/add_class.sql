-- A student adds a class that has room: an insert. A full class
-- or a class held already adds nothing. The count and the insert
-- run serializable: two students must not take the last seat.
-- Classes are uniform, not Zipf: with hot classes full, adds fail
-- and deletes do not, and the table would shrink without end.
\set s random(1, 20000)
\set c random(1, 2500)
begin isolation level serializable;
insert into enrollment
  select :s, cl.id, null from class cl
  where cl.id = :c
    and (select count(*) from enrollment where class_id = cl.id)
        < cl.capacity
  on conflict do nothing;
commit;
