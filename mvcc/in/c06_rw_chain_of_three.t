# c06 -- the dangerous structure with three transactions.
#
#   T1 --rw--> T2 --rw--> T3, and T3 commits first.
#
# T2 is the pivot. T3 takes T2's level, so V+V means all three
# are SERIALIZABLE and T2 must fail. In either mixed pairing
# one link of the chain is an untracked REPEATABLE READ read,
# so nothing fails.

pairings all
session T1
session T2
session T3
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 read R1
@3 T2 read R2
@4 T2 write R1 t2
@5 T3 write R2 t3
@6 T3 commit
@7 T2 commit
@8 T1 commit
@9 check committed T1 T2 T3 when rr
@9 check committed T1 T2 T3 when rr+v
@9 check failed T2 when v+v
@9 check committed T1 T3 when v+v
@10 check row R1 t2 when rr
@10 check row R1 0 when v+v
@11 check row R2 t3
