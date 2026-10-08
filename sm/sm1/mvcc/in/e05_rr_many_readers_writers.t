# e05 -- many RR transactions reading everything and writing
# one row apiece.
#
# Every pair of them has read-write edges in both directions,
# which is exactly the shape SERIALIZABLE refuses. At
# REPEATABLE READ all four must commit: this is the cost RR
# declines to pay, and the reason a caller picks it.

pairings rr
session T1 rr
session T2 rr
session T3 rr
session T4 rr
seed R1 0
seed R2 0
seed R3 0
seed R4 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@1 T4 begin
@2 T1 scan R1 R4
@2 T2 scan R1 R4
@2 T3 scan R1 R4
@2 T4 scan R1 R4
@3 T1 write R1 t1
@3 T1 commit
@4 T2 write R2 t2
@4 T2 commit
@5 T3 write R3 t3
@5 T3 commit
@6 T4 write R4 t4
@6 T4 commit
@7 check committed T1 T2 T3 T4
@8 check rows
