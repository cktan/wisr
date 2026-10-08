# f01 -- READ COMMITTED: the lost update is avoided, not aborted.
#
# Two read committed sessions each set the hot row R1. T2 waits
# for T1's row lock. When T1 commits, T2's write meets a row
# that changed after T2 began: it fails with `updated`, which
# does not abort. T2 reads the newest version with readlatest
# and writes; nothing T1 did is lost.

pairings rr
session T1 rc
session T2 rc
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 1
@3 T2 write R1 2 blocks
@4 T1 commit
@5 check last T2 updated
@6 T2 readlatest R1
@6 check last T2 1
@7 T2 write R1 2
@8 T2 commit
@9 check committed T1 T2
@9 check row R1 2
