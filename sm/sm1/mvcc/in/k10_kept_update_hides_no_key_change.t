# k10 -- a later update that keeps the key does not hide an earlier key
# change of the same transaction.
#
# T1 changes R1's key, then updates R1 again, keeping the key it just
# made. The second version is not flagged: the flag says the key is
# the last committed version's, and it is not. T2's KS lock waits for
# T1; when T1 commits, the key changed after T2's snapshot, and the
# lock fails with conflict.

stores chained
pairings rr
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T2 read R1
@3 T1 write R1 changed
@4 T1 write R1 changed_again keykept
@5 T2 lock R1 keyshare blocks
@6 T1 commit
@7 check last T2 conflict
@8 check committed T1
@9 check row R1 changed_again
