# c17 -- an edge onto a pivot that has already committed.
#
# T3 reads R1; T2 reads R2, writes R1, and commits, so T2 has
# an edge in. T1's write of R2 gives T2 an edge out after it
# committed, making it a pivot. T1 is the pivot's successor
# and commits last, after T2, so the order T3, T2, T1 is
# serial and nothing fails: a pivot is dangerous only when its
# successor commits first (Ports and Grittner).

pairings all
session T1
session T2
session T3
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T3 read R1
@3 T2 read R2
@4 T2 write R1 t2
@5 T2 commit
@6 T1 write R2 t1
@7 T1 commit
@8 check committed T1
