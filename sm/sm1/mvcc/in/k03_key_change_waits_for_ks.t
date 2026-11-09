# k03 -- an update that changes the key waits for a key-share lock.
#
# T1 holds R1 in KS. T2's update changes a key column, so it writes
# in X, which KS refuses: T2 waits until T1 commits, and then
# writes. T1 wrote nothing, so T2 has no conflict.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 lock R1 keyshare
@3 T2 write R1 t2 blocks
@4 T1 commit
@5 check last T2 ok
@6 T2 commit
@7 check committed T1 T2
@8 check row R1 t2
