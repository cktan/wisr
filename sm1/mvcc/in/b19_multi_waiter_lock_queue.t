# b19 -- two writers waiting on one row lock.
#
# b09 has a single waiter; here both T2 and T3 queue behind
# T1. Releasing the lock must wake every waiter and judge each
# one on its own: both took their snapshots before T1
# committed, so both lose first-committer-wins rather than one
# of them inheriting the other's verdict.

pairings all
session T1
session T2
session T3
seed R1 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 write R1 t1
@3 T2 write R1 t2 blocks
@4 T3 write R1 t3 blocks
@5 T1 commit
@6 check last T2 conflict
@7 check last T3 conflict
@8 check row R1 t1
