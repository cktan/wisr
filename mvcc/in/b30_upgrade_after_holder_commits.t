# b30 -- an upgrade that waits for a holder to commit.
#
# b28 breaks a pair of upgrades by dooming one of them; here
# only T1 upgrades, so there is no cycle and nothing to break.
# It simply waits, and T2 committing -- having written nothing,
# so there is no first-committer-wins verdict to inherit --
# lets the write through. The lock is exclusive afterwards,
# which is what T3 finds.

pairings all
session T1
session T2
session T3
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 lock R1 shared
@3 T2 lock R1 shared
@4 T1 write R1 t1 blocks
@5 T2 commit
@6 check last T1 ok
@7 T3 begin
@8 T3 lock R1 shared blocks
@9 T1 commit
@10 check last T3 conflict
@11 check row R1 t1
