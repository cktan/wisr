# c12 -- a read-only transaction alongside a writer.
#
# T1 --rw--> T2 is the only edge, and T1 writes nothing, so
# nobody can have an edge into T1. A read-only transaction that
# failed here would make SERIALIZABLE reporting unusable.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 read R1
@2 T1 read R2
@3 T2 begin
@4 T2 write R1 t2
@5 T2 commit
@6 T1 read R1
@7 T1 commit
@8 check committed T1 T2
