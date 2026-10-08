# p08 -- PostgreSQL's isolation spec temporal-range-integrity.
#
# Ported from PostgreSQL's src/test/isolation/specs (branch
# REL_17_STABLE), under the PostgreSQL License; the expected
# outcomes are copied from its expected/temporal-range-integrity.out.
#
# An offense is recorded against a statute while the statute
# is deleted: each checks the other's table, a write skew
# across two tables.
#
# Table statute, PRIMARY KEY (statute_cite, eff_date), becomes
# rows s:123:<eff_date>; table offense (offense_no PRIMARY KEY)
# rows o:<no>. A count on offense's statute_cite has no index,
# so it scans the whole table.

# PostgreSQL's setup:
#
#   CREATE TABLE statute (statute_cite text NOT NULL, eff_date date NOT NULL, exp_date date, CONSTRAINT statute_pkey PRIMARY KEY (statute_cite, eff_date));
#   INSERT INTO statute VALUES ('123.45(1)a', DATE '2008-01-01', NULL);
#   CREATE TABLE offense (offense_no int NOT NULL, statute_cite text NOT NULL, offense_date date NOT NULL, CONSTRAINT offense_pkey PRIMARY KEY (offense_no));
#
# and each session runs, before its first step:
#
#   s1: BEGIN ISOLATION LEVEL SERIALIZABLE;
#   s2: BEGIN ISOLATION LEVEL SERIALIZABLE;

session s1 ser
session s2 ser
seed s:123:20080101 null

# rx1: SELECT count(*) FROM statute WHERE statute_cite = '123.45(1)a' AND eff_date <= DATE '2009-05-15' AND (exp_date IS NULL OR exp_date > DATE '2009-05-15');
step s1 rx1 scan s:123: s:123:20090515
# wy1: INSERT INTO offense VALUES (1, '123.45(1)a', DATE '2009-05-15');
step s1 wy1 insert o:1 123
# c1: COMMIT;
step s1 c1 commit
# ry2: SELECT count(*) FROM offense WHERE statute_cite = '123.45(1)a' AND offense_date >= DATE '2008-01-01';
step s2 ry2 scan o: o:~
# wx2: DELETE FROM statute WHERE statute_cite = '123.45(1)a' AND eff_date = DATE '2008-01-01';
step s2 wx2 read s:123:20080101 ; delete s:123:20080101
# c2: COMMIT;
step s2 c2 commit

# Every permutation ends as PostgreSQL's does: sm1's SSI applies
# the refinements of Ports and Grittner (commit order, and
# read-only transactions) as PostgreSQL does (SM1_DESIGN.md §7.3).

permutation rx1 wy1 c1 ry2 wx2 c2
expect s1=commit s2=commit
permutation rx1 wy1 ry2 c1 wx2 c2
expect s1=commit s2=serialize
permutation rx1 wy1 ry2 wx2 c1 c2
expect s1=commit s2=serialize
permutation rx1 wy1 ry2 wx2 c2 c1
expect s1=serialize s2=commit
permutation rx1 ry2 wy1 c1 wx2 c2
expect s1=commit s2=serialize
permutation rx1 ry2 wy1 wx2 c1 c2
expect s1=commit s2=serialize
permutation rx1 ry2 wy1 wx2 c2 c1
expect s1=serialize s2=commit
permutation rx1 ry2 wx2 wy1 c1 c2
expect s1=commit s2=serialize
permutation rx1 ry2 wx2 wy1 c2 c1
expect s1=serialize s2=commit
permutation rx1 ry2 wx2 c2 wy1 c1
expect s1=serialize s2=commit
permutation ry2 rx1 wy1 c1 wx2 c2
expect s1=commit s2=serialize
permutation ry2 rx1 wy1 wx2 c1 c2
expect s1=commit s2=serialize
permutation ry2 rx1 wy1 wx2 c2 c1
expect s1=serialize s2=commit
permutation ry2 rx1 wx2 wy1 c1 c2
expect s1=commit s2=serialize
permutation ry2 rx1 wx2 wy1 c2 c1
expect s1=serialize s2=commit
permutation ry2 rx1 wx2 c2 wy1 c1
expect s1=serialize s2=commit
permutation ry2 wx2 rx1 wy1 c1 c2
expect s1=commit s2=serialize
permutation ry2 wx2 rx1 wy1 c2 c1
expect s1=serialize s2=commit
permutation ry2 wx2 rx1 c2 wy1 c1
expect s1=serialize s2=commit
permutation ry2 wx2 c2 rx1 wy1 c1
expect s1=commit s2=commit
