-- A prof reads a class roster. Classes are Zipf-skewed, so some
-- are hot; permute spreads the hot ids over the key range.
\set c permute(random_zipfian(1, 2500, 1.1) - 1, 2500) + 1
select s.id, s.name, e.grade from enrollment e
  join student s on s.id = e.student_id
  where e.class_id = :c;
