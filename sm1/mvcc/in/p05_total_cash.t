# p05 -- PostgreSQL's isolation spec total-cash.
#
# Ported from PostgreSQL's src/test/isolation/specs (branch
# REL_17_STABLE), under the PostgreSQL License; the expected
# outcomes are copied from its expected/total-cash.out.
#
# Each of two transactions takes 200 from one account and sums
# both: a write skew through an aggregate.
#
# Table accounts (accountid PRIMARY KEY, balance) becomes rows
# a:checking, a:savings; SUM scans the whole table.

# PostgreSQL's setup:
#
#   CREATE TABLE accounts (accountid text NOT NULL PRIMARY KEY, balance numeric not null);
#   INSERT INTO accounts VALUES ('checking', 600),('savings',600);
#
# and each session runs, before its first step:
#
#   s1: BEGIN ISOLATION LEVEL SERIALIZABLE;
#   s2: BEGIN ISOLATION LEVEL SERIALIZABLE;

session s1 ser
session s2 ser
seed a:checking 600
seed a:savings 600

# wx1: UPDATE accounts SET balance = balance - 200 WHERE accountid = 'checking';
step s1 wx1 read a:checking ; write a:checking 400
# rxy1: SELECT SUM(balance) FROM accounts;
step s1 rxy1 scan a: a:~
# c1: COMMIT;
step s1 c1 commit
# wy2: UPDATE accounts SET balance = balance - 200 WHERE accountid = 'savings';
step s2 wy2 read a:savings ; write a:savings 400
# rxy2: SELECT SUM(balance) FROM accounts;
step s2 rxy2 scan a: a:~
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

permutation wx1 rxy1 c1 wy2 rxy2 c2
expect s1=commit s2=commit
permutation wx1 rxy1 wy2 c1 rxy2 c2
expect s1=commit s2=serialize
permutation wx1 rxy1 wy2 rxy2 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation wx1 rxy1 wy2 rxy2 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation wx1 wy2 rxy1 c1 rxy2 c2
expect s1=commit s2=serialize
permutation wx1 wy2 rxy1 rxy2 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation wx1 wy2 rxy1 rxy2 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation wx1 wy2 rxy2 rxy1 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation wx1 wy2 rxy2 rxy1 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation wx1 wy2 rxy2 c2 rxy1 c1
expect s1=serialize s2=commit
permutation wy2 wx1 rxy1 c1 rxy2 c2
expect s1=commit s2=serialize
permutation wy2 wx1 rxy1 rxy2 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation wy2 wx1 rxy1 rxy2 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation wy2 wx1 rxy2 rxy1 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation wy2 wx1 rxy2 rxy1 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation wy2 wx1 rxy2 c2 rxy1 c1
expect s1=serialize s2=commit
permutation wy2 rxy2 wx1 rxy1 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
permutation wy2 rxy2 wx1 rxy1 c2 c1
expect s1=serialize s2=commit
sm1 s1=serialize s2=serialize
permutation wy2 rxy2 wx1 c2 rxy1 c1
expect s1=serialize s2=commit
permutation wy2 rxy2 c2 wx1 rxy1 c1
expect s1=commit s2=commit
