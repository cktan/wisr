# c09 -- reads and writes that do not meet.
#
# Neither transaction writes a row the other read, so there is
# no edge in either direction.

pairings all
session T1
session T2
seed R1 0
seed R2 0
seed R3 0
seed R4 0

@1 T1 begin
@1 T2 begin
@2 T1 read R1
@3 T2 read R3
@4 T1 write R2 t1
@5 T2 write R4 t2
@6 T1 commit
@7 T2 commit
@8 check committed T1 T2
