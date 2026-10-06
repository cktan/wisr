# p06 -- PostgreSQL's isolation spec two-ids.
#
# Ported from PostgreSQL's src/test/isolation/specs (branch
# REL_17_STABLE), under the PostgreSQL License; the expected
# outcomes are copied from its expected/two-ids.out.
#
# Three transactions over two one-row tables: s1 bumps D1, s2
# sets D2 from D1, s3 reads D2. A cycle needs all three.
#
# Tables D1 and D2 (id, no key) become rows d1:r and d2:r;
# with no index, every statement scans its table.

# PostgreSQL's setup:
#
#   create table D1 (id int not null);
#   create table D2 (id int not null);
#   insert into D1 values (1);
#   insert into D2 values (1);
#
# and each session runs, before its first step:
#
#   s1: BEGIN ISOLATION LEVEL SERIALIZABLE;
#   s2: BEGIN ISOLATION LEVEL SERIALIZABLE;
#   s3: BEGIN ISOLATION LEVEL SERIALIZABLE;

session s1 ser
session s2 ser
session s3 ser
seed d1:r 1
seed d2:r 1

# wx1: update D1 set id = id + 1;
step s1 wx1 scan d1: d1:~ ; write d1:r 2
# c1: COMMIT;
step s1 c1 commit
# rxwy2: update D2 set id = (select id+1 from D1);
step s2 rxwy2 scan d2: d2:~ ; scan d1: d1:~ ; write d2:r 2
# c2: COMMIT;
step s2 c2 commit
# ry3: select id from D2;
step s3 ry3 scan d2: d2:~
# c3: COMMIT;
step s3 c3 commit

# Where sm1 differs from PostgreSQL, an "sm1" line under the
# permutation's expect line gives sm1's outcome. Every such
# difference here is sm1 failing more transactions, never fewer:
# its SSI applies Cahill's original rule, failing any transaction
# with both an incoming and an outgoing rw-edge, where PostgreSQL
# adds the refinements of Ports and Grittner (commit order, and
# read-only transactions), and fails only a transaction in a real
# cycle (SM1_DESIGN.md §7.3).

permutation wx1 c1 rxwy2 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx1 c1 rxwy2 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation wx1 c1 rxwy2 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation wx1 c1 ry3 rxwy2 c2 c3
expect s1=commit s2=commit s3=commit
permutation wx1 c1 ry3 rxwy2 c3 c2
expect s1=commit s2=commit s3=commit
permutation wx1 c1 ry3 c3 rxwy2 c2
expect s1=commit s2=commit s3=commit
permutation wx1 rxwy2 c1 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx1 rxwy2 c1 ry3 c2 c3
expect s1=commit s2=serialize s3=commit
permutation wx1 rxwy2 c1 ry3 c3 c2
expect s1=commit s2=serialize s3=commit
permutation wx1 rxwy2 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx1 rxwy2 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx1 rxwy2 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx1 rxwy2 ry3 c1 c2 c3
expect s1=commit s2=serialize s3=commit
permutation wx1 rxwy2 ry3 c1 c3 c2
expect s1=commit s2=serialize s3=commit
permutation wx1 rxwy2 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation wx1 rxwy2 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation wx1 rxwy2 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation wx1 rxwy2 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation wx1 ry3 c1 rxwy2 c2 c3
expect s1=commit s2=commit s3=commit
permutation wx1 ry3 c1 rxwy2 c3 c2
expect s1=commit s2=commit s3=commit
permutation wx1 ry3 c1 c3 rxwy2 c2
expect s1=commit s2=commit s3=commit
permutation wx1 ry3 rxwy2 c1 c2 c3
expect s1=commit s2=serialize s3=commit
permutation wx1 ry3 rxwy2 c1 c3 c2
expect s1=commit s2=serialize s3=commit
permutation wx1 ry3 rxwy2 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation wx1 ry3 rxwy2 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation wx1 ry3 rxwy2 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation wx1 ry3 rxwy2 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation wx1 ry3 c3 c1 rxwy2 c2
expect s1=commit s2=commit s3=commit
permutation wx1 ry3 c3 rxwy2 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation wx1 ry3 c3 rxwy2 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 wx1 c1 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy2 wx1 c1 ry3 c2 c3
expect s1=commit s2=serialize s3=commit
permutation rxwy2 wx1 c1 ry3 c3 c2
expect s1=commit s2=serialize s3=commit
permutation rxwy2 wx1 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy2 wx1 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation rxwy2 wx1 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation rxwy2 wx1 ry3 c1 c2 c3
expect s1=commit s2=serialize s3=commit
permutation rxwy2 wx1 ry3 c1 c3 c2
expect s1=commit s2=serialize s3=commit
permutation rxwy2 wx1 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 wx1 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 wx1 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 wx1 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 c2 wx1 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy2 c2 wx1 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation rxwy2 c2 wx1 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation rxwy2 c2 ry3 wx1 c1 c3
expect s1=commit s2=commit s3=commit
permutation rxwy2 c2 ry3 wx1 c3 c1
expect s1=commit s2=commit s3=commit
permutation rxwy2 c2 ry3 c3 wx1 c1
expect s1=commit s2=commit s3=commit
permutation rxwy2 ry3 wx1 c1 c2 c3
expect s1=commit s2=serialize s3=commit
permutation rxwy2 ry3 wx1 c1 c3 c2
expect s1=commit s2=serialize s3=commit
permutation rxwy2 ry3 wx1 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 ry3 wx1 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 ry3 wx1 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 ry3 wx1 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 ry3 c2 wx1 c1 c3
expect s1=commit s2=commit s3=commit
permutation rxwy2 ry3 c2 wx1 c3 c1
expect s1=commit s2=commit s3=commit
permutation rxwy2 ry3 c2 c3 wx1 c1
expect s1=commit s2=commit s3=commit
permutation rxwy2 ry3 c3 wx1 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 ry3 c3 wx1 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation rxwy2 ry3 c3 c2 wx1 c1
expect s1=commit s2=commit s3=commit
permutation ry3 wx1 c1 rxwy2 c2 c3
expect s1=commit s2=commit s3=commit
permutation ry3 wx1 c1 rxwy2 c3 c2
expect s1=commit s2=commit s3=commit
permutation ry3 wx1 c1 c3 rxwy2 c2
expect s1=commit s2=commit s3=commit
permutation ry3 wx1 rxwy2 c1 c2 c3
expect s1=commit s2=serialize s3=commit
permutation ry3 wx1 rxwy2 c1 c3 c2
expect s1=commit s2=serialize s3=commit
permutation ry3 wx1 rxwy2 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 wx1 rxwy2 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 wx1 rxwy2 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 wx1 rxwy2 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 wx1 c3 c1 rxwy2 c2
expect s1=commit s2=commit s3=commit
permutation ry3 wx1 c3 rxwy2 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 wx1 c3 rxwy2 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 rxwy2 wx1 c1 c2 c3
expect s1=commit s2=serialize s3=commit
permutation ry3 rxwy2 wx1 c1 c3 c2
expect s1=commit s2=serialize s3=commit
permutation ry3 rxwy2 wx1 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 rxwy2 wx1 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 rxwy2 wx1 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 rxwy2 wx1 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 rxwy2 c2 wx1 c1 c3
expect s1=commit s2=commit s3=commit
permutation ry3 rxwy2 c2 wx1 c3 c1
expect s1=commit s2=commit s3=commit
permutation ry3 rxwy2 c2 c3 wx1 c1
expect s1=commit s2=commit s3=commit
permutation ry3 rxwy2 c3 wx1 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 rxwy2 c3 wx1 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 rxwy2 c3 c2 wx1 c1
expect s1=commit s2=commit s3=commit
permutation ry3 c3 wx1 c1 rxwy2 c2
expect s1=commit s2=commit s3=commit
permutation ry3 c3 wx1 rxwy2 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 c3 wx1 rxwy2 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 c3 rxwy2 wx1 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 c3 rxwy2 wx1 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=serialize s3=commit
permutation ry3 c3 rxwy2 c2 wx1 c1
expect s1=commit s2=commit s3=commit
