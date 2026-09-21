# b02 -- write-write with no read at all.
#
# T1 never reads R1, and still conflicts: the rule compares the
# row's newest committed version with T1's snapshot, not with
# anything T1 read.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T2 write R1 t2
@3 T2 commit
@4 T1 write R1 t1
@5 check last T1 conflict
@6 check row R1 t2
