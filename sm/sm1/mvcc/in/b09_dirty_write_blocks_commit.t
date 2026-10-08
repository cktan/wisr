# b09 -- a write waits for the row lock, then conflicts.
#
# T1 holds the row lock on R1. T2's write of the same row must
# wait rather than produce a version nobody asked for, and once
# T1 commits, T2 must lose first-committer-wins.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T2 write R1 t2 blocks
@4 T1 commit
@5 check last T2 conflict
@6 check row R1 t1
