# c28 -- write skew around a cycle of three.
#
# c02's skew has two transactions and one edge each way. Here
# the cycle is T1 --rw--> T2 --rw--> T3 --rw--> T1: each
# transaction reads the row its neighbour is about to write,
# and writes the row its other neighbour has read. No two of
# them conflict directly, so every row lock is free and
# nothing blocks; the schedule is serial in no order at all.
# Snapshot isolation allows it, and the mixed pairings do
# too, because a transaction that is not SERIALIZABLE neither
# records its reads nor answers for its writes. Only S/S
# closes the cycle, and then at least one of the three must
# fail.

pairings all
session T1
session T2
session T3
seed R1 0
seed R2 0
seed R3 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 read R1
@2 T1 read R2
@3 T2 read R2
@3 T2 read R3
@4 T3 read R3
@4 T3 read R1
@5 T1 write R1 t1
@6 T2 write R2 t2
@7 T3 write R3 t3
@8 T1 commit?
@8 T2 commit?
@8 T3 commit?
@9 check committed T1 T2 T3 when rr
@9 check committed T1 T2 T3 when rr+s
@9 check not-all-committed T1 T2 T3 when s+s
