# p03 -- PostgreSQL's isolation spec read-only-anomaly.
#
# Ported from PostgreSQL's src/test/isolation/specs (branch
# REL_17_STABLE), under the PostgreSQL License; the expected
# outcomes are copied from its expected/read-only-anomaly.out.
#
# The read-only anomaly under REPEATABLE READ: all three
# commit, though no serial order gives what s3 read. It shows
# what snapshot isolation allows, not what it forbids.
#
# Table bank_account (id PRIMARY KEY, balance) becomes rows
# bX, bY.

# PostgreSQL's setup:
#
#   CREATE TABLE bank_account (id TEXT PRIMARY KEY, balance DECIMAL NOT NULL);
#   INSERT INTO bank_account (id, balance) VALUES ('X', 0), ('Y', 0);
#
# and each session runs, before its first step:
#
#   s1: BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;
#   s2: BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;
#   s3: BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;

session s1 rr
session s2 rr
session s3 rr
seed bX 0
seed bY 0

# s1ry: SELECT balance FROM bank_account WHERE id = 'Y';
step s1 s1ry read bY
# s1wy: UPDATE bank_account SET balance = 20 WHERE id = 'Y';
step s1 s1wy read bY ; write bY 20
# s1c: COMMIT;
step s1 s1c commit
# s2rx: SELECT balance FROM bank_account WHERE id = 'X';
step s2 s2rx read bX
# s2ry: SELECT balance FROM bank_account WHERE id = 'Y';
step s2 s2ry read bY
# s2wx: UPDATE bank_account SET balance = -11 WHERE id = 'X';
step s2 s2wx read bX ; write bX -11
# s2c: COMMIT;
step s2 s2c commit
# s3r: SELECT id, balance FROM bank_account WHERE id IN ('X', 'Y') ORDER BY id;
step s3 s3r read bX ; read bY
# s3c: COMMIT;
step s3 s3c commit

permutation s2rx s2ry s1ry s1wy s1c s3r s2wx s2c s3c
expect s1=commit s2=commit s3=commit
