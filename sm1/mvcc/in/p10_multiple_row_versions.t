# p10 -- PostgreSQL's isolation spec multiple-row-versions.
#
# Ported from PostgreSQL's src/test/isolation/specs (branch
# REL_17_STABLE), under the PostgreSQL License; the expected
# outcomes are copied from its expected/multiple-row-versions.out.
#
# Four transactions over a table of a million rows, where the
# cycle runs through several versions of one row.
#
# Table t (id PRIMARY KEY, txt) is seeded with the three rows
# the steps touch, t:0000001, t:0500000 and t:1000000; reads and
# updates are by primary key.

session s1 ser
session s2 ser
session s3 ser
session s4 ser
seed t:0000001 -
seed t:0500000 -
seed t:1000000 -

# SELECT * FROM t WHERE id = 1000000
step s1 rx1 read t:1000000
# UPDATE t SET txt = 'a' WHERE id = 1
step s1 wz1 read t:0000001 ; write t:0000001 a
step s1 c1 commit
# UPDATE t SET txt = 'b' WHERE id = 1000000
step s2 wx2 read t:1000000 ; write t:1000000 b
step s2 c2 commit
# UPDATE t SET txt = 'c' WHERE id = 1000000
step s3 wx3 read t:1000000 ; write t:1000000 c
# SELECT * FROM t WHERE id = 500000
step s3 ry3 read t:0500000
step s3 c3 commit
# UPDATE t SET txt = 'd' WHERE id = 500000
step s4 wy4 read t:0500000 ; write t:0500000 d
# SELECT * FROM t WHERE id = 1
step s4 rz4 read t:0000001
step s4 c4 commit

# Where sm1 differs from PostgreSQL, an "sm1" line under the
# permutation's expect line gives sm1's outcome. Every such
# difference here is sm1 failing more transactions, never fewer:
# its SSI applies Cahill's original rule, failing any transaction
# with both an incoming and an outgoing rw-edge, where PostgreSQL
# adds the refinements of Ports and Grittner (commit order, and
# read-only transactions), and fails only a transaction in a real
# cycle (SM1_DESIGN.md §7.3).

permutation rx1 wx2 c2 wx3 ry3 wy4 rz4 c4 c3 wz1 c1
expect s1=serialize s2=commit s3=commit s4=commit
sm1 s1=serialize s2=commit s3=serialize s4=commit
