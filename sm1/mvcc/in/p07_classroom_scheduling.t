# p07 -- PostgreSQL's isolation spec classroom-scheduling.
#
# Ported from PostgreSQL's src/test/isolation/specs (branch
# REL_17_STABLE), under the PostgreSQL License; the expected
# outcomes are copied from its expected/classroom-scheduling.out.
#
# Two bookings of room 101 that overlap, each checked against
# the other's absence: a write skew through a predicate.
#
# Table room_reservation, PRIMARY KEY (room_id, start_time),
# becomes rows r:101:<start>, the value the end time. A count
# of bookings starting before a time scans the key range from
# the room's first booking up to that time.

session s1 ser
session s2 ser
seed r:101:1000 1100

# SELECT count(*) ... WHERE room_id = '101' AND start_time < 14:00
#   AND end_time > 13:00
step s1 rx1 scan r:101: r:101:14
# INSERT ... ('101', 13:00, 14:00, 'Carol')
step s1 wy1 insert r:101:1300 1400
step s1 c1 commit
# SELECT count(*) ... WHERE room_id = '101' AND start_time < 14:30
#   AND end_time > 13:30
step s2 ry2 scan r:101: r:101:143
# UPDATE ... SET start_time = 13:30, end_time = 14:30
#   WHERE room_id = '101' AND start_time = 10:00
step s2 wx2 read r:101:1000 ; delete r:101:1000 ; insert r:101:1330 1430
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
