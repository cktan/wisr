# a17 -- an older snapshot goes on reading a dropped store.
#
# A drop is not a deletion of the rows: a transaction whose
# snapshot predates the commit must still find the store
# and everything in it, so the column family outlives the drop
# and goes only when no snapshot can reach it. T1 takes its
# snapshot, T2 drops the second store and commits, and T1
# reads it afterwards exactly as before. A transaction that
# begins after the commit gets notfound instead, which is what
# tells the two apart.

pairings all
session T1
session T2
session T3
seed R1 0

@1 T1 begin
@2 T1 lookup R1
@3 T2 begin
@4 T2 rmstore
@5 T2 commit
@6 T1 lookup R1
@7 check last T1 0
@8 T1 commit
@9 T3 begin
@10 T3 lookup R1
@11 check last T3 notfound
@12 T3 abort
@13 check row R1 0
