# c36 -- write skew whose one edge comes from a read the session's
# read pin would answer with an iterator it already has.
#
# SM1_DESIGN.md §10.1: a serializable read takes its SIREAD before
# its iterator exists. T1 reads R1, which leaves the session's
# read pin holding an iterator. T2 then writes R2, in flight, with
# no SIREAD on R2 for its write to find. T1 reads R2: an iterator
# from before T2's write would not hold T2's version, so T1 would
# step past nothing and owe T2 no edge. T2 reads R3 and T1 writes
# it, the edge T2 --rw--> T1; with T1 --rw--> T2 from R2 the cycle
# closes, and under S+S one of them must fail. So a serializable
# read never seeks an iterator it already has.

pairings all
session T1
session T2
seed R1 0
seed R2 0
seed R3 0

@1 T1 begin
@1 T2 begin
@2 T1 read R1
@3 T2 write R2 t2
@4 T1 read R2
@5 T2 read R3
@6 T1 write R3 t1
@7 T1 commit?
@7 T2 commit?
@8 check committed T1 T2 when rr
@8 check committed T1 T2 when rr+s
@8 check at-most-one-committed T1 T2 when s+s
