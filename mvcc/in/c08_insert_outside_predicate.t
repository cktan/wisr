# c08 -- an insert outside the scanned range is no conflict.
#
# T2's insert lands past T1's predicate, so there is no edge
# from T1; T1's write of a row T2 read gives one edge only. A
# level that failed here would be tracking ranges too coarsely.

pairings all
session T1
session T2
seed R1 0
seed R3 0

@1 T1 begin
@1 T2 begin
@2 T1 scan R1 R2
@3 T2 insert R4 t2
@4 T2 read R3
@5 T1 write R3 t1
@6 T1 commit
@7 T2 commit
@8 check committed T1 T2
@9 check rows
