# p09 -- PostgreSQL's isolation spec project-manager.
#
# Ported from PostgreSQL's src/test/isolation/specs (branch
# REL_17_STABLE), under the PostgreSQL License; the expected
# outcomes are copied from its expected/project-manager.out.
#
# A project is given a manager while that person stops being
# one: each checks the other's table.
#
# Table person (person_id PRIMARY KEY, name, is_project_manager)
# becomes row p:1, the value the flag; table project
# (project_no PRIMARY KEY, ..., project_manager) rows j:<no>. A
# count on project_manager has no index, so it scans the table.

# PostgreSQL's setup:
#
#   CREATE TABLE person (person_id int NOT NULL PRIMARY KEY, name text NOT NULL, is_project_manager bool NOT NULL);
#   INSERT INTO person VALUES (1, 'Robert Haas', true);
#   CREATE TABLE project (project_no int NOT NULL PRIMARY KEY, description text NOT NULL, project_manager int NOT NULL);
#
# and each session runs, before its first step:
#
#   s1: BEGIN ISOLATION LEVEL SERIALIZABLE;
#   s2: BEGIN ISOLATION LEVEL SERIALIZABLE;

session s1 ser
session s2 ser
seed p:1 t

# rx1: SELECT count(*) FROM person WHERE person_id = 1 AND is_project_manager;
step s1 rx1 read p:1
# wy1: INSERT INTO project VALUES (101, 'Build Great Wall', 1);
step s1 wy1 insert j:101 1
# c1: COMMIT;
step s1 c1 commit
# ry2: SELECT count(*) FROM project WHERE project_manager = 1;
step s2 ry2 scan j: j:~
# wx2: UPDATE person SET is_project_manager = false WHERE person_id = 1;
step s2 wx2 read p:1 ; write p:1 f
# c2: COMMIT;
step s2 c2 commit

# Where sm1 differs from PostgreSQL, an "sm1" line under the
# permutation's expect line gives sm1's outcome. Every such
# difference here is sm1 failing more transactions, never fewer:
# its SSI applies Cahill's original rule, failing any transaction
# with both an incoming and an outgoing rw-edge, where PostgreSQL
# adds the refinements of Ports and Grittner (commit order, and
# read-only transactions), and fails only a transaction in a real
# cycle (SM1_DESIGN.md §7.3).

permutation rx1 wy1 c1 ry2 wx2 c2
expect s1=commit s2=commit
permutation rx1 wy1 ry2 c1 wx2 c2
expect s1=commit s2=serialize
permutation rx1 wy1 ry2 wx2 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation rx1 wy1 ry2 wx2 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation rx1 ry2 wy1 c1 wx2 c2
expect s1=commit s2=serialize
permutation rx1 ry2 wy1 wx2 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation rx1 ry2 wy1 wx2 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation rx1 ry2 wx2 wy1 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation rx1 ry2 wx2 wy1 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation rx1 ry2 wx2 c2 wy1 c1
expect s1=serialize s2=commit
permutation ry2 rx1 wy1 c1 wx2 c2
expect s1=commit s2=serialize
permutation ry2 rx1 wy1 wx2 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation ry2 rx1 wy1 wx2 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation ry2 rx1 wx2 wy1 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation ry2 rx1 wx2 wy1 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation ry2 rx1 wx2 c2 wy1 c1
expect s1=serialize s2=commit
permutation ry2 wx2 rx1 wy1 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation ry2 wx2 rx1 wy1 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation ry2 wx2 rx1 c2 wy1 c1
expect s1=serialize s2=commit
permutation ry2 wx2 c2 rx1 wy1 c1
expect s1=commit s2=commit
