# a04 -- read-write-read with an update.
#
# T1 reads R1, T2 updates it and commits, T1 reads again. The
# second read must give the first read's value. T1 only reads,
# so it commits even at S+S: one rw-edge out of T1 is not a
# dangerous structure.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 write R1 t2
@5 T2 commit
@6 T1 read R1
@7 T1 commit
@8 check committed T1 T2
@8 check row R1 t2
