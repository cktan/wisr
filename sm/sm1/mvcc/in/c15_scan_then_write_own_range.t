# c15 -- writing inside a range you scanned yourself.
#
# A transaction's own writes into its own predicate are not a
# conflict: an edge needs two different transactions. T2 reads
# elsewhere, so the tracking is on and simply has nothing to
# report.

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
@4 T1 write R1 t1
@5 T1 insert R3 t1
@6 T1 commit
@7 T2 commit
@8 check committed T1 T2
@9 check rows
