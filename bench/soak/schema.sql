-- schema.sql -- the school of SOAK.md: tables, rows, roles, grants.
--
--   psql -d postgres -f soak/schema.sql
--
-- 1. Make the tables.
-- 2. Load 1000 profs, 20000 students and 2500 classes. A class
--    holds 40 to 60 students, and the load gives each class 40, so
--    one class in 21 is full at the start.
-- 3. Give each student 5 classes: 100000 enrollments. Every class
--    gets exactly 40 of them.
-- 4. Make the secondary indexes, USING btree (digres would make lsm
--    ones), and the two roles.
-- 5. Grant each role what its part of the web app needs.

drop database if exists soak;
-- Roles belong to the cluster, not the database: a rerun drops them.
drop role if exists student_app, prof_app;
create database soak;
\c soak

create table prof (
  id int8 primary key,
  name text not null,
  department text not null
);
create table student (
  id int8 primary key,
  name text not null,
  year int not null
);
create table class (
  id int8 primary key,
  prof_id int8 not null,
  term text not null,
  slot int not null,
  capacity int not null
);
create table enrollment (
  student_id int8 not null,
  class_id int8 not null,
  grade int,
  primary key (student_id, class_id)
);

insert into prof
  select g, 'prof' || g, 'dept' || (g % 40)
  from generate_series(1, 1000) g;
insert into student
  select g, 'student' || g, 1 + g % 4
  from generate_series(1, 20000) g;
insert into class
  select g, 1 + (g - 1) % 1000, 'fall', g % 30, 40 + g % 21
  from generate_series(1, 2500) g;
-- 13 * s + 501 * k, modulo 2500, is a different class for each of
-- the 5 values of k, and each class is hit 40 times in all.
insert into enrollment
  select s, 1 + (13 * s + 501 * k) % 2500, null
  from generate_series(1, 20000) s, generate_series(0, 4) k;

create index enrollment_class on enrollment using btree (class_id, student_id);
analyze;
create index class_prof on class using btree (prof_id);

create role student_app login password 'p';
create role prof_app login password 'p';

grant select on prof, student, class, enrollment to student_app;
grant insert, delete on enrollment to student_app;
grant select on prof, student, class, enrollment to prof_app;
grant update on enrollment to prof_app;
