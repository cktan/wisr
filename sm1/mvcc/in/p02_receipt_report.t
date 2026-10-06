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

session s1 ser
session s2 ser
session s3 ser
seed c:receipt 1222
seed r:1 1222
seed r:2 1222

# INSERT INTO receipt VALUES (3, (SELECT deposit_date FROM ctl
#   WHERE k = 'receipt'), 4.00)
step s1 rxwy1 read c:receipt ; insert r:3 1222
step s1 c1 commit
# UPDATE ctl SET deposit_date = '2008-12-23' WHERE k = 'receipt'
step s2 wx2 read c:receipt ; write c:receipt 1223
step s2 c2 commit
# SELECT * FROM ctl WHERE k = 'receipt'
step s3 rx3 read c:receipt
# SELECT * FROM receipt WHERE deposit_date = '2008-12-22'
step s3 ry3 scan r: r:~
step s3 c3 commit

# Where sm1 differs from PostgreSQL, an "sm1" line under the
# permutation's expect line gives sm1's outcome. Every such
# difference here is sm1 failing more transactions, never fewer:
# its SSI applies Cahill's original rule, failing any transaction
# with both an incoming and an outgoing rw-edge, where PostgreSQL
# adds the refinements of Ports and Grittner (commit order, and
# read-only transactions), and fails only a transaction in a real
# cycle (SM1_DESIGN.md §7.3).

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
sm1 s1=commit s2=commit s3=serialize
permutation rxwy1 wx2 rx3 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rxwy1 wx2 rx3 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rxwy1 wx2 rx3 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rxwy1 wx2 rx3 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 wx2 rx3 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 wx2 rx3 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
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
sm1 s1=commit s2=commit s3=serialize
permutation rxwy1 rx3 wx2 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rxwy1 rx3 wx2 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rxwy1 rx3 wx2 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rxwy1 rx3 wx2 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 wx2 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 wx2 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 ry3 c1 wx2 c2 c3
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 c1 wx2 c3 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 c1 c3 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c1 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 ry3 wx2 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 ry3 c3 c1 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rxwy1 rx3 ry3 c3 wx2 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rxwy1 rx3 ry3 c3 wx2 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
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
sm1 s1=commit s2=commit s3=serialize
permutation wx2 rxwy1 rx3 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation wx2 rxwy1 rx3 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation wx2 rxwy1 rx3 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation wx2 rxwy1 rx3 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rxwy1 rx3 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rxwy1 rx3 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
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
sm1 s1=commit s2=commit s3=serialize
permutation wx2 rx3 rxwy1 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation wx2 rx3 rxwy1 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation wx2 rx3 rxwy1 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation wx2 rx3 rxwy1 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 rxwy1 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 rxwy1 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
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
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 ry3 rxwy1 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 ry3 c2 rxwy1 c1 c3
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 c2 rxwy1 c3 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 c2 c3 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation wx2 rx3 ry3 c3 rxwy1 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation wx2 rx3 ry3 c3 rxwy1 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
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
sm1 s1=commit s2=commit s3=serialize
permutation rx3 rxwy1 wx2 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rx3 rxwy1 wx2 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rx3 rxwy1 wx2 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rx3 rxwy1 wx2 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 wx2 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 wx2 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 ry3 c1 wx2 c2 c3
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 c1 wx2 c3 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 c1 c3 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c1 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 ry3 wx2 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 ry3 c3 c1 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rx3 rxwy1 ry3 c3 wx2 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 rxwy1 ry3 c3 wx2 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 rxwy1 c1 c2 ry3 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rx3 wx2 rxwy1 c1 ry3 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rx3 wx2 rxwy1 c1 ry3 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rx3 wx2 rxwy1 c2 c1 ry3 c3
expect s1=commit s2=commit s3=commit
sm1 s1=commit s2=commit s3=serialize
permutation rx3 wx2 rxwy1 c2 ry3 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 rxwy1 c2 ry3 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c1 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 rxwy1 ry3 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
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
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 ry3 rxwy1 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 ry3 c2 rxwy1 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 c2 rxwy1 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 c2 c3 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation rx3 wx2 ry3 c3 rxwy1 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 wx2 ry3 c3 rxwy1 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
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
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 rxwy1 wx2 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 rxwy1 c3 c1 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 rxwy1 c3 wx2 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 rxwy1 c3 wx2 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c1 c2 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c1 c3 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c2 c1 c3
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c2 c3 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c3 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 wx2 rxwy1 c3 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 wx2 c2 rxwy1 c1 c3
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 c2 rxwy1 c3 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 c2 c3 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 wx2 c3 rxwy1 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 wx2 c3 rxwy1 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 wx2 c3 c2 rxwy1 c1
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 c3 rxwy1 c1 wx2 c2
expect s1=commit s2=commit s3=commit
permutation rx3 ry3 c3 rxwy1 wx2 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 c3 rxwy1 wx2 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 c3 wx2 rxwy1 c1 c2
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 c3 wx2 rxwy1 c2 c1
expect s1=commit s2=commit s3=commit
sm1 s1=serialize s2=commit s3=commit
permutation rx3 ry3 c3 wx2 c2 rxwy1 c1
expect s1=commit s2=commit s3=commit
