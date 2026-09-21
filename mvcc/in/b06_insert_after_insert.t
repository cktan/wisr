# b06 -- an insert over a committed insert of the same row.
#
# R2 is absent in T1's snapshot, so from T1's point of view
# this is not a duplicate key but a row that changed after the
# snapshot: conflict, and retryable. Compare b12.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T1 read R2
@3 T2 begin
@4 T2 insert R2 t2
@5 T2 commit
@6 T1 insert R2 t1
@7 check last T1 conflict
@8 check row R2 t2
