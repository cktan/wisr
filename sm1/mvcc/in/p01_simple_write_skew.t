# p01 -- PostgreSQL's isolation spec simple-write-skew.
#
# Ported from PostgreSQL's src/test/isolation/specs (branch
# REL_17_STABLE), under the PostgreSQL License; the expected
# outcomes are copied from its expected/simple-write-skew.out.
#
# Two serializable transactions: one turns every 'pear' row to
# 'apple', the other every 'apple' row to 'pear'. Run one after
# the other, either value could remain, but not both; any
# overlap must fail one of them.
#
# Table test (i PRIMARY KEY, t) becomes rows t05, t07, t11, the
# value being t. An UPDATE ... WHERE t = ... has no index on t,
# so it reads the whole table: update-where over t..t~.

# PostgreSQL's setup:
#
#   CREATE TABLE test (i int PRIMARY KEY, t text);
#   INSERT INTO test VALUES (5, 'apple'), (7, 'pear'), (11, 'banana');
#
# and each session runs, before its first step:
#
#   s1: BEGIN ISOLATION LEVEL SERIALIZABLE;
#   s2: BEGIN ISOLATION LEVEL SERIALIZABLE;

session s1 ser
session s2 ser
seed t05 apple
seed t07 pear
seed t11 banana

# rwx1: UPDATE test SET t = 'apple' WHERE t = 'pear';
step s1 rwx1 update-where t t~ pear apple
# c1: COMMIT;
step s1 c1 commit
# rwx2: UPDATE test SET t = 'pear' WHERE t = 'apple'
step s2 rwx2 update-where t t~ apple pear
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

permutation rwx1 c1 rwx2 c2
expect s1=commit s2=commit
permutation rwx1 rwx2 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation rwx1 rwx2 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation rwx2 rwx1 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation rwx2 rwx1 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation rwx2 c2 rwx1 c1
expect s1=commit s2=commit
