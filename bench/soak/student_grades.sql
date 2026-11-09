-- A student reads the grades.
\set s random(1, 20000)
select class_id, grade from enrollment where student_id = :s;
