# c27 -- the transaction pointing at a pivot aborts.
#
# The mirror of c26. Same chain, T1 --rw--> T2 --rw--> T3,
# but this time the predecessor T1 aborts instead of the
# successor T3. An aborted T1 cannot be part of a cycle
# either: its abort takes its edge into T2 with it, so T2 is
# no longer a pivot and commits. c26 is the same rule from
# the other end.

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
@7 T2 commit
@8 T3 commit
@9 check committed T2 T3
@10 check row R1 t2
@11 check row R2 t3
