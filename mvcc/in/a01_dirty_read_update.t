# a01 -- dirty read of an update.
#
# T1 updates R1 but does not commit. T2 must not see the new
# value, before or after T1 commits: T2's snapshot was taken
# first either way.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T2 read R1
@4 T1 commit
@5 T2 read R1
@6 T2 commit
@7 check row R1 t1
@7 check rows
