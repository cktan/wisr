# b08 -- writes to different rows never conflict.
#
# The check is per row, not per store: two transactions
# writing disjoint rows are independent, whatever their levels.

pairings all
session T1
session T2
seed R1 0
seed R2 0
seed R3 0
seed R5 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@2 T2 write R2 t2
@3 T1 delete R3
@3 T2 delete R5
@4 T1 insert R4 t1
@5 T1 commit
@5 T2 commit
@6 check committed T1 T2
@7 check rows
