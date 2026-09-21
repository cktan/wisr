# f04 -- READ COMMITTED: a write cannot skip the re-read.
#
# T2's write is refused with `updated`. Trying again without
# readlatest is refused again: sm1 does not let the caller lose
# the predicate. The refusal is not an abort: T2 is still
# active and still holds the row lock, so T3 stays queued behind
# it until T2 ends.

pairings rr
session T1 rc
session T2 rc
session T3 rc
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 1
@3 T2 write R1 2 blocks
@4 T1 commit
@5 check last T2 updated
@6 T3 begin
@7 T3 write R1 3 blocks
@8 T2 write R1 2
@8 check last T2 updated
@9 T2 abort
@10 check last T3 ok
@11 T3 commit
@12 check committed T1 T3
@12 check row R1 3
