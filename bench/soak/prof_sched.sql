-- A prof reads the classes taught.
\set p random(1, 1000)
select id, slot, capacity from class where prof_id = :p;
