# b32 -- two transactions dropping the same store.
#
# A drop is a write of the catalog row naming the store, so
# it takes that row's lock and is judged like any other write.
# T2 waits on T1 rather than dropping a store twice, and
# once T1 commits it loses first-committer-wins: the store
# is gone, but T2 is told it lost a race, not that the
# store never existed. b05 is the same answer for a row.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 rmstore
@3 T2 rmstore blocks
@4 T1 commit
@5 check last T2 conflict
@6 T2 begin
@7 T2 lookup R1
@8 check last T2 notfound
@9 T2 abort
