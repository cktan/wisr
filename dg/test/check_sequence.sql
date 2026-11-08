-- Verify that CHECK constraints reject sequence-related functions.
-- 1. Reject nextval in a CREATE TABLE check.
-- 2. Reject sequence functions in ALTER TABLE checks.
\set ON_ERROR_STOP off
create temp table check_sequence_create (a int check
  (nextval(0::regclass) > 0));
create temp table check_sequence_alter (a int);
alter table check_sequence_alter add check
  (currval(0::regclass) > 0);
alter table check_sequence_alter add check (lastval() > 0);
alter table check_sequence_alter add check (setval(0::regclass, 1) > 0);
alter table check_sequence_alter add check
  (pg_sequence_parameters(0::regclass) is null);
alter table check_sequence_alter add check
  (pg_sequence_last_value(0::regclass) is null);
alter table check_sequence_alter add check
  (pg_get_sequence_data(0::regclass) is null);
alter table check_sequence_alter add check
  (pg_get_serial_sequence('check_sequence_alter', 'a') is null);
drop table check_sequence_alter;
