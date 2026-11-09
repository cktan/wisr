# k06 -- the row modes' conflicts, lock against lock.
#
# T1 holds R1 in NKU, as FOR NO KEY UPDATE takes it. T2's KS is
# compatible with NKU and is taken. T3's S is not: it waits for
# T1. T4's NKU waits too, behind T1, until T1 and T3 end.

pairings rr
session T1
session T2
session T3
session T4
seed R1 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@1 T4 begin
@2 T1 lock R1 nokeyupdate
@3 T2 lock R1 keyshare
@4 check last T2 ok
@5 T3 lock R1 shared blocks
@6 T1 commit
@7 check last T3 ok
@8 T4 lock R1 nokeyupdate blocks
@9 T3 commit
@10 check last T4 ok
@11 T2 commit
@12 T4 commit
@13 check committed T1 T2 T3 T4
