# p02 -- PostgreSQL's isolation spec receipt-report.
#
# Ported from PostgreSQL's src/test/isolation/specs (branch
# REL_17_STABLE), under the PostgreSQL License; the expected
# outcomes are copied from its expected/receipt-report.out.
#
# The paper's Example 2 (Ports and Grittner, VLDB 2012): a
# receipt, a batch close, and a read-only report. Snapshot
# isolation lets the report show a total a later receipt
# changes; serializable must not.
#
# Table ctl (k PRIMARY KEY, deposit_date) becomes row
# c:receipt; table receipt (receipt_no PRIMARY KEY,
# deposit_date, amount) rows r:1, r:2, the value the date. A
# read by primary key is a read of the row; a SELECT on
# deposit_date has no index, so it scans the whole table.

# PostgreSQL's setup:
#
#   CREATE TABLE ctl (k text NOT NULL PRIMARY KEY, deposit_date date NOT NULL);
#   INSERT INTO ctl VALUES ('receipt', DATE '2008-12-22');
#   CREATE TABLE receipt (receipt_no int NOT NULL PRIMARY KEY, deposit_date date NOT NULL, amount numeric(13,2));
#   INSERT INTO receipt VALUES (1, (SELECT deposit_date FROM ctl WHERE k = 'receipt'), 1.00);
#   INSERT INTO receipt VALUES (2, (SELECT deposit_date FROM ctl WHERE k = 'receipt'), 2.00);
#
# and each session runs, before its first step:
#
#   s1: BEGIN ISOLATION LEVEL SERIALIZABLE;
#   s2: BEGIN ISOLATION LEVEL SERIALIZABLE;
#   s3: BEGIN ISOLATION LEVEL SERIALIZABLE, READ ONLY;

session s1 ser
session s2 ser
session s3 ser ro
seed c:receipt 1222
seed r:1 1222
seed r:2 1222

# rxwy1: INSERT INTO receipt VALUES (3, (SELECT deposit_date FROM ctl WHERE k = 'receipt'), 4.00);
step s1 rxwy1 read c:receipt ; insert r:3 1222
# c1: COMMIT;
step s1 c1 commit
# wx2: UPDATE ctl SET deposit_date = DATE '2008-12-23' WHERE k = 'receipt';
step s2 wx2 read c:receipt ; write c:receipt 1223
# c2: COMMIT;
step s2 c2 commit
# rx3: SELECT * FROM ctl WHERE k = 'receipt';
step s3 rx3 read c:receipt
# ry3: SELECT * FROM receipt WHERE deposit_date = DATE '2008-12-22';
step s3 ry3 scan r: r:~
# c3: COMMIT;
step s3 c3 commit

# Every permutation ends as PostgreSQL's does: sm1's SSI applies
# the refinements of Ports and Grittner (commit order, and
# read-only transactions) as PostgreSQL does (SM1_DESIGN.md §7.3),
# and s3 is declared read-only, as in PostgreSQL's setup, so the
# read-only rule applies to it from its begin (§7.4).

permutation rxwy1 c1 wx2 c2 rx3 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 c1 wx2 rx3 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 c1 wx2 rx3 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 c1 wx2 rx3 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 c1 rx3 wx2 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 c1 rx3 wx2 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 c1 rx3 wx2 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 c1 rx3 ry3 wx2 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 c1 rx3 ry3 wx2 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 c1 rx3 ry3 c3 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 c1 c2 rx3 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 c1 rx3 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 c1 rx3 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 c1 rx3 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 c2 c1 rx3 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 c2 rx3 c1 ry3 c3
expect s1=commit s2=commit s3=serialize
permutation rxwy1 wx2 c2 rx3 ry3 c1 c3
expect s1=serialize s2=commit s3=commit
permutation rxwy1 wx2 c2 rx3 ry3 c3 c1
expect s1=serialize s2=commit s3=commit
permutation rxwy1 wx2 rx3 c1 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 c1 wx2 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 c1 wx2 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 c1 wx2 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 c1 ry3 wx2 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 c1 ry3 wx2 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 c1 ry3 c3 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 c1 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 c1 wx2 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 c1 wx2 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 c1 c3 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 c3 c1 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 c3 wx2 c1 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 c3 wx2 c2 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 c1 c2 rx3 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 c1 rx3 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 c1 rx3 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 c1 rx3 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 c2 c1 rx3 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 c2 rx3 c1 ry3 c3
expect s1=commit s2=commit s3=serialize
permutation wx2 rxwy1 c2 rx3 ry3 c1 c3
expect s1=serialize s2=commit s3=commit
permutation wx2 rxwy1 c2 rx3 ry3 c3 c1
expect s1=serialize s2=commit s3=commit
permutation wx2 rxwy1 rx3 c1 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation wx2 c2 rxwy1 c1 rx3 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 c2 rxwy1 rx3 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 c2 rxwy1 rx3 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 c2 rxwy1 rx3 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 c2 rx3 rxwy1 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 c2 rx3 rxwy1 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 c2 rx3 rxwy1 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 c2 rx3 ry3 rxwy1 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 c2 rx3 ry3 rxwy1 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 c2 rx3 ry3 c3 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 c1 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 c2 rxwy1 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 c2 rxwy1 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 c2 rxwy1 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 c2 ry3 rxwy1 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 c2 ry3 rxwy1 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 c2 ry3 c3 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 c2 rxwy1 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 c2 rxwy1 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 c2 c3 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 c3 rxwy1 c1 c2
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 c3 rxwy1 c2 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 c3 c2 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 c1 wx2 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 c1 wx2 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 c1 wx2 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 c1 ry3 wx2 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 c1 ry3 wx2 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 c1 ry3 c3 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 c1 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 c1 wx2 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 c1 wx2 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 c1 c3 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 c3 c1 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 c3 wx2 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 c3 wx2 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 c1 c2 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 c2 rxwy1 c1 ry3 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 c2 rxwy1 ry3 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 c2 rxwy1 ry3 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 c2 ry3 rxwy1 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 c2 ry3 rxwy1 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 c2 ry3 c3 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 c2 rxwy1 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 c2 rxwy1 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 c2 c3 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 c3 rxwy1 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 c3 rxwy1 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 c3 c2 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 c1 wx2 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 c1 wx2 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 c1 c3 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 c3 c1 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 c3 wx2 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 c3 wx2 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c1 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c1 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c2 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c2 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c3 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c3 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 c2 rxwy1 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 c2 rxwy1 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 c2 c3 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 c3 rxwy1 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 c3 rxwy1 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 c3 c2 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 c3 rxwy1 c1 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 c3 rxwy1 wx2 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 c3 rxwy1 wx2 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 c3 wx2 rxwy1 c1 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 c3 wx2 rxwy1 c2 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 c3 wx2 c2 rxwy1 c1
expect s1=commit s2=commit s3=commit
