# k02 -- an update that keeps its key goes past a key-share lock.
#
# T1 holds R1 in KS, as a foreign key check of a child row holds
# its parent. T2's update keeps R1's key: it writes in NKU, which
# KS lets in, so T2 does not wait for T1.

stores chained
pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 lock R1 keyshare
@3 T2 write R1 t2 keykept
@4 check last T2 ok
@5 T2 commit
@6 T1 commit
@7 check committed T1 T2
@8 check row R1 t2
