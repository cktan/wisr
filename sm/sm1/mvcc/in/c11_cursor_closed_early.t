# c11 -- a cursor closed early covers only what it read.
#
# T1 stops its scan at R1 and closes the cursor, so the
# predicate shrinks to the part traversed and T2's insert past
# that point creates no edge back. Only T2 --rw--> T1 remains,
# and one edge is not a cycle.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 open C R1 R5
@3 T1 next C
@4 T1 close C
@5 T2 insert R4 t2
@6 T2 read R2
@7 T1 write R2 t1
@8 T1 commit
@9 T2 commit
@10 check committed T1 T2
