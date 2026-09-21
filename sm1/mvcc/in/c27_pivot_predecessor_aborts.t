# c27 -- the transaction pointing at a pivot aborts.
#
# The mirror of c26. Same chain, T1 --rw--> T2 --rw--> T3,
# but this time the predecessor T1 aborts instead of the
# successor T3. An aborted T1 cannot be part of a cycle
# either, so T2 is again a false positive, and again nothing
# withdraws the flag: wisr records that T2 has an incoming
# edge, not who it came from, so T1 leaving takes nothing
# with it. c26 pays for the same rule from the other end.

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
@6 T1 abort
@7 T2 commit?
@8 T3 commit
@9 check committed T2 T3 when rr
@9 check committed T2 T3 when rr+s
@9 check failed T2 when s+s
@9 check committed T3 when s+s
@10 check row R1 t2 when rr
@10 check row R1 t2 when rr+s
@10 check row R1 0 when s+s
@11 check row R2 t3
