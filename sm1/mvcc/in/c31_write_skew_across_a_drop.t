# c31 -- write skew where one side's read is a storeid lookup.
#
# DESIGN.md §11's argument for tracking catalog reads. T1
# reads R1 and drops the second store; T2 resolves that
# store to read a row from it, then writes R1. Neither
# touches a row the other touches: the only thing joining them
# is that T2 looked up the store T1 dropped. A storeid
# resolution is an ordinary point read of the catalog, SIREAD
# and all, so the edge T2 --rw--> T1 forms, and with T1 --rw-->
# T2 from T1's read of R1 the cycle closes. Were the lookup
# untracked, both would commit and the schedule would match no
# serial order at all -- drop T2's lookup and that is exactly
# what happens, which is c01's shape: one edge is not a cycle.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 read R1
@3 T2 lookup R1
@4 T1 rmstore
@5 T2 write R1 t2
@6 T1 commit?
@6 T2 commit?
@7 check committed T1 T2 when rr
@7 check committed T1 T2 when rr+s
@7 check at-most-one-committed T1 T2 when s+s
