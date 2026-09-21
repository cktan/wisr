# c01 -- one read-write edge is not a cycle.
#
# T1 reads R1, T2 writes it and commits: T1 --rw--> T2, and
# nothing points back. T1 then writes a row nobody read. This
# must commit at every pairing, V+V included: SSI fails a
# transaction only for two adjacent edges with the far one
# committing first.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 write R1 t2
@5 T2 commit
@6 T1 write R2 t1
@7 T1 commit
@8 check committed T1 T2
@9 check rows
