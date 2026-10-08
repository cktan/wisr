# b03 -- an update over a committed delete.
#
# The row still exists in T1's snapshot, so the answer must be
# the retryable conflict, not the "no such row" of b13.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 delete R1
@5 T2 commit
@6 T1 write R1 t1
@7 check last T1 conflict
@8 check row R1 -
