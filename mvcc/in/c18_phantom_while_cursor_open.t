# c18 -- an insert into a range whose cursor is still open.
#
# The range is registered when the cursor opens, before its
# snapshot, so a row inserted while it is open is caught even
# though the cursor never returns it. T2's read of R3 and T1's
# write of it close the cycle.

pairings all
session T1
session T2
seed R1 0
seed R3 0

@1 T1 begin
@1 T2 begin
@2 T1 open C R1 R5
@3 T1 next C
@4 T2 read R3
@5 T2 insert R2 t2
@6 T2 commit
@7 T1 next C
@8 T1 next C
@9 T1 close C
@10 T1 write R3 t1
@11 T1 commit
@12 check committed T1 when rr
@12 check committed T1 when rr+v
@12 check failed T1 when v+v
