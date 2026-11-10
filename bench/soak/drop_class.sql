-- A student drops the first class held: a delete of one row. The
-- IN form is deliberate: "class_id = (subquery)" with an empty
-- subquery gives a NULL key, and digres then scans the whole index.
\set s random(1, 20000)
delete from enrollment where student_id = :s and class_id in (
  select class_id from enrollment where student_id = :s
  order by class_id limit 1);
