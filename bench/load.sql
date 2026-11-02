-- load.sql -- make the database bench and load usertable into it.
--
--   cd bench && psql -d postgres -f load.sql
--
-- Run from bench/: \copy reads usertable.csv.gz from psql's directory.

drop database if exists bench;
create database bench;
\c bench

create table usertable (
  id int8 primary key,
  lastname text,
  firstname text,
  street text,
  city text,
  state text,
  zip text,
  phone text
);

\copy usertable from program 'gzip -dc usertable.csv.gz' with (format csv)

analyze usertable;
