# b24 -- an update waiting on an uncommitted insert that
# aborts.
#
# b18 has the insert commit, and the update then fails with
# conflict: the row appeared after its snapshot. Here the
# insert aborts instead, so no row ever existed, and the
# waiter must be told notfound rather than conflict. The
# distinction matters to a caller: conflict is retryable and
# notfound is not, and only the outcome of the transaction it
# waited on decides which it gets.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 insert R2 t1
@3 T2 write R2 t2 blocks
@4 T1 abort
@5 check last T2 notfound
@6 T2 commit
@7 check committed T2
@8 check row R2 -
