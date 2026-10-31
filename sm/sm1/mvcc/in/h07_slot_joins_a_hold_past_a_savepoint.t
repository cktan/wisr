# h07 -- a slot that joins a hold goes back alone at a rollback.
#
# T1 takes table 7's IS while T2 holds S on it, so the IS goes
# to the lock table, a hold of T1's. T2 commits; past a
# savepoint, T1 asks IX on table 7, which takes a slot: T1's
# second lock on the table. T3's X sweeps the slot into T1's
# hold and waits. T1's rollback to the savepoint undoes only the
# IX: its IS, taken before, still holds, so T3 waits on until T1
# commits.

pairings rr
session T1
session T2
session T3

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T2 lockoid 7 shared
@3 T1 lockoid 7 is
@4 T2 commit
@5 T1 savepoint
@6 T1 lockoid 7 ix
@7 T3 lockoid 7 exclusive blocks
@8 T1 rollback 1
@9 T1 commit
@10 check last T3 ok
@11 T3 commit
@12 check committed T1 T2 T3
