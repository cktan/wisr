# c21 -- a delete inside a scanned range makes the same edge an
# insert does.
#
# T1 scans the range and writes R5; T2 reads R5 and deletes a
# row inside T1's range. The edges run both ways.

pairings all
session T1
session T2
seed R1 0
seed R2 0
seed R5 0

@1 T1 begin
@1 T2 begin
@2 T1 scan R1 R3
@3 T2 read R5
@4 T2 delete R1
@5 T1 write R5 t1
@6 T1 commit?
@7 T2 commit?
@8 check committed T1 T2 when rr
@8 check committed T1 T2 when rr+v
@8 check at-most-one-committed T1 T2 when v+v
