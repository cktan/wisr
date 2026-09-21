# c26 -- the transaction a pivot points at aborts.
#
# c06's chain, T1 --rw--> T2 --rw--> T3, with T3 aborting
# instead of committing. An aborted T3 cannot be part of a
# cycle, so the pivot is a false positive; wisr keeps two
# flags per transaction rather than the edges themselves, so
# nothing withdraws T2's outgoing flag and T2 fails anyway.
# The rule is conservative by design: this is what that costs.

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
@6 T3 abort
@7 T2 commit
@8 T1 commit
@9 check committed T1 T2 when rr
@9 check committed T1 T2 when rr+s
@9 check failed T2 when s+s
@9 check committed T1 when s+s
@10 check row R1 t2 when rr
@10 check row R1 0 when s+s
@11 check row R2 0
