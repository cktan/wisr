# b18 -- an update and a delete over a committed concurrent
# insert.
#
# R2 and R3 are absent in the snapshots of T1 and T3, so an
# update or a delete of them would ordinarily be NOTFOUND.
# First-committer-wins is checked first, and it is the honest
# answer: the row was created after the snapshot, which is a
# concurrency failure and retryable. Compare b06, which makes
# the same point for an insert, and b13, where the row really
# never existed.

pairings all
session T1
session T2
session T3
seed R1 0

@1 T1 begin
@1 T3 begin
@2 T2 begin
@3 T2 insert R2 t2
@3 T2 insert R3 t2
@4 T2 commit
@5 T1 write R2 t1
@6 check last T1 conflict
@7 T3 delete R3
@8 check last T3 conflict
@9 check row R2 t2
@9 check row R3 t2
