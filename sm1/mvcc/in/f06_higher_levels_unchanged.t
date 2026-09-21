# f06 -- REPEATABLE READ and SERIALIZABLE: still first-committer.
#
# The f01 timeline at the levels above READ COMMITTED. T2 waits
# for T1's lock, and when T1 commits, T2's write fails with
# `conflict` and aborts T2. Nothing of the read committed
# protocol applies here.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 1
@3 T2 write R1 2 blocks
@4 T1 commit
@5 check last T2 conflict
@6 check committed T1
@6 check row R1 1
