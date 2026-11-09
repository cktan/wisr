# k05 -- a key-share lock's currency check passes over a committed
# update that kept the key; a share lock's does not.
#
# T2 and T3 read R1 at their snapshots. T1 then updates R1, keeping
# its key, and commits. Above READ COMMITTED a lock fails on a
# version committed after the snapshot, but KS fails only on one
# that changed the key: T2's KS lock is taken, T3's S lock fails.

stores chained
pairings all
session T1
session T2
session T3
seed R1 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T2 read R1
@2 T3 read R1
@3 T1 write R1 t1 keykept
@4 T1 commit
@5 T2 lock R1 keyshare
@6 check last T2 ok
@7 T3 lock R1 shared
@8 check last T3 conflict
@9 T3 abort
@10 T2 commit
@11 check committed T1 T2
@12 check row R1 t1
