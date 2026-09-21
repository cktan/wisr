# e04 -- the catalog is read under the same rules.
#
# sm_lsrel is a scan of the catalog relation, so each
# transaction's sm_mkrel lands in the other's predicate. This
# is c03 with relations instead of rows, and it must behave the
# same way.

pairings all
session T1
session T2

@1 T1 begin
@1 T2 begin
@2 T1 lsrel
@3 T2 lsrel
@4 T1 mkrel
@5 T2 mkrel
@6 T1 commit?
@7 T2 commit?
@8 check committed T1 T2 when rr
@8 check committed T1 T2 when rr+s
@8 check at-most-one-committed T1 T2 when s+s
