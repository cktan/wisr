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

session s1 ser
session s2 ser
seed s:123:20080101 null

# SELECT count(*) FROM statute WHERE statute_cite = '123.45(1)a'
#   AND eff_date <= '2009-05-15' AND (...)
step s1 rx1 scan s:123: s:123:20090515
# INSERT INTO offense VALUES (1, '123.45(1)a', '2009-05-15')
step s1 wy1 insert o:1 123
step s1 c1 commit
# SELECT count(*) FROM offense WHERE statute_cite = '123.45(1)a'
#   AND offense_date >= '2008-01-01'
step s2 ry2 scan o: o:~
# DELETE FROM statute WHERE statute_cite = '123.45(1)a'
#   AND eff_date = '2008-01-01'
step s2 wx2 read s:123:20080101 ; delete s:123:20080101
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
