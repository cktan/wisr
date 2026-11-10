-- A student reads the schedule: classes, slots and profs.
\set s random(1, 20000)
select c.id, c.slot, p.name from enrollment e
  join class c on c.id = e.class_id
  join prof p on p.id = c.prof_id
  where e.student_id = :s;
