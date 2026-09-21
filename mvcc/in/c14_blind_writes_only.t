# c14 -- writes with no reads build no edges.
#
# Neither transaction reads anything, so no read-write conflict
# can exist in either direction, whatever the levels.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T2 write R2 t2
@4 T1 commit
@5 T2 commit
@6 check committed T1 T2
@7 check rows
