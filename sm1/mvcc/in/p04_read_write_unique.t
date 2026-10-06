# p04 -- PostgreSQL's isolation spec read-write-unique.
#
# Ported from PostgreSQL's src/test/isolation/specs (branch
# REL_17_STABLE), under the PostgreSQL License; the expected
# outcomes are copied from its expected/read-write-unique.out.
#
# Two serializable transactions each read an empty table and
# insert the same key. PostgreSQL reports the second as a
# serialization failure, not a unique violation, since the
# duplicate comes of their running at once.
#
# Table test (i PRIMARY KEY) becomes rows t:42 and so on; a
# SELECT * scans the whole table.

# PostgreSQL's setup:
#
#   CREATE TABLE test (i integer PRIMARY KEY);
#
# and each session runs, before its first step:
#
#   s1: BEGIN ISOLATION LEVEL SERIALIZABLE;
#   s2: BEGIN ISOLATION LEVEL SERIALIZABLE;

session s1 ser
session s2 ser

# r1: SELECT * FROM test;
step s1 r1 scan t: t:~
# w1: INSERT INTO test VALUES (42);
step s1 w1 insert t:42 x
# c1: COMMIT;
step s1 c1 commit
# r2: SELECT * FROM test;
step s2 r2 scan t: t:~
# w2: INSERT INTO test VALUES (42);
step s2 w2 insert t:42 x
# c2: COMMIT;
step s2 c2 commit

permutation r1 r2 w1 w2 c1 c2
expect s1=commit s2=serialize
permutation r1 w1 c1 r2 w2 c2
expect s1=commit s2=unique
