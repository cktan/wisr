# c03 -- write skew over a predicate, resolved by inserts.
#
# "Insert it if nobody else has", run twice: each insert
# falsifies the other's scan.

pairings all
session T1
session T2

@1 T1 begin
@1 T2 begin
@2 T1 scan R1 R3
@3 T2 scan R1 R3
@4 T1 insert R1 t1
@5 T2 insert R2 t2
@6 T1 commit?
@7 T2 commit?
@8 check committed T1 T2 when rr
@8 check committed T1 T2 when rr+v
@8 check at-most-one-committed T1 T2 when v+v
@9 check rows when rr
