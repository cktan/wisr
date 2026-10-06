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

# PostgreSQL's setup:
#
#   CREATE TABLE room_reservation (room_id text NOT NULL, start_time timestamp with time zone NOT NULL, end_time timestamp with time zone NOT NULL, description text NOT NULL, CONSTRAINT room_reservation_pkey PRIMARY KEY (room_id, start_time));
#   INSERT INTO room_reservation VALUES ('101', TIMESTAMP WITH TIME ZONE '2010-04-01 10:00', TIMESTAMP WITH TIME ZONE '2010-04-01 11:00', 'Bob');
#
# and each session runs, before its first step:
#
#   s1: BEGIN ISOLATION LEVEL SERIALIZABLE;
#   s2: BEGIN ISOLATION LEVEL SERIALIZABLE;

session s1 ser
session s2 ser
seed r:101:1000 1100

# rx1: SELECT count(*) FROM room_reservation WHERE room_id = '101' AND start_time < TIMESTAMP WITH TIME ZONE '2010-04-01 14:00' AND end_time > TIMESTAMP WITH TIME ZONE '2010-04-01 13:00';
step s1 rx1 scan r:101: r:101:14
# wy1: INSERT INTO room_reservation VALUES ('101', TIMESTAMP WITH TIME ZONE '2010-04-01 13:00', TIMESTAMP WITH TIME ZONE '2010-04-01 14:00', 'Carol');
step s1 wy1 insert r:101:1300 1400
# c1: COMMIT;
step s1 c1 commit
# ry2: SELECT count(*) FROM room_reservation WHERE room_id = '101' AND start_time < TIMESTAMP WITH TIME ZONE '2010-04-01 14:30' AND end_time > TIMESTAMP WITH TIME ZONE '2010-04-01 13:30';
step s2 ry2 scan r:101: r:101:143
# wx2: UPDATE room_reservation SET start_time = TIMESTAMP WITH TIME ZONE '2010-04-01 13:30', end_time = TIMESTAMP WITH TIME ZONE '2010-04-01 14:30' WHERE room_id = '101' AND start_time = TIMESTAMP WITH TIME ZONE '2010-04-01 10:00';
step s2 wx2 read r:101:1000 ; delete r:101:1000 ; insert r:101:1330 1430
# c2: COMMIT;
step s2 c2 commit

# Every permutation ends as PostgreSQL's does: sm1's SSI applies
# the refinements of Ports and Grittner (commit order, and
# read-only transactions) as PostgreSQL does (SM1_DESIGN.md §7.3).

permutation rx1 wy1 c1 ry2 wx2 c2
expect s1=commit s2=commit
permutation rx1 wy1 ry2 c1 wx2 c2
expect s1=commit s2=serialize
permutation rx1 wy1 ry2 wx2 c1 c2
expect s1=commit s2=serialize
permutation rx1 wy1 ry2 wx2 c2 c1
expect s1=serialize s2=commit
permutation rx1 ry2 wy1 c1 wx2 c2
expect s1=commit s2=serialize
permutation rx1 ry2 wy1 wx2 c1 c2
expect s1=commit s2=serialize
permutation rx1 ry2 wy1 wx2 c2 c1
expect s1=serialize s2=commit
permutation rx1 ry2 wx2 wy1 c1 c2
expect s1=commit s2=serialize
permutation rx1 ry2 wx2 wy1 c2 c1
expect s1=serialize s2=commit
permutation rx1 ry2 wx2 c2 wy1 c1
expect s1=serialize s2=commit
permutation ry2 rx1 wy1 c1 wx2 c2
expect s1=commit s2=serialize
permutation ry2 rx1 wy1 wx2 c1 c2
expect s1=commit s2=serialize
permutation ry2 rx1 wy1 wx2 c2 c1
expect s1=serialize s2=commit
permutation ry2 rx1 wx2 wy1 c1 c2
expect s1=commit s2=serialize
permutation ry2 rx1 wx2 wy1 c2 c1
expect s1=serialize s2=commit
permutation ry2 rx1 wx2 c2 wy1 c1
expect s1=serialize s2=commit
permutation ry2 wx2 rx1 wy1 c1 c2
expect s1=commit s2=serialize
permutation ry2 wx2 rx1 wy1 c2 c1
expect s1=serialize s2=commit
permutation ry2 wx2 rx1 c2 wy1 c1
expect s1=serialize s2=commit
permutation ry2 wx2 c2 rx1 wy1 c1
expect s1=commit s2=commit
