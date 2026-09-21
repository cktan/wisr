# b14 -- what the loser of a conflict leaves behind.
#
# T1's earlier writes go with it: the conflict aborts the whole
# transaction, so R1 and R2 keep their old values and only T2's
# change survives.

pairings all
session T1
session T2
seed R1 0
seed R2 0
seed R3 0

@1 T1 begin
@2 T1 write R1 t1
@2 T1 write R2 t1
@3 T2 begin
@4 T2 write R3 t2
@5 T2 commit
@6 T1 write R3 t1
@7 check last T1 conflict
@8 T1 commit
@9 check last T1 notrans
@10 check rows
