# a07 -- no phantom from a delete.
#
# A row deleted after T1's snapshot stays in T1's range for the
# whole transaction.

pairings all
session T1
session T2
seed R1 0
seed R2 0
seed R3 0

@1 T1 begin
@2 T1 scan R1 R3
@3 T2 begin
@4 T2 delete R2
@5 T2 commit
@6 T1 scan R1 R3
@7 T1 commit
@8 check rows
