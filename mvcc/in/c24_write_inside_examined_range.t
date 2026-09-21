# c24 -- a write inside the part an early-closed cursor read.
#
# The other half of c11. There T2's insert landed past the key
# T1 stopped at, and the narrowed predicate let it through;
# here T2 writes R1, which T1 did read before closing. The
# narrowing must keep that key, so T1 --rw--> T2 stands, and
# with T2 --rw--> T1 from T1's write of R2 the cycle is closed.

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
@5 T2 write R1 t2
@6 T2 read R2
@7 T1 write R2 t1
@8 T1 commit?
@9 T2 commit?
@10 check committed T1 T2 when rr
@10 check committed T1 T2 when rr+s
@10 check at-most-one-committed T1 T2 when s+s
