# a11 -- a point read of a row that did not exist yet.
#
# The read keeps saying "no row". T1 only reads here; the
# version where it goes on to insert R2 itself is c05.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T1 read R2
@3 T2 begin
@4 T2 insert R2 t2
@5 T2 commit
@6 T1 read R2
@7 T1 commit
@8 check row R2 t2
