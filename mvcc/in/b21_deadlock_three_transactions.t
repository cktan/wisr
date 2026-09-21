# b21 -- a deadlock cycle of three.
#
# b16's cycle has two members, which a walk of one waits-for
# pointer per transaction finds. This one is T1 -> T2 -> T3 ->
# T1, so the search has to follow the chain the whole way
# round before it can say there is a cycle at all. The
# youngest, T3, is the victim, and dooming it drains the
# queue behind it: R3 is freed, so T2 goes through, and T2
# ending frees R2, so T1 goes through. T2 aborts rather than
# commits, because a committed T2 would hand T1 a
# first-committer-wins conflict instead -- that is b19's
# point, and it would hide this one.

pairings all
session T1
session T2
session T3
seed R1 0
seed R2 0
seed R3 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 write R1 t1
@2 T2 write R2 t2
@2 T3 write R3 t3
@3 T1 write R2 t1 blocks
@4 T2 write R3 t2 blocks
@5 T3 write R1 t3
@6 check last T3 deadlock
@7 T2 abort
@8 check last T1 ok
@9 T1 commit
@10 check committed T1
@11 check row R1 t1
@11 check row R2 t1
@11 check row R3 0
