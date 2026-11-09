-- A prof sets the grade of a student of a class: an update.
\set c permute(random_zipfian(1, 2500, 1.1) - 1, 2500) + 1
\set o random(0, 39)
\set g random(0, 100)
update enrollment set grade = :g
  where class_id = :c and student_id = (
    select student_id from enrollment where class_id = :c
    order by student_id limit 1 offset :o);
