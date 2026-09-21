# c16 -- a point read that steps past a newer version.
#
# T2 reads R2, writes R1, and commits. T1 then reads R1 and
# steps past T2's newer version, which is T1 --rw--> T2, and
# writes R2, which T2 read: T2 --rw--> T1. T2 is a committed
# pivot, so T1 is the one that must fail.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T2 read R2
@3 T2 write R1 t2
@4 T2 commit
@5 T1 read R1
@6 T1 write R2 t1
@7 T1 commit
@8 check committed T1 when rr
@8 check committed T1 when rr+v
@8 check failed T1 when v+v
@9 check row R2 t1 when rr
@9 check row R2 0 when v+v
